/*
 * QEMU IOSU transport backend for upstream usbmuxd.
 *
 * Copyright (C) 2026 qemu-ios-4 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 or version 3.
 */

#define _POSIX_C_SOURCE 200809L

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "device.h"
#include "log.h"
#include "usb.h"

#define IOSU_HEADER_SIZE 16
#define IOSU_CONTROL_SIZE 14
#define IOSU_BULK_SIZE 9
#define IOSU_MAX_PAYLOAD (16U * 1024U * 1024U)
#define IOSU_RESPONSE 1
#define IOSU_ERROR 2
#define IOSU_VERSION 1
#define IOSU_ENUMERATE 1
#define IOSU_CONTROL 2
#define IOSU_BULK_OUT 3
#define IOSU_BULK_IN 4
#define IOSU_SET_CONFIGURATION 5
#define IOSU_SET_INTERFACE 6
#define IOSU_RESET 7

#define USB_DT_DEVICE 1
#define USB_DT_CONFIG 2
#define USB_DT_INTERFACE 4
#define USB_DT_ENDPOINT 5
#define USB_REQ_GET_DESCRIPTOR 6
#define USB_ENDPOINT_IN 0x80
#define USB_ENDPOINT_XFER_BULK 2
#define IPHETH_INTERFACE_CLASS 0xff
#define IPHETH_INTERFACE_SUBCLASS 0xfd
#define IPHETH_INTERFACE_PROTOCOL 1
#define IPHETH_ALTERNATE_SETTING 1

#define IOSU_CONTROL_TIMEOUT_MS 1000
#define IOSU_BULK_IN_TIMEOUT_MS 100
#define IOSU_BULK_OUT_TIMEOUT_MS 10000
#define IOSU_ENUMERATION_TIMEOUT_MS 120000
#define IOSU_ENUMERATION_RESET_INTERVAL_MS 5000
#define IOSU_QUEUE_LIMIT (4U * 1024U * 1024U)
#define IOSU_LOCATION 0x00010001U
#define IOSU_HIGH_SPEED_BPS 480000000ULL

struct packet {
    struct packet *next;
    uint32_t length;
    unsigned char data[];
};

struct usb_device {
    int socket;
    int notify_pipe[2];
    pthread_t worker;
    pthread_mutex_t lock;
    struct packet *tx_head;
    struct packet *tx_tail;
    struct packet *rx_head;
    struct packet *rx_tail;
    size_t tx_bytes;
    size_t rx_bytes;
    uint32_t next_request_id;
    uint16_t pid;
    uint8_t configuration;
    uint8_t interface;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t eth_interface;
    uint8_t eth_ep_in;
    uint8_t eth_ep_out;
    bool eth_probe;
    bool eth_seen;
    bool stop;
    bool dead;
    bool added;
    char serial[256];
};

static struct usb_device *iosu_device;

static uint16_t load_be16(const unsigned char *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint16_t load_le16(const unsigned char *p)
{
    return ((uint16_t)p[1] << 8) | p[0];
}

static uint32_t load_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void store_be16(unsigned char *p, uint16_t value)
{
    p[0] = value >> 8;
    p[1] = value;
}

static void store_be32(unsigned char *p, uint32_t value)
{
    p[0] = value >> 24;
    p[1] = value >> 16;
    p[2] = value >> 8;
    p[3] = value;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);

    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}

static int send_all(int fd, const void *data, size_t size)
{
    const unsigned char *cursor = data;

    while (size) {
#ifdef MSG_NOSIGNAL
        ssize_t count = send(fd, cursor, size, MSG_NOSIGNAL);
#else
        ssize_t count = send(fd, cursor, size, 0);
#endif
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return -1;
        }
        cursor += count;
        size -= count;
    }
    return 0;
}

static int receive_all(int fd, void *data, size_t size)
{
    unsigned char *cursor = data;

    while (size) {
        ssize_t count = recv(fd, cursor, size, 0);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return -1;
        }
        cursor += count;
        size -= count;
    }
    return 0;
}

