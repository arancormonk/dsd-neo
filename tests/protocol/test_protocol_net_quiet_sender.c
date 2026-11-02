// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * EDACS analog voice and the M17 stream encoder over TCP and UDP, with a sender that goes quiet for longer than the
 * socket's 1.5 s receive timeout (a squelched network audio source sends nothing), then sends again (issue #667 D12,
 * as B7). Over TCP both used to take the expired timeout for a closed connection and shut DSD-neo down; over UDP EDACS
 * waited for the sender forever. The quiet spell keeps the input: once it lasts a stream pause EDACS plays it as
 * zero-power silence, so its own squelch count and watchdog end the call, and the encoder waits on. A stall shorter
 * than that pause inserts nothing. A real close or stop still ends the input and shuts down, as before. The M17
 * encoder's stdin read gives way to shutdown on a silent pipe.
 *
 * Real: the TCP and UDP readers and the TCP connect (the engine's net-audio hooks, ConnectBounded() at the default
 * receive timeout), the EDACS triplet collector and the M17 encoder's block reader. Simulated: the senders, a loopback
 * thread.
 */
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/pcm_stream.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "../../src/engine/engine_hooks_install.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "edacs_internal.h"
#include "m17_internal.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <fcntl.h>
#include <io.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* m17_encoder_read_block()'s stop (k_m17_read_stop, private to m17.c): the encoder ends. */
enum { k_m17_read_stop = 0 };

/* Longer than the 1.5 s receive timeout every TCP audio connection runs with (ConnectBounded()). */
enum { k_quiet_ms = 1800, k_first = 1000, k_after = 2000 };

/* A network stall shorter than a stream pause (DSD_ANALOG_STREAM_PAUSE_MIN_MS) while the sender still transmits. */
enum { k_stall_ms = 300 };

#ifdef MSG_NOSIGNAL
#define SERVE_SEND_FLAGS MSG_NOSIGNAL
#else
#define SERVE_SEND_FLAGS 0
#endif

static int
expect(int condition, const char* message) {
    if (!condition) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", message);
        return 1;
    }
    return 0;
}

/* --- The server: one connection, @p count samples of k_first, then quiet and k_after, or a close. --- */

typedef struct {
    dsd_socket_t listener;
    int port;
    int count;
    int after;       /* samples of k_after once the quiet spell ends */
    int quiet_ms;    /* how long the sender is quiet */
    int close_after; /* close once the first samples are out, instead of the quiet spell */
    atomic_int stop;
} server;

static void
send_samples(dsd_socket_t sock, int count, int16_t value) {
    uint8_t bytes[2];
    bytes[0] = (uint8_t)((uint16_t)value & 0xFFU);
    bytes[1] = (uint8_t)((uint16_t)value >> 8);
    for (int i = 0; i < count; i++) {
        (void)dsd_socket_send(sock, bytes, sizeof bytes, SERVE_SEND_FLAGS);
    }
}

static DSD_THREAD_RETURN_TYPE
server_main(void* arg) {
    server* srv = (server*)arg;
    dsd_socket_t conn = DSD_INVALID_SOCKET;
    while (conn == DSD_INVALID_SOCKET && !atomic_load(&srv->stop)) {
        if (dsd_socket_wait(srv->listener, DSD_SOCKET_WAIT_READ, 20U) > 0) {
            conn = dsd_socket_accept(srv->listener, NULL, NULL);
        }
    }
    if (conn != DSD_INVALID_SOCKET) {
        send_samples(conn, srv->count, k_first);
        if (srv->close_after) {
            (void)dsd_socket_close(conn);
            conn = DSD_INVALID_SOCKET;
        } else {
            dsd_sleep_ms((unsigned int)srv->quiet_ms);
            send_samples(conn, srv->after, k_after);
        }
    }
    while (!atomic_load(&srv->stop)) {
        dsd_sleep_ms(5U);
    }
    if (conn != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(conn);
    }
    DSD_THREAD_RETURN;
}

