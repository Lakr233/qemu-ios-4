/*
 * Apple S5L8900 Apple Data Mover
 *
 * The register contract is taken from the iOS 4.2.1 N82
 * AppleS5L8900XADM consumer.  The selected FMC firmware's wire protocol is
 * interpreted at its action boundary instead of executing the private ADM
 * instruction set.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/s5l8900_adm.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "trace.h"

#define ADM_CONTROL                 0x00
#define ADM_COMMAND                 0x04
#define ADM_UPLOAD_DATA_BASE        0x10
#define ADM_EVENT_DATA_BASE         0x30
#define ADM_UPLOAD_ACTION_0         0x50
#define ADM_EVENT_ACTION_0          0x54
#define ADM_UPLOAD_ACTION_BASE      0x64
#define ADM_EVENT_ACTION_BASE       0x80
#define ADM_REG_90                  0x90
#define ADM_REG_94                  0x94
#define ADM_REG_9C                  0x9c

#define ADM_FMC_COMMAND_OFFSET      0x1104
#define ADM_FMC_OPCODE              0x024
#define ADM_FMC_PAGE_COUNT          0x028
#define ADM_FMC_PAD_SIZE            0x02c
#define ADM_FMC_CE_COUNT            0x030
#define ADM_FMC_FLAGS               0x03c
#define ADM_FMC_ERASE_BANK          0x034
#define ADM_FMC_BANKS               0x044
#define ADM_FMC_PAGES               0x244
#define ADM_FMC_DESCRIPTORS         0xa44
#define ADM_FMC_DESCRIPTOR_SIZE     8
#define ADM_FMC_MAX_DESCRIPTORS     512
#define ADM_FMC_MAX_PAGES           S5L8900_ADM_FMC_MAX_PAGES
#define ADM_FMC_BANK_SLOTS          8
#define ADM_FMC_ACTION3_SIZE        0x8000
#define ADM_FMC_COMMAND_SPAN        \
    (ADM_FMC_DESCRIPTORS + (ADM_FMC_MAX_DESCRIPTORS + 1) * \
     ADM_FMC_DESCRIPTOR_SIZE)
#define ADM_FMC_INIT                0x100
#define ADM_FMC_READ                0x200
#define ADM_FMC_READ_MAX_ECC        0x300
#define ADM_FMC_WRITE               0x400
#define ADM_FMC_WRITE_MAX_ECC       0x500
#define ADM_FMC_ERASE               0x600
#define ADM_FMC_COMMAND_RESULT_OK           0
#define ADM_FMC_COMMAND_RESULT_BAD_ARGUMENT 2
#define ADM_FMC_MEDIA_RESULT_OK             0
#define ADM_FMC_READ_RESULT_OK              0
#define ADM_FMC_READ_RESULT_EMPTY           0xfe
/* Backing I/O is synchronous; these timers only separate notification. */
#define ADM_FMC_COMPLETION_NS       1000
#define ADM_FMC_DMA_COMPLETION_NS   1000
#define ADM_FMC_DMA_ARM_GRACE_NS    1000000
#define ADM_FMC_DMA_SEGMENT_NS      1000000
#define ADM_FMC_DMA_REQUEST         2

#define ADM_DMA_NONE                0
#define ADM_DMA_MEMORY_TO_PERIPHERAL 1
#define ADM_DMA_PERIPHERAL_TO_MEMORY 2

#define ADM_CONTROL_RUNNING         BIT(0)
#define ADM_CONTROL_READY           BIT(1)
#define ADM_CONTROL_SOFT_RESET      BIT(2)
#define ADM_IRQ_EVENT               BIT(4)
#define ADM_IRQ_COMMAND             BIT(5)
#define ADM_IRQ_UPLOAD              BIT(6)
#define ADM_IRQ_MASK                (ADM_IRQ_EVENT | ADM_IRQ_COMMAND | \
                                     ADM_IRQ_UPLOAD)

#define ADM_COMMAND_UPLOAD_DATA     0
#define ADM_COMMAND_ARM_EVENT       1
#define ADM_COMMAND_RUN             2
#define ADM_COMMAND_ACK_EVENT       4
#define ADM_COMMAND_ACK_COMMAND     5
#define ADM_COMMAND_ACK_UPLOAD      6

static void s5l8900_adm_update(S5L8900ADMState *s)
{
    qemu_set_irq(s->irq, (s->irq_status & s->control & ADM_IRQ_MASK) != 0);
}

static void s5l8900_adm_clear_prepared_write(S5L8900ADMState *s)
{
    g_free(s->fmc_write_data);
    s->fmc_write_prepared = false;
    s->fmc_write_opcode = 0;
    s->fmc_write_page_count = 0;
    s->fmc_write_pad_size = 0;
    s->fmc_write_command_address = 0;
    s->fmc_write_data_size = 0;
    s->fmc_write_data = NULL;
    memset(s->fmc_write_banks, 0, sizeof(s->fmc_write_banks));
    memset(s->fmc_write_pages, 0, sizeof(s->fmc_write_pages));
}

static void s5l8900_adm_soft_reset(S5L8900ADMState *s)
{
    timer_del(s->completion_timer);
    timer_del(s->dma_completion_timer);
    s->control = 0;
    s->irq_status = 0;
    s->dma_direction = ADM_DMA_NONE;
    s->fmc_event_armed = false;
    s->fmc_run_pending = false;
    s->fmc_completion_is_write = false;
    memset(s->upload_data, 0, sizeof(s->upload_data));
    memset(s->event_data, 0, sizeof(s->event_data));
    memset(s->upload_action, 0, sizeof(s->upload_action));
    memset(s->event_action, 0, sizeof(s->event_action));
    s->reg90 = 0;
    s->reg94 = 0;
    s->reg9c = 0;
    s5l8900_adm_clear_prepared_write(s);
    s5l8900_adm_update(s);
}

static bool s5l8900_adm_dma_read(S5L8900ADMState *s, uint32_t address,
                                 void *data, size_t size)
{
    if ((uint64_t)address + size > (uint64_t)UINT32_MAX + 1) {
        return false;
    }
    return address_space_read(&s->dma_as, address, MEMTXATTRS_UNSPECIFIED,
                              data, size) == MEMTX_OK;
}

static bool s5l8900_adm_dma_write(S5L8900ADMState *s, uint32_t address,
                                  const void *data, size_t size)
{
    if ((uint64_t)address + size > (uint64_t)UINT32_MAX + 1) {
        return false;
    }
    return address_space_write(&s->dma_as, address, MEMTXATTRS_UNSPECIFIED,
                               data, size) == MEMTX_OK;
}

static bool s5l8900_adm_load_u16(S5L8900ADMState *s, uint32_t address,
                                 uint16_t *value)
{
    uint8_t bytes[2];

    if (!s5l8900_adm_dma_read(s, address, bytes, sizeof(bytes))) {
        return false;
    }
    *value = lduw_le_p(bytes);
    return true;
}

static bool s5l8900_adm_load_u32(S5L8900ADMState *s, uint32_t address,
                                 uint32_t *value)
{
    uint8_t bytes[4];

    if (!s5l8900_adm_dma_read(s, address, bytes, sizeof(bytes))) {
        return false;
    }
    *value = ldl_le_p(bytes);
    return true;
}

