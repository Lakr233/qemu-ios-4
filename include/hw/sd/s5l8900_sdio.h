/*
 * Apple S5L8900X SDIO controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SD_S5L8900_SDIO_H
#define HW_SD_S5L8900_SDIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_S5L8900_SDIO "s5l8900-sdio"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900SDIOState, S5L8900_SDIO)

struct S5L8900SDIOState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t ctrl;
    uint32_t dctrl;
    uint32_t command;
    uint32_t argument;
    uint32_t dsta;
    uint32_t responses[4];
    uint32_t clkdiv;
    uint32_t csr;
    uint32_t irq_status;
    uint32_t irq_mask;
    uint32_t buffer_address;
    uint32_t block_length;
    uint32_t block_count;
};

#endif
