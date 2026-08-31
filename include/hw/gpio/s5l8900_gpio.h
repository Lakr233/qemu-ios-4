/*
 * Apple S5L8900 system controller and GPIO
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_S5L8900_GPIO_H
#define HW_GPIO_S5L8900_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_S5L8900_GPIO "s5l8900-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900GPIOState, S5L8900_GPIO)

#define S5L8900_GPIO_PAD_COUNT       25
#define S5L8900_GPIO_PINS_PER_PAD    8
#define S5L8900_GPIO_PIN_COUNT       \
    (S5L8900_GPIO_PAD_COUNT * S5L8900_GPIO_PINS_PER_PAD)
#define S5L8900_GPIO_INTERRUPT_GROUPS 7

struct S5L8900GPIOState {
    SysBusDevice parent_obj;

    MemoryRegion sysic_iomem;
    MemoryRegion gpio_iomem;
    MemoryRegion *vrom_alias;
    uint8_t security_epoch;
    qemu_irq irq[S5L8900_GPIO_INTERRUPT_GROUPS];
    qemu_irq pin_out[S5L8900_GPIO_PIN_COUNT];

    uint32_t power_config[3];
    uint32_t memory_config;
    uint32_t power_state;
    uint32_t int_level[S5L8900_GPIO_INTERRUPT_GROUPS];
    uint32_t int_enable[S5L8900_GPIO_INTERRUPT_GROUPS];
    uint32_t int_type[S5L8900_GPIO_INTERRUPT_GROUPS];
    uint32_t edge_pending[S5L8900_GPIO_INTERRUPT_GROUPS];
    uint32_t pin_level[S5L8900_GPIO_INTERRUPT_GROUPS];
    uint32_t interrupt_level[S5L8900_GPIO_INTERRUPT_GROUPS];

    uint32_t con[S5L8900_GPIO_PAD_COUNT];
    uint32_t output_latch[S5L8900_GPIO_PAD_COUNT];
    uint32_t pud1[S5L8900_GPIO_PAD_COUNT];
    uint32_t pud2[S5L8900_GPIO_PAD_COUNT];
    uint32_t conslp1[S5L8900_GPIO_PAD_COUNT];
    uint32_t conslp2[S5L8900_GPIO_PAD_COUNT];
    uint32_t pudslp1[S5L8900_GPIO_PAD_COUNT];
    uint32_t pudslp2[S5L8900_GPIO_PAD_COUNT];
};

#endif
