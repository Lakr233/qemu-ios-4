/*
 * Apple S5L8900 SHA-1 accelerator
 *
 * The register layout and command sequence follow the iPhone1,2 8C148
 * AppleS5L8900XSHA1 consumer.  The Guest owns SHA-1 padding; the accelerator
 * compresses complete 64-byte blocks and can continue from a supplied IV.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/s5l8900_sha1.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "trace.h"

#define SHA1_CONFIG            0x00
#define SHA1_RESET             0x04
#define SHA1_IRQ_ACK           0x08
#define SHA1_IRQ_ENABLE        0x0c
#define SHA1_DIGEST            0x20
#define SHA1_DATA              0x40
#define SHA1_DMA_CONTROL       0x80
#define SHA1_DMA_ADDRESS       0x84
#define SHA1_DMA_LENGTH        0x8c

#define SHA1_CONFIG_BUSY       BIT(0)
#define SHA1_CONFIG_START      BIT(1)
#define SHA1_CONFIG_IRQ        BIT(2)
#define SHA1_CONFIG_CUSTOM_IV  BIT(3)
#define SHA1_DMA_ENABLE        BIT(0)
#define SHA1_MAX_TRANSFER      (16 * MiB)

static const uint32_t sha1_initial_state[S5L8900_SHA1_DIGEST_WORDS] = {
    0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0,
};

static void s5l8900_sha1_update_irq(S5L8900SHA1State *s)
{
    qemu_set_irq(s->irq, s->irq_pending);
}

static void s5l8900_sha1_compress(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];

    for (unsigned i = 0; i < 16; i++) {
        w[i] = ldl_be_p(block + i * 4);
    }
    for (unsigned i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    for (unsigned i = 0; i < 80; i++) {
        uint32_t f;
        uint32_t k;
        uint32_t next;

        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }

        next = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = next;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static bool s5l8900_sha1_dma_valid(S5L8900SHA1State *s)
{
    uint64_t end = (uint64_t)s->dma_address + s->dma_length;

    if (!s->dma_length || s->dma_length > SHA1_MAX_TRANSFER ||
        s->dma_length % 64 || end > (uint64_t)UINT32_MAX + 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.sha1: invalid DMA transfer contract\n");
        return false;
    }
    return true;
}

static bool s5l8900_sha1_run(S5L8900SHA1State *s)
{
    uint8_t block[64];
    uint32_t state[S5L8900_SHA1_DIGEST_WORDS];

    if (s->config & SHA1_CONFIG_CUSTOM_IV) {
        memcpy(state, s->digest, sizeof(state));
    } else {
        memcpy(state, sha1_initial_state, sizeof(state));
    }

    trace_s5l8900_sha1_run_start(s->config, s->dma_control,
                                 s->dma_address, s->dma_length,
                                 state[0], state[1], state[2], state[3],
                                 state[4]);

    if (s->dma_control & SHA1_DMA_ENABLE) {
        if (!s5l8900_sha1_dma_valid(s)) {
            return false;
        }
        for (uint32_t done = 0; done < s->dma_length; done += sizeof(block)) {
            MemTxResult result = address_space_read(
                &s->dma_as, s->dma_address + done, MEMTXATTRS_UNSPECIFIED,
                block, sizeof(block));

            if (result != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "s5l8900.sha1: DMA input read failed\n");
                return false;
            }
            s5l8900_sha1_compress(state, block);
        }
    } else {
        for (unsigned i = 0; i < S5L8900_SHA1_BLOCK_WORDS; i++) {
            stl_le_p(block + i * 4, s->data[i]);
        }
        s5l8900_sha1_compress(state, block);
    }

    memcpy(s->digest, state, sizeof(state));
    trace_s5l8900_sha1_run_result(state[0], state[1], state[2], state[3],
                                  state[4]);
    return true;
}

static uint64_t s5l8900_sha1_read(void *opaque, hwaddr offset,
                                  unsigned size)
{
    S5L8900SHA1State *s = opaque;

    if (offset >= SHA1_DIGEST &&
        offset < SHA1_DIGEST + S5L8900_SHA1_DIGEST_WORDS * 4) {
        return bswap32(s->digest[(offset - SHA1_DIGEST) / 4]);
    }
    if (offset >= SHA1_DATA &&
        offset < SHA1_DATA + S5L8900_SHA1_BLOCK_WORDS * 4) {
        return s->data[(offset - SHA1_DATA) / 4];
    }

    switch (offset) {
    case SHA1_CONFIG:
        return s->config;
    case SHA1_RESET:
        return s->reset;
    case SHA1_IRQ_ACK:
        return s->irq_pending;
    case SHA1_IRQ_ENABLE:
        return s->irq_enable;
    case SHA1_DMA_CONTROL:
        return s->dma_control;
    case SHA1_DMA_ADDRESS:
        return s->dma_address;
    case SHA1_DMA_LENGTH:
        return s->dma_length;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.sha1: unimplemented read at 0x%03"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void s5l8900_sha1_reset_registers(S5L8900SHA1State *s)
{
    s->config = 0;
    s->irq_enable = 0;
    memset(s->digest, 0, sizeof(s->digest));
    memset(s->data, 0, sizeof(s->data));
    s->dma_control = 0;
    s->dma_address = 0;
    s->dma_length = 0;
    s->irq_pending = false;
    s5l8900_sha1_update_irq(s);
}

static void s5l8900_sha1_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    S5L8900SHA1State *s = opaque;

    if (offset >= SHA1_DIGEST &&
        offset < SHA1_DIGEST + S5L8900_SHA1_DIGEST_WORDS * 4) {
        s->digest[(offset - SHA1_DIGEST) / 4] = bswap32((uint32_t)value);
        return;
    }
    if (offset >= SHA1_DATA &&
        offset < SHA1_DATA + S5L8900_SHA1_BLOCK_WORDS * 4) {
        unsigned index = (offset - SHA1_DATA) / 4;

        s->data[index] = value;
        trace_s5l8900_sha1_data_write(index, value);
        return;
    }

    switch (offset) {
    case SHA1_CONFIG:
        s->config = value;
        if (value & SHA1_CONFIG_START) {
            s->config |= SHA1_CONFIG_BUSY;
            if (!s5l8900_sha1_run(s)) {
                memset(s->digest, 0, sizeof(s->digest));
            }
            s->config &= ~(SHA1_CONFIG_BUSY | SHA1_CONFIG_START);
            if ((value & SHA1_CONFIG_IRQ) && (s->irq_enable & 1)) {
                s->irq_pending = true;
                s5l8900_sha1_update_irq(s);
            }
        }
        break;
    case SHA1_RESET:
        trace_s5l8900_sha1_reset_write(value);
        s->reset = value;
        if (value & 1) {
            s5l8900_sha1_reset_registers(s);
            s->reset = value;
        }
        break;
    case SHA1_IRQ_ACK:
        if (value & 1) {
            s->irq_pending = false;
            s5l8900_sha1_update_irq(s);
        }
        break;
    case SHA1_IRQ_ENABLE:
        s->irq_enable = value;
        break;
    case SHA1_DMA_CONTROL:
        s->dma_control = value;
        break;
    case SHA1_DMA_ADDRESS:
        s->dma_address = value;
        break;
    case SHA1_DMA_LENGTH:
        s->dma_length = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.sha1: unimplemented write 0x%08" PRIx64
                      " at 0x%03" HWADDR_PRIx "\n", value, offset);
        break;
    }
}

static const MemoryRegionOps s5l8900_sha1_ops = {
    .read = s5l8900_sha1_read,
    .write = s5l8900_sha1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_sha1_reset(DeviceState *dev)
{
    S5L8900SHA1State *s = S5L8900_SHA1(dev);

    s->reset = 0;
    s5l8900_sha1_reset_registers(s);
}

static int s5l8900_sha1_post_load(void *opaque, int version_id)
{
    S5L8900SHA1State *s = opaque;

    if (s->config & SHA1_CONFIG_BUSY) {
        return -EINVAL;
    }
    s5l8900_sha1_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_s5l8900_sha1 = {
    .name = TYPE_S5L8900_SHA1,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8900_sha1_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(config, S5L8900SHA1State),
        VMSTATE_UINT32(reset, S5L8900SHA1State),
        VMSTATE_UINT32(irq_enable, S5L8900SHA1State),
        VMSTATE_UINT32_ARRAY(digest, S5L8900SHA1State,
                             S5L8900_SHA1_DIGEST_WORDS),
        VMSTATE_UINT32_ARRAY(data, S5L8900SHA1State,
                             S5L8900_SHA1_BLOCK_WORDS),
        VMSTATE_UINT32(dma_control, S5L8900SHA1State),
        VMSTATE_UINT32(dma_address, S5L8900SHA1State),
        VMSTATE_UINT32(dma_length, S5L8900SHA1State),
        VMSTATE_BOOL(irq_pending, S5L8900SHA1State),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8900_sha1_realize(DeviceState *dev, Error **errp)
{
    S5L8900SHA1State *s = S5L8900_SHA1(dev);

    if (!s->dma_memory) {
        error_setg(errp, TYPE_S5L8900_SHA1 " 'dma-memory' link not set");
        return;
    }
    address_space_init(&s->dma_as, s->dma_memory, "s5l8900-sha1-dma");
}

static const Property s5l8900_sha1_properties[] = {
    DEFINE_PROP_LINK("dma-memory", S5L8900SHA1State, dma_memory,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void s5l8900_sha1_init(Object *obj)
{
    S5L8900SHA1State *s = S5L8900_SHA1(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_sha1_ops, s,
                          TYPE_S5L8900_SHA1, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void s5l8900_sha1_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Apple S5L8900 SHA-1 accelerator";
    dc->realize = s5l8900_sha1_realize;
    dc->vmsd = &vmstate_s5l8900_sha1;
    device_class_set_props(dc, s5l8900_sha1_properties);
    device_class_set_legacy_reset(dc, s5l8900_sha1_reset);
}

static const TypeInfo s5l8900_sha1_info = {
    .name = TYPE_S5L8900_SHA1,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900SHA1State),
    .instance_init = s5l8900_sha1_init,
    .class_init = s5l8900_sha1_class_init,
};

static void s5l8900_sha1_register_types(void)
{
    type_register_static(&s5l8900_sha1_info);
}

type_init(s5l8900_sha1_register_types)
