/*
 * Apple N82 Zephyr2 multitouch controller
 *
 * This implements the HBPP boot acknowledgements and the report control
 * plane consumed by the iPhone 3G AppleMultitouchSPIZ2F52 driver, plus the
 * normal 0xcc finger frames delivered by the same device.
 *
 * The generation-specific report payloads and transaction shapes were
 * independently documented by the MIT-licensed S5LBox project at commit
 * 6f203ba550b49afadee008c7eb55373a838eed33.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/ssi/s5l8900_zephyr2.h"
#include "migration/vmstate.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "trace.h"

#define ZEPHYR2_INTERFACE_VERSION 1
#define ZEPHYR2_MAX_PACKET_SIZE   660
#define ZEPHYR2_FAMILY_ID         0x52
#define ZEPHYR2_PANEL_WIDTH       320
#define ZEPHYR2_PANEL_HEIGHT      480
#define ZEPHYR2_SENSOR_WIDTH      4800
#define ZEPHYR2_SENSOR_HEIGHT     7200
#define ZEPHYR2_FRAME_TYPE        0xcc
#define ZEPHYR2_FRAME_HEADER      10
#define ZEPHYR2_FINGER_SIZE       32
#define ZEPHYR2_SURFACE_MARGIN    75
#define ZEPHYR2_TOUCH_RELEASED    5
#define ZEPHYR2_TOUCH_STARTED     3
#define ZEPHYR2_TOUCH_MOVED       4
#define ZEPHYR2_TOUCH_PRESSURE    160
#define ZEPHYR2_TOUCH_MAJOR       24
#define ZEPHYR2_TOUCH_MINOR       20

#define ZEPHYR2_REPORT_REGION_DESC  0xd0
#define ZEPHYR2_REPORT_FAMILY_ID    0xd1
#define ZEPHYR2_REPORT_SENSOR_INFO  0xd3
#define ZEPHYR2_REPORT_DIMENSIONS   0xd9
#define ZEPHYR2_REPORT_REGION_PARAM 0xa1

static const uint8_t zephyr2_region_desc[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x0f, 0x01, 0x00, 0x0a, 0x00,
};
static const uint8_t zephyr2_region_param[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t zephyr2_dimensions[] = {
    0xc0, 0x12, 0x00, 0x00, 0x20, 0x1c, 0x00, 0x00,
};
static const uint8_t zephyr2_sensor_info[] = {
    0x01, 0x0f, 0x0a, 0x01, 0x00,
};

static bool zephyr2_active(S5L8900Zephyr2State *s)
{
    return s->powered && s->reset_n;
}

static void zephyr2_clear_transfer(S5L8900Zephyr2State *s)
{
    s->command_len = 0;
    s->response_len = 0;
    s->response_pos = 0;
}

static void zephyr2_clear_frames(S5L8900Zephyr2State *s)
{
    memset(s->frames, 0, sizeof(s->frames));
    s->frame_head = 0;
    s->frame_count = 0;
    s->reported_down = false;
}

static void zephyr2_checksum(uint8_t *packet, size_t size)
{
    uint16_t checksum = 0;

    for (size_t i = 0; i < size - 2; i++) {
        checksum += packet[i];
    }
    packet[size - 2] = checksum;
    packet[size - 1] = checksum >> 8;
}

static void zephyr2_set_response(S5L8900Zephyr2State *s,
                                  const uint8_t *data, size_t size)
{
    g_assert(size <= sizeof(s->response));
    memcpy(s->response, data, size);
    s->response_len = size;
    s->response_pos = 0;
}

static bool zephyr2_report(uint8_t id, const uint8_t **data, size_t *size)
{
    static const uint8_t family_id[] = { ZEPHYR2_FAMILY_ID };

    switch (id) {
    case ZEPHYR2_REPORT_FAMILY_ID:
        *data = family_id;
        *size = sizeof(family_id);
        return true;
    case ZEPHYR2_REPORT_SENSOR_INFO:
        *data = zephyr2_sensor_info;
        *size = sizeof(zephyr2_sensor_info);
        return true;
    case ZEPHYR2_REPORT_REGION_DESC:
        *data = zephyr2_region_desc;
        *size = sizeof(zephyr2_region_desc);
        return true;
    case ZEPHYR2_REPORT_REGION_PARAM:
        *data = zephyr2_region_param;
        *size = sizeof(zephyr2_region_param);
        return true;
    case ZEPHYR2_REPORT_DIMENSIONS:
        *data = zephyr2_dimensions;
        *size = sizeof(zephyr2_dimensions);
        return true;
    default:
        return false;
    }
}

static void zephyr2_interface_response(S5L8900Zephyr2State *s)
{
    uint8_t response[16] = { 0 };

    if (!s->firmware_running) {
        return;
    }
    response[0] = 0xe2;
    zephyr2_checksum(response, sizeof(response));
    zephyr2_set_response(s, response, sizeof(response));
}

static void zephyr2_report_info_response(S5L8900Zephyr2State *s, uint8_t id)
{
    const uint8_t *data;
    size_t size;
    uint8_t response[16] = { 0 };
    bool found = s->firmware_running && zephyr2_report(id, &data, &size);

    response[0] = 0xe3;
    response[2] = !found;
    if (found) {
        response[3] = size;
        response[4] = size >> 8;
    }
    zephyr2_checksum(response, sizeof(response));
    zephyr2_set_response(s, response, sizeof(response));
}

static void zephyr2_report_response(S5L8900Zephyr2State *s, uint8_t opcode,
                                    uint8_t id, size_t total)
{
    const uint8_t *data;
    size_t size;
    uint8_t response[S5L8900_ZEPHYR2_RESPONSE_SIZE] = { 0 };

    total = MIN(total, sizeof(response));
    if (total < 5) {
        total = 5;
    }
    response[0] = opcode;
    if (s->firmware_running && zephyr2_report(id, &data, &size)) {
        size = MIN(size, total - 5);
        memcpy(response + 3, data, size);
    }
    zephyr2_checksum(response, total);
    zephyr2_set_response(s, response, total);
}

static void zephyr2_ack_response(S5L8900Zephyr2State *s)
{
    static const uint8_t data_ack[] = { 0x4b, 0xc1 };
    static const uint8_t write_ack[] = { 0x4a, 0xd1 };
    uint8_t read_ack[] = { 0x4b, 0xc1, 0, 0, 0, 0, 0, 0 };
    uint32_t value = s->hbpp_read_address == 0x10008ffc ?
                     0x5a030028 : 0;

    /* The 32-bit value at bytes 2..5 uses the controller's middle endian. */
    read_ack[2] = value >> 8;
    read_ack[3] = value;
    read_ack[4] = value >> 24;
    read_ack[5] = value >> 16;

    switch (s->pending_ack) {
    case ZEPHYR2_ACK_DATA:
        zephyr2_set_response(s, data_ack, sizeof(data_ack));
        break;
    case ZEPHYR2_ACK_WRITE:
        zephyr2_set_response(s, write_ack, sizeof(write_ack));
        break;
    case ZEPHYR2_ACK_READ:
        zephyr2_set_response(s, read_ack, sizeof(read_ack));
        break;
    default:
        break;
    }
}

