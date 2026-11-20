// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <arpa/inet.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/timing.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#if !DSD_PLATFORM_WIN_NATIVE

int
dsd_socket_init(void) {
    /* No-op on POSIX */
    return 0;
}

void
dsd_socket_cleanup(void) {
    /* No-op on POSIX */
}

dsd_socket_t
dsd_socket_create(int domain, int type, int protocol) {
    return socket(domain, type, protocol);
}

int
dsd_socket_close(dsd_socket_t sock) {
    return close(sock);
}

int
dsd_socket_bind(dsd_socket_t sock, const struct sockaddr* addr, int addrlen) {
    return bind(sock, addr, (socklen_t)addrlen);
}

int
dsd_socket_listen(dsd_socket_t sock, int backlog) {
    return listen(sock, backlog);
}

dsd_socket_t
dsd_socket_accept(dsd_socket_t sock, struct sockaddr* addr, int* addrlen) {
    socklen_t slen = addrlen ? (socklen_t)*addrlen : 0;
    dsd_socket_t result = accept(sock, addr, addrlen ? &slen : NULL);
    if (addrlen) {
        *addrlen = (int)slen;
    }
    return result;
}

int
dsd_socket_connect(dsd_socket_t sock, const struct sockaddr* addr, int addrlen) {
    return connect(sock, addr, (socklen_t)addrlen);
}

/* Wait to @p deadline_ms on the monotonic clock, the connect's one deadline (dsd_socket_connect_bounded()), which the
   cancel checks' own time counts against too: a caller's check can block (the decoder services a P25 tick there, which
   can wait on a rigctl reply), and never stretches the bound. */
