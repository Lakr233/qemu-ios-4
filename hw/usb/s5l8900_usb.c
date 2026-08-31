/*
 * Apple S5L8900 Synopsys USB device controller and PHY
 *
 * This models the device-mode register contract consumed by the S5L8900
 * OpeniBoot platform.  USB packets are deliberately kept outside this file's
 * register core until the USB-over-IP transport owns a complete transfer.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/usb/dwc2-regs.h"
#include "hw/usb/s5l8900_usb.h"
#include "hw/usb/usb.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "trace.h"

#define S5L8900_USB_MMIO_SIZE       0x1000
#define S5L8900_USB_PHY_MMIO_SIZE   0x1000
#define S5L8900_USB_ENDPOINTS       16
#define S5L8900_USB_TX_FIFOS        15

#define S5L8900_GHWCFG1             0x00000000
#define S5L8900_GHWCFG2             0x7a8f60d0
#define S5L8900_GHWCFG3             0x082000e8
#define S5L8900_GHWCFG4             0x01f08024

#define GINTSTS_GINNAKEFF           BIT(6)
#define GINTSTS_GOUTNAKEFF          BIT(7)
#define GINTSTS_USBRESET            BIT(12)
#define GINTSTS_ENUMDONE            BIT(13)
#define GINTSTS_IEPINT              BIT(18)
#define GINTSTS_OEPINT              BIT(19)

#define GAHBCFG_GLBL_INTR_EN        BIT(0)

#define EP_CONTROL                  0x00
#define EP_INTERRUPT                0x08
#define EP_TRANSFER_SIZE            0x10
#define EP_DMA_ADDRESS              0x14
#define EP_TX_FIFO_STATUS           0x18
#define EP_REGISTER_STRIDE          0x20
#define EP_IN_BASE                  0x900
#define EP_OUT_BASE                 0xb00
#define DEVICE_TX_FIFO_BASE         0x104
#define DEVICE_TX_FIFO_LAST         0x13c

#define PHY_POWER                   0x00
#define PHY_CLOCK                   0x04
#define PHY_RESET_CONTROL           0x08
#define PHY_REGISTER_COUNT          3

#define IOSU_HEADER_SIZE            16
#define IOSU_MAX_PAYLOAD            (16 * MiB)
#define IOSU_VERSION                1
#define IOSU_RESPONSE               BIT(0)
#define IOSU_ERROR                  BIT(1)

#define IOSU_ENUMERATE              1
#define IOSU_CONTROL                2
#define IOSU_BULK_OUT               3
#define IOSU_BULK_IN                4
#define IOSU_SET_CONFIGURATION      5
#define IOSU_SET_INTERFACE          6
#define IOSU_RESET                  7

#define IOSU_CONTROL_REQUEST_SIZE   14
#define IOSU_BULK_REQUEST_SIZE      9
#define IOSU_ENDPOINT_IN            0x80

#define APPLE_VENDOR_ID             0x05ac
#define APPLE_RECOVERY_PRODUCT_ID   0x1281

#define EP_CONTROL_COMMANDS         (DXEPCTL_EPDIS | DXEPCTL_SETD1PID | \
                                     DXEPCTL_SETD0PID | DXEPCTL_SNAK | \
                                     DXEPCTL_CNAK)

typedef struct S5L8900USBEndpoint {
    uint32_t control;
    uint32_t interrupt;
    uint32_t transfer_size;
    uint32_t dma_address;
} S5L8900USBEndpoint;

typedef enum IOSUTransferKind {
    IOSU_TRANSFER_NONE,
    IOSU_TRANSFER_CONTROL_IN,
    IOSU_TRANSFER_CONTROL_OUT,
    IOSU_TRANSFER_BULK_IN,
    IOSU_TRANSFER_BULK_OUT,
} IOSUTransferKind;

typedef enum IOSUTransferPhase {
    IOSU_PHASE_NONE,
    IOSU_PHASE_SETUP,
    IOSU_PHASE_DATA,
    IOSU_PHASE_STATUS,
} IOSUTransferPhase;

struct S5L8900USBState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion phy_iomem;
    qemu_irq irq;
    qemu_irq cable_present;
    CharFrontend chr;

    uint32_t gotgctl;
    uint32_t gotgint;
    uint32_t gahbcfg;
    uint32_t gusbcfg;
    uint32_t gintsts;
    uint32_t gintmsk;
    uint32_t grxfsiz;
    uint32_t gnptxfsiz;
    uint32_t dieptxf[S5L8900_USB_TX_FIFOS];
    uint32_t dcfg;
    uint32_t dctl;
    uint32_t diepmsk;
    uint32_t doepmsk;
    uint32_t daintmsk;
    uint32_t pcgcctl;
    S5L8900USBEndpoint in_ep[S5L8900_USB_ENDPOINTS];
    S5L8900USBEndpoint out_ep[S5L8900_USB_ENDPOINTS];
    uint32_t phy[PHY_REGISTER_COUNT];

    uint8_t iosu_header[IOSU_HEADER_SIZE];
    uint8_t *iosu_payload;
    uint32_t iosu_header_used;
    uint32_t iosu_payload_size;
    uint32_t iosu_payload_used;
    uint32_t iosu_request_id;
    uint8_t iosu_opcode;
    uint8_t host_configuration;
    uint8_t host_interface;
    uint8_t host_altsetting;
    bool host_enumerated;

    QEMUTimer *transfer_timer;
    uint8_t *transfer_data;
    uint32_t transfer_request_id;
    uint32_t transfer_timeout;
    uint32_t transfer_requested;
    uint32_t transfer_completed;
    uint8_t transfer_opcode;
    uint8_t transfer_endpoint;
    uint8_t transfer_kind;
    uint8_t transfer_phase;
    uint8_t setup_packet[8];
};

static void s5l8900_usb_update_irq(S5L8900USBState *s);
static void s5l8900_usb_service_transfer(S5L8900USBState *s);

static const char s5l8900_recovery_serial[] =
    "CPID:8900 BDID:04 ECID:1 IBFL:0 SRNM:[N82AP] "
    "SRTG:[iBoot-931.71.16]";

static void s5l8900_usb_iosu_reset_parser(S5L8900USBState *s)
{
    g_clear_pointer(&s->iosu_payload, g_free);
    s->iosu_header_used = 0;
    s->iosu_payload_size = 0;
    s->iosu_payload_used = 0;
    s->iosu_request_id = 0;
    s->iosu_opcode = 0;
}

static void s5l8900_usb_iosu_send(S5L8900USBState *s, uint8_t opcode,
                                  uint32_t request_id, uint16_t flags,
                                  const uint8_t *payload, uint32_t size)
{
    uint8_t header[IOSU_HEADER_SIZE] = { 'I', 'O', 'S', 'U' };

    header[4] = IOSU_VERSION;
    header[5] = opcode;
    stw_be_p(header + 6, IOSU_RESPONSE | flags);
    stl_be_p(header + 8, request_id);
    stl_be_p(header + 12, size);
    if (qemu_chr_fe_write_all(&s->chr, header, sizeof(header)) < 0) {
        return;
    }
    if (size) {
        qemu_chr_fe_write_all(&s->chr, payload, size);
    }
}

static void s5l8900_usb_iosu_error(S5L8900USBState *s, const char *message)
{
    s5l8900_usb_iosu_send(s, s->iosu_opcode, s->iosu_request_id,
                          IOSU_ERROR, (const uint8_t *)message,
                          strlen(message));
}

static void s5l8900_usb_clear_transfer(S5L8900USBState *s)
{
    if (s->transfer_timer) {
        timer_del(s->transfer_timer);
    }
    g_clear_pointer(&s->transfer_data, g_free);
    s->transfer_request_id = 0;
    s->transfer_timeout = 0;
    s->transfer_requested = 0;
    s->transfer_completed = 0;
    s->transfer_opcode = 0;
    s->transfer_endpoint = 0;
    s->transfer_kind = IOSU_TRANSFER_NONE;
    s->transfer_phase = IOSU_PHASE_NONE;
    memset(s->setup_packet, 0, sizeof(s->setup_packet));
}

static void s5l8900_usb_fail_transfer(S5L8900USBState *s,
                                      const char *message)
{
    s5l8900_usb_iosu_send(s, s->transfer_opcode, s->transfer_request_id,
                          IOSU_ERROR, (const uint8_t *)message,
                          strlen(message));
    s5l8900_usb_clear_transfer(s);
}

static void s5l8900_usb_finish_transfer(S5L8900USBState *s)
{
    uint8_t result[4];

    trace_s5l8900_usb_transfer_finish(s->transfer_request_id,
                                      s->transfer_kind,
                                      s->transfer_completed);
    if (s->transfer_opcode == IOSU_SET_CONFIGURATION) {
        s->host_configuration = lduw_le_p(s->setup_packet + 2);
        s5l8900_usb_iosu_send(s, s->transfer_opcode,
                              s->transfer_request_id, 0, NULL, 0);
    } else if (s->transfer_opcode == IOSU_SET_INTERFACE) {
        s->host_interface = lduw_le_p(s->setup_packet + 4);
        s->host_altsetting = lduw_le_p(s->setup_packet + 2);
        s5l8900_usb_iosu_send(s, s->transfer_opcode,
                              s->transfer_request_id, 0, NULL, 0);
    } else if (s->transfer_kind == IOSU_TRANSFER_CONTROL_IN ||
        s->transfer_kind == IOSU_TRANSFER_BULK_IN) {
        s5l8900_usb_iosu_send(s, s->transfer_opcode,
                              s->transfer_request_id, 0,
                              s->transfer_data, s->transfer_completed);
    } else {
        stl_be_p(result, s->transfer_completed);
        s5l8900_usb_iosu_send(s, s->transfer_opcode,
                              s->transfer_request_id, 0,
                              result, sizeof(result));
    }
    s5l8900_usb_clear_transfer(s);
}

static void s5l8900_usb_transfer_timeout(void *opaque)
{
    S5L8900USBState *s = opaque;

    if (s->transfer_kind != IOSU_TRANSFER_NONE) {
        s5l8900_usb_fail_transfer(s, "USB transfer timed out");
    }
}

static void s5l8900_usb_begin_transfer(S5L8900USBState *s,
                                       IOSUTransferKind kind,
                                       uint8_t endpoint, uint32_t timeout,
                                       uint32_t length, const uint8_t *data)
{
    g_assert(s->transfer_kind == IOSU_TRANSFER_NONE);

    s->transfer_request_id = s->iosu_request_id;
    s->transfer_opcode = s->iosu_opcode;
    s->transfer_timeout = timeout;
    s->transfer_requested = length;
    s->transfer_completed = 0;
    s->transfer_endpoint = endpoint;
    s->transfer_kind = kind;
    s->transfer_phase = (kind == IOSU_TRANSFER_CONTROL_IN ||
                         kind == IOSU_TRANSFER_CONTROL_OUT) ?
                        IOSU_PHASE_SETUP : IOSU_PHASE_DATA;
    trace_s5l8900_usb_transfer_begin(s->transfer_request_id, kind,
                                     endpoint, length);
    if (length) {
        s->transfer_data = data ? g_memdup2(data, length) : g_malloc(length);
    }
    timer_mod(s->transfer_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + timeout);
}

static bool s5l8900_usb_dma_write(S5L8900USBState *s, uint32_t address,
                                  const uint8_t *data, uint32_t size)
{
    if ((size && address > UINT32_MAX - size + 1) ||
        dma_memory_write(&address_space_memory, address, data, size,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        s5l8900_usb_fail_transfer(s, "USB DMA write failed");
        return false;
    }
    return true;
}

static bool s5l8900_usb_dma_read(S5L8900USBState *s, uint32_t address,
                                 uint8_t *data, uint32_t size)
{
    if ((size && address > UINT32_MAX - size + 1) ||
        dma_memory_read(&address_space_memory, address, data, size,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        s5l8900_usb_fail_transfer(s, "USB DMA read failed");
        return false;
    }
    return true;
}

static uint32_t s5l8900_usb_endpoint_max_packet(
    S5L8900USBEndpoint *endpoint, unsigned index)
{
    uint32_t encoded = endpoint->control & DXEPCTL_MPS_MASK;

    if (index == 0) {
        static const uint8_t ep0_size[] = { 64, 32, 16, 8 };

        return ep0_size[encoded & 3];
    }
    return encoded ? encoded : 64;
}

static void s5l8900_usb_complete_endpoint(S5L8900USBState *s,
                                          S5L8900USBEndpoint *endpoint,
                                          unsigned index, bool setup,
                                          uint32_t transferred,
                                          uint32_t interrupt)
{
    uint32_t capacity = endpoint->transfer_size & DXEPTSIZ_XFERSIZE_MASK;
    uint32_t packet_count =
        DXEPTSIZ_PKTCNT_GET(endpoint->transfer_size);
    uint32_t packets = setup || !transferred ? 1 :
        DIV_ROUND_UP(transferred,
                     s5l8900_usb_endpoint_max_packet(endpoint, index));
    uint32_t setup_count =
        (endpoint->transfer_size & DOEPTSIZ0_SUPCNT_MASK) >>
        DOEPTSIZ0_SUPCNT_SHIFT;

    g_assert(transferred <= capacity);
    packet_count -= MIN(packet_count, packets);
    if (setup_count && setup) {
        setup_count--;
    }
    endpoint->transfer_size &= ~(DXEPTSIZ_XFERSIZE_MASK |
                                 DXEPTSIZ_PKTCNT_MASK |
                                 DOEPTSIZ0_SUPCNT_MASK);
    endpoint->transfer_size |= capacity - transferred;
    endpoint->transfer_size |= DXEPTSIZ_PKTCNT(packet_count);
    endpoint->transfer_size |= DOEPTSIZ0_SUPCNT(setup_count);
    endpoint->control &= ~DXEPCTL_EPENA;
    endpoint->interrupt |= interrupt;
    s5l8900_usb_update_irq(s);
}

static void s5l8900_usb_advance_in_endpoint(S5L8900USBEndpoint *endpoint,
                                             unsigned index,
                                             uint32_t transferred)
{
    uint32_t capacity = endpoint->transfer_size & DXEPTSIZ_XFERSIZE_MASK;
    uint32_t packet_count =
        DXEPTSIZ_PKTCNT_GET(endpoint->transfer_size);
    uint32_t max_packet =
        s5l8900_usb_endpoint_max_packet(endpoint, index);
    uint32_t packets = transferred / max_packet;

    g_assert(transferred && transferred < capacity);
    g_assert(!(transferred % max_packet));
    g_assert(endpoint->dma_address <= UINT32_MAX - transferred);
    packet_count -= MIN(packet_count, packets);
    endpoint->transfer_size &= ~(DXEPTSIZ_XFERSIZE_MASK |
                                 DXEPTSIZ_PKTCNT_MASK);
    endpoint->transfer_size |= capacity - transferred;
    endpoint->transfer_size |= DXEPTSIZ_PKTCNT(packet_count);
    endpoint->dma_address += transferred;
}

static void s5l8900_usb_service_transfer(S5L8900USBState *s)
{
    S5L8900USBEndpoint *endpoint;
    uint32_t capacity;
    uint32_t remaining;
    uint32_t count;

    if (s->transfer_kind == IOSU_TRANSFER_NONE) {
        return;
    }

    trace_s5l8900_usb_transfer_service(s->transfer_request_id,
                                       s->transfer_kind,
                                       s->transfer_phase,
                                       s->transfer_completed);

    if (s->transfer_phase == IOSU_PHASE_SETUP) {
        endpoint = &s->out_ep[0];
        capacity = endpoint->transfer_size & DXEPTSIZ_XFERSIZE_MASK;
        if (!(endpoint->control & DXEPCTL_EPENA) || capacity < 8) {
            return;
        }
        if (!s5l8900_usb_dma_write(s, endpoint->dma_address,
                                   s->setup_packet,
                                   sizeof(s->setup_packet))) {
            return;
        }
        s5l8900_usb_complete_endpoint(s, endpoint, 0, true, 8,
                                      DXEPINT_SETUP);
        if (s->transfer_kind == IOSU_TRANSFER_CONTROL_OUT &&
            !s->transfer_requested) {
            s->transfer_phase = IOSU_PHASE_STATUS;
        } else {
            s->transfer_phase = IOSU_PHASE_DATA;
        }
        return;
    }

    remaining = s->transfer_requested - s->transfer_completed;
    switch (s->transfer_kind) {
    case IOSU_TRANSFER_CONTROL_OUT:
        if (s->transfer_phase == IOSU_PHASE_STATUS) {
            endpoint = &s->in_ep[0];
            capacity = endpoint->transfer_size & DXEPTSIZ_XFERSIZE_MASK;
            if (!(endpoint->control & DXEPCTL_EPENA) || capacity) {
                return;
            }
            s5l8900_usb_complete_endpoint(s, endpoint, 0, false, 0,
                                          DXEPINT_XFERCOMPL);
            s5l8900_usb_finish_transfer(s);
            return;
        }
        endpoint = &s->out_ep[0];
        break;
    case IOSU_TRANSFER_CONTROL_IN:
        endpoint = &s->in_ep[0];
        break;
    case IOSU_TRANSFER_BULK_OUT:
        endpoint = &s->out_ep[s->transfer_endpoint];
        break;
    case IOSU_TRANSFER_BULK_IN:
        endpoint = &s->in_ep[s->transfer_endpoint];
        break;
    case IOSU_TRANSFER_NONE:
    default:
        return;
    }

    capacity = endpoint->transfer_size & DXEPTSIZ_XFERSIZE_MASK;
    if (!(endpoint->control & DXEPCTL_EPENA) ||
        (!capacity && s->transfer_kind != IOSU_TRANSFER_BULK_IN)) {
        return;
    }
    count = MIN(capacity, remaining);

    if (s->transfer_kind == IOSU_TRANSFER_CONTROL_OUT ||
        s->transfer_kind == IOSU_TRANSFER_BULK_OUT) {
        if (!s5l8900_usb_dma_write(s, endpoint->dma_address,
                                   s->transfer_data + s->transfer_completed,
                                   count)) {
            return;
        }
    } else if (!s5l8900_usb_dma_read(
                   s, endpoint->dma_address,
                   s->transfer_data + s->transfer_completed, count)) {
        return;
    }

    s->transfer_completed += count;

    if (s->transfer_kind == IOSU_TRANSFER_CONTROL_OUT) {
        s5l8900_usb_complete_endpoint(s, endpoint, s->transfer_endpoint,
                                      false, count, DXEPINT_XFERCOMPL);
        if (s->transfer_completed == s->transfer_requested) {
            s->transfer_phase = IOSU_PHASE_STATUS;
        }
        return;
    }
    if (s->transfer_kind == IOSU_TRANSFER_CONTROL_IN) {
        uint32_t max_packet = s5l8900_usb_endpoint_max_packet(endpoint, 0);

        s5l8900_usb_complete_endpoint(s, endpoint, s->transfer_endpoint,
                                      false, count, DXEPINT_XFERCOMPL);
        if (s->transfer_completed == s->transfer_requested ||
            count < max_packet) {
            s->out_ep[0].interrupt |= DXEPINT_XFERCOMPL;
            s5l8900_usb_update_irq(s);
            s5l8900_usb_finish_transfer(s);
        }
        return;
    }
    if (s->transfer_kind == IOSU_TRANSFER_BULK_IN) {
        uint32_t max_packet = s5l8900_usb_endpoint_max_packet(
            endpoint, s->transfer_endpoint);

        if (count && count < capacity && count == remaining &&
            !(count % max_packet)) {
            s5l8900_usb_advance_in_endpoint(endpoint,
                                             s->transfer_endpoint, count);
            s5l8900_usb_finish_transfer(s);
            return;
        }
        s5l8900_usb_complete_endpoint(s, endpoint, s->transfer_endpoint,
                                      false, count, DXEPINT_XFERCOMPL);
        if (s->transfer_completed == s->transfer_requested ||
            count < max_packet || count % max_packet) {
            s5l8900_usb_finish_transfer(s);
        }
        return;
    }
    s5l8900_usb_complete_endpoint(s, endpoint, s->transfer_endpoint, false,
                                  count, DXEPINT_XFERCOMPL);
    if (s->transfer_completed == s->transfer_requested) {
        s5l8900_usb_finish_transfer(s);
    }
}

static void s5l8900_usb_iosu_bus_reset(S5L8900USBState *s)
{
    /*
     * A bus reset retires endpoint completion latches from the old USB
     * address/configuration.  Preserve the Guest-programmed endpoint control,
     * transfer-size and DMA registers: iBEC and XNU use those to arm EP0 for
     * the next setup packet without a controller cold reset.
     */
    for (unsigned i = 0; i < S5L8900_USB_ENDPOINTS; i++) {
        s->in_ep[i].interrupt = 0;
        s->out_ep[i].interrupt = 0;
    }
    s->dcfg &= ~DCFG_DEVADDR_MASK;
    s->host_configuration = 0;
    s->host_interface = 0;
    s->host_altsetting = 0;
    s->gintsts |= GINTSTS_USBRESET | GINTSTS_ENUMDONE;
    s5l8900_usb_update_irq(s);
}

