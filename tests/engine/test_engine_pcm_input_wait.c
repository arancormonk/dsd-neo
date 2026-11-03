// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * A TCP PCM input that goes quiet (issue #667 D12, B7). The symbol path reads it in bounded slices, as it reads UDP: a
 * sender that is quiet longer than the socket's receive timeout (a squelched network audio source) keeps its
 * connection, and only a real close reconnects. A reconnect resets analog reception, which clears the stale deadline
 * and moves the boundaries epoch; that move under a running call or candidate is a wake of its own, so a hunt read on
 * a new connection that stays silent, or sends a part of a sample, gives way to the engine's tick, which ends the
 * reception the reconnect cut. Nothing is queued meanwhile. With no reception running the read waits on. Since a quiet
 * sender keeps its connection, keepalive is what finds one that vanished without closing (a half-open link): every
 * TCP audio connection, the startup Connect() and the switch's and reconnect's ConnectBounded(), runs with it.
 *
 * Real: getSymbol(), the TCP reader and connect (the engine's net-audio hooks), the engine's frame-sync hooks (the
 * decoder-ticks wake) and the analog call tracker. Simulated: the TCP server, a thread on a loopback socket.
 */
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/engine/analog_call.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../src/engine/engine_hooks_install.h"
#include "analog_call_test_support.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_env.h"

#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

/* The receive timeout the connections run with (DSD_NEO_RIGCTL_RCVTIMEO): the TCP reader used to take its expiry as
   a closed connection. */
enum { k_rcvtimeo_ms = 200, k_block = 960 };

static int
expect(int condition, const char* message) {
    if (!condition) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", message);
        return 1;
    }
    return 0;
}

/* --- The server: what each accepted connection does. --- */

typedef enum {
    SERVE_IDLE_THEN_SAMPLES, /* 4 samples, quiet for 6 receive timeouts, 4 more samples */
    SERVE_CLOSE,             /* closed at once: the client's read finds the connection's end */
    SERVE_SILENT,            /* nothing, held open */
    SERVE_ONE_BYTE,          /* one byte, half a sample, held open */
    SERVE_LATE_SAMPLE,       /* nothing for 1500 ms, then one sample, held open */
} serve_kind;

typedef struct {
    dsd_socket_t listener;
    int port;
    serve_kind script[4];
    atomic_int accepts;
    atomic_int stop;
    dsd_atomic_u64 resumed_ms; /* when SERVE_IDLE_THEN_SAMPLES ended its quiet spell */
} server;

/* A client that closed its end (the reader before the fix took a quiet sender for one) must not end the test. */
#ifdef MSG_NOSIGNAL
#define SERVE_SEND_FLAGS MSG_NOSIGNAL
#else
#define SERVE_SEND_FLAGS 0
#endif

static void
send_samples(dsd_socket_t sock, int count, int16_t value) {
    uint8_t bytes[2];
    bytes[0] = (uint8_t)((uint16_t)value & 0xFFU);
    bytes[1] = (uint8_t)((uint16_t)value >> 8);
    for (int i = 0; i < count; i++) {
        (void)dsd_socket_send(sock, bytes, sizeof bytes, SERVE_SEND_FLAGS);
    }
}

static void
serve_one(server* srv, dsd_socket_t conn, serve_kind kind) {
    switch (kind) {
        case SERVE_IDLE_THEN_SAMPLES:
            send_samples(conn, 4, 1000);
            dsd_sleep_ms(6U * k_rcvtimeo_ms);
            dsd_atomic_u64_store_relaxed(&srv->resumed_ms, dsd_realtime_mono_ms());
            send_samples(conn, 4, 2000);
            break;
        case SERVE_CLOSE: return;
        case SERVE_ONE_BYTE: (void)dsd_socket_send(conn, "\x10", 1, SERVE_SEND_FLAGS); break;
        case SERVE_LATE_SAMPLE:
            dsd_sleep_ms(1500U);
            send_samples(conn, 1, 3000);
            break;
        default: break;
    }
}

