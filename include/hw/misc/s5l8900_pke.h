/*
 * Apple S5L8900 public-key accelerator
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_S5L8900_PKE_H
#define HW_MISC_S5L8900_PKE_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_S5L8900_PKE "s5l8900-pke"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900PKEState, S5L8900_PKE)

#define S5L8900_PKE_SEGMENT_BYTES 2048

typedef enum S5L8900PKEPhase {
    S5L8900_PKE_PHASE_IDLE,
    S5L8900_PKE_PHASE_MODULAR_EXPONENTIATION,
} S5L8900PKEPhase;

struct S5L8900PKEState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t control;
    uint32_t operation;
    uint32_t status;
    uint32_t segment_config;
    S5L8900PKEPhase phase;
    uint8_t segments[S5L8900_PKE_SEGMENT_BYTES];
};

#endif