static void s5l8900_usb_iosu_enumerate(S5L8900USBState *s)
{
    size_t serial_size = strlen(s5l8900_recovery_serial);
    g_autofree uint8_t *descriptor = g_malloc(6 + serial_size);

    stw_be_p(descriptor, APPLE_VENDOR_ID);
    stw_be_p(descriptor + 2, APPLE_RECOVERY_PRODUCT_ID);
    stw_be_p(descriptor + 4, serial_size);
    memcpy(descriptor + 6, s5l8900_recovery_serial, serial_size);
    if (!s->host_enumerated) {
        s5l8900_usb_iosu_bus_reset(s);
        s->host_enumerated = true;
    }
    s5l8900_usb_iosu_send(s, s->iosu_opcode, s->iosu_request_id, 0,
                          descriptor, 6 + serial_size);
}

static bool s5l8900_usb_iosu_control(S5L8900USBState *s)
{
    uint8_t request_type;
    uint32_t timeout;
    uint32_t length;
    uint32_t expected_size;
    const uint8_t *data = NULL;
    IOSUTransferKind kind;

    if (s->iosu_payload_size < IOSU_CONTROL_REQUEST_SIZE) {
        s5l8900_usb_iosu_error(s, "USB control request is truncated");
        return false;
    }
    request_type = s->iosu_payload[0];
    timeout = ldl_be_p(s->iosu_payload + 6);
    length = ldl_be_p(s->iosu_payload + 10);
    if (length > UINT16_MAX ||
        length > IOSU_MAX_PAYLOAD - IOSU_CONTROL_REQUEST_SIZE) {
        s5l8900_usb_iosu_error(s, "USB control transfer is too large");
        return false;
    }

    kind = (request_type & IOSU_ENDPOINT_IN) ? IOSU_TRANSFER_CONTROL_IN :
                                              IOSU_TRANSFER_CONTROL_OUT;
    expected_size = IOSU_CONTROL_REQUEST_SIZE;
    if (kind == IOSU_TRANSFER_CONTROL_OUT) {
        expected_size += length;
        data = s->iosu_payload + IOSU_CONTROL_REQUEST_SIZE;
    }
    if (s->iosu_payload_size != expected_size) {
        s5l8900_usb_iosu_error(s,
                               "USB control request has invalid data length");
        return false;
    }

    s5l8900_usb_begin_transfer(s, kind, 0, timeout, length, data);
    s->setup_packet[0] = request_type;
    s->setup_packet[1] = s->iosu_payload[1];
    stw_le_p(s->setup_packet + 2, lduw_be_p(s->iosu_payload + 2));
    stw_le_p(s->setup_packet + 4, lduw_be_p(s->iosu_payload + 4));
    stw_le_p(s->setup_packet + 6, length);
    s5l8900_usb_service_transfer(s);
    return true;
}

