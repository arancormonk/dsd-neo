// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The in-process rtl_tcp fixture server's stop() returns while a client stays connected and stops reading (issue #899,
 * review round 7): the server's sends fill the socket buffers, and a blocking send would never come back to see the
 * stop, so stop() would wait in its join for good. The accepted socket is nonblocking, so the bounded send wait sees the
 * stop and the connection ends. stop() is timed only once the server says a send found the socket full (review round
 * 8), so the case never passes on a server that had not yet stalled.
 */

#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <vector>
#include "rtl_tcp_fixture_server.h"

namespace {

/* How long stop() may take with a client that reads nothing: the send wait is 20 ms, so this is ample. */
constexpr uint64_t kStopBoundMs = 2000U;
/* How long the server may take to fill the client's buffers at 2.4 MS/s (about 4.8 MB/s): ample on a loaded host. */
constexpr unsigned int kBackpressureDeadlineMs = 20000U;

/* Connect to the server on @p port with a small receive buffer, ask for a high sample rate so its sends fill the
   buffers quickly, and read nothing. */
dsd_socket_t
connect_and_stall(int port) {
    const dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (sock == DSD_INVALID_SOCKET) {
        return sock;
    }
    int small = 4096;
    (void)dsd_socket_setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(0x7F000001U);
    if (dsd_socket_connect(sock, reinterpret_cast<const struct sockaddr*>(&addr), static_cast<int>(sizeof addr)) != 0) {
        (void)dsd_socket_close(sock);
        return DSD_INVALID_SOCKET;
    }
    const uint32_t rate = 2400000U;
    const unsigned char cmd[5] = {0x02U, static_cast<unsigned char>(rate >> 24), static_cast<unsigned char>(rate >> 16),
                                  static_cast<unsigned char>(rate >> 8), static_cast<unsigned char>(rate)};
    if (dsd_socket_send(sock, cmd, sizeof cmd, 0) != static_cast<int>(sizeof cmd)) {
        (void)dsd_socket_close(sock);
        return DSD_INVALID_SOCKET;
    }
    return sock;
}

} // namespace

int
main(void) {
    if (dsd_socket_init() != 0) {
        return 1;
    }
    dsd_test::RtlTcpFixtureServer server;
    std::string why;
    if (!server.start(DSD_NEO_TEST_IQ_FIXTURE_DIR, {}, {{"noise_floor", 0.0, 0.0}}, &why)) {
        DSD_FPRINTF(stderr, "fixture server stop: start: %s\n", why.c_str());
        dsd_socket_cleanup();
        return 1;
    }
    const dsd_socket_t client = connect_and_stall(server.port());
    if (client == DSD_INVALID_SOCKET) {
        DSD_FPRINTF(stderr, "fixture server stop: the client did not connect\n");
        server.stop();
        dsd_socket_cleanup();
        return 1;
    }
    /* The server's sends fill both buffers at 2.4 MS/s: stop() is timed once a send has found the socket full. */
    if (!server.wait_for_backpressure(kBackpressureDeadlineMs)) {
        DSD_FPRINTF(stderr, "fixture server stop: no send found the socket full within %u ms\n",
                    kBackpressureDeadlineMs);
        server.stop();
        (void)dsd_socket_close(client);
        dsd_socket_cleanup();
        return 1;
    }
    const uint64_t start = dsd_realtime_mono_ms();
    server.stop();
    const uint64_t took = dsd_realtime_mono_ms() - start;
    (void)dsd_socket_close(client);
    dsd_socket_cleanup();
    if (took > kStopBoundMs) {
        DSD_FPRINTF(stderr, "fixture server stop: stop() took %llu ms with a stalled client, want under %llu ms\n",
                    static_cast<unsigned long long>(took), static_cast<unsigned long long>(kStopBoundMs));
        return 1;
    }
    DSD_FPRINTF(stderr, "fixture server stop: stop() took %llu ms with a stalled client\n",
                static_cast<unsigned long long>(took));
    return 0;
}