/* Returns 0 for success, 1 for an IOSU transfer error, and -1 for transport. */
static int iosu_request(struct usb_device *dev, uint8_t opcode,
                        const void *payload, uint32_t payload_size,
                        unsigned char **response, uint32_t *response_size)
{
    unsigned char header[IOSU_HEADER_SIZE] = { 'I', 'O', 'S', 'U' };
    unsigned char *body = NULL;
    uint32_t request_id = ++dev->next_request_id;
    uint32_t size;
    uint16_t flags;
    int result = -1;

    if (payload_size > IOSU_MAX_PAYLOAD) {
        return -1;
    }
    header[4] = IOSU_VERSION;
    header[5] = opcode;
    store_be32(header + 8, request_id);
    store_be32(header + 12, payload_size);
    if (send_all(dev->socket, header, sizeof(header)) < 0 ||
        (payload_size && send_all(dev->socket, payload, payload_size) < 0) ||
        receive_all(dev->socket, header, sizeof(header)) < 0) {
        return -1;
    }
    flags = load_be16(header + 6);
    size = load_be32(header + 12);
    if (memcmp(header, "IOSU", 4) || header[4] != IOSU_VERSION ||
        header[5] != opcode || load_be32(header + 8) != request_id ||
        !(flags & IOSU_RESPONSE) || (flags & ~(IOSU_RESPONSE | IOSU_ERROR)) ||
        size > IOSU_MAX_PAYLOAD) {
        return -1;
    }
    if (size) {
        body = malloc(size);
        if (!body || receive_all(dev->socket, body, size) < 0) {
            goto out;
        }
    }
    if (flags & IOSU_ERROR) {
        result = 1;
        goto out;
    }
    *response = body;
    *response_size = size;
    body = NULL;
    result = 0;

out:
    free(body);
    return result;
}

static int connect_endpoint(const char *endpoint)
{
    struct addrinfo hints = { 0 };
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    const char *separator = strrchr(endpoint, ':');
    char *host;
    int fd = -1;

    if (!separator || separator == endpoint || !separator[1]) {
        usbmuxd_log(LL_ERROR, "QEMU_IOSU_ADDRESS must be HOST:PORT");
        return -1;
    }
    host = strndup(endpoint, separator - endpoint);
    if (!host) {
        return -1;
    }
    if (host[0] == '[' && separator > endpoint + 1 && separator[-1] == ']') {
        memmove(host, host + 1, strlen(host));
        host[strlen(host) - 1] = '\0';
    }
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, separator + 1, &hints, &addresses)) {
        free(host);
        return -1;
    }
    free(host);
    for (address = addresses; address; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype,
                    address->ai_protocol);
        if (fd < 0) {
            continue;
        }
#ifdef SO_NOSIGPIPE
        int one = 1;

        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        if (!connect(fd, address->ai_addr, address->ai_addrlen)) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

static int control_in(struct usb_device *dev, uint8_t request,
                      uint16_t value, uint16_t index, uint16_t length,
                      unsigned char **response, uint32_t *response_size)
{
    unsigned char payload[IOSU_CONTROL_SIZE] = { 0x80, request };

    store_be16(payload + 2, value);
    store_be16(payload + 4, index);
    store_be32(payload + 6, IOSU_CONTROL_TIMEOUT_MS);
    store_be32(payload + 10, length);
    return iosu_request(dev, IOSU_CONTROL, payload, sizeof(payload),
                        response, response_size);
}

static int get_descriptor(struct usb_device *dev, uint8_t type, uint8_t index,
                          uint16_t length, unsigned char **response,
                          uint32_t *response_size)
{
    return control_in(dev, USB_REQ_GET_DESCRIPTOR,
                      ((uint16_t)type << 8) | index, 0, length,
                      response, response_size);
}

