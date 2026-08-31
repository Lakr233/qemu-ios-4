/*
 * Apple S5L8900 synchronous serial controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_S5L8900_SPI_H
#define HW_SSI_S5L8900_SPI_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_S5L8900_SPI "s5l8900-spi"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900SPIState, S5L8900_SPI)

#define S5L8900_SPI_FIFO_DEPTH 8

struct S5L8900SPIState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    SSIBus *ssi;
    qemu_irq irq;
    qemu_irq tx_dreq;

    uint32_t control;
    uint32_t setup;
    uint32_t pin;
    uint32_t clock_divider;
    uint32_t transfer_count;
    uint32_t idd;
    uint32_t tx_fifo[S5L8900_SPI_FIFO_DEPTH];
    uint32_t rx_fifo[S5L8900_SPI_FIFO_DEPTH];
    uint8_t tx_count;
    uint8_t rx_head;
    uint8_t rx_count;
    uint8_t irq_pending;
    bool tx_retrigger;
    bool rx_retrigger;
    bool transfer_count_enabled;
};

SSIBus *s5l8900_spi_get_bus(DeviceState *dev);

#endif