static void s5l8900_adm_fmc_result(S5L8900ADMState *s,
                                   uint32_t command_address,
                                   uint16_t command_result,
                                   uint16_t media_result,
                                   uint16_t read_result)
{
    uint8_t bytes[4];

    stw_le_p(bytes, command_result);
    stw_le_p(bytes + 2, media_result);
    /*
     * CalmADMFMC overlays the input page count with the command and media
     * results.  The driver receives all four result halfwords through the
     * ADM event-data registers before acknowledging the event.
     */
    s5l8900_adm_dma_write(s, command_address + ADM_FMC_PAGE_COUNT,
                          bytes, sizeof(bytes));
    s->event_data[0] = command_result;
    s->event_data[1] = media_result;
    s->event_data[2] = read_result;
    s->event_data[3] = 0;
}

typedef struct S5L8900ADMDescriptorCursor {
    uint32_t command_address;
    uint32_t address;
    uint32_t remaining;
    unsigned index;
} S5L8900ADMDescriptorCursor;

static bool s5l8900_adm_next_descriptor(S5L8900ADMState *s,
                                        S5L8900ADMDescriptorCursor *cursor)
{
    uint32_t offset;
    uint32_t address;
    uint32_t length;

    if (cursor->index >= ADM_FMC_MAX_DESCRIPTORS) {
        return false;
    }
    offset = cursor->command_address + ADM_FMC_DESCRIPTORS +
             cursor->index * ADM_FMC_DESCRIPTOR_SIZE;
    if (!s5l8900_adm_load_u32(s, offset, &address) ||
        !s5l8900_adm_load_u32(s, offset + 4, &length)) {
        return false;
    }
    address = bswap32(address);
    length = bswap32(length);
    if (!length || (uint64_t)address + length > (uint64_t)UINT32_MAX + 1) {
        return false;
    }

    cursor->address = address;
    cursor->remaining = length;
    cursor->index++;
    return true;
}

static bool s5l8900_adm_write_descriptors(
    S5L8900ADMState *s, S5L8900ADMDescriptorCursor *cursor,
    const uint8_t *data, size_t size)
{
    while (size) {
        size_t chunk;

        if (!cursor->remaining && !s5l8900_adm_next_descriptor(s, cursor)) {
            return false;
        }
        chunk = MIN(size, cursor->remaining);
        if (!s5l8900_adm_dma_write(s, cursor->address, data, chunk)) {
            return false;
        }
        trace_s5l8900_adm_fmc_descriptor_transfer(
            "read-delivery", cursor->index - 1, cursor->address, chunk,
            chunk >= sizeof(uint32_t) ? ldl_le_p(data) : UINT32_MAX,
            chunk >= 0x400 + sizeof(uint32_t) ?
                ldl_le_p(data + 0x400) : UINT32_MAX,
            chunk >= 0x404 + sizeof(uint32_t) ?
                ldl_le_p(data + 0x404) : UINT32_MAX);
        cursor->address += chunk;
        cursor->remaining -= chunk;
        data += chunk;
        size -= chunk;
    }
    return true;
}

static bool s5l8900_adm_read_descriptors(
    S5L8900ADMState *s, S5L8900ADMDescriptorCursor *cursor,
    uint8_t *data, size_t size)
{
    while (size) {
        size_t chunk;

        if (!cursor->remaining && !s5l8900_adm_next_descriptor(s, cursor)) {
            return false;
        }
        chunk = MIN(size, cursor->remaining);
        if (!s5l8900_adm_dma_read(s, cursor->address, data, chunk)) {
            return false;
        }
        trace_s5l8900_adm_fmc_descriptor_transfer(
            "write-source", cursor->index - 1, cursor->address, chunk,
            chunk >= sizeof(uint32_t) ? ldl_le_p(data) : UINT32_MAX,
            chunk >= 0x400 + sizeof(uint32_t) ?
                ldl_le_p(data + 0x400) : UINT32_MAX,
            chunk >= 0x404 + sizeof(uint32_t) ?
                ldl_le_p(data + 0x404) : UINT32_MAX);
        cursor->address += chunk;
        cursor->remaining -= chunk;
        data += chunk;
        size -= chunk;
    }
    return true;
}

static bool s5l8900_adm_descriptors_terminated(
    S5L8900ADMState *s, S5L8900ADMDescriptorCursor *cursor,
    bool report_failure)
{
    uint32_t address;
    uint32_t length;
    uint32_t offset;

    if (!cursor->index || cursor->index > ADM_FMC_MAX_DESCRIPTORS) {
        if (report_failure) {
            trace_s5l8900_adm_fmc_descriptor_failure(
                cursor->command_address, cursor->index, "index", 0, 0);
        }
        return false;
    }
    for (unsigned i = cursor->index; i <= ADM_FMC_MAX_DESCRIPTORS; i++) {
        offset = cursor->command_address + ADM_FMC_DESCRIPTORS +
                 i * ADM_FMC_DESCRIPTOR_SIZE;
        if (!s5l8900_adm_load_u32(s, offset + 4, &length)) {
            if (report_failure) {
                trace_s5l8900_adm_fmc_descriptor_failure(
                    cursor->command_address, i, "length-read", 0, 0);
            }
            return false;
        }
        length = bswap32(length);
        if (!length) {
            return true;
        }
        if (i == ADM_FMC_MAX_DESCRIPTORS) {
            if (report_failure) {
                trace_s5l8900_adm_fmc_descriptor_failure(
                    cursor->command_address, i, "unterminated", 0, length);
            }
            return false;
        }
        if (!s5l8900_adm_load_u32(s, offset, &address)) {
            if (report_failure) {
                trace_s5l8900_adm_fmc_descriptor_failure(
                    cursor->command_address, i, "address-read", 0, length);
            }
            return false;
        }
        address = bswap32(address);
        if ((uint64_t)address + length > (uint64_t)UINT32_MAX + 1) {
            if (report_failure) {
                trace_s5l8900_adm_fmc_descriptor_failure(
                    cursor->command_address, i, "address-overflow", address,
                    length);
            }
            return false;
        }
    }
    return false;
}

