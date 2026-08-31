/*
 * Apple S5L8900 I2C controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I2C_S5L8900_I2C_H
#define HW_I2C_S5L8900_I2C_H

#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_S5L8900_I2C "s5l8900-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900I2CState, S5L8900_I2C)

struct S5L8900I2CState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;

    uint32_t control;
    uint32_t status;
    uint32_t address;
    uint32_t data;
    uint32_t line_control;
    uint32_t unknown[3];
    uint32_t operation_status;
    bool active;
    bool data_ready;
};

I2CBus *s5l8900_i2c_get_bus(DeviceState *dev);

#endif
