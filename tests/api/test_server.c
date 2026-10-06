// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Loopback integration tests for the TCP JSON Lines API server: framing, session commands, authentication, the
 * connection limits and their reclamation, refusal of non-JSON (cross-protocol) input, peers that vanish mid-send,
 * and the telemetry feed driven through a real frontend runtime.
 */

#include <assert.h>
#include <dsd-neo/api/api.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !DSD_PLATFORM_WIN_NATIVE
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

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
    /* A third concurrent client is told why, then dropped. */
    dsd_socket_t full = connect_port(port);
    expect_line(full, "\"busy\"");
    expect_eof(full);
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

    /* A row enriched in place after it was sent -- here the older of two, as a late alias lands on a call a newer
       notice already followed -- is sent again, marked as an update, under the same identity. */
    commit_event(state, "Notice", 10U);
    dsd_telemetry_publish_both_and_redraw(opts, state);
    assert(await_line(c, "event", "\"t_name\":\"Notice\"", 3000U));
    Event_History* older = &state->event_history_s[0].Event_History_Items[2];
    assert(strcmp(older->t_name, "Dispatch") == 0);
    DSD_SNPRINTF(older->alias, sizeof older->alias, "%s", "Late Alias");
    state->event_history_s[0].commit_rev++;
    dsd_telemetry_publish_both_and_redraw(opts, state);
    assert(await_line(c, "event", "\"alias\":\"Late Alias\"", 3000U));
    assert(strstr(g_line, "\"update\":true") != NULL && strstr(g_line, "\"t_name\":\"Dispatch\"") != NULL);

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

    dsd_socket_close(c);
    dsd_api_stop();
    /* The observer is gone: publishing now reaches no feed. */
    dsd_telemetry_publish_both_and_redraw(opts, state);
    dsd_app_frontend_runtime_stop();
    freeState(state);
    free(state);
    free(opts);
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
    printf("api server tests passed\n");
    return 0;
}