static bool s5l8900_adm_load_fmc_targets(S5L8900ADMState *s,
                                         uint32_t command_address,
                                         uint16_t opcode,
                                         uint16_t page_count,
                                         uint8_t *banks,
                                         uint32_t *pages,
                                         bool report_failure)
{
    uint16_t target_count = page_count;
    uint16_t ce_count = 0;

    /* Plain opcodes carry one seed per CE; MAX_ECC opcodes carry every page. */
    if (opcode == ADM_FMC_READ || opcode == ADM_FMC_WRITE) {
        if (!s5l8900_adm_load_u16(s,
                                  command_address + ADM_FMC_CE_COUNT,
                                  &ce_count)) {
            if (report_failure) {
                trace_s5l8900_adm_fmc_target_failure(
                    command_address, 0, "ce-count-read", UINT32_MAX,
                    UINT32_MAX);
            }
            return false;
        }
        ce_count = bswap16(ce_count);
        if (!ce_count || ce_count > S5L8900_NAND_BANKS) {
            if (report_failure) {
                trace_s5l8900_adm_fmc_target_failure(
                    command_address, 0, "ce-count", ce_count,
                    UINT32_MAX);
            }
            return false;
        }
        target_count = MIN(page_count, ce_count);
    } else if (opcode != ADM_FMC_READ_MAX_ECC &&
               opcode != ADM_FMC_WRITE_MAX_ECC) {
        return false;
    }

    for (unsigned i = 0; i < target_count; i++) {
        uint32_t page;

        if (!s5l8900_adm_dma_read(s, command_address + ADM_FMC_BANKS + i,
                                  &banks[i], sizeof(banks[i]))) {
            if (report_failure) {
                trace_s5l8900_adm_fmc_target_failure(
                    command_address, i, "bank-read", UINT32_MAX, UINT32_MAX);
            }
            return false;
        }
        if (!s5l8900_adm_load_u32(s,
                                  command_address + ADM_FMC_PAGES + i * 4,
                                  &page)) {
            if (report_failure) {
                trace_s5l8900_adm_fmc_target_failure(
                    command_address, i, "page-read", banks[i], UINT32_MAX);
            }
            return false;
        }
        pages[i] = bswap32(page);
        if (banks[i] >= S5L8900_NAND_BANKS ||
            pages[i] >= S5L8900_NAND_BLOCKS_PER_BANK *
                        S5L8900_NAND_PAGES_PER_BLOCK) {
            if (report_failure) {
                trace_s5l8900_adm_fmc_target_failure(
                    command_address, i, "range", banks[i], pages[i]);
            }
            return false;
        }
    }

    for (unsigned i = target_count; i < page_count; i++) {
        unsigned seed = i % ce_count;
        uint32_t stripe = i / ce_count;

        banks[i] = banks[seed];
        if (uadd32_overflow(pages[seed], stripe, &pages[i]) ||
            pages[i] >= S5L8900_NAND_BLOCKS_PER_BANK *
                        S5L8900_NAND_PAGES_PER_BLOCK) {
            if (report_failure) {
                trace_s5l8900_adm_fmc_target_failure(
                    command_address, i, "compact-page", banks[i],
                    pages[seed]);
            }
            return false;
        }
    }
    return true;
}

static bool s5l8900_adm_fmc_page_is_empty(const uint8_t *page)
{
    const uint8_t *spare = page + S5L8900_NAND_PAGE_DATA_SIZE;
    unsigned programmed = 0;

    for (unsigned i = 0; i < S5L8900_NAND_PAGE_DATA_SIZE; i++) {
        if (page[i] != 0xff) {
            return false;
        }
    }
    /* FIL tolerates one programmed byte in an otherwise-erased spare area. */
    for (unsigned i = 0; i < S5L8900_NAND_PAGE_SPARE_SIZE; i++) {
        if (spare[i] != 0xff && ++programmed > 1) {
            return false;
        }
    }
    return true;
}

static bool s5l8900_adm_fmc_read(S5L8900ADMState *s,
                                 uint32_t command_address,
                                 uint16_t opcode,
                                 uint16_t page_count, uint16_t pad_size,
                                 bool *empty)
{
    S5L8900ADMDescriptorCursor cursor = {
        .command_address = command_address,
    };
    uint8_t banks[ADM_FMC_MAX_PAGES];
    uint32_t pages[ADM_FMC_MAX_PAGES];
    uint8_t page_data[S5L8900_NAND_PAGE_TOTAL_SIZE];

    *empty = false;
    if (!s5l8900_adm_load_fmc_targets(s, command_address, opcode, page_count,
                                      banks, pages, true)) {
        trace_s5l8900_adm_fmc_read_failure(command_address, 0, 0,
                                           "targets", 0, 0);
        return false;
    }
    for (unsigned i = 0; i < page_count; i++) {
        uint32_t pad_address;

        if (!s5l8900_nand_read_page(s->nand, banks[i], pages[i], page_data)) {
            trace_s5l8900_adm_fmc_read_failure(command_address, i,
                                               cursor.index, "nand", banks[i],
                                               pages[i]);
            return false;
        }
        *empty |= s5l8900_adm_fmc_page_is_empty(page_data);
        trace_s5l8900_adm_fmc_read_page(
            i, banks[i], pages[i], ldl_le_p(page_data),
            ldl_le_p(page_data + 0x400), ldl_le_p(page_data + 0x404),
            ldl_le_p(page_data + S5L8900_NAND_PAGE_DATA_SIZE),
            ldl_le_p(page_data + S5L8900_NAND_PAGE_DATA_SIZE + 4),
            ldl_le_p(page_data + S5L8900_NAND_PAGE_DATA_SIZE + 8));
        if (!s5l8900_adm_write_descriptors(s, &cursor, page_data,
                                           S5L8900_NAND_PAGE_DATA_SIZE)) {
            trace_s5l8900_adm_fmc_read_failure(command_address, i,
                                               cursor.index, "descriptor",
                                               banks[i], pages[i]);
            return false;
        }

        if (pad_size) {
            if (uadd32_overflow(s->event_action[3], i * pad_size,
                                &pad_address)) {
                trace_s5l8900_adm_fmc_read_failure(command_address, i,
                                                   cursor.index,
                                                   "pad-address", banks[i],
                                                   pages[i]);
                return false;
            }
            if (!s5l8900_adm_dma_write(
                    s, pad_address,
                    page_data + S5L8900_NAND_PAGE_DATA_SIZE, pad_size)) {
                trace_s5l8900_adm_fmc_read_failure(command_address, i,
                                                   cursor.index,
                                                   "pad-write", banks[i],
                                                   pages[i]);
                return false;
            }
        }
    }
    if (!s5l8900_adm_descriptors_terminated(s, &cursor, true)) {
        trace_s5l8900_adm_fmc_read_failure(command_address, page_count,
                                           cursor.index, "terminator", 0,
                                           0);
        return false;
    }
    return true;
}

static bool s5l8900_adm_load_fmc_write(S5L8900ADMState *s,
                                       uint32_t command_address,
                                       uint16_t page_count, uint16_t pad_size,
                                       uint8_t *pages, bool report_failure)
{
    S5L8900ADMDescriptorCursor cursor = {
        .command_address = command_address,
    };

    for (unsigned i = 0; i < page_count; i++) {
        uint32_t pad_address;
        uint8_t *page_data = pages + i * S5L8900_NAND_PAGE_TOTAL_SIZE;

        memset(page_data, 0xff, S5L8900_NAND_PAGE_TOTAL_SIZE);
        if (!s5l8900_adm_read_descriptors(s, &cursor, page_data,
                                          S5L8900_NAND_PAGE_DATA_SIZE)) {
            if (report_failure) {
                trace_s5l8900_adm_fmc_write_failure(
                    command_address, i, cursor.index, "descriptor", 0, 0);
            }
            return false;
        }
        if (pad_size) {
            if (uadd32_overflow(s->event_action[3], i * pad_size,
                                &pad_address)) {
                if (report_failure) {
                    trace_s5l8900_adm_fmc_write_failure(
                        command_address, i, cursor.index, "pad-address", 0,
                        0);
                }
                return false;
            }
            if (!s5l8900_adm_dma_read(
                    s, pad_address,
                    page_data + S5L8900_NAND_PAGE_DATA_SIZE, pad_size)) {
                if (report_failure) {
                    trace_s5l8900_adm_fmc_write_failure(
                        command_address, i, cursor.index, "pad-read", 0, 0);
                }
                return false;
            }
        }
    }
    if (!s5l8900_adm_descriptors_terminated(s, &cursor, report_failure)) {
        if (report_failure) {
            trace_s5l8900_adm_fmc_write_failure(
                command_address, page_count, cursor.index, "terminator", 0,
                0);
        }
        return false;
    }
    return true;
}

