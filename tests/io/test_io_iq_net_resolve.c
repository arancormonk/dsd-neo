// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * Issue #743: a raw I/Q stream's TCP reader looks a host name up off its own thread, so a stop never waits for the
 * resolver. The lookup of a reconnect is held, as a resolver that does not answer holds getaddrinfo(), and a stop
 * returns within a wait slice; the lookup, let go later, ends on its own. Before, the stop waited for the resolver
 * (here, until a watchdog let the lookup go). POSIX: the executable answers its own getaddrinfo()
 * (fake_getaddrinfo.c).
 */

#include <arpa/inet.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/iq_net.h>
#include <dsd-neo/io/iq_net_spec.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>

#include "fake_getaddrinfo.h"

enum { k_wait_ms = 5000, k_prompt_ms = 1500, k_watchdog_ms = 4000 };

static int g_failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond);                                       \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

/* The reader's notices are not under test. */
void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    (void)level;
    (void)format;
}

static atomic_int g_events[16];

static void
on_event(void* ctx, const dsd_iq_net_event* ev) {
    (void)ctx;
    if ((int)ev->kind >= 0 && (int)ev->kind < 16) {
        atomic_fetch_add(&g_events[(int)ev->kind], 1);
    }
}

static void
on_samples(void* ctx, const float* iq, size_t pairs) {
    (void)ctx;
    (void)iq;
    (void)pairs;
}

/* Wait until pred() holds, up to k_wait_ms. */
static int
wait_for(int (*pred)(void)) {
    const uint64_t until = dsd_realtime_mono_ms() + (uint64_t)k_wait_ms;
    while (!pred()) {
        if (dsd_realtime_mono_ms() > until) {
            return 0;
        }
        dsd_sleep_ms(2U);
    }
    return 1;
}

static int
one_held(void) {
    return dsd_fake_resolver_held() == 1;
}

static int
none_held(void) {
    return dsd_fake_resolver_held() == 0;
}

/* Lets a held lookup go after k_watchdog_ms, so a stop that waits for the resolver ends (and fails) rather than hang. */
static atomic_int g_watchdog_stop;

static DSD_THREAD_RETURN_TYPE
watchdog_main(void* arg) {
    (void)arg;
    const uint64_t until = dsd_realtime_mono_ms() + (uint64_t)k_watchdog_ms;
    while (!atomic_load(&g_watchdog_stop) && dsd_realtime_mono_ms() < until) {
        dsd_sleep_ms(5U);
    }
    dsd_fake_resolver_hold(0);
    DSD_THREAD_RETURN;
}

