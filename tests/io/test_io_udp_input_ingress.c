// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #834: a rigctl retune's mark of the UDP input counts every datagram that reached this host before it, also
 * those still queued in the socket because the receive thread was descheduled, and none that arrives after it. The
 * receive thread is held off the socket through the test seam (DSD_NEO_TEST_HOOKS, udp_input_test_hold_receive()), so
 * the old channel's datagram waits in the socket when the mark is taken; the new channel's, sent after the mark, is
 * read after the drop.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/udp_input.h>
#include <dsd-neo/platform/input_backlog.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/exitflag.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "udp_input_test_support.h"
#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

static int
expect(const char* what, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

static int
bound_port(dsd_socket_t sock) {
    struct sockaddr_in sa;
    DSD_MEMSET(&sa, 0, sizeof sa);
#if DSD_PLATFORM_WIN_NATIVE
    int len = (int)sizeof sa;
#else
    socklen_t len = (socklen_t)sizeof sa;
#endif
    return getsockname(sock, (struct sockaddr*)&sa, &len) == 0 ? (int)ntohs(sa.sin_port) : -1;
}

/* One datagram of @p count PCM16LE samples to the input on @p port. */
static int
send_samples(dsd_socket_t tx, int port, const int16_t* samples, size_t count) {
    uint8_t buf[32];
    if (count * 2U > sizeof buf) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        buf[i * 2U] = (uint8_t)((uint16_t)samples[i] & 0xFFU);
        buf[i * 2U + 1U] = (uint8_t)((uint16_t)samples[i] >> 8);
    }
    struct sockaddr_in dst;
    DSD_MEMSET(&dst, 0, sizeof dst);
    if (dsd_socket_resolve("127.0.0.1", port, &dst) != 0) {
        return -1;
    }
    return dsd_socket_sendto(tx, buf, count * 2U, 0, (struct sockaddr*)&dst, (int)sizeof dst) == (int)(count * 2U) ? 0
                                                                                                                   : -1;
}

/* The next sample, waiting for it (INT16_MIN for none within 10 s: the bound only turns a lost datagram into a
   failure, never a slow receive thread). */
static int16_t
next_sample(dsd_opts* opts) {
    int16_t s = 0;
    for (int tries = 0; tries < 500; tries++) {
        if (udp_input_read_sample_wait(opts, &s, 20U) == 1) {
            return s;
        }
    }
    return INT16_MIN;
}

/* Whether the socket holds a datagram: loopback delivery is immediate, but bounded here all the same. */
static int
socket_holds_one(dsd_socket_t sock) {
    for (int tries = 0; tries < 500; tries++) {
        if (dsd_socket_wait(sock, DSD_SOCKET_WAIT_READ, 20U) > 0) {
            return 1;
        }
    }
    return 0;
}

static int
test_mark_counts_queued_datagrams(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.wav_sample_rate = 48000;
    if (udp_input_start(&opts, "127.0.0.1", 0, 48000) != 0) {
        DSD_FPRINTF(stderr, "udp_input_start failed\n");
        return 1;
    }
    const int port = bound_port(opts.udp_in_sockfd);
    dsd_socket_t tx = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int rc = expect("a sender", tx != DSD_INVALID_SOCKET && port > 0);
    const int16_t old_audio[] = {11, 12, 13};
    const int16_t new_audio[] = {21, 22};
    if (rc == 0) {
        /* The receive thread is descheduled: the old channel's datagram waits in the socket. */
        udp_input_test_hold_receive(1);
        rc |= expect("the old channel's datagram sent", send_samples(tx, port, old_audio, 3U) == 0);
        rc |= expect("it waits in the socket", socket_holds_one(opts.udp_in_sockfd));
        dsd_input_cut cut;
        dsd_input_backlog_mark(&cut);
        char label[160];
        DSD_SNPRINTF(label, sizeof label, "the mark counts the datagram queued in the socket (counted %llu, want 3)",
                     (unsigned long long)cut.arrived);
        rc |= expect(label, cut.measured == 1U && cut.arrived == 3U);
        /* The new channel's, after the mark, and the receive thread runs again. */
        rc |= expect("the new channel's datagram sent", send_samples(tx, port, new_audio, 2U) == 0);
        udp_input_test_hold_receive(0);
        dsd_input_cut now;
        DSD_MEMSET(&now, 0, sizeof now);
        for (int tries = 0; tries < 500 && now.arrived < 5U; tries++) {
            dsd_input_backlog_mark(&now);
            if (now.arrived < 5U) {
                dsd_sleep_ms(20U);
            }
        }
        rc |= expect("both datagrams in the ring", now.arrived == 5U);
        rc |= expect("the drop takes the old channel's samples", udp_input_discard_through(&opts, &cut) == 3L);
        const int16_t first = next_sample(&opts);
        const int16_t second = next_sample(&opts);
        DSD_SNPRINTF(label, sizeof label, "the new channel's samples are read (read %d, %d; want 21, 22)", (int)first,
                     (int)second);
        rc |= expect(label, first == 21 && second == 22);
    }
    udp_input_test_hold_receive(0);
    if (tx != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(tx);
    }
    udp_input_stop(&opts);
    return rc;
}

int
main(void) {
    exitflag = 0;
    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "dsd_socket_init failed\n");
        return 1;
    }
    const int rc = test_mark_counts_queued_datagrams();
    dsd_socket_cleanup();
    if (rc == 0) {
        DSD_FPRINTF(stderr, "IO_UDP_INPUT_INGRESS: OK\n");
    }
    return rc;
}