static bool s5l8900_adm_program_fmc_write(S5L8900ADMState *s,
                                          uint32_t command_address,
                                          uint16_t page_count,
                                          const uint8_t *banks,
                                          const uint32_t *page_numbers,
                                          const uint8_t *pages)
{
    const uint8_t *last_page =
        pages + (page_count - 1) * S5L8900_NAND_PAGE_TOTAL_SIZE;

    trace_s5l8900_adm_fmc_write_commit(
        command_address, page_count,
        ldl_le_p(pages + S5L8900_NAND_PAGE_DATA_SIZE),
        ldl_le_p(last_page + S5L8900_NAND_PAGE_DATA_SIZE),
        banks[0], page_numbers[0], banks[page_count - 1],
        page_numbers[page_count - 1]);
    for (unsigned i = 0; i < page_count; i++) {
        const uint8_t *page = pages + i * S5L8900_NAND_PAGE_TOTAL_SIZE;

        trace_s5l8900_adm_fmc_write_page(
            i, banks[i], page_numbers[i], ldl_le_p(page),
            ldl_le_p(page + 0x400), ldl_le_p(page + 0x404),
            ldl_le_p(page + S5L8900_NAND_PAGE_DATA_SIZE),
            ldl_le_p(page + S5L8900_NAND_PAGE_DATA_SIZE + 4),
            ldl_le_p(page + S5L8900_NAND_PAGE_DATA_SIZE + 8));
        if (!s5l8900_nand_program_page(
                s->nand, banks[i], page_numbers[i], page)) {
            trace_s5l8900_adm_fmc_write_failure(
                command_address, i, 0, "program", banks[i], page_numbers[i]);
            return false;
        }
    }
    return true;
}

static bool s5l8900_adm_fmc_write(S5L8900ADMState *s,
                                  uint32_t command_address,
                                  uint16_t opcode,
                                  uint16_t page_count, uint16_t pad_size)
{
    uint8_t banks[ADM_FMC_MAX_PAGES];
    uint32_t page_numbers[ADM_FMC_MAX_PAGES];
    uint32_t size = page_count * S5L8900_NAND_PAGE_TOTAL_SIZE;
    g_autofree uint8_t *pages = g_malloc(size);

    if (!s5l8900_adm_load_fmc_write(s, command_address, page_count,
                                    pad_size, pages, true)) {
        return false;
    }
    if (!s5l8900_adm_load_fmc_targets(s, command_address, opcode, page_count,
                                      banks, page_numbers, true)) {
        trace_s5l8900_adm_fmc_write_failure(
            command_address, 0, 0, "targets", 0, 0);
        return false;
    }
    return s5l8900_adm_program_fmc_write(s, command_address, page_count,
                                         banks, page_numbers, pages);
}

static bool s5l8900_adm_fmc_command_address(S5L8900ADMState *s,
                                            uint32_t *command_address);

static bool s5l8900_adm_prepare_fmc_write(S5L8900ADMState *s,
                                          bool report_failure)
{
    uint8_t banks[ADM_FMC_MAX_PAGES];
    uint32_t page_numbers[ADM_FMC_MAX_PAGES];
    g_autofree uint8_t *data = NULL;
    uint32_t command_address;
    uint32_t data_size;
    uint16_t opcode;
    uint16_t page_count;
    uint16_t pad_size;
    bool valid;

    s5l8900_adm_clear_prepared_write(s);
    if (!s5l8900_adm_fmc_command_address(s, &command_address) ||
        !s5l8900_adm_load_u16(s, command_address + ADM_FMC_OPCODE,
                              &opcode) ||
        !s5l8900_adm_load_u16(s, command_address + ADM_FMC_PAGE_COUNT,
                              &page_count) ||
        !s5l8900_adm_load_u16(s, command_address + ADM_FMC_PAD_SIZE,
                              &pad_size)) {
        return false;
    }

    page_count = bswap16(page_count);
    pad_size = bswap16(pad_size);
    if (opcode != ADM_FMC_WRITE && opcode != ADM_FMC_WRITE_MAX_ECC) {
        return false;
    }
    valid = page_count && page_count <= ADM_FMC_MAX_PAGES &&
            pad_size <= S5L8900_NAND_PAGE_SPARE_SIZE &&
            (uint64_t)page_count * pad_size <= ADM_FMC_ACTION3_SIZE &&
            (!pad_size || s->event_action[3]);

    if (!valid) {
        return false;
    }
    data_size = page_count * S5L8900_NAND_PAGE_TOTAL_SIZE;
    data = g_malloc(data_size);
    /* RUN, or the final DMA boundary after RUN, owns this transaction. */
    if (!s5l8900_adm_load_fmc_write(s, command_address, page_count,
                                    pad_size, data, report_failure) ||
        !s5l8900_adm_load_fmc_targets(s, command_address, opcode, page_count,
                                      banks, page_numbers, report_failure)) {
        return false;
    }
    s->fmc_write_prepared = true;
    s->fmc_write_opcode = opcode;
    s->fmc_write_page_count = page_count;
    s->fmc_write_pad_size = pad_size;
    s->fmc_write_command_address = command_address;
    s->fmc_write_data_size = data_size;
    s->fmc_write_data = g_steal_pointer(&data);
    memcpy(s->fmc_write_banks, banks, page_count * sizeof(banks[0]));
    memcpy(s->fmc_write_pages, page_numbers,
           page_count * sizeof(page_numbers[0]));
    return true;
}

static void s5l8900_adm_reject_fmc_write(S5L8900ADMState *s)
{
    uint32_t command_address;

    if (s5l8900_adm_fmc_command_address(s, &command_address)) {
        s5l8900_adm_fmc_result(s, command_address,
                               ADM_FMC_COMMAND_RESULT_BAD_ARGUMENT,
                               ADM_FMC_MEDIA_RESULT_OK,
                               ADM_FMC_READ_RESULT_OK);
    }
}

static bool s5l8900_adm_fmc_erase(S5L8900ADMState *s,
                                  uint32_t command_address,
                                  uint16_t page_count)
{
    uint8_t bank;
    uint32_t page;

    return page_count == 1 &&
           s5l8900_adm_dma_read(s, command_address + ADM_FMC_ERASE_BANK,
                                &bank, sizeof(bank)) &&
           s5l8900_adm_load_u32(s, command_address + ADM_FMC_PAGES,
                                &page) &&
           s5l8900_nand_erase_block(s->nand, bank, bswap32(page));
}

static bool s5l8900_adm_fmc_command_address(S5L8900ADMState *s,
                                            uint32_t *command_address)
{
    return !uadd32_overflow(s->event_action[2], ADM_FMC_COMMAND_OFFSET,
                            command_address) &&
           (uint64_t)*command_address + ADM_FMC_COMMAND_SPAN <=
           (uint64_t)UINT32_MAX + 1;
}

