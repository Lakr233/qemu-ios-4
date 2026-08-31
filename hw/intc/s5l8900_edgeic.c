/*
 * Apple S5L8900 edge interrupt controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/intc/s5l8900_edgeic.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define EDGEIC_CONFIG0      0x000
#define EDGEIC_CONFIG1      0x004
#define EDGEIC_LOW_STATUS   0x008
#define EDGEIC_HIGH_STATUS  0x00c

struct S5L8900EdgeICState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t config[2];
    uint32_t pending[2];
    uint32_t input_level[2];
    qemu_irq parent_irq[2];
};

static void s5l8900_edgeic_update(S5L8900EdgeICState *s)
{
    for (unsigned bank = 0; bank < ARRAY_SIZE(s->parent_irq); bank++) {
        qemu_set_irq(s->parent_irq[bank],
                     (s->pending[bank] & s->config[bank]) != 0);
    }
}

static void s5l8900_edgeic_set_irq(void *opaque, int irq, int level)
{
    S5L8900EdgeICState *s = opaque;
    unsigned bank = irq / 32;
    uint32_t bit = BIT(irq % 32);

    if (level && !(s->input_level[bank] & bit)) {
        s->pending[bank] |= bit;
    }
    if (level) {
        s->input_level[bank] |= bit;
    } else {
        s->input_level[bank] &= ~bit;
    }
    s5l8900_edgeic_update(s);
}

static uint64_t s5l8900_edgeic_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    S5L8900EdgeICState *s = opaque;

    switch (offset) {
    case EDGEIC_CONFIG0:
        return s->config[0];
    case EDGEIC_CONFIG1:
        return s->config[1];
    case EDGEIC_LOW_STATUS:
        return s->pending[0];
    case EDGEIC_HIGH_STATUS:
        return s->pending[1];
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.edgeic: unimplemented read at 0x%03"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void s5l8900_edgeic_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    S5L8900EdgeICState *s = opaque;

    switch (offset) {
    case EDGEIC_CONFIG0:
        s->config[0] = value;
        break;
    case EDGEIC_CONFIG1:
        s->config[1] = value;
        break;
    case EDGEIC_LOW_STATUS:
        s->pending[0] &= ~value;
        break;
    case EDGEIC_HIGH_STATUS:
        s->pending[1] &= ~value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.edgeic: unimplemented write 0x%08"
                      PRIx64 " at 0x%03" HWADDR_PRIx "\n", value, offset);
        return;
    }
    s5l8900_edgeic_update(s);
}

static const MemoryRegionOps s5l8900_edgeic_ops = {
    .read = s5l8900_edgeic_read,
    .write = s5l8900_edgeic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_edgeic_reset(DeviceState *dev)
{
    S5L8900EdgeICState *s = S5L8900_EDGEIC(dev);

    memset(s->config, 0, sizeof(s->config));
    memset(s->pending, 0, sizeof(s->pending));
    memset(s->input_level, 0, sizeof(s->input_level));
    s5l8900_edgeic_update(s);
}

static int s5l8900_edgeic_post_load(void *opaque, int version_id)
{
    s5l8900_edgeic_update(opaque);
    return 0;
}

static const VMStateDescription vmstate_s5l8900_edgeic = {
    .name = TYPE_S5L8900_EDGEIC,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8900_edgeic_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(config, S5L8900EdgeICState, 2),
        VMSTATE_UINT32_ARRAY(pending, S5L8900EdgeICState, 2),
        VMSTATE_UINT32_ARRAY(input_level, S5L8900EdgeICState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8900_edgeic_init(Object *obj)
{
    S5L8900EdgeICState *s = S5L8900_EDGEIC(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_edgeic_ops, s,
                          TYPE_S5L8900_EDGEIC, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_in(dev, s5l8900_edgeic_set_irq,
                      S5L8900_EDGEIC_NUM_IRQS);
    for (unsigned bank = 0; bank < ARRAY_SIZE(s->parent_irq); bank++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->parent_irq[bank]);
    }
}

static void s5l8900_edgeic_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->vmsd = &vmstate_s5l8900_edgeic;
    device_class_set_legacy_reset(dc, s5l8900_edgeic_reset);
}

static const TypeInfo s5l8900_edgeic_info = {
    .name = TYPE_S5L8900_EDGEIC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900EdgeICState),
    .instance_init = s5l8900_edgeic_init,
    .class_init = s5l8900_edgeic_class_init,
};

static void s5l8900_edgeic_register_types(void)
{
    type_register_static(&s5l8900_edgeic_info);
}

type_init(s5l8900_edgeic_register_types)
