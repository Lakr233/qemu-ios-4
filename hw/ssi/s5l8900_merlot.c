/*
 * iPhone 3G Merlot LCD panel and National Semiconductor LM2512
 *
 * The N82 bootloader reads the panel's three-byte identity with DCS commands
 * DA, DB, and DC.  Both endpoints otherwise expose the byte-addressed control
 * register path consumed during display initialization.
 *
 * Copyright (c) 2026 QEMU iPhone contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/s5l8900_merlot.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "trace.h"

#define MERLOT_ID_MANUFACTURER 0xda
#define MERLOT_ID_TYPE         0xdb
#define MERLOT_ID_REVISION     0xdc
#define MERLOT_ENTER_REGISTER_MODE 0xde
#define MERLOT_REGISTER_READ       BIT(7)
#define MERLOT_REGISTER_ADDRESS    0x7f
#define MERLOT_STATUS              0x15
#define MERLOT_STATUS_READY        BIT(0)

static uint8_t merlot_identity(S5L8900MerlotState *s, uint8_t command)
{
    return (s->panel_id >> ((command - MERLOT_ID_MANUFACTURER) * 8)) & 0xff;
}

static uint32_t merlot_transfer(SSIPeripheral *peripheral, uint32_t value)
{
    S5L8900MerlotState *s = S5L8900_MERLOT(peripheral);
    S5L8900MerlotClass *smc = S5L8900_MERLOT_GET_CLASS(s);
    uint8_t byte = value;
    uint8_t command = s->have_command ? s->command : byte;
    uint8_t response = 0;

    if (!smc->serializer && s->have_command &&
        s->command >= MERLOT_ID_MANUFACTURER &&
        s->command <= MERLOT_ID_REVISION) {
        response = merlot_identity(s, s->command);
        s->have_command = false;
    } else if (!smc->serializer && s->have_command &&
               s->command & MERLOT_REGISTER_READ) {
        response = s->registers[s->command & MERLOT_REGISTER_ADDRESS];
        s->have_command = false;
    } else if (!s->have_command) {
        s->command = byte;
        s->have_command = true;
    } else {
        s->registers[s->command++] = byte;
    }

    trace_s5l8900_merlot_transfer(smc->serializer, command, byte, response);
    return response;
}

static int merlot_set_cs(SSIPeripheral *peripheral, bool deasserted)
{
    S5L8900MerlotState *s = S5L8900_MERLOT(peripheral);
    S5L8900MerlotClass *smc = S5L8900_MERLOT_GET_CLASS(s);

    if (deasserted) {
        if (!smc->serializer && s->have_command &&
            s->command == MERLOT_ENTER_REGISTER_MODE) {
            s->registers[MERLOT_STATUS] |= MERLOT_STATUS_READY;
        }
        s->have_command = false;
    }
    return 0;
}

static void merlot_reset(DeviceState *dev)
{
    S5L8900MerlotState *s = S5L8900_MERLOT(dev);

    memset(s->registers, 0, sizeof(s->registers));
    s->command = 0;
    s->have_command = false;
}

static const VMStateDescription vmstate_merlot = {
    .name = TYPE_S5L8900_MERLOT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, S5L8900MerlotState),
        VMSTATE_UINT8_ARRAY(registers, S5L8900MerlotState, 256),
        VMSTATE_UINT8(command, S5L8900MerlotState),
        VMSTATE_BOOL(have_command, S5L8900MerlotState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property merlot_properties[] = {
    DEFINE_PROP_UINT32("panel-id", S5L8900MerlotState, panel_id, 0x00b3c21a),
};

static void merlot_realize(SSIPeripheral *peripheral, Error **errp)
{
    (void)peripheral;
    (void)errp;
}

static void merlot_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SSIPeripheralClass *ssc = SSI_PERIPHERAL_CLASS(oc);

    dc->vmsd = &vmstate_merlot;
    device_class_set_legacy_reset(dc, merlot_reset);
    device_class_set_props(dc, merlot_properties);
    ssc->realize = merlot_realize;
    ssc->transfer = merlot_transfer;
    ssc->set_cs = merlot_set_cs;
    ssc->cs_polarity = SSI_CS_LOW;
}

static void merlot_panel_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Apple N82 Merlot LCD panel";
}

static void lm2512_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    S5L8900MerlotClass *smc = S5L8900_MERLOT_CLASS(oc);

    dc->desc = "National Semiconductor LM2512 serializer";
    smc->serializer = true;
}

static const TypeInfo merlot_types[] = {
    {
        .name = TYPE_S5L8900_MERLOT,
        .parent = TYPE_SSI_PERIPHERAL,
        .instance_size = sizeof(S5L8900MerlotState),
        .class_size = sizeof(S5L8900MerlotClass),
        .class_init = merlot_class_init,
        .abstract = true,
    }, {
        .name = TYPE_S5L8900_MERLOT_PANEL,
        .parent = TYPE_S5L8900_MERLOT,
        .class_init = merlot_panel_class_init,
    }, {
        .name = TYPE_S5L8900_LM2512,
        .parent = TYPE_S5L8900_MERLOT,
        .class_init = lm2512_class_init,
    },
};

DEFINE_TYPES(merlot_types)
