/*
 * Apple S5L8900 PowerVR MBX wrapper
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_S5L8900_MBX_H
#define HW_DISPLAY_S5L8900_MBX_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"

typedef struct S5L8900MBXMetal S5L8900MBXMetal;

#define TYPE_S5L8900_MBX "s5l8900-mbx"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900MBXState, S5L8900_MBX)

#define S5L8900_MBX_APERTURE_SIZE 0x01000000
#define S5L8900_MBX_CONTROL_SIZE  0x00002000
#define S5L8900_MBX_REGISTER_COUNT \
    (S5L8900_MBX_CONTROL_SIZE / sizeof(uint32_t))

struct S5L8900MBXState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion aperture;
    MemoryRegion control;
    MemoryRegion ring;
    MemoryRegion fifo;
    MemoryRegion *system_memory;
    qemu_irq irq;

    uint32_t regs[S5L8900_MBX_REGISTER_COUNT];
    uint32_t status;
    bool reset_done;
    uint32_t pending_2d_offset;
    uint32_t pending_2d_count;
    bool pending_2d_valid;
    uint32_t ta_capture_count;
    bool ta_capture_failed;
    bool ta_in_flight;
    QEMUTimer *render_timer;
    bool render_completion_pending;
    /* Retained solely for version-7 migration stream compatibility. */
    bool render_3d_enabled;

    /* Host execution policy and resources; not Guest-visible or migratable. */
    bool metal_enabled;
    S5L8900MBXMetal *metal;
    uint64_t metal_submissions;

    /* Host diagnostics only; deliberately excluded from migration state. */
    uint64_t trace_read_offset;
    uint64_t trace_read_value;
    uint64_t trace_read_count;
    unsigned trace_read_size;
};

#endif /* HW_DISPLAY_S5L8900_MBX_H */
