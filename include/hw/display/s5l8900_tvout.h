/*
 * Apple S5L8900 TV-out register apertures
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_S5L8900_TVOUT_H
#define HW_DISPLAY_S5L8900_TVOUT_H

#include "hw/core/sysbus.h"

#define TYPE_S5L8900_TVOUT "s5l8900-tvout"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900TVOutState, S5L8900_TVOUT)

#define S5L8900_TVOUT_BANK_COUNT 3
#define S5L8900_TVOUT_BANK_SIZE  0x1000
#define S5L8900_TVOUT_REGISTER_COUNT \
    (S5L8900_TVOUT_BANK_SIZE / sizeof(uint32_t))

typedef struct S5L8900TVOutBank {
    MemoryRegion iomem;
    S5L8900TVOutState *owner;
    unsigned index;
} S5L8900TVOutBank;

struct S5L8900TVOutState {
    SysBusDevice parent_obj;

    S5L8900TVOutBank bank[S5L8900_TVOUT_BANK_COUNT];
    qemu_irq irq;
    uint32_t regs[S5L8900_TVOUT_BANK_COUNT]
                 [S5L8900_TVOUT_REGISTER_COUNT];
};

#endif /* HW_DISPLAY_S5L8900_TVOUT_H */