static int find_mux_interface(const unsigned char *config, uint32_t size,
                              uint8_t *interface, uint8_t *ep_in,
                              uint8_t *ep_out)
{
    uint32_t offset = 0;
    bool selected = false;

    *ep_in = 0;
    *ep_out = 0;
    while (offset + 2 <= size) {
        uint8_t length = config[offset];
        uint8_t type = config[offset + 1];

        if (length < 2 || length > size - offset) {
            return -1;
        }
        if (type == USB_DT_INTERFACE && length >= 9) {
            selected = config[offset + 3] == 0 &&
                       config[offset + 5] == INTERFACE_CLASS &&
                       config[offset + 6] == INTERFACE_SUBCLASS &&
                       config[offset + 7] == INTERFACE_PROTOCOL;
            if (selected) {
                *interface = config[offset + 2];
                *ep_in = 0;
                *ep_out = 0;
            }
        } else if (selected && type == USB_DT_ENDPOINT && length >= 7 &&
                   (config[offset + 3] & 3) == USB_ENDPOINT_XFER_BULK) {
            uint8_t endpoint = config[offset + 2];

            if (endpoint & USB_ENDPOINT_IN) {
                *ep_in = endpoint;
            } else {
                *ep_out = endpoint;
            }
            if (*ep_in && *ep_out) {
                return 0;
            }
        }
        offset += length;
    }
    return -1;
}

static int find_ethernet_interface(const unsigned char *config, uint32_t size,
                                   uint8_t *interface, uint8_t *ep_in,
                                   uint8_t *ep_out)
{
    uint32_t offset = 0;
    bool selected = false;

    *ep_in = 0;
    *ep_out = 0;
    while (offset + 2 <= size) {
        uint8_t length = config[offset];
        uint8_t type = config[offset + 1];

        if (length < 2 || length > size - offset) {
            return -1;
        }
        if (type == USB_DT_INTERFACE && length >= 9) {
            selected = config[offset + 3] == IPHETH_ALTERNATE_SETTING &&
                       config[offset + 5] == IPHETH_INTERFACE_CLASS &&
                       config[offset + 6] == IPHETH_INTERFACE_SUBCLASS &&
                       config[offset + 7] == IPHETH_INTERFACE_PROTOCOL;
            if (selected) {
                *interface = config[offset + 2];
                *ep_in = 0;
                *ep_out = 0;
            }
        } else if (selected && type == USB_DT_ENDPOINT && length >= 7 &&
                   (config[offset + 3] & 3) == USB_ENDPOINT_XFER_BULK) {
            uint8_t endpoint = config[offset + 2];

            if (endpoint & USB_ENDPOINT_IN) {
                *ep_in = endpoint;
            } else {
                *ep_out = endpoint;
            }
            if (*ep_in && *ep_out) {
                return 0;
            }
        }
        offset += length;
    }
    return -1;
}

