// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Regression tests for RTL UDP retune control binding and packet delivery.
 */

#include <dsd-neo/io/udp_control.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

typedef struct callback_state {
    dsd_mutex_t mu;
    dsd_cond_t cv;
    int called;
    uint32_t freq;
} callback_state;

static callback_state g_callback_state;

static void
retune_cb(uint32_t new_frequency_hz) {
    callback_state* state = &g_callback_state;
    dsd_mutex_lock(&state->mu);
    state->called = 1;
    state->freq = new_frequency_hz;
    dsd_cond_signal(&state->cv);
    dsd_mutex_unlock(&state->mu);
}

static int
reserve_loopback_port(void) {
    dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == DSD_INVALID_SOCKET) {
        return -1;
    }

    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (dsd_socket_bind(sock, (const struct sockaddr*)&addr, (int)sizeof(addr)) != 0) {
        dsd_socket_close(sock);
        return -1;
    }

#if DSD_PLATFORM_WIN_NATIVE
    int slen = (int)sizeof(addr);
#else
    socklen_t slen = (socklen_t)sizeof(addr);
#endif
    if (getsockname(sock, (struct sockaddr*)&addr, &slen) != 0) {
        dsd_socket_close(sock);
        return -1;
    }
    int port = (int)ntohs(addr.sin_port);
    dsd_socket_close(sock);
    return port;
}

static int
send_retune_packet(int port, uint32_t freq) {
    dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == DSD_INVALID_SOCKET) {
        return -1;
    }

    struct sockaddr_in dst;
    DSD_MEMSET(&dst, 0, sizeof(dst));
    if (dsd_socket_resolve("127.0.0.1", port, &dst) != 0) {
        dsd_socket_close(sock);
        return -1;
    }

    unsigned char packet[5];
    packet[0] = 0;
    packet[1] = (unsigned char)(freq & 0xFFu);
    packet[2] = (unsigned char)((freq >> 8) & 0xFFu);
    packet[3] = (unsigned char)((freq >> 16) & 0xFFu);
    packet[4] = (unsigned char)((freq >> 24) & 0xFFu);

    int sent = dsd_socket_sendto(sock, packet, sizeof(packet), 0, (const struct sockaddr*)&dst, (int)sizeof(dst));
    dsd_socket_close(sock);
    return (sent == (int)sizeof(packet)) ? 0 : -1;
}

static int
wait_for_callback(callback_state* state, unsigned int timeout_ms) {
    int timed_out = 0;
    dsd_mutex_lock(&state->mu);
    while (!state->called && !timed_out) {
        timed_out = (dsd_cond_timedwait(&state->cv, &state->mu, timeout_ms) != 0);
    }
    int called = state->called;
    dsd_mutex_unlock(&state->mu);
    return called;
}

static int
test_loopback_delivery(void) {
    int port = reserve_loopback_port();
    if (port <= 0) {
        DSD_FPRINTF(stderr, "failed to reserve UDP test port\n");
        return 1;
    }

    DSD_MEMSET(&g_callback_state, 0, sizeof(g_callback_state));
    if (dsd_mutex_init(&g_callback_state.mu) != 0 || dsd_cond_init(&g_callback_state.cv) != 0) {
        DSD_FPRINTF(stderr, "failed to initialize callback synchronization\n");
        return 1;
    }

    struct udp_control* ctrl = udp_control_start_bound("127.0.0.1", port, retune_cb);
    if (!ctrl) {
        DSD_FPRINTF(stderr, "udp_control_start_bound failed on 127.0.0.1:%d\n", port);
        dsd_cond_destroy(&g_callback_state.cv);
        dsd_mutex_destroy(&g_callback_state.mu);
        return 1;
    }

    uint32_t expected = 851375000u;
    int test_rc = 0;
    if (send_retune_packet(port, expected) != 0 || !wait_for_callback(&g_callback_state, 2000)) {
        DSD_FPRINTF(stderr, "retune callback was not invoked\n");
        test_rc = 1;
    } else if (g_callback_state.freq != expected) {
        DSD_FPRINTF(stderr, "expected callback frequency %u, got %u\n", expected, g_callback_state.freq);
        test_rc = 1;
    }

    udp_control_stop(ctrl);
    dsd_cond_destroy(&g_callback_state.cv);
    dsd_mutex_destroy(&g_callback_state.mu);
    return test_rc;
}

static int
test_invalid_bind_address_fails(void) {
    struct udp_control* ctrl = udp_control_start_bound("localhost", 9911, retune_cb);
    if (ctrl) {
        DSD_FPRINTF(stderr, "expected non-numeric bind address to fail\n");
        udp_control_stop(ctrl);
        return 1;
    }
    ctrl = udp_control_start(0, retune_cb);
    if (ctrl) {
        DSD_FPRINTF(stderr, "expected port 0 to disable UDP control\n");
        udp_control_stop(ctrl);
        return 1;
    }
    return 0;
}

int
main(void) {
    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "dsd_socket_init failed\n");
        return 1;
    }

    int rc = 0;
    rc |= test_loopback_delivery();
    rc |= test_invalid_bind_address_fails();

    dsd_socket_cleanup();
    return rc;
}
