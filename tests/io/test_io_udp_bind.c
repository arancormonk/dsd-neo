// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Regression test: UDPBind must honor explicit bind addresses and use
 * loopback when the caller does not provide one.
 */

#include <dsd-neo/io/udp_bind.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static int
expect_bind_address(char* host, uint32_t expected_addr) {
    dsd_socket_t sock = UDPBind(host, 0);
    if (sock == DSD_INVALID_SOCKET) {
        DSD_FPRINTF(stderr, "UDPBind failed for host '%s'\n", host ? host : "(null)");
        return 1;
    }

    struct sockaddr_in sa;
    DSD_MEMSET(&sa, 0, sizeof(sa));
#if DSD_PLATFORM_WIN_NATIVE
    int slen = (int)sizeof(sa);
#else
    socklen_t slen = (socklen_t)sizeof(sa);
#endif
    int rc = getsockname(sock, (struct sockaddr*)&sa, &slen);
    dsd_socket_close(sock);
    if (rc != 0 || sa.sin_family != AF_INET || ntohs(sa.sin_port) == 0) {
        DSD_FPRINTF(stderr, "getsockname failed for host '%s'\n", host ? host : "(null)");
        return 1;
    }
    uint32_t actual = ntohl(sa.sin_addr.s_addr);
    if (actual != expected_addr) {
        DSD_FPRINTF(stderr, "host '%s' bound %08X expected %08X\n", host ? host : "(null)", actual, expected_addr);
        return 1;
    }
    return 0;
}

int
main(void) {
    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "dsd_socket_init failed\n");
        return 1;
    }
    int rc = 0;
    char empty[] = "";
    char loopback[] = "127.0.0.1";
    char any[] = "0.0.0.0";
    rc |= expect_bind_address(NULL, INADDR_LOOPBACK);
    rc |= expect_bind_address(empty, INADDR_LOOPBACK);
    rc |= expect_bind_address(loopback, INADDR_LOOPBACK);
    rc |= expect_bind_address(any, INADDR_ANY);
    dsd_socket_cleanup();
    return rc;
}