static bool s5l8900_usb_iosu_bulk(S5L8900USBState *s, bool input)
{
    uint8_t endpoint_address;
    uint8_t endpoint;
    uint32_t timeout;
    uint32_t length;
    uint32_t expected_size;
    const uint8_t *data = NULL;

    if (s->iosu_payload_size < IOSU_BULK_REQUEST_SIZE) {
        s5l8900_usb_iosu_error(s, "USB bulk request is truncated");
        return false;
    }
    endpoint_address = s->iosu_payload[0];
    endpoint = endpoint_address & 0xf;
    timeout = ldl_be_p(s->iosu_payload + 1);
    length = ldl_be_p(s->iosu_payload + 5);
    if (!!(endpoint_address & IOSU_ENDPOINT_IN) != input ||
        endpoint_address & 0x70 || !endpoint) {
        s5l8900_usb_iosu_error(s, "USB bulk endpoint direction is invalid");
        return false;
    }
    if (length > IOSU_MAX_PAYLOAD - IOSU_BULK_REQUEST_SIZE) {
        s5l8900_usb_iosu_error(s, "USB bulk transfer is too large");
        return false;
    }

    expected_size = IOSU_BULK_REQUEST_SIZE;
    if (!input) {
        expected_size += length;
        data = s->iosu_payload + IOSU_BULK_REQUEST_SIZE;
    }
    if (s->iosu_payload_size != expected_size) {
        s5l8900_usb_iosu_error(s,
                               "USB bulk request has invalid data length");
        return false;
    }

    s5l8900_usb_begin_transfer(s,
                               input ? IOSU_TRANSFER_BULK_IN :
                                       IOSU_TRANSFER_BULK_OUT,
                               endpoint, timeout, length, data);
    s5l8900_usb_service_transfer(s);
    return true;
}

