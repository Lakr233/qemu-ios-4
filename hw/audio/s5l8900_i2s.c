/*
 * Samsung S5L8900 I2S controller
 *
 * The initial model implements the register and PL080 request contract used
 * by the iPhone 3G boot software.  Transmit samples are accepted and receive
 * samples are silence until the WM8991 serial-audio path is connected.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/audio/s5l8900_i2s.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define S5L8900_I2S_CLKCON  0x00
#define S5L8900_I2S_TXCON   0x04
#define S5L8900_I2S_TXCOM   0x08
#define S5L8900_I2S_TXDATA  0x10
#define S5L8900_I2S_RXCON   0x30
#define S5L8900_I2S_RXCOM   0x34
#define S5L8900_I2S_RXDATA  0x38
#define S5L8900_I2S_STATUS  0x3c

#define S5L8900_I2S_CLOCK_ENABLE BIT(0)
#define S5L8900_I2S_DMA_ENABLE   BIT(1)
#define S5L8900_I2S_IF_ENABLE    BIT(2)

#define S5L8900_I2S_DMACS 2

struct S5L8900I2SState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t clkcon;
    uint32_t txcon;
    uint32_t txcom;
    uint32_t rxcon;
    uint32_t rxcom;
    qemu_irq tx_dreq[S5L8900_I2S_DMACS];
    qemu_irq rx_dreq[S5L8900_I2S_DMACS];
};

static void s5l8900_i2s_update(S5L8900I2SState *s)
{
    bool clock = s->clkcon & S5L8900_I2S_CLOCK_ENABLE;
    bool tx = clock &&
              (s->txcom & (S5L8900_I2S_DMA_ENABLE |
                           S5L8900_I2S_IF_ENABLE)) ==
              (S5L8900_I2S_DMA_ENABLE | S5L8900_I2S_IF_ENABLE);
    bool rx = clock &&
              (s->rxcom & (S5L8900_I2S_DMA_ENABLE |
                           S5L8900_I2S_IF_ENABLE)) ==
              (S5L8900_I2S_DMA_ENABLE | S5L8900_I2S_IF_ENABLE);

    for (unsigned i = 0; i < S5L8900_I2S_DMACS; i++) {
        qemu_set_irq(s->tx_dreq[i], tx);
        qemu_set_irq(s->rx_dreq[i], rx);
    }
}

static uint64_t s5l8900_i2s_read(void *opaque, hwaddr offset,
                                 unsigned size)
{
    S5L8900I2SState *s = opaque;

    switch (offset) {
    case S5L8900_I2S_CLKCON:
        return s->clkcon;
    case S5L8900_I2S_TXCON:
        return s->txcon;
    case S5L8900_I2S_TXCOM:
        return s->txcom;
    case S5L8900_I2S_RXCON:
        return s->rxcon;
    case S5L8900_I2S_RXCOM:
        return s->rxcom;
    case S5L8900_I2S_RXDATA:
    case S5L8900_I2S_STATUS:
        return 0;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read from unknown offset 0x%" HWADDR_PRIx "\n",
                      TYPE_S5L8900_I2S, offset);
        return 0;
    }
}

static void s5l8900_i2s_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    S5L8900I2SState *s = opaque;

    switch (offset) {
    case S5L8900_I2S_CLKCON:
        s->clkcon = value;
        s5l8900_i2s_update(s);
        break;
    case S5L8900_I2S_TXCON:
        s->txcon = value;
        break;
    case S5L8900_I2S_TXCOM:
        s->txcom = value;
        s5l8900_i2s_update(s);
        break;
    case S5L8900_I2S_TXDATA:
        break;
    case S5L8900_I2S_RXCON:
        s->rxcon = value;
        break;
    case S5L8900_I2S_RXCOM:
        s->rxcom = value;
        s5l8900_i2s_update(s);
        break;
    case S5L8900_I2S_STATUS:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to unknown offset 0x%" HWADDR_PRIx "\n",
                      TYPE_S5L8900_I2S, offset);
        break;
    }
}

static const MemoryRegionOps s5l8900_i2s_ops = {
    .read = s5l8900_i2s_read,
    .write = s5l8900_i2s_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 2,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static int s5l8900_i2s_post_load(void *opaque, int version_id)
{
    s5l8900_i2s_update(opaque);
    return 0;
}

static const VMStateDescription vmstate_s5l8900_i2s = {
    .name = TYPE_S5L8900_I2S,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8900_i2s_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(clkcon, S5L8900I2SState),
        VMSTATE_UINT32(txcon, S5L8900I2SState),
        VMSTATE_UINT32(txcom, S5L8900I2SState),
        VMSTATE_UINT32(rxcon, S5L8900I2SState),
        VMSTATE_UINT32(rxcom, S5L8900I2SState),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8900_i2s_reset(DeviceState *dev)
{
    S5L8900I2SState *s = S5L8900_I2S(dev);

    s->clkcon = 0;
    s->txcon = 0;
    s->txcom = 0;
    s->rxcon = 0;
    s->rxcom = 0;
    s5l8900_i2s_update(s);
}

static void s5l8900_i2s_init(Object *obj)
{
    S5L8900I2SState *s = S5L8900_I2S(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_i2s_ops, s,
                          TYPE_S5L8900_I2S, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    qdev_init_gpio_out_named(DEVICE(obj), s->tx_dreq, "tx-dreq",
                             ARRAY_SIZE(s->tx_dreq));
    qdev_init_gpio_out_named(DEVICE(obj), s->rx_dreq, "rx-dreq",
                             ARRAY_SIZE(s->rx_dreq));
}

static void s5l8900_i2s_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Samsung S5L8900 I2S controller";
    dc->vmsd = &vmstate_s5l8900_i2s;
    device_class_set_legacy_reset(dc, s5l8900_i2s_reset);
}

static const TypeInfo s5l8900_i2s_info = {
    .name = TYPE_S5L8900_I2S,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900I2SState),
    .instance_init = s5l8900_i2s_init,
    .class_init = s5l8900_i2s_class_init,
};

static void s5l8900_i2s_register_types(void)
{
    type_register_static(&s5l8900_i2s_info);
}

type_init(s5l8900_i2s_register_types)
