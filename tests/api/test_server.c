// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Loopback integration tests for the TCP JSON Lines API server: framing, session commands, authentication, the
 * connection limits and their reclamation, refusal of non-JSON (cross-protocol) input, peers that vanish mid-send,
 * the telemetry feed driven through a real frontend runtime, and the scan avoid summary and pages (issue #672).
 */

#include <assert.h>
#include <dsd-neo/api/api.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/site_identity_view.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <dsd-neo/runtime/telemetry.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !DSD_PLATFORM_WIN_NATIVE
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "../../src/app_control/commands_internal.h"
#include "api_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "json.h"

enum { LINE_CAP = 1 << 18 };

static char g_line[LINE_CAP];

/* Read one line (without its newline) within @p timeout_ms. Returns 1 for a line, 0 at end of stream, -1 on timeout
   or error. */
static int
read_line(dsd_socket_t sock, unsigned int timeout_ms) {
    size_t used = 0;
    const uint64_t deadline = dsd_realtime_mono_ms() + timeout_ms;
    for (;;) {
        const uint64_t now = dsd_realtime_mono_ms();
        if (now >= deadline) {
            return -1;
        }
        const int ready = dsd_socket_wait(sock, DSD_SOCKET_WAIT_READ, (unsigned int)(deadline - now));
        if (ready <= 0) {
            continue;
        }
        char c = '\0';
        const int n = dsd_socket_recv(sock, &c, 1U, 0);
        if (n == 0) {
            return used > 0U ? -1 : 0;
        }
        if (n < 0) {
            return -1;
        }
        if (c == '\n') {
            g_line[used] = '\0';
            return 1;
        }
        assert(used + 1U < sizeof g_line);
        g_line[used++] = c;
    }
}

static void
expect_line(dsd_socket_t sock, const char* needle) {
    if (read_line(sock, 5000U) != 1 || strstr(g_line, needle) == NULL) {
        DSD_FPRINTF(stderr, "expected a line containing %s, got: %s\n", needle, g_line);
        assert(0);
    }
}

static void
expect_eof(dsd_socket_t sock) {
    const int rc = read_line(sock, 5000U);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "expected end of stream, got %d: %s\n", rc, rc == 1 ? g_line : "");
        assert(0);
    }
}

/* The client side of these tests sends to a server that may already have dropped it; that must cost an error, not a
   SIGPIPE, so the server-side SIGPIPE check (test_vanishing_peer) stays meaningful. */
#ifdef MSG_NOSIGNAL
#define TEST_SEND_FLAGS MSG_NOSIGNAL
#else
#define TEST_SEND_FLAGS 0
#endif

static int
send_bytes(dsd_socket_t sock, const char* s, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const int n = dsd_socket_send(sock, s + sent, len - sent, TEST_SEND_FLAGS);
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static void
send_str(dsd_socket_t sock, const char* s) {
    assert(send_bytes(sock, s, strlen(s)) == 0);
}

static dsd_socket_t
connect_port(int port) {
    struct sockaddr_in addr;
    assert(dsd_socket_resolve("127.0.0.1", port, &addr) == 0);
    dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(sock != DSD_INVALID_SOCKET);
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)dsd_socket_setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, (int)sizeof(one));
#endif
    assert(dsd_socket_connect(sock, (struct sockaddr*)&addr, (int)sizeof(addr)) == 0);
    return sock;
}

/* Connect and consume the welcome line. */
static dsd_socket_t
open_session(int port, const char* welcome_needle) {
    dsd_socket_t sock = connect_port(port);
    expect_line(sock, welcome_needle);
    return sock;
}

static int
start_server(const char* token, int max_clients, int auth_timeout_ms) {
    dsd_api_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.port = DSD_API_PORT_EPHEMERAL;
    cfg.max_clients = max_clients;
    cfg.auth_timeout_ms = auth_timeout_ms;
    if (token != NULL) {
        DSD_SNPRINTF(cfg.token, sizeof cfg.token, "%s", token);
    }
    assert(dsd_api_start(&cfg) == 0);
    assert(dsd_api_is_running());
    const int port = dsd_api_bound_port();
    assert(port > 0 && port <= 65535);
    return port;
}

static void
test_lifecycle(void) {
    dsd_api_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    assert(dsd_api_start(&cfg) == 0); /* port 0: disabled */
    assert(!dsd_api_is_running());
    cfg.port = 70000;
    assert(dsd_api_start(&cfg) != 0);
    cfg.port = DSD_API_PORT_EPHEMERAL;
    DSD_SNPRINTF(cfg.bind_addr, sizeof cfg.bind_addr, "%s", "0.0.0.0");
    assert(dsd_api_start(&cfg) != 0); /* off loopback without a token */
    DSD_SNPRINTF(cfg.bind_addr, sizeof cfg.bind_addr, "%s", "localhost");
    assert(dsd_api_start(&cfg) != 0); /* not a numeric address */
    cfg.bind_addr[0] = '\0';
    cfg.max_clients = 33;
    assert(dsd_api_start(&cfg) != 0);
    assert(!dsd_api_is_running());

    cfg.max_clients = 0;
    assert(dsd_api_start(&cfg) == 0);
    assert(dsd_api_start(&cfg) == 0); /* same configuration: no-op */
    DSD_SNPRINTF(cfg.token, sizeof cfg.token, "%s", "other");
    assert(dsd_api_start(&cfg) != 0); /* different configuration while running */
    dsd_api_stop();
    dsd_api_stop();
    assert(!dsd_api_is_running());
    assert(dsd_api_bound_port() == 0);
}

static void
test_session_commands(void) {
    const int port = start_server(NULL, 2, 0);
    dsd_socket_t c = connect_port(port);
    expect_line(c, "\"type\":\"welcome\"");
    assert(strstr(g_line, "\"auth_required\":false") != NULL);

    send_str(c, "{\"id\":1,\"cmd\":\"ping\"}\n");
    expect_line(c, "{\"id\":1,\"ok\":true,\"protocol\":1,\"authenticated\":true}");

    /* Two requests in one write, then one split across writes: answered in order, whole. */
    send_str(c, "{\"id\":2,\"cmd\":\"hello\"}\r\n\n   \n{\"id\":\"three\",\"cmd\":\"ping\"}\n{\"id\":4,\"cmd\":\"pi");
    expect_line(c, "{\"id\":2,\"ok\":true");
    expect_line(c, "{\"id\":\"three\",\"ok\":true");
    dsd_sleep_ms(100);
    send_str(c, "ng\"}\n");
    expect_line(c, "{\"id\":4,\"ok\":true");

    send_str(c, "{\"id\":5,\"cmd\":\"list_commands\"}\n");
    expect_line(c, "\"commands\":[{\"name\":\"toggle_mute\"");

    send_str(c, "{\"id\":6,\"cmd\":\"subscribe\",\"params\":{\"topics\":[\"status\",\"event\"]}}\n");
    expect_line(c, "{\"id\":6,\"ok\":true,\"topics\":[\"event\",\"status\"]}");
    send_str(c, "{\"id\":7,\"cmd\":\"subscribe\",\"topics\":[\"bogus\"]}\n");
    expect_line(c, "\"invalid_params\"");
    send_str(c, "{\"id\":8,\"cmd\":\"unsubscribe\",\"topics\":[\"event\"]}\n");
    expect_line(c, "{\"id\":8,\"ok\":true,\"topics\":[\"status\"]}");

    send_str(c, "{\"id\":9,\"cmd\":\"get\",\"params\":{\"what\":\"bogus\"}}\n");
    expect_line(c, "\"invalid_params\"");
    send_str(c, "{\"id\":10,\"cmd\":\"get\",\"what\":\"metrics\"}\n");
    expect_line(c, "{\"id\":10,\"ok\":true,\"what\":\"metrics\",\"data\":null}");

    /* No decoder session is open in this test: control commands are refused, the connection stays. */
    send_str(c, "{\"id\":11,\"cmd\":\"toggle_mute\"}\n");
    expect_line(c, "\"rejected\"");
    send_str(c, "{\"id\":12,\"cmd\":\"nope\"}\n");
    expect_line(c, "\"unknown_command\"");
    send_str(c, "{\"id\":13}\n");
    expect_line(c, "\"bad_request\"");
    send_str(c, "[1,2]\n");
    expect_line(c, "\"bad_request\"");
    send_str(c, "{\"id\":14,\"cmd\":\"ping\",\"params\":[]}\n");
    expect_line(c, "\"bad_request\"");
    send_str(c, "{\"id\":15,\"cmd\":\"ping\"}\n");
    expect_line(c, "{\"id\":15,\"ok\":true");
    dsd_socket_close(c);
    dsd_api_stop();
}

