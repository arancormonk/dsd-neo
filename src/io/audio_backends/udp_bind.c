// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/io/udp_bind.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <sys/socket.h>
#endif
#include "dsd-neo/core/safe_api.h"

dsd_socket_t
UDPBind(char* hostname, int portno) {
    dsd_socket_t sockfd;
    struct sockaddr_in serveraddr;
    const char* bind_host = (hostname && hostname[0] != '\0') ? hostname : "127.0.0.1";

    /* socket: create the socket */
    //UDP socket
    sockfd = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (sockfd == DSD_INVALID_SOCKET) {
        DSD_FPRINTF(stderr, "ERROR opening UDP socket\n");
        perror("ERROR opening UDP socket");
        return DSD_INVALID_SOCKET;
    }

    /* build the server's Internet address */
    DSD_MEMSET((char*)&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons((uint16_t)portno);
    if (strcmp(bind_host, "0.0.0.0") == 0) {
        serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (dsd_socket_resolve(bind_host, portno, &serveraddr) != 0) {
        DSD_FPRINTF(stderr, "ERROR resolving UDP bind address %s\n", bind_host);
        dsd_socket_close(sockfd);
        return DSD_INVALID_SOCKET;
    }

    //Bind socket to listening
    if (dsd_socket_bind(sockfd, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) != 0) {
        perror("ERROR on binding UDP Port");
        dsd_socket_close(sockfd);
        return DSD_INVALID_SOCKET;
    }

    //set these for non blocking when no samples to read (very short timeout)
    dsd_socket_set_recv_timeout(sockfd, 1); // 1ms timeout

    return sockfd;
}
