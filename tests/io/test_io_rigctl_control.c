// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-multi-level-implicit-pointer-conversion)
/*
 * Rigctl control-plane tests use socket stubs to verify command/response
 * behavior without requiring a live rigctl server or network service.
 */

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
#include <limits.h>
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
    assert(!RestoreScanModulation(140, DSD_ANALOG_DEMOD_FM, 0));
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
    assert(RestoreScanModulation(140, DSD_ANALOG_DEMOD_FM, 0));
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
    assert(RestoreScanModulation(141, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const lost_am[] = {"m\n", "M AM 6000\n", "M AM 10000\n", "M NFM 0\n", NULL};
    assert(sent_since(0, lost_am));
    push_response("RPRT 0\n");
    assert(SetScanRowModulation(141, DSD_ANALOG_DEMOD_AM, 6000));
    push_response("RPRT 0\n");
    push_response("RPRT 0\n");
    assert(RestoreScanModulation(141, DSD_ANALOG_DEMOD_FM, 0));
    static const char* const again_am[] = {"M AM 6000\n", "M AM 10000\n", "M NFM 0\n", NULL};
    assert(sent_since(4, again_am));
    assert(RestoreScanModulation(141, DSD_ANALOG_DEMOD_FM, 0));
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
    assert(RestoreScanModulation(136, DSD_ANALOG_DEMOD_FM, 0));
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
    assert(RestoreScanModulation(191, DSD_ANALOG_DEMOD_FM, 0));
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
    assert(RestoreScanModulation(193, DSD_ANALOG_DEMOD_FM, 0));
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
    assert(RestoreScanModulation(209, DSD_ANALOG_DEMOD_FM, 0));
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
    assert(RestoreScanModulation(195, DSD_ANALOG_DEMOD_FM, 0));
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
    assert(RestoreScanModulation(201, DSD_ANALOG_DEMOD_FM, 0));
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
    assert(RestoreScanModulation(212, DSD_ANALOG_DEMOD_FM, 0));
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
    rc |= test_io_control_set_freq_refuses_past_the_tuner_limit();
    rc |= test_io_control_set_freq_rejects_missing_rtl_context();
    rc |= test_io_control_set_freq_propagates_rtl_deferred();
    rc |= test_io_control_set_freq_caches_applied_rtl_frequency();
    rc |= test_io_control_set_freq_retains_accepted_rtl_timeout_target();
    return rc;
}

// NOLINTEND(bugprone-multi-level-implicit-pointer-conversion)