static void s5l8900_usb_iosu_handle_request(S5L8900USBState *s)
{
    if (s->transfer_kind != IOSU_TRANSFER_NONE) {
        s5l8900_usb_iosu_error(s, "another USB transfer is pending");
        return;
    }

    switch (s->iosu_opcode) {
    case IOSU_ENUMERATE:
        if (s->iosu_payload_size) {
            s5l8900_usb_iosu_error(s, "ENUMERATE payload must be empty");
            break;
        }
        s5l8900_usb_iosu_enumerate(s);
        break;
    case IOSU_SET_CONFIGURATION:
        if (s->iosu_payload_size != 1) {
            s5l8900_usb_iosu_error(s, "invalid USB configuration");
            break;
        }
        if (s->iosu_payload[0] == s->host_configuration) {
            s5l8900_usb_iosu_send(s, s->iosu_opcode,
                                  s->iosu_request_id, 0, NULL, 0);
            break;
        }
        s5l8900_usb_begin_transfer(s, IOSU_TRANSFER_CONTROL_OUT, 0,
                                   10000, 0, NULL);
        s->setup_packet[0] = 0;
        s->setup_packet[1] = USB_REQ_SET_CONFIGURATION;
        stw_le_p(s->setup_packet + 2, s->iosu_payload[0]);
        s5l8900_usb_service_transfer(s);
        break;
    case IOSU_SET_INTERFACE:
        if (s->iosu_payload_size != 2) {
            s5l8900_usb_iosu_error(s, "invalid USB interface selection");
            break;
        }
        if (s->iosu_payload[0] == s->host_interface &&
            s->iosu_payload[1] == s->host_altsetting) {
            s5l8900_usb_iosu_send(s, s->iosu_opcode,
                                  s->iosu_request_id, 0, NULL, 0);
            break;
        }
        s5l8900_usb_begin_transfer(s, IOSU_TRANSFER_CONTROL_OUT, 0,
                                   10000, 0, NULL);
        s->setup_packet[0] = USB_DIR_OUT | USB_TYPE_STANDARD |
                             USB_RECIP_INTERFACE;
        s->setup_packet[1] = USB_REQ_SET_INTERFACE;
        stw_le_p(s->setup_packet + 2, s->iosu_payload[1]);
        stw_le_p(s->setup_packet + 4, s->iosu_payload[0]);
        s5l8900_usb_service_transfer(s);
        break;
    case IOSU_RESET:
        if (s->iosu_payload_size) {
            s5l8900_usb_iosu_error(s, "RESET payload must be empty");
            break;
        }
        s5l8900_usb_iosu_bus_reset(s);
        s5l8900_usb_iosu_send(s, s->iosu_opcode, s->iosu_request_id,
                              0, NULL, 0);
        break;
    case IOSU_CONTROL:
        s5l8900_usb_iosu_control(s);
        break;
    case IOSU_BULK_OUT:
        s5l8900_usb_iosu_bulk(s, false);
        break;
    case IOSU_BULK_IN:
        s5l8900_usb_iosu_bulk(s, true);
        break;
    default:
        s5l8900_usb_iosu_error(s, "unknown USB-over-IP opcode");
        break;
    }
}

