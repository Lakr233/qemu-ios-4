/*
 * Apple S5L8900 TV-out register apertures
 *
 * N82AP publishes three ordered 4 KiB apertures for its tv-out node.  The
 * initial model preserves that mapping and register state without assigning
 * unproven encoder, mixer, or interrupt semantics to the individual banks.
 *
 * Copyright (c) 2026 QEMU iPhone contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/display/s5l8900_tvout.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

static uint64_t s5l8900_tvout_read(void *opaque, hwaddr offset,
                                   unsigned size)
{
    S5L8900TVOutBank *bank = opaque;

    return bank->owner->regs[bank->index][offset / sizeof(uint32_t)];
}

static void s5l8900_tvout_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    S5L8900TVOutBank *bank = opaque;

    bank->owner->regs[bank->index][offset / sizeof(uint32_t)] = value;
}

static const MemoryRegionOps s5l8900_tvout_ops = {
    .read = s5l8900_tvout_read,
    .write = s5l8900_tvout_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_tvout_reset(DeviceState *dev)
{
    S5L8900TVOutState *s = S5L8900_TVOUT(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static const VMStateDescription vmstate_s5l8900_tvout = {
    .name = TYPE_S5L8900_TVOUT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_2DARRAY(regs, S5L8900TVOutState,
                               S5L8900_TVOUT_BANK_COUNT,
                               S5L8900_TVOUT_REGISTER_COUNT),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8900_tvout_realize(DeviceState *dev, Error **errp)
{
    static const char *const bank_names[S5L8900_TVOUT_BANK_COUNT] = {
        "s5l8900-tvout.bank0",
        "s5l8900-tvout.bank1",
        "s5l8900-tvout.bank2",
    };
    S5L8900TVOutState *s = S5L8900_TVOUT(dev);

    for (unsigned i = 0; i < S5L8900_TVOUT_BANK_COUNT; i++) {
        s->bank[i].owner = s;
        s->bank[i].index = i;
        memory_region_init_io(&s->bank[i].iomem, OBJECT(dev),
                              &s5l8900_tvout_ops, &s->bank[i], bank_names[i],
                              S5L8900_TVOUT_BANK_SIZE);
        sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->bank[i].iomem);
    }
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void s5l8900_tvout_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s5l8900_tvout_realize;
    dc->vmsd = &vmstate_s5l8900_tvout;
    dc->desc = "Apple S5L8900 TV-out register apertures";
    device_class_set_legacy_reset(dc, s5l8900_tvout_reset);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo s5l8900_tvout_info = {
    .name = TYPE_S5L8900_TVOUT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900TVOutState),
    .class_init = s5l8900_tvout_class_init,
};

static void s5l8900_tvout_register_types(void)
{
    type_register_static(&s5l8900_tvout_info);
}

type_init(s5l8900_tvout_register_types)
