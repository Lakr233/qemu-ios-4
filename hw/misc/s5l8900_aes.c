/*
 * Apple S5L8900 AES accelerator
 *
 * The DMA register contract follows the S5L8900 OpeniBoot producer.  Fused
 * GID and UID keys are supplied by QEMU secret objects and are never exposed
 * through the Guest-visible key registers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "crypto/cipher.h"
#include "crypto/secret_common.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/s5l8900_aes.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "trace.h"

#define AES_CONTROL             0x00
#define AES_GO                  0x04
#define AES_UNKNOWN0            0x08
#define AES_STATUS              0x0c
#define AES_UNKNOWN1            0x10
#define AES_KEYLEN              0x14
#define AES_TRANSFER_SIZE       0x18
#define AES_OUTPUT_ADDRESS      0x20
#define AES_OUTPUT_CAPACITY     0x24
#define AES_INPUT_ADDRESS       0x28
#define AES_INPUT_CAPACITY      0x2c
#define AES_AUXILIARY_ADDRESS   0x30
#define AES_SIZE3               0x34
#define AES_KEY                 0x4c
#define AES_KEY_TYPE            0x6c
#define AES_IV                  0x74

#define AES_STATUS_DONE         BIT(0)
#define AES_STATUS_OUTPUT_REQ   BIT(1)
#define AES_STATUS_INPUT_REQ    BIT(2)
#define AES_KEYLEN_ENCRYPT      BIT(0)
#define AES_MAX_TRANSFER        (128 * MiB)
/*
 * Keep completion asynchronous with respect to GO, but publish it at the
 * next virtual-clock opportunity.  AppleS5L8900XAES's synchronous path polls
 * status only 0x2710 times; a 1 us delay can expire that loop before TCG
 * returns to the main loop and lets the timer run.
 */
#define AES_COMPLETION_NS       1
#define AES_KBAG_BUNDLE_HEADER  12
#define AES_KBAG_MAX_RECORDS    1024

static const uint8_t s5l8900_aes_kbag_magic[8] = {
    'S', '5', 'K', 'B', 'G', '0', '1', 0,
};

typedef enum S5L8900AESKeyType {
    S5L8900_AES_CUSTOM_KEY,
    S5L8900_AES_GID_KEY,
    S5L8900_AES_UID_KEY,
} S5L8900AESKeyType;

static void s5l8900_aes_update_irq(S5L8900AESState *s)
{
    qemu_set_irq(s->irq, (s->status & s->unknown1 & 0x7) != 0);
}

static void s5l8900_aes_complete(S5L8900AESState *s, int status)
{
    if (status < 0) {
        trace_s5l8900_aes_failure(s->key_type, s->keylen,
                                  s->transfer_size,
                                  s->input_address, s->input_capacity,
                                  s->output_address, s->output_capacity,
                                  s->auxiliary_address,
                                  s->size3);
        s->active = false;
        s->remaining = 0;
        s->status = 0;
    } else {
        s->status = status;
    }
    s5l8900_aes_update_irq(s);
}

static bool s5l8900_aes_get_custom_key(S5L8900AESState *s,
                                       uint8_t *key, size_t key_length)
{
    unsigned first_word = S5L8900_AES_KEY_WORDS - key_length / 4;

    for (unsigned i = 0; i < key_length / 4; i++) {
        stl_be_p(key + i * 4, s->key[first_word + i]);
    }
    return true;
}

static bool s5l8900_aes_get_key(S5L8900AESState *s, uint8_t *key,
                                size_t key_length)
{
    switch (s->key_type) {
    case S5L8900_AES_CUSTOM_KEY:
        return s5l8900_aes_get_custom_key(s, key, key_length);
    case S5L8900_AES_GID_KEY:
        if (key_length == sizeof(s->gid_key) && s->has_gid_key) {
            memcpy(key, s->gid_key, key_length);
            return true;
        }
        break;
    case S5L8900_AES_UID_KEY:
        if (key_length == sizeof(s->uid_key) && s->has_uid_key) {
            memcpy(key, s->uid_key, key_length);
            return true;
        }
        break;
    default:
        break;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "s5l8900.aes: selected key type is unavailable\n");
    return false;
}

