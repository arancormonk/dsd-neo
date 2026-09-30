// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #589: both rigctl reconnect paths (the '9' key and the menu's host/port entry) go through
 * svc_rigctl_connect(). A reconnect opens the new socket while the old one is still open, hands it what the old socket
 * knew of its peer (RigctlRebindPeer(), the same peer when the host and port are), closes the old socket and forgets
 * the engine's legacy tune cache. A reconnect that fails while a connection is live changes nothing.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/platform/sockets.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "services.h"

/* The calls the service made, in order: C Connect(), R RigctlRebindPeer(), X dsd_socket_close(), F the engine forget. */
static char g_calls[16];
static size_t g_call_count = 0;

static dsd_socket_t g_connect_result = DSD_INVALID_SOCKET;
static char g_connect_host[256];
static int g_connect_port = 0;

static dsd_socket_t g_rebind_old = 0;
static dsd_socket_t g_rebind_new = 0;
static int g_rebind_same = -1;

static dsd_socket_t g_closed_sock = 0;

static void
note_call(char call) {
    if (g_call_count + 1U < sizeof g_calls) {
        g_calls[g_call_count++] = call;
        g_calls[g_call_count] = '\0';
    }
}

static void
reset_fakes(void) {
    DSD_MEMSET(g_calls, 0, sizeof g_calls);
    g_call_count = 0;
    g_connect_result = DSD_INVALID_SOCKET;
    g_connect_host[0] = '\0';
    g_connect_port = 0;
    g_rebind_old = 0;
    g_rebind_new = 0;
    g_rebind_same = -1;
    g_closed_sock = 0;
}

dsd_socket_t
Connect(char* hostname, int portno) {
    note_call('C');
    DSD_SNPRINTF(g_connect_host, sizeof g_connect_host, "%s", hostname ? hostname : "");
    g_connect_port = portno;
    return g_connect_result;
}

void
RigctlRebindPeer(dsd_socket_t old_fd, dsd_socket_t new_fd, int same_endpoint) {
    note_call('R');
    g_rebind_old = old_fd;
    g_rebind_new = new_fd;
    g_rebind_same = same_endpoint;
}

int
dsd_socket_close(dsd_socket_t sock) {
    note_call('X');
    g_closed_sock = sock;
    return 0;
}

void
dsd_engine_rigctl_tune_cache_forget(void) {
    note_call('F');
}

/* A session connected to @p host:@p port over socket @p fd (use_rigctl set when @p live). */
static void
seed_opts(dsd_opts* opts, const char* host, int port, dsd_socket_t fd, int live) {
    DSD_MEMSET(opts, 0, sizeof *opts);
    DSD_SNPRINTF(opts->rigctlhostname, sizeof opts->rigctlhostname, "%s", host);
    opts->rigctlportno = port;
    opts->rigctl_sockfd = fd;
    opts->use_rigctl = live;
}

static void
expect_reconnected(const dsd_opts* opts, const char* host, int port, dsd_socket_t fd) {
    assert(strcmp(opts->rigctlhostname, host) == 0);
    assert(opts->rigctlportno == port);
    assert(opts->rigctl_sockfd == fd);
    assert(opts->use_rigctl == 1);
}

/* The same host and port: the new socket takes the peer's record before the old one is closed. */
static void
test_same_endpoint_hands_the_record_over_then_closes(void) {
    static dsd_opts opts;
    reset_fakes();
    seed_opts(&opts, "localhost", 4532, (dsd_socket_t)41, 1);
    g_connect_result = (dsd_socket_t)42;
    assert(svc_rigctl_connect(&opts, "localhost", 4532) == 0);
    assert(strcmp(g_calls, "CRXF") == 0);
    assert(strcmp(g_connect_host, "localhost") == 0 && g_connect_port == 4532);
    assert(g_rebind_old == (dsd_socket_t)41 && g_rebind_new == (dsd_socket_t)42 && g_rebind_same == 1);
    assert(g_closed_sock == (dsd_socket_t)41);
    expect_reconnected(&opts, "localhost", 4532, (dsd_socket_t)42);

    /* Host names do not differ by case. */
    reset_fakes();
    seed_opts(&opts, "localhost", 4532, (dsd_socket_t)41, 1);
    g_connect_result = (dsd_socket_t)42;
    assert(svc_rigctl_connect(&opts, "LocalHost", 4532) == 0);
    assert(g_rebind_same == 1);
    expect_reconnected(&opts, "LocalHost", 4532, (dsd_socket_t)42);
}