static void
test_rejects_non_json(void) {
    const int port = start_server(NULL, 0, 0);
    /* A browser can aim an HTTP request at a loopback port; its request line is not JSON, so the connection ends there
       and the body (which a page controls) is never read as a request. */
    dsd_socket_t c = open_session(port, "\"type\":\"welcome\"");
    send_str(c, "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: text/plain\r\n\r\n{\"id\":1,\"cmd\":\"ping\"}\n");
    expect_line(c, "\"parse_error\"");
    expect_eof(c);
    dsd_socket_close(c);

    /* A line longer than the limit is refused and ends the connection. */
    c = open_session(port, "\"type\":\"welcome\"");
    const size_t big = 300000U;
    char* junk = (char*)malloc(big);
    assert(junk != NULL);
    DSD_MEMSET(junk, 'a', big);
    (void)send_bytes(c, junk, big);
    free(junk);
    expect_line(c, "\"line_too_long\"");
    expect_eof(c);
    dsd_socket_close(c);

    /* Deep nesting is a parse error, not a crash. */
    c = open_session(port, "\"type\":\"welcome\"");
    char deep[4096];
    DSD_MEMSET(deep, '[', sizeof deep - 2U);
    deep[sizeof deep - 2U] = '\n';
    deep[sizeof deep - 1U] = '\0';
    send_str(c, deep);
    expect_line(c, "\"parse_error\"");
    expect_eof(c);
    dsd_socket_close(c);
    dsd_api_stop();
}

static void
test_client_slots_are_reclaimed(void) {
    const int port = start_server(NULL, 2, 0);
    /* Far more sequential clients than slots: each departure frees its slot. */
    for (int i = 0; i < 12; i++) {
        dsd_socket_t c = open_session(port, "\"type\":\"welcome\"");
        if (i % 2 == 0) {
            send_str(c, "{\"cmd\":\"ping\"}\n");
            expect_line(c, "\"ok\":true");
        }
        dsd_socket_close(c);
        dsd_sleep_ms(30);
    }
    dsd_sleep_ms(200);
    dsd_socket_t a = open_session(port, "\"type\":\"welcome\"");
    dsd_socket_t b = open_session(port, "\"type\":\"welcome\"");
    /* A third concurrent client is told why, then dropped -- after a linger in which what it sends is still read, so
       a request it had already sent cannot turn the close into a reset that discards the reply. A socket closed
       outright would answer the first send below with a reset, and the second would fail. */
    dsd_socket_t full = connect_port(port);
    expect_line(full, "\"busy\"");
    expect_eof(full);
    send_str(full, "{\"cmd\":\"ping\"}\n");
    dsd_sleep_ms(100);
    assert(send_bytes(full, "{\"cmd\":\"ping\"}\n", strlen("{\"cmd\":\"ping\"}\n")) == 0);
    dsd_socket_close(full);
    dsd_socket_close(a);
    dsd_socket_close(b);
    dsd_api_stop();
}

static void
test_vanishing_peer(void) {
    const int port = start_server(NULL, 0, 0);
    /* Ask for a lot of output, then hang up without reading it: the server's sends fail on a dead connection. Where a
       write to a closed socket raises SIGPIPE, this kills the test unless the server suppresses it. */
    for (int round = 0; round < 3; round++) {
        dsd_socket_t c = open_session(port, "\"type\":\"welcome\"");
        for (int i = 0; i < 40; i++) {
            if (send_bytes(c, "{\"cmd\":\"list_commands\"}\n", 24U) != 0) {
                break;
            }
        }
        dsd_socket_close(c);
    }
    dsd_sleep_ms(300);
    dsd_socket_t c = open_session(port, "\"type\":\"welcome\"");
    send_str(c, "{\"id\":1,\"cmd\":\"ping\"}\n");
    expect_line(c, "{\"id\":1,\"ok\":true");
    dsd_socket_close(c);

    /* A client that never reads is dropped once its backlog passes the cap, and the server carries on. */
    c = open_session(port, "\"type\":\"welcome\"");
    int closed = 0;
    for (int i = 0; i < 2000 && !closed; i++) {
        closed = send_bytes(c, "{\"cmd\":\"list_commands\"}\n", 24U) != 0;
    }
    dsd_socket_close(c);
    c = open_session(port, "\"type\":\"welcome\"");
    dsd_socket_close(c);
    dsd_api_stop();
}

static void
test_authentication(void) {
    int port = start_server("s3cret token", 0, 400);
    dsd_socket_t c = open_session(port, "\"auth_required\":true");
    send_str(c, "{\"id\":1,\"cmd\":\"ping\"}\n");
    expect_line(c, "\"authenticated\":false");
    send_str(c, "{\"id\":2,\"cmd\":\"subscribe\",\"topics\":[\"status\"]}\n");
    expect_line(c, "\"unauthorized\"");
    send_str(c, "{\"id\":3,\"cmd\":\"auth\",\"params\":{\"token\":\"s3cret token\"}}\n");
    expect_line(c, "{\"id\":3,\"ok\":true,\"authenticated\":true}");
    send_str(c, "{\"id\":4,\"cmd\":\"subscribe\",\"topics\":[\"status\"]}\n");
    expect_line(c, "{\"id\":4,\"ok\":true");
    /* Authenticated sessions are not subject to the auth deadline. */
    dsd_sleep_ms(700);
    send_str(c, "{\"id\":5,\"cmd\":\"ping\"}\n");
    expect_line(c, "{\"id\":5,\"ok\":true");
    dsd_socket_close(c);

    /* The token at the top level works too. */
    c = open_session(port, "\"auth_required\":true");
    send_str(c, "{\"cmd\":\"auth\",\"token\":\"s3cret token\"}\n");
    expect_line(c, "\"authenticated\":true");
    dsd_socket_close(c);
    dsd_api_stop();

    /* A reused config whose token was overwritten by a shorter one still has stale bytes after the terminator; only
       the text is the token. */
    dsd_api_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.port = DSD_API_PORT_EPHEMERAL;
    cfg.auth_timeout_ms = 400;
    DSD_SNPRINTF(cfg.token, sizeof cfg.token, "%s", "short");
    DSD_MEMSET(cfg.token + 6, 'x', 20U); /* what an earlier, longer token leaves behind */
    assert(dsd_api_start(&cfg) == 0);
    c = open_session(dsd_api_bound_port(), "\"auth_required\":true");
    send_str(c, "{\"cmd\":\"auth\",\"token\":\"short\"}\n");
    expect_line(c, "\"authenticated\":true");
    dsd_socket_close(c);
    dsd_api_stop();
    port = start_server("s3cret token", 0, 400);

    /* And once more at the top level after a restart. */
    c = open_session(port, "\"auth_required\":true");
    send_str(c, "{\"cmd\":\"auth\",\"token\":\"s3cret token\"}\n");
    expect_line(c, "\"authenticated\":true");
    dsd_socket_close(c);

    /* A wrong token -- a prefix, a longer string, or none -- is answered once, then the connection ends. */
    static const char* const k_wrong[] = {
        "{\"cmd\":\"auth\",\"params\":{\"token\":\"s3cret\"}}\n",
        "{\"cmd\":\"auth\",\"params\":{\"token\":\"s3cret token!\"}}\n",
        "{\"cmd\":\"auth\",\"params\":{}}\n",
    };
    for (size_t i = 0; i < sizeof k_wrong / sizeof k_wrong[0]; i++) {
        c = open_session(port, "\"auth_required\":true");
        send_str(c, k_wrong[i]);
        expect_line(c, "\"unauthorized\"");
        expect_eof(c);
        dsd_socket_close(c);
    }

    /* A connection that never authenticates is closed at the deadline, freeing its slot. */
    c = open_session(port, "\"auth_required\":true");
    expect_eof(c);
    dsd_socket_close(c);
    dsd_api_stop();
}

/* Read lines until one has @p type and contains @p needle; returns 1, or 0 when none arrives in @p timeout_ms. */
static int
await_line(dsd_socket_t c, const char* type, const char* needle, unsigned int timeout_ms) {
    char want_type[64];
    DSD_SNPRINTF(want_type, sizeof want_type, "\"type\":\"%s\"", type);
    const uint64_t deadline = dsd_realtime_mono_ms() + timeout_ms;
    while (dsd_realtime_mono_ms() < deadline) {
        if (read_line(c, 100U) == 1 && strstr(g_line, want_type) != NULL
            && (needle == NULL || strstr(g_line, needle) != NULL)) {
            return 1;
        }
    }
    return 0;
}

/* Read lines until one contains @p needle; returns 1, or 0 when none arrives in @p timeout_ms. */
static int
await_text(dsd_socket_t c, const char* needle, unsigned int timeout_ms) {
    const uint64_t deadline = dsd_realtime_mono_ms() + timeout_ms;
    while (dsd_realtime_mono_ms() < deadline) {
        if (read_line(c, 100U) == 1 && strstr(g_line, needle) != NULL) {
            return 1;
        }
    }
    return 0;
}

static void
commit_event(dsd_state* state, const char* t_name, uint32_t source_id) {
    Event_History* row = &state->event_history_s[0].Event_History_Items[0];
    DSD_SNPRINTF(row->t_name, sizeof row->t_name, "%s", t_name);
    DSD_SNPRINTF(row->alias, sizeof row->alias, "%s", "Unit \"7\"\nline2");
    row->source_id = source_id;
    row->target_id = 101U;
    push_event_history(&state->event_history_s[0]);
}