static int enumerate_device(struct usb_device *dev)
{
    unsigned char *response = NULL;
    uint32_t size = 0;
    uint64_t deadline;
    uint64_t next_reset;
    uint8_t configurations;
    uint8_t index;
    int result;

    result = iosu_request(dev, IOSU_ENUMERATE, NULL, 0, &response, &size);
    if (result || size < 6 || load_be16(response) != VID_APPLE) {
        free(response);
        return -1;
    }
    size_t serial_size = load_be16(response + 4);
    if (serial_size != size - 6 || serial_size >= sizeof(dev->serial)) {
        free(response);
        return -1;
    }
    memcpy(dev->serial, response + 6, serial_size);
    dev->serial[serial_size] = '\0';
    free(response);

    response = NULL;
    size = 0;
    result = iosu_request(dev, IOSU_RESET, NULL, 0, &response, &size);
    if (result || size) {
        free(response);
        return -1;
    }
    free(response);

    deadline = mstime64() + IOSU_ENUMERATION_TIMEOUT_MS;
    next_reset = mstime64() + IOSU_ENUMERATION_RESET_INTERVAL_MS;
    do {
        response = NULL;
        size = 0;
        result = get_descriptor(dev, USB_DT_DEVICE, 0, 18,
                                &response, &size);
        if (!result) {
            break;
        }
        free(response);
        response = NULL;
        size = 0;
        if (mstime64() >= next_reset) {
            result = iosu_request(dev, IOSU_RESET, NULL, 0,
                                  &response, &size);
            if (result || size) {
                free(response);
                return -1;
            }
            free(response);
            response = NULL;
            size = 0;
            result = -1;
            next_reset = mstime64() + IOSU_ENUMERATION_RESET_INTERVAL_MS;
        }
    } while (mstime64() < deadline);
    if (result || size != 18 || response[0] < 18 ||
        response[1] != USB_DT_DEVICE || load_le16(response + 8) != VID_APPLE) {
        free(response);
        return -1;
    }
    dev->pid = load_le16(response + 10);
    configurations = response[17];
    free(response);

    for (index = 0; index < configurations; index++) {
        uint16_t total;
        uint8_t mux_interface = 0;
        uint8_t mux_ep_in = 0;
        uint8_t mux_ep_out = 0;
        uint8_t eth_interface = 0;
        uint8_t eth_ep_in = 0;
        uint8_t eth_ep_out = 0;
        bool has_mux;
        bool has_ethernet;

        response = NULL;
        size = 0;
        if (get_descriptor(dev, USB_DT_CONFIG, index, 9, &response, &size) ||
            size != 9 || response[0] < 9 || response[1] != USB_DT_CONFIG) {
            free(response);
            return -1;
        }
        total = load_le16(response + 2);
        free(response);
        if (total < 9) {
            return -1;
        }
        response = NULL;
        size = 0;
        if (get_descriptor(dev, USB_DT_CONFIG, index, total,
                           &response, &size) || size != total) {
            free(response);
            return -1;
        }
        has_mux = !find_mux_interface(response, size, &mux_interface,
                                      &mux_ep_in, &mux_ep_out);
        has_ethernet = dev->eth_probe &&
                       !find_ethernet_interface(response, size,
                                                &eth_interface,
                                                &eth_ep_in, &eth_ep_out);
        if (has_mux && (!dev->configuration || has_ethernet)) {
            dev->configuration = response[5];
            dev->interface = mux_interface;
            dev->ep_in = mux_ep_in;
            dev->ep_out = mux_ep_out;
            dev->eth_interface = has_ethernet ? eth_interface : 0;
            dev->eth_ep_in = has_ethernet ? eth_ep_in : 0;
            dev->eth_ep_out = has_ethernet ? eth_ep_out : 0;
        }
        free(response);
        if (has_mux && (!dev->eth_probe || has_ethernet)) {
            break;
        }
    }
    if (!dev->configuration) {
        usbmuxd_log(LL_ERROR, "Guest exposes no AppleUSBMux interface");
        return -1;
    }

    unsigned char configuration = dev->configuration;
    response = NULL;
    size = 0;
    result = iosu_request(dev, IOSU_SET_CONFIGURATION, &configuration, 1,
                          &response, &size);
    free(response);
    if (result || size) {
        return -1;
    }
    if (dev->eth_interface) {
        unsigned char setting[2] = {
            dev->eth_interface, IPHETH_ALTERNATE_SETTING
        };

        response = NULL;
        size = 0;
        result = iosu_request(dev, IOSU_SET_INTERFACE, setting,
                              sizeof(setting), &response, &size);
        free(response);
        if (result || size) {
            return -1;
        }
    }
    usbmuxd_log(LL_NOTICE,
                "IOSU selected configuration %u interface %u endpoints 0x%02x/0x%02x",
                dev->configuration, dev->interface, dev->ep_out, dev->ep_in);
    if (dev->eth_interface) {
        usbmuxd_log(LL_NOTICE,
                    "IOSU selected Apple USB Ethernet interface %u alt %u endpoints 0x%02x/0x%02x",
                    dev->eth_interface, IPHETH_ALTERNATE_SETTING,
                    dev->eth_ep_out, dev->eth_ep_in);
    }
    return 0;
}

