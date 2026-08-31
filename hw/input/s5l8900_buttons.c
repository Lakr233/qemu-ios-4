/*
 * Apple N82AP physical-button wiring
 *
 * The five pins, polarities, and Guest meanings are from the iPhone1,2 8C148
 * DeviceTree and AppleM68Buttons.  They were independently documented by the
 * MIT-licensed S5LBox project at commit 6f203ba550b49afadee008c7eb55373a838eed33.
 *
 * Copyright (c) 2026 QEMU iPhone contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/input/s5l8900_buttons.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

static bool s5l8900_button_level(S5L8900ButtonsState *s, unsigned button)
{
    bool pressed = (s->pressed & BIT(button)) != 0;

    switch (button) {
    case S5L8900_BUTTON_HOLD:
    case S5L8900_BUTTON_MENU:
        return pressed;
    case S5L8900_BUTTON_VOLUME_UP:
    case S5L8900_BUTTON_VOLUME_DOWN:
        return !pressed;
    case S5L8900_BUTTON_RINGER:
        /* AppleM68Buttons' second inversion makes raw high mean muted. */
        return pressed;
    default:
        g_assert_not_reached();
    }
}

static void s5l8900_buttons_apply(S5L8900ButtonsState *s)
{
    for (unsigned i = 0; i < S5L8900_BUTTON_COUNT; i++) {
        bool level = s5l8900_button_level(s, i);

        qemu_set_irq(s->pin_level[i], level);
        qemu_set_irq(s->interrupt_level[i], level);
    }
}

static void s5l8900_buttons_event(DeviceState *dev, QemuConsole *src,
                                  QemuInputEvent *evt)
{
    S5L8900ButtonsState *s = S5L8900_BUTTONS(dev);
    int qcode = qemu_input_linux_to_qcode(evt->key.key);
    unsigned button;
    bool pressed = evt->key.down;

    switch (qcode) {
    case Q_KEY_CODE_POWER:
        button = S5L8900_BUTTON_HOLD;
        break;
    case Q_KEY_CODE_HOME:
        button = S5L8900_BUTTON_MENU;
        break;
    case Q_KEY_CODE_VOLUMEUP:
        button = S5L8900_BUTTON_VOLUME_UP;
        break;
    case Q_KEY_CODE_VOLUMEDOWN:
        button = S5L8900_BUTTON_VOLUME_DOWN;
        break;
    case Q_KEY_CODE_AUDIOMUTE:
        button = S5L8900_BUTTON_RINGER;
        if (pressed == s->ringer_key_down) {
            return;
        }
        s->ringer_key_down = pressed;
        if (!pressed) {
            return;
        }
        pressed = !(s->pressed & BIT(button));
        break;
    default:
        return;
    }

    if (pressed) {
        s->pressed |= BIT(button);
    } else {
        s->pressed &= ~BIT(button);
    }
    qemu_set_irq(s->pin_level[button],
                 s5l8900_button_level(s, button));
    qemu_set_irq(s->interrupt_level[button],
                 s5l8900_button_level(s, button));
}

static const QemuInputHandler s5l8900_buttons_handler = {
    .name = "iPhone 3G buttons",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = s5l8900_buttons_event,
};

static void s5l8900_buttons_reset_enter(Object *obj, ResetType type)
{
    S5L8900ButtonsState *s = S5L8900_BUTTONS(obj);

    s->pressed = 0;
    s->ringer_key_down = false;
}

static void s5l8900_buttons_reset_exit(Object *obj, ResetType type)
{
    S5L8900ButtonsState *s = S5L8900_BUTTONS(obj);

    s5l8900_buttons_apply(s);
}

static int s5l8900_buttons_post_load(void *opaque, int version_id)
{
    S5L8900ButtonsState *s = opaque;

    s->pressed &= MAKE_64BIT_MASK(0, S5L8900_BUTTON_COUNT);
    s5l8900_buttons_apply(s);
    return 0;
}

static const VMStateDescription vmstate_s5l8900_buttons = {
    .name = TYPE_S5L8900_BUTTONS,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8900_buttons_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(pressed, S5L8900ButtonsState),
        VMSTATE_BOOL(ringer_key_down, S5L8900ButtonsState),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8900_buttons_realize(DeviceState *dev, Error **errp)
{
    S5L8900ButtonsState *s = S5L8900_BUTTONS(dev);

    s->handler = qemu_input_handler_register(dev, &s5l8900_buttons_handler);
    s5l8900_buttons_apply(s);
}

static void s5l8900_buttons_unrealize(DeviceState *dev)
{
    S5L8900ButtonsState *s = S5L8900_BUTTONS(dev);

    g_clear_pointer(&s->handler, qemu_input_handler_unregister);
}

static void s5l8900_buttons_init(Object *obj)
{
    S5L8900ButtonsState *s = S5L8900_BUTTONS(obj);

    qdev_init_gpio_out_named(DEVICE(obj), s->pin_level, "pin-level",
                             S5L8900_BUTTON_COUNT);
    qdev_init_gpio_out_named(DEVICE(obj), s->interrupt_level,
                             "interrupt-level",
                             S5L8900_BUTTON_COUNT);
}

static void s5l8900_buttons_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    rc->phases.enter = s5l8900_buttons_reset_enter;
    rc->phases.exit = s5l8900_buttons_reset_exit;
    dc->desc = "Apple N82AP physical buttons";
    dc->realize = s5l8900_buttons_realize;
    dc->unrealize = s5l8900_buttons_unrealize;
    dc->vmsd = &vmstate_s5l8900_buttons;
}

static const TypeInfo s5l8900_buttons_info = {
    .name = TYPE_S5L8900_BUTTONS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900ButtonsState),
    .instance_init = s5l8900_buttons_init,
    .class_init = s5l8900_buttons_class_init,
};

static void s5l8900_buttons_register_types(void)
{
    type_register_static(&s5l8900_buttons_info);
}

type_init(s5l8900_buttons_register_types)