/* The decoder objects the session runs, in static storage. */
static dsd_opts g_opts;
static dsd_state g_state;

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    server srv;
    dsd_thread_t thread;
    int serving;
} session;

static session g_session;

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
    struct sockaddr_in bound;
    DSD_MEMSET(&bound, 0, sizeof bound);
#if DSD_PLATFORM_WIN_NATIVE
    int bound_len = (int)sizeof bound;
#else
    socklen_t bound_len = (socklen_t)sizeof bound;
#endif
    if (dsd_socket_bind(srv->listener, (struct sockaddr*)&addr, sizeof addr) != 0
        || getsockname(srv->listener, (struct sockaddr*)&bound, &bound_len) != 0
        || dsd_socket_listen(srv->listener, 1) != 0) {
        return -1;
    }
    srv->port = (int)ntohs(bound.sin_port);
    return 0;
}

/* A TCP input on a connection to the server, at 48 kHz and unit volume. */
static int
session_open_paced(session* s, int count, int close_after, int after, int quiet_ms) {
    DSD_MEMSET(s, 0, sizeof(*s));
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    s->opts = &g_opts;
    s->state = &g_state;
    s->srv.listener = DSD_INVALID_SOCKET;
    s->srv.count = count;
    s->srv.close_after = close_after;
    s->srv.after = after;
    s->srv.quiet_ms = quiet_ms;
    s->opts->tcp_sockfd = DSD_INVALID_SOCKET;
    s->opts->audio_in_type = AUDIO_IN_TCP;
    s->opts->wav_sample_rate = 48000;
    s->opts->input_volume_multiplier = 1;
    exitflag = 0;
    if (server_listen(&s->srv) != 0 || dsd_thread_create(&s->thread, server_main, &s->srv) != 0) {
        return -1;
    }
    s->serving = 1;
    s->opts->tcp_sockfd = ConnectBounded("127.0.0.1", s->srv.port, 1, NULL, NULL);
    if (s->opts->tcp_sockfd == DSD_INVALID_SOCKET) {
        return -1;
    }
    s->opts->tcp_in_ctx = dsd_net_audio_input_hook_tcp_open(s->opts->tcp_sockfd, 48000);
    return s->opts->tcp_in_ctx ? 0 : -1;
}

/* The usual session: as many samples after the quiet spell as before it, quiet for k_quiet_ms. */
static int
session_open(session* s, int count, int close_after) {
    return session_open_paced(s, count, close_after, count, k_quiet_ms);
}

static void
session_close(session* s) {
    if (s->serving) {
        atomic_store(&s->srv.stop, 1);
        (void)dsd_thread_join(s->thread);
    }
    if (s->srv.listener != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(s->srv.listener);
    }
    if (s->opts->tcp_in_ctx) {
        dsd_net_audio_input_hook_tcp_close(s->opts->tcp_in_ctx);
    }
    if (s->opts->tcp_sockfd != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(s->opts->tcp_sockfd);
    }
    exitflag = 0;
}

static int
block_holds(const short* block, size_t count, short value) {
    for (size_t i = 0; i < count; i++) {
        if (block[i] == value) {
            return 1;
        }
    }
    return 0;
}

/* EDACS analog voice: the quiet spell comes out as silence, and the sender's samples after it are read on the same
   connection. Nothing shuts down. */
