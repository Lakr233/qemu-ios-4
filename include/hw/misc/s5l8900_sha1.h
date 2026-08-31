/*
 * Apple S5L8900 SHA-1 accelerator
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_S5L8900_SHA1_H
#define HW_MISC_S5L8900_SHA1_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_S5L8900_SHA1 "s5l8900-sha1"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900SHA1State, S5L8900_SHA1)

#define S5L8900_SHA1_DIGEST_WORDS 5
#define S5L8900_SHA1_BLOCK_WORDS 16

struct S5L8900SHA1State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion *dma_memory;
    AddressSpace dma_as;
    qemu_irq irq;

    uint32_t config;
    uint32_t reset;
    uint32_t irq_enable;
    uint32_t digest[S5L8900_SHA1_DIGEST_WORDS];
    uint32_t data[S5L8900_SHA1_BLOCK_WORDS];
    uint32_t dma_control;
    uint32_t dma_address;
    uint32_t dma_length;
    bool irq_pending;
};

#endif
