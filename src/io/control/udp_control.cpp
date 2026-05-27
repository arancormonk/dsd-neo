// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief UDP-based remote control interface implementation.
 *
 * Provides a background UDP listener to accept retune commands and invokes a
 * user-supplied callback with the requested frequency. Supports clean start/stop
 * semantics and resource management.
 */

#include <dsd-neo/io/udp_control.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/log.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#endif
#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <sys/socket.h>
#endif
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/platform.h"

struct udp_control {
    int port;
    char bindaddr[64];
    dsd_socket_t sockfd;
    dsd_thread_t thread;
    udp_control_retune_cb cb;
    std::atomic<int> stop_flag;
};

/**
 * @brief Convert 4-byte little-endian payload (following a leading command byte)
 * to a 32-bit unsigned integer.
 *
 * @param buf Pointer to 5-byte buffer where buf[0] is a command and buf[1..4]
 *            encode the value as little-endian.
 * @return Parsed 32-bit value.
 */
static unsigned int
udp_chars_to_int(const unsigned char* buf) {
    int i;
    unsigned int val = 0;
    for (i = 1; i < 5; i++) {
        val = val | ((buf[i]) << ((i - 1) * 8));
    }
    return val;
}

/**
 * @brief UDP control thread entry.
 *
 * Receives 5-byte messages on the socket opened by the start function. When a
 * valid tune command is received, invokes the registered callback with the new
 * frequency.
 *
 * @param arg Pointer to `udp_control`.
 * @return NULL on exit.
 */
static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    udp_thread_fn(void* arg) {
    udp_control* ctrl = static_cast<udp_control*>(arg);
    unsigned char buffer[5];

    DSD_MEMSET(buffer, 0, sizeof(buffer));
    LOG_INFO("RTL UDP retune control listening on %s:%d\n", ctrl->bindaddr, ctrl->port);

    while (!ctrl->stop_flag.load()) {
        int n = dsd_socket_recv(ctrl->sockfd, buffer, 5, 0);
        if (n <= 0) {
            continue;
        }
        if (n == 5 && buffer[0] == 0) {
            unsigned int new_freq = udp_chars_to_int(buffer);
            if (ctrl->cb) {
                ctrl->cb(new_freq);
            }
            LOG_INFO("\nTuning to: %u [Hz] \n", new_freq);
        }
    }
    DSD_THREAD_RETURN;
}

/**
 * @brief Start UDP control thread.
 *
 * Starts a background UDP listener on the specified port. On valid messages,
 * invokes the provided retune callback with the parsed frequency.
 *
 * @param udp_port UDP port to bind and listen on (0 disables/start no-op).
 * @param cb Callback invoked upon receiving a valid retune command.
 * @return Opaque handle on success; NULL on failure.
 */
extern "C" udp_control*
udp_control_start(int udp_port, udp_control_retune_cb cb) {
    return udp_control_start_bound("127.0.0.1", udp_port, cb);
}

extern "C" udp_control*
udp_control_start_bound(const char* bindaddr, int udp_port, udp_control_retune_cb cb) {
    if (udp_port <= 0 || udp_port > 65535) {
        return NULL;
    }
    const char* effective_bindaddr = (bindaddr && bindaddr[0] != '\0') ? bindaddr : "127.0.0.1";

    struct sockaddr_in serv_addr;
    DSD_MEMSET(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((uint16_t)udp_port);
#if DSD_PLATFORM_WIN_NATIVE
    if (InetPtonA(AF_INET, effective_bindaddr, &serv_addr.sin_addr) != 1) {
#else
    if (inet_pton(AF_INET, effective_bindaddr, &serv_addr.sin_addr) != 1) {
#endif
        LOG_ERROR("Invalid RTL UDP control bind address: %s\n", effective_bindaddr);
        return NULL;
    }

    dsd_socket_t sockfd = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd == DSD_INVALID_SOCKET) {
        LOG_ERROR("Failed to open RTL UDP control socket for %s:%d\n", effective_bindaddr, udp_port);
        return NULL;
    }

    if (dsd_socket_bind(sockfd, reinterpret_cast<struct sockaddr*>(&serv_addr), sizeof(serv_addr)) != 0) {
        LOG_ERROR("Failed to bind RTL UDP control socket on %s:%d\n", effective_bindaddr, udp_port);
        dsd_socket_close(sockfd);
        return NULL;
    }

    (void)dsd_socket_set_recv_timeout(sockfd, 250);

    udp_control* ctrl = static_cast<udp_control*>(malloc(sizeof(udp_control)));
    if (!ctrl) {
        dsd_socket_close(sockfd);
        return NULL;
    }
    ctrl->port = udp_port;
    DSD_SNPRINTF(ctrl->bindaddr, sizeof ctrl->bindaddr, "%s", effective_bindaddr);
    ctrl->bindaddr[sizeof ctrl->bindaddr - 1] = '\0';
    ctrl->sockfd = sockfd;
    ctrl->cb = cb;
    ctrl->stop_flag.store(0);
    int rc = dsd_thread_create(&ctrl->thread, udp_thread_fn, ctrl);
    if (rc != 0) {
        dsd_socket_close(sockfd);
        free(ctrl);
        return NULL;
    }
    return ctrl;
}

/**
 * @brief Stop UDP control thread and free resources.
 *
 * Closes the socket, joins the worker thread, and releases the handle.
 *
 * @param ctrl Opaque handle returned by `udp_control_start` (safe to pass NULL).
 */
extern "C" void
udp_control_stop(udp_control* ctrl) {
    if (!ctrl) {
        return;
    }
    ctrl->stop_flag.store(1);
    if (ctrl->sockfd != DSD_INVALID_SOCKET) {
        dsd_socket_shutdown(ctrl->sockfd, SHUT_RDWR);
    }
    dsd_thread_join(ctrl->thread);
    if (ctrl->sockfd != DSD_INVALID_SOCKET) {
        dsd_socket_close(ctrl->sockfd);
        ctrl->sockfd = DSD_INVALID_SOCKET;
    }
    free(ctrl);
}
