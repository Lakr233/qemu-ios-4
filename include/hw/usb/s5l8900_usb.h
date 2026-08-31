/*
 * Apple S5L8900 USB device controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_USB_S5L8900_USB_H
#define HW_USB_S5L8900_USB_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_S5L8900_USB "s5l8900-usb"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900USBState, S5L8900_USB)

#endif
