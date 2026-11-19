// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The built-in libcurl transport against a loopback server, with the real libcurl's header and size handling.
 *
 * tile_fetch.c is compiled into this test with DSD_TILE_FETCH_TEST_PLAIN_HTTP, so the transport speaks plain http to
 * 127.0.0.1 (the shipped library is https only, and a test has no certificate the system trusts). Everything else is
 * the production path: options, caps, the header and body callbacks, the session's share handle.
 *
 * What it pins: an answer over the body cap still says what it was. With a declared Content-Length over the cap,
 * libcurl stops with CURLE_FILESIZE_EXCEEDED, and libcurl 8.5 and older stop before the header block's closing
 * blank line reaches the callback (7.65 delivers only the status line). The status and every header line libcurl
 * delivered, Retry-After included, must survive, so the tile worker backs off for as long as the server asked.
 */

#include <arpa/inet.h>
#include <curl/curl.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/git_ver.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/tile_fetch.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "test_env.h"

/* tile_fetch.c and curl_common.c are compiled on their own here; the runtime library normally supplies these. */
const char GIT_TAG[] = "v0.0.0-test";

void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    (void)level;
    (void)format;
}

static int g_failures = 0;

static void
expect(const char* what, int ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

/* One connection, one scripted answer: the header block, then up to body_bytes of 'x', then close. */
typedef struct {
    dsd_socket_t listen_sock;
    dsd_thread_t thread;
    const char* head;
    size_t body_bytes;
    int accepted;
} answer_server;

static void
drain_request(dsd_socket_t client) {
    char buf[2048];
    size_t used = 0;
    while (used + 1U < sizeof(buf)) {
        const int n = dsd_socket_recv(client, buf + used, sizeof(buf) - 1U - used, 0);
        if (n <= 0) {
            return;
        }
        used += (size_t)n;
        buf[used] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL) {
            return;
        }
    }
}

static DSD_THREAD_RETURN_TYPE
answer_server_thread(void* arg) {
    answer_server* s = (answer_server*)arg;
    dsd_socket_t client = dsd_socket_accept(s->listen_sock, NULL, NULL);
    if (client == DSD_INVALID_SOCKET) {
        DSD_THREAD_RETURN;
    }
    s->accepted = 1;
    (void)dsd_socket_set_recv_timeout(client, 5000U);
    (void)dsd_socket_set_send_timeout(client, 5000U);
    drain_request(client);
    (void)dsd_socket_send(client, s->head, strlen(s->head), 0);
    static char chunk[16384];
    DSD_MEMSET(chunk, 'x', sizeof(chunk));
    size_t left = s->body_bytes;
    while (left > 0U) {
        const size_t n = left < sizeof(chunk) ? left : sizeof(chunk);
        if (dsd_socket_send(client, chunk, n, 0) <= 0) {
            break; /* libcurl closed the connection once the cap was hit */
        }
        left -= n;
    }
    (void)dsd_socket_shutdown(client, SHUT_RDWR);
    (void)dsd_socket_close(client);
    DSD_THREAD_RETURN;
}

static int
answer_server_start(answer_server* s, char* url, size_t url_cap) {
    s->listen_sock = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (s->listen_sock == DSD_INVALID_SOCKET) {
        return -1;
    }
    (void)dsd_socket_set_recv_timeout(s->listen_sock, 10000U);
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t addr_len = (socklen_t)sizeof(addr);
    if (dsd_socket_bind(s->listen_sock, (const struct sockaddr*)&addr, (int)sizeof(addr)) != 0
        || dsd_socket_listen(s->listen_sock, 1) != 0
        || getsockname(s->listen_sock, (struct sockaddr*)&addr, &addr_len) != 0) {
        (void)dsd_socket_close(s->listen_sock);
        return -1;
    }
    const int n = DSD_SNPRINTF(url, url_cap, "http://127.0.0.1:%u/7/64/42.png", (unsigned int)ntohs(addr.sin_port));
    if (n <= 0 || (size_t)n >= url_cap || dsd_thread_create(&s->thread, answer_server_thread, s) != 0) {
        (void)dsd_socket_close(s->listen_sock);
        return -1;
    }
    return 0;
}

/* Fetch once from a server answering @p head plus @p body_bytes. */
static dsd_tile_fetch_status
fetch_answer(const char* head, size_t body_bytes, dsd_tile_fetch_session* session, dsd_tile_fetch_response* resp) {
    answer_server server;
    DSD_MEMSET(&server, 0, sizeof(server));
    server.head = head;
    server.body_bytes = body_bytes;
    char url[128];
    if (answer_server_start(&server, url, sizeof(url)) != 0) {
        expect("loopback server started", 0);
        DSD_MEMSET(resp, 0, sizeof(*resp));
        return DSD_TILE_FETCH_ERR_SETUP;
    }
    dsd_tile_fetch_request req;
    DSD_MEMSET(&req, 0, sizeof(req));
    req.url = url;
    req.session = session;
    req.connect_timeout_ms = 5000;
    req.total_timeout_ms = 10000;
    const dsd_tile_fetch_status st = dsd_tile_fetch(&req, resp);
    (void)dsd_thread_join(server.thread);
    (void)dsd_socket_close(server.listen_sock);
    expect("curl reached the loopback server", server.accepted == 1);
    return st;
}