static bool s5l8900_aes_transfer_valid(S5L8900AESState *s)
{
    uint64_t input_end = (uint64_t)s->input_address + s->input_capacity;
    uint64_t output_end = (uint64_t)s->output_address + s->output_capacity;

    if (s->transfer_size < 16 || s->transfer_size > AES_MAX_TRANSFER ||
        !s->input_capacity || !s->output_capacity ||
        s->size3 < s->transfer_size ||
        input_end > (uint64_t)UINT32_MAX + 1 ||
        output_end > (uint64_t)UINT32_MAX + 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.aes: invalid DMA transfer contract\n");
        return false;
    }
    if (s->active &&
        (s->input_segment_offset > s->input_capacity ||
         s->output_segment_offset > s->output_capacity)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.aes: invalid DMA segment cursor\n");
        return false;
    }
    return true;
}

static const uint8_t *s5l8900_aes_find_kbag(S5L8900AESState *s,
                                             const uint8_t *input,
                                             size_t input_length,
                                             bool encrypt)
{
    size_t offset = AES_KBAG_BUNDLE_HEADER;
    uint32_t count;

    if (!s->gid_kbags) {
        return NULL;
    }

    count = ldl_le_p(s->gid_kbags + sizeof(s5l8900_aes_kbag_magic));
    for (uint32_t i = 0; i < count; i++) {
        uint32_t record_length = ldl_le_p(s->gid_kbags + offset);
        const uint8_t *wrapped = s->gid_kbags + offset + sizeof(uint32_t);
        const uint8_t *clear = wrapped + record_length;
        const uint8_t *candidate = encrypt ? clear : wrapped;

        if (input_length == record_length &&
            !memcmp(input, candidate, record_length)) {
            return encrypt ? wrapped : clear;
        }
        offset += sizeof(uint32_t) + 2 * record_length;
    }
    return NULL;
}

static int s5l8900_aes_run(S5L8900AESState *s)
{
    g_autoptr(QCryptoCipher) cipher = NULL;
    g_autofree uint8_t *input = NULL;
    g_autofree uint8_t *output = NULL;
    QCryptoCipherAlgo algorithm;
    Error *local_err = NULL;
    uint8_t key[32] = { 0 };
    size_t key_length;
    size_t crypto_size;
    size_t input_available;
    size_t output_available;
    hwaddr input_address;
    hwaddr output_address;
    uint32_t output_before0 = 0;
    uint8_t output_before[sizeof(output_before0)];
    MemTxResult result;
    int ret;

    switch (extract32(s->keylen, 16, 2)) {
    case 0:
        algorithm = QCRYPTO_CIPHER_ALGO_AES_128;
        key_length = 16;
        break;
    case 1:
        algorithm = QCRYPTO_CIPHER_ALGO_AES_192;
        key_length = 24;
        break;
    case 2:
        algorithm = QCRYPTO_CIPHER_ALGO_AES_256;
        key_length = 32;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.aes: invalid AES key length\n");
        return -1;
    }

    if (!s5l8900_aes_transfer_valid(s)) {
        return -1;
    }
    /*
     * The S5L8900 engine consumes complete AES blocks and leaves a trailing
     * partial block untouched.  The 5A347 iBEC writes the IMG3 DATA logical
     * length directly (for example 0x9b5c for the DeviceTree), while the
     * OpeniBoot consumer expresses the same contract by rounding down before
     * submitting the operation.
     */
    if (!s->active) {
        s->remaining = s->transfer_size & ~0xfU;
        s->input_segment_offset = 0;
        s->output_segment_offset = 0;
        for (unsigned i = 0; i < S5L8900_AES_IV_WORDS; i++) {
            stl_be_p(s->continuation_iv + i * 4, s->iv[i]);
        }
        s->active = true;
    }

    input_available = s->input_capacity - s->input_segment_offset;
    output_available = s->output_capacity - s->output_segment_offset;
    crypto_size = MIN(s->remaining, MIN(input_available, output_available));
    crypto_size &= ~(size_t)0xf;
    if (!crypto_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.aes: DMA segment cannot supply an AES block\n");
        return -1;
    }
    input_address = s->input_address + s->input_segment_offset;
    output_address = s->output_address + s->output_segment_offset;

    input = g_malloc(crypto_size);
    output = g_malloc(crypto_size);
    result = address_space_read(&s->dma_as, input_address,
                                MEMTXATTRS_UNSPECIFIED, input,
                                crypto_size);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.aes: DMA input read failed\n");
        return -1;
    }
    /*
     * Retain the destination's first word as a producer/consumer witness.
     * AppleKeyStore fills the selected result buffer with 0xfeedface before
     * submitting the request, so this distinguishes a request that reached
     * the engine from an earlier driver rejection without changing DMA
     * behavior when the diagnostic read itself is unavailable.
     */
    if (address_space_read(&s->dma_as, output_address,
                           MEMTXATTRS_UNSPECIFIED, output_before,
                           sizeof(output_before)) == MEMTX_OK) {
        output_before0 = ldl_le_p(output_before);
    }

    if (s->key_type == S5L8900_AES_GID_KEY && s->gid_kbags &&
        s->input_segment_offset == 0 &&
        crypto_size == (s->transfer_size & ~0xfU)) {
        const uint8_t *kbag = s5l8900_aes_find_kbag(
            s, input, crypto_size, s->keylen & AES_KEYLEN_ENCRYPT);

        if (kbag) {
            memcpy(output, kbag, crypto_size);
            goto write_output;
        }
        if (!s->has_gid_key) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "s5l8900.aes: GID KBAG is not in the configured "
                          "oracle\n");
            return -1;
        }
    }

    if (!s5l8900_aes_get_key(s, key, key_length)) {
        return -1;
    }
    cipher = qcrypto_cipher_new(algorithm, QCRYPTO_CIPHER_MODE_CBC,
                                key, key_length, &local_err);
    if (!cipher) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.aes: cannot create cipher: %s\n",
                      error_get_pretty(local_err));
        error_free(local_err);
        return -1;
    }
    if (qcrypto_cipher_setiv(cipher, s->continuation_iv,
                             sizeof(s->continuation_iv), &local_err) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.aes: cannot set IV: %s\n",
                      error_get_pretty(local_err));
        error_free(local_err);
        return -1;
    }

    if (s->keylen & AES_KEYLEN_ENCRYPT) {
        ret = qcrypto_cipher_encrypt(cipher, input, output, crypto_size,
                                     &local_err);
    } else {
        ret = qcrypto_cipher_decrypt(cipher, input, output, crypto_size,
                                     &local_err);
    }
    if (ret < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.aes: cipher operation failed: %s\n",
                      error_get_pretty(local_err));
        error_free(local_err);
        return -1;
    }

    if (s->keylen & AES_KEYLEN_ENCRYPT) {
        memcpy(s->continuation_iv, output + crypto_size - 16, 16);
    } else {
        memcpy(s->continuation_iv, input + crypto_size - 16, 16);
    }