static struct packet *packet_new(const unsigned char *data, uint32_t length)
{
    struct packet *packet;

    if (length > IOSU_MAX_PAYLOAD - IOSU_BULK_SIZE ||
        sizeof(*packet) > SIZE_MAX - length) {
        return NULL;
    }
    packet = malloc(sizeof(*packet) + length);
    if (!packet) {
        return NULL;
    }
    packet->next = NULL;
    packet->length = length;
    if (length) {
        memcpy(packet->data, data, length);
    }
    return packet;
}

static void packet_append(struct packet **head, struct packet **tail,
                          struct packet *packet)
{
    if (*tail) {
        (*tail)->next = packet;
    } else {
        *head = packet;
    }
    *tail = packet;
}

static struct packet *packet_pop(struct packet **head, struct packet **tail)
{
    struct packet *packet = *head;

    if (packet) {
        *head = packet->next;
        if (!*head) {
            *tail = NULL;
        }
        packet->next = NULL;
    }
    return packet;
}

static int bulk_transfer(struct usb_device *dev, uint8_t endpoint, bool input,
                         const unsigned char *data, uint32_t length,
                         unsigned char **response, uint32_t *response_size)
{
    uint32_t payload_size = IOSU_BULK_SIZE + (input ? 0 : length);
    unsigned char *payload = malloc(payload_size);
    int result;

    if (!payload) {
        return -1;
    }
    payload[0] = endpoint;
    store_be32(payload + 1, input ? IOSU_BULK_IN_TIMEOUT_MS :
                                   IOSU_BULK_OUT_TIMEOUT_MS);
    store_be32(payload + 5, length);
    if (!input && length) {
        memcpy(payload + IOSU_BULK_SIZE, data, length);
    }
    result = iosu_request(dev, input ? IOSU_BULK_IN : IOSU_BULK_OUT,
                          payload, payload_size, response, response_size);
    free(payload);
    return result;
}

static void notify_main(struct usb_device *dev)
{
    unsigned char byte = 1;

    if (write(dev->notify_pipe[1], &byte, 1) < 0 && errno != EAGAIN &&
        errno != EWOULDBLOCK) {
        usbmuxd_log(LL_DEBUG, "IOSU notification pipe write failed: %s",
                    strerror(errno));
    }
}

static void mark_dead(struct usb_device *dev)
{
    pthread_mutex_lock(&dev->lock);
    dev->dead = true;
    pthread_mutex_unlock(&dev->lock);
    notify_main(dev);
}

