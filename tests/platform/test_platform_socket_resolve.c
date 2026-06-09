// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

int
main(void) {
    int rc = 0;
    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "socket init failed\n");
        return 1;
    }

    rc |= expect_numeric_ipv4_resolves("127.0.0.1", 7355);
    rc |= expect_numeric_ipv4_resolves("192.168.1.50", 7355);

    dsd_socket_cleanup();
    return rc ? 1 : 0;
}
