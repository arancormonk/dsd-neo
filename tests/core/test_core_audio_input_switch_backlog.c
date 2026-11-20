// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #834: an input switch to UDP that fails keeps the running input, and with it the input a rigctl retune's drop is
 * measured on. The UDP input's queue registers with the input backlog before its receive thread starts; when that
 * thread does not start (as on a host out of threads), the TCP or stdin reader the switch keeps must be the measured
 * input again: a retune marked after the failed switch drops the reader's queued old audio, and so does one marked
 * before it. The real TCP, stdin and UDP backends run here; udp_input.c is built into this test with its thread-start
 * seam (DSD_NEO_TEST_HOOKS, udp_input_test_fail_thread_starts()).
 */

#include <arpa/inet.h>
#include <dsd-neo/core/audio_input_switch.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/io/tcp_input.h>
#include <dsd-neo/io/udp_input.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/input_backlog.h>
#include <dsd-neo/platform/pcm_stream.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "udp_input_test_support.h"

static int g_failures;

static void
check(int condition, const char* what) {
    if (!condition) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

static dsd_opts g_opts;
static dsd_state g_state;

static void
session_init(void) {
    DSD_MEMSET(&g_opts, 0, sizeof g_opts);
    DSD_MEMSET(&g_state, 0, sizeof g_state);
    g_opts.wav_sample_rate = 48000;
    g_opts.wav_decimator = 48000;
    g_opts.wav_interpolator = 1;
    g_opts.pulse_digi_rate_in = 48000;
    g_opts.tcp_sockfd = DSD_INVALID_SOCKET;
    g_opts.udp_in_sockfd = DSD_INVALID_SOCKET;
    g_state.samplesPerSymbol = 10;
    g_state.symbolCenter = 4;
    g_state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    dsd_input_backlog_retire();
}

/* A loopback UDP port nothing is bound to (bound and released here). */
static int
free_udp_port(void) {
    dsd_socket_t s = dsd_socket_create(AF_INET, SOCK_DGRAM, 0);
    if (s == DSD_INVALID_SOCKET) {
        return -1;
    }
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = (socklen_t)sizeof addr;
    int port = -1;
    if (dsd_socket_bind(s, (struct sockaddr*)&addr, sizeof addr) == 0
        && getsockname(s, (struct sockaddr*)&addr, &len) == 0) {
        port = (int)ntohs(addr.sin_port);
    }
    (void)dsd_socket_close(s);
    return port;
}

/* Switch to UDP on a free loopback port while the receive thread cannot start: the switch keeps the running input. */
static void
switch_to_udp_without_threads(const char* what) {
    dsd_audio_input_request req;
    DSD_MEMSET(&req, 0, sizeof req);
    req.kind = DSD_AUDIO_INPUT_UDP;
    req.host = "127.0.0.1";
    req.port = free_udp_port();
    req.tcp_sockfd = DSD_INVALID_SOCKET;
    check(req.port > 0, "a free UDP port");
    udp_input_test_fail_thread_starts(1);
    const int rc = dsd_audio_switch_input(&g_opts, &g_state, &req);
    char label[160];
    DSD_SNPRINTF(label, sizeof label, "%s: a UDP switch whose receive thread does not start keeps the input (rc %d)",
                 what, rc);
    check(rc == DSD_AUDIO_INPUT_KEPT && g_opts.udp_in_ctx == NULL, label);
}

typedef struct {
    dsd_socket_t listener;
    dsd_socket_t client;
    dsd_socket_t server;
} socket_pair;

static int
socket_pair_open(socket_pair* pair) {
    pair->listener = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    pair->client = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    pair->server = DSD_INVALID_SOCKET;
    if (pair->listener == DSD_INVALID_SOCKET || pair->client == DSD_INVALID_SOCKET) {
        return -1;
    }
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct sockaddr_in bound;
    DSD_MEMSET(&bound, 0, sizeof bound);
    socklen_t bound_len = (socklen_t)sizeof bound;
    if (dsd_socket_bind(pair->listener, (struct sockaddr*)&addr, sizeof addr) != 0
        || getsockname(pair->listener, (struct sockaddr*)&bound, &bound_len) != 0
        || dsd_socket_listen(pair->listener, 1) != 0
        || dsd_socket_connect(pair->client, (struct sockaddr*)&bound, sizeof bound) != 0) {
        return -1;
    }
    pair->server = dsd_socket_accept(pair->listener, NULL, NULL);
    return pair->server == DSD_INVALID_SOCKET ? -1 : 0;
}

static void
socket_pair_close(const socket_pair* pair) {
    if (pair->server != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(pair->server);
    }
    (void)dsd_socket_close(pair->client);
    (void)dsd_socket_close(pair->listener);
}

/* Wait until @p sock holds @p bytes ready (they were sent already; the bound only turns a lost send into a failure). */
static int
socket_holds(dsd_socket_t sock, long bytes) {
    for (int tries = 0; tries < 1000; tries++) {
        if (dsd_socket_readable_bytes(sock) >= bytes) {
            return 1;
        }
        dsd_sleep_ms(10U);
    }
    return 0;
}

static const uint8_t k_old_audio[] = {0x01U, 0x00U, 0x02U, 0x00U}; /* two samples of the channel left */
static const uint8_t k_new_audio[] = {0x03U, 0x00U};               /* one sample of the channel tuned to */

/* A retune marked now, after the failed switch: measured on the kept reader, and its drop takes the old audio. */
static void
check_mark_after(const char* what, uint64_t queued) {
    dsd_input_cut cut;
    DSD_MEMSET(&cut, 0, sizeof cut);
    dsd_input_backlog_mark(&cut);
    char label[192];
    DSD_SNPRINTF(label, sizeof label,
                 "%s: a retune marked after the failed switch is measured on the kept reader (measured %u, %llu bytes, "
                 "expected %llu)",
                 what, (unsigned)cut.measured, (unsigned long long)cut.arrived, (unsigned long long)queued);
    check(cut.measured == 1U && cut.arrived == queued, label);
}

static void
test_tcp_reader_kept(void) {
    session_init();
    socket_pair pair;
    check(socket_pair_open(&pair) == 0, "tcp: loopback connection");
    check(dsd_socket_send(pair.server, k_old_audio, sizeof k_old_audio, 0) == (int)sizeof k_old_audio,
          "tcp: send old audio");
    check(socket_holds(pair.client, (long)sizeof k_old_audio), "tcp: it arrived");
    g_opts.tcp_in_ctx = tcp_input_open(pair.client, 48000);
    g_opts.tcp_sockfd = pair.client;
    g_opts.audio_in_type = AUDIO_IN_TCP;
    check(g_opts.tcp_in_ctx != NULL, "tcp: reader open");
    dsd_input_cut before;
    dsd_input_backlog_mark(&before);

    switch_to_udp_without_threads("tcp");
    check(g_opts.audio_in_type == AUDIO_IN_TCP && g_opts.tcp_sockfd == pair.client, "tcp: the TCP input runs on");
    check_mark_after("tcp", sizeof k_old_audio);
    check(tcp_input_discard_through(g_opts.tcp_in_ctx, &before) == 2,
          "tcp: a retune marked before the failed switch still drops the old audio");
    check(dsd_socket_send(pair.server, k_new_audio, sizeof k_new_audio, 0) == (int)sizeof k_new_audio,
          "tcp: send new audio");
    int16_t s = 0;
    check(tcp_input_read_sample_wait(g_opts.tcp_in_ctx, &s, 1000U) == 1 && s == 3, "tcp: and the new audio is read");
    tcp_input_close(g_opts.tcp_in_ctx);
    g_opts.tcp_in_ctx = NULL;
    socket_pair_close(&pair);
}

static void
test_tcp_reader_kept_marked_after(void) {
    session_init();
    socket_pair pair;
    check(socket_pair_open(&pair) == 0, "tcp after: loopback connection");
    check(dsd_socket_send(pair.server, k_old_audio, sizeof k_old_audio, 0) == (int)sizeof k_old_audio,
          "tcp after: send old audio");
    check(socket_holds(pair.client, (long)sizeof k_old_audio), "tcp after: it arrived");
    g_opts.tcp_in_ctx = tcp_input_open(pair.client, 48000);
    g_opts.tcp_sockfd = pair.client;
    g_opts.audio_in_type = AUDIO_IN_TCP;

    switch_to_udp_without_threads("tcp after");
    dsd_input_cut cut;
    dsd_input_backlog_mark(&cut);
    check(tcp_input_discard_through(g_opts.tcp_in_ctx, &cut) == 2,
          "tcp after: a retune marked after the failed switch drops the old audio");
    tcp_input_close(g_opts.tcp_in_ctx);
    g_opts.tcp_in_ctx = NULL;
    socket_pair_close(&pair);
}

static void
test_stdin_reader_kept(void) {
    session_init();
    int fds[2];
    check(pipe(fds) == 0, "stdin: pipe");
    check(dsd_write(fds[1], k_old_audio, sizeof k_old_audio) == (ssize_t)sizeof k_old_audio, "stdin: old audio");
    g_opts.audio_in_stdin = dsd_pcm_stream_open_fd(fds[0]);
    g_opts.audio_in_type = AUDIO_IN_STDIN;
    check(g_opts.audio_in_stdin != NULL, "stdin: reader open");
    dsd_input_cut before;
    dsd_input_backlog_mark(&before);

    switch_to_udp_without_threads("stdin");
    check(g_opts.audio_in_type == AUDIO_IN_STDIN && g_opts.audio_in_stdin != NULL, "stdin: the stdin input runs on");
    check_mark_after("stdin", sizeof k_old_audio);
    dsd_input_cut after;
    dsd_input_backlog_mark(&after);
    check(dsd_pcm_stream_discard_through(g_opts.audio_in_stdin, &after) == 2,
          "stdin: a retune marked after the failed switch drops the old audio");
    check(dsd_write(fds[1], k_new_audio, sizeof k_new_audio) == (ssize_t)sizeof k_new_audio, "stdin: new audio");
    int16_t s = 0;
    check(dsd_pcm_stream_read_wait(g_opts.audio_in_stdin, &s, 1000U) == DSD_PCM_STREAM_SAMPLE && s == 3,
          "stdin: and the new audio is read");
    check(dsd_pcm_stream_discard_through(g_opts.audio_in_stdin, &before) == DSD_INPUT_DISCARD_READ_PAST,
          "stdin: the earlier mark's audio is gone already: the reader read past it");
    dsd_pcm_stream_close(g_opts.audio_in_stdin);
    g_opts.audio_in_stdin = NULL;
    (void)close(fds[0]);
    (void)close(fds[1]);
}

/* A UDP input that runs, then a switch to another UDP endpoint whose thread does not start: the old one is started
   again on its own endpoint and is the measured input. */
static void
test_udp_restarted(void) {
    session_init();
    const int port = free_udp_port();
    check(port > 0 && udp_input_start(&g_opts, "127.0.0.1", port, 48000) == 0, "udp: the first UDP input runs");
    g_opts.audio_in_type = AUDIO_IN_UDP;
    DSD_SNPRINTF(g_opts.udp_in_bindaddr, sizeof g_opts.udp_in_bindaddr, "%s", "127.0.0.1");
    g_opts.udp_in_portno = port;

    dsd_audio_input_request req;
    DSD_MEMSET(&req, 0, sizeof req);
    req.kind = DSD_AUDIO_INPUT_UDP;
    req.host = "127.0.0.1";
    req.port = free_udp_port();
    req.tcp_sockfd = DSD_INVALID_SOCKET;
    udp_input_test_fail_thread_starts(1);
    check(dsd_audio_switch_input(&g_opts, &g_state, &req) == DSD_AUDIO_INPUT_RESTARTED,
          "udp: the old UDP input starts again");
    dsd_input_cut cut;
    DSD_MEMSET(&cut, 0, sizeof cut);
    dsd_input_backlog_mark(&cut);
    check(cut.measured == 1U && dsd_input_backlog_cut_applies(&cut, g_opts.udp_in_ctx),
          "udp: the restarted UDP input is the measured input");
    udp_input_stop(&g_opts);
}

int
main(void) {
    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_socket_init\n");
        return 1;
    }
    dsd_net_audio_input_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.tcp_open = tcp_input_open;
    hooks.tcp_close = tcp_input_close;
    hooks.tcp_read_sample_wait = tcp_input_read_sample_wait;
    hooks.udp_start = udp_input_start;
    hooks.udp_stop = udp_input_stop;
    hooks.tcp_discard_through = tcp_input_discard_through;
    hooks.udp_discard_through = udp_input_discard_through;
    dsd_net_audio_input_hooks_set(&hooks);

    test_tcp_reader_kept();
    test_tcp_reader_kept_marked_after();
    test_stdin_reader_kept();
    test_udp_restarted();

    dsd_socket_cleanup();
    if (g_failures != 0) {
        return 1;
    }
    printf("CORE_AUDIO_INPUT_SWITCH_BACKLOG: OK\n");
    return 0;
}