write_output:
    trace_s5l8900_aes_operation(s->key_type, s->keylen,
                                s->transfer_size,
                                input_address, output_address,
                                ldl_le_p(input), output_before0,
                                ldl_le_p(output));
    result = address_space_write(&s->dma_as, output_address,
                                 MEMTXATTRS_UNSPECIFIED, output,
                                 crypto_size);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.aes: DMA output write failed\n");
        return -1;
    }
    s->remaining -= crypto_size;
    s->input_segment_offset += crypto_size;
    s->output_segment_offset += crypto_size;
    if (!s->remaining) {
        s->active = false;
        return AES_STATUS_DONE;
    }

    ret = 0;
    if (s->input_capacity - s->input_segment_offset < 16) {
        ret |= AES_STATUS_INPUT_REQ;
    }
    if (s->output_capacity - s->output_segment_offset < 16) {
        ret |= AES_STATUS_OUTPUT_REQ;
    }
    if (!ret) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.aes: DMA segment made incomplete progress\n");
        return -1;
    }
    return ret;
}

static void s5l8900_aes_complete_timer(void *opaque)
{
    S5L8900AESState *s = opaque;

    s5l8900_aes_complete(s, s5l8900_aes_run(s));
}

static uint64_t s5l8900_aes_read(void *opaque, hwaddr offset,
                                 unsigned size)
{
    S5L8900AESState *s = opaque;
    uint32_t word;
    unsigned shift;

    if (offset >= AES_KEY &&
        offset + size <= AES_KEY + S5L8900_AES_KEY_WORDS * 4) {
        word = s->key[(offset - AES_KEY) / 4];
        shift = ((offset - AES_KEY) % 4) * 8;
        return extract32(word, shift, size * 8);
    }
    if (offset >= AES_IV &&
        offset + size <= AES_IV + S5L8900_AES_IV_WORDS * 4) {
        word = s->iv[(offset - AES_IV) / 4];
        shift = ((offset - AES_IV) % 4) * 8;
        return extract32(word, shift, size * 8);
    }

    if (size != 4) {
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.aes: unimplemented %u-byte read at 0x%03"
                      HWADDR_PRIx "\n", size, offset);
        return 0;
    }

    switch (offset) {
    case AES_CONTROL:
        return s->control;
    case AES_GO:
        return 0;
    case AES_UNKNOWN0:
        return s->unknown0;
    case AES_STATUS:
        /*
         * The stock synchronous driver spins on this register for only
         * 0x2710 reads.  TCG may execute that complete loop before returning
         * to the main loop to dispatch even a one-nanosecond timer.  Service
         * the already-deferred operation on the first poll: completion still
         * cannot occur in the GO write, while interrupt-only clients retain
         * the nonzero timer boundary.
         */
        if (timer_pending(s->completion_timer)) {
            timer_del(s->completion_timer);
            s5l8900_aes_complete(s, s5l8900_aes_run(s));
        }
        return s->status;
    case AES_UNKNOWN1:
        return s->unknown1;
    case AES_KEYLEN:
        return s->keylen;
    case AES_TRANSFER_SIZE:
        return s->transfer_size;
    case AES_INPUT_ADDRESS:
        return s->input_address;
    case AES_INPUT_CAPACITY:
        return s->input_capacity;
    case AES_OUTPUT_ADDRESS:
        return s->output_address;
    case AES_OUTPUT_CAPACITY:
        return s->output_capacity;
    case AES_AUXILIARY_ADDRESS:
        return s->auxiliary_address;
    case AES_SIZE3:
        return s->size3;
    case AES_KEY_TYPE:
        return s->key_type;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.aes: unimplemented read at 0x%03"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void s5l8900_aes_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    S5L8900AESState *s = opaque;
    uint32_t *word;
    unsigned shift;

    if (offset >= AES_KEY &&
        offset + size <= AES_KEY + S5L8900_AES_KEY_WORDS * 4) {
        word = &s->key[(offset - AES_KEY) / 4];
        shift = ((offset - AES_KEY) % 4) * 8;
        *word = deposit32(*word, shift, size * 8, value);
        return;
    }
    if (offset >= AES_IV &&
        offset + size <= AES_IV + S5L8900_AES_IV_WORDS * 4) {
        word = &s->iv[(offset - AES_IV) / 4];
        shift = ((offset - AES_IV) % 4) * 8;
        *word = deposit32(*word, shift, size * 8, value);
        return;
    }

    if (size != 4) {
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.aes: unimplemented %u-byte write at 0x%03"
                      HWADDR_PRIx "\n", size, offset);
        return;
    }

    switch (offset) {
    case AES_CONTROL:
        s->control = value;
        if (value & 1) {
            s->active = false;
            s->remaining = 0;
            s->input_segment_offset = 0;
            s->output_segment_offset = 0;
            s->status = 0;
            s5l8900_aes_update_irq(s);
        }
        break;
    case AES_GO:
        if ((value & 1) && !(s->status &
                            (AES_STATUS_INPUT_REQ | AES_STATUS_OUTPUT_REQ)) &&
            !timer_pending(s->completion_timer)) {
            s->status = 0;
            s5l8900_aes_update_irq(s);
            timer_mod(s->completion_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      AES_COMPLETION_NS);
        }
        break;
    case AES_UNKNOWN0:
        s->unknown0 = value;
        break;
    case AES_STATUS:
        s->status &= ~value;
        s5l8900_aes_update_irq(s);
        break;
    case AES_UNKNOWN1:
        s->unknown1 = value;
        s5l8900_aes_update_irq(s);
        break;
    case AES_KEYLEN:
        s->keylen = value;
        break;
    case AES_TRANSFER_SIZE:
        s->transfer_size = value;
        break;
    case AES_INPUT_ADDRESS:
        s->input_address = value;
        if (s->active && (s->status & AES_STATUS_INPUT_REQ)) {
            s->input_segment_offset = 0;
        }
        break;
    case AES_INPUT_CAPACITY:
        s->input_capacity = value;
        break;
    case AES_OUTPUT_ADDRESS:
        s->output_address = value;
        if (s->active && (s->status & AES_STATUS_OUTPUT_REQ)) {
            s->output_segment_offset = 0;
        }
        break;
    case AES_OUTPUT_CAPACITY:
        s->output_capacity = value;
        break;
    case AES_AUXILIARY_ADDRESS:
        s->auxiliary_address = value;
        break;
    case AES_SIZE3:
        s->size3 = value;
        break;
    case AES_KEY_TYPE:
        s->key_type = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.aes: unimplemented write 0x%08" PRIx64
                      " at 0x%03" HWADDR_PRIx "\n", value, offset);
        break;
    }
}

