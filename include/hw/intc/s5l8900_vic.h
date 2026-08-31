/*
 * ARM PrimeCell PL192 interrupt controller as integrated in S5L8900.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INTC_S5L8900_VIC_H
#define HW_INTC_S5L8900_VIC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_S5L8900_VIC "s5l8900-vic"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900VICState, S5L8900_VIC)

#endif