static void
test_feed(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(opts != NULL && state != NULL);
    initOpts(opts);
    initState(state);
    assert(state->event_history_s != NULL);
    dsd_app_frontend_runtime_start(opts, state);
    const int port = start_server(NULL, 0, 0);
    /* What the CLI does right after starting the server, before the decoder runs: the feed's first look. */
    dsd_telemetry_publish_both_and_redraw(opts, state);

    dsd_socket_t c = open_session(port, "\"type\":\"welcome\"");
    send_str(c, "{\"id\":1,\"cmd\":\"subscribe\",\"topics\":[\"event\",\"status\",\"call\"]}\n");
    expect_line(c, "{\"id\":1,\"ok\":true");

    /* A row committed right after the subscription is sent at the next publish, whole and escaped. */
    commit_event(state, "Dispatch", 7U);
    dsd_telemetry_publish_both_and_redraw(opts, state);
    assert(await_line(c, "event", "\"t_name\":\"Dispatch\"", 3000U));
    assert(strstr(g_line, "\"update\":false") != NULL);
    assert(strstr(g_line, "\"source_id\":7,\"target_id\":101") != NULL);
    assert(strstr(g_line, "\"alias\":\"Unit \\\"7\\\"\\nline2\"") != NULL);
    assert(strstr(g_line, "\"slot\":0,\"push\":") != NULL);
    assert(await_line(c, "status", "\"tg_policy\":{\"context\":\"", 3000U));
    assert(strstr(g_line, "\"decryption\":{\"target_id\":\"\",\"tune_generation\":\"") != NULL);

    /* Issue #637: status carries the receive verdict and the configured filters and names, in receive_set's spelling,
       so a client can read one before it replaces it. */
    char receive_error[160];
    assert(dsd_receive_edit_parse(&opts->receive, DSD_RECEIVE_NAC, 1, "293/A12", "370", "", receive_error,
                                  sizeof receive_error)
           == 0);
    assert(
        dsd_receive_edit_parse(&opts->receive, 4, 0, "", "", "--nac-name 293=Fire", receive_error, sizeof receive_error)
        == 0);
    /* The feed sends status at most every 250 ms: publish until it carries the edited policy. */
    int receive_seen = 0;
    for (int attempt = 0; attempt < 30 && !receive_seen; attempt++) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
        receive_seen = await_line(c, "status", "\"receive\":{\"status\":\"Checking received identifier\"", 100U);
    }
    assert(receive_seen);
    assert(strstr(g_line, "{\"kind\":0,\"enabled\":0,\"allow\":\"\",\"block\":\"\"}") != NULL);
    assert(strstr(g_line, "{\"kind\":3,\"enabled\":1,\"allow\":\"293/A12\",\"block\":\"370\"}") != NULL);
    assert(strstr(g_line, "\"names\":\"--nac-name \\\"293=Fire\\\"\"") != NULL);
    DSD_MEMSET(&opts->receive, 0, sizeof opts->receive);

    /* Issue #671 (D12): status carries the scan state every frontend shows: none, a -Y list running, paused, and a
       trunk scan paused, with its target count. */
    static const struct {
        int scanner_mode;
        int trunk_scan;
        uint8_t lcn_paused;
        uint8_t trunk_paused;
        const char* want;
    } k_scan[] = {
        {0, 0, 0U, 0U, "\"scan\":{\"kind\":\"none\",\"state\":\"off\",\"targets\":0}"},
        {1, 0, 0U, 0U, "\"scan\":{\"kind\":\"channel\",\"state\":\"running\",\"targets\":3}"},
        {0, 0, 1U, 0U, "\"scan\":{\"kind\":\"channel\",\"state\":\"paused\",\"targets\":3}"},
        {0, 1, 0U, 1U, "\"scan\":{\"kind\":\"trunk\",\"state\":\"paused\",\"targets\":2}"},
    };

    state->lcn_freq_count = 3;
    state->trunk_scan_target_count = 2U;
    for (size_t i = 0; i < sizeof k_scan / sizeof k_scan[0]; i++) {
        opts->scanner_mode = k_scan[i].scanner_mode;
        opts->trunk_scan_enabled = k_scan[i].trunk_scan;
        state->lcn_scan_paused = k_scan[i].lcn_paused;
        state->trunk_scan_paused = k_scan[i].trunk_paused;
        int scan_seen = 0;
        for (int attempt = 0; attempt < 30 && !scan_seen; attempt++) {
            dsd_telemetry_publish_both_and_redraw(opts, state);
            scan_seen = await_line(c, "status", k_scan[i].want, 100U);
        }
        if (!scan_seen) {
            DSD_FPRINTF(stderr, "status never carried %s\n", k_scan[i].want);
        }
        assert(scan_seen);
    }
    opts->scanner_mode = 0;
    opts->trunk_scan_enabled = 0;
    state->lcn_scan_paused = 0U;
    state->trunk_scan_paused = 0U;
    state->lcn_freq_count = 0;
    state->trunk_scan_target_count = 0U;

    /* A row enriched in place after it was sent -- here the older of two, as a late alias lands on a call a newer
       notice already followed -- is sent again, marked as an update, under the same identity. */
    commit_event(state, "Notice", 10U);
    dsd_telemetry_publish_both_and_redraw(opts, state);
    assert(await_line(c, "event", "\"t_name\":\"Notice\"", 3000U));
    /* A row no reception named reports kind "none" (6), never kind 0 ("slot"). */
    assert(strstr(g_line, "\"receive_kind\":6,\"receive_value\":0,\"receive_known\":false") != NULL);
    Event_History* older = &state->event_history_s[0].Event_History_Items[2];
    assert(strcmp(older->t_name, "Dispatch") == 0);
    DSD_SNPRINTF(older->alias, sizeof older->alias, "%s", "Late Alias");
    state->event_history_s[0].commit_rev++;
    dsd_telemetry_publish_both_and_redraw(opts, state);
    assert(await_line(c, "event", "\"alias\":\"Late Alias\"", 3000U));
    assert(strstr(g_line, "\"update\":true") != NULL && strstr(g_line, "\"t_name\":\"Dispatch\"") != NULL);

    /* A row carries the frequency it was heard on and its access code (issue #575), and a row that learns them in
       place -- a reacquired segment filling what the first never decoded -- is sent again. */
    Event_History* staged = &state->event_history_s[0].Event_History_Items[0];
    staged->freq_hz = 851012500;
    staged->access_code_kind = (uint8_t)DSD_ACCESS_CODE_NAC;
    staged->access_code = 0x293U;
    DSD_SNPRINTF(staged->service_label, sizeof staged->service_label, "%s", "Hospital West");
    staged->receive_identity.kind = DSD_RECEIVE_NAC;
    staged->receive_identity.known = 1;
    staged->receive_identity.value = 0x293U;
    commit_event(state, "Tuned", 12U);
    dsd_telemetry_publish_both_and_redraw(opts, state);
    assert(await_line(c, "event", "\"t_name\":\"Tuned\"", 3000U));
    assert(strstr(g_line, "\"channel\":0,\"freq_hz\":851012500,\"access_code_kind\":2,\"access_code\":659,") != NULL);
    assert(strstr(g_line, "\"service_label\":\"Hospital West\"") != NULL);
    assert(strstr(g_line, "\"receive_kind\":3,\"receive_value\":659,\"receive_known\":true,\"receive_slot\":0")
           != NULL);
    staged->freq_hz = 0;
    staged->access_code_kind = (uint8_t)DSD_ACCESS_CODE_NONE;
    staged->access_code = 0U;
    commit_event(state, "Untuned", 13U);
    dsd_telemetry_publish_both_and_redraw(opts, state);
    assert(await_line(c, "event", "\"t_name\":\"Untuned\"", 3000U));
    assert(strstr(g_line, "\"freq_hz\":0,\"access_code_kind\":0,\"access_code\":0,") != NULL);
    Event_History* untuned = &state->event_history_s[0].Event_History_Items[1];
    assert(strcmp(untuned->t_name, "Untuned") == 0);
    untuned->freq_hz = 852000000;
    state->event_history_s[0].commit_rev++;
    dsd_telemetry_publish_both_and_redraw(opts, state);
    assert(await_line(c, "event", "\"freq_hz\":852000000", 3000U));
    assert(strstr(g_line, "\"update\":true") != NULL && strstr(g_line, "\"t_name\":\"Untuned\"") != NULL);

    /* A late service identity alone changes the row's digest and reaches subscribers. */
    DSD_SNPRINTF(untuned->service_label, sizeof untuned->service_label, "%s", "Road crews");
    untuned->receive_identity.kind = DSD_RECEIVE_RAN;
    untuned->receive_identity.known = 1;
    untuned->receive_identity.value = 23;
    state->event_history_s[0].commit_rev++;
    dsd_telemetry_publish_both_and_redraw(opts, state);
    assert(await_line(c, "event", "\"service_label\":\"Road crews\"", 3000U));
    assert(strstr(g_line, "\"update\":true") != NULL);
    assert(strstr(g_line, "\"receive_kind\":2,\"receive_value\":23,\"receive_known\":true") != NULL);
    untuned->access_code_kind = (uint8_t)DSD_ACCESS_CODE_RAN;
    untuned->access_code = 23U;
    state->event_history_s[0].commit_rev++;
    dsd_telemetry_publish_both_and_redraw(opts, state);
    assert(await_line(c, "event", "\"access_code_kind\":3,\"access_code\":23", 3000U));
    assert(strstr(g_line, "\"update\":true") != NULL && strstr(g_line, "\"t_name\":\"Untuned\"") != NULL);

    /* CRC provenance (discussion #602) goes out as two 0/1 flags -- crc_invalid, a check failed; crc_ras, only the DMR
       RAS heuristic accepted it -- and a row whose bits change in place is sent again. */
    assert(strstr(g_line, "\"crc_invalid\":0,\"crc_ras\":0,") != NULL);

    static const struct {
        uint8_t bits;
        const char* json;
    } crc_cases[] = {
        {DSD_CRC_RAS_UNVERIFIED, "\"crc_invalid\":0,\"crc_ras\":1,"},
        {DSD_CRC_FAILED | DSD_CRC_RAS_UNVERIFIED, "\"crc_invalid\":1,\"crc_ras\":1,"},
        {DSD_CRC_FAILED, "\"crc_invalid\":1,\"crc_ras\":0,"},
    };

    for (size_t i = 0; i < sizeof crc_cases / sizeof crc_cases[0]; i++) {
        untuned->crc_invalid = crc_cases[i].bits;
        state->event_history_s[0].commit_rev++;
        dsd_telemetry_publish_both_and_redraw(opts, state);
        assert(await_line(c, "event", crc_cases[i].json, 3000U));
        assert(strstr(g_line, "\"update\":true") != NULL && strstr(g_line, "\"t_name\":\"Untuned\"") != NULL);
    }

    /* Issue #799: a row names the site it was heard on and the numbering of its channel, each part only when its valid
       bit is set, after the keys that were always there; an identity that changes alone is sent again. */
    {
        Event_History* site_row = untuned;
        site_row->site_id.family = (uint8_t)DSD_RXID_FAMILY_P25;
        site_row->site_id.valid = DSD_RXID_P25_WACN | DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE;
        site_row->site_id.p25_wacn = 0xBEE00U;
        site_row->site_id.p25_sysid = 0x293U;
        site_row->site_id.p25_rfss = 1U;
        site_row->site_id.p25_site = 7U;
        state->event_history_s[0].commit_rev++;
        dsd_telemetry_publish_both_and_redraw(opts, state);
        assert(await_line(c, "event", "\"site\":{\"family\":2,", 3000U));
        /* An identity-only change sends the row again. */
        assert(strstr(g_line, "\"update\":true") != NULL && strstr(g_line, "\"t_name\":\"Untuned\"") != NULL);
        assert(strstr(g_line, "\"site\":{\"family\":2,\"wacn\":781824,\"sysid\":659,\"rfss\":1,\"site\":7}") != NULL);
        assert(strstr(g_line, "\"lra\"") == NULL && strstr(g_line, "\"dmr_net\"") == NULL);
        assert(strstr(g_line, "\"channel_kind\":\"none\"") != NULL);
        assert(strstr(g_line, "\"site_text\":\"P25 \xC2\xB7 ") != NULL);
        /* The pinned neighbours of channel and access code are untouched. */
        assert(strstr(g_line, "\"channel\":0,\"freq_hz\":852000000,\"access_code_kind\":3,\"access_code\":23,")
               != NULL);

        /* Capacity Plus: a site number and no network number; the channel's numbering is an LSN. */
        DSD_MEMSET(&site_row->site_id, 0, sizeof site_row->site_id);
        site_row->site_id.family = (uint8_t)DSD_RXID_FAMILY_DMR;
        site_row->site_id.valid = DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_BRAND;
        site_row->site_id.dmr_trunk = (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS;
        site_row->site_id.dmr_site = 2U;
        DSD_SNPRINTF(site_row->site_id.dmr_site_text, sizeof site_row->site_id.dmr_site_text, "%s",
                     "Cap+ \xC2\xB7 Site 2");
        DSD_SNPRINTF(site_row->site_id.dmr_brand, sizeof site_row->site_id.dmr_brand, "%s", "Capacity Plus");
        site_row->channel = 5U;
        site_row->call_channel_kind = (uint8_t)DSD_CALL_CHANNEL_LSN;
        state->event_history_s[0].commit_rev++;
        dsd_telemetry_publish_both_and_redraw(opts, state);
        assert(await_line(c, "event", "\"channel_kind\":\"lsn\"", 3000U));
        assert(strstr(g_line, "\"update\":true") != NULL);
        assert(strstr(g_line, "\"dmr_site\":2") != NULL && strstr(g_line, "\"dmr_net\"") == NULL);
        assert(strstr(g_line, "\"dmr_trunk\":2") != NULL);
        assert(strstr(g_line, "\"dmr_site_text\":\"Cap+ \xC2\xB7 Site 2\"") != NULL);
        assert(strstr(g_line, "\"dmr_brand\":\"Capacity Plus\"") != NULL);
        assert(strstr(g_line, "\"wacn\"") == NULL);
        assert(
            strstr(g_line, "\"site_text\":\"DMR \xC2\xB7 Cap+ \xC2\xB7 Site 2 \xC2\xB7 Capacity Plus \xC2\xB7 LSN 5\"")
            != NULL);

        /* The channel's numbering alone is an update too. */
        site_row->call_channel_kind = (uint8_t)DSD_CALL_CHANNEL_LCN;
        state->event_history_s[0].commit_rev++;
        dsd_telemetry_publish_both_and_redraw(opts, state);
        assert(await_line(c, "event", "\"channel_kind\":\"lcn\"", 3000U));
        assert(strstr(g_line, "\"update\":true") != NULL);

        /* The longest DMR line the record can hold: every text field full and every part set. The API sends it
           whole, as the frontends spell it into a DSD_APP_SITE_HISTORY_TEXT_SIZE buffer. */
        DSD_MEMSET(&site_row->site_id, 0, sizeof site_row->site_id);
        site_row->site_id.family = (uint8_t)DSD_RXID_FAMILY_DMR;
        site_row->site_id.valid = DSD_RXID_ACCESS | DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_BRAND
                                  | DSD_RXID_DMR_REST_LSN | DSD_RXID_DMR_XPT_FREE_LCN | DSD_RXID_DMR_RAS;
        site_row->site_id.dmr_trunk = (uint8_t)DSD_RXID_DMR_TRUNK_TIER3;
        site_row->site_id.access_kind = (uint8_t)DSD_ACCESS_CODE_COLOR_CODE;
        site_row->site_id.access = 15U;
        site_row->site_id.dmr_rest_lsn = 255U;
        site_row->site_id.dmr_xpt_free_lcn = 15U;
        DSD_MEMSET(site_row->site_id.dmr_site_text, 'S', sizeof site_row->site_id.dmr_site_text - 1U);
        DSD_MEMSET(site_row->site_id.dmr_brand, 'B', sizeof site_row->site_id.dmr_brand - 1U);
        site_row->channel = UINT32_MAX;
        site_row->call_channel_kind = (uint8_t)DSD_CALL_CHANNEL_LPCN;
        char want[DSD_APP_SITE_HISTORY_TEXT_SIZE];
        const size_t want_len = dsd_app_site_history_text(&site_row->site_id, site_row->call_channel_kind,
                                                          site_row->channel, want, sizeof want);
        assert(want_len > 0U && want_len + 1U < sizeof want);
        char want_json[DSD_APP_SITE_HISTORY_TEXT_SIZE + 16];
        DSD_SNPRINTF(want_json, sizeof want_json, "\"site_text\":\"%s\"", want);
        state->event_history_s[0].commit_rev++;
        dsd_telemetry_publish_both_and_redraw(opts, state);
        assert(await_line(c, "event", "\"channel_kind\":\"lpcn\"", 3000U));
        assert(strstr(g_line, want_json) != NULL);
        assert(strstr(want, "LPCN 4294967295") != NULL);

        DSD_MEMSET(&site_row->site_id, 0, sizeof site_row->site_id);
        site_row->channel = 0U;
        site_row->call_channel_kind = (uint8_t)DSD_CALL_CHANNEL_NONE;
        state->event_history_s[0].commit_rev++;
        dsd_telemetry_publish_both_and_redraw(opts, state);
        assert(await_line(c, "event", "\"site\":{\"family\":0}", 3000U));
        assert(strstr(g_line, "\"site_text\":\"\"") != NULL);
    }

    /* Issue #773: a call slot carries the call kind once (the key the feed already had), the source radio's id and
       label apart from src_text, the target's own label and the protocol's display name. */
    {
        dsd_call_observation call = dsd_call_observation_data(DSD_SYNC_NXDN_POS, 0U, 4242U, 2001U);
        call.kind = DSD_CALL_KIND_PRIVATE_VOICE;
        call.observed_m = dsd_decode_now_mono_s();
        assert(dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN) > 0);
        Event_History* row = &state->event_history_s[0].Event_History_Items[0];
        row->target_id = 2001U;
        DSD_SNPRINTF(row->t_name, sizeof row->t_name, "%s", "Dispatcher 7");
        row->source_id = 4242U;
        DSD_SNPRINTF(row->s_name, sizeof row->s_name, "%s", "Engine 21");
        int call_seen = 0;
        for (int attempt = 0; attempt < 30 && !call_seen; attempt++) {
            dsd_telemetry_publish_both_and_redraw(opts, state);
            call_seen = await_line(c, "call", "\"tg_label\":\"Dispatcher 7\"", 100U);
        }
        assert(call_seen);
        assert(strstr(g_line, "\"src_text\":\"Engine 21 (4242)\"") != NULL);
        assert(strstr(g_line, "\"src_id\":4242,\"src_label\":\"Engine 21\",\"tg_label\":\"Dispatcher 7\","
                              "\"protocol_label\":\"NXDN\"")
               != NULL);
        int kind_keys = 0;
        for (const char* k = strstr(g_line, "\"kind\":"); k != NULL; k = strstr(k + 1, "\"kind\":")) {
            kind_keys++;
        }
        assert(kind_keys == 1 && strstr(g_line, "\"kind\":3") != NULL);
        assert(dsd_call_state_end(state, 0U, dsd_decode_now_mono_s()) > 0);
        DSD_MEMSET(row, 0, sizeof *row);
    }

    /* A one-shot get reads the cached record. */
    send_str(c, "{\"id\":2,\"cmd\":\"get\",\"what\":\"status\"}\n");
    assert(await_text(c, "{\"id\":2,\"ok\":true,\"what\":\"status\",\"data\":{\"type\":\"status\"", 3000U));

    /* Unsubscribed, the client gets no events; resubscribed, it gets the rows committed from then on -- even one
       committed before any further publish -- and none of those it missed. */
    send_str(c, "{\"id\":3,\"cmd\":\"unsubscribe\",\"topics\":[\"event\"]}\n");
    assert(await_text(c, "{\"id\":3,\"ok\":true", 3000U));
    commit_event(state, "Missed", 8U);
    dsd_telemetry_publish_both_and_redraw(opts, state);
    send_str(c, "{\"id\":4,\"cmd\":\"subscribe\",\"topics\":[\"event\"]}\n");
    assert(await_text(c, "{\"id\":4,\"ok\":true", 3000U));
    commit_event(state, "Fresh", 9U);
    dsd_telemetry_publish_both_and_redraw(opts, state);
    int saw_missed = 0;
    int saw_fresh = 0;
    const uint64_t deadline = dsd_realtime_mono_ms() + 3000U;
    while (dsd_realtime_mono_ms() < deadline && !saw_fresh) {
        if (read_line(c, 100U) == 1 && strstr(g_line, "\"type\":\"event\"") != NULL) {
            saw_missed |= strstr(g_line, "\"t_name\":\"Missed\"") != NULL;
            saw_fresh |= strstr(g_line, "\"t_name\":\"Fresh\"") != NULL;
        }
    }
    assert(saw_fresh && !saw_missed);

    /* A P25 neighbor heard only in an abbreviated announcement has no WACN or LRA: unknown, not 0. */
    state->p25_nb_entries[0].freq = 851037500;
    state->p25_nb_entries[0].sysid = 0x125;
    state->p25_nb_entries[0].wacn_valid = 0;
    state->p25_nb_entries[0].lra_valid = 0;
    state->p25_nb_count = 1;
    send_str(c, "{\"id\":5,\"cmd\":\"subscribe\",\"topics\":[\"system\"]}\n");
    assert(await_text(c, "{\"id\":5,\"ok\":true", 3000U));
    int saw_neighbor = 0;
    const uint64_t system_deadline = dsd_realtime_mono_ms() + 3000U;
    while (dsd_realtime_mono_ms() < system_deadline && !saw_neighbor) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
        saw_neighbor = read_line(c, 100U) == 1 && strstr(g_line, "\"type\":\"system\"") != NULL
                       && strstr(g_line, "{\"freq_hz\":851037500,\"wacn\":null,\"sysid\":293,") != NULL;
    }
    assert(saw_neighbor);
    assert(strstr(g_line, "\"site\":0,\"lra\":null,") != NULL);

    /* Issue #637: clearing the history while subscribed sends no blank rows: a cleared row (receive kind "none") is
       the empty row the feed suppresses. */
    init_event_history(&state->event_history_s[0], 0, DSD_EVENT_HISTORY_LEN);
    dsd_telemetry_publish_both_and_redraw(opts, state);
    commit_event(state, "AfterClear", 12U);
    dsd_telemetry_publish_both_and_redraw(opts, state);
    int blank_events = 0;
    int saw_after_clear = 0;
    const uint64_t clear_deadline = dsd_realtime_mono_ms() + 3000U;
    while (dsd_realtime_mono_ms() < clear_deadline && !saw_after_clear) {
        if (read_line(c, 100U) != 1 || strstr(g_line, "\"type\":\"event\"") == NULL) {
            continue;
        }
        saw_after_clear = strstr(g_line, "\"t_name\":\"AfterClear\"") != NULL;
        blank_events += !saw_after_clear && strstr(g_line, "\"t_name\":\"\"") != NULL;
    }
    assert(saw_after_clear && blank_events == 0);

    /* Stopping drains: a row published just before the stop still reaches the subscriber, then the stream ends. */
    commit_event(state, "Final", 11U);
    dsd_telemetry_publish_both_and_redraw(opts, state);
    dsd_api_stop();
    assert(await_line(c, "event", "\"t_name\":\"Final\"", 3000U));
    int ended = 0;
    for (int i = 0; i < 200 && !ended; i++) {
        ended = read_line(c, 100U) == 0;
    }
    assert(ended);
    dsd_socket_close(c);
    /* The observer is gone: publishing now reaches no feed. */
    dsd_telemetry_publish_both_and_redraw(opts, state);
    dsd_app_frontend_runtime_stop();
    freeState(state);
    free(state);
    free(opts);
}

