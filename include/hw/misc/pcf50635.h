/*
 * NXP PCF50635 power-management unit
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_PCF50635_H
#define HW_MISC_PCF50635_H

#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_PCF50635 "pcf50635"
OBJECT_DECLARE_SIMPLE_TYPE(PCF50635State, PCF50635)

#endif
