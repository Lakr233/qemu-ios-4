/*
 * Apple S5L8900 flash-memory controller
 *
 * The consumed interface is documented by OpeniBoot's S5L8900 NAND driver.
 * The selected iPhone 3G contract is the 8 GB, four-bank Toshiba
 * TH58NVG6D1DTG80 (ID bytes 98 d5 94 ba).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/block/s5l8900_nand.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

#define NAND_DEVICE_ID             0xba94d598
#define NAND_PAGES_PER_BANK        \
    (S5L8900_NAND_BLOCKS_PER_BANK * S5L8900_NAND_PAGES_PER_BLOCK)
#define NAND_BACKING_SIZE          \
    ((uint64_t)S5L8900_NAND_BANKS * NAND_PAGES_PER_BANK * \
     S5L8900_NAND_PAGE_TOTAL_SIZE)

#define FMCTRL0                    0x000
#define FMCTRL1                    0x004
#define NAND_CMD                   0x008
#define FMADDR0                    0x00c
#define FMADDR1                    0x010
#define FMANUM                     0x02c
#define FMDNUM                     0x030
#define NAND_REG_44                0x044
#define FMCSTAT                    0x048
#define FMFIFO                     0x080
#define RSCTRL                     0x100

#define FMCTRL1_TRANSFER_ADDRESS   BIT(0)
#define FMCTRL1_READ_DATA          BIT(1)
#define FMCTRL1_FLUSH_TX           BIT(6)
#define FMCTRL1_FLUSH_RX           BIT(7)
#define FMCTRL1_WRITE_DATA         0x7f4

#define FMCSTAT_READY              BIT(0)
#define FMCSTAT_COMMAND_DONE       BIT(1)
#define FMCSTAT_ADDRESS_DONE       BIT(2)
#define FMCSTAT_TRANSFER_DONE      BIT(3)
#define FMCSTAT_BANK_READY(bank)   BIT((bank) + 4)

#define NAND_CMD_READ0             0x00
#define NAND_CMD_PROGRAM0          0x80
#define NAND_CMD_PROGRAM_CONFIRM   0x10
#define NAND_CMD_READ_ID           0x90
#define NAND_CMD_READ_STATUS       0x70
#define NAND_CMD_READ_CONFIRM      0x30
#define NAND_CMD_ERASE0            0x60
#define NAND_CMD_ERASE_CONFIRM     0xd0
#define NAND_CMD_RESET             0xff

#define NANDECC_DATA               0x004
#define NANDECC_ECC                0x008
#define NANDECC_START              0x00c
#define NANDECC_STATUS             0x010
#define NANDECC_SETUP              0x014
#define NANDECC_CLEARINT           0x040

static unsigned s5l8900_nand_selected_bank(S5L8900NANDState *s)
{
    uint32_t bank_mask = extract32(s->fmctrl0, 1, 8);

    return bank_mask ? ctz32(bank_mask) : S5L8900_NAND_BANKS;
}

static void s5l8900_nand_update(S5L8900NANDState *s)
{
    qemu_set_irq(s->irq, (s->fmcstat & ~FMCSTAT_READY) != 0);
    qemu_set_irq(s->ecc_irq, s->ecc_irq_pending);
    qemu_set_irq(s->dreq,
                 s->transfer_kind != S5L8900_NAND_TRANSFER_NONE &&
                 s->transfer_remaining != 0);
}

static uint64_t s5l8900_nand_page_offset(unsigned bank, uint32_t page)
{
    return ((uint64_t)bank * NAND_PAGES_PER_BANK + page) *
           S5L8900_NAND_PAGE_TOTAL_SIZE;
}

uint32_t s5l8900_nand_device_id(S5L8900NANDState *s, unsigned bank)
{
    return s && bank < S5L8900_NAND_BANKS ? NAND_DEVICE_ID : 0;
}

bool s5l8900_nand_read_page(S5L8900NANDState *s, unsigned bank,
                            uint32_t page, uint8_t *data)
{
    if (bank >= S5L8900_NAND_BANKS || page >= NAND_PAGES_PER_BANK) {
        memset(data, 0xff, S5L8900_NAND_PAGE_TOTAL_SIZE);
        return false;
    }
    if (!s->blk) {
        memset(data, 0xff, S5L8900_NAND_PAGE_TOTAL_SIZE);
        return true;
    }

    return blk_pread(s->blk, s5l8900_nand_page_offset(bank, page),
                     S5L8900_NAND_PAGE_TOTAL_SIZE, data, 0) == 0;
}

static bool s5l8900_nand_write_page(S5L8900NANDState *s, unsigned bank,
                                    uint32_t page, const uint8_t *data)
{
    if (!s->blk || s->read_only || bank >= S5L8900_NAND_BANKS ||
        page >= NAND_PAGES_PER_BANK) {
        return false;
    }

    return blk_pwrite(s->blk, s5l8900_nand_page_offset(bank, page),
                      S5L8900_NAND_PAGE_TOTAL_SIZE, data, 0) == 0;
}

bool s5l8900_nand_program_page(S5L8900NANDState *s, unsigned bank,
                               uint32_t page, const uint8_t *data)
{
    uint8_t programmed[S5L8900_NAND_PAGE_TOTAL_SIZE];

    if (!s5l8900_nand_read_page(s, bank, page, programmed)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(programmed); i++) {
        programmed[i] &= data[i];
    }
    return s5l8900_nand_write_page(s, bank, page, programmed);
}

bool s5l8900_nand_erase_block(S5L8900NANDState *s, unsigned bank,
                              uint32_t page)
{
    uint8_t erased[S5L8900_NAND_PAGE_TOTAL_SIZE];
    uint32_t first_page;

    if (!s->blk || s->read_only || bank >= S5L8900_NAND_BANKS ||
        page >= NAND_PAGES_PER_BANK) {
        return false;
    }

    first_page = page - page % S5L8900_NAND_PAGES_PER_BLOCK;
    memset(erased, 0xff, sizeof(erased));
    for (uint32_t i = 0; i < S5L8900_NAND_PAGES_PER_BLOCK; i++) {
        if (!s5l8900_nand_write_page(s, bank, first_page + i,
                                     erased)) {
            return false;
        }
    }
    return true;
}

static void s5l8900_nand_finish_transfer(S5L8900NANDState *s)
{
    s->transfer_kind = S5L8900_NAND_TRANSFER_NONE;
    s->transfer_remaining = 0;
    s5l8900_nand_update(s);
}

static void s5l8900_nand_arm_transfer(S5L8900NANDState *s,
                                      S5L8900NANDTransferKind kind)
{
    uint64_t count = (uint64_t)s->fmdnum + 1;
    uint64_t available = 0;
    unsigned bank = s5l8900_nand_selected_bank(s);

    switch (kind) {
    case S5L8900_NAND_TRANSFER_ID:
        available = 9;
        break;
    case S5L8900_NAND_TRANSFER_STATUS:
        available = 1;
        break;
    case S5L8900_NAND_TRANSFER_PAGE_READ:
    case S5L8900_NAND_TRANSFER_PAGE_WRITE:
        available = S5L8900_NAND_PAGE_TOTAL_SIZE - s->data_cursor;
        break;
    case S5L8900_NAND_TRANSFER_NONE:
        break;
    }

    if (count > available || count > UINT32_MAX) {
        s->operation_failed = true;
        s5l8900_nand_finish_transfer(s);
        return;
    }

    s->transfer_kind = kind;
    s->transfer_remaining = count;
    if (kind == S5L8900_NAND_TRANSFER_ID) {
        trace_s5l8900_nand_id_transfer_arm(bank, s->fmdnum, count);
    }
    s->fmcstat |= FMCSTAT_TRANSFER_DONE;
    s5l8900_nand_update(s);
}

static void s5l8900_nand_transfer_address(S5L8900NANDState *s)
{
    uint32_t column = 0;

    if (s->command == NAND_CMD_ERASE0 && s->fmanum == 2) {
        s->page = s->fmaddr0;
    } else if (s->fmanum == 4) {
        s->page = extract32(s->fmaddr0, 16, 16) |
                  extract32(s->fmaddr1, 0, 8) << 16;
        column = extract32(s->fmaddr0, 0, 16);
    } else if (s->command == NAND_CMD_READ_ID && s->fmanum == 0) {
        s->page = 0;
        column = extract32(s->fmaddr0, 0, 8);
    } else {
        s->operation_failed = true;
    }

    if (column > S5L8900_NAND_PAGE_TOTAL_SIZE) {
        s->operation_failed = true;
    }
    s->data_cursor = MIN(column, (uint32_t)S5L8900_NAND_PAGE_TOTAL_SIZE);
    s->fmcstat |= FMCSTAT_ADDRESS_DONE;
    s5l8900_nand_update(s);
}

static void s5l8900_nand_program_confirm(S5L8900NANDState *s)
{
    unsigned bank = s5l8900_nand_selected_bank(s);

    if (s->page_state != S5L8900_NAND_PAGE_PROGRAM ||
        !s5l8900_nand_program_page(s, bank, s->page, s->page_data)) {
        s->operation_failed = true;
    }
    s->page_state = S5L8900_NAND_PAGE_IDLE;
}

static void s5l8900_nand_command(S5L8900NANDState *s, uint32_t command)
{
    unsigned bank = s5l8900_nand_selected_bank(s);

    s->command = command & 0xff;
    switch (s->command) {
    case NAND_CMD_READ0:
    case NAND_CMD_READ_ID:
    case NAND_CMD_READ_STATUS:
    case NAND_CMD_ERASE0:
        break;
    case NAND_CMD_PROGRAM0:
        memset(s->page_data, 0xff, sizeof(s->page_data));
        s->page_state = S5L8900_NAND_PAGE_PROGRAM;
        break;
    case NAND_CMD_READ_CONFIRM:
        if (s5l8900_nand_read_page(s, bank, s->page, s->page_data)) {
            s->page_state = S5L8900_NAND_PAGE_READ;
        } else {
            s->page_state = S5L8900_NAND_PAGE_IDLE;
            s->operation_failed = true;
        }
        break;
    case NAND_CMD_PROGRAM_CONFIRM:
        s5l8900_nand_program_confirm(s);
        break;
    case NAND_CMD_ERASE_CONFIRM:
        if (!s5l8900_nand_erase_block(s, bank, s->page)) {
            s->operation_failed = true;
        }
        break;
    case NAND_CMD_RESET:
        s->operation_failed = false;
        s->page_state = S5L8900_NAND_PAGE_IDLE;
        s5l8900_nand_finish_transfer(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.nand: unimplemented command 0x%02x\n",
                      s->command);
        s->operation_failed = true;
        break;
    }

    s->fmcstat |= FMCSTAT_READY | FMCSTAT_COMMAND_DONE;
    if (bank < S5L8900_NAND_BANKS) {
        s->fmcstat |= FMCSTAT_BANK_READY(bank);
    }
    s5l8900_nand_update(s);
}

static uint64_t s5l8900_nand_fifo_read(S5L8900NANDState *s, unsigned size)
{
    uint8_t bytes[4] = { 0xff, 0xff, 0xff, 0xff };
    unsigned consume = MIN(size, s->transfer_remaining);
    unsigned bank = s5l8900_nand_selected_bank(s);
    uint32_t index = s->fmdnum + 1 - s->transfer_remaining;
    uint32_t device_id = s5l8900_nand_device_id(s, bank);
    bool id_transfer = s->transfer_kind == S5L8900_NAND_TRANSFER_ID;
    uint32_t value;

    for (unsigned i = 0; i < consume; i++) {
        switch (s->transfer_kind) {
        case S5L8900_NAND_TRANSFER_ID:
            if (device_id) {
                bytes[i] = extract32(device_id,
                                     (s->fmdnum + 1 -
                                      s->transfer_remaining + i) % 4 * 8,
                                     8);
            } else {
                bytes[i] = 0;
            }
            break;
        case S5L8900_NAND_TRANSFER_STATUS:
            bytes[i] = BIT(6) | (s->operation_failed ? BIT(0) : 0);
            break;
        case S5L8900_NAND_TRANSFER_PAGE_READ:
            bytes[i] = s->page_data[s->data_cursor++];
            break;
        default:
            break;
        }
    }

    value = ldl_le_p(bytes);
    if (id_transfer) {
        trace_s5l8900_nand_id_fifo_read(bank, index, size, value,
                                        s->transfer_remaining - consume);
    }
    s->transfer_remaining -= consume;
    if (!s->transfer_remaining) {
        s5l8900_nand_finish_transfer(s);
    } else {
        s5l8900_nand_update(s);
    }
    return value;
}

static void s5l8900_nand_fifo_write(S5L8900NANDState *s, uint64_t value,
                                    unsigned size)
{
    uint8_t bytes[8];
    unsigned consume = MIN(size, s->transfer_remaining);

    stq_le_p(bytes, value);
    if (s->transfer_kind != S5L8900_NAND_TRANSFER_PAGE_WRITE ||
        s->page_state != S5L8900_NAND_PAGE_PROGRAM) {
        s->operation_failed = true;
        return;
    }

    for (unsigned i = 0; i < consume; i++) {
        s->page_data[s->data_cursor++] = bytes[i];
    }
    s->transfer_remaining -= consume;
    if (!s->transfer_remaining) {
        s5l8900_nand_finish_transfer(s);
    } else {
        s5l8900_nand_update(s);
    }
}

static uint64_t s5l8900_nand_read(void *opaque, hwaddr offset,
                                  unsigned size)
{
    S5L8900NANDState *s = opaque;

    switch (offset) {
    case FMCTRL0:
        return s->fmctrl0;
    case FMCTRL1:
        return s->fmctrl1;
    case NAND_CMD:
        return s->command;
    case FMADDR0:
        return s->fmaddr0;
    case FMADDR1:
        return s->fmaddr1;
    case FMANUM:
        return s->fmanum;
    case FMDNUM:
        return s->fmdnum;
    case NAND_REG_44:
        return s->reg44;
    case FMCSTAT:
        return s->fmcstat;
    case FMFIFO:
        return s5l8900_nand_fifo_read(s, size);
    case RSCTRL:
        return s->rsctrl;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.nand: unimplemented read at 0x%03" HWADDR_PRIx
                      "\n", offset);
        return 0;
    }
}

static void s5l8900_nand_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    S5L8900NANDState *s = opaque;

    switch (offset) {
    case FMCTRL0:
        s->fmctrl0 = value;
        break;
    case FMCTRL1:
        s->fmctrl1 = value;
        if (value & (FMCTRL1_FLUSH_TX | FMCTRL1_FLUSH_RX)) {
            s5l8900_nand_finish_transfer(s);
        }
        if (value & FMCTRL1_TRANSFER_ADDRESS) {
            s5l8900_nand_transfer_address(s);
        }
        if ((value & FMCTRL1_WRITE_DATA) == FMCTRL1_WRITE_DATA) {
            s5l8900_nand_arm_transfer(s,
                                      S5L8900_NAND_TRANSFER_PAGE_WRITE);
        } else if (value & FMCTRL1_READ_DATA) {
            if (s->command == NAND_CMD_READ_ID) {
                s5l8900_nand_arm_transfer(s, S5L8900_NAND_TRANSFER_ID);
            } else if (s->command == NAND_CMD_READ_STATUS) {
                s5l8900_nand_arm_transfer(s, S5L8900_NAND_TRANSFER_STATUS);
            } else if (s->page_state == S5L8900_NAND_PAGE_READ) {
                s5l8900_nand_arm_transfer(s,
                                          S5L8900_NAND_TRANSFER_PAGE_READ);
            } else {
                s->operation_failed = true;
            }
        }
        break;
    case NAND_CMD:
        s5l8900_nand_command(s, value);
        break;
    case FMADDR0:
        s->fmaddr0 = value;
        break;
    case FMADDR1:
        s->fmaddr1 = value;
        break;
    case FMANUM:
        s->fmanum = value;
        break;
    case FMDNUM:
        s->fmdnum = value;
        break;
    case NAND_REG_44:
        s->reg44 = value;
        break;
    case FMCSTAT:
        s->fmcstat &= ~((uint32_t)value & ~FMCSTAT_READY);
        s5l8900_nand_update(s);
        break;
    case FMFIFO:
        s5l8900_nand_fifo_write(s, value, size);
        break;
    case RSCTRL:
        s->rsctrl = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.nand: unimplemented write 0x%" PRIx64
                      " at 0x%03" HWADDR_PRIx "\n", value, offset);
        break;
    }
}

static const MemoryRegionOps s5l8900_nand_ops = {
    .read = s5l8900_nand_read,
    .write = s5l8900_nand_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static unsigned s5l8900_ecc_size(uint32_t setup)
{
    switch (setup & ~3U) {
    case 0:
        return 10;
    case 4:
        return 15;
    case 8:
        return 20;
    default:
        return 0;
    }
}

static void s5l8900_ecc_start(S5L8900NANDState *s, uint32_t operation)
{
    uint8_t data[4 * 512];
    uint8_t ecc[4 * 20];
    unsigned sectors = (s->ecc_setup & 3) + 1;
    unsigned ecc_size = s5l8900_ecc_size(s->ecc_setup);
    size_t data_size = sectors * 512;
    size_t code_size = sectors * ecc_size;
    MemTxResult result;

    s->ecc_start = operation;
    s->ecc_status = 0;
    if (!ecc_size || operation < 1 || operation > 2 ||
        (uint64_t)s->ecc_data + data_size > (uint64_t)UINT32_MAX + 1 ||
        (uint64_t)s->ecc_code + code_size > (uint64_t)UINT32_MAX + 1) {
        s->ecc_status = 1;
        goto done;
    }

    result = address_space_read(&s->dma_as, s->ecc_data,
                                MEMTXATTRS_UNSPECIFIED, data, data_size);
    if (result != MEMTX_OK) {
        s->ecc_status = 1;
        goto done;
    }

    if (operation == 1) {
        result = address_space_read(&s->dma_as, s->ecc_code,
                                    MEMTXATTRS_UNSPECIFIED, ecc, code_size);
        if (result != MEMTX_OK) {
            s->ecc_status = 1;
        }
    } else {
        /* An erased codeword is the one encoding that can be proven here. */
        for (size_t i = 0; i < data_size; i++) {
            if (data[i] != 0xff) {
                s->ecc_status = 1;
                goto done;
            }
        }
        memset(ecc, 0xff, code_size);
        result = address_space_write(&s->dma_as, s->ecc_code,
                                     MEMTXATTRS_UNSPECIFIED, ecc, code_size);
        if (result != MEMTX_OK) {
            s->ecc_status = 1;
        }
    }