/*
 * Issue #803 (T5): rr_apply_import's response only acknowledges the queue; the import's outcome, its tune's included,
 * arrives in the status feed's message. An RTL input whose stream is not running cannot take the tune: the response is
 * "queued" and carries no outcome, and status.message is the partial apply, which on main read "Applied: RR import".
 */
static void
test_rr_apply_outcome_in_status_feed(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(opts != NULL && state != NULL);
    initOpts(opts);
    opts->audio_out_type = 9; /* the null output: no host audio stream */
    initState(state);
    opts->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0");
    dsd_app_frontend_runtime_start(opts, state);
    const int port = start_server(NULL, 0, 0);
    dsd_telemetry_publish_both_and_redraw(opts, state);
    dsd_socket_t c = open_session(port, "\"type\":\"welcome\"");
    send_str(c, "{\"id\":1,\"cmd\":\"subscribe\",\"topics\":[\"status\"]}\n");
    expect_line(c, "{\"id\":1,\"ok\":true");

    send_str(c, "{\"id\":2,\"cmd\":\"rr_apply_import\",\"params\":{\"decode_mode\":2,\"trunking\":true,"
                "\"tune_hz\":851375000}}\n");
    assert(await_text(c, "{\"id\":2,", 3000U));
    assert(strstr(g_line, "\"ok\":true,\"cmd\":\"rr_apply_import\",\"status\":\"queued\"") != NULL);
    assert(strstr(g_line, "RR import") == NULL && strstr(g_line, "\"message\"") == NULL);

    /* The decoder applies it, and the feed's next status carries the outcome. */
    assert(dsd_app_drain_cmds(opts, state) == 1);
    const char* want = "\"message\":{\"text\":\"Failed: RR import -> tune to 851.375000 MHz failed; mode applied\"";
    int seen = 0;
    for (int attempt = 0; attempt < 30 && !seen; attempt++) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
        seen = await_line(c, "status", want, 100U);
    }
    if (!seen) {
        DSD_FPRINTF(stderr, "status never carried %s (decoder message \"%s\")\n", want, state->ui_msg);
    }
    assert(seen);
    dsd_socket_close(c);
    dsd_api_stop();
    dsd_app_frontend_runtime_stop();
    freeState(state);
    free(state);
    free(opts);
}

