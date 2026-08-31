/*
 * Intersil ISL29003 ambient-light sensor
 *
 * The iPhone 3G N82AP DeviceTree wires this device at I2C address 0x44
 * and connects its active-low open-drain interrupt to GPIO 73.  The model
 * exposes a fixed ambient-light input and updates a conversion when the
 * guest programs the command register.  Conversion timing is intentionally
 * not modeled until a guest consumer requires it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/sensor/isl29003.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define ISL29003_REGISTER_COUNT 8

#define ISL29003_COMMAND        0x00
#define ISL29003_CONTROL        0x01
#define ISL29003_THRESHOLD_HIGH 0x02
#define ISL29003_THRESHOLD_LOW  0x03
#define ISL29003_SENSOR_LOW     0x04
#define ISL29003_SENSOR_HIGH    0x05
#define ISL29003_TIMER_LOW      0x06
#define ISL29003_TIMER_HIGH     0x07

#define ISL29003_COMMAND_ENABLE     BIT(7)
#define ISL29003_COMMAND_POWER_DOWN BIT(6)
#define ISL29003_COMMAND_WIDTH_MASK 0x03
#define ISL29003_CONTROL_INTERRUPT  BIT(5)
#define ISL29003_CONTROL_GAIN_SHIFT 2

struct ISL29003State {
    I2CSlave parent_obj;

    qemu_irq irq;
    uint8_t registers[ISL29003_REGISTER_COUNT];
    uint8_t pointer;
    bool expect_pointer;
    uint32_t ambient_lux;
};

static void isl29003_update_irq(ISL29003State *s)
{
    qemu_set_irq(s->irq,
                 !(s->registers[ISL29003_CONTROL] &
                   ISL29003_CONTROL_INTERRUPT));
}

static void isl29003_convert(ISL29003State *s)
{
    static const uint32_t maximum_count[] = {
        UINT16_MAX, 0x0fff, 0x00ff, 0x000f,
    };
    uint8_t command = s->registers[ISL29003_COMMAND];
    uint8_t control = s->registers[ISL29003_CONTROL];
    unsigned width = command & ISL29003_COMMAND_WIDTH_MASK;
    unsigned gain = extract32(control, ISL29003_CONTROL_GAIN_SHIFT, 2);
    bool enabled = (command & ISL29003_COMMAND_ENABLE) &&
                   !(command & ISL29003_COMMAND_POWER_DOWN);
    uint32_t count = 0;
    uint32_t full_scale = 1000U << (gain * 2);
    uint32_t high = s->registers[ISL29003_THRESHOLD_HIGH] << 8;
    uint32_t low = s->registers[ISL29003_THRESHOLD_LOW] << 8;

    if (enabled) {
        count = MIN((uint64_t)s->ambient_lux * maximum_count[width] /
                    full_scale, maximum_count[width]);
        if (count > high || count < low) {
            s->registers[ISL29003_CONTROL] |=
                ISL29003_CONTROL_INTERRUPT;
        }
    }

    s->registers[ISL29003_SENSOR_LOW] = count;
    s->registers[ISL29003_SENSOR_HIGH] = count >> 8;
    s->registers[ISL29003_TIMER_LOW] = enabled ? maximum_count[width] : 0;
    s->registers[ISL29003_TIMER_HIGH] =
        enabled ? maximum_count[width] >> 8 : 0;
    isl29003_update_irq(s);
}

static uint8_t isl29003_recv(I2CSlave *i2c)
{
    ISL29003State *s = ISL29003(i2c);
    uint8_t reg = s->pointer++;

    if (reg >= ISL29003_REGISTER_COUNT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "isl29003: read outside register file at 0x%02x\n",
                      reg);
        return 0xff;
    }
    return s->registers[reg];
}

static void isl29003_write(ISL29003State *s, uint8_t reg, uint8_t value)
{
    switch (reg) {
    case ISL29003_COMMAND:
        s->registers[reg] = value & ~BIT(4);
        isl29003_convert(s);
        break;
    case ISL29003_CONTROL:
        s->registers[reg] = value & 0x2f;
        isl29003_update_irq(s);
        break;
    case ISL29003_THRESHOLD_HIGH:
    case ISL29003_THRESHOLD_LOW:
        s->registers[reg] = value;
        break;
    case ISL29003_SENSOR_LOW:
    case ISL29003_SENSOR_HIGH:
    case ISL29003_TIMER_LOW:
    case ISL29003_TIMER_HIGH:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "isl29003: write outside register file at 0x%02x\n",
                      reg);
        break;
    }
}

static int isl29003_send(I2CSlave *i2c, uint8_t value)
{
    ISL29003State *s = ISL29003(i2c);

    if (s->expect_pointer) {
        s->pointer = value;
        s->expect_pointer = false;
    } else {
        isl29003_write(s, s->pointer++, value);
    }
    return 0;
}

static int isl29003_event(I2CSlave *i2c, enum i2c_event event)
{
    ISL29003State *s = ISL29003(i2c);

    if (event == I2C_START_SEND) {
        s->expect_pointer = true;
    }
    return 0;
}

static void isl29003_reset(DeviceState *dev)
{
    ISL29003State *s = ISL29003(dev);

    memset(s->registers, 0, sizeof(s->registers));
    s->registers[ISL29003_THRESHOLD_HIGH] = 0xff;
    s->pointer = 0;
    s->expect_pointer = false;
    isl29003_convert(s);
}

static int isl29003_post_load(void *opaque, int version_id)
{
    ISL29003State *s = opaque;

    isl29003_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_isl29003 = {
    .name = TYPE_ISL29003,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = isl29003_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, ISL29003State),
        VMSTATE_UINT8_ARRAY(registers, ISL29003State,
                            ISL29003_REGISTER_COUNT),
        VMSTATE_UINT8(pointer, ISL29003State),
        VMSTATE_BOOL(expect_pointer, ISL29003State),
        VMSTATE_UINT32(ambient_lux, ISL29003State),
        VMSTATE_END_OF_LIST()
    },
};

static void isl29003_realize(DeviceState *dev, Error **errp)
{
    ISL29003State *s = ISL29003(dev);

    qdev_init_gpio_out_named(dev, &s->irq, "irq", 1);
}

static const Property isl29003_properties[] = {
    DEFINE_PROP_UINT32("ambient-lux", ISL29003State, ambient_lux, 500),
};

static void isl29003_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    dc->desc = "Intersil ISL29003 ambient-light sensor";
    dc->realize = isl29003_realize;
    dc->vmsd = &vmstate_isl29003;
    device_class_set_props(dc, isl29003_properties);
    device_class_set_legacy_reset(dc, isl29003_reset);
    sc->event = isl29003_event;
    sc->recv = isl29003_recv;
    sc->send = isl29003_send;
}

static const TypeInfo isl29003_info = {
    .name = TYPE_ISL29003,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(ISL29003State),
    .class_init = isl29003_class_init,
};

static void isl29003_register_types(void)
{
    type_register_static(&isl29003_info);
}

type_init(isl29003_register_types)
