// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-multi-level-implicit-pointer-conversion)
/*
 * Rigctl control-plane tests use socket stubs to verify command/response
 * behavior without requiring a live rigctl server or network service.
 */

#include <assert.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/rtl_stream_fwd.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"

#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#define MAX_COMMANDS  32
#define MAX_RESPONSES 32

static char g_commands[MAX_COMMANDS][64];
static size_t g_command_count;
static const char* g_responses[MAX_RESPONSES];
static size_t g_response_count;
static size_t g_response_index;
static dsd_socket_t g_create_result;
static int g_send_result;
static int g_resolve_result;
static int g_connect_result;
static int g_resolve_count;
static int g_bounded_connect_count;
static unsigned int g_bounded_timeout_ms;
static int g_bounded_cancelled;
static int g_close_count;
static dsd_socket_t g_closed_sock;
static int g_recv_timeout_count;
static unsigned int g_last_timeout_ms;
static int g_setsockopt_count;
static int g_last_setsockopt_level;
static int g_last_setsockopt_name;
static int g_rtl_tune_calls;
static uint32_t g_rtl_tune_freq;
static int g_rtl_tune_result;
static uint32_t g_rtl_last_applied_freq;
static dsdneoRuntimeConfig g_config;
/* What the scan scope says of a manual tune's request (dsd_scan_mode_rigctl_request_is_session(); 1: the session's,
   off any scan), and the state it was asked about. */
static int g_session_request;
static const dsd_state* g_session_request_state;
/* Whether the configured -Y list is typed (dsd_channel_modes_present()), which a manual tune's request weighs on top of
   the scope's answer (dsd_channel_modes_rigctl_request_is_session()). */
static int g_channel_modes_present;

static void
reset_stubs(void) {
    DSD_MEMSET(g_commands, 0, sizeof(g_commands));
    for (size_t i = 0; i < MAX_RESPONSES; i++) {
        g_responses[i] = NULL;
    }
    g_command_count = 0;
    g_response_count = 0;
    g_response_index = 0;
    g_create_result = 41;
    g_send_result = -2;
    g_resolve_result = 0;
    g_connect_result = 0;
    g_resolve_count = 0;
    g_bounded_connect_count = 0;
    g_bounded_timeout_ms = 0U;
    g_bounded_cancelled = 0;
    g_close_count = 0;
    g_closed_sock = DSD_INVALID_SOCKET;
    g_recv_timeout_count = 0;
    g_last_timeout_ms = 0;
    g_setsockopt_count = 0;
    g_last_setsockopt_level = 0;
    g_last_setsockopt_name = 0;
    g_rtl_tune_calls = 0;
    g_rtl_tune_freq = 0U;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_last_applied_freq = 0U;
    DSD_MEMSET(&g_config, 0, sizeof(g_config));
    g_config.rigctl_rcvtimeo_ms = 1500;
    g_session_request = 0;
    g_session_request_state = NULL;
    g_channel_modes_present = 0;
}

int
dsd_scan_mode_rigctl_request_is_session(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    g_session_request_state = state;
    return g_session_request;
}

int
dsd_channel_modes_present(const dsd_state* state) {
    (void)state;
    return g_channel_modes_present;
}

int
rtl_stream_tune(RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    g_rtl_tune_calls++;
    g_rtl_tune_freq = center_freq_hz;
    return g_rtl_tune_result;
}

