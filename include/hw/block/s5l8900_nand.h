/*
 * Apple S5L8900 flash-memory controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_BLOCK_S5L8900_NAND_H
#define HW_BLOCK_S5L8900_NAND_H

#include "hw/core/sysbus.h"
#include "system/block-backend.h"
#include "qom/object.h"

#define TYPE_S5L8900_NAND "s5l8900-nand"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900NANDState, S5L8900_NAND)

#define S5L8900_NAND_BANKS             4
#define S5L8900_NAND_BLOCKS_PER_BANK   4096
#define S5L8900_NAND_PAGES_PER_BLOCK   128
#define S5L8900_NAND_PAGE_DATA_SIZE    4096
#define S5L8900_NAND_PAGE_SPARE_SIZE   216
#define S5L8900_NAND_PAGE_TOTAL_SIZE   \
    (S5L8900_NAND_PAGE_DATA_SIZE + S5L8900_NAND_PAGE_SPARE_SIZE)

typedef enum S5L8900NANDTransferKind {
    S5L8900_NAND_TRANSFER_NONE,
    S5L8900_NAND_TRANSFER_ID,
    S5L8900_NAND_TRANSFER_STATUS,
    S5L8900_NAND_TRANSFER_PAGE_READ,
    S5L8900_NAND_TRANSFER_PAGE_WRITE,
} S5L8900NANDTransferKind;

typedef enum S5L8900NANDPageState {
    S5L8900_NAND_PAGE_IDLE,
    S5L8900_NAND_PAGE_READ,
    S5L8900_NAND_PAGE_PROGRAM,
} S5L8900NANDPageState;

struct S5L8900NANDState {
    SysBusDevice parent_obj;

    MemoryRegion nand_iomem;
    MemoryRegion ecc_iomem;
    MemoryRegion *dma_memory;
    AddressSpace dma_as;
    BlockBackend *blk;
    qemu_irq irq;
    qemu_irq ecc_irq;
    qemu_irq dreq;

    uint32_t fmctrl0;
    uint32_t fmctrl1;
    uint32_t command;
    uint32_t fmaddr0;
    uint32_t fmaddr1;
    uint32_t fmanum;
    uint32_t fmdnum;
    uint32_t reg44;
    uint32_t fmcstat;
    uint32_t rsctrl;

    uint32_t page;
    uint32_t data_cursor;
    uint32_t transfer_remaining;
    uint8_t transfer_kind;
    uint8_t page_state;
    bool operation_failed;
    bool read_only;
    uint8_t page_data[S5L8900_NAND_PAGE_TOTAL_SIZE];

    uint32_t ecc_data;
    uint32_t ecc_code;
    uint32_t ecc_start;
    uint32_t ecc_status;
    uint32_t ecc_setup;
    bool ecc_irq_pending;
};

bool s5l8900_nand_read_page(S5L8900NANDState *s, unsigned bank,
                            uint32_t page, uint8_t *data);
uint32_t s5l8900_nand_device_id(S5L8900NANDState *s, unsigned bank);
bool s5l8900_nand_program_page(S5L8900NANDState *s, unsigned bank,
                               uint32_t page, const uint8_t *data);
bool s5l8900_nand_erase_block(S5L8900NANDState *s, unsigned bank,
                              uint32_t page);

#endif