static bool s5l8900_usb_iosu_parse_header(S5L8900USBState *s)
{
    uint16_t flags;

    s->iosu_opcode = s->iosu_header[5];
    flags = lduw_be_p(s->iosu_header + 6);
    s->iosu_request_id = ldl_be_p(s->iosu_header + 8);
    s->iosu_payload_size = ldl_be_p(s->iosu_header + 12);

    if (memcmp(s->iosu_header, "IOSU", 4) ||
        s->iosu_header[4] != IOSU_VERSION || flags) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900-usb: incompatible USB-over-IP frame\n");
        return false;
    }
    if (s->iosu_payload_size > IOSU_MAX_PAYLOAD) {
        s5l8900_usb_iosu_error(s,
                               "USB-over-IP request exceeds payload limit");
        return false;
    }
    if (s->iosu_payload_size) {
        s->iosu_payload = g_malloc(s->iosu_payload_size);
    }
    return true;
}

static int s5l8900_usb_iosu_can_read(void *opaque)
{
    S5L8900USBState *s = opaque;

    if (s->transfer_kind != IOSU_TRANSFER_NONE) {
        return 0;
    }
    if (s->iosu_header_used < IOSU_HEADER_SIZE) {
        return IOSU_HEADER_SIZE - s->iosu_header_used;
    }
    return MIN(s->iosu_payload_size - s->iosu_payload_used, INT_MAX);
}

