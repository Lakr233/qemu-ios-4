/*
 * Apple S5L8900 watchdog
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_WATCHDOG_S5L8900_WATCHDOG_H
#define HW_WATCHDOG_S5L8900_WATCHDOG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_S5L8900_WATCHDOG "s5l8900-watchdog"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900WatchdogState, S5L8900_WATCHDOG)

struct S5L8900WatchdogState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;

    uint32_t control;
    bool irq_pending;
};

#endif
