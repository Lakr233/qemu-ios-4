/*
 * Apple S5L8900 clock controllers
 *
 * The reset tuple is the captured S5L8900 configuration consumed by
 * OpeniBoot's clock_setup().  Clock consumers remain fixed-frequency until
 * a live iPhone1,2 producer trace proves their dynamic gate/divider wiring.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/s5l8900_clock.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define CLOCK0_CONFIG       0x000
#define CLOCK0_ADJ1         0x008
#define CLOCK0_ADJ2         0x404

#define CLOCK1_CONFIG0      0x000
#define CLOCK1_CONFIG1      0x004
#define CLOCK1_CONFIG2      0x008
#define CLOCK1_PLL0CON      0x020
#define CLOCK1_PLL3CON      0x02c
#define CLOCK1_PLL0LCNT     0x030
#define CLOCK1_PLL3LCNT     0x03c
#define CLOCK1_PLLLOCK      0x040
#define CLOCK1_PLLMODE      0x044
#define CLOCK1_CL2_GATES    0x048
#define CLOCK1_CL3_GATES    0x04c

#define CLOCK1_PLL_LOCKED   0x0000000f

static uint64_t s5l8900_clock0_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    S5L8900ClockState *s = opaque;

    switch (offset) {
    case CLOCK0_CONFIG:
        return s->clock0_config;
    case CLOCK0_ADJ1:
        return s->clock0_adj1;
    case CLOCK0_ADJ2:
        return s->clock0_adj2;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.clock0: unimplemented read at 0x%03"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void s5l8900_clock0_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    S5L8900ClockState *s = opaque;

    switch (offset) {
    case CLOCK0_CONFIG:
        s->clock0_config = value;
        break;
    case CLOCK0_ADJ1:
        s->clock0_adj1 = value;
        break;
    case CLOCK0_ADJ2:
        s->clock0_adj2 = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.clock0: unimplemented write 0x%08" PRIx64
                      " at 0x%03" HWADDR_PRIx "\n", value, offset);
        break;
    }
}

static uint64_t s5l8900_clock1_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    S5L8900ClockState *s = opaque;

    if (offset <= CLOCK1_CONFIG2) {
        return s->config[offset / sizeof(uint32_t)];
    }
    if (offset >= CLOCK1_PLL0CON && offset <= CLOCK1_PLL3CON) {
        return s->pll_con[(offset - CLOCK1_PLL0CON) / sizeof(uint32_t)];
    }
    if (offset >= CLOCK1_PLL0LCNT && offset <= CLOCK1_PLL3LCNT) {
        return s->pll_lcnt[(offset - CLOCK1_PLL0LCNT) / sizeof(uint32_t)];
    }

    switch (offset) {
    case CLOCK1_PLLLOCK:
        return CLOCK1_PLL_LOCKED;
    case CLOCK1_PLLMODE:
        return s->pll_mode;
    case CLOCK1_CL2_GATES:
        return s->gates[0];
    case CLOCK1_CL3_GATES:
        return s->gates[1];
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.clock1: unimplemented read at 0x%03"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void s5l8900_clock1_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    S5L8900ClockState *s = opaque;

    if (offset <= CLOCK1_CONFIG2) {
        s->config[offset / sizeof(uint32_t)] = value;
        return;
    }
    if (offset >= CLOCK1_PLL0CON && offset <= CLOCK1_PLL3CON) {
        s->pll_con[(offset - CLOCK1_PLL0CON) / sizeof(uint32_t)] = value;
        return;
    }
    if (offset >= CLOCK1_PLL0LCNT && offset <= CLOCK1_PLL3LCNT) {
        s->pll_lcnt[(offset - CLOCK1_PLL0LCNT) / sizeof(uint32_t)] = value;
        return;
    }

    switch (offset) {
    case CLOCK1_PLLMODE:
        s->pll_mode = value;
        break;
    case CLOCK1_CL2_GATES:
        s->gates[0] = value;
        break;
    case CLOCK1_CL3_GATES:
        s->gates[1] = value;
        break;
    case CLOCK1_PLLLOCK:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.clock1: PLLLOCK is read-only\n");
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.clock1: unimplemented write 0x%08" PRIx64
                      " at 0x%03" HWADDR_PRIx "\n", value, offset);
        break;
    }
}

static const MemoryRegionOps s5l8900_clock0_ops = {
    .read = s5l8900_clock0_read,
    .write = s5l8900_clock0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static const MemoryRegionOps s5l8900_clock1_ops = {
    .read = s5l8900_clock1_read,
    .write = s5l8900_clock1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_clock_reset(DeviceState *dev)
{
    S5L8900ClockState *s = S5L8900_CLOCK(dev);

    s->clock0_config = 0;
    s->clock0_adj1 = 0;
    s->clock0_adj2 = 0;

    s->config[0] = 0x01021000;
    s->config[1] = 0x51135103;
    s->config[2] = 0x31010000;
    s->pll_con[0] = 0x08005000;
    s->pll_con[1] = 0x06006700;
    s->pll_con[2] = 0x35009c02;
    s->pll_con[3] = 0x08004801;
    memset(s->pll_lcnt, 0, sizeof(s->pll_lcnt));
    s->pll_mode = 0x000a003a;
    memset(s->gates, 0, sizeof(s->gates));
}

static const VMStateDescription vmstate_s5l8900_clock = {
    .name = TYPE_S5L8900_CLOCK,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(clock0_config, S5L8900ClockState),
        VMSTATE_UINT32(clock0_adj1, S5L8900ClockState),
        VMSTATE_UINT32(clock0_adj2, S5L8900ClockState),
        VMSTATE_UINT32_ARRAY(config, S5L8900ClockState, 3),
        VMSTATE_UINT32_ARRAY(pll_con, S5L8900ClockState, 4),
        VMSTATE_UINT32_ARRAY(pll_lcnt, S5L8900ClockState, 4),
        VMSTATE_UINT32(pll_mode, S5L8900ClockState),
        VMSTATE_UINT32_ARRAY(gates, S5L8900ClockState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8900_clock_init(Object *obj)
{
    S5L8900ClockState *s = S5L8900_CLOCK(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->clock0_iomem, obj, &s5l8900_clock0_ops, s,
                          "s5l8900.clock0", 0x1000);
    memory_region_init_io(&s->clock1_iomem, obj, &s5l8900_clock1_ops, s,
                          "s5l8900.clock1", 0x1000);
    sysbus_init_mmio(sbd, &s->clock0_iomem);
    sysbus_init_mmio(sbd, &s->clock1_iomem);
}

static void s5l8900_clock_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Apple S5L8900 clock controllers";
    dc->vmsd = &vmstate_s5l8900_clock;
    device_class_set_legacy_reset(dc, s5l8900_clock_reset);
}

static const TypeInfo s5l8900_clock_info = {
    .name = TYPE_S5L8900_CLOCK,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900ClockState),
    .instance_init = s5l8900_clock_init,
    .class_init = s5l8900_clock_class_init,
};

static void s5l8900_clock_register_types(void)
{
    type_register_static(&s5l8900_clock_info);
}

type_init(s5l8900_clock_register_types)
