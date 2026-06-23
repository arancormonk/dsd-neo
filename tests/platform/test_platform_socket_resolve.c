// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif

static int
expect_numeric_ipv4_resolves(const char* host, int port) {
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof(addr));

    if (dsd_socket_resolve(host, port, &addr) != 0) {
        DSD_FPRINTF(stderr, "expected %s:%d to resolve\n", host, port);
        return 1;
    }
    if (addr.sin_family != AF_INET) {
        DSD_FPRINTF(stderr, "expected AF_INET for %s, got %d\n", host, (int)addr.sin_family);
        return 1;
    }
    if (ntohs(addr.sin_port) != port) {
        DSD_FPRINTF(stderr, "expected port %d for %s, got %d\n", port, host, (int)ntohs(addr.sin_port));
        return 1;
    }

    unsigned long expected_addr = inet_addr(host);
    if (expected_addr == INADDR_NONE) {
        DSD_FPRINTF(stderr, "test host %s is not a valid IPv4 literal\n", host);
        return 1;
    }
    if (addr.sin_addr.s_addr != expected_addr) {
        DSD_FPRINTF(stderr, "resolved address mismatch for %s\n", host);
        return 1;
    }

    return 0;
}

#if !DSD_PLATFORM_WIN_NATIVE
static int
get_bound_addr(dsd_socket_t sock, struct sockaddr_in* addr) {
    socklen_t len = sizeof(*addr);
    DSD_MEMSET(addr, 0, sizeof(*addr));
    if (getsockname(sock, (struct sockaddr*)addr, &len) != 0) {
        DSD_FPRINTF(stderr, "getsockname failed: %s\n", strerror(errno));
        return 1;
    }
    if (addr->sin_family != AF_INET || ntohs(addr->sin_port) == 0) {
        DSD_FPRINTF(stderr, "expected bound IPv4 socket with ephemeral port\n");
        return 1;
    }
    return 0;
}

static int
expect_udp_loopback_round_trips(void) {
    int rc = 0;
    dsd_socket_t rx = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    dsd_socket_t tx = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (rx == DSD_INVALID_SOCKET || tx == DSD_INVALID_SOCKET) {
        DSD_FPRINTF(stderr, "failed to create UDP sockets\n");
        rc = 1;
        goto done;
    }

    struct sockaddr_in bind_addr;
    DSD_MEMSET(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = htons(0);
    if (dsd_socket_bind(rx, (const struct sockaddr*)&bind_addr, (int)sizeof(bind_addr)) != 0) {
        DSD_FPRINTF(stderr, "UDP bind failed: %s\n", strerror(dsd_socket_get_error()));
        rc = 1;
        goto done;
    }

    struct sockaddr_in rx_addr;
    rc |= get_bound_addr(rx, &rx_addr);

    const struct timeval expected_tv = {.tv_sec = 1, .tv_usec = 250000};
    if (dsd_socket_set_recv_timeout(rx, 1250) != 0 || dsd_socket_set_send_timeout(tx, 1250) != 0) {
        DSD_FPRINTF(stderr, "socket timeout setup failed\n");
        rc = 1;
        goto done;
    }
    struct timeval got_tv;
    int got_len = (int)sizeof(got_tv);
    if (dsd_socket_getsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &got_tv, &got_len) != 0 || got_len != (int)sizeof(got_tv)
        || got_tv.tv_sec != expected_tv.tv_sec || got_tv.tv_usec != expected_tv.tv_usec) {
        DSD_FPRINTF(stderr, "receive timeout round-trip mismatch\n");
        rc = 1;
    }

    errno = 0;
    if (dsd_socket_getsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &got_tv, NULL) != -1 || errno != EINVAL) {
        DSD_FPRINTF(stderr, "expected getsockopt NULL length to fail with EINVAL\n");
        rc = 1;
    }

    if (dsd_socket_set_nonblocking(rx, 1) != 0 || !(fcntl(rx, F_GETFL, 0) & O_NONBLOCK)
        || dsd_socket_set_nonblocking(rx, 0) != 0 || (fcntl(rx, F_GETFL, 0) & O_NONBLOCK)) {
        DSD_FPRINTF(stderr, "nonblocking toggle mismatch\n");
        rc = 1;
    }

    const char first[] = "udp-sendto";
    if (dsd_socket_sendto(tx, first, sizeof(first), 0, (const struct sockaddr*)&rx_addr, (int)sizeof(rx_addr))
        != (int)sizeof(first)) {
        DSD_FPRINTF(stderr, "UDP sendto failed\n");
        rc = 1;
        goto done;
    }
    char buf[32];
    struct sockaddr_in src_addr;
    int src_len = (int)sizeof(src_addr);
    DSD_MEMSET(&src_addr, 0, sizeof(src_addr));
    int n = dsd_socket_recvfrom(rx, buf, sizeof(buf), 0, (struct sockaddr*)&src_addr, &src_len);
    if (n != (int)sizeof(first) || memcmp(buf, first, sizeof(first)) != 0 || src_len <= 0) {
        DSD_FPRINTF(stderr, "UDP recvfrom payload/source mismatch\n");
        rc = 1;
    }

    if (dsd_socket_connect(tx, (const struct sockaddr*)&rx_addr, (int)sizeof(rx_addr)) != 0) {
        DSD_FPRINTF(stderr, "UDP connect failed\n");
        rc = 1;
        goto done;
    }
    const char second[] = "udp-send";
    if (dsd_socket_send(tx, second, sizeof(second), 0) != (int)sizeof(second)
        || dsd_socket_recv(rx, buf, sizeof(buf), 0) != (int)sizeof(second)
        || memcmp(buf, second, sizeof(second)) != 0) {
        DSD_FPRINTF(stderr, "UDP connected send/recv mismatch\n");
        rc = 1;
    }

    errno = EAGAIN;
    if (dsd_socket_get_error() != EAGAIN) {
        DSD_FPRINTF(stderr, "socket error wrapper did not return errno\n");
        rc = 1;
    }