static void zephyr2_queue_frame(S5L8900Zephyr2State *s, uint8_t event)
{
    uint8_t index;
    uint8_t *frame;
    uint8_t *finger;

    if (s->frame_count == S5L8900_ZEPHYR2_FRAME_QUEUE_SIZE) {
        index = (s->frame_head + s->frame_count - 1) %
                S5L8900_ZEPHYR2_FRAME_QUEUE_SIZE;
    } else {
        index = (s->frame_head + s->frame_count) %
                S5L8900_ZEPHYR2_FRAME_QUEUE_SIZE;
        s->frame_count++;
    }

    frame = s->frames[index];
    memset(frame, 0, S5L8900_ZEPHYR2_FRAME_SIZE);
    uint32_t sequence = ++s->next_frame;
    int32_t x_min = -ZEPHYR2_SURFACE_MARGIN;
    int32_t x_max = ZEPHYR2_SURFACE_MARGIN + 9 * 5600 / 11;
    int32_t y_min = -ZEPHYR2_SURFACE_MARGIN;
    int32_t y_max = ZEPHYR2_SURFACE_MARGIN + 14 * 3600 / 7;
    uint32_t pixel_x = (uint32_t)s->input_x *
                       (ZEPHYR2_PANEL_WIDTH - 1u) /
                       ZEPHYR2_SENSOR_WIDTH;
    uint32_t pixel_y = (uint32_t)(ZEPHYR2_SENSOR_HEIGHT - s->input_y) *
                       (ZEPHYR2_PANEL_HEIGHT - 1u) /
                       ZEPHYR2_SENSOR_HEIGHT;
    uint32_t x_step = (2u * pixel_x + 1u) *
                      (uint32_t)(x_max - x_min) /
                      (2u * ZEPHYR2_PANEL_WIDTH);
    uint32_t y_step = (2u * pixel_y + 1u) *
                      (uint32_t)(y_max - y_min) /
                      (2u * ZEPHYR2_PANEL_HEIGHT);
    int32_t surface_x = x_min + (int32_t)x_step;
    int32_t surface_y = y_max - (int32_t)y_step;

    frame[0] = ZEPHYR2_FRAME_TYPE;
    frame[1] = sequence;
    frame[3] = 1;
    stl_le_p(frame + 6, sequence * 16u);

    finger = frame + ZEPHYR2_FRAME_HEADER;
    finger[0] = 1;
    finger[1] = event;
    finger[2] = 1;
    finger[3] = 1;
    stl_le_p(finger + 4, (uint32_t)surface_x << 8);
    stl_le_p(finger + 8, (uint32_t)surface_y << 8);
    stw_le_p(finger + 20, event == ZEPHYR2_TOUCH_RELEASED ? 0 :
             ZEPHYR2_TOUCH_PRESSURE);
    stw_le_p(finger + 28,
             ZEPHYR2_TOUCH_MAJOR * (ZEPHYR2_SENSOR_WIDTH /
                                     ZEPHYR2_PANEL_WIDTH));
    stw_le_p(finger + 30,
             ZEPHYR2_TOUCH_MINOR * (ZEPHYR2_SENSOR_WIDTH /
                                     ZEPHYR2_PANEL_WIDTH));
    qemu_irq_pulse(s->atn);
}