/*============================================================================
 * Scan avoids (issue #672): status.scan_avoids and the paged get
 *============================================================================*/

/* A decoder with an API session: one connection subscribed to status, to wait for the feed's ticks, and one with no
   subscription, whose next line is always the reply to its last request. */
typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    dsd_socket_t sub;
    dsd_socket_t get;
} avoid_rig;

/* Publish until a status line carries @p needle. The feed refreshes its scan avoid copy on the tick that sends status,
   just before it encodes the line, so once the line is seen every `get` answers from that copy or a newer one. */
static void
await_status(avoid_rig* rig, const char* needle) {
    for (int attempt = 0; attempt < 40; attempt++) {
        dsd_telemetry_publish_both_and_redraw(rig->opts, rig->state);
        if (await_line(rig->sub, "status", needle, 100U)) {
            return;
        }
    }
    DSD_FPRINTF(stderr, "status never carried %s\n", needle);
    assert(0);
}

/* One feed tick after every change made so far, shown by a synctype nothing else sets. */
static void
await_tick(avoid_rig* rig, int marker) {
    char needle[48];
    rig->state->synctype = marker;
    DSD_SNPRINTF(needle, sizeof needle, "\"synctype\":%d,", marker);
    await_status(rig, needle);
}

static dsd_json_node*
parse_line(void) {
    dsd_json_node* root = NULL;
    char err[96];
    if (dsd_json_parse(g_line, &root, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "not JSON (%s): %s\n", err, g_line);
        assert(0);
    }
    return root;
}

static void
send_get_scan_avoids(dsd_socket_t c, long long offset, long long limit) {
    char off[48] = "";
    char lim[48] = "";
    if (offset >= 0) {
        DSD_SNPRINTF(off, sizeof off, ",\"offset\":%lld", offset);
    }
    if (limit >= 0) {
        DSD_SNPRINTF(lim, sizeof lim, ",\"limit\":%lld", limit);
    }
    char request[160];
    DSD_SNPRINTF(request, sizeof request, "{\"cmd\":\"get\",\"params\":{\"what\":\"scan_avoids\"%s%s}}\n", off, lim);
    send_str(c, request);
}

