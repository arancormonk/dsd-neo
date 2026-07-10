// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-multi-level-implicit-pointer-conversion)
/*
 * Rigctl control-plane tests use socket stubs to verify command/response
 * behavior without requiring a live rigctl server or network service.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/rtl_stream_fwd.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"

#if !DSD_PLATFORM_WIN_NATIVE
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#define MAX_COMMANDS  16
#define MAX_RESPONSES 16

static char g_commands[MAX_COMMANDS][64];
static size_t g_command_count;
static const char* g_responses[MAX_RESPONSES];
static size_t g_response_count;
static size_t g_response_index;
static dsd_socket_t g_create_result;
static int g_resolve_result;
static int g_connect_result;
static int g_close_count;
static dsd_socket_t g_closed_sock;
static int g_recv_timeout_count;
static unsigned int g_last_timeout_ms;
static int g_setsockopt_count;
static int g_last_setsockopt_level;
static int g_last_setsockopt_name;
static int g_sleep_count;
static int g_sendto_count;
static unsigned char g_last_sendto_payload[8];
static size_t g_last_sendto_len;
static int g_last_sendto_port;
static int g_rtl_tune_calls;
static uint32_t g_rtl_tune_freq;
static int g_rtl_tune_result;
static dsdneoRuntimeConfig g_config;

bool dsd_rigctl_test_get_signal_level(dsd_socket_t sockfd, double* dB);
bool dsd_rigctl_test_get_squelch_level(dsd_socket_t sockfd, double* dB);
bool dsd_rigctl_test_set_squelch_level(dsd_socket_t sockfd, double dB);
bool dsd_rigctl_test_get_signal_level_ex(dsd_socket_t sockfd, double* dB, int n_samp);
void dsd_rigctl_test_rtl_udp_tune(dsd_opts* opts, dsd_state* state, long int frequency);

static void
reset_stubs(void) {
    DSD_MEMSET(g_commands, 0, sizeof(g_commands));
    DSD_MEMSET(g_responses, 0, sizeof(g_responses));
    g_command_count = 0;
    g_response_count = 0;
    g_response_index = 0;
    g_create_result = 41;
    g_resolve_result = 0;
    g_connect_result = 0;
    g_close_count = 0;
    g_closed_sock = DSD_INVALID_SOCKET;
    g_recv_timeout_count = 0;
    g_last_timeout_ms = 0;
    g_setsockopt_count = 0;
    g_last_setsockopt_level = 0;
    g_last_setsockopt_name = 0;
    g_sleep_count = 0;
    g_sendto_count = 0;
    DSD_MEMSET(g_last_sendto_payload, 0, sizeof(g_last_sendto_payload));
    g_last_sendto_len = 0;
    g_last_sendto_port = 0;
    g_rtl_tune_calls = 0;
    g_rtl_tune_freq = 0U;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    DSD_MEMSET(&g_config, 0, sizeof(g_config));
    g_config.rigctl_rcvtimeo_ms = 1500;
}

int
rtl_stream_tune(RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    g_rtl_tune_calls++;
    g_rtl_tune_freq = center_freq_hz;
    return g_rtl_tune_result;
}

static void
push_response(const char* response) {
    assert(g_response_count < MAX_RESPONSES);
    g_responses[g_response_count++] = response;
}

void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    (void)level;
    (void)format;
}

const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    return &g_config;
}

void
dsd_neo_config_init(const dsd_opts* opts) {
    (void)opts;
}

void
dsd_sleep_ms(unsigned int ms) {
    (void)ms;
    g_sleep_count++;
}

dsd_socket_t
dsd_socket_create(int domain, int type, int protocol) {
    (void)domain;
    (void)type;
    (void)protocol;
    return g_create_result;
}

int
dsd_socket_close(dsd_socket_t sock) {
    g_close_count++;
    g_closed_sock = sock;
    return 0;
}

int
dsd_socket_resolve(const char* hostname, int port, struct sockaddr_in* addr) {
    (void)hostname;
    if (addr) {
        DSD_MEMSET(addr, 0, sizeof(*addr));
        addr->sin_family = AF_INET;
        addr->sin_port = htons((uint16_t)port);
    }
    return g_resolve_result;
}

int
dsd_socket_connect(dsd_socket_t sock, const struct sockaddr* addr, int addrlen) {
    (void)sock;
    (void)addr;
    (void)addrlen;
    return g_connect_result;
}

int
dsd_socket_set_recv_timeout(dsd_socket_t sock, unsigned int timeout_ms) {
    (void)sock;
    g_recv_timeout_count++;
    g_last_timeout_ms = timeout_ms;
    return 0;
}

int
dsd_socket_setsockopt(dsd_socket_t sock, int level, int optname, const void* optval, int optlen) {
    (void)sock;
    (void)optval;
    (void)optlen;
    g_setsockopt_count++;
    g_last_setsockopt_level = level;
    g_last_setsockopt_name = optname;
    return 0;
}

int
dsd_socket_send(dsd_socket_t sock, const void* buf, size_t len, int flags) {
    (void)sock;
    (void)flags;
    assert(g_command_count < MAX_COMMANDS);
    size_t copy_len = len;
    if (copy_len >= sizeof(g_commands[g_command_count])) {
        copy_len = sizeof(g_commands[g_command_count]) - 1U;
    }
    DSD_MEMCPY(g_commands[g_command_count], buf, copy_len);
    g_commands[g_command_count][copy_len] = '\0';
    g_command_count++;
    return (int)len;
}

int
dsd_socket_sendto(dsd_socket_t sock, const void* buf, size_t len, int flags, const struct sockaddr* dest_addr,
                  int addrlen) {
    (void)sock;
    (void)flags;
    (void)addrlen;
    g_sendto_count++;
    g_last_sendto_len = len;
    size_t copy_len = len;
    if (copy_len > sizeof(g_last_sendto_payload)) {
        copy_len = sizeof(g_last_sendto_payload);
    }
    if (buf && copy_len > 0U) {
        DSD_MEMCPY(g_last_sendto_payload, buf, copy_len);
    }
    if (dest_addr) {
        const struct sockaddr_in* in = (const struct sockaddr_in*)dest_addr;
        g_last_sendto_port = ntohs(in->sin_port);
    }
    return (int)len;
}

int
dsd_socket_recv(dsd_socket_t sock, void* buf, size_t len, int flags) {
    (void)sock;
    (void)flags;
    if (!buf || len == 0U || g_response_index >= g_response_count) {
        return 0;
    }
    const char* response = g_responses[g_response_index++];
    size_t response_len = strlen(response);
    if (response_len > len) {
        response_len = len;
    }
    DSD_MEMCPY(buf, response, response_len);
    return (int)response_len;
}

static int
test_connect_failure_cleanup(void) {
    reset_stubs();
    char host[] = "rig.invalid";
    g_create_result = 77;
    g_resolve_result = -1;
    assert(Connect(host, 4532) == DSD_INVALID_SOCKET);
    assert(g_close_count == 1);
    assert(g_closed_sock == 77);
    assert(g_recv_timeout_count == 0);

    reset_stubs();
    g_create_result = 78;
    g_connect_result = -1;
    assert(Connect(host, 4532) == DSD_INVALID_SOCKET);
    assert(g_close_count == 1);
    assert(g_closed_sock == 78);
    assert(g_recv_timeout_count == 0);
    return 0;
}

static int
test_connect_success_uses_rigctl_timeout(void) {
    reset_stubs();
    char host[] = "127.0.0.1";
    g_create_result = 79;
    g_config.rigctl_rcvtimeo_ms = 2345;
    g_config.rigctl_rcvtimeo_is_set = 1;
    g_config.tcp_rcvtimeo_ms = 3456;
    g_config.tcp_rcvtimeo_is_set = 1;

    assert(Connect(host, 4532) == 79);
    assert(g_close_count == 0);
    assert(g_recv_timeout_count == 1);
    assert(g_last_timeout_ms == 2345U);
    assert(g_setsockopt_count == 1);
    assert(g_last_setsockopt_level == IPPROTO_TCP);
    assert(g_last_setsockopt_name == TCP_NODELAY);
    return 0;
}

static int
test_setfreq_success_failure_and_cache(void) {
    reset_stubs();
    push_response("RPRT 0\n");
    assert(SetFreq(100, 851012500L));
    assert(g_command_count == 1);
    assert(strcmp(g_commands[0], "F 851012500\n") == 0);

    assert(SetFreq(100, 851012500L));
    assert(g_command_count == 1);

    push_response("RPRT 1\n");
    assert(!SetFreq(100, 851025000L));
    assert(g_command_count == 2);
    assert(strcmp(g_commands[1], "F 851025000\n") == 0);
    return 0;
}

static int
test_setmodulation_fallback_and_cache(void) {
    reset_stubs();
    push_response("RPRT 1\n");
    push_response("RPRT 0\n");
    assert(SetModulation(101, 12500));
    assert(g_command_count == 2);
    assert(strcmp(g_commands[0], "M NFM 12500\n") == 0);
    assert(strcmp(g_commands[1], "M FM 12500\n") == 0);

    assert(SetModulation(101, 12500));
    assert(g_command_count == 2);

    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    assert(!SetModulation(101, 25000));
    assert(g_command_count == 4);
    assert(strcmp(g_commands[2], "M NFM 25000\n") == 0);
    assert(strcmp(g_commands[3], "M FM 25000\n") == 0);
    return 0;
}

static int
test_get_current_freq_parses_first_line_and_errors(void) {
    reset_stubs();
    push_response("851037500\nRPRT 0\n");
    assert(GetCurrentFreq(102) == 851037500L);
    assert(g_command_count == 1);
    assert(strcmp(g_commands[0], "f\n") == 0);

    reset_stubs();
    push_response("RPRT 1");
    assert(GetCurrentFreq(102) == 0L);
    return 0;
}

static int
test_io_control_set_freq_validation_and_rigctl_dispatch(void) {
    reset_stubs();
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    assert(io_control_set_freq(NULL, &state, 851000000L) == -1);
    assert(io_control_set_freq(&opts, &state, 0L) == -1);
    assert(g_command_count == 0);

    opts.use_rigctl = 1;
    opts.rigctl_sockfd = 103;
    opts.setmod_bw = 12500;
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");

    assert(io_control_set_freq(&opts, &state, 851050000L) == 0);
    assert(opts.rtlsdr_center_freq == 851050000U);
    assert(g_command_count == 2);
    assert(strcmp(g_commands[0], "M NFM 12500\n") == 0);
    assert(strcmp(g_commands[1], "F 851050000\n") == 0);
    return 0;
}

static int
test_io_control_set_freq_propagates_rtl_deferred(void) {
    reset_stubs();
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtlsdr_center_freq = 851000000U;
    state.rtl_ctx = (RtlSdrContext*)&state;
    g_rtl_tune_result = RTL_STREAM_TUNE_DEFERRED;

    assert(io_control_set_freq(&opts, &state, 851075000L) == RTL_STREAM_TUNE_DEFERRED);
    assert(g_rtl_tune_calls == 1);
    assert(g_rtl_tune_freq == 851075000U);
    assert(opts.rtlsdr_center_freq == 851000000U);

    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    assert(io_control_set_freq(&opts, &state, 851075000L) == RTL_STREAM_TUNE_OK);
    assert(g_rtl_tune_calls == 2);
    assert(opts.rtlsdr_center_freq == 851075000U);
    return 0;
}

static int
test_signal_and_squelch_helpers(void) {
    reset_stubs();
    double value = -999.0;
    push_response("12.34\n");
    assert(dsd_rigctl_test_get_signal_level(104, &value));
    assert(value == 12.3);
    assert(g_command_count == 1);
    assert(strcmp(g_commands[0], "l\n") == 0);

    reset_stubs();
    value = -999.0;
    push_response("0.0\n");
    assert(!dsd_rigctl_test_get_signal_level(104, &value));
    assert(value == 0.0);

    reset_stubs();
    value = -999.0;
    push_response("12.0 trailing\n");
    assert(!dsd_rigctl_test_get_signal_level(104, &value));
    assert(value == -999.0);

    reset_stubs();
    value = -999.0;
    push_response("-45.66\n");
    assert(dsd_rigctl_test_get_squelch_level(104, &value));
    assert(value == -45.7);
    assert(g_command_count == 1);
    assert(strcmp(g_commands[0], "l SQL\n") == 0);

    reset_stubs();
    push_response("RPRT 1");
    assert(!dsd_rigctl_test_get_squelch_level(104, &value));

    reset_stubs();
    push_response("RPRT 0");
    assert(dsd_rigctl_test_set_squelch_level(104, 4.5));
    assert(g_command_count == 1);
    assert(strcmp(g_commands[0], "L SQL 4.500000\n") == 0);

    reset_stubs();
    push_response("RPRT 1");
    assert(!dsd_rigctl_test_set_squelch_level(104, 4.5));
    return 0;
}

static int
test_signal_level_average_tolerates_failed_samples(void) {
    reset_stubs();
    double value = -999.0;
    push_response("10.04\n");
    push_response("RPRT 1");
    push_response("14.04\n");

    assert(dsd_rigctl_test_get_signal_level_ex(105, &value, 3));
    assert(value == 12.0);
    assert(g_command_count == 3);
    assert(g_sleep_count == 3);
    assert(strcmp(g_commands[0], "l\n") == 0);
    assert(strcmp(g_commands[1], "l\n") == 0);
    assert(strcmp(g_commands[2], "l\n") == 0);
    return 0;
}

static int
test_legacy_udp_tune_payload_and_cache(void) {
    reset_stubs();
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.rtl_udp_port = 6021;

    dsd_rigctl_test_rtl_udp_tune(&opts, &state, 0x12345678L);
    assert(opts.rtlsdr_center_freq == 0x12345678U);
    assert(g_sendto_count == 1);
    assert(g_last_sendto_len == 5U);
    assert(g_last_sendto_port == 6021);
    assert(g_last_sendto_payload[0] == 0x00);
    assert(g_last_sendto_payload[1] == 0x78);
    assert(g_last_sendto_payload[2] == 0x56);
    assert(g_last_sendto_payload[3] == 0x34);
    assert(g_last_sendto_payload[4] == 0x12);
    assert(g_close_count == 1);

    dsd_rigctl_test_rtl_udp_tune(&opts, &state, 0x12345678L);
    assert(g_sendto_count == 1);

    dsd_rigctl_test_rtl_udp_tune(&opts, &state, 0x12345679L);
    assert(g_sendto_count == 2);

    reset_stubs();
    g_create_result = DSD_INVALID_SOCKET;
    opts.rtlsdr_center_freq = 0;
    dsd_rigctl_test_rtl_udp_tune(&opts, &state, 0x12345680L);
    assert(opts.rtlsdr_center_freq == 0x12345680U);
    assert(g_sendto_count == 0);
    assert(g_close_count == 0);
    return 0;
}

int
main(void) {
    int rc = 0;
    rc |= test_connect_failure_cleanup();
    rc |= test_connect_success_uses_rigctl_timeout();
    rc |= test_setfreq_success_failure_and_cache();
    rc |= test_setmodulation_fallback_and_cache();
    rc |= test_get_current_freq_parses_first_line_and_errors();
    rc |= test_io_control_set_freq_validation_and_rigctl_dispatch();
    rc |= test_io_control_set_freq_propagates_rtl_deferred();
    rc |= test_signal_and_squelch_helpers();
    rc |= test_signal_level_average_tolerates_failed_samples();
    rc |= test_legacy_udp_tune_payload_and_cache();
    return rc;
}

// NOLINTEND(bugprone-multi-level-implicit-pointer-conversion)
