/*
 * iPhone 3G Merlot panel and LM2512 serializer
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_S5L8900_MERLOT_H
#define HW_SSI_S5L8900_MERLOT_H

#include "hw/ssi/ssi.h"

#define TYPE_S5L8900_MERLOT "s5l8900-merlot"
#define TYPE_S5L8900_MERLOT_PANEL "s5l8900-merlot-panel"
#define TYPE_S5L8900_LM2512 "s5l8900-lm2512"

OBJECT_DECLARE_TYPE(S5L8900MerlotState, S5L8900MerlotClass,
                    S5L8900_MERLOT)

struct S5L8900MerlotClass {
    SSIPeripheralClass parent_class;
    bool serializer;
};

struct S5L8900MerlotState {
    SSIPeripheral parent_obj;

    uint32_t panel_id;
    uint8_t registers[256];
    uint8_t command;
    bool have_command;
};

#endif /* HW_SSI_S5L8900_MERLOT_H */