static uint32_t s5l8900_adm_fmc_dma_direction(S5L8900ADMState *s)
{
    uint32_t command_address;
    uint16_t opcode;
    uint16_t result_or_page_count;

    if (!s5l8900_adm_fmc_command_address(s, &command_address) ||
        !s5l8900_adm_load_u16(s, command_address + ADM_FMC_OPCODE,
                              &opcode) ||
        !s5l8900_adm_load_u16(s,
                              command_address + ADM_FMC_PAGE_COUNT,
                              &result_or_page_count)) {
        return ADM_DMA_NONE;
    }

    switch (opcode) {
    case ADM_FMC_READ:
    case ADM_FMC_READ_MAX_ECC:
        return result_or_page_count == ADM_FMC_COMMAND_RESULT_OK ?
               ADM_DMA_PERIPHERAL_TO_MEMORY : ADM_DMA_NONE;
    case ADM_FMC_WRITE:
    case ADM_FMC_WRITE_MAX_ECC:
        return ADM_DMA_MEMORY_TO_PERIPHERAL;
    case ADM_FMC_INIT:
    case ADM_FMC_ERASE:
        return ADM_DMA_NONE;
    default:
        return ADM_DMA_NONE;
    }
}

static void s5l8900_adm_trace_fmc_state(S5L8900ADMState *s,
                                        const char *where)
{
    if (!trace_event_get_state_backends(TRACE_S5L8900_ADM_FMC_STATE)) {
        return;
    }
    trace_s5l8900_adm_fmc_state(
        where, s5l8900_adm_fmc_dma_direction(s), s->dma_direction,
        s->fmc_event_armed, s->fmc_run_pending,
        s->fmc_completion_is_write, s->fmc_write_prepared,
        timer_pending(s->completion_timer),
        timer_pending(s->dma_completion_timer));
}

static void s5l8900_adm_trace_skipped_fmc_write(S5L8900ADMState *s)
{
    uint32_t command_address = UINT32_MAX;
    uint32_t first_lpn = UINT32_MAX;
    uint32_t last_lpn = UINT32_MAX;
    uint32_t last_pad_address;
    uint16_t opcode = 0;
    uint16_t page_count = 0;
    uint16_t pad_size = 0;

    if (!trace_event_get_state_backends(
            TRACE_S5L8900_ADM_FMC_WRITE_SKIPPED)) {
        return;
    }
    if (s5l8900_adm_fmc_command_address(s, &command_address) &&
        s5l8900_adm_load_u16(s, command_address + ADM_FMC_OPCODE,
                             &opcode) &&
        s5l8900_adm_load_u16(s, command_address + ADM_FMC_PAGE_COUNT,
                             &page_count) &&
        s5l8900_adm_load_u16(s, command_address + ADM_FMC_PAD_SIZE,
                             &pad_size)) {
        page_count = bswap16(page_count);
        pad_size = bswap16(pad_size);
        if (page_count && page_count <= ADM_FMC_MAX_PAGES &&
            pad_size >= sizeof(first_lpn) &&
            pad_size <= S5L8900_NAND_PAGE_SPARE_SIZE &&
            s->event_action[3] &&
            !uadd32_overflow(s->event_action[3],
                             (page_count - 1) * pad_size,
                             &last_pad_address)) {
            s5l8900_adm_load_u32(s, s->event_action[3], &first_lpn);
            s5l8900_adm_load_u32(s, last_pad_address, &last_lpn);
        }
    }
    trace_s5l8900_adm_fmc_write_skipped(
        command_address, opcode, page_count, pad_size, first_lpn, last_lpn);
}

static bool s5l8900_adm_run_fmc(S5L8900ADMState *s)
{
    uint32_t command_address;
    uint16_t opcode;
    uint16_t page_count;
    uint16_t pad_size;
    uint16_t flags;
    bool empty = false;
    bool success = false;

    if (s->fmc_write_prepared) {
        command_address = s->fmc_write_command_address;
        opcode = s->fmc_write_opcode;
        page_count = s->fmc_write_page_count;
        pad_size = s->fmc_write_pad_size;
        flags = 0;
        success = s5l8900_adm_program_fmc_write(
            s, command_address, page_count, s->fmc_write_banks,
            s->fmc_write_pages, s->fmc_write_data);
        s5l8900_adm_clear_prepared_write(s);
        goto complete;
    }
    s5l8900_adm_clear_prepared_write(s);
    if (!s5l8900_adm_fmc_command_address(s, &command_address) ||
        !s5l8900_adm_load_u16(s, command_address + ADM_FMC_FLAGS, &flags) ||
        !s5l8900_adm_load_u16(s, command_address + ADM_FMC_OPCODE,
                              &opcode) ||
        !s5l8900_adm_load_u16(s, command_address + ADM_FMC_PAGE_COUNT,
                              &page_count) ||
        !s5l8900_adm_load_u16(s, command_address + ADM_FMC_PAD_SIZE,
                              &pad_size)) {
        return false;
    }
    flags = bswap16(flags);
    page_count = bswap16(page_count);
    pad_size = bswap16(pad_size);
    if (opcode == ADM_FMC_INIT) {
        success = page_count == 0 && pad_size == 0;
    } else if (page_count && page_count <= ADM_FMC_MAX_PAGES) {
        switch (opcode) {
        case ADM_FMC_READ:
        case ADM_FMC_READ_MAX_ECC:
        case ADM_FMC_WRITE:
        case ADM_FMC_WRITE_MAX_ECC:
            if (pad_size <= S5L8900_NAND_PAGE_SPARE_SIZE &&
                (uint64_t)page_count * pad_size <= ADM_FMC_ACTION3_SIZE &&
                (!pad_size || s->event_action[3])) {
                if (opcode == ADM_FMC_READ ||
                    opcode == ADM_FMC_READ_MAX_ECC) {
                    success = s5l8900_adm_fmc_read(s, command_address, opcode,
                                                   page_count, pad_size,
                                                   &empty);
                } else {
                    success = s5l8900_adm_fmc_write(s, command_address,
                                                    opcode,
                                                    page_count, pad_size);
                }
            }
            break;
        case ADM_FMC_ERASE:
            success = s5l8900_adm_fmc_erase(s, command_address,
                                            page_count);
            break;
        default:
            break;
        }
    }

complete:
    s5l8900_adm_fmc_result(s, command_address,
                           success ? ADM_FMC_COMMAND_RESULT_OK :
                                     ADM_FMC_COMMAND_RESULT_BAD_ARGUMENT,
                           ADM_FMC_MEDIA_RESULT_OK,
                           success && empty ? ADM_FMC_READ_RESULT_EMPTY :
                                              ADM_FMC_READ_RESULT_OK);
    trace_s5l8900_adm_fmc_run(command_address, opcode, page_count,
                              pad_size, flags,
                              success ? ADM_FMC_COMMAND_RESULT_OK :
                                        ADM_FMC_COMMAND_RESULT_BAD_ARGUMENT,
                              ADM_FMC_MEDIA_RESULT_OK,
                              success && empty ? ADM_FMC_READ_RESULT_EMPTY :
                                                 ADM_FMC_READ_RESULT_OK);
    if (!success) {
        return false;
    }
    return opcode == ADM_FMC_READ || opcode == ADM_FMC_READ_MAX_ECC;
}

static bool s5l8900_adm_schedule_dma(
    S5L8900ADMState *s, uint32_t dma_direction,
    PL080ExternalRequestDirection pl080_direction)
{
    if (s->fmc_event_armed && s->dma_direction == ADM_DMA_NONE &&
        !timer_pending(s->dma_completion_timer) &&
        pl080_has_external_request(s->dmac, ADM_FMC_DMA_REQUEST,
                                   pl080_direction)) {
        s->fmc_event_armed = false;
        s->dma_direction = dma_direction;
        if (dma_direction == ADM_DMA_MEMORY_TO_PERIPHERAL &&
            s->fmc_run_pending) {
            /* The terminal count now owns completion for this RUN. */
            timer_del(s->completion_timer);
        }
        timer_mod(s->dma_completion_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  ADM_FMC_DMA_COMPLETION_NS);
        return true;
    }
    return false;
}

