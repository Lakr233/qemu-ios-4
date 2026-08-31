/*
 * Apple S5L8900 UART controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_S5L8900_UART_H
#define HW_CHAR_S5L8900_UART_H

#include "chardev/char-fe.h"
#include "hw/core/sysbus.h"
#include "qemu/fifo8.h"
#include "qom/object.h"

#define TYPE_S5L8900_UART "s5l8900-uart"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900UARTState, S5L8900_UART)

#define S5L8900_UART_RX_FIFO_DEPTH 16

struct S5L8900UARTState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    CharFrontend chr;
    qemu_irq irq;
    Fifo8 rx_fifo;

    uint32_t ulcon;
    uint32_t ucon;
    uint32_t ufcon;
    uint32_t umcon;
    uint32_t ubaud;
    uint32_t udivslot;
};

#endif