/* `get scan_avoids` (an offset or limit below 0 is left out); returns the parsed reply, already checked to be ok. */
static dsd_json_node*
get_scan_avoids(avoid_rig* rig, long long offset, long long limit) {
    send_get_scan_avoids(rig->get, offset, limit);
    static const char k_head[] = "{\"id\":null,\"ok\":true,\"what\":\"scan_avoids\",\"data\":{";
    assert(read_line(rig->get, 5000U) == 1);
    if (strncmp(g_line, k_head, sizeof k_head - 1U) != 0) {
        DSD_FPRINTF(stderr, "get scan_avoids was answered: %s\n", g_line);
        assert(0);
    }
    return parse_line();
}

static void
expect_get_refused(avoid_rig* rig, const char* params) {
    char request[192];
    DSD_SNPRINTF(request, sizeof request, "{\"cmd\":\"get\",\"params\":{\"what\":\"scan_avoids\",%s}}\n", params);
    send_str(rig->get, request);
    assert(read_line(rig->get, 5000U) == 1);
    if (strstr(g_line, "\"ok\":false") == NULL || strstr(g_line, "\"invalid_params\"") == NULL) {
        DSD_FPRINTF(stderr, "get with %s was answered: %s\n", params, g_line);
        assert(0);
    }
}

static const dsd_json_node*
json_at(const dsd_json_node* n, const char* key) {
    const dsd_json_node* v = dsd_json_obj_get(n, key);
    if (v == NULL) {
        DSD_FPRINTF(stderr, "no \"%s\"\n", key);
        assert(0);
    }
    return v;
}

static uint64_t
json_u64(const dsd_json_node* n, const char* key) {
    uint64_t v = 0U;
    assert(dsd_json_as_u64(json_at(n, key), &v) == 0);
    return v;
}

static const char*
json_str(const dsd_json_node* n, const char* key) {
    const char* s = dsd_json_as_str(json_at(n, key));
    assert(s != NULL);
    return s;
}

/* A string node holding the decimal value @p v (how the API writes 64-bit identifiers). */
static void
expect_u64_str(const dsd_json_node* n, const char* key, uint64_t v) {
    char want[24];
    DSD_SNPRINTF(want, sizeof want, "%" PRIu64, v);
    assert(json_at(n, key)->type == DSD_JSON_STRING && strcmp(json_str(n, key), want) == 0);
}

static const dsd_json_node*
data_entries(const dsd_json_node* root, size_t* count) {
    const dsd_json_node* entries = json_at(json_at(root, "data"), "entries");
    assert(entries->type == DSD_JSON_ARRAY);
    *count = entries->count;
    return entries;
}

/* The status.scan_avoids object of an available -Y list of @p total avoided rows. */
static void
want_lcn_status(char* out, size_t cap, const dsd_state* state, size_t total) {
    DSD_SNPRINTF(out, cap,
                 "\"scan_avoids\":{\"available\":true,\"kind\":\"lcn\",\"total\":%zu,\"instance\":\"%" PRIu64
                 "\",\"list_rev\":\"%" PRIu64 "\",\"revision\":\"%" PRIu64 "\"}",
                 total, state->state_instance, state->lcn_list_rev, state->lcn_avoid_rev);
}

static void
want_trunk_status(char* out, size_t cap, const dsd_state* state, int available, size_t total) {
    char total_text[24] = "null";
    if (available) {
        DSD_SNPRINTF(total_text, sizeof total_text, "%zu", total);
    }
    DSD_SNPRINTF(out, cap,
                 "\"scan_avoids\":{\"available\":%s,\"kind\":\"trunk_scan\",\"total\":%s,\"instance\":\"%" PRIu64
                 "\",\"session\":%u,\"revision\":\"%" PRIu64 "\"}",
                 available ? "true" : "false", total_text, state->state_instance, (unsigned)state->trunk_scan_session,
                 state->trunk_scan_avoid_rev);
}

static long
lcn_test_hz(int row) {
    return 450000000L + (long)row * 12500L;
}

/* Replace the -Y list with @p rows rows named "<prefix> <row>", avoiding the rows @p avoid picks. */
static void
fill_lcn_list(dsd_state* state, int rows, const char* prefix, int (*avoid)(int row)) {
    dsd_state_trunk_lcn_replace_begin(state);
    assert(dsd_state_trunk_lcn_reserve(state, (size_t)rows) == 0);
    for (int row = 0; row < rows; row++) {
        char name[32];
        DSD_SNPRINTF(name, sizeof name, "%s %d", prefix, row);
        *dsd_state_trunk_lcn_slot(state, row) = lcn_test_hz(row);
        assert(dsd_state_trunk_lcn_name_set(state, (size_t)row, name) == 0);
    }
    state->lcn_freq_count = rows;
    for (int row = 0; row < rows; row++) {
        if (avoid(row)) {
            assert(dsd_state_trunk_lcn_avoid_set(state, (size_t)row, 1) == 0);
        }
    }
}

static int
avoid_every_97th(int row) {
    return row % 97 == 0;
}

/* Check one -Y entry against the row it must be: the identity scan_avoid_remove takes, the name and the view's label. */
static void
expect_lcn_entry(const dsd_json_node* e, const dsd_state* state, int row, const char* prefix) {
    char name[32];
    DSD_SNPRINTF(name, sizeof name, "%s %d", prefix, row);
    assert(strcmp(json_str(e, "kind"), "lcn") == 0);
    expect_u64_str(e, "instance", state->state_instance);
    expect_u64_str(e, "list_rev", state->lcn_list_rev);
    assert(json_u64(e, "row") == (uint64_t)row);
    assert(json_u64(e, "frequency_hz") == (uint64_t)lcn_test_hz(row));
    assert(strcmp(json_str(e, "name"), name) == 0);
}

/* A large sparse -Y list: every avoided row reached exactly once by paging, in row order, past the embedded rows. */
static void
scan_avoids_lcn_paging(avoid_rig* rig) {
    enum { ROWS = 20000, AVOIDED = 207 }; /* rows 0, 97, ... 19982 */

    fill_lcn_list(rig->state, ROWS, "Ch", avoid_every_97th);
    assert(rig->state->lcn_avoid_total == (size_t)AVOIDED);
    char want[320];
    want_lcn_status(want, sizeof want, rig->state, AVOIDED);
    await_status(rig, want);

    size_t seen = 0U;
    for (int page = 0;; page++) {
        /* The first page uses the defaults: offset 0, limit 64. */
        dsd_json_node* root = get_scan_avoids(rig, page == 0 ? -1 : (long long)seen, page == 0 ? -1 : 64);
        const dsd_json_node* data = json_at(root, "data");
        assert(json_at(data, "available")->boolean == 1 && strcmp(json_str(data, "kind"), "lcn") == 0);
        assert(json_u64(data, "total") == (uint64_t)AVOIDED);
        expect_u64_str(data, "instance", rig->state->state_instance);
        expect_u64_str(data, "list_rev", rig->state->lcn_list_rev);
        expect_u64_str(data, "revision", rig->state->lcn_avoid_rev);
        assert(json_u64(data, "offset") == seen && json_u64(data, "limit") == 64U);
        size_t count = 0U;
        const dsd_json_node* entries = data_entries(root, &count);
        assert(count == (AVOIDED - seen < 64U ? AVOIDED - seen : 64U));
        for (size_t i = 0; i < count; i++) {
            expect_lcn_entry(entries->items[i], rig->state, (int)(seen + i) * 97, "Ch");
        }
        seen += count;
        dsd_json_free(root);
        if (count < 64U) {
            break;
        }
    }
    assert(seen == (size_t)AVOIDED);

    /* One entry, labelled as every frontend lists it; the row number first. */
    dsd_json_node* root = get_scan_avoids(rig, 1, 1);
    size_t count = 0U;
    const dsd_json_node* entries = data_entries(root, &count);
    assert(count == 1U && strcmp(json_str(entries->items[0], "label"), "Row 98  451.212500 MHz  Ch 97") == 0);
    /* An entry's identity fields are scan_avoid_remove's params as they are. */
    char request[384];
    DSD_SNPRINTF(request, sizeof request,
                 "{\"id\":7,\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"lcn\",\"instance\":\"%s\","
                 "\"list_rev\":\"%s\",\"row\":97,\"frequency_hz\":451212500,\"name\":\"Ch 97\",\"label\":\"x\"}}\n",
                 json_str(entries->items[0], "instance"), json_str(entries->items[0], "list_rev"));
    dsd_json_free(root);
    send_str(rig->get, request);
    expect_line(rig->get, "{\"id\":7,\"ok\":true");

    /* Past the end: an empty page of the same list, never an error. */
    root = get_scan_avoids(rig, AVOIDED, -1);
    assert(json_u64(json_at(root, "data"), "total") == (uint64_t)AVOIDED);
    (void)data_entries(root, &count);
    assert(count == 0U);
    dsd_json_free(root);
    send_str(rig->get, "{\"cmd\":\"get\",\"params\":{\"what\":\"scan_avoids\",\"offset\":\"18446744073709551615\"}}\n");
    assert(read_line(rig->get, 5000U) == 1);
    root = parse_line();
    (void)data_entries(root, &count);
    assert(count == 0U);
    dsd_json_free(root);

    expect_get_refused(rig, "\"limit\":0");
    expect_get_refused(rig, "\"limit\":65");
    expect_get_refused(rig, "\"limit\":-1");
    expect_get_refused(rig, "\"limit\":1.5");
    expect_get_refused(rig, "\"offset\":-1");
    expect_get_refused(rig, "\"offset\":\"x\"");
    expect_get_refused(rig, "\"offset\":true");
}

