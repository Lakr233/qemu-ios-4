/*
 * Apple S5L8900 public-key accelerator
 *
 * The register aperture, segment RAM, and RSA command boundary are derived
 * from the iPhone1,2 8C148 iBSS consumer.  That boot chain uses the PKE for
 * 64-, 128-, and 256-byte RSA public operations with exponent 65537.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/s5l8900_pke.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

#define PKE_CONTROL          0x000
#define PKE_COMMAND          0x008
#define PKE_OPERATION        0x00c
#define PKE_STATUS           0x010
#define PKE_SEGMENT_CONFIG   0x014
#define PKE_SOFTWARE_RESET   0x024
#define PKE_SEGMENT_BASE     0x800
#define PKE_MMIO_SIZE        0x1000
#define PKE_RSA_EXPONENT     65537
#define PKE_MAX_LIMBS        (256 / sizeof(uint32_t))
#define PKE_CONFIG_FINAL     BIT(1)

static int pke_compare(const uint32_t *a, const uint32_t *b, size_t limbs)
{
    for (size_t i = limbs; i > 0; i--) {
        if (a[i - 1] != b[i - 1]) {
            return a[i - 1] > b[i - 1] ? 1 : -1;
        }
    }
    return 0;
}

static bool pke_is_zero(const uint32_t *value, size_t limbs)
{
    for (size_t i = 0; i < limbs; i++) {
        if (value[i]) {
            return false;
        }
    }
    return true;
}

/* a and b are reduced, so one subtraction is sufficient for (a + b) mod n. */
static void pke_mod_add(uint32_t *result, const uint32_t *a,
                        const uint32_t *b, const uint32_t *modulus,
                        size_t limbs)
{
    uint32_t sum[PKE_MAX_LIMBS + 1] = { 0 };
    uint64_t carry = 0;

    for (size_t i = 0; i < limbs; i++) {
        uint64_t word = (uint64_t)a[i] + b[i] + carry;

        sum[i] = word;
        carry = word >> 32;
    }
    sum[limbs] = carry;

    if (carry || pke_compare(sum, modulus, limbs) >= 0) {
        uint64_t borrow = 0;

        for (size_t i = 0; i < limbs; i++) {
            uint64_t subtrahend = (uint64_t)modulus[i] + borrow;
            uint32_t old = sum[i];

            sum[i] = old - subtrahend;
            borrow = old < subtrahend;
        }
        sum[limbs] -= borrow;
    }

    memcpy(result, sum, limbs * sizeof(*result));
}

static void pke_mod_reduce(uint32_t *result, const uint32_t *value,
                           const uint32_t *modulus, size_t limbs)
{
    uint32_t input[PKE_MAX_LIMBS];
    uint32_t one[PKE_MAX_LIMBS] = { 1 };

    memcpy(input, value, limbs * sizeof(*input));
    memset(result, 0, limbs * sizeof(*result));
    for (size_t bit = limbs * 32; bit > 0; bit--) {
        pke_mod_add(result, result, result, modulus, limbs);
        if (input[(bit - 1) / 32] & BIT((bit - 1) % 32)) {
            pke_mod_add(result, result, one, modulus, limbs);
        }
    }
}

static void pke_mod_multiply(uint32_t *result, const uint32_t *a,
                             const uint32_t *b, const uint32_t *modulus,
                             size_t limbs)
{
    uint32_t accumulator[PKE_MAX_LIMBS] = { 0 };
    uint32_t addend[PKE_MAX_LIMBS];

    memcpy(addend, a, limbs * sizeof(*addend));
    for (size_t bit = 0; bit < limbs * 32; bit++) {
        if (b[bit / 32] & BIT(bit % 32)) {
            pke_mod_add(accumulator, accumulator, addend, modulus, limbs);
        }
        pke_mod_add(addend, addend, addend, modulus, limbs);
    }
    memcpy(result, accumulator, limbs * sizeof(*result));
}

static size_t s5l8900_pke_segment_size(const S5L8900PKEState *s)
{
    unsigned shift = s->segment_config >> 6;

    return shift <= 2 ? 256 >> shift : 0;
}

static bool s5l8900_pke_rsa_public(S5L8900PKEState *s)
{
    uint32_t modulus[PKE_MAX_LIMBS] = { 0 };
    uint32_t base[PKE_MAX_LIMBS] = { 0 };
    uint32_t result[PKE_MAX_LIMBS] = { 0 };
    uint32_t product[PKE_MAX_LIMBS];
    size_t bytes = s5l8900_pke_segment_size(s);
    size_t limbs;

    if (!bytes) {
        return false;
    }
    limbs = bytes / sizeof(uint32_t);
    for (size_t i = 0; i < limbs; i++) {
        modulus[i] = ldl_le_p(s->segments + i * sizeof(uint32_t));
        base[i] = ldl_le_p(s->segments + bytes + i * sizeof(uint32_t));
    }
    if (pke_is_zero(modulus, limbs)) {
        return false;
    }
    if (modulus[0] == 1 && pke_is_zero(modulus + 1, limbs - 1)) {
        memset(s->segments + bytes, 0, bytes);
        return true;
    }

    pke_mod_reduce(base, base, modulus, limbs);
    result[0] = 1;
    for (int bit = 16; bit >= 0; bit--) {
        pke_mod_multiply(product, result, result, modulus, limbs);
        memcpy(result, product, limbs * sizeof(*result));
        if (PKE_RSA_EXPONENT & BIT(bit)) {
            pke_mod_multiply(product, result, base, modulus, limbs);
            memcpy(result, product, limbs * sizeof(*result));
        }
    }
    for (size_t i = 0; i < limbs; i++) {
        stl_le_p(s->segments + bytes + i * sizeof(uint32_t), result[i]);
    }
    return true;
}