done:
    s->ecc_irq_pending = true;
    s5l8900_nand_update(s);
}

static uint64_t s5l8900_ecc_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8900NANDState *s = opaque;

    switch (offset) {
    case NANDECC_DATA:
        return s->ecc_data;
    case NANDECC_ECC:
        return s->ecc_code;
    case NANDECC_START:
        return s->ecc_start;
    case NANDECC_STATUS:
        return s->ecc_status;
    case NANDECC_SETUP:
        return s->ecc_setup;
    default:
        return 0;
    }
}

static void s5l8900_ecc_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    S5L8900NANDState *s = opaque;

    switch (offset) {
    case NANDECC_DATA:
        s->ecc_data = value;
        break;
    case NANDECC_ECC:
        s->ecc_code = value;
        break;
    case NANDECC_START:
        s5l8900_ecc_start(s, value);
        break;
    case NANDECC_SETUP:
        s->ecc_setup = value;
        break;
    case NANDECC_CLEARINT:
        if (value & 1) {
            s->ecc_irq_pending = false;
            s5l8900_nand_update(s);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps s5l8900_ecc_ops = {
    .read = s5l8900_ecc_read,
    .write = s5l8900_ecc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_nand_reset(DeviceState *dev)
{
    S5L8900NANDState *s = S5L8900_NAND(dev);

    s->fmctrl0 = 0;
    s->fmctrl1 = 0;
    s->command = 0;
    s->fmaddr0 = 0;
    s->fmaddr1 = 0;
    s->fmanum = 0;
    s->fmdnum = 0;
    s->reg44 = 0;
    s->fmcstat = FMCSTAT_READY;
    s->rsctrl = 0;
    s->page = 0;
    s->data_cursor = 0;
    s->transfer_remaining = 0;
    s->transfer_kind = S5L8900_NAND_TRANSFER_NONE;
    s->page_state = S5L8900_NAND_PAGE_IDLE;
    s->operation_failed = false;
    memset(s->page_data, 0xff, sizeof(s->page_data));
    s->ecc_data = 0;
    s->ecc_code = 0;
    s->ecc_start = 0;
    s->ecc_status = 0;
    s->ecc_setup = 0;
    s->ecc_irq_pending = false;
    s5l8900_nand_update(s);
}

static int s5l8900_nand_post_load(void *opaque, int version_id)
{
    S5L8900NANDState *s = opaque;

    if (s->transfer_kind > S5L8900_NAND_TRANSFER_PAGE_WRITE ||
        s->page_state > S5L8900_NAND_PAGE_PROGRAM ||
        s->data_cursor > S5L8900_NAND_PAGE_TOTAL_SIZE ||
        s->transfer_remaining > S5L8900_NAND_PAGE_TOTAL_SIZE ||
        ((s->transfer_kind == S5L8900_NAND_TRANSFER_NONE) !=
         (s->transfer_remaining == 0))) {
        return -EINVAL;
    }
    s5l8900_nand_update(s);
    return 0;
}

static const VMStateDescription vmstate_s5l8900_nand = {
    .name = TYPE_S5L8900_NAND,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8900_nand_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(fmctrl0, S5L8900NANDState),
        VMSTATE_UINT32(fmctrl1, S5L8900NANDState),
        VMSTATE_UINT32(command, S5L8900NANDState),
        VMSTATE_UINT32(fmaddr0, S5L8900NANDState),
        VMSTATE_UINT32(fmaddr1, S5L8900NANDState),
        VMSTATE_UINT32(fmanum, S5L8900NANDState),
        VMSTATE_UINT32(fmdnum, S5L8900NANDState),
        VMSTATE_UINT32(reg44, S5L8900NANDState),
        VMSTATE_UINT32(fmcstat, S5L8900NANDState),
        VMSTATE_UINT32(rsctrl, S5L8900NANDState),
        VMSTATE_UINT32(page, S5L8900NANDState),
        VMSTATE_UINT32(data_cursor, S5L8900NANDState),
        VMSTATE_UINT32(transfer_remaining, S5L8900NANDState),
        VMSTATE_UINT8(transfer_kind, S5L8900NANDState),
        VMSTATE_UINT8(page_state, S5L8900NANDState),
        VMSTATE_BOOL(operation_failed, S5L8900NANDState),
        VMSTATE_UINT8_ARRAY(page_data, S5L8900NANDState,
                            S5L8900_NAND_PAGE_TOTAL_SIZE),
        VMSTATE_UINT32(ecc_data, S5L8900NANDState),
        VMSTATE_UINT32(ecc_code, S5L8900NANDState),
        VMSTATE_UINT32(ecc_start, S5L8900NANDState),
        VMSTATE_UINT32(ecc_status, S5L8900NANDState),
        VMSTATE_UINT32(ecc_setup, S5L8900NANDState),
        VMSTATE_BOOL(ecc_irq_pending, S5L8900NANDState),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8900_nand_realize(DeviceState *dev, Error **errp)
{
    S5L8900NANDState *s = S5L8900_NAND(dev);

    if (!s->dma_memory) {
        error_setg(errp, TYPE_S5L8900_NAND " 'dma-memory' link not set");
        return;
    }
    address_space_init(&s->dma_as, s->dma_memory, "s5l8900-nand-dma");

    if (s->blk) {
        int64_t length = blk_getlength(s->blk);
        uint64_t permissions;

        if (length != NAND_BACKING_SIZE) {
            error_setg(errp, TYPE_S5L8900_NAND
                       " backing size must be exactly %" PRIu64
                       " bytes, got %" PRId64,
                       NAND_BACKING_SIZE, length);
            return;
        }
        s->read_only = !blk_supports_write_perm(s->blk);
        permissions = BLK_PERM_CONSISTENT_READ |
                      (s->read_only ? 0 : BLK_PERM_WRITE);
        if (blk_set_perm(s->blk, permissions, BLK_PERM_ALL, errp) < 0) {
            return;
        }
    }
}

static const Property s5l8900_nand_properties[] = {
    DEFINE_PROP_LINK("dma-memory", S5L8900NANDState, dma_memory,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_DRIVE("drive", S5L8900NANDState, blk),
};

static void s5l8900_nand_init(Object *obj)
{
    S5L8900NANDState *s = S5L8900_NAND(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->nand_iomem, obj, &s5l8900_nand_ops, s,
                          "s5l8900.nand", 0x1000);
    memory_region_init_io(&s->ecc_iomem, obj, &s5l8900_ecc_ops, s,
                          "s5l8900.nand-ecc", 0x1000);
    sysbus_init_mmio(sbd, &s->nand_iomem);
    sysbus_init_mmio(sbd, &s->ecc_iomem);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->ecc_irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->dreq, "dreq", 1);
}

static void s5l8900_nand_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Apple S5L8900 flash-memory controller";
    dc->realize = s5l8900_nand_realize;
    dc->vmsd = &vmstate_s5l8900_nand;
    device_class_set_props(dc, s5l8900_nand_properties);
    device_class_set_legacy_reset(dc, s5l8900_nand_reset);
}

static const TypeInfo s5l8900_nand_info = {
    .name = TYPE_S5L8900_NAND,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900NANDState),
    .instance_init = s5l8900_nand_init,
    .class_init = s5l8900_nand_class_init,
};

static void s5l8900_nand_register_types(void)
{
    type_register_static(&s5l8900_nand_info);
}

type_init(s5l8900_nand_register_types)