static void
set_trunk_avoids(dsd_state* state, const dsd_trunk_scan_avoid_entry* entries, size_t count) {
    assert(dsd_state_trunk_scan_avoids_set(state, entries, count) == 0);
    state->trunk_scan_avoid_rev = dsd_state_rev_next();
}

static void
expect_trunk_entry(const dsd_json_node* e, uint32_t session, const char* id, uint64_t hz, const char* name,
                   const char* label) {
    assert(strcmp(json_str(e, "kind"), "trunk_scan") == 0);
    assert(json_at(e, "session")->type == DSD_JSON_NUMBER && json_u64(e, "session") == session);
    assert(strcmp(json_str(e, "target_id"), id) == 0 && json_u64(e, "frequency_hz") == hz);
    assert(strcmp(json_str(e, "name"), name) == 0 && strcmp(json_str(e, "label"), label) == 0);
}

/* A trunk scan's avoided targets; the copy is rebuilt only when the view's key moves. */
static void
scan_avoids_trunk_scan(avoid_rig* rig) {
    static const dsd_trunk_scan_avoid_entry k_targets[] = {
        {1U, 0U, 851012500U, "county-p25", "County Fire"},
        {3U, 0U, 0U, "state-p25", ""},
        {4U, 0U, 852000000U, "city-dmr", "City"},
    };
    rig->opts->trunk_scan_enabled = 1;
    rig->state->trunk_scan_session = 4000000000U;
    set_trunk_avoids(rig->state, k_targets, 3U);
    char want[320];
    want_trunk_status(want, sizeof want, rig->state, 1, 3U);
    await_status(rig, want);

    dsd_json_node* root = get_scan_avoids(rig, -1, -1);
    const dsd_json_node* data = json_at(root, "data");
    assert(json_at(data, "session")->type == DSD_JSON_NUMBER && json_u64(data, "session") == 4000000000U);
    assert(dsd_json_obj_get(data, "list_rev") == NULL);
    size_t count = 0U;
    const dsd_json_node* entries = data_entries(root, &count);
    assert(count == 3U);
    expect_trunk_entry(entries->items[0], 4000000000U, "county-p25", 851012500U, "County Fire",
                       "#1  county-p25  851.012500 MHz  County Fire");
    expect_trunk_entry(entries->items[1], 4000000000U, "state-p25", 0U, "", "#3  state-p25  frequency unknown");
    expect_trunk_entry(entries->items[2], 4000000000U, "city-dmr", 852000000U, "City",
                       "#4  city-dmr  852.000000 MHz  City");
    dsd_json_free(root);

    /* A change under an unchanged key is not collected: the feed did no work at that key. */
    DSD_SNPRINTF(rig->state->trunk_scan_avoids[0].label, sizeof rig->state->trunk_scan_avoids[0].label, "%s",
                 "Renamed");
    await_tick(rig, 9001);
    root = get_scan_avoids(rig, 0, 1);
    entries = data_entries(root, &count);
    assert(count == 1U && strcmp(json_str(entries->items[0], "name"), "County Fire") == 0);
    dsd_json_free(root);
    /* A new revision is. */
    rig->state->trunk_scan_avoid_rev = dsd_state_rev_next();
    want_trunk_status(want, sizeof want, rig->state, 1, 3U);
    await_status(rig, want);
    root = get_scan_avoids(rig, 0, 1);
    entries = data_entries(root, &count);
    assert(count == 1U && strcmp(json_str(entries->items[0], "name"), "Renamed") == 0);
    dsd_json_free(root);
}

/* status.scan_avoids and get agree on availability: both from the summary the feed holds. */
static void
expect_unavailable(avoid_rig* rig) {
    char want[320];
    want_trunk_status(want, sizeof want, rig->state, 0, 0U);
    await_status(rig, want);
    dsd_json_node* root = get_scan_avoids(rig, -1, -1);
    const dsd_json_node* data = json_at(root, "data");
    assert(json_at(data, "available")->boolean == 0 && json_at(data, "total")->type == DSD_JSON_NULL);
    expect_u64_str(data, "revision", rig->state->trunk_scan_avoid_rev);
    size_t count = 1U;
    (void)data_entries(root, &count);
    assert(count == 0U);
    dsd_json_free(root);
}

/* T2: list A collected, then revision B whose collection fails, then B again once it can be collected. A store the
   collection cannot read (a count with no list) fails dsd_app_scan_avoid_raw_collect() just as a failed allocation
   would. At each step status and get answer alike, and the failure never leaves A's entries served as current. */
static void
scan_avoids_failed_collection(avoid_rig* rig) {
    dsd_trunk_scan_avoid_entry* list = rig->state->trunk_scan_avoids;
    rig->state->trunk_scan_avoids = NULL;
    rig->state->trunk_scan_avoid_count = 2U;
    rig->state->trunk_scan_avoid_rev = dsd_state_rev_next();
    expect_unavailable(rig);
    /* Another tick at the same key: still unavailable, and A's entries do not come back. (That the feed retries at
       this key is shown below: the recovery happens at this same revision.) */
    await_tick(rig, 9002);
    expect_unavailable(rig);

    rig->state->trunk_scan_avoids = list;
    char want[320];
    want_trunk_status(want, sizeof want, rig->state, 1, 2U);
    await_status(rig, want);
    dsd_json_node* root = get_scan_avoids(rig, -1, -1);
    expect_u64_str(json_at(root, "data"), "revision", rig->state->trunk_scan_avoid_rev);
    size_t count = 0U;
    const dsd_json_node* entries = data_entries(root, &count);
    assert(count == 2U && strcmp(json_str(entries->items[1], "target_id"), "state-p25") == 0);
    dsd_json_free(root);

    /* The coordinator could not publish its list: unavailable until it can. */
    rig->state->trunk_scan_avoids_ok = 0U;
    rig->state->trunk_scan_avoid_rev = dsd_state_rev_next();
    expect_unavailable(rig);
    rig->state->trunk_scan_avoids_ok = 1U;
    rig->state->trunk_scan_avoid_rev = dsd_state_rev_next();
    want_trunk_status(want, sizeof want, rig->state, 1, 2U);
    await_status(rig, want);
}

static int
avoid_row_1(int row) {
    return row == 1;
}

/* A decoder restart (a new state instance) replaces the copy, even for the same list. */
static void
scan_avoids_restart(avoid_rig* rig) {
    rig->opts->trunk_scan_enabled = 0;
    fill_lcn_list(rig->state, 3, "Ch", avoid_row_1);
    char want[320];
    want_lcn_status(want, sizeof want, rig->state, 1U);
    await_status(rig, want);
    const uint64_t before = rig->state->state_instance;

    /* The restart lands right after a tick, and one publish follows: the feed refreshes at that publish instead of
       serving the old instance's entries until the next 250 ms update. */
    freeState(rig->state);
    initState(rig->state);
    assert(rig->state->state_instance != before);
    fill_lcn_list(rig->state, 3, "Ch", avoid_row_1);
    dsd_telemetry_publish_both_and_redraw(rig->opts, rig->state);
    dsd_json_node* root = get_scan_avoids(rig, -1, -1);
    expect_u64_str(json_at(root, "data"), "instance", rig->state->state_instance);
    size_t count = 0U;
    const dsd_json_node* entries = data_entries(root, &count);
    assert(count == 1U);
    expect_lcn_entry(entries->items[0], rig->state, 1, "Ch");
    dsd_json_free(root);
    /* status refreshed at the same publish. */
    want_lcn_status(want, sizeof want, rig->state, 1U);
    assert(await_line(rig->sub, "status", want, 3000U));

    /* A list shrunk past an avoided row without a recount: the total is the entries a page reaches, not the stale
       count (lcn_avoid_total still says 2). */
    assert(dsd_state_trunk_lcn_avoid_set(rig->state, 2U, 1) == 0);
    rig->state->lcn_freq_count = 2;
    rig->state->lcn_avoid_rev = dsd_state_rev_next();
    assert(rig->state->lcn_avoid_total == 2U);
    want_lcn_status(want, sizeof want, rig->state, 1U);
    await_status(rig, want);
    root = get_scan_avoids(rig, -1, -1);
    assert(json_u64(json_at(root, "data"), "total") == 1U);
    (void)data_entries(root, &count);
    assert(count == 1U);
    dsd_json_free(root);
}

static int
avoid_even_below_300(int row) {
    return row < 300 && row % 2 == 0;
}

static int
avoid_1_mod_4(int row) {
    return row % 4 == 1;
}