static void s5l8900_adm_complete_dma(void *opaque)
{
    S5L8900ADMState *s = opaque;
    PL080ExternalRequestDirection direction;
    bool completed;
    bool retry;

    s5l8900_adm_trace_fmc_state(s, "dma-enter");
    if (!(s->control & ADM_CONTROL_RUNNING)) {
        s->dma_direction = ADM_DMA_NONE;
        s->fmc_event_armed = false;
        s->fmc_run_pending = false;
        s->fmc_completion_is_write = false;
        s5l8900_adm_clear_prepared_write(s);
        return;
    }
    if (s->dma_direction == ADM_DMA_NONE && s->fmc_event_armed &&
        s5l8900_adm_fmc_dma_direction(s) ==
            ADM_DMA_PERIPHERAL_TO_MEMORY) {
        if (!s5l8900_adm_schedule_dma(
                s, ADM_DMA_PERIPHERAL_TO_MEMORY,
                PL080_EXTERNAL_PERIPHERAL_TO_MEMORY)) {
            timer_mod(s->dma_completion_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      ADM_FMC_DMA_ARM_GRACE_NS);
        }
        return;
    }
    if (s->dma_direction == ADM_DMA_MEMORY_TO_PERIPHERAL) {
        direction = PL080_EXTERNAL_MEMORY_TO_PERIPHERAL;
    } else if (s->dma_direction == ADM_DMA_PERIPHERAL_TO_MEMORY) {
        direction = PL080_EXTERNAL_PERIPHERAL_TO_MEMORY;
    } else {
        return;
    }
    completed = pl080_complete_external_request(s->dmac,
                                                ADM_FMC_DMA_REQUEST,
                                                direction, &retry);
    if (completed && !retry && s->fmc_run_pending &&
        s->dma_direction == ADM_DMA_MEMORY_TO_PERIPHERAL &&
        !s->fmc_write_prepared) {
        /*
         * A pre-RUN terminal count only proves DMA readiness.  The shared
         * command record can still be a complete stale transaction, so RUN
         * must precede the immutable snapshot.
         */
        if (!s5l8900_adm_prepare_fmc_write(s, true)) {
            s5l8900_adm_reject_fmc_write(s);
        }
    }
    if (retry) {
        timer_mod(s->dma_completion_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  ADM_FMC_DMA_SEGMENT_NS);
    } else {
        uint32_t completed_direction = s->dma_direction;

        s->dma_direction = ADM_DMA_NONE;
        if (!completed) {
            s->fmc_run_pending = false;
        }
        if (completed &&
            completed_direction == ADM_DMA_MEMORY_TO_PERIPHERAL &&
            s->fmc_run_pending) {
            s->fmc_run_pending = false;
            s->fmc_completion_is_write = true;
            timer_mod(s->completion_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      ADM_FMC_COMPLETION_NS);
        }
    }
    s5l8900_adm_trace_fmc_state(s, "dma-exit");
}

static void s5l8900_adm_complete_fmc(void *opaque)
{
    S5L8900ADMState *s = opaque;

    s5l8900_adm_trace_fmc_state(s, "fmc-enter");
    if (!(s->control & ADM_CONTROL_RUNNING)) {
        s->fmc_completion_is_write = false;
        return;
    }
    if (s->fmc_run_pending && !s->fmc_write_prepared) {
        if (s->dma_direction == ADM_DMA_NONE && s->fmc_event_armed &&
            s5l8900_adm_fmc_dma_direction(s) ==
                ADM_DMA_MEMORY_TO_PERIPHERAL) {
            if (!s5l8900_adm_schedule_dma(
                    s, ADM_DMA_MEMORY_TO_PERIPHERAL,
                    PL080_EXTERNAL_MEMORY_TO_PERIPHERAL)) {
                /*
                 * RUN owns this write.  The producer starts PL080 only after
                 * issuing RUN, so a Host scheduling pause may outlive any
                 * fixed grace period between those two Guest operations.
                 */
                timer_mod(s->completion_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          ADM_FMC_DMA_ARM_GRACE_NS);
            }
            return;
        }
        /*
         * ARM_EVENT also advertises lifecycle readiness.  Without an M2P
         * request there is no write transaction to decode: the shared action
         * buffer may still contain a stale, write-shaped record.
         */
        s5l8900_adm_trace_skipped_fmc_write(s);
        goto publish;
    }
    if (s->fmc_completion_is_write) {
        if (s->fmc_write_prepared) {
            s5l8900_adm_run_fmc(s);
        }
    } else if (s5l8900_adm_run_fmc(s)) {
        s5l8900_adm_schedule_dma(s, ADM_DMA_PERIPHERAL_TO_MEMORY,
                                 PL080_EXTERNAL_PERIPHERAL_TO_MEMORY);
    }
publish:
    s->fmc_completion_is_write = false;
    s->fmc_run_pending = false;
    /* ARM_EVENT readiness belongs to this published event, with or without DMA. */
    s->fmc_event_armed = false;
    s->irq_status |= ADM_IRQ_EVENT;
    s5l8900_adm_update(s);
    s5l8900_adm_trace_fmc_state(s, "fmc-exit");
}

static void s5l8900_adm_start(S5L8900ADMState *s)
{
    static const uint8_t post = 0x50;
    uint8_t device_ids[ADM_FMC_BANK_SLOTS * sizeof(uint32_t)] = { 0 };
    bool complete = false;

    if (s->event_action[2]) {
        complete = s5l8900_adm_dma_write(
            s, s->event_action[2], &post, sizeof(post));
    }
    if (s->event_action[3]) {
        for (unsigned bank = 0; bank < ADM_FMC_BANK_SLOTS; bank++) {
            stl_le_p(device_ids + bank * sizeof(uint32_t),
                     s5l8900_nand_device_id(s->nand, bank));
        }
        complete &= s5l8900_adm_dma_write(
            s, s->event_action[3], device_ids, sizeof(device_ids));
    }
    if (complete) {
        /*
         * Starting CalmADMFMC is itself a firmware transaction.  The 8C148
         * consumer waits for its event after reading POST 0x50 and the eight
         * device-ID slots; publishing only the buffers leaves it asleep until
         * its watchdog panics during reboot.
         */
        s->irq_status |= ADM_IRQ_EVENT;
    }
}

