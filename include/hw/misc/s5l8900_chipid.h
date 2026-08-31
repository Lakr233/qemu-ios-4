/*
 * Apple S5L8900 chip identification registers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_S5L8900_CHIPID_H
#define HW_MISC_S5L8900_CHIPID_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_S5L8900_CHIPID "s5l8900-chipid"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900ChipIDState, S5L8900_CHIPID)

struct S5L8900ChipIDState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t security_epoch;
};

#endif
