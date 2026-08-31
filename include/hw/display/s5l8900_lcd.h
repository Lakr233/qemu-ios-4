/*
 * Apple S5L8900 LCD controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_S5L8900_LCD_H
#define HW_DISPLAY_S5L8900_LCD_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "ui/console.h"

#define TYPE_S5L8900_LCD "s5l8900-lcd"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900LCDState, S5L8900_LCD)

#define S5L8900_LCD_MMIO_SIZE      0x1000
#define S5L8900_LCD_REGISTER_COUNT (S5L8900_LCD_MMIO_SIZE / sizeof(uint32_t))
#define S5L8900_LCD_WINDOWS        5

struct S5L8900LCDState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion *fbmem;
    MemoryRegionSection fbsection;
    MemoryRegionSection composite_fbsections[S5L8900_LCD_WINDOWS];
    qemu_irq irq;
    QemuConsole *con;
    QEMUTimer *frame_timer;

    uint32_t regs[S5L8900_LCD_REGISTER_COUNT];
    uint32_t frame_remainder;

    hwaddr cached_fb_base;
    uint32_t cached_stride;
    uint8_t cached_format;
    hwaddr composite_cached_fb_base[S5L8900_LCD_WINDOWS];
    uint32_t composite_cached_stride[S5L8900_LCD_WINDOWS];
    uint8_t composite_cached_format[S5L8900_LCD_WINDOWS];
    uint8_t *composite_shadow[S5L8900_LCD_WINDOWS];
    bool scanning;
    bool invalidate;
    bool blanked;
};

#endif /* HW_DISPLAY_S5L8900_LCD_H */