static unsigned int
loaded_version(void) {
    /* Single-threaded test, after dsd_tile_fetch_available() ran libcurl's global init. */
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    return info != NULL ? info->version_num : 0U;
}

/* A declared Content-Length over the body cap: CURLE_FILESIZE_EXCEEDED, the status and Retry-After kept. */
static void
test_declared_length_over_cap(dsd_tile_fetch_session* session) {
    static const char kHead[] = "HTTP/1.1 503 Service Unavailable\r\n"
                                "Content-Length: 600000\r\n"
                                "Retry-After: 30\r\n"
                                "\r\n";
    dsd_tile_fetch_response resp;
    const dsd_tile_fetch_status st = fetch_answer(kHead, 600000U, session, &resp);
    expect("a declared length over the cap is too large", st == DSD_TILE_FETCH_ERR_TOO_LARGE);
    expect("with its 503", resp.http_status == 503);
    if (loaded_version() >= 0x075100U) {
        /* 7.81 and later hand every header line over before they act on the declared length (8.5 and 7.81 all but
         * the closing blank line, 8.23 the whole block). 7.65 stops after the status line: there is no Retry-After to
         * keep then, and the 503 alone still backs off. Versions between were not measured. */
        expect("and the Retry-After libcurl delivered",
               strcmp(resp.retry_after, "30") == 0 && (resp.header_present & DSD_TILE_FETCH_HDR_RETRY_AFTER) != 0U);
    } else {
        DSD_FPRINTF(stderr, "NOTE: libcurl %#x delivers no header line past the status line here\n", loaded_version());
    }
    dsd_tile_fetch_response_free(&resp);
}

/* No declared length: the body callback refuses at the cap, after the whole header block. */
static void
test_streamed_body_over_cap(dsd_tile_fetch_session* session) {
    static const char kHead[] = "HTTP/1.1 429 Too Many Requests\r\n"
                                "Retry-After: 120\r\n"
                                "Connection: close\r\n"
                                "\r\n";
    dsd_tile_fetch_response resp;
    const dsd_tile_fetch_status st = fetch_answer(kHead, DSD_TILE_FETCH_BODY_CAP_BYTES + 65536U, session, &resp);
    expect("a streamed body over the cap is too large", st == DSD_TILE_FETCH_ERR_TOO_LARGE);
    expect("with its 429", resp.http_status == 429);
    expect("and its Retry-After", strcmp(resp.retry_after, "120") == 0);
    dsd_tile_fetch_response_free(&resp);
}

/* An oversized tile is still an oversized tile: a 200 with a declared length over the cap. */
static void
test_oversized_tile(dsd_tile_fetch_session* session) {
    static const char kHead[] = "HTTP/1.1 200 OK\r\nContent-Length: 600000\r\nContent-Type: image/png\r\n\r\n";
    dsd_tile_fetch_response resp;
    const dsd_tile_fetch_status st = fetch_answer(kHead, 600000U, session, &resp);
    expect("an oversized tile is too large", st == DSD_TILE_FETCH_ERR_TOO_LARGE);
    expect("with its 200", resp.http_status == 200);
    expect("and no body handed out", resp.body == NULL);
    dsd_tile_fetch_response_free(&resp);
}

int
main(void) {
    /* The server keeps sending after libcurl hangs up at the cap: a write to that socket must fail, not kill us. */
    (void)signal(SIGPIPE, SIG_IGN);
    /* Loopback only: a proxy the runner exports must not take these requests elsewhere. */
    (void)dsd_test_setenv("no_proxy", "*", 1);
    (void)dsd_test_setenv("NO_PROXY", "*", 1);
    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "FAIL: sockets\n");
        return 1;
    }
    if (!dsd_tile_fetch_available()) {
        DSD_FPRINTF(stderr, "SKIP: the built-in transport is unavailable (no http or no asynchronous resolver)\n");
        dsd_socket_cleanup();
        return 0;
    }
    test_declared_length_over_cap(NULL);
    test_streamed_body_over_cap(NULL);
    test_oversized_tile(NULL);
    /* The same through a session, as the tile worker fetches. */
    dsd_tile_fetch_session* session = dsd_tile_fetch_session_create();
    test_declared_length_over_cap(session);
    test_streamed_body_over_cap(session);
    test_oversized_tile(session);
    dsd_tile_fetch_session_destroy(session);
    dsd_socket_cleanup();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stdout, "RUNTIME_TILE_FETCH_PLAIN_HTTP: all checks passed\n");
    return 0;
}