int
rtl_stream_get_last_applied_freq(uint32_t* out_freq_hz) {
    if (!out_freq_hz) {
        return -1;
    }
    *out_freq_hz = g_rtl_last_applied_freq;
    return 0;
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
dsd_neo_config_init(void) {}

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
    g_resolve_count++;
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

/* A pending connection that asks the cancel callback once, as dsd_socket_connect_bounded() does every 100 ms. */
int
dsd_socket_connect_bounded(dsd_socket_t sock, const struct sockaddr* addr, int addrlen, unsigned int timeout_ms,
                           dsd_socket_cancel_fn cancelled, void* context, int* error_code) {
    (void)sock;
    (void)addr;
    (void)addrlen;
    g_bounded_connect_count++;
    g_bounded_timeout_ms = timeout_ms;
    if (cancelled && cancelled(context)) {
        g_bounded_cancelled++;
        if (error_code) {
            *error_code = -1;
        }
        return -1;
    }
    if (error_code) {
        *error_code = 0;
    }
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
    if (g_send_result != -2) {
        return g_send_result;
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
cancel_always(void* context) {
    (void)context;
    return 1;
}

/* Issue #634: the TCP audio input's menu connect and its reconnect connect within a bound, with the rigctl receive
   timeout and options, and a reconnect connects to the address the running input's connection went to, looking
   nothing up. */
static int
test_connect_bounded_resolves_once_and_bounds_the_connect(void) {
    reset_stubs();
    g_create_result = 81;
    assert(ConnectBounded("audio.example", 7356, 1, NULL, NULL) == 81);
    assert(g_resolve_count == 1);
    assert(g_bounded_connect_count == 1);
    assert(g_bounded_timeout_ms == DSD_CONNECT_BOUNDED_TIMEOUT_MS);
    assert(g_recv_timeout_count == 1);
    assert(g_last_setsockopt_name == TCP_NODELAY);
    ConnectKeepTcpAudioAddress("audio.example", 7356); /* the switch took it */

    /* A reconnect to the same host and port reuses the address. */
    g_create_result = 82;
    assert(ConnectBounded("audio.example", 7356, 0, NULL, NULL) == 82);
    assert(g_resolve_count == 1);
    assert(g_bounded_connect_count == 2);

    /* A connect elsewhere that no switch took (it failed) leaves the running input's address alone. */
    g_create_result = 87;
    assert(ConnectBounded("other.example", 7400, 1, NULL, NULL) == 87);
    assert(g_resolve_count == 2);
    ConnectKeepTcpAudioAddress("audio.example", 7356); /* not the last connection: nothing changes */
    assert(ConnectBounded("audio.example", 7356, 0, NULL, NULL) == 87);
    assert(g_resolve_count == 2);

    /* Another port, or a fresh connect, looks the host up. */
    g_create_result = 83;
    assert(ConnectBounded("audio.example", 7357, 0, NULL, NULL) == 83);
    assert(g_resolve_count == 3);
    assert(ConnectBounded("audio.example", 7357, 1, NULL, NULL) == 83);
    assert(g_resolve_count == 4);
    return 0;
}

/* The startup connection (Connect()) is kept too: the first reconnect of a TCP input started with -i tcp looks nothing
   up either. */
static int
test_startup_connect_is_kept_for_the_reconnect(void) {
    reset_stubs();
    g_create_result = 88;
    char host[] = "start.example";
    assert(Connect(host, 7410) == 88);
    assert(g_resolve_count == 1);
    ConnectKeepTcpAudioAddress("start.example", 7410);
    assert(ConnectBounded("start.example", 7410, 0, NULL, NULL) == 88);
    assert(g_resolve_count == 1);
    return 0;
}

static int
test_connect_bounded_cancel_and_failures_close_the_socket(void) {
    reset_stubs();
    g_create_result = 84;
    assert(ConnectBounded("audio.example", 7358, 1, cancel_always, NULL) == DSD_INVALID_SOCKET);
    assert(g_bounded_cancelled == 1);
    assert(g_close_count == 1);
    assert(g_closed_sock == 84);
    assert(g_recv_timeout_count == 0);

    reset_stubs();
    g_create_result = 85;
    g_connect_result = -1;
    assert(ConnectBounded("audio.example", 7359, 1, NULL, NULL) == DSD_INVALID_SOCKET);
    assert(g_close_count == 1);
    assert(g_closed_sock == 85);

    /* A lookup that fails opens no socket, and leaves nothing for a reconnect to reuse. */
    reset_stubs();
    g_resolve_result = -1;
    assert(ConnectBounded("nowhere.invalid", 7360, 1, NULL, NULL) == DSD_INVALID_SOCKET);
    assert(g_bounded_connect_count == 0);
    assert(g_close_count == 0);
    g_resolve_result = 0;
    g_create_result = 86;
    assert(ConnectBounded("nowhere.invalid", 7360, 0, NULL, NULL) == 86);
    assert(g_resolve_count == 2);

    reset_stubs();
    assert(ConnectBounded("", 7361, 1, NULL, NULL) == DSD_INVALID_SOCKET);
    assert(ConnectBounded("audio.example", 0, 1, NULL, NULL) == DSD_INVALID_SOCKET);
    assert(g_resolve_count == 0);
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

    reset_stubs();
    assert(!SetFreq(110, 851037500L));
    assert(g_command_count == 1);

    reset_stubs();
    g_send_result = -1;
    assert(!SetFreq(111, 851050000L));

    reset_stubs();
    g_send_result = 2;
    assert(!SetFreq(112, 851062500L));
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

    reset_stubs();
    assert(!SetModulation(113, 6250));
    assert(g_command_count == 1);

    reset_stubs();
    g_send_result = 1;
    assert(!SetModulation(114, 7500));
    return 0;
}

/* Issue #526: an AM scan row asks the peer for "M AM <width>". The cache is keyed on the demodulator and the passband
 * together, so FM and AM at one width are two requests. FM at the peer's normal passband (0) is sent only to undo a
 * request made on the socket -- an AM row's demodulator here -- and never to a peer nothing was asked of. A refusal
 * fails the request and leaves the cache on what the peer last accepted, so the next call asks again. */
static int
test_setmodulation_kind_am_and_passband_restore(void) {
    reset_stubs();
    assert(SetModulationKind(120, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 0);

    push_response("RPRT 0\n");
    assert(SetModulationKind(120, DSD_ANALOG_DEMOD_AM, 8333));
    assert(g_command_count == 1 && strcmp(g_commands[0], "M AM 8333\n") == 0);
    assert(SetModulationKind(120, DSD_ANALOG_DEMOD_AM, 8333));
    assert(g_command_count == 1);

    push_response("RPRT 0\n");
    assert(SetModulationKind(120, DSD_ANALOG_DEMOD_FM, 8333));
    assert(g_command_count == 2 && strcmp(g_commands[1], "M NFM 8333\n") == 0);

    /* Off AM again at the peer's normal passband, through the FM-token fallback. */
    push_response("RPRT 0\n");
    assert(SetModulationKind(120, DSD_ANALOG_DEMOD_AM, 6000));
    push_response("RPRT -11\n");
    push_response("RPRT 0\n");
    assert(SetModulationKind(120, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 5 && strcmp(g_commands[2], "M AM 6000\n") == 0);
    assert(strcmp(g_commands[3], "M NFM 0\n") == 0 && strcmp(g_commands[4], "M FM 0\n") == 0);
    assert(SetModulationKind(120, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 5);

    /* A peer that refuses AM gets no FM-token retry, and the cache keeps its FM. */
    push_response("RPRT -1\n");
    assert(!SetModulationKind(120, DSD_ANALOG_DEMOD_AM, 8333));
    assert(g_command_count == 6 && strcmp(g_commands[5], "M AM 8333\n") == 0);
    assert(SetModulationKind(120, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 6);
    push_response("RPRT 0\n");
    assert(SetModulationKind(120, DSD_ANALOG_DEMOD_AM, 8333));
    assert(g_command_count == 7);

    /* A dead link fails the request after the one send. */
    reset_stubs();
    assert(!SetModulationKind(121, DSD_ANALOG_DEMOD_AM, 8333));
    assert(g_command_count == 1);

    /* A reply lost after the request went out leaves what the peer runs unknown: the peer may have taken the AM, so the
       FM undo that would match the cache before it is sent all the same. */
    reset_stubs();
    push_response("RPRT 0\n");
    assert(SetModulationKind(122, DSD_ANALOG_DEMOD_AM, 8333));
    push_response("RPRT 0\n");
    assert(SetModulationKind(122, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 2 && strcmp(g_commands[1], "M NFM 0\n") == 0);
    assert(!SetModulationKind(122, DSD_ANALOG_DEMOD_AM, 6000));
    assert(g_command_count == 3 && strcmp(g_commands[2], "M AM 6000\n") == 0);
    push_response("RPRT 0\n");
    assert(SetModulationKind(122, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 4 && strcmp(g_commands[3], "M NFM 0\n") == 0);
    /* Once answered, the cache holds again. */
    assert(SetModulationKind(122, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 4);
    return 0;
}

/* Whether the commands sent since @p first are exactly @p want (NULL-terminated). */
static int
sent_since(size_t first, const char* const* want) {
    size_t i = 0;
    for (; want[i] != NULL; i++) {
        if (first + i >= g_command_count || strcmp(g_commands[first + i], want[i]) != 0) {
            return 0;
        }
    }
    return first + i == g_command_count;
}

/*
 * Issue #526: SDR++ and GQRX take a passband of 0 as "unchanged", and SDR++ keeps each passband it is sent, so an FM
 * undo at passband 0 would leave a row's own passband in force. Before a scan row first changes a demodulator's
 * passband the peer is asked for its own ("m"); a peer on the other demodulator is switched to this one at passband 0
 * and asked again. The FM undo sends the own FM passband back explicitly, and the scan's restore puts back the other
 * demodulator's before asking for what the session runs. The next scan reads them again.
 */
static int
test_scan_row_modulation_reads_and_restores_the_own_passband(void) {
    reset_stubs();
    assert(SetModulationKind(130, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 0);

    /* An nfm row's own 25 kHz on a peer at FM 12.5 kHz that refuses the NFM token, as SDR++ and GQRX do. */
    push_response("FM\n12500\n");
    push_response("RPRT 1\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(130, DSD_ANALOG_DEMOD_FM, 25000));
    static const char* const row_fm[] = {"m\n", "M NFM 25000\n", "M FM 25000\n", NULL};
    assert(sent_since(0, row_fm));
    /* A row without a width of its own: FM at the peer's own 12.5 kHz, sent as such, once. */
    push_response("RPRT 1\n");
    push_response("RPRT 0\n");
    assert(SetModulationKind(130, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const undo_fm[] = {"M NFM 12500\n", "M FM 12500\n", NULL};
    assert(sent_since(3, undo_fm));
    assert(SetModulationKind(130, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 5);

    /* An am row: the peer runs FM, so it is switched to AM at its own passband and asked for that (10 kHz, whose reply
       arrives in two reads), then for the row's 6 kHz. The FM passband read before stays the one to undo to. */
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("AM\n");
    push_response("10000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(130, DSD_ANALOG_DEMOD_AM, 6000));
    static const char* const row_am[] = {"m\n", "M AM 0\n", "m\n", "M AM 6000\n", NULL};
    assert(sent_since(5, row_am));
    assert(SetScanRowModulation(130, DSD_ANALOG_DEMOD_AM, 6000));
    assert(g_command_count == 9);

    /* The scan leaves on the am row: AM goes back to its own 10 kHz first, then FM to its own 12.5 kHz. */
    push_response("RPRT 0\n");
    push_response("RPRT 1\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(130, DSD_ANALOG_DEMOD_FM, 0, false));
    static const char* const restore[] = {"M AM 10000\n", "M NFM 12500\n", "M FM 12500\n", NULL};
    assert(sent_since(9, restore));
    assert(SetModulationKind(130, DSD_ANALOG_DEMOD_FM, 0));
    assert(RestoreScanModulation(130, DSD_ANALOG_DEMOD_FM, 0, false));
    assert(g_command_count == 12);

    /* The next scan reads the peer's own passbands again, whatever it runs now (a WFM broadcast here). */
    push_response("WFM\n200000\n");
    push_response("RPRT 1\n");
    push_response("RPRT 0\n");
    push_response("FM\n11000\n");
    push_response("RPRT 1\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(130, DSD_ANALOG_DEMOD_FM, 20000));
    static const char* const reread[] = {"m\n", "M NFM 0\n", "M FM 0\n", "m\n", "M NFM 20000\n", "M FM 20000\n", NULL};
    assert(sent_since(12, reread));
    push_response("RPRT 1\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(130, DSD_ANALOG_DEMOD_FM, 0, false));
    static const char* const restore_fm[] = {"M NFM 11000\n", "M FM 11000\n", NULL};
    assert(sent_since(18, restore_fm));

    /* A peer that cannot say what it runs: the row still gets its AM, the undo falls back to passband 0, and the AM
       passband it never read is not put back. */
    reset_stubs();
    push_response("RPRT -11\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(131, DSD_ANALOG_DEMOD_AM, 6000));
    push_response("RPRT 0\n");
    assert(SetModulationKind(131, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const unknown[] = {"m\n", "M AM 6000\n", "M NFM 0\n", NULL};
    assert(sent_since(0, unknown));
    assert(RestoreScanModulation(131, DSD_ANALOG_DEMOD_FM, 0, false));
    assert(g_command_count == 3);
    /* ...nor one that answers with a passband it cannot parse, or cuts the reply short. */
    reset_stubs();
    push_response("FM\nwide\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(132, DSD_ANALOG_DEMOD_FM, 25000));
    push_response("RPRT 0\n");
    assert(SetModulationKind(132, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 3 && strcmp(g_commands[2], "M NFM 0\n") == 0);
    reset_stubs();
    push_response("FM\n");
    assert(!SetScanRowModulation(133, DSD_ANALOG_DEMOD_FM, 25000));

    /* A refused row request leaves nothing to put back, and the session's own request goes out as asked. */
    reset_stubs();
    push_response("AM\n9000\n");
    push_response("RPRT -1\n");
    assert(!SetScanRowModulation(134, DSD_ANALOG_DEMOD_AM, 6000));
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(134, DSD_ANALOG_DEMOD_FM, 7000, false));
    static const char* const refused[] = {"m\n", "M AM 6000\n", "M NFM 7000\n", NULL};
    assert(sent_since(0, refused));

    /* A new connection on the number of a socket closed before it is a peer nothing was asked of. */
    reset_stubs();
    push_response("RPRT 0\n");
    assert(SetModulationKind(135, DSD_ANALOG_DEMOD_AM, 6000));
    char host[] = "127.0.0.1";
    g_create_result = 135;
    assert(Connect(host, 4532) == 135);
    assert(SetModulationKind(135, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 1);
    return 0;
}

/*
 * An undo the peer did not accept -- its reply lost here (an empty read), so the request may never have landed -- keeps
 * what was read of that demodulator's own passband: the peer may still run the row's passband, which a fresh read
 * would take for its own, and every later undo would then return the peer to the row's. The next undo sends the
 * passband read before the row changed it, and the next scan's row request asks for no read.
 */
static int
test_scan_restore_keeps_the_own_passband_an_undo_did_not_put_back(void) {
    /* The session's FM undo is lost: an nfm row's own 25 kHz on a peer at FM 12.5 kHz. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 1\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(140, DSD_ANALOG_DEMOD_FM, 25000));
    push_response("");
    assert(!RestoreScanModulation(140, DSD_ANALOG_DEMOD_FM, 0, false));
    static const char* const lost_fm[] = {"m\n", "M NFM 25000\n", "M FM 25000\n", "M NFM 12500\n", NULL};
    assert(sent_since(0, lost_fm));
    /* The next scan's first row without a width of its own sends the peer's own 12.5 kHz, not 0 ("unchanged"). */
    push_response("RPRT 1\n");
    push_response("RPRT 0\n");
    assert(SetModulationKind(140, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const undo_fm[] = {"M NFM 12500\n", "M FM 12500\n", NULL};
    assert(sent_since(4, undo_fm));
    /* A widened row reads nothing (the peer would report the row's 25 kHz), and its restore returns the peer to
       12.5 kHz. */
    push_response("RPRT 1\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(140, DSD_ANALOG_DEMOD_FM, 25000));
    push_response("RPRT 1\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(140, DSD_ANALOG_DEMOD_FM, 0, false));
    static const char* const again_fm[] = {"M NFM 25000\n", "M FM 25000\n", "M NFM 12500\n", "M FM 12500\n", NULL};
    assert(sent_since(6, again_fm));
    /* That restore was answered, so the scan after it reads the own passband again. */
    push_response("FM\n12500\n");
    push_response("RPRT 1\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(140, DSD_ANALOG_DEMOD_FM, 20000));
    assert(g_command_count == 13 && strcmp(g_commands[10], "m\n") == 0);

    /* The other demodulator's undo is lost: an am row's 6 kHz on a peer at AM 10 kHz. The session's FM request is
       answered, so only the AM passband stays to be put back, by the next restore; the am row of the scan between
       reads nothing (the peer would report its 6 kHz). */
    reset_stubs();
    push_response("AM\n10000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(141, DSD_ANALOG_DEMOD_AM, 6000));
    push_response("");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(141, DSD_ANALOG_DEMOD_FM, 0, false));
    static const char* const lost_am[] = {"m\n", "M AM 6000\n", "M AM 10000\n", "M NFM 0\n", NULL};
    assert(sent_since(0, lost_am));
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(141, DSD_ANALOG_DEMOD_AM, 6000));
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(141, DSD_ANALOG_DEMOD_FM, 0, false));
    static const char* const again_am[] = {"M AM 6000\n", "M AM 10000\n", "M NFM 0\n", NULL};
    assert(sent_since(4, again_am));
    assert(RestoreScanModulation(141, DSD_ANALOG_DEMOD_FM, 0, false));
    assert(g_command_count == 7);
    return 0;
}

/*
 * A peer's answer to "m" can arrive over several reads, its refusal included ("RPRT" and then " -11\n"). The whole
 * first line is read before it is taken for a refusal or a mode, so no part of it is left to be read as the reply to
 * the row's request, and every later reply stays with its own command.
 */
static int
test_scan_row_mode_query_reads_a_split_reply_whole(void) {
    /* A refusal split after "RPRT": the row's AM is still accepted, and the frequency after it too. */
    reset_stubs();
    push_response("RPRT");
    push_response(" -11\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(150, DSD_ANALOG_DEMOD_AM, 6000));
    push_response("RPRT 0\n");
    assert(SetFreq(150, 118300000L));
    static const char* const refused[] = {"m\n", "M AM 6000\n", "F 118300000\n", NULL};
    assert(sent_since(0, refused));
    /* ...its undo falls back to the peer's normal passband, as for any peer that cannot say what it runs. */
    push_response("RPRT 0\n");
    assert(SetModulationKind(150, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 4 && strcmp(g_commands[3], "M NFM 0\n") == 0);

    /* A refusal split inside its first word is a refusal as well. */
    reset_stubs();
    push_response("RP");
    push_response("RT -11\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(151, DSD_ANALOG_DEMOD_AM, 8333));
    static const char* const split_word[] = {"m\n", "M AM 8333\n", NULL};
    assert(sent_since(0, split_word));

    /* A mode split inside its first line is read whole: the peer's own FM 12.5 kHz is what the undo sends. */
    reset_stubs();
    push_response("F");
    push_response("M\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(152, DSD_ANALOG_DEMOD_FM, 25000));
    push_response("RPRT 0\n");
    assert(SetModulationKind(152, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const split_mode[] = {"m\n", "M NFM 25000\n", "M NFM 12500\n", NULL};
    assert(sent_since(0, split_mode));
    return 0;
}

/* What the peer runs as far as this client knows, read without I/O: FM on a socket nothing was asked of, AM once it
 * accepts an AM request, still AM after an FM request it refuses, and FM once it accepts one. A lost reply to a request
 * for the other demodulator leaves the peer on either, so it is not known until the peer accepts a request; a lost
 * reply to one for the demodulator it runs leaves that one known. */
static int
test_cached_modulation_kind_follows_what_the_peer_accepted(void) {
    reset_stubs();
    assert(CachedModulationKind(160) == DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(160, DSD_ANALOG_DEMOD_AM, 6000));
    assert(CachedModulationKind(160) == DSD_ANALOG_DEMOD_AM);
    assert(CachedModulationKind(161) == DSD_ANALOG_DEMOD_FM);
    push_response("RPRT -1\n");
    push_response("RPRT -1\n");
    assert(!SetModulationKind(160, DSD_ANALOG_DEMOD_FM, 0));
    assert(CachedModulationKind(160) == DSD_ANALOG_DEMOD_AM);
    assert(!SetModulationKind(160, DSD_ANALOG_DEMOD_FM, 0));
    assert(CachedModulationKind(160) == DSD_RIGCTL_KIND_UNKNOWN);
    push_response("RPRT 0\n");
    assert(SetModulationKind(160, DSD_ANALOG_DEMOD_FM, 0));
    assert(CachedModulationKind(160) == DSD_ANALOG_DEMOD_FM);
    static const char* const sent[] = {"M AM 6000\n", "M NFM 0\n", "M FM 0\n", "M NFM 0\n", "M NFM 0\n", NULL};
    assert(sent_since(0, sent));

    /* The FM peer's reply to a passband is lost: still FM. Its reply to AM is lost: either, until one is accepted. */
    assert(!SetModulationKind(160, DSD_ANALOG_DEMOD_FM, 12500));
    assert(CachedModulationKind(160) == DSD_ANALOG_DEMOD_FM);
    assert(!SetModulationKind(160, DSD_ANALOG_DEMOD_AM, 8333));
    assert(CachedModulationKind(160) == DSD_RIGCTL_KIND_UNKNOWN);
    push_response("RPRT 0\n");
    assert(SetModulationKind(160, DSD_ANALOG_DEMOD_AM, 8333));
    assert(CachedModulationKind(160) == DSD_ANALOG_DEMOD_AM);
    /* ...and an am row's own request whose reply is lost after the switch it made to read the AM passband. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("AM\n10000\n");
    assert(!SetScanRowModulation(162, DSD_ANALOG_DEMOD_AM, 6000));
    assert(CachedModulationKind(162) == DSD_ANALOG_DEMOD_AM);
    return 0;
}

/*
 * Issue #526: a scan tune that fails after its modulation request changed what the peer runs puts back what
 * CachedModulation() read before the request (RevertModulation()), so the row still on air is not heard through the
 * failed row's demodulator or passband: an am row's AM accepted before its frequency is refused, or the switch to AM
 * that read the peer's own AM passband before the am row's width was refused. Nothing is sent when nothing known
 * changed, and a peer whose demodulator was not known has nothing to go back to.
 */
static int
test_revert_modulation_puts_back_what_the_peer_ran(void) {
    /* An nfm row's own 12.5 kHz on air, then an am row's AM accepted and its frequency refused: back to FM 12.5 kHz. */
    reset_stubs();
    push_response("FM\n16000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(170, DSD_ANALOG_DEMOD_FM, 12500));
    const dsd_rigctl_modulation on_air = CachedModulation(170);
    assert(on_air.kind == DSD_ANALOG_DEMOD_FM && on_air.bandwidth == 12500);
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("AM\n10000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(170, DSD_ANALOG_DEMOD_AM, 6000));
    push_response("RPRT -1\n");
    assert(!SetFreq(170, 118300000L));
    push_response("RPRT 0\n");
    assert(RevertModulation(170, on_air));
    static const char* const freq_refused[] = {"m\n",         "M NFM 12500\n", "m\n",           "M AM 0\n", "m\n",
                                               "M AM 6000\n", "F 118300000\n", "M NFM 12500\n", NULL};
    assert(sent_since(0, freq_refused));
    assert(CachedModulationKind(170) == DSD_ANALOG_DEMOD_FM);
    /* Put back, nothing is sent again. */
    assert(RevertModulation(170, on_air));
    assert(g_command_count == 8);

    /* A peer nothing was asked of switched to AM to read its AM passband, then refusing the am row's width: back to FM
       at its own passband, read before the switch. */
    reset_stubs();
    const dsd_rigctl_modulation untouched = CachedModulation(171);
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("AM\n10000\n");
    push_response("RPRT -1\n");
    assert(!SetScanRowModulation(171, DSD_ANALOG_DEMOD_AM, 6000));
    assert(CachedModulationKind(171) == DSD_ANALOG_DEMOD_AM);
    push_response("RPRT 1\n");
    push_response("RPRT 0\n");
    assert(RevertModulation(171, untouched));
    static const char* const width_refused[] = {"m\n",           "M AM 0\n",     "m\n", "M AM 6000\n",
                                                "M NFM 12500\n", "M FM 12500\n", NULL};
    assert(sent_since(0, width_refused));
    assert(CachedModulationKind(171) == DSD_ANALOG_DEMOD_FM);

    /* -B taken by a peer nothing was asked of before: its FM stays, and no passband was known to go back to. */
    reset_stubs();
    const dsd_rigctl_modulation fresh = CachedModulation(172);
    push_response("RPRT 0\n");
    assert(SetModulationKind(172, DSD_ANALOG_DEMOD_FM, 7000));
    assert(RevertModulation(172, fresh));
    assert(g_command_count == 1);

    /* The first nfm row's own 25 kHz taken by a peer nothing was asked of, then its frequency refused: back to the
       peer's own FM 16 kHz, read before the row changed it, which the cache then holds as the peer's own. */
    reset_stubs();
    const dsd_rigctl_modulation first_row = CachedModulation(175);
    push_response("FM\n16000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(175, DSD_ANALOG_DEMOD_FM, 25000));
    push_response("RPRT -1\n");
    assert(!SetFreq(175, 154430000L));
    push_response("RPRT 0\n");
    assert(RevertModulation(175, first_row));
    static const char* const first_row_refused[] = {"m\n", "M NFM 25000\n", "F 154430000\n", "M NFM 16000\n", NULL};
    assert(sent_since(0, first_row_refused));
    const dsd_rigctl_modulation reverted = CachedModulation(175);
    assert(reverted.kind == DSD_ANALOG_DEMOD_FM && reverted.bandwidth == 0);
    /* Put back, nothing is sent again, and the next digital row's FM at the peer's own passband is already in force. */
    assert(RevertModulation(175, first_row));
    assert(SetModulationKind(175, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 4);

    /* The same row after a lost reply left the passband on air not known: back to the peer's own, read by the row. */
    reset_stubs();
    assert(!SetModulationKind(176, DSD_ANALOG_DEMOD_FM, 12500));
    const dsd_rigctl_modulation lost = CachedModulation(176);
    assert(lost.kind == DSD_ANALOG_DEMOD_FM && lost.bandwidth == INT_MIN);
    push_response("FM\n16000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(176, DSD_ANALOG_DEMOD_FM, 25000));
    push_response("RPRT 0\n");
    assert(RevertModulation(176, lost));
    static const char* const after_lost[] = {"M NFM 12500\n", "m\n", "M NFM 25000\n", "M NFM 16000\n", NULL};
    assert(sent_since(0, after_lost));

    /* ...but where the failed tune put the peer back on its own passband (a digital row's FM undo), nothing better is
       known to go back to, so nothing is sent. */
    reset_stubs();
    assert(!SetModulationKind(177, DSD_ANALOG_DEMOD_FM, 12500));
    const dsd_rigctl_modulation lost_undo = CachedModulation(177);
    push_response("RPRT 0\n");
    assert(SetModulationKind(177, DSD_ANALOG_DEMOD_FM, 0));
    assert(RevertModulation(177, lost_undo));
    static const char* const undo_kept[] = {"M NFM 12500\n", "M NFM 0\n", NULL};
    assert(sent_since(0, undo_kept));

    /* A peer that may run either demodulator: nothing to go back to. */
    reset_stubs();
    push_response("RPRT 0\n");
    assert(SetModulationKind(173, DSD_ANALOG_DEMOD_FM, 7000));
    assert(!SetModulationKind(173, DSD_ANALOG_DEMOD_AM, 6000));
    const dsd_rigctl_modulation either = CachedModulation(173);
    assert(either.kind == DSD_RIGCTL_KIND_UNKNOWN);
    push_response("RPRT 0\n");
    assert(SetModulationKind(173, DSD_ANALOG_DEMOD_AM, 6000));
    assert(!RevertModulation(173, either));
    assert(g_command_count == 3 && CachedModulationKind(173) == DSD_ANALOG_DEMOD_AM);

    /* A revert whose reply is lost leaves the peer on either demodulator. */
    reset_stubs();
    push_response("RPRT 0\n");
    assert(SetModulationKind(174, DSD_ANALOG_DEMOD_FM, 7000));
    const dsd_rigctl_modulation fm = CachedModulation(174);
    push_response("RPRT 0\n");
    assert(SetModulationKind(174, DSD_ANALOG_DEMOD_AM, 6000));
    assert(!RevertModulation(174, fm));
    assert(g_command_count == 3 && strcmp(g_commands[2], "M NFM 7000\n") == 0);
    assert(CachedModulationKind(174) == DSD_RIGCTL_KIND_UNKNOWN);
    return 0;
}

/* Issue #526: Connect() also opens the TCP audio input's socket, and reconnects it while the rigctl socket stays open.
 * A connection on another number leaves the rigctl socket's record alone: the peer an am row put on AM is still asked
 * for FM, and taken for one on AM, rather than for a peer nothing was asked of. */
static int
test_connect_on_another_socket_keeps_the_rigctl_record(void) {
    reset_stubs();
    push_response("RPRT 0\n");
    assert(SetModulationKind(136, DSD_ANALOG_DEMOD_AM, 6000));
    char host[] = "127.0.0.1";
    g_create_result = 137;
    assert(Connect(host, 7355) == 137);
    assert(CachedModulationKind(136) == DSD_ANALOG_DEMOD_AM);
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(136, DSD_ANALOG_DEMOD_FM, 0, false));
    static const char* const sent[] = {"M AM 6000\n", "M NFM 0\n", NULL};
    assert(sent_since(0, sent));
    assert(CachedModulationKind(136) == DSD_ANALOG_DEMOD_FM);
    return 0;
}

/*
 * Issue #589: a rigctl reconnect to the same host and port opens a new socket on another number while the old one is
 * still open, and hands it the old socket's record (RigctlRebindPeer()). A scan's am row left the peer on AM at 6 kHz:
 * the next row's FM undo and the scan's restore still send the peer's own passbands, read before the row changed them,
 * rather than taking the new socket for a peer nothing was asked of and sending nothing.
 */
static int
test_rebind_hands_the_scan_record_to_a_reconnect(void) {
    char host[] = "127.0.0.1";

    /* An am row on 190: the peer ran FM at 12.5 kHz and runs AM at 10 kHz of its own. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("AM\n10000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(190, DSD_ANALOG_DEMOD_AM, 6000));
    g_create_result = 191;
    assert(Connect(host, 4532) == 191);
    RigctlRebindPeer(190, 191, 1);
    assert(CachedModulationKind(191) == DSD_ANALOG_DEMOD_AM);
    /* The next digital row: FM at the peer's own 12.5 kHz. */
    push_response("RPRT 0\n");
    assert(SetModulationKind(191, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const undo[] = {"M NFM 12500\n", NULL};
    assert(sent_since(4, undo));
    /* The scan stops: AM goes back to its own 10 kHz, then FM to its own 12.5 kHz. */
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(191, DSD_ANALOG_DEMOD_FM, 0, false));
    static const char* const restore[] = {"M AM 10000\n", "M NFM 12500\n", NULL};
    assert(sent_since(5, restore));
    assert(CachedModulationKind(191) == DSD_ANALOG_DEMOD_FM);

    /* An nfm row's own 25 kHz on 192, and the scan stops right after the reconnect: the peer gets its own 12.5 kHz
       back rather than keeping the row's. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(192, DSD_ANALOG_DEMOD_FM, 25000));
    g_create_result = 193;
    assert(Connect(host, 4532) == 193);
    RigctlRebindPeer(192, 193, 1);
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(193, DSD_ANALOG_DEMOD_FM, 0, false));
    static const char* const restore_fm[] = {"M NFM 12500\n", NULL};
    assert(sent_since(2, restore_fm));
    const dsd_rigctl_modulation after = CachedModulation(193);
    assert(after.kind == DSD_ANALOG_DEMOD_FM && after.bandwidth == 0);

    /* The peer may have restarted, or been changed, while the connection was down (often why it is reconnected): a
       request for what it last accepted is sent all the same, and the own passbands read before are still what the
       restore sends, with no read taking the row's for the peer's own. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("AM\n10000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(208, DSD_ANALOG_DEMOD_AM, 6000));
    g_create_result = 209;
    assert(Connect(host, 4532) == 209);
    RigctlRebindPeer(208, 209, 1);
    const dsd_rigctl_modulation rebound = CachedModulation(209);
    assert(rebound.kind == DSD_ANALOG_DEMOD_AM && rebound.bandwidth == 6000);
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(209, DSD_ANALOG_DEMOD_AM, 6000));
    static const char* const again[] = {"M AM 6000\n", NULL};
    assert(sent_since(4, again));
    assert(SetScanRowModulation(209, DSD_ANALOG_DEMOD_AM, 6000));
    assert(g_command_count == 5);
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(209, DSD_ANALOG_DEMOD_FM, 0, false));
    static const char* const restore_both[] = {"M AM 10000\n", "M NFM 12500\n", NULL};
    assert(sent_since(5, restore_both));

    /* What the peer last accepted is still what a failed tune puts back: a tune right after the reconnect whose
       frequency the peer refuses returns the peer to the passband of the row still on air (its own 25 kHz), not to the
       peer's own from before the scan. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(215, DSD_ANALOG_DEMOD_FM, 25000));
    g_create_result = 216;
    assert(Connect(host, 4532) == 216);
    RigctlRebindPeer(215, 216, 1);
    const dsd_rigctl_modulation before = CachedModulation(216);
    assert(before.kind == DSD_ANALOG_DEMOD_FM && before.bandwidth == 25000);
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(216, DSD_ANALOG_DEMOD_FM, 20000));
    push_response("RPRT 0\n");
    assert(RevertModulation(216, before));
    static const char* const put_back[] = {"M NFM 20000\n", "M NFM 25000\n", NULL};
    assert(sent_since(2, put_back));
    return 0;
}

/*
 * Issue #589: a reconnect to another host or port may reach another peer, whose own passbands the old record does not
 * describe, or the same one under another name. While the old peer may run AM (an am row's, or either after a lost
 * reply), the new socket's demodulator is not known: FM is sent before anything else is taken for it, a refusal
 * fails (so a best-effort tune fails too, CachedModulationKind()), and the scan's restore sends FM. The old peer's own
 * passbands are never sent to the new one.
 */
static int
test_rebind_to_another_endpoint_asks_a_peer_that_may_run_am_for_fm(void) {
    char host[] = "10.0.0.2";

    /* The scan stops right after the reconnect: the restore asks for FM at the new peer's own passband (0). */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("AM\n10000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(194, DSD_ANALOG_DEMOD_AM, 6000));
    g_create_result = 195;
    assert(Connect(host, 4532) == 195);
    RigctlRebindPeer(194, 195, 0);
    assert(CachedModulationKind(195) == DSD_RIGCTL_KIND_UNKNOWN);
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(195, DSD_ANALOG_DEMOD_FM, 0, false));
    static const char* const restore[] = {"M NFM 0\n", NULL};
    assert(sent_since(4, restore));
    assert(CachedModulationKind(195) == DSD_ANALOG_DEMOD_FM);

    /* A refusal of the FM leaves the demodulator not known, and the next request asks again. */
    reset_stubs();
    push_response("RPRT 0\n");
    assert(SetModulationKind(196, DSD_ANALOG_DEMOD_AM, 6000));
    g_create_result = 197;
    assert(Connect(host, 4532) == 197);
    RigctlRebindPeer(196, 197, 0);
    push_response("RPRT -1\n");
    push_response("RPRT -1\n");
    assert(!SetModulationKind(197, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const refused[] = {"M NFM 0\n", "M FM 0\n", NULL};
    assert(sent_since(1, refused));
    assert(CachedModulationKind(197) == DSD_RIGCTL_KIND_UNKNOWN);
    push_response("RPRT 0\n");
    assert(SetModulationKind(197, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 4 && strcmp(g_commands[3], "M NFM 0\n") == 0);
    assert(CachedModulationKind(197) == DSD_ANALOG_DEMOD_FM);

    /* A lost reply to a request for AM leaves the old peer on either demodulator: the same holds. */
    reset_stubs();
    assert(!SetModulationKind(198, DSD_ANALOG_DEMOD_AM, 6000));
    assert(CachedModulationKind(198) == DSD_RIGCTL_KIND_UNKNOWN);
    g_create_result = 199;
    assert(Connect(host, 4532) == 199);
    RigctlRebindPeer(198, 199, 0);
    assert(CachedModulationKind(199) == DSD_RIGCTL_KIND_UNKNOWN);
    return 0;
}

/*
 * Issue #589: a reconnect to another host or port while the old peer runs FM (an nfm row's own passband, say) opens a
 * fresh record, as a first connection does: a peer nothing was asked of keeps its own settings, and one that refuses
 * mode requests (a rigctld radio) does not fail every best-effort tune. The old peer's own passband is not sent to it.
 * With no record of a change to the old socket's peer (nothing it accepted, or no old socket at all) nothing is handed
 * over either. Whatever is handed over, the record names the new socket afterwards: the old one is closed next, and a
 * connection that later gets its number back (the TCP audio input's, outside the P25 SM tick guard) must not reset the
 * record the watchdog's retunes use.
 */
static int
test_rebind_opens_a_fresh_record_where_no_am_is_left_behind(void) {
    char host[] = "10.0.0.2";

    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(200, DSD_ANALOG_DEMOD_FM, 25000));
    g_create_result = 201;
    assert(Connect(host, 4532) == 201);
    RigctlRebindPeer(200, 201, 0);
    assert(CachedModulationKind(201) == DSD_ANALOG_DEMOD_FM);
    assert(SetModulationKind(201, DSD_ANALOG_DEMOD_FM, 0));
    assert(RestoreScanModulation(201, DSD_ANALOG_DEMOD_FM, 0, false));
    assert(g_command_count == 2);
    push_response("RPRT -1\n");
    push_response("RPRT -1\n");
    assert(!SetModulationKind(201, DSD_ANALOG_DEMOD_FM, 12500));
    assert(CachedModulationKind(201) == DSD_ANALOG_DEMOD_FM);

    /* Nothing this client asked changed the old socket's peer (FM at its own passband is never sent to such a peer, and
       -B was refused): nothing is handed over, even for the same endpoint, and the new socket is a peer nothing was
       asked of, as on a first connection, which keeps its own settings. */
    reset_stubs();
    assert(SetModulationKind(211, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT -1\n");
    push_response("RPRT -1\n");
    assert(!SetModulationKind(211, DSD_ANALOG_DEMOD_FM, 12500));
    g_create_result = 212;
    assert(Connect(host, 4532) == 212);
    RigctlRebindPeer(211, 212, 1);
    assert(dsd_rigctl_test_record_socket() == 212);
    assert(SetModulationKind(212, DSD_ANALOG_DEMOD_FM, 0));
    assert(RestoreScanModulation(212, DSD_ANALOG_DEMOD_FM, 0, false));
    assert(g_command_count == 2);

    /* The record describes another socket than the one replaced: none of it is handed over. */
    reset_stubs();
    push_response("RPRT 0\n");
    assert(SetModulationKind(202, DSD_ANALOG_DEMOD_AM, 6000));
    RigctlRebindPeer(203, 204, 1);
    assert(dsd_rigctl_test_record_socket() == 204);
    assert(CachedModulationKind(204) == DSD_ANALOG_DEMOD_FM);
    assert(SetModulationKind(204, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 1);

    /* No old socket: a record left on DSD_INVALID_SOCKET (a request made on no connection) is not handed over. */
    reset_stubs();
    assert(!SetModulationKind(DSD_INVALID_SOCKET, DSD_ANALOG_DEMOD_AM, 6000));
    RigctlRebindPeer(DSD_INVALID_SOCKET, 205, 0);
    assert(dsd_rigctl_test_record_socket() == 205);
    assert(CachedModulationKind(205) == DSD_ANALOG_DEMOD_FM);
    assert(SetModulationKind(205, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 1);
    return 0;
}

/* Issue #589: once a reconnect closes the old rigctl socket, a later connection can get its number back. The
 * frequency last set on the closed socket says nothing of the new connection's peer, so the rebind forgets it and a
 * connection on a reused number is sent it again. A connection that replaces no rigctl socket (the TCP audio input's)
 * leaves it alone. */
static int
test_rebind_forgets_the_frequency_a_reused_number_would_match(void) {
    char host[] = "127.0.0.1";
    reset_stubs();
    push_response("RPRT 0\n");
    assert(SetFreq(206, 851012500L));
    g_create_result = 207;
    assert(Connect(host, 7355) == 207);
    assert(SetFreq(206, 851012500L));
    assert(g_command_count == 1);
    /* Two rigctl reconnects: 206 is replaced by 210 and closed, then 210 by a connection that gets 206 back. */
    g_create_result = 210;
    assert(Connect(host, 4532) == 210);
    RigctlRebindPeer(206, 210, 1);
    g_create_result = 206;
    assert(Connect(host, 4532) == 206);
    RigctlRebindPeer(210, 206, 1);
    push_response("RPRT 0\n");
    assert(SetFreq(206, 851012500L));
    assert(g_command_count == 2 && strcmp(g_commands[1], "F 851012500\n") == 0);
    return 0;
}

/* A reply that fills the whole receive buffer (1024 bytes) is terminated inside every caller's buffer: the frequency
 * query, the frequency set and the modulation set each read one. */
static int
test_full_size_replies_stay_in_bounds(void) {
    static char freq_reply[1100];
    static char ok_reply[1100];
    DSD_MEMSET(freq_reply, 'x', sizeof freq_reply - 1U);
    DSD_MEMCPY(freq_reply, "851037500\n", 10U);
    freq_reply[sizeof freq_reply - 1U] = '\0';
    DSD_MEMSET(ok_reply, ' ', sizeof ok_reply - 1U);
    DSD_MEMCPY(ok_reply, "RPRT 0\n", 7U);
    ok_reply[sizeof ok_reply - 1U] = '\0';

    reset_stubs();
    push_response(freq_reply);
    assert(GetCurrentFreq(123) == 851037500L);
    push_response(ok_reply);
    assert(SetFreq(123, 851050000L));
    push_response(ok_reply);
    assert(SetModulationKind(123, DSD_ANALOG_DEMOD_AM, 8333));
    assert(g_command_count == 3);
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

    /* A reading strtol() cannot hold is unknown (0), not the LONG_MAX it clamps to, which where long is 32-bit
     * (Windows) is itself a plausible 2147483647 Hz; a negative reading is no frequency either. */
    reset_stubs();
    push_response("99999999999999999999\n");
    assert(GetCurrentFreq(102) == 0L);
    reset_stubs();
    push_response("-851037500\n");
    assert(GetCurrentFreq(102) == 0L);
#if LONG_MAX < 4294967295LL
    /* One past a 32-bit long already overflows there. */
    reset_stubs();
    push_response("2147483648\n");
    assert(GetCurrentFreq(102) == 0L);
#endif
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
    assert(io_control_set_freq(&opts, &state, 851000000L) == -1);
    assert(g_command_count == 0);

    opts.use_rigctl = 1;
    opts.rigctl_sockfd = 103;
    opts.setmod_bw = 12500;
    /* A digital session on PCM audio input (the zeroed input type is Pulse): -B is the FM passband the peer
       demodulates the input through, so it is captured like any passband this client sets (issue #621): the peer's
       own passband is read first, then -B, then the frequency. */
    push_response("FM\n16000\n");
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");

    assert(io_control_set_freq(&opts, &state, 851050000L) == 0);
    assert(opts.rtlsdr_center_freq == 851050000U);
    assert(g_command_count == 3);
    assert(strcmp(g_commands[0], "m\n") == 0);
    assert(strcmp(g_commands[1], "M NFM 12500\n") == 0);
    assert(strcmp(g_commands[2], "F 851050000\n") == 0);

    /* The peer runs -B already: only the frequency, whose refusal fails the tune. */
    reset_stubs();
    opts.rtlsdr_center_freq = 851050000U;
    push_response("RPRT 1\n");
    assert(io_control_set_freq(&opts, &state, 851062500L) == -1);
    assert(g_command_count == 1 && strcmp(g_commands[0], "F 851062500\n") == 0);
    assert(opts.rtlsdr_center_freq == 851050000U);

    reset_stubs();
    opts.rigctl_sockfd = DSD_INVALID_SOCKET;
    assert(io_control_set_freq(&opts, &state, 851075000L) == -1);
    assert(g_command_count == 0);
    return 0;
}

/* A session on the FM monitor of audio input with a rigctl peer on @p sockfd, nothing configured. */
static void
fm_monitor_session(dsd_opts* opts, dsd_socket_t sockfd) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->audio_in_type = AUDIO_IN_TCP;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = sockfd;
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
}

/*
 * Issue #621: a manual tune on the FM monitor the peer demodulates asks for the passband that monitor runs, not for a
 * bare -B: the configured NFM width, read first so a later return to the peer's own sends the passband read, strictly
 * (a refusal fails the tune before the frequency moves). -B standing in for an unset width goes through the same
 * capturing call, and a refusal fails the tune as -B always has here. Without either nothing is asked; a digital
 * session still sends -B as before (test_io_control_set_freq_validation_and_rigctl_dispatch()).
 */
static int
test_io_control_set_freq_asks_the_fm_monitor_passband(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    /* The configured width, its own passband (16 kHz) read first; then the frequency. */
    reset_stubs();
    fm_monitor_session(&opts, 220);
    opts.analog_nfm_bandwidth_hz = 12500;
    opts.setmod_bw = 7000;
    push_response("FM\n16000\n");
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155475000L) == 0);
    static const char* const configured[] = {"m\n", "M NFM 12500\n", "F 155475000\n", NULL};
    assert(sent_since(0, configured));
    /* Cleared with no -B: the peer's own 16 kHz, never 0 ("unchanged" to SDR++ and GQRX). */
    push_response("RPRT 0\n");
    assert(SetModulationKind(220, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 4 && strcmp(g_commands[3], "M NFM 16000\n") == 0);

    /* A refused width fails the tune before the frequency moves. */
    reset_stubs();
    fm_monitor_session(&opts, 221);
    opts.analog_nfm_bandwidth_hz = 12500;
    opts.rtlsdr_center_freq = 155000000U;
    push_response("FM\n16000\n");
    push_response("RPRT -1\n");
    push_response("RPRT -1\n");
    assert(io_control_set_freq(&opts, &state, 155500000L) == -1);
    static const char* const refused[] = {"m\n", "M NFM 12500\n", "M FM 12500\n", NULL};
    assert(sent_since(0, refused));
    assert(opts.rtlsdr_center_freq == 155000000U);

    /* -B 20000 standing in, through the capturing call: cleared to the peer's own, the passband read goes back. */
    reset_stubs();
    fm_monitor_session(&opts, 222);
    opts.setmod_bw = 20000;
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155525000L) == 0);
    static const char* const setmod[] = {"m\n", "M NFM 20000\n", "F 155525000\n", NULL};
    assert(sent_since(0, setmod));
    push_response("RPRT 0\n");
    assert(SetModulationKind(222, DSD_ANALOG_DEMOD_FM, 0));
    assert(g_command_count == 4 && strcmp(g_commands[3], "M NFM 12500\n") == 0);
    /* ...and a refused -B fails the tune, as it always has on this path. */
    reset_stubs();
    opts.setmod_bw = 25000;
    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    assert(io_control_set_freq(&opts, &state, 155550000L) == -1);
    static const char* const setmod_refused[] = {"M NFM 25000\n", "M FM 25000\n", NULL};
    assert(sent_since(0, setmod_refused));

    /* A peer that cannot answer "m" still takes the passband; its return to its own falls back to 0. */
    reset_stubs();
    fm_monitor_session(&opts, 223);
    opts.setmod_bw = 20000;
    push_response("RPRT -11\n");
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155575000L) == 0);
    push_response("RPRT 0\n");
    assert(SetModulationKind(223, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const unknown[] = {"m\n", "M NFM 20000\n", "F 155575000\n", "M NFM 0\n", NULL};
    assert(sent_since(0, unknown));

    /* Neither a width nor -B: the peer keeps its own passband; only the frequency goes out. */
    reset_stubs();
    fm_monitor_session(&opts, 224);
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155600000L) == 0);
    static const char* const own[] = {"F 155600000\n", NULL};
    assert(sent_since(0, own));

    /* The AM monitor asks for its width, the 6 kHz default when none is set. */
    reset_stubs();
    fm_monitor_session(&opts, 225);
    opts.analog_demod = DSD_ANALOG_DEMOD_AM;
    opts.setmod_bw = 12500;
    push_response("AM\n10000\n");
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 118300000L) == 0);
    static const char* const am[] = {"m\n", "M AM 6000\n", "F 118300000\n", NULL};
    assert(sent_since(0, am));
    return 0;
}

/*
 * Issue #621: once a scan leaves, the session's request comes last; where it is a passband this client sets on the
 * monitor the peer demodulates (session_passband: a width, or -B standing in), it goes through the capturing call and
 * that demodulator's record is kept, so a later return to the peer's own sends the passband read before the scan, not
 * 0. The other demodulator's passband still goes back first and its record is reset, as before. Without the flag the
 * session's demodulator is reset too, as it always was.
 */
static int
test_scan_restore_keeps_the_record_of_a_session_passband(void) {
    /* An nfm row's own 25 kHz on a peer at FM 12.5 kHz; the session asks for its configured 20 kHz. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(230, DSD_ANALOG_DEMOD_FM, 25000));
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(230, DSD_ANALOG_DEMOD_FM, 20000, true));
    /* The configured width cleared: the peer's own 12.5 kHz read before the scan. */
    push_response("RPRT 0\n");
    assert(SetModulationKind(230, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const kept[] = {"m\n", "M NFM 25000\n", "M NFM 20000\n", "M NFM 12500\n", NULL};
    assert(sent_since(0, kept));

    /* The same without the flag: the record goes, and the clear can only send 0. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(231, DSD_ANALOG_DEMOD_FM, 25000));
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(231, DSD_ANALOG_DEMOD_FM, 20000, false));
    push_response("RPRT 0\n");
    assert(SetModulationKind(231, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const reset[] = {"m\n", "M NFM 25000\n", "M NFM 20000\n", "M NFM 0\n", NULL};
    assert(sent_since(0, reset));

    /* A scan whose rows never changed FM (an am row's 6 kHz on a peer at FM 16 kHz, which reads both): AM goes back to
       its own first, then the session's 20 kHz, and a later clear sends the FM passband read. */
    reset_stubs();
    push_response("FM\n16000\n");
    push_response("RPRT 0\n");
    push_response("AM\n10000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(232, DSD_ANALOG_DEMOD_AM, 6000));
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(232, DSD_ANALOG_DEMOD_FM, 20000, true));
    push_response("RPRT 0\n");
    assert(SetModulationKind(232, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const am_scan[] = {"m\n",          "M AM 0\n",      "m\n",           "M AM 6000\n",
                                          "M AM 10000\n", "M NFM 20000\n", "M NFM 16000\n", NULL};
    assert(sent_since(0, am_scan));
    /* The AM record was reset by its restore: the next am row reads the peer's own AM passband again. */
    reset_stubs();
    push_response("FM\n16000\n");
    push_response("RPRT 0\n");
    push_response("AM\n9000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(232, DSD_ANALOG_DEMOD_AM, 6000));
    static const char* const am_again[] = {"m\n", "M AM 0\n", "m\n", "M AM 6000\n", NULL};
    assert(sent_since(0, am_again));

    /* A session that never ran a scan row: the restore's capturing call reads the peer's own passband first. */
    reset_stubs();
    push_response("FM\n11000\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(233, DSD_ANALOG_DEMOD_FM, 20000, true));
    push_response("RPRT 0\n");
    assert(SetModulationKind(233, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const fresh[] = {"m\n", "M NFM 20000\n", "M NFM 11000\n", NULL};
    assert(sent_since(0, fresh));

    /* A peer that cannot answer "m" still gets 0 on the clear. */
    reset_stubs();
    push_response("RPRT -11\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(234, DSD_ANALOG_DEMOD_FM, 20000, true));
    push_response("RPRT 0\n");
    assert(SetModulationKind(234, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const unread[] = {"m\n", "M NFM 20000\n", "M NFM 0\n", NULL};
    assert(sent_since(0, unread));
    return 0;
}

/*
 * Issue #621: a scan's restore keeps the record of the session's demodulator when the session sets a passband there
 * (session_passband), so the session's later return to the peer's own sends the passband read. Once the peer accepts
 * that return it runs its own passband again, which the operator may then change, so the record is spent as the restore
 * would have reset it: the next change reads it again ("m"). Only that record is: within a scan (records of the scan's
 * own rows) a reading serves the whole scan, as issue #526 has it, whatever the peer took back. A row change the peer
 * answered, or a read, after the restore makes the record the scan's; a reconnect to the same endpoint hands it over as
 * it is, another endpoint opens a fresh record. A restore the peer refused, or whose reply was lost, keeps the record
 * for the session all the same. A return the peer did not take, or one sent as 0 because the peer could not say what
 * it ran, spends nothing.
 */
static int
test_return_to_the_own_passband_after_a_session_restore(void) {
    /* A session passband kept across a scan's leave, then cleared; the operator sets 9 kHz on the peer meanwhile. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(240, DSD_ANALOG_DEMOD_FM, 20000, true));
    push_response("RPRT 0\n");
    assert(SetModulationKind(240, DSD_ANALOG_DEMOD_FM, 0));
    push_response("FM\n9000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(240, DSD_ANALOG_DEMOD_FM, 25000));
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(240, DSD_ANALOG_DEMOD_FM, 0, false));
    static const char* const session[] = {
        "m\n", "M NFM 20000\n", "M NFM 12500\n", "m\n", "M NFM 25000\n", "M NFM 9000\n", NULL};
    assert(sent_since(0, session));

    /* Within a scan nothing is spent: an nfm row's own 25 kHz, a row without one back on the peer's own 12.5 kHz, then
       another row's own 20 kHz, which reads nothing, and the undo after it sends the scan's reading. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(241, DSD_ANALOG_DEMOD_FM, 25000));
    push_response("RPRT 0\n");
    assert(SetModulationKind(241, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(241, DSD_ANALOG_DEMOD_FM, 20000));
    push_response("RPRT 0\n");
    assert(SetModulationKind(241, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const scan[] = {"m\n", "M NFM 25000\n", "M NFM 12500\n", "M NFM 20000\n", "M NFM 12500\n", NULL};
    assert(sent_since(0, scan));

    /* ...nor by a failed tune's revert to the peer's own passband. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(242, DSD_ANALOG_DEMOD_FM, 25000));
    push_response("RPRT 0\n");
    assert(SetModulationKind(242, DSD_ANALOG_DEMOD_FM, 0));
    const dsd_rigctl_modulation own = CachedModulation(242);
    assert(own.kind == DSD_ANALOG_DEMOD_FM && own.bandwidth == 0);
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(242, DSD_ANALOG_DEMOD_FM, 20000));
    push_response("RPRT 0\n");
    assert(RevertModulation(242, own));
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(242, DSD_ANALOG_DEMOD_FM, 16000));
    static const char* const revert[] = {
        "m\n", "M NFM 25000\n", "M NFM 12500\n", "M NFM 20000\n", "M NFM 12500\n", "M NFM 16000\n", NULL};
    assert(sent_since(0, revert));

    /* A session-kept reading the next scan's row used is that scan's: the scan's return spends nothing. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(243, DSD_ANALOG_DEMOD_FM, 20000, true));
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(243, DSD_ANALOG_DEMOD_FM, 25000));
    push_response("RPRT 0\n");
    assert(SetModulationKind(243, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(243, DSD_ANALOG_DEMOD_FM, 22000));
    static const char* const next_scan[] = {"m\n",           "M NFM 20000\n", "M NFM 25000\n",
                                            "M NFM 12500\n", "M NFM 22000\n", NULL};
    assert(sent_since(0, next_scan));

    /* A return the peer refused spends nothing: the next change reads nothing, and the next return sends the reading,
       which, accepted, spends it. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(244, DSD_ANALOG_DEMOD_FM, 20000, true));
    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    assert(!SetModulationKind(244, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 0\n");
    assert(SetModulationKind(244, DSD_ANALOG_DEMOD_FM, 0));
    push_response("FM\n9000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(244, DSD_ANALOG_DEMOD_FM, 25000));
    static const char* const refused[] = {"m\n",           "M NFM 20000\n", "M NFM 12500\n", "M FM 12500\n",
                                          "M NFM 12500\n", "m\n",           "M NFM 25000\n", NULL};
    assert(sent_since(0, refused));

    /* A peer that could not say what it ran gets 0, which may leave it on the session's passband: nothing is read
       again (a read would take that passband for the peer's own). */
    reset_stubs();
    push_response("RPRT -11\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(245, DSD_ANALOG_DEMOD_FM, 20000, true));
    push_response("RPRT 0\n");
    assert(SetModulationKind(245, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(245, DSD_ANALOG_DEMOD_FM, 25000));
    static const char* const unread[] = {"m\n", "M NFM 20000\n", "M NFM 0\n", "M NFM 25000\n", NULL};
    assert(sent_since(0, unread));

    /* A reconnect to the same peer hands the record over, mark included: a return the peer accepts there is its own
       passband as read, so it spends the reading as before the reconnect, and the next change reads it again. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(246, DSD_ANALOG_DEMOD_FM, 20000, true));
    char host[] = "127.0.0.1";
    g_create_result = 247;
    assert(Connect(host, 4532) == 247);
    RigctlRebindPeer(246, 247, 1);
    push_response("RPRT 0\n");
    assert(SetModulationKind(247, DSD_ANALOG_DEMOD_FM, 0));
    push_response("FM\n9000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(247, DSD_ANALOG_DEMOD_FM, 25000));
    static const char* const rebound[] = {"m\n", "M NFM 20000\n", "M NFM 12500\n", "m\n", "M NFM 25000\n", NULL};
    assert(sent_since(0, rebound));
    /* Another endpoint is another record: nothing of the old one, its mark included, is handed over. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(248, DSD_ANALOG_DEMOD_FM, 20000, true));
    g_create_result = 249;
    assert(Connect(host, 4532) == 249);
    RigctlRebindPeer(248, 249, 0);
    push_response("FM\n20000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(249, DSD_ANALOG_DEMOD_FM, 25000));
    static const char* const elsewhere[] = {"m\n", "M NFM 20000\n", "m\n", "M NFM 25000\n", NULL};
    assert(sent_since(0, elsewhere));

    /* A session-passband restore the peer refused, or whose reply was lost, keeps the record for the session all the
       same: the session's next return, once accepted, spends it, so the width set after the operator's 9 kHz reads
       that, and the clear after it sends it back. */
    static const char* const answers[] = {"RPRT 1\n", ""};
    for (size_t i = 0; i < sizeof answers / sizeof answers[0]; i++) {
        const dsd_socket_t sock = (dsd_socket_t)(260 + i);
        reset_stubs();
        push_response("FM\n12500\n");
        push_response("RPRT 0\n");
        assert(SetScanRowModulation(sock, DSD_ANALOG_DEMOD_FM, 25000));
        push_response(answers[i]);
        if (i == 0) {
            push_response("RPRT 1\n");
        }
        assert(!RestoreScanModulation(sock, DSD_ANALOG_DEMOD_FM, 20000, true));
        const size_t after_restore = g_command_count;
        push_response("RPRT 0\n");
        assert(SetModulationKind(sock, DSD_ANALOG_DEMOD_FM, 0));
        push_response("FM\n9000\n");
        push_response("RPRT 0\n");
        assert(SetScanRowModulation(sock, DSD_ANALOG_DEMOD_FM, 16000));
        RigctlMarkSessionPassband(sock, DSD_ANALOG_DEMOD_FM);
        push_response("RPRT 0\n");
        assert(SetModulationKind(sock, DSD_ANALOG_DEMOD_FM, 0));
        static const char* const after[] = {"M NFM 12500\n", "m\n", "M NFM 16000\n", "M NFM 9000\n", NULL};
        assert(sent_since(after_restore, after));
    }
    return 0;
}

/*
 * Issue #621: a passband the session asks for off any scan (the start ask, a live edit, a manual tune, the legacy -Y
 * step) has no scan restore to reset the reading of the peer's own taken for it, so the request says it is the
 * session's (RigctlMarkSessionPassband()). Once the peer takes its own passband back, the reading is spent, and the
 * next change reads it again: a width set, cleared, set and cleared again on a plain -fA session sends the passband the
 * operator gave the peer in between, not the reading from before. A scan's requests are not marked, and their reading
 * lasts the scan (issue #526).
 */
static int
test_session_passband_off_a_scan_reads_the_own_passband_again(void) {
    /* The session's requests as the engine makes them: the capturing call, then the mark. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(250, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(250, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(250, DSD_ANALOG_DEMOD_FM, 0));
    /* The operator sets 9 kHz on the peer, then a width is set and cleared again. */
    push_response("FM\n9000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(250, DSD_ANALOG_DEMOD_FM, 16000));
    RigctlMarkSessionPassband(250, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(250, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const session[] = {
        "m\n", "M NFM 20000\n", "M NFM 12500\n", "m\n", "M NFM 16000\n", "M NFM 9000\n", NULL};
    assert(sent_since(0, session));
    /* A mark for a socket the record does not describe changes nothing, nor opens a record for it. */
    RigctlMarkSessionPassband(251, DSD_ANALOG_DEMOD_FM);
    assert(dsd_rigctl_test_record_socket() == 250);

    /* A manual tune off any scan is the session's request: it asks for the state it was given. */
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stubs();
    fm_monitor_session(&opts, 252);
    g_session_request = 1;
    opts.analog_nfm_bandwidth_hz = 20000;
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155475000L) == 0);
    assert(g_session_request_state == &state);
    push_response("RPRT 0\n");
    assert(SetModulationKind(252, DSD_ANALOG_DEMOD_FM, 0));
    opts.analog_nfm_bandwidth_hz = 16000;
    push_response("FM\n9000\n");
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155500000L) == 0);
    push_response("RPRT 0\n");
    assert(SetModulationKind(252, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const manual[] = {"m\n",           "M NFM 20000\n", "F 155475000\n",
                                         "M NFM 12500\n", "m\n",           "M NFM 16000\n",
                                         "F 155500000\n", "M NFM 9000\n",  NULL};
    assert(sent_since(0, manual));

    /* The same during a scan: the scan's reading lasts the scan, and the second clear sends it. */
    reset_stubs();
    fm_monitor_session(&opts, 253);
    g_session_request = 0;
    opts.analog_nfm_bandwidth_hz = 20000;
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155475000L) == 0);
    push_response("RPRT 0\n");
    assert(SetModulationKind(253, DSD_ANALOG_DEMOD_FM, 0));
    opts.analog_nfm_bandwidth_hz = 16000;
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155500000L) == 0);
    push_response("RPRT 0\n");
    assert(SetModulationKind(253, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const scan[] = {"m\n",           "M NFM 20000\n", "F 155475000\n", "M NFM 12500\n",
                                       "M NFM 16000\n", "F 155500000\n", "M NFM 12500\n", NULL};
    assert(sent_since(0, scan));
    return 0;
}

/*
 * Issue #621: a manual tune asks for the passband the session runs even where that is the peer's own (FOLLOW with -B 0,
 * or the FM monitor with neither a width nor -B): a passband this client set earlier (a session width on the monitor,
 * or one a refused live return left in force) would otherwise stay on the peer across every later manual tune. FM at
 * the peer's own is sent as the passband read before this client changed it, and is a no-op for a peer nothing was
 * asked of or one already back on its own. Best-effort: a refusal never fails a tune that did not fail before.
 */
static int
test_io_control_set_freq_returns_to_the_own_passband(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    /* A session width on the FM monitor, then a digital mode with -B 0: the peer's own 12.5 kHz goes back first. */
    reset_stubs();
    fm_monitor_session(&opts, 270);
    g_session_request = 1;
    opts.analog_nfm_bandwidth_hz = 20000;
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155475000L) == 0);
    opts.analog_only = 0;
    opts.monitor_input_audio = 0;
    opts.frame_dmr = 1;
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 461000000L) == 0);
    /* Already back on its own: only the frequency. */
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 461012500L) == 0);
    static const char* const digital[] = {
        "m\n", "M NFM 20000\n", "F 155475000\n", "M NFM 12500\n", "F 461000000\n", "F 461012500\n", NULL};
    assert(sent_since(0, digital));

    /* A peer nothing was asked of keeps its own settings: only the frequency. */
    reset_stubs();
    fm_monitor_session(&opts, 271);
    opts.analog_only = 0;
    opts.monitor_input_audio = 0;
    opts.frame_dmr = 1;
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 461025000L) == 0);
    static const char* const untouched[] = {"F 461025000\n", NULL};
    assert(sent_since(0, untouched));

    /* The FM monitor with neither a width nor -B, after a refused live return left the session width on the peer: the
       peer's own goes back; refused again, the tune still lands. */
    reset_stubs();
    fm_monitor_session(&opts, 272);
    g_session_request = 1;
    opts.analog_nfm_bandwidth_hz = 20000;
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155475000L) == 0);
    opts.analog_nfm_bandwidth_hz = 0;
    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    assert(!SetModulationKind(272, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    push_response("RPRT 0\n");
    opts.rtlsdr_center_freq = 0U;
    assert(io_control_set_freq(&opts, &state, 155500000L) == 0);
    assert(opts.rtlsdr_center_freq == 155500000U);
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155525000L) == 0);
    static const char* const peer_own[] = {"m\n",
                                           "M NFM 20000\n",
                                           "F 155475000\n",
                                           "M NFM 12500\n",
                                           "M FM 12500\n",
                                           "M NFM 12500\n",
                                           "M FM 12500\n",
                                           "F 155500000\n",
                                           "M NFM 12500\n",
                                           "F 155525000\n",
                                           NULL};
    assert(sent_since(0, peer_own));
    return 0;
}

/*
 * Issue #621: a reading of the peer's own passband taken for a session request the peer refused is retired once the
 * peer is known to still run its own passband (nothing this client asked was taken, or the cache already says so): the
 * peer keeps that passband, which the operator may change before the next request, so the next request reads it again.
 * A refusal while the peer runs a passband this client set keeps the reading, which a return still needs. A scan row's
 * reading is not affected (issue #526): it lasts the scan.
 */
static int
test_a_refused_session_capture_is_retired(void) {
    /* The first session width is refused; the rollback to the peer's own needs no request. The operator sets 9 kHz. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    assert(!SetScanRowModulation(280, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(280, DSD_ANALOG_DEMOD_FM);
    assert(SetModulationKind(280, DSD_ANALOG_DEMOD_FM, 0));
    push_response("FM\n9000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(280, DSD_ANALOG_DEMOD_FM, 16000));
    RigctlMarkSessionPassband(280, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(280, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const retired[] = {
        "m\n", "M NFM 20000\n", "M FM 20000\n", "m\n", "M NFM 16000\n", "M NFM 9000\n", NULL};
    assert(sent_since(0, retired));

    /* The same with no rollback after it (a start ask the peer refused): the reading is retired when the refusal is
       marked, so the next width reads the operator's 9 kHz. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    assert(!SetScanRowModulation(284, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(284, DSD_ANALOG_DEMOD_FM);
    push_response("FM\n9000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(284, DSD_ANALOG_DEMOD_FM, 16000));
    RigctlMarkSessionPassband(284, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(284, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const no_rollback[] = {
        "m\n", "M NFM 20000\n", "M FM 20000\n", "m\n", "M NFM 16000\n", "M NFM 9000\n", NULL};
    assert(sent_since(0, no_rollback));

    /* A refused edit while the peer runs the session's 20 kHz keeps the reading: the clear sends 12.5 kHz. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(281, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(281, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    assert(!SetScanRowModulation(281, DSD_ANALOG_DEMOD_FM, 16000));
    RigctlMarkSessionPassband(281, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(281, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const kept[] = {"m\n", "M NFM 20000\n", "M NFM 16000\n", "M FM 16000\n", "M NFM 12500\n", NULL};
    assert(sent_since(0, kept));

    /* A rollback that finds the peer already on its own passband retires the reading too: a scan's restore asked for
       the session's 20 kHz after an undo had put the peer back on its own, and the peer refused it. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(283, DSD_ANALOG_DEMOD_FM, 25000));
    push_response("RPRT 0\n");
    assert(SetModulationKind(283, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    assert(!RestoreScanModulation(283, DSD_ANALOG_DEMOD_FM, 20000, true));
    assert(SetModulationKind(283, DSD_ANALOG_DEMOD_FM, 0));
    push_response("FM\n9000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(283, DSD_ANALOG_DEMOD_FM, 16000));
    RigctlMarkSessionPassband(283, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(283, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const rollback[] = {"m\n",           "M NFM 25000\n", "M NFM 12500\n",
                                           "M NFM 20000\n", "M FM 20000\n",  "m\n",
                                           "M NFM 16000\n", "M NFM 9000\n",  NULL};
    assert(sent_since(0, rollback));

    /* A scan row's refused request keeps its reading for the scan: the next row reads nothing. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    assert(!SetScanRowModulation(282, DSD_ANALOG_DEMOD_FM, 20000));
    assert(SetModulationKind(282, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(282, DSD_ANALOG_DEMOD_FM, 16000));
    static const char* const scan[] = {"m\n", "M NFM 20000\n", "M FM 20000\n", "M NFM 16000\n", NULL};
    assert(sent_since(0, scan));
    return 0;
}

/*
 * Issue #621: once a session return spent a reading, the next change reads the peer's own passband afresh; when that
 * read fails (refused, or its reply lost) the peer's own passband is not known, so a later return falls back to 0
 * (Hamlib's normal passband) rather than send the spent reading as if it were still the peer's own.
 */
static int
test_a_failed_fresh_read_forgets_a_spent_reading(void) {
    static const char* const answers[] = {"RPRT -11\n", ""};
    for (size_t i = 0; i < sizeof answers / sizeof answers[0]; i++) {
        const dsd_socket_t sock = (dsd_socket_t)(290 + i);
        reset_stubs();
        push_response("FM\n12500\n");
        push_response("RPRT 0\n");
        assert(SetScanRowModulation(sock, DSD_ANALOG_DEMOD_FM, 20000));
        RigctlMarkSessionPassband(sock, DSD_ANALOG_DEMOD_FM);
        push_response("RPRT 0\n");
        assert(SetModulationKind(sock, DSD_ANALOG_DEMOD_FM, 0));
        push_response(answers[i]);
        push_response("RPRT 0\n");
        assert(SetScanRowModulation(sock, DSD_ANALOG_DEMOD_FM, 16000));
        RigctlMarkSessionPassband(sock, DSD_ANALOG_DEMOD_FM);
        push_response("RPRT 0\n");
        assert(SetModulationKind(sock, DSD_ANALOG_DEMOD_FM, 0));
        static const char* const sent[] = {"m\n", "M NFM 20000\n", "M NFM 12500\n", "m\n", "M NFM 16000\n", "M NFM 0\n",
                                           NULL};
        assert(sent_since(0, sent));
    }
    return 0;
}

/*
 * Issue #621: the peer is known to run its own passband only where this client sent it that passband as read. A return
 * sent as 0 because no reading was left (a fresh read that failed) may leave the peer on this client's width, since
 * SDR++ and GQRX keep the passband they had: it spends nothing, and neither does a later FM at 0 the cache takes for a
 * repeat of it (a manual tune or a legacy step asks one at each tune), nor a refused session request marked there. A
 * spend there would have the next width read this client's earlier width back as the peer's own.
 */
static int
test_a_return_sent_as_0_is_not_the_peers_own(void) {
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(300, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(300, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(300, DSD_ANALOG_DEMOD_FM, 0));
    /* The fresh read fails, so the clear after the next width sends 0 (the peer keeps 16 kHz). */
    push_response("RPRT -11\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(300, DSD_ANALOG_DEMOD_FM, 16000));
    RigctlMarkSessionPassband(300, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(300, DSD_ANALOG_DEMOD_FM, 0));
    /* The next FM at 0 (a manual tune, a legacy step) is a repeat: no request, and nothing spent. */
    assert(SetModulationKind(300, DSD_ANALOG_DEMOD_FM, 0));
    /* A refused session request marked here spends nothing either. */
    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    assert(!SetScanRowModulation(300, DSD_ANALOG_DEMOD_FM, 22000));
    RigctlMarkSessionPassband(300, DSD_ANALOG_DEMOD_FM);
    /* So the next width reads nothing (the peer would report this client's 16 kHz), and its clear sends 0 again. */
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(300, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(300, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(300, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const sent[] = {"m\n",       "M NFM 20000\n", "M NFM 12500\n", "m\n",           "M NFM 16000\n",
                                       "M NFM 0\n", "M NFM 22000\n", "M FM 22000\n",  "M NFM 20000\n", "M NFM 0\n",
                                       NULL};
    assert(sent_since(0, sent));
    return 0;
}

/*
 * Issue #621: a scan's restore that asks for the session's passband marks the reading the session's, as every session
 * request is (RigctlMarkSessionPassband()); refused while the peer is known to run its own passband (an undo put it
 * back there as read), that reading is spent at once, as the marker spends it, so the next session width reads the
 * peer's own again rather than reuse the one the scan took.
 */
static int
test_a_refused_session_restore_on_the_peers_own_is_spent(void) {
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(302, DSD_ANALOG_DEMOD_FM, 25000));
    push_response("RPRT 0\n");
    assert(SetModulationKind(302, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    assert(!RestoreScanModulation(302, DSD_ANALOG_DEMOD_FM, 20000, true));
    /* The operator sets 9 kHz on the peer; the next session width, with nothing in between, reads it. */
    push_response("FM\n9000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(302, DSD_ANALOG_DEMOD_FM, 16000));
    RigctlMarkSessionPassband(302, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(302, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const sent[] = {"m\n",           "M NFM 25000\n", "M NFM 12500\n",
                                       "M NFM 20000\n", "M FM 20000\n",  "m\n",
                                       "M NFM 16000\n", "M NFM 9000\n",  NULL};
    assert(sent_since(0, sent));
    return 0;
}

/* Issue #621: the legacy -Y leg asks for -B at each step and leaves a repeat to the record (SetModulationKind()), which
 * every writer updates: after a live width on the monitor the same -B goes out again. */
static int
test_setmodulation_repeat_follows_every_writer(void) {
    reset_stubs();
    push_response("RPRT 0\n");
    assert(SetModulation(295, 12000));
    assert(SetModulation(295, 12000));
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(295, DSD_ANALOG_DEMOD_FM, 25000));
    push_response("RPRT 0\n");
    assert(SetModulation(295, 12000));
    static const char* const sent[] = {"M NFM 12000\n", "m\n", "M NFM 25000\n", "M NFM 12000\n", NULL};
    assert(sent_since(0, sent));
    return 0;
}

/*
 * Issue #621: once the peer has taken a reading of its own passband back, that reading is retired: it is never sent
 * again, and the demodulator is as if this client had not changed it until a new change reads afresh. A reconnect to
 * the same endpoint hands the record over unconfirmed, which re-sends a passband but has nothing of this client's to
 * undo, so FM at the peer's own (a manual tune on the FM monitor with neither a width nor -B) sends nothing; and a
 * switch back from AM after a read that failed asks for FM at 0, not for the retired reading.
 */
static int
test_a_retired_reading_is_never_sent_again(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    char host[] = "127.0.0.1";

    /* A session width set and cleared on the FM monitor: the peer's own 12.5 kHz goes back and the reading is spent. */
    reset_stubs();
    fm_monitor_session(&opts, 310);
    g_session_request = 1;
    opts.analog_nfm_bandwidth_hz = 20000;
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155475000L) == 0);
    opts.analog_nfm_bandwidth_hz = 0;
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155500000L) == 0);
    /* The operator sets 9 kHz on the peer, and rigctl reconnects to the same endpoint. The next manual tune asks for
       FM at the peer's own: nothing of this client's stands, so only the frequency goes out. */
    g_create_result = 311;
    assert(Connect(host, 4532) == 311);
    RigctlRebindPeer(310, 311, 1);
    opts.rigctl_sockfd = 311;
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155525000L) == 0);
    /* A new width reads the operator's 9 kHz, which its clear sends back. */
    opts.analog_nfm_bandwidth_hz = 16000;
    push_response("FM\n9000\n");
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155550000L) == 0);
    opts.analog_nfm_bandwidth_hz = 0;
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155575000L) == 0);
    static const char* const reconnect[] = {"m\n",           "M NFM 20000\n", "F 155475000\n", "M NFM 12500\n",
                                            "F 155500000\n", "F 155525000\n", "m\n",           "M NFM 16000\n",
                                            "F 155550000\n", "M NFM 9000\n",  "F 155575000\n", NULL};
    assert(sent_since(0, reconnect));

    /* The same spend, then the AM monitor on a peer that cannot say what it runs (no FM reading comes with it), then
       FM at the peer's own again: the switch back asks for FM at 0, never for the retired 12.5 kHz. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(312, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(312, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(312, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT -11\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(312, DSD_ANALOG_DEMOD_AM, 6000));
    RigctlMarkSessionPassband(312, DSD_ANALOG_DEMOD_AM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(312, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const unread[] = {"m\n", "M NFM 20000\n", "M NFM 12500\n", "m\n", "M AM 6000\n", "M NFM 0\n",
                                         NULL};
    assert(sent_since(0, unread));
    g_session_request = 0;
    return 0;
}

/*
 * Issue #621: on PCM audio input the peer's FM passband is what a digital mode is heard through, so -B there is one
 * more passband this client sets: it is captured (the peer's own passband read first, "m") before it overwrites that
 * passband, best-effort and strict exactly as -B was on each path. Entering the FM monitor at the same width then finds
 * the reading in place, and a later return to the peer's own sends the passband read, never "M NFM 0". On an RTL-family
 * or symbol input the peer only follows the frequency: -B goes out as before, with no read.
 */
static int
test_follow_at_b_on_audio_input_reads_the_own_passband_first(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    /* A digital session with -B 20 kHz tunes: the read, -B, the frequency. Then the FM monitor at a configured 20 kHz,
       the passband the peer runs already; then -B 0 and the width cleared: the peer's own 12.5 kHz. */
    reset_stubs();
    fm_monitor_session(&opts, 320);
    opts.analog_only = 0;
    opts.monitor_input_audio = 0;
    opts.frame_dmr = 1;
    opts.setmod_bw = 20000;
    g_session_request = 1;
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 461000000L) == 0);
    opts.frame_dmr = 0;
    opts.analog_only = 1;
    opts.monitor_input_audio = 1;
    opts.analog_nfm_bandwidth_hz = 20000;
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155475000L) == 0);
    opts.setmod_bw = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 155500000L) == 0);
    static const char* const monitor[] = {
        "m\n", "M NFM 20000\n", "F 461000000\n", "F 155475000\n", "M NFM 12500\n", "F 155500000\n", NULL};
    assert(sent_since(0, monitor));

    /* The digital session's own -B undone (-B 0) sends the reading and retires it; the operator sets 9 kHz; -B again
       reads afresh, and its undo sends that. */
    reset_stubs();
    fm_monitor_session(&opts, 321);
    opts.analog_only = 0;
    opts.monitor_input_audio = 0;
    opts.frame_dmr = 1;
    opts.setmod_bw = 20000;
    g_session_request = 1;
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 461000000L) == 0);
    opts.setmod_bw = 0;
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 461012500L) == 0);
    opts.setmod_bw = 20000;
    push_response("FM\n9000\n");
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 461025000L) == 0);
    opts.setmod_bw = 0;
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(io_control_set_freq(&opts, &state, 461037500L) == 0);
    static const char* const digital[] = {"m\n", "M NFM 20000\n", "F 461000000\n", "M NFM 12500\n", "F 461012500\n",
                                          "m\n", "M NFM 20000\n", "F 461025000\n", "M NFM 9000\n",  "F 461037500\n",
                                          NULL};
    assert(sent_since(0, digital));

    /* A refused -B still fails the tune before the frequency moves, as it always has on this path. */
    reset_stubs();
    fm_monitor_session(&opts, 322);
    opts.analog_only = 0;
    opts.monitor_input_audio = 0;
    opts.frame_dmr = 1;
    opts.setmod_bw = 25000;
    g_session_request = 1;
    opts.rtlsdr_center_freq = 461000000U;
    push_response("FM\n12500\n");
    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    assert(io_control_set_freq(&opts, &state, 461050000L) == -1);
    static const char* const refused[] = {"m\n", "M NFM 25000\n", "M FM 25000\n", NULL};
    assert(sent_since(0, refused));
    assert(opts.rtlsdr_center_freq == 461000000U);

    /* On an RTL-family or symbol input the peer only follows the frequency: -B as before, nothing read. */
    static const int inputs[] = {AUDIO_IN_RTL, AUDIO_IN_SYMBOL_BIN};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
        reset_stubs();
        fm_monitor_session(&opts, (dsd_socket_t)(323 + i));
        opts.audio_in_type = inputs[i];
        opts.analog_only = 0;
        opts.monitor_input_audio = 0;
        opts.frame_dmr = 1;
        opts.setmod_bw = 20000;
        push_response("RPRT 0\n");
        push_response("RPRT 0\n");
        assert(io_control_set_freq(&opts, &state, 461000000L) == 0);
        static const char* const follow[] = {"M NFM 20000\n", "F 461000000\n", NULL};
        assert(sent_since(0, follow));
    }
    g_session_request = 0;
    return 0;
}

/*
 * Issue #621: a session request's lookup of the peer's own passband reads the demodulator the peer runs as well as the
 * one asked for. Both readings serve that request: once the peer takes the asked-for demodulator's passband back, or
 * refuses the request while on its own, the other demodulator's reading that nothing of this client's holds is
 * retired with it, so a later request for that demodulator reads what the operator may have changed meanwhile. A
 * reading the session still holds (a passband of this client's stands on that demodulator) stays, and a scan's
 * readings keep their lifetime, the scan (issue #526).
 */
static int
test_a_session_readings_of_the_other_demodulator_ends_with_it(void) {
    /* The FM monitor's width on a peer running AM at 10 kHz: AM is read in passing, then FM. The clear returns the
       peer to its own FM and retires both readings; the operator sets AM to 9 kHz; the next am row reads it afresh,
       and the scan's restore sends 9 kHz back, not 10. */
    reset_stubs();
    push_response("AM\n10000\n");
    push_response("RPRT 0\n");
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(330, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(330, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(330, DSD_ANALOG_DEMOD_FM, 0));
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("AM\n9000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(330, DSD_ANALOG_DEMOD_AM, 6000));
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(330, DSD_ANALOG_DEMOD_FM, 0, false));
    static const char* const retired[] = {"m\n",           "M NFM 0\n",   "m\n",           "M NFM 20000\n",
                                          "M NFM 12500\n", "m\n",         "M AM 0\n",      "m\n",
                                          "M AM 6000\n",   "M AM 9000\n", "M NFM 12500\n", NULL};
    assert(sent_since(0, retired));

    /* While the width stands, the AM reading taken with it is what the session's AM monitor relies on (no new read),
       and once the session's AM passband stands too, the FM undo retires FM's reading alone: AM's still has an undo
       to serve, which the scan's restore sends before it switches the peer back to FM at its own passband -- as 0,
       since the retired reading is never sent again (the operator may have changed it while the peer ran it). */
    reset_stubs();
    push_response("AM\n10000\n");
    push_response("RPRT 0\n");
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(331, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(331, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(331, DSD_ANALOG_DEMOD_AM, 6000));
    RigctlMarkSessionPassband(331, DSD_ANALOG_DEMOD_AM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(331, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(331, DSD_ANALOG_DEMOD_FM, 0, false));
    static const char* const held[] = {
        "m\n", "M NFM 0\n", "m\n", "M NFM 20000\n", "M AM 6000\n", "M NFM 12500\n", "M AM 10000\n", "M NFM 0\n", NULL};
    assert(sent_since(0, held));

    /* A scan's readings (no mark) last the scan: the undo retires nothing, and the am row after it reads nothing. */
    reset_stubs();
    push_response("AM\n10000\n");
    push_response("RPRT 0\n");
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(332, DSD_ANALOG_DEMOD_FM, 25000));
    push_response("RPRT 0\n");
    assert(SetModulationKind(332, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(332, DSD_ANALOG_DEMOD_AM, 6000));
    static const char* const scan[] = {"m\n",           "M NFM 0\n",   "m\n", "M NFM 25000\n",
                                       "M NFM 12500\n", "M AM 6000\n", NULL};
    assert(sent_since(0, scan));

    /* A session request refused while the peer runs its own passband (switched to FM at its own to be read) retires
       the AM reading taken in passing too: the next AM request reads afresh. */
    reset_stubs();
    push_response("AM\n10000\n");
    push_response("RPRT 0\n");
    push_response("FM\n12500\n");
    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    assert(!SetScanRowModulation(333, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(333, DSD_ANALOG_DEMOD_FM);
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("AM\n9000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(333, DSD_ANALOG_DEMOD_AM, 6000));
    RigctlMarkSessionPassband(333, DSD_ANALOG_DEMOD_AM);
    static const char* const refused[] = {"m\n", "M NFM 0\n", "m\n", "M NFM 20000\n", "M FM 20000\n",
                                          "m\n", "M AM 0\n",  "m\n", "M AM 6000\n",   NULL};
    assert(sent_since(0, refused));
    return 0;
}

/*
 * Issue #526: a row request whose reply was lost may have reached the peer, so the passband asked for may stand: the
 * scan's restore still sends the peer's own back for that demodulator, and then switches the peer to FM at the own
 * passband read before.
 */
static int
test_a_row_request_whose_reply_was_lost_is_still_undone(void) {
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("AM\n10000\n");
    assert(!SetScanRowModulation(340, DSD_ANALOG_DEMOD_AM, 6000));
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(340, DSD_ANALOG_DEMOD_FM, 0, false));
    static const char* const undone[] = {"m\n",          "M AM 0\n",      "m\n", "M AM 6000\n",
                                         "M AM 10000\n", "M NFM 12500\n", NULL};
    assert(sent_since(0, undone));
    return 0;
}

/*
 * Issue #621: "the peer runs its own passband" is one rule, whether or not a reconnect left the cache unconfirmed: the
 * last request the peer accepted left it on that demodulator at its own passband, with a reading behind the 0. A
 * session width the peer refuses right after a reconnect to the same endpoint therefore retires the fresh reading at
 * once, as it would without the reconnect, the rollback's FM at 0 finds nothing to undo, and the next width reads what
 * the operator set meanwhile rather than reuse the reading.
 */
static int
test_a_refusal_on_an_unconfirmed_cache_retires_the_reading(void) {
    char host[] = "127.0.0.1";
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(350, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(350, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(350, DSD_ANALOG_DEMOD_FM, 0));
    /* A reconnect to the same endpoint hands the record over unconfirmed. */
    g_create_result = 351;
    assert(Connect(host, 4532) == 351);
    RigctlRebindPeer(350, 351, 1);
    /* A width the peer refuses: the reading just taken is of no use, and the peer runs its own passband. */
    push_response("FM\n12500\n");
    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    assert(!SetScanRowModulation(351, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(351, DSD_ANALOG_DEMOD_FM);
    /* The rollback asks for FM at the peer's own: nothing of this client's to undo, nothing sent. */
    assert(SetModulationKind(351, DSD_ANALOG_DEMOD_FM, 0));
    /* The operator sets 9 kHz; the next width reads it, and its clear sends it back. */
    push_response("FM\n9000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(351, DSD_ANALOG_DEMOD_FM, 16000));
    RigctlMarkSessionPassband(351, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(351, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const fresh[] = {"m\n", "M NFM 20000\n", "M NFM 12500\n", "m\n", "M NFM 20000\n", "M FM 20000\n",
                                        "m\n", "M NFM 16000\n", "M NFM 9000\n",  NULL};
    assert(sent_since(0, fresh));
    return 0;
}

/*
 * Issue #621: a tune that fails after its request returned the peer to its own passband puts back the passband this
 * client had set (RevertModulation()): that return is undone, so the reading it retired is reinstated and the later
 * clear sends the passband read, not 0. A revert whose reply was lost may have stood, so it reinstates too; one the
 * peer refused leaves the return in force, so nothing comes back and the next width reads afresh.
 */
static int
test_a_failed_tune_reinstates_the_reading_its_return_retired(void) {
    /* A session width on the FM monitor, then a scan's digital row with -B 0: the return is taken, the frequency is
       refused, the revert puts the width back. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(360, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(360, DSD_ANALOG_DEMOD_FM);
    const dsd_rigctl_modulation width = CachedModulation(360);
    assert(width.kind == DSD_ANALOG_DEMOD_FM && width.bandwidth == 20000);
    push_response("RPRT 0\n");
    assert(SetModulationKind(360, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 1\n");
    assert(!SetFreq(360, 461000000L));
    push_response("RPRT 0\n");
    assert(RevertModulation(360, width));
    push_response("RPRT 0\n");
    assert(SetModulationKind(360, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const reinstated[] = {
        "m\n", "M NFM 20000\n", "M NFM 12500\n", "F 461000000\n", "M NFM 20000\n", "M NFM 12500\n", NULL};
    assert(sent_since(0, reinstated));

    /* The session's AM monitor: the return to FM retires FM's reading taken in passing; the revert back to AM
       reinstates it, and the later switch to FM at the peer's own sends it. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("AM\n10000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(361, DSD_ANALOG_DEMOD_AM, 6000));
    RigctlMarkSessionPassband(361, DSD_ANALOG_DEMOD_AM);
    const dsd_rigctl_modulation am = CachedModulation(361);
    push_response("RPRT 0\n");
    assert(SetModulationKind(361, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 1\n");
    assert(!SetFreq(361, 461000000L));
    push_response("RPRT 0\n");
    assert(RevertModulation(361, am));
    push_response("RPRT 0\n");
    assert(SetModulationKind(361, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const incidental[] = {"m\n",         "M AM 0\n",      "m\n",
                                             "M AM 6000\n", "M NFM 12500\n", "F 461000000\n",
                                             "M AM 6000\n", "M NFM 12500\n", NULL};
    assert(sent_since(0, incidental));

    /* A revert whose reply was lost: the width may stand, so the reading comes back and the clear sends it. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(362, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(362, DSD_ANALOG_DEMOD_FM);
    const dsd_rigctl_modulation lost = CachedModulation(362);
    push_response("RPRT 0\n");
    assert(SetModulationKind(362, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 1\n");
    assert(!SetFreq(362, 461000000L));
    assert(!RevertModulation(362, lost));
    push_response("RPRT 0\n");
    assert(SetModulationKind(362, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const stood[] = {
        "m\n", "M NFM 20000\n", "M NFM 12500\n", "F 461000000\n", "M NFM 20000\n", "M NFM 12500\n", NULL};
    assert(sent_since(0, stood));

    /* A revert the peer refused leaves the return in force: nothing comes back, and the next width reads afresh. */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(363, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(363, DSD_ANALOG_DEMOD_FM);
    const dsd_rigctl_modulation refused = CachedModulation(363);
    push_response("RPRT 0\n");
    assert(SetModulationKind(363, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 1\n");
    assert(!SetFreq(363, 461000000L));
    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    assert(!RevertModulation(363, refused));
    push_response("FM\n9000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(363, DSD_ANALOG_DEMOD_FM, 16000));
    RigctlMarkSessionPassband(363, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(363, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const kept_return[] = {
        "m\n",          "M NFM 20000\n", "M NFM 12500\n", "F 461000000\n", "M NFM 20000\n",
        "M FM 20000\n", "m\n",           "M NFM 16000\n", "M NFM 9000\n",  NULL};
    assert(sent_since(0, kept_return));

    /* What a return holds lasts only until the next request this client sends: the session's AM monitor, a digital
       row's return to FM (taken, retiring FM's reading taken in passing) whose tune lands, the am row again, then a
       digital row's return to FM sent as 0 (no reading left) whose tune fails. The revert puts AM back, but later
       requests superseded the hold, so nothing comes back, and the next return to FM is 0 again, not the 12.5 kHz the
       operator may have changed meanwhile (test_a_hold_lives_only_until_the_next_request() for a revert of an am
       row's request). */
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("AM\n10000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(364, DSD_ANALOG_DEMOD_AM, 6000));
    RigctlMarkSessionPassband(364, DSD_ANALOG_DEMOD_AM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(364, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 0\n");
    assert(SetFreq(364, 461000000L));
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(364, DSD_ANALOG_DEMOD_AM, 6000));
    const dsd_rigctl_modulation am_again = CachedModulation(364);
    push_response("RPRT 0\n");
    assert(SetModulationKind(364, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 1\n");
    assert(!SetFreq(364, 461012500L));
    push_response("RPRT 0\n");
    assert(RevertModulation(364, am_again));
    push_response("RPRT 0\n");
    assert(SetModulationKind(364, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const superseded[] = {"m\n",           "M AM 0\n",      "m\n",         "M AM 6000\n",
                                             "M NFM 12500\n", "F 461000000\n", "M AM 6000\n", "M NFM 0\n",
                                             "F 461012500\n", "M AM 6000\n",   "M NFM 0\n",   NULL};
    assert(sent_since(0, superseded));
    return 0;
}

/*
 * Issue #621: what a return holds for a revert lives only until the next request this client sends, whichever
 * demodulator it names: a failed tune's revert reinstates a retired reading only where the request it undoes is that
 * return. A revert that undoes a later request leaves the readings retired, so no later return sends a passband the
 * operator may have changed since. Here the session's AM monitor on a peer running FM at 12.5 kHz (FM read in passing),
 * a digital row's return to FM (FM's reading retired and held) whose tune lands, then two am rows: the second one's
 * frequency is refused, and its revert undoes that row's request, not the return. The next return to FM sends 0, not
 * the retired 12.5 kHz.
 */
static int
test_a_hold_lives_only_until_the_next_request(void) {
    reset_stubs();
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    push_response("AM\n9000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(370, DSD_ANALOG_DEMOD_AM, 10000));
    RigctlMarkSessionPassband(370, DSD_ANALOG_DEMOD_AM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(370, DSD_ANALOG_DEMOD_FM, 0));
    push_response("RPRT 0\n");
    assert(SetFreq(370, 461000000L));
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(370, DSD_ANALOG_DEMOD_AM, 6000));
    push_response("RPRT 0\n");
    assert(SetFreq(370, 461012500L));
    const dsd_rigctl_modulation am_row = CachedModulation(370);
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(370, DSD_ANALOG_DEMOD_AM, 8000));
    push_response("RPRT 1\n");
    assert(!SetFreq(370, 461025000L));
    push_response("RPRT 0\n");
    assert(RevertModulation(370, am_row));
    push_response("RPRT 0\n");
    assert(SetModulationKind(370, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const am_rows[] = {
        "m\n",         "M AM 0\n",      "m\n",         "M AM 10000\n",  "M NFM 12500\n", "F 461000000\n",
        "M AM 6000\n", "F 461012500\n", "M AM 8000\n", "F 461025000\n", "M AM 6000\n",   "M NFM 0\n",
        NULL};
    assert(sent_since(0, am_rows));
    return 0;
}

/*
 * Issue #621: as above, for the other demodulator's hold: a session width set and cleared on the FM monitor while the
 * peer ran AM at 10 kHz retires both readings. A new width reads FM afresh, an nfm row's frequency is refused, and its
 * revert puts the new width back, undoing the row's request, not the clear. The operator sets AM to 8 kHz: the am row
 * reads it, and the scan's restore sends 8 kHz back, not the retired 10.
 */
static int
test_a_hold_of_the_other_demodulator_ends_at_the_next_request(void) {
    reset_stubs();
    push_response("AM\n10000\n");
    push_response("RPRT 0\n");
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(371, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(371, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(371, DSD_ANALOG_DEMOD_FM, 0));
    push_response("FM\n12500\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(371, DSD_ANALOG_DEMOD_FM, 16000));
    RigctlMarkSessionPassband(371, DSD_ANALOG_DEMOD_FM);
    const dsd_rigctl_modulation width = CachedModulation(371);
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(371, DSD_ANALOG_DEMOD_FM, 11000));
    push_response("RPRT 1\n");
    assert(!SetFreq(371, 461000000L));
    push_response("RPRT 0\n");
    assert(RevertModulation(371, width));
    push_response("FM\n16000\n");
    push_response("RPRT 0\n");
    push_response("AM\n8000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(371, DSD_ANALOG_DEMOD_AM, 6000));
    push_response("RPRT 0\n");
    assert(SetFreq(371, 461012500L));
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(371, DSD_ANALOG_DEMOD_FM, 16000, true));
    static const char* const nfm_row[] = {
        "m\n",           "M NFM 0\n",     "m\n",           "M NFM 20000\n", "M NFM 12500\n", "m\n",
        "M NFM 16000\n", "M NFM 11000\n", "F 461000000\n", "M NFM 16000\n", "m\n",           "M AM 0\n",
        "m\n",           "M AM 6000\n",   "F 461012500\n", "M AM 8000\n",   "M NFM 16000\n", NULL};
    assert(sent_since(0, nfm_row));
    return 0;
}

/*
 * Issue #621: a session request the peer refuses while it runs its own passband retires the reading at once
 * (RigctlMarkSessionPassband()), but no return retired it, so a failed tune's revert brings nothing back: here the
 * revert undoes the switch to FM that the read made, from an am row's AM, and the next FM width reads what the
 * operator set meanwhile.
 */
static int
test_a_mark_retires_without_a_hold(void) {
    reset_stubs();
    push_response("AM\n10000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(372, DSD_ANALOG_DEMOD_AM, 6000));
    const dsd_rigctl_modulation am_row = CachedModulation(372);
    /* A session's -B at 7 kHz: the read switches the peer to FM at its own passband, and the peer refuses the -B. */
    push_response("AM\n6000\n");
    push_response("RPRT 0\n");
    push_response("FM\n12500\n");
    push_response("RPRT 1\n");
    push_response("RPRT 1\n");
    assert(!SetScanRowModulation(372, DSD_ANALOG_DEMOD_FM, 7000));
    RigctlMarkSessionPassband(372, DSD_ANALOG_DEMOD_FM);
    /* The best-effort tune goes on (the peer runs FM), its frequency is refused, and the revert puts the am row back. */
    push_response("RPRT 1\n");
    assert(!SetFreq(372, 461000000L));
    push_response("RPRT 0\n");
    assert(RevertModulation(372, am_row));
    /* The operator sets FM to 9 kHz: the next FM width reads it, and its clear sends it back. */
    push_response("AM\n6000\n");
    push_response("RPRT 0\n");
    push_response("FM\n9000\n");
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(372, DSD_ANALOG_DEMOD_FM, 20000));
    RigctlMarkSessionPassband(372, DSD_ANALOG_DEMOD_FM);
    push_response("RPRT 0\n");
    assert(SetModulationKind(372, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const unheld[] = {"m\n",          "M AM 6000\n", "m\n",           "M NFM 0\n",    "m\n",
                                         "M NFM 7000\n", "M FM 7000\n", "F 461000000\n", "M AM 6000\n",  "m\n",
                                         "M NFM 0\n",    "m\n",         "M NFM 20000\n", "M NFM 9000\n", NULL};
    assert(sent_since(0, unheld));
    return 0;
}

/*
 * Issue #621: a manual tune tells a scan's request from the session's as every other rigctl leg does
 * (dsd_channel_modes_rigctl_request_is_session()): a typed -Y list configured before its first row's scope is entered
 * is a scan's, whose reading lasts the scan, though the scope alone would call it the session's; an untyped list's
 * requests are the session's, whose reading a return retires.
 */
static int
test_a_manual_tune_weighs_a_typed_list_before_its_first_scope(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    for (int typed = 1; typed >= 0; typed--) {
        const dsd_socket_t sockfd = typed ? 373 : 374;
        reset_stubs();
        fm_monitor_session(&opts, sockfd);
        opts.scanner_mode = 1;
        g_session_request = 1;
        g_channel_modes_present = typed;
        opts.analog_nfm_bandwidth_hz = 20000;
        push_response("FM\n12500\n");
        push_response("RPRT 0\n");
        push_response("RPRT 0\n");
        assert(io_control_set_freq(&opts, &state, 155475000L) == 0);
        assert(g_session_request_state == &state);
        push_response("RPRT 0\n");
        assert(SetModulationKind(sockfd, DSD_ANALOG_DEMOD_FM, 0));
        /* The operator sets 9 kHz; the next width reads it only where the return retired the session's reading. */
        opts.analog_nfm_bandwidth_hz = 16000;
        if (!typed) {
            push_response("FM\n9000\n");
        }
        push_response("RPRT 0\n");
        push_response("RPRT 0\n");
        assert(io_control_set_freq(&opts, &state, 155500000L) == 0);
        push_response("RPRT 0\n");
        assert(SetModulationKind(sockfd, DSD_ANALOG_DEMOD_FM, 0));
        static const char* const scan[] = {"m\n",           "M NFM 20000\n", "F 155475000\n", "M NFM 12500\n",
                                           "M NFM 16000\n", "F 155500000\n", "M NFM 12500\n", NULL};
        static const char* const session[] = {"m\n",           "M NFM 20000\n", "F 155475000\n",
                                              "M NFM 12500\n", "m\n",           "M NFM 16000\n",
                                              "F 155500000\n", "M NFM 9000\n",  NULL};
        assert(sent_since(0, typed ? scan : session));
    }
    return 0;
}

/* The tuner path carries a uint32_t, so where long is 64-bit a frequency past 4294967295 Hz would wrap to an unrelated
 * channel: it is refused before either backend is asked, and the cached centre stays where it was. 4294967295 Hz
 * itself still reaches the backend. */
static int
test_io_control_set_freq_refuses_past_the_tuner_limit(void) {
#if LONG_MAX > 4294967295L
    static dsd_opts opts;
    static dsd_state state;

    reset_stubs();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.use_rigctl = 1;
    opts.rigctl_sockfd = 104;
    opts.setmod_bw = 12500;
    opts.rtlsdr_center_freq = 851000000U;
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    /* 4294967296 + 851000000 wraps to 851000000 as a uint32_t. */
    assert(io_control_set_freq(&opts, &state, 5145967296L) == -1);
    assert(g_command_count == 0);
    assert(opts.rtlsdr_center_freq == 851000000U);

    reset_stubs();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtlsdr_center_freq = 851000000U;
    state.rtl_ctx = (RtlSdrContext*)&state;
    assert(io_control_set_freq(&opts, &state, 4294967296L) == -1);
    assert(io_control_set_freq(&opts, &state, 5145967296L) == -1);
    assert(g_rtl_tune_calls == 0);
    assert(opts.rtlsdr_center_freq == 851000000U);

    assert(io_control_set_freq(&opts, &state, 4294967295L) == RTL_STREAM_TUNE_OK);
    assert(g_rtl_tune_calls == 1);
    assert(g_rtl_tune_freq == 4294967295U);
    assert(opts.rtlsdr_center_freq == 4294967295U);
#endif
    return 0;
}

static int
test_io_control_set_freq_rejects_missing_rtl_context(void) {
    reset_stubs();
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtlsdr_center_freq = 851000000U;

    assert(io_control_set_freq(&opts, &state, 851075000L) == -1);
    assert(g_rtl_tune_calls == 0);
    assert(opts.rtlsdr_center_freq == 851000000U);
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
    g_rtl_last_applied_freq = 851075000U;
    assert(io_control_set_freq(&opts, &state, 851075000L) == RTL_STREAM_TUNE_OK);
    assert(g_rtl_tune_calls == 2);
    assert(opts.rtlsdr_center_freq == 851075000U);
    return 0;
}

static int
test_io_control_set_freq_caches_applied_rtl_frequency(void) {
    reset_stubs();
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtlsdr_center_freq = 851000000U;
    state.rtl_ctx = (RtlSdrContext*)&state;
    g_rtl_last_applied_freq = 851100000U;

    assert(io_control_set_freq(&opts, &state, 851075000L) == RTL_STREAM_TUNE_OK);
    assert(g_rtl_tune_calls == 1);
    assert(g_rtl_tune_freq == 851075000U);
    assert(opts.rtlsdr_center_freq == 851100000U);
    return 0;
}

static int
test_io_control_set_freq_retains_accepted_rtl_timeout_target(void) {
    reset_stubs();
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtlsdr_center_freq = 851000000U;
    state.rtl_ctx = (RtlSdrContext*)&state;
    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    g_rtl_last_applied_freq = 851000000U;

    assert(io_control_set_freq(&opts, &state, 851125000L) == RTL_STREAM_TUNE_TIMEOUT);
    assert(g_rtl_tune_calls == 1);
    assert(g_rtl_tune_freq == 851125000U);
    assert(opts.rtlsdr_center_freq == 851125000U);
    return 0;
}

int
main(void) {
    int rc = 0;
    rc |= test_connect_failure_cleanup();
    rc |= test_connect_success_uses_rigctl_timeout();
    rc |= test_connect_bounded_resolves_once_and_bounds_the_connect();
    rc |= test_connect_bounded_cancel_and_failures_close_the_socket();
    rc |= test_startup_connect_is_kept_for_the_reconnect();
    rc |= test_setfreq_success_failure_and_cache();
    rc |= test_setmodulation_fallback_and_cache();
    rc |= test_setmodulation_kind_am_and_passband_restore();
    rc |= test_scan_row_modulation_reads_and_restores_the_own_passband();
    rc |= test_scan_restore_keeps_the_own_passband_an_undo_did_not_put_back();
    rc |= test_scan_row_mode_query_reads_a_split_reply_whole();
    rc |= test_cached_modulation_kind_follows_what_the_peer_accepted();
    rc |= test_revert_modulation_puts_back_what_the_peer_ran();
    rc |= test_connect_on_another_socket_keeps_the_rigctl_record();
    rc |= test_rebind_hands_the_scan_record_to_a_reconnect();
    rc |= test_rebind_to_another_endpoint_asks_a_peer_that_may_run_am_for_fm();
    rc |= test_rebind_opens_a_fresh_record_where_no_am_is_left_behind();
    rc |= test_rebind_forgets_the_frequency_a_reused_number_would_match();
    rc |= test_full_size_replies_stay_in_bounds();
    rc |= test_get_current_freq_parses_first_line_and_errors();
    rc |= test_io_control_set_freq_validation_and_rigctl_dispatch();
    rc |= test_io_control_set_freq_asks_the_fm_monitor_passband();
    rc |= test_scan_restore_keeps_the_record_of_a_session_passband();
    rc |= test_return_to_the_own_passband_after_a_session_restore();
    rc |= test_session_passband_off_a_scan_reads_the_own_passband_again();
    rc |= test_io_control_set_freq_returns_to_the_own_passband();
    rc |= test_a_refused_session_capture_is_retired();
    rc |= test_a_failed_fresh_read_forgets_a_spent_reading();
    rc |= test_setmodulation_repeat_follows_every_writer();
    rc |= test_a_return_sent_as_0_is_not_the_peers_own();
    rc |= test_a_refused_session_restore_on_the_peers_own_is_spent();
    rc |= test_a_retired_reading_is_never_sent_again();
    rc |= test_follow_at_b_on_audio_input_reads_the_own_passband_first();
    rc |= test_a_session_readings_of_the_other_demodulator_ends_with_it();
    rc |= test_a_row_request_whose_reply_was_lost_is_still_undone();
    rc |= test_a_refusal_on_an_unconfirmed_cache_retires_the_reading();
    rc |= test_a_failed_tune_reinstates_the_reading_its_return_retired();
    rc |= test_a_hold_lives_only_until_the_next_request();
    rc |= test_a_hold_of_the_other_demodulator_ends_at_the_next_request();
    rc |= test_a_mark_retires_without_a_hold();
    rc |= test_a_manual_tune_weighs_a_typed_list_before_its_first_scope();
    rc |= test_io_control_set_freq_refuses_past_the_tuner_limit();
    rc |= test_io_control_set_freq_rejects_missing_rtl_context();
    rc |= test_io_control_set_freq_propagates_rtl_deferred();
    rc |= test_io_control_set_freq_caches_applied_rtl_frequency();
    rc |= test_io_control_set_freq_retains_accepted_rtl_timeout_target();
    return rc;
}

// NOLINTEND(bugprone-multi-level-implicit-pointer-conversion)
