// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The TCP audio connect service: a bounded connect, then the input switch (issue #634), which keeps the running input
 * when the new one does not open (that half is CORE_AUDIO_INPUT_SWITCH's). Here the connect and the switch are fakes.
 */

#include <assert.h>
#include <dsd-neo/core/audio_input_switch.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/exitflag.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "services.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static dsd_socket_t g_connect_result = DSD_INVALID_SOCKET;
static int g_connect_calls = 0;
static char g_last_connect_host[256];
static int g_last_connect_port = 0;
static int g_last_connect_resolve = -1;
static int g_cancel_seen = -1;

static int g_switch_result = DSD_AUDIO_INPUT_SWITCHED;
static int g_switch_calls = 0;
static dsd_audio_input_request g_last_request;
static char g_last_request_host[256];

static int g_socket_close_calls = 0;
static dsd_socket_t g_closed_socket = DSD_INVALID_SOCKET;

static int g_keep_calls = 0;
static char g_kept_host[256];
static int g_kept_port = 0;

volatile uint8_t exitflag = 0; // NOLINT(misc-use-internal-linkage)

static void
reset_fakes(void) {
    g_connect_result = DSD_INVALID_SOCKET;
    g_connect_calls = 0;
    g_last_connect_host[0] = '\0';
    g_last_connect_port = 0;
    g_last_connect_resolve = -1;
    g_cancel_seen = -1;
    g_switch_result = DSD_AUDIO_INPUT_SWITCHED;
    g_switch_calls = 0;
    DSD_MEMSET(&g_last_request, 0, sizeof g_last_request);
    g_last_request_host[0] = '\0';
    g_socket_close_calls = 0;
    g_closed_socket = DSD_INVALID_SOCKET;
    g_keep_calls = 0;
    g_kept_host[0] = '\0';
    g_kept_port = 0;
    exitflag = 0;
}

dsd_socket_t
ConnectBounded(const char* hostname, int portno, int resolve, dsd_socket_cancel_fn cancelled, void* context) {
    g_connect_calls++;
    DSD_SNPRINTF(g_last_connect_host, sizeof(g_last_connect_host), "%s", hostname ? hostname : "");
    g_last_connect_port = portno;
    g_last_connect_resolve = resolve;
    g_cancel_seen = cancelled ? cancelled(context) : -1;
    return g_connect_result;
}

void
ConnectKeepTcpAudioAddress(const char* hostname, int portno) {
    g_keep_calls++;
    DSD_SNPRINTF(g_kept_host, sizeof g_kept_host, "%s", hostname ? hostname : "");
    g_kept_port = portno;
}

int
dsd_audio_switch_input(dsd_opts* opts, dsd_state* state, const dsd_audio_input_request* req) {
    (void)opts;
    (void)state;
    g_switch_calls++;
    g_last_request = *req;
    DSD_SNPRINTF(g_last_request_host, sizeof g_last_request_host, "%s", req->host ? req->host : "");
    g_last_request.host = NULL;
    return g_switch_result;
}

int
dsd_socket_close(dsd_socket_t sockfd) {
    g_socket_close_calls++;
    g_closed_socket = sockfd;
    return 0;
}

static dsd_opts g_opts;
static dsd_state g_state;

static void
test_a_failed_connect_switches_nothing(void) {
    reset_fakes();
    g_connect_result = DSD_INVALID_SOCKET;
    assert(svc_tcp_connect_audio(&g_opts, &g_state, "new.example", 1300) == DSD_AUDIO_INPUT_KEPT);
    assert(g_connect_calls == 1);
    assert(strcmp(g_last_connect_host, "new.example") == 0);
    assert(g_last_connect_port == 1300);
    /* A menu connect looks the host up afresh; the reconnect reuses that address. */
    assert(g_last_connect_resolve == 1);
    assert(g_switch_calls == 0);
    assert(g_socket_close_calls == 0);
    assert(g_keep_calls == 0);
}

static void
test_a_switch_that_keeps_the_input_closes_the_new_socket(void) {
    reset_fakes();
    g_connect_result = (dsd_socket_t)77;
    g_switch_result = DSD_AUDIO_INPUT_KEPT;
    assert(svc_tcp_connect_audio(&g_opts, &g_state, "127.0.0.1", 9000) == DSD_AUDIO_INPUT_KEPT);
    assert(g_switch_calls == 1);
    assert(g_last_request.kind == DSD_AUDIO_INPUT_TCP);
    assert(g_last_request.tcp_sockfd == (dsd_socket_t)77);
    assert(g_socket_close_calls == 1);
    assert(g_closed_socket == (dsd_socket_t)77);
    /* The running input's reconnect keeps going back to the address it had. */
    assert(g_keep_calls == 0);
}

static void
test_a_switch_takes_the_socket(void) {
    reset_fakes();
    g_connect_result = (dsd_socket_t)88;
    assert(svc_tcp_connect_audio(&g_opts, &g_state, "live.example", 1400) == DSD_AUDIO_INPUT_SWITCHED);
    assert(g_switch_calls == 1);
    assert(g_last_request.kind == DSD_AUDIO_INPUT_TCP);
    assert(strcmp(g_last_request_host, "live.example") == 0);
    assert(g_last_request.port == 1400);
    assert(g_last_request.tcp_sockfd == (dsd_socket_t)88);
    assert(g_socket_close_calls == 0);
    /* The input runs on this connection: its reconnect goes back to the address it went to. */
    assert(g_keep_calls == 1);
    assert(strcmp(g_kept_host, "live.example") == 0);
    assert(g_kept_port == 1400);
}

/* The host may be the options' own (the '8' key connects to tcp_hostname): the service connects to a copy. */
static void
test_the_host_may_be_the_options_own(void) {
    reset_fakes();
    DSD_SNPRINTF(g_opts.tcp_hostname, sizeof g_opts.tcp_hostname, "%s", "own.example");
    g_connect_result = (dsd_socket_t)89;
    assert(svc_tcp_connect_audio(&g_opts, &g_state, g_opts.tcp_hostname, 1500) == DSD_AUDIO_INPUT_SWITCHED);
    assert(strcmp(g_last_connect_host, "own.example") == 0);
    assert(strcmp(g_last_request_host, "own.example") == 0);
}

static void
test_the_connect_is_cancelled_by_shutdown(void) {
    reset_fakes();
    g_connect_result = DSD_INVALID_SOCKET;
    (void)svc_tcp_connect_audio(&g_opts, &g_state, "slow.example", 1600);
    assert(g_cancel_seen == 0);
    reset_fakes();
    exitflag = 1;
    (void)svc_tcp_connect_audio(&g_opts, &g_state, "slow.example", 1600);
    assert(g_cancel_seen == 1);
}

static void
test_invalid_requests_connect_nowhere(void) {
    reset_fakes();
    assert(svc_tcp_connect_audio(&g_opts, &g_state, "", 1300) == DSD_AUDIO_INPUT_KEPT);
    assert(svc_tcp_connect_audio(&g_opts, &g_state, "host", 0) == DSD_AUDIO_INPUT_KEPT);
    assert(svc_tcp_connect_audio(&g_opts, &g_state, "host", 65536) == DSD_AUDIO_INPUT_KEPT);
    assert(svc_tcp_connect_audio(&g_opts, NULL, "host", 1300) == DSD_AUDIO_INPUT_KEPT);
    assert(svc_tcp_connect_audio(NULL, &g_state, "host", 1300) == DSD_AUDIO_INPUT_KEPT);
    assert(svc_tcp_connect_audio(&g_opts, &g_state, NULL, 1300) == DSD_AUDIO_INPUT_KEPT);
    assert(g_connect_calls == 0);
    assert(g_switch_calls == 0);
}

int
main(void) {
    test_a_failed_connect_switches_nothing();
    test_a_switch_that_keeps_the_input_closes_the_new_socket();
    test_a_switch_takes_the_socket();
    test_the_host_may_be_the_options_own();
    test_the_connect_is_cancelled_by_shutdown();
    test_invalid_requests_connect_nowhere();
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