static void zephyr2_pointer_event(DeviceState *dev, QemuConsole *src,
                                  QemuInputEvent *evt)
{
    S5L8900Zephyr2State *s = S5L8900_ZEPHYR2(dev);
    uint16_t value;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        if (evt->abs.axis == INPUT_AXIS_X) {
            value = qemu_input_scale_axis(evt->abs.value,
                                          INPUT_EVENT_ABS_MIN,
                                          INPUT_EVENT_ABS_MAX,
                                          0, ZEPHYR2_SENSOR_WIDTH);
            s->input_dirty |= s->input_x != value;
            s->input_x = value;
        } else if (evt->abs.axis == INPUT_AXIS_Y) {
            value = qemu_input_scale_axis(evt->abs.value,
                                          INPUT_EVENT_ABS_MIN,
                                          INPUT_EVENT_ABS_MAX,
                                          ZEPHYR2_SENSOR_HEIGHT, 0);
            s->input_dirty |= s->input_y != value;
            s->input_y = value;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            s->input_dirty |= s->input_down != evt->btn.down;
            s->input_down = evt->btn.down;
        }
        break;
    default:
        break;
    }
}

static void zephyr2_pointer_sync(DeviceState *dev)
{
    S5L8900Zephyr2State *s = S5L8900_ZEPHYR2(dev);
    uint8_t event;

    if (!s->input_dirty) {
        return;
    }
    s->input_dirty = false;
    if (!zephyr2_active(s) || !s->firmware_running) {
        s->reported_down = false;
        return;
    }
    if (s->input_down) {
        event = s->reported_down ? ZEPHYR2_TOUCH_MOVED :
                                   ZEPHYR2_TOUCH_STARTED;
    } else if (s->reported_down) {
        event = ZEPHYR2_TOUCH_RELEASED;
    } else {
        return;
    }
    zephyr2_queue_frame(s, event);
    s->reported_down = s->input_down;
}