static void s5l8900_adm_command(S5L8900ADMState *s, uint32_t command)
{
    uint32_t previous_status = s->irq_status;

    switch (command) {
    case ADM_COMMAND_UPLOAD_DATA:
        if (s->control & ADM_CONTROL_RUNNING) {
            s->irq_status |= ADM_IRQ_COMMAND;
        }
        break;
    case ADM_COMMAND_ARM_EVENT:
        if (s->control & ADM_CONTROL_RUNNING) {
            uint32_t direction = s5l8900_adm_fmc_dma_direction(s);

            s->fmc_event_armed = true;
            if (direction == ADM_DMA_MEMORY_TO_PERIPHERAL) {
                s5l8900_adm_schedule_dma(
                    s, direction, PL080_EXTERNAL_MEMORY_TO_PERIPHERAL);
            } else if (direction == ADM_DMA_PERIPHERAL_TO_MEMORY) {
                if (!s5l8900_adm_schedule_dma(
                        s, direction,
                        PL080_EXTERNAL_PERIPHERAL_TO_MEMORY)) {
                    timer_mod(s->dma_completion_timer,
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                              ADM_FMC_DMA_ARM_GRACE_NS);
                }
            }
            s5l8900_adm_trace_fmc_state(s, "arm");
        }
        break;
    case ADM_COMMAND_RUN:
        if ((s->control & ADM_CONTROL_RUNNING) &&
            !timer_pending(s->completion_timer)) {
            uint32_t direction = s5l8900_adm_fmc_dma_direction(s);

            if (s->dma_direction == ADM_DMA_NONE && s->fmc_event_armed &&
                timer_pending(s->dma_completion_timer) &&
                direction != ADM_DMA_PERIPHERAL_TO_MEMORY) {
                timer_del(s->dma_completion_timer);
                s->fmc_event_armed = false;
            }
            if (s->fmc_write_prepared) {
                s->fmc_completion_is_write = true;
                timer_mod(s->completion_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          ADM_FMC_COMPLETION_NS);
            } else if (s->dma_direction == ADM_DMA_MEMORY_TO_PERIPHERAL) {
                s->fmc_run_pending = true;
            } else if (s->fmc_event_armed &&
                       direction == ADM_DMA_MEMORY_TO_PERIPHERAL) {
                s->fmc_run_pending = true;
                if (!s5l8900_adm_schedule_dma(
                        s, ADM_DMA_MEMORY_TO_PERIPHERAL,
                        PL080_EXTERNAL_MEMORY_TO_PERIPHERAL)) {
                    timer_mod(s->completion_timer,
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                              ADM_FMC_DMA_ARM_GRACE_NS);
                }
            } else if (!timer_pending(s->dma_completion_timer)) {
                if (direction == ADM_DMA_MEMORY_TO_PERIPHERAL) {
                    if (!s5l8900_adm_prepare_fmc_write(s, true)) {
                        s5l8900_adm_reject_fmc_write(s);
                    }
                    s->fmc_completion_is_write = true;
                }
                timer_mod(s->completion_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          ADM_FMC_COMPLETION_NS);
            }
            s5l8900_adm_trace_fmc_state(s, "run");
        }
        break;
    case ADM_COMMAND_ACK_EVENT:
        s->irq_status &= ~ADM_IRQ_EVENT;
        break;
    case ADM_COMMAND_ACK_COMMAND:
        s->irq_status &= ~ADM_IRQ_COMMAND;
        break;
    case ADM_COMMAND_ACK_UPLOAD:
        s->irq_status &= ~ADM_IRQ_UPLOAD;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.adm: unimplemented command 0x%08x\n",
                      command);
        break;
    }
    s5l8900_adm_update(s);
    trace_s5l8900_adm_command_write(command, s->control, previous_status,
                                    s->irq_status);
}

static uint64_t s5l8900_adm_read(void *opaque, hwaddr offset,
                                 unsigned size)
{
    S5L8900ADMState *s = opaque;

    if (offset >= ADM_UPLOAD_DATA_BASE && offset < ADM_EVENT_DATA_BASE) {
        return s->upload_data[(offset - ADM_UPLOAD_DATA_BASE) / 4];
    }
    if (offset >= ADM_EVENT_DATA_BASE && offset < ADM_UPLOAD_ACTION_0) {
        return s->event_data[(offset - ADM_EVENT_DATA_BASE) / 4];
    }
    if (offset >= 0x68 && offset <= 0x80) {
        return s->upload_action[(offset - ADM_UPLOAD_ACTION_BASE) / 4];
    }
    if (offset >= 0x84 && offset <= 0x8c) {
        return s->event_action[(offset - ADM_EVENT_ACTION_BASE) / 4];
    }

    switch (offset) {
    case ADM_CONTROL:
        return s->control | ADM_CONTROL_READY;
    case ADM_COMMAND:
        return s->irq_status;
    case ADM_UPLOAD_ACTION_0:
        return s->upload_action[0];
    case ADM_EVENT_ACTION_0:
        return s->event_action[0];
    case ADM_REG_90:
        return s->reg90;
    case ADM_REG_94:
        return s->reg94;
    case ADM_REG_9C:
        return s->reg9c;
    default:
        return 0;
    }
}

static void s5l8900_adm_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    S5L8900ADMState *s = opaque;

    if (offset >= ADM_UPLOAD_DATA_BASE && offset < ADM_EVENT_DATA_BASE) {
        s->upload_data[(offset - ADM_UPLOAD_DATA_BASE) / 4] = value;
        return;
    }
    if (offset >= ADM_EVENT_DATA_BASE && offset < ADM_UPLOAD_ACTION_0) {
        s->event_data[(offset - ADM_EVENT_DATA_BASE) / 4] = value;
        return;
    }
    if (offset >= 0x68 && offset <= 0x80) {
        s->upload_action[(offset - ADM_UPLOAD_ACTION_BASE) / 4] = value;
        return;
    }
    if (offset >= 0x84 && offset <= 0x8c) {
        s->event_action[(offset - ADM_EVENT_ACTION_BASE) / 4] = value;
        return;
    }

    switch (offset) {
    case ADM_CONTROL:
        trace_s5l8900_adm_control_write(s->control, value);
        if (value & ADM_CONTROL_SOFT_RESET) {
            s5l8900_adm_soft_reset(s);
        } else {
            s->control = value & (ADM_CONTROL_RUNNING | ADM_IRQ_MASK);
            if (!(s->control & ADM_CONTROL_RUNNING)) {
                timer_del(s->completion_timer);
                timer_del(s->dma_completion_timer);
                s->dma_direction = ADM_DMA_NONE;
                s->fmc_event_armed = false;
                s->fmc_run_pending = false;
                s->fmc_completion_is_write = false;
                s5l8900_adm_clear_prepared_write(s);
            }
            if (s->control & ADM_CONTROL_RUNNING) {
                /*
                 * RUNNING is also the firmware start command.  XNU rewrites
                 * it when CalmADMFMC is reinitialized without an SoC reset
                 * and waits for a fresh POST transaction.
                 */
                s5l8900_adm_start(s);
            }
            s5l8900_adm_update(s);
        }
        break;
    case ADM_COMMAND:
        s5l8900_adm_command(s, value);
        break;
    case ADM_UPLOAD_ACTION_0:
        s->upload_action[0] = value;
        break;
    case ADM_EVENT_ACTION_0:
        s->event_action[0] = value;
        break;
    case ADM_REG_90:
        s->reg90 = value;
        break;
    case ADM_REG_94:
        s->reg94 = value;
        break;
    case ADM_REG_9C:
        s->reg9c = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.adm: unimplemented write 0x%08" PRIx64
                      " at 0x%03" HWADDR_PRIx "\n", value, offset);
        break;
    }
}