static const MemoryRegionOps s5l8900_aes_ops = {
    .read = s5l8900_aes_read,
    .write = s5l8900_aes_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_aes_reset(DeviceState *dev)
{
    S5L8900AESState *s = S5L8900_AES(dev);

    timer_del(s->completion_timer);
    s->control = 0;
    s->unknown0 = 0;
    s->status = 0;
    s->unknown1 = 0;
    s->keylen = 0;
    s->transfer_size = 0;
    s->input_address = 0;
    s->input_capacity = 0;
    s->output_address = 0;
    s->output_capacity = 0;
    s->auxiliary_address = 0;
    s->size3 = 0;
    s->remaining = 0;
    s->input_segment_offset = 0;
    s->output_segment_offset = 0;
    memset(s->continuation_iv, 0, sizeof(s->continuation_iv));
    s->active = false;
    memset(s->key, 0, sizeof(s->key));
    s->key_type = 0;
    memset(s->iv, 0, sizeof(s->iv));
    s5l8900_aes_update_irq(s);
}

static int s5l8900_aes_post_load(void *opaque, int version_id)
{
    S5L8900AESState *s = opaque;

    if (s->status & ~(AES_STATUS_DONE | AES_STATUS_INPUT_REQ |
                      AES_STATUS_OUTPUT_REQ)) {
        return -EINVAL;
    }
    if (!s->active && s->remaining) {
        return -EINVAL;
    }
    s5l8900_aes_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_s5l8900_aes = {
    .name = TYPE_S5L8900_AES,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = s5l8900_aes_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(control, S5L8900AESState),
        VMSTATE_UINT32(unknown0, S5L8900AESState),
        VMSTATE_UINT32(status, S5L8900AESState),
        VMSTATE_UINT32(unknown1, S5L8900AESState),
        VMSTATE_UINT32(keylen, S5L8900AESState),
        VMSTATE_UINT32(transfer_size, S5L8900AESState),
        VMSTATE_UINT32(output_address, S5L8900AESState),
        VMSTATE_UINT32(output_capacity, S5L8900AESState),
        VMSTATE_UINT32(input_address, S5L8900AESState),
        VMSTATE_UINT32(input_capacity, S5L8900AESState),
        VMSTATE_UINT32(auxiliary_address, S5L8900AESState),
        VMSTATE_UINT32(size3, S5L8900AESState),
        VMSTATE_UINT32_V(remaining, S5L8900AESState, 2),
        VMSTATE_UINT32_V(input_segment_offset, S5L8900AESState, 2),
        VMSTATE_UINT32_V(output_segment_offset, S5L8900AESState, 2),
        VMSTATE_UINT8_ARRAY_V(continuation_iv, S5L8900AESState, 16, 2),
        VMSTATE_BOOL_V(active, S5L8900AESState, 2),
        VMSTATE_UINT32_ARRAY(key, S5L8900AESState,
                             S5L8900_AES_KEY_WORDS),
        VMSTATE_UINT32(key_type, S5L8900AESState),
        VMSTATE_UINT32_ARRAY(iv, S5L8900AESState,
                             S5L8900_AES_IV_WORDS),
        VMSTATE_TIMER_PTR(completion_timer, S5L8900AESState),
        VMSTATE_END_OF_LIST()
    },
};

static bool s5l8900_aes_load_secret(const char *id, uint8_t key[16],
                                    Error **errp)
{
    g_autofree uint8_t *data = NULL;
    size_t length;

    if (!id) {
        return false;
    }
    if (qcrypto_secret_lookup(id, &data, &length, errp) < 0) {
        return false;
    }
    if (length != 16) {
        error_setg(errp, "AES fuse secret '%s' must contain exactly 16 bytes",
                   id);
        return false;
    }
    memcpy(key, data, length);
    return true;
}

static bool s5l8900_aes_validate_kbags(const uint8_t *data, size_t length,
                                        Error **errp)
{
    size_t offset = AES_KBAG_BUNDLE_HEADER;
    uint32_t count;

    if (length < AES_KBAG_BUNDLE_HEADER ||
        memcmp(data, s5l8900_aes_kbag_magic,
               sizeof(s5l8900_aes_kbag_magic))) {
        error_setg(errp, "S5L8900 GID KBAG bundle has an invalid header");
        return false;
    }

    count = ldl_le_p(data + sizeof(s5l8900_aes_kbag_magic));
    if (!count || count > AES_KBAG_MAX_RECORDS) {
        error_setg(errp, "S5L8900 GID KBAG bundle has an invalid count");
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t record_length;

        if (length - offset < sizeof(uint32_t)) {
            error_setg(errp, "S5L8900 GID KBAG bundle is truncated");
            return false;
        }
        record_length = ldl_le_p(data + offset);
        offset += sizeof(uint32_t);
        if (record_length != 32 && record_length != 40 &&
            record_length != 48) {
            error_setg(errp,
                       "S5L8900 GID KBAG record has an invalid length");
            return false;
        }
        if (record_length > (length - offset) / 2) {
            error_setg(errp, "S5L8900 GID KBAG bundle is truncated");
            return false;
        }
        offset += 2 * record_length;
    }

    if (offset != length) {
        error_setg(errp, "S5L8900 GID KBAG bundle has trailing data");
        return false;
    }
    return true;
}

static bool s5l8900_aes_load_kbags(S5L8900AESState *s, Error **errp)
{
    g_autofree uint8_t *data = NULL;
    size_t length;

    if (!s->gid_kbag_secret) {
        return true;
    }
    if (qcrypto_secret_lookup(s->gid_kbag_secret, &data, &length, errp) < 0) {
        return false;
    }
    if (!s5l8900_aes_validate_kbags(data, length, errp)) {
        return false;
    }
    s->gid_kbags = g_steal_pointer(&data);
    return true;
}

static void s5l8900_aes_realize(DeviceState *dev, Error **errp)
{
    S5L8900AESState *s = S5L8900_AES(dev);

    if (!s->dma_memory) {
        error_setg(errp, TYPE_S5L8900_AES " 'dma-memory' link not set");
        return;
    }
    address_space_init(&s->dma_as, s->dma_memory, "s5l8900-aes-dma");

    s->has_gid_key = s5l8900_aes_load_secret(s->gid_key_secret,
                                              s->gid_key, errp);
    if (s->gid_key_secret && !s->has_gid_key) {
        return;
    }
    if (!s5l8900_aes_load_kbags(s, errp)) {
        return;
    }
    s->has_uid_key = s5l8900_aes_load_secret(s->uid_key_secret,
                                              s->uid_key, errp);
}

static const Property s5l8900_aes_properties[] = {
    DEFINE_PROP_LINK("dma-memory", S5L8900AESState, dma_memory,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_STRING("gid-key-secret", S5L8900AESState, gid_key_secret),
    DEFINE_PROP_STRING("gid-kbag-secret", S5L8900AESState,
                       gid_kbag_secret),
    DEFINE_PROP_STRING("uid-key-secret", S5L8900AESState, uid_key_secret),
};

static void s5l8900_aes_init(Object *obj)
{
    S5L8900AESState *s = S5L8900_AES(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_aes_ops, s,
                          TYPE_S5L8900_AES, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->completion_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       s5l8900_aes_complete_timer, s);
}

static void s5l8900_aes_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Apple S5L8900 AES accelerator";
    dc->realize = s5l8900_aes_realize;
    dc->vmsd = &vmstate_s5l8900_aes;
    device_class_set_props(dc, s5l8900_aes_properties);
    device_class_set_legacy_reset(dc, s5l8900_aes_reset);
}

static void s5l8900_aes_finalize(Object *obj)
{
    S5L8900AESState *s = S5L8900_AES(obj);

    timer_free(s->completion_timer);
    g_free(s->gid_kbags);
}

static const TypeInfo s5l8900_aes_info = {
    .name = TYPE_S5L8900_AES,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900AESState),
    .instance_init = s5l8900_aes_init,
    .instance_finalize = s5l8900_aes_finalize,
    .class_init = s5l8900_aes_class_init,
};

static void s5l8900_aes_register_types(void)
{
    type_register_static(&s5l8900_aes_info);
}

type_init(s5l8900_aes_register_types)