static const QemuInputHandler zephyr2_pointer_handler = {
    .name = "iPhone 3G touchscreen",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = zephyr2_pointer_event,
    .sync = zephyr2_pointer_sync,
};

static void zephyr2_frame_length_response(S5L8900Zephyr2State *s,
                                           uint8_t command)
{
    uint8_t response[16] = { 0 };
    uint16_t packet_size = s->frame_count ?
                           S5L8900_ZEPHYR2_FRAME_PAYLOAD_SIZE + 2 : 0;

    response[0] = command;
    stw_le_p(response + 1, packet_size);
    zephyr2_checksum(response, sizeof(response));
    zephyr2_set_response(s, response, sizeof(response));
}

static void zephyr2_frame_response(S5L8900Zephyr2State *s,
                                    const uint8_t *command)
{
    uint8_t response[S5L8900_ZEPHYR2_FRAME_PAYLOAD_SIZE + 7] = { 0 };
    uint16_t checksum = 0;
    uint8_t *frame;

    if (!s->frame_count) {
        zephyr2_frame_length_response(s, command[0]);
        return;
    }

    frame = s->frames[s->frame_head];
    response[0] = command[0];
    response[1] = (S5L8900_ZEPHYR2_FRAME_PAYLOAD_SIZE + 2) & 0xff;
    stw_le_p(response + 2, S5L8900_ZEPHYR2_FRAME_PAYLOAD_SIZE + 2);
    response[4] = 0 - response[0] - response[1] - response[2] - response[3];
    memcpy(response + 5, frame, S5L8900_ZEPHYR2_FRAME_PAYLOAD_SIZE);
    for (size_t i = 0; i < S5L8900_ZEPHYR2_FRAME_PAYLOAD_SIZE; i++) {
        checksum += frame[i];
    }
    stw_le_p(response + 5 + S5L8900_ZEPHYR2_FRAME_PAYLOAD_SIZE, checksum);
    zephyr2_set_response(s, response, sizeof(response));
}

static void zephyr2_consume_frame(S5L8900Zephyr2State *s)
{
    if (!s->frame_count) {
        return;
    }

    s->frame_head = (s->frame_head + 1) %
                    S5L8900_ZEPHYR2_FRAME_QUEUE_SIZE;
    s->frame_count--;
    if (s->frame_count) {
        qemu_irq_pulse(s->atn);
    }
}

static size_t zephyr2_expected_command(S5L8900Zephyr2State *s)
{
    const uint8_t *command = s->command_prefix;

    if (s->command_len < 2) {
        return 0;
    }
    if (command[0] == 0x1a && command[1] == 0xa1) {
        return s->pending_ack == ZEPHYR2_ACK_READ ? 8 :
               s->pending_ack != ZEPHYR2_ACK_NONE ? 2 : 16;
    }
    if (command[0] == 0x1c && command[1] == 0x73) {
        return 8;
    }
    if (command[0] == 0x1e && command[1] == 0x33) {
        return 16;
    }
    if (command[0] == 0x1f && command[1] == 0x01) {
        return 2;
    }
    if (command[0] == 0x1d && command[1] == 0x53) {
        return 12;
    }
    switch (command[0]) {
    case 0xe1:
    case 0xe2:
    case 0xe3:
    case 0xe4:
    case 0xe5:
    case 0xe6:
    case 0xee:
        return 16;
    case 0xe7:
        if (s->command_len >= 5 && command[2] == 1) {
            return MIN((size_t)lduw_le_p(command + 3) + 5,
                       sizeof(s->response));
        }
        return 16;
    case 0xea:
    case 0xeb:
        if (s->command_len >= 3 && command[2] == 1 && s->frame_count) {
            return S5L8900_ZEPHYR2_FRAME_PAYLOAD_SIZE + 7;
        }
        return 16;
    default:
        return 0;
    }
}