static DSD_THREAD_RETURN_TYPE
server_main(void* arg) {
    server* srv = (server*)arg;
    dsd_socket_t held[4];
    int n = 0;
    while (!atomic_load(&srv->stop)) {
        if (n >= 4 || dsd_socket_wait(srv->listener, DSD_SOCKET_WAIT_READ, 20U) <= 0) {
            dsd_sleep_ms(5U);
            continue;
        }
        const dsd_socket_t conn = dsd_socket_accept(srv->listener, NULL, NULL);
        if (conn == DSD_INVALID_SOCKET) {
            continue;
        }
        held[n] = conn;
        atomic_store(&srv->accepts, n + 1);
        serve_one(srv, conn, srv->script[n]);
        if (srv->script[n] == SERVE_CLOSE) {
            (void)dsd_socket_close(conn);
            held[n] = DSD_INVALID_SOCKET;
        }
        n++;
    }
    for (int i = 0; i < n; i++) {
        if (held[i] != DSD_INVALID_SOCKET) {
            (void)dsd_socket_close(held[i]);
        }
    }
    DSD_THREAD_RETURN;
}

static int
server_listen(server* srv) {
    srv->listener = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (srv->listener == DSD_INVALID_SOCKET) {
        return -1;
    }
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    struct sockaddr_in bound;
    DSD_MEMSET(&bound, 0, sizeof bound);
#if DSD_PLATFORM_WIN_NATIVE
    int bound_len = (int)sizeof bound;
#else
    socklen_t bound_len = (socklen_t)sizeof bound;
#endif
    if (dsd_socket_bind(srv->listener, (struct sockaddr*)&addr, sizeof addr) != 0
        || getsockname(srv->listener, (struct sockaddr*)&bound, &bound_len) != 0
        || dsd_socket_listen(srv->listener, 4) != 0) {
        (void)dsd_socket_close(srv->listener);
        return -1;
    }
    srv->port = (int)ntohs(bound.sin_port);
    return 0;
}

/* --- The session. --- */

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    server srv;
    dsd_thread_t thread;
    int serving; /* the server thread runs */
} session;