static int
test_edacs_quiet_sender(void) {
    session* s = &g_session;
    if (expect(session_open(s, 960, 0) == 0, "EDACS: session opens")) {
        session_close(s);
        return 1;
    }
    static short a1[960];
    static short a2[960];
    static short a3[960];
    int rc = 0;
    int silence = 0;
    int after = 0;
    const uint64_t deadline_ms = dsd_realtime_mono_ms() + 10000U;
    while (!after && dsd_realtime_mono_ms() < deadline_ms) {
        double pwr = -1.0;
        if (!edacs_collect_analog_triplet(s->opts, s->state, NULL, a1, a2, a3, &pwr)) {
            break;
        }
        silence |= block_holds(a3, 960U, 0) && pwr < 1.0;
        after = block_holds(a1, 960U, k_after) || block_holds(a2, 960U, k_after) || block_holds(a3, 960U, k_after);
    }
    rc |= expect(after, "EDACS: the samples after the quiet spell are read");
    rc |= expect(silence, "EDACS: the quiet spell plays as silence with no power");
    rc |= expect(dsd_exitflag_load() == 0 && s->opts->tcp_in_ctx != NULL, "EDACS: a quiet sender keeps the input");
    session_close(s);
    return rc;
}

/* EDACS analog voice: a stall shorter than a stream pause inserts no silence. The sender's samples on either side of it
   make up one triplet, so a level-squelched call goes on as it did before the bounded reads. */
static int
test_edacs_short_stall(void) {
    session* s = &g_session;
    if (expect(session_open_paced(s, 960, 0, 1920, k_stall_ms) == 0, "EDACS stall: session opens")) {
        session_close(s);
        return 1;
    }
    static short a1[960];
    static short a2[960];
    static short a3[960];
    double pwr = -1.0;
    int rc = expect(edacs_collect_analog_triplet(s->opts, s->state, NULL, a1, a2, a3, &pwr) == 1,
                    "EDACS stall: the triplet across the stall is read");
    rc |= expect(!block_holds(a1, 960U, 0) && !block_holds(a2, 960U, 0) && !block_holds(a3, 960U, 0),
                 "EDACS stall: no silence was inserted");
    rc |= expect(a1[959] == k_first && a2[0] == k_after && a3[959] == k_after, "EDACS stall: the samples in order");
    rc |= expect(dsd_exitflag_load() == 0 && s->opts->tcp_in_ctx != NULL, "EDACS stall: the input stays");
    session_close(s);
    return rc;
}

/* EDACS analog voice: a real close still closes the input and shuts down. */
static int
test_edacs_close(void) {
    session* s = &g_session;
    if (expect(session_open(s, 960, 1) == 0, "EDACS close: session opens")) {
        session_close(s);
        return 1;
    }
    static short a1[960];
    static short a2[960];
    static short a3[960];
    double pwr = -1.0;
    int rc = expect(edacs_collect_analog_triplet(s->opts, s->state, NULL, a1, a2, a3, &pwr) == 0,
                    "EDACS close: the triplet the close cut is refused");
    rc |= expect(dsd_exitflag_load() == 1 && s->opts->tcp_in_ctx == NULL, "EDACS close: closed and shut down");
    session_close(s);
    return rc;
}

/* --- UDP: a sender thread on a loopback port the UDP input listens on. --- */

typedef struct {
    dsd_socket_t sock;
    struct sockaddr_in to;
} udp_sender;

static void
udp_send_samples(const udp_sender* sender, int count, int16_t value) {
    uint8_t bytes[1920];
    for (size_t i = 0; i < (size_t)count && i < 960U; i++) {
        bytes[2U * i] = (uint8_t)((uint16_t)value & 0xFFU);
        bytes[(2U * i) + 1U] = (uint8_t)((uint16_t)value >> 8);
    }
    (void)dsd_socket_sendto(sender->sock, bytes, (size_t)count * 2U, 0, (const struct sockaddr*)&sender->to,
                            (int)sizeof sender->to);
}

/* 960 samples of k_first, quiet for k_quiet_ms, then 960 of k_after. */
static DSD_THREAD_RETURN_TYPE
udp_sender_main(void* arg) {
    const udp_sender* sender = (const udp_sender*)arg;
    udp_send_samples(sender, 960, k_first);
    dsd_sleep_ms(k_quiet_ms);
    udp_send_samples(sender, 960, k_after);
    DSD_THREAD_RETURN;
}

