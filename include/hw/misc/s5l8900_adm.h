/*
 * Apple S5L8900 Apple Data Mover
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_S5L8900_ADM_H
#define HW_MISC_S5L8900_ADM_H

#include "hw/block/s5l8900_nand.h"
#include "hw/core/sysbus.h"
#include "hw/dma/pl080.h"
#include "qom/object.h"

#define TYPE_S5L8900_ADM "s5l8900-adm"
#define S5L8900_ADM_FMC_MAX_PAGES 512
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900ADMState, S5L8900_ADM)

struct S5L8900ADMState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion *dma_memory;
    AddressSpace dma_as;
    S5L8900NANDState *nand;
    PL080State *dmac;
    qemu_irq irq;
    QEMUTimer *completion_timer;
    QEMUTimer *dma_completion_timer;

    uint32_t control;
    uint32_t irq_status;
    uint32_t dma_direction;
    uint32_t upload_data[8];
    uint32_t event_data[8];
    uint32_t upload_action[8];
    uint32_t event_action[4];
    uint32_t reg90;
    uint32_t reg94;
    uint32_t reg9c;

    bool fmc_event_armed;
    bool fmc_run_pending;
    bool fmc_completion_is_write;
    bool fmc_write_prepared;
    uint16_t fmc_write_opcode;
    uint16_t fmc_write_page_count;
    uint16_t fmc_write_pad_size;
    uint32_t fmc_write_command_address;
    uint32_t fmc_write_data_size;
    uint8_t *fmc_write_data;
    uint8_t fmc_write_banks[S5L8900_ADM_FMC_MAX_PAGES];
    uint32_t fmc_write_pages[S5L8900_ADM_FMC_MAX_PAGES];
};

#endif