static void
test_stop_during_a_held_reconnect_lookup(void) {
    dsd_socket_t listener = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    DSD_MEMSET(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = (socklen_t)sizeof(sa);
    if (listener == DSD_INVALID_SOCKET || dsd_socket_bind(listener, (struct sockaddr*)&sa, (int)sizeof(sa)) != 0
        || dsd_socket_listen(listener, 4) != 0 || getsockname(listener, (struct sockaddr*)&sa, &len) != 0) {
        CHECK(!"a loopback listener");
        if (listener != DSD_INVALID_SOCKET) {
            (void)dsd_socket_close(listener);
        }
        return;
    }

    dsd_fake_resolver_reset(0x7F000001U, 0x7F000001U);
    dsd_iq_net_spec spec;
    DSD_MEMSET(&spec, 0, sizeof(spec));
    spec.kind = DSD_IQ_NET_KIND_TCP;
    DSD_SNPRINTF(spec.host, sizeof(spec.host), "%s", DSD_FAKE_RESOLVER_HOST);
    spec.port = (int)ntohs(sa.sin_port);
    spec.rate_hz = 48000U;
    spec.format = DSD_IQ_NET_FORMAT_CS16;
    dsd_iq_net_reader_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    cfg.on_samples = on_samples;
    cfg.on_event = on_event;
    dsd_iq_net_reader* r = dsd_iq_net_reader_create(&spec, &cfg);
    dsd_iq_net_open_error err;
    CHECK(r && dsd_iq_net_reader_open(r, &err) == 0);
    CHECK(dsd_fake_resolver_lookups() == 1);
    CHECK(dsd_socket_wait(listener, DSD_SOCKET_WAIT_READ, k_wait_ms) > 0);
    dsd_socket_t conn = dsd_socket_accept(listener, NULL, NULL);
    CHECK(conn != DSD_INVALID_SOCKET && dsd_iq_net_reader_start(r) == 0);

    /* The sender goes away and the resolver stops answering: the reconnect's lookup waits. */
    dsd_fake_resolver_hold(1);
    (void)dsd_socket_close(conn);
    CHECK(wait_for(one_held));
    dsd_thread_t watchdog;
    atomic_store(&g_watchdog_stop, 0);
    CHECK(dsd_thread_create(&watchdog, watchdog_main, NULL) == 0);

    const uint64_t t0 = dsd_realtime_mono_ms();
    dsd_iq_net_reader_stop(r);
    const uint64_t took = dsd_realtime_mono_ms() - t0;
    if (took >= (uint64_t)k_prompt_ms) {
        DSD_FPRINTF(stderr, "stop took %llu ms while the lookup was held\n", (unsigned long long)took);
    }
    CHECK(took < (uint64_t)k_prompt_ms);
    CHECK(dsd_iq_net_reader_state(r) == DSD_IQ_NET_STATE_STOPPED);
    CHECK(atomic_load(&g_events[DSD_IQ_NET_EVENT_RECONNECT]) == 0);

    /* The resolver answers at last: the abandoned lookup ends on its own. */
    atomic_store(&g_watchdog_stop, 1);
    (void)dsd_thread_join(watchdog);
    dsd_fake_resolver_hold(0);
    CHECK(wait_for(none_held));
    dsd_iq_net_reader_destroy(r);
    (void)dsd_socket_close(listener);
    /* Time for the helper to free its lookup before the leak check at exit. */
    dsd_sleep_ms(50U);
}

typedef struct {
    dsd_iq_net_reader* reader;
    atomic_int done;
    int rc;
} open_job;

static DSD_THREAD_RETURN_TYPE
open_main(void* arg) {
    open_job* job = (open_job*)arg;
    dsd_iq_net_open_error err;
    job->rc = dsd_iq_net_reader_open(job->reader, &err);
    atomic_store(&job->done, 1);
    DSD_THREAD_RETURN;
}

static dsd_iq_net_reader*
tcp_reader_for(const char* host, int port) {
    dsd_iq_net_spec spec;
    DSD_MEMSET(&spec, 0, sizeof(spec));
    spec.kind = DSD_IQ_NET_KIND_TCP;
    DSD_SNPRINTF(spec.host, sizeof(spec.host), "%s", host);
    spec.port = port;
    spec.rate_hz = 48000U;
    spec.format = DSD_IQ_NET_FORMAT_CS16;
    dsd_iq_net_reader_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    cfg.on_samples = on_samples;
    cfg.on_event = on_event;
    return dsd_iq_net_reader_create(&spec, &cfg);
}

/* A host that only looks like an address ("127.0.0.1.", which inet_pton() refuses) is a name the resolver is asked
   for: its lookup is held as any name's, and a stop during the open ends the open at once. Before, it skipped the
   helper as numeric, and the open waited for the resolver (here, until the watchdog let it go). */
static void
test_stop_during_a_held_open_of_a_dotted_name(void) {
    dsd_fake_resolver_reset(0x7F000001U, 0x7F000001U);
    dsd_iq_net_reader* r = tcp_reader_for("127.0.0.1.", 9);
    CHECK(r != NULL);
    if (!r) {
        return;
    }
    dsd_fake_resolver_hold(1);
    static open_job job;
    job.reader = r;
    job.rc = 0;
    atomic_store(&job.done, 0);
    dsd_thread_t opener;
    CHECK(dsd_thread_create(&opener, open_main, &job) == 0);
    CHECK(wait_for(one_held));
    dsd_thread_t watchdog;
    atomic_store(&g_watchdog_stop, 0);
    CHECK(dsd_thread_create(&watchdog, watchdog_main, NULL) == 0);

    const uint64_t t0 = dsd_realtime_mono_ms();
    dsd_iq_net_reader_stop(r);
    while (!atomic_load(&job.done) && dsd_realtime_mono_ms() - t0 < (uint64_t)k_wait_ms) {
        dsd_sleep_ms(2U);
    }
    const uint64_t took = dsd_realtime_mono_ms() - t0;
    if (took >= (uint64_t)k_prompt_ms) {
        DSD_FPRINTF(stderr, "the open took %llu ms after the stop while the lookup was held\n",
                    (unsigned long long)took);
    }
    CHECK(took < (uint64_t)k_prompt_ms);
    (void)dsd_thread_join(opener);
    CHECK(job.rc == -1);

    atomic_store(&g_watchdog_stop, 1);
    (void)dsd_thread_join(watchdog);
    dsd_fake_resolver_hold(0);
    CHECK(wait_for(none_held));
    dsd_iq_net_reader_destroy(r);
    dsd_sleep_ms(50U);
}

/* A real IPv4 address asks no resolver: it opens while every lookup is held. */
static void
test_numeric_address_opens_while_the_resolver_is_held(void) {
    dsd_socket_t listener = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    DSD_MEMSET(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = (socklen_t)sizeof(sa);
    if (listener == DSD_INVALID_SOCKET || dsd_socket_bind(listener, (struct sockaddr*)&sa, (int)sizeof(sa)) != 0
        || dsd_socket_listen(listener, 4) != 0 || getsockname(listener, (struct sockaddr*)&sa, &len) != 0) {
        CHECK(!"a loopback listener");
        if (listener != DSD_INVALID_SOCKET) {
            (void)dsd_socket_close(listener);
        }
        return;
    }
    dsd_fake_resolver_reset(0x7F000001U, 0x7F000001U);
    dsd_fake_resolver_hold(1);
    dsd_iq_net_reader* r = tcp_reader_for("127.0.0.1", (int)ntohs(sa.sin_port));
    dsd_iq_net_open_error err;
    CHECK(r && dsd_iq_net_reader_open(r, &err) == 0);
    CHECK(dsd_fake_resolver_held() == 0 && dsd_fake_resolver_lookups() == 0);
    dsd_fake_resolver_hold(0);
    dsd_iq_net_reader_destroy(r);
    (void)dsd_socket_close(listener);
}

int
main(void) {
    (void)dsd_socket_init();
    test_stop_during_a_held_reconnect_lookup();
    test_stop_during_a_held_open_of_a_dotted_name();
    test_numeric_address_opens_while_the_resolver_is_held();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