static void zephyr2_process_command(S5L8900Zephyr2State *s)
{
    const uint8_t *command = s->command_prefix;

    trace_s5l8900_zephyr2_command(command[0], s->command_len,
                                  s->pending_ack, s->firmware_running);

    if (command[0] == 0x1a && command[1] == 0xa1) {
        zephyr2_ack_response(s);
    } else if (command[0] == 0x1c && command[1] == 0x73) {
        s->hbpp_read_address = (uint32_t)command[2] << 8 |
                               (uint32_t)command[3] |
                               (uint32_t)command[4] << 24 |
                               (uint32_t)command[5] << 16;
        s->pending_ack = ZEPHYR2_ACK_READ;
        qemu_irq_pulse(s->atn);
    } else if (command[0] == 0x1e && command[1] == 0x33) {
        s->pending_ack = ZEPHYR2_ACK_WRITE;
        qemu_irq_pulse(s->atn);
    } else if (command[0] == 0x1f && command[1] == 0x01) {
        s->pending_ack = ZEPHYR2_ACK_DATA;
        qemu_irq_pulse(s->atn);
    } else if (command[0] == 0x1d && command[1] == 0x53) {
        s->firmware_running = true;
    }
    s->command_len = 0;
}

static bool zephyr2_application_command(uint8_t command)
{
    switch (command) {
    case 0xe1:
    case 0xe2:
    case 0xe3:
    case 0xe4:
    case 0xe5:
    case 0xe6:
    case 0xe7:
    case 0xea:
    case 0xeb:
    case 0xee:
        return true;
    default:
        return false;
    }
}

static void zephyr2_application_response(S5L8900Zephyr2State *s,
                                          size_t position)
{
    const uint8_t *command = s->command_prefix;
    uint8_t response[16] = { 0 };

    if (position == 0) {
        response[0] = command[0];
        zephyr2_checksum(response, sizeof(response));
        zephyr2_set_response(s, response, sizeof(response));
    }

    switch (command[0]) {
    case 0xe2:
        if (position == 0) {
            zephyr2_interface_response(s);
        }
        break;
    case 0xe3:
        if (position == 1) {
            zephyr2_report_info_response(s, command[1]);
        }
        break;
    case 0xe6:
    case 0xe7:
        if (position == 1) {
            zephyr2_report_response(s, command[0], command[1], 16);
        } else if (command[0] == 0xe7 && position == 4 &&
                   command[2] == 1) {
            zephyr2_report_response(s, command[0], command[1],
                                    (size_t)lduw_le_p(command + 3) + 5);
        }
        break;
    case 0xea:
    case 0xeb:
        if (position == 1) {
            zephyr2_frame_length_response(s, command[0]);
        } else if (position == 2 && command[2] == 1) {
            zephyr2_frame_response(s, command);
        }
        break;
    default:
        break;
    }

    if (position < s->response_len) {
        s->response_pos = position;
    }
}

