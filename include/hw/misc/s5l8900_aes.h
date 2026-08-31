/*
 * Apple S5L8900 AES accelerator
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_S5L8900_AES_H
#define HW_MISC_S5L8900_AES_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_S5L8900_AES "s5l8900-aes"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900AESState, S5L8900_AES)

#define S5L8900_AES_KEY_WORDS 8
#define S5L8900_AES_IV_WORDS 4

struct S5L8900AESState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion *dma_memory;
    AddressSpace dma_as;
    qemu_irq irq;
    QEMUTimer *completion_timer;

    uint32_t control;
    uint32_t unknown0;
    uint32_t status;
    uint32_t unknown1;
    uint32_t keylen;
    uint32_t transfer_size;
    uint32_t output_address;
    uint32_t output_capacity;
    uint32_t input_address;
    uint32_t input_capacity;
    uint32_t auxiliary_address;
    uint32_t size3;
    uint32_t remaining;
    uint32_t input_segment_offset;
    uint32_t output_segment_offset;
    uint8_t continuation_iv[16];
    bool active;
    uint32_t key[S5L8900_AES_KEY_WORDS];
    uint32_t key_type;
    uint32_t iv[S5L8900_AES_IV_WORDS];

    char *gid_key_secret;
    char *gid_kbag_secret;
    char *uid_key_secret;
    uint8_t *gid_kbags;
    uint8_t gid_key[16];
    uint8_t uid_key[16];
    bool has_gid_key;
    bool has_uid_key;
};

#endif