static void s5l8900_usb_iosu_read(void *opaque, const uint8_t *buf, int size)
{
    S5L8900USBState *s = opaque;

    while (size) {
        if (s->iosu_header_used < IOSU_HEADER_SIZE) {
            uint32_t count = MIN(size,
                                 IOSU_HEADER_SIZE - s->iosu_header_used);

            memcpy(s->iosu_header + s->iosu_header_used, buf, count);
            s->iosu_header_used += count;
            buf += count;
            size -= count;
            if (s->iosu_header_used != IOSU_HEADER_SIZE) {
                continue;
            }
            if (!s5l8900_usb_iosu_parse_header(s)) {
                s5l8900_usb_iosu_reset_parser(s);
                continue;
            }
            if (!s->iosu_payload_size) {
                s5l8900_usb_iosu_handle_request(s);
                s5l8900_usb_iosu_reset_parser(s);
            }
            continue;
        }

        uint32_t count = MIN(size,
                             s->iosu_payload_size - s->iosu_payload_used);

        memcpy(s->iosu_payload + s->iosu_payload_used, buf, count);
        s->iosu_payload_used += count;
        buf += count;
        size -= count;
        if (s->iosu_payload_used == s->iosu_payload_size) {
            s5l8900_usb_iosu_handle_request(s);
            s5l8900_usb_iosu_reset_parser(s);
        }
    }
}

static void s5l8900_usb_iosu_event(void *opaque, QEMUChrEvent event)
{
    S5L8900USBState *s = opaque;

    if (event == CHR_EVENT_OPENED || event == CHR_EVENT_CLOSED) {
        s5l8900_usb_iosu_reset_parser(s);
        s5l8900_usb_clear_transfer(s);
    }
}

static uint32_t s5l8900_usb_daint(S5L8900USBState *s)
{
    uint32_t value = 0;

    for (unsigned i = 0; i < S5L8900_USB_ENDPOINTS; i++) {
        if (s->in_ep[i].interrupt) {
            value |= BIT(i);
        }
        if (s->out_ep[i].interrupt) {
            value |= BIT(i + 16);
        }
    }
    return value;
}

static uint32_t s5l8900_usb_gintsts(S5L8900USBState *s)
{
    uint32_t status = s->gintsts;

    for (unsigned i = 0; i < S5L8900_USB_ENDPOINTS; i++) {
        if ((s->daintmsk & BIT(i)) &&
            (s->in_ep[i].interrupt & s->diepmsk)) {
            status |= GINTSTS_IEPINT;
        }
        if ((s->daintmsk & BIT(i + 16)) &&
            (s->out_ep[i].interrupt & s->doepmsk)) {
            status |= GINTSTS_OEPINT;
        }
    }
    return status;
}

static void s5l8900_usb_update_irq(S5L8900USBState *s)
{
    bool level = (s->gahbcfg & GAHBCFG_GLBL_INTR_EN) &&
                 (s5l8900_usb_gintsts(s) & s->gintmsk);

    qemu_set_irq(s->irq, level);
}

static bool s5l8900_usb_decode_endpoint(hwaddr offset,
                                        S5L8900USBEndpoint **endpoint,
                                        hwaddr *reg,
                                        S5L8900USBState *s)
{
    unsigned index;

    if (offset >= EP_IN_BASE &&
        offset < EP_IN_BASE + S5L8900_USB_ENDPOINTS * EP_REGISTER_STRIDE) {
        index = (offset - EP_IN_BASE) / EP_REGISTER_STRIDE;
        *reg = (offset - EP_IN_BASE) % EP_REGISTER_STRIDE;
        *endpoint = &s->in_ep[index];
        return true;
    }
    if (offset >= EP_OUT_BASE &&
        offset < EP_OUT_BASE + S5L8900_USB_ENDPOINTS * EP_REGISTER_STRIDE) {
        index = (offset - EP_OUT_BASE) / EP_REGISTER_STRIDE;
        *reg = (offset - EP_OUT_BASE) % EP_REGISTER_STRIDE;
        *endpoint = &s->out_ep[index];
        return true;
    }
    return false;
}

static uint64_t s5l8900_usb_endpoint_read(S5L8900USBEndpoint *endpoint,
                                           hwaddr reg)
{
    switch (reg) {
    case EP_CONTROL:
        return endpoint->control;
    case EP_INTERRUPT:
        return endpoint->interrupt;
    case EP_TRANSFER_SIZE:
        return endpoint->transfer_size;
    case EP_DMA_ADDRESS:
        return endpoint->dma_address;
    case EP_TX_FIFO_STATUS:
        return 0xffff;
    default:
        return 0;
    }
}

static void s5l8900_usb_endpoint_write(S5L8900USBState *s,
                                        S5L8900USBEndpoint *endpoint,
                                        bool input, hwaddr reg,
                                        uint32_t value)
{
    trace_s5l8900_usb_endpoint_write(input, reg, value,
                                     endpoint->control,
                                     endpoint->transfer_size,
                                     endpoint->dma_address);
    switch (reg) {
    case EP_CONTROL: {
        uint32_t status = endpoint->control & DXEPCTL_NAKSTS;

        endpoint->control = (value & ~EP_CONTROL_COMMANDS &
                             ~DXEPCTL_NAKSTS) | status;
        if (value & DXEPCTL_SNAK) {
            endpoint->control |= DXEPCTL_NAKSTS;
            if (input) {
                endpoint->interrupt |= DXEPINT_INEPNAKEFF;
            }
        }
        if (value & DXEPCTL_CNAK) {
            endpoint->control &= ~DXEPCTL_NAKSTS;
        }
        if (value & DXEPCTL_EPDIS) {
            endpoint->control &= ~DXEPCTL_EPENA;
            endpoint->interrupt |= DXEPINT_EPDISBLD;
        }
        break;
    }
    case EP_INTERRUPT:
        endpoint->interrupt &= ~value;
        break;
    case EP_TRANSFER_SIZE:
        endpoint->transfer_size = value;
        break;
    case EP_DMA_ADDRESS:
        endpoint->dma_address = value;
        break;
    case EP_TX_FIFO_STATUS:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900-usb: invalid endpoint write at 0x%" HWADDR_PRIx
                      "\n", reg);
        break;
    }
    s5l8900_usb_update_irq(s);
    s5l8900_usb_service_transfer(s);
}