static uint32_t zephyr2_transfer(SSIPeripheral *peripheral, uint32_t value)
{
    S5L8900Zephyr2State *s = S5L8900_ZEPHYR2(peripheral);
    size_t expected;
    uint8_t response = 0;
    bool hbpp;
    bool application;
    size_t position;

    if (!zephyr2_active(s)) {
        return 0;
    }

    /* A boot image DATA packet may continue across a chip-select edge. */
    if (s->hbpp_data_active) {
        if (s->hbpp_data_received < s->hbpp_data_expected) {
            s->hbpp_data_received++;
        }
        if (s->hbpp_data_received == s->hbpp_data_expected) {
            trace_s5l8900_zephyr2_data("complete", s->hbpp_data_expected,
                                       s->hbpp_data_received);
            s->hbpp_data_active = false;
            s->pending_ack = ZEPHYR2_ACK_DATA;
            qemu_irq_pulse(s->atn);
        }
        return 0;
    }

    /*
     * The bootloader attention word is the exception to the Z2 driver's
     * usual TX-then-RX phasing.  The initial sixteen-byte HBPP probe is a
     * physical loopback, while the two- and eight-byte forms return their
     * acknowledgement during the very clocks carrying 1a a1.  In particular,
     * queuing the answer after byte two is too late: AppleMultitouchZ2SPI has
     * already captured two zero bytes and rejects the device.
     *
     * The identical 1a a1 prefix is disambiguated only by the preceding
     * command.  DATA, CALIB and WRREG request a two-byte acknowledgement,
     * RDREG requests eight bytes, and no pending command means the probe.
     */
    position = s->command_len;
    if (s->command_len < sizeof(s->command_prefix)) {
        s->command_prefix[s->command_len] = value;
    }
    if (s->command_len == UINT32_MAX) {
        return 0;
    }
    s->command_len++;
    hbpp = s->command_prefix[0] == 0x1a &&
           (s->command_len == 1 || s->command_prefix[1] == 0xa1);

    if (hbpp) {
        if (s->command_len == 1 &&
            s->pending_ack != ZEPHYR2_ACK_NONE) {
            zephyr2_ack_response(s);
        }
        if (s->pending_ack != ZEPHYR2_ACK_NONE &&
            s->response_pos < s->response_len) {
            response = s->response[s->response_pos++];
        } else {
            /* RESET_N is followed by the same physical HBPP loopback probe. */
            response = value;
        }

        expected = zephyr2_expected_command(s);
        if (expected && s->command_len == expected) {
            trace_s5l8900_zephyr2_hbpp(expected, s->pending_ack,
                                       response, s->firmware_running);
            s->pending_ack = ZEPHYR2_ACK_NONE;
            zephyr2_clear_transfer(s);
        }
        return response;
    }

    /*
     * 18 e1 is the two-byte bootloader idle/attention word.  It is looped
     * back while HBPP is active, just like the probe.  When followed by
     * 30 01 it is also the prefix of a DATA upload.  The big-endian word
     * count at DATA bytes 2..3 defines a 14 + 4W byte packet, and the Apple
     * driver splits the header from the body with a chip-select edge.
     */
    if (!s->firmware_running && s->command_prefix[0] == 0x18 &&
        (s->command_len == 1 || s->command_prefix[1] == 0xe1)) {
        response = s->command_len <= 2 ? value : 0;
        if (s->command_len == 6 && s->command_prefix[2] == 0x30 &&
            s->command_prefix[3] == 0x01) {
            uint32_t words = (uint32_t)s->command_prefix[4] << 8 |
                             s->command_prefix[5];

            s->hbpp_data_expected = 14 + 4 * words;
            s->hbpp_data_received = 4;
            s->hbpp_data_active = true;
            trace_s5l8900_zephyr2_data("begin", s->hbpp_data_expected,
                                       s->hbpp_data_received);
        }
        return response;
    }

    application = s->firmware_running &&
                  zephyr2_application_command(s->command_prefix[0]);
    if (application) {
        zephyr2_application_response(s, position);
        if (s->response_pos < s->response_len) {
            response = s->response[s->response_pos++];
        }
        expected = zephyr2_expected_command(s);
        if (expected && s->command_len == expected) {
            trace_s5l8900_zephyr2_command(s->command_prefix[0],
                                          s->command_len, s->pending_ack,
                                          s->firmware_running);
            if ((s->command_prefix[0] == 0xea ||
                 s->command_prefix[0] == 0xeb) &&
                s->command_prefix[2] == 1) {
                zephyr2_consume_frame(s);
            }
            zephyr2_clear_transfer(s);
        }
        return response;
    }

    if (s->response_pos < s->response_len) {
        return s->response[s->response_pos++];
    }
    expected = zephyr2_expected_command(s);
    if (expected && s->command_len == expected) {
        zephyr2_process_command(s);
    }
    return 0;
}