static int
socket_wait_connect(dsd_socket_t sock, uint64_t deadline_ms, dsd_socket_cancel_fn cancelled, void* context) {
    int code = 0;
    for (;;) {
        if (cancelled && cancelled(context)) {
            code = ECANCELED;
            return code;
        }
        const uint64_t now_ms = dsd_time_monotonic_ms();
        if (now_ms >= deadline_ms) {
            break;
        }
        const uint64_t left_ms = deadline_ms - now_ms;
        const unsigned int wait_ms = left_ms > 100U ? 100U : (unsigned int)left_ms;
        struct timeval wait = {0, (long)wait_ms * 1000L};
        fd_set writable, errors;
        FD_ZERO(&writable);
        FD_ZERO(&errors);
        FD_SET(sock, &writable);
        FD_SET(sock, &errors);
        const int ready = select(sock + 1, NULL, &writable, &errors, &wait);
        if (ready < 0) {
            code = dsd_socket_get_error();
            if (code == EINTR) {
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
        /* Expired with the wait: no further check runs, so no blocking callback starts past the deadline. */
        if (dsd_time_monotonic_ms() >= deadline_ms) {
            break;
        }
    }
    return ETIMEDOUT;
}

int
dsd_socket_connect_bounded(dsd_socket_t sock, const struct sockaddr* addr, int addrlen, unsigned int timeout_ms,
                           // Cppcheck 2.21 loses names after a callback typedef; these match sockets.h.
                           // cppcheck-suppress funcArgNamesDifferentUnnamed
                           dsd_socket_cancel_fn cancelled, void* context, int* error_code) {
    const uint64_t deadline_ms = dsd_time_monotonic_ms() + timeout_ms;
    int code = 0;
    int result = -1;
    if (sock < 0 || sock >= FD_SETSIZE) {
        code = EINVAL;
        goto done;
    }
    if (cancelled && cancelled(context)) {
        code = ECANCELED;
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
    if (code != EINPROGRESS && code != EWOULDBLOCK) {
        goto restore;
    }
    code = socket_wait_connect(sock, deadline_ms, cancelled, context);
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
    return (int)send(sock, buf, len, flags);
}

int
dsd_socket_sendto(dsd_socket_t sock, const void* buf, size_t len, int flags, const struct sockaddr* dest_addr,
                  int addrlen) {
    return (int)sendto(sock, buf, len, flags, dest_addr, (socklen_t)addrlen);
}

int
dsd_socket_recv(dsd_socket_t sock, void* buf, size_t len, int flags) {
    return (int)recv(sock, buf, len, flags);
}

int
dsd_socket_recvfrom(dsd_socket_t sock, void* buf, size_t len, int flags, struct sockaddr* src_addr, int* addrlen) {
    socklen_t slen = addrlen ? (socklen_t)*addrlen : 0;
    int result = (int)recvfrom(sock, buf, len, flags, src_addr, addrlen ? &slen : NULL);
    if (addrlen) {
        *addrlen = (int)slen;
    }
    return result;
}

int
dsd_socket_setsockopt(dsd_socket_t sock, int level, int optname, const void* optval, int optlen) {
    return setsockopt(sock, level, optname, optval, (socklen_t)optlen);
}

int
dsd_socket_getsockopt(dsd_socket_t sock, int level, int optname, void* optval, int* optlen) {
    if (!optlen) {
        errno = EINVAL;
        return -1;
    }

    socklen_t slen = (socklen_t)*optlen;
    int result = getsockopt(sock, level, optname, optval, &slen);
    if (result == 0) {
        *optlen = (int)slen;
    }
    return result;
}

int
dsd_socket_shutdown(dsd_socket_t sock, int how) {
    return shutdown(sock, how);
}

int
dsd_socket_get_error(void) {
    return errno;
}

int
dsd_socket_set_nonblocking(dsd_socket_t sock, int nonblock) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (nonblock) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(sock, F_SETFL, flags);
}

long
dsd_socket_readable_bytes(dsd_socket_t sock) {
    int ready = 0;
    if (sock == DSD_INVALID_SOCKET || ioctl(sock, FIONREAD, &ready) != 0 || ready < 0) {
        return -1;
    }
    return (long)ready;
}

int
dsd_socket_wait(dsd_socket_t sock, int events, unsigned int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = sock;
    pfd.events = 0;
    pfd.revents = 0;
    if (events & DSD_SOCKET_WAIT_READ) {
        pfd.events |= POLLIN;
    }
    if (events & DSD_SOCKET_WAIT_WRITE) {
        pfd.events |= POLLOUT;
    }
    if (pfd.events == 0) {
        return -1;
    }
    const int timeout = timeout_ms > (unsigned int)INT_MAX ? INT_MAX : (int)timeout_ms;
    const int ready = poll(&pfd, 1, timeout);
    if (ready < 0) {
        return errno == EINTR ? 0 : -1;
    }
    if (ready == 0) {
        return 0;
    }
    if (pfd.revents & POLLNVAL) {
        return -1;
    }
    const int broken = (pfd.revents & (POLLERR | POLLHUP)) != 0;
    int out = 0;
    if ((events & DSD_SOCKET_WAIT_READ) && (broken || (pfd.revents & POLLIN))) {
        out |= DSD_SOCKET_WAIT_READ;
    }
    if ((events & DSD_SOCKET_WAIT_WRITE) && (broken || (pfd.revents & POLLOUT))) {
        out |= DSD_SOCKET_WAIT_WRITE;
    }
    return out;
}

int
dsd_socket_set_recv_timeout(dsd_socket_t sock, unsigned int timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (suseconds_t)(timeout_ms % 1000U) * 1000;
    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int
dsd_socket_set_send_timeout(dsd_socket_t sock, unsigned int timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (suseconds_t)(timeout_ms % 1000U) * 1000;
    return setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* One TCP-level keepalive option as the int setsockopt() takes, a value past it clamped. */
static int
keepalive_tcp_option(dsd_socket_t sock, int name, unsigned int value) {
    const int v = value > (unsigned int)INT_MAX ? INT_MAX : (int)value;
    return setsockopt(sock, IPPROTO_TCP, name, &v, sizeof(v)) == 0 ? 0 : -1;
}

int
dsd_socket_set_keepalive(dsd_socket_t sock, unsigned int idle_s, unsigned int interval_s, unsigned int count) {
    const int on = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) != 0) {
        return -1;
    }
    int rc = 0;
#if defined(TCP_KEEPIDLE)
    rc |= keepalive_tcp_option(sock, TCP_KEEPIDLE, idle_s);
#elif defined(TCP_KEEPALIVE)
    /* macOS names the idle time TCP_KEEPALIVE. */
    rc |= keepalive_tcp_option(sock, TCP_KEEPALIVE, idle_s);
#else
    (void)idle_s;
#endif
#if defined(TCP_KEEPINTVL)
    rc |= keepalive_tcp_option(sock, TCP_KEEPINTVL, interval_s);
#else
    (void)interval_s;
#endif
#if defined(TCP_KEEPCNT)
    rc |= keepalive_tcp_option(sock, TCP_KEEPCNT, count);
#else
    (void)count;
#endif
    return rc;
}

int
dsd_socket_resolve(const char* hostname, int port, struct sockaddr_in* addr) {
    /* A port outside 0..65535 is refused rather than narrowed: 105659 would reach port 40123 (issue #743 B15). */
    if (!hostname || !addr || port < 0 || port > 65535) {
        return -1;
    }

    DSD_MEMSET(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t)port);

    /* Try numeric address first */
    if (inet_pton(AF_INET, hostname, &addr->sin_addr) == 1) {
        return 0;
    }

    /* Look the name up, the first IPv4 answer, as on Windows. getaddrinfo() is thread-safe where gethostbyname() is
       not: rtl_tcp looks its host up again on its reader thread at each reconnect while the decoder thread can be
       looking up rigctl's, UDP output's or M17's (issue #743 B18). */
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    DSD_MEMSET(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(hostname, NULL, &hints, &result) != 0 || !result) {
        return -1;
    }
    int rc = -1;
    if (result->ai_family == AF_INET && result->ai_addr && result->ai_addrlen >= (socklen_t)sizeof(*addr)) {
        struct sockaddr_in found;
        DSD_MEMCPY(&found, result->ai_addr, sizeof(found));
        addr->sin_addr = found.sin_addr;
        rc = 0;
    }
    freeaddrinfo(result);
    return rc;
}

#endif /* !DSD_PLATFORM_WIN_NATIVE */
