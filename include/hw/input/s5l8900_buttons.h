/*
 * Apple N82AP physical-button wiring
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INPUT_S5L8900_BUTTONS_H
#define HW_INPUT_S5L8900_BUTTONS_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "ui/input.h"

#define TYPE_S5L8900_BUTTONS "s5l8900-buttons"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900ButtonsState, S5L8900_BUTTONS)

#define S5L8900_BUTTON_COUNT 5

enum S5L8900Button {
    S5L8900_BUTTON_HOLD,
    S5L8900_BUTTON_MENU,
    S5L8900_BUTTON_VOLUME_UP,
    S5L8900_BUTTON_VOLUME_DOWN,
    S5L8900_BUTTON_RINGER,
};

struct S5L8900ButtonsState {
    SysBusDevice parent_obj;

    QemuInputHandlerState *handler;
    qemu_irq pin_level[S5L8900_BUTTON_COUNT];
    qemu_irq interrupt_level[S5L8900_BUTTON_COUNT];
    uint8_t pressed;
    bool ringer_key_down;
};

#endif