static uint64_t s5l8900_usb_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8900USBState *s = S5L8900_USB(opaque);
    S5L8900USBEndpoint *endpoint;
    hwaddr reg;

    if (s5l8900_usb_decode_endpoint(offset, &endpoint, &reg, s)) {
        return s5l8900_usb_endpoint_read(endpoint, reg);
    }
    if (offset >= DEVICE_TX_FIFO_BASE && offset <= DEVICE_TX_FIFO_LAST) {
        return s->dieptxf[(offset - DEVICE_TX_FIFO_BASE) / 4];
    }

    switch (offset) {
    case GOTGCTL:
        return s->gotgctl;
    case GOTGINT:
        return s->gotgint;
    case GAHBCFG:
        return s->gahbcfg;
    case GUSBCFG:
        return s->gusbcfg;
    case GRSTCTL:
        return GRSTCTL_AHBIDLE;
    case GINTSTS:
        return s5l8900_usb_gintsts(s);
    case GINTMSK:
        return s->gintmsk;
    case GRXFSIZ:
        return s->grxfsiz;
    case GNPTXFSIZ:
        return s->gnptxfsiz;
    case GNPTXSTS:
        return 0x00ff0000 | 0xffff;
    case GHWCFG1:
        return S5L8900_GHWCFG1;
    case GHWCFG2:
        return S5L8900_GHWCFG2;
    case GHWCFG3:
        return S5L8900_GHWCFG3;
    case GHWCFG4:
        return S5L8900_GHWCFG4;
    case DCFG:
        return s->dcfg;
    case DCTL:
        return s->dctl;
    case DSTS:
        return 0;
    case DIEPMSK:
        return s->diepmsk;
    case DOEPMSK:
        return s->doepmsk;
    case DAINT:
        return s5l8900_usb_daint(s);
    case DAINTMSK:
        return s->daintmsk;
    case PCGCTL:
        return s->pcgcctl;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900-usb: unimplemented read at 0x%" HWADDR_PRIx
                      "\n", offset);
        return 0;
    }
}

static void s5l8900_usb_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    S5L8900USBState *s = S5L8900_USB(opaque);
    S5L8900USBEndpoint *endpoint;
    hwaddr reg;

    if (s5l8900_usb_decode_endpoint(offset, &endpoint, &reg, s)) {
        s5l8900_usb_endpoint_write(s, endpoint,
                                   offset < EP_OUT_BASE, reg, value);
        return;
    }
    if (offset >= DEVICE_TX_FIFO_BASE && offset <= DEVICE_TX_FIFO_LAST) {
        s->dieptxf[(offset - DEVICE_TX_FIFO_BASE) / 4] = value;
        return;
    }

    switch (offset) {
    case GOTGCTL:
        s->gotgctl = value;
        break;
    case GOTGINT:
        s->gotgint &= ~value;
        break;
    case GAHBCFG:
        s->gahbcfg = value;
        break;
    case GUSBCFG:
        s->gusbcfg = value;
        break;
    case GRSTCTL:
        /* Reset/flush command bits complete synchronously. */
        break;
    case GINTSTS:
        s->gintsts &= ~value;
        break;
    case GINTMSK:
        s->gintmsk = value;
        break;
    case GRXFSIZ:
        s->grxfsiz = value;
        break;
    case GNPTXFSIZ:
        s->gnptxfsiz = value;
        break;
    case DCFG:
        s->dcfg = value;
        break;
    case DCTL:
        s->dctl = (value & ~(DCTL_SGNPINNAK | DCTL_CGNPINNAK |
                             DCTL_SGOUTNAK | DCTL_CGOUTNAK |
                             DCTL_GNPINNAKSTS | DCTL_GOUTNAKSTS)) |
                  (s->dctl & (DCTL_GNPINNAKSTS | DCTL_GOUTNAKSTS));
        if (value & DCTL_SGNPINNAK) {
            s->dctl |= DCTL_GNPINNAKSTS;
            s->gintsts |= GINTSTS_GINNAKEFF;
        }
        if (value & DCTL_CGNPINNAK) {
            s->dctl &= ~DCTL_GNPINNAKSTS;
            s->gintsts &= ~GINTSTS_GINNAKEFF;
        }
        if (value & DCTL_SGOUTNAK) {
            s->dctl |= DCTL_GOUTNAKSTS;
            s->gintsts |= GINTSTS_GOUTNAKEFF;
        }
        if (value & DCTL_CGOUTNAK) {
            s->dctl &= ~DCTL_GOUTNAKSTS;
            s->gintsts &= ~GINTSTS_GOUTNAKEFF;
        }
        break;
    case DSTS:
        break;
    case DIEPMSK:
        s->diepmsk = value;
        break;
    case DOEPMSK:
        s->doepmsk = value;
        break;
    case DAINT:
        for (unsigned i = 0; i < S5L8900_USB_ENDPOINTS; i++) {
            if (value & BIT(i)) {
                s->in_ep[i].interrupt = 0;
            }
            if (value & BIT(i + 16)) {
                s->out_ep[i].interrupt = 0;
            }
        }
        break;
    case DAINTMSK:
        s->daintmsk = value;
        break;
    case PCGCTL:
        s->pcgcctl = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900-usb: unimplemented write 0x%" PRIx64
                      " at 0x%" HWADDR_PRIx "\n", value, offset);
        break;
    }
    s5l8900_usb_update_irq(s);
}

static const MemoryRegionOps s5l8900_usb_ops = {
    .read = s5l8900_usb_read,
    .write = s5l8900_usb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static uint64_t s5l8900_usb_phy_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    S5L8900USBState *s = S5L8900_USB(opaque);

    if (offset <= PHY_RESET_CONTROL) {
        return s->phy[offset / 4];
    }
    qemu_log_mask(LOG_UNIMP,
                  "s5l8900-usb-phy: unimplemented read at 0x%" HWADDR_PRIx
                  "\n", offset);
    return 0;
}

static void s5l8900_usb_phy_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    S5L8900USBState *s = S5L8900_USB(opaque);

    if (offset <= PHY_RESET_CONTROL) {
        s->phy[offset / 4] = value;
        return;
    }
    qemu_log_mask(LOG_UNIMP,
                  "s5l8900-usb-phy: unimplemented write 0x%" PRIx64
                  " at 0x%" HWADDR_PRIx "\n", value, offset);
}

