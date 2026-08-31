/*
 * Apple S5L8900 edge interrupt controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INTC_S5L8900_EDGEIC_H
#define HW_INTC_S5L8900_EDGEIC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_S5L8900_EDGEIC "s5l8900-edgeic"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900EdgeICState, S5L8900_EDGEIC)

#define S5L8900_EDGEIC_NUM_IRQS 64

#endif
