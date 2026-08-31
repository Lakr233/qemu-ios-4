/*
 * NXP PCF50635 power-management unit
 *
 * The PCF50635 used by the iPhone 3G is an Apple-specific relative of the
 * documented PCF50633.  This model implements only the register transport,
 * battery/accessory ADC, RTC, and general-purpose memory contracts consumed
 * by the iPhone 3G boot software.  Other power, regulator, and interrupt
 * behavior remains deliberately unimplemented until an N82AP
 * producer/consumer pair establishes it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/pcf50635.h"
#include "migration/vmstate.h"
#include "qemu/bcd.h"
#include "qemu/module.h"
#include "system/rtc.h"
#include "trace.h"

#define PCF50635_REGISTER_COUNT  0x100

#define PCF50635_INT_STATUS_BASE 0x02
#define PCF50635_INT_MASK_BASE   0x07
#define PCF50635_INT_STATUS_COUNT 5
#define PCF50635_INT1_USB_INSERT BIT(2)
#define PCF50635_INT1_USB_REMOVE BIT(3)
#define PCF50635_MBCS1           0x4b
#define PCF50635_MBCS2           0x4c
#define PCF50635_ADCC1           0x54
#define PCF50635_ADCS1           0x55
#define PCF50635_ADCS3           0x57
#define PCF50635_RTCSC           0x59
#define PCF50635_RTCMN           0x5a
#define PCF50635_RTCHR           0x5b
#define PCF50635_RTCWD           0x5c
#define PCF50635_RTCDT           0x5d
#define PCF50635_RTCMT           0x5e
#define PCF50635_RTCYR           0x5f

#define PCF50635_ADCC1_START      BIT(0)
#define PCF50635_ADCC1_MUX_SHIFT  4
#define PCF50635_ADCC1_MUX_MASK   0xf
#define PCF50635_ADCC1_MUX_BATTERY_DIVIDED 0
#define PCF50635_ADCC1_MUX_USB_DIVIDED     2
#define PCF50635_ADCC1_MUX_ACCESSORY       7
#define PCF50635_ADCS3_READY      BIT(7)
#define PCF50635_MBCS1_USB_PRESENT BIT(0)
#define PCF50635_MBCS1_USB_OK      BIT(1)
#define PCF50635_MBCS2_CHARGER_MASK (BIT(4) | BIT(5))
#define PCF50635_MBCS2_CHARGER_USB BIT(5)
#define PCF50635_ADC_FULL_SCALE_MV 6000
#define PCF50635_ADC_MAX          1023

struct PCF50635State {
    I2CSlave parent_obj;

    qemu_irq irq;
    uint8_t registers[PCF50635_REGISTER_COUNT];
    uint8_t pointer;
    bool expect_pointer;
    int64_t rtc_offset;
    uint8_t weekday_offset;
    uint16_t battery_millivolts;
    bool usb_power_present;
};

static void pcf50635_update_irq(PCF50635State *s)
{
    uint8_t pending = 0;

    for (unsigned i = 0; i < PCF50635_INT_STATUS_COUNT; i++) {
        pending |= s->registers[PCF50635_INT_STATUS_BASE + i] &
                   ~s->registers[PCF50635_INT_MASK_BASE + i];
    }
    trace_pcf50635_irq(pending, !pending);
    qemu_set_irq(s->irq, !pending);
}

static void pcf50635_set_usb_power(void *opaque, int line, int level)
{
    PCF50635State *s = opaque;
    bool present = level != 0;

    if (s->usb_power_present == present) {
        return;
    }
    s->usb_power_present = present;
    s->registers[PCF50635_INT_STATUS_BASE] |=
        present ? PCF50635_INT1_USB_INSERT : PCF50635_INT1_USB_REMOVE;
    pcf50635_update_irq(s);
}

static void pcf50635_capture_rtc(PCF50635State *s)
{
    struct tm now;

    qemu_get_timedate(&now, s->rtc_offset);
    s->registers[PCF50635_RTCSC] = to_bcd(now.tm_sec);
    s->registers[PCF50635_RTCMN] = to_bcd(now.tm_min);
    s->registers[PCF50635_RTCHR] = to_bcd(now.tm_hour);
    s->registers[PCF50635_RTCWD] =
        (now.tm_wday + s->weekday_offset) % 7;
    s->registers[PCF50635_RTCDT] = to_bcd(now.tm_mday);
    s->registers[PCF50635_RTCMT] = to_bcd(now.tm_mon + 1);
    s->registers[PCF50635_RTCYR] = to_bcd(now.tm_year - 100);
}

static void pcf50635_write_rtc(PCF50635State *s, uint8_t reg, uint8_t value)
{
    struct tm now;

    qemu_get_timedate(&now, s->rtc_offset);
    switch (reg) {
    case PCF50635_RTCSC:
        now.tm_sec = from_bcd(value & 0x7f);
        break;
    case PCF50635_RTCMN:
        now.tm_min = from_bcd(value & 0x7f);
        break;
    case PCF50635_RTCHR:
        now.tm_hour = from_bcd(value & 0x3f);
        break;
    case PCF50635_RTCWD:
        s->weekday_offset = ((value & 7) - now.tm_wday + 7) % 7;
        return;
    case PCF50635_RTCDT:
        now.tm_mday = from_bcd(value & 0x3f);
        break;
    case PCF50635_RTCMT:
        now.tm_mon = from_bcd(value & 0x1f) - 1;
        break;
    case PCF50635_RTCYR:
        now.tm_year = from_bcd(value) + 100;
        break;
    default:
        g_assert_not_reached();
    }
    s->rtc_offset = qemu_timedate_diff(&now);
}

static void pcf50635_sample_adc(PCF50635State *s)
{
    unsigned mux = (s->registers[PCF50635_ADCC1] >>
                    PCF50635_ADCC1_MUX_SHIFT) & PCF50635_ADCC1_MUX_MASK;
    unsigned millivolts;

    switch (mux) {
    case PCF50635_ADCC1_MUX_BATTERY_DIVIDED:
        millivolts = s->battery_millivolts;
        break;
    case PCF50635_ADCC1_MUX_USB_DIVIDED:
        /* An ordinary USB host leaves the charger-identification sense low. */
        millivolts = 0;
        break;
    case PCF50635_ADCC1_MUX_ACCESSORY:
        /* No 30-pin accessory resistor is an open, full-scale input. */
        millivolts = PCF50635_ADC_FULL_SCALE_MV;
        break;
    default:
        millivolts = 0;
        break;
    }
    millivolts = MIN(millivolts, PCF50635_ADC_FULL_SCALE_MV);
    unsigned sample = (millivolts * PCF50635_ADC_MAX +
                       PCF50635_ADC_FULL_SCALE_MV / 2) /
                      PCF50635_ADC_FULL_SCALE_MV;

    s->registers[PCF50635_ADCS1] = sample >> 2;
    s->registers[PCF50635_ADCS3] = PCF50635_ADCS3_READY | (sample & 3);
}