/* Another host or another port may be another peer: the record is handed over as such, and the old socket closed. */
static void
test_another_endpoint_closes_the_old_socket(void) {
    static dsd_opts opts;
    reset_fakes();
    seed_opts(&opts, "localhost", 4532, (dsd_socket_t)41, 1);
    g_connect_result = (dsd_socket_t)42;
    assert(svc_rigctl_connect(&opts, "10.0.0.2", 4532) == 0);
    assert(strcmp(g_calls, "CRXF") == 0);
    assert(g_rebind_old == (dsd_socket_t)41 && g_rebind_new == (dsd_socket_t)42 && g_rebind_same == 0);
    assert(g_closed_sock == (dsd_socket_t)41);
    expect_reconnected(&opts, "10.0.0.2", 4532, (dsd_socket_t)42);

    reset_fakes();
    seed_opts(&opts, "localhost", 4532, (dsd_socket_t)41, 1);
    g_connect_result = (dsd_socket_t)42;
    assert(svc_rigctl_connect(&opts, "localhost", 7356) == 0);
    assert(g_rebind_same == 0 && g_closed_sock == (dsd_socket_t)41);
    expect_reconnected(&opts, "localhost", 7356, (dsd_socket_t)42);
}

/* With no live connection there is nothing to hand over and nothing to close: no socket (a failed start), rigctl off,
 * or opts zeroed as tests leave them, whose socket 0 is never closed. */
static void
test_first_connection_closes_nothing(void) {
    static dsd_opts opts;
    reset_fakes();
    seed_opts(&opts, "localhost", 4532, DSD_INVALID_SOCKET, 0);
    g_connect_result = (dsd_socket_t)42;
    assert(svc_rigctl_connect(&opts, "localhost", 4532) == 0);
    assert(strcmp(g_calls, "CRF") == 0);
    assert(g_rebind_old == DSD_INVALID_SOCKET && g_rebind_new == (dsd_socket_t)42 && g_rebind_same == 0);
    expect_reconnected(&opts, "localhost", 4532, (dsd_socket_t)42);

    reset_fakes();
    seed_opts(&opts, "localhost", 4532, (dsd_socket_t)41, 0);
    g_connect_result = (dsd_socket_t)42;
    assert(svc_rigctl_connect(&opts, "localhost", 4532) == 0);
    assert(strcmp(g_calls, "CRF") == 0 && g_rebind_old == DSD_INVALID_SOCKET);

    reset_fakes();
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.use_rigctl = 1;
    g_connect_result = (dsd_socket_t)42;
    assert(svc_rigctl_connect(&opts, "localhost", 4532) == 0);
    assert(strcmp(g_calls, "CRF") == 0 && g_rebind_old == DSD_INVALID_SOCKET);
    expect_reconnected(&opts, "localhost", 4532, (dsd_socket_t)42);
}

/* A reconnect that fails while a connection is live changes nothing: the connection in use stays, with its peer
 * record, so a scan's restore still reaches the peer it changed. */
static void
test_failed_reconnect_keeps_the_live_connection(void) {
    static dsd_opts opts;
    reset_fakes();
    seed_opts(&opts, "localhost", 4532, (dsd_socket_t)41, 1);
    g_connect_result = DSD_INVALID_SOCKET;
    assert(svc_rigctl_connect(&opts, "bad.example", 4533) == -1);
    assert(strcmp(g_calls, "C") == 0);
    assert(strcmp(g_connect_host, "bad.example") == 0 && g_connect_port == 4533);
    expect_reconnected(&opts, "localhost", 4532, (dsd_socket_t)41);
}

/* Without a live connection a failed connect records the endpoint for the next attempt and leaves rigctl off. */
static void
test_failed_first_connection_records_the_endpoint(void) {
    static dsd_opts opts;
    reset_fakes();
    seed_opts(&opts, "old.example", 4000, DSD_INVALID_SOCKET, 0);
    g_connect_result = DSD_INVALID_SOCKET;
    assert(svc_rigctl_connect(&opts, "rig.local", 4532) == -1);
    assert(strcmp(g_calls, "C") == 0);
    assert(strcmp(opts.rigctlhostname, "rig.local") == 0);
    assert(opts.rigctlportno == 4532);
    assert(opts.rigctl_sockfd == DSD_INVALID_SOCKET);
    assert(opts.use_rigctl == 0);

    reset_fakes();
    assert(svc_rigctl_connect(&opts, "localhost", 0) == -1);
    assert(svc_rigctl_connect(&opts, NULL, 4532) == -1);
    assert(svc_rigctl_connect(NULL, "localhost", 4532) == -1);
    assert(g_call_count == 0);
    assert(strcmp(opts.rigctlhostname, "rig.local") == 0);
}

int
main(void) {
    test_same_endpoint_hands_the_record_over_then_closes();
    test_another_endpoint_closes_the_old_socket();
    test_first_connection_closes_nothing();
    test_failed_reconnect_keeps_the_live_connection();
    test_failed_first_connection_records_the_endpoint();
    return 0;
}