static void s5l8900_pke_command(S5L8900PKEState *s, uint32_t command)
{
    unsigned lhs = s->operation >> 24;
    unsigned modulus = (s->operation >> 8) & 0xff;
    unsigned destination = s->operation & 0xff;

    if (command == 9 && lhs == 1 && modulus == 0 && destination == 3) {
        s->phase = S5L8900_PKE_PHASE_MODULAR_EXPONENTIATION;
        return;
    }
    if (command == 1 && (s->segment_config & PKE_CONFIG_FINAL) &&
        s->operation == 0x02020001 &&
        s->phase == S5L8900_PKE_PHASE_MODULAR_EXPONENTIATION) {
        if (!s5l8900_pke_rsa_public(s)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "s5l8900.pke: invalid RSA public operation\n");
        }
        s->phase = S5L8900_PKE_PHASE_IDLE;
    }
}

static uint64_t s5l8900_pke_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8900PKEState *s = opaque;
    uint32_t value;

    if (offset >= PKE_SEGMENT_BASE &&
        offset <= PKE_SEGMENT_BASE + sizeof(s->segments) - size) {
        value = ldl_le_p(s->segments + offset - PKE_SEGMENT_BASE);
        trace_s5l8900_pke_segment_read(offset - PKE_SEGMENT_BASE, value);
        return value;
    }

    switch (offset) {
    case PKE_CONTROL:
        value = s->control;
        break;
    case PKE_COMMAND:
        value = 0;
        break;
    case PKE_OPERATION:
        value = s->operation;
        break;
    case PKE_STATUS:
        value = s->status;
        break;
    case PKE_SEGMENT_CONFIG:
        value = s->segment_config;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.pke: unimplemented read at 0x%03"
                      HWADDR_PRIx "\n", offset);
        value = 0;
        break;
    }

    return value;
}

static void s5l8900_pke_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    S5L8900PKEState *s = opaque;
    uint32_t value32 = value;

    if (offset >= PKE_SEGMENT_BASE &&
        offset <= PKE_SEGMENT_BASE + sizeof(s->segments) - size) {
        stl_le_p(s->segments + offset - PKE_SEGMENT_BASE, value32);
        trace_s5l8900_pke_segment_write(offset - PKE_SEGMENT_BASE, value32);
        return;
    }

    trace_s5l8900_pke_control_write(offset, value32);
    switch (offset) {
    case PKE_CONTROL:
        s->control = value32;
        break;
    case PKE_COMMAND:
        s5l8900_pke_command(s, value32);
        break;
    case PKE_OPERATION:
        s->operation = value32;
        break;
    case PKE_STATUS:
        s->status = value32;
        break;
    case PKE_SEGMENT_CONFIG:
        s->segment_config = value32;
        break;
    case PKE_SOFTWARE_RESET:
        s->control = 0;
        s->operation = 0;
        s->status = 0;
        s->segment_config = 0;
        s->phase = S5L8900_PKE_PHASE_IDLE;
        memset(s->segments, 0, sizeof(s->segments));
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.pke: unimplemented write 0x%08" PRIx32
                      " at 0x%03" HWADDR_PRIx "\n", value32, offset);
        break;
    }
}

static const MemoryRegionOps s5l8900_pke_ops = {
    .read = s5l8900_pke_read,
    .write = s5l8900_pke_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void s5l8900_pke_reset(DeviceState *dev)
{
    S5L8900PKEState *s = S5L8900_PKE(dev);

    s->control = 0;
    s->operation = 0;
    s->status = 0;
    s->segment_config = 0;
    s->phase = S5L8900_PKE_PHASE_IDLE;
    memset(s->segments, 0, sizeof(s->segments));
}

static const VMStateDescription vmstate_s5l8900_pke = {
    .name = TYPE_S5L8900_PKE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(control, S5L8900PKEState),
        VMSTATE_UINT32(operation, S5L8900PKEState),
        VMSTATE_UINT32(status, S5L8900PKEState),
        VMSTATE_UINT32(segment_config, S5L8900PKEState),
        VMSTATE_UINT32(phase, S5L8900PKEState),
        VMSTATE_UINT8_ARRAY(segments, S5L8900PKEState,
                            S5L8900_PKE_SEGMENT_BYTES),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8900_pke_init(Object *obj)
{
    S5L8900PKEState *s = S5L8900_PKE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_pke_ops, s,
                          TYPE_S5L8900_PKE, PKE_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void s5l8900_pke_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, s5l8900_pke_reset);
    dc->vmsd = &vmstate_s5l8900_pke;
}

static const TypeInfo s5l8900_pke_info = {
    .name = TYPE_S5L8900_PKE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900PKEState),
    .instance_init = s5l8900_pke_init,
    .class_init = s5l8900_pke_class_init,
};

static void s5l8900_pke_register_types(void)
{
    type_register_static(&s5l8900_pke_info);
}

type_init(s5l8900_pke_register_types)
