/*
 * Intersil ISL29003 ambient-light sensor
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SENSOR_ISL29003_H
#define HW_SENSOR_ISL29003_H

#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_ISL29003 "isl29003"
OBJECT_DECLARE_SIMPLE_TYPE(ISL29003State, ISL29003)

#endif
