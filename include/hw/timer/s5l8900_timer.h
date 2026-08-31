/*
 * Apple S5L8900 timer controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_S5L8900_TIMER_H
#define HW_TIMER_S5L8900_TIMER_H

#include "hw/core/ptimer.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_S5L8900_TIMER "s5l8900-timer"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900TimerState, S5L8900_TIMER)

#define S5L8900_TIMER_COUNT 7

typedef struct S5L8900TimerContext {
    S5L8900TimerState *parent;
    unsigned index;
} S5L8900TimerContext;

struct S5L8900TimerState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer[S5L8900_TIMER_COUNT];
    S5L8900TimerContext context[S5L8900_TIMER_COUNT];

    uint32_t config[S5L8900_TIMER_COUNT];
    uint32_t state[S5L8900_TIMER_COUNT];
    uint32_t count_buffer[S5L8900_TIMER_COUNT];
    uint32_t count_buffer2[S5L8900_TIMER_COUNT];
    uint32_t prescaler[S5L8900_TIMER_COUNT];
    uint32_t rtc_control[5];
    uint32_t irq_status;
};

#endif
