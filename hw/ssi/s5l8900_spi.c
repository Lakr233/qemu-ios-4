/*
 * Apple S5L8900 synchronous serial controller
 *
 * The consumed interface is documented by OpeniBoot's S5L8900 SPI driver.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/ssi/s5l8900_spi.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

#define SPI_CONTROL             0x00
#define SPI_SETUP               0x04
#define SPI_STATUS              0x08
#define SPI_PIN                 0x0c
#define SPI_TXDATA              0x10
#define SPI_RXDATA              0x20
#define SPI_CLOCK_DIVIDER       0x30
#define SPI_TRANSFER_COUNT      0x34
#define SPI_IDD                 0x38

#define SPI_CONTROL_ENABLE      BIT(0)
#define SPI_CONTROL_RX_RESET    BIT(2)
#define SPI_CONTROL_TX_RESET    BIT(3)

#define SPI_SETUP_TRANSMIT_JUNK BIT(0)
#define SPI_SETUP_DMA           BIT(6)
#define SPI_SETUP_TX_SERVICE    BIT(7)
#define SPI_SETUP_RX_SERVICE    BIT(8)
#define SPI_SETUP_SERVICE_MASK  (SPI_SETUP_TX_SERVICE | \
                                 SPI_SETUP_RX_SERVICE)

#define SPI_STATUS_RX_SERVICE   BIT(0)
#define SPI_STATUS_TX_SERVICE   BIT(1)
#define SPI_STATUS_IRQ_MASK     0x0f
#define SPI_STATUS_TX_COUNT_SHIFT 4
#define SPI_STATUS_RX_COUNT_SHIFT 8

static uint32_t s5l8900_spi_word_mask(S5L8900SPIState *s)
{
    unsigned bytes = 1U << extract32(s->setup, 13, 2);

    return bytes == sizeof(uint32_t) ? UINT32_MAX :
           MAKE_64BIT_MASK(0, bytes * 8);
}

static void s5l8900_spi_update_irq(S5L8900SPIState *s)
{
    qemu_set_irq(s->irq, s->irq_pending != 0);
}

static void s5l8900_spi_update_dreq(S5L8900SPIState *s)
{
    qemu_set_irq(s->tx_dreq, (s->setup & SPI_SETUP_DMA) != 0);
}

static void s5l8900_spi_mark_tx_service(S5L8900SPIState *s)
{
    if (s->irq_pending & SPI_STATUS_TX_SERVICE) {
        s->tx_retrigger = true;
    } else {
        s->irq_pending |= SPI_STATUS_TX_SERVICE;
    }
}

static void s5l8900_spi_mark_rx_service(S5L8900SPIState *s)
{
    if (s->irq_pending & SPI_STATUS_RX_SERVICE) {
        s->rx_retrigger = true;
    } else {
        s->irq_pending |= SPI_STATUS_RX_SERVICE;
    }
}

static bool s5l8900_spi_transfer_active(S5L8900SPIState *s)
{
    return (s->control & SPI_CONTROL_ENABLE) ||
           (s->setup & SPI_SETUP_DMA) ||
           (s->setup & SPI_SETUP_SERVICE_MASK);
}

static void s5l8900_spi_drain_tx(S5L8900SPIState *s)
{
    unsigned transferred;
    bool dma = (s->setup & SPI_SETUP_DMA) != 0;

    if (!s5l8900_spi_transfer_active(s) || !s->tx_count) {
        return;
    }

    transferred = dma ? s->tx_count :
                  MIN(s->tx_count,
                      S5L8900_SPI_FIFO_DEPTH - s->rx_count);
    if (s->transfer_count_enabled) {
        transferred = MIN(transferred, s->transfer_count);
    }
    for (unsigned i = 0; i < transferred; i++) {
        uint32_t tx = s->tx_fifo[i];
        uint32_t rx = ssi_transfer(s->ssi, tx);

        if (s->transfer_count_enabled) {
            s->transfer_count--;
        }
        if (!dma) {
            unsigned tail = (s->rx_head + s->rx_count) %
                            S5L8900_SPI_FIFO_DEPTH;

            s->rx_fifo[tail] = rx;
            s->rx_count++;
        }
        trace_s5l8900_spi_fifo_push(false, tx, rx, s->rx_count,
                                    s->transfer_count);
    }
    if (!transferred) {
        return;
    }
    s->tx_count -= transferred;
    memmove(s->tx_fifo, &s->tx_fifo[transferred],
            s->tx_count * sizeof(s->tx_fifo[0]));
    if (!dma) {
        s5l8900_spi_mark_rx_service(s);
        s5l8900_spi_mark_tx_service(s);
    }
}

static void s5l8900_spi_fill_rx(S5L8900SPIState *s)
{
    uint32_t dummy = s5l8900_spi_word_mask(s);

    if (!s5l8900_spi_transfer_active(s) ||
        !(s->setup & SPI_SETUP_TRANSMIT_JUNK) ||
        !s->transfer_count_enabled || s->rx_count) {
        return;
    }

    s->rx_head = 0;
    while (s->rx_count < S5L8900_SPI_FIFO_DEPTH &&
           s->transfer_count) {
        uint32_t rx = ssi_transfer(s->ssi, dummy);

        s->rx_fifo[s->rx_count++] = rx;
        s->transfer_count--;
        trace_s5l8900_spi_fifo_push(true, dummy, rx, s->rx_count,
                                    s->transfer_count);
    }
    if (s->rx_count) {
        s5l8900_spi_mark_rx_service(s);
    }
}

static uint32_t s5l8900_spi_status(S5L8900SPIState *s)
{
    return s->irq_pending |
           (s->tx_count << SPI_STATUS_TX_COUNT_SHIFT) |
           (s->rx_count << SPI_STATUS_RX_COUNT_SHIFT);
}

static uint64_t s5l8900_spi_read(void *opaque, hwaddr offset,
                                  unsigned size)
{
    S5L8900SPIState *s = opaque;
    uint32_t value;

    if (offset != SPI_RXDATA && size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.spi: invalid %u-byte read at 0x%03"
                      HWADDR_PRIx "\n", size, offset);
        return 0;
    }

    switch (offset) {
    case SPI_CONTROL:
        return s->control;
    case SPI_SETUP:
        return s->setup;
    case SPI_STATUS:
        return s5l8900_spi_status(s);
    case SPI_PIN:
        return s->pin;
    case SPI_RXDATA:
        if (!s->rx_count) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "s5l8900.spi: read from empty RX FIFO\n");
            return 0;
        }
        value = s->rx_fifo[s->rx_head];
        s->rx_head = (s->rx_head + 1) % S5L8900_SPI_FIFO_DEPTH;
        s->rx_count--;
        trace_s5l8900_spi_fifo_pop(value, s->rx_count,
                                   s5l8900_spi_status(s));
        if (!s->rx_count &&
            !(s->irq_pending & SPI_STATUS_TX_SERVICE)) {
            s5l8900_spi_fill_rx(s);
            s5l8900_spi_update_irq(s);
        }
        return value;
    case SPI_CLOCK_DIVIDER:
        return s->clock_divider;
    case SPI_TRANSFER_COUNT:
        return s->transfer_count;
    case SPI_IDD:
        return s->idd;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.spi: unimplemented read at 0x%03"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void s5l8900_spi_write_status(S5L8900SPIState *s, uint32_t value)
{
    uint8_t clear = value & SPI_STATUS_IRQ_MASK;
    uint32_t before = s5l8900_spi_status(s);

    if (clear & SPI_STATUS_TX_SERVICE) {
        if (s->tx_retrigger) {
            s->tx_retrigger = false;
        } else {
            s->irq_pending &= ~SPI_STATUS_TX_SERVICE;
        }
    }
    if (clear & SPI_STATUS_RX_SERVICE) {
        if (s->rx_retrigger) {
            s->rx_retrigger = false;
        } else {
            s->irq_pending &= ~SPI_STATUS_RX_SERVICE;
        }
        s5l8900_spi_drain_tx(s);
        s5l8900_spi_fill_rx(s);
    }
    s->irq_pending &= ~(clear & ~(SPI_STATUS_TX_SERVICE |
                                  SPI_STATUS_RX_SERVICE));
    trace_s5l8900_spi_status_ack(value, before, s5l8900_spi_status(s),
                                 s->transfer_count);
    s5l8900_spi_update_irq(s);
}

static void s5l8900_spi_write_control(S5L8900SPIState *s, uint32_t value)
{
    if (value & SPI_CONTROL_RX_RESET) {
        s->rx_head = 0;
        s->rx_count = 0;
        s->transfer_count = 0;
        s->transfer_count_enabled = false;
        s->rx_retrigger = false;
        s->irq_pending &= ~SPI_STATUS_RX_SERVICE;
    }
    if (value & SPI_CONTROL_TX_RESET) {
        s->tx_count = 0;
        s->tx_retrigger = false;
        s->irq_pending &= ~SPI_STATUS_TX_SERVICE;
    }
    if (value & (SPI_CONTROL_RX_RESET | SPI_CONTROL_TX_RESET)) {
        s->control = 0;
    } else {
        s->control = value & SPI_CONTROL_ENABLE;
        s5l8900_spi_drain_tx(s);
        s5l8900_spi_fill_rx(s);
    }
    s5l8900_spi_update_irq(s);
}

static void s5l8900_spi_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    S5L8900SPIState *s = opaque;

    if (offset != SPI_TXDATA && size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.spi: invalid %u-byte write at 0x%03"
                      HWADDR_PRIx "\n", size, offset);
        return;
    }

    switch (offset) {
    case SPI_CONTROL:
        s5l8900_spi_write_control(s, value);
        break;
    case SPI_SETUP:
        s->setup = value;
        s5l8900_spi_drain_tx(s);
        s5l8900_spi_fill_rx(s);
        s5l8900_spi_update_irq(s);
        s5l8900_spi_update_dreq(s);
        break;
    case SPI_STATUS:
        s5l8900_spi_write_status(s, value);
        break;
    case SPI_PIN:
        s->pin = value;
        break;
    case SPI_TXDATA:
        if (s->tx_count == S5L8900_SPI_FIFO_DEPTH) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "s5l8900.spi: write to full TX FIFO\n");
            break;
        }
        s->tx_fifo[s->tx_count++] = value & s5l8900_spi_word_mask(s);
        s5l8900_spi_drain_tx(s);
        s5l8900_spi_update_irq(s);
        break;
    case SPI_CLOCK_DIVIDER:
        s->clock_divider = value & 0x3ff;
        break;
    case SPI_TRANSFER_COUNT:
        s->transfer_count = value;
        s->transfer_count_enabled = value != 0;
        s->rx_head = 0;
        s->rx_count = 0;
        s->rx_retrigger = false;
        s->irq_pending &= ~SPI_STATUS_RX_SERVICE;
        s5l8900_spi_update_irq(s);
        break;
    case SPI_IDD:
        s->idd = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.spi: unimplemented write 0x%08" PRIx64
                      " at 0x%03" HWADDR_PRIx "\n", value, offset);
        break;
    }
}

static const MemoryRegionOps s5l8900_spi_ops = {
    .read = s5l8900_spi_read,
    .write = s5l8900_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_spi_reset(DeviceState *dev)
{
    S5L8900SPIState *s = S5L8900_SPI(dev);

    s->control = 0;
    s->setup = 0;
    s->pin = 0;
    s->clock_divider = 0;
    s->transfer_count = 0;
    s->idd = 0;
    memset(s->tx_fifo, 0, sizeof(s->tx_fifo));
    memset(s->rx_fifo, 0, sizeof(s->rx_fifo));
    s->tx_count = 0;
    s->rx_head = 0;
    s->rx_count = 0;
    s->irq_pending = 0;
    s->tx_retrigger = false;
    s->rx_retrigger = false;
    s->transfer_count_enabled = false;
    s5l8900_spi_update_irq(s);
    s5l8900_spi_update_dreq(s);
}

static int s5l8900_spi_post_load(void *opaque, int version_id)
{
    S5L8900SPIState *s = opaque;

    if (version_id < 2) {
        s->rx_retrigger = false;
        s->transfer_count_enabled = s->transfer_count != 0;
    }

    if (s->tx_count > S5L8900_SPI_FIFO_DEPTH ||
        s->rx_head >= S5L8900_SPI_FIFO_DEPTH ||
        s->rx_count > S5L8900_SPI_FIFO_DEPTH ||
        s->irq_pending & ~SPI_STATUS_IRQ_MASK) {
        return -EINVAL;
    }
    s->control &= SPI_CONTROL_ENABLE;
    s5l8900_spi_update_irq(s);
    s5l8900_spi_update_dreq(s);
    return 0;
}

static const VMStateDescription vmstate_s5l8900_spi = {
    .name = TYPE_S5L8900_SPI,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = s5l8900_spi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(control, S5L8900SPIState),
        VMSTATE_UINT32(setup, S5L8900SPIState),
        VMSTATE_UINT32(pin, S5L8900SPIState),
        VMSTATE_UINT32(clock_divider, S5L8900SPIState),
        VMSTATE_UINT32(transfer_count, S5L8900SPIState),
        VMSTATE_UINT32(idd, S5L8900SPIState),
        VMSTATE_UINT32_ARRAY(tx_fifo, S5L8900SPIState,
                             S5L8900_SPI_FIFO_DEPTH),
        VMSTATE_UINT32_ARRAY(rx_fifo, S5L8900SPIState,
                             S5L8900_SPI_FIFO_DEPTH),
        VMSTATE_UINT8(tx_count, S5L8900SPIState),
        VMSTATE_UINT8(rx_head, S5L8900SPIState),
        VMSTATE_UINT8(rx_count, S5L8900SPIState),
        VMSTATE_UINT8(irq_pending, S5L8900SPIState),
        VMSTATE_BOOL(tx_retrigger, S5L8900SPIState),
        VMSTATE_BOOL_V(rx_retrigger, S5L8900SPIState, 2),
        VMSTATE_BOOL_V(transfer_count_enabled, S5L8900SPIState, 2),
        VMSTATE_END_OF_LIST()
    },
};

SSIBus *s5l8900_spi_get_bus(DeviceState *dev)
{
    return S5L8900_SPI(dev)->ssi;
}

static void s5l8900_spi_init(Object *obj)
{
    S5L8900SPIState *s = S5L8900_SPI(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_spi_ops, s,
                          TYPE_S5L8900_SPI, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out_named(dev, &s->tx_dreq, "tx-dreq", 1);
    s->ssi = ssi_create_bus(dev, "ssi");
}

static void s5l8900_spi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Apple S5L8900 synchronous serial controller";
    dc->vmsd = &vmstate_s5l8900_spi;
    device_class_set_legacy_reset(dc, s5l8900_spi_reset);
}

static const TypeInfo s5l8900_spi_info = {
    .name = TYPE_S5L8900_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900SPIState),
    .instance_init = s5l8900_spi_init,
    .class_init = s5l8900_spi_class_init,
};

static void s5l8900_spi_register_types(void)
{
    type_register_static(&s5l8900_spi_info);
}

type_init(s5l8900_spi_register_types)
