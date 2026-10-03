// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Loopback integration test for the TCP JSON Lines API server. */

#include <assert.h>
#include <dsd-neo/api/api.h>
#include <dsd-neo/api/json.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/timing.h>
#include <stdio.h>
#include <string.h>

#if !DSD_PLATFORM_WIN_NATIVE
#include <netinet/in.h>
#endif

static int
read_line(dsd_socket_t sock, char* out, size_t cap) {
    size_t used = 0;
    while (used + 1U < cap) {
        char c = '\0';
        const int n = dsd_socket_recv(sock, &c, 1U, 0);
        if (n <= 0) {
            break;
        }
        out[used++] = c;
        if (c == '\n') {
            break;
        }
    }
    out[used] = '\0';
    return used > 0U ? 0 : -1;
}

static int
send_str(dsd_socket_t sock, const char* s) {
    const size_t len = strlen(s);
    size_t sent = 0;
    while (sent < len) {
        const int n = dsd_socket_send(sock, s + sent, len - sent, 0);
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static dsd_socket_t
connect_port(int port) {
    struct sockaddr_in addr;
    if (dsd_socket_resolve("127.0.0.1", port, &addr) != 0) {
        return DSD_INVALID_SOCKET;
    }
    dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == DSD_INVALID_SOCKET) {
        return DSD_INVALID_SOCKET;
    }
    if (dsd_socket_connect(sock, (struct sockaddr*)&addr, (int)sizeof(addr)) != 0) {
        dsd_socket_close(sock);
        return DSD_INVALID_SOCKET;
    }
    (void)dsd_socket_set_recv_timeout(sock, 2000);
    return sock;
}

int
main(void) {
    dsd_api_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.bind_addr[0] = '\0';

    int port = 39411;
    dsd_socket_t client = DSD_INVALID_SOCKET;
    for (int attempt = 0; attempt < 20 && client == DSD_INVALID_SOCKET; attempt++) {
        cfg.port = port + attempt;
        if (dsd_api_start(&cfg) != 0) {
            continue;
        }
        client = connect_port(cfg.port);
        if (client == DSD_INVALID_SOCKET) {
            dsd_api_stop();
        }
    }
    assert(dsd_api_is_running());
    assert(client != DSD_INVALID_SOCKET);

    char line[65536];

    assert(read_line(client, line, sizeof line) == 0);
    assert(strstr(line, "\"type\":\"welcome\"") != NULL);

    assert(send_str(client, "{\"id\":1,\"cmd\":\"ping\"}\n") == 0);
    assert(read_line(client, line, sizeof line) == 0);
    assert(strstr(line, "\"ok\":true") != NULL);

    assert(send_str(client, "{\"id\":2,\"cmd\":\"list_commands\"}\n") == 0);
    assert(read_line(client, line, sizeof line) == 0);
    assert(strstr(line, "\"commands\"") != NULL);
    assert(strstr(line, "\"toggle_mute\"") != NULL);
    dsd_json_node* catalog = NULL;
    char err[64] = "";
    assert(dsd_json_parse(line, &catalog, err, sizeof err) == 0);
    assert(catalog->type == DSD_JSON_OBJECT);
    assert(dsd_json_obj_get(catalog, "commands") != NULL);
    dsd_json_free(catalog);

    assert(send_str(client, "{\"id\":3,\"cmd\":\"subscribe\",\"params\":{\"topics\":[\"status\",\"event\"]}}\n") == 0);
    assert(read_line(client, line, sizeof line) == 0);
    assert(strstr(line, "\"status\"") != NULL);

    /* An unknown command is rejected without closing the session. */
    assert(send_str(client, "{\"id\":4,\"cmd\":\"not_a_command\"}\n") == 0);
    assert(read_line(client, line, sizeof line) == 0);
    assert(strstr(line, "\"unknown_command\"") != NULL);

    dsd_socket_close(client);
    dsd_api_stop();
    assert(!dsd_api_is_running());
    printf("api server loopback test passed\n");
    return 0;
}
