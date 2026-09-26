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
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
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

static void
reset_stubs(void) {
    DSD_MEMSET(g_commands, 0, sizeof(g_commands));
    DSD_MEMSET(g_responses, 0, sizeof(g_responses));
    g_command_count = 0;
    g_response_count = 0;
    g_response_index = 0;
    g_create_result = 41;
    g_send_result = -2;
    g_resolve_result = 0;
    g_connect_result = 0;
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
    assert(RestoreScanModulation(130, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const restore[] = {"M AM 10000\n", "M NFM 12500\n", "M FM 12500\n", NULL};
    assert(sent_since(9, restore));
    assert(SetModulationKind(130, DSD_ANALOG_DEMOD_FM, 0));
    assert(RestoreScanModulation(130, DSD_ANALOG_DEMOD_FM, 0));
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
    assert(RestoreScanModulation(130, DSD_ANALOG_DEMOD_FM, 0));
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
    assert(RestoreScanModulation(131, DSD_ANALOG_DEMOD_FM, 0));
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
    assert(RestoreScanModulation(134, DSD_ANALOG_DEMOD_FM, 7000));
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
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");

    assert(io_control_set_freq(&opts, &state, 851050000L) == 0);
    assert(opts.rtlsdr_center_freq == 851050000U);
    assert(g_command_count == 2);
    assert(strcmp(g_commands[0], "M NFM 12500\n") == 0);
    assert(strcmp(g_commands[1], "F 851050000\n") == 0);

    reset_stubs();
    opts.rtlsdr_center_freq = 851050000U;
    push_response("RPRT 1\n");
    assert(io_control_set_freq(&opts, &state, 851062500L) == -1);
    assert(opts.rtlsdr_center_freq == 851050000U);

    reset_stubs();
    opts.rigctl_sockfd = DSD_INVALID_SOCKET;
    assert(io_control_set_freq(&opts, &state, 851075000L) == -1);
    assert(g_command_count == 0);
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
    rc |= test_setfreq_success_failure_and_cache();
    rc |= test_setmodulation_fallback_and_cache();
    rc |= test_setmodulation_kind_am_and_passband_restore();
    rc |= test_scan_row_modulation_reads_and_restores_the_own_passband();
    rc |= test_full_size_replies_stay_in_bounds();
    rc |= test_get_current_freq_parses_first_line_and_errors();
    rc |= test_io_control_set_freq_validation_and_rigctl_dispatch();
    rc |= test_io_control_set_freq_rejects_missing_rtl_context();
    rc |= test_io_control_set_freq_propagates_rtl_deferred();
    rc |= test_io_control_set_freq_caches_applied_rtl_frequency();
    rc |= test_io_control_set_freq_retains_accepted_rtl_timeout_target();
    return rc;
}

// NOLINTEND(bugprone-multi-level-implicit-pointer-conversion)