static void *worker_main(void *opaque)
{
    struct usb_device *dev = opaque;

    for (;;) {
        struct packet *packet;
        unsigned char *response = NULL;
        uint32_t size = 0;
        int result;

        pthread_mutex_lock(&dev->lock);
        if (dev->stop) {
            pthread_mutex_unlock(&dev->lock);
            break;
        }
        packet = packet_pop(&dev->tx_head, &dev->tx_tail);
        if (packet) {
            dev->tx_bytes -= packet->length;
        }
        pthread_mutex_unlock(&dev->lock);

        if (packet) {
            result = bulk_transfer(dev, dev->ep_out, false, packet->data,
                                   packet->length, &response, &size);
            if (result || size != 4 || load_be32(response) != packet->length) {
                free(response);
                free(packet);
                mark_dead(dev);
                break;
            }
            free(response);
            if (packet->length && packet->length % USB_PACKET_SIZE == 0) {
                response = NULL;
                size = 0;
                result = bulk_transfer(dev, dev->ep_out, false, NULL, 0,
                                       &response, &size);
                if (result || size != 4 || load_be32(response)) {
                    free(response);
                    free(packet);
                    mark_dead(dev);
                    break;
                }
                free(response);
            }
            free(packet);
            continue;
        }

        if (dev->eth_ep_in) {
            result = bulk_transfer(dev, dev->eth_ep_in, true, NULL, 1516,
                                   &response, &size);
            if (result < 0) {
                free(response);
                mark_dead(dev);
                break;
            }
            if (!result && size && !dev->eth_seen) {
                dev->eth_seen = true;
                usbmuxd_log(LL_NOTICE,
                            "IOSU received first Apple USB Ethernet frame (%u bytes, prefix %02x%02x%02x%02x)",
                            size, response[0], size > 1 ? response[1] : 0,
                            size > 2 ? response[2] : 0,
                            size > 3 ? response[3] : 0);
            }
            free(response);
            response = NULL;
            size = 0;
        }

        result = bulk_transfer(dev, dev->ep_in, true, NULL, USB_MRU,
                               &response, &size);
        if (result < 0 || (!result && size > USB_MRU)) {
            free(response);
            mark_dead(dev);
            break;
        }
        if (!result && size) {
            packet = packet_new(response, size);
            free(response);
            if (!packet) {
                mark_dead(dev);
                break;
            }
            pthread_mutex_lock(&dev->lock);
            if (dev->rx_bytes > IOSU_QUEUE_LIMIT - packet->length) {
                pthread_mutex_unlock(&dev->lock);
                free(packet);
                mark_dead(dev);
                break;
            }
            packet_append(&dev->rx_head, &dev->rx_tail, packet);
            dev->rx_bytes += packet->length;
            pthread_mutex_unlock(&dev->lock);
            notify_main(dev);
        } else {
            free(response);
        }
    }
    return NULL;
}

const char *usb_get_serial(struct usb_device *dev)
{
    return dev ? dev->serial : NULL;
}

uint32_t usb_get_location(struct usb_device *dev)
{
    return dev ? IOSU_LOCATION : 0;
}

uint16_t usb_get_pid(struct usb_device *dev)
{
    return dev ? dev->pid : 0;
}

uint64_t usb_get_speed(struct usb_device *dev)
{
    return dev ? IOSU_HIGH_SPEED_BPS : 0;
}

int usb_send(struct usb_device *dev, const unsigned char *buf, int length)
{
    struct packet *packet;

    if (!dev || length < 0 || length > USB_MTU) {
        return -1;
    }
    packet = packet_new(buf, length);
    if (!packet) {
        return -1;
    }
    pthread_mutex_lock(&dev->lock);
    if (dev->stop || dev->dead ||
        dev->tx_bytes > IOSU_QUEUE_LIMIT - packet->length) {
        pthread_mutex_unlock(&dev->lock);
        free(packet);
        return -1;
    }
    packet_append(&dev->tx_head, &dev->tx_tail, packet);
    dev->tx_bytes += packet->length;
    pthread_mutex_unlock(&dev->lock);
    return 0;
}

void usb_get_fds(struct fdlist *list)
{
    if (iosu_device && iosu_device->notify_pipe[0] >= 0) {
        fdlist_add(list, FD_USB, iosu_device->notify_pipe[0], POLLIN);
    }
}

int usb_get_timeout(void)
{
    return 1000;
}

int usb_process(void)
{
    struct usb_device *dev = iosu_device;
    unsigned char bytes[64];

    if (!dev) {
        return 0;
    }
    while (read(dev->notify_pipe[0], bytes, sizeof(bytes)) > 0) {
    }
    for (;;) {
        struct packet *packet;

        pthread_mutex_lock(&dev->lock);
        packet = packet_pop(&dev->rx_head, &dev->rx_tail);
        if (packet) {
            dev->rx_bytes -= packet->length;
        }
        pthread_mutex_unlock(&dev->lock);
        if (!packet) {
            break;
        }
        if (dev->added) {
            device_data_input(dev, packet->data, packet->length);
        }
        free(packet);
    }
    pthread_mutex_lock(&dev->lock);
    bool dead = dev->dead;
    pthread_mutex_unlock(&dev->lock);
    if (dead && dev->added) {
        device_remove(dev);
        dev->added = false;
    }
    return 0;
}

