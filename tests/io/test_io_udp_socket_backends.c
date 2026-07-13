// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * UDP audio/M17 backend tests use socket stubs to verify setup, address
 * selection, and send/receive behavior without a live network peer.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/io/m17_udp.h>
#include <dsd-neo/io/udp_audio.h>
#include <dsd-neo/io/udp_bind.h>
#include <dsd-neo/io/udp_socket_connect.h>
#include <dsd-neo/platform/sockets.h>
#include <stdint.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"

#if !DSD_PLATFORM_WIN_NATIVE
#include <netinet/in.h>
#include <sys/socket.h>
#endif

static dsd_socket_t g_create_result;
static int g_setsockopt_result;
static int g_resolve_result;
static int g_create_count;
static int g_setsockopt_count;
static int g_resolve_count;
static int g_bind_result;
static int g_bind_count;
static int g_close_count;
static int g_recv_timeout_result;
static int g_recv_timeout_count;
static unsigned int g_last_recv_timeout_ms;
static char g_last_resolve_host[64];
static int g_last_resolve_port;
static uint32_t g_last_bind_addr;
static int g_last_bind_port;
static dsd_socket_t g_last_sendto_sock;
static uint8_t g_last_sendto_data[32];
static size_t g_last_sendto_len;
static int g_sendto_result;
static int g_recvfrom_result;
static uint8_t g_recvfrom_payload[32];
static size_t g_recvfrom_payload_len;

static void
reset_stubs(void) {
    g_create_result = 11;
    g_setsockopt_result = 0;
    g_resolve_result = 0;
    g_bind_result = 0;
    g_recv_timeout_result = 0;
    g_create_count = 0;
    g_setsockopt_count = 0;
    g_resolve_count = 0;
    g_bind_count = 0;
    g_close_count = 0;
    g_recv_timeout_count = 0;
    g_last_recv_timeout_ms = 0U;
    DSD_MEMSET(g_last_resolve_host, 0, sizeof(g_last_resolve_host));
    g_last_resolve_port = 0;
    g_last_bind_addr = 0U;
    g_last_bind_port = 0;
    g_last_sendto_sock = DSD_INVALID_SOCKET;
    DSD_MEMSET(g_last_sendto_data, 0, sizeof(g_last_sendto_data));
    g_last_sendto_len = 0;
    g_sendto_result = 0;
    g_recvfrom_result = 0;
    DSD_MEMSET(g_recvfrom_payload, 0, sizeof(g_recvfrom_payload));
    g_recvfrom_payload_len = 0;
}

dsd_socket_t
dsd_socket_create(int domain, int type, int protocol) {
    (void)domain;
    (void)type;
    (void)protocol;
    g_create_count++;
    return g_create_result;
}

int
dsd_socket_close(dsd_socket_t sock) {
    (void)sock;
    g_close_count++;
    return 0;
}

int
dsd_socket_bind(dsd_socket_t sock, const struct sockaddr* addr, int addrlen) {
    (void)sock;
    (void)addrlen;
    g_bind_count++;
    const struct sockaddr_in* in = (const struct sockaddr_in*)addr;
    if (in) {
        g_last_bind_addr = ntohl(in->sin_addr.s_addr);
        g_last_bind_port = ntohs(in->sin_port);
    }
    return g_bind_result;
}

int
dsd_socket_set_recv_timeout(dsd_socket_t sock, unsigned int timeout_ms) {
    (void)sock;
    g_recv_timeout_count++;
    g_last_recv_timeout_ms = timeout_ms;
    return g_recv_timeout_result;
}

int
dsd_socket_setsockopt(dsd_socket_t sock, int level, int optname, const void* optval, int optlen) {
    (void)sock;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    g_setsockopt_count++;
    return g_setsockopt_result;
}

int
dsd_socket_resolve(const char* hostname, int port, struct sockaddr_in* addr) {
    g_resolve_count++;
    DSD_SNPRINTF(g_last_resolve_host, sizeof(g_last_resolve_host), "%s", hostname ? hostname : "");
    g_last_resolve_port = port;
    if (addr) {
        DSD_MEMSET(addr, 0, sizeof(*addr));
        addr->sin_family = AF_INET;
        addr->sin_port = htons((uint16_t)port);
    }
    return g_resolve_result;
}

