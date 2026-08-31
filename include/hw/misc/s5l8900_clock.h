/*
 * Apple S5L8900 clock controllers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_S5L8900_CLOCK_H
#define HW_MISC_S5L8900_CLOCK_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_S5L8900_CLOCK "s5l8900-clock"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900ClockState, S5L8900_CLOCK)

struct S5L8900ClockState {
    SysBusDevice parent_obj;

    MemoryRegion clock0_iomem;
    MemoryRegion clock1_iomem;

    uint32_t clock0_config;
    uint32_t clock0_adj1;
    uint32_t clock0_adj2;
    uint32_t config[3];
    uint32_t pll_con[4];
    uint32_t pll_lcnt[4];
    uint32_t pll_mode;
    uint32_t gates[2];
};

#endif
