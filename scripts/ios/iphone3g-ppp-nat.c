/*
 * Rootless UART PPP to libslirp bridge for the iPhone1,2 development board.
 * The Guest runs its stock pppd; this process is its peer and NAT boundary.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "iphone3g-ppp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libslirp.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_POLL_FDS 1024
#define TX_BUFFER_SIZE (2u * PPP_MAX_FRAME + 2u)

typedef struct Bridge Bridge;

typedef struct BridgeTimer {
    struct BridgeTimer *next;
    SlirpTimerCb callback;
    void *callback_opaque;
    int64_t expires_ms;
    bool active;
} BridgeTimer;

struct Bridge {
    ppp_peer_t ppp;
    Slirp *slirp;
    BridgeTimer *timers;
    struct pollfd pollfds[MAX_POLL_FDS];
    int slirp_events[MAX_POLL_FDS];
    size_t poll_count;
    int uart_fd;
    uint8_t tx[TX_BUFFER_SIZE];
    size_t tx_offset;
    size_t tx_length;
    uint64_t ethernet_in;
    uint64_t ethernet_out;
    uint64_t non_ipv4_out;
    ppp_phase_t last_phase;
    bool ppp_started;
    uint64_t started_ns;
};

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int64_t slirp_clock_get_ns(void *opaque)
{
    (void)opaque;
    return (int64_t)monotonic_ns();
}

static void *slirp_timer_new(SlirpTimerCb callback, void *callback_opaque,
                             void *opaque)
{
    Bridge *bridge = opaque;
    BridgeTimer *timer = calloc(1, sizeof(*timer));
    if (!timer) {
        return NULL;
    }
    timer->callback = callback;
    timer->callback_opaque = callback_opaque;
    timer->next = bridge->timers;
    bridge->timers = timer;
    return timer;
}

static void slirp_timer_free(void *timer_opaque, void *opaque)
{
    Bridge *bridge = opaque;
    BridgeTimer *timer = timer_opaque;
    BridgeTimer **link = &bridge->timers;
    while (*link && *link != timer) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = timer->next;
    }
    free(timer);
}

static void slirp_timer_mod(void *timer_opaque, int64_t expires_ms,
                            void *opaque)
{
    (void)opaque;
    BridgeTimer *timer = timer_opaque;
    timer->expires_ms = expires_ms;
    timer->active = true;
}

static void slirp_notify(void *opaque)
{
    (void)opaque;
}

static void slirp_guest_error(const char *message, void *opaque)
{
    (void)opaque;
    fprintf(stderr, "libslirp guest error: %s\n", message);
}

static slirp_ssize_t slirp_send_packet(const void *packet, size_t length,
                                       void *opaque)
{
    Bridge *bridge = opaque;
    const uint8_t *frame = packet;
    if (length < 14) {
        return (slirp_ssize_t)length;
    }
    uint16_t ether_type = (uint16_t)((frame[12] << 8) | frame[13]);
    if (ether_type != 0x0800) {
        bridge->non_ipv4_out++;
        return (slirp_ssize_t)length;
    }
    if (ppp_send_ip(&bridge->ppp, frame + 14, length - 14)) {
        bridge->ethernet_out++;
        if (bridge->ethernet_out == 1) {
            fprintf(stderr, "NAT first response: %zu IPv4 bytes to Guest\n",
                    length - 14);
        }
    }
    return (slirp_ssize_t)length;
}

static void ppp_ip_input(void *opaque, const uint8_t *packet, size_t length)
{
    static const uint8_t guest_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    static const uint8_t gateway_mac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
    Bridge *bridge = opaque;
    uint8_t frame[14 + PPP_MRU_DEFAULT];
    if (length > PPP_MRU_DEFAULT) {
        return;
    }
    memcpy(frame, gateway_mac, sizeof(gateway_mac));
    memcpy(frame + 6, guest_mac, sizeof(guest_mac));
    frame[12] = 0x08;
    frame[13] = 0x00;
    memcpy(frame + 14, packet, length);
    bridge->ethernet_in++;
    if (bridge->ethernet_in == 1) {
        fprintf(stderr, "NAT first request: %zu IPv4 bytes from Guest\n", length);
    }
    slirp_input(bridge->slirp, frame, (int)(length + 14));
}

static int slirp_add_poll(slirp_os_socket fd, int events, void *opaque)
{
    Bridge *bridge = opaque;
    if (bridge->poll_count >= MAX_POLL_FDS) {
        return -1;
    }
    size_t index = bridge->poll_count++;
    short native = 0;
    if (events & SLIRP_POLL_IN) {
        native |= POLLIN;
    }
    if (events & SLIRP_POLL_OUT) {
        native |= POLLOUT;
    }
    if (events & SLIRP_POLL_PRI) {
        native |= POLLPRI;
    }
    bridge->pollfds[index].fd = fd;
    bridge->pollfds[index].events = native;
    bridge->pollfds[index].revents = 0;
    bridge->slirp_events[index] = events;
    return (int)index;
}

static int slirp_get_revents(int index, void *opaque)
{
    Bridge *bridge = opaque;
    if (index < 0 || (size_t)index >= bridge->poll_count) {
        return 0;
    }
    short native = bridge->pollfds[index].revents;
    int events = 0;
    if (native & POLLIN) {
        events |= SLIRP_POLL_IN;
    }
    if (native & POLLOUT) {
        events |= SLIRP_POLL_OUT;
    }
    if (native & POLLPRI) {
        events |= SLIRP_POLL_PRI;
    }
    if (native & POLLERR) {
        events |= SLIRP_POLL_ERR;
    }
    if (native & POLLHUP) {
        events |= SLIRP_POLL_HUP;
    }
    return events & (bridge->slirp_events[index] | SLIRP_POLL_ERR |
                     SLIRP_POLL_HUP);
}

static void run_timers(Bridge *bridge, int64_t now_ms)
{
    for (;;) {
        BridgeTimer *due = NULL;
        for (BridgeTimer *timer = bridge->timers; timer; timer = timer->next) {
            if (timer->active && timer->expires_ms <= now_ms) {
                due = timer;
                break;
            }
        }
        if (!due) {
            return;
        }
        due->active = false;
        due->callback(due->callback_opaque);
    }
}

static uint32_t timer_timeout(Bridge *bridge, int64_t now_ms,
                              uint32_t timeout_ms)
{
    for (BridgeTimer *timer = bridge->timers; timer; timer = timer->next) {
        if (!timer->active) {
            continue;
        }
        int64_t remaining = timer->expires_ms - now_ms;
        uint32_t candidate = remaining <= 0 ? 0 :
            (remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining);
        if (candidate < timeout_ms) {
            timeout_ms = candidate;
        }
    }
    return timeout_ms;
}

static int connect_uart(const char *path, unsigned timeout_seconds)
{
    uint64_t deadline = monotonic_ns() + (uint64_t)timeout_seconds * 1000000000ull;
    for (;;) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("socket");
            return -1;
        }
        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        if (strlen(path) >= sizeof(address.sun_path)) {
            fprintf(stderr, "UART socket path is too long: %s\n", path);
            close(fd);
            return -1;
        }
        strcpy(address.sun_path, path);
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
                perror("fcntl");
                close(fd);
                return -1;
            }
            return fd;
        }
        int saved_errno = errno;
        close(fd);
        if (monotonic_ns() >= deadline) {
            errno = saved_errno;
            perror("connect UART");
            return -1;
        }
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&delay, NULL);
    }
}

static bool init_slirp(Bridge *bridge)
{
    SlirpConfig config;
    memset(&config, 0, sizeof(config));
    config.version = 4;
    config.in_enabled = true;
    config.disable_host_loopback = true;
    if (inet_pton(AF_INET, "10.0.2.0", &config.vnetwork) != 1 ||
        inet_pton(AF_INET, "255.255.255.0", &config.vnetmask) != 1 ||
        inet_pton(AF_INET, "10.0.2.2", &config.vhost) != 1 ||
        inet_pton(AF_INET, "10.0.2.15", &config.vdhcp_start) != 1 ||
        inet_pton(AF_INET, "10.0.2.3", &config.vnameserver) != 1) {
        return false;
    }

    SlirpCb callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.send_packet = slirp_send_packet;
    callbacks.guest_error = slirp_guest_error;
    callbacks.clock_get_ns = slirp_clock_get_ns;
    callbacks.timer_new = slirp_timer_new;
    callbacks.timer_free = slirp_timer_free;
    callbacks.timer_mod = slirp_timer_mod;
    callbacks.notify = slirp_notify;
    bridge->slirp = slirp_new(&config, &callbacks, bridge);
    return bridge->slirp != NULL;
}

static bool flush_uart(Bridge *bridge)
{
    if (bridge->tx_offset == bridge->tx_length) {
        bridge->tx_offset = 0;
        bridge->tx_length = ppp_output(&bridge->ppp, bridge->tx,
                                       sizeof(bridge->tx));
    }
    while (bridge->tx_offset < bridge->tx_length) {
        ssize_t count = send(bridge->uart_fd, bridge->tx + bridge->tx_offset,
                             bridge->tx_length - bridge->tx_offset, 0);
        if (count > 0) {
            bridge->tx_offset += (size_t)count;
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                          errno == EINTR)) {
            return true;
        }
        return false;
    }
    return true;
}

static int run_bridge(Bridge *bridge, unsigned witness_packets)
{
    uint8_t input[4096];
    bridge->last_phase = ppp_phase(&bridge->ppp);
    fprintf(stderr, "PPP phase: %s\n", ppp_phase_name(bridge->last_phase));

    while (!stop_requested) {
        uint64_t now_ns = monotonic_ns();
        uint32_t now_ms = (uint32_t)((now_ns - bridge->started_ns) / 1000000ull);
        if (bridge->ppp_started) {
            ppp_tick(&bridge->ppp, now_ms);
        }
        run_timers(bridge, (int64_t)(now_ns / 1000000ull));

        ppp_phase_t phase = ppp_phase(&bridge->ppp);
        if (phase != bridge->last_phase) {
            fprintf(stderr, "PPP phase: %s\n", ppp_phase_name(phase));
            bridge->last_phase = phase;
        }
        if (witness_packets && bridge->ethernet_in >= witness_packets &&
            bridge->ethernet_out >= witness_packets) {
            return 0;
        }

        bridge->poll_count = 1;
        bridge->pollfds[0].fd = bridge->uart_fd;
        bridge->pollfds[0].events = POLLIN;
        if (bridge->tx_offset < bridge->tx_length ||
            ppp_output_pending(&bridge->ppp)) {
            bridge->pollfds[0].events |= POLLOUT;
        }
        bridge->pollfds[0].revents = 0;
        bridge->slirp_events[0] = 0;
        uint32_t timeout_ms = 100;
        slirp_pollfds_fill_socket(bridge->slirp, &timeout_ms, slirp_add_poll,
                                  bridge);
        timeout_ms = timer_timeout(bridge, (int64_t)(now_ns / 1000000ull),
                                   timeout_ms);
        int result = poll(bridge->pollfds, (nfds_t)bridge->poll_count,
                          (int)timeout_ms);
        if (result < 0 && errno != EINTR) {
            perror("poll");
            return 1;
        }
        if (result >= 0) {
            slirp_pollfds_poll(bridge->slirp, 0, slirp_get_revents, bridge);
        }
        if (bridge->pollfds[0].revents & POLLIN) {
            ssize_t count = recv(bridge->uart_fd, input, sizeof(input), 0);
            if (count > 0) {
                if (!bridge->ppp_started) {
                    /*
                     * iBSS and iBEC poll the debug UART before launchd starts
                     * pppd.  An eager Configure-Request is therefore boot
                     * input, not PPP traffic, and can prevent iBEC USB
                     * re-enumeration.  Let the Guest's first octet prove the
                     * line has changed ownership before transmitting.
                     */
                    ppp_open(&bridge->ppp);
                    bridge->ppp_started = true;
                    fprintf(stderr,
                            "Guest PPP activity detected; starting LCP\n");
                }
                ppp_input(&bridge->ppp, input, (size_t)count);
            } else if (count == 0 ||
                       (errno != EAGAIN && errno != EWOULDBLOCK &&
                        errno != EINTR)) {
                fprintf(stderr, "UART disconnected\n");
                return 1;
            }
        }
        if ((bridge->pollfds[0].revents & POLLOUT) && !flush_uart(bridge)) {
            fprintf(stderr, "UART write failed: %s\n", strerror(errno));
            return 1;
        }
        if (bridge->pollfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "UART poll reported disconnect\n");
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *socket_path = NULL;
    unsigned connect_timeout = 300;
    unsigned witness_packets = 0;
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--socket") == 0 && index + 1 < argc) {
            socket_path = argv[++index];
        } else if (strcmp(argv[index], "--connect-timeout") == 0 &&
                   index + 1 < argc) {
            connect_timeout = (unsigned)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argv[index], "--witness-packets") == 0 &&
                   index + 1 < argc) {
            witness_packets = (unsigned)strtoul(argv[++index], NULL, 10);
        } else {
            fprintf(stderr, "usage: %s --socket PATH [--connect-timeout S] "
                    "[--witness-packets N]\n", argv[0]);
            return 2;
        }
    }
    if (!socket_path) {
        fprintf(stderr, "--socket is required\n");
        return 2;
    }

    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);
    signal(SIGPIPE, SIG_IGN);

    Bridge bridge;
    memset(&bridge, 0, sizeof(bridge));
    bridge.uart_fd = -1;
    bridge.started_ns = monotonic_ns();
    ppp_init(&bridge.ppp, NULL);
    ppp_set_ip_sink(&bridge.ppp, ppp_ip_input, &bridge);
    if (!init_slirp(&bridge)) {
        fprintf(stderr, "could not initialize libslirp\n");
        return 1;
    }
    bridge.uart_fd = connect_uart(socket_path, connect_timeout);
    if (bridge.uart_fd < 0) {
        slirp_cleanup(bridge.slirp);
        return 1;
    }
    fprintf(stderr, "connected UART4 at %s; NAT 10.0.2.15 -> libslirp\n",
            socket_path);
    int result = run_bridge(&bridge, witness_packets);
    fprintf(stderr,
            "PPP summary: phase=%s guest_ip=%s guest_ipv4=%llu "
            "nat_ipv4=%llu fcs_errors=%llu dropped=%llu\n",
            ppp_phase_name(ppp_phase(&bridge.ppp)),
            bridge.ppp.assigned ? "10.0.2.15" : "none",
            (unsigned long long)bridge.ethernet_in,
            (unsigned long long)bridge.ethernet_out,
            (unsigned long long)bridge.ppp.stats.fcs_errors,
            (unsigned long long)bridge.ppp.stats.ip_frames_dropped);
    close(bridge.uart_fd);
    slirp_cleanup(bridge.slirp);
    return result;
}