int
dsd_socket_sendto(dsd_socket_t sock, const void* buf, size_t len, int flags, const struct sockaddr* dest_addr,
                  int addrlen) {
    (void)flags;
    (void)dest_addr;
    (void)addrlen;
    g_last_sendto_sock = sock;
    g_last_sendto_len = len;
    size_t copy_len = len;
    if (copy_len > sizeof(g_last_sendto_data)) {
        copy_len = sizeof(g_last_sendto_data);
    }
    if (buf && copy_len > 0U) {
        DSD_MEMCPY(g_last_sendto_data, buf, copy_len);
    }
    return g_sendto_result ? g_sendto_result : (int)len;
}

int
dsd_socket_recvfrom(dsd_socket_t sock, void* buf, size_t len, int flags, struct sockaddr* src_addr, int* addrlen) {
    (void)sock;
    (void)flags;
    (void)src_addr;
    (void)addrlen;
    size_t copy_len = g_recvfrom_payload_len;
    if (copy_len > len) {
        copy_len = len;
    }
    if (buf && copy_len > 0U) {
        DSD_MEMCPY(buf, g_recvfrom_payload, copy_len);
    }
    return g_recvfrom_result ? g_recvfrom_result : (int)copy_len;
}

static void
init_opts(dsd_opts* opts) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_SNPRINTF(opts->udp_hostname, sizeof(opts->udp_hostname), "%s", "239.1.2.3");
    opts->udp_portno = 23456;
    DSD_SNPRINTF(opts->m17_hostname, sizeof(opts->m17_hostname), "%s", "239.4.5.6");
    opts->m17_portno = 17000;
}

static int
test_udp_connect_failures(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_opts(&opts);
    DSD_MEMSET(&state, 0, sizeof(state));

    reset_stubs();
    g_create_result = DSD_INVALID_SOCKET;
    assert(udp_socket_connect(&opts, &state) == -1);
    assert(g_create_count == 1);
    assert(g_setsockopt_count == 0);
    assert(g_resolve_count == 0);

    reset_stubs();
    g_setsockopt_result = 7;
    assert(udp_socket_connect(&opts, &state) == 7);
    assert(opts.udp_sockfd == 11);
    assert(g_setsockopt_count == 1);
    assert(g_resolve_count == 0);

    reset_stubs();
    g_resolve_result = -1;
    assert(udp_socket_connect(&opts, &state) == -1);
    assert(g_resolve_count == 1);
    assert(strcmp(g_last_resolve_host, "239.1.2.3") == 0);
    assert(g_last_resolve_port == 23456);

    reset_stubs();
    g_create_result = DSD_INVALID_SOCKET;
    assert(udp_socket_connectA(&opts, &state) == -1);
    assert(g_create_count == 1);
    assert(g_setsockopt_count == 0);
    assert(g_resolve_count == 0);

    reset_stubs();
    g_setsockopt_result = 8;
    assert(udp_socket_connectA(&opts, &state) == 8);
    assert(opts.udp_sockfdA == 11);
    assert(g_setsockopt_count == 1);
    assert(g_resolve_count == 0);

    reset_stubs();
    g_resolve_result = -1;
    assert(udp_socket_connectA(&opts, &state) == -1);
    assert(g_resolve_count == 1);
    assert(strcmp(g_last_resolve_host, "239.1.2.3") == 0);
    assert(g_last_resolve_port == 23458);
    return 0;
}

static int
test_udp_connect_success_and_send_paths(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_opts(&opts);
    DSD_MEMSET(&state, 0, sizeof(state));

    reset_stubs();
    assert(udp_socket_connect(&opts, &state) == 0);
    assert(opts.udp_sockfd == 11);
    assert(g_setsockopt_count == 1);
    assert(g_resolve_count == 1);
    assert(strcmp(g_last_resolve_host, "239.1.2.3") == 0);
    assert(g_last_resolve_port == 23456);

    const uint8_t payload[] = {1, 2, 3, 4};
    udp_socket_blaster(&opts, &state, sizeof(payload), payload);
    assert(g_last_sendto_sock == opts.udp_sockfd);
    assert(g_last_sendto_len == sizeof(payload));
    assert(memcmp(g_last_sendto_data, payload, sizeof(payload)) == 0);

    reset_stubs();
    init_opts(&opts);
    assert(udp_socket_connectA(&opts, &state) == 0);
    assert(opts.udp_sockfdA == 11);
    assert(g_last_resolve_port == 23458);

    const uint8_t analog_payload[] = {9, 8, 7};
    udp_socket_blasterA(&opts, &state, sizeof(analog_payload), analog_payload);
    assert(g_last_sendto_sock == opts.udp_sockfdA);
    assert(g_last_sendto_len == sizeof(analog_payload));
    assert(memcmp(g_last_sendto_data, analog_payload, sizeof(analog_payload)) == 0);
    return 0;
}

