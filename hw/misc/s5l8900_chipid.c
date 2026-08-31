/*
 * Apple S5L8900 chip identification registers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/s5l8900_chipid.h"
#include "hw/core/qdev-properties.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define CHIPID_INFO           0x000
#define CHIPID_SPI_CLOCK_TYPE 0x004
#define CHIPID_SECURITY_INFO  0x008
#define CHIPID_REVISION_2     0x02000000
#define CHIPID_DEVICE_ID      (0x8900U << 16)
#define CHIPID_SECURITY_DOMAIN_1 BIT(8)

static uint64_t s5l8900_chipid_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    S5L8900ChipIDState *s = opaque;

    switch (offset) {
    case CHIPID_INFO:
        return 0;
    case CHIPID_SPI_CLOCK_TYPE:
        return CHIPID_REVISION_2;
    case CHIPID_SECURITY_INFO:
        return CHIPID_DEVICE_ID | CHIPID_SECURITY_DOMAIN_1 |
               s->security_epoch;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.chipid: unimplemented read at 0x%03"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static const Property s5l8900_chipid_properties[] = {
    DEFINE_PROP_UINT8("security-epoch", S5L8900ChipIDState,
                      security_epoch, 5),
};

static const MemoryRegionOps s5l8900_chipid_ops = {
    .read = s5l8900_chipid_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_chipid_init(Object *obj)
{
    S5L8900ChipIDState *s = S5L8900_CHIPID(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_chipid_ops, s,
                          "s5l8900.chipid", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void s5l8900_chipid_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Apple S5L8900 chip identification";
    device_class_set_props(dc, s5l8900_chipid_properties);
}

static const TypeInfo s5l8900_chipid_info = {
    .name = TYPE_S5L8900_CHIPID,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900ChipIDState),
    .instance_init = s5l8900_chipid_init,
    .class_init = s5l8900_chipid_class_init,
};

static void s5l8900_chipid_register_types(void)
{
    type_register_static(&s5l8900_chipid_info);
}

type_init(s5l8900_chipid_register_types)
