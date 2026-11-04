// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_SOCKETS_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_SOCKETS_H_H

/**
 * @file
 * @brief Cross-platform socket abstraction for DSD-neo.
 *
 * Provides a unified API for BSD sockets (POSIX) and Winsock2 (Windows).
 */

#include <dsd-neo/platform/platform.h>
#include <stddef.h>

#if DSD_PLATFORM_WIN_NATIVE
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET dsd_socket_t;
#define DSD_INVALID_SOCKET INVALID_SOCKET
#define DSD_SOCKET_ERROR   SOCKET_ERROR
#else
#include <sys/socket.h>
typedef int dsd_socket_t;
#define DSD_INVALID_SOCKET (-1)
#define DSD_SOCKET_ERROR   (-1)
#endif

#ifndef __cplusplus
struct sockaddr_in;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize socket subsystem.
 *
 * Must be called before any socket operations on Windows.
 * No-op on POSIX systems.
 *
 * @return 0 on success, non-zero on failure.
 */
int dsd_socket_init(void);

/**
 * @brief Cleanup socket subsystem.
 *
 * Should be called at program termination on Windows.
 * No-op on POSIX systems.
 */
void dsd_socket_cleanup(void);

/**
 * @brief Create a socket.
 *
 * @param domain    Address family (AF_INET, AF_INET6).
 * @param type      Socket type (SOCK_STREAM, SOCK_DGRAM).
 * @param protocol  Protocol (IPPROTO_TCP, IPPROTO_UDP, 0).
 * @return Socket handle, or DSD_INVALID_SOCKET on error.
 */
dsd_socket_t dsd_socket_create(int domain, int type, int protocol);

/**
 * @brief Close a socket.
 *
 * @param sock      Socket to close.
 * @return 0 on success, non-zero on failure.
 */
int dsd_socket_close(dsd_socket_t sock);

/**
 * @brief Bind socket to address.
 *
 * @param sock      Socket handle.
 * @param addr      Address structure.
 * @param addrlen   Size of address structure.
 * @return 0 on success, non-zero on failure.
 */
int dsd_socket_bind(dsd_socket_t sock, const struct sockaddr* addr, int addrlen);

/**
 * @brief Listen for connections on a socket.
 *
 * @param sock      Socket handle.
 * @param backlog   Maximum length of pending connection queue.
 * @return 0 on success, non-zero on failure.
 */
int dsd_socket_listen(dsd_socket_t sock, int backlog);

/**
 * @brief Accept a connection on a socket.
 *
 * @param sock      Socket handle.
 * @param addr      Address structure for client (output, may be NULL).
 * @param addrlen   Address length (in/out, may be NULL).
 * @return New socket handle, or DSD_INVALID_SOCKET on error.
 */
dsd_socket_t dsd_socket_accept(dsd_socket_t sock, struct sockaddr* addr, int* addrlen);

/**
 * @brief Connect socket to remote address.
 *
 * @param sock      Socket handle.
 * @param addr      Remote address.
 * @param addrlen   Size of address structure.
 * @return 0 on success, non-zero on failure.
 */
int dsd_socket_connect(dsd_socket_t sock, const struct sockaddr* addr, int addrlen);
/** Connect a new blocking socket within timeout_ms, checking cancellation every
 * 100 ms. Restores blocking mode. Returns 0 on success, -1 with native error.
 * Hostname resolution is separate and follows the system resolver's timeout. */
typedef int (*dsd_socket_cancel_fn)(void* context);
int dsd_socket_connect_bounded(dsd_socket_t sock, const struct sockaddr* addr, int addrlen, unsigned int timeout_ms,
                               dsd_socket_cancel_fn cancelled, void* context, int* error_code);

/**
 * @brief Send data on a connected socket.
 *
 * @param sock      Socket handle.
 * @param buf       Data buffer.
 * @param len       Buffer length.
 * @param flags     Send flags.
 * @return Number of bytes sent, or negative on error.
 */
int dsd_socket_send(dsd_socket_t sock, const void* buf, size_t len, int flags);

/**
 * @brief Send data to a specific address (UDP).
 *
 * @param sock      Socket handle.
 * @param buf       Data buffer.
 * @param len       Buffer length.
 * @param flags     Send flags.
 * @param dest_addr Destination address.
 * @param addrlen   Size of address structure.
 * @return Number of bytes sent, or negative on error.
 */
int dsd_socket_sendto(dsd_socket_t sock, const void* buf, size_t len, int flags, const struct sockaddr* dest_addr,
                      int addrlen);

/**
 * @brief Receive data from a connected socket.
 *
 * @param sock      Socket handle.
 * @param buf       Buffer for received data.
 * @param len       Buffer length.
 * @param flags     Receive flags.
 * @return Number of bytes received, 0 on connection closed, negative on error.
 */
int dsd_socket_recv(dsd_socket_t sock, void* buf, size_t len, int flags);

/**
 * @brief Receive data from any sender (UDP).
 *
 * @param sock      Socket handle.
 * @param buf       Buffer for received data.
 * @param len       Buffer length.
 * @param flags     Receive flags.
 * @param src_addr  Source address (output, may be NULL).
 * @param addrlen   Address length (in/out, may be NULL).
 * @return Number of bytes received, negative on error.
 */
int dsd_socket_recvfrom(dsd_socket_t sock, void* buf, size_t len, int flags, struct sockaddr* src_addr, int* addrlen);