static const MemoryRegionOps s5l8900_usb_phy_ops = {
    .read = s5l8900_usb_phy_read,
    .write = s5l8900_usb_phy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_usb_reset(DeviceState *dev)
{
    S5L8900USBState *s = S5L8900_USB(dev);

    s5l8900_usb_clear_transfer(s);
    s->gotgctl = 0;
    s->gotgint = 0;
    s->gahbcfg = 0;
    s->gusbcfg = 0;
    s->gintsts = 0;
    s->gintmsk = 0;
    s->grxfsiz = 0;
    s->gnptxfsiz = 0;
    memset(s->dieptxf, 0, sizeof(s->dieptxf));
    s->dcfg = 0;
    s->dctl = DCTL_SFTDISCON;
    s->diepmsk = 0;
    s->doepmsk = 0;
    s->daintmsk = 0;
    s->pcgcctl = 0;
    memset(s->in_ep, 0, sizeof(s->in_ep));
    memset(s->out_ep, 0, sizeof(s->out_ep));
    memset(s->phy, 0, sizeof(s->phy));
    s->host_configuration = 0;
    s->host_interface = 0;
    s->host_altsetting = 0;
    s->host_enumerated = false;
    s5l8900_usb_update_irq(s);
}

static int s5l8900_usb_post_load(void *opaque, int version_id)
{
    S5L8900USBState *s = opaque;

    s5l8900_usb_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_s5l8900_usb_endpoint = {
    .name = TYPE_S5L8900_USB "/endpoint",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(control, S5L8900USBEndpoint),
        VMSTATE_UINT32(interrupt, S5L8900USBEndpoint),
        VMSTATE_UINT32(transfer_size, S5L8900USBEndpoint),
        VMSTATE_UINT32(dma_address, S5L8900USBEndpoint),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_s5l8900_usb = {
    .name = TYPE_S5L8900_USB,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8900_usb_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(gotgctl, S5L8900USBState),
        VMSTATE_UINT32(gotgint, S5L8900USBState),
        VMSTATE_UINT32(gahbcfg, S5L8900USBState),
        VMSTATE_UINT32(gusbcfg, S5L8900USBState),
        VMSTATE_UINT32(gintsts, S5L8900USBState),
        VMSTATE_UINT32(gintmsk, S5L8900USBState),
        VMSTATE_UINT32(grxfsiz, S5L8900USBState),
        VMSTATE_UINT32(gnptxfsiz, S5L8900USBState),
        VMSTATE_UINT32_ARRAY(dieptxf, S5L8900USBState,
                             S5L8900_USB_TX_FIFOS),
        VMSTATE_UINT32(dcfg, S5L8900USBState),
        VMSTATE_UINT32(dctl, S5L8900USBState),
        VMSTATE_UINT32(diepmsk, S5L8900USBState),
        VMSTATE_UINT32(doepmsk, S5L8900USBState),
        VMSTATE_UINT32(daintmsk, S5L8900USBState),
        VMSTATE_UINT32(pcgcctl, S5L8900USBState),
        VMSTATE_STRUCT_ARRAY(in_ep, S5L8900USBState,
                             S5L8900_USB_ENDPOINTS, 1,
                             vmstate_s5l8900_usb_endpoint,
                             S5L8900USBEndpoint),
        VMSTATE_STRUCT_ARRAY(out_ep, S5L8900USBState,
                             S5L8900_USB_ENDPOINTS, 1,
                             vmstate_s5l8900_usb_endpoint,
                             S5L8900USBEndpoint),
        VMSTATE_UINT32_ARRAY(phy, S5L8900USBState, PHY_REGISTER_COUNT),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8900_usb_init(Object *obj)
{
    S5L8900USBState *s = S5L8900_USB(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_usb_ops, s,
                          TYPE_S5L8900_USB, S5L8900_USB_MMIO_SIZE);
    memory_region_init_io(&s->phy_iomem, obj, &s5l8900_usb_phy_ops, s,
                          TYPE_S5L8900_USB ".phy",
                          S5L8900_USB_PHY_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_mmio(sbd, &s->phy_iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->cable_present,
                             "cable-present", 1);
}

static void s5l8900_usb_realize(DeviceState *dev, Error **errp)
{
    S5L8900USBState *s = S5L8900_USB(dev);

    s->transfer_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                     s5l8900_usb_transfer_timeout, s);
    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_set_handlers(&s->chr, s5l8900_usb_iosu_can_read,
                                 s5l8900_usb_iosu_read,
                                 s5l8900_usb_iosu_event, NULL, s, NULL, true);
        /*
         * A configured transport represents an attached, powered cable.
         * Socket reconnects represent USB re-enumeration and must not create
         * a false VBUS removal/insertion edge at the PMU.
         */
        qemu_set_irq(s->cable_present, 1);
    }
}

static void s5l8900_usb_unrealize(DeviceState *dev)
{
    S5L8900USBState *s = S5L8900_USB(dev);

    s5l8900_usb_iosu_reset_parser(s);
    qemu_chr_fe_deinit(&s->chr, true);
    timer_free(s->transfer_timer);
    s->transfer_timer = NULL;
}

static const Property s5l8900_usb_properties[] = {
    DEFINE_PROP_CHR("chardev", S5L8900USBState, chr),
};

static void s5l8900_usb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, s5l8900_usb_reset);
    dc->desc = "Apple S5L8900 USB device controller";
    dc->realize = s5l8900_usb_realize;
    dc->unrealize = s5l8900_usb_unrealize;
    dc->vmsd = &vmstate_s5l8900_usb;
    device_class_set_props(dc, s5l8900_usb_properties);
}

static const TypeInfo s5l8900_usb_info = {
    .name = TYPE_S5L8900_USB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900USBState),
    .instance_init = s5l8900_usb_init,
    .class_init = s5l8900_usb_class_init,
};

static void s5l8900_usb_register_types(void)
{
    type_register_static(&s5l8900_usb_info);
}

type_init(s5l8900_usb_register_types)
