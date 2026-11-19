// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #834: a rigctl retune's mark of the UDP input counts every datagram that reached this host before it, also
 * those still queued in the socket because the receive thread was descheduled, and none that arrives after it. The
 * receive thread is held off the socket through the test seam (DSD_NEO_TEST_HOOKS, udp_input_test_hold_receive()), so
 * the old channel's datagram waits in the socket when the mark is taken; the new channel's, sent after the mark, is
 * read after the drop. The hold is set before the input starts, so the thread never waits on the socket with it set
 * (a thread waiting there would wake for the datagram and take it).
 *
 * A sender that never lets the socket go quiet (udp_input_test_endless_socket()) holds no lock for long: the decoder
 * still reads, a mark still returns, and the input still stops.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/udp_input.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/input_backlog.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/exitflag.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
    /* The receive thread is descheduled from its start: the old channel's datagram will wait in the socket. */
    udp_input_test_hold_receive(1);
    if (udp_input_start(&opts, "127.0.0.1", 0, 48000) != 0) {
        udp_input_test_hold_receive(0);
        DSD_FPRINTF(stderr, "udp_input_start failed\n");
        return 1;
    }
    const int port = bound_port(opts.udp_in_sockfd);
    dsd_socket_t tx = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int rc = expect("a sender", tx != DSD_INVALID_SOCKET && port > 0);
    const int16_t old_audio[] = {11, 12, 13};
    const int16_t new_audio[] = {21, 22};
    if (rc == 0) {
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

/* Issue #834: a mark is a complete count however long its drain is held up. 100 old datagrams wait (the receive
   thread held; queued through the test seam, so exactly 100 whatever the system's receive buffer holds), and the mark's
   drain is descheduled for 300 ms at its first read: it still takes all 100 before it returns, so none of them is
   counted after the cutoff and played on the new channel. Before, a drain that ran 250 ms on the real clock stopped
   after its first batch of 64 and returned that count as the mark. */
static int
test_mark_is_complete_after_a_stall(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.wav_sample_rate = 48000;
    udp_input_test_hold_receive(1);
    if (udp_input_start(&opts, "127.0.0.1", 0, 48000) != 0) {
        udp_input_test_hold_receive(0);
        DSD_FPRINTF(stderr, "udp_input_start failed\n");
        return 1;
    }
    udp_input_test_queue_datagrams(100);
    udp_input_test_stall_next_read(300U);
    dsd_input_cut cut;
    dsd_input_backlog_mark(&cut);
    char label[160];
    DSD_SNPRINTF(label, sizeof label, "a stalled mark still counts every queued datagram (counted %llu, want 100)",
                 (unsigned long long)cut.arrived);
    int rc = expect(label, cut.measured == 1U && cut.arrived == 100U);
    udp_input_test_queue_datagrams(0);
    udp_input_test_stall_next_read(0U);
    udp_input_test_hold_receive(0);
    udp_input_stop(&opts);
    return rc;
}

/* What the endless sender's case does on a thread of its own, so a lock held without end shows as a case that never
   finishes: a sample read, a mark taken and the input stopped, each while the socket never goes quiet. */
typedef struct {
    dsd_opts* opts;
    atomic_int read_one;
    atomic_int marked;
    atomic_int stopped;
} endless_case;

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    endless_case_run(void* arg) {
    endless_case* c = (endless_case*)arg;
    int16_t s = 1;
    for (int tries = 0; tries < 500 && !atomic_load(&c->read_one); tries++) {
        if (udp_input_read_sample_wait(c->opts, &s, 20U) == 1) {
            atomic_store(&c->read_one, 1);
        }
    }
    dsd_input_cut cut;
    dsd_input_backlog_mark(&cut);
    atomic_store(&c->marked, 1);
    udp_input_stop(c->opts);
    atomic_store(&c->stopped, 1);
    DSD_THREAD_RETURN;
}

/* Issue #834: a sender that keeps the socket readable faster than it is drained never keeps the ring's lock taken: the
   receive thread lets it go between batches, so the decoder reads and a stop gets through, and a backlog mark drains
   for a bounded budget. Before, the drain ran while the socket held anything, so with such a sender nothing else ever
   took the lock again. A case that has not finished after 10 s ends the test. */
static int
test_endless_sender_holds_no_lock(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.wav_sample_rate = 48000;
    if (udp_input_start(&opts, "127.0.0.1", 0, 48000) != 0) {
        DSD_FPRINTF(stderr, "udp_input_start failed\n");
        return 1;
    }
    const int port = bound_port(opts.udp_in_sockfd);
    dsd_socket_t tx = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int rc = expect("a sender for the endless case", tx != DSD_INVALID_SOCKET && port > 0);
    const int16_t wake[] = {1};
    udp_input_test_endless_socket(1);
    /* The first real datagram wakes the receive thread, which then finds the socket never empty. */
    rc |= expect("the waking datagram sent", rc == 0 && send_samples(tx, port, wake, 1U) == 0);
    static endless_case c;
    c.opts = &opts;
    atomic_store(&c.read_one, 0);
    atomic_store(&c.marked, 0);
    atomic_store(&c.stopped, 0);
    dsd_thread_t th;
    if (rc != 0 || dsd_thread_create(&th, endless_case_run, &c) != 0) {
        udp_input_test_endless_socket(0);
        udp_input_stop(&opts);
        return 1;
    }
    for (int waited = 0; waited < 1000 && !atomic_load(&c.stopped); waited++) {
        dsd_sleep_ms(10U);
    }
    if (!atomic_load(&c.stopped)) {
        DSD_FPRINTF(stderr, "FAIL: with a sender that never stops, the case is stuck after 10 s (read %d, marked %d)\n",
                    atomic_load(&c.read_one), atomic_load(&c.marked));
        exit(1); /* the stuck thread holds the input: nothing can be cleaned up */
    }
    (void)dsd_thread_join(th);
    udp_input_test_endless_socket(0);
    rc |= expect("the decoder read a sample while the sender went on", atomic_load(&c.read_one));
    (void)dsd_socket_close(tx);
    return rc;
}

int
main(void) {
    exitflag = 0;
    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "dsd_socket_init failed\n");
        return 1;
    }
    int rc = test_mark_counts_queued_datagrams();
    rc |= test_mark_is_complete_after_a_stall();
    rc |= test_endless_sender_holds_no_lock();
    dsd_socket_cleanup();
    if (rc == 0) {
        DSD_FPRINTF(stderr, "IO_UDP_INPUT_INGRESS: OK\n");
    }
    return rc;
}