/**
 * @brief Set socket option.
 *
 * @param sock      Socket handle.
 * @param level     Option level (SOL_SOCKET, IPPROTO_TCP, etc.).
 * @param optname   Option name.
 * @param optval    Option value.
 * @param optlen    Option value length.
 * @return 0 on success, non-zero on failure.
 */
int dsd_socket_setsockopt(dsd_socket_t sock, int level, int optname, const void* optval, int optlen);

/**
 * @brief Get socket option.
 *
 * @param sock      Socket handle.
 * @param level     Option level (SOL_SOCKET, IPPROTO_TCP, etc.).
 * @param optname   Option name.
 * @param optval    Option value buffer.
 * @param optlen    Option value length (in/out).
 * @return 0 on success, non-zero on failure.
 */
int dsd_socket_getsockopt(dsd_socket_t sock, int level, int optname, void* optval, int* optlen);

/**
 * @brief Shutdown socket for reading, writing, or both.
 *
 * @param sock      Socket handle.
 * @param how       SHUT_RD, SHUT_WR, or SHUT_RDWR.
 * @return 0 on success, non-zero on failure.
 */
int dsd_socket_shutdown(dsd_socket_t sock, int how);

/**
 * @brief Get last socket error code.
 *
 * @return Error code (errno on POSIX, WSAGetLastError on Windows).
 */
int dsd_socket_get_error(void);

/**
 * @brief Set socket to non-blocking mode.
 *
 * @param sock      Socket handle.
 * @param nonblock  1 for non-blocking, 0 for blocking.
 * @return 0 on success, non-zero on failure.
 */
int dsd_socket_set_nonblocking(dsd_socket_t sock, int nonblock);

/** Readiness flags for dsd_socket_wait(). */
enum { DSD_SOCKET_WAIT_READ = 1, DSD_SOCKET_WAIT_WRITE = 2 };

/**
 * @brief Wait up to @p timeout_ms for @p sock to become readable and/or writable.
 *
 * A socket at end of stream or with a pending error counts as ready for every
 * requested direction, so the following recv()/send() reports the condition.
 *
 * @param sock        Socket handle.
 * @param events      DSD_SOCKET_WAIT_READ and/or DSD_SOCKET_WAIT_WRITE.
 * @param timeout_ms  Longest wait in milliseconds (0 polls).
 * @return The ready subset of @p events, 0 on timeout or a signal interruption,
 *         or -1 on failure.
 */
int dsd_socket_wait(dsd_socket_t sock, int events, unsigned int timeout_ms);

/**
 * @brief Set socket receive timeout.
 *
 * @param sock          Socket handle.
 * @param timeout_ms    Timeout in milliseconds (0 = no timeout).
 * @return 0 on success, non-zero on failure.
 */
int dsd_socket_set_recv_timeout(dsd_socket_t sock, unsigned int timeout_ms);

/**
 * @brief Set socket send timeout.
 *
 * @param sock          Socket handle.
 * @param timeout_ms    Timeout in milliseconds (0 = no timeout).
 * @return 0 on success, non-zero on failure.
 */
int dsd_socket_set_send_timeout(dsd_socket_t sock, unsigned int timeout_ms);

/** Keepalive timing that finds a half-open TCP link (a peer that vanished without closing: its host lost power, a
 *  network or NAT mapping dropped) on a connection that may carry nothing for a long time: probes start after 15 s
 *  with nothing received, go out every 5 s, and the fourth unanswered one fails the connection, about 35 s in all. */
#define DSD_SOCKET_KEEPALIVE_IDLE_S     15U
#define DSD_SOCKET_KEEPALIVE_INTERVAL_S 5U
#define DSD_SOCKET_KEEPALIVE_COUNT      4U

/**
 * @brief Turn on TCP keepalive, so a read on a connection whose peer vanished fails instead of waiting forever.
 *
 * SO_KEEPALIVE, then the timing wherever the platform lets a socket set it: the idle time (TCP_KEEPIDLE; macOS's
 * TCP_KEEPALIVE; on Windows TCP_KEEPIDLE, else SIO_KEEPALIVE_VALS), the probe interval (TCP_KEEPINTVL, or
 * SIO_KEEPALIVE_VALS) and the probe count (TCP_KEEPCNT; where Windows lacks it, its fixed 10). A platform with none of
 * the timing options keeps its system-wide keepalive timing (on Linux, hours).
 *
 * @param sock        Connected (or connecting) TCP socket.
 * @param idle_s      Seconds with nothing received before the first probe.
 * @param interval_s  Seconds between unanswered probes.
 * @param count       Unanswered probes that fail the connection.
 * @return 0 when keepalive is on with every timing option the platform has, non-zero when any option failed.
 */
int dsd_socket_set_keepalive(dsd_socket_t sock, unsigned int idle_s, unsigned int interval_s, unsigned int count);

/**
 * @brief Resolve hostname to address.
 *
 * @param hostname  Hostname to resolve.
 * @param port      Port number.
 * @param addr      Output sockaddr_in structure.
 * @return 0 on success, non-zero on failure.
 */
int dsd_socket_resolve(const char* hostname, int port, struct sockaddr_in* addr);

/* Shutdown constants (may not be defined on Windows) */
#ifndef SHUT_RD
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2
#endif

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_SOCKETS_H_H */