static int
test_m17_connect_send_and_receive(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_opts(&opts);
    DSD_MEMSET(&state, 0, sizeof(state));

    reset_stubs();
    assert(udp_socket_connectM17(&opts, &state) == 0);
    assert(opts.m17_udp_sock == 11);
    assert(g_setsockopt_count == 1);
    assert(strcmp(g_last_resolve_host, "239.4.5.6") == 0);
    assert(g_last_resolve_port == 17000);

    const uint8_t frame[] = {0x4D, 0x31, 0x37};
    assert(m17_socket_blaster(&opts, &state, sizeof(frame), frame) == (int)sizeof(frame));
    assert(g_last_sendto_sock == opts.m17_udp_sock);
    assert(g_last_sendto_len == sizeof(frame));
    assert(memcmp(g_last_sendto_data, frame, sizeof(frame)) == 0);

    const uint8_t reply[] = {0x41, 0x43, 0x4B, 0x4E};
    DSD_MEMCPY(g_recvfrom_payload, reply, sizeof(reply));
    g_recvfrom_payload_len = sizeof(reply);
    uint8_t out[8] = {0};
    assert(m17_socket_receiver(&opts, out) == (int)sizeof(reply));
    assert(memcmp(out, reply, sizeof(reply)) == 0);
    return 0;
}

static int
test_m17_connect_failures(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_opts(&opts);
    DSD_MEMSET(&state, 0, sizeof(state));

    reset_stubs();
    g_create_result = DSD_INVALID_SOCKET;
    assert(udp_socket_connectM17(&opts, &state) == -1);
    assert(g_setsockopt_count == 0);

    reset_stubs();
    g_setsockopt_result = 9;
    assert(udp_socket_connectM17(&opts, &state) == 9);
    assert(g_resolve_count == 0);
    assert(g_close_count == 1);
    assert(opts.m17_udp_sock == DSD_INVALID_SOCKET);

    reset_stubs();
    init_opts(&opts);
    g_resolve_result = -1;
    assert(udp_socket_connectM17(&opts, &state) == -1);
    assert(g_resolve_count == 1);
    assert(g_close_count == 1);
    assert(opts.m17_udp_sock == DSD_INVALID_SOCKET);
    return 0;
}

static int
test_udp_bind_paths(void) {
    char empty[] = "";
    char loopback[] = "127.0.0.1";
    char any[] = "0.0.0.0";

    reset_stubs();
    g_create_result = DSD_INVALID_SOCKET;
    assert(UDPBind(loopback, 17001) == DSD_INVALID_SOCKET);
    assert(g_create_count == 1);
    assert(g_resolve_count == 0);
    assert(g_bind_count == 0);
    assert(g_close_count == 0);

    reset_stubs();
    g_resolve_result = -1;
    assert(UDPBind(loopback, 17002) == DSD_INVALID_SOCKET);
    assert(g_resolve_count == 1);
    assert(strcmp(g_last_resolve_host, "127.0.0.1") == 0);
    assert(g_last_resolve_port == 17002);
    assert(g_bind_count == 0);
    assert(g_close_count == 1);

    reset_stubs();
    g_bind_result = -1;
    assert(UDPBind(empty, 17003) == DSD_INVALID_SOCKET);
    assert(g_resolve_count == 1);
    assert(strcmp(g_last_resolve_host, "127.0.0.1") == 0);
    assert(g_bind_count == 1);
    assert(g_close_count == 1);
    assert(g_recv_timeout_count == 0);

    reset_stubs();
    assert(UDPBind(any, 17004) == 11);
    assert(g_resolve_count == 0);
    assert(g_bind_count == 1);
    assert(g_last_bind_addr == INADDR_ANY);
    assert(g_last_bind_port == 17004);
    assert(g_recv_timeout_count == 1);
    assert(g_last_recv_timeout_ms == 1U);

    reset_stubs();
    assert(UDPBind(NULL, 17005) == 11);
    assert(g_resolve_count == 1);
    assert(strcmp(g_last_resolve_host, "127.0.0.1") == 0);
    assert(g_last_bind_port == 17005);
    assert(g_recv_timeout_count == 1);
    return 0;
}

int
main(void) {
    int rc = 0;
    rc |= test_udp_connect_failures();
    rc |= test_udp_connect_success_and_send_paths();
    rc |= test_m17_connect_send_and_receive();
    rc |= test_m17_connect_failures();
    rc |= test_udp_bind_paths();
    return rc;
}