done:
    if (rx != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(rx);
    }
    if (tx != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(tx);
    }
    return rc;
}

static int
expect_tcp_loopback_accepts(void) {
    int rc = 0;
    dsd_socket_t listener = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    dsd_socket_t client = DSD_INVALID_SOCKET;
    dsd_socket_t accepted = DSD_INVALID_SOCKET;
    dsd_socket_t client2 = DSD_INVALID_SOCKET;
    dsd_socket_t accepted2 = DSD_INVALID_SOCKET;
    if (listener == DSD_INVALID_SOCKET) {
        DSD_FPRINTF(stderr, "failed to create TCP listener\n");
        return 1;
    }

    int reuse = 1;
    (void)dsd_socket_setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, (int)sizeof(reuse));

    struct sockaddr_in bind_addr;
    DSD_MEMSET(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = htons(0);
    if (dsd_socket_bind(listener, (const struct sockaddr*)&bind_addr, (int)sizeof(bind_addr)) != 0
        || dsd_socket_listen(listener, 2) != 0) {
        DSD_FPRINTF(stderr, "TCP bind/listen failed: %s\n", strerror(dsd_socket_get_error()));
        rc = 1;
        goto done;
    }

    struct sockaddr_in listener_addr;
    rc |= get_bound_addr(listener, &listener_addr);

    client = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (client == DSD_INVALID_SOCKET
        || dsd_socket_connect(client, (const struct sockaddr*)&listener_addr, (int)sizeof(listener_addr)) != 0) {
        DSD_FPRINTF(stderr, "TCP connect failed: %s\n", strerror(dsd_socket_get_error()));
        rc = 1;
        goto done;
    }

    struct sockaddr_in peer_addr;
    int peer_len = (int)sizeof(peer_addr);
    DSD_MEMSET(&peer_addr, 0, sizeof(peer_addr));
    accepted = dsd_socket_accept(listener, (struct sockaddr*)&peer_addr, &peer_len);
    if (accepted == DSD_INVALID_SOCKET || peer_len <= 0 || peer_addr.sin_family != AF_INET) {
        DSD_FPRINTF(stderr, "TCP accept with peer address failed\n");
        rc = 1;
        goto done;
    }

    const char payload[] = "tcp-payload";
    char buf[32];
    if (dsd_socket_send(client, payload, sizeof(payload), 0) != (int)sizeof(payload)
        || dsd_socket_recv(accepted, buf, sizeof(buf), 0) != (int)sizeof(payload)
        || memcmp(buf, payload, sizeof(payload)) != 0) {
        DSD_FPRINTF(stderr, "TCP send/recv payload mismatch\n");
        rc = 1;
    }
    if (dsd_socket_shutdown(accepted, SHUT_RDWR) != 0) {
        DSD_FPRINTF(stderr, "TCP shutdown failed\n");
        rc = 1;
    }

    client2 = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (client2 == DSD_INVALID_SOCKET
        || dsd_socket_connect(client2, (const struct sockaddr*)&listener_addr, (int)sizeof(listener_addr)) != 0) {
        DSD_FPRINTF(stderr, "second TCP connect failed\n");
        rc = 1;
        goto done;
    }
    accepted2 = dsd_socket_accept(listener, NULL, NULL);
    if (accepted2 == DSD_INVALID_SOCKET) {
        DSD_FPRINTF(stderr, "TCP accept with NULL peer address failed\n");
        rc = 1;
    }

done:
    if (accepted2 != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(accepted2);
    }
    if (client2 != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(client2);
    }
    if (accepted != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(accepted);
    }
    if (client != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(client);
    }
    if (listener != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(listener);
    }
    return rc;
}
#endif

int
main(void) {
    int rc = 0;
    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "socket init failed\n");
        return 1;
    }

    rc |= expect_numeric_ipv4_resolves("127.0.0.1", 7355);
    rc |= expect_numeric_ipv4_resolves("192.168.1.50", 7355);
    if (dsd_socket_resolve(NULL, 7355, &(struct sockaddr_in){0}) != -1) {
        DSD_FPRINTF(stderr, "expected NULL hostname resolve to fail\n");
        rc = 1;
    }
    if (dsd_socket_resolve("127.0.0.1", 7355, NULL) != -1) {
        DSD_FPRINTF(stderr, "expected NULL output resolve to fail\n");
        rc = 1;
    }
#if !DSD_PLATFORM_WIN_NATIVE
    rc |= expect_udp_loopback_round_trips();
    rc |= expect_tcp_loopback_accepts();
#endif

    dsd_socket_cleanup();
    return rc ? 1 : 0;
}
