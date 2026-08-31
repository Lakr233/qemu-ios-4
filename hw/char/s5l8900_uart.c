/*
 * Apple S5L8900 UART controller
 *
 * The register contract follows the S5L8900 OpeniBoot UART driver.  It is
 * intentionally separate from later Samsung UART generations because their
 * FIFO status fields are not compatible.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/char/s5l8900_uart.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define UART_ULCON              0x00
#define UART_UCON               0x04
#define UART_UFCON              0x08
#define UART_UMCON              0x0c
#define UART_UTRSTAT            0x10
#define UART_UERSTAT            0x14
#define UART_UFSTAT             0x18
#define UART_UMSTAT             0x1c
#define UART_UTXH               0x20
#define UART_URXH               0x24
#define UART_UBAUD              0x28
#define UART_UDIVSLOT           0x2c

#define UART_UCON_RX_MODE_MASK  0x3
#define UART_UCON_IRQ_OR_POLL   0x1
#define UART_UCON_LOOPBACK      BIT(5)
#define UART_UFCON_ENABLE       BIT(0)
#define UART_UFCON_RX_RESET     BIT(1)
#define UART_UFCON_TX_RESET     BIT(2)
#define UART_UTRSTAT_RX_READY   BIT(0)
#define UART_UTRSTAT_TX_EMPTY   (BIT(1) | BIT(2))
#define UART_UFSTAT_RX_FULL     BIT(8)
#define UART_UMSTAT_CTS         BIT(0)

static bool s5l8900_uart_rx_irq_enabled(S5L8900UARTState *s)
{
    return (s->ucon & UART_UCON_RX_MODE_MASK) == UART_UCON_IRQ_OR_POLL;
}

static void s5l8900_uart_update_irq(S5L8900UARTState *s)
{
    qemu_set_irq(s->irq, !fifo8_is_empty(&s->rx_fifo) &&
                 s5l8900_uart_rx_irq_enabled(s));
}

static uint32_t s5l8900_uart_utrstat(S5L8900UARTState *s)
{
    return UART_UTRSTAT_TX_EMPTY |
           (fifo8_is_empty(&s->rx_fifo) ? 0 : UART_UTRSTAT_RX_READY);
}

static uint32_t s5l8900_uart_ufstat(S5L8900UARTState *s)
{
    unsigned used = fifo8_num_used(&s->rx_fifo);

    return (used == S5L8900_UART_RX_FIFO_DEPTH ?
            UART_UFSTAT_RX_FULL : used) & 0x1ff;
}

static void s5l8900_uart_push_rx(S5L8900UARTState *s, uint8_t byte)
{
    if (fifo8_is_full(&s->rx_fifo)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.uart: receive FIFO overflow\n");
        return;
    }

    fifo8_push(&s->rx_fifo, byte);
    s5l8900_uart_update_irq(s);
}

static uint64_t s5l8900_uart_read(void *opaque, hwaddr offset,
                                  unsigned size)
{
    S5L8900UARTState *s = opaque;
    uint8_t byte;

    switch (offset) {
    case UART_ULCON:
        return s->ulcon;
    case UART_UCON:
        return s->ucon;
    case UART_UFCON:
        return s->ufcon;
    case UART_UMCON:
        return s->umcon;
    case UART_UTRSTAT:
        return s5l8900_uart_utrstat(s);
    case UART_UERSTAT:
        return 0;
    case UART_UFSTAT:
        return s5l8900_uart_ufstat(s);
    case UART_UMSTAT:
        return UART_UMSTAT_CTS;
    case UART_URXH:
        if (fifo8_is_empty(&s->rx_fifo)) {
            return 0;
        }
        byte = fifo8_pop(&s->rx_fifo);
        qemu_chr_fe_accept_input(&s->chr);
        s5l8900_uart_update_irq(s);
        return byte;
    case UART_UBAUD:
        return s->ubaud;
    case UART_UDIVSLOT:
        return s->udivslot;
    case UART_UTXH:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.uart: read from write-only UTXH\n");
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.uart: unimplemented read at 0x%03"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void s5l8900_uart_write_ufcon(S5L8900UARTState *s, uint32_t value)
{
    if (value & UART_UFCON_RX_RESET) {
        fifo8_reset(&s->rx_fifo);
        qemu_chr_fe_accept_input(&s->chr);
    }
    s->ufcon = value & ~(UART_UFCON_RX_RESET | UART_UFCON_TX_RESET);
    s5l8900_uart_update_irq(s);
}

static void s5l8900_uart_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    S5L8900UARTState *s = opaque;
    uint8_t byte = value;

    switch (offset) {
    case UART_ULCON:
        s->ulcon = value;
        break;
    case UART_UCON:
        s->ucon = value;
        s5l8900_uart_update_irq(s);
        break;
    case UART_UFCON:
        s5l8900_uart_write_ufcon(s, value);
        break;
    case UART_UMCON:
        s->umcon = value;
        break;
    case UART_UTXH:
        if (s->ucon & UART_UCON_LOOPBACK) {
            s5l8900_uart_push_rx(s, byte);
        } else if (qemu_chr_fe_backend_connected(&s->chr)) {
            qemu_chr_fe_write_all(&s->chr, &byte, 1);
        }
        break;
    case UART_UBAUD:
        s->ubaud = value;
        break;
    case UART_UDIVSLOT:
        s->udivslot = value;
        break;
    case UART_UTRSTAT:
    case UART_UERSTAT:
    case UART_UFSTAT:
    case UART_UMSTAT:
    case UART_URXH:
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.uart: unimplemented write 0x%08" PRIx64
                      " at 0x%03" HWADDR_PRIx "\n", value, offset);
        break;
    }
}

static const MemoryRegionOps s5l8900_uart_ops = {
    .read = s5l8900_uart_read,
    .write = s5l8900_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static int s5l8900_uart_can_receive(void *opaque)
{
    S5L8900UARTState *s = opaque;

    return fifo8_num_free(&s->rx_fifo);
}

static void s5l8900_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    S5L8900UARTState *s = opaque;

    assert(size <= fifo8_num_free(&s->rx_fifo));
    fifo8_push_all(&s->rx_fifo, buf, size);
    s5l8900_uart_update_irq(s);
}

static void s5l8900_uart_reset(DeviceState *dev)
{
    S5L8900UARTState *s = S5L8900_UART(dev);

    s->ulcon = 0;
    s->ucon = 0x3000;
    s->ufcon = 0;
    s->umcon = 0;
    s->ubaud = 0;
    s->udivslot = 0;
    fifo8_reset(&s->rx_fifo);
    s5l8900_uart_update_irq(s);
}

static int s5l8900_uart_post_load(void *opaque, int version_id)
{
    S5L8900UARTState *s = opaque;

    s5l8900_uart_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_s5l8900_uart = {
    .name = TYPE_S5L8900_UART,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8900_uart_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ulcon, S5L8900UARTState),
        VMSTATE_UINT32(ucon, S5L8900UARTState),
        VMSTATE_UINT32(ufcon, S5L8900UARTState),
        VMSTATE_UINT32(umcon, S5L8900UARTState),
        VMSTATE_UINT32(ubaud, S5L8900UARTState),
        VMSTATE_UINT32(udivslot, S5L8900UARTState),
        VMSTATE_FIFO8(rx_fifo, S5L8900UARTState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property s5l8900_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", S5L8900UARTState, chr),
};

static void s5l8900_uart_realize(DeviceState *dev, Error **errp)
{
    S5L8900UARTState *s = S5L8900_UART(dev);

    qemu_chr_fe_set_handlers(&s->chr, s5l8900_uart_can_receive,
                             s5l8900_uart_receive, NULL, NULL,
                             s, NULL, true);
}

static void s5l8900_uart_init(Object *obj)
{
    S5L8900UARTState *s = S5L8900_UART(obj);

    fifo8_create(&s->rx_fifo, S5L8900_UART_RX_FIFO_DEPTH);
    memory_region_init_io(&s->iomem, obj, &s5l8900_uart_ops, s,
                          TYPE_S5L8900_UART, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void s5l8900_uart_finalize(Object *obj)
{
    S5L8900UARTState *s = S5L8900_UART(obj);

    fifo8_destroy(&s->rx_fifo);
}

static void s5l8900_uart_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Apple S5L8900 UART controller";
    dc->realize = s5l8900_uart_realize;
    dc->vmsd = &vmstate_s5l8900_uart;
    device_class_set_props(dc, s5l8900_uart_properties);
    device_class_set_legacy_reset(dc, s5l8900_uart_reset);
}

static const TypeInfo s5l8900_uart_info = {
    .name = TYPE_S5L8900_UART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900UARTState),
    .instance_init = s5l8900_uart_init,
    .instance_finalize = s5l8900_uart_finalize,
    .class_init = s5l8900_uart_class_init,
};

static void s5l8900_uart_register_types(void)
{
    type_register_static(&s5l8900_uart_info);
}

type_init(s5l8900_uart_register_types)