static int
session_open(session* s, serve_kind first, serve_kind second) {
    *s = (session){0};
    s->srv.listener = DSD_INVALID_SOCKET;
    s->opts = calloc(1, sizeof(*s->opts));
    s->state = calloc(1, sizeof(*s->state));
    if (!s->opts || !s->state) {
        return -1;
    }
    s->state->event_history_s = calloc(2, sizeof(*s->state->event_history_s));
    if (!s->state->event_history_s) {
        return -1;
    }
    init_event_history(&s->state->event_history_s[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&s->state->event_history_s[1], 0, DSD_EVENT_HISTORY_LEN);
    dsd_opts* opts = s->opts;
    opts->tcp_sockfd = DSD_INVALID_SOCKET;
    opts->audio_in_type = AUDIO_IN_TCP;
    opts->wav_sample_rate = 48000;
    opts->input_volume_multiplier = 1;
    /* The analog monitor, which the call tracker follows; a TCP input is no radio, so no squelch is needed (D2). */
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    s->state->samplesPerSymbol = 1;
    s->state->symbolCenter = 0;
    s->state->analog_rx.carrier_open = 1;
    s->state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_NONE;
    exitflag = 0;

    dsd_atomic_u64_init(&s->srv.resumed_ms, 0U);
    s->srv.script[0] = first;
    s->srv.script[1] = second;
    s->srv.script[2] = SERVE_SILENT;
    s->srv.script[3] = SERVE_SILENT;
    if (server_listen(&s->srv) != 0 || dsd_thread_create(&s->thread, server_main, &s->srv) != 0) {
        return -1;
    }
    s->serving = 1;
    DSD_SNPRINTF(opts->tcp_hostname, sizeof opts->tcp_hostname, "%s", "127.0.0.1");
    opts->tcp_portno = s->srv.port;
    opts->tcp_sockfd = ConnectBounded(opts->tcp_hostname, opts->tcp_portno, 1, NULL, NULL);
    if (opts->tcp_sockfd == DSD_INVALID_SOCKET) {
        return -1;
    }
    ConnectKeepTcpAudioAddress(opts->tcp_hostname, opts->tcp_portno);
    opts->tcp_in_ctx = dsd_net_audio_input_hook_tcp_open(opts->tcp_sockfd, opts->wav_sample_rate);
    return opts->tcp_in_ctx ? 0 : -1;
}

/* Also after a session_open() that failed part-way. */
static void
session_close(session* s) {
    if (s->serving) {
        atomic_store(&s->srv.stop, 1);
        (void)dsd_thread_join(s->thread);
    }
    if (s->srv.listener != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(s->srv.listener);
    }
    if (s->opts && s->opts->tcp_in_ctx) {
        dsd_net_audio_input_hook_tcp_close(s->opts->tcp_in_ctx);
    }
    if (s->opts && s->opts->tcp_sockfd != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(s->opts->tcp_sockfd);
    }
    if (s->state) {
        dsd_state_ext_free_all(s->state);
        free(s->state->event_history_s);
    }
    free(s->state);
    free(s->opts);
}

/* @p blocks RECEPTION blocks of the monitor: a candidate, or past DSD_ANALOG_CALL_MIN_MS a call. The received-tone
   tap reads the first, as the symbol path hands it each block, so the receiver's session runs: a reset moves its
   boundaries epoch. */
static void
receive_blocks(session* s, int blocks) {
    static float block[k_block];
    for (int i = 0; i < k_block; i++) {
        block[i] = 1000.0f;
    }
    dsd_analog_rx_tap(s->opts, s->state, block, k_block);
    for (int b = 0; b < blocks; b++) {
        dsd_test_analog_call_feed(s->opts, s->state, block, k_block, 48000, DSD_ANALOG_CALL_RECEPTION);
    }
}

/* B7: a sender quiet for six receive timeouts keeps its connection, and its samples after the pause are read on it.
   The read waits through the pause: nothing is queued or due. */
static int
test_an_idle_connection_is_kept(void) {
    session s;
    if (expect(session_open(&s, SERVE_IDLE_THEN_SAMPLES, SERVE_SILENT) == 0, "session opens")) {
        session_close(&s);
        return 1;
    }
    int rc = 0;
    s.opts->analog_only = 0;
    s.opts->monitor_input_audio = 0;
    uint64_t quiet_from_ms = 0U;
    for (int i = 0; i < 8 && rc == 0; i++) {
        (void)getSymbol(s.opts, s.state, 0);
        if (i == 3) {
            quiet_from_ms = dsd_realtime_mono_ms();
        }
    }
    /* How long the read sat in the quiet spell: from its fourth sample to the sender's resume. A loaded host may read
       the fourth sample late, so only two receive timeouts of the six are asked for. */
    const uint64_t resumed_ms = dsd_atomic_u64_load_relaxed(&s.srv.resumed_ms);
    const uint64_t waited_ms = resumed_ms > quiet_from_ms ? resumed_ms - quiet_from_ms : 0U;
    rc |= expect(atomic_load(&s.srv.accepts) == 1, "an idle connection is not reconnected");
    rc |= expect(s.state->symbolcnt == 8U, "every sample is read, on the one connection");
    rc |= expect(s.state->input_interrupted == 0 && s.state->input_fallback_pending == 0, "the input stays");
    rc |= expect(s.opts->tcp_in_ctx != NULL, "the connection is kept");
    rc |= expect(waited_ms >= (uint64_t)k_rcvtimeo_ms * 2U, "the read waited through receive timeouts");
    session_close(&s);
    return rc;
}

/* One integer socket option of @p sock, or -1 when it cannot be read. */
static int
sockopt_int(dsd_socket_t sock, int level, int name) {
    int value = 0;
    int len = (int)sizeof value;
    return dsd_socket_getsockopt(sock, level, name, &value, &len) == 0 ? value : -1;
}

/* Keepalive on @p sock with the half-open bound (DSD_SOCKET_KEEPALIVE_*), as far as the platform sets it per socket.
   Windows reports SO_KEEPALIVE only: its timing may come from an ioctl getsockopt() does not read back. */
static int
expect_keepalive(dsd_socket_t sock, const char* label) {
    int rc = expect(sockopt_int(sock, SOL_SOCKET, SO_KEEPALIVE) > 0, label);
#if !DSD_PLATFORM_WIN_NATIVE
#if defined(TCP_KEEPIDLE)
    rc |= expect(sockopt_int(sock, IPPROTO_TCP, TCP_KEEPIDLE) == (int)DSD_SOCKET_KEEPALIVE_IDLE_S, label);
#elif defined(TCP_KEEPALIVE)
    rc |= expect(sockopt_int(sock, IPPROTO_TCP, TCP_KEEPALIVE) == (int)DSD_SOCKET_KEEPALIVE_IDLE_S, label);
#endif
#if defined(TCP_KEEPINTVL)
    rc |= expect(sockopt_int(sock, IPPROTO_TCP, TCP_KEEPINTVL) == (int)DSD_SOCKET_KEEPALIVE_INTERVAL_S, label);
#endif
#if defined(TCP_KEEPCNT)
    rc |= expect(sockopt_int(sock, IPPROTO_TCP, TCP_KEEPCNT) == (int)DSD_SOCKET_KEEPALIVE_COUNT, label);
#endif
#endif
    return rc;
}

/* A quiet sender keeps its connection (B7), so a sender that vanished without a FIN or RST is found only by
   keepalive, which then fails the read and the reconnect runs. A real half-open peer cannot be made on loopback: the
   options are what is checked, on the switch's (and reconnect's) connection and on the startup one. */
static int
test_connections_run_with_keepalive(void) {
    session s;
    if (expect(session_open(&s, SERVE_SILENT, SERVE_SILENT) == 0, "session opens")) {
        session_close(&s);
        return 1;
    }
    int rc = expect_keepalive(s.opts->tcp_sockfd, "a bounded connect (switch, reconnect) runs with keepalive");
    char host[] = "127.0.0.1";
    const dsd_socket_t startup = Connect(host, s.opts->tcp_portno);
    rc |= expect(startup != DSD_INVALID_SOCKET, "the startup connect connects");
    if (startup != DSD_INVALID_SOCKET) {
        rc |= expect_keepalive(startup, "the startup connect runs with keepalive");
        (void)dsd_socket_close(startup);
    }
    session_close(&s);
    return rc;
}

/* The connection ends under a running reception (a call after @p blocks RECEPTION blocks, or a candidate), and the
   new one stays silent or sends half a sample: the hunt read leaves for the engine's tick with nothing queued, the
   reconnect's reset having cleared the stale deadline, and the tick ends the reception. */
static int
run_reconnect_ends_the_reception(serve_kind second, int blocks, const char* label) {
    session s;
    if (expect(session_open(&s, SERVE_CLOSE, second) == 0, label)) {
        session_close(&s);
        return 1;
    }
    int rc = 0;
    /* The first connection's close is already on the client socket before the reception starts, so the read below
       meets it at once, well inside the running reception's stale deadline, however the server thread ran. */
    const uint64_t ready_by_ms = dsd_realtime_mono_ms() + 5000U;
    while ((dsd_socket_wait(s.opts->tcp_sockfd, DSD_SOCKET_WAIT_READ, 10U) <= 0 || atomic_load(&s.srv.accepts) < 1)
           && dsd_realtime_mono_ms() < ready_by_ms) {
        dsd_sleep_ms(1U);
    }
    rc |= expect(atomic_load(&s.srv.accepts) >= 1, "the server accepted and closed the first connection");
    receive_blocks(&s, blocks);
    rc |= expect(dsd_engine_analog_call_active(s.state) == (blocks * k_block >= DSD_ANALOG_CALL_MIN_MS * 48),
                 "the reception runs");
    const uint32_t boundaries = s.state->analog_rx.boundaries;
    const uint64_t start_ms = dsd_realtime_mono_ms();
    (void)getSymbol(s.opts, s.state, 0);
    const uint64_t waited_ms = dsd_realtime_mono_ms() - start_ms;
    rc |= expect(atomic_load(&s.srv.accepts) == 2, "the closed connection is reconnected once");
    rc |= expect(s.state->analog_rx.boundaries != boundaries && s.state->analog_rx.stale_after_ms == 0U,
                 "the reconnect reset reception");
    rc |= expect(s.state->input_interrupted == 1, "the hunt read left for the tick");
    rc |= expect(s.state->input_fallback_pending == 0 && s.opts->tcp_in_ctx != NULL, "the new connection is kept");
    rc |= expect(waited_ms >= DSD_ANALOG_STREAM_PAUSE_MIN_MS && waited_ms < 5000U, "left after a stream pause");
    rc |= expect(dsd_engine_analog_call_due(s.state) == 1, "the moved epoch is due");
    dsd_engine_analog_call_tick(s.opts, s.state);
    rc |= expect(dsd_engine_analog_call_due(s.state) == 0 && !dsd_engine_analog_call_active(s.state),
                 "the tick ended the reception");
    if (rc) {
        DSD_FPRINTF(stderr, "  in: %s\n", label);
    }
    session_close(&s);
    return rc;
}

/* With no reception running, a silent new connection is waited on: the read comes back with its late sample. */
static int
test_a_reconnect_with_no_reception_waits(void) {
    session s;
    if (expect(session_open(&s, SERVE_CLOSE, SERVE_LATE_SAMPLE) == 0, "session opens")) {
        session_close(&s);
        return 1;
    }
    int rc = 0;
    (void)getSymbol(s.opts, s.state, 0);
    rc |= expect(atomic_load(&s.srv.accepts) == 2, "reconnected once");
    rc |= expect(s.state->input_interrupted == 0 && s.state->symbolcnt == 1U, "the late sample was read");
    session_close(&s);
    return rc;
}

int
main(void) {
    if (dsd_test_setenv("DSD_NEO_RIGCTL_RCVTIMEO", "200", 1) != 0 || dsd_socket_init() != 0) {
        return 1;
    }
    dsd_engine_net_audio_input_hooks_install();
    dsd_engine_frame_sync_hooks_install();
    int rc = 0;
    rc |= test_an_idle_connection_is_kept();
    rc |= test_connections_run_with_keepalive();
    rc |= run_reconnect_ends_the_reception(SERVE_SILENT, 30, "a call, reconnected to silence");
    rc |= run_reconnect_ends_the_reception(SERVE_ONE_BYTE, 30, "a call, reconnected to one byte");
    rc |= run_reconnect_ends_the_reception(SERVE_SILENT, 5, "a candidate, reconnected to silence");
    rc |= run_reconnect_ends_the_reception(SERVE_ONE_BYTE, 5, "a candidate, reconnected to one byte");
    rc |= test_a_reconnect_with_no_reception_waits();
    dsd_frame_sync_hooks_set(NULL);
    dsd_net_audio_input_hooks_set((dsd_net_audio_input_hooks){0});
    dsd_socket_cleanup();
    if (rc == 0) {
        printf("ENGINE_PCM_INPUT_WAIT: OK\n");
    }
    return rc;
}