static int pcf50635_event(I2CSlave *i2c, enum i2c_event event)
{
    PCF50635State *s = PCF50635(i2c);

    switch (event) {
    case I2C_START_RECV:
        pcf50635_capture_rtc(s);
        break;
    case I2C_START_SEND:
        s->expect_pointer = true;
        break;
    default:
        break;
    }
    return 0;
}

static uint8_t pcf50635_recv(I2CSlave *i2c)
{
    PCF50635State *s = PCF50635(i2c);
    uint8_t reg = s->pointer++;
    uint8_t value = s->registers[reg];

    if (reg == PCF50635_MBCS1) {
        value &= ~(PCF50635_MBCS1_USB_PRESENT | PCF50635_MBCS1_USB_OK);
        if (s->usb_power_present) {
            value |= PCF50635_MBCS1_USB_PRESENT | PCF50635_MBCS1_USB_OK;
        }
    } else if (reg == PCF50635_MBCS2) {
        value &= ~PCF50635_MBCS2_CHARGER_MASK;
        if (s->usb_power_present) {
            value |= PCF50635_MBCS2_CHARGER_USB;
        }
    }
    if (reg >= PCF50635_INT_STATUS_BASE &&
        reg < PCF50635_INT_STATUS_BASE + PCF50635_INT_STATUS_COUNT) {
        s->registers[reg] = 0;
        pcf50635_update_irq(s);
    }
    trace_pcf50635_read(reg, value);
    return value;
}

