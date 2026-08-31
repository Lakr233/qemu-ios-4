/*
 * Wolfson WM8991 audio codec
 *
 * This model provides the 2-wire control interface used during iPhone 3G
 * bring-up.  The serial-audio datapath is intentionally left to the S5L8900
 * I2S integration rather than approximated through an unrelated codec.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/audio/wm8991.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define WM8991_REGISTER_COUNT 0x40
#define WM8991_CHIP_ID        0x8990

struct WM8991State {
    I2CSlave parent_obj;

    uint16_t registers[WM8991_REGISTER_COUNT];
    uint8_t pointer;
    uint8_t high_byte;
    bool expect_pointer;
    bool low_byte;
};

/* WM8991 Production Data, December 2008, revision 4.0. */
static const uint16_t wm8991_register_defaults[WM8991_REGISTER_COUNT] = {
    [0] = WM8991_CHIP_ID,
    [1] = 0x0000,
    [2] = 0x6000,
    [3] = 0x0000,
    [4] = 0x4050,
    [5] = 0x4000,
    [6] = 0x01c8,
    [7] = 0x0000,
    [8] = 0x0040,
    [9] = 0x0040,
    [10] = 0x0004,
    [11] = 0x00c0,
    [12] = 0x00c0,
    [13] = 0x0000,
    [14] = 0x0100,
    [15] = 0x00c0,
    [16] = 0x00c0,
    [18] = 0x0000,
    [19] = 0x1000,
    [20] = 0x1010,
    [21] = 0x1010,
    [22] = 0x8000,
    [23] = 0x0800,
    [24] = 0x008b,
    [25] = 0x008b,
    [26] = 0x008b,
    [27] = 0x008b,
    [28] = 0x0000,
    [29] = 0x0000,
    [30] = 0x0066,
    [31] = 0x0022,
    [32] = 0x0079,
    [33] = 0x0079,
    [34] = 0x0003,
    [35] = 0x0003,
    [37] = 0x0100,
    [39] = 0x0000,
    [40] = 0x0000,
    [41] = 0x0000,
    [42] = 0x0000,
    [43] = 0x0000,
    [44] = 0x0000,
    [45] = 0x0000,
    [46] = 0x0000,
    [47] = 0x0000,
    [48] = 0x0000,
    [49] = 0x0000,
    [50] = 0x0000,
    [51] = 0x0180,
    [52] = 0x0000,
    [53] = 0x0000,
    [54] = 0x0000,
    [55] = 0x0000,
    [56] = 0x0000,
    [57] = 0x0000,
    [58] = 0x0000,
    [60] = 0x0008,
    [61] = 0x0031,
    [62] = 0x0026,
};

static void wm8991_reset_registers(WM8991State *s)
{
    memcpy(s->registers, wm8991_register_defaults, sizeof(s->registers));
}

static int wm8991_event(I2CSlave *i2c, enum i2c_event event)
{
    WM8991State *s = WM8991(i2c);

    switch (event) {
    case I2C_START_RECV:
        s->low_byte = false;
        break;
    case I2C_START_SEND:
        s->expect_pointer = true;
        s->low_byte = false;
        break;
    case I2C_FINISH:
        s->expect_pointer = false;
        s->low_byte = false;
        break;
    default:
        break;
    }
    return 0;
}

static uint8_t wm8991_recv(I2CSlave *i2c)
{
    WM8991State *s = WM8991(i2c);
    uint16_t value = s->pointer < WM8991_REGISTER_COUNT ?
                     s->registers[s->pointer] : 0;
    uint8_t data;

    if (!s->low_byte) {
        data = value >> 8;
        s->low_byte = true;
    } else {
        data = value;
        s->low_byte = false;
        s->pointer++;
    }
    return data;
}

static int wm8991_send(I2CSlave *i2c, uint8_t data)
{
    WM8991State *s = WM8991(i2c);
    uint8_t reg;

    if (s->expect_pointer) {
        s->pointer = data;
        s->expect_pointer = false;
        return 0;
    }

    if (!s->low_byte) {
        s->high_byte = data;
        s->low_byte = true;
        return 0;
    }

    reg = s->pointer++;
    s->low_byte = false;
    if (reg == 0) {
        wm8991_reset_registers(s);
    } else if (reg < WM8991_REGISTER_COUNT) {
        s->registers[reg] = (s->high_byte << 8) | data;
    }
    return 0;
}

static void wm8991_reset(DeviceState *dev)
{
    WM8991State *s = WM8991(dev);

    wm8991_reset_registers(s);
    s->pointer = 0;
    s->high_byte = 0;
    s->expect_pointer = false;
    s->low_byte = false;
}

static const VMStateDescription vmstate_wm8991 = {
    .name = TYPE_WM8991,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, WM8991State),
        VMSTATE_UINT16_ARRAY(registers, WM8991State,
                             WM8991_REGISTER_COUNT),
        VMSTATE_UINT8(pointer, WM8991State),
        VMSTATE_UINT8(high_byte, WM8991State),
        VMSTATE_BOOL(expect_pointer, WM8991State),
        VMSTATE_BOOL(low_byte, WM8991State),
        VMSTATE_END_OF_LIST()
    },
};

static void wm8991_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    dc->desc = "Wolfson WM8991 audio codec";
    dc->vmsd = &vmstate_wm8991;
    device_class_set_legacy_reset(dc, wm8991_reset);
    sc->event = wm8991_event;
    sc->recv = wm8991_recv;
    sc->send = wm8991_send;
}

static const TypeInfo wm8991_info = {
    .name = TYPE_WM8991,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(WM8991State),
    .class_init = wm8991_class_init,
};

static void wm8991_register_types(void)
{
    type_register_static(&wm8991_info);
}

type_init(wm8991_register_types)