static int zephyr2_set_cs(SSIPeripheral *peripheral, bool deasserted)
{
    S5L8900Zephyr2State *s = S5L8900_ZEPHYR2(peripheral);

    if (deasserted) {
        if (!s->firmware_running || s->hbpp_data_active ||
            s->pending_ack != ZEPHYR2_ACK_NONE) {
            trace_s5l8900_zephyr2_select(s->command_len > 0 ?
                                         s->command_prefix[0] : 0,
                                         s->command_len > 1 ?
                                         s->command_prefix[1] : 0,
                                         s->command_len,
                                         s->hbpp_data_active,
                                         s->hbpp_data_received,
                                         s->hbpp_data_expected);
        }
        zephyr2_clear_transfer(s);
    }
    return 0;
}

static void zephyr2_set_power(void *opaque, int line, int level)
{
    S5L8900Zephyr2State *s = opaque;

    trace_s5l8900_zephyr2_power(level, s->firmware_running);
    if (!level) {
        s->firmware_running = false;
        s->pending_ack = ZEPHYR2_ACK_NONE;
        s->hbpp_data_active = false;
        s->hbpp_data_expected = 0;
        s->hbpp_data_received = 0;
        zephyr2_clear_frames(s);
        zephyr2_clear_transfer(s);
    }
    s->powered = level;
}

static void zephyr2_set_reset(void *opaque, int line, int level)
{
    S5L8900Zephyr2State *s = opaque;

    trace_s5l8900_zephyr2_reset(level, s->firmware_running);
    if (!level) {
        /*
         * RESET_N restarts the controller but does not remove power from its
         * SRAM.  The Apple driver pulses this line again when leaving UI
         * lock; treating that pulse like an LDO power loss discards the
         * downloaded application image and leaves the driver talking to a
         * bootloader that it does not intend to reprogram.  Only the power
         * input above erases the flashless part's volatile firmware.
         */
        s->pending_ack = ZEPHYR2_ACK_NONE;
        s->hbpp_data_active = false;
        s->hbpp_data_expected = 0;
        s->hbpp_data_received = 0;
        zephyr2_clear_frames(s);
        zephyr2_clear_transfer(s);
    }
    s->reset_n = level;
}

static void zephyr2_reset(DeviceState *dev)
{
    S5L8900Zephyr2State *s = S5L8900_ZEPHYR2(dev);

    s->powered = false;
    s->reset_n = false;
    s->firmware_running = false;
    s->pending_ack = ZEPHYR2_ACK_NONE;
    s->hbpp_read_address = 0;
    s->hbpp_data_active = false;
    s->hbpp_data_expected = 0;
    s->hbpp_data_received = 0;
    s->input_x = 0;
    s->input_y = ZEPHYR2_SENSOR_HEIGHT;
    s->input_down = false;
    s->input_dirty = false;
    s->next_frame = 0;
    memset(s->command_prefix, 0, sizeof(s->command_prefix));
    memset(s->response, 0, sizeof(s->response));
    zephyr2_clear_frames(s);
    zephyr2_clear_transfer(s);
    qemu_set_irq(s->atn, 0);
}

static int zephyr2_post_load(void *opaque, int version_id)
{
    S5L8900Zephyr2State *s = opaque;

    if (s->pending_ack > ZEPHYR2_ACK_READ ||
        s->hbpp_data_received > s->hbpp_data_expected ||
        s->response_len > sizeof(s->response) ||
        s->response_pos > s->response_len ||
        s->input_x > ZEPHYR2_SENSOR_WIDTH ||
        s->input_y > ZEPHYR2_SENSOR_HEIGHT ||
        s->frame_head >= S5L8900_ZEPHYR2_FRAME_QUEUE_SIZE ||
        s->frame_count > S5L8900_ZEPHYR2_FRAME_QUEUE_SIZE) {
        return -EINVAL;
    }
    qemu_set_irq(s->atn, 0);
    return 0;
}