static int pcf50635_send(I2CSlave *i2c, uint8_t data)
{
    PCF50635State *s = PCF50635(i2c);
    uint8_t reg;

    if (s->expect_pointer) {
        s->pointer = data;
        s->expect_pointer = false;
        return 0;
    }

    reg = s->pointer++;
    if (reg >= PCF50635_INT_STATUS_BASE &&
        reg < PCF50635_INT_STATUS_BASE + PCF50635_INT_STATUS_COUNT) {
        /* Interrupt status is read-only and cleared by pcf50635_recv(). */
    } else if (reg >= PCF50635_RTCSC && reg <= PCF50635_RTCYR) {
        pcf50635_write_rtc(s, reg, data);
    } else {
        s->registers[reg] = data;
    }
    trace_pcf50635_write(reg, data);
    if (reg == PCF50635_ADCC1 && (data & PCF50635_ADCC1_START)) {
        pcf50635_sample_adc(s);
        s->registers[PCF50635_ADCC1] &= ~PCF50635_ADCC1_START;
    }
    if (reg >= PCF50635_INT_MASK_BASE &&
        reg < PCF50635_INT_MASK_BASE + PCF50635_INT_STATUS_COUNT) {
        pcf50635_update_irq(s);
    }
    return 0;
}

static void pcf50635_reset(DeviceState *dev)
{
    PCF50635State *s = PCF50635(dev);

    s->pointer = 0;
    s->expect_pointer = false;
    pcf50635_update_irq(s);
}

static int pcf50635_post_load(void *opaque, int version_id)
{
    PCF50635State *s = opaque;

    pcf50635_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_pcf50635 = {
    .name = TYPE_PCF50635,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pcf50635_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, PCF50635State),
        VMSTATE_UINT8_ARRAY(registers, PCF50635State,
                            PCF50635_REGISTER_COUNT),
        VMSTATE_UINT8(pointer, PCF50635State),
        VMSTATE_BOOL(expect_pointer, PCF50635State),
        VMSTATE_INT64(rtc_offset, PCF50635State),
        VMSTATE_UINT8(weekday_offset, PCF50635State),
        VMSTATE_END_OF_LIST()
    },
};

static void pcf50635_realize(DeviceState *dev, Error **errp)
{
    PCF50635State *s = PCF50635(dev);

    qdev_init_gpio_in_named(dev, pcf50635_set_usb_power,
                            "usb-power-present", 1);
    qdev_init_gpio_out_named(dev, &s->irq, "irq", 1);
    if (s->usb_power_present) {
        s->registers[PCF50635_INT_STATUS_BASE] |=
            PCF50635_INT1_USB_INSERT;
    }
    pcf50635_update_irq(s);
}

static const Property pcf50635_properties[] = {
    DEFINE_PROP_UINT16("battery-millivolts", PCF50635State,
                       battery_millivolts, 4000),
    DEFINE_PROP_BOOL("usb-power-present", PCF50635State,
                     usb_power_present, false),
};

static void pcf50635_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    dc->desc = "NXP PCF50635 power-management unit";
    dc->realize = pcf50635_realize;
    dc->vmsd = &vmstate_pcf50635;
    device_class_set_legacy_reset(dc, pcf50635_reset);
    device_class_set_props(dc, pcf50635_properties);
    sc->event = pcf50635_event;
    sc->recv = pcf50635_recv;
    sc->send = pcf50635_send;
}

static const TypeInfo pcf50635_info = {
    .name = TYPE_PCF50635,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(PCF50635State),
    .class_init = pcf50635_class_init,
};

static void pcf50635_register_types(void)
{
    type_register_static(&pcf50635_info);
}

type_init(pcf50635_register_types)