static const MemoryRegionOps s5l8900_adm_ops = {
    .read = s5l8900_adm_read,
    .write = s5l8900_adm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_adm_reset(DeviceState *dev)
{
    s5l8900_adm_soft_reset(S5L8900_ADM(dev));
}

static int s5l8900_adm_post_load(void *opaque, int version_id)
{
    S5L8900ADMState *s = opaque;
    uint32_t expected_write_size =
        s->fmc_write_page_count * S5L8900_NAND_PAGE_TOTAL_SIZE;

    if (s->control & ~(ADM_CONTROL_RUNNING | ADM_IRQ_MASK) ||
        s->irq_status & ~ADM_IRQ_MASK ||
        s->dma_direction > ADM_DMA_PERIPHERAL_TO_MEMORY ||
        (s->fmc_event_armed && s->dma_direction != ADM_DMA_NONE) ||
        (s->fmc_run_pending &&
         s->dma_direction != ADM_DMA_MEMORY_TO_PERIPHERAL &&
         !s->fmc_event_armed) ||
        (s->fmc_completion_is_write &&
         (s->fmc_run_pending || !timer_pending(s->completion_timer))) ||
        (s->fmc_write_prepared &&
         (s->fmc_write_opcode != ADM_FMC_WRITE &&
          s->fmc_write_opcode != ADM_FMC_WRITE_MAX_ECC)) ||
        (s->fmc_write_prepared &&
         (!s->fmc_write_page_count ||
          s->fmc_write_page_count > ADM_FMC_MAX_PAGES ||
          s->fmc_write_pad_size > S5L8900_NAND_PAGE_SPARE_SIZE ||
          s->fmc_write_data_size != expected_write_size ||
          !s->fmc_write_data)) ||
        (!s->fmc_write_prepared &&
         (s->fmc_write_data_size || s->fmc_write_data)) ||
        s->fmc_write_data_size >
            ADM_FMC_MAX_PAGES * S5L8900_NAND_PAGE_TOTAL_SIZE) {
        return -EINVAL;
    }
    if (version_id < 3 && s->fmc_write_prepared &&
        !s5l8900_adm_load_fmc_targets(s, s->fmc_write_command_address,
                                      s->fmc_write_opcode,
                                      s->fmc_write_page_count,
                                      s->fmc_write_banks,
                                      s->fmc_write_pages, false)) {
        return -EINVAL;
    }
    s5l8900_adm_update(s);
    return 0;
}

static int s5l8900_adm_pre_load(void *opaque)
{
    S5L8900ADMState *s = opaque;

    s->fmc_event_armed = false;
    s->fmc_run_pending = false;
    s->fmc_completion_is_write = false;
    s5l8900_adm_clear_prepared_write(s);
    return 0;
}

static void s5l8900_adm_realize(DeviceState *dev, Error **errp)
{
    S5L8900ADMState *s = S5L8900_ADM(dev);

    if (!s->dma_memory) {
        error_setg(errp, TYPE_S5L8900_ADM " 'dma-memory' link not set");
        return;
    }
    if (!s->nand) {
        error_setg(errp, TYPE_S5L8900_ADM " 'nand' link not set");
        return;
    }
    if (!s->dmac) {
        error_setg(errp, TYPE_S5L8900_ADM " 'dmac' link not set");
        return;
    }
    address_space_init(&s->dma_as, s->dma_memory, "s5l8900-adm-dma");
}

static const Property s5l8900_adm_properties[] = {
    DEFINE_PROP_LINK("dma-memory", S5L8900ADMState, dma_memory,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("nand", S5L8900ADMState, nand,
                     TYPE_S5L8900_NAND, S5L8900NANDState *),
    DEFINE_PROP_LINK("dmac", S5L8900ADMState, dmac,
                     TYPE_PL080, PL080State *),
};

static const VMStateDescription vmstate_s5l8900_adm = {
    .name = TYPE_S5L8900_ADM,
    .version_id = 5,
    .minimum_version_id = 1,
    .pre_load = s5l8900_adm_pre_load,
    .post_load = s5l8900_adm_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(control, S5L8900ADMState),
        VMSTATE_UINT32(irq_status, S5L8900ADMState),
        VMSTATE_UINT32(dma_direction, S5L8900ADMState),
        VMSTATE_UINT32_ARRAY(upload_data, S5L8900ADMState, 8),
        VMSTATE_UINT32_ARRAY(event_data, S5L8900ADMState, 8),
        VMSTATE_UINT32_ARRAY(upload_action, S5L8900ADMState, 8),
        VMSTATE_UINT32_ARRAY(event_action, S5L8900ADMState, 4),
        VMSTATE_UINT32(reg90, S5L8900ADMState),
        VMSTATE_UINT32(reg94, S5L8900ADMState),
        VMSTATE_UINT32(reg9c, S5L8900ADMState),
        VMSTATE_BOOL_V(fmc_event_armed, S5L8900ADMState, 4),
        VMSTATE_BOOL_V(fmc_run_pending, S5L8900ADMState, 4),
        VMSTATE_BOOL_V(fmc_completion_is_write, S5L8900ADMState, 5),
        VMSTATE_BOOL_V(fmc_write_prepared, S5L8900ADMState, 2),
        VMSTATE_UINT16_V(fmc_write_opcode, S5L8900ADMState, 2),
        VMSTATE_UINT16_V(fmc_write_page_count, S5L8900ADMState, 2),
        VMSTATE_UINT16_V(fmc_write_pad_size, S5L8900ADMState, 2),
        VMSTATE_UINT32_V(fmc_write_command_address, S5L8900ADMState, 2),
        VMSTATE_UINT32_V(fmc_write_data_size, S5L8900ADMState, 2),
        VMSTATE_VBUFFER_ALLOC_UINT32(fmc_write_data, S5L8900ADMState, 2,
                                     NULL, fmc_write_data_size),
        VMSTATE_UINT8_ARRAY_V(fmc_write_banks, S5L8900ADMState,
                              S5L8900_ADM_FMC_MAX_PAGES, 3),
        VMSTATE_UINT32_ARRAY_V(fmc_write_pages, S5L8900ADMState,
                               S5L8900_ADM_FMC_MAX_PAGES, 3),
        VMSTATE_TIMER_PTR(completion_timer, S5L8900ADMState),
        VMSTATE_TIMER_PTR(dma_completion_timer, S5L8900ADMState),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8900_adm_init(Object *obj)
{
    S5L8900ADMState *s = S5L8900_ADM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_adm_ops, s,
                          "s5l8900.adm", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->completion_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       s5l8900_adm_complete_fmc, s);
    s->dma_completion_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                           s5l8900_adm_complete_dma, s);
}

static void s5l8900_adm_finalize(Object *obj)
{
    S5L8900ADMState *s = S5L8900_ADM(obj);

    s5l8900_adm_clear_prepared_write(s);
    timer_free(s->completion_timer);
    timer_free(s->dma_completion_timer);
}

static void s5l8900_adm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Apple S5L8900 Apple Data Mover";
    dc->realize = s5l8900_adm_realize;
    dc->vmsd = &vmstate_s5l8900_adm;
    device_class_set_props(dc, s5l8900_adm_properties);
    device_class_set_legacy_reset(dc, s5l8900_adm_reset);
}

static const TypeInfo s5l8900_adm_info = {
    .name = TYPE_S5L8900_ADM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900ADMState),
    .instance_init = s5l8900_adm_init,
    .instance_finalize = s5l8900_adm_finalize,
    .class_init = s5l8900_adm_class_init,
};

static void s5l8900_adm_register_types(void)
{
    type_register_static(&s5l8900_adm_info);
}

type_init(s5l8900_adm_register_types)
