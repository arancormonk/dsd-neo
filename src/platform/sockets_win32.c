// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/sockets.h>

#if DSD_PLATFORM_WIN_NATIVE

#include <errno.h>

/* Winsock initialization tracking */
static int s_wsa_initialized = 0;

int
dsd_socket_init(void) {
    if (s_wsa_initialized) {
        return 0;
    }

    WSADATA wsa;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (result != 0) {
        return result;
    }
    s_wsa_initialized = 1;
    return 0;
}

void
dsd_socket_cleanup(void) {
    if (s_wsa_initialized) {
        WSACleanup();
        s_wsa_initialized = 0;
    }
}

dsd_socket_t
dsd_socket_create(int domain, int type, int protocol) {
    /* Ensure Winsock is initialized */
    if (!s_wsa_initialized) {
        if (dsd_socket_init() != 0) {
            return DSD_INVALID_SOCKET;
        }
    }
    return socket(domain, type, protocol);
}

int
dsd_socket_close(dsd_socket_t sock) {
    return closesocket(sock);
}

int
dsd_socket_bind(dsd_socket_t sock, const struct sockaddr* addr, int addrlen) {
    return bind(sock, addr, addrlen);
}

int
dsd_socket_listen(dsd_socket_t sock, int backlog) {
    return listen(sock, backlog);
}

dsd_socket_t
dsd_socket_accept(dsd_socket_t sock, struct sockaddr* addr, int* addrlen) {
    return accept(sock, addr, addrlen);
}

int
dsd_socket_connect(dsd_socket_t sock, const struct sockaddr* addr, int addrlen) {
    return connect(sock, addr, addrlen);
}

static int
socket_wait_connect(dsd_socket_t sock, unsigned int timeout_ms, dsd_socket_cancel_fn cancelled, void* context) {
    int code = 0;
    for (unsigned int elapsed = 0; elapsed < timeout_ms;) {
        if (cancelled && cancelled(context)) {
            code = WSAEINTR;
            return code;
        }
        unsigned int wait_ms = timeout_ms - elapsed;
        if (wait_ms > 100U) {
            wait_ms = 100U;
        }
        struct timeval wait = {0, (long)wait_ms * 1000L};
        fd_set writable, errors;
        FD_ZERO(&writable);
        FD_ZERO(&errors);
        FD_SET(sock, &writable);
        FD_SET(sock, &errors);
        const int ready = select(0, NULL, &writable, &errors, &wait);
        elapsed += wait_ms;
        if (ready < 0) {
            code = dsd_socket_get_error();
            if (code == WSAEINTR) {
                continue;
            }
            return code;
        }
        if (ready > 0) {
            int length = (int)sizeof(code);
            if (dsd_socket_getsockopt(sock, SOL_SOCKET, SO_ERROR, &code, &length) != 0) {
                code = dsd_socket_get_error();
            }
            return code;
        }
    }
    return WSAETIMEDOUT;
}

int
dsd_socket_connect_bounded(dsd_socket_t sock, const struct sockaddr* addr, int addrlen, unsigned int timeout_ms,
                           // Cppcheck 2.21 loses names after a callback typedef; these match sockets.h.
                           // cppcheck-suppress funcArgNamesDifferentUnnamed
                           dsd_socket_cancel_fn cancelled, void* context, int* error_code) {
    int code = 0;
    int result = -1;
    if (cancelled && cancelled(context)) {
        code = WSAEINTR;
        goto done;
    }
    if (dsd_socket_set_nonblocking(sock, 1) != 0) {
        code = dsd_socket_get_error();
        goto done;
    }
    if (dsd_socket_connect(sock, addr, addrlen) == 0) {
        result = 0;
        goto restore;
    }
    code = dsd_socket_get_error();
    if (code != WSAEWOULDBLOCK && code != WSAEINPROGRESS) {
        goto restore;
    }
    code = socket_wait_connect(sock, timeout_ms, cancelled, context);
    result = code == 0 ? 0 : -1;
restore:
    if (dsd_socket_set_nonblocking(sock, 0) != 0 && result == 0) {
        code = dsd_socket_get_error();
        result = -1;
    }
done:
    if (error_code) {
        *error_code = code;
    }
    return result;
}

int
dsd_socket_send(dsd_socket_t sock, const void* buf, size_t len, int flags) {
    return send(sock, (const char*)buf, (int)len, flags);
}

int
dsd_socket_sendto(dsd_socket_t sock, const void* buf, size_t len, int flags, const struct sockaddr* dest_addr,
                  int addrlen) {
    return sendto(sock, (const char*)buf, (int)len, flags, dest_addr, addrlen);
}

int
dsd_socket_recv(dsd_socket_t sock, void* buf, size_t len, int flags) {
    return recv(sock, (char*)buf, (int)len, flags);
}

int
dsd_socket_recvfrom(dsd_socket_t sock, void* buf, size_t len, int flags, struct sockaddr* src_addr, int* addrlen) {
    return recvfrom(sock, (char*)buf, (int)len, flags, src_addr, addrlen);
}

int
dsd_socket_setsockopt(dsd_socket_t sock, int level, int optname, const void* optval, int optlen) {
    return setsockopt(sock, level, optname, (const char*)optval, optlen);
}

int
dsd_socket_getsockopt(dsd_socket_t sock, int level, int optname, void* optval, int* optlen) {
    if (!optlen) {
        WSASetLastError(WSAEINVAL);
        return DSD_SOCKET_ERROR;
    }
    return getsockopt(sock, level, optname, (char*)optval, optlen);
}

int
dsd_socket_shutdown(dsd_socket_t sock, int how) {
    /* Map POSIX constants to Windows constants */
    int win_how;
    switch (how) {
        case SHUT_RD: win_how = SD_RECEIVE; break;
        case SHUT_WR: win_how = SD_SEND; break;
        case SHUT_RDWR: win_how = SD_BOTH; break;
        default: return -1;
    }
    return shutdown(sock, win_how);
}

int
dsd_socket_get_error(void) {
    return WSAGetLastError();
}

int
dsd_socket_set_nonblocking(dsd_socket_t sock, int nonblock) {
    u_long mode = nonblock ? 1 : 0;
    return ioctlsocket(sock, FIONBIO, &mode);
}

int
dsd_socket_set_recv_timeout(dsd_socket_t sock, unsigned int timeout_ms) {
    /* Windows uses DWORD (milliseconds) for socket timeouts */
    DWORD tv = timeout_ms;
    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
}

int
dsd_socket_set_send_timeout(dsd_socket_t sock, unsigned int timeout_ms) {
    /* Windows uses DWORD (milliseconds) for socket timeouts */
    DWORD tv = timeout_ms;
    return setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
}

int
dsd_socket_resolve(const char* hostname, int port, struct sockaddr_in* addr) {
    if (!hostname || !addr) {
        return -1;
    }

    /* Ensure Winsock is initialized */
    if (!s_wsa_initialized) {
        if (dsd_socket_init() != 0) {
            return -1;
        }
    }

    struct addrinfo hints;
    struct addrinfo* result = NULL;
    DSD_MEMSET(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;

    int gai = getaddrinfo(hostname, NULL, &hints, &result);
    if (gai != 0 || !result || !result->ai_addr) {
        if (result) {
            freeaddrinfo(result);
        }
        return -1;
    }

    DSD_MEMCPY(addr, result->ai_addr, sizeof(*addr));
    freeaddrinfo(result);
    addr->sin_port = htons((u_short)port);
    return 0;
}

#endif /* DSD_PLATFORM_WIN_NATIVE */