int usb_process_timeout(int msec)
{
    struct pollfd pfd;

    if (!iosu_device) {
        return 0;
    }
    pfd.fd = iosu_device->notify_pipe[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, msec) < 0 && errno != EINTR) {
        return -1;
    }
    return usb_process();
}

int usb_discover(void)
{
    return iosu_device && !iosu_device->dead ? 1 : 0;
}

void usb_autodiscover(int enable)
{
    (void)enable;
}

static void free_packets(struct packet *packet)
{
    while (packet) {
        struct packet *next = packet->next;

        free(packet);
        packet = next;
    }
}

int usb_init(void)
{
    const char *endpoint = getenv("QEMU_IOSU_ADDRESS");
    const char *ethernet_probe = getenv("QEMU_IOSU_ETHERNET_PROBE");
    struct usb_device *dev = calloc(1, sizeof(*dev));
    int error;

    if (!dev) {
        return -1;
    }
    dev->socket = -1;
    dev->notify_pipe[0] = -1;
    dev->notify_pipe[1] = -1;
    dev->eth_probe = ethernet_probe && !strcmp(ethernet_probe, "1");
    if (!endpoint || !*endpoint) {
        endpoint = "127.0.0.1:1337";
    }
    dev->socket = connect_endpoint(endpoint);
    if (dev->socket < 0) {
        usbmuxd_log(LL_ERROR, "Cannot connect to QEMU IOSU at %s", endpoint);
        free(dev);
        return -1;
    }
    if (pipe(dev->notify_pipe) < 0 || set_nonblocking(dev->notify_pipe[0]) < 0 ||
        set_nonblocking(dev->notify_pipe[1]) < 0) {
        usbmuxd_log(LL_ERROR, "Cannot create IOSU notification pipe");
        if (dev->notify_pipe[0] >= 0) {
            close(dev->notify_pipe[0]);
            close(dev->notify_pipe[1]);
        }
        close(dev->socket);
        free(dev);
        return -1;
    }
    pthread_mutex_init(&dev->lock, NULL);
    if (enumerate_device(dev) < 0) {
        usbmuxd_log(LL_ERROR, "IOSU could not enumerate the Guest USBMux interface");
        goto fail;
    }
    error = pthread_create(&dev->worker, NULL, worker_main, dev);
    if (error) {
        usbmuxd_log(LL_ERROR, "Cannot start IOSU worker: %s", strerror(error));
        goto fail;
    }
    iosu_device = dev;
    if (device_add(dev) < 0) {
        usb_shutdown();
        return -1;
    }
    dev->added = true;
    usbmuxd_log(LL_NOTICE, "Connected QEMU IOSU device %04x:%04x at %s",
                VID_APPLE, dev->pid, endpoint);
    return 1;

fail:
    pthread_mutex_destroy(&dev->lock);
    close(dev->notify_pipe[0]);
    close(dev->notify_pipe[1]);
    close(dev->socket);
    free(dev);
    return -1;
}

void usb_shutdown(void)
{
    struct usb_device *dev = iosu_device;

    if (!dev) {
        return;
    }
    pthread_mutex_lock(&dev->lock);
    dev->stop = true;
    pthread_mutex_unlock(&dev->lock);
    shutdown(dev->socket, SHUT_RDWR);
    pthread_join(dev->worker, NULL);
    if (dev->added) {
        device_remove(dev);
        dev->added = false;
    }
    close(dev->socket);
    close(dev->notify_pipe[0]);
    close(dev->notify_pipe[1]);
    free_packets(dev->tx_head);
    free_packets(dev->rx_head);
    pthread_mutex_destroy(&dev->lock);
    free(dev);
    iosu_device = NULL;
}