/* A loopback UDP port nothing holds: bound by the system and let go again. */
static int
free_udp_port(void) {
    const dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#if DSD_PLATFORM_WIN_NATIVE
    int len = (int)sizeof addr;
#else
    socklen_t len = (socklen_t)sizeof addr;
#endif
    int port = -1;
    if (sock != DSD_INVALID_SOCKET && dsd_socket_bind(sock, (struct sockaddr*)&addr, sizeof addr) == 0
        && getsockname(sock, (struct sockaddr*)&addr, &len) == 0) {
        port = (int)ntohs(addr.sin_port);
    }
    if (sock != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(sock);
    }
    return port;
}

/* A UDP input listening on a free loopback port, and a sender aimed at it. */
static int
udp_session_open(udp_sender* sender) {
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    g_opts.audio_in_type = AUDIO_IN_UDP;
    g_opts.wav_sample_rate = 48000;
    g_opts.input_volume_multiplier = 1;
    g_opts.tcp_sockfd = DSD_INVALID_SOCKET;
    exitflag = 0;
    const int port = free_udp_port();
    sender->sock = dsd_socket_create(AF_INET, SOCK_DGRAM, 0);
    DSD_MEMSET(&sender->to, 0, sizeof sender->to);
    sender->to.sin_family = AF_INET;
    sender->to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sender->to.sin_port = htons((uint16_t)port);
    if (port <= 0 || sender->sock == DSD_INVALID_SOCKET) {
        return -1;
    }
    return dsd_net_audio_input_hook_udp_start(&g_opts, "127.0.0.1", port, 48000) == 0 ? 0 : -1;
}

/* EDACS analog voice over UDP: a sender quiet for a stream pause and more plays as silence, so the call's own timers
   can end it, and the sender's samples after it are read. It used to wait for the sender forever. */
static int
test_edacs_udp_quiet_sender(void) {
    udp_sender sender;
    if (expect(udp_session_open(&sender) == 0, "EDACS UDP: input starts")) {
        return 1;
    }
    dsd_thread_t thread;
    int rc = expect(dsd_thread_create(&thread, udp_sender_main, &sender) == 0, "EDACS UDP: sender thread");
    static short a1[960];
    static short a2[960];
    static short a3[960];
    int silence = 0;
    int after = 0;
    const uint64_t deadline_ms = dsd_realtime_mono_ms() + 10000U;
    while (!after && dsd_realtime_mono_ms() < deadline_ms) {
        double pwr = -1.0;
        if (!edacs_collect_analog_triplet(&g_opts, &g_state, NULL, a1, a2, a3, &pwr)) {
            break;
        }
        silence |= block_holds(a3, 960U, 0) && pwr < 1.0;
        after = block_holds(a1, 960U, k_after) || block_holds(a2, 960U, k_after) || block_holds(a3, 960U, k_after);
    }
    (void)dsd_thread_join(thread);
    rc |= expect(silence, "EDACS UDP: the quiet spell plays as silence with no power");
    rc |= expect(after, "EDACS UDP: the samples after the quiet spell are read");
    rc |= expect(dsd_exitflag_load() == 0, "EDACS UDP: a quiet sender keeps the input");

    /* A stopped input still ends the call and asks for shutdown, as before. */
    dsd_net_audio_input_hook_udp_stop(&g_opts);
    double pwr = -1.0;
    rc |= expect(edacs_collect_analog_triplet(&g_opts, &g_state, NULL, a1, a2, a3, &pwr) == 0,
                 "EDACS UDP stop: the triplet is refused");
    rc |= expect(dsd_exitflag_load() == 1, "EDACS UDP stop: shut down");
    (void)dsd_socket_close(sender.sock);
    exitflag = 0;
    return rc;
}

