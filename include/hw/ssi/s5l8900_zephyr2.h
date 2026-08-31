/*
 * Apple N82 Zephyr2 multitouch controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_S5L8900_ZEPHYR2_H
#define HW_SSI_S5L8900_ZEPHYR2_H

#include "hw/ssi/ssi.h"
#include "qom/object.h"
#include "ui/input.h"

#define TYPE_S5L8900_ZEPHYR2 "s5l8900-zephyr2"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900Zephyr2State, S5L8900_ZEPHYR2)

#define S5L8900_ZEPHYR2_COMMAND_PREFIX_SIZE 16
#define S5L8900_ZEPHYR2_RESPONSE_SIZE       1024
/*
 * Keep the historical storage size migration-compatible.  The verified
 * 0xcc one-contact payload consumes only the first 42 bytes.
 */
#define S5L8900_ZEPHYR2_FRAME_SIZE          52
#define S5L8900_ZEPHYR2_FRAME_PAYLOAD_SIZE  42
#define S5L8900_ZEPHYR2_FRAME_QUEUE_SIZE    8

typedef enum S5L8900Zephyr2Ack {
    ZEPHYR2_ACK_NONE,
    ZEPHYR2_ACK_DATA,
    ZEPHYR2_ACK_WRITE,
    ZEPHYR2_ACK_READ,
} S5L8900Zephyr2Ack;

struct S5L8900Zephyr2State {
    SSIPeripheral parent_obj;

    qemu_irq atn;
    bool powered;
    bool reset_n;
    bool firmware_running;
    uint8_t pending_ack;
    uint32_t hbpp_read_address;
    bool hbpp_data_active;
    uint32_t hbpp_data_expected;
    uint32_t hbpp_data_received;
    uint8_t command_prefix[S5L8900_ZEPHYR2_COMMAND_PREFIX_SIZE];
    uint32_t command_len;
    uint8_t response[S5L8900_ZEPHYR2_RESPONSE_SIZE];
    uint16_t response_len;
    uint16_t response_pos;

    QemuInputHandlerState *input_handler;
    uint16_t input_x;
    uint16_t input_y;
    bool input_down;
    bool reported_down;
    bool input_dirty;
    uint32_t next_frame;
    uint8_t frames[S5L8900_ZEPHYR2_FRAME_QUEUE_SIZE]
                  [S5L8900_ZEPHYR2_FRAME_SIZE];
    uint8_t frame_head;
    uint8_t frame_count;
};

#endif