static const VMStateDescription vmstate_zephyr2 = {
    .name = TYPE_S5L8900_ZEPHYR2,
    .version_id = 4,
    .minimum_version_id = 1,
    .post_load = zephyr2_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, S5L8900Zephyr2State),
        VMSTATE_BOOL(powered, S5L8900Zephyr2State),
        VMSTATE_BOOL(reset_n, S5L8900Zephyr2State),
        VMSTATE_BOOL(firmware_running, S5L8900Zephyr2State),
        VMSTATE_UINT8(pending_ack, S5L8900Zephyr2State),
        VMSTATE_UINT32_V(hbpp_read_address, S5L8900Zephyr2State, 3),
        VMSTATE_BOOL_V(hbpp_data_active, S5L8900Zephyr2State, 4),
        VMSTATE_UINT32_V(hbpp_data_expected, S5L8900Zephyr2State, 4),
        VMSTATE_UINT32_V(hbpp_data_received, S5L8900Zephyr2State, 4),
        VMSTATE_UINT8_ARRAY(command_prefix, S5L8900Zephyr2State,
                            S5L8900_ZEPHYR2_COMMAND_PREFIX_SIZE),
        VMSTATE_UINT32(command_len, S5L8900Zephyr2State),
        VMSTATE_UINT8_ARRAY(response, S5L8900Zephyr2State,
                            S5L8900_ZEPHYR2_RESPONSE_SIZE),
        VMSTATE_UINT16(response_len, S5L8900Zephyr2State),
        VMSTATE_UINT16(response_pos, S5L8900Zephyr2State),
        VMSTATE_UINT16_V(input_x, S5L8900Zephyr2State, 2),
        VMSTATE_UINT16_V(input_y, S5L8900Zephyr2State, 2),
        VMSTATE_BOOL_V(input_down, S5L8900Zephyr2State, 2),
        VMSTATE_BOOL_V(reported_down, S5L8900Zephyr2State, 2),
        VMSTATE_BOOL_V(input_dirty, S5L8900Zephyr2State, 2),
        VMSTATE_UINT32_V(next_frame, S5L8900Zephyr2State, 2),
        VMSTATE_UINT8_2DARRAY_V(frames, S5L8900Zephyr2State,
                               S5L8900_ZEPHYR2_FRAME_QUEUE_SIZE,
                               S5L8900_ZEPHYR2_FRAME_SIZE, 2),
        VMSTATE_UINT8_V(frame_head, S5L8900Zephyr2State, 2),
        VMSTATE_UINT8_V(frame_count, S5L8900Zephyr2State, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void zephyr2_init(Object *obj)
{
    S5L8900Zephyr2State *s = S5L8900_ZEPHYR2(obj);
    DeviceState *dev = DEVICE(obj);

    qdev_init_gpio_in_named(dev, zephyr2_set_power, "power", 1);
    qdev_init_gpio_in_named(dev, zephyr2_set_reset, "reset", 1);
    qdev_init_gpio_out_named(dev, &s->atn, "atn", 1);
}

static void zephyr2_realize(SSIPeripheral *peripheral, Error **errp)
{
    S5L8900Zephyr2State *s = S5L8900_ZEPHYR2(peripheral);

    s->input_handler = qemu_input_handler_register(DEVICE(peripheral),
                                                    &zephyr2_pointer_handler);
}

static void zephyr2_unrealize(DeviceState *dev)
{
    S5L8900Zephyr2State *s = S5L8900_ZEPHYR2(dev);

    g_clear_pointer(&s->input_handler, qemu_input_handler_unregister);
}

static void zephyr2_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SSIPeripheralClass *ssc = SSI_PERIPHERAL_CLASS(oc);

    dc->desc = "Apple N82 Zephyr2 multitouch controller";
    dc->vmsd = &vmstate_zephyr2;
    dc->unrealize = zephyr2_unrealize;
    device_class_set_legacy_reset(dc, zephyr2_reset);
    ssc->realize = zephyr2_realize;
    ssc->transfer = zephyr2_transfer;
    ssc->set_cs = zephyr2_set_cs;
    ssc->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo zephyr2_info = {
    .name = TYPE_S5L8900_ZEPHYR2,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(S5L8900Zephyr2State),
    .instance_init = zephyr2_init,
    .class_init = zephyr2_class_init,
};

static void zephyr2_register_types(void)
{
    type_register_static(&zephyr2_info);
}

type_init(zephyr2_register_types)
