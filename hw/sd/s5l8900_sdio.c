/*
 * Apple S5L8900X SDIO controller
 *
 * The register layout and command handshake are consumed by the iOS 4.2.1
 * AppleS5L8900XSDIO driver.  The iPhone 3G DeviceTree declares no attached
 * SDIO functions, so this model completes commands with an all-zero response.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/sd/s5l8900_sdio.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define SDIO_CTRL              0x00
#define SDIO_DCTRL             0x04
#define SDIO_COMMAND           0x08
#define SDIO_ARGUMENT          0x0c
#define SDIO_STATE             0x10
#define SDIO_STATUS_ACK        0x14
#define SDIO_DATA_STATUS       0x18
#define SDIO_FIFO_STATUS       0x1c
#define SDIO_RESPONSE0         0x20
#define SDIO_CLKDIV            0x30
#define SDIO_CSR               0x34
#define SDIO_IRQ_STATUS        0x38
#define SDIO_IRQ_MASK          0x3c
#define SDIO_BUFFER_ADDRESS    0x44
#define SDIO_BLOCK_LENGTH      0x48
#define SDIO_BLOCK_COUNT       0x4c
#define SDIO_REMAINING_BLOCKS  0x50
#define SDIO_RESET             0x6c

#define SDIO_COMMAND_START     BIT(31)
#define SDIO_DSTA_COMMAND_READY BIT(0)
#define SDIO_DSTA_COMMAND_DONE BIT(4)

static void s5l8900_sdio_update_irq(S5L8900SDIOState *s)
{
    qemu_set_irq(s->irq, (s->irq_status & s->irq_mask) != 0);
}

static void s5l8900_sdio_reset(DeviceState *dev)
{
    S5L8900SDIOState *s = S5L8900_SDIO(dev);

    s->ctrl = 0;
    s->dctrl = 0;
    s->command = 0;
    s->argument = 0;
    s->dsta = SDIO_DSTA_COMMAND_READY;
    memset(s->responses, 0, sizeof(s->responses));
    s->clkdiv = 0;
    s->csr = 0;
    s->irq_status = 0;
    s->irq_mask = 0;
    s->buffer_address = 0;
    s->block_length = 0;
    s->block_count = 0;
    s5l8900_sdio_update_irq(s);
}

static uint64_t s5l8900_sdio_read(void *opaque, hwaddr offset,
                                  unsigned size)
{
    S5L8900SDIOState *s = opaque;

    switch (offset) {
    case SDIO_CTRL:
        return s->ctrl;
    case SDIO_DCTRL:
        return s->dctrl;
    case SDIO_COMMAND:
        return s->command;
    case SDIO_ARGUMENT:
        return s->argument;
    case SDIO_STATE:
        return 0;
    case SDIO_STATUS_ACK:
        return 0;
    case SDIO_DATA_STATUS:
        return s->dsta;
    case SDIO_FIFO_STATUS:
        return 0;
    case SDIO_RESPONSE0 ... SDIO_RESPONSE0 + 3 * sizeof(uint32_t):
        return s->responses[(offset - SDIO_RESPONSE0) / sizeof(uint32_t)];
    case SDIO_CLKDIV:
        return s->clkdiv;
    case SDIO_CSR:
        return s->csr;
    case SDIO_IRQ_STATUS:
        return s->irq_status;
    case SDIO_IRQ_MASK:
        return s->irq_mask;
    case SDIO_BUFFER_ADDRESS:
        return s->buffer_address;
    case SDIO_BLOCK_LENGTH:
        return s->block_length;
    case SDIO_BLOCK_COUNT:
        return s->block_count;
    case SDIO_REMAINING_BLOCKS:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented read at 0x%" HWADDR_PRIx "\n",
                      TYPE_S5L8900_SDIO, offset);
        return 0;
    }
}

static void s5l8900_sdio_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    S5L8900SDIOState *s = opaque;

    switch (offset) {
    case SDIO_CTRL:
        s->ctrl = value;
        break;
    case SDIO_DCTRL:
        s->dctrl = value;
        break;
    case SDIO_COMMAND:
        s->command = value;
        if (s->command & SDIO_COMMAND_START) {
            memset(s->responses, 0, sizeof(s->responses));
            s->dsta |= SDIO_DSTA_COMMAND_READY | SDIO_DSTA_COMMAND_DONE;
        }
        break;
    case SDIO_ARGUMENT:
        s->argument = value;
        break;
    case SDIO_STATUS_ACK:
        s->dsta &= ~(value & ~SDIO_DSTA_COMMAND_READY);
        break;
    case SDIO_DATA_STATUS:
    case SDIO_FIFO_STATUS:
    case SDIO_RESPONSE0 ... SDIO_RESPONSE0 + 3 * sizeof(uint32_t):
    case SDIO_REMAINING_BLOCKS:
        break;
    case SDIO_CLKDIV:
        s->clkdiv = value;
        break;
    case SDIO_CSR:
        s->csr = value;
        break;
    case SDIO_IRQ_STATUS:
        s->irq_status &= ~value;
        s5l8900_sdio_update_irq(s);
        break;
    case SDIO_IRQ_MASK:
        s->irq_mask = value;
        s5l8900_sdio_update_irq(s);
        break;
    case SDIO_BUFFER_ADDRESS:
        s->buffer_address = value;
        break;
    case SDIO_BLOCK_LENGTH:
        s->block_length = value;
        break;
    case SDIO_BLOCK_COUNT:
        s->block_count = value;
        break;
    case SDIO_RESET:
        device_cold_reset(DEVICE(s));
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented write at 0x%" HWADDR_PRIx "\n",
                      TYPE_S5L8900_SDIO, offset);
        break;
    }
}

static const MemoryRegionOps s5l8900_sdio_ops = {
    .read = s5l8900_sdio_read,
    .write = s5l8900_sdio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const VMStateDescription vmstate_s5l8900_sdio = {
    .name = TYPE_S5L8900_SDIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, S5L8900SDIOState),
        VMSTATE_UINT32(dctrl, S5L8900SDIOState),
        VMSTATE_UINT32(command, S5L8900SDIOState),
        VMSTATE_UINT32(argument, S5L8900SDIOState),
        VMSTATE_UINT32(dsta, S5L8900SDIOState),
        VMSTATE_UINT32_ARRAY(responses, S5L8900SDIOState, 4),
        VMSTATE_UINT32(clkdiv, S5L8900SDIOState),
        VMSTATE_UINT32(csr, S5L8900SDIOState),
        VMSTATE_UINT32(irq_status, S5L8900SDIOState),
        VMSTATE_UINT32(irq_mask, S5L8900SDIOState),
        VMSTATE_UINT32(buffer_address, S5L8900SDIOState),
        VMSTATE_UINT32(block_length, S5L8900SDIOState),
        VMSTATE_UINT32(block_count, S5L8900SDIOState),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8900_sdio_init(Object *obj)
{
    S5L8900SDIOState *s = S5L8900_SDIO(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_sdio_ops, s,
                          TYPE_S5L8900_SDIO, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void s5l8900_sdio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, s5l8900_sdio_reset);
    dc->vmsd = &vmstate_s5l8900_sdio;
}

static const TypeInfo s5l8900_sdio_info = {
    .name = TYPE_S5L8900_SDIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900SDIOState),
    .instance_init = s5l8900_sdio_init,
    .class_init = s5l8900_sdio_class_init,
};

static void s5l8900_sdio_register_types(void)
{
    type_register_static(&s5l8900_sdio_info);
}

type_init(s5l8900_sdio_register_types)