/* One page served while the list may have changed: whole, and of one list only. */
static void
expect_consistent_page(long long offset) {
    dsd_json_node* root = parse_line();
    const dsd_json_node* data = json_at(root, "data");
    const uint64_t total = json_u64(data, "total");
    assert(total == 150U || total == 100U);
    const char* prefix = total == 150U ? "X" : "Y";
    size_t count = 0U;
    const dsd_json_node* entries = data_entries(root, &count);
    const uint64_t left = total > (uint64_t)offset ? total - (uint64_t)offset : 0U;
    assert(count == (left < 64U ? left : 64U));
    for (size_t i = 0; i < count; i++) {
        const uint64_t k = (uint64_t)offset + i;
        const int row = total == 150U ? (int)(k * 2U) : (int)(k * 4U + 1U);
        char name[32];
        DSD_SNPRINTF(name, sizeof name, "%s %d", prefix, row);
        assert(json_u64(entries->items[i], "row") == (uint64_t)row);
        assert(strcmp(json_str(entries->items[i], "name"), name) == 0);
        assert(strcmp(json_str(entries->items[i], "list_rev"), json_str(data, "list_rev")) == 0);
    }
    dsd_json_free(root);
}

/* Pages requested while the decode thread swaps in new lists: every reply is formatted from one immutable copy (run
   under asan/tsan, a copy freed or rewritten under a reader shows). */
static void
scan_avoids_concurrent_paging(avoid_rig* rig) {
    static const long long k_offsets[] = {0, 40, 64, 99, 128, 149};

    enum { OFFSETS = (int)(sizeof k_offsets / sizeof k_offsets[0]) };

    fill_lcn_list(rig->state, 400, "X", avoid_even_below_300);
    char want[320];
    want_lcn_status(want, sizeof want, rig->state, 150U);
    await_status(rig, want);
    for (int round = 1; round <= 10; round++) {
        if (round % 2 == 0) {
            fill_lcn_list(rig->state, 400, "X", avoid_even_below_300);
        } else {
            fill_lcn_list(rig->state, 400, "Y", avoid_1_mod_4);
        }
        /* Past the feed's interval, so the next publish collects the new list. */
        dsd_sleep_ms(260);
        for (int i = 0; i < OFFSETS; i++) {
            send_get_scan_avoids(rig->get, k_offsets[i], 64);
        }
        dsd_telemetry_publish_both_and_redraw(rig->opts, rig->state);
        for (int i = 0; i < OFFSETS; i++) {
            assert(read_line(rig->get, 5000U) == 1);
            expect_consistent_page(k_offsets[i]);
        }
    }
}

/* The largest page -- 64 entries whose ids and labels are all control bytes, each escaped to six -- stays a small
   fraction of the 1 MiB backlog a client may have, so paging never costs a client its connection. */
static void
scan_avoids_largest_page(avoid_rig* rig) {
    enum { TARGETS = 64, OUTBOX_BYTES = 1 << 20 };

    dsd_trunk_scan_avoid_entry* list = (dsd_trunk_scan_avoid_entry*)calloc(TARGETS, sizeof(*list));
    assert(list != NULL);
    for (uint32_t i = 0; i < TARGETS; i++) {
        list[i].ordinal = i + 1U;
        list[i].frequency_hz = 851012500U;
        DSD_MEMSET(list[i].id, 0x01, sizeof list[i].id - 1U);
        DSD_MEMSET(list[i].label, 0x02, sizeof list[i].label - 1U);
    }
    rig->opts->trunk_scan_enabled = 1;
    set_trunk_avoids(rig->state, list, TARGETS);
    free(list);
    char want[320];
    want_trunk_status(want, sizeof want, rig->state, 1, TARGETS);
    await_status(rig, want);
    dsd_json_node* root = get_scan_avoids(rig, -1, -1);
    size_t count = 0U;
    (void)data_entries(root, &count);
    assert(count == (size_t)TARGETS);
    /* About 102 KiB: under an eighth of the backlog. */
    assert(strlen(g_line) < (size_t)OUTBOX_BYTES / 8U);
    dsd_json_free(root);
}

static void
test_scan_avoids(void) {
    avoid_rig rig;
    rig.opts = (dsd_opts*)calloc(1U, sizeof(*rig.opts));
    rig.state = (dsd_state*)calloc(1U, sizeof(*rig.state));
    assert(rig.opts != NULL && rig.state != NULL);
    initOpts(rig.opts);
    initState(rig.state);
    dsd_app_frontend_runtime_start(rig.opts, rig.state);
    const int port = start_server(NULL, 0, 0);
    rig.sub = open_session(port, "\"type\":\"welcome\"");
    send_str(rig.sub, "{\"id\":1,\"cmd\":\"subscribe\",\"topics\":[\"status\"]}\n");
    expect_line(rig.sub, "{\"id\":1,\"ok\":true");
    rig.get = open_session(port, "\"type\":\"welcome\"");

    /* No scan and no channel map: nothing to avoid, and nothing is listed. */
    char want[256];
    DSD_SNPRINTF(want, sizeof want,
                 "\"scan_avoids\":{\"available\":true,\"kind\":\"none\",\"total\":0,\"instance\":\"%" PRIu64
                 "\",\"revision\":\"0\"}",
                 rig.state->state_instance);
    await_status(&rig, want);
    dsd_json_node* root = get_scan_avoids(&rig, -1, -1);
    assert(strcmp(json_str(json_at(root, "data"), "kind"), "none") == 0);
    size_t count = 1U;
    (void)data_entries(root, &count);
    assert(count == 0U);
    dsd_json_free(root);

    scan_avoids_lcn_paging(&rig);
    scan_avoids_trunk_scan(&rig);
    scan_avoids_failed_collection(&rig);
    scan_avoids_restart(&rig);
    scan_avoids_concurrent_paging(&rig);
    scan_avoids_largest_page(&rig);

    /* With no client connected the feed keeps no copy and no cached record, so a client connecting later is never
       served the list or the status the last one saw as current: null until the first update, which the next publish
       runs. */
    dsd_socket_close(rig.get);
    dsd_socket_close(rig.sub);
    for (int i = 0; i < 500 && dsd_api_client_count() > 0; i++) {
        dsd_sleep_ms(10);
    }
    assert(dsd_api_client_count() == 0);
    dsd_telemetry_publish_both_and_redraw(rig.opts, rig.state);
    rig.get = open_session(port, "\"type\":\"welcome\"");
    send_get_scan_avoids(rig.get, -1, -1);
    expect_line(rig.get, "{\"id\":null,\"ok\":true,\"what\":\"scan_avoids\",\"data\":null}");
    send_str(rig.get, "{\"id\":2,\"cmd\":\"get\",\"params\":{\"what\":\"status\"}}\n");
    expect_line(rig.get, "{\"id\":2,\"ok\":true,\"what\":\"status\",\"data\":null}");
    dsd_telemetry_publish_both_and_redraw(rig.opts, rig.state);
    root = get_scan_avoids(&rig, 0, 1);
    (void)data_entries(root, &count);
    assert(count == 1U);
    dsd_json_free(root);

    dsd_api_stop();
    dsd_socket_close(rig.get);
    /* The copy went with the feed: a reconnecting client sees nothing until the feed has looked again. */
    const int port2 = start_server(NULL, 0, 0);
    rig.get = open_session(port2, "\"type\":\"welcome\"");
    send_get_scan_avoids(rig.get, -1, -1);
    expect_line(rig.get, "{\"id\":null,\"ok\":true,\"what\":\"scan_avoids\",\"data\":null}");
    dsd_socket_close(rig.get);
    dsd_api_stop();
    dsd_app_frontend_runtime_stop();
    freeState(rig.state);
    free(rig.state);
    free(rig.opts);
}

/* A request still queued when the decoder stops is cancelled as the command session closes; the server, stopped right
   after (as the CLI does, most likely while its result pump sleeps), still reports that before it disconnects. */
static void
test_results_at_stop(void) {
    dsd_app_frontend_runtime_start(NULL, NULL);
    const int port = start_server(NULL, 0, 0);
    dsd_socket_t c = open_session(port, "\"type\":\"welcome\"");
    send_str(c, "{\"id\":1,\"cmd\":\"subscribe\",\"topics\":[\"result\"]}\n");
    expect_line(c, "{\"id\":1,\"ok\":true");
    /* No decoder drains the queue here: the request stays queued. */
    send_str(c, "{\"id\":2,\"cmd\":\"decryption_apply\",\"params\":{\"source\":0}}\n");
    expect_line(c, "{\"id\":2,\"ok\":true");
    const char* id_at = strstr(g_line, "\"request_id\":\"");
    assert(id_at != NULL);
    char request_id[64];
    DSD_SNPRINTF(request_id, sizeof request_id, "%s", id_at);
    char* id_end = strchr(request_id + strlen("\"request_id\":\""), '"');
    assert(id_end != NULL);
    id_end[1] = '\0';

    dsd_app_frontend_runtime_stop();
    dsd_api_stop();
    assert(await_line(c, "result", request_id, 3000U));
    assert(strstr(g_line, "\"command\":\"decryption_apply\"") != NULL);
    assert(strstr(g_line, "\"status\":-6,\"ok\":false") != NULL);
    expect_eof(c);
    dsd_socket_close(c);
}

int
main(void) {
    assert(dsd_socket_init() == 0);
    test_lifecycle();
    test_session_commands();
    test_rejects_non_json();
    test_client_slots_are_reclaimed();
    test_vanishing_peer();
    test_authentication();
    test_feed();
    test_rr_apply_outcome_in_status_feed();
    test_scan_avoids();
    test_results_at_stop();
    printf("api server tests passed\n");
    return 0;
}