/* The M17 encoder: one block before the quiet spell, the next one after it, on the same connection. */
static int
test_m17_quiet_sender(void) {
    session* s = &g_session;
    if (expect(session_open(s, 960, 0) == 0, "M17: session opens")) {
        session_close(s);
        return 1;
    }
    m17_encoder_input in;
    m17_encoder_input_init(&in, 48000);
    short out[160];
    int rc = expect(m17_encoder_read_block(s->opts, s->state, &in, out, 160U) == 1, "M17: the first block");
    rc |= expect(m17_encoder_read_block(s->opts, s->state, &in, out, 160U) == 1, "M17: the block after the quiet");
    rc |= expect(dsd_exitflag_load() == 0 && s->opts->tcp_in_ctx != NULL, "M17: a quiet sender keeps the input");
    session_close(s);
    return rc;
}

/* The M17 encoder: a real close still closes the input and shuts down. */
static int
test_m17_close(void) {
    session* s = &g_session;
    if (expect(session_open(s, 960, 1) == 0, "M17 close: session opens")) {
        session_close(s);
        return 1;
    }
    m17_encoder_input in;
    m17_encoder_input_init(&in, 48000);
    short out[160];
    int rc = expect(m17_encoder_read_block(s->opts, s->state, &in, out, 160U) == 1, "M17 close: the first block");
    rc |= expect(m17_encoder_read_block(s->opts, s->state, &in, out, 160U) == k_m17_read_stop,
                 "M17 close: the close stops the encoder");
    rc |= expect(dsd_exitflag_load() == 1 && s->opts->tcp_in_ctx == NULL, "M17 close: closed and shut down");
    session_close(s);
    return rc;
}

/* --- The M17 encoder on a silent stdin pipe: shutdown, requested from another thread, ends the wait. --- */

static DSD_THREAD_RETURN_TYPE
request_shutdown_later(void* arg) {
    (void)arg;
    dsd_sleep_ms(300U);
    dsd_exitflag_store(1);
    DSD_THREAD_RETURN;
}

static int
make_pipe(int fds[2]) {
#if DSD_PLATFORM_WIN_NATIVE
    return _pipe(fds, 65536U, _O_BINARY);
#else
    return pipe(fds);
#endif
}

static int
test_m17_stdin_shutdown(void) {
    int fds[2] = {-1, -1};
    if (expect(make_pipe(fds) == 0, "M17 stdin: pipe")) {
        return 1;
    }
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_STDIN;
    opts.input_volume_multiplier = 1;
    opts.audio_in_stdin = dsd_pcm_stream_open_fd(fds[0]);
    exitflag = 0;
    m17_encoder_input in;
    m17_encoder_input_init(&in, 8000);
    short out[160];
    dsd_thread_t thread;
    int rc = expect(dsd_thread_create(&thread, request_shutdown_later, NULL) == 0, "M17 stdin: thread");
    const int got = m17_encoder_read_block(&opts, &state, &in, out, 160U);
    (void)dsd_thread_join(thread);
    rc |= expect(got == k_m17_read_stop, "M17 stdin: shutdown ends the silent wait");
    rc |= expect(opts.audio_in_stdin != NULL, "M17 stdin: an open pipe is no end of input");
    dsd_pcm_stream_close(opts.audio_in_stdin);
    (void)dsd_close(fds[1]);
    (void)dsd_close(fds[0]);
    exitflag = 0;
    return rc;
}

int
main(void) {
    if (dsd_socket_init() != 0) {
        return 1;
    }
    dsd_engine_net_audio_input_hooks_install();
    int rc = 0;
    rc |= test_edacs_quiet_sender();
    rc |= test_edacs_close();
    rc |= test_edacs_short_stall();
    rc |= test_edacs_udp_quiet_sender();
    rc |= test_m17_quiet_sender();
    rc |= test_m17_close();
    rc |= test_m17_stdin_shutdown();
    dsd_net_audio_input_hooks_set((dsd_net_audio_input_hooks){0});
    dsd_socket_cleanup();
    if (rc == 0) {
        printf("PROTOCOL_NET_QUIET_SENDER: OK\n");
    }
    return rc;
}
