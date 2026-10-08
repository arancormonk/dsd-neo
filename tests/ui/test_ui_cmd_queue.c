// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic queue-level contracts for app-control commands.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/rx_tone_view.h>
#include <dsd-neo/app_control/scan_row_view.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/rtl_stream_fwd.h>
#include <dsd-neo/io/udp_input.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <dsd-neo/runtime/squelch.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <errno.h>
#include <math.h>
#include <sndfile.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#endif
#include <dsd-neo/core/file_io.h>

#include <dsd-neo/core/audio_input_switch.h>
#include "../../src/app_control/commands_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/runtime/config.h"
#include "services.h"
#include "test_support.h"

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
static int g_io_control_tune_result = RTL_STREAM_TUNE_OK;
static int g_io_control_tune_calls = 0;
static long int g_io_control_tune_freq = 0;
static dsd_trunk_tune_result g_cc_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
static int g_cc_tune_calls = 0;
static long int g_cc_tune_freq = 0;
static int g_cc_tune_ted_sps = 0;
static int g_cc_profile_at_tune = -1;
static int g_skip_arm_refused = 0;
/* TCP audio connect and Pulse input open: the real functions unless a test arms a result. */
static int g_tcp_connect_stub_armed = 0;
static int g_tcp_connect_stub_rc = 0;
static int g_tcp_connect_calls = 0;
static int g_reconfigure_output_fails = 0;
/* Whether the last output reconfigure ran under the P25 SM tick guard (issue #634); -1 before one runs. */
static int g_reconfigure_guarded = -1;
static int g_open_audio_input_stub_armed = 0;
static int g_open_audio_input_stub_rc = 0;
static int g_open_audio_input_calls = 0;
/* Rigctl connect (issue #589): the real service unless a test arms a result; the endpoint it was asked for. */
static int g_rigctl_connect_stub_armed = 0;
static int g_rigctl_connect_stub_rc = 0;
static int g_rigctl_connect_calls = 0;
static char g_rigctl_connect_host[256];
static int g_rigctl_connect_port = 0;
/* The decoder state the connect was given, which it passes to the width ask (issue #621). */
static const dsd_state* g_rigctl_connect_state = NULL;

/* A rigctl peer that demodulates audio input (issue #621): the real requests unless a test arms it. It answers each
   request it is sent as scripted (taken, unless a test scripts a refusal or a lost reply), runs what it took or what a
   lost reply may have left it on, and is sent nothing for a request the client knows it runs, as the client's record of
   the peer skips one (dsd_rigctl.c). */
enum { RIGCTL_FAKE_TAKE = 0, RIGCTL_FAKE_REFUSE, RIGCTL_FAKE_LOSE };

static int g_rigctl_fake_armed = 0;
static int g_rigctl_fake_answers[4];
static int g_rigctl_fake_answer_count = 0;
static int g_rigctl_fake_calls = 0;     /* requests asked of the client */
static int g_rigctl_fake_sent = 0;      /* requests sent to the peer */
static int g_rigctl_fake_unguarded = 0; /* requests sent outside the P25 SM tick guard */
static int g_rigctl_fake_kind = 0;      /* the demodulator and passband the peer runs */
static int g_rigctl_fake_bw = 0;
static int g_rigctl_fake_known = 1;      /* whether the client knows what the peer runs */
static int g_rigctl_fake_last_kind = -1; /* the last request sent */
static int g_rigctl_fake_last_bw = -1;

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol name.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
bool __real_SetScanRowModulation(dsd_socket_t sockfd, int kind, int bandwidth);
bool __wrap_SetScanRowModulation(dsd_socket_t sockfd, int kind, int bandwidth);
bool __real_SetModulationKind(dsd_socket_t sockfd, int kind, int bandwidth);
bool __wrap_SetModulationKind(dsd_socket_t sockfd, int kind, int bandwidth);
int __real_svc_tcp_connect_audio(dsd_opts* opts, dsd_state* state, const char* host, int port);
int __wrap_svc_tcp_connect_audio(dsd_opts* opts, dsd_state* state, const char* host, int port);
int __real_svc_rigctl_connect(dsd_opts* opts, const dsd_state* state, const char* host, int port);
int __wrap_svc_rigctl_connect(dsd_opts* opts, const dsd_state* state, const char* host, int port);
int __real_openAudioInput(dsd_opts* opts);
int __wrap_openAudioInput(dsd_opts* opts);
int __real_dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts);
int __wrap_dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts);
int __wrap_io_control_set_freq(dsd_opts* opts, const dsd_state* state, long int freq);
dsd_trunk_tune_result __wrap_dsd_trunk_tuning_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq,
                                                              int ted_sps, uint64_t* out_request_id);
int __real_dsd_tg_policy_call_skip_arm(dsd_state* state, uint32_t id, uint32_t src, int fallback, double now_mono_s);
int __wrap_dsd_tg_policy_call_skip_arm(dsd_state* state, uint32_t id, uint32_t src, int fallback, double now_mono_s);

int
__wrap_dsd_tg_policy_call_skip_arm(dsd_state* state, uint32_t id, uint32_t src, int fallback, double now_mono_s) {
    return g_skip_arm_refused ? -1 : __real_dsd_tg_policy_call_skip_arm(state, id, src, fallback, now_mono_s);
}

/* An armed connect succeeds or fails without a socket; a success leaves the options as the
   real service's input switch does (issue #634): TCP input on the requested endpoint, named for it, a new stream. */
int
__wrap_svc_tcp_connect_audio(dsd_opts* opts, dsd_state* state, const char* host, int port) {
    if (!g_tcp_connect_stub_armed) {
        return __real_svc_tcp_connect_audio(opts, state, host, port);
    }
    g_tcp_connect_calls++;
    if (g_tcp_connect_stub_rc == DSD_AUDIO_INPUT_SWITCHED && opts && host) {
        char next_host[sizeof opts->tcp_hostname];
        DSD_SNPRINTF(next_host, sizeof next_host, "%s", host);
        DSD_SNPRINTF(opts->tcp_hostname, sizeof opts->tcp_hostname, "%s", next_host);
        opts->tcp_portno = port;
        opts->audio_in_type = AUDIO_IN_TCP;
        if (dsd_opts_audio_in_dev_is_radio_spec(opts->audio_in_dev)) {
            DSD_SNPRINTF(opts->radio_in_dev, sizeof opts->radio_in_dev, "%s", opts->audio_in_dev);
        }
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "tcp:%s:%d", next_host, port);
        dsd_opts_reset_pcm_input_state(opts);
    }
    return g_tcp_connect_stub_rc;
}

/* An armed connect succeeds or fails without a socket; a success leaves the options as the real service does: rigctl
   on over a new socket to the requested endpoint. */
int
__wrap_svc_rigctl_connect(dsd_opts* opts, const dsd_state* state, const char* host, int port) {
    if (!g_rigctl_connect_stub_armed) {
        return __real_svc_rigctl_connect(opts, state, host, port);
    }
    g_rigctl_connect_calls++;
    DSD_SNPRINTF(g_rigctl_connect_host, sizeof g_rigctl_connect_host, "%s", host ? host : "");
    g_rigctl_connect_port = port;
    g_rigctl_connect_state = state;
    if (g_rigctl_connect_stub_rc == 0 && opts && host) {
        DSD_SNPRINTF(opts->rigctlhostname, sizeof opts->rigctlhostname, "%s", host);
        opts->rigctlportno = port;
        opts->rigctl_sockfd = (dsd_socket_t)42;
        opts->use_rigctl = 1;
    }
    return g_rigctl_connect_stub_rc;
}

int
__wrap_openAudioInput(dsd_opts* opts) {
    if (!g_open_audio_input_stub_armed) {
        return __real_openAudioInput(opts);
    }
    g_open_audio_input_calls++;
    if (g_open_audio_input_stub_rc == 0) {
        /* A Pulse open that succeeds is a new stream, as the real one notes (CORE_AUDIO_OPEN_INPUT). */
        dsd_opts_note_pcm_stream(opts);
    }
    return g_open_audio_input_stub_rc;
}

/* The output's follow of the input policy, failing on demand (issue #634). */
int
__wrap_dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts) {
    g_reconfigure_guarded = !p25_sm_tick_guard_try_enter();
    if (!g_reconfigure_guarded) {
        p25_sm_tick_guard_leave();
    }
    if (g_reconfigure_output_fails) {
        return -1;
    }
    return __real_dsd_audio_reconfigure_output_for_input_policy(opts);
}

int
__wrap_io_control_set_freq(dsd_opts* opts, const dsd_state* state, long int freq) {
    (void)opts;
    (void)state;
    g_io_control_tune_calls++;
    g_io_control_tune_freq = freq;
    return g_io_control_tune_result;
}

dsd_trunk_tune_result
__wrap_dsd_trunk_tuning_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                        uint64_t* out_request_id) {
    (void)opts;
    if (out_request_id) {
        *out_request_id = 0U;
    }
    g_cc_tune_calls++;
    g_cc_tune_freq = freq;
    g_cc_tune_ted_sps = ted_sps;
    g_cc_profile_at_tune = state ? state->sps_hunt_idx : -1;
    return g_cc_tune_result;
}

static bool
rigctl_fake_request(int kind, int bandwidth) {
    g_rigctl_fake_calls++;
    if (g_rigctl_fake_known && g_rigctl_fake_kind == kind && g_rigctl_fake_bw == bandwidth) {
        return true;
    }
    const int answer =
        g_rigctl_fake_sent < g_rigctl_fake_answer_count ? g_rigctl_fake_answers[g_rigctl_fake_sent] : RIGCTL_FAKE_TAKE;
    g_rigctl_fake_sent++;
    g_rigctl_fake_last_kind = kind;
    g_rigctl_fake_last_bw = bandwidth;
    if (p25_sm_tick_guard_try_enter()) {
        p25_sm_tick_guard_leave();
        g_rigctl_fake_unguarded++;
    }
    if (answer == RIGCTL_FAKE_REFUSE) {
        return false;
    }
    g_rigctl_fake_kind = kind;
    g_rigctl_fake_bw = bandwidth;
    g_rigctl_fake_known = answer == RIGCTL_FAKE_TAKE;
    return answer == RIGCTL_FAKE_TAKE;
}

bool
__wrap_SetScanRowModulation(dsd_socket_t sockfd, int kind, int bandwidth) {
    if (!g_rigctl_fake_armed) {
        return __real_SetScanRowModulation(sockfd, kind, bandwidth);
    }
    return rigctl_fake_request(kind, bandwidth);
}

bool
__wrap_SetModulationKind(dsd_socket_t sockfd, int kind, int bandwidth) {
    if (!g_rigctl_fake_armed) {
        return __real_SetModulationKind(sockfd, kind, bandwidth);
    }
    return rigctl_fake_request(kind, bandwidth);
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

/* Arm the fake rigctl peer on socket 5 of @p opts (UDP audio input): it runs @p kind at @p bandwidth, as the client
   knows, and takes every request. */
static void
arm_rigctl_fake(dsd_opts* opts, int kind, int bandwidth) {
    opts->audio_in_type = AUDIO_IN_UDP;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = (dsd_socket_t)5;
    g_rigctl_fake_armed = 1;
    g_rigctl_fake_kind = kind;
    g_rigctl_fake_bw = bandwidth;
    g_rigctl_fake_known = 1;
    g_rigctl_fake_answer_count = 0;
    g_rigctl_fake_calls = 0;
    g_rigctl_fake_sent = 0;
    g_rigctl_fake_unguarded = 0;
    g_rigctl_fake_last_kind = -1;
    g_rigctl_fake_last_bw = -1;
}

/* The answers to the next requests sent, from now: @p first, then @p second, then taken. Counts start over. */
static void
script_rigctl_fake(int first, int second) {
    g_rigctl_fake_answers[0] = first;
    g_rigctl_fake_answers[1] = second;
    g_rigctl_fake_answer_count = 2;
    g_rigctl_fake_calls = 0;
    g_rigctl_fake_sent = 0;
    g_rigctl_fake_unguarded = 0;
}

static void
disarm_rigctl_fake(dsd_opts* opts) {
    g_rigctl_fake_armed = 0;
    g_rigctl_fake_answer_count = 0;
    opts->use_rigctl = 0;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
}

static void
reset_io_control_tune_stub(int result) {
    g_io_control_tune_result = result;
    g_io_control_tune_calls = 0;
    g_io_control_tune_freq = 0;
}

static void
arm_tcp_connect_stub(int armed, int rc) {
    g_tcp_connect_stub_armed = armed;
    g_tcp_connect_stub_rc = rc;
    g_tcp_connect_calls = 0;
}

static void
arm_rigctl_connect_stub(int armed, int rc) {
    g_rigctl_connect_stub_armed = armed;
    g_rigctl_connect_stub_rc = rc;
    g_rigctl_connect_calls = 0;
    g_rigctl_connect_host[0] = '\0';
    g_rigctl_connect_port = 0;
    g_rigctl_connect_state = NULL;
}

static void
arm_open_audio_input_stub(int armed, int rc) {
    g_open_audio_input_stub_armed = armed;
    g_open_audio_input_stub_rc = rc;
    g_open_audio_input_calls = 0;
}

static void
reset_cc_tune_stub(dsd_trunk_tune_result result) {
    g_cc_tune_result = result;
    g_cc_tune_calls = 0;
    g_cc_tune_freq = 0;
    g_cc_tune_ted_sps = 0;
    g_cc_profile_at_tune = -1;
}
#endif

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u64(const char* tag, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %llu want %llu\n", tag, (unsigned long long)got, (unsigned long long)want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: expectation failed\n", tag);
        return 1;
    }
    return 0;
}

/* A tone the receiver locked before the command, as the analog tap publishes it. */
static void
seed_received_tone(dsd_state* state) {
    state->analog_rx.carrier_open = 1;
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    state->analog_rx.ctcss_tenths_hz = 1000;
}

/* A DCS code the receiver locked before the command (issue #523): D023N. */
static void
seed_received_code(dsd_state* state) {
    state->analog_rx.carrier_open = 1;
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_DCS;
    state->analog_rx.dcs_code = 023;
    state->analog_rx.dcs_inverted = 0;
}

static int
expect_received_tone_cleared(const char* tag, const dsd_state* state, uint32_t seeded_generation) {
    const int cleared = state->analog_rx.tone_state != DSD_ANALOG_TONE_STATE_LOCKED
                        && state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE
                        && state->analog_rx.ctcss_tenths_hz == 0 && state->analog_rx.dcs_code == 0
                        && state->analog_rx.dcs_inverted == 0 && state->analog_rx.carrier_open == 0
                        && state->analog_rx.generation != seeded_generation;
    return expect_true(tag, cleared);
}

/* The tone seed_received_tone() published is still there, in the generation it was seeded in. */
static int
expect_received_tone_kept(const char* tag, const dsd_state* state, uint32_t seeded_generation) {
    const int kept = state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED
                     && state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_CTCSS
                     && state->analog_rx.ctcss_tenths_hz == 1000 && state->analog_rx.carrier_open == 1
                     && state->analog_rx.generation == seeded_generation;
    return expect_true(tag, kept);
}

static int
enc_lockout_inert(const dsd_state* state) {
    return state != NULL && dsd_enc_lockout_active_count(state) == 0;
}

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (!got || !want || strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got \"%s\" want \"%s\"\n", tag, got ? got : "(null)", want ? want : "(null)");
        return 1;
    }
    return 0;
}

static int
expect_contains(const char* tag, const char* haystack, const char* needle) {
    if (!haystack || !needle || !strstr(haystack, needle)) {
        DSD_FPRINTF(stderr, "%s: \"%s\" does not contain \"%s\"\n", tag, haystack ? haystack : "(null)",
                    needle ? needle : "(null)");
        return 1;
    }
    return 0;
}

static int
write_file_bytes(const char* path, const void* data, size_t n) {
    FILE* f = dsd_fopen_private(path, "wb");
    if (!f) {
        DSD_FPRINTF(stderr, "failed to create %s\n", path);
        return 1;
    }
    int rc = 0;
    if (n > 0U && fwrite(data, 1U, n, f) != n) {
        DSD_FPRINTF(stderr, "failed to write %s\n", path);
        rc = 1;
    }
    if (fclose(f) != 0) {
        DSD_FPRINTF(stderr, "failed to close %s\n", path);
        rc = 1;
    }
    return rc;
}

/*
 * Input switches (issue #634) run for real in these tests: files they write into a temporary working directory, UDP
 * bound on the loopback through the real backend, and Pulse through the openAudioInput() wrap where the toolchain has
 * one.
 */
static void
put_le16(unsigned char* p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xFFU);
    p[1] = (unsigned char)((v >> 8) & 0xFFU);
}

static void
put_le32(unsigned char* p, uint32_t v) {
    put_le16(p, (uint16_t)(v & 0xFFFFU));
    put_le16(p + 2, (uint16_t)(v >> 16));
}

/* A mono PCM16 WAV of @p samples silent samples at @p rate_hz. */
static int
write_pcm16_wav(const char* path, uint32_t rate_hz, uint32_t samples) {
    unsigned char buf[44U + 2U * 64U];
    if (samples > 64U) {
        return 1;
    }
    DSD_MEMSET(buf, 0, sizeof buf);
    const uint32_t data_bytes = samples * 2U;
    DSD_MEMCPY(buf, "RIFF", 4);
    put_le32(buf + 4, 36U + data_bytes);
    DSD_MEMCPY(buf + 8, "WAVEfmt ", 8);
    put_le32(buf + 16, 16U);
    put_le16(buf + 20, 1U);
    put_le16(buf + 22, 1U);
    put_le32(buf + 24, rate_hz);
    put_le32(buf + 28, rate_hz * 2U);
    put_le16(buf + 32, 2U);
    put_le16(buf + 34, 16U);
    DSD_MEMCPY(buf + 36, "data", 4);
    put_le32(buf + 40, data_bytes);
    return write_file_bytes(path, buf, 44U + data_bytes);
}

static void
install_udp_input_hooks(void) {
    dsd_net_audio_input_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.udp_start = udp_input_start;
    hooks.udp_stop = udp_input_stop;
    hooks.udp_read_sample = udp_input_read_sample;
    hooks.udp_read_sample_wait = udp_input_read_sample_wait;
    dsd_net_audio_input_hooks_set(hooks);
}

static void
clear_net_audio_input_hooks(void) {
    dsd_net_audio_input_hooks none;
    DSD_MEMSET(&none, 0, sizeof none);
    dsd_net_audio_input_hooks_set(none);
}

/* A UDP socket bound on a loopback port the system picked, which the caller closes. */
static dsd_socket_t
bind_loopback_udp(int* out_port) {
    *out_port = -1;
    dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == DSD_INVALID_SOCKET) {
        return DSD_INVALID_SOCKET;
    }
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct sockaddr_in bound;
    DSD_MEMSET(&bound, 0, sizeof bound);
#if DSD_PLATFORM_WIN_NATIVE
    int bound_len = (int)sizeof bound;
#else
    socklen_t bound_len = (socklen_t)sizeof bound;
#endif
    if (dsd_socket_bind(sock, (struct sockaddr*)&addr, sizeof addr) != 0
        || getsockname(sock, (struct sockaddr*)&bound, &bound_len) != 0) {
        dsd_socket_close(sock);
        return DSD_INVALID_SOCKET;
    }
    *out_port = (int)ntohs(bound.sin_port);
    return sock;
}

/* A loopback UDP port nothing holds: bound by the system and let go again. */
static int
free_loopback_udp_port(void) {
    int port = -1;
    const dsd_socket_t sock = bind_loopback_udp(&port);
    if (sock != DSD_INVALID_SOCKET) {
        dsd_socket_close(sock);
    }
    return port;
}

static void
init_test_context(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    initOpts(opts);
    initState(state);
    state->cli_argc_effective = 0;
    state->cli_argv = NULL;
    dsd_app_frontend_runtime_start(opts, state);
}

/*
 * DECODE_MODE_SET opens the sink the new mode writes to when the session plays to a local audio device, which is
 * what initOpts() selects. Cases that change the decode mode play to the null output instead, so no test opens a
 * host audio stream whether or not the ensure helpers are link-time wrapped on this toolchain. Where they are wrapped,
 * main() fails the run if any case reaches them off the null output.
 */
static void
init_decode_mode_context(dsd_opts* opts, dsd_state* state) {
    init_test_context(opts, state);
    opts->audio_out_type = 9;
}

static int
post_empty(int id) {
    return dsd_app_command_submit(id, NULL, 0U);
}

static int
post_i32(int id, int32_t value) {
    return dsd_app_command_submit(id, &value, sizeof(value));
}

static int
post_u32(int id, uint32_t value) {
    return dsd_app_command_submit(id, &value, sizeof(value));
}

static int
post_u64(int id, uint64_t value) {
    return dsd_app_command_submit(id, &value, sizeof(value));
}

static int
post_double(int id, double value) {
    return dsd_app_command_submit(id, &value, sizeof(value));
}

static int
post_float(int id, float value) {
    return dsd_app_command_submit(id, &value, sizeof(value));
}

static int
post_string(int id, const char* value) {
    return dsd_app_command_submit(id, value, strlen(value) + 1U);
}

static int
post_host_port(int id, const char* host, int32_t port) {
    uint8_t payload[256 + sizeof(port)];
    DSD_MEMSET(payload, 0, sizeof(payload));
    DSD_SNPRINTF((char*)payload, 256U, "%s", host);
    DSD_MEMCPY(payload + 256U, &port, sizeof(port));
    return dsd_app_command_submit(id, payload, sizeof(payload));
}

static int
post_hytera_key(uint64_t h, uint64_t k1, uint64_t k2, uint64_t k3, uint64_t k4) {
    struct {
        uint64_t H;
        uint64_t K1;
        uint64_t K2;
        uint64_t K3;
        uint64_t K4;
    } payload;

    payload.H = h;
    payload.K1 = k1;
    payload.K2 = k2;
    payload.K3 = k3;
    payload.K4 = k4;
    return dsd_app_command_submit(DSD_APP_CMD_KEY_HYTERA_SET, &payload, sizeof(payload));
}

static int
post_aes_key(uint64_t k1, uint64_t k2, uint64_t k3, uint64_t k4) {
    struct {
        uint64_t K1;
        uint64_t K2;
        uint64_t K3;
        uint64_t K4;
    } payload;

    payload.K1 = k1;
    payload.K2 = k2;
    payload.K3 = k3;
    payload.K4 = k4;
    return dsd_app_command_submit(DSD_APP_CMD_KEY_AES_SET, &payload, sizeof(payload));
}

static int
post_p2_params(uint64_t wacn, uint64_t sysid, uint64_t cc) {
    struct {
        uint64_t w;
        uint64_t s;
        uint64_t n;
    } payload;

    payload.w = wacn;
    payload.s = sysid;
    payload.n = cc;
    return dsd_app_command_submit(DSD_APP_CMD_P25_P2_PARAMS_SET, &payload, sizeof(payload));
}

static int
post_call_alert_events(uint8_t events) {
    return dsd_app_command_submit(DSD_APP_CMD_CALL_ALERT_EVENTS_SET, &events, sizeof(events));
}

static int
test_command_api(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    /* The UDP input command binds now (issue #634): a loopback port nothing holds, not one a parallel test may. */
    install_udp_input_hooks();
    const int udp_port = free_loopback_udp_port();
    rc |= expect_true("typed udp input free port", udp_port > 0);

    rc |= expect_int("typed action rejects setter", dsd_app_command_action(DSD_APP_CMD_GAIN_SET), -1);
    rc |= expect_int("typed i32 rejects action", dsd_app_command_set_i32(DSD_APP_CMD_TOGGLE_MUTE, 1), -1);
    rc |= expect_int("typed rtl frequency rejects i32", dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_FREQ, 1), -1);
    rc |= expect_int("typed string rejects null", dsd_app_command_set_string(DSD_APP_CMD_INPUT_WAV_SET, NULL), -1);
    rc |= expect_int("typed endpoint rejects action",
                     dsd_app_command_set_endpoint(DSD_APP_CMD_TOGGLE_MUTE, "127.0.0.1", -1), -1);
    rc |= expect_int("typed udp input rejects null", dsd_app_command_set_endpoint(DSD_APP_CMD_UDP_INPUT_CFG, NULL, 0),
                     -1);
    rc |= expect_int("typed p25 payload rejects null", dsd_app_command_set_p25_p2_params(NULL), -1);
    rc |= expect_int("typed hytera payload rejects null", dsd_app_command_set_hytera_key(NULL), -1);
    rc |= expect_int("typed aes payload rejects null", dsd_app_command_set_aes_key(NULL), -1);
    rc |= expect_int("typed dsp payload rejects null", dsd_app_command_dsp_op(NULL), -1);
    rc |= expect_int("typed config payload rejects null", dsd_app_command_apply_config(NULL), -1);

    dsd_app_p25_p2_params_payload p2 = {0xABCDEU, 0x123U, 0x456U};
    dsd_app_hytera_key_payload hytera = {0xAU, 1U, 2U, 3U, 4U};
    dsd_app_aes_key_payload aes = {9U, 10U, 11U, 12U};
    dsd_app_dsp_payload dsp = {0};
    (void)dsd_enc_lockout_note(&state, 2468U, 1, 0x84, 0x1234);

    rc |= expect_int("typed action posts", dsd_app_command_action(DSD_APP_CMD_UI_SHOW_CHANNELS_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |=
        expect_int("typed gain posts", dsd_app_command_set_i32(DSD_APP_CMD_GAIN_SET, 5), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed gain coalesces to latest", dsd_app_command_set_i32(DSD_APP_CMD_GAIN_SET, 9),
                     DSD_APP_COMMAND_SUBMIT_COALESCED);
    rc |= expect_int("typed u8 posts", dsd_app_command_set_u8(DSD_APP_CMD_CALL_ALERT_EVENTS_SET, 3U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed u32 posts", dsd_app_command_set_u32(DSD_APP_CMD_TG_HOLD_SET, 2468U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed rtl frequency u32 posts", dsd_app_command_set_u32(DSD_APP_CMD_RTL_SET_FREQ, 3000000000U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed u64 posts", dsd_app_command_set_u64(DSD_APP_CMD_KEY_RC4DES_SET, 0x55U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed double posts", dsd_app_command_set_double(DSD_APP_CMD_HANGTIME_SET, 3.5),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed float posts", dsd_app_command_set_float(DSD_APP_CMD_CONST_GATE_DELTA, 1.0f),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed string posts", dsd_app_command_set_string(DSD_APP_CMD_M17_USER_DATA_SET, "0,DST,SRC"),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed endpoint posts",
                     dsd_app_command_set_endpoint(DSD_APP_CMD_RIGCTL_CONNECT_CFG, "127.0.0.1", -1),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed udp input endpoint posts",
                     dsd_app_command_set_endpoint(DSD_APP_CMD_UDP_INPUT_CFG, "127.0.0.1", udp_port),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed p25 payload posts", dsd_app_command_set_p25_p2_params(&p2), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed hytera payload posts", dsd_app_command_set_hytera_key(&hytera),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed aes payload posts", dsd_app_command_set_aes_key(&aes), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed dsp payload posts", dsd_app_command_dsp_op(&dsp), DSD_APP_COMMAND_SUBMIT_QUEUED);

    rc |= expect_int("typed commands applied with coalescing", dsd_app_drain_cmds(&opts, &state), 15);
    rc |= expect_int("typed action toggled channels", opts.frontend_display.show_channels, 1);
    rc |= expect_int("typed gain applied latest", (int)opts.audio_gain, 9);
    rc |= expect_str("typed udp input bind copied", opts.udp_in_bindaddr, "127.0.0.1");
    rc |= expect_int("typed udp input port copied", opts.udp_in_portno, udp_port);
    rc |= expect_u64("typed tg hold set", state.tg_hold, 2468ULL);
    rc |= expect_u64("typed rc4des key set", state.R, 0x55ULL);
    rc |= expect_true("typed hangtime set", opts.trunk_hangtime > 3.49 && opts.trunk_hangtime < 3.51);
    rc |= expect_str("typed m17 payload copied", state.m17dat, "0,DST,SRC");
    rc |= expect_u64("typed p2 wacn set", state.p2_wacn, 0xABCDEULL);
    rc |= expect_u64("typed p2 sysid set", state.p2_sysid, 0x123ULL);
    rc |= expect_u64("typed p2 cc set", state.p2_cc, 0x456ULL);
    rc |= expect_u64("typed aes key loaded", state.A1[0], 9ULL);
    rc |= expect_int("typed aes key load flag", state.aes_key_loaded[0], 1);
    rc |= expect_int("typed canonical aes key byte 7", state.aes_key[7], 9);
    rc |= expect_int("typed canonical aes key byte 15", state.aes_key[15], 10);
    rc |= expect_true("manual RC4/AES key changes invalidate enc lockouts", enc_lockout_inert(&state));
    rc |= expect_true("key changes retain stale entries for re-verification",
                      dsd_enc_lockout_lookup(&state, 2468U, 1, NULL));

    (void)dsd_enc_lockout_note(&state, 9753U, 0, 0xAA, 0x0002);
    rc |=
        expect_int("standalone AES key change posts", dsd_app_command_set_aes_key(&aes), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("standalone AES key change applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("standalone AES key change invalidates enc lockouts", enc_lockout_inert(&state));

    (void)dsd_enc_lockout_note(&state, 8642U, 1, 0x81, 0x0003);
    rc |= expect_int("standalone RC4/DES key change posts", dsd_app_command_set_u64(DSD_APP_CMD_KEY_RC4DES_SET, 0xAAU),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("standalone RC4/DES key change applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("standalone RC4/DES key change invalidates enc lockouts", enc_lockout_inert(&state));

    (void)dsd_enc_lockout_note(&state, 8642U, 1, 0x81, 0x0003);
    rc |= expect_true("re-noted target locks at the new epoch", !enc_lockout_inert(&state));
    rc |= expect_int("enc lockout purge posts", dsd_app_command_action(DSD_APP_CMD_ENC_LOCKOUT_CLEAR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("enc lockout purge applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("enc lockout purge drops every entry", !dsd_enc_lockout_lookup(&state, 8642U, 1, NULL));

    closeAudioInDevice(&opts);
    clear_net_audio_input_hooks();
    freeState(&state);
    return rc;
}

/*
 * Spectrum tap-to-tune shares the queue with the settings tune but must never
 * share a coalescing slot with it: a tap storm has to collapse onto its own
 * newest target while a pending settings tune still lands.
 *
 * Trunking is left on here so draining cannot reach the tuner; the gate itself
 * is asserted against the wrapped tune stub further down.
 */
static int
test_manual_tune_queue_semantics(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    opts.trunk_enable = 1;

    rc |= expect_int("manual tune rejects i32", dsd_app_command_set_i32(DSD_APP_CMD_MANUAL_TUNE, 1), -1);
    rc |= expect_int("manual tune u32 posts", dsd_app_command_set_u32(DSD_APP_CMD_MANUAL_TUNE, 3000000000U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tap storm coalesces onto the newest target",
                     dsd_app_command_set_u32(DSD_APP_CMD_MANUAL_TUNE, 851000000U), DSD_APP_COMMAND_SUBMIT_COALESCED);
    rc |= expect_int("a settings tune never absorbs a tap",
                     dsd_app_command_set_u32(DSD_APP_CMD_RTL_SET_FREQ, 852000000U), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("a tap never absorbs a settings tune",
                     dsd_app_command_set_u32(DSD_APP_CMD_MANUAL_TUNE, 853000000U), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("distinct tune entries drain", dsd_app_drain_cmds(&opts, &state), 3);

    /* An undersized payload is consumed but must reach no handler at all — with
     * trunking on, even the refusal toast would prove it got that far. */
    state.ui_msg[0] = '\0';
    uint8_t short_payload = 0xFFU;
    dsd_app_command_submit(DSD_APP_CMD_MANUAL_TUNE, &short_payload, sizeof(short_payload));
    rc |= expect_int("short manual tune payload drains", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("short manual tune payload is ignored", state.ui_msg, "");

    freeState(&state);
    return rc;
}

static int
test_setter_coalescing_preserves_fifo_boundaries(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);

    rc |= expect_int("fifo first gain queued", dsd_app_command_set_i32(DSD_APP_CMD_GAIN_SET, 5),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("fifo gain delta queued", dsd_app_command_set_i32(DSD_APP_CMD_GAIN_DELTA, +1),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("fifo second gain queued", dsd_app_command_set_i32(DSD_APP_CMD_GAIN_SET, 11),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);

    rc |= expect_int("fifo-separated setters drain independently", dsd_app_drain_cmds(&opts, &state), 3);
    rc |= expect_int("fifo-separated setters preserve final gain", (int)opts.audio_gain, 11);
    freeState(&state);
    return rc;
}

static int
test_visibility_and_queue_overflow(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);

    rc |= expect_int("initial queue empty", dsd_app_drain_cmds(&opts, &state), 0);

    post_empty(DSD_APP_CMD_UI_SHOW_DSP_PANEL_TOGGLE);
    post_empty(DSD_APP_CMD_UI_SHOW_P25_METRICS_TOGGLE);
    post_empty(DSD_APP_CMD_UI_SHOW_P25_AFFIL_TOGGLE);
    post_empty(DSD_APP_CMD_UI_SHOW_P25_NEIGHBORS_TOGGLE);
    post_empty(DSD_APP_CMD_UI_SHOW_P25_IDEN_TOGGLE);
    post_empty(DSD_APP_CMD_UI_SHOW_P25_CCC_TOGGLE);
    post_empty(DSD_APP_CMD_UI_SHOW_CHANNELS_TOGGLE);
    post_empty(DSD_APP_CMD_UI_SHOW_P25_CALLSIGN_TOGGLE);
    rc |= expect_int("visibility commands applied", dsd_app_drain_cmds(&opts, &state), 8);
    rc |= expect_int("dsp panel visible", opts.frontend_display.show_dsp_panel, 1);
    rc |= expect_int("p25 metrics visible", opts.frontend_display.show_p25_metrics, 1);
    rc |= expect_int("p25 affiliations visible", opts.frontend_display.show_p25_affiliations, 1);
    rc |= expect_int("p25 neighbors visible", opts.frontend_display.show_p25_neighbors, 1);
    rc |= expect_int("p25 iden visible", opts.frontend_display.show_p25_iden_plan, 1);
    rc |= expect_int("p25 candidates visible", opts.frontend_display.show_p25_cc_candidates, 1);
    rc |= expect_int("channels visible", opts.frontend_display.show_channels, 1);
    rc |= expect_int("callsign visible", opts.frontend_display.show_p25_callsign_decode, 1);

    opts.frontend_display.show_channels = 0;
    for (int i = 0; i < 140; i++) {
        post_empty(DSD_APP_CMD_UI_SHOW_CHANNELS_TOGGLE);
    }
    rc |= expect_int("overflow keeps bounded queue depth", dsd_app_drain_cmds(&opts, &state), 127);
    rc |= expect_int("overflow drains newest visibility toggles", opts.frontend_display.show_channels, 1);

    freeState(&state);
    return rc;
}

static int
test_key_and_runtime_state_commands(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);

    opts.dmr_mute_encL = 1;
    opts.dmr_mute_encR = 1;
    state.payload_keyid = 9;
    state.payload_keyidR = 10;

    post_u32(DSD_APP_CMD_KEY_BASIC_SET, 0x12345678U);
    post_u32(DSD_APP_CMD_KEY_SCRAMBLER_SET, 0x11112222U);
    post_u64(DSD_APP_CMD_KEY_RC4DES_SET, 0x55667788ULL);
    post_hytera_key(0xAU, 1U, 2U, 0U, 0U);
    post_aes_key(3U, 4U, 5U, 6U);
    post_string(DSD_APP_CMD_M17_USER_DATA_SET, "0,DEST,SOURCE");
    post_i32(DSD_APP_CMD_RIGCTL_SET_MOD_BW, 12500);
    post_u32(DSD_APP_CMD_TG_HOLD_SET, 4567U);
    post_double(DSD_APP_CMD_HANGTIME_SET, 2.5);
    post_i32(DSD_APP_CMD_SLOT_PREF_SET, 1);
    post_i32(DSD_APP_CMD_SLOTS_ONOFF_SET, 2);
    post_p2_params(0xABCDEU, 0x123U, 0x456U);

    rc |= expect_int("key/runtime commands applied", dsd_app_drain_cmds(&opts, &state), 12);
    rc |= expect_u64("basic key loaded", state.K, 0x12345678ULL);
    rc |= expect_u64("scrambler key loaded", state.R, 0x55667788ULL);
    rc |= expect_u64("rc4des key mirror loaded", state.RR, 0x55667788ULL);
    rc |= expect_int("key mute reset left", opts.dmr_mute_encL, 0);
    rc |= expect_int("key mute reset right", opts.dmr_mute_encR, 0);
    rc |= expect_int("payload key reset left", state.payload_keyid, 0);
    rc |= expect_int("payload key reset right", state.payload_keyidR, 0);
    rc |= expect_u64("aes key loaded", state.A1[0], 3ULL);
    rc |= expect_int("aes key load flag left", state.aes_key_loaded[0], 1);
    rc |= expect_int("canonical aes key byte 7", state.aes_key[7], 3);
    rc |= expect_int("canonical aes key byte 31", state.aes_key[31], 6);
    rc |= expect_int("aes key segments left", state.aes_key_segments[0], 4);
    rc |= expect_u64("hytera state cleared by aes", state.H, 0ULL);
    rc |= expect_int("m17 user data copied", strncmp(state.m17dat, "0,DEST,SOURCE", sizeof(state.m17dat)), 0);
    rc |= expect_int("rigctl bw set", opts.setmod_bw, 12500);
    rc |= expect_u64("tg hold set", state.tg_hold, 4567ULL);
    rc |= expect_true("hangtime set", opts.trunk_hangtime > 2.49 && opts.trunk_hangtime < 2.51);
    rc |= expect_int("slot preference set", opts.slot_preference, 1);
    rc |= expect_int("slot1 disabled by mask", opts.slot1_on, 0);
    rc |= expect_int("slot2 enabled by mask", opts.slot2_on, 1);
    rc |= expect_u64("p2 wacn set", state.p2_wacn, 0xABCDEULL);
    rc |= expect_u64("p2 sysid set", state.p2_sysid, 0x123ULL);
    rc |= expect_u64("p2 cc set", state.p2_cc, 0x456ULL);

    post_i32(DSD_APP_CMD_SLOT_PREF_SET, 2);
    rc |= expect_int("slot preference auto drain count", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("slot preference auto set", opts.slot_preference, 2);
    rc |= expect_true("slot preference auto toast", strstr(state.ui_msg, "Slot preference -> Auto") != NULL);

    uint8_t short_payload = 0xFFU;
    state.K = 0x99999999U;
    state.A1[0] = 0x55U;
    post_u32(DSD_APP_CMD_KEY_BASIC_SET, 0x01020304U);
    dsd_app_command_submit(DSD_APP_CMD_KEY_AES_SET, &short_payload, sizeof(short_payload));
    post_string(DSD_APP_CMD_M17_USER_DATA_SET, "");
    rc |= expect_int("short key payload commands applied", dsd_app_drain_cmds(&opts, &state), 3);
    rc |= expect_u64("basic key still updates before short aes", state.K, 0x01020304ULL);
    rc |= expect_u64("short aes ignored", state.A1[0], 0x55ULL);
    rc |= expect_str("empty m17 payload clears value", state.m17dat, "");

    freeState(&state);
    return rc;
}

static int
test_file_network_and_import_commands(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    const char* symbol_out = "ui_cmd_queue_symbols_out.bin";
    const char* symbol_in = "ui_cmd_queue_symbols_in.bin";
    const char* missing_csv = "ui_cmd_queue_missing.csv";
    const char* key_csv = "ui_cmd_queue_keys.csv";
    const unsigned char symbol_data[] = {0x12U, 0x34U, 0x56U, 0x78U};
    static const unsigned char key_data[] = "Key ID,Key\n1,12345\n";

    // DSP_OUT_SET creates ./DSP; run from a temp dir so no DSP folder is left where the binary was launched.
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_ui_cmd_queue_files") != 0) {
        DSD_FPRINTF(stderr, "temp working directory setup failed: %s\n", strerror(errno));
        return 1;
    }

    init_test_context(&opts, &state);

    rc |= write_file_bytes(symbol_in, symbol_data, sizeof(symbol_data));
    rc |= write_file_bytes(key_csv, key_data, sizeof(key_data) - 1U);

    post_string(DSD_APP_CMD_DSP_OUT_SET, "stream.float");
    post_string(DSD_APP_CMD_SYMCAP_OPEN, symbol_out);
    post_string(DSD_APP_CMD_SYMBOL_IN_OPEN, symbol_in);
    rc |= expect_int("file command group applied", dsd_app_drain_cmds(&opts, &state), 3);
    rc |= expect_int("dsp output enabled", opts.use_dsp_output, 1);
    rc |= expect_str("dsp output path", opts.dsp_out_file, "./DSP/stream.float");
    rc |= expect_str("symbol output path", opts.symbol_out_file, symbol_out);
    rc |= expect_true("symbol output opened", opts.symbol_out_f != NULL);
    rc |= expect_true("symbol input opened", opts.symbolfile != NULL);
    rc |= expect_int("symbol input type selected", opts.audio_in_type, AUDIO_IN_SYMBOL_BIN);
    rc |= expect_int("symbol replay format reset", state.symbol_replay_format, 0);
    rc |= expect_int("symbol replay header reset", state.symbol_replay_header_checked, 0);

    if (opts.symbol_out_f) {
        fclose(opts.symbol_out_f);
        opts.symbol_out_f = NULL;
    }
    if (opts.symbolfile) {
        fclose(opts.symbolfile);
        opts.symbolfile = NULL;
    }

    post_string(DSD_APP_CMD_PULSE_OUT_SET, "sink0");
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    arm_open_audio_input_stub(1, 0);
#endif
    post_string(DSD_APP_CMD_PULSE_IN_SET, "source0");
    rc |= expect_int("pulse command group applied", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_str("pulse output selected", opts.audio_out_dev, "pulse");
    rc |= expect_int("pulse output type selected", opts.audio_out_type, 0);
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    /* The device opens, under the name a saved config reopens (issue #634). */
    rc |= expect_int("pulse input opened", g_open_audio_input_calls, 1);
    rc |= expect_str("pulse input selected", opts.audio_in_dev, "pulse:source0");
    rc |= expect_str("pulse input device", opts.pa_input_idx, "source0");
    rc |= expect_int("pulse input type selected", opts.audio_in_type, AUDIO_IN_PULSE);
    arm_open_audio_input_stub(0, 0);
#else
    /* The host may have no capture device by that name: either it opened, or the input is the one before. */
    if (opts.audio_in_type == AUDIO_IN_PULSE) {
        rc |= expect_str("pulse input selected", opts.audio_in_dev, "pulse:source0");
    } else {
        rc |= expect_int("pulse input failure keeps the input", opts.audio_in_type, AUDIO_IN_SYMBOL_BIN);
        rc |= expect_contains("pulse input failure toast", state.ui_msg, "Failed: Pulse input");
    }
#endif

    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "unchanged");
    opts.audio_in_type = AUDIO_IN_STDIN;
    dsd_app_command_submit(DSD_APP_CMD_UDP_INPUT_CFG, NULL, 0U);
    rc |= expect_int("malformed udp input command applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("malformed udp input leaves device", opts.audio_in_dev, "unchanged");
    rc |= expect_int("malformed udp input leaves type", opts.audio_in_type, AUDIO_IN_STDIN);

    post_host_port(DSD_APP_CMD_UDP_OUT_CFG, "127.0.0.1", 0);
    post_host_port(DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG, "127.0.0.1", -1);
    post_host_port(DSD_APP_CMD_RIGCTL_CONNECT_CFG, "127.0.0.1", -1);
    rc |= expect_int("network failure command group applied", dsd_app_drain_cmds(&opts, &state), 3);
    rc |= expect_contains("rigctl failure toast", state.ui_msg, "Rigctl connect failed");
    rc |= expect_int("rigctl remains disabled", opts.use_rigctl, 0);

    post_empty(DSD_APP_CMD_LRRP_SET_DSDP);
    rc |= expect_int("lrrp dsdp applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("lrrp dsdp path", opts.lrrp_out_file, "DSDPlus.LRRP");
    rc |= expect_int("lrrp dsdp enabled", opts.lrrp_file_output, 1);

    post_string(DSD_APP_CMD_LRRP_SET_CUSTOM, "positions.csv");
    rc |= expect_int("lrrp custom applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("lrrp custom path", opts.lrrp_out_file, "positions.csv");

    post_string(DSD_APP_CMD_IMPORT_CHANNEL_MAP, missing_csv);
    post_string(DSD_APP_CMD_IMPORT_GROUP_LIST, missing_csv);
    post_string(DSD_APP_CMD_IMPORT_KEYS_DEC, missing_csv);
    post_string(DSD_APP_CMD_IMPORT_KEYS_HEX, missing_csv);
    rc |= expect_int("import failure group applied", dsd_app_drain_cmds(&opts, &state), 4);
    rc |= expect_str("failed channel import keeps path", opts.chan_in_file, "");
    rc |= expect_str("failed group import keeps path", opts.group_in_file, "");
    rc |= expect_str("key import path copied", opts.key_in_file, missing_csv);
    rc |= expect_contains("key import failure toast", state.ui_msg, "Failed: Keys (HEX)");

    (void)dsd_enc_lockout_note(&state, 3579U, 0, 0xAA, 0x0004);
    post_string(DSD_APP_CMD_IMPORT_KEYS_DEC, key_csv);
    rc |= expect_int("successful runtime key import applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("successful runtime key import invalidates enc lockouts", enc_lockout_inert(&state));

    post_string(DSD_APP_CMD_EVENT_LOG_SET, "events.log");
    rc |= expect_int("event log set applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("event log path set", opts.event_out_file, "events.log");
    post_empty(DSD_APP_CMD_EVENT_LOG_DISABLE);
    rc |= expect_int("event log disable applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("event log disabled", opts.event_out_file, "");

    remove(symbol_out);
    remove(symbol_in);
    remove(key_csv);
    rc |= expect_int("dsp output directory created in the working directory", dsd_test_rmdir("DSP"), 0);
    rc |= expect_int("file command temp dir restored and emptied", dsd_test_temp_cwd_leave(&cwd), 0);
    freeState(&state);
    return rc;
}

static int
test_p25_bandplan_commands(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    const char* plan_csv = "ui_cmd_queue_p25_bandplan.csv";
    const char* export_csv = "ui_cmd_queue_p25_bandplan_export.csv";
    const char* missing_csv = "ui_cmd_queue_p25_bandplan_missing.csv";
    static const unsigned char plan_data[] =
        "iden,base_hz,spacing_hz,type,tx_offset_hz,bandwidth_hz,wacn,sysid\n0,851006250,6250,1,-45000000,12500,,\n";

    remove(plan_csv);
    remove(export_csv);
    remove(missing_csv);
    init_test_context(&opts, &state);
    rc |= write_file_bytes(plan_csv, plan_data, sizeof(plan_data) - 1U);

    // A missing file fails the dry run: nothing recorded, failure toast.
    post_string(DSD_APP_CMD_IMPORT_P25_BANDPLAN, missing_csv);
    rc |= expect_int("bandplan import of missing file applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("bandplan import of missing file records no path", opts.p25_bandplan_in_file, "");
    rc |= expect_int("bandplan import of missing file loads nothing", state.p25_bandplan_row_count, 0);
    rc |= expect_contains("bandplan import failure toast", state.ui_msg, "Failed: P25 band plan import");

    // A real plan lands in the live state, records its path and seeds IDEN 0.
    post_string(DSD_APP_CMD_IMPORT_P25_BANDPLAN, plan_csv);
    rc |= expect_int("bandplan import applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("bandplan import path recorded", opts.p25_bandplan_in_file, plan_csv);
    rc |= expect_int("bandplan import stored one row", state.p25_bandplan_row_count, 1);
    rc |= expect_true("bandplan import seeded IDEN 0 base", state.p25_iden_fdma[0].base_freq == 851006250L / 5);
    rc |= expect_contains("bandplan import success toast", state.ui_msg, "Applied: P25 band plan imported");

    // Under trunk scan the import is refused (per-target p25_bandplan_csv instead).
    opts.trunk_scan_enabled = 1;
    opts.p25_bandplan_in_file[0] = '\0';
    post_string(DSD_APP_CMD_IMPORT_P25_BANDPLAN, plan_csv);
    rc |= expect_int("bandplan import under trunk scan applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("bandplan import under trunk scan records no path", opts.p25_bandplan_in_file, "");
    rc |= expect_contains("bandplan import under trunk scan toast", state.ui_msg, "Failed: P25 band plan import");
    opts.trunk_scan_enabled = 0;

    // Export writes the seeded table back out and reports the row count.
    post_string(DSD_APP_CMD_EXPORT_P25_BANDPLAN, export_csv);
    rc |= expect_int("bandplan export applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_contains("bandplan export success toast", state.ui_msg, "Applied: 1 P25 band plan row(s) exported");
    FILE* exported = dsd_fopen_existing_regular_file(export_csv, "rb");
    rc |= expect_true("bandplan export wrote the file", exported != NULL);
    if (exported) {
        fclose(exported);
    }

    // An unwritable path fails through the same handler.
    post_string(DSD_APP_CMD_EXPORT_P25_BANDPLAN, "ui_cmd_queue_no_such_dir/plan.csv");
    rc |= expect_int("bandplan export to bad path applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_contains("bandplan export failure toast", state.ui_msg, "Failed: P25 band plan export");

    remove(plan_csv);
    remove(export_csv);
    freeState(&state);
    return rc;
}

static int
test_io_and_state_commands(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    const int compact_before = opts.frontend_terminal_display.terminal_compact;

    opts.slot1_on = 1;
    opts.slot2_on = 1;
    opts.slot_preference = 0;
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frontend_display.const_gate_other = 0.05f;
    opts.frontend_display.const_gate_qpsk = 0.10f;
    opts.frontend_display.const_norm_mode = 0;
    opts.frontend_terminal_display.eye_unicode = 0;
    opts.frontend_terminal_display.eye_color = 0;
    opts.use_lpf = 0;
    opts.use_hpf = 0;
    opts.use_pbf = 0;
    opts.use_hpf_d = 0;
    opts.aggressive_framesync = 0;
    opts.call_alert_events = 0xFFU;
    opts.call_alert = 0;
    opts.p25_lcw_retune = 0;
    opts.reverse_mute = 0;
    opts.inverted_x2tdma = 0;
    opts.inverted_dmr = 0;
    opts.inverted_dpmr = 0;
    opts.inverted_m17 = 0;
    opts.m17encoder = 1;
    opts.frame_provoice = 1;
    state.ea_mode = 0;

    post_empty(DSD_APP_CMD_CONST_TOGGLE);
    post_empty(DSD_APP_CMD_CONST_NORM_TOGGLE);
    post_float(DSD_APP_CMD_CONST_GATE_DELTA, 1.0f);
    post_empty(DSD_APP_CMD_EYE_TOGGLE);
    post_empty(DSD_APP_CMD_EYE_UNICODE_TOGGLE);
    post_empty(DSD_APP_CMD_EYE_COLOR_TOGGLE);
    post_empty(DSD_APP_CMD_FSK_HIST_TOGGLE);
    post_empty(DSD_APP_CMD_SPECTRUM_TOGGLE);
    post_empty(DSD_APP_CMD_TOGGLE_COMPACT);
    post_empty(DSD_APP_CMD_SLOT1_TOGGLE);
    post_empty(DSD_APP_CMD_SLOT2_TOGGLE);
    post_empty(DSD_APP_CMD_SLOT_PREF_CYCLE);
    post_empty(DSD_APP_CMD_PAYLOAD_TOGGLE);
    post_empty(DSD_APP_CMD_P25_GA_TOGGLE);
    post_empty(DSD_APP_CMD_LPF_TOGGLE);
    post_empty(DSD_APP_CMD_HPF_TOGGLE);
    post_empty(DSD_APP_CMD_PBF_TOGGLE);
    post_empty(DSD_APP_CMD_HPF_D_TOGGLE);
    post_empty(DSD_APP_CMD_AGGR_SYNC_TOGGLE);
    post_empty(DSD_APP_CMD_CALL_ALERT_TOGGLE);
    post_call_alert_events(0U);
    post_empty(DSD_APP_CMD_LCW_RETUNE_TOGGLE);
    post_empty(DSD_APP_CMD_P25_CC_CAND_TOGGLE);
    post_empty(DSD_APP_CMD_REVERSE_MUTE_TOGGLE);
    post_empty(DSD_APP_CMD_INV_X2_TOGGLE);
    post_empty(DSD_APP_CMD_INV_DMR_TOGGLE);
    post_empty(DSD_APP_CMD_INV_DPMR_TOGGLE);
    post_empty(DSD_APP_CMD_INV_M17_TOGGLE);
    post_string(DSD_APP_CMD_INPUT_WAV_SET, "input.wav");
    post_string(DSD_APP_CMD_INPUT_SYM_STREAM_SET, "symbols.f32");
    post_empty(DSD_APP_CMD_INPUT_SET_PULSE);
    post_host_port(DSD_APP_CMD_UDP_INPUT_CFG, "0.0.0.0", 7355);
    post_empty(DSD_APP_CMD_M17_TX_TOGGLE);
    post_empty(DSD_APP_CMD_PROVOICE_ESK_TOGGLE);
    post_empty(DSD_APP_CMD_PROVOICE_MODE_TOGGLE);
    post_empty(DSD_APP_CMD_LRRP_DISABLE);
    post_empty(DSD_APP_CMD_DMR_RESET);

    rc |= expect_int("io/state commands applied", dsd_app_drain_cmds(&opts, &state), 37);
    /* The M17 stream encoder keeps the input it started on, whose rate set its decimation: the four input switches
       are refused and open nothing (issue #634). */
    rc |= expect_int("input switches refused under the m17 stream encoder", opts.audio_in_type, AUDIO_IN_RTL);
    rc |= expect_true("no udp input bound", opts.udp_in_ctx == NULL);
    rc |= expect_true("no file opened", opts.audio_in_file == NULL && opts.symbolfile == NULL);
    rc |= expect_int("compact toggled", opts.frontend_terminal_display.terminal_compact, !compact_before);
    rc |= expect_int("slot1 disabled", opts.slot1_on, 0);
    rc |= expect_int("slot2 disabled", opts.slot2_on, 0);
    rc |= expect_int("slot preference cycled", opts.slot_preference, 1);
    rc |= expect_int("payload toggled", opts.payload, 1);
    rc |= expect_int("p25 ga toggled", opts.frontend_display.show_p25_group_affiliations, 1);
    rc |= expect_int("lpf toggled", opts.use_lpf, 1);
    rc |= expect_int("hpf toggled", opts.use_hpf, 1);
    rc |= expect_int("pbf toggled", opts.use_pbf, 1);
    rc |= expect_int("hpf-d toggled", opts.use_hpf_d, 1);
    rc |= expect_int("aggressive sync toggled", opts.aggressive_framesync, 1);
    rc |= expect_int("call alert disabled by empty event mask", opts.call_alert, 0);
    rc |= expect_int("call alert events masked to zero", opts.call_alert_events, 0);
    rc |= expect_int("lcw retune toggled", opts.p25_lcw_retune, 1);
    rc |= expect_int("candidate preference toggled", opts.p25_prefer_candidates, 1);
    rc |= expect_int("x2 inverted", opts.inverted_x2tdma, 1);
    rc |= expect_int("dmr inverted", opts.inverted_dmr, 1);
    rc |= expect_int("dpmr inverted", opts.inverted_dpmr, 1);
    rc |= expect_int("m17 inverted", opts.inverted_m17, 1);
    rc |= expect_int("constellation toggled", opts.frontend_display.constellation, 1);
    rc |= expect_int("constellation normalization toggled", opts.frontend_display.const_norm_mode, 1);
    rc |= expect_true("constellation gate clamped", opts.frontend_display.const_gate_other > 0.89f
                                                        && opts.frontend_display.const_gate_other <= 0.90f);
    rc |= expect_int("eye toggled", opts.frontend_display.eye_view, 1);
    rc |= expect_int("eye unicode toggled", opts.frontend_terminal_display.eye_unicode, 1);
    rc |= expect_int("eye color toggled", opts.frontend_terminal_display.eye_color, 1);
    rc |= expect_int("fsk histogram toggled", opts.frontend_display.fsk_hist_view, 1);
    rc |= expect_int("spectrum toggled", opts.frontend_display.spectrum_view, 1);
    rc |= expect_int("m17 tx toggled", state.m17encoder_tx, 1);
    rc |= expect_int("provoice esk toggled", state.esk_mask, 0xA0);
    rc |= expect_int("provoice mode toggled", state.ea_mode, 1);
    rc |= expect_int("lrrp disabled", opts.lrrp_file_output, 0);
    rc |= expect_int("dmr reset rest channel", state.dmr_rest_channel, -1);
    rc |= expect_int("dmr reset mfid", state.dmr_mfid, -1);

    post_empty(DSD_APP_CMD_FORCE_PRIV_TOGGLE);
    post_empty(DSD_APP_CMD_FORCE_PRIV_TOGGLE);
    post_empty(DSD_APP_CMD_FORCE_RC4_TOGGLE);
    post_empty(DSD_APP_CMD_SLOT1_TOGGLE);
    post_empty(DSD_APP_CMD_SLOT2_TOGGLE);
    post_empty(DSD_APP_CMD_SLOT_PREF_CYCLE);
    post_empty(DSD_APP_CMD_SLOT_PREF_CYCLE);
    post_empty(DSD_APP_CMD_M17_TX_TOGGLE);
    post_empty(DSD_APP_CMD_PROVOICE_ESK_TOGGLE);
    post_empty(DSD_APP_CMD_PROVOICE_MODE_TOGGLE);
    rc |= expect_int("second command group applied", dsd_app_drain_cmds(&opts, &state), 10);
    rc |= expect_int("force rc4 selected", state.M, 0x21);
    rc |= expect_int("slot1 re-enabled", opts.slot1_on, 1);
    rc |= expect_int("slot2 re-enabled", opts.slot2_on, 1);
    rc |= expect_int("slot preference wrapped", opts.slot_preference, 0);
    rc |= expect_int("m17 tx toggled off sets eot", state.m17encoder_tx, 0);
    rc |= expect_int("m17 tx eot set", state.m17encoder_eot, 1);
    rc |= expect_int("provoice esk toggled back", state.esk_mask, 0);
    rc |= expect_int("provoice mode toggled back", state.ea_mode, 0);

    freeState(&state);
    return rc;
}

static int
test_compact_visualizer_toast(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);

    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frontend_terminal_display.terminal_compact = 1;

    post_empty(DSD_APP_CMD_CONST_TOGGLE);
    rc |= expect_int("constellation toggle applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("constellation enabled", opts.frontend_display.constellation, 1);
    rc |= expect_contains("constellation compact toast", state.ui_msg, "hidden in compact view");

    /* Switching a visualizer off raises no hint */
    state.ui_msg[0] = '\0';
    post_empty(DSD_APP_CMD_CONST_TOGGLE);
    rc |= expect_int("constellation untoggle applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("no toast on visualizer off", state.ui_msg, "");

    /* No hint outside compact view */
    opts.frontend_terminal_display.terminal_compact = 0;
    post_empty(DSD_APP_CMD_SPECTRUM_TOGGLE);
    rc |= expect_int("spectrum toggle applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("no toast in full view", state.ui_msg, "");

    /* Eye view shares the hint path while compact */
    opts.frontend_terminal_display.terminal_compact = 1;
    post_empty(DSD_APP_CMD_EYE_TOGGLE);
    rc |= expect_int("eye toggle applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_contains("eye compact toast", state.ui_msg, "hidden in compact view");

    freeState(&state);
    return rc;
}

static int
seed_active_canonical_calls(dsd_opts* opts, dsd_state* state, long vc_freq, int tg) {
    int rc = 0;
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        dsd_call_observation observation = {
            .protocol = DSD_SYNC_P25P1_POS,
            .slot = (uint8_t)slot,
            .kind = DSD_CALL_KIND_GROUP_VOICE,
            .ota_target_id = (uint32_t)(tg + slot),
            .policy_target_id = (uint32_t)(tg + slot),
            .ota_source_id = (uint32_t)(tg + slot + 10),
            .frequency_hz = vc_freq,
            .observed_m = 1.0 + slot,
        };
        rc |=
            expect_int("seed canonical call", dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN), 1);
        dsd_event_sync_slot(opts, state, (uint8_t)slot);
    }
    return rc;
}

static int
expect_call_phase(const char* tag, const dsd_state* state, uint8_t slot, dsd_call_phase want) {
    dsd_call_snapshot snapshot;
    if (dsd_call_state_get(state, slot, &snapshot) != 1) {
        DSD_FPRINTF(stderr, "%s: canonical call unavailable\n", tag);
        return 1;
    }
    return expect_int(tag, snapshot.phase, want);
}

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
// A patched call's policy target is the member WG the grant matched, while its
// over-the-air target stays the supergroup the frontends show.
static void
seed_active_p25_patched_voice(dsd_opts* opts, dsd_state* state, long cc_freq, long vc_freq, int tg, int policy_tg) {
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->frame_p25p1 = 1;
    opts->trunk_is_tuned = 1;
    state->p25_cc_freq = cc_freq;
    state->trunk_cc_freq = cc_freq;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = vc_freq;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = vc_freq;
    state->last_cc_sync_time = 123;
    state->last_cc_sync_time_m = 42.0;
    state->synctype = DSD_SYNC_P25P1_POS;
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    state->samplesPerSymbol = 7;
    state->symbolCenter = 3;
    state->p25_cc_is_tdma = 0;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state->sps_hunt_counter = 17;
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = (uint32_t)tg,
        .policy_target_id = (uint32_t)policy_tg,
        .ota_source_id = (uint32_t)(tg + 1),
        .frequency_hz = vc_freq,
        .observed_m = 1.0,
    };
    if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0) {
        dsd_event_sync_slot(opts, state, 0U);
    }
}

static void
seed_active_p25_voice(dsd_opts* opts, dsd_state* state, long cc_freq, long vc_freq, int tg) {
    seed_active_p25_patched_voice(opts, state, cc_freq, vc_freq, tg, tg);
}

static int
test_manual_tune_commands_commit_only_after_acceptance(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

#ifdef USE_RADIO
    init_test_context(&opts, &state);
    reset_io_control_tune_stub(RTL_STREAM_TUNE_TIMEOUT);
    rc |= expect_int("accepted RTL frequency timeout queued",
                     dsd_app_command_set_u32(DSD_APP_CMD_RTL_SET_FREQ, 851500000U), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("accepted RTL frequency timeout drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("accepted RTL frequency timeout reports pending",
                      strstr(state.ui_msg, "Accepted: RTL frequency -> 851500000 Hz (pending)") != NULL);
    rc |= expect_int("accepted RTL frequency timeout tune calls", g_io_control_tune_calls, 1);
    opts.scanner_mode = 1;
    post_u32(DSD_APP_CMD_RTL_SET_FREQ, 852000000U);
    rc |= expect_int("legacy scanner tune drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("legacy manual tune preserves scanner", opts.scanner_mode, 1);
    rc |= expect_int("typed metadata for manual tune", dsd_channel_mode_set(&state, 0, DSD_SCAN_MODE_DMR), 0);
    rc |= expect_int("typed mode before manual tune", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR), 0);
    post_u32(DSD_APP_CMD_RTL_SET_FREQ, 853000000U);
    rc |= expect_int("typed scanner tune drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("typed manual tune releases scanner", opts.scanner_mode, 0);
    rc |= expect_int("typed manual tune releases mode", dsd_scan_mode_active(&state), DSD_SCAN_MODE_INHERIT);
    rc |= expect_contains("manual tune explains scanner exit", state.ui_msg, "scanner stopped");
    freeState(&state);
#endif

    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 851000000L, 852000000L, 1201);
    rc |= seed_active_canonical_calls(&opts, &state, 852000000L, 1201);
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_DEFERRED);
    rc |= expect_int("deferred return-to-CC queued", dsd_app_command_action(DSD_APP_CMD_RETURN_CC),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("deferred return-to-CC drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("deferred return-to-CC CC tune calls", g_cc_tune_calls, 1);
    rc |= expect_int("deferred return-to-CC raw tune calls", g_io_control_tune_calls, 0);
    rc |= expect_true("deferred return-to-CC frequency", g_cc_tune_freq == 851000000L);
    rc |= expect_int("deferred return-to-CC TED SPS", g_cc_tune_ted_sps, 10);
    rc |= expect_int("deferred return-to-CC profile staged before commit", g_cc_profile_at_tune,
                     DSD_FRAME_SYNC_SPS_PROFILE_4800_2);
    rc |= expect_int("deferred return-to-CC keeps trunk tuned", opts.trunk_is_tuned, 1);
    rc |= expect_true("deferred return-to-CC keeps VC", state.p25_vc_freq[0] == 852000000L);
    rc |= expect_true("deferred return-to-CC keeps CC sync", state.last_cc_sync_time_m == 42.0);
    rc |= expect_int("deferred return-to-CC keeps SPS", state.samplesPerSymbol, 7);
    rc |= expect_int("deferred return-to-CC keeps SPS profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_4800_2);
    rc |= expect_int("deferred return-to-CC keeps SPS hunt counter", state.sps_hunt_counter, 17);
    rc |= expect_call_phase("deferred return-to-CC keeps canonical slot 1 active", &state, 0U, DSD_CALL_PHASE_ACTIVE);
    rc |= expect_call_phase("deferred return-to-CC keeps canonical slot 2 active", &state, 1U, DSD_CALL_PHASE_ACTIVE);

    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_PENDING);
    rc |= expect_int("accepted timeout return-to-CC queued", dsd_app_command_action(DSD_APP_CMD_RETURN_CC),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("accepted timeout return-to-CC drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("accepted timeout clears trunk tuned", opts.trunk_is_tuned, 0);
    rc |= expect_true("accepted timeout clears VC", state.p25_vc_freq[0] == 0L);
    rc |= expect_true("accepted timeout refreshes CC sync", state.last_cc_sync_time_m > 42.0);
    rc |= expect_int("accepted timeout selects P25 SPS profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    rc |= expect_int("accepted timeout resets SPS hunt counter", state.sps_hunt_counter, 0);
    rc |= expect_int("accepted timeout used profile-aware CC tune", g_cc_tune_calls, 1);
    rc |= expect_int("accepted timeout profile was staged before commit", g_cc_profile_at_tune,
                     DSD_FRAME_SYNC_SPS_PROFILE_4800_2);
    rc |= expect_call_phase("accepted return-to-CC ends canonical slot 1", &state, 0U, DSD_CALL_PHASE_ENDED);
    rc |= expect_call_phase("accepted return-to-CC ends canonical slot 2", &state, 1U, DSD_CALL_PHASE_ENDED);
    rc |= expect_int("accepted return-to-CC commits slot 1 history",
                     (int)state.event_history_s[0].Event_History_Items[1].target_id, 1201);
    rc |= expect_int("accepted return-to-CC commits slot 2 history",
                     (int)state.event_history_s[1].Event_History_Items[1].target_id, 1202);
    freeState(&state);

    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 852500000L, 853500000L, 1202);
    opts.frame_nxdn48 = 1;
    state.synctype = DSD_SYNC_NXDN_POS;
    state.lastsynctype = DSD_SYNC_NXDN_POS;
    state.p25_cc_is_tdma = 1;
    state.samplesPerSymbol = 20;
    state.symbolCenter = 9;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    state.sps_hunt_counter = 29;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_TIMEOUT);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_int("generic return-to-CC queued", dsd_app_command_action(DSD_APP_CMD_RETURN_CC),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("generic return-to-CC drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("generic return-to-CC raw tune calls", g_io_control_tune_calls, 1);
    rc |= expect_true("generic return-to-CC frequency", g_io_control_tune_freq == 852500000L);
    rc |= expect_int("generic return-to-CC CC tune calls", g_cc_tune_calls, 0);
    rc |= expect_int("generic return-to-CC keeps SPS", state.samplesPerSymbol, 20);
    rc |= expect_int("generic return-to-CC keeps symbol center", state.symbolCenter, 9);
    rc |= expect_int("generic return-to-CC keeps SPS profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_2400_4);
    rc |= expect_int("generic return-to-CC keeps SPS hunt counter", state.sps_hunt_counter, 29);
    freeState(&state);

    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 853000000L, 854000000L, 2201);
    dsd_tg_policy_entry dispatch_entry;
    rc |= expect_int(
        "seed lockout named row",
        dsd_tg_policy_make_exact_entry(2201, "A", "Dispatch", DSD_TG_POLICY_SOURCE_IMPORTED, &dispatch_entry), 0);
    rc |= expect_int("append lockout named row", dsd_tg_policy_append_exact(&state, &dispatch_entry), 0);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_DEFERRED);
    const uint64_t lockout_history_revision = state.event_history_s[0].revision;
    rc |= expect_int("deferred lockout queued", dsd_app_command_set_u8(DSD_APP_CMD_LOCKOUT_SLOT, 0U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("deferred lockout drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("deferred lockout tune calls", g_cc_tune_calls, 1);
    rc |= expect_int("deferred lockout keeps trunk tuned", opts.trunk_is_tuned, 1);
    rc |= expect_true("deferred lockout keeps VC", state.p25_vc_freq[0] == 854000000L);
    rc |= expect_true("deferred lockout keeps CC sync", state.last_cc_sync_time_m == 42.0);
    rc |= expect_int("deferred lockout keeps SPS", state.samplesPerSymbol, 7);
    char lockout_mode[8] = {0};
    char lockout_name[50] = {0};
    rc |= expect_int("deferred lockout policy installed",
                     dsd_tg_policy_lookup_label(&state, 2201U, lockout_mode, sizeof(lockout_mode), lockout_name,
                                                sizeof(lockout_name)),
                     1);
    rc |= expect_str("deferred lockout policy mode", lockout_mode, "B");
    rc |= expect_str("deferred lockout policy name", lockout_name, "Dispatch");
    rc |= expect_true("deferred lockout advances event history revision",
                      state.event_history_s[0].revision > lockout_history_revision);
    rc |= expect_true("deferred lockout reports cleanup separately",
                      strstr(state.ui_msg, "TG 2201 locked out; return-to-CC tune failed") != NULL);
    rc |= expect_call_phase("deferred lockout keeps canonical call active", &state, 0U, DSD_CALL_PHASE_ACTIVE);
    freeState(&state);

    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 853500000L, 854500000L, 2202);
    state.p25_cc_is_tdma = 2;
    state.samplesPerSymbol = 8;
    state.symbolCenter = 3;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state.sps_hunt_counter = 19;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_TIMEOUT);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_DEFERRED);
    rc |= expect_int("profile-neutral lockout queued", dsd_app_command_set_u8(DSD_APP_CMD_LOCKOUT_SLOT, 0U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("profile-neutral lockout drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("profile-neutral lockout raw tune calls", g_io_control_tune_calls, 1);
    rc |= expect_true("profile-neutral lockout frequency", g_io_control_tune_freq == 853500000L);
    rc |= expect_int("profile-neutral lockout CC tune calls", g_cc_tune_calls, 0);
    rc |= expect_int("profile-neutral lockout keeps SPS", state.samplesPerSymbol, 8);
    rc |= expect_int("profile-neutral lockout keeps symbol center", state.symbolCenter, 3);
    rc |=
        expect_int("profile-neutral lockout keeps SPS profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    rc |= expect_int("profile-neutral lockout keeps SPS hunt counter", state.sps_hunt_counter, 19);
    freeState(&state);

    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 0L, 854500000L, 2203);
    state.carrier = 1;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_DEFERRED);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_DEFERRED);
    rc |= expect_int("no-CC lockout queued", dsd_app_command_set_u8(DSD_APP_CMD_LOCKOUT_SLOT, 0U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("no-CC lockout drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("no-CC lockout skips raw tune", g_io_control_tune_calls, 0);
    rc |= expect_int("no-CC lockout skips CC tune", g_cc_tune_calls, 0);
    rc |= expect_int("no-CC lockout clears trunk tuned", opts.trunk_is_tuned, 0);
    rc |= expect_true("no-CC lockout clears P25 VC", state.p25_vc_freq[0] == 0L);
    rc |= expect_true("no-CC lockout clears trunk VC", state.trunk_vc_freq[0] == 0L);
    rc |= expect_int("no-CC lockout runs no-carrier cleanup", state.carrier, 0);
    rc |= expect_true("no-CC lockout keeps CC unknown", state.trunk_cc_freq == 0L && state.p25_cc_freq == 0L);
    rc |= expect_call_phase("no-CC lockout ends canonical call", &state, 0U, DSD_CALL_PHASE_ENDED);
    freeState(&state);

    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 855000000L, 856000000L, 3201);
    rc |= seed_active_canonical_calls(&opts, &state, 856000000L, 3201);
    state.lcn_freq_count = 4;
    state.lcn_freq_roll = 0;
    state.trunk_lcn_freq[0] = 0L;
    state.trunk_lcn_freq[1] = 857000000L;
    state.trunk_lcn_freq[2] = 0L;
    state.trunk_lcn_freq[3] = 858000000L;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_DEFERRED);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_int("deferred channel cycle queued", dsd_app_command_action(DSD_APP_CMD_CHANNEL_CYCLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("deferred channel cycle drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("deferred channel cycle raw tune calls", g_io_control_tune_calls, 1);
    rc |= expect_true("deferred channel cycle frequency", g_io_control_tune_freq == 857000000L);
    rc |= expect_int("deferred channel cycle CC tune calls", g_cc_tune_calls, 0);
    rc |= expect_int("deferred channel cycle keeps roll", state.lcn_freq_roll, 0);
    rc |= expect_int("deferred channel cycle keeps tuned", opts.trunk_is_tuned, 1);
    rc |= expect_true("deferred channel cycle keeps VC", state.p25_vc_freq[0] == 856000000L);
    rc |= expect_true("deferred channel cycle keeps CC sync", state.last_cc_sync_time_m == 42.0);
    rc |= expect_call_phase("deferred channel cycle keeps canonical slot 1 active", &state, 0U, DSD_CALL_PHASE_ACTIVE);
    rc |= expect_call_phase("deferred channel cycle keeps canonical slot 2 active", &state, 1U, DSD_CALL_PHASE_ACTIVE);

    state.samplesPerSymbol = 8;
    state.symbolCenter = 3;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state.sps_hunt_counter = 23;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_TIMEOUT);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_DEFERRED);
    rc |= expect_int("accepted channel cycle queued", dsd_app_command_action(DSD_APP_CMD_CHANNEL_CYCLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("accepted channel cycle drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("accepted channel cycle skips empty entry", state.lcn_freq_roll, 2);
    rc |= expect_int("accepted channel cycle clears tuned", opts.trunk_is_tuned, 0);
    rc |= expect_true("accepted channel cycle clears P25 VC", state.p25_vc_freq[0] == 0L);
    rc |= expect_true("accepted channel cycle refreshes CC sync", state.last_cc_sync_time_m > 42.0);
    rc |= expect_int("accepted channel cycle uses raw tune", g_io_control_tune_calls, 1);
    rc |= expect_true("accepted channel cycle frequency", g_io_control_tune_freq == 857000000L);
    rc |= expect_int("accepted channel cycle skips CC tune", g_cc_tune_calls, 0);
    rc |= expect_int("accepted channel cycle keeps SPS", state.samplesPerSymbol, 8);
    rc |= expect_int("accepted channel cycle keeps symbol center", state.symbolCenter, 3);
    rc |= expect_int("accepted channel cycle keeps SPS profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    rc |= expect_int("accepted channel cycle keeps SPS hunt counter", state.sps_hunt_counter, 23);
    rc |= expect_call_phase("accepted channel cycle ends canonical slot 1", &state, 0U, DSD_CALL_PHASE_ENDED);
    rc |= expect_call_phase("accepted channel cycle ends canonical slot 2", &state, 1U, DSD_CALL_PHASE_ENDED);

    rc |= expect_int("later channel cycle queued", dsd_app_command_action(DSD_APP_CMD_CHANNEL_CYCLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("later channel cycle drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("later channel cycle reaches frequency after empty entry", g_io_control_tune_freq == 858000000L);
    rc |= expect_int("later channel cycle advances past second frequency", state.lcn_freq_roll, 4);

    rc |= expect_int("wrapped channel cycle queued", dsd_app_command_action(DSD_APP_CMD_CHANNEL_CYCLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("wrapped channel cycle drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("wrapped channel cycle skips leading empty entry", g_io_control_tune_freq == 857000000L);
    rc |= expect_int("wrapped channel cycle advances from recovered entry", state.lcn_freq_roll, 2);
    freeState(&state);

    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    return rc;
}

static dsd_trunk_tune_result
stub_tune_to_freq_ok(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)ted_sps;
    (void)request_id;
    return freq > 0 ? DSD_TRUNK_TUNE_RESULT_OK : DSD_TRUNK_TUNE_RESULT_FAILED;
}

/* #506: the user lockout retunes the radio to the control channel itself, bypassing
 * p25_sm_release(). The P25 SM must still be handed its parked state, or it keeps
 * judging every later grant as a preemption of the assignment it was following. */
static int
test_lockout_hands_p25_sm_back_to_cc(int persist) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    opts.persist_tg_lockouts = (uint8_t)persist;
    seed_active_p25_voice(&opts, &state, 851000000L, 852000000L, 1201);
    opts.trunk_tune_group_calls = 1;
    state.trunk_chan_map[0x1234] = 852000000L;
    state.trunk_chan_map[0x1235] = 853000000L;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_freq_request = stub_tune_to_freq_ok});

    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    p25_sm_init_ctx(sm, &opts, &state);
    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 852000000L, 1201, 1202, 0);
    p25_sm_event(sm, &opts, &state, &ev);
    ev = p25_sm_ev_active_call(0, 1201, 0, 1202, 1, 0);
    p25_sm_event(sm, &opts, &state, &ev);
    rc |= expect_int("SM follows the seeded assignment", p25_sm_get_state(sm), P25_SM_TUNED);
    rc |= expect_int("SM slot carries voice before lockout", sm->slots[0].voice_active, 1);

    // A refused CC tune leaves the assignment, and the SM, exactly where they were.
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_DEFERRED);
    rc |= expect_int("deferred lockout queued", dsd_app_command_set_u8(DSD_APP_CMD_LOCKOUT_SLOT, 0U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("deferred lockout drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("deferred lockout leaves the SM tuned", p25_sm_get_state(sm), P25_SM_TUNED);
    rc |= expect_int("deferred lockout keeps the SM slot voice", sm->slots[0].voice_active, 1);
    rc |= expect_true("deferred lockout keeps the SM voice channel", sm->vc_freq_hz == 852000000L);

    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_int("lockout queued", dsd_app_command_set_u8(DSD_APP_CMD_LOCKOUT_SLOT, 0U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("lockout drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("lockout tuned the CC", g_cc_tune_calls, 1);
    rc |= expect_int("lockout clears trunk tuned", opts.trunk_is_tuned, 0);
    rc |= expect_int("lockout parks the P25 SM on the CC", p25_sm_get_state(sm), P25_SM_ON_CC);
    rc |= expect_int("lockout clears the SM slot voice", sm->slots[0].voice_active, 0);
    rc |= expect_true("lockout clears the SM voice channel", sm->vc_freq_hz == 0);
    rc |= expect_int("lockout gates grants until the CC decodes", sm->cc_sync_pending, 1);

    // The site keeps trunking: once the CC decodes again, a grant for another
    // talkgroup is followed instead of being refused as a preemption.
    state.p25_last_cc_msg_time_m = dsd_decode_now_mono_s() + 0.01;
    ev = p25_sm_ev_group_grant(0x1234, 852000000L, 1201, 1202, 0);
    p25_sm_event(sm, &opts, &state, &ev);
    rc |= expect_true("avoided grant remains blocked", p25_sm_get_state(sm) != P25_SM_TUNED);
    rc |= expect_str("grant refusal identifies lockout kind", state.p25_sm_last_reason,
                     persist ? "grant-blocked-mode" : "grant-blocked-session-avoid");
    ev = p25_sm_ev_group_grant(0x1235, 853000000L, 1300, 1301, 0);
    p25_sm_event(sm, &opts, &state, &ev);
    rc |= expect_int("next grant is followed after lockout", p25_sm_get_state(sm), P25_SM_TUNED);
    rc |= expect_true("next grant tunes the new voice channel", sm->vc_freq_hz == 853000000L);
    rc |= expect_int("next grant marks trunk tuned", opts.trunk_is_tuned, 1);

    // The manual return-to-CC command takes the same shortcut past the SM.
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    rc |=
        expect_int("return-to-CC queued", dsd_app_command_action(DSD_APP_CMD_RETURN_CC), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("return-to-CC drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("return-to-CC tuned the CC", g_cc_tune_calls, 1);
    rc |= expect_int("return-to-CC parks the P25 SM on the CC", p25_sm_get_state(sm), P25_SM_ON_CC);
    rc |= expect_true("return-to-CC clears the SM voice channel", sm->vc_freq_hz == 0);

    p25_sm_init_ctx(sm, NULL, NULL);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_trunk_tuning_requests_reset();
    freeState(&state);
    return rc;
}

static int
test_skip_hands_p25_sm_back_to_cc(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    opts.persist_tg_lockouts = 1;
    seed_active_p25_voice(&opts, &state, 851000000L, 852000000L, 1201);
    opts.trunk_tune_group_calls = 1;
    state.trunk_chan_map[0x1234] = 852000000L;
    state.trunk_chan_map[0x1235] = 853000000L;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_freq_request = stub_tune_to_freq_ok});

    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    p25_sm_init_ctx(sm, &opts, &state);
    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 852000000L, 1201, 1202, 0);
    p25_sm_event(sm, &opts, &state, &ev);
    ev = p25_sm_ev_active_call(0, 1201, 0, 1202, 1, 0);
    p25_sm_event(sm, &opts, &state, &ev);
    rc |= expect_int("SM follows the seeded assignment", p25_sm_get_state(sm), P25_SM_TUNED);
    rc |= expect_int("SM slot carries voice before skip", sm->slots[0].voice_active, 1);

    // A refused CC tune leaves the assignment, and the SM, exactly where they were.
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_DEFERRED);
    rc |= expect_int("deferred skip queued", dsd_app_command_set_u8(DSD_APP_CMD_SKIP_SLOT, 0U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("deferred skip drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("deferred skip leaves the SM tuned", p25_sm_get_state(sm), P25_SM_TUNED);
    rc |= expect_int("deferred skip keeps the SM slot voice", sm->slots[0].voice_active, 1);
    rc |= expect_true("deferred skip keeps the SM voice channel", sm->vc_freq_hz == 852000000L);

    rc |= expect_true("refused skip remains armed",
                      dsd_tg_policy_call_skip_active(&state, 1201, dsd_decode_now_mono_s()));
    int muted = 0;
    rc |= expect_int("refused skip media gate", dsd_audio_group_gate_mono(&opts, &state, 1201, 0, &muted), 0);
    rc |= expect_int("refused skip remains muted", muted, 1);
    rc |= expect_str("refused skip toast", state.ui_msg, "TG 1201 skipped; return-to-CC tune failed");
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_int("skip queued", dsd_app_command_set_u8(DSD_APP_CMD_SKIP_SLOT, 0U), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("skip drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("skip tuned the CC", g_cc_tune_calls, 1);
    rc |= expect_int("skip clears trunk tuned", opts.trunk_is_tuned, 0);
    rc |= expect_int("skip parks the P25 SM on the CC", p25_sm_get_state(sm), P25_SM_ON_CC);
    rc |= expect_int("skip clears the SM slot voice", sm->slots[0].voice_active, 0);
    rc |= expect_true("skip clears the SM voice channel", sm->vc_freq_hz == 0);
    rc |= expect_int("skip gates grants until the CC decodes", sm->cc_sync_pending, 1);

    // The site keeps trunking: once the CC decodes again, a grant for another
    // talkgroup is followed instead of being refused as a preemption.
    state.p25_last_cc_msg_time_m = dsd_decode_now_mono_s() + 0.01;
    ev = p25_sm_ev_group_grant(0x1234, 852000000L, 1201, 1202, 0);
    p25_sm_event(sm, &opts, &state, &ev);
    rc |= expect_true("skipped grant remains blocked", p25_sm_get_state(sm) != P25_SM_TUNED);
    rc |= expect_str("grant refusal identifies skip kind", state.p25_sm_last_reason, "grant-blocked-call-skip");
    const double refresh_m = dsd_decode_now_mono_s();
    rc |= expect_int("backdate active skip", dsd_tg_policy_call_skip_arm(&state, 1201, 1202, 0, refresh_m - 10.0), 0);
    p25_sm_event(sm, &opts, &state, &ev);
    rc |= expect_true("repeated grant refreshes skip", dsd_tg_policy_call_skip_active(&state, 1201, refresh_m + 10.0));
    state.p25_patch_count = 1;
    state.p25_patch_sgid[0] = 1201;
    state.p25_patch_is_patch[0] = state.p25_patch_active[0] = 1;
    state.p25_patch_last_update[0] = time(NULL);
    state.p25_patch_wgid_count[0] = 1;
    state.p25_patch_wgid[0][0] = 1300;
    p25_sm_event(sm, &opts, &state, &ev);
    rc |= expect_int("skipped supergroup rejects eligible member", p25_sm_get_state(sm), P25_SM_ON_CC);
    rc |= expect_str("patched skip reason", state.p25_sm_last_reason, "grant-blocked-call-skip");
    rc |= expect_true("one skip entry for patched call", dsd_tg_policy_call_skip_count(&state, refresh_m) == 1);
    ev = p25_sm_ev_group_grant(0x1235, 853000000L, 1300, 1301, 0);
    p25_sm_event(sm, &opts, &state, &ev);
    rc |= expect_int("next grant is followed after skip", p25_sm_get_state(sm), P25_SM_TUNED);
    rc |= expect_true("next grant tunes the new voice channel", sm->vc_freq_hz == 853000000L);
    rc |= expect_int("next grant marks trunk tuned", opts.trunk_is_tuned, 1);

    // The manual return-to-CC command takes the same shortcut past the SM.
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    rc |=
        expect_int("return-to-CC queued", dsd_app_command_action(DSD_APP_CMD_RETURN_CC), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("return-to-CC drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("return-to-CC tuned the CC", g_cc_tune_calls, 1);
    rc |= expect_int("return-to-CC parks the P25 SM on the CC", p25_sm_get_state(sm), P25_SM_ON_CC);
    rc |= expect_true("return-to-CC clears the SM voice channel", sm->vc_freq_hz == 0);

    p25_sm_init_ctx(sm, NULL, NULL);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_trunk_tuning_requests_reset();
    freeState(&state);
    return rc;
}

struct skip_reader {
    dsd_opts* opts;
    dsd_state* state;
    atomic_int stop;
    int failures;
    unsigned int blocked;
    unsigned int allowed;
};

static DSD_THREAD_RETURN_TYPE
skip_reader_run(void* opaque) {
    struct skip_reader* reader = opaque;
    while (!atomic_load(&reader->stop)) {
        p25_sm_tick_guard_enter();
        int left = -1, right = -1;
        dsd_tg_policy_decision decision;
        reader->failures |= dsd_audio_group_gate_dual(reader->opts, reader->state, 1201, 1201, 0, 0, &left, &right);
        reader->failures |= dsd_tg_policy_evaluate_group_call(reader->opts, reader->state, 1201, 1202, 0, 0, &decision);
        const int blocked = (decision.block_reasons & DSD_TG_POLICY_BLOCK_CALL_SKIP) != 0;
        reader->failures |= left != blocked || right != blocked || decision.audio_allowed != !blocked
                            || decision.tune_allowed != !blocked || decision.record_allowed != !blocked
                            || decision.stream_allowed != !blocked
                            || dsd_tg_policy_call_skip_count(reader->state, dsd_decode_now_mono_s()) != (size_t)blocked;
        if (blocked) {
            ++reader->blocked;
        } else {
            ++reader->allowed;
        }
        p25_sm_tick_guard_leave();
        dsd_thread_yield();
    }
    DSD_THREAD_RETURN;
}

static int
test_skip_command_reader_stress(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 851000000L, 852000000L, 1201);
    opts.trunk_tune_group_calls = 1;
    state.synctype = state.lastsynctype = DSD_SYNC_P25P2_POS;
    const dsd_call_observation call = {.protocol = DSD_SYNC_P25P2_POS,
                                       .slot = 0,
                                       .kind = DSD_CALL_KIND_GROUP_VOICE,
                                       .ota_target_id = 1201,
                                       .policy_target_id = 1201,
                                       .ota_source_id = 1202,
                                       .observed_m = dsd_decode_now_mono_s()};
    int rc = expect_int("stress seeds Phase 2 call", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN), 1);
    rc |= expect_int("stress creates retained store",
                     dsd_tg_policy_call_skip_arm(&state, 1201, 1202, 0, call.observed_m), 0);
    dsd_tg_policy_call_skip_clear(&state);
    uint64_t context = 0;
    dsd_tg_policy_table_version(&state, &context, NULL);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_FAILED);
    struct skip_reader reader = {.opts = &opts, .state = &state};
    atomic_init(&reader.stop, 0);
    dsd_thread_t thread;
    const int started = dsd_thread_create(&thread, skip_reader_run, &reader) == 0;
    rc |= expect_true("skip reader started", started);
    if (started) {
        for (int i = 0; i < 100; ++i) {
            rc |= expect_true("stress skip queued", dsd_app_command_set_u8(DSD_APP_CMD_SKIP_SLOT, 0) > 0);
            rc |= expect_int("stress skip drained", dsd_app_drain_cmds(&opts, &state), 1);
            dsd_sleep_ms(1);
            rc |= expect_true("stress clear queued",
                              dsd_app_command_submit(DSD_APP_CMD_TG_SESSION_AVOID_CLEAR, &context, sizeof context) > 0);
            rc |= expect_int("stress clear drained", dsd_app_drain_cmds(&opts, &state), 1);
            dsd_sleep_ms(1);
        }
        atomic_store(&reader.stop, 1);
        dsd_thread_join(thread);
        rc |= expect_int("concurrent policy verdicts consistent", reader.failures, 0);
        rc |= expect_true("reader observed both policy states", reader.blocked > 0 && reader.allowed > 0);
        rc |= expect_true("stress final count cleared",
                          dsd_tg_policy_call_skip_count(&state, dsd_decode_now_mono_s()) == 0);
    }
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    freeState(&state);
    return rc;
}

/* #506 follow-up: the manual channel cycle moves the radio exactly the way the
 * lockout and the return-to-CC do -- through the tuning hooks, past
 * p25_sm_release() -- on both of its legs. Either one left the SM judging later
 * grants as preemptions of the assignment it was still following. */
static int
test_channel_cycle_hands_p25_sm_back_to_cc(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 851000000L, 852000000L, 1201);
    opts.trunk_tune_group_calls = 1;
    state.trunk_chan_map[0x1234] = 852000000L;
    state.trunk_chan_map[0x1235] = 853000000L;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_freq_request = stub_tune_to_freq_ok});

    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    p25_sm_init_ctx(sm, &opts, &state);
    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 852000000L, 1201, 1202, 0);
    p25_sm_event(sm, &opts, &state, &ev);
    ev = p25_sm_ev_active_call(0, 1201, 0, 1202, 1, 0);
    p25_sm_event(sm, &opts, &state, &ev);
    rc |= expect_int("SM follows the seeded assignment", p25_sm_get_state(sm), P25_SM_TUNED);
    rc |= expect_int("SM slot carries voice before the cycle", sm->slots[0].voice_active, 1);

    // Candidate leg: p25_prefer_candidates routes the cycle through the P25 CC hook.
    opts.p25_prefer_candidates = 1;
    rc |= expect_int("candidate seeded", p25_cc_add_candidate(&state, 857000000L, 1), 1);
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_int("candidate cycle queued", dsd_app_command_action(DSD_APP_CMD_CHANNEL_CYCLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("candidate cycle drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("candidate cycle tuned the candidate", g_cc_tune_calls, 1);
    rc |= expect_true("candidate cycle used the candidate frequency", g_cc_tune_freq == 857000000L);
    rc |= expect_int("candidate cycle parks the P25 SM on the CC", p25_sm_get_state(sm), P25_SM_ON_CC);
    rc |= expect_int("candidate cycle clears the SM slot voice", sm->slots[0].voice_active, 0);
    rc |= expect_true("candidate cycle clears the SM voice channel", sm->vc_freq_hz == 0);
    rc |= expect_int("candidate cycle gates grants until the CC decodes", sm->cc_sync_pending, 1);

    // The site keeps trunking: the next grant is followed instead of refused as a
    // preemption of the call the SM was still holding.
    state.p25_last_cc_msg_time_m = dsd_decode_now_mono_s() + 0.01;
    ev = p25_sm_ev_group_grant(0x1235, 853000000L, 1300, 1301, 0);
    p25_sm_event(sm, &opts, &state, &ev);
    rc |= expect_int("next grant is followed after a candidate cycle", p25_sm_get_state(sm), P25_SM_TUNED);
    rc |= expect_true("next grant tunes the new voice channel", sm->vc_freq_hz == 853000000L);
    ev = p25_sm_ev_active_call(0, 1300, 0, 1301, 1, 0);
    p25_sm_event(sm, &opts, &state, &ev);
    rc |= expect_int("SM slot carries voice again", sm->slots[0].voice_active, 1);

    // LCN leg: with no candidate preference the same key falls through to the raw
    // channel cycle, which moves the radio just as far past the SM.
    opts.p25_prefer_candidates = 0;
    state.lcn_freq_count = 1;
    state.lcn_freq_roll = 0;
    state.trunk_lcn_freq[0] = 851000000L;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_int("channel cycle queued", dsd_app_command_action(DSD_APP_CMD_CHANNEL_CYCLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("channel cycle drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("channel cycle used the raw tune", g_io_control_tune_calls, 1);
    rc |= expect_int("channel cycle parks the P25 SM on the CC", p25_sm_get_state(sm), P25_SM_ON_CC);
    rc |= expect_int("channel cycle clears the SM slot voice", sm->slots[0].voice_active, 0);
    rc |= expect_true("channel cycle clears the SM voice channel", sm->vc_freq_hz == 0);
    rc |= expect_int("channel cycle gates grants until the CC decodes", sm->cc_sync_pending, 1);

    p25_sm_init_ctx(sm, NULL, NULL);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_trunk_tuning_requests_reset();
    freeState(&state);
    return rc;
}

/*
 * The manual channel cycle and scan avoid on an untyped -Y list move an external radio
 * through rigctl on PCM input: no RTL stream generation moves and io_control does not advance
 * the trunk-tuning generation, so nothing else tells the analog tap (issue #522). An accepted
 * step -- pending included -- clears the received tone; a refused one leaves the radio, and so
 * its tone, where they were.
 */
static int
test_manual_scan_steps_clear_received_tone(void) {
    static const struct {
        int cmd;
        int tune_result;
        int clears;
        const char* tag;
    } cases[] = {
        {DSD_APP_CMD_CHANNEL_CYCLE, RTL_STREAM_TUNE_OK, 1, "channel cycle by rigctl clears the received tone"},
        {DSD_APP_CMD_CHANNEL_CYCLE, RTL_STREAM_TUNE_TIMEOUT, 1, "pending channel cycle clears the received tone"},
        {DSD_APP_CMD_CHANNEL_CYCLE, RTL_STREAM_TUNE_FAILED, 0, "refused channel cycle keeps the received tone"},
        {DSD_APP_CMD_SCAN_AVOID, RTL_STREAM_TUNE_OK, 1, "scan avoid by rigctl clears the received tone"},
        {DSD_APP_CMD_SCAN_AVOID, RTL_STREAM_TUNE_FAILED, 0, "refused scan avoid step keeps the received tone"},
    };

    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        init_test_context(&opts, &state);
        opts.scanner_mode = 1;
        opts.use_rigctl = 1;
        opts.audio_in_type = AUDIO_IN_PULSE;
        state.lcn_freq_count = 3;
        state.trunk_lcn_freq[0] = 461012500L;
        state.trunk_lcn_freq[1] = 462012500L;
        state.trunk_lcn_freq[2] = 463012500L;
        state.lcn_freq_roll = 1; /* row 0 is on air */
        seed_received_tone(&state);
        const uint32_t seeded = state.analog_rx.generation;
        reset_io_control_tune_stub(cases[i].tune_result);
        rc |= expect_int(cases[i].tag, dsd_app_command_action(cases[i].cmd), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(cases[i].tag, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(cases[i].tag, g_io_control_tune_calls, 1);
        rc |= expect_true(cases[i].tag, g_io_control_tune_freq == 462012500L);
        if (cases[i].clears) {
            rc |= expect_received_tone_cleared(cases[i].tag, &state, seeded);
        } else {
            rc |= expect_true(cases[i].tag, state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED
                                                && state.analog_rx.ctcss_tenths_hz == 1000
                                                && state.analog_rx.generation == seeded);
        }
        freeState(&state);
    }
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    return rc;
}

/*
 * Tap-to-tune is gated on the live options at drain time, not on the frontend
 * hiding the affordance, and an accepted tap tears down call state so the
 * decoder re-acquires on the new frequency instead of aging out.
 *
 * Every check here observes ui_cmd_handle_manual_tune(), which is compiled only
 * with the radio pipeline -- including the refusals, which are its early returns.
 */
#ifdef USE_RADIO
static int
test_manual_tune_trunking_gate_and_reacquisition(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 851000000L, 852000000L, 1201);
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    rc |= expect_int("manual tune under trunking queued", dsd_app_command_set_u32(DSD_APP_CMD_MANUAL_TUNE, 853125000U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("manual tune under trunking drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("manual tune under trunking never reaches the tuner", g_io_control_tune_calls, 0);
    rc |= expect_contains("manual tune under trunking explains itself", state.ui_msg, "Trunking active");
    rc |= expect_int("manual tune under trunking is a failed command", dsd_app_command_test_last_failed(), 1);
    rc |= expect_int("manual tune under trunking leaves the trunker tuned", opts.trunk_is_tuned, 1);
    rc |= expect_true("manual tune under trunking leaves the VC", state.p25_vc_freq[0] == 852000000L);
    freeState(&state);

    /* Conventional scanner mode owns the tuner too: it steps the channel map on
     * its own once the hangtime expires, so a tap accepted here would be undone
     * a few seconds later, after a toast claiming it had worked. */
    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 851000000L, 852000000L, 1201);
    opts.trunk_enable = 0;
    opts.scanner_mode = 1;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    rc |= expect_int("manual tune under the scanner queued",
                     dsd_app_command_set_u32(DSD_APP_CMD_MANUAL_TUNE, 853125000U), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("manual tune under the scanner drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("manual tune under the scanner never reaches the tuner", g_io_control_tune_calls, 0);
    rc |= expect_contains("manual tune under the scanner explains itself", state.ui_msg, "Scanner active");
    rc |= expect_int("manual tune under the scanner is a failed command", dsd_app_command_test_last_failed(), 1);
    opts.scanner_mode = 0;
    freeState(&state);

    /* The trunk scan is the third owner, refused the same way. */
    init_test_context(&opts, &state);
    opts.trunk_scan_enabled = 1;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    rc |= expect_int("manual tune under the trunk scan queued",
                     dsd_app_command_set_u32(DSD_APP_CMD_MANUAL_TUNE, 853125000U), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("manual tune under the trunk scan drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("manual tune under the trunk scan never reaches the tuner", g_io_control_tune_calls, 0);
    rc |= expect_contains("manual tune under the trunk scan explains itself", state.ui_msg, "Trunk scan active");
    rc |= expect_int("manual tune under the trunk scan is a failed command", dsd_app_command_test_last_failed(), 1);
    opts.trunk_scan_enabled = 0;
    freeState(&state);

    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 851000000L, 852000000L, 1201);
    rc |= seed_active_canonical_calls(&opts, &state, 852000000L, 1201);
    opts.trunk_enable = 0;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    rc |= expect_int("manual tune queued", dsd_app_command_set_u32(DSD_APP_CMD_MANUAL_TUNE, 853125000U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("manual tune drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("manual tune reaches the tuner once", g_io_control_tune_calls, 1);
    rc |= expect_true("manual tune targets the tapped frequency", g_io_control_tune_freq == 853125000L);
    rc |= expect_contains("manual tune reports applied", state.ui_msg, "Applied: tuned -> 853125000 Hz");
    rc |= expect_int("an applied manual tune is no failed command", dsd_app_command_test_last_failed(), 0);
    rc |= expect_call_phase("manual tune ends canonical slot 1", &state, 0U, DSD_CALL_PHASE_ENDED);
    rc |= expect_call_phase("manual tune ends canonical slot 2", &state, 1U, DSD_CALL_PHASE_ENDED);
    rc |= expect_int("manual tune clears trunk tuned", opts.trunk_is_tuned, 0);
    rc |= expect_true("manual tune clears the VC", state.p25_vc_freq[0] == 0L);
    freeState(&state);

    /* A tune the backend only accepted (no hardware confirmation yet) still has
     * to reset for re-acquisition — the same rule ui_cmd_apply_status_from_tune_rc
     * encodes for RTL_SET_FREQ. */
    init_test_context(&opts, &state);
    seed_active_canonical_calls(&opts, &state, 852000000L, 1301);
    opts.trunk_enable = 0;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_TIMEOUT);
    rc |= expect_int("pending manual tune queued", dsd_app_command_set_u32(DSD_APP_CMD_MANUAL_TUNE, 854000000U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("pending manual tune drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_contains("pending manual tune reports pending", state.ui_msg, "(pending)");
    rc |= expect_call_phase("pending manual tune still ends slot 1", &state, 0U, DSD_CALL_PHASE_ENDED);
    freeState(&state);

    /* A refused tune must leave call state alone. */
    init_test_context(&opts, &state);
    seed_active_canonical_calls(&opts, &state, 852000000L, 1401);
    opts.trunk_enable = 0;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_FAILED);
    rc |= expect_int("failed manual tune queued", dsd_app_command_set_u32(DSD_APP_CMD_MANUAL_TUNE, 855000000U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("failed manual tune drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_contains("failed manual tune reports failure", state.ui_msg, "Failed: tune -> 855000000 Hz");
    rc |= expect_call_phase("failed manual tune keeps slot 1 active", &state, 0U, DSD_CALL_PHASE_ACTIVE);
    freeState(&state);

    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    return rc;
}
#endif

#ifdef USE_RADIO
/*
 * A frequency change from the menu or a spectrum tap is a new channel: the tone heard on
 * the old one must not stay on screen (issue #522). Both commands clear it once the tune is
 * accepted -- pending included, since the hardware is already moving -- and a refused tune
 * leaves the receiver, and so its tone, where they were. These paths do not run the
 * acquisition reset, so they clear it themselves rather than waiting for the tap to notice.
 */
static int
test_retune_commands_clear_received_tone(void) {
    static const struct {
        int cmd;
        int tune_result;
        int clears;
        const char* tag;
    } cases[] = {
        {DSD_APP_CMD_RTL_SET_FREQ, RTL_STREAM_TUNE_OK, 1, "rtl set freq applied"},
        {DSD_APP_CMD_RTL_SET_FREQ, RTL_STREAM_TUNE_TIMEOUT, 1, "rtl set freq pending"},
        {DSD_APP_CMD_RTL_SET_FREQ, RTL_STREAM_TUNE_FAILED, 0, "rtl set freq refused"},
        {DSD_APP_CMD_MANUAL_TUNE, RTL_STREAM_TUNE_OK, 1, "manual tune applied"},
        {DSD_APP_CMD_MANUAL_TUNE, RTL_STREAM_TUNE_TIMEOUT, 1, "manual tune pending"},
        {DSD_APP_CMD_MANUAL_TUNE, RTL_STREAM_TUNE_FAILED, 0, "manual tune refused"},
    };

    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    /* A CTCSS tone, then a DCS code (issue #523): the same boundary clears either. */
    for (int code = 0; code < 2; code++) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            init_test_context(&opts, &state);
            opts.trunk_enable = 0;
            opts.scanner_mode = 0;
            if (code) {
                seed_received_code(&state);
            } else {
                seed_received_tone(&state);
            }
            const uint32_t seeded = state.analog_rx.generation;
            /* Name the pass too: a failure says whether the tone or the code survived. */
            char tag[96];
            DSD_SNPRINTF(tag, sizeof(tag), "%s (%s)", cases[i].tag, code ? "DCS" : "CTCSS");
            reset_io_control_tune_stub(cases[i].tune_result);
            rc |= expect_int(tag, dsd_app_command_set_u32(cases[i].cmd, 853125000U), DSD_APP_COMMAND_SUBMIT_QUEUED);
            rc |= expect_int(tag, dsd_app_drain_cmds(&opts, &state), 1);
            rc |= expect_int(tag, g_io_control_tune_calls, 1);
            if (cases[i].clears) {
                rc |= expect_received_tone_cleared(tag, &state, seeded);
            } else {
                const int kept =
                    code ? (state.analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_DCS && state.analog_rx.dcs_code == 023)
                         : state.analog_rx.ctcss_tenths_hz == 1000;
                rc |= expect_true(tag, state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED && kept
                                           && state.analog_rx.generation == seeded);
            }
            freeState(&state);
        }
    }
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    return rc;
}
#endif

/*
 * Releasing the tuner is how a frontend says "stop moving this on your own"
 * without knowing which of the two owners is active — so it has to be an
 * unconditional clear of both, safe to repeat, and it has to leave behind a
 * decoder that can be tuned by hand.
 */
static int
test_tuner_release(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

    /* Trunking: the flags go down and the follow state goes with them. Leaving
     * trunk_is_tuned or the VC set would keep the DMR sync-time stamping alive
     * and block the decoder from ever learning a new control channel. */
    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 851000000L, 852000000L, 1201);
    rc |= seed_active_canonical_calls(&opts, &state, 852000000L, 1201);
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    rc |=
        expect_int("release queued", dsd_app_command_action(DSD_APP_CMD_TUNER_RELEASE), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("release drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("release clears trunking", opts.trunk_enable, 0);
    rc |= expect_int("release clears the scanner", opts.scanner_mode, 0);
    rc |= expect_int("release clears trunk tuned", opts.trunk_is_tuned, 0);
    rc |= expect_true("release clears the VC", state.p25_vc_freq[0] == 0L && state.trunk_vc_freq[0] == 0L);
    rc |= expect_call_phase("release ends canonical slot 1", &state, 0U, DSD_CALL_PHASE_ENDED);
    rc |= expect_call_phase("release ends canonical slot 2", &state, 1U, DSD_CALL_PHASE_ENDED);
    rc |= expect_contains("release explains itself", state.ui_msg, "Automatic tuning stopped");
    rc |= expect_int("release never touches the tuner itself", g_io_control_tune_calls, 0);

    /* And it is the whole point that a tap now works where it was refused --
       which only means anything where the tap has a handler at all. */
#ifdef USE_RADIO
    rc |= expect_int("tune after release queued", dsd_app_command_set_u32(DSD_APP_CMD_MANUAL_TUNE, 853125000U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tune after release drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("tune after release reaches the tuner", g_io_control_tune_calls, 1);
    rc |= expect_contains("tune after release reports applied", state.ui_msg, "Applied: tuned -> 853125000 Hz");
#endif
    freeState(&state);

    /* Conventional scanner mode is the other owner, and a frontend cannot tell
     * the two apart — one release has to cover both, including both at once. */
    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 851000000L, 852000000L, 1201);
    opts.scanner_mode = 1;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    rc |= expect_int("release under both queued", dsd_app_command_action(DSD_APP_CMD_TUNER_RELEASE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("release under both drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("release under both clears trunking", opts.trunk_enable, 0);
    rc |= expect_int("release under both clears the scanner", opts.scanner_mode, 0);

    /* Repeating it is a no-op, not a toggle back on: the frontend re-sends this
     * whenever it re-enters explore mode and cannot know the current state. */
    rc |= expect_int("second release queued", dsd_app_command_action(DSD_APP_CMD_TUNER_RELEASE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("second release drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("second release leaves trunking off", opts.trunk_enable, 0);
    rc |= expect_int("second release leaves the scanner off", opts.scanner_mode, 0);
    freeState(&state);

    /* It carries no payload, so the setter APIs must not accept it — that is
     * what keeps the action-id list and the payload rules from drifting. */
    rc |= expect_int("release rejects a u32 payload", dsd_app_command_set_u32(DSD_APP_CMD_TUNER_RELEASE, 1U),
                     DSD_APP_COMMAND_SUBMIT_REJECTED);
    rc |= expect_int("release rejects an i32 payload", dsd_app_command_set_i32(DSD_APP_CMD_TUNER_RELEASE, 1),
                     DSD_APP_COMMAND_SUBMIT_REJECTED);

    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    return rc;
}

/* A radio input of @p audio_in_dev, an I/Q replay ("iqreplay:...") or a live device. */
static void
init_radio_context(dsd_opts* opts, dsd_state* state, const char* audio_in_dev) {
    init_test_context(opts, state);
    opts->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", audio_in_dev);
}

/*
 * Issue #575: an I/Q replay plays the tuning its capture recorded and defers every other retune unseen, so a tap, the
 * menu's frequency and the tuner release are refused while the input in force is a replay, with the reason, as a failed
 * command: none reaches the tuner, and the release leaves trunking where it was. On a live radio the same commands go
 * through.
 */
static int
test_replay_refuses_tunes_and_release(void) {
    static const char kReason[] = "An I/Q replay cannot retune.";
    static const char kReplay[] = "iqreplay:capture.iq.json";
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

#ifdef USE_RADIO
    char what[128];

    static const struct {
        int cmd;
        const char* tag;
    } tunes[] = {
        {DSD_APP_CMD_MANUAL_TUNE, "manual tune"},
        {DSD_APP_CMD_RTL_SET_FREQ, "rtl set freq"},
    };

    for (size_t i = 0; i < sizeof(tunes) / sizeof(tunes[0]); i++) {
        init_radio_context(&opts, &state, kReplay);
        reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
        DSD_SNPRINTF(what, sizeof(what), "%s during a replay", tunes[i].tag);
        rc |= expect_int(what, dsd_app_command_set_u32(tunes[i].cmd, 853125000U), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(what, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(what, g_io_control_tune_calls, 0);
        rc |= expect_contains(what, state.ui_msg, kReason);
        rc |= expect_int(what, dsd_app_command_test_last_failed(), 1);
        freeState(&state);

        init_radio_context(&opts, &state, "rtl:0");
        reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
        DSD_SNPRINTF(what, sizeof(what), "%s on a live radio", tunes[i].tag);
        rc |= expect_int(what, dsd_app_command_set_u32(tunes[i].cmd, 853125000U), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(what, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(what, g_io_control_tune_calls, 1);
        rc |= expect_true(what, strstr(state.ui_msg, kReason) == NULL);
        rc |= expect_int(what, dsd_app_command_test_last_failed(), 0);
        freeState(&state);
    }
#endif

    init_radio_context(&opts, &state, kReplay);
    seed_active_p25_voice(&opts, &state, 851000000L, 852000000L, 1201);
    opts.scanner_mode = 1;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    rc |= expect_int("release during a replay queued", dsd_app_command_action(DSD_APP_CMD_TUNER_RELEASE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("release during a replay drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_contains("release during a replay explains itself", state.ui_msg, kReason);
    rc |= expect_int("release during a replay fails", dsd_app_command_test_last_failed(), 1);
    rc |= expect_int("release during a replay keeps trunking", opts.trunk_enable, 1);
    rc |= expect_int("release during a replay keeps the scanner", opts.scanner_mode, 1);
    rc |= expect_int("release during a replay keeps trunk tuned", opts.trunk_is_tuned, 1);
    rc |= expect_true("release during a replay keeps the VC", state.p25_vc_freq[0] == 852000000L);
    rc |= expect_int("release during a replay never touches the tuner", g_io_control_tune_calls, 0);
    freeState(&state);

    init_radio_context(&opts, &state, "rtl:0");
    seed_active_p25_voice(&opts, &state, 851000000L, 852000000L, 1201);
    rc |= expect_int("release on a live radio queued", dsd_app_command_action(DSD_APP_CMD_TUNER_RELEASE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("release on a live radio drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("release on a live radio clears trunking", opts.trunk_enable, 0);
    rc |= expect_int("release on a live radio is no failed command", dsd_app_command_test_last_failed(), 0);
    rc |= expect_contains("release on a live radio explains itself", state.ui_msg, "Automatic tuning stopped");
    freeState(&state);

    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    return rc;
}
#endif

/* A decode-mode change is a boundary the received tone or code (issues #522, #523) must not
   cross: it goes through the acquisition reset, which forgets it. */
static int
test_decode_mode_change_clears_received_tone(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_decode_mode_context(&opts, &state);
    seed_received_tone(&state);
    const uint32_t seeded = state.analog_rx.generation;
    rc |= expect_int("tone mode change queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tone mode change drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_received_tone_cleared("decode mode change clears the received tone", &state, seeded);
    freeState(&state);

    /* A received DCS code (issue #523) goes the same way. */
    init_decode_mode_context(&opts, &state);
    seed_received_code(&state);
    const uint32_t seeded_code = state.analog_rx.generation;
    rc |= expect_int("code mode change queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("code mode change drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_received_tone_cleared("decode mode change clears the received code", &state, seeded_code);
    freeState(&state);
    return rc;
}

/* Issue #527: how many times one drained command said the configured tone filter has no effect. */
static int
tone_filter_warnings_from(dsd_opts* opts, dsd_state* state, int id, const void* payload, size_t size, int value) {
    static const char* const expected =
        "the tone filter (--tone-allow/--tone-block, [analog] tone_filter) has no effect";
    dsd_test_capture_stderr cap;
    char log[8192] = {0};
    if (dsd_test_capture_stderr_begin(&cap, "tone-live-warn") != 0) {
        return -1;
    }
    const int queued = payload ? dsd_app_command_submit(id, payload, size) : dsd_app_command_set_i32(id, value);
    const int drained = dsd_app_drain_cmds(opts, state);
    (void)dsd_test_capture_stderr_end(&cap);
    (void)dsd_test_capture_stderr_read(&cap, log, sizeof log);
    if (queued != DSD_APP_COMMAND_SUBMIT_QUEUED || drained != 1) {
        return -1;
    }
    int count = 0;
    for (const char* at = strstr(log, expected); at; at = strstr(at + 1, expected)) {
        count++;
    }
    return count;
}

/*
 * Issue #527: a tone filter set while AM or a digital mode is active warns once, in a running session too. A
 * decode-mode change that takes the FM monitor away from the policy in force says so once, and another digital mode
 * after it does not say it again; a loaded config that sets another policy nothing runs says so, and loading it again,
 * or with its code respelled, does not; going back to the FM monitor says nothing.
 */
static int
test_tone_filter_warns_when_a_command_leaves_it_unheard(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_decode_mode_context(&opts, &state);
    rc |= expect_int("tone live: -fA", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tone live: -fA drained", dsd_app_drain_cmds(&opts, &state), 1);
    opts.analog_tone_filter = DSD_TONE_FILTER_ALLOW;
    rc |= expect_int("tone live: list", dsd_tone_set_parse("100.0", &opts.analog_tone_set, NULL, 0), 0);

    rc |=
        expect_int("tone live: to DMR warns",
                   tone_filter_warnings_from(&opts, &state, DSD_APP_CMD_DECODE_MODE_SET, NULL, 0, DSDCFG_MODE_DMR), 1);
    rc |= expect_int("tone live: DMR to P25 stays quiet",
                     tone_filter_warnings_from(&opts, &state, DSD_APP_CMD_DECODE_MODE_SET, NULL, 0, DSDCFG_MODE_P25P1),
                     0);

    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_analog = 1;
    cfg.analog_tone_filter = DSD_TONE_FILTER_BLOCK;
    rc |= expect_int("tone live: config list", dsd_tone_set_parse("D023N", &cfg.analog_tone_set, NULL, 0), 0);
    rc |= expect_int("tone live: a config's policy under P25 warns",
                     tone_filter_warnings_from(&opts, &state, DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof cfg, 0), 1);
    rc |= expect_int("tone live: the config loaded again stays quiet",
                     tone_filter_warnings_from(&opts, &state, DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof cfg, 0), 0);
    /* D047I is D023N's other spelling: the same policy, still unheard, is not news. */
    rc |= expect_int("tone live: config list respelled", dsd_tone_set_parse("D047I", &cfg.analog_tone_set, NULL, 0), 0);
    rc |= expect_int("tone live: the config respelled stays quiet",
                     tone_filter_warnings_from(&opts, &state, DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof cfg, 0), 0);
    rc |= expect_int("tone live: the policy stands", opts.analog_tone_filter, DSD_TONE_FILTER_BLOCK);
    rc |= expect_int("tone live: back to -fA stays quiet",
                     tone_filter_warnings_from(&opts, &state, DSD_APP_CMD_DECODE_MODE_SET, NULL, 0, DSDCFG_MODE_ANALOG),
                     0);

    /* No policy, nothing to say. */
    opts.analog_tone_filter = DSD_TONE_FILTER_OFF;
    rc |=
        expect_int("tone live: no policy stays quiet",
                   tone_filter_warnings_from(&opts, &state, DSD_APP_CMD_DECODE_MODE_SET, NULL, 0, DSDCFG_MODE_DMR), 0);
    DSD_MEMSET(&opts.analog_tone_set, 0, sizeof opts.analog_tone_set);
    freeState(&state);
    return rc;
}

/*
 * Issue #527: a -Y scan with no channel map stays on the configured decode mode, which is what hears tones until a map
 * comes, as the command line weighs it at start. A map imported at runtime whose rows leave nothing hearing them says
 * so once under the FM monitor, which heard them before it; under a digital mode, where the start already said it,
 * it stays quiet. A map with an nfm row hears them, and one without after it says so again.
 */
static int
test_tone_filter_warns_when_a_channel_map_leaves_it_unheard(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_ui_cmd_queue_tone_map") != 0) {
        DSD_FPRINTF(stderr, "temp working directory setup failed: %s\n", strerror(errno));
        return 1;
    }
    static const char digital[] = "channel,frequency_hz,mode\n1,461000000,dmr\n2,851012500,p25\n";
    static const char with_nfm[] = "channel,frequency_hz,mode\n1,461000000,dmr\n2,154430000,nfm\n";
    rc |= write_file_bytes("tone_digital.csv", digital, strlen(digital));
    rc |= write_file_bytes("tone_nfm.csv", with_nfm, strlen(with_nfm));
    static const char digital_path[] = "tone_digital.csv";
    static const char nfm_path[] = "tone_nfm.csv";
    for (int analog = 1; analog >= 0; analog--) {
        init_decode_mode_context(&opts, &state);
        const int mode = analog ? DSDCFG_MODE_ANALOG : DSDCFG_MODE_DMR;
        rc |= expect_int("tone map: mode", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, mode),
                         DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int("tone map: mode drained", dsd_app_drain_cmds(&opts, &state), 1);
        opts.scanner_mode = 1;
        opts.analog_tone_filter = DSD_TONE_FILTER_ALLOW;
        rc |= expect_int("tone map: list", dsd_tone_set_parse("100.0", &opts.analog_tone_set, NULL, 0), 0);

        rc |= expect_int(analog ? "tone map: -fA, digital map warns" : "tone map: DMR, digital map stays quiet",
                         tone_filter_warnings_from(&opts, &state, DSD_APP_CMD_IMPORT_CHANNEL_MAP, digital_path,
                                                   sizeof digital_path, 0),
                         analog ? 1 : 0);
        rc |= expect_int("tone map: rows imported", state.lcn_freq_count, 2);
        rc |= expect_int("tone map: digital map again stays quiet",
                         tone_filter_warnings_from(&opts, &state, DSD_APP_CMD_IMPORT_CHANNEL_MAP, digital_path,
                                                   sizeof digital_path, 0),
                         0);
        rc |= expect_int(
            "tone map: nfm row stays quiet",
            tone_filter_warnings_from(&opts, &state, DSD_APP_CMD_IMPORT_CHANNEL_MAP, nfm_path, sizeof nfm_path, 0), 0);
        rc |= expect_int("tone map: digital map after the nfm row warns",
                         tone_filter_warnings_from(&opts, &state, DSD_APP_CMD_IMPORT_CHANNEL_MAP, digital_path,
                                                   sizeof digital_path, 0),
                         1);
        opts.scanner_mode = 0;
        opts.analog_tone_filter = DSD_TONE_FILTER_OFF;
        DSD_MEMSET(&opts.analog_tone_set, 0, sizeof opts.analog_tone_set);
        freeState(&state);
    }
    (void)remove(digital_path);
    (void)remove(nfm_path);
    if (dsd_test_temp_cwd_leave(&cwd) != 0) {
        rc = 1;
    }
    return rc;
}

/* --- The live tone-filter editor: DSD_APP_CMD_TONE_FILTER_SET (510) --- */

/* Queue one tone-filter edit through the frontends' setter and drain it, with the toast cleared first. */
static int
submit_tone_filter(dsd_opts* opts, dsd_state* state, int32_t mode, const char* list, const char* label) {
    state->ui_msg[0] = '\0';
    int rc = expect_int(label, dsd_app_command_set_tone_filter(mode, list), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/* The policy @p mode and @p set are @p want_mode and the list @p want_list, as the parser writes it back. */
static int
expect_tone_policy(const char* tag, int mode, const dsd_tone_set* set, int want_mode, const char* want_list) {
    char list[DSD_TONE_LIST_TEXT_MAX + 1] = "";
    (void)dsd_tone_set_format(set, list, sizeof list);
    int rc = expect_int(tag, mode, want_mode);
    rc |= expect_str(tag, list, want_list);
    return rc;
}

static int
expect_configured_tone_policy(const char* tag, const dsd_opts* opts, const dsd_state* state, int want_mode,
                              const char* want_list) {
    int mode = -1;
    dsd_tone_set set;
    DSD_MEMSET(&set, 0, sizeof set);
    dsd_scan_mode_configured_tone_policy(opts, state, &mode, &set);
    return expect_tone_policy(tag, mode, &set, want_mode, want_list);
}

/* The analog FM monitor, where the tone filter acts, playing to the null output. */
static void
init_tone_filter_context(dsd_opts* opts, dsd_state* state) {
    init_decode_mode_context(opts, state);
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
}

/*
 * The editor sets the whole configured policy, mode and list, validated by the list parser: allow and block take a
 * list, off keeps the one it is given (or none), and the toast names what is now configured. A refused edit -- a comma
 * list, a list policy without a list, no such mode, a malformed payload -- changes nothing and says why without
 * repeating the text. Two queued edits each run, so a refused second never discards the first, and a list the payload
 * cannot hold is refused at submission rather than cut short.
 */
static int
test_tone_filter_set_edits_the_configured_policy(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_tone_filter_context(&opts, &state);
    rc |= submit_tone_filter(&opts, &state, DSD_TONE_FILTER_ALLOW, "100/d023", "tone set: allow");
    rc |= expect_tone_policy("tone set: allow in force", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_ALLOW, "100.0/D023N");
    rc |= expect_str("tone set: allow toast", state.ui_msg, "Applied: Tone filter -> allow 100.0 Hz/D023N");
    rc |= submit_tone_filter(&opts, &state, DSD_TONE_FILTER_BLOCK, "D023I", "tone set: block");
    rc |= expect_tone_policy("tone set: block in force", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_BLOCK, "D023I");
    rc |= expect_str("tone set: block toast", state.ui_msg, "Applied: Tone filter -> block D023I");
    rc |= submit_tone_filter(&opts, &state, DSD_TONE_FILTER_OFF, "D023I", "tone set: off keeping the list");
    rc |= expect_tone_policy("tone set: off keeps its list", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_OFF, "D023I");
    rc |= expect_str("tone set: off toast", state.ui_msg, "Applied: Tone filter -> off");
    rc |= submit_tone_filter(&opts, &state, DSD_TONE_FILTER_OFF, "", "tone set: off without a list");
    rc |= expect_tone_policy("tone set: off clears the list", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_OFF, "");
    rc |= submit_tone_filter(&opts, &state, DSD_TONE_FILTER_ALLOW, "67.0/100.0", "tone set: allow again");

    static const struct {
        int32_t mode;
        const char* list;
        const char* toast;
    } refused[] = {
        {DSD_TONE_FILTER_ALLOW, "100.0,67.0", "Refused: tone filter: use / between entries, not commas"},
        {DSD_TONE_FILTER_BLOCK, "",
         "Refused: tone filter: block needs a list of CTCSS tones or DCS codes, e.g. 67.0/100.0/D023N"},
        {DSD_TONE_FILTER_BLOCK, "D023N/xyzzy",
         "Refused: tone filter: entry 2 is not a standard CTCSS tone or DCS code"},
        {DSD_TONE_FILTER_OFF, "100/100.0", "Refused: tone filter: entry 2 repeats entry 1 (100.0 Hz)"},
        {7, "100.0", "Refused: tone filter: the mode must be off, allow or block"},
    };

    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        rc |= submit_tone_filter(&opts, &state, refused[i].mode, refused[i].list, refused[i].toast);
        rc |= expect_str(refused[i].toast, state.ui_msg, refused[i].toast);
        rc |= expect_tone_policy(refused[i].toast, opts.analog_tone_filter, &opts.analog_tone_set,
                                 DSD_TONE_FILTER_ALLOW, "67.0/100.0");
    }

    /* Malformed payloads are refused whole: a short one, and a list with no terminator in its field. */
    int32_t bare_mode = DSD_TONE_FILTER_OFF;
    rc |= expect_int("tone set: short payload queued",
                     dsd_app_command_submit(DSD_APP_CMD_TONE_FILTER_SET, &bare_mode, sizeof bare_mode),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tone set: short payload drained", dsd_app_drain_cmds(&opts, &state), 1);
    dsd_app_tone_filter_payload unterminated;
    DSD_MEMSET(&unterminated, '7', sizeof unterminated);
    unterminated.mode = DSD_TONE_FILTER_OFF;
    rc |= expect_int("tone set: unterminated payload queued",
                     dsd_app_command_submit(DSD_APP_CMD_TONE_FILTER_SET, &unterminated, sizeof unterminated),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tone set: unterminated payload drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_tone_policy("tone set: malformed payloads change nothing", opts.analog_tone_filter,
                             &opts.analog_tone_set, DSD_TONE_FILTER_ALLOW, "67.0/100.0");

    /* A list longer than the payload holds is refused at submission, never cut to a list nobody typed. */
    char longest[DSD_APP_TONE_FILTER_LIST_SIZE + 1];
    DSD_MEMSET(longest, '/', sizeof longest);
    longest[sizeof longest - 1U] = '\0';
    rc |= expect_int("tone set: overlong list refused", dsd_app_command_set_tone_filter(DSD_TONE_FILTER_ALLOW, longest),
                     DSD_APP_COMMAND_SUBMIT_REJECTED);
    rc |= expect_int("tone set: overlong list queued nothing", dsd_app_drain_cmds(&opts, &state), 0);

    /* Each of two queued edits is judged on its own: a refused second (a typo) leaves the first applied. */
    rc |= expect_int("tone set: first edit queued", dsd_app_command_set_tone_filter(DSD_TONE_FILTER_BLOCK, "D754N"),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tone set: second edit queued apart",
                     dsd_app_command_set_tone_filter(DSD_TONE_FILTER_ALLOW, "67,100"), DSD_APP_COMMAND_SUBMIT_QUEUED);
    state.ui_msg[0] = '\0';
    rc |= expect_int("tone set: both edits drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_tone_policy("tone set: the first edit stands", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_BLOCK, "D754N");
    rc |= expect_str("tone set: the second is refused", state.ui_msg,
                     "Refused: tone filter: use / between entries, not commas");
    /* Two valid edits both run, the newer last. */
    rc |= expect_int("tone set: third edit queued", dsd_app_command_set_tone_filter(DSD_TONE_FILTER_BLOCK, "D023N"),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tone set: fourth edit queued apart",
                     dsd_app_command_set_tone_filter(DSD_TONE_FILTER_ALLOW, "82.5"), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tone set: two valid edits drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_tone_policy("tone set: the newer edit stands", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_ALLOW, "82.5");
    opts.analog_tone_filter = DSD_TONE_FILTER_OFF;
    DSD_MEMSET(&opts.analog_tone_set, 0, sizeof opts.analog_tone_set);
    freeState(&state);
    return rc;
}

/*
 * The configured-view rule: while an nfm row with its own tone policy is on air, an edit changes the configured policy,
 * the row keeps its own, and the toast says the row overrides the edit. A save writes the configured policy, never the
 * row's; the next row without a policy of its own, and the row's departure, run the edit. A refused edit under the row
 * changes neither.
 */
static int
test_tone_filter_set_under_a_tone_row(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_tone_filter_context(&opts, &state);
    opts.analog_tone_filter = DSD_TONE_FILTER_ALLOW;
    rc |= expect_int("tone row: configured list", dsd_tone_set_parse("100.0", &opts.analog_tone_set, NULL, 0), 0);
    rc |= expect_int("tone row: scope", dsd_scan_mode_begin(&opts, &state), 0);
    rc |= expect_int("tone row: nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM), 0);
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_TONE;
    row.tone_filter = DSD_TONE_FILTER_BLOCK;
    rc |= expect_int("tone row: row list", dsd_tone_set_parse("67.0", &row.tone_set, NULL, 0), 0);
    rc |= expect_int("tone row: row installed", dsd_scan_mode_options(&opts, &state, &row), 0);

    rc |= submit_tone_filter(&opts, &state, DSD_TONE_FILTER_ALLOW, "D754N", "tone row: shadowed edit");
    rc |= expect_tone_policy("tone row: the row keeps its own", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_BLOCK, "67.0");
    rc |= expect_configured_tone_policy("tone row: the edit is the configured policy", &opts, &state,
                                        DSD_TONE_FILTER_ALLOW, "D754N");
    rc |= expect_str("tone row: shadowed toast", state.ui_msg,
                     "Default tone filter -> allow D754N; this channel overrides it (block 67.0 Hz)");
    dsdneoUserConfig saved;
    dsd_snapshot_opts_to_user_config(&opts, &state, &saved);
    rc |= expect_tone_policy("tone row: a save writes the configured policy", saved.analog_tone_filter,
                             &saved.analog_tone_set, DSD_TONE_FILTER_ALLOW, "D754N");

    rc |= submit_tone_filter(&opts, &state, DSD_TONE_FILTER_BLOCK, "", "tone row: refused edit");
    rc |= expect_str("tone row: refused toast", state.ui_msg,
                     "Refused: tone filter: block needs a list of CTCSS tones or DCS codes, e.g. 67.0/100.0/D023N");
    rc |= expect_tone_policy("tone row: a refusal leaves the row", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_BLOCK, "67.0");
    rc |= expect_configured_tone_policy("tone row: a refusal leaves the configured policy", &opts, &state,
                                        DSD_TONE_FILTER_ALLOW, "D754N");

    /* The next row without a policy of its own runs the edit; one with its own shadows the next edit again. */
    rc |= expect_int("tone row: row without a policy", dsd_scan_mode_options(&opts, &state, NULL), 0);
    rc |= expect_tone_policy("tone row: the edit runs", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_ALLOW, "D754N");
    rc |= submit_tone_filter(&opts, &state, DSD_TONE_FILTER_BLOCK, "D023N", "tone row: edit with no row policy");
    rc |= expect_tone_policy("tone row: an unshadowed edit is in force", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_BLOCK, "D023N");
    rc |= expect_str("tone row: unshadowed toast", state.ui_msg, "Applied: Tone filter -> block D023N");
    rc |= expect_int("tone row: row reinstalled", dsd_scan_mode_options(&opts, &state, &row), 0);
    /* Off that keeps the list (the terminal's Off) keeps the configured list, never the row's own on air. */
    rc |= expect_int("tone row: off queued", dsd_app_command_set_tone_filter_mode(DSD_TONE_FILTER_OFF),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tone row: off drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_configured_tone_policy("tone row: off keeps the configured list", &opts, &state, DSD_TONE_FILTER_OFF,
                                        "D023N");
    rc |= expect_tone_policy("tone row: off leaves the row's own", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_BLOCK, "67.0");
    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_tone_policy("tone row: departure restores the configured policy", opts.analog_tone_filter,
                             &opts.analog_tone_set, DSD_TONE_FILTER_OFF, "D023N");
    opts.analog_tone_filter = DSD_TONE_FILTER_OFF;
    DSD_MEMSET(&opts.analog_tone_set, 0, sizeof opts.analog_tone_set);
    freeState(&state);
    return rc;
}

/* Queue one mode-only edit (keep_list, the terminal's Off) and drain it, with the toast cleared first. */
static int
submit_tone_filter_mode(dsd_opts* opts, dsd_state* state, int32_t mode, const char* label) {
    state->ui_msg[0] = '\0';
    int rc = expect_int(label, dsd_app_command_set_tone_filter_mode(mode), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/*
 * The terminal's Off sets the mode alone (keep_list): the list it keeps is the configured one when the edit runs on the
 * decoder thread, so an edit or a loaded config queued before it is what Off keeps, never a frontend's older copy of
 * the list. Allow and block can keep the list too, and are refused, changing nothing, when none is configured. A
 * keep_list payload that also carries a list, or a keep_list other than 0 or 1, is malformed and changes nothing.
 */
static int
test_tone_filter_mode_keeps_the_decoder_list(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_tone_filter_context(&opts, &state);
    rc |= submit_tone_filter(&opts, &state, DSD_TONE_FILTER_ALLOW, "100.0", "tone keep: configured");

    /* An edit queued before Off: Off keeps the list that edit set. */
    rc |= expect_int("tone keep: edit queued", dsd_app_command_set_tone_filter(DSD_TONE_FILTER_ALLOW, "67.0"),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tone keep: off after the edit queued", dsd_app_command_set_tone_filter_mode(DSD_TONE_FILTER_OFF),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    state.ui_msg[0] = '\0';
    rc |= expect_int("tone keep: edit and off drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_tone_policy("tone keep: off keeps the queued edit's list", opts.analog_tone_filter,
                             &opts.analog_tone_set, DSD_TONE_FILTER_OFF, "67.0");
    rc |= expect_str("tone keep: off toast", state.ui_msg, "Applied: Tone filter -> off");

    /* A config queued before Off: Off keeps the list it loaded. */
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_analog = 1;
    cfg.analog_tone_filter = DSD_TONE_FILTER_ALLOW;
    rc |= expect_int("tone keep: config list", dsd_tone_set_parse("82.5", &cfg.analog_tone_set, NULL, 0), 0);
    rc |= expect_true("tone keep: config queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("tone keep: off after the config queued",
                     dsd_app_command_set_tone_filter_mode(DSD_TONE_FILTER_OFF), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tone keep: config and off drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_tone_policy("tone keep: off keeps the loaded list", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_OFF, "82.5");

    /* Allow on the kept list; with none configured, block is refused and changes nothing, and off stays off. */
    rc |= submit_tone_filter_mode(&opts, &state, DSD_TONE_FILTER_ALLOW, "tone keep: allow");
    rc |= expect_tone_policy("tone keep: allow on the kept list", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_ALLOW, "82.5");
    rc |= submit_tone_filter(&opts, &state, DSD_TONE_FILTER_OFF, "", "tone keep: list cleared");
    rc |= submit_tone_filter_mode(&opts, &state, DSD_TONE_FILTER_BLOCK, "tone keep: block without a list");
    rc |= expect_str("tone keep: block without a list refused", state.ui_msg,
                     "Refused: tone filter: block needs a list of CTCSS tones or DCS codes, e.g. 67.0/100.0/D023N");
    rc |= expect_tone_policy("tone keep: the refusal changes nothing", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_OFF, "");
    rc |= submit_tone_filter_mode(&opts, &state, DSD_TONE_FILTER_OFF, "tone keep: off without a list");
    rc |= expect_tone_policy("tone keep: off without a list", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_OFF, "");
    rc |= submit_tone_filter_mode(&opts, &state, 7, "tone keep: no such mode");
    rc |= expect_str("tone keep: no such mode refused", state.ui_msg,
                     "Refused: tone filter: the mode must be off, allow or block");

    /* Malformed: keep_list with a list, and a keep_list that is neither 0 nor 1. */
    rc |= submit_tone_filter(&opts, &state, DSD_TONE_FILTER_ALLOW, "100.0", "tone keep: allow again");
    dsd_app_tone_filter_payload payload;
    DSD_MEMSET(&payload, 0, sizeof payload);
    payload.mode = DSD_TONE_FILTER_OFF;
    payload.keep_list = 1;
    DSD_SNPRINTF(payload.list, sizeof payload.list, "%s", "67.0");
    rc |= expect_int("tone keep: keep with a list queued",
                     dsd_app_command_submit(DSD_APP_CMD_TONE_FILTER_SET, &payload, sizeof payload),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    payload.keep_list = 2;
    payload.list[0] = '\0';
    rc |= expect_int("tone keep: keep of 2 queued",
                     dsd_app_command_submit(DSD_APP_CMD_TONE_FILTER_SET, &payload, sizeof payload),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tone keep: malformed drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_tone_policy("tone keep: malformed payloads change nothing", opts.analog_tone_filter,
                             &opts.analog_tone_set, DSD_TONE_FILTER_ALLOW, "100.0");
    opts.analog_tone_filter = DSD_TONE_FILTER_OFF;
    DSD_MEMSET(&opts.analog_tone_set, 0, sizeof opts.analog_tone_set);
    freeState(&state);
    return rc;
}

/*
 * A tone-filter edit is not a decoder change. Frame sync writes the detected Phase 2 polarity into dsd_opts while a P25
 * row of a mixed list is on air; had the command suspended and re-applied the scope, that live value would read as an
 * acquisition change and end the followed call. The edit reaches the configured policy and, the row setting none of its
 * own, dsd_opts.
 */
static int
test_tone_filter_edit_keeps_live_acquisition(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_decode_mode_context(&opts, &state);
    rc |= expect_int("tone p25: scope", dsd_scan_mode_begin(&opts, &state), 0);
    rc |= expect_int("tone p25: row entered", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_P25), 0);
    rc |= expect_int("tone p25: row options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    opts.inverted_p2 = 1;
    opts.trunk_is_tuned = 1;
    state.trunk_vc_freq[0] = 851012500;
    rc |= submit_tone_filter(&opts, &state, DSD_TONE_FILTER_BLOCK, "D023N", "tone p25: edit");
    rc |= expect_int("tone p25: keeps the detected polarity", opts.inverted_p2, 1);
    rc |= expect_int("tone p25: keeps the followed call", opts.trunk_is_tuned, 1);
    rc |= expect_true("tone p25: keeps the voice channel", state.trunk_vc_freq[0] == 851012500);
    rc |= expect_int("tone p25: the row stays", dsd_scan_mode_active(&state), DSD_SCAN_MODE_P25);
    rc |= expect_tone_policy("tone p25: the edit is in force", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_BLOCK, "D023N");
    rc |= expect_configured_tone_policy("tone p25: the edit is configured", &opts, &state, DSD_TONE_FILTER_BLOCK,
                                        "D023N");
    dsd_scan_mode_leave(&opts, &state);
    opts.trunk_is_tuned = 0;
    opts.analog_tone_filter = DSD_TONE_FILTER_OFF;
    DSD_MEMSET(&opts.analog_tone_set, 0, sizeof opts.analog_tone_set);
    freeState(&state);
    return rc;
}

/* An edit that sets a list policy on a session where nothing runs detection (a digital mode) says once that it has no
   effect, as a loaded config does; the same policy again, or respelled, says nothing new. */
static int
test_tone_filter_set_warns_when_unheard(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_decode_mode_context(&opts, &state);
    rc |= expect_int("tone edit warn: DMR", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, DSDCFG_MODE_DMR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tone edit warn: DMR drained", dsd_app_drain_cmds(&opts, &state), 1);
    dsd_app_tone_filter_payload payload;
    DSD_MEMSET(&payload, 0, sizeof payload);
    payload.mode = DSD_TONE_FILTER_ALLOW;
    DSD_SNPRINTF(payload.list, sizeof payload.list, "%s", "D023N");
    rc |= expect_int("tone edit warn: a list policy under DMR warns",
                     tone_filter_warnings_from(&opts, &state, DSD_APP_CMD_TONE_FILTER_SET, &payload, sizeof payload, 0),
                     1);
    DSD_SNPRINTF(payload.list, sizeof payload.list, "%s", "D047I");
    rc |= expect_int("tone edit warn: respelled stays quiet",
                     tone_filter_warnings_from(&opts, &state, DSD_APP_CMD_TONE_FILTER_SET, &payload, sizeof payload, 0),
                     0);
    rc |= expect_tone_policy("tone edit warn: the edit stands", opts.analog_tone_filter, &opts.analog_tone_set,
                             DSD_TONE_FILTER_ALLOW, "D047I");
    payload.mode = DSD_TONE_FILTER_OFF;
    rc |= expect_int("tone edit warn: off stays quiet",
                     tone_filter_warnings_from(&opts, &state, DSD_APP_CMD_TONE_FILTER_SET, &payload, sizeof payload, 0),
                     0);
    DSD_MEMSET(&opts.analog_tone_set, 0, sizeof opts.analog_tone_set);
    freeState(&state);
    return rc;
}

/*
 * A menu input switch closes the input in use and opens the new one (issue #634), a new receiver: the tone the old input
 * carried goes with it (issue #522). One that cannot open its input changes nothing, the received tone included, and
 * fails the command.
 */
static int
test_input_switch_clears_received_tone(void) {
    int rc = 0;
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_ui_input_switch_tone") != 0) {
        return expect_true("input switch temp directory", 0);
    }
    static const unsigned char k_symbols[16] = {0};
    rc |= write_pcm16_wav("input.wav", 48000U, 8U);
    rc |= write_file_bytes("symbols.sym", k_symbols, sizeof k_symbols);
    install_udp_input_hooks();
    const int udp_port = free_loopback_udp_port();
    rc |= expect_true("a free loopback udp port", udp_port > 0);

    static const struct {
        const char* value; /**< string payload, or NULL for none */
        const char* tag;
        int cmd;
        int input_type;
    } cases[] = {
        {"input.wav", "wav input clears the received tone", DSD_APP_CMD_INPUT_WAV_SET, AUDIO_IN_WAV},
        {NULL, "udp input clears the received tone", DSD_APP_CMD_UDP_INPUT_CFG, AUDIO_IN_UDP},
        {"symbols.sym", "symbol stream input clears the received tone", DSD_APP_CMD_INPUT_SYM_STREAM_SET,
         AUDIO_IN_SYMBOL_FLT},
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
        {NULL, "pulse input clears the received tone", DSD_APP_CMD_INPUT_SET_PULSE, AUDIO_IN_PULSE},
        {"source0", "named pulse source clears the received tone", DSD_APP_CMD_PULSE_IN_SET, AUDIO_IN_PULSE},
#endif
    };

    static dsd_opts opts;
    static dsd_state state;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        init_test_context(&opts, &state);
        seed_received_tone(&state);
        const uint32_t seeded = state.analog_rx.generation;
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
        arm_open_audio_input_stub(1, 0);
#endif
        int queued = 0;
        if (cases[i].cmd == DSD_APP_CMD_UDP_INPUT_CFG) {
            queued = post_host_port(cases[i].cmd, "127.0.0.1", udp_port);
        } else if (cases[i].value) {
            queued = post_string(cases[i].cmd, cases[i].value);
        } else {
            queued = post_empty(cases[i].cmd);
        }
        rc |= expect_int(cases[i].tag, queued, DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(cases[i].tag, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(cases[i].tag, dsd_app_command_test_last_failed(), 0);
        rc |= expect_int(cases[i].tag, opts.audio_in_type, cases[i].input_type);
        /* The input is open, not only named (issue #634). */
        if (cases[i].input_type == AUDIO_IN_WAV) {
            rc |= expect_true(cases[i].tag, opts.audio_in_file != NULL);
        } else if (cases[i].input_type == AUDIO_IN_UDP) {
            rc |= expect_true(cases[i].tag, opts.udp_in_ctx != NULL);
        } else if (cases[i].input_type == AUDIO_IN_SYMBOL_FLT) {
            rc |= expect_true(cases[i].tag, opts.symbolfile != NULL);
        }
        rc |= expect_received_tone_cleared(cases[i].tag, &state, seeded);
        rc |= expect_int(cases[i].tag, state.input_boundary, 1);
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
        arm_open_audio_input_stub(0, 0);
#endif
        closeAudioInDevice(&opts);
        freeState(&state);
    }

    /* A file that does not open: the input, its tone and its stream stay. */
    init_test_context(&opts, &state);
    seed_received_tone(&state);
    const uint32_t seeded = state.analog_rx.generation;
    const uint32_t stream = opts.pcm_input_generation;
    const int type_before = opts.audio_in_type;
    rc |= expect_int("failed wav input queued", post_string(DSD_APP_CMD_INPUT_WAV_SET, "missing.wav"),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("failed wav input drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("failed wav input fails the command", dsd_app_command_test_last_failed(), 1);
    rc |= expect_int("failed wav input keeps the input", opts.audio_in_type, type_before);
    rc |= expect_received_tone_kept("failed wav input keeps the received tone", &state, seeded);
    rc |= expect_true("failed wav input is no new stream", opts.pcm_input_generation == stream);
    rc |= expect_int("failed wav input crosses no boundary", state.input_boundary, 0);
    rc |= expect_contains("failed wav input toast", state.ui_msg, "Failed: WAV input -> missing.wav");
    freeState(&state);

    clear_net_audio_input_hooks();
    (void)remove("input.wav");
    (void)remove("symbols.sym");
    rc |= expect_int("input switch temp directory removed", dsd_test_temp_cwd_leave(&cwd), 0);
    return rc;
}

/*
 * Input > Switch source > Symbol file for a capture (.bin): the WAV that played closes, and the capture is replayed at
 * symbol pace as -i x.bin replays one (issue #634). One that does not open leaves the WAV playing.
 */
static int
test_symbol_in_open_replaces_the_playback(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_ui_symbol_in_open") != 0) {
        return expect_true("symbol in temp directory", 0);
    }
    static const unsigned char k_capture[16] = {0};
    rc |= write_pcm16_wav("voice.wav", 48000U, 8U);
    rc |= write_file_bytes("capture.bin", k_capture, sizeof k_capture);
    init_test_context(&opts, &state);
    (void)post_string(DSD_APP_CMD_INPUT_WAV_SET, "voice.wav");
    rc |= expect_int("wav playback drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("wav playback runs", opts.audio_in_type, AUDIO_IN_WAV);

    (void)post_string(DSD_APP_CMD_SYMBOL_IN_OPEN, "missing.bin");
    rc |= expect_int("missing capture drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("missing capture fails", dsd_app_command_test_last_failed(), 1);
    rc |= expect_true("missing capture keeps the wav", opts.audio_in_type == AUDIO_IN_WAV && opts.audio_in_file);

    (void)post_string(DSD_APP_CMD_SYMBOL_IN_OPEN, "capture.bin");
    rc |= expect_int("capture drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("capture replays", opts.audio_in_type, AUDIO_IN_SYMBOL_BIN);
    rc |= expect_true("capture open, wav closed", opts.symbolfile != NULL && opts.audio_in_file == NULL);
    rc |= expect_int("capture paced", state.use_throttle, 1);
    rc |= expect_str("capture named", opts.audio_in_dev, "capture.bin");
    rc |= expect_contains("capture toast", state.ui_msg, "Applied: Symbol input -> capture.bin");
    closeAudioInDevice(&opts);
    freeState(&state);
    (void)remove("voice.wav");
    (void)remove("capture.bin");
    rc |= expect_int("symbol in temp directory removed", dsd_test_temp_cwd_leave(&cwd), 0);
    return rc;
}

/*
 * A scan's timing snapshots belong to the input that runs, so a menu input switch waits for the scan to stop (issue
 * #634): -Y with rows, or --trunk-scan with targets. A scanner switched on with no rows to visit is no scan. The engine
 * modes that read no switched input refuse every switch.
 */
static int
test_input_switches_refused_while_a_scan_is_in_force(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_ui_switch_scan") != 0) {
        return expect_true("scan refusal temp directory", 0);
    }
    rc |= write_pcm16_wav("voice.wav", 48000U, 8U);

    init_test_context(&opts, &state);
    opts.scanner_mode = 1;
    state.lcn_freq_count = 2;
    (void)post_string(DSD_APP_CMD_INPUT_WAV_SET, "voice.wav");
    rc |= expect_int("-Y with rows drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("-Y with rows refuses", opts.audio_in_type, AUDIO_IN_PULSE);
    rc |= expect_contains("-Y with rows toast", state.ui_msg, "Unsupported: stop the scan first");
    state.lcn_freq_count = 0;
    (void)post_string(DSD_APP_CMD_INPUT_WAV_SET, "voice.wav");
    rc |= expect_int("a scanner with no rows drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("a scanner with no rows is no scan", opts.audio_in_type, AUDIO_IN_WAV);
    opts.scanner_mode = 0;

    /* Stop replay is a switch too. */
    opts.trunk_scan_enabled = 1;
    state.trunk_scan_target_count = 1U;
    (void)post_empty(DSD_APP_CMD_STOP_PLAYBACK);
    rc |= expect_int("trunk scan drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("trunk scan refuses stop replay", opts.audio_in_type, AUDIO_IN_WAV);
    rc |= expect_contains("trunk scan toast", state.ui_msg, "Unsupported: stop the scan first");
    opts.trunk_scan_enabled = 0;
    state.trunk_scan_target_count = 0U;

    opts.playfiles = 1;
    (void)post_empty(DSD_APP_CMD_INPUT_SET_PULSE);
    rc |= expect_int("mbe playback drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("mbe playback refuses", opts.audio_in_type, AUDIO_IN_WAV);
    rc |= expect_contains("mbe playback toast", state.ui_msg, "Unsupported: MBE playback");
    opts.playfiles = 0;

    closeAudioInDevice(&opts);
    freeState(&state);
    (void)remove("voice.wav");
    rc |= expect_int("scan refusal temp directory removed", dsd_test_temp_cwd_leave(&cwd), 0);
    return rc;
}

/*
 * Replay last replays the capture closed most recently (issue #634): one a stop closed, one a new capture replaced,
 * one a rotation replaced. Stopping a capture leaves the running input's name alone.
 */
static int
test_capture_close_records_the_last_capture(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_ui_capture_last") != 0) {
        return expect_true("capture temp directory", 0);
    }
    init_test_context(&opts, &state);
    (void)post_string(DSD_APP_CMD_SYMCAP_OPEN, "one.bin");
    (void)post_string(DSD_APP_CMD_SYMCAP_OPEN, "two.bin");
    rc |= expect_int("two captures drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_str("a new capture records the one it replaced", opts.symbol_capture_last, "one.bin");

    opts.symbol_out_file_is_auto = 1;
    opts.symbol_out_file_creation_time = dsd_decode_time() - 4000;
    rotate_symbol_out_file(&opts, &state);
    rc |= expect_str("a rotation records the capture it closed", opts.symbol_capture_last, "two.bin");
    char rotated[sizeof opts.symbol_out_file];
    DSD_SNPRINTF(rotated, sizeof rotated, "%s", opts.symbol_out_file);
    rc |= expect_true("the rotation opened another capture",
                      opts.symbol_out_f != NULL && strcmp(rotated, "two.bin") != 0);

    (void)post_empty(DSD_APP_CMD_SYMCAP_STOP);
    rc |= expect_int("stop drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("a stop records the capture it closed", opts.symbol_capture_last, rotated);
    rc |= expect_str("a stop leaves the input's name", opts.audio_in_dev, "pulse");
    freeState(&state);
    (void)remove("one.bin");
    (void)remove("two.bin");
    (void)remove(rotated);
    rc |= expect_int("capture temp directory removed", dsd_test_temp_cwd_leave(&cwd), 0);
    return rc;
}

/*
 * A UDP switch to a port another socket holds: the UDP input that ran had to be stopped to free its own port, and is
 * started again on it, a new stream on the old input, so the received tone goes and the engine ends the reception
 * (issue #634). The command fails and says so.
 */
static int
test_a_refused_udp_bind_restarts_the_old_input(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    install_udp_input_hooks();
    init_test_context(&opts, &state);
    const int first = free_loopback_udp_port();
    int held = -1;
    const dsd_socket_t holder = bind_loopback_udp(&held);
    rc |= expect_true("loopback ports", first > 0 && held > 0 && holder != DSD_INVALID_SOCKET);
    (void)post_host_port(DSD_APP_CMD_UDP_INPUT_CFG, "127.0.0.1", first);
    rc |= expect_int("udp drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("udp runs", opts.audio_in_type, AUDIO_IN_UDP);
    state.input_boundary = 0;
    seed_received_tone(&state);
    const uint32_t seeded = state.analog_rx.generation;
    (void)post_host_port(DSD_APP_CMD_UDP_INPUT_CFG, "127.0.0.1", held);
    rc |= expect_int("held port drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("held port fails", dsd_app_command_test_last_failed(), 1);
    rc |= expect_true("the old udp runs again", opts.audio_in_type == AUDIO_IN_UDP && opts.udp_in_ctx != NULL);
    rc |= expect_int("on its own port", opts.udp_in_portno, first);
    rc |= expect_contains("restart toast", state.ui_msg, "the UDP input before it was restarted");
    rc |= expect_received_tone_cleared("a restarted udp input is a new reception", &state, seeded);
    rc |= expect_int("the engine ends the reception", state.input_boundary, 1);
    if (holder != DSD_INVALID_SOCKET) {
        dsd_socket_close(holder);
    }
    closeAudioInDevice(&opts);
    clear_net_audio_input_hooks();
    freeState(&state);
    return rc;
}

/*
 * A config's reopen of the running input goes through the input switch (issue #634): a UDP endpoint that does not bind
 * restarts the running one and puts back the name the config wrote, which hides the boundary from the received-tone
 * check, so the reopen takes it; a file config naming a named pipe is refused at runtime, so the decoder thread never
 * waits in its open.
 */
static int
test_config_reopen_failures_keep_the_input(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    install_udp_input_hooks();
    init_test_context(&opts, &state);
    const int first = free_loopback_udp_port();
    int held = -1;
    const dsd_socket_t holder = bind_loopback_udp(&held);
    (void)post_host_port(DSD_APP_CMD_UDP_INPUT_CFG, "127.0.0.1", first);
    rc |= expect_int("udp drained", dsd_app_drain_cmds(&opts, &state), 1);
    char running[sizeof opts.audio_in_dev];
    DSD_SNPRINTF(running, sizeof running, "%s", opts.audio_in_dev);
    state.input_boundary = 0;
    seed_received_tone(&state);
    const uint32_t seeded = state.analog_rx.generation;
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_UDP;
    DSD_SNPRINTF(cfg.udp_addr, sizeof cfg.udp_addr, "%s", "127.0.0.1");
    cfg.udp_port = held;
    rc |= expect_true("udp config queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("udp config drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("the running udp runs on", opts.audio_in_type == AUDIO_IN_UDP && opts.udp_in_ctx != NULL);
    rc |= expect_int("on its own port", opts.udp_in_portno, first);
    rc |= expect_str("its name put back", opts.audio_in_dev, running);
    rc |= expect_received_tone_cleared("a restarted udp input is a new reception", &state, seeded);
    rc |= expect_int("the engine ends the reception", state.input_boundary, 1);
    if (holder != DSD_INVALID_SOCKET) {
        dsd_socket_close(holder);
    }
    closeAudioInDevice(&opts);
    clear_net_audio_input_hooks();
    freeState(&state);

#if !DSD_PLATFORM_WIN_NATIVE
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_ui_config_fifo") != 0) {
        return rc | expect_true("config fifo temp directory", 0);
    }
    rc |= write_pcm16_wav("voice.wav", 48000U, 8U);
    rc |= expect_int("fifo created", mkfifo("pipe.raw", 0600), 0);
    init_test_context(&opts, &state);
    (void)post_string(DSD_APP_CMD_INPUT_WAV_SET, "voice.wav");
    rc |= expect_int("wav drained", dsd_app_drain_cmds(&opts, &state), 1);
    SNDFILE* const playing = opts.audio_in_file;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_FILE;
    DSD_SNPRINTF(cfg.file_path, sizeof cfg.file_path, "%s", "pipe.raw");
    rc |= expect_true("fifo config queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("fifo config drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("the wav plays on", opts.audio_in_type == AUDIO_IN_WAV && opts.audio_in_file == playing);
    rc |= expect_str("the wav keeps its name", opts.audio_in_dev, "voice.wav");
    closeAudioInDevice(&opts);
    freeState(&state);
    (void)remove("voice.wav");
    (void)remove("pipe.raw");
    rc |= expect_int("config fifo temp directory removed", dsd_test_temp_cwd_leave(&cwd), 0);
#endif
    return rc;
}

/* A config whose file input does not open is not applied, the file rate it staged for later file opens included: a
   headerless file the menu opens next runs at the session's raw rate, not the failed config's (issue #634). */
static int
test_failed_config_reopen_stages_no_file_rate(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static dsdneoUserConfig cfg;
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_ui_config_staged_rate") != 0) {
        return expect_true("staged rate temp directory", 0);
    }
    static const unsigned char k_pcm[64] = {0};
    rc |= write_pcm16_wav("voice.wav", 48000U, 8U);
    rc |= write_file_bytes("voice.pcm", k_pcm, sizeof k_pcm);
    init_test_context(&opts, &state);
    (void)post_string(DSD_APP_CMD_INPUT_WAV_SET, "voice.wav");
    rc |= expect_int("wav drained", dsd_app_drain_cmds(&opts, &state), 1);
    const int staged_before = opts.staged_file_sample_rate;
    const int sps_before = state.samplesPerSymbol;

    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_FILE;
    cfg.file_sample_rate = 96000;
    DSD_SNPRINTF(cfg.file_path, sizeof cfg.file_path, "%s", "missing.wav");
    rc |= expect_true("missing-file config queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("missing-file config drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("the wav plays on", opts.audio_in_dev, "voice.wav");
    rc |= expect_int("the failed config stages no file rate", opts.staged_file_sample_rate, staged_before);

    (void)post_string(DSD_APP_CMD_INPUT_WAV_SET, "voice.pcm");
    rc |= expect_int("headerless file drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("the headerless file opens", opts.audio_in_type, AUDIO_IN_WAV);
    rc |= expect_int("at the session's raw rate", opts.wav_sample_rate, 48000);
    rc |= expect_int("the timing stays at that rate", state.samplesPerSymbol, sps_before);

    closeAudioInDevice(&opts);
    freeState(&state);
    (void)remove("voice.wav");
    (void)remove("voice.pcm");
    rc |= expect_int("staged rate temp directory removed", dsd_test_temp_cwd_leave(&cwd), 0);
    return rc;
}

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
/* An output that cannot follow the new input's policy fails the command with its own toast, and the input that opened
   stays (issue #634): it is the input now, whatever the output does. */
static int
test_output_reconfigure_failure_keeps_the_switch(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_ui_switch_output") != 0) {
        return expect_true("output failure temp directory", 0);
    }
    rc |= write_pcm16_wav("voice.wav", 48000U, 8U);
    init_test_context(&opts, &state);
    g_reconfigure_output_fails = 1;
    g_reconfigure_guarded = -1;
    (void)post_string(DSD_APP_CMD_INPUT_WAV_SET, "voice.wav");
    rc |= expect_int("output failure drained", dsd_app_drain_cmds(&opts, &state), 1);
    /* The output follows the switch under the tick guard: the watchdog may flush audio into the stream it closes. */
    rc |= expect_int("the output reconfigure ran under the tick guard", g_reconfigure_guarded, 1);
    g_reconfigure_output_fails = 0;
    rc |= expect_int("output failure fails the command", dsd_app_command_test_last_failed(), 1);
    rc |= expect_true("the wav is the input", opts.audio_in_type == AUDIO_IN_WAV && opts.audio_in_file != NULL);
    rc |= expect_contains("the output's own toast", state.ui_msg, "Failed: audio output reconfigure");
    closeAudioInDevice(&opts);
    freeState(&state);
    (void)remove("voice.wav");
    rc |= expect_int("output failure temp directory removed", dsd_test_temp_cwd_leave(&cwd), 0);
    return rc;
}
#endif

/*
 * Replay last replays the capture closed most recently (issue #634), and stopping that playback switches to the Pulse
 * input. Each is an input switch that clears the tone the old input carried (issue #522).
 */
static int
test_playback_switches_clear_received_tone(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    char path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(path, sizeof path, "dsd_rx_tone_replay");
    if (fd < 0) {
        return expect_true("replay temporary path available", 0);
    }
    dsd_close(fd);
    static const unsigned char k_symbols[] = {0x01, 0x03, 0x00, 0x02};
    rc |= write_file_bytes(path, k_symbols, sizeof k_symbols);

    init_test_context(&opts, &state);
    DSD_SNPRINTF(opts.symbol_capture_last, sizeof opts.symbol_capture_last, "%s", path);
    seed_received_tone(&state);
    uint32_t seeded = state.analog_rx.generation;
    rc |= expect_int("replay last queued", post_empty(DSD_APP_CMD_REPLAY_LAST), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("replay last drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("replay last switches to the symbol file", opts.audio_in_type, AUDIO_IN_SYMBOL_BIN);
    rc |= expect_str("replay last names the capture", opts.audio_in_dev, path);
    rc |= expect_int("replay last paces the replay", state.use_throttle, 1);
    rc |= expect_received_tone_cleared("replay last clears the received tone", &state, seeded);

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    /* Stopping that playback switches to the configured Pulse input, whatever the output. */
    opts.audio_out_type = 9;
    seed_received_tone(&state);
    seeded = state.analog_rx.generation;
    arm_open_audio_input_stub(1, 0);
    rc |= expect_int("stop playback queued", post_empty(DSD_APP_CMD_STOP_PLAYBACK), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("stop playback drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("stop playback closes the symbol file", opts.symbolfile == NULL);
    rc |= expect_int("stop playback switches to pulse", opts.audio_in_type, AUDIO_IN_PULSE);
    rc |= expect_received_tone_cleared("stop playback clears the received tone", &state, seeded);
    arm_open_audio_input_stub(0, 0);
#endif

    /* With no playback running, stop changes nothing. */
    closeAudioInDevice(&opts);
    freeState(&state);
    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_PULSE;
    seed_received_tone(&state);
    seeded = state.analog_rx.generation;
    rc |= expect_int("stop without playback queued", post_empty(DSD_APP_CMD_STOP_PLAYBACK),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("stop without playback drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("stop without playback keeps the input", opts.audio_in_type, AUDIO_IN_PULSE);
    rc |= expect_received_tone_kept("stop without playback keeps the tone", &state, seeded);
    rc |= expect_contains("stop without playback toast", state.ui_msg, "No playback to stop");

    /* With no capture closed yet, replay last has nothing to replay. */
    freeState(&state);
    init_test_context(&opts, &state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", path);
    rc |=
        expect_int("replay without capture queued", post_empty(DSD_APP_CMD_REPLAY_LAST), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("replay without capture drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("replay without capture fails", dsd_app_command_test_last_failed(), 1);
    rc |= expect_true("replay without capture opens nothing", opts.symbolfile == NULL);
    closeAudioInDevice(&opts);
    freeState(&state);
    remove(path);
    return rc;
}

/*
 * A config apply that moves the input is an input switch like the commands that make one,
 * and clears the received tone (issue #522); one that changes an unrelated setting leaves the
 * tone and its generation alone, so a settings change does not restart acquisition.
 */
static int
test_config_apply_input_change_clears_received_tone(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

    init_test_context(&opts, &state);
    seed_received_tone(&state);
    uint32_t seeded = state.analog_rx.generation;
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_alerts = 1;
    cfg.call_alert_enabled = 1;
    rc |= expect_true("unrelated config queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("unrelated config drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("unrelated config keeps the received tone",
                      state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED
                          && state.analog_rx.ctcss_tenths_hz == 1000 && state.analog_rx.carrier_open == 1
                          && state.analog_rx.generation == seeded);

    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_TCP;
    DSD_SNPRINTF(cfg.tcp_host, sizeof cfg.tcp_host, "%s", "127.0.0.1");
    cfg.tcp_port = 7355;
    rc |= expect_true("input config queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("input config drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("input config moves the input", opts.audio_in_dev, "tcp:127.0.0.1:7355");
    rc |= expect_received_tone_cleared("input config clears the received tone", &state, seeded);
    freeState(&state);
    return rc;
}

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
/* A config apply that moves a Pulse input to another device opens another PCM stream (issue #628): the generation the
   PCM noise squelch keys its references on moves with it, so it forgets what it learned on the old device. The same
   device, restated, is no new stream. */
static int
test_config_apply_pulse_device_is_a_new_stream(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse:old_source");
    arm_open_audio_input_stub(1, 0);
    const uint32_t generation = opts.pcm_input_generation;
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_PULSE;
    DSD_SNPRINTF(cfg.pulse_input, sizeof cfg.pulse_input, "%s", "new_source");
    rc |= expect_true("pulse device config queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("pulse device config drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("pulse device config moves the input", opts.audio_in_dev, "pulse:new_source");
    rc |= expect_int("pulse device config reopens the input", g_open_audio_input_calls, 1);
    rc |= expect_true("another device is another stream", opts.pcm_input_generation != generation);
    const uint32_t moved = opts.pcm_input_generation;
    rc |= expect_true("same device config queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("same device config drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("the same device is the same stream", opts.pcm_input_generation == moved);
    arm_open_audio_input_stub(0, 0);
    freeState(&state);
    return rc;
}
#endif

/* A config apply carrying only a [mode], drained on this thread as the decoder drains it. */
static int
submit_config_mode(dsd_opts* opts, dsd_state* state, dsdneoUserDecodeMode mode, const char* label) {
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_mode = 1;
    cfg.decode_mode = mode;
    int rc = expect_int(label, dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof(cfg)),
                        DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/*
 * A config apply that changes the decode mode is a decode-mode change like the command that
 * makes one, and clears the received tone (issue #522). Out of the analog monitor, the row is
 * hidden and no monitor block need arrive to forget the tone, so coming straight back would put
 * the old reception's tone on screen again; into it, whatever the publication holds was heard
 * before the change. Every runtime config apply carries the mode, so one that restates the
 * mode the session is in keeps the tone and its generation.
 */
static int
test_config_apply_mode_change_clears_received_tone(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rx_tone view;

    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_PULSE;
    rc |= expect_int("analog monitor set up",
                     dsd_apply_decode_mode_preset(DSDCFG_MODE_ANALOG, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state), 0);
    seed_received_tone(&state);
    uint32_t seeded = state.analog_rx.generation;
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_ANALOG, "config restating analog");
    rc |= expect_true("config restating analog keeps the received tone",
                      state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED
                          && state.analog_rx.ctcss_tenths_hz == 1000 && state.analog_rx.carrier_open == 1
                          && state.analog_rx.generation == seeded);
    rc |= expect_int("received row shown in analog", dsd_app_rx_tone_view(&opts, &state, 0.0, &view), 1);
    rc |= expect_str("received row shows the tone", view.text, "CTCSS 100.0 Hz");

    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_DMR, "config to dmr");
    rc |= expect_int("config to dmr leaves the analog monitor", opts.analog_only, 0);
    rc |= expect_received_tone_cleared("config to dmr clears the received tone", &state, seeded);

    /* Straight back, with no monitor block read in between. */
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_ANALOG, "config back to analog");
    rc |= expect_int("config back to analog", opts.analog_only, 1);
    rc |= expect_int("received row shown again", dsd_app_rx_tone_view(&opts, &state, 0.0, &view), 1);
    rc |= expect_int("received row has no carrier yet", view.status, DSD_APP_RX_TONE_NO_CARRIER);
    rc |= expect_true("received row does not bring the old tone back", view.ctcss_tenths_hz == 0);

    /* Into the analog monitor, a tone the publication still holds goes too. */
    rc |= expect_int("dmr set up",
                     dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state), 0);
    seed_received_tone(&state);
    seeded = state.analog_rx.generation;
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_ANALOG, "config into analog");
    rc |= expect_received_tone_cleared("config into analog clears the received tone", &state, seeded);
    freeState(&state);
    return rc;
}

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
/*
 * Connecting TCP audio -- from the settings form with a host and port, or the menu's
 * connect to the configured endpoint -- puts a new stream on the input, and the tone the old
 * input carried goes with it (issue #522). That includes reconnecting TCP to TCP at the same
 * rate, which moves no rate and no generation the tap could notice. A refused connection
 * leaves the input, and so its tone, where they were.
 */
static int
test_tcp_connect_clears_received_tone(void) {
    static const struct {
        int cmd;
        int connect_rc;
        int clears;
        const char* tag;
    } cases[] = {
        {DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG, 0, 1, "tcp connect to an endpoint clears the received tone"},
        {DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG, DSD_AUDIO_INPUT_KEPT, 0,
         "refused tcp connect to an endpoint keeps the received tone"},
        {DSD_APP_CMD_TCP_CONNECT_AUDIO, 0, 1, "tcp reconnect clears the received tone"},
        {DSD_APP_CMD_TCP_CONNECT_AUDIO, DSD_AUDIO_INPUT_KEPT, 0, "refused tcp reconnect keeps the received tone"},
    };

    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        init_test_context(&opts, &state);
        opts.audio_in_type = AUDIO_IN_TCP;
        DSD_SNPRINTF(opts.tcp_hostname, sizeof opts.tcp_hostname, "%s", "127.0.0.1");
        opts.tcp_portno = 7355;
        seed_received_tone(&state);
        const uint32_t seeded = state.analog_rx.generation;
        arm_tcp_connect_stub(1, cases[i].connect_rc);
        const int queued = cases[i].cmd == DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG
                               ? post_host_port(cases[i].cmd, "127.0.0.1", 7355)
                               : post_empty(cases[i].cmd);
        rc |= expect_int(cases[i].tag, queued, DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(cases[i].tag, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(cases[i].tag, g_tcp_connect_calls, 1);
        rc |= expect_int(cases[i].tag, opts.audio_in_type, AUDIO_IN_TCP);
        if (cases[i].clears) {
            rc |= expect_received_tone_cleared(cases[i].tag, &state, seeded);
        } else {
            rc |= expect_true(cases[i].tag, state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED
                                                && state.analog_rx.ctcss_tenths_hz == 1000
                                                && state.analog_rx.generation == seeded);
        }
        freeState(&state);
    }
    arm_tcp_connect_stub(0, 0);
    return rc;
}

/*
 * Issue #589: the '9' key reconnects rigctl to the TCP input's host at the rigctl port through the service the menu's
 * connect takes too, which hands the new socket the peer's record and closes the old one. It reports the service's
 * outcome, as the menu's connect does, and its toasts name the host it asked for, since a reconnect that fails keeps
 * the old endpoint in the options. Both hand the service the decoder state, which its width ask weighs a typed -Y list
 * by (issue #621).
 */
static int
test_rigctl_reconnect_key_uses_the_connect_service(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    for (int connects = 0; connects < 2; connects++) {
        const char* tag = connects ? "rigctl reconnect key" : "refused rigctl reconnect key";
        init_test_context(&opts, &state);
        DSD_SNPRINTF(opts.tcp_hostname, sizeof opts.tcp_hostname, "%s", "sdr.example");
        DSD_SNPRINTF(opts.rigctlhostname, sizeof opts.rigctlhostname, "%s", "localhost");
        opts.rigctlportno = 4532;
        opts.rigctl_sockfd = (dsd_socket_t)41;
        opts.use_rigctl = 1;
        arm_rigctl_connect_stub(1, connects ? 0 : -1);
        rc |= expect_int(tag, post_empty(DSD_APP_CMD_RIGCTL_CONNECT), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(tag, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(tag, dsd_app_command_test_last_failed(), connects ? 0 : 1);
        rc |= expect_int(tag, g_rigctl_connect_calls, 1);
        rc |= expect_str(tag, g_rigctl_connect_host, "sdr.example");
        rc |= expect_int(tag, g_rigctl_connect_port, 4532);
        rc |= expect_int(tag, g_rigctl_connect_state == &state, 1);
        rc |= expect_contains(tag, state.ui_msg,
                              connects ? "Rigctl connected: sdr.example:4532"
                                       : "Rigctl connect failed: sdr.example:4532");
        freeState(&state);
    }
    /* The menu's connect to a host and port. */
    init_test_context(&opts, &state);
    arm_rigctl_connect_stub(1, 0);
    rc |= expect_int("menu rigctl connect", post_host_port(DSD_APP_CMD_RIGCTL_CONNECT_CFG, "rig.example", 4533),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("menu rigctl connect", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("menu rigctl connect", g_rigctl_connect_calls, 1);
    rc |= expect_str("menu rigctl connect", g_rigctl_connect_host, "rig.example");
    rc |= expect_int("menu rigctl connect", g_rigctl_connect_port, 4533);
    rc |= expect_int("menu rigctl connect", g_rigctl_connect_state == &state, 1);
    rc |= expect_contains("menu rigctl connect", state.ui_msg, "Rigctl connected: rig.example:4533");
    freeState(&state);
    arm_rigctl_connect_stub(0, 0);
    return rc;
}

/*
 * Stopping a playback onto a Pulse input that does not open leaves the playback running (issue #634): a failed switch
 * changes nothing, so the file stays open, its tone stays, and no new stream begins.
 */
static int
test_stop_playback_pulse_failure_keeps_the_playback(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_ui_stop_playback_fail") != 0) {
        return expect_true("stop playback temp directory", 0);
    }
    rc |= write_pcm16_wav("playing.wav", 48000U, 8U);
    init_test_context(&opts, &state);
    rc |= expect_int("playback queued", post_string(DSD_APP_CMD_INPUT_WAV_SET, "playing.wav"),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("playback drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("playback runs", opts.audio_in_type, AUDIO_IN_WAV);
    state.input_boundary = 0;
    opts.audio_out_type = 0;
    seed_received_tone(&state);
    const uint32_t seeded = state.analog_rx.generation;
    const uint32_t generation = opts.pcm_input_generation;
    SNDFILE* const playing = opts.audio_in_file;
    arm_open_audio_input_stub(1, -1);
    rc |= expect_int("stop playback onto pulse queued", post_empty(DSD_APP_CMD_STOP_PLAYBACK),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("stop playback onto pulse drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("stop playback tried to open pulse", g_open_audio_input_calls, 1);
    rc |= expect_int("stop playback onto a failed pulse fails", dsd_app_command_test_last_failed(), 1);
    rc |= expect_int("the playback keeps running", opts.audio_in_type, AUDIO_IN_WAV);
    rc |= expect_true("the file stays open", opts.audio_in_file == playing && playing != NULL);
    rc |= expect_str("the playback keeps its name", opts.audio_in_dev, "playing.wav");
    rc |= expect_received_tone_kept("a failed pulse open keeps the received tone", &state, seeded);
    rc |= expect_true("a failed pulse open is no new stream", opts.pcm_input_generation == generation);
    rc |= expect_int("a failed pulse open crosses no boundary", state.input_boundary, 0);
    arm_open_audio_input_stub(0, 0);
    closeAudioInDevice(&opts);
    freeState(&state);
    (void)remove("playing.wav");
    rc |= expect_int("stop playback temp directory removed", dsd_test_temp_cwd_leave(&cwd), 0);
    return rc;
}

/*
 * Stopping a symbol playback that ran over live Pulse input opens Pulse again: another PCM stream, which may be
 * another source (the default one changed meanwhile), so the generation the PCM noise squelch keys its references on
 * moves and it forgets what it learned before the playback (issue #628).
 */
static int
test_stop_playback_reopens_pulse_as_a_new_stream(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    opts.audio_out_type = 0;
    opts.audio_in_type = AUDIO_IN_SYMBOL_FLT;
    opts.symbolfile = NULL;
    DSD_SNPRINTF(opts.pa_input_idx, sizeof opts.pa_input_idx, "%s", "mic");
    const uint32_t generation = opts.pcm_input_generation;
    arm_open_audio_input_stub(1, 0);
    rc |=
        expect_int("stop symbol playback queued", post_empty(DSD_APP_CMD_STOP_PLAYBACK), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("stop symbol playback drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("stop symbol playback opens pulse", g_open_audio_input_calls, 1);
    rc |= expect_int("stop symbol playback switched to pulse", opts.audio_in_type, AUDIO_IN_PULSE);
    rc |= expect_true("pulse after a playback is a new stream", opts.pcm_input_generation != generation);
    /* The configured device, named as a saved config reopens it (issue #634). */
    rc |= expect_str("stop symbol playback keeps the configured device", opts.pa_input_idx, "mic");
    rc |= expect_str("stop symbol playback names the input", opts.audio_in_dev, "pulse:mic");
    arm_open_audio_input_stub(0, 0);
    freeState(&state);
    return rc;
}
#endif

/*
 * Modulation and decode mode as setters rather than toggles. A panel showing
 * both choices has to be able to ask for the one it is not on, and re-asserting
 * the state it is already on must cost nothing — a segmented control re-sends
 * itself whenever the engine publishes a frame it did not cause.
 */
static int
test_modulation_and_decode_mode_setters(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

    init_test_context(&opts, &state);
    opts.mod_c4fm = 1;
    opts.mod_qpsk = 0;
    state.rf_mod = 0;

    rc |= expect_int("qpsk queued", dsd_app_command_set_i32(DSD_APP_CMD_MOD_SET, 1), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("qpsk drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("qpsk selected", opts.mod_qpsk, 1);
    rc |= expect_int("qpsk clears c4fm", opts.mod_c4fm, 0);
    rc |= expect_int("qpsk moves rf_mod", state.rf_mod, 1);

    /* Asking again for what it is already on leaves the timing the decoder has
     * settled on alone. Observable through samplesPerSymbol, which the apply
     * path rewrites. */
    state.samplesPerSymbol = 12345;
    rc |= expect_int("repeat qpsk queued", dsd_app_command_set_i32(DSD_APP_CMD_MOD_SET, 1),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("repeat qpsk drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("repeat qpsk changes nothing", state.samplesPerSymbol, 12345);

    rc |= expect_int("c4fm queued", dsd_app_command_set_i32(DSD_APP_CMD_MOD_SET, 0), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("c4fm drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("c4fm selected", opts.mod_c4fm, 1);
    rc |= expect_int("c4fm clears qpsk", opts.mod_qpsk, 0);
    rc |= expect_int("c4fm moves rf_mod", state.rf_mod, 0);
    rc |= expect_true("c4fm rebuilds timing", state.samplesPerSymbol != 12345);
    freeState(&state);

    /* GFSK is the third rf_mod value, and the one the DMR and EDACS/ProVoice
     * presets leave behind. Asking for C4FM from there is a real change, so the
     * idempotency guard must not swallow it. */
    init_test_context(&opts, &state);
    opts.mod_c4fm = 0;
    opts.mod_qpsk = 0;
    opts.mod_gfsk = 1;
    state.rf_mod = 2;
    state.samplesPerSymbol = 12345;
    rc |= expect_int("c4fm from gfsk queued", dsd_app_command_set_i32(DSD_APP_CMD_MOD_SET, 0),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("c4fm from gfsk drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("c4fm from gfsk selected", opts.mod_c4fm, 1);
    rc |= expect_int("c4fm from gfsk clears gfsk", opts.mod_gfsk, 0);
    rc |= expect_int("c4fm from gfsk moves rf_mod", state.rf_mod, 0);
    rc |= expect_true("c4fm from gfsk rebuilds timing", state.samplesPerSymbol != 12345);

    /* And the other segment from the same starting point. */
    opts.mod_c4fm = 0;
    opts.mod_qpsk = 0;
    opts.mod_gfsk = 1;
    state.rf_mod = 2;
    state.samplesPerSymbol = 12345;
    rc |= expect_int("qpsk from gfsk queued", dsd_app_command_set_i32(DSD_APP_CMD_MOD_SET, 1),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("qpsk from gfsk drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("qpsk from gfsk selected", opts.mod_qpsk, 1);
    rc |= expect_int("qpsk from gfsk clears gfsk", opts.mod_gfsk, 0);
    rc |= expect_int("qpsk from gfsk moves rf_mod", state.rf_mod, 1);

    /* And back to GFSK, which is why it is a choice and not just a reading: the
     * preset that selected it is not re-run by the modulation control, so without
     * this the first tap on any other segment is a one-way door. */
    state.samplesPerSymbol = 12345;
    rc |= expect_int("gfsk queued", dsd_app_command_set_i32(DSD_APP_CMD_MOD_SET, 2), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("gfsk drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("gfsk selected", opts.mod_gfsk, 1);
    rc |= expect_int("gfsk clears c4fm", opts.mod_c4fm, 0);
    rc |= expect_int("gfsk clears qpsk", opts.mod_qpsk, 0);
    rc |= expect_int("gfsk moves rf_mod", state.rf_mod, 2);
    rc |= expect_true("gfsk rebuilds timing", state.samplesPerSymbol != 12345);

    /* Idempotent on its own value too, same as the other two. */
    state.samplesPerSymbol = 12345;
    rc |= expect_int("repeat gfsk queued", dsd_app_command_set_i32(DSD_APP_CMD_MOD_SET, 2),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("repeat gfsk drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("repeat gfsk changes nothing", state.samplesPerSymbol, 12345);

    /* A modulation that does not exist is refused rather than clamped: clamping
     * would land the demodulator on C4FM, which nobody asked for. */
    rc |= expect_int("out of range queued", dsd_app_command_set_i32(DSD_APP_CMD_MOD_SET, 3),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("out of range drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("out of range leaves modulation alone", state.rf_mod, 2);
    rc |= expect_int("out of range leaves timing alone", state.samplesPerSymbol, 12345);
    rc |=
        expect_int("negative queued", dsd_app_command_set_i32(DSD_APP_CMD_MOD_SET, -1), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("negative drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("negative leaves modulation alone", state.rf_mod, 2);
    freeState(&state);

    /* Timing comes from the decode set, not from a constant. ProVoice is the one
     * mode this control reaches that is not 4800 symbols/s, and applying a
     * modulation at 4800 on a 9600-baud signal is a decoder that stops decoding.
     *
     * Started from C4FM so the request below is a real change: ProVoice runs on a
     * two-level profile, where every request lands on GFSK, so one made from GFSK
     * is already satisfied. */
    init_test_context(&opts, &state);
    opts.frame_provoice = 1;
    opts.frame_p25p1 = 0;
    opts.frame_p25p2 = 0;
    opts.frame_dmr = 0;
    opts.frame_nxdn48 = 0;
    opts.frame_nxdn96 = 0;
    opts.frame_ysf = 0;
    opts.frame_m17 = 0;
    opts.frame_dstar = 0;
    opts.frame_x2tdma = 0;
    opts.frame_dpmr = 0;
    opts.mod_c4fm = 1;
    opts.mod_qpsk = 0;
    opts.mod_gfsk = 0;
    state.rf_mod = 0;
    rc |= expect_int("provoice gfsk queued", dsd_app_command_set_i32(DSD_APP_CMD_MOD_SET, 2),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("provoice gfsk drained", dsd_app_drain_cmds(&opts, &state), 1);
    /* 48 kHz / 9600 = 5, the value the EDACS/ProVoice preset itself installs. */
    rc |= expect_int("provoice keeps its 9600 timing", state.samplesPerSymbol, 5);
    rc |= expect_int("provoice hunts on the 9600 profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_9600_2);
    freeState(&state);

    /* Two-level decode sets have one modulation. Asking for C4FM on ProVoice used
     * to be honoured in opts->mod_* and then undone in state->rf_mod alone, by
     * frame_sync_apply_sps_hunt_profile() normalising the 9600/2 profile back to
     * GFSK — leaving the control reading C4FM off flags the demodulator had
     * already contradicted, with no tap able to resynchronise them because the two
     * disagreeing is exactly what the idempotency guard tests. */
    init_test_context(&opts, &state);
    opts.frame_provoice = 1;
    opts.frame_p25p1 = 0;
    opts.frame_p25p2 = 0;
    opts.frame_dmr = 0;
    opts.frame_nxdn48 = 0;
    opts.frame_nxdn96 = 0;
    opts.frame_ysf = 0;
    opts.frame_m17 = 0;
    opts.frame_dstar = 0;
    opts.frame_x2tdma = 0;
    opts.frame_dpmr = 0;
    opts.mod_c4fm = 1;
    opts.mod_qpsk = 0;
    opts.mod_gfsk = 0;
    state.rf_mod = 0;
    rc |= expect_int("provoice c4fm queued", dsd_app_command_set_i32(DSD_APP_CMD_MOD_SET, 0),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("provoice c4fm drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("provoice c4fm lands on gfsk", state.rf_mod, 2);
    rc |= expect_int("provoice c4fm sets the gfsk flag", opts.mod_gfsk, 1);
    rc |= expect_int("provoice c4fm clears the c4fm flag", opts.mod_c4fm, 0);
    /* And having landed there, it stays: the two readings now agree, so the guard
     * fires and the timing is not rebuilt on every repeat of the same tap. */
    state.samplesPerSymbol = 12345;
    rc |= expect_int("provoice repeat c4fm queued", dsd_app_command_set_i32(DSD_APP_CMD_MOD_SET, 0),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("provoice repeat c4fm drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("provoice repeat c4fm changes nothing", state.samplesPerSymbol, 12345);
    freeState(&state);

    /* AUTO enables ProVoice next to the 4800 modes, so its flag alone must not
     * drag everything else to 9600. */
    init_test_context(&opts, &state);
    opts.frame_provoice = 1;
    opts.frame_p25p1 = 1;
    opts.mod_c4fm = 1;
    opts.mod_qpsk = 0;
    opts.mod_gfsk = 0;
    state.rf_mod = 0;
    rc |=
        expect_int("auto qpsk queued", dsd_app_command_set_i32(DSD_APP_CMD_MOD_SET, 1), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("auto qpsk drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("auto stays on 4800 timing", state.samplesPerSymbol, 10);
    rc |= expect_int("auto hunts on the 4800 profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    freeState(&state);

    /* Decode mode goes through the same preset helper the CLI uses, so a mode
     * chosen here means what it means at startup. DMR must leave P25 off. */
    init_decode_mode_context(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 0;
    /* Mid-session the hunt is wherever the previous mode left it — here the P25p2
     * 6000 profile, part-way through its dwell. */
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state.sps_hunt_counter = 11;
    rc |= seed_active_canonical_calls(&opts, &state, 852000000L, 1501);
    rc |= expect_int("dmr mode queued", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("dmr mode drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("dmr enabled", opts.frame_dmr, 1);
    rc |= expect_int("p25p1 disabled", opts.frame_p25p1, 0);
    rc |= expect_int("p25p2 disabled", opts.frame_p25p2, 0);
    rc |= expect_contains("decode mode explains itself", state.ui_msg, "DMR");
    /* Left on the old mode's profile the hunt overwrites the timing installed
     * here on its very next pass, and the new mode never gets a chance. */
    rc |= expect_int("dmr mode selects the 4800 hunt profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    rc |= expect_int("dmr mode restarts the hunt dwell", state.sps_hunt_counter, 0);
    rc |= expect_int("dmr mode installs 4800 timing", state.samplesPerSymbol, 10);
    /* A call open on the protocol we just stopped decoding would never be closed
     * by the one we started, so it has to end here. */
    rc |= expect_call_phase("decode change ends slot 1", &state, 0U, DSD_CALL_PHASE_ENDED);
    rc |= expect_call_phase("decode change ends slot 2", &state, 1U, DSD_CALL_PHASE_ENDED);
    freeState(&state);

    /* A mode on a different symbol rate has to carry the hunt with it: NXDN48 is
     * 2400 sym/s, and a decoder left on the 4800 profile is looking for it at
     * twice the symbol clock through a 12.5 kHz filter. */
    init_decode_mode_context(&opts, &state);
    opts.frame_dmr = 1;
    opts.frame_p25p1 = 0;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.sps_hunt_counter = 5;
    rc |= expect_int("nxdn48 mode queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_NXDN48),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("nxdn48 mode drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("nxdn48 enabled", opts.frame_nxdn48, 1);
    rc |= expect_int("nxdn48 selects the 2400 hunt profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_2400_4);
    rc |= expect_int("nxdn48 restarts the hunt dwell", state.sps_hunt_counter, 0);
    rc |= expect_int("nxdn48 installs 2400 timing", state.samplesPerSymbol, 20);
    freeState(&state);

    /* The session's stream shape survives a mode change, because the backend fixed
     * it when the stream was opened. dmr_stereo is not part of that shape and must
     * not be dragged along with it: it selects two-slot decoding, and the DMR
     * playback paths mix the two slots down themselves when the stream is mono
     * (playSynthesizedVoiceSS3/FS3). Held to dmr_stereo == 1 here because the
     * alternative pairs it with dmr_mono == 0, which no preset produces and which
     * dmr_handle_voice() has no branch for on MS voice. */
    init_decode_mode_context(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dmr = 0;
    opts.pulse_digi_out_channels = 1;
    opts.pulse_digi_rate_out = 8000;
    opts.dmr_stereo = 0;
    rc |= expect_int("mono dmr mode queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("mono dmr mode drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("mono session keeps its one channel", opts.pulse_digi_out_channels, 1);
    rc |= expect_int("mono session keeps its rate", opts.pulse_digi_rate_out, 8000);
    rc |= expect_int("dmr still decodes both slots", opts.dmr_stereo, 1);
    rc |= expect_int("and not as dmr mono", opts.dmr_mono, 0);
    freeState(&state);

    /* Asking for the mode already in effect must change nothing at all. DecodeChip
     * taps whether or not it is already selected, so a stray tap on the lit chip
     * would otherwise run the whole teardown above: it would end a live call and
     * put the modulation back to the preset's, discarding the operator's own pick.
     * ui_handle_mod_set() has held this contract since it was written; this is the
     * same one for the decode chips beside it. */
    init_decode_mode_context(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 0;
    rc |= expect_int("first dmr mode queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("first dmr mode drained", dsd_app_drain_cmds(&opts, &state), 1);
    /* The session is on DMR now. The operator picks QPSK and a call opens. */
    opts.mod_c4fm = 0;
    opts.mod_qpsk = 1;
    opts.mod_gfsk = 0;
    state.rf_mod = 1;
    state.sps_hunt_counter = 7;
    rc |= seed_active_canonical_calls(&opts, &state, 852000000L, 1502);
    rc |= expect_int("repeat dmr mode queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("repeat dmr mode drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("repeat keeps the operator's modulation", opts.mod_qpsk, 1);
    rc |= expect_int("repeat leaves the hunt dwell alone", state.sps_hunt_counter, 7);
    rc |= expect_call_phase("repeat leaves slot 1 active", &state, 0U, DSD_CALL_PHASE_ACTIVE);
    rc |= expect_call_phase("repeat leaves slot 2 active", &state, 1U, DSD_CALL_PHASE_ACTIVE);
    rc |= expect_contains("repeat still names the mode", state.ui_msg, "DMR");
    freeState(&state);

    /* Two picker rows spell DMR, and this toast is the only thing that says which
     * one landed. It has to name the row the operator chose, which means reading
     * the same table the picker was built from rather than a second one. */
    init_decode_mode_context(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dmr = 0;
    rc |= expect_int("dmr mono mode queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR_MONO),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("dmr mono mode drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_contains("dmr mono names its own row", state.ui_msg, "DMR (single slot)");
    freeState(&state);

    /* The payload travels as a plain int32 but dsdneoUserDecodeMode is packed to a
     * byte, so an out-of-range value does not stay out of range: 260 casts to 4,
     * which is DSDCFG_MODE_DMR, and the DMR preset would run for a command nobody
     * could have meant. */
    init_decode_mode_context(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dmr = 0;
    rc |= expect_int("out-of-range mode queued", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, 260),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("out-of-range mode drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("out-of-range mode does not alias onto dmr", opts.frame_dmr, 0);
    rc |= expect_int("out-of-range mode leaves p25 alone", opts.frame_p25p1, 1);
    freeState(&state);

    /* Back to auto re-enables the set the engine starts with. */
    init_decode_mode_context(&opts, &state);
    opts.frame_dmr = 1;
    opts.frame_p25p1 = 0;
    rc |=
        expect_int("auto mode queued", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_AUTO),
                   DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("auto mode drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("auto re-enables p25", opts.frame_p25p1, 1);
    rc |= expect_int("auto keeps dmr", opts.frame_dmr, 1);
    rc |= expect_contains("auto names its own row", state.ui_msg, "Auto");

    /* Both carry a payload, so the payload-less action API must refuse them. */
    rc |= expect_int("mod set rejects an action submit", dsd_app_command_action(DSD_APP_CMD_MOD_SET),
                     DSD_APP_COMMAND_SUBMIT_REJECTED);
    rc |= expect_int("decode mode rejects an action submit", dsd_app_command_action(DSD_APP_CMD_DECODE_MODE_SET),
                     DSD_APP_COMMAND_SUBMIT_REJECTED);
    freeState(&state);
    return rc;
}

/*
 * TRUNK_SET is the half of tuner ownership a view can name. TUNER_RELEASE clears
 * both owners without knowing which held it; this one says "trunking, on" or
 * "trunking, off" from a frontend that does know, which is what a control
 * offering trunking as a choice needs and what TRUNK_TOGGLE cannot express.
 */
static int
test_trunk_set(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

    init_test_context(&opts, &state);
    opts.trunk_enable = 0;
    opts.scanner_mode = 0;

    rc |=
        expect_int("trunk on queued", dsd_app_command_set_i32(DSD_APP_CMD_TRUNK_SET, 1), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("trunk on drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("trunk on enables trunking", opts.trunk_enable, 1);

    /* Asking for it again is not a flip. This is the whole reason the command
     * exists: TRUNK_TOGGLE here would hand the tuner straight back. */
    rc |= expect_int("repeat trunk on queued", dsd_app_command_set_i32(DSD_APP_CMD_TRUNK_SET, 1),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("repeat trunk on drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("repeat trunk on stays on", opts.trunk_enable, 1);

    rc |= expect_int("trunk off queued", dsd_app_command_set_i32(DSD_APP_CMD_TRUNK_SET, 0),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("trunk off drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("trunk off disables trunking", opts.trunk_enable, 0);

    /* Scanner mode is the other automatic owner, and SCANNER_TOGGLE already
     * clears trunking on the way in. Without the same exclusion here, asking for
     * trunking from a scanning session would leave two owners driving the tuner. */
    opts.scanner_mode = 1;
    rc |= expect_int("trunk on over scanner queued", dsd_app_command_set_i32(DSD_APP_CMD_TRUNK_SET, 1),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("trunk on over scanner drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("trunk on enables trunking over scanner", opts.trunk_enable, 1);
    rc |= expect_int("trunk on clears scanner mode", opts.scanner_mode, 0);

    /* Turning it off is the narrow half: TUNER_RELEASE stays the way to clear
     * both, so this must not reach into scanner mode on the way out. */
    opts.scanner_mode = 1;
    rc |= expect_int("trunk off over scanner queued", dsd_app_command_set_i32(DSD_APP_CMD_TRUNK_SET, 0),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("trunk off over scanner drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("trunk off leaves scanner mode alone", opts.scanner_mode, 1);

    /* Carries a payload, so the payload-less action API must refuse it — a bare
     * action submit would arrive with no bytes and read as "stop trunking". */
    rc |= expect_int("trunk set rejects an action submit", dsd_app_command_action(DSD_APP_CMD_TRUNK_SET),
                     DSD_APP_COMMAND_SUBMIT_REJECTED);
    freeState(&state);
    return rc;
}

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
/* The scan control stub and the test below observe tunes through the io_control_set_freq() wrap. */
static int g_scan_control_calls = 0;
static int g_scan_control_last_op = -1;
static int g_scan_control_result = 0;

static int
fake_scan_control(dsd_opts* opts, dsd_state* state, int op) {
    (void)opts;
    g_scan_control_calls++;
    g_scan_control_last_op = op;
    if (op == DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE && g_scan_control_result >= 0) {
        DSD_SNPRINTF(state->trunk_scan_active_id, sizeof(state->trunk_scan_active_id), "incoming");
    }
    return g_scan_control_result;
}

/*
 * On-the-fly scan controls (#380). Under -Y they act on the scan list in dsd_state; under
 * --trunk-scan they are handed to the coordinator through the control hook; with neither
 * scanner running they are accepted and declined with a status message.
 */
static int
test_scan_hold_avoid_commands(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

    init_test_context(&opts, &state);
    opts.scanner_mode = 1;
    opts.audio_in_type = AUDIO_IN_RTL;
    state.lcn_freq_count = 4;
    state.trunk_lcn_freq[0] = 0L;
    state.trunk_lcn_freq[1] = 857000000L;
    state.trunk_lcn_freq[2] = 0L;
    state.trunk_lcn_freq[3] = 858000000L;
    state.lcn_freq_roll = 2; /* row 1 (857 MHz) is on air */
    state.last_cc_sync_time_m = 42.0;
    opts.scan_max_visit_ms = 1000;
    state.scan_visit_since_m = 42.0;
    state.scan_visit_roll_seen = state.lcn_freq_roll;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);

    /* Hold: flips the flag, leaves the dwell alone; release restarts the dwell. */
    rc |= expect_int("scan hold queued", dsd_app_command_action(DSD_APP_CMD_SCAN_HOLD_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("scan hold drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("scan hold sets flag", state.lcn_scan_hold, 1);
    rc |= expect_int("scan hold publishes timing before snapshot", state.scan_timing.reason, DSD_SCAN_STAY_MANUAL_HOLD);
    rc |= expect_true("scan hold keeps dwell", state.last_cc_sync_time_m == 42.0);
    rc |= expect_contains("scan hold toast", state.ui_msg, "hold on");
    rc |= expect_int("scan hold release queued", dsd_app_command_action(DSD_APP_CMD_SCAN_HOLD_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("scan hold release drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("scan hold release clears flag", state.lcn_scan_hold, 0);
    rc |= expect_true("scan hold release restarts dwell", state.last_cc_sync_time_m > 42.0);
    rc |= expect_true("scan hold release restarts the cap", state.scan_visit_since_m >= state.last_cc_sync_time_m);
    rc |= expect_true("scan hold release publishes a full cap",
                      state.scan_timing.visit_deadline_m >= state.last_cc_sync_time_m + 1.0);
    rc |= expect_contains("scan hold release toast", state.ui_msg, "hold off");
    rc |= expect_int("scan release publishes hangtime", state.scan_timing.reason, DSD_SCAN_STAY_HANGTIME);
    rc |= expect_true("scan release publishes a fresh deadline",
                      state.scan_timing.deadline_m > state.last_cc_sync_time_m);

    // A hold and release drained without a gate tick must also reset the voice
    // qualify window, rather than retaining an expired sync from the old visit.
    opts.scan_voice_only = 1;
    state.scan_voice_gate_sync_m = 42.0;
    state.scan_voice_gate_hold_seen = 0U;
    rc |= expect_int("voice-gate hold queued", dsd_app_command_action(DSD_APP_CMD_SCAN_HOLD_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("voice-gate release queued", dsd_app_command_action(DSD_APP_CMD_SCAN_HOLD_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("voice-gate toggles drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_true("voice-gate release resets old sync", state.scan_voice_gate_sync_m < 0.0);
    rc |= expect_true("voice-gate release restarts the visit", state.scan_voice_gate_arrive_m > 42.0);
    rc |= expect_int("unsynced release uses the fresh hangtime", state.scan_timing.reason, DSD_SCAN_STAY_HANGTIME);
    opts.scan_voice_only = 0;

    /* Manual next while held still moves, and the hold stays on. */
    state.lcn_scan_hold = 1;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    rc |= expect_int("held cycle queued", dsd_app_command_action(DSD_APP_CMD_CHANNEL_CYCLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("held cycle drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("held cycle tunes next row", g_io_control_tune_freq == 858000000L);
    rc |= expect_int("held cycle advances roll", state.lcn_freq_roll, 4);
    rc |= expect_int("held cycle keeps hold", state.lcn_scan_hold, 1);
    state.lcn_scan_hold = 0;

    /* Avoid: flags the row on air and steps to the next usable one in the same command. */
    state.lcn_freq_roll = 2;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    rc |=
        expect_int("scan avoid queued", dsd_app_command_action(DSD_APP_CMD_SCAN_AVOID), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("scan avoid drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("scan avoid flags the row on air", dsd_state_trunk_lcn_avoid_get(&state, 1U), 1);
    rc |= expect_int("scan avoid counts", (int)state.lcn_avoid_count, 1);
    rc |= expect_int("scan avoid tunes", g_io_control_tune_calls, 1);
    rc |= expect_true("scan avoid tunes next usable row", g_io_control_tune_freq == 858000000L);
    rc |= expect_int("scan avoid advances roll", state.lcn_freq_roll, 4);
    rc |= expect_contains("scan avoid toast", state.ui_msg, "857.0000");

    /* Refused when it would leave no usable row: nothing flagged, nothing tuned. */
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    rc |= expect_int("last-row avoid queued", dsd_app_command_action(DSD_APP_CMD_SCAN_AVOID),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("last-row avoid drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("last-row avoid leaves the row", dsd_state_trunk_lcn_avoid_get(&state, 3U), 0);
    rc |= expect_int("last-row avoid keeps count", (int)state.lcn_avoid_count, 1);
    rc |= expect_int("last-row avoid does not tune", g_io_control_tune_calls, 0);
    rc |= expect_int("last-row avoid keeps roll", state.lcn_freq_roll, 4);
    rc |= expect_contains("last-row avoid toast", state.ui_msg, "last usable");

    /* Manual next skips the avoided row: from the end of the list it wraps past rows 0-2. */
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    rc |= expect_int("cycle past avoided queued", dsd_app_command_action(DSD_APP_CMD_CHANNEL_CYCLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("cycle past avoided drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("cycle past avoided lands on the usable row", g_io_control_tune_freq == 858000000L);
    rc |= expect_int("cycle past avoided roll", state.lcn_freq_roll, 4);

    /* Clear: every flag goes, the count follows, the toast says how many. */
    rc |= expect_int("scan avoid clear queued", dsd_app_command_action(DSD_APP_CMD_SCAN_AVOID_CLEAR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("scan avoid clear drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("scan avoid clear unflags", dsd_state_trunk_lcn_avoid_get(&state, 1U), 0);
    rc |= expect_int("scan avoid clear zeroes count", (int)state.lcn_avoid_count, 0);
    rc |= expect_contains("scan avoid clear toast", state.ui_msg, "1 scan avoid");

    /* Nothing on air yet (roll 0): refused. */
    state.lcn_freq_roll = 0;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    rc |= expect_int("no-row avoid queued", dsd_app_command_action(DSD_APP_CMD_SCAN_AVOID),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("no-row avoid drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("no-row avoid flags nothing", (int)state.lcn_avoid_count, 0);
    rc |= expect_int("no-row avoid does not tune", g_io_control_tune_calls, 0);
    rc |= expect_contains("no-row avoid toast", state.ui_msg, "on air");
    freeState(&state);

    /* --trunk-scan: every control goes to the coordinator hook and touches no -Y state. */
    init_test_context(&opts, &state);
    opts.trunk_scan_enabled = 1;
    opts.audio_in_type = AUDIO_IN_RTL;
    dsd_trunk_scan_hooks hooks = {0};
    hooks.control = fake_scan_control;
    dsd_trunk_scan_hooks_set(&hooks);
    g_scan_control_calls = 0;
    g_scan_control_result = 1;
    rc |= expect_int("trunk-scan hold queued", dsd_app_command_action(DSD_APP_CMD_SCAN_HOLD_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("trunk-scan hold drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("trunk-scan hold reaches hook", g_scan_control_calls, 1);
    rc |= expect_int("trunk-scan hold op", g_scan_control_last_op, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE);
    rc |= expect_int("trunk-scan hold leaves -Y flag", state.lcn_scan_hold, 0);
    rc |= expect_contains("trunk-scan hold toast", state.ui_msg, "hold on");
    g_scan_control_result = 0;
    DSD_SNPRINTF(state.trunk_scan_active_id, sizeof(state.trunk_scan_active_id), "avoided");
    rc |= expect_int("trunk-scan avoid queued", dsd_app_command_action(DSD_APP_CMD_SCAN_AVOID),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("trunk-scan avoid drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("trunk-scan avoid op", g_scan_control_last_op, DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE);
    rc |= expect_int("trunk-scan avoid leaves -Y count", (int)state.lcn_avoid_count, 0);
    rc |= expect_contains("trunk-scan avoid names the outgoing target", state.ui_msg, "Avoiding target avoided");
    g_scan_control_result = 2;
    rc |= expect_int("trunk-scan clear queued", dsd_app_command_action(DSD_APP_CMD_SCAN_AVOID_CLEAR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("trunk-scan clear drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("trunk-scan clear op", g_scan_control_last_op, DSD_TRUNK_SCAN_CONTROL_AVOID_CLEAR);
    rc |= expect_contains("trunk-scan clear toast", state.ui_msg, "2");
    g_scan_control_result = DSD_TRUNK_SCAN_CONTROL_REFUSED;
    rc |= expect_int("trunk-scan refused avoid queued", dsd_app_command_action(DSD_APP_CMD_SCAN_AVOID),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("trunk-scan refused avoid drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_contains("trunk-scan refused avoid toast", state.ui_msg, "last usable");
    g_scan_control_result = DSD_TRUNK_SCAN_CONTROL_BUSY;
    rc |= expect_int("trunk-scan busy hold queued", dsd_app_command_action(DSD_APP_CMD_SCAN_HOLD_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("trunk-scan busy hold drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_contains("trunk-scan busy toast", state.ui_msg, "busy");
    rc |= expect_int("trunk-scan controls all reached hook", g_scan_control_calls, 5);
    /* Next channel means next target here, not a walk of the parked target's LCN list. */
    state.lcn_freq_count = 2;
    state.trunk_lcn_freq[0] = 857000000L;
    state.trunk_lcn_freq[1] = 858000000L;
    state.lcn_freq_roll = 1;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    g_scan_control_result = 0;
    rc |= expect_int("trunk-scan cycle queued", dsd_app_command_action(DSD_APP_CMD_CHANNEL_CYCLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("trunk-scan cycle drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("trunk-scan cycle op", g_scan_control_last_op, DSD_TRUNK_SCAN_CONTROL_ADVANCE);
    rc |= expect_int("trunk-scan cycle reached hook", g_scan_control_calls, 6);
    rc |= expect_int("trunk-scan cycle leaves the LCN roll", state.lcn_freq_roll, 1);
    rc |= expect_int("trunk-scan cycle does not raw tune", g_io_control_tune_calls, 0);
    g_scan_control_result = DSD_TRUNK_SCAN_CONTROL_REFUSED;
    rc |= expect_int("trunk-scan refused cycle queued", dsd_app_command_action(DSD_APP_CMD_CHANNEL_CYCLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("trunk-scan refused cycle drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_contains("trunk-scan refused cycle toast", state.ui_msg, "only one target");
    dsd_trunk_scan_hooks_set(NULL);
    freeState(&state);

    /* Neither scanner running: accepted, declined, nothing changes. */
    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    state.lcn_freq_count = 2;
    state.trunk_lcn_freq[0] = 857000000L;
    state.trunk_lcn_freq[1] = 858000000L;
    state.lcn_freq_roll = 1;
    g_scan_control_calls = 0;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    rc |= expect_int("idle hold queued", dsd_app_command_action(DSD_APP_CMD_SCAN_HOLD_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("idle hold drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("idle hold leaves flag", state.lcn_scan_hold, 0);
    rc |= expect_contains("idle hold toast", state.ui_msg, "Not scanning");
    state.ui_msg[0] = '\0';
    rc |=
        expect_int("idle avoid queued", dsd_app_command_action(DSD_APP_CMD_SCAN_AVOID), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("idle avoid drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("idle avoid flags nothing", (int)state.lcn_avoid_count, 0);
    rc |= expect_int("idle avoid does not tune", g_io_control_tune_calls, 0);
    rc |= expect_contains("idle avoid toast", state.ui_msg, "Not scanning");
    state.ui_msg[0] = '\0';
    rc |= expect_int("idle clear queued", dsd_app_command_action(DSD_APP_CMD_SCAN_AVOID_CLEAR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("idle clear drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_contains("idle clear toast", state.ui_msg, "Not scanning");
    rc |= expect_int("idle controls never reach hook", g_scan_control_calls, 0);
    freeState(&state);

    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    return rc;
}

/*
 * Issue #575: the return to the control channel and the channel cycle (the next LCN, P25 candidate or -Y row) are
 * retunes the user asks for, so an I/Q replay refuses them as it refuses a tap, with the reason, as a failed command:
 * none reaches the tuner. A session never runs --trunk-scan on a replay (trunk scan refuses that input at start and
 * refuses input switches), but the refusal sits ahead of the cycle's trunk-scan leg too, and the hold and avoid, which
 * act on the scan itself, still reach the coordinator. On a live radio the same commands go through.
 */
static int
test_replay_refuses_channel_cycle_and_return_cc(void) {
    static const char kReason[] = "An I/Q replay cannot retune.";
    static const char kReplay[] = "iqreplay:capture.iq.json";
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

    for (int replay = 1; replay >= 0; replay--) {
        const char* where = replay ? "during a replay" : "on a live radio";
        char what[128];

        init_radio_context(&opts, &state, replay ? kReplay : "rtl:0");
        seed_active_p25_voice(&opts, &state, 851000000L, 852000000L, 1201);
        reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
        reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
        DSD_SNPRINTF(what, sizeof(what), "return to CC %s", where);
        rc |= expect_int(what, dsd_app_command_action(DSD_APP_CMD_RETURN_CC), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(what, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(what, g_cc_tune_calls + g_io_control_tune_calls, replay ? 0 : 1);
        rc |= expect_int(what, strstr(state.ui_msg, kReason) != NULL ? 1 : 0, replay);
        rc |= expect_int(what, dsd_app_command_test_last_failed(), replay);
        if (replay) {
            rc |= expect_int(what, opts.trunk_is_tuned, 1);
            rc |= expect_true(what, state.p25_vc_freq[0] == 852000000L);
        }
        freeState(&state);

        init_radio_context(&opts, &state, replay ? kReplay : "rtl:0");
        seed_active_p25_voice(&opts, &state, 855000000L, 856000000L, 3201);
        state.lcn_freq_count = 2;
        state.lcn_freq_roll = 0;
        state.trunk_lcn_freq[0] = 857000000L;
        state.trunk_lcn_freq[1] = 858000000L;
        reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
        reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
        DSD_SNPRINTF(what, sizeof(what), "channel cycle %s", where);
        rc |= expect_int(what, dsd_app_command_action(DSD_APP_CMD_CHANNEL_CYCLE), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(what, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(what, g_cc_tune_calls + g_io_control_tune_calls, replay ? 0 : 1);
        rc |= expect_int(what, strstr(state.ui_msg, kReason) != NULL ? 1 : 0, replay);
        rc |= expect_int(what, dsd_app_command_test_last_failed(), replay);
        rc |= expect_int(what, state.lcn_freq_roll, replay ? 0 : 1);
        freeState(&state);

        init_radio_context(&opts, &state, replay ? kReplay : "rtl:0");
        opts.trunk_scan_enabled = 1;
        dsd_trunk_scan_hooks hooks = {0};
        hooks.control = fake_scan_control;
        dsd_trunk_scan_hooks_set(&hooks);
        g_scan_control_calls = 0;
        g_scan_control_result = 0;
        DSD_SNPRINTF(what, sizeof(what), "trunk-scan next target %s", where);
        rc |= expect_int(what, dsd_app_command_action(DSD_APP_CMD_CHANNEL_CYCLE), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(what, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(what, g_scan_control_calls, replay ? 0 : 1);
        rc |= expect_int(what, strstr(state.ui_msg, kReason) != NULL ? 1 : 0, replay);
        rc |= expect_int(what, dsd_app_command_test_last_failed(), replay);
        g_scan_control_result = 1;
        DSD_SNPRINTF(what, sizeof(what), "trunk-scan hold %s", where);
        rc |= expect_int(what, dsd_app_command_action(DSD_APP_CMD_SCAN_HOLD_TOGGLE), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(what, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(what, g_scan_control_last_op, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE);
        rc |= expect_int(what, dsd_app_command_test_last_failed(), 0);
        g_scan_control_result = 0;
        DSD_SNPRINTF(what, sizeof(what), "trunk-scan avoid %s", where);
        rc |= expect_int(what, dsd_app_command_action(DSD_APP_CMD_SCAN_AVOID), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(what, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(what, g_scan_control_last_op, DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE);
        dsd_trunk_scan_hooks_set(NULL);
        freeState(&state);
    }

    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    return rc;
}
#endif

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
/* The codes the carrier on air decoded, and the Phase 2 seed it proved. */
static void
seed_carrier_codes(dsd_state* state) {
    state->dmr_color_code = 7U;
    state->dmr_confidence_locked = 1;
    state->dmr_confidence_color_code = 7;
    state->nxdn_last_ran = 21U;
    state->dpmr_color_code = 12;
    state->p2_cc = 0x293ULL;
    state->p2_cc_verified = 1U;
}

/* Forgotten, as the carrier boundary forgets them, or kept; the Phase 2 seed, the descrambling key, stays either way. */
static int
expect_carrier_codes(const char* what, const dsd_state* state, int forgotten) {
    char tag[160];
    int rc = 0;
    DSD_SNPRINTF(tag, sizeof tag, "%s: DMR colour code", what);
    rc |= expect_int(tag, (int)state->dmr_color_code, forgotten ? 16 : 7);
    DSD_SNPRINTF(tag, sizeof tag, "%s: DMR confidence lock", what);
    rc |= expect_int(tag, (int)state->dmr_confidence_locked, forgotten ? 0 : 1);
    DSD_SNPRINTF(tag, sizeof tag, "%s: NXDN RAN", what);
    rc |= expect_true(tag, state->nxdn_last_ran == (forgotten ? (unsigned int)-1 : 21U));
    DSD_SNPRINTF(tag, sizeof tag, "%s: dPMR colour code", what);
    rc |= expect_int(tag, state->dpmr_color_code, forgotten ? -1 : 12);
    DSD_SNPRINTF(tag, sizeof tag, "%s: Phase 2 seed proof", what);
    rc |= expect_int(tag, (int)state->p2_cc_verified, forgotten ? 0 : 1);
    DSD_SNPRINTF(tag, sizeof tag, "%s: Phase 2 seed", what);
    rc |= expect_true(tag, state->p2_cc == 0x293ULL);
    return rc;
}

#ifdef USE_RADIO
/* A Phase 2 call opened and synced on slot 0, as an unscrambled MAC_PTT opens one. */
static const Event_History*
observe_phase2_call(dsd_opts* opts, dsd_state* state, uint32_t target) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = target + 1U,
    };
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    dsd_event_sync_slot(opts, state, 0U);
    return &state->event_history_s[0].Event_History_Items[0];
}
#endif

/*
 * Issue #575: an accepted retune the user asks for moves the receiver to another carrier, which can sync before any
 * no-carrier pass forgets the codes the previous one decoded. The tune forgets them itself, as the carrier boundary
 * does (dsd_engine_forget_carrier_codes()): a Phase 2 call on the new carrier records no NAC until that carrier proves
 * the seed. A refused or failed tune leaves the receiver, and the codes, where they were.
 */
static int
test_accepted_retunes_forget_the_carrier_codes(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

#ifdef USE_RADIO
    init_radio_context(&opts, &state, "rtl:0");
    seed_carrier_codes(&state);
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    const Event_History* row = observe_phase2_call(&opts, &state, 4100U);
    rc |= expect_true("verified reception records its NAC",
                      row->access_code_kind == (uint8_t)DSD_ACCESS_CODE_NAC && row->access_code == 0x293U);
    post_u32(DSD_APP_CMD_MANUAL_TUNE, 853125000U);
    rc |= expect_int("accepted tap drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("accepted tap reaches the tuner", g_io_control_tune_calls, 1);
    rc |= expect_carrier_codes("accepted tap", &state, 1);
    row = observe_phase2_call(&opts, &state, 4200U);
    rc |= expect_int("a Phase 2 call after the tap is another", (int)row->target_id, 4200);
    rc |= expect_true("a Phase 2 call after the tap records no NAC",
                      row->access_code_kind == (uint8_t)DSD_ACCESS_CODE_NONE);
    freeState(&state);

    init_radio_context(&opts, &state, "rtl:0");
    seed_carrier_codes(&state);
    reset_io_control_tune_stub(-1);
    post_u32(DSD_APP_CMD_MANUAL_TUNE, 853125000U);
    rc |= expect_int("failed tap drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_carrier_codes("failed tap", &state, 0);
    freeState(&state);

    init_radio_context(&opts, &state, "rtl:0");
    seed_carrier_codes(&state);
    opts.trunk_enable = 1;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    post_u32(DSD_APP_CMD_MANUAL_TUNE, 853125000U);
    rc |= expect_int("refused tap drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("refused tap never tunes", g_io_control_tune_calls, 0);
    rc |= expect_carrier_codes("refused tap", &state, 0);
    freeState(&state);

    init_radio_context(&opts, &state, "rtl:0");
    seed_carrier_codes(&state);
    reset_io_control_tune_stub(RTL_STREAM_TUNE_TIMEOUT);
    post_u32(DSD_APP_CMD_RTL_SET_FREQ, 853125000U);
    rc |= expect_int("accepted frequency entry drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_carrier_codes("accepted frequency entry", &state, 1);
    freeState(&state);

    init_radio_context(&opts, &state, "rtl:0");
    seed_carrier_codes(&state);
    reset_io_control_tune_stub(-1);
    post_u32(DSD_APP_CMD_RTL_SET_FREQ, 853125000U);
    rc |= expect_int("failed frequency entry drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_carrier_codes("failed frequency entry", &state, 0);
    freeState(&state);
#endif

    for (int accepted = 1; accepted >= 0; accepted--) {
        char what[96];
        init_radio_context(&opts, &state, "rtl:0");
        seed_active_p25_voice(&opts, &state, 851000000L, 852000000L, 1201);
        seed_carrier_codes(&state);
        reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
        reset_cc_tune_stub(accepted ? DSD_TRUNK_TUNE_RESULT_OK : DSD_TRUNK_TUNE_RESULT_DEFERRED);
        DSD_SNPRINTF(what, sizeof what, "%s return to CC", accepted ? "accepted" : "deferred");
        rc |= expect_int(what, dsd_app_command_action(DSD_APP_CMD_RETURN_CC), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(what, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_carrier_codes(what, &state, accepted);
        freeState(&state);

        init_radio_context(&opts, &state, "rtl:0");
        seed_active_p25_voice(&opts, &state, 855000000L, 856000000L, 3201);
        state.lcn_freq_count = 2;
        state.lcn_freq_roll = 0;
        state.trunk_lcn_freq[0] = 857000000L;
        state.trunk_lcn_freq[1] = 858000000L;
        seed_carrier_codes(&state);
        reset_io_control_tune_stub(accepted ? RTL_STREAM_TUNE_OK : RTL_STREAM_TUNE_DEFERRED);
        reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
        DSD_SNPRINTF(what, sizeof what, "%s channel cycle", accepted ? "accepted" : "deferred");
        rc |= expect_int(what, dsd_app_command_action(DSD_APP_CMD_CHANNEL_CYCLE), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(what, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_carrier_codes(what, &state, accepted);
        freeState(&state);
    }

    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    return rc;
}

/*
 * Per-row keys through the command queue: a channel cycle onto a keyed row
 * installs its set, a runtime key import while parked lands in the globals
 * and survives the next leave, and scanner toggle off restores the baseline.
 */
static int
test_scan_row_keys_commands(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    const char* hex_csv = "ui_cmd_queue_rowkey_hex.csv";
    static const unsigned char hex_data[] = "key id(hex),key value (hex)\n0007,0000000000001234\n";

    init_test_context(&opts, &state);
    remove(hex_csv);
    rc |= expect_int("rowkey hex file written", write_file_bytes(hex_csv, hex_data, sizeof(hex_data) - 1U), 0);

    state.lcn_freq_count = 2;
    state.lcn_freq_roll = 1;
    state.trunk_lcn_freq[0] = 857000000L;
    state.trunk_lcn_freq[1] = 858000000L;
    state.keyloader = 0;
    state.K = 0xBEEFULL;
    {
        dsd_key_set ks;
        DSD_MEMSET(&ks, 0, sizeof(ks));
        ks.entries = (dsd_key_set_entry*)calloc(1U, sizeof(*ks.entries));
        if (ks.entries == NULL) {
            remove(hex_csv);
            freeState(&state);
            return 1;
        }
        ks.count = 1U;
        ks.present = 1;
        ks.keyloader = 1;
        ks.entries[0].index = 9U;
        ks.entries[0].value = 999ULL;
        ks.entries[0].loaded = 1U;
        if (dsd_state_trunk_lcn_keys_set(&state, 1U, &ks) != 0) {
            remove(hex_csv);
            freeState(&state);
            return 1;
        }
    }

    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    opts.audio_in_type = AUDIO_IN_RTL;
    rc |= expect_int("keyed cycle queued", dsd_app_command_action(DSD_APP_CMD_CHANNEL_CYCLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("keyed cycle drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("keyed cycle tunes the keyed row", g_io_control_tune_freq == 858000000L);
    rc |= expect_u64("keyed cycle installs the row set", state.rkey_array[9], 999ULL);
    rc |= expect_int("keyed cycle arms keyloader", state.keyloader, 1);

    // A runtime import while parked edits the globals underneath the row set.
    post_string(DSD_APP_CMD_IMPORT_KEYS_HEX, hex_csv);
    rc |= expect_int("parked key import drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_u64("parked import keeps the row set live", state.rkey_array[9], 999ULL);
    dsd_scan_keys_leave(&state);
    rc |= expect_u64("parked import survives the leave", state.rkey_array[7], 0x1234ULL);
    rc |= expect_int("parked import arms the baseline", state.keyloader, 1);
    rc |= expect_u64("leave drops the row slot", state.rkey_array[9], 0ULL);

    // Scanner toggle off hands the foreground keyring back to the globals.
    rc |= expect_int("repark installs again", dsd_scan_keys_enter(&state, dsd_state_trunk_lcn_keys_get(&state, 1U)), 1);
    opts.scanner_mode = 1;
    rc |= expect_int("scanner toggle queued", dsd_app_command_action(DSD_APP_CMD_SCANNER_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("scanner toggle drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("scanner toggle leaves scanner mode", opts.scanner_mode, 0);
    rc |= expect_int("scanner toggle leaves the swap", (int)state.scan_keys_active_set, 0);
    rc |= expect_u64("scanner toggle restores the imported slot", state.rkey_array[7], 0x1234ULL);
    rc |= expect_u64("scanner toggle drops the row slot", state.rkey_array[9], 0ULL);

    remove(hex_csv);
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    freeState(&state);
    return rc;
}
#endif

/*
 * Voice-gated scan (#381). The on/off flag is a plain int32 setter applied
 * through the service; the qualify/hold windows ride the same path with the
 * service clamping them to 100..600000 ms. Short payloads are ignored at
 * drain, and the two ms setters coalesce while the flag does not.
 */
static int
test_scan_voice_gate_commands(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);

    rc |= expect_int("voice-only rejects action shape", dsd_app_command_action(DSD_APP_CMD_SCAN_VOICE_ONLY_SET),
                     DSD_APP_COMMAND_SUBMIT_REJECTED);
    rc |= expect_int("voice-only rejects u32 shape", dsd_app_command_set_u32(DSD_APP_CMD_SCAN_VOICE_ONLY_SET, 1U),
                     DSD_APP_COMMAND_SUBMIT_REJECTED);
    rc |= expect_int("voice qualify rejects double shape",
                     dsd_app_command_set_double(DSD_APP_CMD_SCAN_VOICE_QUALIFY_MS_SET, 1.5),
                     DSD_APP_COMMAND_SUBMIT_REJECTED);
    rc |= expect_int("voice hold rejects action shape", dsd_app_command_action(DSD_APP_CMD_SCAN_VOICE_HOLD_MS_SET),
                     DSD_APP_COMMAND_SUBMIT_REJECTED);

    rc |= expect_int("voice-only queued", post_i32(DSD_APP_CMD_SCAN_VOICE_ONLY_SET, 1), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("voice-only drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("voice-only applied", opts.scan_voice_only, 1);
    rc |= expect_contains("voice-only toast", state.ui_msg, "Voice-only scan -> On");

    rc |= expect_int("voice-only off queued", post_i32(DSD_APP_CMD_SCAN_VOICE_ONLY_SET, 7),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("voice-only off drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("voice-only nonzero means on", opts.scan_voice_only, 1);
    rc |= expect_int("voice-only clear queued", post_i32(DSD_APP_CMD_SCAN_VOICE_ONLY_SET, 0),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("voice-only clear drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("voice-only cleared", opts.scan_voice_only, 0);
    rc |= expect_contains("voice-only off toast", state.ui_msg, "Voice-only scan -> Off");

    rc |= expect_int("voice qualify low queued", post_i32(DSD_APP_CMD_SCAN_VOICE_QUALIFY_MS_SET, 50),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("voice qualify low drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("voice qualify clamped low", opts.scan_voice_qualify_ms, 100);
    rc |= expect_contains("voice qualify toast", state.ui_msg, "Voice qualify -> 100 ms");

    rc |= expect_int("voice hold high queued", post_i32(DSD_APP_CMD_SCAN_VOICE_HOLD_MS_SET, 9999999),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("voice hold high drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("voice hold clamped high", opts.scan_voice_hold_ms, 600000);
    rc |= expect_contains("voice hold toast", state.ui_msg, "Voice hold -> 600000 ms");

    rc |= expect_int("voice qualify in-range queued", post_i32(DSD_APP_CMD_SCAN_VOICE_QUALIFY_MS_SET, 1500),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("voice qualify in-range drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("voice qualify kept", opts.scan_voice_qualify_ms, 1500);

    /* Short payloads drain without touching state, like the short key vectors. */
    {
        uint8_t short_payload = 0xFFU;
        int before_only = opts.scan_voice_only;
        int before_qualify = opts.scan_voice_qualify_ms;
        dsd_app_command_submit(DSD_APP_CMD_SCAN_VOICE_ONLY_SET, &short_payload, sizeof(short_payload));
        dsd_app_command_submit(DSD_APP_CMD_SCAN_VOICE_QUALIFY_MS_SET, &short_payload, sizeof(short_payload));
        rc |= expect_int("short voice payloads drained", dsd_app_drain_cmds(&opts, &state), 2);
        rc |= expect_int("short voice-only ignored", opts.scan_voice_only, before_only);
        rc |= expect_int("short voice qualify ignored", opts.scan_voice_qualify_ms, before_qualify);
    }

    /* The ms windows collapse onto the newest value; the flag lands every time. */
    rc |= expect_int("voice qualify first queued", post_i32(DSD_APP_CMD_SCAN_VOICE_QUALIFY_MS_SET, 1000),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("voice qualify coalesces", post_i32(DSD_APP_CMD_SCAN_VOICE_QUALIFY_MS_SET, 2000),
                     DSD_APP_COMMAND_SUBMIT_COALESCED);
    rc |= expect_int("voice hold first queued", post_i32(DSD_APP_CMD_SCAN_VOICE_HOLD_MS_SET, 3000),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("voice hold coalesces", post_i32(DSD_APP_CMD_SCAN_VOICE_HOLD_MS_SET, 4000),
                     DSD_APP_COMMAND_SUBMIT_COALESCED);
    rc |= expect_int("coalesced voice windows drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_int("voice qualify kept latest", opts.scan_voice_qualify_ms, 2000);
    rc |= expect_int("voice hold kept latest", opts.scan_voice_hold_ms, 4000);

    rc |= expect_int("voice-only first queued", post_i32(DSD_APP_CMD_SCAN_VOICE_ONLY_SET, 1),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("voice-only never coalesces", post_i32(DSD_APP_CMD_SCAN_VOICE_ONLY_SET, 0),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("voice-only pair drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_int("voice-only kept latest", opts.scan_voice_only, 0);

    freeState(&state);
    return rc;
}

static int
test_scoped_setting_toggles(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    init_test_context(opts, state);
    int rc = 0;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    opts->use_cosine_filter = 1;
    opts->monitor_input_audio = 0;
    opts->inverted_dmr = 0;
    opts->inverted_x2tdma = 0;
    opts->inverted_dpmr = 0;
    opts->inverted_m17 = 0;
    rc |= expect_int("enter DMR for toggles", dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR), 0);
    const int commands[] = {DSD_APP_CMD_INV_DMR_TOGGLE,       DSD_APP_CMD_INV_X2_TOGGLE,
                            DSD_APP_CMD_INV_DPMR_TOGGLE,      DSD_APP_CMD_INV_M17_TOGGLE,
                            DSD_APP_CMD_COSINE_FILTER_TOGGLE, DSD_APP_CMD_INPUT_MONITOR_TOGGLE};
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        if (commands[i] == DSD_APP_CMD_INPUT_MONITOR_TOGGLE) {
            const dsd_call_observation call = {.protocol = DSD_SYNC_DMR_BS_VOICE_POS,
                                               .kind = DSD_CALL_KIND_GROUP_VOICE,
                                               .ota_target_id = 1201,
                                               .observed_m = 1.0};
            rc |= expect_int("seed call before monitor toggle",
                             dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN), 1);
            state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
            state->rf_mod = 2;
            state->sps_hunt_counter = 17;
        }
        rc |= expect_int("scoped toggle queued", dsd_app_command_action(commands[i]), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int("scoped toggle drained", dsd_app_drain_cmds(opts, state), 1);
    }
    dsd_call_snapshot call;
    rc |= expect_int("monitor keeps call", dsd_call_state_get(state, 0, &call), 1);
    rc |= expect_int("monitor keeps call active", call.phase, DSD_CALL_PHASE_ACTIVE);
    rc |= expect_int("monitor keeps acquired sync", state->synctype, DSD_SYNC_DMR_BS_VOICE_POS);
    rc |= expect_int("monitor keeps acquired modulation", state->rf_mod, 2);
    rc |= expect_int("monitor keeps hunt accounting", state->sps_hunt_counter, 17);
    rc |= expect_int("hop to NXDN", dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NXDN48), 0);
    rc |= expect_int("hop keeps invert", opts->inverted_dmr, 1);
    rc |= expect_int("hop keeps filter", opts->use_cosine_filter, 0);
    rc |= expect_int("hop keeps monitor", opts->monitor_input_audio, 1);
    dsd_scan_mode_leave(opts, state);
    rc |= expect_int("exit keeps invert", opts->inverted_dmr, 1);
    rc |= expect_int("exit keeps X2 invert", opts->inverted_x2tdma, 1);
    rc |= expect_int("exit keeps dPMR invert", opts->inverted_dpmr, 1);
    rc |= expect_int("exit keeps M17 invert", opts->inverted_m17, 1);
    rc |= expect_int("exit keeps filter", opts->use_cosine_filter, 0);
    rc |= expect_int("exit keeps monitor", opts->monitor_input_audio, 1);
    rc |= expect_int("enter P25 for helper toggle", dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_P25), 0);
    rc |= expect_int("helper toggle queued", dsd_app_command_action(DSD_APP_CMD_MOD_P2_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("helper toggle drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_int("helper remains on row", opts->mod_p25p2_profile_lock, 1);
    rc |= expect_int("helper applies 6000 on first press", state->sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    rc |= expect_int("helper applies current input timing", state->samplesPerSymbol, 8);
    dsd_scan_mode_leave(opts, state);
    rc |= expect_int("exit keeps helper", opts->mod_p25p2_profile_lock, 1);
    rc |= expect_int("exit keeps helper profile", state->sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    /* These commands leave the trunk-scan owner running. Its decoder scope must
     * survive until the coordinator switches targets or shuts down. */
    opts->trunk_scan_enabled = 1;
    rc |= expect_int("enter target NXDN", dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NXDN48), 0);
    rc |= expect_int("trunk enable queued", dsd_app_command_set_i32(DSD_APP_CMD_TRUNK_SET, 1),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("trunk enable drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_int("enable preserves target class", dsd_scan_mode_active(state), DSD_SCAN_MODE_NXDN48);
    rc |= expect_int("release queued with target owner", dsd_app_command_action(DSD_APP_CMD_TUNER_RELEASE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("release drained with target owner", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_int("release preserves target owner", opts->trunk_scan_enabled, 1);
    rc |= expect_int("release preserves target class", dsd_scan_mode_active(state), DSD_SCAN_MODE_NXDN48);
    post_empty(DSD_APP_CMD_SCANNER_TOGGLE);
    rc |= expect_int("scanner toggle during trunk scan drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_int("trunk scan excludes conventional scanner", opts->scanner_mode, 0);
    rc |= expect_int("refused scanner preserves target", dsd_scan_mode_active(state), DSD_SCAN_MODE_NXDN48);
    freeState(state);
    free(state);
    free(opts);
    return rc;
}

static int
test_scoped_mode_commands_and_config(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    init_decode_mode_context(opts, state);
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 96000;
    int rc = 0;
    rc |= expect_int("configured NXDN",
                     dsd_apply_decode_mode_preset(DSDCFG_MODE_NXDN48, DSD_DECODE_PRESET_PROFILE_CLI, opts, state), 0);
    opts->scanner_mode = 1;
    rc |= expect_int("enter scan P25", dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_P25), 0);
    state->sps_hunt_counter = 17;
    rc |= expect_int("repeat configured mode queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_NXDN48),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("repeat configured mode drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_int("repeat keeps row P25", opts->frame_p25p1, 1);
    rc |= expect_int("repeat keeps dwell", state->sps_hunt_counter, 17);
    rc |= expect_int("change configured mode queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_M17),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("change configured mode drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_int("row still P25", opts->frame_p25p1, 1);
    rc |= expect_int("row excludes M17", opts->frame_m17, 0);
    dsdneoUserConfig saved;
    dsd_snapshot_opts_to_user_config(opts, state, &saved);
    rc |= expect_int("configuration saves baseline M17", saved.decode_mode, DSDCFG_MODE_M17);
    rc |= expect_int("scanner toggle queued", dsd_app_command_action(DSD_APP_CMD_SCANNER_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("scanner toggle drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_int("exit restores M17", opts->frame_m17, 1);
    rc |= expect_int("exit restores M17 filter", opts->use_cosine_filter, 0);
    rc |= expect_int("exit restores configured timing at live input rate", state->samplesPerSymbol, 20);
    rc |= expect_int("exit restores configured profile", state->sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    opts->scanner_mode = 1;
    rc |= expect_int("reenter scan P25", dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_P25), 0);
    dsd_key_set row_keys = {0};
    row_keys.present = 1;
    opts->mod_cli_lock = 0;
    rc |= expect_int("config exit row keys", dsd_scan_keys_enter(state, &row_keys), 1);
    dsdneoUserConfig cfg = {0};
    cfg.has_trunking = 1;
    cfg.trunk_scanner = 0;
    cfg.has_mode = 1;
    cfg.decode_mode = DSDCFG_MODE_NXDN48;
    rc |= expect_int("config exit queued", dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof(cfg)),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("config exit drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_int("config stops scanner", opts->scanner_mode, 0);
    rc |= expect_int("config releases scope", dsd_scan_mode_active(state), DSD_SCAN_MODE_INHERIT);
    rc |= expect_int("config keeps new baseline", opts->frame_nxdn48, 1);
    rc |= expect_int("config releases P25", opts->frame_p25p1, 0);
    rc |= expect_int("config releases row keys", state->scan_keys_active_set, 0);
    freeState(state);
    free(state);
    free(opts);
    return rc;
}

static int
test_scoped_row_option_commands(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    init_test_context(opts, state);
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    opts->scan_voice_only = 0;
    opts->scan_voice_hold_ms = 2000;
    opts->aggressive_framesync = 1;
    opts->dmr_crc_relaxed_default = 0;
    opts->dmr_mute_encL = opts->dmr_mute_encR = 1;
    opts->trunk_tune_data_calls = 0;
    opts->trunk_tune_enc_calls = 1;
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", "global.csv");
    state->M = 0;
    int rc = expect_int("enter option row", dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR), 0);
    const dsd_scan_option_values row = {.present = DSD_SCAN_OPT_FORCE | DSD_SCAN_OPT_CRC | DSD_SCAN_OPT_VOICE
                                                   | DSD_SCAN_OPT_HOLD | DSD_SCAN_OPT_GROUP | DSD_SCAN_OPT_MUTE_DMR
                                                   | DSD_SCAN_OPT_DATA | DSD_SCAN_OPT_ENC,
                                        .force = 0x21,
                                        .strict_crc = 0,
                                        .voice_only = 1,
                                        .hold_ms = 4000,
                                        .mute_dmr = 1,
                                        .tune_data_calls = 1,
                                        .tune_enc_calls = 0,
                                        .group_file = "row.csv"};
    rc |= expect_int("install row options", dsd_scan_mode_options(opts, state, &row), 0);
    rc |= expect_int("queue force default", post_empty(DSD_APP_CMD_FORCE_PRIV_TOGGLE), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("queue CRC default", post_empty(DSD_APP_CMD_AGGR_SYNC_TOGGLE), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("queue hold default", dsd_app_command_set_i32(DSD_APP_CMD_SCAN_VOICE_HOLD_MS_SET, 3000),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("queue mute default", post_empty(DSD_APP_CMD_ALL_MUTES_TOGGLE), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("queue data default", post_empty(DSD_APP_CMD_TRUNK_DATA_TOGGLE), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |=
        expect_int("queue encrypted default", post_empty(DSD_APP_CMD_TRUNK_ENC_TOGGLE), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("option commands drained", dsd_app_drain_cmds(opts, state), 6);
    rc |= expect_int("row keeps data policy", opts->trunk_tune_data_calls, 1);
    rc |= expect_int("row keeps encrypted policy", opts->trunk_tune_enc_calls, 0);
    rc |= expect_int("row keeps force", state->M, 0x21);
    rc |= expect_int("row keeps hold", opts->scan_voice_hold_ms, 4000);
    rc |= expect_int("row keeps mute", opts->dmr_mute_encL, 1);
    dsd_scan_settings configured;
    dsd_scan_mode_configured(opts, state, &configured);
    rc |= expect_int("configured force updated", configured.force_key, 1);
    rc |= expect_int("configured mute updated", configured.dmr_mute_encL, 0);
    rc |= expect_int("configured CRC updated", configured.aggressive_framesync, 0);
    rc |= expect_int("configured CSBK CRC default preserved", configured.dmr_crc_relaxed_default, 0);
    rc |= expect_int("configured data updated", configured.trunk_tune_data_calls, 1);
    rc |= expect_int("configured encrypted updated", configured.trunk_tune_enc_calls, 0);
    rc |= expect_int("queue data default again", post_empty(DSD_APP_CMD_TRUNK_DATA_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("queue encrypted default again", post_empty(DSD_APP_CMD_TRUNK_ENC_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("policy commands drained", dsd_app_drain_cmds(opts, state), 2);
    rc |= expect_int("row data override survives toggle", opts->trunk_tune_data_calls, 1);
    rc |= expect_int("row encrypted override survives toggle", opts->trunk_tune_enc_calls, 0);
    dsdneoUserConfig saved;
    dsd_snapshot_opts_to_user_config(opts, state, &saved);
    rc |= expect_int("saved configured data", saved.trunk_tune_data_calls, 0);
    rc |= expect_int("saved configured encrypted", saved.trunk_tune_enc_calls, 1);
    rc |= expect_int("saved configured hold", saved.trunk_scan_voice_hold_ms, 3000);
    rc |= expect_int("saved configured gate", saved.trunk_scan_voice_only, 0);
    rc |= expect_str("saved configured groups", saved.trunk_group_csv, "global.csv");
    rc |= expect_int("clear row options", dsd_scan_mode_options(opts, state, NULL), 0);
    rc |= expect_int("clear options restores force", state->M, 1);
    rc |= expect_int("clear options restores hold", opts->scan_voice_hold_ms, 3000);
    rc |= expect_int("clear options restores mute", opts->dmr_mute_encL, 0);
    rc |= expect_str("clear options restores groups", opts->group_in_file, "global.csv");
    rc |= expect_int("clear options restores data policy", opts->trunk_tune_data_calls, 0);
    rc |= expect_int("clear options restores encrypted policy", opts->trunk_tune_enc_calls, 1);
    dsd_scan_mode_leave(opts, state);
    freeState(state);
    free(state);
    free(opts);
    return rc;
}

/* Direct key commands keep their live key ownership while their unmute decision
 * persists in the configured scope, including beneath an explicit row mute. */
static int
test_scoped_direct_key_mutes(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    init_test_context(opts, state);
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    const int commands[] = {DSD_APP_CMD_KEY_BASIC_SET, DSD_APP_CMD_KEY_SCRAMBLER_SET, DSD_APP_CMD_KEY_RC4DES_SET,
                            DSD_APP_CMD_KEY_HYTERA_SET, DSD_APP_CMD_KEY_AES_SET};
    int rc = 0;
    for (int row_mutes = 0; row_mutes < 2; row_mutes++) {
        for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
            opts->dmr_mute_encL = opts->dmr_mute_encR = 1;
            state->R = 99;
            dsd_key_set keys = {0};
            keys.present = 1;
            keys.scalars.R = 55;
            rc |= expect_true("enter row keys", dsd_scan_keys_enter(state, &keys));
            rc |= expect_int("enter key command scope", dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR), 0);
            dsd_scan_option_values row = {.present = DSD_SCAN_OPT_MUTE_DMR, .mute_dmr = 1};
            (void)dsd_scan_mode_options(opts, state, row_mutes ? &row : NULL);
            uint64_t payload[5] = {7, 8, 9, 10, 11};
            uint32_t short_key = 7;
            const void* data = i < 2 ? (const void*)&short_key : (const void*)payload;
            size_t size = i < 2    ? sizeof(uint32_t)
                          : i == 2 ? sizeof(uint64_t)
                          : i == 3 ? sizeof(dsd_app_hytera_key_payload)
                                   : sizeof(dsd_app_aes_key_payload);
            rc |= expect_int("post direct key", dsd_app_command_submit(commands[i], data, size),
                             DSD_APP_COMMAND_SUBMIT_QUEUED);
            rc |= expect_int("direct key drained", dsd_app_drain_cmds(opts, state), 1);
            rc |= expect_int("row mute respected left", opts->dmr_mute_encL, row_mutes);
            rc |= expect_int("row mute respected right", opts->dmr_mute_encR, row_mutes);
            dsd_scan_settings configured;
            dsd_scan_mode_configured(opts, state, &configured);
            rc |= expect_int("direct key saved left unmute", configured.dmr_mute_encL, 0);
            rc |= expect_int("direct key saved right unmute", configured.dmr_mute_encR, 0);
            rc |= expect_true("direct key retains row ownership", state->scan_keys_active_set != 0);
            const uint64_t live[] = {state->K, state->R, state->RR, state->K4, state->A4[0]};
            const uint64_t expected[] = {7, 7, 7, 11, 10};
            rc |= expect_true("direct key updates live material", live[i] == expected[i]);
            post_empty(DSD_APP_CMD_AGGR_SYNC_TOGGLE);
            (void)dsd_app_drain_cmds(opts, state);
            rc |= expect_int("CRC toggle preserves row mute", opts->dmr_mute_encL, row_mutes);
            (void)dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NXDN48);
            (void)dsd_scan_mode_options(opts, state, NULL);
            rc |= expect_int("next row inherits unmute", opts->dmr_mute_encL, 0);
            dsd_scan_keys_leave(state);
            rc |= expect_true("leave restores global key", state->R == 99);
            dsd_scan_mode_leave(opts, state);
        }
    }
    for (int relaxed = 0; relaxed < 2; relaxed++) {
        opts->dmr_crc_relaxed_default = (uint8_t)relaxed;
        for (int toggle = 0; toggle < 2; toggle++) {
            post_empty(DSD_APP_CMD_AGGR_SYNC_TOGGLE);
            (void)dsd_app_drain_cmds(opts, state);
            rc |= expect_int("menu CRC toggle preserves CSBK policy", opts->dmr_crc_relaxed_default, relaxed);
        }
    }
    freeState(state);
    free(state);
    free(opts);
    return rc;
}

static int
test_source_alias_commands(void) {
    int rc = 0;
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    const char* path = "ui_cmd_queue_sources.csv";
    const char* missing = "ui_cmd_queue_sources_missing.csv";
    static const unsigned char data[] = "id,name,tags\n1201,Engine 21,Fire\n2000-2099,Dispatch,Ops\n";
    char name[50];
    remove(missing);
    init_test_context(opts, state);
    post_string(DSD_APP_CMD_IMPORT_SRC_LIST, missing);
    rc |= expect_int("source missing drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_contains("source failure toast", state->ui_msg, "Failed: Source ID list import ->");
    rc |= expect_str("source missing no path", opts->src_in_file, "");
    rc |= expect_int("source missing not loaded", dsd_source_alias_loaded(state), 0);
    rc |= write_file_bytes(path, data, sizeof(data) - 1U);
    post_string(DSD_APP_CMD_IMPORT_SRC_LIST, path);
    rc |= expect_int("source import drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_contains("source success toast", state->ui_msg, "Applied: Source ID list imported ->");
    rc |= expect_str("source imported path", opts->src_in_file, path);
    rc |= expect_int("source exact lookup", dsd_source_alias_lookup(state, 1201, name, sizeof name), 1);
    rc |= expect_str("source exact name", name, "Engine 21");
    rc |= expect_int("source range lookup", dsd_source_alias_lookup(state, 2050, name, sizeof name), 1);
    rc |= expect_str("source range name", name, "Dispatch");
    post_string(DSD_APP_CMD_IMPORT_SRC_LIST, missing);
    rc |= expect_int("source failed replacement drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_contains("source replacement failure toast", state->ui_msg, "Failed: Source ID list import ->");
    rc |= expect_str("source failed replacement keeps path", opts->src_in_file, path);
    rc |= expect_int("source failed replacement keeps lookup", dsd_source_alias_lookup(state, 1201, name, sizeof name),
                     1);
    rc |= expect_str("source failed replacement keeps name", name, "Engine 21");
    static const unsigned char empty[] = "id,name\n";
    rc |= write_file_bytes(path, empty, sizeof(empty) - 1U);
    post_string(DSD_APP_CMD_IMPORT_SRC_LIST, path);
    rc |= expect_int("source empty import drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_int("source empty import loaded", dsd_source_alias_loaded(state), 1);
    rc |= expect_int("source empty import replaces aliases", (int)dsd_source_alias_count(state), 0);
    rc |= expect_contains("source empty import applied", state->ui_msg, "Applied:");
    /* Through the public action helper, as the Qt bridge submits it: a clear that is not
       in the action allowlist is rejected before it is ever queued. */
    rc |= expect_int("source clear accepted as action", dsd_app_command_action(DSD_APP_CMD_IMPORT_SRC_LIST_CLEAR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("source clear drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_str("source clear toast", state->ui_msg, "Applied: Source ID list cleared");
    rc |= expect_str("source cleared path", opts->src_in_file, "");
    rc |= expect_int("source cleared lookup", dsd_source_alias_lookup(state, 1201, name, sizeof name), 0);
    /* The sibling clears the same frontend flow relies on. */
    rc |= expect_int("channel map clear accepted as action",
                     dsd_app_command_action(DSD_APP_CMD_IMPORT_CHANNEL_MAP_CLEAR), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("group list clear accepted as action", dsd_app_command_action(DSD_APP_CMD_IMPORT_GROUP_LIST_CLEAR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("keys clear accepted as action", dsd_app_command_action(DSD_APP_CMD_IMPORT_KEYS_CLEAR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("sibling clears drained", dsd_app_drain_cmds(opts, state), 3);
    freeState(state);
    free(state);
    free(opts);
    remove(path);
    return rc;
}

static int
test_talkgroup_list_commands(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    init_test_context(opts, state);
    const char* path = "dsd_neo_test_talkgroup_commands.csv";
    static const char csv[] = "DEC,Mode,Name,Tag\n1001,A,Fire Dispatch,FIRE\n2001,A,EMS Dispatch,EMS\n"
                              "3001,D,Radio,FIRE\n4001,A,Untagged\n";
    int rc = write_file_bytes(path, csv, sizeof csv - 1U);
    DSD_SNPRINTF(opts->group_in_file, sizeof opts->group_in_file, "%s", path);
    rc |= expect_int("load talkgroup command fixture", dsd_tg_policy_reload_group_file(opts, state), 0);
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    seed_active_p25_voice(opts, state, 851000000L, 852000000L, 1001);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
#endif
    dsd_app_tg_listen_payload one = {1001, 1001, 0};
    rc |= expect_int("block talkgroup queued", dsd_app_command_set_tg_listen(&one), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("block talkgroup drained", dsd_app_drain_cmds(opts, state), 1);
    dsd_tg_policy_lookup lookup;
    rc |= expect_int("blocked talkgroup lookup", dsd_tg_policy_lookup_id(state, 1001, &lookup), 0);
    rc |= expect_str("blocked mode", lookup.entry.mode, "B");
    rc |= expect_str("blocked name retained", lookup.entry.name, "Fire Dispatch");
    rc |= expect_str("blocked tags retained", lookup.entry.tags, "FIRE");
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    rc |= expect_int("blocked call returns to CC", g_cc_tune_calls, 1);
#endif
    FILE* file = dsd_fopen_existing_regular_file(path, "r");
    char contents[512] = {0};
    if (file) {
        size_t bytes = fread(contents, 1, sizeof contents - 1U, file);
        contents[bytes] = '\0';
        const int close_result = fclose(file);
        rc |= expect_int("close rewritten groups", close_result, 0);
    } else {
        rc = 1;
    }
    rc |= expect_contains("rewrite contains named blocked row", contents, "1001,B,Fire Dispatch,FIRE\n");
    rc |= expect_int("reload persisted blocked mode", dsd_tg_policy_reload_group_file(opts, state), 0);
    rc |= expect_int("reloaded talkgroup lookup", dsd_tg_policy_lookup_id(state, 1001, &lookup), 0);
    rc |= expect_str("reloaded mode", lookup.entry.mode, "B");
    one.listen = 1;
    rc |= expect_int("listen queued", dsd_app_command_set_tg_listen(&one), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("listen drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_int("listening lookup", dsd_tg_policy_lookup_id(state, 1001, &lookup), 0);
    rc |= expect_str("listening mode", lookup.entry.mode, "A");
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    rc |= expect_int("listen does not retune", g_cc_tune_calls, 1);
#endif
    dsd_app_tg_listen_all_payload all = {0, "FIRE"};
    rc |= expect_int("category block queued", dsd_app_command_set_tg_listen_all(&all), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("category block drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_int("category row lookup", dsd_tg_policy_lookup_id(state, 1001, &lookup), 0);
    rc |= expect_str("category row blocked", lookup.entry.mode, "B");
    rc |= expect_int("other category lookup", dsd_tg_policy_lookup_id(state, 2001, &lookup), 0);
    rc |= expect_str("other category unchanged", lookup.entry.mode, "A");
    rc |= expect_int("alias row lookup", dsd_tg_policy_lookup_id(state, 3001, &lookup), 0);
    rc |= expect_str("alias row unchanged", lookup.entry.mode, "D");
    rc |= expect_int("untagged row lookup", dsd_tg_policy_lookup_id(state, 4001, &lookup), 0);
    rc |= expect_str("untagged row unchanged", lookup.entry.mode, "A");
    freeState(state);
    free(state);
    free(opts);
    remove(path);
    return rc;
}

static int
lockout_patched_call(dsd_opts* opts, dsd_state* state, uint32_t supergroup, uint32_t member) {
    const dsd_call_observation observation = {.protocol = DSD_SYNC_P25P1_POS,
                                              .slot = 0U,
                                              .kind = DSD_CALL_KIND_GROUP_VOICE,
                                              .ota_target_id = supergroup,
                                              .policy_target_id = member,
                                              .ota_source_id = 1,
                                              .observed_m = dsd_decode_now_mono_s()};
    int rc = expect_int("seed patched call", dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN), 1);
    dsd_event_sync_slot(opts, state, 0U);
    rc |= expect_true("patched lockout queued", dsd_app_command_set_u8(DSD_APP_CMD_LOCKOUT_SLOT, 0U) > 0);
    rc |= expect_int("patched lockout drained", dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/* On a patched P25 call the policy target is whichever member WG the grant
 * matched, while the frontends show the supergroup. User blocks address the
 * supergroup: Lock out writes it, and a list edit blocking it releases the call.
 * A block on the member alone would leave the supergroup free to tune again. */
static int
test_patched_call_user_blocks_target_supergroup(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    init_test_context(opts, state);
    const char* path = "dsd_neo_test_patched_lockout.csv";
    static const char csv[] = "DEC,Mode,Name,Tag\n2001,A,Member WG,PATCH\n2002,A,Other Member WG,PATCH\n";
    int rc = write_file_bytes(path, csv, sizeof csv - 1U);
    DSD_SNPRINTF(opts->group_in_file, sizeof opts->group_in_file, "%s", path);
    rc |= expect_int("load patched lockout fixture", dsd_tg_policy_reload_group_file(opts, state), 0);
    dsd_tg_policy_lookup lookup;

    opts->persist_tg_lockouts = 0;
    rc |= lockout_patched_call(opts, state, 1001, 2001);
    rc |= expect_true("session lockout avoids the supergroup", dsd_tg_policy_session_avoid_contains(state, 1001));
    rc |= expect_true("session lockout leaves the member", !dsd_tg_policy_session_avoid_contains(state, 2001));
    rc |= expect_str("session lockout names the supergroup", state->ui_msg, "TG 1001 locked out for this session");
    rc |= expect_contains("session lockout event names the supergroup",
                          state->event_history_s[0].Event_History_Items[1].internal_str,
                          "Target: 1001; has been locked out; Session Only.");
    dsd_tg_policy_session_avoid_clear(state);

    opts->persist_tg_lockouts = 1;
    rc |= lockout_patched_call(opts, state, 1002, 2001);
    rc |= expect_int("saved lockout supergroup lookup", dsd_tg_policy_lookup_id(state, 1002, &lookup), 0);
    rc |= expect_str("saved lockout blocks the supergroup", lookup.entry.mode, "B");
    rc |= expect_int("saved lockout member lookup", dsd_tg_policy_lookup_id(state, 2001, &lookup), 0);
    rc |= expect_str("saved lockout leaves the member", lookup.entry.mode, "A");

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    // Blocking the supergroup in the list releases its patched call, even
    // though the member the call was matched on still listens.
    seed_active_p25_patched_voice(opts, state, 851000000L, 852000000L, 1101, 2002);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    dsd_app_tg_listen_payload block_sg = {1101, 1101, 0};
    rc |= expect_true("supergroup block queued", dsd_app_command_set_tg_listen(&block_sg) > 0);
    rc |= expect_int("supergroup block drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_int("supergroup block releases patched call", g_cc_tune_calls, 1);

    // An allow-list miss on the supergroup stays open to a listed member, so an
    // unrelated edit must not release a call followed through that member.
    opts->trunk_use_allow_list = 1;
    seed_active_p25_patched_voice(opts, state, 851000000L, 852000000L, 1201, 2002);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    dsd_app_tg_listen_payload block_other = {3001, 3001, 0};
    rc |= expect_true("unrelated block queued", dsd_app_command_set_tg_listen(&block_other) > 0);
    rc |= expect_int("unrelated block drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_int("allow-list miss on supergroup keeps member call", g_cc_tune_calls, 0);
    opts->trunk_use_allow_list = 0;

    // A Lock out whose return to the CC is deferred leaves the patched call
    // running and judged on its member; the supergroup avoid must still mute it,
    // as locking the member used to.
    opts->persist_tg_lockouts = 0;
    seed_active_p25_patched_voice(opts, state, 851000000L, 852000000L, 1301, 2002);
    state->p25_patch_count = 1;
    state->p25_patch_sgid[0] = 1301;
    state->p25_patch_is_patch[0] = 1U;
    state->p25_patch_active[0] = 1U;
    state->p25_patch_last_update[0] = time(NULL);
    state->p25_patch_wgid_count[0] = 1U;
    state->p25_patch_wgid[0][0] = 2002;
    int muted = -1;
    rc |= expect_true("patched call audible before lockout",
                      dsd_audio_group_gate_mono(opts, state, 1301, 0, &muted) == 0 && muted == 0);
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_DEFERRED);
    rc |= expect_true("deferred patched lockout queued", dsd_app_command_set_u8(DSD_APP_CMD_LOCKOUT_SLOT, 0U) > 0);
    rc |= expect_int("deferred patched lockout drained", dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_true("deferred patched lockout avoids the supergroup",
                      dsd_tg_policy_session_avoid_contains(state, 1301));
    rc |= expect_call_phase("deferred patched lockout keeps the call", state, 0U, DSD_CALL_PHASE_ACTIVE);
    rc |= expect_true("deferred patched lockout mutes the call",
                      dsd_audio_group_gate_mono(opts, state, 1301, 0, &muted) == 0 && muted == 1);
    dsd_tg_policy_session_avoid_clear(state);
#endif
    freeState(state);
    free(state);
    free(opts);
    remove(path);
    return rc;
}

/* An export completion must survive the rest of a drain before the UI polls. */
static int
test_talkgroup_export_result(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    init_test_context(opts, state);
    const char* path = "dsd_neo_test_d1_retained_export.csv";
    remove(path);
    int rc = expect_int("seed retained export policy", dsd_tg_policy_set_mode(state, 42, 42, "A"), 0);

    union {
        unsigned char bytes[sizeof(dsd_app_tg_export_payload) + 128];
        // cppcheck-suppress unusedStructMember -- this member provides alignment for the payload header.
        uint64_t align;
    } storage = {0};

    dsd_app_tg_export_payload* request = (dsd_app_tg_export_payload*)storage.bytes;
    dsd_tg_policy_table_version(state, &request->policy_context, &request->policy_generation);
    DSD_SNPRINTF(request->path, sizeof storage.bytes - offsetof(dsd_app_tg_export_payload, path), "%s", path);
    rc |= expect_int("retained export queued",
                     dsd_app_command_submit(DSD_APP_CMD_TG_LIST_EXPORT, request, sizeof storage),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    post_empty(DSD_APP_CMD_IMPORT_SRC_LIST_CLEAR);
    rc |= expect_int("export and unrelated command drain together", dsd_app_drain_cmds(opts, state), 2);
    rc |= expect_str("unrelated command overwrote export toast", state->ui_msg, "Applied: Source ID list cleared");
    dsd_app_tg_export_result result;
    rc |= expect_int("completion available on first poll", dsd_app_tg_export_result_get(&result), 1);
    rc |= expect_true("first completion has sequence", result.sequence != 0);
    rc |= expect_int("retained export succeeded", result.success, 1);
    rc |= expect_true("retained request context", result.policy_context == request->policy_context);
    rc |= expect_true("retained request generation", result.policy_generation == request->policy_generation);
    rc |= expect_str("retained written path", result.path, path);
    rc |= expect_str("persistence activated before success", opts->group_in_file, result.path);
    const dsd_app_tg_export_result success = result;
    post_empty(DSD_APP_CMD_UI_MSG_CLEAR);
    dsd_app_drain_cmds(opts, state);
    dsd_app_tg_export_result_get(&result);
    rc |= expect_true("unrelated drain preserves sequence", result.sequence == success.sequence);
    rc |= expect_str("unrelated drain preserves path", result.path, path);
    rc |= expect_int("null completion output refused", dsd_app_tg_export_result_get(NULL), 0);

    /* Failures retain the request identity too, including refused contexts. */
    for (int failure = 0; failure < 4; ++failure) {
        dsd_scan_row_profile profile = {0};
        if (failure == 2) {
            profile.values.present = DSD_SCAN_OPT_GROUP;
            dsd_scan_groups_begin(state);
            dsd_scan_groups_enter(state, &profile);
        }
        dsd_tg_policy_table_version(state, &request->policy_context, &request->policy_generation);
        if (failure == 0) {
            request->policy_context++;
        } else if (failure == 1) {
            request->policy_generation++;
        } else if (failure == 3) {
            DSD_SNPRINTF(request->path, sizeof storage.bytes - offsetof(dsd_app_tg_export_payload, path),
                         "%s/missing.csv", path);
        }
        uint64_t sequence = result.sequence;
        dsd_app_command_submit(DSD_APP_CMD_TG_LIST_EXPORT, request, sizeof storage);
        post_empty(DSD_APP_CMD_IMPORT_SRC_LIST_CLEAR);
        rc |= expect_int("failed export and unrelated command drained", dsd_app_drain_cmds(opts, state), 2);
        rc |= expect_int("failed export result available", dsd_app_tg_export_result_get(&result), 1);
        rc |= expect_true("each completion advances sequence", result.sequence > sequence);
        rc |= expect_int("retained failure", result.success, 0);
        rc |= expect_true("failed request context retained", result.policy_context == request->policy_context);
        rc |= expect_true("failed request generation retained", result.policy_generation == request->policy_generation);
        rc |= expect_str("failed destination retained", result.path, request->path);
        rc |= expect_str("failed export keeps successful persistence", opts->group_in_file, path);
        if (failure == 2) {
            dsd_scan_groups_leave(state);
        }
    }
    /* Identical requests are still distinct completions; reads return owned copies. */
    DSD_SNPRINTF(request->path, sizeof storage.bytes - offsetof(dsd_app_tg_export_payload, path), "%s", path);
    dsd_tg_policy_table_version(state, &request->policy_context, &request->policy_generation);
    for (int repeat = 0; repeat < 2; ++repeat) {
        uint64_t sequence = result.sequence;
        dsd_app_command_submit(DSD_APP_CMD_TG_LIST_EXPORT, request, sizeof storage);
        post_empty(DSD_APP_CMD_IMPORT_SRC_LIST_CLEAR);
        dsd_app_drain_cmds(opts, state);
        dsd_app_tg_export_result_get(&result);
        rc |= expect_true("repeated successful export advances sequence", result.sequence > sequence && result.success);
        rc |= expect_str("prior owned copy preserved", success.path, path);
    }
    uint64_t sequence = result.sequence;
    dsd_app_command_submit(DSD_APP_CMD_TG_LIST_EXPORT, request, offsetof(dsd_app_tg_export_payload, path));
    post_empty(DSD_APP_CMD_IMPORT_SRC_LIST_CLEAR);
    dsd_app_drain_cmds(opts, state);
    dsd_app_tg_export_result_get(&result);
    rc |= expect_true("invalid envelope publishes failure", result.sequence > sequence && !result.success);
    rc |= expect_str("invalid path is not published", result.path, "");
    freeState(state);
    free(state);
    free(opts);
    remove(path);
    return rc;
}

/* WP-D1: exercise the decoder-thread API, including the resulting effective policy. */
static int
test_talkgroup_row_commands(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    dsd_state* loaded = calloc(1, sizeof(*loaded));
    if (!opts || !state || !loaded) {
        free(opts);
        free(state);
        free(loaded);
        return 1;
    }
    init_test_context(opts, state);
    int rc = 0;
    const char* path = "dsd_neo_test_d1_export.csv";
    remove(path);
    /* Exact A deletion: blocked range, allowed range in allowlist, last allowlist row. */
    for (int scenario = 0; scenario < 3; ++scenario) {
        dsd_tg_policy_clear(state);
        opts->trunk_use_allow_list = scenario != 0;
        opts->trunk_tune_group_calls = 1;
        dsd_tg_policy_entry e;
        if (scenario < 2) {
            dsd_tg_policy_make_exact_entry(1000, scenario == 0 ? "B" : "A", "Range", DSD_TG_POLICY_SOURCE_IMPORTED, &e);
            e.id_end = 1099;
            e.is_range = 1;
            dsd_tg_policy_add_range_entry(state, &e);
        }
        dsd_tg_policy_set_mode(state, 1001, 1001, "A");
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
        seed_active_p25_voice(opts, state, 851000000L, 852000000L, 1001);
        reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
#endif
        dsd_app_tg_range_payload p = {1001, 1001, 0, 0};
        dsd_tg_policy_table_version(state, &p.policy_context, &p.policy_generation);
        dsd_app_command_submit(DSD_APP_CMD_TG_ROW_REMOVE, &p, sizeof p);
        rc |= expect_int("remove drains", dsd_app_drain_cmds(opts, state), 1);
        dsd_tg_policy_decision decision;
        dsd_tg_policy_evaluate_group_call(opts, state, 1001, 0, 0, 0, &decision);
        rc |= expect_int("resulting policy allow", decision.tune_allowed, scenario == 1);
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
        rc |= expect_int("exact-over-range and allowlist release", g_cc_tune_calls, scenario != 1);
        dsd_call_snapshot call;
        dsd_call_state_get(state, 0, &call);
        rc |= expect_int("canonical active state after removal", call.phase == DSD_CALL_PHASE_ACTIVE, scenario == 1);
#endif
    }
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    /* Row-set release, and a metadata edit must not release an already blocked call. */
    dsd_tg_policy_clear(state);
    opts->trunk_use_allow_list = 0;
    dsd_tg_policy_set_mode(state, 1001, 1001, "A");
    seed_active_p25_voice(opts, state, 851000000L, 852000000L, 1001);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    dsd_app_tg_row_payload block = {.id_start = 1001, .id_end = 1001, .fields = DSD_APP_TG_FIELD_LISTEN, .listen = 0};
    dsd_tg_policy_table_version(state, &block.policy_context, &block.policy_generation);
    dsd_app_command_submit(DSD_APP_CMD_TG_ROW_SET, &block, sizeof block);
    dsd_app_drain_cmds(opts, state);
    rc |= expect_int("row set releases newly blocked call", g_cc_tune_calls, 1);
    seed_active_p25_voice(opts, state, 851000000L, 852000000L, 1001);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    block.fields = DSD_APP_TG_FIELD_NAME;
    DSD_SNPRINTF(block.name, sizeof block.name, "%s", "Renamed");
    dsd_tg_policy_table_version(state, &block.policy_context, &block.policy_generation);
    dsd_app_command_submit(DSD_APP_CMD_TG_ROW_SET, &block, sizeof block);
    dsd_app_drain_cmds(opts, state);
    rc |= expect_int("metadata edit does not newly block call", g_cc_tune_calls, 0);
#endif
    dsd_tg_policy_clear(state);
    dsd_tg_policy_entry e;
    const char* modes[] = {"A", "B", "D", "DE"};
    for (int i = 0; i < 4; ++i) {
        dsd_tg_policy_make_exact_entry(2000 + i, modes[i], "Named", DSD_TG_POLICY_SOURCE_IMPORTED, &e);
        e.priority = i * 25;
        e.preempt = i == 1;
        dsd_tg_policy_append_exact(state, &e);
    }
    dsd_tg_policy_make_exact_entry(3000, "D", "Learned", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS, &e);
    dsd_tg_policy_append_exact(state, &e);
    e.id_start = 4000;
    e.id_end = 4099;
    e.is_range = 1;
    DSD_SNPRINTF(e.mode, sizeof e.mode, "%s", "A");
    e.source = DSD_TG_POLICY_SOURCE_IMPORTED;
    dsd_tg_policy_add_range_entry(state, &e);
    dsd_app_tg_row_payload edit = {
        .id_start = 2000, .id_end = 2000, .fields = DSD_APP_TG_FIELD_PRIORITY, .priority = 50};
    dsd_tg_policy_table_version(state, &edit.policy_context, &edit.policy_generation);
    edit.policy_context++;
    dsd_app_command_submit(DSD_APP_CMD_TG_ROW_SET, &edit, sizeof edit);
    dsd_app_drain_cmds(opts, state);
    rc |= expect_contains("stale context set refused", state->ui_msg, "stale");
    dsd_tg_policy_table_version(state, &edit.policy_context, &edit.policy_generation);
    edit.policy_generation++;
    dsd_app_command_submit(DSD_APP_CMD_TG_ROW_SET, &edit, sizeof edit);
    dsd_app_drain_cmds(opts, state);
    rc |= expect_contains("stale generation set refused", state->ui_msg, "stale");
    edit.priority = 99;
    edit.id_start = edit.id_end = 2002;
    dsd_tg_policy_table_version(state, &edit.policy_context, &edit.policy_generation);
    dsd_app_command_submit(DSD_APP_CMD_TG_ROW_SET, &edit, sizeof edit);
    dsd_app_drain_cmds(opts, state);
    dsd_tg_policy_lookup lookup;
    dsd_tg_policy_lookup_id(state, 2002, &lookup);
    rc |= expect_int("alias priority unchanged", lookup.entry.priority, 50);
    dsd_app_tg_range_payload rem = {2002, 2002, edit.policy_context, edit.policy_generation};
    dsd_app_command_submit(DSD_APP_CMD_TG_ROW_REMOVE, &rem, sizeof rem);
    dsd_app_drain_cmds(opts, state);
    rc |= expect_int("mode D remove refused", (int)dsd_tg_policy_entry_count(state), 6);

    union {
        unsigned char bytes[sizeof(dsd_app_tg_export_payload) + 128];
        // cppcheck-suppress unusedStructMember -- this member provides alignment for the payload header.
        uint64_t align;
    } storage = {0};

    dsd_app_tg_export_payload* exp = (dsd_app_tg_export_payload*)storage.bytes;
    DSD_SNPRINTF(exp->path, sizeof storage.bytes - offsetof(dsd_app_tg_export_payload, path), "%s", path);
    dsd_tg_policy_table_version(state, &exp->policy_context, &exp->policy_generation);
    exp->policy_context++;
    dsd_app_command_submit(DSD_APP_CMD_TG_LIST_EXPORT, exp, sizeof storage);
    dsd_app_drain_cmds(opts, state);
    rc |= expect_contains("stale export refused", state->ui_msg, "stale");
    rc |= expect_str("stale export keeps empty path", opts->group_in_file, "");
    dsd_tg_policy_table_version(state, &exp->policy_context, &exp->policy_generation);
    rc |= expect_int("inherited policy scan scope", dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR), 0);
    dsd_app_command_submit(DSD_APP_CMD_TG_LIST_EXPORT, exp, sizeof storage);
    dsd_app_drain_cmds(opts, state);
    rc |= expect_str("export sets persistence path", opts->group_in_file, path);
    rc |= expect_contains("export completion toast", state->ui_msg, "Applied:");
    rc |= expect_int("reload export", dsd_tg_policy_reload_group_file(opts, loaded), 0);
    rc |= expect_int("canonical rows exported", (int)dsd_tg_policy_entry_count(loaded), 6);
    for (size_t i = 0; i < 6; ++i) {
        dsd_tg_policy_entry actual;
        dsd_tg_policy_entry_at(state, i, &e);
        if (!dsd_tg_policy_entry_at(loaded, i, &actual)) {
            rc = 1;
            continue;
        }
        rc |= expect_str("export mode roundtrip", actual.mode, e.mode);
        rc |= expect_str("export name roundtrip", actual.name, e.name);
        rc |= expect_int("export priority roundtrip", actual.priority, e.priority);
        rc |= expect_int("export preempt roundtrip", actual.preempt, e.preempt);
        rc |= expect_int("export range roundtrip", actual.id_end, e.id_end);
    }
    dsd_scan_mode_leave(opts, state);
    rc |= expect_int("rotate inherited policy row", dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NXDN48), 0);
    post_empty(DSD_APP_CMD_ALL_MUTES_TOGGLE);
    dsd_app_drain_cmds(opts, state);
    rc |= expect_str("export path survives rotation and scoped command", opts->group_in_file, path);
    /* A failed export cannot redirect subsequent persistence. */
    DSD_SNPRINTF(exp->path, sizeof storage.bytes - offsetof(dsd_app_tg_export_payload, path), "%s",
                 "dsd_neo_d1_missing_dir/groups.csv");
    dsd_app_command_submit(DSD_APP_CMD_TG_LIST_EXPORT, exp, sizeof storage);
    dsd_app_drain_cmds(opts, state);
    rc |= expect_str("failed export keeps path", opts->group_in_file, path);
    rc |= expect_contains("failed export toast", state->ui_msg, "failed");
    exp->path[0] = 0;
    dsd_app_command_submit(DSD_APP_CMD_TG_LIST_EXPORT, exp, sizeof storage);
    dsd_app_drain_cmds(opts, state);
    rc |= expect_contains("empty export path refused", state->ui_msg, "Invalid");
    DSD_SNPRINTF(exp->path, sizeof storage.bytes - offsetof(dsd_app_tg_export_payload, path), "%s", path);
    edit.id_start = edit.id_end = 2000;
    edit.fields = DSD_APP_TG_FIELD_NAME | DSD_APP_TG_FIELD_TAGS;
    DSD_SNPRINTF(edit.name, sizeof edit.name, "%s", "Persisted");
    DSD_SNPRINTF(edit.tags, sizeof edit.tags, "%s", "FIRE");
    dsd_tg_policy_table_version(state, &edit.policy_context, &edit.policy_generation);
    dsd_app_command_submit(DSD_APP_CMD_TG_ROW_SET, &edit, sizeof edit);
    dsd_app_drain_cmds(opts, state);
    dsd_tg_policy_reload_group_file(opts, loaded);
    dsd_tg_policy_lookup_id(loaded, 2000, &lookup);
    rc |= expect_str("edit persists to exported file", lookup.entry.name, "Persisted");
    rc |= expect_str("tags persist to exported file", lookup.entry.tags, "FIRE");
    rem.id_start = rem.id_end = 2000;
    dsd_tg_policy_table_version(state, &rem.policy_context, &rem.policy_generation);
    rem.policy_generation++;
    dsd_app_command_submit(DSD_APP_CMD_TG_ROW_REMOVE, &rem, sizeof rem);
    dsd_app_drain_cmds(opts, state);
    rc |= expect_contains("stale remove refused", state->ui_msg, "stale");
    dsd_tg_policy_table_version(state, &rem.policy_context, &rem.policy_generation);
    dsd_app_command_submit(DSD_APP_CMD_TG_ROW_REMOVE, &rem, sizeof rem);
    dsd_app_drain_cmds(opts, state);
    dsd_tg_policy_reload_group_file(opts, loaded);
    dsd_tg_policy_lookup_id(loaded, 2000, &lookup);
    rc |= expect_int("remove persists to exported file", lookup.match, DSD_TG_POLICY_MATCH_NONE);
    dsd_scan_row_profile profile = {0};
    profile.values.present = DSD_SCAN_OPT_GROUP;
    dsd_scan_groups_begin(state);
    dsd_scan_groups_enter(state, &profile);
    dsd_tg_policy_table_version(state, &exp->policy_context, &exp->policy_generation);
    dsd_app_command_submit(DSD_APP_CMD_TG_LIST_EXPORT, exp, sizeof storage);
    dsd_app_drain_cmds(opts, state);
    rc |= expect_contains("scan export refused", state->ui_msg, "scan");
    dsd_scan_groups_leave(state);
    remove(path);
    freeState(loaded);
    free(loaded);
    freeState(state);
    free(state);
    free(opts);
    return rc;
}

static int
test_direct_key_updates_preserve_fifo(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    dsd_app_key_direct_payload basic = {DSD_APP_KEY_TYPE_BASIC, "42"};
    dsd_app_key_direct_payload rc4 = {DSD_APP_KEY_TYPE_RC4, "0011223344"};
    int rc = 0;
    rc |=
        expect_int("BASIC direct key queued", dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &basic, sizeof basic),
                   DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |=
        expect_int("independent RC4 direct key queued",
                   dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &rc4, sizeof rc4), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("different direct-key types both drain", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_true("both direct-key slots erased", dsd_app_command_test_storage_cleared());
    DSD_SECURE_ZERO(&basic, sizeof basic);
    DSD_SECURE_ZERO(&rc4, sizeof rc4);
    freeState(&state);
    return rc;
}

static int
test_coalesced_setter_erases_old_tail(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    unsigned char padded[72];
    const int32_t gain = 5;
    DSD_MEMSET(padded, 0x5a, sizeof padded);
    DSD_MEMCPY(padded, &gain, sizeof gain);
    int rc = 0;
    // Gain is coalescible. Fill the accepted envelope beyond its scalar value
    // to prove a shorter overwrite erases bytes rather than only reducing n.
    rc |= expect_int("padded setter queued", dsd_app_command_submit(DSD_APP_CMD_GAIN_SET, padded, sizeof padded),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("short setter coalesced", dsd_app_command_set_i32(DSD_APP_CMD_GAIN_SET, 9),
                     DSD_APP_COMMAND_SUBMIT_COALESCED);
    rc |= expect_true("coalescing erased old payload tail", dsd_app_command_test_tail_padding_cleared());
    rc |= expect_int("coalesced setter drains once", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("new setter value applied", (int)opts.audio_gain, 9);
    rc |= expect_true("coalesced setter slot erased on drain", dsd_app_command_test_storage_cleared());
    DSD_SECURE_ZERO(padded, sizeof padded);
    freeState(&state);
    return rc;
}

static int
test_foundation_commands(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    int rc = 0;
    rc |= expect_int("row set id", DSD_APP_CMD_TG_ROW_SET, 592);
    rc |= expect_int("row remove id", DSD_APP_CMD_TG_ROW_REMOVE, 593);
    rc |= expect_int("export id", DSD_APP_CMD_TG_LIST_EXPORT, 594);
    rc |= expect_int("direct key id", DSD_APP_CMD_KEY_DIRECT_SET, 652);
    rc |= expect_int("force key id", DSD_APP_CMD_FORCE_KEY_SET, 653);
    dsd_app_tg_row_payload row = {0};
    dsd_tg_policy_table_version(&state, &row.policy_context, &row.policy_generation);
    row.id_start = row.id_end = 42;
    row.fields = DSD_APP_TG_FIELD_NAME;
    DSD_SNPRINTF(row.name, sizeof row.name, "%s", "Dispatch");
    dsd_app_command_submit(DSD_APP_CMD_TG_ROW_SET, &row, sizeof row);
    rc |= expect_int("row stub drains", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_contains("row applied toast", state.ui_msg, "Applied:");
    row.policy_context++;
    dsd_app_command_submit(DSD_APP_CMD_TG_ROW_SET, &row, sizeof row);
    dsd_app_drain_cmds(&opts, &state);
    rc |= expect_contains("stale row rejected", state.ui_msg, "stale");
    dsd_app_tg_range_payload range = {42, 42, 0, 0};
    dsd_tg_policy_table_version(&state, &range.policy_context, &range.policy_generation);
    dsd_app_command_submit(DSD_APP_CMD_TG_ROW_REMOVE, &range, sizeof range);
    dsd_app_drain_cmds(&opts, &state);
    rc |= expect_contains("remove applied toast", state.ui_msg, "Applied:");

    union {
        unsigned char bytes[sizeof(dsd_app_tg_export_payload) + 32];
        // cppcheck-suppress unusedStructMember -- this member provides alignment for the payload header.
        uint64_t alignment;
    } export_storage = {0};

    dsd_app_tg_export_payload* export_payload = (dsd_app_tg_export_payload*)export_storage.bytes;
    dsd_tg_policy_table_version(&state, &export_payload->policy_context, &export_payload->policy_generation);
    // The flexible path member owns only the bytes remaining after the header.
    DSD_SNPRINTF(export_payload->path, sizeof export_storage.bytes - offsetof(dsd_app_tg_export_payload, path), "%s",
                 "test.csv");
    dsd_app_command_submit(DSD_APP_CMD_TG_LIST_EXPORT, export_payload, sizeof export_storage);
    dsd_app_drain_cmds(&opts, &state);
    rc |= expect_contains("export applied toast", state.ui_msg, "Applied:");
    remove("test.csv");
    dsd_app_key_direct_payload key = {DSD_APP_KEY_TYPE_RC4, "0011223344"};
    dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &key, sizeof key);
    dsd_app_drain_cmds(&opts, &state);
    rc |= expect_true("direct key applied", state.R == 0x0011223344ULL && state.RR == state.R);
    rc |= expect_true("key absent from toast", strstr(state.ui_msg, key.value) == NULL);
    rc |= expect_true("drained slot erased", dsd_app_command_test_storage_cleared());
    // Put the secret at the eviction head, then fill all 127 usable slots.
    dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &key, sizeof key);
    for (int i = 0; i < 126; ++i) {
        post_empty(DSD_APP_CMD_TOGGLE_COMPACT);
    }
    dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &key, sizeof key);
    rc |= expect_true("evicted sensitive slot erased", dsd_app_command_test_storage_cleared());
    key.value[1] = '\0';
    dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &key, 5);
    rc |= expect_true("short rejected key tail erased", dsd_app_command_test_tail_padding_cleared());
    rc |= expect_int("full queue drains including rejected key", dsd_app_drain_cmds(&opts, &state), 127);
    rc |= expect_true("rejected slot erased", dsd_app_command_test_storage_cleared());
    dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, NULL, sizeof key);
    dsd_app_drain_cmds(&opts, &state);
    rc |= expect_true("null payload never reuses old key", dsd_app_command_test_storage_cleared());
    rc |= expect_int("force setter queued", dsd_app_command_set_i32(DSD_APP_CMD_FORCE_KEY_SET, 2),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    dsd_app_drain_cmds(&opts, &state);
    rc |= expect_true("force setter applied", state.M == 0x21);
    DSD_SECURE_ZERO(&key, sizeof key);
    freeState(&state);
    return rc;
}

/* WP-D2: configured force, effective row override, epoch and rejection contracts. */
static int
test_direct_key_and_force_scope(void) {
    static dsd_state state;
    static dsd_opts opts;
    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    int rc = 0;
    rc |= expect_int("enter force scope", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR), 0);
    dsd_scan_option_values row = {.present = DSD_SCAN_OPT_FORCE | DSD_SCAN_OPT_MUTE_DMR, .force = 0x21, .mute_dmr = 1};
    (void)dsd_scan_mode_options(&opts, &state, &row);
    uint64_t epoch = state.enc_lockout_key_epoch;
    dsd_app_command_set_i32(DSD_APP_CMD_FORCE_KEY_SET, 1);
    dsd_app_drain_cmds(&opts, &state);
    dsd_scan_settings configured;
    dsd_scan_mode_configured(&opts, &state, &configured);
    rc |= expect_true("force configured and effective scopes", configured.force_key == 1 && state.M == 0x21);
    rc |= expect_true("force change bumps epoch", state.enc_lockout_key_epoch != epoch);
    epoch = state.enc_lockout_key_epoch;
    dsd_app_command_set_i32(DSD_APP_CMD_FORCE_KEY_SET, 1);
    dsd_app_command_set_i32(DSD_APP_CMD_FORCE_KEY_SET, 3);
    dsd_app_drain_cmds(&opts, &state);
    rc |= expect_true("idempotent or invalid force keeps epoch", state.enc_lockout_key_epoch == epoch);
    dsd_app_key_direct_payload key = {DSD_APP_KEY_TYPE_BASIC, "0"};
    dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &key, sizeof key);
    dsd_app_drain_cmds(&opts, &state);
    dsd_scan_mode_configured(&opts, &state, &configured);
    rc |= expect_true("direct zero arms configured decryption",
                      configured.dmr_mute_encL == 0 && configured.dmr_mute_encR == 0);
    rc |= expect_true("direct respects row mute", opts.dmr_mute_encL == 1 && opts.dmr_mute_encR == 1);
    rc |= expect_true("direct bumps epoch", state.enc_lockout_key_epoch != epoch);
    epoch = state.enc_lockout_key_epoch;
    for (int type = 0; type < 4; ++type) {
        key.key_type = type;
        DSD_MEMSET(key.value, 'Z', sizeof key.value);
        key.value[sizeof key.value - 1] = 0;
        dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &key, sizeof key);
        dsd_app_drain_cmds(&opts, &state);
        rc |= expect_true("invalid direct is atomic", state.K == 0 && state.enc_lockout_key_epoch == epoch);
        rc |= expect_true("invalid text never in toast", strstr(state.ui_msg, key.value) == NULL);
        rc |= expect_contains("invalid toast names shape", state.ui_msg, "Expected");
    }
    DSD_MEMSET(key.value, 'Z', sizeof key.value);
    dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &key, sizeof key);
    dsd_app_drain_cmds(&opts, &state);
    rc |= expect_true("unterminated direct rejected", state.enc_lockout_key_epoch == epoch);
    DSD_SECURE_ZERO(&key, sizeof key);
    (void)dsd_scan_mode_options(&opts, &state, NULL);
    rc |= expect_true("unkeyed row inherits force and mute", state.M == 1 && opts.dmr_mute_encL == 0);
    dsd_scan_mode_leave(&opts, &state);
    freeState(&state);
    return rc;
}

/* A CSV row has activated both signalled KIDs before the global edit arrives. */
static int
test_direct_key_preserves_active_keyring(int reject) {
    static dsd_state state;
    static dsd_opts opts;
    int rc = 0;
    for (int type = DSD_APP_KEY_TYPE_BASIC; type <= DSD_APP_KEY_TYPE_SCRAMBLER; ++type) {
        init_test_context(&opts, &state);
        opts.audio_in_type = AUDIO_IN_WAV;
        opts.wav_sample_rate = 48000;
        state.K = 23;
        state.R = 83;
        state.RR = 89;
        state.rkey_array[3] = 19;
        state.rkey_array_loaded[3] = 1;
        rc |= expect_int("enter active key test scope", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR), 0);
        dsd_scan_option_values row_options = {.present = DSD_SCAN_OPT_MUTE_DMR, .mute_dmr = 1};
        (void)dsd_scan_mode_options(&opts, &state, &row_options);
        dsd_key_set row = {.count = 8, .present = 1, .keyloader = 1};
        row.entries = calloc(row.count, sizeof(*row.entries));
        if (!row.entries) {
            freeState(&state);
            return 1;
        }
        const int offsets[] = {0, 0x101, 0x201, 0x301};
        for (int slot = 0; slot < 2; ++slot) {
            for (int segment = 0; segment < 4; ++segment) {
                dsd_key_set_entry* entry = &row.entries[slot * 4 + segment];
                entry->index = (uint32_t)((slot ? 11 : 7) + offsets[segment]);
                entry->value = 17U + (uint64_t)slot * 4U + (uint64_t)segment;
                entry->loaded = 1;
            }
        }
        rc |= expect_true("enter populated CSV key row", dsd_scan_keys_enter(&state, &row));
        state.payload_keyid = 7;
        state.payload_keyidR = 11;
        keyring_activate_slot(&opts, &state, 0);
        keyring_activate_slot(&opts, &state, 1);
        // The vocoder can have filled this derived AES buffer since row entry, too.
        DSD_MEMSET(state.aes_key, 0x5a, sizeof state.aes_key);
        rc |= expect_true("positive key activation marker", state.R == 17 && state.RR == 21 && state.A4[0] == 20
                                                                && state.A4[1] == 24 && state.aes_key_segments[0] == 4
                                                                && state.aes_key_segments[1] == 4);
        dsd_key_set effective = {0}, baseline = {0}, after = {0};
        rc |= expect_int("capture activated keys", dsd_key_set_capture(&effective, &state), 0);
        rc |= expect_int("capture configured keys", dsd_key_set_copy(&baseline, &state.scan_keys_baseline), 0);
        const uint64_t epoch = state.enc_lockout_key_epoch;
        dsd_scan_settings configured;
        dsd_scan_mode_configured(&opts, &state, &configured);
        dsd_app_key_direct_payload key = {.key_type = type};
        DSD_SNPRINTF(key.value, sizeof key.value, "%s",
                     reject                         ? "invalid"
                     : type == DSD_APP_KEY_TYPE_HEX ? "0000000000"
                                                    : "0");
        rc |= expect_int("post global key during active call",
                         dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &key, sizeof key),
                         DSD_APP_COMMAND_SUBMIT_QUEUED);
        DSD_SECURE_ZERO(&key, sizeof key);
        rc |= expect_int("drain global key during active call", dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_true(reject ? "rejection preserves signalled KIDs" : "global edit preserves signalled KIDs",
                          state.payload_keyid == 7 && state.payload_keyidR == 11);
        rc |= expect_int("capture effective keys after command", dsd_key_set_capture(&after, &state), 0);
        rc |= expect_true(reject ? "rejection preserves activated scalar and AES state"
                                 : "global edit preserves activated scalar and AES state",
                          dsd_key_set_equal(&effective, &after));
        rc |= expect_true("row identity preserved",
                          state.scan_keys_active_set && dsd_key_set_equal(&row, &state.scan_keys_active));
        if (reject) {
            rc |= expect_true("rejection preserves baseline", dsd_key_set_equal(&baseline, &state.scan_keys_baseline));
            rc |= expect_true("rejection preserves epoch", state.enc_lockout_key_epoch == epoch);
            dsd_scan_settings after_settings;
            dsd_scan_mode_configured(&opts, &state, &after_settings);
            rc |= expect_true("rejection preserves configured mutes",
                              after_settings.dmr_mute_encL == configured.dmr_mute_encL
                                  && after_settings.dmr_mute_encR == configured.dmr_mute_encR
                                  && after_settings.unmute_encrypted_p25 == configured.unmute_encrypted_p25);
        } else {
            rc |= expect_true("accepted baseline edit bumps epoch", state.enc_lockout_key_epoch != epoch);
            rc |= expect_true("global basic baseline overlay",
                              state.scan_keys_baseline.scalars.K == (type == DSD_APP_KEY_TYPE_BASIC ? 0 : 23));
            rc |= expect_true("global scalar baseline overlay",
                              state.scan_keys_baseline.scalars.R == (type >= DSD_APP_KEY_TYPE_RC4 ? 0 : 83));
            rc |= expect_true("global right baseline overlay",
                              state.scan_keys_baseline.scalars.RR == (type == DSD_APP_KEY_TYPE_RC4 ? 0 : 89));
        }
        keyring_activate_slot(&opts, &state, 0);
        keyring_activate_slot(&opts, &state, 1);
        rc |= expect_true("next vocoder activation uses signalled row keys",
                          state.R == 17 && state.RR == 21 && state.A4[0] == 20 && state.A4[1] == 24
                              && state.aes_key_loaded[0] && state.aes_key_loaded[1]);
        rc |= expect_true("active CSV keyloader retained", state.keyloader == 1);
        rc |= expect_true("active row mute retained", opts.dmr_mute_encL == 1 && opts.dmr_mute_encR == 1);
        dsd_key_set_free(&effective);
        dsd_key_set_free(&baseline);
        dsd_key_set_free(&after);
        dsd_key_set_free(&row);
        dsd_scan_keys_leave(&state);
        dsd_scan_mode_leave(&opts, &state);
        freeState(&state);
    }
    return rc;
}

struct restart_producer {
    dsd_mutex_t mutex;
    dsd_cond_t condition;
    int phase;
    int result[3];
};

static DSD_THREAD_RETURN_TYPE
restart_producer_run(void* opaque) {
    struct restart_producer* producer = opaque;
    dsd_app_key_direct_payload key = {DSD_APP_KEY_TYPE_BASIC, "73"};
    for (int phase = 0; phase < 3; ++phase) {
        dsd_mutex_lock(&producer->mutex);
        while (producer->phase != phase * 2 + 1) {
            dsd_cond_wait(&producer->condition, &producer->mutex);
        }
        producer->result[phase] = dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &key, sizeof key);
        producer->phase++;
        dsd_cond_signal(&producer->condition);
        dsd_mutex_unlock(&producer->mutex);
    }
    DSD_SECURE_ZERO(&key, sizeof key);
    DSD_THREAD_RETURN;
}

static void
restart_producer_step(struct restart_producer* producer, int phase) {
    dsd_mutex_lock(&producer->mutex);
    producer->phase = phase * 2 + 1;
    dsd_cond_signal(&producer->condition);
    if (phase == 0) {
        dsd_mutex_unlock(&producer->mutex);
        dsd_app_frontend_runtime_stop();
        dsd_mutex_lock(&producer->mutex);
    }
    while (producer->phase != phase * 2 + 2) {
        dsd_cond_wait(&producer->condition, &producer->mutex);
    }
    dsd_mutex_unlock(&producer->mutex);
}

static int
test_producer_stop_restart(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    struct restart_producer producer = {0};
    dsd_mutex_init(&producer.mutex);
    dsd_cond_init(&producer.condition);
    dsd_thread_t thread;
    dsd_app_frontend_runtime_start(&opts, &state);
    int rc = expect_int("producer thread started", dsd_thread_create(&thread, restart_producer_run, &producer), 0);
    if (!rc) {
        restart_producer_step(&producer, 0);
        dsd_app_frontend_runtime_stop();
        restart_producer_step(&producer, 1);
        dsd_app_frontend_runtime_start(&opts, &state);
        rc |= expect_int("restart has no prior producer commands", dsd_app_drain_cmds(&opts, &state), 0);
        restart_producer_step(&producer, 2);
        dsd_thread_join(thread);
        rc |= expect_true("racing producer is admitted or rejected",
                          producer.result[0] == DSD_APP_COMMAND_SUBMIT_QUEUED
                              || producer.result[0] == DSD_APP_COMMAND_SUBMIT_REJECTED);
        rc |= expect_int("closed session rejects producer", producer.result[1], DSD_APP_COMMAND_SUBMIT_REJECTED);
        rc |= expect_true("persistent producer submits to current session", producer.result[2] > 0);
        rc |= expect_int("only current producer request drains", dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_true("current request applied", state.K == 73);
    }
    dsd_app_frontend_runtime_stop();
    dsd_cond_destroy(&producer.condition);
    dsd_mutex_destroy(&producer.mutex);
    freeState(&state);
    return rc;
}

static int
test_session_queue_cancellation(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    int rc = 0;
    for (int failed = 0; failed < 2; ++failed) {
        dsd_app_frontend_runtime_start(&opts, &state);
        dsd_app_key_direct_payload key = {DSD_APP_KEY_TYPE_BASIC, "73"};
        rc |= expect_true("session key accepted",
                          dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &key, sizeof key) > 0);
        if (failed) {
            opts.scanner_mode = 1;
            opts.trunk_scan_enabled = 1;
            rc |= expect_true("actual failed engine setup", dsd_engine_run_with_lifecycle(&opts, &state, NULL) != 0);
            opts.scanner_mode = 0;
            opts.trunk_scan_enabled = 0;
        }
        dsd_app_frontend_runtime_stop();
        rc |= expect_true("stop or failure erases pending storage", dsd_app_command_test_storage_cleared());
        rc |= expect_int("closed session rejects late key",
                         dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &key, sizeof key),
                         DSD_APP_COMMAND_SUBMIT_REJECTED);
        DSD_SECURE_ZERO(&key, sizeof key);
        state.K = 19;
        dsd_app_frontend_runtime_start(&opts, &state);
        rc |= expect_int("restart has no stale commands", dsd_app_drain_cmds(&opts, &state), 0);
        rc |= expect_true("restart keeps new system key", state.K == 19);
        dsd_app_frontend_runtime_stop();
    }
    freeState(&state);
    DSD_SECURE_ZERO(&state, sizeof state);
    return rc;
}

static int
test_export_disposal(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    int rc = 0;

    struct {
        uint64_t context;
        unsigned int generation;
    } request = {0};

    /* Use byte offsets because the flexible wire member can precede tail padding. */
    unsigned char wire[offsetof(dsd_app_tg_export_payload, path) + 1024] = {0};
    dsd_tg_policy_table_version(&state, &request.context, &request.generation);
    DSD_MEMCPY(wire + offsetof(dsd_app_tg_export_payload, policy_context), &request.context, sizeof request.context);
    DSD_MEMCPY(wire + offsetof(dsd_app_tg_export_payload, policy_generation), &request.generation,
               sizeof request.generation);
    char path[1024];
    const int fd = dsd_test_mkstemp(path, sizeof path, "cancelled_export");
    if (fd < 0) {
        dsd_app_frontend_runtime_stop();
        freeState(&state);
        return expect_true("export temporary path available", 0);
    }
    dsd_close(fd);
    DSD_SNPRINTF((char*)wire + offsetof(dsd_app_tg_export_payload, path),
                 sizeof wire - offsetof(dsd_app_tg_export_payload, path), "%s", path);
    remove(path);
    for (int cancel = 0; cancel < 2; ++cancel) {
        dsd_app_frontend_runtime_start(&opts, &state);
        dsd_app_tg_export_result before = {0}, after = {0};
        dsd_app_tg_export_result_get(&before);
        rc |= expect_true("export accepted before disposal",
                          dsd_app_command_submit(DSD_APP_CMD_TG_LIST_EXPORT, wire, sizeof wire) > 0);
        if (cancel) {
            dsd_app_frontend_runtime_stop();
        } else {
            for (int i = 0; i < 127; ++i) {
                post_empty(DSD_APP_CMD_TOGGLE_COMPACT);
            }
        }
        dsd_app_tg_export_result_get(&after);
        rc |= expect_true("discarded export completes as failure", after.sequence > before.sequence && !after.success);
        rc |= expect_true("discarded export remains identifiable", after.policy_context == request.context
                                                                       && after.policy_generation == request.generation
                                                                       && strcmp(after.path, path) == 0);
        rc |= expect_str("discarded export preserves path", opts.group_in_file, "");
        FILE* file = fopen(path, "rb");
        rc |= expect_true("discarded export writes no file", !file);
        if (file) {
            fclose(file);
        }
        dsd_app_drain_cmds(&opts, &state);
        dsd_app_frontend_runtime_stop();
    }
    freeState(&state);
    return rc;
}

/* Group files are text: production writes them in text mode, so lines end in CRLF on Windows. Reading in text mode
 * compares the lines every platform writes, and leaves the LF fixtures the tests seed unchanged. */
static int
expect_file_bytes(const char* path, const char* expected) {
    char contents[1024] = {0};
    FILE* file = dsd_fopen_existing_regular_file(path, "r");
    if (!file) {
        return 1;
    }
    const size_t bytes = fread(contents, 1, sizeof contents - 1U, file);
    const int failed = ferror(file) || bytes != strlen(expected) || strcmp(contents, expected) != 0;
    fclose(file);
    return expect_true("group file bytes unchanged", !failed);
}

static int
test_temporary_lockout_commands(uint8_t slot) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    char path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(path, sizeof path, "dsd_temporary_tg");
    if (fd < 0) {
        freeState(&state);
        return 1;
    }
    dsd_close(fd);
    char export_path[DSD_TEST_PATH_MAX];
    const int export_fd = dsd_test_mkstemp(export_path, sizeof export_path, "dsd_temporary_tg_export");
    if (export_fd < 0) {
        freeState(&state);
        remove(path);
        return 1;
    }
    dsd_close(export_fd);
    const char* csv = "id,mode,name,notes\n123,A,Dispatch,keep this unmodeled note\n456,A,EMS,note\n";
    int rc = write_file_bytes(path, csv, strlen(csv));
    DSD_SNPRINTF(opts.group_in_file, sizeof opts.group_in_file, "%s", path);
    rc |= expect_int("import temporary fixture", dsd_tg_policy_reload_group_file(&opts, &state), 0);
    dsd_call_observation observation = {.protocol = DSD_SYNC_P25P1_POS,
                                        .slot = slot,
                                        .kind = DSD_CALL_KIND_GROUP_VOICE,
                                        .ota_target_id = 123,
                                        .policy_target_id = 123,
                                        .ota_source_id = 1,
                                        .observed_m = 1.0};
    rc |= expect_int("seed lockout call", dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN), 1);
    dsd_event_sync_slot(&opts, &state, slot);
    rc |= expect_true("queue temporary preference", dsd_app_command_set_i32(DSD_APP_CMD_TG_LOCKOUT_PERSIST_SET, 0) > 0);
    rc |= expect_true("queue temporary slot lockout", dsd_app_command_set_u8(DSD_APP_CMD_LOCKOUT_SLOT, slot) > 0);
    rc |= expect_true("queue persistent preference after lockout",
                      dsd_app_command_set_i32(DSD_APP_CMD_TG_LOCKOUT_PERSIST_SET, 1) > 0);
    rc |= expect_int("FIFO preference and lockout drain", dsd_app_drain_cmds(&opts, &state), 3);
    rc |= expect_true("preference only affects subsequent commands",
                      opts.persist_tg_lockouts && dsd_tg_policy_session_avoid_contains(&state, 123));
    rc |= expect_file_bytes(path, csv);
    rc |= expect_true("invalid persistence queued", dsd_app_command_set_i32(DSD_APP_CMD_TG_LOCKOUT_PERSIST_SET, 2) > 0);
    rc |= expect_int("invalid persistence drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("invalid persistence unchanged", opts.persist_tg_lockouts == 1);
    dsd_tg_policy_lookup lookup;
    rc |= expect_int("lookup original row", dsd_tg_policy_lookup_id(&state, 123, &lookup), 0);
    rc |= expect_str("temporary avoid preserves saved mode", lookup.entry.mode, "A");
    rc |= expect_str("temporary avoid preserves label", lookup.entry.name, "Dispatch");

    // An ordinary edit persists, but must not serialize the temporary overlay.
    dsd_app_tg_listen_payload block = {456, 456, 0};
    rc |= expect_true("queue permanent list edit", dsd_app_command_set_tg_listen(&block) > 0);
    rc |= expect_int("permanent list edit drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_file_bytes(path, "id,mode,name\n123,A,Dispatch\n456,B,EMS\n");
    rc |= expect_true("edit keeps temporary avoid", dsd_tg_policy_session_avoid_contains(&state, 123));

    // Export uses the same canonical writer even when its source snapshot has avoids.
    union {
        unsigned char bytes[1200];
        dsd_app_tg_export_payload alignment; /* MSVC's C <stddef.h> has no max_align_t. */
    } storage = {{0}};

    dsd_app_tg_export_payload* export = (dsd_app_tg_export_payload*)storage.bytes;
    dsd_tg_policy_table_version(&state, &export->policy_context, &export->policy_generation);
    DSD_SNPRINTF(export->path, sizeof storage.bytes - offsetof(dsd_app_tg_export_payload, path), "%s", export_path);
    rc |= expect_true("export temporary policy queued",
                      dsd_app_command_submit(DSD_APP_CMD_TG_LIST_EXPORT, export,
                                             offsetof(dsd_app_tg_export_payload, path) + strlen(export_path) + 1U)
                          > 0);
    rc |= expect_int("export temporary policy drained", dsd_app_drain_cmds(&opts, &state), 1);
    dsd_app_tg_export_result result;
    rc |= expect_true("export reports successful write", dsd_app_tg_export_result_get(&result) && result.success);
    rc |= expect_str("export reports new destination", result.path, export_path);
    rc |= expect_file_bytes(export_path, "id,mode,name\n123,A,Dispatch\n456,B,EMS\n");
    rc |= expect_file_bytes(path, "id,mode,name\n123,A,Dispatch\n456,B,EMS\n");

    uint64_t context = 0;
    dsd_tg_policy_table_version(&state, &context, NULL);
    const uint64_t wrong_context = context + 1U;
    rc |= expect_true("stale clear queued",
                      dsd_app_command_submit(DSD_APP_CMD_TG_SESSION_AVOID_CLEAR, &wrong_context, sizeof wrong_context)
                          > 0);
    rc |= expect_int("stale clear drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("stale clear preserves avoids", dsd_tg_policy_session_avoid_contains(&state, 123));
    (void)dsd_enc_lockout_note(&state, 789, 1, 0x84, 1);
    rc |= expect_true("current clear queued",
                      dsd_app_command_submit(DSD_APP_CMD_TG_SESSION_AVOID_CLEAR, &context, sizeof context) > 0);
    rc |= expect_int("current clear drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("current clear removes temporary avoid", !dsd_tg_policy_session_avoid_contains(&state, 123));
    rc |= expect_true("clear retains encryption lockout", dsd_enc_lockout_lookup(&state, 789, 1, NULL));
    rc |= expect_file_bytes(path, "id,mode,name\n123,A,Dispatch\n456,B,EMS\n");
    rc |= expect_int("reloaded file", dsd_tg_policy_reload_group_file(&opts, &state), 0);
    dsd_tg_policy_decision decision;
    rc |= expect_true("temporary TG receives again",
                      dsd_tg_policy_evaluate_group_call(&opts, &state, 123, 1, 0, 0, &decision) == 0
                          && decision.tune_allowed);
    rc |= expect_true("saved block remains",
                      dsd_tg_policy_evaluate_group_call(&opts, &state, 456, 1, 0, 0, &decision) == 0
                          && !decision.tune_allowed);
    freeState(&state);
    remove(path);
    remove(export_path);
    return rc;
}

static int
test_skip_commands(uint8_t slot) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    char path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(path, sizeof path, "dsd_call_skip");
    if (fd < 0) {
        freeState(&state);
        return 1;
    }
    dsd_close(fd);
    char export_path[DSD_TEST_PATH_MAX];
    const int export_fd = dsd_test_mkstemp(export_path, sizeof export_path, "dsd_call_skip_export");
    if (export_fd < 0) {
        freeState(&state);
        remove(path);
        return 1;
    }
    dsd_close(export_fd);
    const char* csv = "id,mode,name,notes\n123,A,Dispatch,keep this unmodeled note\n456,A,EMS,note\n";
    int rc = write_file_bytes(path, csv, strlen(csv));
    DSD_SNPRINTF(opts.group_in_file, sizeof opts.group_in_file, "%s", path);
    rc |= expect_int("import skip fixture", dsd_tg_policy_reload_group_file(&opts, &state), 0);
    uint64_t context = 0;
    unsigned int generation = 0;
    dsd_tg_policy_table_version(&state, &context, &generation);
    rc |= expect_true("idle skip queued", dsd_app_command_set_u8(DSD_APP_CMD_SKIP_SLOT, slot) > 0);
    rc |= expect_int("idle skip drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("idle skip no-op", dsd_tg_policy_call_skip_count(&state, dsd_decode_now_mono_s()) == 0);
    rc |= expect_true("idle skip stages no event",
                      strstr(state.event_history_s[slot].Event_History_Items[0].internal_str, "call skipped.") == NULL);
    rc |= expect_true("idle skip commits no event",
                      strstr(state.event_history_s[slot].Event_History_Items[1].internal_str, "call skipped.") == NULL);
    const int protocols[] = {DSD_SYNC_P25P1_POS, DSD_SYNC_P25P2_POS, DSD_SYNC_DMR_BS_VOICE_POS, DSD_SYNC_NXDN_POS,
                             DSD_SYNC_P25P2_POS};
    for (int persist = 0; persist <= 1; ++persist) {
        opts.persist_tg_lockouts = (uint8_t)persist;
        for (size_t i = 0; i < sizeof protocols / sizeof protocols[0]; ++i) {
            dsd_event_history_reset(&state);
            const double now = dsd_decode_now_mono_s();
            dsd_call_observation observation = {.protocol = protocols[i],
                                                .slot = slot,
                                                .kind =
                                                    i == 4 ? DSD_CALL_KIND_PRIVATE_VOICE : DSD_CALL_KIND_GROUP_VOICE,
                                                .ota_target_id = 123,
                                                .policy_target_id = 456,
                                                .ota_source_id = 1,
                                                .observed_m = now};
            rc |=
                expect_int("seed skip call", dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN), 1);
            dsd_event_sync_slot(&opts, &state, slot);
            opts.frame_provoice = 1;
            rc |= expect_true("ProVoice skip queued", dsd_app_command_set_u8(DSD_APP_CMD_SKIP_SLOT, slot) > 0);
            rc |= expect_int("ProVoice skip drained", dsd_app_drain_cmds(&opts, &state), 1);
            rc |= expect_true("ProVoice skip no-op", dsd_tg_policy_call_skip_count(&state, now) == 0);
            opts.frame_provoice = 0;
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
            g_skip_arm_refused = 1;
            rc |= expect_true("refused skip arm queued", dsd_app_command_set_u8(DSD_APP_CMD_SKIP_SLOT, slot) > 0);
            rc |= expect_int("refused skip arm drained", dsd_app_drain_cmds(&opts, &state), 1);
            g_skip_arm_refused = 0;
            rc |= expect_str("refused skip arm toast", state.ui_msg, "Could not skip TG 123");
            rc |= expect_call_phase("refused skip arm keeps call active", &state, slot, DSD_CALL_PHASE_ACTIVE);
            rc |= expect_true("refused skip arm keeps ledger empty", dsd_tg_policy_call_skip_count(&state, now) == 0);
            rc |= expect_true("refused skip arm stages no event",
                              strstr(state.event_history_s[slot].Event_History_Items[0].internal_str, "call skipped.")
                                  == NULL);
            rc |= expect_true("refused skip arm commits no event",
                              strstr(state.event_history_s[slot].Event_History_Items[1].internal_str, "call skipped.")
                                  == NULL);
#endif
            // Deliberately disagree with the call snapshot: fallback follows the call itself.
            state.synctype = state.lastsynctype = DSD_SYNC_P25P1_POS;
            const uint8_t requested_slot = persist ? (uint8_t)(slot | 2U) : slot;
            rc |= expect_true("skip queued", dsd_app_command_set_u8(DSD_APP_CMD_SKIP_SLOT, requested_slot) > 0);
            rc |= expect_int("skip drained", dsd_app_drain_cmds(&opts, &state), 1);
            rc |= expect_str("skip toast", state.ui_msg, "TG 123 skipped");
            rc |= expect_call_phase("skip ends call", &state, slot, DSD_CALL_PHASE_ENDED);
            rc |=
                expect_contains("skip committed event", state.event_history_s[slot].Event_History_Items[1].internal_str,
                                "Target: 123; call skipped.");
            rc |= expect_str("skip leaves no staged text",
                             state.event_history_s[slot].Event_History_Items[0].internal_str, "");
            rc |= expect_true("one OTA skip per press", dsd_tg_policy_call_skip_active(&state, 123, now)
                                                            && !dsd_tg_policy_call_skip_active(&state, 456, now)
                                                            && dsd_tg_policy_call_skip_count(&state, now) == 1);
            const int fallback = i >= 2;
            rc |= expect_int("snapshot determines refresh rule", dsd_tg_policy_call_skip_touch(&state, 123, now + 10.0),
                             !fallback);
            rc |=
                expect_true("skip alive before quiet expiry", dsd_tg_policy_call_skip_active(&state, 123, now + 10.0));
            rc |= expect_int("snapshot determines lifetime", dsd_tg_policy_call_skip_active(&state, 123, now + 20.0),
                             !fallback);
            rc |= expect_file_bytes(path, csv);
            unsigned int after = 0;
            dsd_tg_policy_table_version(&state, NULL, &after);
            rc |= expect_true("skip preserves edit generation", after == generation);
            const uint64_t wrong = context + 1;
            rc |= expect_true("stale skip clear queued",
                              dsd_app_command_submit(DSD_APP_CMD_TG_SESSION_AVOID_CLEAR, &wrong, sizeof wrong) > 0);
            rc |= expect_int("stale skip clear drained", dsd_app_drain_cmds(&opts, &state), 1);
            rc |= expect_true("stale clear preserves skip", dsd_tg_policy_call_skip_active(&state, 123, now));
            rc |= expect_true("skip clear queued",
                              dsd_app_command_submit(DSD_APP_CMD_TG_SESSION_AVOID_CLEAR, &context, sizeof context) > 0);
            rc |= expect_int("skip clear drained", dsd_app_drain_cmds(&opts, &state), 1);
            rc |= expect_true("clear drops skip", dsd_tg_policy_call_skip_count(&state, now) == 0);
        }
    }
    rc |= expect_int("export seed skip", dsd_tg_policy_call_skip_arm(&state, 123, 1, 0, dsd_decode_now_mono_s()), 0);

    union {
        unsigned char bytes[1200];
        dsd_app_tg_export_payload alignment; /* MSVC's C <stddef.h> has no max_align_t. */
    } storage = {{0}};

    dsd_app_tg_export_payload* export = (dsd_app_tg_export_payload*)storage.bytes;
    dsd_tg_policy_table_version(&state, &export->policy_context, &export->policy_generation);
    DSD_SNPRINTF(export->path, sizeof storage.bytes - offsetof(dsd_app_tg_export_payload, path), "%s", export_path);
    rc |= expect_true("export skip policy queued",
                      dsd_app_command_submit(DSD_APP_CMD_TG_LIST_EXPORT, export,
                                             offsetof(dsd_app_tg_export_payload, path) + strlen(export_path) + 1U)
                          > 0);
    rc |= expect_int("export skip policy drained", dsd_app_drain_cmds(&opts, &state), 1);
    dsd_app_tg_export_result result;
    rc |= expect_true("skip export successful", dsd_app_tg_export_result_get(&result) && result.success);
    rc |= expect_file_bytes(export_path, "id,mode,name\n123,A,Dispatch\n456,A,EMS\n");
    rc |= expect_file_bytes(path, csv);
    freeState(&state);
    remove(path);
    remove(export_path);
    return rc;
}

static int
test_config_keeps_trunk_scan_lifecycle(void) {
    /* The coordinator is installed at startup only, so a live config may not flip the flag
     * the scanner-exclusivity refusals and P25 recovery admission read (#554). */
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    for (int installed = 0; installed <= 1; ++installed) {
        init_test_context(&opts, &state);
        opts.trunk_scan_enabled = installed;
        opts.trunk_scan_idle_dwell_ms = 100;
        dsdneoUserConfig cfg = {0};
        cfg.has_trunk_scan = 1;
        cfg.trunk_scan_enabled = !installed;
        cfg.trunk_scan_idle_dwell_ms = 4321;
        rc |= expect_true("trunk-scan lifecycle config queued", dsd_app_command_apply_config(&cfg) > 0);
        rc |= expect_int("trunk-scan lifecycle config drained", dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int("live config keeps the coordinator flag", opts.trunk_scan_enabled, installed);
        rc |= expect_int("live config still applies trunk-scan tuning", opts.trunk_scan_idle_dwell_ms, 4321);
        freeState(&state);
    }
    return rc;
}

static int
test_config_refuses_scanner_under_trunk_scan(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    opts.trunk_scan_enabled = 1;
    opts.trunk_enable = 1;
    opts.persist_tg_lockouts = 1;
    dsdneoUserConfig cfg = {0};
    cfg.has_trunking = 1;
    cfg.trunk_scanner = 1;
    /* A conflicting config must be refused even before trying to load this path. */
    DSD_SNPRINTF(cfg.trunk_group_csv, sizeof(cfg.trunk_group_csv), "missing-policy-554.csv");
    int rc = expect_true("conflicting scanner config queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("conflicting scanner config drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("conflicting scanner config toast", state.ui_msg,
                     "Trunk scan active: conventional scanner unavailable");
    rc |= expect_true("conflicting scanner config preserves ownership",
                      opts.trunk_scan_enabled == 1 && opts.trunk_enable == 1 && opts.scanner_mode == 0);
    rc |= expect_true("conflicting scanner config preserves policy defaults",
                      opts.persist_tg_lockouts == 1 && opts.group_in_file[0] == '\0');
    freeState(&state);
    return rc;
}

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
static int g_config_rtl_creates;
/* The threshold a stream would open with: rtl_demod_config copies it into the demod. */
static double g_config_rtl_create_squelch = -1.0;
static int g_config_rtl_create_frame_p25p1 = -1;
/* Issue #578: 1 has a create succeed, with a context of zeroed storage (the stream calls left unwrapped do nothing with
   it), so the start after it runs; 0, the default, fails every create. The device string, family and analog profile
   the last create was handed: what its start would open. */
static int g_config_rtl_open_ok;
static void* g_config_rtl_ctx[2];
static char g_config_rtl_create_dev[64];
static int g_config_rtl_create_analog_only = -1;
static int g_config_rtl_create_kind = -1;
static int g_config_rtl_create_width_hz = -1;
/* The DSP bandwidth the last create was handed. */
static int g_config_rtl_create_bw_khz = -1;
static int g_config_rtl_create_gain = -1;
static long g_config_rtl_create_freq = -1;
/* Starts made, and how many of the next ones fail. A start that fails on options that run the analog family records
   a refusal of their width at g_config_rtl_refuse_rate_hz when that is above 0, as the stream's analog channel check
   does at the rate the device delivered (rtl_stream_start_analog_refusal()); otherwise it fails for a device reason and
   records none. Every start forgets the last one's record. */
static int g_config_rtl_starts;
static int g_config_rtl_fail_starts;
static int g_config_rtl_refuse_rate_hz;
static int g_config_rtl_refused;
static int g_config_rtl_refused_kind;
static int g_config_rtl_refused_width_hz;
static int g_config_rtl_refused_rate_hz;
/* Issue #578: above 0, the DSP rate every device delivers. A start that does not fail for its device, handed options
   that run the analog family with an explicit width that rate cannot filter, is refused there and records the refusal
   at that rate, as the stream's analog channel check refuses a width once the device has set its rate; one handed a
   profile that rate runs (a digital row's, a width it filters) starts. */
static int g_config_rtl_device_rate_hz;
/* Issue #578: whether the last create was handed an I/Q capture request (--iq-capture), and how many creates were. */
static int g_config_rtl_create_capture = -1;
static int g_config_rtl_captures;
/* Whether a start that fails does so after it opened the capture writer, which writes the file anew (worker creation,
   an Airspy SDK that does not start: 1), or before (a device that does not open, a width refused: 0); and what
   rtl_stream_start_opened_capture() reports for the last start, which every create forgets. */
static int g_config_rtl_fail_after_capture;
static int g_config_rtl_opened_capture;

/* Issue #578: 1 has the next Airspy start that opens its device see the device stop at once: its monitor thread
   (airspy_monitor()) latches a device failure before the start returns success. One-shot. */
static int g_config_rtl_airspy_stops_after_open;

/* Issue #578: 1 has the next start that fails for a device reason open its device first, as one does that fails after
   the capture opened (an Airspy then clears the latch), without opening the capture. One-shot. */
static int g_config_rtl_next_fail_opens;

/* Issue #578: the input failure a start latches for the session (dsd_input_failure_report()), as the io does: an Airspy
   that does not open latches a device failure, which ends the session with a failure exit
   (dsd_engine_run_with_lifecycle()), and one that opens clears the latch (airspy_source_open()). A width refused before
   the device opens latches nothing, and the other inputs here latch nothing and clear nothing. @p opened says whether
   the start opened its device. */
static void
config_rtl_start_latch(int opened) {
    if (strncmp(g_config_rtl_create_dev, "airspy", 6) != 0) {
        return;
    }
    if (!opened) {
        dsd_input_failure_report(DSD_INPUT_FAILURE_DEVICE, -5);
        return;
    }
    dsd_input_failure_clear();
    if (g_config_rtl_airspy_stops_after_open) {
        g_config_rtl_airspy_stops_after_open = 0;
        dsd_input_failure_report(DSD_INPUT_FAILURE_DEVICE, -1);
    }
}

static void
reset_config_rtl_wrap(void) {
    dsd_input_failure_clear();
    g_config_rtl_airspy_stops_after_open = 0;
    g_config_rtl_next_fail_opens = 0;
    g_config_rtl_creates = 0;
    g_config_rtl_create_capture = -1;
    g_config_rtl_captures = 0;
    g_config_rtl_fail_after_capture = g_config_rtl_opened_capture = 0;
    g_config_rtl_open_ok = 0;
    g_config_rtl_create_dev[0] = '\0';
    g_config_rtl_create_analog_only = g_config_rtl_create_kind = g_config_rtl_create_width_hz = -1;
    g_config_rtl_create_bw_khz = -1;
    g_config_rtl_starts = g_config_rtl_fail_starts = g_config_rtl_refuse_rate_hz = 0;
    g_config_rtl_refused = g_config_rtl_refused_kind = g_config_rtl_refused_width_hz = g_config_rtl_refused_rate_hz = 0;
    g_config_rtl_device_rate_hz = 0;
}

/* Whether the start of the last create is refused at the rate every device delivers (g_config_rtl_device_rate_hz). */
static int
config_rtl_device_rate_refuses_width(void) {
    return g_config_rtl_device_rate_hz > 0 && g_config_rtl_create_analog_only == 1 && g_config_rtl_create_width_hz > 0
           && !dsd_analog_width_realizable(g_config_rtl_create_width_hz, g_config_rtl_device_rate_hz);
}

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol name.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
int __wrap_rtl_stream_create(dsd_opts* opts, RtlSdrContext** out_ctx);
int __wrap_rtl_stream_start(RtlSdrContext* ctx);
int __wrap_rtl_stream_start_analog_refusal(int* out_kind, int* out_width_hz, int* out_rate_hz);
int __wrap_rtl_stream_start_opened_capture(void);
int __wrap_rtl_stream_stop(RtlSdrContext* ctx);
int __wrap_rtl_stream_destroy(RtlSdrContext* ctx);

int
__wrap_rtl_stream_create(dsd_opts* opts, RtlSdrContext** out_ctx) {
    g_config_rtl_create_squelch = opts ? opts->rtl_squelch_level : -1.0;
    g_config_rtl_create_frame_p25p1 = opts ? opts->frame_p25p1 : -1;
    DSD_SNPRINTF(g_config_rtl_create_dev, sizeof g_config_rtl_create_dev, "%s", opts ? opts->audio_in_dev : "");
    g_config_rtl_create_analog_only = opts ? dsd_opts_is_analog_family(opts) : -1;
    g_config_rtl_create_kind = opts ? opts->analog_demod : -1;
    g_config_rtl_create_width_hz = opts ? dsd_opts_analog_width_hz(opts) : -1;
    g_config_rtl_create_bw_khz = opts ? opts->rtl_dsp_bw_khz : -1;
    g_config_rtl_create_gain = opts ? opts->rtl_gain_value : -1;
    g_config_rtl_create_freq = opts ? (long)opts->rtlsdr_center_freq : -1;
    g_config_rtl_create_capture = opts ? opts->iq_capture_requested : -1;
    g_config_rtl_captures += g_config_rtl_create_capture == 1;
    g_config_rtl_opened_capture = 0;
    *out_ctx = g_config_rtl_open_ok ? (RtlSdrContext*)g_config_rtl_ctx : NULL;
    ++g_config_rtl_creates;
    return g_config_rtl_open_ok ? 0 : -1;
}

int
__wrap_rtl_stream_start(RtlSdrContext* ctx) {
    (void)ctx;
    ++g_config_rtl_starts;
    g_config_rtl_refused = 0;
    const int capturing = g_config_rtl_create_capture == 1;
    if (g_config_rtl_fail_starts <= 0 && config_rtl_device_rate_refuses_width()) {
        g_config_rtl_refused = 1;
        g_config_rtl_refused_kind = g_config_rtl_create_kind;
        g_config_rtl_refused_width_hz = g_config_rtl_create_width_hz;
        g_config_rtl_refused_rate_hz = g_config_rtl_device_rate_hz;
        return -1;
    }
    if (g_config_rtl_fail_starts <= 0) {
        g_config_rtl_opened_capture = capturing;
        config_rtl_start_latch(1);
        return 0;
    }
    --g_config_rtl_fail_starts;
    if (g_config_rtl_refuse_rate_hz > 0 && g_config_rtl_create_analog_only == 1) {
        g_config_rtl_refused = 1;
        g_config_rtl_refused_kind = g_config_rtl_create_kind;
        g_config_rtl_refused_width_hz = g_config_rtl_create_width_hz;
        g_config_rtl_refused_rate_hz = g_config_rtl_refuse_rate_hz;
    } else {
        g_config_rtl_opened_capture = capturing && g_config_rtl_fail_after_capture;
        /* A start that failed after the capture opened had opened its device. */
        config_rtl_start_latch(g_config_rtl_fail_after_capture || g_config_rtl_next_fail_opens);
        g_config_rtl_next_fail_opens = 0;
    }
    return -1;
}

int
__wrap_rtl_stream_start_opened_capture(void) {
    return g_config_rtl_opened_capture;
}

int
__wrap_rtl_stream_start_analog_refusal(int* out_kind, int* out_width_hz, int* out_rate_hz) {
    if (!g_config_rtl_refused) {
        return 0;
    }
    if (out_kind) {
        *out_kind = g_config_rtl_refused_kind;
    }
    if (out_width_hz) {
        *out_width_hz = g_config_rtl_refused_width_hz;
    }
    if (out_rate_hz) {
        *out_rate_hz = g_config_rtl_refused_rate_hz;
    }
    return 1;
}

/* A restart stops and destroys the running context, which the tests fake with a pointer to their own storage. */
int
__wrap_rtl_stream_stop(RtlSdrContext* ctx) {
    (void)ctx;
    return 0;
}

int
__wrap_rtl_stream_destroy(RtlSdrContext* ctx) {
    (void)ctx;
    return 0;
}

/* Issue #578: the last log line about the I/Q capture (record_test_log(), installed once, from main()). */
static char g_capture_log[512];

static void
record_capture_log(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    if (level == LOG_LEVEL_WARN && text && strstr(text, "I/Q capture")) {
        DSD_SNPRINTF(g_capture_log, sizeof g_capture_log, "%s", text);
    }
}

/* The stream the rollback started runs the input that ran, without the I/Q capture: the start that failed was handed
   the capture, the recovery start was not, the capture stays off for the session, and the log names the file, as kept
   when the start that failed never opened it, or as written over when it had (@p written_over). */
static int
expect_capture_stopped(const char* label, const dsd_opts* opts, int written_over) {
    int rc = expect_int(label, g_config_rtl_creates == 2 && g_config_rtl_captures == 1, 1);
    rc |= expect_int(label, g_config_rtl_create_capture, 0);
    rc |= expect_int(label, opts->iq_capture_requested, 0);
    rc |= expect_int(label, strstr(g_capture_log, "I/Q capture stopped") != NULL && strstr(g_capture_log, "cap.iq"), 1);
    rc |= expect_int(label, strstr(g_capture_log, "which is kept") != NULL, written_over ? 0 : 1);
    rc |= expect_int(label, strstr(g_capture_log, "had already reopened cap.iq") != NULL, written_over ? 1 : 0);
    return rc;
}

/* Issue #578: the input the P25 SM watchdog reads while it holds its tick guard (the device, the input type, the
   rtl_tcp flag, the tuning and the DSP bandwidth a restart opens it at), and whether a command wrote any of it without
   holding that guard. Armed on a command's
   options (g_guard_probe_opts), every entry of the guard compares the input with what it was when the guard was last
   left, or when the probe was armed: a difference is a write made outside the guard, which the watchdog could have
   read half done. Disarming compares the input the command left with the last leave the same way. */
typedef struct {
    dsd_audio_in_type type;
    char dev[sizeof(((dsd_opts*)0)->audio_in_dev)];
    int rtltcp_enabled;
    uint32_t freq;
    int bw_khz;
} guard_probe_input;

static const dsd_opts* g_guard_probe_opts;
static guard_probe_input g_guard_probe_left;
static int g_guard_probe_unguarded;
static int g_guard_probe_holds;
/* A watchdog retune that completed just before the command took the guard (0: none): the next entry after it is armed
   moves the options' frequency here, as dsd_engine_tune_rtl() writes it while the watchdog holds the guard, which it
   left just before. A command that read the frequency outside the guard read the one from before the retune. */
static dsd_opts* g_guard_probe_retune_opts;
static uint32_t g_guard_probe_retune_hz;

static void
guard_probe_take(const dsd_opts* opts, guard_probe_input* out) {
    DSD_MEMSET(out, 0, sizeof *out);
    out->type = opts->audio_in_type;
    DSD_MEMCPY(out->dev, opts->audio_in_dev, sizeof out->dev);
    out->rtltcp_enabled = opts->rtltcp_enabled;
    out->freq = opts->rtlsdr_center_freq;
    out->bw_khz = opts->rtl_dsp_bw_khz;
}

/* Count a write made since the guard was last left. */
static void
guard_probe_check(const dsd_opts* opts) {
    guard_probe_input now;
    guard_probe_take(opts, &now);
    const guard_probe_input* left = &g_guard_probe_left;
    if (now.type != left->type || strncmp(now.dev, left->dev, sizeof now.dev) != 0
        || now.rtltcp_enabled != left->rtltcp_enabled || now.freq != left->freq || now.bw_khz != left->bw_khz) {
        ++g_guard_probe_unguarded;
    }
}

static void
guard_probe_arm(const dsd_opts* opts) {
    guard_probe_take(opts, &g_guard_probe_left);
    g_guard_probe_unguarded = g_guard_probe_holds = 0;
    g_guard_probe_opts = opts;
}

static void
guard_probe_disarm(void) {
    if (g_guard_probe_opts) {
        guard_probe_check(g_guard_probe_opts);
    }
    g_guard_probe_opts = NULL;
    g_guard_probe_retune_opts = NULL;
    g_guard_probe_retune_hz = 0U;
}

/* Arm the probe on @p opts with a watchdog retune to @p freq_hz completing just before the command's first hold. */
static void
guard_probe_arm_with_retune(dsd_opts* opts, uint32_t freq_hz) {
    guard_probe_arm(opts);
    g_guard_probe_retune_opts = opts;
    g_guard_probe_retune_hz = freq_hz;
}

void __real_p25_sm_tick_guard_enter(void);
void __real_p25_sm_tick_guard_leave(void);
void __wrap_p25_sm_tick_guard_enter(void);
void __wrap_p25_sm_tick_guard_leave(void);

void
__wrap_p25_sm_tick_guard_enter(void) {
    __real_p25_sm_tick_guard_enter();
    if (g_guard_probe_opts) {
        ++g_guard_probe_holds;
        guard_probe_check(g_guard_probe_opts);
    }
    if (g_guard_probe_retune_opts && g_guard_probe_retune_hz != 0U) {
        /* The watchdog's write, under its own hold: no write of the command's. */
        g_guard_probe_retune_opts->rtlsdr_center_freq = g_guard_probe_retune_hz;
        g_guard_probe_retune_hz = 0U;
    }
}

void
__wrap_p25_sm_tick_guard_leave(void) {
    if (g_guard_probe_opts) {
        guard_probe_take(g_guard_probe_opts, &g_guard_probe_left);
    }
    __real_p25_sm_tick_guard_leave();
}

/* The rate a radio row told the RTL output rescale the symbol timing is in (issue #634); -1 when none was noted. */
static int g_noted_timing_rate = -1;
static int g_noted_timing_calls = 0;

void __real_dsd_symbol_note_timing_rate(int rate_hz);
void __wrap_dsd_symbol_note_timing_rate(int rate_hz);

void
__wrap_dsd_symbol_note_timing_rate(int rate_hz) {
    g_noted_timing_calls++;
    g_noted_timing_rate = rate_hz;
    __real_dsd_symbol_note_timing_rate(rate_hz);
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

static int
test_config_rtl_restart_under_policy_guard(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.audio_out_type = 9;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "rtl:0:851375000:22:0:24:0:2");
    dsdneoUserConfig cfg = {0};
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTL;
    cfg.rtl_device = 0;
    DSD_SNPRINTF(cfg.rtl_freq, sizeof(cfg.rtl_freq), "460.125M");
    cfg.rtl_gain = 22;
    cfg.rtl_bw_khz = 24;
    cfg.rtl_volume = 2;
    g_config_rtl_creates = 0;
    int rc = expect_true("RTL restart config queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("RTL restart config completes without nesting", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("RTL restart config reached stream creation", g_config_rtl_creates, 1);
    rc |= expect_true("failed RTL restart leaves no stream", state.rtl_ctx == NULL && opts.rtl_started == 0);
    freeState(&state);
    return rc;
}

/*
 * Issue #524: while AM runs on an RTL-SDR input, a DSP bandwidth that cannot filter the AM width (the 6 kHz default
 * needs 8 kHz or more) is refused before anything changes. Restarting at it would have the stream start refuse the AM
 * channel and leave the decoder with no input, and stop it. A config apply that gives the input such a bandwidth is
 * refused the same way. A bandwidth that fits restarts the stream as before.
 */
static int
test_rtl_bandwidth_held_to_am_width(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "rtl:0:118100000:22:0:48:0:1");
    opts.rtl_dsp_bw_khz = 48;
    (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_AM, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state);
    g_config_rtl_creates = 0;
    state.ui_msg[0] = '\0';
    int rc = expect_int("am rtl bw 6 queued", post_i32(DSD_APP_CMD_RTL_SET_BW, 6), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("am rtl bw 6 drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("am rtl bw 6 not stored", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_int("am rtl bw 6 restarted nothing", g_config_rtl_creates, 0);
    rc |= expect_true("am rtl bw 6 toast gives the reason",
                      strstr(state.ui_msg, "Refused: DSP BW 6 kHz cannot filter AM 6 kHz (max 4.2 kHz)") != NULL);
    rc |= expect_true("am rtl bw 6 leaves AM", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_AM);

    rc |= expect_int("am rtl bw 8 queued", post_i32(DSD_APP_CMD_RTL_SET_BW, 8), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("am rtl bw 8 drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("am rtl bw 8 stored", opts.rtl_dsp_bw_khz, 8);
    rc |= expect_int("am rtl bw 8 restarts the stream", g_config_rtl_creates, 1);

    /* A config that reopens the input at 4 kHz is refused whole. */
    opts.rtl_dsp_bw_khz = 48;
    g_config_rtl_creates = 0;
    dsdneoUserConfig cfg = {0};
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTL;
    cfg.rtl_device = 0;
    DSD_SNPRINTF(cfg.rtl_freq, sizeof(cfg.rtl_freq), "118.1M");
    cfg.rtl_gain = 22;
    cfg.rtl_bw_khz = 4;
    state.ui_msg[0] = '\0';
    rc |= expect_true("am config bw 4 queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("am config bw 4 drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("am config bw 4 not applied", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_int("am config bw 4 restarted nothing", g_config_rtl_creates, 0);
    rc |= expect_true("am config bw 4 toast",
                      strstr(state.ui_msg, "Config not applied: AM 6 kHz does not fit the 4 kHz DSP rate (max 2.4 kHz)")
                          != NULL);
    freeState(&state);
    return rc;
}
#endif

/* --- Issue #521: squelch edits beneath a row or target that overrides it --- */

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
static dsd_trunk_tune_result
gain_scan_tune_to_cc_ok(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)ted_sps;
    (void)request_id;
    if (state) {
        state->trunk_cc_freq = freq;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

/* Issue #518 follow-up: a config whose [input] spec is the running input's still sets its gain. A live gain edit does
 * not rewrite the spec, so reloading the config the session started from used to leave the edited gain running; and a
 * SoapySDR spec carries no gain, so a gain-only change wrote the options without reopening the device. */
static int
test_config_gain_applies_when_the_spec_is_unchanged(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.audio_out_type = 9;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "rtl:0:851375000:22:0:24:0:2");
    opts.rtl_gain_value = 30; /* edited live; the spec still says 22 */
    opts.rtl_dsp_bw_khz = 24;
    opts.rtl_volume_multiplier = 2;
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 0;
    state.rtl_ctx = (RtlSdrContext*)g_config_rtl_ctx;

    dsdneoUserConfig cfg = {0};
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTL;
    cfg.rtl_device = 0;
    DSD_SNPRINTF(cfg.rtl_freq, sizeof(cfg.rtl_freq), "851375000");
    cfg.rtl_gain = 22;
    cfg.rtl_gain_is_set = 1;
    cfg.rtl_ppm_is_set = 1;
    cfg.rtl_bw_khz = 24;
    cfg.rtl_volume = 2;
    g_config_rtl_creates = 0;
    g_config_rtl_create_gain = -1;
    int rc = expect_true("same-spec config queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("same-spec config drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("same-spec config keeps the spec", opts.audio_in_dev, "rtl:0:851375000:22:0:24:0:2");
    rc |= expect_int("same-spec config sets its gain", opts.rtl_gain_value, 22);
    rc |= expect_int("same-spec config reopens on its gain", g_config_rtl_create_gain, 22);

    /* The same gain again reopens nothing. */
    g_config_rtl_creates = 0;
    rc |= expect_true("unchanged config queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("unchanged config drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("unchanged gain reopens nothing", g_config_rtl_creates, 0);

    /* A SoapySDR spec has no gain in it: a gain-only change still reopens the device on it. The device keeps the
       frequency and DSP bandwidth it runs (a scanner may own the frequency, and a new rate would need a reopen's width
       checks), whatever frequency and bandwidth the config was saved with. */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "soapy:driver=rtlsdr");
    opts.rtl_gain_value = 30;
    opts.rtlsdr_center_freq = 852000000U;
    dsdneoUserConfig soapy = cfg;
    soapy.input_source = DSDCFG_INPUT_SOAPY;
    DSD_SNPRINTF(soapy.soapy_args, sizeof(soapy.soapy_args), "driver=rtlsdr");
    soapy.rtl_gain = 15;
    soapy.rtl_bw_khz = 48;
    g_config_rtl_creates = 0;
    g_config_rtl_create_gain = -1;
    g_config_rtl_create_freq = -1;
    rc |= expect_true("soapy gain config queued", dsd_app_command_apply_config(&soapy) > 0);
    rc |= expect_int("soapy gain config drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("soapy gain config keeps the spec", opts.audio_in_dev, "soapy:driver=rtlsdr");
    rc |= expect_int("soapy gain config sets its gain", opts.rtl_gain_value, 15);
    rc |= expect_int("soapy gain config reopens the device on it", g_config_rtl_create_gain, 15);
    rc |= expect_true("soapy gain config keeps the running frequency",
                      opts.rtlsdr_center_freq == 852000000U && g_config_rtl_create_freq == 852000000L);
    rc |= expect_int("soapy gain config keeps the running DSP bandwidth", opts.rtl_dsp_bw_khz, 24);

    state.rtl_ctx = NULL;
    g_config_rtl_open_ok = 0;
    freeState(&state);
    return rc;
}

/* Issue #518 follow-up: a config that restarts the running input for its gain alone (its [input] builds no spec of its
 * own here: no rtl_freq) reopens the stream on the analog width it runs, so the width is held to the running rate
 * before anything is torn down, as any reopen holds it. An AM width the running DSP bandwidth cannot filter refuses
 * the whole config with the input left running. */
static int
test_config_gain_restart_holds_the_running_width(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "rtl:0:118100000:22:0:6:0:1");
    (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_AM, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state);
    opts.rtl_dsp_bw_khz = 6; /* below the 8 kHz the AM default needs */
    opts.rtl_gain_value = 22;
    g_config_rtl_open_ok = 1;
    /* No stream context: the check holds the width to the DSP bandwidth, as RTL_SET_BW's test does. */
    state.rtl_ctx = NULL;

    dsdneoUserConfig cfg = {0};
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTL;
    cfg.rtl_gain = 30;
    cfg.rtl_gain_is_set = 1;
    g_config_rtl_creates = 0;
    state.ui_msg[0] = '\0';
    int rc = expect_true("gain-only config queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("gain-only config drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("gain-only config tears nothing down", g_config_rtl_creates, 0);
    rc |= expect_int("gain-only config leaves the gain", opts.rtl_gain_value, 22);
    rc |= expect_true("gain-only config says why", strstr(state.ui_msg, "Config not applied:") != NULL);
    state.rtl_ctx = NULL;
    g_config_rtl_open_ok = 0;
    freeState(&state);
    return rc;
}

/* Issue #518 follow-up: under --trunk-scan the gain and tuner-autogain controls edit the configured gain the scan puts
 * back at every target switch. A live edit used to be reverted to the scan-start gain at the next switch; one made while
 * a target with its own rtl_gain is parked says that target overrides it. The real coordinator and command path run
 * over the wrapped RTL stream, with every retune landing at once. */
static int
test_rtl_gain_commands_edit_the_trunk_scan_default(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.audio_out_type = 9;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "rtl:0:851000000:22:0:48:0:2");
    opts.rtl_gain_value = 22;
    opts.trunk_scan_enabled = 1;
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 0;
    state.rtl_ctx = (RtlSdrContext*)g_config_rtl_ctx;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_cc_request = gain_scan_tune_to_cc_ok});

    char dir[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    int rc = expect_true("gain scan dir", dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_gain_scan") != NULL);
    rc |= expect_int("gain scan csv path", dsd_test_path_join(csv, sizeof csv, dir, "targets.csv"), 0);
    static const char k_targets[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,rtl_gain\n"
                                    "own,p25-trunk,851000000,,3000,,own gain,10\n"
                                    "inherit,dmr-trunk,452000000,,3000,,inherits\n";
    rc |= write_file_bytes(csv, k_targets, sizeof k_targets - 1U);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", csv);
    char err[256] = {0};
    const int init_rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    if (init_rc != 0) {
        DSD_FPRINTF(stderr, "gain scan init: %s\n", err);
    }
    rc |= expect_int("gain scan init", init_rc, 0);
    rc |= expect_int("parked on the target with its own gain", opts.rtl_gain_value, 10);

    rc |= expect_true("shadowed gain queued", post_i32(DSD_APP_CMD_RTL_SET_GAIN, 30) > 0);
    rc |= expect_int("shadowed gain drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("shadowed gain keeps the target's", opts.rtl_gain_value, 10);
    rc |= expect_str("shadowed gain toast", state.ui_msg, "Default RTL gain -> 30; this channel overrides it (10)");
    dsdneoUserConfig saved;
    dsd_snapshot_opts_to_user_config(&opts, &state, &saved);
    rc |= expect_int("save writes the configured gain", saved.rtl_gain, 30);

    rc |= expect_int("advance", dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_ADVANCE), 0);
    rc |= expect_int("the inheriting target runs the edited gain", opts.rtl_gain_value, 30);

    /* A stream opens with the environment's autogain default; the restart puts the scan's configured one back. */
    int before = -1;
    rc |= expect_int("configured autogain known", dsd_engine_trunk_scan_saved_tuner_autogain(&state, &before), 1);
    rtl_stream_set_tuner_autogain(!before);
    rc |= expect_true("agc gain queued", post_i32(DSD_APP_CMD_RTL_SET_GAIN, 0) > 0);
    rc |= expect_int("agc gain drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("agc gain in force", opts.rtl_gain_value, 0);
    rc |= expect_str("agc gain toast", state.ui_msg, "Applied: RTL gain -> AGC");
    rc |= expect_int("the restart applies the configured autogain", rtl_stream_get_tuner_autogain(), before);

    /* The autogain toggle flips the configured setting both ways rather than reading the stream, which a retune under
       a manual gain always leaves off; under AGC the stream follows it. */
    const dsd_app_dsp_payload toggle = {.op = DSD_APP_DSP_OP_TUNER_AUTOGAIN_TOGGLE};
    for (int i = 0; i < 2; i++) {
        rc |= expect_true("autogain toggle queued",
                          dsd_app_command_submit(DSD_APP_CMD_DSP_OP, &toggle, sizeof toggle) > 0);
        rc |= expect_int("autogain toggle drained", dsd_app_drain_cmds(&opts, &state), 1);
        int now = -1;
        (void)dsd_engine_trunk_scan_saved_tuner_autogain(&state, &now);
        const int want = (i == 0) ? !before : before;
        rc |= expect_int("autogain toggle flips the configured setting", now, want);
        rc |= expect_str("autogain toggle toast", state.ui_msg,
                         want ? "Applied: tuner autogain -> On" : "Applied: tuner autogain -> Off");
        rc |= expect_int("autogain toggle reaches the stream under AGC", rtl_stream_get_tuner_autogain(), want);
    }

    rc |= expect_int("advance away", dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_ADVANCE), 0);
    rc |= expect_int("own gain again", opts.rtl_gain_value, 10);

    /* A config's gain is the configured gain too: while the target's own gain is parked the device reopens on the
       target's, and a restart under a manual gain leaves the supervisor off. */
    dsdneoUserConfig cfg = {0};
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTL;
    cfg.rtl_device = 0;
    DSD_SNPRINTF(cfg.rtl_freq, sizeof(cfg.rtl_freq), "851.0125M");
    cfg.rtl_gain = 30;
    cfg.rtl_bw_khz = 48;
    cfg.rtl_volume = 2;
    g_config_rtl_create_gain = -1;
    rtl_stream_set_tuner_autogain(1);
    rc |= expect_true("gain config queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("gain config drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("config keeps the target's gain in force", opts.rtl_gain_value, 10);
    rc |= expect_int("config reopens on the target's gain", g_config_rtl_create_gain, 10);
    rc |= expect_int("config sets the configured gain", state.trunk_scan_configured_gain, 30);
    rc |= expect_int("manual gain reopen leaves autogain off", rtl_stream_get_tuner_autogain(), 0);

    /* A rolled-back config puts the configured gain back. */
    DSD_SNPRINTF(cfg.rtl_freq, sizeof(cfg.rtl_freq), "851.025M");
    cfg.rtl_gain = 35;
    g_config_rtl_fail_starts = 1;
    rc |= expect_true("failing config queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("failing config drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("rolled-back config keeps the configured gain", state.trunk_scan_configured_gain, 30);
    rc |= expect_int("rolled-back config keeps the target's gain", opts.rtl_gain_value, 10);
    g_config_rtl_fail_starts = 0;

    rc |= expect_int("advance back", dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_ADVANCE), 0);
    rc |= expect_int("the config's gain reaches the inheriting target", opts.rtl_gain_value, 30);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    rc |= expect_int("shutdown keeps the configured gain", opts.rtl_gain_value, 30);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    state.rtl_ctx = NULL;
    g_config_rtl_open_ok = 0;
    (void)remove(csv);
    (void)dsd_test_rmdir(dir);
    return rc;
}
#endif

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
/* The level the RTL demodulator was last handed, by the scan scope (runtime hook) or by a
 * command (rtl_stream_set_channel_squelch, wrapped below); the last writer wins, as it does in
 * the demod. */
static int g_cmd_squelch_pushes;
static double g_cmd_squelch_pushed = -1.0;

static void
record_cmd_squelch_push(double mean_power) {
    g_cmd_squelch_pushes++;
    g_cmd_squelch_pushed = mean_power;
}

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol name.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
void __wrap_rtl_stream_set_channel_squelch(float level);

void
__wrap_rtl_stream_set_channel_squelch(float level) {
    record_cmd_squelch_push((double)level);
}

/* An auto squelch reaches the demod whole (issue #518 follow-up). */
static int g_cmd_squelch_setting_pushes;
static dsd_squelch_setting g_cmd_squelch_setting_pushed;

void __wrap_rtl_stream_set_channel_squelch_setting(const dsd_squelch_setting* setting);

void
__wrap_rtl_stream_set_channel_squelch_setting(const dsd_squelch_setting* setting) {
    g_cmd_squelch_setting_pushes++;
    g_cmd_squelch_setting_pushed = *setting;
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

/* The demod keeps a float, so a command's push is compared at float precision. */
static int
expect_squelch_db(const char* tag, double level, double db) {
    const double want = dsd_squelch_level_from_sql(db);
    return expect_true(tag, fabs(level - want) <= 1e-6 * fmax(fabs(level), fabs(want)));
}

static double
configured_squelch(const dsd_state* state) {
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(state);
    return configured ? configured->rtl_squelch_level : -1.0;
}

static void
init_squelch_row_context(dsd_opts* opts, dsd_state* state) {
    init_test_context(opts, state);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->audio_out_type = 9;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "rtl:0:851375000:22:0:24:-80:2");
    opts->rtl_squelch_level = dsd_squelch_level_from_sql(-80.0);
    const dsd_rtl_stream_metrics_hooks hooks = {.set_channel_squelch = record_cmd_squelch_push};
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    g_cmd_squelch_pushes = 0;
    g_cmd_squelch_pushed = -1.0;
}

static int
submit_squelch_setting(int mode, int margin_db, double level) {
    dsd_app_squelch_setting_payload payload;
    DSD_MEMSET(&payload, 0, sizeof payload);
    payload.mode = mode;
    payload.margin_db = margin_db;
    payload.level = level;
    return dsd_app_command_submit(DSD_APP_CMD_RTL_SET_SQL_SETTING, &payload, sizeof payload);
}

/* DSD_APP_CMD_RTL_SET_SQL_SETTING (issue #518 follow-up): the whole setting as the configured default. An auto squelch
 * reaches the demod whole, a level as a level, a margin out of range is refused, and under a row's own squelch the
 * edit waits beneath it and the save writes it. */
static int
test_squelch_setting_command(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_squelch_row_context(&opts, &state);
    g_cmd_squelch_setting_pushes = 0;
    int rc = expect_true("auto queued", submit_squelch_setting(DSD_SQUELCH_MODE_AUTO, 6, 0.0) > 0);
    rc |= expect_int("auto drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |=
        expect_true("auto in force", opts.rtl_squelch_mode == DSD_SQUELCH_MODE_AUTO && opts.rtl_squelch_margin_db == 6);
    rc |= expect_squelch_db("auto keeps the level", opts.rtl_squelch_level, -80.0);
    rc |= expect_true("auto pushed whole", g_cmd_squelch_setting_pushes == 1
                                               && g_cmd_squelch_setting_pushed.mode == DSD_SQUELCH_MODE_AUTO
                                               && g_cmd_squelch_setting_pushed.margin_db == 6);
    rc |= expect_str("auto toast", state.ui_msg, "Applied: squelch -> auto +6 dB");

    rc |= expect_true("bad margin queued", submit_squelch_setting(DSD_SQUELCH_MODE_AUTO, 2, 0.0) > 0);
    rc |= expect_int("bad margin drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("bad margin refused", opts.rtl_squelch_margin_db, 6);
    rc |= expect_str("bad margin toast", state.ui_msg, "Failed: squelch update");

    g_cmd_squelch_pushes = 0;
    rc |= expect_true("level queued",
                      submit_squelch_setting(DSD_SQUELCH_MODE_LEVEL, 0, dsd_squelch_level_from_sql(-50.0)) > 0);
    rc |= expect_int("level drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("level in force", opts.rtl_squelch_mode == DSD_SQUELCH_MODE_LEVEL);
    rc |= expect_squelch_db("level stored", opts.rtl_squelch_level, -50.0);
    rc |= expect_true("level pushed as a level", g_cmd_squelch_pushes == 1 && g_cmd_squelch_setting_pushes == 1);
    rc |= expect_squelch_db("level demod value", g_cmd_squelch_pushed, -50.0);
    rc |= expect_str("level toast", state.ui_msg, "Applied: squelch -> -50.0 dB");

    /* Requests of the two modes queued before one drain both apply, in order, so each keeps what it sets (the margin,
       the level); two of one mode coalesce to the later. */
    rc |= expect_int("auto then level: auto", submit_squelch_setting(DSD_SQUELCH_MODE_AUTO, 9, 0.0),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("auto then level: level apart",
                     submit_squelch_setting(DSD_SQUELCH_MODE_LEVEL, 0, dsd_squelch_level_from_sql(-45.0)),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("auto then level drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_true("level in force, the margin kept",
                      opts.rtl_squelch_mode == DSD_SQUELCH_MODE_LEVEL && opts.rtl_squelch_margin_db == 9);
    rc |= expect_squelch_db("the level", opts.rtl_squelch_level, -45.0);
    rc |= expect_int("level then auto: level",
                     submit_squelch_setting(DSD_SQUELCH_MODE_LEVEL, 0, dsd_squelch_level_from_sql(-40.0)),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("level then auto: auto apart", submit_squelch_setting(DSD_SQUELCH_MODE_AUTO, 7, 0.0),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("level then auto drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |=
        expect_true("auto in force", opts.rtl_squelch_mode == DSD_SQUELCH_MODE_AUTO && opts.rtl_squelch_margin_db == 7);
    rc |= expect_squelch_db("the level kept beneath", opts.rtl_squelch_level, -40.0);
    rc |=
        expect_int("auto again", submit_squelch_setting(DSD_SQUELCH_MODE_AUTO, 11, 0.0), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("one mode coalesces", submit_squelch_setting(DSD_SQUELCH_MODE_AUTO, 12, 0.0),
                     DSD_APP_COMMAND_SUBMIT_COALESCED);
    rc |= expect_int("coalesced drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("the later margin", opts.rtl_squelch_margin_db, 12);

    /* Beneath a row's own squelch: the default moves, the row stays in force and the demod hears nothing. */
    rc |= expect_int("row entered", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR), 0);
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_SQUELCH;
    row.squelch_db = -60;
    rc |= expect_int("row installed", dsd_scan_mode_options(&opts, &state, &row), 0);
    g_cmd_squelch_setting_pushes = 0;
    g_cmd_squelch_pushes = 0;
    rc |= expect_true("shadowed auto queued", submit_squelch_setting(DSD_SQUELCH_MODE_AUTO, 8, 0.0) > 0);
    rc |= expect_int("shadowed auto drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("shadowed auto keeps the row", opts.rtl_squelch_mode == DSD_SQUELCH_MODE_LEVEL);
    rc |= expect_squelch_db("shadowed auto keeps the row level", opts.rtl_squelch_level, -60.0);
    rc |= expect_true("shadowed auto edits the default",
                      dsd_scan_mode_configured_view(&state)->rtl_squelch_mode == DSD_SQUELCH_MODE_AUTO
                          && dsd_scan_mode_configured_view(&state)->rtl_squelch_margin_db == 8);
    rc |= expect_true("shadowed auto leaves the demod alone",
                      g_cmd_squelch_setting_pushes == 0 && g_cmd_squelch_pushes == 0);
    rc |= expect_str("shadowed auto toast", state.ui_msg,
                     "Default squelch auto +8 dB; this channel overrides it (-60.0 dB)");
    dsdneoUserConfig saved;
    dsd_snapshot_opts_to_user_config(&opts, &state, &saved);
    rc |= expect_true("save writes the auto default",
                      saved.rtl_sql_mode == DSD_SQUELCH_MODE_AUTO && saved.rtl_sql_margin_db == 8);
    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_true("leave puts the auto default in force",
                      opts.rtl_squelch_mode == DSD_SQUELCH_MODE_AUTO && g_cmd_squelch_setting_pushes == 0);

    /* The noise squelch: in force and pushed whole; the AM monitor on its own refuses it and says why. */
    rc |= expect_true("noise queued", submit_squelch_setting(DSD_SQUELCH_MODE_NOISE, 14, 0.0) > 0);
    rc |= expect_int("noise drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("noise in force",
                      opts.rtl_squelch_mode == DSD_SQUELCH_MODE_NOISE && opts.rtl_squelch_margin_db == 14);
    rc |= expect_true("noise pushed whole", g_cmd_squelch_setting_pushes == 1
                                                && g_cmd_squelch_setting_pushed.mode == DSD_SQUELCH_MODE_NOISE
                                                && g_cmd_squelch_setting_pushed.margin_db == 14);
    rc |= expect_str("noise toast", state.ui_msg, "Applied: squelch -> noise +14 dB");
    opts.analog_only = 1;
    opts.analog_demod = DSD_ANALOG_DEMOD_AM;
    const dsd_squelch_setting level40 = dsd_squelch_setting_of_level(dsd_squelch_level_from_sql(-40.0));
    dsd_squelch_setting_store(&opts, &level40);
    rc |= expect_true("noise on AM queued", submit_squelch_setting(DSD_SQUELCH_MODE_NOISE, 10, 0.0) > 0);
    rc |= expect_int("noise on AM drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("noise on AM refused", opts.rtl_squelch_mode == DSD_SQUELCH_MODE_LEVEL);
    rc |=
        expect_str("noise on AM toast", state.ui_msg, "Refused: the noise squelch needs an FM channel; AM takes auto");
    opts.analog_only = 0;
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_state_ext_free_all(&state);
    return rc;
}

/* Every squelch editor edits the configured default, never the row's value; the row keeps its
 * threshold in dsd_opts and in the demod, a shadowed edit says so, and a save writes only the
 * default. Airspy edits and the input enables are not scoped: a stream they open starts on the
 * row's acquisition and threshold, the ones in force. */
static int
test_squelch_commands_edit_the_configured_default(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_squelch_row_context(&opts, &state);
    int rc = expect_int("squelch row entered", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR), 0);
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_SQUELCH;
    row.squelch_db = -60;
    rc |= expect_int("squelch row installed", dsd_scan_mode_options(&opts, &state, &row), 0);
    rc |= expect_int("squelch row pushed once", g_cmd_squelch_pushes, 1);
    rc |= expect_squelch_db("squelch row demod level", g_cmd_squelch_pushed, -60.0);
    const int row_frame_p25p1 = opts.frame_p25p1;
    rc |= expect_true("the DMR row differs from the configured decoder set",
                      dsd_scan_mode_configured_view(&state)->frame_p25p1 != row_frame_p25p1);

    /* A shadowed edit reaches the default alone: the demod keeps the row's threshold and hears
     * nothing, since nothing it gates on changed. */
    g_cmd_squelch_pushes = 0;
    rc |= expect_true("shadowed squelch queued", dsd_app_command_set_double(DSD_APP_CMD_RTL_SET_SQL_DB, -75.0) > 0);
    rc |= expect_int("shadowed squelch drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_squelch_db("shadowed squelch keeps the row", opts.rtl_squelch_level, -60.0);
    rc |= expect_squelch_db("shadowed squelch edits the default", configured_squelch(&state), -75.0);
    rc |= expect_int("shadowed squelch leaves the demod alone", g_cmd_squelch_pushes, 0);
    rc |= expect_squelch_db("shadowed squelch demod level", g_cmd_squelch_pushed, -60.0);
    rc |= expect_str("shadowed squelch toast", state.ui_msg,
                     "Default squelch -75.0 dB; this channel overrides it (-60.0 dB)");
    dsdneoUserConfig saved;
    dsd_snapshot_opts_to_user_config(&opts, &state, &saved);
    rc |= expect_int("save keeps the user default", saved.rtl_sql, -75);

    /* No override on air: the edit applies to what is in force, and the toast says just that. */
    rc |= expect_int("row squelch cleared", dsd_scan_mode_options(&opts, &state, NULL), 0);
    rc |= expect_squelch_db("cleared row hands back the edited default", g_cmd_squelch_pushed, -75.0);
    rc |= expect_true("plain squelch queued", dsd_app_command_set_double(DSD_APP_CMD_RTL_SET_SQL_DB, -70.0) > 0);
    rc |= expect_int("plain squelch drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_squelch_db("plain squelch in force", opts.rtl_squelch_level, -70.0);
    rc |= expect_squelch_db("plain squelch edits the default", configured_squelch(&state), -70.0);
    rc |= expect_squelch_db("plain squelch demod level", g_cmd_squelch_pushed, -70.0);
    rc |= expect_str("plain squelch toast", state.ui_msg, "Applied: squelch -> -70.0 dB");
    rc |= expect_int("row squelch reinstalled", dsd_scan_mode_options(&opts, &state, &row), 0);
    rc |= expect_squelch_db("reinstalled row demod level", g_cmd_squelch_pushed, -60.0);

    /* AIRSPY_SET rewrites no squelch and stays unscoped: the stream it reopens starts on the
     * row's threshold and decoder set, not the configured baseline. */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "airspy");
    g_config_rtl_creates = 0;
    dsd_app_airspy_setting_payload edit;
    DSD_MEMSET(&edit, 0, sizeof edit);
    DSD_SNPRINTF(edit.key, sizeof edit.key, "%s", "airspy_bias_tee");
    DSD_SNPRINTF(edit.value, sizeof edit.value, "%s", "on");
    rc |= expect_true("airspy edit queued", dsd_app_command_submit(DSD_APP_CMD_AIRSPY_SET, &edit, sizeof edit) > 0);
    rc |= expect_int("airspy edit drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("airspy edit reopened the stream", g_config_rtl_creates >= 1);
    rc |= expect_squelch_db("airspy reopen opens on the row", g_config_rtl_create_squelch, -60.0);
    rc |= expect_int("airspy reopen keeps the row's decoder set", g_config_rtl_create_frame_p25p1, row_frame_p25p1);
    rc |= expect_squelch_db("airspy edit keeps the row", opts.rtl_squelch_level, -60.0);
    rc |= expect_squelch_db("airspy edit keeps the default", configured_squelch(&state), -70.0);
    rc |= expect_squelch_db("airspy edit demod level", g_cmd_squelch_pushed, -60.0);

    /* Enabling an input does not rewrite the squelch either: the stream it opens starts on the
     * row's level, the one in force, and the default stays put. The Airspy enable rewrites the
     * device spec first, so it gets its own step. */
    g_config_rtl_creates = 0;
    rc |= expect_true("airspy enable input queued", post_empty(DSD_APP_CMD_AIRSPY_ENABLE_INPUT) > 0);
    rc |= expect_int("airspy enable input drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("airspy enable input selects the Airspy", opts.audio_in_dev, "airspy");
    rc |= expect_true("airspy enable input opened a stream", g_config_rtl_creates >= 1);
    rc |= expect_squelch_db("airspy enable input opens on the row", g_config_rtl_create_squelch, -60.0);
    rc |= expect_squelch_db("airspy enable input keeps the row", opts.rtl_squelch_level, -60.0);
    rc |= expect_squelch_db("airspy enable input keeps the default", configured_squelch(&state), -70.0);

    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "rtl:0:851375000:22:0:24:-70:2");
    g_config_rtl_creates = 0;
    rc |= expect_true("enable input queued", post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT) > 0);
    rc |= expect_int("enable input drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("enable input opened a stream", g_config_rtl_creates >= 1);
    rc |= expect_squelch_db("enable input opens on the row", g_config_rtl_create_squelch, -60.0);
    rc |= expect_squelch_db("enable input keeps the default", configured_squelch(&state), -70.0);

    /* CONFIG_APPLY pushes its own default while suspended; the row's level comes back after. */
    dsdneoUserConfig cfg = {0};
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTL;
    cfg.rtl_device = 0;
    DSD_SNPRINTF(cfg.rtl_freq, sizeof(cfg.rtl_freq), "851.375M");
    cfg.rtl_gain = 22;
    cfg.rtl_bw_khz = 24;
    cfg.rtl_sql = -65;
    cfg.rtl_volume = 2;
    g_cmd_squelch_pushes = 0;
    rc |= expect_true("squelch config queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("squelch config drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_squelch_db("config keeps the row", opts.rtl_squelch_level, -60.0);
    rc |= expect_squelch_db("config edits the default", configured_squelch(&state), -65.0);
    rc |= expect_true("config re-pushes the row", g_cmd_squelch_pushes >= 1);
    rc |= expect_squelch_db("config demod level", g_cmd_squelch_pushed, -60.0);
    dsd_snapshot_opts_to_user_config(&opts, &state, &saved);
    rc |= expect_int("save after config keeps the user default", saved.rtl_sql, -65);

    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_squelch_db("leave restores the default", opts.rtl_squelch_level, -65.0);
    rc |= expect_squelch_db("leave pushes the default", g_cmd_squelch_pushed, -65.0);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    freeState(&state);
    return rc;
}

/* A squelch nudge is not a decoder change. Frame sync writes the detected Phase 2 polarity into
 * dsd_opts while a P25 row is on air; had the setter suspended and re-applied the scope, that
 * live value would read as an acquisition change and end the followed call. */
static int
test_squelch_edit_keeps_live_acquisition(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_squelch_row_context(&opts, &state);
    int rc = expect_int("p25 row entered", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_P25), 0);
    rc |= expect_int("p25 row options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    rc |= expect_int("p25 row polarity from the preset", opts.inverted_p2, 0);
    for (int shadowed = 0; shadowed <= 1; shadowed++) {
        if (shadowed) {
            dsd_scan_option_values row = {0};
            row.present = DSD_SCAN_OPT_SQUELCH;
            row.squelch_db = -60;
            rc |= expect_int("p25 row squelch", dsd_scan_mode_options(&opts, &state, &row), 0);
        }
        opts.inverted_p2 = 1;
        opts.trunk_is_tuned = 1;
        state.trunk_vc_freq[0] = 851012500;
        const double db = shadowed ? -75.0 : -70.0;
        rc |= expect_true("live squelch queued", dsd_app_command_set_double(DSD_APP_CMD_RTL_SET_SQL_DB, db) > 0);
        rc |= expect_int("live squelch drained", dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_squelch_db("live squelch edits the default", configured_squelch(&state), db);
        rc |= expect_squelch_db("live squelch level in force", opts.rtl_squelch_level, shadowed ? -60.0 : db);
        rc |= expect_int("live squelch keeps the detected polarity", opts.inverted_p2, 1);
        rc |= expect_int("live squelch keeps the followed call", opts.trunk_is_tuned, 1);
        rc |= expect_true("live squelch keeps the voice channel", state.trunk_vc_freq[0] == 851012500);
        rc |= expect_int("live squelch is not an acquisition change", dsd_scan_mode_active(&state), DSD_SCAN_MODE_P25);
    }
    dsd_scan_mode_leave(&opts, &state);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    freeState(&state);
    return rc;
}
#endif

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP) && defined(DSD_NEO_TEST_IO_CONTROL_WRAP)
/* How a replay is left mid-run: an input switch to Pulse (ui_input_switched()), a stream restart
   (svc_rtl_stop_locked()), and a stop of playback onto the Pulse input. A switch that fails leaves nothing (issue #634):
   test_replay_kept_by_a_failed_stop(). */
enum {
    REPLAY_LEAVE_INPUT_SWITCH = 0,
    REPLAY_LEAVE_RTL_RESTART = 1,
    REPLAY_LEAVE_STOP_PLAYBACK = 2,
};

static int
leave_replay_through(int how, dsd_opts* opts, dsd_state* state) {
    if (how == REPLAY_LEAVE_INPUT_SWITCH) {
        if (post_empty(DSD_APP_CMD_INPUT_SET_PULSE) != DSD_APP_COMMAND_SUBMIT_QUEUED) {
            return -1;
        }
    } else if (how == REPLAY_LEAVE_RTL_RESTART) {
        if (dsd_app_command_action(DSD_APP_CMD_RTL_RESTART) != DSD_APP_COMMAND_SUBMIT_QUEUED) {
            return -1;
        }
    } else if (post_empty(DSD_APP_CMD_STOP_PLAYBACK) != DSD_APP_COMMAND_SUBMIT_QUEUED) {
        return -1;
    }
    return dsd_app_drain_cmds(opts, state);
}

/*
 * Issue #572: a replay left mid-run puts the decode clock back on the system clock, and the stamps the replay took on
 * its capture clock go on ageing from where they stood. A recent-activity row, a call ended by sync loss and the FSK
 * watchdog's last request, all stamped at capture time, read as just now right after the leave, never ahead of it, and
 * a row past its TTL expires. A leave that fell back to the platform monotonic clock's origin would leave them ~1.8e9 s
 * in the future: the row would never expire and every window from them would read negative.
 */
static int
test_replay_leave_keeps_decode_stamps_ageing(void) {
    static const struct {
        int how;
        const char* tag;
    } cases[] = {
        {REPLAY_LEAVE_INPUT_SWITCH, "replay left by an input switch"},
        {REPLAY_LEAVE_RTL_RESTART, "replay left by a stream restart"},
        {REPLAY_LEAVE_STOP_PLAYBACK, "replay left by stopping playback onto pulse"},
    };

    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char* case_tag = cases[i].tag;
        char tag[160];
        init_test_context(&opts, &state);
        reset_config_rtl_wrap();
        if (cases[i].how == REPLAY_LEAVE_RTL_RESTART) {
            opts.audio_in_type = AUDIO_IN_RTL;
            state.rtl_ctx = (RtlSdrContext*)g_config_rtl_ctx;
            g_config_rtl_open_ok = 1;
        } else if (cases[i].how == REPLAY_LEAVE_STOP_PLAYBACK) {
            opts.audio_out_type = 0;
            opts.audio_in_type = AUDIO_IN_WAV;
            arm_open_audio_input_stub(1, 0);
        } else {
            arm_open_audio_input_stub(1, 0);
        }
        dsd_decode_clock_use_replay(1788245497LL); /* 2026-09-01T06:51:37Z */
        dsd_decode_clock_set_media_ns(5ULL * 1000000000ULL);

        dsd_call_observation call = dsd_call_observation_data(DSD_SYNC_DMR_BS_VOICE_POS, 0U, 5678U, 1234U);
        call.kind = DSD_CALL_KIND_GROUP_VOICE;
        call.policy_target_id = 1234U;
        DSD_SNPRINTF(tag, sizeof tag, "%s: stamps taken on the capture clock", case_tag);
        rc |= expect_int(tag, dsd_recent_activity_publish(&state, 1U, &call, NULL, 0U), 1);
        rc |= expect_true(tag, dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) > 0);
        rc |= expect_true(tag, dsd_call_state_end_ex(&state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS) > 0);
        state.rtl_fsk_reacquire_last_request_m = dsd_decode_now_mono_s();
        const double stamped_m = dsd_decode_now_mono_s();

        DSD_SNPRINTF(tag, sizeof tag, "%s: the leave ran", case_tag);
        rc |= expect_int(tag, leave_replay_through(cases[i].how, &opts, &state), 1);
        DSD_SNPRINTF(tag, sizeof tag, "%s: the decode clock is the system's", case_tag);
        rc |= expect_int(tag, (int)dsd_decode_clock_source(), (int)DSD_DECODE_CLOCK_SYSTEM);
        const double now_m = dsd_decode_now_mono_s();
        DSD_SNPRINTF(tag, sizeof tag, "%s: decode-mono time goes on from the capture time", case_tag);
        rc |= expect_true(tag, now_m >= stamped_m && now_m - stamped_m < 1.0);
        dsd_call_snapshot ended;
        DSD_SNPRINTF(tag, sizeof tag, "%s: the ended call is just behind now", case_tag);
        rc |= expect_true(tag, dsd_call_state_get(&state, 0U, &ended) > 0 && ended.phase == DSD_CALL_PHASE_ENDED);
        rc |= expect_true(tag, now_m >= ended.ended_m && now_m - ended.ended_m < 1.0);
        DSD_SNPRINTF(tag, sizeof tag, "%s: the watchdog's last request is not in the future", case_tag);
        rc |= expect_true(tag, now_m >= state.rtl_fsk_reacquire_last_request_m);
        /* The row stands within a TTL longer than the time since, and expires past a shorter one. */
        DSD_SNPRINTF(tag, sizeof tag, "%s: the recent-activity row stands within 5 s", case_tag);
        rc |= expect_int(tag, dsd_recent_activity_expire(&state, 0U, 5000U), 0);
        dsd_sleep_ms(20U);
        DSD_SNPRINTF(tag, sizeof tag, "%s: the recent-activity row expires past 1 ms", case_tag);
        rc |= expect_int(tag, dsd_recent_activity_expire(&state, 0U, 1U), 1);
        DSD_SNPRINTF(tag, sizeof tag, "%s: the watchdog's request ages at real time", case_tag);
        rc |= expect_true(tag, dsd_decode_now_mono_s() - state.rtl_fsk_reacquire_last_request_m >= 0.02);

        arm_open_audio_input_stub(0, 0);
        state.rtl_ctx = NULL;
        reset_config_rtl_wrap();
        dsd_decode_clock_use_system();
        freeState(&state);
    }
    return rc;
}

/* A stop of playback whose Pulse input does not open changes nothing (issue #634): the playback runs on, and a replay's
   capture clock still times what the decoder reads. */
static int
test_replay_kept_by_a_failed_stop(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    opts.audio_out_type = 0;
    opts.audio_in_type = AUDIO_IN_WAV;
    arm_open_audio_input_stub(1, -1);
    dsd_decode_clock_use_replay(1788245497LL);
    dsd_decode_clock_set_media_ns(5ULL * 1000000000ULL);
    rc |= expect_int("failed stop ran", leave_replay_through(REPLAY_LEAVE_STOP_PLAYBACK, &opts, &state), 1);
    rc |= expect_int("failed stop fails", dsd_app_command_test_last_failed(), 1);
    rc |= expect_int("failed stop keeps the playback", opts.audio_in_type, AUDIO_IN_WAV);
    rc |=
        expect_int("failed stop keeps the capture clock", (int)dsd_decode_clock_source(), (int)DSD_DECODE_CLOCK_REPLAY);
    arm_open_audio_input_stub(0, 0);
    dsd_decode_clock_use_system();
    freeState(&state);
    return rc;
}
#endif

#ifdef DSD_NEO_TEST_AUDIO_ENSURE_WRAP
/* The sink helpers a decode-mode change calls, recorded instead of run (every build, radio or not). */
static int g_ensure_analog_calls;
static int g_ensure_digital_calls;
/* The output layout in the options when the digital sink was last asked for: the one a stream opened there gets. */
static int g_ensure_digital_channels;
static int g_ensure_digital_rate;
/*
 * Calls that reached the sink helpers from a session not playing to the null output. Where the helpers are not wrapped
 * (macOS, Windows, other compilers) each would open a host audio stream or socket, so every case that changes the
 * decode mode starts from init_decode_mode_context(); main() checks this once every case has run.
 */
static int g_ensure_off_null_calls;

static void
note_ensure_output(const dsd_opts* opts, const char* helper) {
    if (opts->audio_out_type != 9) {
        g_ensure_off_null_calls++;
        DSD_FPRINTF(stderr, "%s reached with audio_out_type %d; start the case from init_decode_mode_context()\n",
                    helper, opts->audio_out_type);
    }
}

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol name.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
int __wrap_dsd_audio_ensure_analog_output(dsd_opts* opts);
int __wrap_dsd_audio_ensure_digital_output(dsd_opts* opts);

int
__wrap_dsd_audio_ensure_analog_output(dsd_opts* opts) {
    note_ensure_output(opts, "dsd_audio_ensure_analog_output()");
    g_ensure_analog_calls++;
    return 0;
}

int
__wrap_dsd_audio_ensure_digital_output(dsd_opts* opts) {
    note_ensure_output(opts, "dsd_audio_ensure_digital_output()");
    g_ensure_digital_calls++;
    g_ensure_digital_channels = opts->pulse_digi_out_channels;
    g_ensure_digital_rate = opts->pulse_digi_rate_out;
    return 0;
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

/*
 * DECODE_MODE_SET opens the sink the new mode writes to: the raw monitor sink for Analog, the digital voice sink for
 * a digital mode. Holds without a radio front end.
 */
static int
test_decode_mode_set_ensures_family_sink(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    g_ensure_analog_calls = g_ensure_digital_calls = 0;
    rc |= expect_int("analog sink mode queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("analog sink mode drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("analog mode ensures the raw sink", g_ensure_analog_calls, 1);
    rc |= expect_int("analog mode leaves the digital sink", g_ensure_digital_calls, 0);

    g_ensure_analog_calls = g_ensure_digital_calls = 0;
    rc |= expect_int("digital sink mode queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_NXDN48),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("digital sink mode drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("digital mode ensures the digital sink", g_ensure_digital_calls, 1);
    rc |= expect_int("digital mode leaves the raw sink", g_ensure_analog_calls, 0);
    freeState(&state);
    return rc;
}

/*
 * A [mode] preset also carries an audio layout, but the session's output streams keep the one they were opened with,
 * so a config apply keeps the session's layout as DECODE_MODE_SET does. A config that moves a -fA session (mono) to
 * DMR opens the digital sink mono, and a later [mode] keeps the options on the layout that stream has: here P25
 * Phase 1 after Analog, which reuses the stream the DMR config opened. A stereo DMR session keeps its two channels
 * through a mono [mode]. Letting the preset change it would dispatch mono writes into the stereo stream (reading past
 * each buffer) or stereo writes into a mono one.
 */
static int
test_config_apply_keeps_session_output_layout(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    rc |= expect_int("layout: -fA session",
                     dsd_apply_decode_mode_preset(DSDCFG_MODE_ANALOG, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state), 0);
    rc |= expect_int("layout: -fA session is mono", opts.pulse_digi_out_channels, 1);
    g_ensure_analog_calls = g_ensure_digital_calls = 0;
    g_ensure_digital_channels = g_ensure_digital_rate = 0;
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_DMR, "layout: config dmr");
    rc |= expect_int("layout: dmr applied", opts.analog_only == 0 && opts.frame_dmr == 1, 1);
    rc |= expect_int("layout: digital sink asked for", g_ensure_digital_calls, 1);
    rc |= expect_int("layout: digital sink opened mono", g_ensure_digital_channels, 1);
    rc |= expect_int("layout: digital sink at the session rate", g_ensure_digital_rate, 8000);
    rc |= expect_int("layout: dmr keeps the session's channel", opts.pulse_digi_out_channels, 1);
    rc |= expect_int("layout: dmr still decodes both slots", opts.dmr_stereo, 1);
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_ANALOG, "layout: config analog");
    rc |= expect_int("layout: analog applied", opts.analog_only, 1);
    rc |= expect_int("layout: analog keeps the session's channel", opts.pulse_digi_out_channels, 1);
    g_ensure_digital_calls = 0;
    g_ensure_digital_channels = 0;
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_P25P1, "layout: config p25p1");
    rc |= expect_int("layout: p25p1 applied", opts.analog_only == 0 && opts.frame_p25p1 == 1, 1);
    rc |= expect_int("layout: p25p1 asks for the digital sink", g_ensure_digital_calls, 1);
    rc |= expect_int("layout: p25p1 asks for it mono", g_ensure_digital_channels, 1);
    rc |= expect_int("layout: p25p1 keeps the stream's channel", opts.pulse_digi_out_channels, 1);
    freeState(&state);

    init_decode_mode_context(&opts, &state);
    rc |= expect_int("layout: stereo dmr session",
                     dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state), 0);
    rc |= expect_int("layout: dmr session is stereo", opts.pulse_digi_out_channels, 2);
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_P25P1, "layout: stereo session to p25p1");
    rc |= expect_int("layout: stereo session runs p25p1", opts.frame_p25p1 == 1 && opts.frame_dmr == 0, 1);
    rc |= expect_int("layout: stereo session keeps two channels", opts.pulse_digi_out_channels, 2);
    rc |= expect_int("layout: stereo session keeps its rate", opts.pulse_digi_rate_out, 8000);
    freeState(&state);
    return rc;
}
#endif

/* A part-collected analog monitor block the decoder holds: 400 of its 960 samples, first and last marked. */
static void
seed_partial_analog_block(dsd_state* state) {
    state->analog_sample_counter = 400;
    state->analog_out_f[0] = 1234.0f;
    state->analog_out_f[399] = -77.0f;
}

static int
expect_partial_analog_block(const char* tag, const dsd_state* state, int kept) {
    if (kept) {
        return expect_true(tag, state->analog_sample_counter == 400 && fabsf(state->analog_out_f[0] - 1234.0f) < 1e-3f
                                    && fabsf(state->analog_out_f[399] + 77.0f) < 1e-3f);
    }
    return expect_true(tag, state->analog_sample_counter == 0 && fabsf(state->analog_out_f[0]) < 1e-6f
                                && fabsf(state->analog_out_f[399]) < 1e-6f);
}

/*
 * The decoder collects every unsynced sample into the analog monitor block (analog_out_f), whether or not a digital
 * session monitors it, and plays a block once it is full. The samples a digital session collected are the old
 * family's (on an RTL front end, the digital discriminator rather than monitor audio), so a change that moves the
 * decoder between the analog and digital families drops the part-collected block: otherwise the first block the new
 * family completes would start with them, audible once Analog opens the raw sink. A change inside a family keeps it.
 * DECODE_MODE_SET and a config apply's [mode] both hold to this.
 */
static int
test_family_change_discards_partial_analog_block(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    rc |= expect_int("block: dmr start queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("block: dmr start drained", dsd_app_drain_cmds(&opts, &state), 1);

    seed_partial_analog_block(&state);
    rc |= expect_int("block: nxdn48 queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_NXDN48),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("block: nxdn48 drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("block: nxdn48 applied", opts.frame_nxdn48 == 1 && opts.analog_only == 0, 1);
    rc |= expect_partial_analog_block("block: digital to digital keeps the block", &state, 1);

    rc |= expect_int("block: analog queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("block: analog drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("block: analog applied", opts.analog_only, 1);
    rc |= expect_partial_analog_block("block: digital to analog drops the block", &state, 0);

    seed_partial_analog_block(&state);
    rc |=
        expect_int("block: dmr queued", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR),
                   DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("block: dmr drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("block: dmr applied", opts.frame_dmr == 1 && opts.analog_only == 0, 1);
    rc |= expect_partial_analog_block("block: analog to digital drops the block", &state, 0);

    seed_partial_analog_block(&state);
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_P25P1, "block: config p25p1");
    rc |= expect_int("block: config p25p1 applied", opts.frame_p25p1 == 1 && opts.analog_only == 0, 1);
    rc |= expect_partial_analog_block("block: config inside the digital family keeps the block", &state, 1);

    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_ANALOG, "block: config analog");
    rc |= expect_int("block: config analog applied", opts.analog_only, 1);
    rc |= expect_partial_analog_block("block: config onto analog drops the block", &state, 0);

    seed_partial_analog_block(&state);
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_ANALOG, "block: config analog again");
    rc |= expect_partial_analog_block("block: config staying analog keeps the block", &state, 1);

    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_DMR, "block: config dmr");
    rc |= expect_int("block: config dmr applied", opts.frame_dmr == 1 && opts.analog_only == 0, 1);
    rc |= expect_partial_analog_block("block: config off analog drops the block", &state, 0);
    freeState(&state);
    return rc;
}

/* Unguarded: the NFM width command's range and store run in every build, radio or not. */
static int
expect_toast(const char* label, const dsd_state* state, const char* text) {
    if (strstr(state->ui_msg, text) == NULL) {
        DSD_FPRINTF(stderr, "%s: toast \"%s\" does not contain \"%s\"\n", label, state->ui_msg, text);
        return 1;
    }
    return 0;
}

static int
submit_nfm_width(dsd_opts* opts, dsd_state* state, int32_t hz, const char* label) {
    int rc =
        expect_int(label, dsd_app_command_set_i32(DSD_APP_CMD_NFM_BANDWIDTH_SET, hz), DSD_APP_COMMAND_SUBMIT_QUEUED);
    state->ui_msg[0] = '\0';
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/*
 * Issue #525: DSD_APP_CMD_NFM_BANDWIDTH_SET is compiled into every build, radio or not, and so are its range check,
 * its store and its toasts. On PCM input (no front end, no wraps) the width is configuration only: a width outside
 * 8000..25000 Hz is refused and the previous one stays, an in-range one is stored, and 0 selects the default.
 */
static int
test_nfm_bandwidth_set_on_pcm_input(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_PULSE;
    opts.analog_only = 1;
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    /* PCM audio arrives demodulated and no channel filter runs on it, so neither a refused nor a stored width changes
       what the monitor hears: the received tone (issue #522) and its generation stay. */
    seed_received_tone(&state);
    const uint32_t seeded = state.analog_rx.generation;

    rc |= submit_nfm_width(&opts, &state, 30000, "pcm nfm 30000");
    rc |= expect_int("pcm nfm 30000 keeps the default", opts.analog_nfm_bandwidth_hz, 0);
    rc |= expect_toast("pcm nfm 30000 toast", &state, "Refused: NFM bandwidth 30 kHz is outside 8 kHz to 25 kHz");
    rc |= expect_received_tone_kept("pcm nfm 30000 keeps the received tone", &state, seeded);
    rc |= submit_nfm_width(&opts, &state, 12500, "pcm nfm 12500");
    rc |= expect_int("pcm nfm 12500 stored", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_toast("pcm nfm 12500 toast", &state, "Applied: NFM bandwidth -> 12.5 kHz");
    rc |= expect_received_tone_kept("pcm nfm 12500 keeps the received tone", &state, seeded);
    rc |= submit_nfm_width(&opts, &state, 7999, "pcm nfm 7999");
    rc |= expect_int("pcm nfm 7999 keeps 12500", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_toast("pcm nfm 7999 toast", &state, "Refused: NFM bandwidth 7.999 kHz is outside 8 kHz to 25 kHz");
    rc |= submit_nfm_width(&opts, &state, 0, "pcm nfm default");
    rc |= expect_int("pcm nfm default stored", opts.analog_nfm_bandwidth_hz, 0);
    rc |= expect_toast("pcm nfm default toast", &state, "Applied: NFM bandwidth -> default");

    /* An int32 setter only, and one that coalesces: taps on the Radio sheet's stepper queued before the decoder drains
       collapse onto the newest width, which is the one applied and toasted. */
    rc |= expect_int("nfm width rejects action shape", dsd_app_command_action(DSD_APP_CMD_NFM_BANDWIDTH_SET),
                     DSD_APP_COMMAND_SUBMIT_REJECTED);
    rc |= expect_int("nfm width rejects u32 shape", dsd_app_command_set_u32(DSD_APP_CMD_NFM_BANDWIDTH_SET, 12500U),
                     DSD_APP_COMMAND_SUBMIT_REJECTED);
    rc |= expect_int("nfm width first queued", dsd_app_command_set_i32(DSD_APP_CMD_NFM_BANDWIDTH_SET, 12500),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("nfm width coalesces", dsd_app_command_set_i32(DSD_APP_CMD_NFM_BANDWIDTH_SET, 20000),
                     DSD_APP_COMMAND_SUBMIT_COALESCED);
    state.ui_msg[0] = '\0';
    rc |= expect_int("coalesced nfm width drained once", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("coalesced nfm width kept latest", opts.analog_nfm_bandwidth_hz, 20000);
    rc |= expect_toast("coalesced nfm width toast", &state, "Applied: NFM bandwidth -> 20 kHz");
    /* A payload too short for an int32 is refused at drain and changes nothing. */
    {
        uint8_t short_payload = 0x10U;
        (void)dsd_app_command_submit(DSD_APP_CMD_NFM_BANDWIDTH_SET, &short_payload, sizeof(short_payload));
        rc |= expect_int("short nfm width drained", dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int("short nfm width ignored", opts.analog_nfm_bandwidth_hz, 20000);
    }
    opts.analog_nfm_bandwidth_hz = 0;

    opts.analog_only = 0;
    freeState(&state);
    return rc;
}

/* Issue #621: the last warning logged about a rigctl peer. */
static char g_rigctl_log[256];

static void
record_rigctl_log(dsd_neo_log_level_t level, const char* text) {
    if (level == LOG_LEVEL_WARN && text && strstr(text, "rigctl peer")) {
        DSD_SNPRINTF(g_rigctl_log, sizeof g_rigctl_log, "%s", text);
    }
}

/* The run's one log tap, installed first thing in main(): a tap is never replaced. It keeps the I/Q capture lines the
   RTL cases read and the rigctl peer warnings. */
static void
record_test_log(dsd_neo_log_level_t level, const char* text, void* ctx) {
#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
    record_capture_log(level, text, ctx);
#else
    (void)ctx;
#endif
    record_rigctl_log(level, text);
}

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
/* The requests sent to the fake rigctl peer: @p sent of them, the last @p kind at @p bandwidth, all under the P25 SM
   tick guard. */
static int
expect_rigctl_sent(const char* label, int sent, int kind, int bandwidth) {
    if (g_rigctl_fake_sent == sent
        && (sent == 0 || (g_rigctl_fake_last_kind == kind && g_rigctl_fake_last_bw == bandwidth))
        && g_rigctl_fake_unguarded == 0) {
        return 0;
    }
    DSD_FPRINTF(stderr, "%s: sent %d (last kind %d at %d Hz, %d unguarded), want %d (kind %d at %d Hz)\n", label,
                g_rigctl_fake_sent, g_rigctl_fake_last_kind, g_rigctl_fake_last_bw, g_rigctl_fake_unguarded, sent, kind,
                bandwidth);
    return 1;
}

/* The fake rigctl peer runs @p kind at @p bandwidth, as the client knows. */
static int
expect_rigctl_runs(const char* label, int kind, int bandwidth) {
    return expect_true(label, g_rigctl_fake_kind == kind && g_rigctl_fake_bw == bandwidth && g_rigctl_fake_known);
}

static int
submit_peer_am_width(dsd_opts* opts, dsd_state* state, int32_t hz, const char* label) {
    state->ui_msg[0] = '\0';
    int rc = expect_int(label, post_i32(DSD_APP_CMD_AM_BANDWIDTH_SET, hz), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

static int
submit_rigctl_setmod_bw(dsd_opts* opts, dsd_state* state, int32_t hz, const char* label) {
    state->ui_msg[0] = '\0';
    int rc = expect_int(label, post_i32(DSD_APP_CMD_RIGCTL_SET_MOD_BW, hz), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/*
 * Issue #621: an AM width edit under an am scan row on air, on audio input with a rigctl peer. The peer demodulates,
 * so the width is the passband it is asked for, and the edit asks for it at once (before, nothing asked until the next
 * retune). A refusal puts the width back and fails the command, and the peer is asked again for the passband the
 * session then runs: a lost reply may have left it on the refused one.
 */
static int
test_am_width_asks_the_rigctl_peer(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state);
    arm_rigctl_fake(&opts, DSD_ANALOG_DEMOD_AM, DSD_ANALOG_AM_WIDTH_DEFAULT_HZ);
    rc |= expect_int("rigctl am row: on air", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_AM), 0);
    rc |= expect_int("rigctl am row: no width", dsd_scan_mode_options(&opts, &state, NULL), 0);

    rc |= submit_peer_am_width(&opts, &state, 8000, "rigctl am 8000");
    rc |= expect_int("rigctl am 8000 stored", opts.analog_am_bandwidth_hz, 8000);
    rc |= expect_rigctl_sent("rigctl am 8000 asked", 1, DSD_ANALOG_DEMOD_AM, 8000);
    rc |= expect_rigctl_runs("rigctl am 8000 runs", DSD_ANALOG_DEMOD_AM, 8000);
    rc |= expect_toast("rigctl am 8000 toast", &state, "Applied: AM passband -> 8 kHz");
    rc |= expect_int("rigctl am 8000 completed", dsd_app_command_test_last_failed(), 0);

    /* Refused: the width goes back, and the peer, which kept 8 kHz, is asked for it again (nothing to send). */
    script_rigctl_fake(RIGCTL_FAKE_REFUSE, RIGCTL_FAKE_TAKE);
    rc |= submit_peer_am_width(&opts, &state, 10000, "rigctl am 10000 refused");
    rc |= expect_int("rigctl am 10000 refused: width back", opts.analog_am_bandwidth_hz, 8000);
    rc |= expect_int("rigctl am 10000 refused: configured back",
                     dsd_scan_mode_configured_view(&state)->analog_am_bandwidth_hz, 8000);
    rc |= expect_rigctl_sent("rigctl am 10000 refused: asked once", 1, DSD_ANALOG_DEMOD_AM, 10000);
    rc |= expect_int("rigctl am 10000 refused: re-asked", g_rigctl_fake_calls, 2);
    rc |= expect_rigctl_runs("rigctl am 10000 refused: peer kept 8 kHz", DSD_ANALOG_DEMOD_AM, 8000);
    rc |= expect_toast("rigctl am 10000 refused: toast", &state,
                       "Refused: AM bandwidth -> 10 kHz: the rigctl peer refused the passband");
    rc |= expect_int("rigctl am 10000 refused: failed", dsd_app_command_test_last_failed(), 1);

    /* A lost reply may have left the peer on 10 kHz: the re-ask puts it back on 8 kHz. */
    script_rigctl_fake(RIGCTL_FAKE_LOSE, RIGCTL_FAKE_TAKE);
    rc |= submit_peer_am_width(&opts, &state, 10000, "rigctl am 10000 lost");
    rc |= expect_int("rigctl am 10000 lost: width back", opts.analog_am_bandwidth_hz, 8000);
    rc |= expect_rigctl_sent("rigctl am 10000 lost: re-asked", 2, DSD_ANALOG_DEMOD_AM, 8000);
    rc |= expect_rigctl_runs("rigctl am 10000 lost: peer back on 8 kHz", DSD_ANALOG_DEMOD_AM, 8000);
    rc |= expect_toast("rigctl am 10000 lost: toast", &state,
                       "Refused: AM bandwidth -> 10 kHz: the rigctl peer refused the passband");

    /* An NFM edit under the am row is the setting alone: nothing runs it, nothing is asked. */
    script_rigctl_fake(RIGCTL_FAKE_TAKE, RIGCTL_FAKE_TAKE);
    rc |= submit_nfm_width(&opts, &state, 12500, "rigctl am row: nfm edit");
    rc |= expect_int("rigctl am row: nfm edit stored", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_rigctl_sent("rigctl am row: nfm edit asks nothing", 0, 0, 0);
    rc |= expect_toast("rigctl am row: nfm edit toast", &state, "Applied: NFM bandwidth -> 12.5 kHz");

    dsd_scan_mode_leave(&opts, &state);
    disarm_rigctl_fake(&opts);
    opts.analog_am_bandwidth_hz = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    freeState(&state);
    return rc;
}

/*
 * Issue #621: the NFM width and -B on the FM monitor (-fA, no scan) with a rigctl peer. The configured NFM width is the
 * passband the peer is asked for, and -B stands in while it is unset: a clear asks for -B, and a -B edit asks for it
 * while it stands in. A refused -B goes back. Off the monitor (a P25 session) -B is stored only, for the next tune.
 */
static int
test_nfm_width_and_setmod_bw_ask_the_rigctl_peer(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_ANALOG, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state);
    opts.setmod_bw = 12500;
    arm_rigctl_fake(&opts, DSD_ANALOG_DEMOD_FM, 12500);

    rc |= submit_nfm_width(&opts, &state, 20000, "rigctl nfm 20000");
    rc |= expect_rigctl_sent("rigctl nfm 20000 asked", 1, DSD_ANALOG_DEMOD_FM, 20000);
    rc |= expect_toast("rigctl nfm 20000 toast", &state, "Applied: NFM passband -> 20 kHz");
    rc |= submit_nfm_width(&opts, &state, 0, "rigctl nfm clear");
    rc |= expect_int("rigctl nfm clear stored", opts.analog_nfm_bandwidth_hz, 0);
    rc |= expect_rigctl_sent("rigctl nfm clear asks -B", 2, DSD_ANALOG_DEMOD_FM, 12500);
    rc |= expect_toast("rigctl nfm clear toast", &state, "Applied: NFM passband -> 12.5 kHz (-B)");

    /* -B standing in for the unset width is asked for at once. */
    rc |= submit_rigctl_setmod_bw(&opts, &state, 20000, "rigctl -B 20000");
    rc |= expect_int("rigctl -B 20000 stored", opts.setmod_bw, 20000);
    rc |= expect_rigctl_sent("rigctl -B 20000 asked", 3, DSD_ANALOG_DEMOD_FM, 20000);
    rc |= expect_toast("rigctl -B 20000 toast", &state, "Applied: Rigctl setmod BW -> 20000 Hz");
    /* Refused: -B goes back, and the session's passband is asked again; the peer, which kept 20 kHz, is sent nothing
       for it. */
    script_rigctl_fake(RIGCTL_FAKE_REFUSE, RIGCTL_FAKE_TAKE);
    rc |= submit_rigctl_setmod_bw(&opts, &state, 25000, "rigctl -B 25000 refused");
    rc |= expect_int("rigctl -B 25000 refused: -B back", opts.setmod_bw, 20000);
    rc |= expect_rigctl_sent("rigctl -B 25000 refused: asked once", 1, DSD_ANALOG_DEMOD_FM, 25000);
    rc |= expect_int("rigctl -B 25000 refused: re-asked", g_rigctl_fake_calls, 2);
    rc |= expect_rigctl_runs("rigctl -B 25000 refused: peer kept -B", DSD_ANALOG_DEMOD_FM, 20000);
    rc |= expect_toast("rigctl -B 25000 refused: toast", &state,
                       "Refused: Rigctl setmod BW -> 25000 Hz: the rigctl peer refused the passband");
    rc |= expect_int("rigctl -B 25000 refused: failed", dsd_app_command_test_last_failed(), 1);
    /* A lost reply may have left the peer on 25 kHz: the re-ask puts it back on -B. */
    script_rigctl_fake(RIGCTL_FAKE_LOSE, RIGCTL_FAKE_TAKE);
    rc |= submit_rigctl_setmod_bw(&opts, &state, 25000, "rigctl -B 25000 lost");
    rc |= expect_int("rigctl -B 25000 lost: -B back", opts.setmod_bw, 20000);
    rc |= expect_rigctl_sent("rigctl -B 25000 lost: re-asked", 2, DSD_ANALOG_DEMOD_FM, 20000);
    rc |= expect_rigctl_runs("rigctl -B 25000 lost: peer back on -B", DSD_ANALOG_DEMOD_FM, 20000);
    rc |= expect_toast("rigctl -B 25000 lost: toast", &state,
                       "Refused: Rigctl setmod BW -> 25000 Hz: the rigctl peer refused the passband");
    rc |= expect_int("rigctl -B 25000 lost: failed", dsd_app_command_test_last_failed(), 1);

    /* With a configured width -B is not what the monitor asks for: stored only. */
    rc |= submit_nfm_width(&opts, &state, 16000, "rigctl nfm 16000");
    script_rigctl_fake(RIGCTL_FAKE_TAKE, RIGCTL_FAKE_TAKE);
    rc |= submit_rigctl_setmod_bw(&opts, &state, 11000, "rigctl -B under a width");
    rc |= expect_int("rigctl -B under a width stored", opts.setmod_bw, 11000);
    rc |= expect_rigctl_sent("rigctl -B under a width asks nothing", 0, 0, 0);
    rc |= expect_toast("rigctl -B under a width toast", &state, "Applied: Rigctl setmod BW -> 11000 Hz");

    /* A P25 session runs no monitor the peer demodulates: -B waits for the next tune. */
    (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_P25P1, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state);
    opts.analog_nfm_bandwidth_hz = 0;
    rc |= submit_rigctl_setmod_bw(&opts, &state, 9000, "rigctl -B on p25");
    rc |= expect_int("rigctl -B on p25 stored", opts.setmod_bw, 9000);
    rc |= expect_rigctl_sent("rigctl -B on p25 asks nothing", 0, 0, 0);
    rc |= expect_toast("rigctl -B on p25 toast", &state, "Applied: Rigctl setmod BW -> 9000 Hz");

    /* Back onto the monitor with a decode-mode switch: the follow after the command asks for -B. A refusal there
       leaves the switch standing and says so. */
    script_rigctl_fake(RIGCTL_FAKE_REFUSE, RIGCTL_FAKE_TAKE);
    state.ui_msg[0] = '\0';
    rc |= expect_int("rigctl analog switch queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("rigctl analog switch drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("rigctl analog switch stands", opts.analog_only, 1);
    rc |= expect_rigctl_sent("rigctl analog switch asked -B", 1, DSD_ANALOG_DEMOD_FM, 9000);
    rc |= expect_toast("rigctl analog switch toast", &state, "Rigctl peer refused NFM 9 kHz (see log)");
    rc |= expect_int("rigctl analog switch completed", dsd_app_command_test_last_failed(), 0);

    disarm_rigctl_fake(&opts);
    opts.setmod_bw = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    freeState(&state);
    return rc;
}

/*
 * Issue #621: a rigctl peer a command brings, where none demodulated the input before -- the first connect of the '9'
 * key, a switch from a radio input onto TCP audio -- is asked for a width only, as the start and a reconnect ask it. On
 * -fA with -B and no NFM width nothing goes out, as before #621 (a tune asks for -B), and the command's own toast
 * stands; with a configured NFM width that width goes out, once.
 */
static int
test_a_new_rigctl_peer_is_asked_for_a_width_only(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    for (int width = 0; width < 2; width++) {
        const int sends = width ? 1 : 0;
        char tag[96];
        init_decode_mode_context(&opts, &state);
        (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_ANALOG, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state);
        opts.setmod_bw = 12500;
        opts.analog_nfm_bandwidth_hz = width ? 20000 : 0;
        /* The '9' key with no rigctl connection before. */
        arm_rigctl_fake(&opts, DSD_ANALOG_DEMOD_FM, 0);
        opts.use_rigctl = 0;
        opts.rigctl_sockfd = DSD_INVALID_SOCKET;
        DSD_SNPRINTF(opts.tcp_hostname, sizeof opts.tcp_hostname, "%s", "sdr.example");
        opts.rigctlportno = 4532;
        arm_rigctl_connect_stub(1, 0);
        DSD_SNPRINTF(tag, sizeof tag, "new peer on connect (nfm %d)", opts.analog_nfm_bandwidth_hz);
        state.ui_msg[0] = '\0';
        rc |= expect_int(tag, post_empty(DSD_APP_CMD_RIGCTL_CONNECT), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(tag, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(tag, opts.use_rigctl == 1 && g_rigctl_connect_calls == 1, 1);
        rc |= expect_rigctl_sent(tag, sends, DSD_ANALOG_DEMOD_FM, 20000);
        rc |= expect_int(tag, g_rigctl_fake_calls, sends);
        rc |= expect_toast(tag, &state, "Rigctl connected: sdr.example:4532");
        rc |= expect_int(tag, dsd_app_command_test_last_failed(), 0);
        arm_rigctl_connect_stub(0, 0);

        /* A switch from an RTL input, whose I/Q DSD-neo demodulates while the peer follows the frequency, onto TCP. */
        arm_rigctl_fake(&opts, DSD_ANALOG_DEMOD_FM, 0);
        opts.audio_in_type = AUDIO_IN_RTL;
        arm_tcp_connect_stub(1, DSD_AUDIO_INPUT_SWITCHED);
        DSD_SNPRINTF(tag, sizeof tag, "new peer on a tcp switch (nfm %d)", opts.analog_nfm_bandwidth_hz);
        rc |= expect_int(tag, post_host_port(DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG, "127.0.0.1", 7355),
                         DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(tag, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(tag, opts.audio_in_type, AUDIO_IN_TCP);
        rc |= expect_rigctl_sent(tag, sends, DSD_ANALOG_DEMOD_FM, 20000);
        rc |= expect_int(tag, g_rigctl_fake_calls, sends);
        arm_tcp_connect_stub(0, 0);
        disarm_rigctl_fake(&opts);
        freeState(&state);
    }
    opts.setmod_bw = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    return rc;
}

/*
 * Issue #621: a config apply that moves a DMR session with a rigctl peer on audio input onto the FM monitor asks the
 * peer for -B. A peer that refuses it leaves nothing to put back (the config set no width): the config stays applied,
 * the command fails, and the toast and the log say so.
 */
static int
test_config_mode_switch_refused_by_the_rigctl_peer(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state);
    opts.setmod_bw = 12500;
    arm_rigctl_fake(&opts, DSD_ANALOG_DEMOD_FM, 0);
    script_rigctl_fake(RIGCTL_FAKE_REFUSE, RIGCTL_FAKE_TAKE);
    g_rigctl_log[0] = '\0';
    state.ui_msg[0] = '\0';
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_ANALOG, "rigctl config analog refused");
    rc |= expect_int("rigctl config analog refused: the config stays", opts.analog_only, 1);
    rc |= expect_int("rigctl config analog refused: no width set", opts.analog_nfm_bandwidth_hz, 0);
    rc |= expect_rigctl_sent("rigctl config analog refused: asked -B", 1, DSD_ANALOG_DEMOD_FM, 12500);
    rc |= expect_int("rigctl config analog refused: nothing re-asked", g_rigctl_fake_calls, 1);
    rc |= expect_int("rigctl config analog refused: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_toast("rigctl config analog refused: toast", &state,
                       "Config applied; the rigctl peer refused NFM 12.5 kHz (see log)");
    rc |= expect_true("rigctl config analog refused: logged",
                      strstr(g_rigctl_log, "rigctl peer refused NFM 12.5 kHz after a config apply") != NULL);
    disarm_rigctl_fake(&opts);
    opts.setmod_bw = 0;
    freeState(&state);
    return rc;
}

/* Put the -fA session with NFM 20000 back on the monitor, the fake peer running that width as the client knows. */
static void
reset_monitor_on_rigctl_fake(dsd_opts* opts, dsd_state* state, int setmod_bw) {
    (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_ANALOG, DSD_DECODE_PRESET_PROFILE_CLI, opts, state);
    opts->analog_nfm_bandwidth_hz = 20000;
    opts->setmod_bw = setmod_bw;
    arm_rigctl_fake(opts, DSD_ANALOG_DEMOD_FM, 20000);
}

static int
submit_peer_decode_mode(dsd_opts* opts, dsd_state* state, dsdneoUserDecodeMode mode, const char* label) {
    state->ui_msg[0] = '\0';
    int rc = expect_int(label, dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)mode),
                        DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/*
 * Issue #621 (review): leaving the FM monitor for a digital mode on audio input with a rigctl peer undoes the passband
 * the monitor set (here the configured 20 kHz) with what a digital mode asks for -- FM at -B, or at the peer's own
 * passband without -B -- rather than leave the peer on 20 kHz until another tune. Best-effort: a refusal keeps the
 * switch, a config apply's included, with a warning. A -B edit on the digital session still waits for a tune.
 */
static int
test_leaving_the_monitor_undoes_the_rigctl_passband(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    reset_monitor_on_rigctl_fake(&opts, &state, 12500);
    rc |= submit_peer_decode_mode(&opts, &state, DSDCFG_MODE_P25P1, "rigctl monitor -> p25");
    rc |= expect_int("rigctl monitor -> p25: on p25", opts.frame_p25p1 == 1 && opts.analog_only == 0, 1);
    rc |= expect_rigctl_sent("rigctl monitor -> p25: -B asked", 1, DSD_ANALOG_DEMOD_FM, 12500);
    rc |= expect_rigctl_runs("rigctl monitor -> p25: peer on -B", DSD_ANALOG_DEMOD_FM, 12500);
    rc |= expect_int("rigctl monitor -> p25: completed", dsd_app_command_test_last_failed(), 0);

    /* A -B edit on P25 is stored for the next tune. */
    rc |= submit_rigctl_setmod_bw(&opts, &state, 9000, "rigctl -B on p25 after the monitor");
    rc |= expect_rigctl_sent("rigctl -B on p25 after the monitor asks nothing", 1, DSD_ANALOG_DEMOD_FM, 12500);

    /* Without -B: back to the peer's own passband. */
    reset_monitor_on_rigctl_fake(&opts, &state, 0);
    rc |= submit_peer_decode_mode(&opts, &state, DSDCFG_MODE_P25P1, "rigctl monitor -> p25, no -B");
    rc |= expect_rigctl_sent("rigctl monitor -> p25, no -B: own passband asked", 1, DSD_ANALOG_DEMOD_FM, 0);

    /* Refused: the switch stands, with a warning, and nothing is asked again. */
    reset_monitor_on_rigctl_fake(&opts, &state, 12500);
    script_rigctl_fake(RIGCTL_FAKE_REFUSE, RIGCTL_FAKE_TAKE);
    g_rigctl_log[0] = '\0';
    rc |= submit_peer_decode_mode(&opts, &state, DSDCFG_MODE_P25P1, "rigctl monitor -> p25 refused");
    rc |= expect_int("rigctl monitor -> p25 refused: on p25", opts.frame_p25p1 == 1 && opts.analog_only == 0, 1);
    rc |= expect_int("rigctl monitor -> p25 refused: asked once", g_rigctl_fake_calls, 1);
    rc |= expect_toast("rigctl monitor -> p25 refused: toast", &state, "Rigctl peer refused NFM 12.5 kHz (see log)");
    rc |= expect_true("rigctl monitor -> p25 refused: logged",
                      strstr(g_rigctl_log, "rigctl peer refused NFM 12.5 kHz; the setting stands") != NULL);
    rc |= expect_int("rigctl monitor -> p25 refused: completed", dsd_app_command_test_last_failed(), 0);

    /* A config apply that leaves the monitor is not failed by the refusal, and puts nothing back. */
    reset_monitor_on_rigctl_fake(&opts, &state, 12500);
    script_rigctl_fake(RIGCTL_FAKE_REFUSE, RIGCTL_FAKE_TAKE);
    state.ui_msg[0] = '\0';
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_P25P1, "rigctl config monitor -> p25 refused");
    rc |= expect_int("rigctl config monitor -> p25 refused: on p25", opts.frame_p25p1 == 1 && opts.analog_only == 0, 1);
    rc |= expect_int("rigctl config monitor -> p25 refused: width kept", opts.analog_nfm_bandwidth_hz, 20000);
    rc |= expect_int("rigctl config monitor -> p25 refused: asked once", g_rigctl_fake_calls, 1);
    rc |= expect_toast("rigctl config monitor -> p25 refused: toast", &state,
                       "Rigctl peer refused NFM 12.5 kHz (see log)");
    rc |= expect_int("rigctl config monitor -> p25 refused: completed", dsd_app_command_test_last_failed(), 0);

    disarm_rigctl_fake(&opts);
    opts.setmod_bw = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    freeState(&state);
    return rc;
}

/*
 * Issue #621 (review): an explicit width or -B edit whose setting the request in force reads asks the peer whether or
 * not the request changed. After a refused switch onto the monitor the configured width the peer refused is unchanged,
 * and resubmitting it reaches the peer; once the peer runs it, a resubmit sends nothing, since the client's record of
 * the peer skips a confirmed match. The same holds for -B standing in for an unset NFM width.
 */
static int
test_a_width_retry_reaches_the_rigctl_peer(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state);
    opts.analog_nfm_bandwidth_hz = 20000;
    opts.setmod_bw = 0;
    arm_rigctl_fake(&opts, DSD_ANALOG_DEMOD_FM, 12500);
    script_rigctl_fake(RIGCTL_FAKE_REFUSE, RIGCTL_FAKE_TAKE);
    rc |= submit_peer_decode_mode(&opts, &state, DSDCFG_MODE_ANALOG, "rigctl retry: dmr -> analog refused");
    rc |= expect_int("rigctl retry: on the monitor", opts.analog_only, 1);
    rc |= expect_rigctl_sent("rigctl retry: width refused", 1, DSD_ANALOG_DEMOD_FM, 20000);
    rc |= expect_rigctl_runs("rigctl retry: peer kept 12.5 kHz", DSD_ANALOG_DEMOD_FM, 12500);
    rc |= expect_toast("rigctl retry: refusal toast", &state, "Rigctl peer refused NFM 20 kHz (see log)");

    script_rigctl_fake(RIGCTL_FAKE_TAKE, RIGCTL_FAKE_TAKE);
    rc |= submit_nfm_width(&opts, &state, 20000, "rigctl retry: the same width");
    rc |= expect_rigctl_sent("rigctl retry: the same width asked", 1, DSD_ANALOG_DEMOD_FM, 20000);
    rc |= expect_rigctl_runs("rigctl retry: peer on 20 kHz", DSD_ANALOG_DEMOD_FM, 20000);
    rc |= expect_toast("rigctl retry: the same width toast", &state, "Applied: NFM passband -> 20 kHz");
    rc |= expect_int("rigctl retry: the same width completed", dsd_app_command_test_last_failed(), 0);
    script_rigctl_fake(RIGCTL_FAKE_TAKE, RIGCTL_FAKE_TAKE);
    rc |= submit_nfm_width(&opts, &state, 20000, "rigctl retry: a confirmed match");
    rc |= expect_rigctl_sent("rigctl retry: a confirmed match sends nothing", 0, 0, 0);
    rc |= expect_toast("rigctl retry: a confirmed match toast", &state, "Applied: NFM passband -> 20 kHz");

    /* -B standing in for the unset width, refused on the way onto the monitor, then resubmitted. */
    (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state);
    opts.analog_nfm_bandwidth_hz = 0;
    opts.setmod_bw = 12500;
    arm_rigctl_fake(&opts, DSD_ANALOG_DEMOD_FM, 9000);
    script_rigctl_fake(RIGCTL_FAKE_REFUSE, RIGCTL_FAKE_TAKE);
    rc |= submit_peer_decode_mode(&opts, &state, DSDCFG_MODE_ANALOG, "rigctl -B retry: dmr -> analog refused");
    rc |= expect_rigctl_sent("rigctl -B retry: -B refused", 1, DSD_ANALOG_DEMOD_FM, 12500);
    rc |= expect_rigctl_runs("rigctl -B retry: peer kept 9 kHz", DSD_ANALOG_DEMOD_FM, 9000);
    script_rigctl_fake(RIGCTL_FAKE_TAKE, RIGCTL_FAKE_TAKE);
    rc |= submit_rigctl_setmod_bw(&opts, &state, 12500, "rigctl -B retry: the same -B");
    rc |= expect_rigctl_sent("rigctl -B retry: the same -B asked", 1, DSD_ANALOG_DEMOD_FM, 12500);
    rc |= expect_toast("rigctl -B retry: toast", &state, "Applied: Rigctl setmod BW -> 12500 Hz");
    script_rigctl_fake(RIGCTL_FAKE_TAKE, RIGCTL_FAKE_TAKE);
    rc |= submit_rigctl_setmod_bw(&opts, &state, 12500, "rigctl -B retry: a confirmed match");
    rc |= expect_rigctl_sent("rigctl -B retry: a confirmed match sends nothing", 0, 0, 0);

    /* A retry whose reply is lost leaves the peer on either passband: the rollback, back to the very same setting, asks
       the peer again, and the peer record (unknown after the lost reply) lets it out. The width first, then -B. */
    (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state);
    opts.analog_nfm_bandwidth_hz = 20000;
    opts.setmod_bw = 0;
    arm_rigctl_fake(&opts, DSD_ANALOG_DEMOD_FM, 12500);
    script_rigctl_fake(RIGCTL_FAKE_REFUSE, RIGCTL_FAKE_TAKE);
    rc |= submit_peer_decode_mode(&opts, &state, DSDCFG_MODE_ANALOG, "rigctl lost retry: dmr -> analog refused");
    script_rigctl_fake(RIGCTL_FAKE_LOSE, RIGCTL_FAKE_TAKE);
    rc |= submit_nfm_width(&opts, &state, 20000, "rigctl lost retry: the same width");
    rc |= expect_int("rigctl lost retry: width kept", opts.analog_nfm_bandwidth_hz, 20000);
    rc |= expect_rigctl_sent("rigctl lost retry: recovery asked", 2, DSD_ANALOG_DEMOD_FM, 20000);
    rc |= expect_rigctl_runs("rigctl lost retry: peer known again", DSD_ANALOG_DEMOD_FM, 20000);
    rc |= expect_toast("rigctl lost retry: toast", &state,
                       "Refused: NFM bandwidth -> 20 kHz: the rigctl peer refused the passband");
    rc |= expect_int("rigctl lost retry: failed", dsd_app_command_test_last_failed(), 1);

    (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state);
    opts.analog_nfm_bandwidth_hz = 0;
    opts.setmod_bw = 12500;
    arm_rigctl_fake(&opts, DSD_ANALOG_DEMOD_FM, 9000);
    script_rigctl_fake(RIGCTL_FAKE_REFUSE, RIGCTL_FAKE_TAKE);
    rc |= submit_peer_decode_mode(&opts, &state, DSDCFG_MODE_ANALOG, "rigctl lost -B retry: dmr -> analog refused");
    script_rigctl_fake(RIGCTL_FAKE_LOSE, RIGCTL_FAKE_TAKE);
    rc |= submit_rigctl_setmod_bw(&opts, &state, 12500, "rigctl lost -B retry: the same -B");
    rc |= expect_int("rigctl lost -B retry: -B kept", opts.setmod_bw, 12500);
    rc |= expect_rigctl_sent("rigctl lost -B retry: recovery asked", 2, DSD_ANALOG_DEMOD_FM, 12500);
    rc |= expect_rigctl_runs("rigctl lost -B retry: peer known again", DSD_ANALOG_DEMOD_FM, 12500);
    rc |= expect_toast("rigctl lost -B retry: toast", &state,
                       "Refused: Rigctl setmod BW -> 12500 Hz: the rigctl peer refused the passband");
    rc |= expect_int("rigctl lost -B retry: failed", dsd_app_command_test_last_failed(), 1);

    disarm_rigctl_fake(&opts);
    opts.setmod_bw = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    freeState(&state);
    return rc;
}

static int
submit_config_am_width(dsd_opts* opts, dsd_state* state, int width_hz, const char* label) {
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_analog = 1;
    cfg.analog_am_bandwidth_hz = width_hz;
    state->ui_msg[0] = '\0';
    int rc = expect_int(label, dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof cfg),
                        DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/*
 * Issue #621: a config apply that changes the AM width under an am row on air, on audio input with a rigctl peer, asks
 * the peer for it after the scope resumes, under the P25 SM tick guard the config apply holds. A refusal or a lost
 * reply puts the width back, asks again for the passband the session then runs (the peer may run the refused one), and
 * fails the command, with the rest of the config applied.
 */
static int
test_config_apply_asks_the_rigctl_peer(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state);
    arm_rigctl_fake(&opts, DSD_ANALOG_DEMOD_AM, DSD_ANALOG_AM_WIDTH_DEFAULT_HZ);
    rc |= expect_int("rigctl config: am row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_AM), 0);
    rc |= expect_int("rigctl config: no width", dsd_scan_mode_options(&opts, &state, NULL), 0);

    rc |= submit_config_am_width(&opts, &state, 8000, "rigctl config am 8000");
    rc |= expect_int("rigctl config am 8000 in force", opts.analog_am_bandwidth_hz, 8000);
    rc |= expect_rigctl_sent("rigctl config am 8000 asked", 1, DSD_ANALOG_DEMOD_AM, 8000);
    rc |= expect_int("rigctl config am 8000 completed", dsd_app_command_test_last_failed(), 0);

    script_rigctl_fake(RIGCTL_FAKE_REFUSE, RIGCTL_FAKE_TAKE);
    g_rigctl_log[0] = '\0';
    rc |= submit_config_am_width(&opts, &state, 10000, "rigctl config am 10000 refused");
    rc |= expect_int("rigctl config refused: width back", opts.analog_am_bandwidth_hz, 8000);
    rc |= expect_int("rigctl config refused: configured back",
                     dsd_scan_mode_configured_view(&state)->analog_am_bandwidth_hz, 8000);
    rc |= expect_rigctl_sent("rigctl config refused: asked once", 1, DSD_ANALOG_DEMOD_AM, 10000);
    rc |= expect_int("rigctl config refused: re-asked", g_rigctl_fake_calls, 2);
    rc |= expect_rigctl_runs("rigctl config refused: peer kept 8 kHz", DSD_ANALOG_DEMOD_AM, 8000);
    rc |= expect_int("rigctl config refused: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_toast("rigctl config refused: toast", &state,
                       "Config applied; the rigctl peer refused AM 10 kHz: the AM width stays 8 kHz");
    rc |= expect_true(
        "rigctl config refused: logged",
        strstr(g_rigctl_log, "rigctl peer refused AM 10 kHz after a config apply; the AM width stays 8 kHz") != NULL);

    script_rigctl_fake(RIGCTL_FAKE_LOSE, RIGCTL_FAKE_TAKE);
    rc |= submit_config_am_width(&opts, &state, 10000, "rigctl config am 10000 lost");
    rc |= expect_int("rigctl config lost: width back", opts.analog_am_bandwidth_hz, 8000);
    rc |= expect_rigctl_sent("rigctl config lost: re-asked", 2, DSD_ANALOG_DEMOD_AM, 8000);
    rc |= expect_rigctl_runs("rigctl config lost: peer back on 8 kHz", DSD_ANALOG_DEMOD_AM, 8000);
    rc |= expect_int("rigctl config lost: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_toast("rigctl config lost: toast", &state,
                       "Config applied; the rigctl peer refused AM 10 kHz: the AM width stays 8 kHz");

    dsd_scan_mode_leave(&opts, &state);
    disarm_rigctl_fake(&opts);
    opts.analog_am_bandwidth_hz = 0;
    freeState(&state);
    return rc;
}
#endif

static int
submit_pcm_squelch(dsd_opts* opts, dsd_state* state, int mode, int margin_db, double level) {
    dsd_app_squelch_setting_payload payload;
    DSD_MEMSET(&payload, 0, sizeof payload);
    payload.mode = mode;
    payload.margin_db = margin_db;
    payload.level = level;
    state->ui_msg[0] = '\0';
    int rc = expect_true("pcm squelch queued",
                         dsd_app_command_submit(DSD_APP_CMD_RTL_SET_SQL_SETTING, &payload, sizeof payload) > 0);
    rc |= expect_int("pcm squelch drained", dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/*
 * The squelch commands on audio input (issue #628), in every build: radio-off builds dispatch them too. A level and the
 * noise squelch are stored with their toasts; the auto squelch, which has no channel power to learn a floor from on
 * audio input, is refused and says why; the AM monitor on its own still refuses noise.
 */
static int
test_squelch_commands_on_pcm_input(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_UDP;
    opts.analog_only = 1;
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;

    state.ui_msg[0] = '\0';
    rc |= expect_true("pcm level queued", dsd_app_command_set_double(DSD_APP_CMD_RTL_SET_SQL_DB, -50.0) > 0);
    rc |= expect_int("pcm level drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("pcm level stored",
                      opts.rtl_squelch_mode == DSD_SQUELCH_MODE_LEVEL
                          && fabs(opts.rtl_squelch_level - dsd_squelch_level_from_sql(-50.0)) <= 1e-12);
    rc |= expect_str("pcm level toast", state.ui_msg, "Applied: squelch -> -50.0 dB");

    rc |= submit_pcm_squelch(&opts, &state, DSD_SQUELCH_MODE_NOISE, 12, 0.0);
    rc |= expect_true("pcm noise stored",
                      opts.rtl_squelch_mode == DSD_SQUELCH_MODE_NOISE && opts.rtl_squelch_margin_db == 12);
    rc |= expect_str("pcm noise toast", state.ui_msg, "Applied: squelch -> noise +12 dB");

    rc |= submit_pcm_squelch(&opts, &state, DSD_SQUELCH_MODE_AUTO, 6, 0.0);
    rc |= expect_true("pcm auto refused",
                      opts.rtl_squelch_mode == DSD_SQUELCH_MODE_NOISE && opts.rtl_squelch_margin_db == 12);
    rc |= expect_str("pcm auto toast", state.ui_msg,
                     "Refused: the auto squelch needs a radio input; audio input takes a level or noise");

    state.ui_msg[0] = '\0';
    rc |= expect_true("pcm off queued", dsd_app_command_set_double(DSD_APP_CMD_RTL_SET_SQL_DB, 0.0) > 0);
    rc |= expect_int("pcm off drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |=
        expect_true("pcm off stored", opts.rtl_squelch_mode == DSD_SQUELCH_MODE_LEVEL && opts.rtl_squelch_level <= 0.0);
    rc |= expect_str("pcm off toast", state.ui_msg, "Applied: squelch -> off");

    opts.analog_demod = DSD_ANALOG_DEMOD_AM;
    rc |= submit_pcm_squelch(&opts, &state, DSD_SQUELCH_MODE_NOISE, 10, 0.0);
    rc |= expect_true("pcm noise on AM refused", opts.rtl_squelch_mode == DSD_SQUELCH_MODE_LEVEL);
    rc |= expect_str("pcm noise on AM toast", state.ui_msg,
                     "Refused: the noise squelch needs an FM channel; AM audio takes a level");

    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    opts.analog_only = 0;
    freeState(&state);
    return rc;
}

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_ANALOG_WRAP) && defined(DSD_NEO_TEST_AUDIO_ENSURE_WRAP)
/* The RTL receive-family requests a decode-mode change makes, recorded instead of run. */
static int g_rx_sequence;
static int g_analog_req_calls;
static int g_analog_req_family;
static int g_analog_req_kind;
static int g_analog_req_width_hz;
static int g_analog_req_order;
static int g_demod_req_calls;
static int g_demod_req_order;
static int g_demod_req_rate;
static int g_demod_req_ted_sps;
/* Whether the last family request was the marked digital landing (rtl_stream_request_digital_family_landing(), issue
   #583), and the cqpsk_explicit it carried (-1: a plain request). */
static int g_analog_req_landing;
static int g_analog_req_landing_explicit;
/* What rtl_stream_analog_family_active() reports: the front end runs the analog family (its monitor output, or a
   symbol profile a CQPSK toggle or typed row applied under it). */
static int g_fake_analog_family;
/* Outstanding work that lands a receive family on a front end that runs the digital family (issue #583): a retune that
   carries the digital family, which rtl_stream_family_landing_after_pending() counts beside the live analog family. */
static int g_fake_family_landing_outstanding;
static unsigned int g_fake_digital_rate;
/* The output rate a switch out of the analog family lands the FSK discriminator on
   (rtl_stream_output_rate_for_family()), which a SoapySDR or Airspy device's demod rate is resampled to. 0 reports
   g_fake_digital_rate for either modulation, as the fake stream otherwise tells them apart nowhere. */
static unsigned int g_fake_digital_fsk_rate;
/* DSD_NEO_CQPSK as the stream applies it to an open and to a switch out of the analog family: -1 unset, else the CQPSK
   state it forces. */
static int g_fake_cqpsk_env = -1;
/* What rtl_stream_check_analog_profile() answers (0 accepts), and what it was asked. */
static int g_analog_check_result;
static int g_analog_check_calls;
static int g_analog_check_family;
static int g_analog_check_kind;
static int g_analog_check_width_hz;
/* The CQPSK state the stream publishes: 1 while CQPSK runs (under -fA, the DSP menu's toggle holding the front end off
   the monitor). */
static int g_fake_cqpsk;
/* The fake stream's receive request numbers (rtl_stream_receive_request_seq(), rtl_stream_receive_request_outcome()):
   the last one queued, the last one the demod thread has taken (demod_thread_lands(): the published CQPSK state above
   is the stream's from then), the last analog request queued, and an analog request refused where it landed. */
static uint32_t g_fake_rx_seq;
static uint32_t g_fake_rx_settled;
static uint32_t g_fake_rx_analog_seq;
static uint32_t g_fake_rx_refused;
/* Requests dropped with an analog request the demod thread never took (RTL_STREAM_RX_REQUEST_REPLACED): the first of
   the chain of analog requests the queued one replaced in turn, and the last run the stream dropped, first to last
   (0: none), as a later analog request replacing the queued one, or a retune retiring it, leaves them. */
static uint32_t g_fake_rx_analog_first;
static uint32_t g_fake_rx_replaced_first;
static uint32_t g_fake_rx_replaced_last;
/* What the stream kept when it refused (rtl_stream_receive_request_refusal()): its family, the analog width and the
   analog kind, and whether it kept the analog monitor output (0: the digital family, or a symbol profile applied under
   the analog family, a typed row's or CQPSK). */
static int g_fake_rx_kept_analog;
static int g_fake_rx_kept_width_hz;
static int g_fake_rx_kept_kind;
static int g_fake_rx_kept_monitor;
/* The widest analog width rtl_stream_request_analog_profile() queues (0: any): a front end a retune has moved to a rate
   that cannot filter a wider one refuses it at once. The kind's default (width 0) never trips it. */
static int g_fake_analog_req_max_hz;
/* The analog monitor the stream publishes (rtl_stream_get_analog_profile()): its kind (-1: none, as off the monitor),
   its effective width, and whether the channel filter sets that width. */
static int g_fake_monitor_kind = -1;
static int g_fake_monitor_width_hz;
static int g_fake_monitor_lpf_on;
/* What the stream runs on the analog family, on its monitor output or not (rtl_stream_get_analog_setting(), reported
   while g_fake_analog_family is set): the analog kind and the configured width (0: the kind's default) of the last
   analog request the demod thread took, or the ones it kept when it refused one where it landed. A typed digital row's
   symbol profile, or CQPSK, applied under the family leaves them as they were. */
static int g_fake_run_kind;
static int g_fake_run_width_hz;
/* The last analog request queued (its family, kind and width), which the demod thread taking it makes what the stream
   runs. */
static int g_fake_rx_analog_family;
static int g_fake_rx_analog_kind;
static int g_fake_rx_analog_width_hz;
/* The CQPSK state the requests queued since the last landing leave the stream on (rtl_stream_requested_cqpsk()). */
static int g_fake_cqpsk_after;
/* What rtl_stream_request_analog_profile() answers (0 queues it): -1 is a front end that refuses the request at the
   rate it publishes now, although rtl_stream_check_analog_profile() took it (a retune in between). */
static int g_analog_req_result;
/* When the demod thread takes a request, at points the decoder's drain cannot see (one-shot, cleared once used). The
   next analog request queued is taken as soon as it is queued, before the drain's next command: 1; and a retune then
   moves the rate so that the front end refuses every request after it at once (g_analog_req_result): 2. */
static int g_fake_take_next_analog_at_once;
/* ... everything queued so far is taken just before the next analog request is queued, while the command making it
   runs, after the drain has asked what became of the last one. */
static int g_fake_take_before_next_analog;
/* What rtl_stream_get_demod_rate_hz() reports: the demod rate the stream publishes. */
static int g_fake_demod_rate_hz;
/* What rtl_stream_get_request_rate_hz() reports: the rate a stream publishes from its start on, which a request is held
   to before the demod thread has run a block. */
static int g_fake_request_rate_hz;
/* What rtl_stream_output_rate() reports: the rate the decoder reads and times symbols for. 0, as the real one answers
   for the tests' fake context, has the decoder time for the input's own rate. */
static uint32_t g_fake_output_rate_hz;
/* The CQPSK state of the last demod profile requested. */
static int g_demod_req_cqpsk;
/* The channel filter the last demod profile request named (dsd_rtl_stream_channel_profile). */
static int g_demod_req_chan;
/* The digital decode modes the decoder notes with the front end (rtl_stream_set_digital_decode_modes()). */
static int g_modes_note_calls;
static int g_modes_note_order;
static int g_modes_note_dmr;
static int g_modes_note_nxdn48;

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol name.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
int __wrap_rtl_stream_check_analog_profile(int family, int kind, int width_hz);
int __wrap_rtl_stream_request_analog_profile(int family, int kind, int width_hz);
int __wrap_rtl_stream_request_demod_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile,
                                            int ted_sps, int ted_sps_is_override);
int __wrap_rtl_stream_analog_family_active(void);
int __wrap_rtl_stream_family_landing_after_pending(void);
int __wrap_rtl_stream_request_digital_family_landing(int cqpsk_explicit);
unsigned int __wrap_rtl_stream_output_rate_for_family(int family, int cqpsk_enable, int symbol_rate_hz,
                                                      int cqpsk_explicit);
void __wrap_rtl_stream_set_digital_decode_modes(const dsd_opts* opts);
uint32_t __wrap_rtl_stream_receive_request_seq(void);
int __wrap_rtl_stream_receive_request_outcome(uint32_t seq);
int __wrap_rtl_stream_receive_request_refusal(uint32_t seq, int* out_analog_family, int* out_width_hz, int* out_kind,
                                              int* out_monitor);
int __wrap_rtl_stream_get_analog_profile(int* out_kind, int* out_width_hz, int* out_lpf_on);
int __wrap_rtl_stream_get_analog_setting(int* out_kind, int* out_width_hz);
int __wrap_rtl_stream_requested_cqpsk(void);
int __wrap_rtl_stream_get_demod_rate_hz(void);
int __wrap_rtl_stream_get_request_rate_hz(void);
uint32_t __wrap_rtl_stream_output_rate(const RtlSdrContext* ctx);

uint32_t
__wrap_rtl_stream_receive_request_seq(void) {
    return g_fake_rx_seq;
}

int
__wrap_rtl_stream_receive_request_outcome(uint32_t seq) {
    if (g_fake_rx_settled - seq > 0x7FFFFFFFU) {
        return RTL_STREAM_RX_REQUEST_PENDING;
    }
    if (seq != 0U && seq == g_fake_rx_refused) {
        return RTL_STREAM_RX_REQUEST_REFUSED;
    }
    if (seq != 0U && g_fake_rx_replaced_first != 0U
        && seq - g_fake_rx_replaced_first <= g_fake_rx_replaced_last - g_fake_rx_replaced_first) {
        return RTL_STREAM_RX_REQUEST_REPLACED;
    }
    return RTL_STREAM_RX_REQUEST_SETTLED;
}

/* Whether an analog request is still queued: the last one, not taken yet. */
static int
fake_rx_analog_queued(void) {
    return g_fake_rx_analog_seq != 0U
           && __wrap_rtl_stream_receive_request_outcome(g_fake_rx_analog_seq) == RTL_STREAM_RX_REQUEST_PENDING;
}

/* The queued analog request, the ones it replaced in turn and every request through @p last_seq are dropped without the
   demod thread taking any of them. */
static void
fake_rx_drop_queued_analog(uint32_t last_seq) {
    g_fake_rx_replaced_first = g_fake_rx_analog_first;
    g_fake_rx_replaced_last = last_seq;
}

/* The demod thread takes everything queued so far: an analog request still queued is what the stream runs from here. */
static void
fake_rx_take_queued(void) {
    if (fake_rx_analog_queued() && g_fake_rx_analog_family == DSD_RX_FAMILY_ANALOG) {
        g_fake_run_kind = g_fake_rx_analog_kind;
        g_fake_run_width_hz = g_fake_rx_analog_width_hz;
    }
    g_fake_rx_settled = g_fake_rx_seq;
}

int
__wrap_rtl_stream_receive_request_refusal(uint32_t seq, int* out_analog_family, int* out_width_hz, int* out_kind,
                                          int* out_monitor) {
    if (__wrap_rtl_stream_receive_request_outcome(seq) != RTL_STREAM_RX_REQUEST_REFUSED) {
        return 0;
    }
    if (out_monitor) {
        *out_monitor = g_fake_rx_kept_monitor;
    }
    if (out_analog_family) {
        *out_analog_family = g_fake_rx_kept_analog;
    }
    if (out_width_hz) {
        *out_width_hz = g_fake_rx_kept_width_hz;
    }
    if (out_kind) {
        *out_kind = g_fake_rx_kept_kind;
    }
    return 1;
}

int
__wrap_rtl_stream_get_analog_profile(int* out_kind, int* out_width_hz, int* out_lpf_on) {
    if (g_fake_monitor_kind < 0) {
        return 0;
    }
    if (out_kind) {
        *out_kind = g_fake_monitor_kind;
    }
    if (out_width_hz) {
        *out_width_hz = g_fake_monitor_width_hz;
    }
    if (out_lpf_on) {
        *out_lpf_on = g_fake_monitor_lpf_on;
    }
    return 1;
}

int
__wrap_rtl_stream_get_analog_setting(int* out_kind, int* out_width_hz) {
    if (out_kind) {
        *out_kind = g_fake_analog_family ? g_fake_run_kind : 0;
    }
    if (out_width_hz) {
        *out_width_hz = g_fake_analog_family ? g_fake_run_width_hz : 0;
    }
    return g_fake_analog_family ? 1 : 0;
}

int
__wrap_rtl_stream_requested_cqpsk(void) {
    return (__wrap_rtl_stream_receive_request_outcome(g_fake_rx_seq) == RTL_STREAM_RX_REQUEST_PENDING)
               ? g_fake_cqpsk_after
               : g_fake_cqpsk;
}

int
__wrap_rtl_stream_get_demod_rate_hz(void) {
    return g_fake_demod_rate_hz;
}

int
__wrap_rtl_stream_get_request_rate_hz(void) {
    return g_fake_request_rate_hz;
}

uint32_t
__wrap_rtl_stream_output_rate(const RtlSdrContext* ctx) {
    return ctx ? g_fake_output_rate_hz : 0U;
}

int
__wrap_rtl_stream_check_analog_profile(int family, int kind, int width_hz) {
    g_analog_check_calls++;
    g_analog_check_family = family;
    g_analog_check_kind = kind;
    g_analog_check_width_hz = width_hz;
    return g_analog_check_result;
}

int
__wrap_rtl_stream_request_analog_profile(int family, int kind, int width_hz) {
    g_analog_req_calls++;
    g_analog_req_landing = 0;
    g_analog_req_landing_explicit = -1;
    g_analog_req_family = family;
    g_analog_req_kind = kind;
    g_analog_req_width_hz = width_hz;
    g_analog_req_order = ++g_rx_sequence;
    if (g_analog_req_result != 0) {
        return g_analog_req_result;
    }
    if (family == DSD_RX_FAMILY_ANALOG && g_fake_analog_req_max_hz > 0 && width_hz > g_fake_analog_req_max_hz) {
        return -1;
    }
    if (g_fake_take_before_next_analog) {
        g_fake_take_before_next_analog = 0;
        fake_rx_take_queued();
    }
    g_fake_cqpsk_after = (family == DSD_RX_FAMILY_ANALOG) ? 0 : __wrap_rtl_stream_requested_cqpsk();
    const int replaces_analog = fake_rx_analog_queued();
    if (replaces_analog) {
        fake_rx_drop_queued_analog(g_fake_rx_seq);
    }
    g_fake_rx_analog_seq = ++g_fake_rx_seq;
    if (!replaces_analog) {
        g_fake_rx_analog_first = g_fake_rx_analog_seq;
    }
    g_fake_rx_analog_family = family;
    g_fake_rx_analog_kind = kind;
    g_fake_rx_analog_width_hz = width_hz;
    if (g_fake_take_next_analog_at_once) {
        g_analog_req_result = (g_fake_take_next_analog_at_once == 2) ? -1 : 0;
        g_fake_take_next_analog_at_once = 0;
        fake_rx_take_queued();
    }
    return 0;
}

int
__wrap_rtl_stream_request_demod_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile,
                                        int ted_sps, int ted_sps_is_override) {
    (void)levels;
    (void)ted_sps_is_override;
    g_demod_req_calls++;
    g_demod_req_cqpsk = cqpsk_enable;
    g_demod_req_chan = channel_profile;
    g_demod_req_rate = symbol_rate_hz;
    g_demod_req_ted_sps = ted_sps;
    g_demod_req_order = ++g_rx_sequence;
    g_fake_cqpsk_after = (cqpsk_enable >= 0) ? (cqpsk_enable ? 1 : 0) : __wrap_rtl_stream_requested_cqpsk();
    ++g_fake_rx_seq;
    return 0;
}

int
__wrap_rtl_stream_analog_family_active(void) {
    return g_fake_analog_family;
}

/* A digital retune queued now lands on a family's landing: the analog family runs, or outstanding work lands one. */
int
__wrap_rtl_stream_family_landing_after_pending(void) {
    return (g_fake_analog_family || g_fake_family_landing_outstanding) ? 1 : 0;
}

/* The digital family request a republish marks to land the digital family's landing (issue #583): queued as the family
   request it is, and recorded as the marked one. */
int
__wrap_rtl_stream_request_digital_family_landing(int cqpsk_explicit) {
    const int rc = __wrap_rtl_stream_request_analog_profile(DSD_RX_FAMILY_DIGITAL, DSD_ANALOG_DEMOD_FM, 0);
    g_analog_req_landing = 1;
    g_analog_req_landing_explicit = cqpsk_explicit;
    return rc;
}

/* A switch out of the analog family lands on the CQPSK state an open of the mode would, DSD_NEO_CQPSK's when set,
 * unless it is a trunk-scan target's own choice (issue #583), which no command here asks for. */
unsigned int
__wrap_rtl_stream_output_rate_for_family(int family, int cqpsk_enable, int symbol_rate_hz, int cqpsk_explicit) {
    (void)family;
    (void)symbol_rate_hz;
    const int landing_cqpsk = (g_fake_cqpsk_env >= 0 && !cqpsk_explicit) ? g_fake_cqpsk_env : cqpsk_enable;
    return (landing_cqpsk <= 0 && g_fake_digital_fsk_rate > 0U) ? g_fake_digital_fsk_rate : g_fake_digital_rate;
}

void
__wrap_rtl_stream_set_digital_decode_modes(const dsd_opts* opts) {
    g_modes_note_calls++;
    g_modes_note_dmr = opts ? opts->frame_dmr : -1;
    g_modes_note_nxdn48 = opts ? opts->frame_nxdn48 : -1;
    g_modes_note_order = ++g_rx_sequence;
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

/* The demod thread has taken everything queued so far (or a restart dropped it): the stream publishes @p cqpsk from
   here. */
static void
demod_thread_lands(int cqpsk) {
    g_fake_cqpsk = cqpsk;
    fake_rx_take_queued();
}

/* The demod thread has taken everything queued so far, and refused the last analog request where it landed (a retune
   moved the demod rate after it was checked): the front end keeps the receive profile, and the CQPSK state, it had,
   which the stream records as the family it stayed on (@p analog_family) and the analog kind and width it runs, on
   the monitor output unless CQPSK holds it off, and runs from here. */
static void
demod_thread_refuses_analog_keeping_kind(int analog_family, int kind, int width_hz) {
    g_fake_rx_kept_analog = analog_family;
    g_fake_rx_kept_kind = kind;
    g_fake_rx_kept_width_hz = width_hz;
    g_fake_rx_kept_monitor = (analog_family && !g_fake_cqpsk) ? 1 : 0;
    g_fake_rx_refused = g_fake_rx_analog_seq;
    g_fake_rx_settled = g_fake_rx_seq;
    if (analog_family) {
        g_fake_run_kind = kind;
        g_fake_run_width_hz = width_hz;
    }
}

/* ... a scan leave's return to the monitor, which the front end refused keeping the family @p analog_family, the
   monitor output or not (@p monitor: 0 under a typed digital row's channel profile, whose width setting the row never
   touched), the kind @p kind and the width setting @p width_hz. */
static void
demod_thread_refuses_leave_keeping(int analog_family, int monitor, int kind, int width_hz) {
    demod_thread_refuses_analog_keeping_kind(analog_family, kind, width_hz);
    g_fake_rx_kept_monitor = monitor;
}

/* ... on the FM monitor, or on the digital family. */
static void
demod_thread_refuses_analog_keeping(int analog_family, int width_hz) {
    demod_thread_refuses_analog_keeping_kind(analog_family, DSD_ANALOG_DEMOD_FM, width_hz);
}

/* A scan row's retune lands its receive family before the demod thread has taken the requests still queued, and
   retires them (rtl_stream_retire_requests_before_family()): they settle without the demod thread running them, and an
   analog request among them reads replaced, with the requests dropped with it. */
static void
retune_retires_the_queued_requests(void) {
    if (fake_rx_analog_queued()) {
        fake_rx_drop_queued_analog(g_fake_rx_seq);
    }
    g_fake_rx_settled = g_fake_rx_seq;
}

/* The stream restarts: the open drops what the previous stream left queued and forgets a refusal, and the requests
   replaced, it recorded. */
static void
stream_reopens(int cqpsk) {
    g_fake_rx_refused = 0U;
    g_fake_rx_replaced_first = g_fake_rx_replaced_last = 0U;
    demod_thread_lands(cqpsk);
}

/* Each step starts on a stream that has taken the last step's requests (demod_thread_lands()), so what the step sets
   in g_fake_cqpsk is what the stream publishes. */
static void
reset_rx_family_wrap(void) {
    reset_config_rtl_wrap();
    demod_thread_lands(g_fake_cqpsk);
    g_fake_rx_replaced_first = g_fake_rx_replaced_last = 0U;
    g_analog_check_result = 0;
    g_analog_req_result = 0;
    g_fake_analog_req_max_hz = 0;
    g_fake_request_rate_hz = 0;
    g_fake_digital_fsk_rate = 0U;
    g_fake_cqpsk_env = -1;
    g_fake_family_landing_outstanding = 0;
    g_analog_req_landing = 0;
    g_analog_req_landing_explicit = -1;
    g_fake_monitor_kind = -1;
    g_fake_monitor_width_hz = g_fake_monitor_lpf_on = 0;
    g_fake_rx_kept_monitor = 0;
    g_fake_take_next_analog_at_once = g_fake_take_before_next_analog = 0;
    g_analog_check_calls = g_analog_check_family = g_analog_check_kind = g_analog_check_width_hz = 0;
    g_rx_sequence = 0;
    g_analog_req_calls = g_analog_req_family = g_analog_req_kind = g_analog_req_width_hz = g_analog_req_order = 0;
    g_demod_req_calls = g_demod_req_order = g_demod_req_rate = g_demod_req_ted_sps = g_demod_req_cqpsk = 0;
    g_demod_req_chan = -1;
    g_modes_note_calls = g_modes_note_order = g_modes_note_dmr = g_modes_note_nxdn48 = 0;
    g_ensure_analog_calls = g_ensure_digital_calls = 0;
}

/*
 * DECODE_MODE_SET Analog on a live digital RTL session has to move the front end
 * onto the analog family (it used to publish nothing for analog, leaving the RTL
 * stream on the old digital demodulator) and open the monitor's raw sink. Going
 * back to a digital mode asks for the digital family before its symbol profile,
 * and times the decoder for the rate the digital stream will run at -- not the
 * analog monitor's resampled rate it is still reading when the command runs.
 */
static int
test_decode_mode_set_switches_rtl_receive_family(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    rc |= expect_int("dmr start queued", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("dmr start drained", dsd_app_drain_cmds(&opts, &state), 1);

    reset_rx_family_wrap();
    rc |= expect_int("analog queued", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("analog drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("analog preset applied", opts.analog_only, 1);
    rc |= expect_int("analog profile published", g_analog_req_calls, 1);
    rc |= expect_int("analog family requested", g_analog_req_family, DSD_RX_FAMILY_ANALOG);
    rc |= expect_int("FM requested", g_analog_req_kind, DSD_ANALOG_DEMOD_FM);
    rc |= expect_int("default width travels as 0", g_analog_req_width_hz, 0);
    rc |= expect_int("no symbol profile for analog", g_demod_req_calls, 0);
    rc |= expect_int("analog notes no digital modes", g_modes_note_calls, 0);
    rc |= expect_int("raw sink ensured", g_ensure_analog_calls, 1);
    rc |= expect_int("digital sink not asked for", g_ensure_digital_calls, 0);

    /* Back to DMR while the front end still runs the analog monitor at a 24 kHz DSP rate. */
    reset_rx_family_wrap();
    g_fake_analog_family = 1;
    g_fake_digital_rate = 24000U;
    rc |= expect_int("dmr queued", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("dmr drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("family request made", g_analog_req_calls, 1);
    rc |= expect_int("digital family requested", g_analog_req_family, DSD_RX_FAMILY_DIGITAL);
    rc |= expect_int("symbol profile follows", g_demod_req_calls, 1);
    rc |= expect_int("family before profile", g_analog_req_order < g_demod_req_order, 1);
    /* The stream's options are the -fA session's, which name no digital mode: the decoder notes DMR's with it. */
    rc |= expect_int("dmr modes noted", g_modes_note_calls, 1);
    rc |= expect_int("noted modes are DMR's", g_modes_note_dmr, 1);
    rc |= expect_int("modes noted before the family request", g_modes_note_order < g_analog_req_order, 1);
    rc |= expect_int("decoder timed for the digital rate", state.samplesPerSymbol, 5);
    rc |= expect_int("front end timed for the digital rate", g_demod_req_ted_sps, 5);
    /* Timed for the switch's landing, so the request is the one that lands there (issue #583); no scan target makes the
       CQPSK choice its own, so DSD_NEO_CQPSK decides it as an open of the mode would. */
    rc |= expect_int("family request lands the prediction", g_analog_req_landing, 1);
    rc |= expect_int("the CQPSK state is no target's own", g_analog_req_landing_explicit, 0);
    rc |= expect_int("digital sink ensured", g_ensure_digital_calls, 1);
    rc |= expect_int("raw sink not asked for", g_ensure_analog_calls, 0);

    /* A configured width travels with the next analog request. */
    reset_rx_family_wrap();
    g_fake_analog_family = 0;
    opts.analog_nfm_bandwidth_hz = 12500;
    rc |= expect_int("analog again queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("analog again drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("configured width requested", g_analog_req_width_hz, 12500);

    g_fake_digital_rate = 0U;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * DECODE_MODE_SET Analog that the running RTL front end would refuse (an explicit NFM width its demod rate cannot
 * realize, say) is asked about before anything changes: the command fails with a toast, and the session stays in its
 * digital mode with the digital front end, sink and options it had. Committing the preset first would leave an Analog
 * decoder on the digital demodulator, and a second Analog pick would take the already-selected shortcut and never
 * retry. Once the front end takes the profile, the same command switches.
 */
static int
test_decode_mode_set_refused_analog_profile_changes_nothing(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    rc |= expect_int("refusal: dmr start queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("refusal: dmr start drained", dsd_app_drain_cmds(&opts, &state), 1);
    opts.analog_nfm_bandwidth_hz = 25000;
    opts.mod_cli_lock = 1;

    reset_rx_family_wrap();
    g_analog_check_result = -1;
    state.ui_msg[0] = '\0';
    rc |= expect_int("refusal: analog queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("refusal: analog drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("refusal: front end asked once", g_analog_check_calls, 1);
    rc |= expect_int("refusal: asked for the analog family", g_analog_check_family, DSD_RX_FAMILY_ANALOG);
    rc |= expect_int("refusal: asked for NFM", g_analog_check_kind, DSD_ANALOG_DEMOD_FM);
    rc |= expect_int("refusal: asked for the configured width", g_analog_check_width_hz, 25000);
    rc |= expect_int("refusal: decoder stays digital", opts.analog_only, 0);
    rc |= expect_int("refusal: DMR still decoded", opts.frame_dmr, 1);
    rc |= expect_int("refusal: mode still DMR", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_DMR, 1);
    rc |= expect_int("refusal: modulation lock kept", opts.mod_cli_lock, 1);
    rc |= expect_int("refusal: no family request", g_analog_req_calls, 0);
    rc |= expect_int("refusal: no symbol profile", g_demod_req_calls, 0);
    rc |= expect_int("refusal: no raw sink opened", g_ensure_analog_calls, 0);
    rc |= expect_int("refusal: toast names the refusal", strstr(state.ui_msg, "refused") != NULL, 1);

    /* The front end takes it now: the same pick switches, with no already-selected shortcut in the way. */
    reset_rx_family_wrap();
    rc |= expect_int("accepted: analog queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("accepted: analog drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("accepted: decoder on analog", opts.analog_only, 1);
    rc |= expect_int("accepted: analog family requested", g_analog_req_family, DSD_RX_FAMILY_ANALOG);
    rc |= expect_int("accepted: width travels", g_analog_req_width_hz, 25000);
    rc |= expect_int("accepted: raw sink ensured", g_ensure_analog_calls, 1);

    /* A digital pick is never asked about. */
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= expect_int("digital: dmr queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("digital: dmr drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("digital: front end not asked", g_analog_check_calls, 0);
    rc |= expect_int("digital: DMR applied", opts.analog_only == 0 && opts.frame_dmr == 1, 1);

    g_analog_check_result = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * A -fA session whose front end a CQPSK toggle has moved onto symbols: the analog profile is no longer published, but
 * the stream still runs the analog family, and picking a digital mode leaves it. The decoder asks for the digital
 * family before the mode's symbol profile and times itself for the rate the digital stream will run at, as it does
 * from the monitor output.
 */
static int
test_decode_mode_set_leaves_analog_family_off_the_monitor(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    rc |= expect_int("cqpsk -fA start queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("cqpsk -fA start drained", dsd_app_drain_cmds(&opts, &state), 1);

    reset_rx_family_wrap();
    g_fake_analog_family = 1;
    g_fake_digital_rate = 24000U;
    rc |= expect_int("cqpsk dmr queued", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("cqpsk dmr drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("cqpsk: digital family requested", g_analog_req_family, DSD_RX_FAMILY_DIGITAL);
    rc |= expect_int("cqpsk: family request made once", g_analog_req_calls, 1);
    rc |= expect_int("cqpsk: symbol profile follows", g_demod_req_calls, 1);
    rc |= expect_int("cqpsk: family before profile", g_analog_req_order < g_demod_req_order, 1);
    rc |= expect_int("cqpsk: decoder timed for the digital rate", state.samplesPerSymbol, 5);
    rc |= expect_int("cqpsk: front end timed for the digital rate", g_demod_req_ted_sps, 5);

    g_fake_analog_family = 0;
    g_fake_digital_rate = 0U;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * A typed digital scan row on an analog session runs its symbol profile on the analog family, as it always has. A
 * scoped command that republishes the row's profile asks for the digital family only when the configured mode is
 * digital: on the -fA baseline it queues the row's symbol profile alone, so the front end is not switched in the
 * middle of the row. On a digital baseline the same republish asks for the digital family first (a no-op on a
 * digital front end).
 */
static int
test_typed_row_republish_follows_configured_family(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    rc |= expect_int("row -fA baseline queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("row -fA baseline drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("typed DMR row on -fA", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR), 0);
    rc |= expect_int("typed DMR row options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    rc |= expect_int("row runs DMR", opts.frame_dmr, 1);

    reset_rx_family_wrap();
    g_fake_analog_family = 1;
    rc |= expect_int("row republish queued", dsd_app_command_action(DSD_APP_CMD_INV_DMR_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("row republish drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("-fA row: row profile republished", g_demod_req_calls, 1);
    rc |= expect_int("-fA row: no family request", g_analog_req_calls, 0);
    rc |= expect_int("-fA row: the row's modes are not noted", g_modes_note_calls, 0);
    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_int("-fA row left", opts.analog_only, 1);

    rc |= expect_int("row dmr baseline queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("row dmr baseline drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("typed NXDN row on DMR", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NXDN48), 0);
    rc |= expect_int("typed NXDN row options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    reset_rx_family_wrap();
    g_fake_analog_family = 0;
    rc |= expect_int("digital row republish queued", dsd_app_command_action(DSD_APP_CMD_INV_DMR_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("digital row republish drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("digital row: digital family requested", g_analog_req_family, DSD_RX_FAMILY_DIGITAL);
    rc |= expect_int("digital row: family request made once", g_analog_req_calls, 1);
    rc |= expect_int("digital row: row profile follows", g_demod_req_calls, 1);
    rc |= expect_int("digital row: family before profile", g_analog_req_order < g_demod_req_order, 1);
    /* Nothing outstanding lands a family (issue #583): the request is the plain one, a no-op on a digital front end. */
    rc |= expect_int("digital row: not a landing", g_analog_req_landing, 0);
    rc |= expect_int("digital row: the row's modes are not noted", g_modes_note_calls, 0);
    dsd_scan_mode_leave(&opts, &state);

    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * A digital mode picked while a typed scan row runs on a -fA session is the configured mode, noted with the front end
 * for the switch the row's leave makes: the decoder notes the options it applies the preset to (the configuration,
 * before the row's constraint is reapplied), not the row's, and republishing the row's profile after the update notes
 * nothing more.
 */
static int
test_mode_change_under_row_notes_configured_modes(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    rc |= expect_int("under row: -fA baseline queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("under row: -fA baseline drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("under row: typed NXDN row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NXDN48), 0);
    rc |= expect_int("under row: typed NXDN row options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    rc |= expect_int("under row: row runs NXDN48", opts.frame_nxdn48, 1);

    reset_rx_family_wrap();
    g_fake_analog_family = 1;
    rc |= expect_int("under row: dmr queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("under row: dmr drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("under row: configured modes noted once", g_modes_note_calls, 1);
    rc |= expect_int("under row: noted modes are the configured DMR", g_modes_note_dmr, 1);
    rc |= expect_int("under row: not the row's NXDN48", g_modes_note_nxdn48, 0);
    rc |= expect_int("under row: the row still runs NXDN48", opts.frame_nxdn48, 1);
    dsd_scan_mode_leave(&opts, &state);

    g_fake_analog_family = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * A config whose [mode] moves a live RTL session into or out of analog has to switch the receive family and open the
 * new family's sink the way DECODE_MODE_SET does, or the analog preset runs on the digital demodulator (and back). A
 * [mode] that stays inside its family asks the front end for nothing new, and opens no sink unless it writes raw audio
 * a digital start did not open a sink for (ProVoice).
 */
static int
test_config_apply_switches_rtl_receive_family(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_DMR, "config dmr start");

    reset_rx_family_wrap();
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_ANALOG, "config analog");
    rc |= expect_int("config analog preset applied", opts.analog_only, 1);
    rc |= expect_int("config analog profile published", g_analog_req_calls, 1);
    rc |= expect_int("config analog family requested", g_analog_req_family, DSD_RX_FAMILY_ANALOG);
    rc |= expect_int("config analog FM requested", g_analog_req_kind, DSD_ANALOG_DEMOD_FM);
    rc |= expect_int("config analog asks for no symbol profile", g_demod_req_calls, 0);
    rc |= expect_int("config analog ensures the raw sink", g_ensure_analog_calls, 1);
    rc |= expect_int("config analog leaves the digital sink", g_ensure_digital_calls, 0);

    /* Back to DMR while the front end still runs the analog monitor at a 24 kHz DSP rate. */
    reset_rx_family_wrap();
    g_fake_analog_family = 1;
    g_fake_digital_rate = 24000U;
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_DMR, "config dmr");
    rc |= expect_int("config dmr leaves analog", opts.analog_only, 0);
    rc |= expect_int("config digital family requested", g_analog_req_family, DSD_RX_FAMILY_DIGITAL);
    rc |= expect_int("config family request made once", g_analog_req_calls, 1);
    rc |= expect_int("config symbol profile follows", g_demod_req_calls, 1);
    rc |= expect_int("config family before profile", g_analog_req_order < g_demod_req_order, 1);
    rc |= expect_int("config decoder timed for the digital rate", state.samplesPerSymbol, 5);
    rc |= expect_int("config digital sink ensured", g_ensure_digital_calls, 1);
    rc |= expect_int("config raw sink not asked for", g_ensure_analog_calls, 0);

    /* A [mode] inside the same family keeps the earlier config-apply behaviour, except that the front end learns the
       digital modes it now configures: after the live switch above its opening options name none, and the noted ones
       pick the FSK channel profile its CQPSK toggle returns to (NXDN48's 6.25 kHz, not DMR's 12.5 kHz). */
    reset_rx_family_wrap();
    g_fake_analog_family = 0;
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_NXDN48, "config nxdn48");
    rc |= expect_int("config same family asks for no family", g_analog_req_calls, 0);
    rc |= expect_int("config same family asks for no profile", g_demod_req_calls, 0);
    rc |= expect_int("config same family opens no sink", g_ensure_analog_calls + g_ensure_digital_calls, 0);
    rc |= expect_int("config same family notes its digital modes", g_modes_note_calls, 1);
    rc |= expect_int("config same family notes NXDN48", g_modes_note_nxdn48, 1);
    rc |= expect_int("config same family no longer notes DMR", g_modes_note_dmr, 0);

    /* DMR-family session to ProVoice: still digital, but ProVoice writes the raw stream (UDP port + 2 with UDP output)
       that a digital start did not open. */
    reset_rx_family_wrap();
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_EDACS_PV, "config provoice");
    rc |= expect_int("config provoice stays digital", opts.analog_only, 0);
    rc |= expect_int("config provoice preset applied", opts.frame_provoice, 1);
    rc |= expect_int("config provoice asks for no family", g_analog_req_calls, 0);
    rc |= expect_int("config provoice opens its raw sink", g_ensure_digital_calls, 1);
    rc |= expect_int("config provoice is not the analog monitor", g_ensure_analog_calls, 0);

    g_fake_digital_rate = 0U;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * A config whose [mode] picks Analog on a live digital RTL session is asked about before it is applied, as
 * DECODE_MODE_SET is: when the running front end would refuse the analog profile (an explicit NFM width its demod rate
 * cannot realize, say), the whole config is refused with a toast and the session keeps its digital mode, front end,
 * sink and options. Applying the preset first would leave an Analog decoder on the digital demodulator, and picking
 * Analog again would take the already-selected shortcut and never retry. Once the front end takes the profile, the
 * same config switches. A config that stays digital is never asked about.
 */
static int
test_config_apply_refused_analog_profile_changes_nothing(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_DMR, "config refusal: dmr start");
    opts.analog_nfm_bandwidth_hz = 25000;

    reset_rx_family_wrap();
    g_analog_check_result = -1;
    state.ui_msg[0] = '\0';
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_ANALOG, "config refusal: analog");
    rc |= expect_int("config refusal: front end asked once", g_analog_check_calls, 1);
    rc |= expect_int("config refusal: asked for the analog family", g_analog_check_family, DSD_RX_FAMILY_ANALOG);
    rc |= expect_int("config refusal: asked for NFM", g_analog_check_kind, DSD_ANALOG_DEMOD_FM);
    rc |= expect_int("config refusal: asked for the configured width", g_analog_check_width_hz, 25000);
    rc |= expect_int("config refusal: decoder stays digital", opts.analog_only, 0);
    rc |= expect_int("config refusal: DMR still decoded", opts.frame_dmr, 1);
    rc |= expect_int("config refusal: mode still DMR", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_DMR, 1);
    rc |= expect_int("config refusal: no family request", g_analog_req_calls, 0);
    rc |= expect_int("config refusal: no symbol profile", g_demod_req_calls, 0);
    rc |= expect_int("config refusal: no raw sink opened", g_ensure_analog_calls, 0);
    rc |= expect_int("config refusal: toast names the refusal", strstr(state.ui_msg, "refused") != NULL, 1);

    /* The front end takes it now: the same config switches. */
    reset_rx_family_wrap();
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_ANALOG, "config accepted: analog");
    rc |= expect_int("config accepted: front end asked", g_analog_check_calls, 1);
    rc |= expect_int("config accepted: decoder on analog", opts.analog_only, 1);
    rc |= expect_int("config accepted: analog family requested", g_analog_req_family, DSD_RX_FAMILY_ANALOG);
    rc |= expect_int("config accepted: width travels", g_analog_req_width_hz, 25000);
    rc |= expect_int("config accepted: raw sink ensured", g_ensure_analog_calls, 1);

    /* Back to a digital [mode]: never asked about. */
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_DMR, "config digital: dmr");
    rc |= expect_int("config digital: front end not asked", g_analog_check_calls, 0);
    rc |= expect_int("config digital: DMR applied", opts.analog_only == 0 && opts.frame_dmr == 1, 1);

    g_analog_check_result = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* One DECODE_MODE_SET through the queue. */
static int
submit_decode_mode(dsd_opts* opts, dsd_state* state, dsdneoUserDecodeMode mode, const char* label) {
    int rc = expect_int(label, dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)mode),
                        DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/*
 * AM (issue #524) switches live across the analog and digital families on a running RTL session. DMR -> AM asks the
 * front end about the AM profile first, then publishes it (the analog family, the AM kind, the configured AM width) and
 * opens the monitor's raw sink. AM -> Analog and back is a live FM <-> AM switch on the running monitor: the kind the
 * request carries changes, nothing else. AM -> DMR leaves the analog family ahead of DMR's symbol profile. The AM width
 * a refused check would not run leaves the session where it was.
 */
static int
test_decode_mode_set_am_switches_live(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_DMR, "am: dmr start");

    reset_rx_family_wrap();
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, "am: dmr -> am");
    rc |= expect_int("am: preset applied", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_AM, 1);
    rc |= expect_int("am: AM detector chosen", opts.analog_demod, DSD_ANALOG_DEMOD_AM);
    rc |= expect_int("am: front end asked about AM", g_analog_check_kind, DSD_ANALOG_DEMOD_AM);
    rc |= expect_int("am: front end asked for the default width", g_analog_check_width_hz, 0);
    rc |= expect_int("am: analog family requested", g_analog_req_family, DSD_RX_FAMILY_ANALOG);
    rc |= expect_int("am: AM requested", g_analog_req_kind, DSD_ANALOG_DEMOD_AM);
    rc |= expect_int("am: default width travels as 0", g_analog_req_width_hz, 0);
    rc |= expect_int("am: no symbol profile", g_demod_req_calls, 0);
    rc |= expect_int("am: raw sink ensured", g_ensure_analog_calls, 1);
    rc |= expect_int("am: toast names AM", strstr(state.ui_msg, "Decoding AM") != NULL, 1);

    reset_rx_family_wrap();
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_ANALOG, "am: am -> analog");
    rc |= expect_int("am -> analog: FM monitor", opts.analog_only == 1 && opts.analog_demod == DSD_ANALOG_DEMOD_FM, 1);
    rc |= expect_int("am -> analog: FM requested", g_analog_req_kind, DSD_ANALOG_DEMOD_FM);
    rc |= expect_int("am -> analog: still the analog family", g_analog_req_family, DSD_RX_FAMILY_ANALOG);
    rc |= expect_int("am -> analog: no symbol profile", g_demod_req_calls, 0);

    reset_rx_family_wrap();
    g_fake_analog_family = 1;
    opts.analog_am_bandwidth_hz = 8000;
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, "am: analog -> am");
    rc |= expect_int("analog -> am: AM requested", g_analog_req_kind, DSD_ANALOG_DEMOD_AM);
    rc |= expect_int("analog -> am: configured width travels", g_analog_req_width_hz, 8000);
    rc |= expect_int("analog -> am: checked at that width", g_analog_check_width_hz, 8000);

    reset_rx_family_wrap();
    g_fake_digital_rate = 48000U;
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_DMR, "am: am -> dmr");
    rc |= expect_int("am -> dmr: digital", opts.analog_only == 0 && opts.frame_dmr == 1, 1);
    rc |= expect_int("am -> dmr: detector back to FM", opts.analog_demod, DSD_ANALOG_DEMOD_FM);
    rc |= expect_int("am -> dmr: digital family requested", g_analog_req_family, DSD_RX_FAMILY_DIGITAL);
    rc |= expect_int("am -> dmr: symbol profile follows",
                     g_demod_req_calls == 1 && g_analog_req_order < g_demod_req_order, 1);
    rc |= expect_int("am -> dmr: digital sink ensured", g_ensure_digital_calls, 1);

    /* The front end would refuse the AM profile: nothing changes. */
    reset_rx_family_wrap();
    g_fake_analog_family = 0;
    g_analog_check_result = -1;
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, "am: refused");
    rc |= expect_int("am refused: still DMR", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_DMR, 1);
    rc |= expect_int("am refused: no request", g_analog_req_calls, 0);

    g_analog_check_result = 0;
    g_fake_digital_rate = 0U;
    opts.analog_am_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * A live switch from the FM monitor to AM (issue #524) is a decode-mode change like any other: it forgets a received
 * CTCSS tone or DCS code (issues #522, #523) through the acquisition reset, so the AM monitor, which detects neither,
 * never shows the FM monitor's lock.
 */
static int
test_decode_mode_set_am_clears_received_tone(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    for (int code = 0; code < 2; code++) {
        char tag[64];
        DSD_SNPRINTF(tag, sizeof(tag), "analog -> am clears the received %s", code ? "code" : "tone");
        init_decode_mode_context(&opts, &state);
        opts.audio_in_type = AUDIO_IN_RTL;
        state.rtl_ctx = (RtlSdrContext*)fake_ctx;
        rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_ANALOG, "am clear: analog start");
        reset_rx_family_wrap();
        g_fake_analog_family = 1;
        if (code) {
            seed_received_code(&state);
        } else {
            seed_received_tone(&state);
        }
        const uint32_t seeded = state.analog_rx.generation;
        rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, tag);
        rc |= expect_int(tag, opts.analog_only == 1 && opts.analog_demod == DSD_ANALOG_DEMOD_AM, 1);
        rc |= expect_int(tag, g_analog_req_kind, DSD_ANALOG_DEMOD_AM);
        rc |= expect_received_tone_cleared(tag, &state, seeded);
        g_fake_analog_family = 0;
        state.rtl_ctx = NULL;
        freeState(&state);
    }
    reset_rx_family_wrap();
    return rc;
}

/* Post DSD_APP_CMD_AM_BANDWIDTH_SET and drain it. */
static int
submit_am_width(dsd_opts* opts, dsd_state* state, int32_t hz, const char* label) {
    state->ui_msg[0] = '\0';
    int rc =
        expect_int(label, dsd_app_command_set_i32(DSD_APP_CMD_AM_BANDWIDTH_SET, hz), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/*
 * The AM width command on a running RTL session (issue #524). With AM on air the width is held to the running front
 * end (the check the request makes) and applied live; a width it refuses is not stored and the toast says why. Under
 * another preset the width is only stored, for the next switch to AM.
 */
static int
test_am_bandwidth_set_applies_live(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, "am width: am start");

    reset_rx_family_wrap();
    rc |= submit_am_width(&opts, &state, 12000, "am width: 12 kHz");
    rc |= expect_int("am width: stored", opts.analog_am_bandwidth_hz, 12000);
    rc |= expect_int(
        "am width: checked against the front end",
        g_analog_check_calls == 1 && g_analog_check_kind == DSD_ANALOG_DEMOD_AM && g_analog_check_width_hz == 12000, 1);
    rc |= expect_int("am width: applied live",
                     g_analog_req_calls == 1 && g_analog_req_family == DSD_RX_FAMILY_ANALOG
                         && g_analog_req_kind == DSD_ANALOG_DEMOD_AM && g_analog_req_width_hz == 12000,
                     1);
    rc |= expect_int("am width: toast", strstr(state.ui_msg, "AM bandwidth -> 12 kHz") != NULL, 1);

    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_am_width(&opts, &state, 20000, "am width: refused by the front end");
    rc |= expect_int("am width: refused width not stored", opts.analog_am_bandwidth_hz, 12000);
    rc |= expect_int("am width: refused width not requested", g_analog_req_calls, 0);
    rc |= expect_int("am width: refusal toast",
                     strstr(state.ui_msg, "Refused: the RTL front end refused AM 20 kHz (see log)") != NULL, 1);
    g_analog_check_result = 0;

    reset_rx_family_wrap();
    rc |= submit_am_width(&opts, &state, 0, "am width: back to the default");
    rc |= expect_int("am width: default stored as 0", opts.analog_am_bandwidth_hz, 0);
    rc |= expect_int("am width: default applied live", g_analog_req_calls == 1 && g_analog_req_width_hz == 0, 1);
    rc |= expect_int("am width: default toast", strstr(state.ui_msg, "AM bandwidth -> default") != NULL, 1);

    /* On the FM monitor the AM width waits for the next switch to AM. */
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_ANALOG, "am width: analog");
    reset_rx_family_wrap();
    rc |= submit_am_width(&opts, &state, 10000, "am width: under analog");
    rc |= expect_int("am width: stored under analog", opts.analog_am_bandwidth_hz, 10000);
    rc |= expect_int("am width: nothing requested under analog", g_analog_req_calls, 0);
    rc |= expect_int("am width: under analog the width is configuration",
                     strstr(state.ui_msg, "Applied: AM bandwidth -> 10 kHz") != NULL, 1);

    opts.analog_am_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * A config whose [mode] picks AM on the running FM monitor is a live FM -> AM switch: asked about first, then published
 * with the AM kind, although the session never leaves the analog family. A config that only changes the AM width
 * under AM republishes the profile with the new width.
 */
static int
test_config_apply_am_within_the_analog_family(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_ANALOG, "config am: analog start");

    reset_rx_family_wrap();
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_AM, "config am: analog -> am");
    rc |= expect_int("config am: on AM", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_AM, 1);
    rc |= expect_int("config am: front end asked about AM", g_analog_check_kind, DSD_ANALOG_DEMOD_AM);
    rc |= expect_int("config am: AM published", g_analog_req_calls == 1 && g_analog_req_kind == DSD_ANALOG_DEMOD_AM, 1);

    reset_rx_family_wrap();
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_mode = 1;
    cfg.decode_mode = DSDCFG_MODE_AM;
    cfg.has_analog = 1;
    cfg.analog_am_bandwidth_hz = 9000;
    rc |= expect_int("config am width queued", dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof cfg),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("config am width drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("config am width: stored", opts.analog_am_bandwidth_hz, 9000);
    rc |= expect_int("config am width: checked at the new width", g_analog_check_width_hz, 9000);
    rc |= expect_int("config am width: republished", g_analog_req_calls == 1 && g_analog_req_width_hz == 9000, 1);

    /* Restating the same config changes nothing on the front end. */
    reset_rx_family_wrap();
    rc |= expect_int("config am same queued", dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof cfg),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("config am same drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("config am same: nothing republished", g_analog_req_calls, 0);

    opts.analog_am_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * The AM width command under a -Y scan row on an AM session (issue #524). The command edits the configured width; it
 * is held to the running front end's rate under any row, since a row never changes the DSP rate and the configured AM
 * profile runs at it again. On a blank (INHERIT) row the front end runs the configured AM profile, so the width
 * applies live, as without a row. A typed digital row runs its own symbol profile: the width is stored and nothing is
 * requested until the row's leave returns to the AM monitor with it. A width the rate refuses is refused under either
 * row.
 */
static int
test_am_bandwidth_set_under_scan_rows(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, "am row: am start");

    rc |= expect_int("am row: blank row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_INHERIT), 0);
    rc |= expect_int("am row: blank row options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    reset_rx_family_wrap();
    rc |= submit_am_width(&opts, &state, 8000, "am row: 8 kHz on a blank row");
    rc |= expect_int("am row: blank row stores the width", opts.analog_am_bandwidth_hz, 8000);
    rc |= expect_int("am row: blank row checks the width", g_analog_check_calls == 1 && g_analog_check_width_hz == 8000,
                     1);
    rc |= expect_int("am row: blank row applies it live",
                     g_analog_req_calls == 1 && g_analog_req_family == DSD_RX_FAMILY_ANALOG
                         && g_analog_req_kind == DSD_ANALOG_DEMOD_AM && g_analog_req_width_hz == 8000,
                     1);
    rc |= expect_int("am row: blank row toast", strstr(state.ui_msg, "Applied: AM bandwidth -> 8 kHz") != NULL, 1);

    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_am_width(&opts, &state, 20000, "am row: 20 kHz refused on a blank row");
    rc |= expect_int("am row: refused width not stored", opts.analog_am_bandwidth_hz, 8000);
    rc |= expect_int("am row: refused width not requested", g_analog_req_calls, 0);
    rc |= expect_int("am row: refusal toast",
                     strstr(state.ui_msg, "Refused: the RTL front end refused AM 20 kHz (see log)") != NULL, 1);
    dsd_scan_mode_leave(&opts, &state);

    rc |= expect_int("am row: typed DMR row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR), 0);
    rc |= expect_int("am row: typed DMR row options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    rc |= expect_int("am row: the row runs DMR", opts.frame_dmr == 1 && opts.analog_only == 0, 1);
    reset_rx_family_wrap();
    g_fake_analog_family = 1;
    g_analog_check_result = -1;
    rc |= submit_am_width(&opts, &state, 20000, "am row: 20 kHz refused under a typed row");
    rc |= expect_int(
        "am row: typed row checks against the running rate",
        g_analog_check_calls == 1 && g_analog_check_kind == DSD_ANALOG_DEMOD_AM && g_analog_check_width_hz == 20000, 1);
    rc |= expect_int("am row: typed row refusal keeps the width", opts.analog_am_bandwidth_hz, 8000);
    rc |= expect_int("am row: typed row refusal toast",
                     strstr(state.ui_msg, "Refused: the RTL front end refused AM 20 kHz (see log)") != NULL, 1);

    reset_rx_family_wrap();
    g_fake_analog_family = 1;
    rc |= submit_am_width(&opts, &state, 10000, "am row: 10 kHz under a typed row");
    rc |= expect_int("am row: typed row stores the width", opts.analog_am_bandwidth_hz, 10000);
    rc |= expect_int("am row: typed row edits the configured width",
                     dsd_scan_mode_configured_view(&state)->analog_am_bandwidth_hz, 10000);
    rc |= expect_int("am row: typed row requests nothing", g_analog_req_calls + g_demod_req_calls, 0);
    rc |= expect_int("am row: typed row keeps running DMR", opts.frame_dmr, 1);
    rc |= expect_int("am row: typed row toast", strstr(state.ui_msg, "Applied: AM bandwidth -> 10 kHz") != NULL, 1);
    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_int("am row: back on AM with the new width",
                     dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_AM && opts.analog_am_bandwidth_hz == 10000, 1);

    /* A config apply under a row (a scoped command) holds its AM width to the running rate too. */
    rc |= expect_int("am row: typed row again", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR), 0);
    rc |= expect_int("am row: typed row again options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_analog = 1;
    cfg.analog_am_bandwidth_hz = 20000;
    state.ui_msg[0] = '\0';
    rc |= expect_int("am row: config width queued", dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof cfg),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("am row: config width drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("am row: config width checked", g_analog_check_calls >= 1 && g_analog_check_width_hz == 20000, 1);
    rc |= expect_int("am row: config width refused", opts.analog_am_bandwidth_hz, 10000);
    rc |= expect_int("am row: config refusal toast",
                     strstr(state.ui_msg, "Config not applied: the RTL front end refused AM 20 kHz (see log)") != NULL,
                     1);
    dsd_scan_mode_leave(&opts, &state);

    /* An nfm row with its own width runs over the AM session (issue #526): the AM width edit is the configured
       default's, which no row shadows, and waits for the leave, since the row runs FM; an NFM edit is shadowed by the
       row's own width, and says so. The leave keeps both edits. */
    g_analog_check_result = 0;
    dsd_scan_option_values nfm_row = {0};
    nfm_row.present = DSD_SCAN_OPT_BANDWIDTH;
    nfm_row.channel_bw_hz = 12500;
    rc |= expect_int("am row: nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM), 0);
    rc |= expect_int("am row: nfm row options", dsd_scan_mode_options(&opts, &state, &nfm_row), 0);
    rc |= expect_int("am row: nfm row runs FM at its width",
                     opts.analog_demod == DSD_ANALOG_DEMOD_FM && opts.analog_nfm_bandwidth_hz == 12500, 1);
    reset_rx_family_wrap();
    g_fake_analog_family = 1;
    rc |= submit_am_width(&opts, &state, 15000, "am row: 15 kHz under an nfm row");
    rc |= expect_int("am row: nfm row edits the configured AM width",
                     dsd_scan_mode_configured_view(&state)->analog_am_bandwidth_hz, 15000);
    rc |= expect_int("am row: nfm row keeps its own width", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("am row: nfm row requests nothing for AM", g_analog_req_calls, 0);
    rc |= expect_toast("am row: nfm row AM toast", &state, "Applied: AM bandwidth -> 15 kHz");
    reset_rx_family_wrap();
    g_fake_analog_family = 1;
    rc |= submit_nfm_width(&opts, &state, 20000, "am row: NFM edit under the nfm row");
    rc |= expect_toast("am row: NFM edit is shadowed", &state,
                       "Default NFM bandwidth -> 20 kHz; this channel overrides it (12.5 kHz)");
    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_int("am row: leave back on AM with the edit",
                     dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_AM && opts.analog_am_bandwidth_hz == 15000
                         && opts.analog_nfm_bandwidth_hz == 20000,
                     1);

    g_analog_check_result = 0;
    g_fake_analog_family = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    opts.analog_am_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * AM runs only on an I/Q input (issue #524). Switching a running AM session's input to PCM (Pulse here) falls it back
 * to the Analog monitor with the reason, as a start with a config's decode = am on PCM does, rather than leave AM
 * running on audio that arrives demodulated (where the received-tone detector, FM-only, would stay off). The operator
 * moved the session there, so autosave stays on and keeps the new input with the mode it runs, unlike a config whose
 * own decode = am meets PCM input. Once an input is open its type alone decides: a session started with --iq-replay
 * and switched to Pulse keeps the replay request and spec, and AM is still refused there.
 */
static int
test_input_switch_to_pcm_leaves_am(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, "pcm switch: am start");
    rc |= expect_int("pcm switch: on AM", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_AM, 1);

    state.rtl_ctx = NULL;
    state.config_autosave_enabled = 1;
    arm_open_audio_input_stub(1, 0);
    rc |=
        expect_int("pcm switch: pulse queued", post_empty(DSD_APP_CMD_INPUT_SET_PULSE), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("pcm switch: pulse drained", dsd_app_drain_cmds(&opts, &state), 1);
    arm_open_audio_input_stub(0, 0);
    rc |= expect_int("pcm switch: on Pulse", opts.audio_in_type, AUDIO_IN_PULSE);
    rc |= expect_int("pcm switch: fell back to Analog", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_ANALOG, 1);
    rc |= expect_int("pcm switch: FM detector", opts.analog_demod, DSD_ANALOG_DEMOD_FM);
    rc |= expect_int("pcm switch: toast gives the reason",
                     strstr(state.ui_msg, "Decoding Analog: AM demodulation needs an IQ radio input") != NULL, 1);
    rc |= expect_int("pcm switch: autosave stays on", state.config_autosave_enabled, 1);
    state.config_autosave_enabled = 0;

    /* A replay session switched to Pulse: the request and spec stay, the input is PCM. */
    opts.iq_replay_requested = 1;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "iqreplay:capture.iq");
    state.ui_msg[0] = '\0';
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, "pcm switch: am after replay");
    rc |=
        expect_int("pcm switch: AM refused after replay", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_ANALOG, 1);
    rc |= expect_int("pcm switch: refusal toast",
                     strstr(state.ui_msg, "Refused: AM demodulation needs an IQ radio input") != NULL, 1);
    opts.iq_replay_requested = 0;
    freeState(&state);
    return rc;
}

/*
 * The fallback lands on Analog whatever the NFM width. An AM session on an RTL-SDR at a 24 kHz DSP bandwidth may keep
 * an explicit NFM width that rate cannot filter (25 kHz), since AM does not run it. A live switch to TCP audio names the
 * input it opened and keeps the RTL device string for the RTL-SDR row (issue #634), but no channel filter runs on PCM
 * input, so no DSP rate holds the width there: the fallback goes ahead with the width stored, and no later command
 * repeats a refusal.
 */
static int
test_tcp_switch_leaves_am_with_unfit_nfm_width(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:118.1M:22:0:24");
    opts.rtl_dsp_bw_khz = 24;
    opts.analog_nfm_bandwidth_hz = 25000;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    g_analog_check_result = 0;
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, "tcp switch: am start");
    rc |= expect_int("tcp switch: on AM", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_AM, 1);

    state.rtl_ctx = NULL;
    state.ui_msg[0] = '\0';
    arm_tcp_connect_stub(1, 0);
    rc |= expect_int("tcp switch: connect queued", post_host_port(DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG, "127.0.0.1", 7355),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tcp switch: connect drained", dsd_app_drain_cmds(&opts, &state), 1);
    arm_tcp_connect_stub(0, 0);
    rc |= expect_int("tcp switch: on TCP audio", opts.audio_in_type, AUDIO_IN_TCP);
    rc |= expect_str("tcp switch: names the input", opts.audio_in_dev, "tcp:127.0.0.1:7355");
    rc |= expect_str("tcp switch: device string kept for the rtl row", opts.radio_in_dev, "rtl:0:118.1M:22:0:24");
    rc |= expect_int("tcp switch: fell back to Analog", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_ANALOG, 1);
    rc |= expect_int("tcp switch: FM detector", opts.analog_demod, DSD_ANALOG_DEMOD_FM);
    rc |= expect_int("tcp switch: NFM width kept", opts.analog_nfm_bandwidth_hz, 25000);
    rc |= expect_int("tcp switch: toast gives the reason",
                     strstr(state.ui_msg, "Decoding Analog: AM demodulation needs an IQ radio input") != NULL, 1);

    /* A width set on the PCM session is stored, not held to the old device's rate (22 kHz does not fit 24 kHz). */
    rc |= submit_nfm_width(&opts, &state, 22000, "tcp switch: pcm width");
    rc |= expect_int("tcp switch: pcm width stored", opts.analog_nfm_bandwidth_hz, 22000);
    rc |= expect_toast("tcp switch: pcm width toast", &state, "Applied: NFM bandwidth -> 22 kHz");
    opts.analog_nfm_bandwidth_hz = 0;
    freeState(&state);
    return rc;
}

/* An -fA session on an RTL-SDR input whose DSP bandwidth is 24 kHz, with the front end on the analog monitor. */
static void
init_nfm_session(dsd_opts* opts, dsd_state* state, RtlSdrContext* fake_ctx) {
    init_decode_mode_context(opts, state);
    opts->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:851.375M:0:0:24");
    opts->rtl_dsp_bw_khz = 24;
    state->rtl_ctx = fake_ctx;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    (void)dsd_app_drain_cmds(opts, state);
    reset_rx_family_wrap();
    g_fake_cqpsk = 0;
}

static void
submit_cqpsk_toggle(void) {
    dsd_app_dsp_payload dsp = {0};
    dsp.op = DSD_APP_DSP_OP_TOGGLE_CQ;
    (void)dsd_app_command_dsp_op(&dsp);
}

/*
 * Issue #525: DSD_APP_CMD_NFM_BANDWIDTH_SET edits the configured NFM width and hands it to the running front end, live.
 * A width outside 8000..25000 Hz, or one the front end cannot filter at its DSP rate, is refused with a toast that
 * names the values and the fix, and the previous width stays: never clamped. 0 selects the default.
 */
static int
test_nfm_bandwidth_set_applies_live_and_refuses(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    rc |= expect_int("nfm: session is analog", opts.analog_only, 1);

    rc |= submit_nfm_width(&opts, &state, 12500, "nfm 12500");
    rc |= expect_int("nfm 12500 stored", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("nfm 12500 checked with the front end", g_analog_check_calls, 1);
    rc |= expect_int("nfm 12500 checked as NFM", g_analog_check_kind, DSD_ANALOG_DEMOD_FM);
    rc |= expect_int("nfm 12500 checked width", g_analog_check_width_hz, 12500);
    rc |= expect_int("nfm 12500 requested live", g_analog_req_calls, 1);
    rc |= expect_int("nfm 12500 request family", g_analog_req_family, DSD_RX_FAMILY_ANALOG);
    rc |= expect_int("nfm 12500 request width", g_analog_req_width_hz, 12500);
    rc |= expect_toast("nfm 12500 toast", &state, "Applied: NFM bandwidth -> 12.5 kHz");

    reset_rx_family_wrap();
    rc |= submit_nfm_width(&opts, &state, 30000, "nfm 30000");
    rc |= expect_int("nfm 30000 keeps the width", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("nfm 30000 asks nothing", g_analog_check_calls + g_analog_req_calls, 0);
    rc |= expect_toast("nfm 30000 toast", &state, "Refused: NFM bandwidth 30 kHz is outside 8 kHz to 25 kHz");
    rc |= submit_nfm_width(&opts, &state, -1, "nfm -1");
    rc |= expect_int("nfm -1 keeps the width", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_toast("nfm -1 toast", &state, "Refused: NFM bandwidth -1 Hz is outside");

    /* The front end refuses a width its 24 kHz DSP rate cannot filter: the toast names both, the limit and the fix.
       The channel keeps its filter, so the tone received on it (issue #522) stays too. */
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    seed_received_tone(&state);
    const uint32_t seeded = state.analog_rx.generation;
    rc |= submit_nfm_width(&opts, &state, 25000, "nfm 25000");
    rc |= expect_int("nfm 25000 keeps the width", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("nfm 25000 not requested", g_analog_req_calls, 0);
    rc |=
        expect_toast("nfm 25000 toast", &state,
                     "Refused: NFM 25 kHz does not fit the 24 kHz DSP rate (max 20.4 kHz); use a 48 kHz DSP bandwidth");
    rc |= expect_received_tone_kept("nfm 25000 keeps the received tone", &state, seeded);
    /* Refused for a reason the rate does not explain (DSD_NEO_CHANNEL_LPF=0, say): the front end logged it. */
    rc |= submit_nfm_width(&opts, &state, 20000, "nfm 20000 refused");
    rc |= expect_int("nfm 20000 keeps the width", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_toast("nfm 20000 toast", &state, "Refused: the RTL front end refused NFM 20 kHz (see log)");
    g_analog_check_result = 0;

    /* 0 is the default: never refused for its rate, and requested as 0. */
    reset_rx_family_wrap();
    rc |= submit_nfm_width(&opts, &state, 0, "nfm default");
    rc |= expect_int("nfm default stored", opts.analog_nfm_bandwidth_hz, 0);
    rc |= expect_int("nfm default not checked", g_analog_check_calls, 0);
    rc |= expect_int("nfm default requested", g_analog_req_calls == 1 && g_analog_req_width_hz == 0, 1);
    rc |= expect_toast("nfm default toast", &state, "Applied: NFM bandwidth -> default");

    /* CQPSK toggled on under -fA holds the front end off the monitor: the width is stored and waits, and turning CQPSK
       off returns to the monitor through the analog profile with that width, not with the one it had. */
    reset_rx_family_wrap();
    g_fake_cqpsk = 1;
    rc |= submit_nfm_width(&opts, &state, 20000, "nfm under cqpsk");
    rc |= expect_int("nfm under cqpsk stored", opts.analog_nfm_bandwidth_hz, 20000);
    rc |= expect_int("nfm under cqpsk checked", g_analog_check_calls, 1);
    rc |= expect_int("nfm under cqpsk not requested", g_analog_req_calls, 0);
    submit_cqpsk_toggle();
    rc |= expect_int("cqpsk off drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("cqpsk off queues no demod profile of its own", g_demod_req_calls, 0);
    rc |= expect_int("cqpsk off requests the analog profile", g_analog_req_calls, 1);
    rc |= expect_int("cqpsk off: CQPSK requested off", __wrap_rtl_stream_requested_cqpsk(), 0);
    rc |= expect_int("cqpsk off: the width set meanwhile", g_analog_req_width_hz, 20000);
    rc |= expect_int("cqpsk off: NFM",
                     g_analog_req_kind == DSD_ANALOG_DEMOD_FM && g_analog_req_family == DSD_RX_FAMILY_ANALOG, 1);
    /* Turning CQPSK on is the demod profile alone. */
    reset_rx_family_wrap();
    g_fake_cqpsk = 0;
    submit_cqpsk_toggle();
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("cqpsk on: demod profile only", g_demod_req_calls == 1 && g_analog_req_calls == 0, 1);

    /* With no stream running, an RTL-SDR input's DSP bandwidth is the rate the next start will run at. */
    state.rtl_ctx = NULL;
    reset_rx_family_wrap();
    rc |= submit_nfm_width(&opts, &state, 25000, "nfm no stream");
    rc |= expect_int("nfm no stream keeps the width", opts.analog_nfm_bandwidth_hz, 20000);
    rc |= expect_int("nfm no stream asks no front end", g_analog_check_calls, 0);
    rc |=
        expect_toast("nfm no stream toast", &state,
                     "Refused: NFM 25 kHz does not fit the 24 kHz DSP rate (max 20.4 kHz); use a 48 kHz DSP bandwidth");
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;

    /* A digital session does not use the width: only its range is checked, and the front end is not asked. */
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR);
    (void)dsd_app_drain_cmds(&opts, &state);
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_nfm_width(&opts, &state, 25000, "nfm digital");
    rc |= expect_int("nfm digital stored", opts.analog_nfm_bandwidth_hz, 25000);
    rc |= expect_int("nfm digital asks nothing", g_analog_check_calls + g_analog_req_calls, 0);

    /* ...but a switch back to Analog holds the stored width to the rate first, and changes nothing when it cannot
       run. */
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= expect_int("analog with 25 kHz queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    state.ui_msg[0] = '\0';
    rc |= expect_int("analog with 25 kHz drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("analog with 25 kHz refused", opts.analog_only, 0);
    rc |= expect_int("analog with 25 kHz asks for nothing", g_analog_req_calls, 0);
    rc |= expect_toast("analog with 25 kHz toast", &state,
                       "Failed: Analog -> NFM 25 kHz does not fit the 24 kHz DSP rate (max 20.4 kHz)");

    g_analog_check_result = 0;
    g_fake_cqpsk = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * A width set right after a switch onto Analog, before the demod thread has taken that switch, has to replace the
 * width the switch carries: the front end still publishes the digital family then, and a request held back until it
 * reaches the monitor would let the queued switch land with the old width.
 */
static int
test_nfm_bandwidth_set_replaces_a_queued_switch(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:48");
    opts.rtl_dsp_bw_khz = 48;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR);
    (void)dsd_app_drain_cmds(&opts, &state);

    reset_rx_family_wrap();
    g_fake_analog_family = 0;
    g_fake_cqpsk = 0;
    rc |= expect_int("switch queued", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("width queued", dsd_app_command_set_i32(DSD_APP_CMD_NFM_BANDWIDTH_SET, 12500),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("both drained in one pass", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_int("switch and width both requested", g_analog_req_calls, 2);
    rc |= expect_int("the last request carries the new width", g_analog_req_width_hz, 12500);
    rc |= expect_int("the last request is the analog family", g_analog_req_family, DSD_RX_FAMILY_ANALOG);

    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* Submit a DSP-menu CQPSK toggle and an NFM width, in that order, for one drain. */
static int
queue_cqpsk_toggle_then_width(int32_t hz, const char* label) {
    submit_cqpsk_toggle();
    return expect_int(label, dsd_app_command_set_i32(DSD_APP_CMD_NFM_BANDWIDTH_SET, hz), DSD_APP_COMMAND_SUBMIT_QUEUED);
}

/*
 * A CQPSK toggle and a width change drained in the same pass, before the demod thread has taken the toggle: the front
 * end still publishes the CQPSK state it had, so the width has to go by what was queued, not by what is published.
 * Turning CQPSK on must not be undone by the width's analog request (which would drop the queued toggle); turning it
 * off must end on the new width, not on the one the toggle's return to the monitor carries. The same holds when the
 * stream moves on (a restart) after a toggle it never took: the published state is the stream's again.
 */
static int
test_nfm_bandwidth_set_after_a_queued_cqpsk_toggle(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    opts.analog_nfm_bandwidth_hz = 16000;

    /* CQPSK on, then a width, one drain: the toggle stands and the width waits for the monitor. */
    reset_rx_family_wrap();
    g_fake_cqpsk = 0;
    rc |= queue_cqpsk_toggle_then_width(12500, "cqpsk on + width queued");
    rc |= expect_int("cqpsk on + width drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_int("cqpsk on + width: CQPSK requested on", g_demod_req_calls == 1 && g_demod_req_cqpsk == 1, 1);
    rc |= expect_int("cqpsk on + width: the toggle is not undone", g_analog_req_calls, 0);
    rc |= expect_int("cqpsk on + width: width stored", opts.analog_nfm_bandwidth_hz, 12500);

    /* The toggle has landed: CQPSK off, then a width, one drain. The last request carries the new width. */
    reset_rx_family_wrap();
    g_fake_cqpsk = 1;
    rc |= queue_cqpsk_toggle_then_width(20000, "cqpsk off + width queued");
    rc |= expect_int("cqpsk off + width drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_int("cqpsk off + width: no demod profile", g_demod_req_calls, 0);
    rc |= expect_int("cqpsk off + width: CQPSK requested off", __wrap_rtl_stream_requested_cqpsk(), 0);
    rc |= expect_int("cqpsk off + width: toggle and width both requested", g_analog_req_calls, 2);
    rc |= expect_int("cqpsk off + width: the last request carries the new width", g_analog_req_width_hz, 20000);

    /* Two toggles in one drain flip twice: the second reads the first, queued but not taken, and goes back to the
       monitor through the analog profile, which replaces the CQPSK-on profile in the stream's queue. */
    reset_rx_family_wrap();
    g_fake_cqpsk = 0;
    submit_cqpsk_toggle();
    submit_cqpsk_toggle();
    rc |= expect_int("two toggles drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_int("two toggles: on, then off",
                     g_demod_req_calls == 1 && g_demod_req_cqpsk == 1 && g_analog_req_calls == 1
                         && g_analog_req_order > g_demod_req_order,
                     1);
    rc |= expect_int("two toggles: CQPSK requested off", __wrap_rtl_stream_requested_cqpsk(), 0);
    rc |= expect_int("two toggles: back to the monitor with the width", g_analog_req_width_hz, 20000);

    /* CQPSK on, never taken, then the stream restarts (it opens on the -fA monitor): a width goes straight to it. */
    reset_rx_family_wrap();
    g_fake_cqpsk = 0;
    submit_cqpsk_toggle();
    (void)dsd_app_drain_cmds(&opts, &state);
    demod_thread_lands(0);
    g_analog_req_calls = 0;
    rc |= submit_nfm_width(&opts, &state, 12500, "width after a restart");
    rc |= expect_int("width after a restart: requested", g_analog_req_calls == 1 && g_analog_req_width_hz == 12500, 1);

    g_fake_cqpsk = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * CQPSK toggled on under AM from the DSP menu, then off again: the return to the monitor is the analog request alone,
 * which turns CQPSK off as it enters the monitor. A CQPSK-off demod profile queued with it could be taken on its own at
 * a block boundary before the analog request was queued, putting the FSK channel profile on the monitor output with
 * the FM discriminator, AM audio lost, where a refusal of the analog request would leave it. So none is queued, and a
 * return the front end refuses, at once (its rate cannot filter the AM width, the default included) or where it lands
 * (a retune moved the rate), leaves CQPSK on.
 */
static int
test_cqpsk_off_under_am_refused_keeps_cqpsk(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, "am cqpsk: am start");

    reset_rx_family_wrap();
    g_fake_cqpsk = 1;
    g_analog_req_result = -1;
    submit_cqpsk_toggle();
    rc |= expect_int("am cqpsk off refused at once: drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("am cqpsk off refused at once: asked for AM",
                     g_analog_req_calls == 1 && g_analog_req_kind == DSD_ANALOG_DEMOD_AM, 1);
    rc |= expect_int("am cqpsk off refused at once: no demod profile", g_demod_req_calls, 0);
    rc |= expect_int("am cqpsk off refused at once: CQPSK stays on", __wrap_rtl_stream_requested_cqpsk(), 1);

    reset_rx_family_wrap();
    g_fake_cqpsk = 1;
    submit_cqpsk_toggle();
    rc |= expect_int("am cqpsk off refused landing: drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("am cqpsk off refused landing: the analog request alone",
                     g_analog_req_calls == 1 && g_demod_req_calls == 0, 1);
    demod_thread_refuses_analog_keeping_kind(1, DSD_ANALOG_DEMOD_AM, 0);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("am cqpsk off refused landing: CQPSK stays on", __wrap_rtl_stream_requested_cqpsk(), 1);
    rc |= expect_int("am cqpsk off refused landing: still AM",
                     opts.analog_only == 1 && opts.analog_demod == DSD_ANALOG_DEMOD_AM, 1);

    /* Accepted, the return is the analog request with the AM kind, and nothing else. */
    reset_rx_family_wrap();
    g_fake_cqpsk = 1;
    submit_cqpsk_toggle();
    rc |= expect_int("am cqpsk off: drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("am cqpsk off: back to the AM monitor",
                     g_demod_req_calls == 0 && g_analog_req_calls == 1 && g_analog_req_kind == DSD_ANALOG_DEMOD_AM
                         && g_analog_req_family == DSD_RX_FAMILY_ANALOG,
                     1);
    rc |= expect_int("am cqpsk off: CQPSK requested off", __wrap_rtl_stream_requested_cqpsk(), 0);

    reset_rx_family_wrap();
    g_fake_cqpsk = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * A P25 session on CQPSK switched to Analog, and a width set, in the same drain: the front end still publishes CQPSK,
 * the digital session's, but the switch has asked for the monitor. The width's request replaces the switch's, so the
 * monitor opens on the new width.
 */
static int
test_nfm_bandwidth_set_after_a_queued_switch_from_cqpsk(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:48");
    opts.rtl_dsp_bw_khz = 48;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_P25P1);
    (void)dsd_app_drain_cmds(&opts, &state);

    reset_rx_family_wrap();
    g_fake_cqpsk = 1;
    g_fake_analog_family = 0;
    rc |= expect_int("cqpsk to analog queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("cqpsk to analog width queued", dsd_app_command_set_i32(DSD_APP_CMD_NFM_BANDWIDTH_SET, 12500),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("cqpsk to analog drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_int("cqpsk to analog: switch and width both requested", g_analog_req_calls, 2);
    rc |= expect_int("cqpsk to analog: the last request carries the new width", g_analog_req_width_hz, 12500);
    rc |= expect_int("cqpsk to analog: the analog family", g_analog_req_family, DSD_RX_FAMILY_ANALOG);

    g_fake_cqpsk = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * The width is configuration, so under a scan row the command runs against the baseline: on a typed digital row of an
 * -fA session it is stored without switching the front end in the middle of the row, and the row keeps running; on a
 * row that inherits the analog monitor it reaches the front end once the row's constraint is back.
 */
static int
test_nfm_bandwidth_set_under_scan_rows(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    rc |= expect_int("nfm row: typed DMR row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR), 0);
    rc |= expect_int("nfm row: typed DMR options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    reset_rx_family_wrap();
    rc |= submit_nfm_width(&opts, &state, 12500, "nfm row");
    rc |= expect_int("nfm row: width stored", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("nfm row: held to the front end", g_analog_check_calls, 1);
    rc |= expect_int("nfm row: front end not switched", g_analog_req_calls, 0);
    rc |= expect_int("nfm row: row still runs DMR", opts.frame_dmr == 1 && opts.analog_only == 0, 1);
    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_int("nfm row: baseline analog again", opts.analog_only, 1);
    rc |= expect_int("nfm row: width kept for the leave", opts.analog_nfm_bandwidth_hz, 12500);

    rc |= expect_int("nfm inherit: row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_INHERIT), 0);
    rc |= expect_int("nfm inherit: options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    reset_rx_family_wrap();
    rc |= submit_nfm_width(&opts, &state, 16000, "nfm inherit");
    rc |= expect_int("nfm inherit: width stored", opts.analog_nfm_bandwidth_hz, 16000);
    rc |= expect_int("nfm inherit: requested live", g_analog_req_calls == 1 && g_analog_req_width_hz == 16000, 1);
    dsd_scan_mode_leave(&opts, &state);

    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * A width edit is not a decoder change, so it must not disturb a row's acquisition. Frame sync writes the detected
 * Phase 2 polarity into dsd_opts while a P25 row is on air, and trunk following keeps the voice channel; had the
 * width command suspended and re-applied the scope, that live polarity would read as an acquisition change and end
 * the followed call. The edit still lands on the configured width and is still held to the front end's rate, since
 * the -fA session returns to the monitor when the row ends.
 */
static int
test_nfm_width_edit_keeps_live_acquisition(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    opts.rtl_dsp_bw_khz = 48;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:48");
    int rc = expect_int("width p25 row entered", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_P25), 0);
    rc |= expect_int("width p25 row options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    rc |= expect_int("width p25 row polarity from the preset", opts.inverted_p2, 0);
    opts.inverted_p2 = 1;
    opts.trunk_is_tuned = 1;
    state.trunk_vc_freq[0] = 851012500;
    reset_rx_family_wrap();
    rc |= submit_nfm_width(&opts, &state, 12500, "width under p25 row");
    rc |= expect_int("width under p25 row: stored", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("width under p25 row: held to the front end", g_analog_check_calls, 1);
    rc |= expect_int("width under p25 row: front end not switched", g_analog_req_calls, 0);
    rc |= expect_int("width under p25 row: keeps the detected polarity", opts.inverted_p2, 1);
    rc |= expect_int("width under p25 row: keeps the followed call", opts.trunk_is_tuned, 1);
    rc |= expect_true("width under p25 row: keeps the voice channel", state.trunk_vc_freq[0] == 851012500);
    rc |= expect_int("width under p25 row: row still P25", dsd_scan_mode_active(&state), DSD_SCAN_MODE_P25);
    rc |= expect_int("width under p25 row: row still digital", opts.analog_only, 0);

    /* Refused, it changes nothing either. */
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_nfm_width(&opts, &state, 25000, "refused width under p25 row");
    rc |= expect_int("refused width under p25 row: width kept", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("refused width under p25 row: keeps the detected polarity", opts.inverted_p2, 1);
    rc |= expect_int("refused width under p25 row: keeps the followed call", opts.trunk_is_tuned, 1);
    g_analog_check_result = 0;

    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_int("width p25 row left: baseline analog", opts.analog_only, 1);
    rc |= expect_int("width p25 row left: configured width", opts.analog_nfm_bandwidth_hz, 12500);
    opts.analog_nfm_bandwidth_hz = 0;
    opts.trunk_is_tuned = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* A -Y channel map of a DMR row and an nfm row (row 1), the nfm row with its own width when @p row_width_hz > 0. */
static void
load_dmr_and_nfm_rows(dsd_state* state, int row_width_hz) {
    state->lcn_freq_count = 2;
    state->trunk_lcn_freq[0] = 851012500L;
    state->trunk_lcn_freq[1] = 154430000L;
    (void)dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_DMR);
    (void)dsd_channel_mode_set(state, 1, DSD_SCAN_MODE_NFM);
    dsd_scan_row_profile* profile = NULL;
    if (row_width_hz > 0 && dsd_scan_profile_ensure(&profile) == 0) {
        profile->values.present = DSD_SCAN_OPT_BANDWIDTH;
        profile->values.channel_bw_hz = row_width_hz;
    }
    if (dsd_channel_profile_set(state, 1, profile) != 0) {
        dsd_scan_profile_free(profile);
    }
}

/*
 * Issue #526: under a scan row that takes the configured NFM width, the front end can still run another row's own width
 * (a retune in flight, or one a live request superseded, has not moved it off the row before). A width edit the front
 * end then refuses where it lands puts the configured width back to what it was before the edit, never to the width
 * the front end kept: that is the other row's, and must not become the default a save writes. A row with a width of
 * its own keeps its width in force as the front end kept it, with the configured width as it was.
 */
static int
test_refused_width_under_a_scan_row_keeps_the_configured_width(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    opts.analog_nfm_bandwidth_hz = 8000;
    rc |= expect_int("inheriting row: nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM), 0);
    rc |= expect_int("inheriting row: no width", dsd_scan_mode_options(&opts, &state, NULL), 0);
    reset_rx_family_wrap();
    rc |= submit_nfm_width(&opts, &state, 12500, "inheriting row: edit");
    rc |= expect_int("inheriting row: requested", g_analog_req_calls == 1 && g_analog_req_width_hz == 12500, 1);
    rc |= expect_int("inheriting row: configured while pending",
                     dsd_scan_mode_configured_view(&state)->analog_nfm_bandwidth_hz, 12500);
    demod_thread_refuses_analog_keeping(1, 9000);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("inheriting row: configured width from before the edit",
                     dsd_scan_mode_configured_view(&state)->analog_nfm_bandwidth_hz, 8000);
    rc |= expect_int("inheriting row: the row runs it", opts.analog_nfm_bandwidth_hz, 8000);
    rc |= expect_int("inheriting row: refusal reported", strncmp(state.ui_msg, "Refused: ", 9) == 0, 1);
    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_int("inheriting row: the leave keeps it", opts.analog_nfm_bandwidth_hz, 8000);

    /* A row with its own width: an edit reaches no front end there, but a request that carried the row's width (its
       entry) can still be refused where it lands; the width in force follows the front end, the configured width
       stays. */
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 12500;
    rc |= expect_int("own width row: nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM), 0);
    rc |= expect_int("own width row: its width", dsd_scan_mode_options(&opts, &state, &row), 0);
    reset_rx_family_wrap();
    rc |= expect_int("own width row: republished", svc_publish_analog_bandwidth(&opts, &state, DSD_ANALOG_DEMOD_FM, -1),
                     0);
    demod_thread_refuses_analog_keeping(1, 9000);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("own width row: in force as kept", opts.analog_nfm_bandwidth_hz, 9000);
    rc |= expect_int("own width row: configured width stays",
                     dsd_scan_mode_configured_view(&state)->analog_nfm_bandwidth_hz, 8000);
    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_int("own width row: the leave restores it", opts.analog_nfm_bandwidth_hz, 8000);

    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #518: a "this channel" width edit applied on air asks the front end for the row's new width
 * (svc_publish_row_analog_width()). Refused where it lands, the request is the row edit's to settle
 * (dsd_app_scan_row_edit_settle_refusal()): the target goes back to the width it ran before, and the configured width
 * stays as it was, even where it equals the width refused, which the configured-width path would have rolled back.
 */
static int
test_refused_row_width_request_puts_the_edit_back(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    opts.analog_nfm_bandwidth_hz = 20000;
    opts.trunk_scan_enabled = 1;
    g_fake_request_rate_hz = 48000;
    char dir[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    rc |= expect_true("row width dir", dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_row_width") != NULL);
    rc |= expect_int("row width csv", dsd_test_path_join(csv, sizeof csv, dir, "targets.csv"), 0);
    static const char k_targets[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\n"
                                    "fire,nfm-conventional,154430000,,3000,,,--nfm-bandwidth-hz 12500\n";
    rc |= write_file_bytes(csv, k_targets, sizeof k_targets - 1U);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", csv);
    char err[256] = {0};
    (void)dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    rc |= expect_int("row width: target on air", state.scan_row_scanner == DSD_SCAN_ROW_SCANNER_TRUNK_SCAN, 1);
    rc |= expect_int("row width: its own width", opts.analog_nfm_bandwidth_hz, 12500);

    dsd_app_scan_row_view view;
    (void)dsd_app_scan_row_view_get(&opts, &state, &view);
    dsd_app_scan_row_edit_payload p;
    rc |= expect_int("row width: payload",
                     dsd_app_scan_row_view_payload(&view, DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, &p), 0);
    p.width_hz = 20000;
    reset_rx_family_wrap();
    rc |= expect_true("row width: edit queued", dsd_app_command_scan_row_edit(&p) > 0);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("row width: edit in force", opts.analog_nfm_bandwidth_hz, 20000);
    rc |= expect_int("row width: asked for 20 kHz", g_analog_req_calls == 1 && g_analog_req_width_hz == 20000, 1);

    demod_thread_refuses_analog_keeping(1, 12500);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("row width: the edit is put back", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("row width: configured width untouched",
                     dsd_scan_mode_configured_analog_width(&opts, &state, DSD_ANALOG_DEMOD_FM), 20000);
    rc |= expect_int("row width: the row's refusal reported",
                     strncmp(state.ui_msg, "Refused: this channel's NFM bandwidth: ", 39) == 0, 1);
    rc |= expect_int("row width: no edit left", state.scan_row_edited, 0);

    /* The row follows the configured 20 kHz, then asks for 25 kHz of its own; the configured default moves to 16 kHz
       while that request is out, which the row's own width keeps from the front end. The front end refuses 25 kHz
       keeping 20 kHz: the row follows the default again, now 16 kHz, so the settle asks the front end for it. */
    rc |= expect_int("row width: inherit payload",
                     dsd_app_scan_row_view_payload(&view, DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_INHERIT, &p), 0);
    rc |= expect_true("row width: inherit queued", dsd_app_command_scan_row_edit(&p) > 0);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("row width: follows the default", opts.analog_nfm_bandwidth_hz, 20000);
    demod_thread_lands(0);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("row width: own payload",
                     dsd_app_scan_row_view_payload(&view, DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, &p), 0);
    p.width_hz = 25000;
    rc |= expect_true("row width: 25 kHz queued", dsd_app_command_scan_row_edit(&p) > 0);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("row width: asked for 25 kHz", g_analog_req_width_hz, 25000);
    rc |= expect_int("row width: default queued", dsd_app_command_set_i32(DSD_APP_CMD_NFM_BANDWIDTH_SET, 16000),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("row width: the row's own width stays in force", opts.analog_nfm_bandwidth_hz, 25000);
    const int requests_before = g_analog_req_calls;
    demod_thread_refuses_analog_keeping(1, 20000);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("row width: back on the default", opts.analog_nfm_bandwidth_hz, 16000);
    rc |= expect_int("row width: the default stays",
                     dsd_scan_mode_configured_analog_width(&opts, &state, DSD_ANALOG_DEMOD_FM), 16000);
    rc |= expect_int("row width: asked for the default once",
                     g_analog_req_calls == requests_before + 1 && g_analog_req_width_hz == 16000, 1);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    (void)remove(csv);
    (void)dsd_test_rmdir(dir);
    g_fake_request_rate_hz = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #526: an nfm scan row's own --nfm-bandwidth-hz runs over the configured width while the row is on air. The width
 * command still edits the configured width, without suspending the row: the row keeps its width, the front end is asked
 * for nothing, and the toast says the row overrides the edit. A row without a width of its own, and the row's leave, put
 * the edited width in force. On a digital session an nfm row without a width runs the configured width, so an edit is
 * held to the front end's rate and handed to it live there too.
 */
static int
test_nfm_bandwidth_set_under_a_width_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    rc |= submit_nfm_width(&opts, &state, 20000, "width row: configured 20 kHz");
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 12500;
    rc |= expect_int("width row: nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM), 0);
    rc |= expect_int("width row: its width", dsd_scan_mode_options(&opts, &state, &row), 0);
    rc |= expect_int("width row: in force", opts.analog_nfm_bandwidth_hz, 12500);

    reset_rx_family_wrap();
    rc |= submit_nfm_width(&opts, &state, 16000, "width row: shadowed edit");
    rc |= expect_int("width row: row keeps its width", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("width row: baseline takes the edit",
                     dsd_scan_mode_configured_view(&state)->analog_nfm_bandwidth_hz, 16000);
    rc |= expect_int("width row: held to the front end", g_analog_check_calls, 1);
    rc |= expect_int("width row: front end not asked to change", g_analog_req_calls, 0);
    rc |= expect_int("width row: row not suspended",
                     dsd_scan_mode_active(&state) == DSD_SCAN_MODE_NFM && !dsd_scan_mode_updating(&state), 1);
    rc |= expect_toast("width row: toast names the row", &state,
                       "Default NFM bandwidth -> 16 kHz; this channel overrides it (12.5 kHz)");
    /* Refused, the baseline keeps its width. */
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_nfm_width(&opts, &state, 25000, "width row: refused edit");
    rc |= expect_int("width row: refused edit keeps the baseline",
                     dsd_scan_mode_configured_view(&state)->analog_nfm_bandwidth_hz, 16000);
    rc |= expect_int("width row: refused edit keeps the row", opts.analog_nfm_bandwidth_hz, 12500);
    g_analog_check_result = 0;

    /* The same row without a width runs the edited default, and takes the next edit at once. */
    rc |= expect_int("width row: no width", dsd_scan_mode_options(&opts, &state, NULL), 0);
    rc |= expect_int("width row: default in force", opts.analog_nfm_bandwidth_hz, 16000);
    reset_rx_family_wrap();
    rc |= submit_nfm_width(&opts, &state, 11250, "width row: edit in force");
    rc |= expect_int("width row: edit in force stored", opts.analog_nfm_bandwidth_hz, 11250);
    rc |= expect_int("width row: edit requested live", g_analog_req_calls == 1 && g_analog_req_width_hz == 11250, 1);
    rc |= expect_toast("width row: edit in force toast", &state, "Applied: NFM bandwidth -> 11.25 kHz");
    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_int("width row: leave keeps the edit", opts.analog_nfm_bandwidth_hz, 11250);

    /* A digital session: the -Y map's nfm row without a width, on air here, is the one receiver the configured width
       runs on. Only a scanner puts an nfm row on air, from the map it visits. */
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("width row digital: session", opts.analog_only, 0);
    load_dmr_and_nfm_rows(&state, 0);
    opts.scanner_mode = 1;
    rc |= expect_int("width row digital: nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM), 0);
    rc |= expect_int("width row digital: no width", dsd_scan_mode_options(&opts, &state, NULL), 0);
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_nfm_width(&opts, &state, 25000, "width row digital: refused");
    rc |= expect_int("width row digital: refused keeps the width", opts.analog_nfm_bandwidth_hz, 11250);
    rc |= expect_int("width row digital: refused asks for nothing", g_analog_req_calls, 0);
    g_analog_check_result = 0;
    reset_rx_family_wrap();
    rc |= submit_nfm_width(&opts, &state, 12500, "width row digital: applied");
    rc |= expect_int("width row digital: requested live", g_analog_req_calls == 1 && g_analog_req_width_hz == 12500, 1);
    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_int("width row digital: leave keeps the edit",
                     opts.analog_only == 0 && opts.analog_nfm_bandwidth_hz == 12500, 1);

    opts.scanner_mode = 0;
    dsd_channel_modes_clear(&state);
    (void)dsd_channel_profile_set(&state, 1, NULL);
    state.lcn_freq_count = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * A switch onto Analog under a scan row is held to the explicit NFM width's rate too. Checking the front end is skipped
 * under a row for the default, which no rate refuses, but the row's resume or leave requests the analog profile with
 * an explicit width, and a width the front end refused there would leave an Analog decoder on a digital front end.
 */
static int
test_decode_mode_analog_under_a_row_holds_the_nfm_width(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:24");
    opts.rtl_dsp_bw_khz = 24;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR);
    (void)dsd_app_drain_cmds(&opts, &state);
    opts.analog_nfm_bandwidth_hz = 25000; /* stored on the digital session, as a config or the command can */
    rc |= expect_int("row: inherit", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_INHERIT), 0);
    rc |= expect_int("row: options", dsd_scan_mode_options(&opts, &state, NULL), 0);

    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= expect_int("row analog queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    state.ui_msg[0] = '\0';
    rc |= expect_int("row analog drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("row analog: front end asked with the width", g_analog_check_width_hz, 25000);
    rc |= expect_int("row analog: decoder stays digital", opts.analog_only, 0);
    rc |= expect_int("row analog: configured mode still digital",
                     dsd_scan_mode_configured_preset(&opts, &state) == DSDCFG_MODE_DMR, 1);
    rc |= expect_int("row analog: nothing requested", g_analog_req_calls, 0);
    rc |= expect_toast("row analog toast", &state,
                       "Failed: Analog -> NFM 25 kHz does not fit the 24 kHz DSP rate (max 20.4 kHz)");

    /* A config [mode] onto Analog under the row is held the same way. */
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    state.ui_msg[0] = '\0';
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_ANALOG, "row config analog");
    rc |= expect_int("row config analog: decoder stays digital", opts.analog_only, 0);
    rc |= expect_int("row config analog: front end asked with the width", g_analog_check_width_hz, 25000);
    rc |= expect_toast("row config analog toast", &state,
                       "Config not applied: NFM 25 kHz does not fit the 24 kHz DSP rate (max 20.4 kHz)");
    dsd_scan_mode_leave(&opts, &state);

    g_analog_check_result = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* For submit_config_nfm_width(): a [mode] section with no decode key (a demod or LRRP setting alone), which the loader
   marks present (has_mode) with decode_mode left unset. */
enum { SUBMIT_CONFIG_MODE_NO_DECODE = -1 };

/* A config with an [analog] width. @p mode DSDCFG_MODE_UNSET leaves [mode] out, SUBMIT_CONFIG_MODE_NO_DECODE gives one
   without a decode key, and any other value is its decode. */
static int
submit_config_nfm_width(dsd_opts* opts, dsd_state* state, int mode, int width_hz, const char* label) {
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_mode = mode != DSDCFG_MODE_UNSET;
    cfg.decode_mode = mode == SUBMIT_CONFIG_MODE_NO_DECODE ? DSDCFG_MODE_UNSET : (dsdneoUserDecodeMode)mode;
    cfg.has_analog = 1;
    cfg.analog_nfm_bandwidth_hz = width_hz;
    int rc = expect_int(label, dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof(cfg)),
                        DSD_APP_COMMAND_SUBMIT_QUEUED);
    state->ui_msg[0] = '\0';
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/*
 * A config that gives the analog monitor a new [analog] nfm_bandwidth_hz is held to the front end before anything is
 * applied, as the command is: a width it cannot filter leaves the whole config unapplied. An accepted one reaches a
 * running monitor as a width-only change, and a [mode] that moves onto the monitor asks with the new width.
 */
static int
test_config_apply_holds_nfm_width_to_the_front_end(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);

    rc |= submit_config_nfm_width(&opts, &state, DSDCFG_MODE_UNSET, 12500, "cfg nfm 12500");
    rc |= expect_int("cfg nfm 12500 applied", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("cfg nfm 12500 checked", g_analog_check_calls == 1 && g_analog_check_width_hz == 12500, 1);
    rc |= expect_int("cfg nfm 12500 requested", g_analog_req_calls == 1 && g_analog_req_width_hz == 12500, 1);

    reset_rx_family_wrap();
    g_analog_check_result = -1;
    /* A config left unapplied is no boundary: the received tone (issue #522) stays with its generation. */
    seed_received_tone(&state);
    const uint32_t seeded = state.analog_rx.generation;
    rc |= submit_config_nfm_width(&opts, &state, DSDCFG_MODE_ANALOG, 25000, "cfg nfm 25000");
    rc |= expect_int("cfg nfm 25000 not applied", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("cfg nfm 25000 not requested", g_analog_req_calls, 0);
    rc |= expect_toast("cfg nfm 25000 toast", &state,
                       "Config not applied: NFM 25 kHz does not fit the 24 kHz DSP rate (max 20.4 kHz); use a 48 kHz");
    rc |= expect_received_tone_kept("cfg nfm 25000 keeps the received tone", &state, seeded);
    /* A [mode] without a decode key applies no preset, so the session stays on the monitor and the width is held the
       same way: it is not a move onto a digital decoder that leaves the width unused. */
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_config_nfm_width(&opts, &state, SUBMIT_CONFIG_MODE_NO_DECODE, 25000, "cfg nfm 25000 no decode");
    rc |= expect_int("cfg nfm 25000 no decode: still analog", opts.analog_only, 1);
    rc |= expect_int("cfg nfm 25000 no decode: front end asked", g_analog_check_width_hz, 25000);
    rc |= expect_int("cfg nfm 25000 no decode: not applied", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("cfg nfm 25000 no decode: not requested", g_analog_req_calls, 0);
    rc |= expect_toast("cfg nfm 25000 no decode toast", &state,
                       "Config not applied: NFM 25 kHz does not fit the 24 kHz DSP rate (max 20.4 kHz); use a 48 kHz");
    g_analog_check_result = 0;

    /* The same width again is not a change: nothing to ask or request. */
    reset_rx_family_wrap();
    rc |= submit_config_nfm_width(&opts, &state, DSDCFG_MODE_ANALOG, 12500, "cfg nfm same");
    rc |= expect_int("cfg nfm same asks nothing", g_analog_check_calls + g_analog_req_calls, 0);

    /* From a digital session, a [mode] moving onto the monitor is asked about with the width it brings. */
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR);
    (void)dsd_app_drain_cmds(&opts, &state);
    reset_rx_family_wrap();
    rc |= submit_config_nfm_width(&opts, &state, DSDCFG_MODE_ANALOG, 16000, "cfg onto analog");
    rc |= expect_int("cfg onto analog asked with the new width", g_analog_check_width_hz, 16000);
    rc |= expect_int("cfg onto analog applied", opts.analog_only == 1 && opts.analog_nfm_bandwidth_hz == 16000, 1);
    rc |= expect_int("cfg onto analog requested with the new width", g_analog_req_width_hz, 16000);

    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * A config apply is a scoped command: the row on air is suspended while it runs and resumed after. The configured NFM
 * width is an acquisition setting of the analog family an untyped -fA row runs, so a config that changes only that
 * width reaches the front end with the profile the resume republishes. An nfm row that sets its own width keeps it
 * over the apply, and the front end is asked for nothing until the row leaves.
 */
static int
test_config_apply_width_under_scan_rows(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    rc |= expect_int("cfg inherit: row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_INHERIT), 0);
    rc |= expect_int("cfg inherit: options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    reset_rx_family_wrap();
    rc |= submit_config_nfm_width(&opts, &state, DSDCFG_MODE_UNSET, 12500, "cfg inherit");
    rc |= expect_int("cfg inherit: width in force", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("cfg inherit: row still scoped", dsd_scan_mode_configured_view(&state) != NULL, 1);
    rc |= expect_int("cfg inherit: requested at the new width",
                     g_analog_req_calls >= 1 && g_analog_req_width_hz == 12500, 1);
    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_int("cfg inherit: configured width kept", opts.analog_nfm_bandwidth_hz, 12500);

    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 11250;
    rc |= expect_int("cfg width row: nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM), 0);
    rc |= expect_int("cfg width row: its width", dsd_scan_mode_options(&opts, &state, &row), 0);
    reset_rx_family_wrap();
    rc |= submit_config_nfm_width(&opts, &state, DSDCFG_MODE_UNSET, 16000, "cfg width row");
    rc |= expect_int("cfg width row: row keeps its width", opts.analog_nfm_bandwidth_hz, 11250);
    const dsd_scan_settings* baseline = dsd_scan_mode_configured_view(&state);
    rc |=
        expect_int("cfg width row: baseline takes the apply", baseline ? baseline->analog_nfm_bandwidth_hz : -1, 16000);
    rc |= expect_int("cfg width row: front end not switched", g_analog_req_calls, 0);
    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_int("cfg width row: configured width after the row", opts.analog_nfm_bandwidth_hz, 16000);

    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* A config apply whose only [analog] change is the channel widths @p nfm_hz and @p am_hz, with no [mode]. */
static int
submit_config_widths(dsd_opts* opts, dsd_state* state, int nfm_hz, int am_hz, const char* label) {
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_analog = 1;
    cfg.analog_nfm_bandwidth_hz = nfm_hz;
    cfg.analog_am_bandwidth_hz = am_hz;
    int rc = expect_int(label, dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof(cfg)),
                        DSD_APP_COMMAND_SUBMIT_QUEUED);
    state->ui_msg[0] = '\0';
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/* A config that changed only a width the row on air does not filter with left the row alone: nothing republished
   and no acquisition reset (the received tone the row's monitor published is still there), with both widths
   configured as the config gives them, in dsd_opts as in the scope's baseline. */
static int
expect_row_left_alone(const char* label, const dsd_opts* opts, const dsd_state* state, uint32_t seeded, int nfm_hz,
                      int am_hz) {
    int rc = expect_int(label, g_analog_req_calls + g_demod_req_calls, 0);
    rc |= expect_received_tone_kept(label, state, seeded);
    const dsd_scan_settings* baseline = dsd_scan_mode_configured_view(state);
    rc |= expect_int(label, baseline != NULL, 1);
    if (baseline) {
        rc |= expect_int(label,
                         baseline->analog_nfm_bandwidth_hz == nfm_hz && baseline->analog_am_bandwidth_hz == am_hz, 1);
    }
    rc |= expect_int(label, opts->analog_nfm_bandwidth_hz == nfm_hz && opts->analog_am_bandwidth_hz == am_hz, 1);
    return rc;
}

/*
 * Issue #524: only the channel width of the analog kind the row on air runs is an acquisition setting there. A config
 * apply, a scoped command, that changes only the other kind's width -- am_bandwidth_hz under an nfm row or a blank row
 * of an -fA session, nfm_bandwidth_hz under a blank row of an AM session -- leaves the row alone, and the width waits in
 * the configuration for the next switch to its kind. One that changes the width in force still reaches the front end.
 */
static int
test_config_apply_idle_width_under_scan_rows(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    opts.analog_nfm_bandwidth_hz = 12500;

    rc |= expect_int("idle am, blank fm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_INHERIT), 0);
    rc |= expect_int("idle am, blank fm row: options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    reset_rx_family_wrap();
    seed_received_tone(&state);
    uint32_t seeded = state.analog_rx.generation;
    rc |= submit_config_widths(&opts, &state, 12500, 10000, "idle am, blank fm row: config");
    rc |= expect_row_left_alone("idle am, blank fm row: left alone", &opts, &state, seeded, 12500, 10000);
    dsd_scan_mode_leave(&opts, &state);

    rc |= expect_int("idle am, nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM), 0);
    rc |= expect_int("idle am, nfm row: options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    reset_rx_family_wrap();
    seed_received_tone(&state);
    seeded = state.analog_rx.generation;
    rc |= submit_config_widths(&opts, &state, 12500, 8000, "idle am, nfm row: config");
    rc |= expect_row_left_alone("idle am, nfm row: left alone", &opts, &state, seeded, 12500, 8000);
    /* The NFM width the row runs is in force: it reaches the front end. */
    reset_rx_family_wrap();
    rc |= submit_config_widths(&opts, &state, 16000, 8000, "nfm in force, nfm row: config");
    rc |= expect_int(
        "nfm in force, nfm row: requested at the new width",
        g_analog_req_calls >= 1 && g_analog_req_kind == DSD_ANALOG_DEMOD_FM && g_analog_req_width_hz == 16000, 1);
    dsd_scan_mode_leave(&opts, &state);

    /* An AM session's blank row runs the AM width: the NFM width is the idle one there. */
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, "idle nfm: am session");
    rc |= expect_int("idle nfm, blank am row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_INHERIT), 0);
    rc |= expect_int("idle nfm, blank am row: options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    rc |= expect_int("idle nfm, blank am row: runs AM", opts.analog_demod, DSD_ANALOG_DEMOD_AM);
    reset_rx_family_wrap();
    seed_received_tone(&state);
    seeded = state.analog_rx.generation;
    rc |= submit_config_widths(&opts, &state, 20000, 8000, "idle nfm, blank am row: config");
    rc |= expect_row_left_alone("idle nfm, blank am row: left alone", &opts, &state, seeded, 20000, 8000);
    reset_rx_family_wrap();
    rc |= submit_config_widths(&opts, &state, 20000, 10000, "am in force, blank am row: config");
    rc |= expect_int(
        "am in force, blank am row: requested at the new width",
        g_analog_req_calls >= 1 && g_analog_req_kind == DSD_ANALOG_DEMOD_AM && g_analog_req_width_hz == 10000, 1);
    dsd_scan_mode_leave(&opts, &state);

    opts.analog_nfm_bandwidth_hz = 0;
    opts.analog_am_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* An nfm row that takes the configured NFM width, which is @p width_hz, with the front end running that width. */
static int
enter_inheriting_nfm_row(dsd_opts* opts, dsd_state* state, int width_hz, const char* label) {
    opts->analog_nfm_bandwidth_hz = width_hz;
    int rc = expect_int(label, dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NFM), 0);
    rc |= expect_int(label, dsd_scan_mode_options(opts, state, NULL), 0);
    reset_rx_family_wrap();
    return rc;
}

/* After a refusal where the request landed: the configured width, the width the row runs, and the toast. */
static int
expect_refused_back_to(dsd_opts* opts, dsd_state* state, int width_hz, const char* label) {
    state->ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(opts, state);
    int rc = expect_int(label, dsd_scan_mode_configured_view(state)->analog_nfm_bandwidth_hz, width_hz);
    rc |= expect_int(label, opts->analog_nfm_bandwidth_hz, width_hz);
    rc |= expect_int(label, strncmp(state->ui_msg, "Refused: ", 9) == 0, 1);
    return rc;
}

/*
 * Issue #526: under a scan row that takes the configured NFM width, a width change the front end refuses where it lands
 * puts the configured width back to the one the front end runs, so the row runs what the receiver does. Two edits made
 * before the demod thread took either: the second replaced the first in the stream's queue, so the front end ran
 * neither, and the refusal of the second puts back the width from before both. Had the first landed, its width stands.
 * A config apply's [analog] width, which reaches the front end once the row's constraint is back, is put back the same
 * way.
 */
static int
test_refused_width_under_a_row_returns_to_the_width_run(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);

    rc |= enter_inheriting_nfm_row(&opts, &state, 8000, "replaced edit: row");
    rc |= submit_nfm_width(&opts, &state, 12500, "replaced edit: first");
    rc |= submit_nfm_width(&opts, &state, 20000, "replaced edit: second");
    rc |= expect_int("replaced edit: both requested", g_analog_req_calls == 2 && g_analog_req_width_hz == 20000, 1);
    demod_thread_refuses_analog_keeping(1, 8000);
    rc |= expect_refused_back_to(&opts, &state, 8000, "replaced edit: back to the width before both");
    dsd_scan_mode_leave(&opts, &state);

    rc |= enter_inheriting_nfm_row(&opts, &state, 8000, "landed edit: row");
    rc |= submit_nfm_width(&opts, &state, 12500, "landed edit: first");
    rc |= submit_nfm_width(&opts, &state, 20000, "landed edit: second");
    demod_thread_refuses_analog_keeping(1, 12500); /* the first landed before the second was queued */
    rc |= expect_refused_back_to(&opts, &state, 12500, "landed edit: the first one's width stands");
    dsd_scan_mode_leave(&opts, &state);

    rc |= enter_inheriting_nfm_row(&opts, &state, 8000, "config width: row");
    rc |= submit_config_nfm_width(&opts, &state, DSDCFG_MODE_UNSET, 12500, "config width: apply");
    rc |= expect_int("config width: requested once the row is back",
                     g_analog_req_calls >= 1 && g_analog_req_width_hz == 12500, 1);
    demod_thread_refuses_analog_keeping(1, 8000);
    rc |= expect_refused_back_to(&opts, &state, 8000, "config width: back to the width before the apply");
    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_int("config width: the leave keeps it", opts.analog_nfm_bandwidth_hz, 8000);

    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* A config whose [input] builds an RTL-SDR input at DSP bandwidth @p rtl_bw_khz, with an [analog] width when
   @p width_hz is not negative. */
static int
submit_config_rtl_bw(dsd_opts* opts, dsd_state* state, int rtl_bw_khz, int width_hz, const char* label) {
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTL;
    DSD_SNPRINTF(cfg.rtl_freq, sizeof cfg.rtl_freq, "%s", "146.52M");
    cfg.rtl_bw_khz = rtl_bw_khz;
    cfg.has_analog = width_hz >= 0;
    cfg.analog_nfm_bandwidth_hz = width_hz >= 0 ? width_hz : 0;
    int rc = expect_int(label, dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof(cfg)),
                        DSD_APP_COMMAND_SUBMIT_QUEUED);
    state->ui_msg[0] = '\0';
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/*
 * A config that moves the RTL DSP bandwidth reopens the device at the new rate, so an explicit NFM width is held to
 * that rate, not to the one the session runs at now: a config raising the bandwidth for a wider width applies, and one
 * lowering it under the width in use is refused as RTL_SET_BW is, instead of failing the reopen and leaving no stream.
 */
static int
test_config_apply_holds_nfm_width_to_a_new_dsp_bandwidth(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);

    /* No stream (the reopen's failure is the wrapped stream create's, not the width's): 24 -> 48 kHz with 25 kHz. */
    state.rtl_ctx = NULL;
    g_analog_check_result = -1; /* what a 24 kHz front end would answer, if it were asked */
    rc |= submit_config_rtl_bw(&opts, &state, 48, 25000, "cfg 24->48 with 25 kHz");
    rc |= expect_int("cfg 24->48: front end not asked", g_analog_check_calls, 0);
    rc |= expect_int("cfg 24->48: width applied", opts.analog_nfm_bandwidth_hz, 25000);
    rc |= expect_int("cfg 24->48: rate applied", opts.rtl_dsp_bw_khz, 48);
    g_analog_check_result = 0;

    /* A running stream at 48 kHz with an explicit 16 kHz: a config lowering the bandwidth to 16 kHz is refused before
       anything changes (nothing reopens), whatever the running front end says of the width at its own rate. */
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    opts.analog_nfm_bandwidth_hz = 16000;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:146.52M:0:0:48");
    char dev_before[sizeof opts.audio_in_dev];
    DSD_SNPRINTF(dev_before, sizeof dev_before, "%s", opts.audio_in_dev);
    reset_rx_family_wrap();
    rc |= submit_config_rtl_bw(&opts, &state, 16, -1, "cfg 48->16 under 16 kHz");
    rc |= expect_int("cfg 48->16: refused, rate kept", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_int("cfg 48->16: width kept", opts.analog_nfm_bandwidth_hz, 16000);
    rc |= expect_str("cfg 48->16: input unchanged", opts.audio_in_dev, dev_before);
    rc |= expect_int("cfg 48->16: stream kept", state.rtl_ctx == (RtlSdrContext*)fake_ctx, 1);
    rc |= expect_int("cfg 48->16: front end not asked", g_analog_check_calls, 0);
    rc |= expect_toast("cfg 48->16 toast", &state,
                       "Config not applied: NFM 16 kHz does not fit the 16 kHz DSP rate (max 13.2 kHz); use a 24 or 48 "
                       "kHz DSP bandwidth");

    /* The default keeps its historical rule at any rate: nothing to hold. */
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    rc |= submit_config_rtl_bw(&opts, &state, 12, -1, "cfg 48->12 default width");
    rc |= expect_int("cfg 48->12 default width: rate applied", opts.rtl_dsp_bw_khz, 12);

    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * What a config's [input] does to the rate is what its hot restart does: a reopen only when it builds an RTL-SDR or
 * rtl_tcp spec other than the running input's, at rtl_bw_khz as the config gives it. An unsupported value (20) is the
 * rate the reopened device runs at; an RTL spec over a running Airspy reopens an RTL-SDR even at the rtl_bw_khz the
 * session already has, where the Airspy ran at the rate it forced; and an rtl_tcp source that names the host and port
 * in use without rtl_freq reopens nothing, so its rtl_bw_khz changes no rate and refuses nothing.
 */
static int
test_config_apply_holds_nfm_width_to_the_rate_the_reopen_runs_at(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);

    /* rtl_bw_khz = 20 reopens at 20 kHz, which cannot filter 20 kHz (the reopen would fail and leave no stream). */
    opts.analog_nfm_bandwidth_hz = 20000;
    opts.rtl_dsp_bw_khz = 48;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:146.52M:0:0:48");
    reset_rx_family_wrap();
    rc |= submit_config_rtl_bw(&opts, &state, 20, -1, "cfg rtl_bw_khz 20");
    rc |= expect_int("cfg rtl_bw_khz 20: refused, rate kept", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_toast("cfg rtl_bw_khz 20 toast", &state,
                       "Config not applied: NFM 20 kHz does not fit the 20 kHz DSP rate (max 16.8 kHz); use a 24 or 48 "
                       "kHz DSP bandwidth");
    /* The loader keeps any integer for rtl_bw_khz (its range only warns). One too large for a rate in Hz is held as
       the saturated rate, which no width fits, rather than overflowing into a negative one, and the refusal names the
       setting rather than a DSP rate built from it. So does one above every selectable bandwidth that no width fits
       (the channel filter would need more taps than it has). */
    rc |= submit_config_rtl_bw(&opts, &state, 3000000, -1, "cfg rtl_bw_khz 3000000");
    rc |= expect_int("cfg rtl_bw_khz 3000000: refused, rate kept", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_toast("cfg rtl_bw_khz 3000000 toast", &state,
                       "Config not applied: rtl_bw_khz 3000000 is not a DSP bandwidth; use a 24 or 48 kHz DSP "
                       "bandwidth");
    rc |= submit_config_rtl_bw(&opts, &state, 200, -1, "cfg rtl_bw_khz 200");
    rc |= expect_int("cfg rtl_bw_khz 200: refused, rate kept", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_toast("cfg rtl_bw_khz 200 toast", &state,
                       "Config not applied: rtl_bw_khz 200 is not a DSP bandwidth; use a 24 or 48 kHz DSP bandwidth");

    /* An Airspy running an explicit 25 kHz at its forced rate; a config moving to an RTL-SDR at the same 24 kHz
       rtl_bw_khz value would reopen at 24 kHz, which cannot filter it. */
    opts.analog_nfm_bandwidth_hz = 25000;
    opts.rtl_dsp_bw_khz = 24;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
    reset_rx_family_wrap();
    rc |= submit_config_rtl_bw(&opts, &state, 24, -1, "cfg airspy to rtl");
    rc |= expect_str("cfg airspy to rtl: input unchanged", opts.audio_in_dev, "airspy");
    rc |= expect_int("cfg airspy to rtl: front end not asked", g_analog_check_calls, 0);
    rc |= expect_toast("cfg airspy to rtl toast", &state,
                       "Config not applied: NFM 25 kHz does not fit the 24 kHz DSP rate (max 20.4 kHz); use a 48 kHz "
                       "DSP bandwidth");

    /* rtl_tcp at the host and port in use, no rtl_freq: nothing reopens, so the rtl_bw_khz it carries moves nothing. */
    opts.rtl_dsp_bw_khz = 48;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtltcp:127.0.0.1:1234");
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTLTCP;
    DSD_SNPRINTF(cfg.rtltcp_host, sizeof cfg.rtltcp_host, "%s", "127.0.0.1");
    cfg.rtltcp_port = 1234;
    cfg.rtl_bw_khz = 24;
    reset_rx_family_wrap();
    rc |= expect_int("cfg same rtl_tcp queued", dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof(cfg)),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    state.ui_msg[0] = '\0';
    rc |= expect_int("cfg same rtl_tcp drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("cfg same rtl_tcp: not refused", strstr(state.ui_msg, "Config not applied") == NULL, 1);
    rc |= expect_str("cfg same rtl_tcp: input unchanged", opts.audio_in_dev, "rtltcp:127.0.0.1:1234");
    rc |= expect_int("cfg same rtl_tcp: rate unchanged", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_int("cfg same rtl_tcp: width unchanged", opts.analog_nfm_bandwidth_hz, 25000);

    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* A config whose [input] reopens a SoapySDR or Airspy device (source @p source) at rtl_bw_khz @p rtl_bw_khz, with an
   [analog] NFM width. */
static int
submit_config_device_source(dsd_opts* opts, dsd_state* state, dsdneoUserInputSource source, int rtl_bw_khz,
                            int width_hz, const char* label) {
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = source;
    if (source == DSDCFG_INPUT_AIRSPY) {
        dsd_airspy_config_defaults(&cfg.airspy);
    } else {
        DSD_SNPRINTF(cfg.soapy_args, sizeof cfg.soapy_args, "%s", "driver=airspy");
    }
    cfg.rtl_bw_khz = rtl_bw_khz;
    cfg.has_analog = 1;
    cfg.analog_nfm_bandwidth_hz = width_hz;
    int rc = expect_int(label, dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof(cfg)),
                        DSD_APP_COMMAND_SUBMIT_QUEUED);
    state->ui_msg[0] = '\0';
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/* Submit @p cfg as a config apply and drain it, as the decoder does. */
static int
submit_config(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg, const char* label) {
    int rc = expect_int(label, dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, cfg, sizeof(*cfg)),
                        DSD_APP_COMMAND_SUBMIT_QUEUED);
    state->ui_msg[0] = '\0';
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/*
 * A config whose [input] reopens a SoapySDR or Airspy device over the running RTL-SDR runs an explicit NFM width at the
 * rate that device delivers (an Airspy's 78,125 Hz, say), which neither the running input's DSP rate nor rtl_bw_khz
 * gives. It is held to neither: the reopened stream's start checks the width at the delivered rate. An Airspy source
 * over a running Airspy reopens nothing (it applies live), so the running front end still holds it, as it does for one
 * that reopens the Airspy for its monitor volume alone (issue #578): the device delivers the rate it runs now.
 */
static int
test_config_apply_leaves_a_soapy_or_airspy_reopen_to_its_start(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    /* No stream (the reopen's failure is the wrapped stream create's, not the width's): the 24 kHz RTL-SDR input's DSP
       bandwidth cannot filter 25 kHz, and would refuse it if it were the rate. */
    state.rtl_ctx = NULL;
    g_analog_check_result = -1;
    rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_AIRSPY, 0, 25000, "cfg airspy over rtl");
    rc |= expect_int("cfg airspy over rtl: not refused", strstr(state.ui_msg, "Config not applied") == NULL, 1);
    rc |= expect_int("cfg airspy over rtl: front end not asked", g_analog_check_calls, 0);
    rc |= expect_int("cfg airspy over rtl: width applied", opts.analog_nfm_bandwidth_hz, 25000);
    rc |= expect_str("cfg airspy over rtl: reopened as the Airspy", opts.audio_in_dev, "airspy");

    /* A SoapySDR source at rtl_bw_khz 48, over the same 24 kHz RTL-SDR input. */
    opts.analog_nfm_bandwidth_hz = 0;
    opts.rtl_dsp_bw_khz = 24;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:24");
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_SOAPY, 48, 25000, "cfg soapy over rtl");
    rc |= expect_int("cfg soapy over rtl: not refused", strstr(state.ui_msg, "Config not applied") == NULL, 1);
    rc |= expect_int("cfg soapy over rtl: width applied", opts.analog_nfm_bandwidth_hz, 25000);
    rc |= expect_str("cfg soapy over rtl: reopened as the SoapySDR device", opts.audio_in_dev, "soapy:driver=airspy");

    /* An Airspy source over a running Airspy reopens nothing: the running front end holds the new width. */
    opts.analog_nfm_bandwidth_hz = 12500;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_AIRSPY, 0, 25000, "cfg airspy over airspy");
    rc |= expect_int("cfg airspy over airspy: front end asked",
                     g_analog_check_calls == 1 && g_analog_check_width_hz == 25000, 1);
    rc |= expect_int("cfg airspy over airspy: width kept", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("cfg airspy over airspy: refused", strstr(state.ui_msg, "Config not applied") != NULL, 1);

    /* ...unless it moves the DSP bandwidth (12 -> 48 kHz), which the Airspy path applies by reopening the device, at
       the rate it delivers for the new bandwidth: an Airspy's 2.5 MS/s capture is decimated to 19,531 Hz at 12 kHz but
       to 78,125 Hz at 48 kHz. The running front end's rate says nothing about that, so the width waits for the reopened
       stream's start, as it does for any SoapySDR or Airspy reopen. That start takes it here (one that refuses it puts
       the running input back, test_config_reopen_that_fails_keeps_the_running_input()). */
    opts.rtl_dsp_bw_khz = 12;
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    g_config_rtl_open_ok = 1;
    rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_AIRSPY, 48, 25000, "cfg airspy 12->48 kHz");
    rc |= expect_int("cfg airspy 12->48 kHz: front end not asked", g_analog_check_calls, 0);
    rc |= expect_int("cfg airspy 12->48 kHz: not refused", strstr(state.ui_msg, "Config not applied") == NULL, 1);
    rc |= expect_int("cfg airspy 12->48 kHz: width applied", opts.analog_nfm_bandwidth_hz, 25000);
    rc |= expect_int("cfg airspy 12->48 kHz: reopened", g_config_rtl_creates == 1 && state.rtl_ctx != NULL, 1);

    /* The issue's own case: a running Airspy at 12 kHz (19,531 Hz) on NFM 12.5 kHz, and a config that changes only the
       monitor volume with a 25 kHz width. The volume is copied when the stream opens, so the Airspy reopens for it, but
       at the rate it runs now: the running front end holds the width up front, and nothing is applied. */
    opts.analog_nfm_bandwidth_hz = 12500;
    opts.rtl_dsp_bw_khz = 12;
    opts.rtl_volume_multiplier = 2;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    g_fake_demod_rate_hz = 19531;
    g_config_rtl_open_ok = 1;
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_AIRSPY;
    cfg.airspy = opts.airspy;
    cfg.rtl_bw_khz = 12;
    cfg.rtl_volume = 3;
    cfg.has_analog = 1;
    cfg.analog_nfm_bandwidth_hz = 25000;
    rc |= submit_config(&opts, &state, &cfg, "cfg airspy volume only");
    rc |= expect_int("cfg airspy volume only: front end asked",
                     g_analog_check_calls == 1 && g_analog_check_width_hz == 25000, 1);
    rc |= expect_int("cfg airspy volume only: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_toast("cfg airspy volume only toast", &state,
                       "Config not applied: NFM 25 kHz does not fit the 19.531 kHz DSP rate");
    rc |= expect_int("cfg airspy volume only: nothing reopened", g_config_rtl_creates, 0);
    rc |= expect_int("cfg airspy volume only: stream kept", state.rtl_ctx == (RtlSdrContext*)fake_ctx, 1);
    rc |= expect_int("cfg airspy volume only: width kept", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("cfg airspy volume only: volume kept", opts.rtl_volume_multiplier, 2);

    reset_config_rtl_wrap();
    g_fake_demod_rate_hz = 0;
    g_analog_check_result = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #578: an Airspy reopened for its monitor volume alone runs at the rate it runs now, on the configured settings
 * the apply leaves (it runs inside a scan row's suspended scope), so the running front end holds the configured width
 * up front even when the config leaves it unchanged. An Analog-configured Airspy at a 12 kHz DSP bandwidth (19,531 Hz)
 * scans a typed NXDN48 row, whose digital profile the stream runs, with a configured NFM width of 25 kHz that rate
 * cannot filter (the DSP bandwidth was lowered while the row ran). A config that changes the volume and the frequency,
 * with no [analog], would reopen the Airspy on that width, which its start refuses: it is refused first, naming the
 * width and the rate, with nothing applied and nothing reopened. A configured width the running rate filters reopens
 * the Airspy for the new volume as before.
 */
static int
test_config_volume_only_reopen_holds_the_unchanged_width(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
    dsd_airspy_config_defaults(&opts.airspy);
    opts.rtl_dsp_bw_khz = 12;
    opts.rtl_volume_multiplier = 2;
    opts.rtlsdr_center_freq = 851375000U;
    opts.analog_nfm_bandwidth_hz = 25000;
    opts.scanner_mode = 1;
    rc |= expect_int("volume only under a row: typed row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NXDN48), 0);
    rc |= expect_int("volume only under a row: row options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    reset_rx_family_wrap();
    g_fake_demod_rate_hz = 19531;
    g_config_rtl_open_ok = 1;
    g_config_rtl_device_rate_hz = 19531;
    g_analog_check_result = -1;
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_AIRSPY;
    cfg.airspy = opts.airspy;
    cfg.rtl_bw_khz = 12;
    cfg.rtl_volume = 3;
    DSD_SNPRINTF(cfg.rtl_freq, sizeof cfg.rtl_freq, "%s", "852.5M");
    rc |= submit_config(&opts, &state, &cfg, "volume only under a row");
    rc |= expect_int(
        "volume only under a row: the front end asked",
        g_analog_check_calls == 1 && g_analog_check_kind == DSD_ANALOG_DEMOD_FM && g_analog_check_width_hz == 25000, 1);
    rc |= expect_int("volume only under a row: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_toast("volume only under a row: toast", &state,
                       "Config not applied: NFM 25 kHz does not fit the 19.531 kHz DSP rate");
    rc |= expect_int("volume only under a row: nothing reopened", g_config_rtl_creates, 0);
    rc |= expect_int("volume only under a row: stream kept", state.rtl_ctx == (RtlSdrContext*)fake_ctx, 1);
    rc |= expect_int("volume only under a row: volume kept", opts.rtl_volume_multiplier, 2);
    rc |= expect_int("volume only under a row: frequency kept", opts.rtlsdr_center_freq == 851375000U, 1);
    rc |= expect_int("volume only under a row: the row still on air",
                     dsd_scan_mode_active(&state) == DSD_SCAN_MODE_NXDN48 && opts.analog_only == 0, 1);
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(&state);
    rc |= expect_int("volume only under a row: width kept", configured && configured->analog_nfm_bandwidth_hz == 25000,
                     1);

    /* A configured 12.5 kHz the running rate filters: the Airspy reopens for the volume. */
    (void)dsd_scan_mode_set_configured_analog_width(&opts, &state, DSD_ANALOG_DEMOD_FM, 12500);
    reset_rx_family_wrap();
    g_fake_demod_rate_hz = 19531;
    g_config_rtl_open_ok = 1;
    g_config_rtl_device_rate_hz = 19531;
    rc |= submit_config(&opts, &state, &cfg, "volume only, width fits");
    rc |= expect_int("volume only, width fits: the front end asked",
                     g_analog_check_calls == 1 && g_analog_check_width_hz == 12500, 1);
    rc |= expect_int("volume only, width fits: applied", dsd_app_command_test_last_failed(), 0);
    rc |= expect_int("volume only, width fits: reopened", g_config_rtl_creates == 1 && state.rtl_ctx != NULL, 1);
    rc |= expect_int("volume only, width fits: volume applied", opts.rtl_volume_multiplier, 3);

    dsd_scan_mode_leave(&opts, &state);
    reset_config_rtl_wrap();
    g_fake_demod_rate_hz = 0;
    g_analog_check_result = 0;
    opts.scanner_mode = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* The stream the rollback started runs the input the config apply found: the second create was handed @p dev, and the
   session has a stream again. */
static int
expect_restarted_on(const char* label, const dsd_opts* opts, const dsd_state* state, const char* dev) {
    int rc = expect_int(label, g_config_rtl_creates == 2 && g_config_rtl_starts == 2, 1);
    rc |= expect_str(label, g_config_rtl_create_dev, dev);
    rc |= expect_str(label, opts->audio_in_dev, dev);
    rc |= expect_int(label, state->rtl_ctx != NULL && opts->rtl_started == 1, 1);
    return rc;
}

/*
 * Issue #578: a config apply whose stream start fails never loses the running input. A reopen onto a SoapySDR or Airspy
 * device is held only to the rules every rate shares before it commits, so its start can still refuse the width at the
 * rate the device delivers; any start can fail for its device. Either way the apply puts back the input and its device
 * settings, the tuning, [mode], [demod] and the [analog] widths, restarts the input that ran, says why ("Config not
 * applied: ...") and fails. Nothing is published to the front end for the settings it refused.
 */
static int
test_config_reopen_that_fails_keeps_the_running_input(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;

    /* An -fA session on an RTL-SDR at 24 kHz with NFM 12.5 kHz; a config opens a SoapySDR device with a 25 kHz width,
       and its start refuses 25 kHz at the 24 kHz rate the device delivered. */
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    opts.analog_nfm_bandwidth_hz = 12500;
    reset_rx_family_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    g_config_rtl_refuse_rate_hz = 24000;
    rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_SOAPY, 0, 25000, "cfg soapy refused");
    rc |= expect_int("cfg soapy refused: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_toast("cfg soapy refused toast", &state,
                       "Config not applied: NFM 25 kHz does not fit the 24 kHz DSP rate (max 20.4 kHz)");
    rc |= expect_restarted_on("cfg soapy refused: back on the RTL-SDR", &opts, &state, "rtl:0:851.375M:0:0:24");
    rc |= expect_int("cfg soapy refused: restarted on 12.5 kHz", g_config_rtl_create_width_hz, 12500);
    rc |= expect_int("cfg soapy refused: width put back", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("cfg soapy refused: rate put back", opts.rtl_dsp_bw_khz, 24);
    rc |= expect_int("cfg soapy refused: nothing published", g_analog_req_calls, 0);

    /* A DMR session on the same RTL-SDR with a stored 25 kHz; a config opens an Airspy with [mode] analog, and the
       Airspy's start refuses the width at 19,531 Hz. The session goes back to DMR, and the stream it restarts runs
       the digital family. */
    rc |=
        expect_int("dmr session queued", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR),
                   DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("dmr session drained", dsd_app_drain_cmds(&opts, &state), 1);
    opts.analog_nfm_bandwidth_hz = 25000;
    reset_rx_family_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    g_config_rtl_refuse_rate_hz = 19531;
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_AIRSPY;
    dsd_airspy_config_defaults(&cfg.airspy);
    cfg.has_mode = 1;
    cfg.decode_mode = DSDCFG_MODE_ANALOG;
    rc |= submit_config(&opts, &state, &cfg, "cfg airspy onto analog refused");
    rc |= expect_int("cfg airspy onto analog refused: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_toast("cfg airspy onto analog refused toast", &state,
                       "Config not applied: NFM 25 kHz does not fit the 19.531 kHz DSP rate (max 16.377 kHz)");
    rc |= expect_restarted_on("cfg airspy onto analog refused: back on the RTL-SDR", &opts, &state,
                              "rtl:0:851.375M:0:0:24");
    rc |= expect_int("cfg airspy onto analog refused: restarted off the analog family", g_config_rtl_create_analog_only,
                     0);
    rc |= expect_int("cfg airspy onto analog refused: back to DMR",
                     dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_DMR && opts.analog_only == 0, 1);
    rc |= expect_int("cfg airspy onto analog refused: width kept", opts.analog_nfm_bandwidth_hz, 25000);
    rc |= expect_int("cfg airspy onto analog refused: nothing published", g_analog_req_calls + g_demod_req_calls, 0);
    rc |= expect_int("cfg airspy onto analog refused: no analog sink", g_ensure_analog_calls, 0);

    /* A running rtl_tcp input and a config naming another host, whose start fails for the connection: no width to
       blame, so the reason points at the log. */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtltcp:127.0.0.1:1234");
    opts.rtltcp_enabled = 1;
    DSD_SNPRINTF(opts.rtltcp_hostname, sizeof opts.rtltcp_hostname, "%s", "127.0.0.1");
    opts.rtltcp_portno = 1234;
    opts.rtl_squelch_level = dsd_squelch_level_from_sql(-60.0);
    reset_rx_family_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTLTCP;
    DSD_SNPRINTF(cfg.rtltcp_host, sizeof cfg.rtltcp_host, "%s", "192.0.2.7");
    cfg.rtltcp_port = 1234;
    rc |= submit_config(&opts, &state, &cfg, "cfg rtl_tcp host fails");
    rc |= expect_int("cfg rtl_tcp host fails: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_str("cfg rtl_tcp host fails toast", state.ui_msg,
                     "Config not applied: the rtl_tcp input did not start (see log)");
    rc |=
        expect_restarted_on("cfg rtl_tcp host fails: back on the running host", &opts, &state, "rtltcp:127.0.0.1:1234");
    rc |= expect_str("cfg rtl_tcp host fails: host put back", opts.rtltcp_hostname, "127.0.0.1");
    rc |= expect_int("cfg rtl_tcp host fails: rtl_tcp kept", opts.rtltcp_enabled == 1 && opts.rtltcp_portno == 1234, 1);
    rc |= expect_true("cfg rtl_tcp host fails: squelch put back",
                      fabs(opts.rtl_squelch_level - dsd_squelch_level_from_sql(-60.0)) < 1e-12);

    reset_config_rtl_wrap();
    opts.rtltcp_enabled = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #578: with no stream running before it, a config whose [input] names a radio input still opens that input, as
 * it always has. When that start fails there is no input to put back: the config stays applied with no input running,
 * the message says why, and the apply fails, as a failed Input > Switch source does. An Airspy with no stream opens
 * the config's settings the same way, rather than starting the settings it replaced.
 */
static int
test_config_reopen_with_no_stream_reports_the_failed_start(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;

    /* An -fA session on an RTL-SDR at 24 kHz with no stream, and a SoapySDR device whose start refuses 25 kHz. */
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    opts.analog_nfm_bandwidth_hz = 12500;
    state.rtl_ctx = NULL;
    reset_rx_family_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    g_config_rtl_refuse_rate_hz = 24000;
    rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_SOAPY, 0, 25000, "no stream soapy refused");
    rc |= expect_int("no stream soapy refused: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_toast("no stream soapy refused toast", &state,
                       "Config applied; no input running: NFM 25 kHz does not fit the 24 kHz DSP rate");
    rc |= expect_int("no stream soapy refused: tried once", g_config_rtl_creates == 1 && g_config_rtl_starts == 1, 1);
    rc |= expect_int("no stream soapy refused: no stream", state.rtl_ctx == NULL, 1);
    rc |= expect_str("no stream soapy refused: config's input kept", opts.audio_in_dev, "soapy:driver=airspy");
    rc |= expect_int("no stream soapy refused: config's width kept", opts.analog_nfm_bandwidth_hz, 25000);

    /* An rtl_tcp input with no stream, and a config naming another host, whose start fails for the connection. */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtltcp:127.0.0.1:1234");
    opts.rtltcp_enabled = 1;
    DSD_SNPRINTF(opts.rtltcp_hostname, sizeof opts.rtltcp_hostname, "%s", "127.0.0.1");
    opts.rtltcp_portno = 1234;
    opts.analog_nfm_bandwidth_hz = 12500;
    reset_rx_family_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTLTCP;
    DSD_SNPRINTF(cfg.rtltcp_host, sizeof cfg.rtltcp_host, "%s", "192.0.2.7");
    cfg.rtltcp_port = 1234;
    rc |= submit_config(&opts, &state, &cfg, "no stream rtl_tcp fails");
    rc |= expect_int("no stream rtl_tcp fails: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_str("no stream rtl_tcp fails toast", state.ui_msg,
                     "Config applied; no input running: the rtl_tcp input did not start (see log)");
    rc |= expect_int("no stream rtl_tcp fails: tried once", g_config_rtl_creates, 1);
    rc |= expect_str("no stream rtl_tcp fails: config's host kept", opts.rtltcp_hostname, "192.0.2.7");

    /* An Airspy with no stream, and a config with a new sample rate, whose start fails. */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
    opts.rtltcp_enabled = 0;
    dsd_airspy_config_defaults(&opts.airspy);
    reset_rx_family_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_AIRSPY;
    cfg.airspy = opts.airspy;
    cfg.airspy.sample_rate = 2500000;
    rc |= submit_config(&opts, &state, &cfg, "no stream airspy fails");
    rc |= expect_int("no stream airspy fails: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_str("no stream airspy fails toast", state.ui_msg,
                     "Config applied; no input running: the Airspy input did not start (see log)");
    rc |= expect_int("no stream airspy fails: tried once", g_config_rtl_creates, 1);
    rc |= expect_int("no stream airspy fails: config's rate kept", (int)opts.airspy.sample_rate, 2500000);
    rc |= expect_int("no stream airspy fails: no stream", state.rtl_ctx == NULL, 1);

    reset_config_rtl_wrap();
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #578: a config rollback puts back everything the config's [mode], [demod] and [analog] set that the decoder
 * reads, not only the settings a scan row snapshots. An EDACS-EA session with ESK, two extra LRRP ports and no tone
 * filter loads a config for standard EDACS, other LRRP ports and a tone filter, whose rtl_tcp host does not start: the
 * session keeps its EDACS variant, its LRRP ports and its tone policy with the input that ran.
 */
static int
test_config_rollback_restores_mode_owned_settings(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    rc |= expect_int(
        "edacs session",
        dsd_apply_decode_mode_preset(DSDCFG_MODE_EDACS_PV, DSD_DECODE_PRESET_PROFILE_CONFIG, &opts, &state), 0);
    state.ea_mode = 1;
    state.esk_mask = 0xA0;
    opts.lrrp_extra_ports[0] = 4001;
    opts.lrrp_extra_ports[1] = 4002;
    opts.lrrp_extra_port_count = 2;
    opts.analog_tone_filter = DSD_TONE_FILTER_OFF;
    DSD_MEMSET(&opts.analog_tone_set, 0, sizeof opts.analog_tone_set);
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtltcp:127.0.0.1:1234");
    opts.rtltcp_enabled = 1;
    DSD_SNPRINTF(opts.rtltcp_hostname, sizeof opts.rtltcp_hostname, "%s", "127.0.0.1");
    opts.rtltcp_portno = 1234;
    opts.rtl_dsp_bw_khz = 48;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    reset_rx_family_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;

    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTLTCP;
    DSD_SNPRINTF(cfg.rtltcp_host, sizeof cfg.rtltcp_host, "%s", "192.0.2.7");
    cfg.rtltcp_port = 1234;
    cfg.has_mode = 1;
    cfg.decode_mode = DSDCFG_MODE_EDACS_PV;
    cfg.has_edacs_variant = 1;
    cfg.edacs_ea = 0;
    cfg.edacs_esk = 0;
    DSD_SNPRINTF(cfg.dmr_lrrp_ports, sizeof cfg.dmr_lrrp_ports, "%s", "4005");
    cfg.has_analog = 1;
    cfg.analog_tone_filter = DSD_TONE_FILTER_ALLOW;
    rc |= expect_int("mode-owned: tone list", dsd_tone_set_parse("100.0", &cfg.analog_tone_set, NULL, 0), 0);
    rc |= submit_config(&opts, &state, &cfg, "mode-owned rollback");
    rc |= expect_int("mode-owned rollback: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_str("mode-owned rollback toast", state.ui_msg,
                     "Config not applied: the rtl_tcp input did not start (see log)");
    rc |= expect_restarted_on("mode-owned rollback: back on the running host", &opts, &state, "rtltcp:127.0.0.1:1234");
    rc |= expect_int("mode-owned rollback: EDACS-EA kept", state.ea_mode, 1);
    rc |= expect_int("mode-owned rollback: ESK kept", state.esk_mask, 0xA0);
    rc |= expect_int(
        "mode-owned rollback: LRRP ports kept",
        opts.lrrp_extra_port_count == 2 && opts.lrrp_extra_ports[0] == 4001 && opts.lrrp_extra_ports[1] == 4002, 1);
    rc |=
        expect_int("mode-owned rollback: tone filter kept off",
                   opts.analog_tone_filter == DSD_TONE_FILTER_OFF && dsd_tone_set_count(&opts.analog_tone_set) == 0, 1);
    rc |= expect_int("mode-owned rollback: still EDACS",
                     dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_EDACS_PV && opts.frame_provoice == 1, 1);

    reset_config_rtl_wrap();
    opts.rtltcp_enabled = 0;
    opts.lrrp_extra_port_count = 0;
    state.ea_mode = 0;
    state.esk_mask = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #578: every stream start opens the I/Q capture (--iq-capture) anew, writing over the file. A config whose
 * reopen fails has already stopped the stream that ran, which closed the capture with what it had recorded, so the
 * restart that puts that input back runs without the capture: it stays off for the rest of the session, the log says
 * so, and the message does when it fits.
 */
static int
test_config_rollback_keeps_the_iq_capture(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    opts.analog_nfm_bandwidth_hz = 12500;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtltcp:127.0.0.1:1234");
    opts.rtltcp_enabled = 1;
    DSD_SNPRINTF(opts.rtltcp_hostname, sizeof opts.rtltcp_hostname, "%s", "127.0.0.1");
    opts.rtltcp_portno = 1234;
    opts.iq_capture_requested = 1;
    DSD_SNPRINTF(opts.iq_capture_path, sizeof opts.iq_capture_path, "%s", "cap.iq");
    reset_rx_family_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    g_capture_log[0] = '\0';
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTLTCP;
    DSD_SNPRINTF(cfg.rtltcp_host, sizeof cfg.rtltcp_host, "%s", "192.0.2.7");
    cfg.rtltcp_port = 1234;
    rc |= submit_config(&opts, &state, &cfg, "capture cfg rtl_tcp host fails");
    rc |= expect_capture_stopped("capture cfg rtl_tcp host fails: recording kept", &opts, 0);
    rc |= expect_str("capture cfg rtl_tcp host fails: host put back", opts.audio_in_dev, "rtltcp:127.0.0.1:1234");
    rc |= expect_str("capture cfg rtl_tcp host fails toast", state.ui_msg,
                     "Config not applied: the rtl_tcp input did not start (see log); I/Q capture stopped");

    reset_config_rtl_wrap();
    opts.rtltcp_enabled = 0;
    opts.iq_capture_requested = 0;
    opts.iq_capture_path[0] = '\0';
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * A CQPSK toggle the stream has not settled stands until it has: the demod thread may have cleared the output for it
 * (moving the output generation) without publishing CQPSK on yet, or a retune may have come between, and the stream
 * still publishes CQPSK off. A width set then must wait for the monitor rather than queue an analog request that would
 * undo the toggle. Once the toggle has landed, the published state decides.
 */
static int
test_nfm_width_waits_for_an_unsettled_cqpsk_toggle(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    opts.analog_nfm_bandwidth_hz = 16000;

    reset_rx_family_wrap();
    g_fake_cqpsk = 0;
    submit_cqpsk_toggle();
    rc |= expect_int("unsettled toggle drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("unsettled toggle: CQPSK requested on", g_demod_req_calls == 1 && g_demod_req_cqpsk == 1, 1);
    /* A later drain: the toggle is still pending, and the stream still publishes CQPSK off. */
    rc |= submit_nfm_width(&opts, &state, 12500, "width while the toggle is pending");
    rc |= expect_int("pending toggle: width stored", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("pending toggle: the toggle is not undone", g_analog_req_calls, 0);
    /* Landed: CQPSK on is published, and still holds the width back. */
    demod_thread_lands(1);
    rc |= submit_nfm_width(&opts, &state, 20000, "width once the toggle landed");
    rc |= expect_int("landed toggle: the toggle is not undone", g_analog_req_calls, 0);
    /* Turning CQPSK off returns to the monitor with the width set meanwhile. */
    submit_cqpsk_toggle();
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("toggle off: back to the monitor with the width",
                     g_analog_req_calls == 1 && g_analog_req_width_hz == 20000, 1);

    g_fake_cqpsk = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * A width the front end took when asked can still be refused: by the request itself, when a retune moved the rate
 * between the check and the request, or by the demod thread where the request lands, when it moved after. Either way
 * the width is not in force, so it never stays configured as if it were: refused at once, the command says so and
 * keeps the width it had; refused where it landed, the next drain puts back the width the front end kept and says why.
 */
static int
test_nfm_width_refused_after_the_check(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
    opts.analog_nfm_bandwidth_hz = 12500;
    g_fake_demod_rate_hz = 16000; /* where the stream's rate has moved to */
    g_fake_request_rate_hz = 16000;

    /* The request refused at once. */
    reset_rx_family_wrap();
    g_analog_req_result = -1;
    rc |= submit_nfm_width(&opts, &state, 20000, "width refused by its request");
    rc |= expect_int("request refused: checked first", g_analog_check_calls, 1);
    rc |= expect_int("request refused: width kept", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_toast("request refused toast", &state,
                       "Refused: NFM 20 kHz does not fit the 16 kHz DSP rate (max 13.2 kHz); raise the DSP bandwidth "
                       "or narrow the NFM width");
    g_analog_req_result = 0;

    /* The request refused where it landed: applied at first, put back once the stream says so. */
    reset_rx_family_wrap();
    rc |= submit_nfm_width(&opts, &state, 20000, "width refused where it lands");
    rc |= expect_int("landing: requested", g_analog_req_calls == 1 && g_analog_req_width_hz == 20000, 1);
    rc |= expect_int("landing: stored while pending", opts.analog_nfm_bandwidth_hz, 20000);
    rc |= expect_toast("landing: applied toast", &state, "Applied: NFM bandwidth -> 20 kHz");
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("landing: still pending, still stored", opts.analog_nfm_bandwidth_hz, 20000);
    demod_thread_refuses_analog_keeping(1, 12500);
    state.ui_msg[0] = '\0';
    rc |= expect_int("landing: settled on an empty drain", dsd_app_drain_cmds(&opts, &state), 0);
    rc |= expect_int("landing: the width the front end kept is back", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_toast("landing: refusal toast", &state,
                       "Refused: NFM 20 kHz does not fit the 16 kHz DSP rate (max 13.2 kHz); raise the DSP bandwidth "
                       "or narrow the NFM width");
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("landing: reported once", state.ui_msg[0] == '\0', 1);

    /* Two widths before either lands, the second refused: the front end kept the width from before both. */
    reset_rx_family_wrap();
    rc |= submit_nfm_width(&opts, &state, 16000, "first of two");
    rc |= submit_nfm_width(&opts, &state, 20000, "second of two");
    demod_thread_refuses_analog_keeping(1, 12500);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("two widths: back to the width before both", opts.analog_nfm_bandwidth_hz, 12500);

    /* The first one landed while the second was being queued, and the second was refused: the front end kept the
       first one's width, and that, not the width from before both, is what goes back. */
    reset_rx_family_wrap();
    rc |= submit_nfm_width(&opts, &state, 16000, "first, taken");
    rc |= submit_nfm_width(&opts, &state, 20000, "second, refused");
    demod_thread_refuses_analog_keeping(1, 16000);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("first taken: the width the front end kept", opts.analog_nfm_bandwidth_hz, 16000);

    /* Refused, and the stream reopened before the next drain: the new stream opened on the width configured, so the
       old stream's refusal puts nothing back. */
    reset_rx_family_wrap();
    rc |= submit_nfm_width(&opts, &state, 20000, "refused, then a reopen");
    demod_thread_refuses_analog_keeping(1, 16000);
    stream_reopens(0);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("reopen: the width the new stream opened on stays", opts.analog_nfm_bandwidth_hz, 20000);
    rc |= expect_int("reopen: nothing to report", state.ui_msg[0] == '\0', 1);
    opts.analog_nfm_bandwidth_hz = 12500;

    /* One that lands is simply in force. */
    reset_rx_family_wrap();
    rc |= submit_nfm_width(&opts, &state, 16000, "width that lands");
    demod_thread_lands(0);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("lands: in force", opts.analog_nfm_bandwidth_hz, 16000);
    rc |= expect_int("lands: nothing to report", state.ui_msg[0] == '\0', 1);

    /* A config's width refused by its request: the rest of the config applies, the width does not. */
    reset_rx_family_wrap();
    g_analog_req_result = -1;
    rc |= submit_config_nfm_width(&opts, &state, DSDCFG_MODE_UNSET, 20000, "config width refused by its request");
    rc |= expect_int("config request refused: width kept", opts.analog_nfm_bandwidth_hz, 16000);
    rc |= expect_int("config request refused: toast", strstr(state.ui_msg, "Refused: NFM 20 kHz") != NULL, 1);
    g_analog_req_result = 0;

    g_fake_demod_rate_hz = 0;
    g_fake_request_rate_hz = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * A CQPSK scan row on an -fA session left (the scanner stopped), and a width set before the demod thread takes the
 * leave's switch back to the monitor. The leave queued that switch through the runtime hooks, not through app-control,
 * and the stream still publishes the row's CQPSK: the width goes by what the stream was asked for, whoever asked, so it
 * replaces the width the queued switch carries instead of waiting for a CQPSK that is already on its way out.
 */
static int
test_nfm_width_follows_a_queued_scan_leave(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    opts.analog_nfm_bandwidth_hz = 12500;
    const dsd_rtl_stream_metrics_hooks hooks = {.apply_analog_profile = rtl_stream_request_analog_profile,
                                                .analog_family_active = rtl_stream_analog_family_active};
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    rc |= expect_int("leave: P25 row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_P25), 0);
    rc |= expect_int("leave: P25 row options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    reset_rx_family_wrap();
    g_fake_cqpsk = 1; /* the row's CQPSK profile, taken and published */
    g_fake_analog_family = 1;

    dsd_engine_channel_scan_leave(&opts, &state);
    rc |= expect_int("leave: back on the monitor", opts.analog_only, 1);
    rc |= expect_int(
        "leave: switch queued with the width",
        g_analog_req_calls == 1 && g_analog_req_family == DSD_RX_FAMILY_ANALOG && g_analog_req_width_hz == 12500, 1);
    rc |= submit_nfm_width(&opts, &state, 20000, "width after a queued leave");
    rc |= expect_int("queued leave: width stored", opts.analog_nfm_bandwidth_hz, 20000);
    rc |= expect_int("queued leave: the switch gets the new width",
                     g_analog_req_calls == 2 && g_analog_req_width_hz == 20000, 1);

    dsd_rtl_stream_metrics_hooks_set(NULL);
    g_fake_cqpsk = 0;
    g_fake_analog_family = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* The runtime hooks a scan leave asks the front end through, reaching the fake stream's wraps. */
static const dsd_rtl_stream_metrics_hooks g_scan_leave_hooks = {
    .apply_analog_profile = rtl_stream_request_analog_profile,
    .analog_profile = rtl_stream_get_analog_profile,
    .analog_family_active = rtl_stream_analog_family_active};

/* An RTL-SDR session at a 48 kHz DSP bandwidth on the analog monitor of @p kind with the configured width @p width_hz
   of that kind, scanning a row of @p row_mode (@p row: its options, NULL for none) on the analog family, after a retune
   landed the front end on a 16 kHz rate: every request taken, the scan leave's hooks installed. */
static int
init_scan_row_on_analog(dsd_opts* opts, dsd_state* state, RtlSdrContext* fake_ctx, int kind, int width_hz,
                        dsd_scan_mode row_mode, const dsd_scan_option_values* row, const char* label) {
    init_decode_mode_context(opts, state);
    reset_rx_family_wrap();
    opts->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:851.375M:0:0:48");
    opts->rtl_dsp_bw_khz = 48;
    state->rtl_ctx = fake_ctx;
    if (kind == DSD_ANALOG_DEMOD_AM) {
        opts->analog_am_bandwidth_hz = width_hz;
    } else {
        opts->analog_nfm_bandwidth_hz = width_hz;
    }
    int rc = submit_decode_mode(opts, state, kind == DSD_ANALOG_DEMOD_AM ? DSDCFG_MODE_AM : DSDCFG_MODE_ANALOG, label);
    demod_thread_lands(0);
    (void)dsd_app_drain_cmds(opts, state);
    dsd_rtl_stream_metrics_hooks_set(&g_scan_leave_hooks);
    opts->scanner_mode = 1;
    rc |= expect_int(label, dsd_scan_mode_enter(opts, state, row_mode), 0);
    rc |= expect_int(label, dsd_scan_mode_options(opts, state, row), 0);
    reset_rx_family_wrap();
    g_fake_analog_family = 1;
    g_fake_demod_rate_hz = 16000;
    g_fake_request_rate_hz = 16000;
    return rc;
}

/* Stop the scanner (DSD_APP_CMD_SCANNER_TOGGLE) and drain, as the decoder does. */
static int
submit_scanner_stop(dsd_opts* opts, dsd_state* state, const char* label) {
    int rc = expect_int(label, dsd_app_command_action(DSD_APP_CMD_SCANNER_TOGGLE), DSD_APP_COMMAND_SUBMIT_QUEUED);
    state->ui_msg[0] = '\0';
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    rc |= expect_int(label, opts->scanner_mode, 0);
    return rc;
}

/* The last request the front end was asked for is the analog monitor of @p kind at @p width_hz, after @p calls in all
   (a request refused at once counted). */
static int
expect_last_monitor_request(const char* label, int calls, int kind, int width_hz) {
    return expect_int(label,
                      g_analog_req_calls == calls && g_analog_req_family == DSD_RX_FAMILY_ANALOG
                          && g_analog_req_kind == kind && g_analog_req_width_hz == width_hz,
                      1);
}

/* The front end takes what is still queued; nothing is left for the next case. */
static void
finish_scan_row_on_analog(dsd_opts* opts, dsd_state* state) {
    demod_thread_lands(0);
    (void)dsd_app_drain_cmds(opts, state);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    reset_rx_family_wrap();
    g_fake_analog_family = 0;
    g_fake_demod_rate_hz = 0;
    g_fake_request_rate_hz = 0;
    opts->analog_nfm_bandwidth_hz = 0;
    opts->analog_am_bandwidth_hz = 0;
    state->rtl_ctx = NULL;
    freeState(state);
}

/*
 * Issue #578: a scan stop whose return to the analog monitor the front end refuses. An -fA session on an RTL-SDR at a
 * 48 kHz DSP bandwidth with NFM 16 kHz scans a typed NXDN48 row, whose 6.25 kHz channel profile holds the front end
 * off the monitor, so a retune lands it on a 16 kHz rate that cannot filter 16 kHz. Stopping the scanner asks the
 * front end back onto NFM 16 kHz, which it refuses, at once or where the request lands, keeping the row's profile;
 * the row never touched the width setting, so the stream's record alone would read like a no-op. The decoder follows
 * what the front end kept and says why: off the monitor, the kind's default, which is asked for and configured (the
 * NFM default is never refused); still on the monitor of that kind (an nfm row with its own width), the width that
 * monitor runs. An AM default the rate refuses as well changes nothing but the toast. A width set while the leave was
 * still queued carries the leave on.
 */
static int
test_refused_scan_leave_is_reported_and_reconciled(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;

    /* Refused at once: the rate the front end publishes cannot filter 16 kHz. */
    rc |= init_scan_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, DSD_ANALOG_DEMOD_FM, 16000,
                                  DSD_SCAN_MODE_NXDN48, NULL, "leave at once");
    g_fake_analog_req_max_hz = 13200;
    rc |= submit_scanner_stop(&opts, &state, "leave at once: stopped");
    rc |= expect_int("leave at once: back on the NFM default", opts.analog_nfm_bandwidth_hz, 0);
    rc |=
        expect_int("leave at once: still Analog", opts.analog_only == 1 && opts.analog_demod == DSD_ANALOG_DEMOD_FM, 1);
    rc |= expect_last_monitor_request("leave at once: the default asked for", 2, DSD_ANALOG_DEMOD_FM, 0);
    rc |= expect_toast("leave at once toast", &state,
                       "Refused: NFM 16 kHz does not fit the 16 kHz DSP rate; the monitor is back on the NFM default");
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("leave at once: reported once", state.ui_msg[0] == '\0', 1);
    finish_scan_row_on_analog(&opts, &state);

    /* Refused where it lands: a retune in flight moves the rate after the request was queued. */
    rc |= init_scan_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, DSD_ANALOG_DEMOD_FM, 16000,
                                  DSD_SCAN_MODE_NXDN48, NULL, "leave on landing");
    rc |= submit_scanner_stop(&opts, &state, "leave on landing: stopped");
    rc |= expect_last_monitor_request("leave on landing: the leave queued", 1, DSD_ANALOG_DEMOD_FM, 16000);
    rc |= expect_int("leave on landing: width kept while pending", opts.analog_nfm_bandwidth_hz, 16000);
    rc |= expect_int("leave on landing: nothing refused yet", strstr(state.ui_msg, "Refused") == NULL, 1);
    demod_thread_refuses_leave_keeping(1, 0, DSD_ANALOG_DEMOD_FM, 16000);
    state.ui_msg[0] = '\0';
    rc |= expect_int("leave on landing: settled on an empty drain", dsd_app_drain_cmds(&opts, &state), 0);
    rc |= expect_int("leave on landing: back on the NFM default", opts.analog_nfm_bandwidth_hz, 0);
    rc |= expect_last_monitor_request("leave on landing: the default asked for", 2, DSD_ANALOG_DEMOD_FM, 0);
    rc |= expect_toast("leave on landing toast", &state,
                       "Refused: NFM 16 kHz does not fit the 16 kHz DSP rate; the monitor is back on the NFM default");
    finish_scan_row_on_analog(&opts, &state);

    /* An nfm row with its own 12.5 kHz keeps the monitor at 12.5 kHz: the configured width takes it, and nothing more
       is asked of the front end. */
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 12500;
    rc |= init_scan_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, DSD_ANALOG_DEMOD_FM, 16000,
                                  DSD_SCAN_MODE_NFM, &row, "leave on the monitor");
    g_fake_monitor_kind = DSD_ANALOG_DEMOD_FM;
    g_fake_monitor_width_hz = 12500;
    g_fake_monitor_lpf_on = 1;
    g_fake_analog_req_max_hz = 13200;
    rc |= submit_scanner_stop(&opts, &state, "leave on the monitor: stopped");
    rc |= expect_int("leave on the monitor: the width it kept", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_last_monitor_request("leave on the monitor: only the leave's request", 1, DSD_ANALOG_DEMOD_FM, 16000);
    rc |= expect_toast("leave on the monitor toast", &state,
                       "Refused: NFM 16 kHz does not fit the 16 kHz DSP rate; the monitor keeps NFM 12.5 kHz");
    finish_scan_row_on_analog(&opts, &state);

    /* -fM with AM 10 kHz at a 7.5 kHz rate, which cannot filter the AM default either: the toast says so, and the
       width and the front end stay as they are. */
    rc |= init_scan_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, DSD_ANALOG_DEMOD_AM, 10000,
                                  DSD_SCAN_MODE_NXDN48, NULL, "am leave");
    g_fake_demod_rate_hz = 7500;
    g_fake_request_rate_hz = 7500;
    g_fake_analog_req_max_hz = 5000;
    g_analog_check_result = -1;
    rc |= submit_scanner_stop(&opts, &state, "am leave: stopped");
    rc |= expect_int("am leave: width unchanged", opts.analog_am_bandwidth_hz, 10000);
    rc |= expect_int("am leave: still AM", opts.analog_only == 1 && opts.analog_demod == DSD_ANALOG_DEMOD_AM, 1);
    rc |= expect_last_monitor_request("am leave: no default asked for", 1, DSD_ANALOG_DEMOD_AM, 10000);
    rc |= expect_toast("am leave toast", &state,
                       "Refused: AM 10 kHz does not fit the 7.5 kHz DSP rate; the AM default does not fit it either");
    finish_scan_row_on_analog(&opts, &state);

    /* -fM with AM 10 kHz leaving an nfm row, whose FM monitor the front end keeps: its 7.5 kHz rate refuses the
       leave's AM 10 kHz at once, and the AM default is asked for and taken. A retune in flight then lands it on a rate
       that refuses the default as well, keeping FM: the default request carried the leave on, so the configured AM
       width goes back to the 10 kHz from before it, and the toast says the default is refused too. */
    rc |= init_scan_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, DSD_ANALOG_DEMOD_AM, 10000,
                                  DSD_SCAN_MODE_NFM, NULL, "am default refused on landing");
    g_fake_demod_rate_hz = 7500;
    g_fake_request_rate_hz = 7500;
    g_fake_analog_req_max_hz = 5000;
    g_fake_monitor_kind = DSD_ANALOG_DEMOD_FM;
    g_fake_monitor_width_hz = 16000;
    g_fake_monitor_lpf_on = 1;
    rc |= submit_scanner_stop(&opts, &state, "am default refused on landing: stopped");
    rc |= expect_int("am default refused on landing: the default configured", opts.analog_am_bandwidth_hz, 0);
    rc |=
        expect_last_monitor_request("am default refused on landing: the default asked for", 2, DSD_ANALOG_DEMOD_AM, 0);
    rc |= expect_toast("am default refused on landing: first toast", &state,
                       "Refused: AM 10 kHz does not fit the 7.5 kHz DSP rate; the monitor is back on the AM default");
    demod_thread_refuses_leave_keeping(1, 1, DSD_ANALOG_DEMOD_FM, 16000);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("am default refused on landing: the AM width back", opts.analog_am_bandwidth_hz, 10000);
    rc |= expect_int("am default refused on landing: still AM",
                     opts.analog_only == 1 && opts.analog_demod == DSD_ANALOG_DEMOD_AM, 1);
    rc |= expect_last_monitor_request("am default refused on landing: nothing more asked", 2, DSD_ANALOG_DEMOD_AM, 0);
    rc |= expect_toast("am default refused on landing toast", &state,
                       "Refused: AM 10 kHz does not fit the 7.5 kHz DSP rate; the AM default does not fit it either");
    finish_scan_row_on_analog(&opts, &state);

    /* A width set while the leave is still queued replaces its request and carries the leave on: refused where it
       lands off the monitor, the NFM default is what the decoder goes back to. */
    rc |= init_scan_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, DSD_ANALOG_DEMOD_FM, 16000,
                                  DSD_SCAN_MODE_NXDN48, NULL, "width after a leave");
    rc |= submit_scanner_stop(&opts, &state, "width after a leave: stopped");
    rc |= submit_nfm_width(&opts, &state, 20000, "width after a leave: 20 kHz");
    rc |= expect_last_monitor_request("width after a leave: requested", 2, DSD_ANALOG_DEMOD_FM, 20000);
    demod_thread_refuses_leave_keeping(1, 0, DSD_ANALOG_DEMOD_FM, 16000);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("width after a leave: back on the NFM default", opts.analog_nfm_bandwidth_hz, 0);
    rc |= expect_last_monitor_request("width after a leave: the default asked for", 3, DSD_ANALOG_DEMOD_FM, 0);
    rc |= expect_toast("width after a leave toast", &state,
                       "Refused: NFM 20 kHz does not fit the 16 kHz DSP rate; the monitor is back on the NFM default");
    finish_scan_row_on_analog(&opts, &state);
    return rc;
}

/* A refused scan leave that a later request decided is left to it: CQPSK stays what the front end was last asked for
   (on), nothing more is asked of the monitor than the @p calls requests made before, the configured width of @p kind
   stays @p width_hz, and nothing is toasted, in this drain or the next. */
static int
expect_leave_left_to_a_later_request(const char* label, dsd_opts* opts, dsd_state* state, int kind, int width_hz,
                                     int calls) {
    int rc = expect_int(label, __wrap_rtl_stream_requested_cqpsk(), 1);
    rc |= expect_int(label, g_analog_req_calls, calls);
    rc |= expect_int(label, kind == DSD_ANALOG_DEMOD_AM ? opts->analog_am_bandwidth_hz : opts->analog_nfm_bandwidth_hz,
                     width_hz);
    rc |= expect_int(label, opts->analog_only == 1 && opts->analog_demod == kind, 1);
    rc |= expect_str(label, state->ui_msg, "");
    (void)dsd_app_drain_cmds(opts, state);
    rc |= expect_str(label, state->ui_msg, "");
    return rc;
}

/*
 * Issue #578: a scan leave's return to the analog monitor that the front end refuses once a later request is queued
 * behind it, from anywhere, which decides what the front end runs instead. Stopping the scanner queues the return, and
 * CQPSK toggled on from the DSP menu before the demod thread takes it queues its profile after it; the demod thread
 * refuses the return where it lands and turns CQPSK on. Asking for the kind's default then would turn CQPSK off again
 * behind the operator's back, so the refusal is left to the later request: nothing is asked, the configured width stays
 * and nothing is toasted (the stream logged the refusal). The same holds for the AM default a refused leave fell back
 * on, refused where it lands behind a later request, which keeps the default the leave's toast named, and for a leave
 * refused at once while the request before it was still queued.
 */
static int
test_refused_scan_leave_is_left_to_a_later_request(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;

    /* Refused where it lands, taken together with the CQPSK toggle queued after it. */
    rc |= init_scan_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, DSD_ANALOG_DEMOD_FM, 16000,
                                  DSD_SCAN_MODE_NXDN48, NULL, "cqpsk after a leave");
    rc |= submit_scanner_stop(&opts, &state, "cqpsk after a leave: stopped");
    rc |= expect_last_monitor_request("cqpsk after a leave: the leave queued", 1, DSD_ANALOG_DEMOD_FM, 16000);
    submit_cqpsk_toggle();
    rc |= expect_int("cqpsk after a leave: toggled", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("cqpsk after a leave: CQPSK on queued", g_demod_req_calls == 1 && g_demod_req_cqpsk == 1, 1);
    g_fake_cqpsk = 1;
    demod_thread_refuses_leave_keeping(1, 0, DSD_ANALOG_DEMOD_FM, 16000);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_leave_left_to_a_later_request("cqpsk after a leave: left to CQPSK", &opts, &state, DSD_ANALOG_DEMOD_FM,
                                               16000, 1);
    finish_scan_row_on_analog(&opts, &state);

    /* -fM with AM 10 kHz leaving an nfm row at a 7.5 kHz rate: the leave is refused at once and the AM default asked
       for, and CQPSK is toggled on before the demod thread takes that; it refuses the default where it lands, keeping
       the FM family with CQPSK on. */
    rc |= init_scan_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, DSD_ANALOG_DEMOD_AM, 10000,
                                  DSD_SCAN_MODE_NFM, NULL, "cqpsk after an am default");
    g_fake_demod_rate_hz = 7500;
    g_fake_request_rate_hz = 7500;
    g_fake_analog_req_max_hz = 5000;
    g_fake_monitor_kind = DSD_ANALOG_DEMOD_FM;
    g_fake_monitor_width_hz = 16000;
    g_fake_monitor_lpf_on = 1;
    rc |= submit_scanner_stop(&opts, &state, "cqpsk after an am default: stopped");
    rc |= expect_last_monitor_request("cqpsk after an am default: the default asked for", 2, DSD_ANALOG_DEMOD_AM, 0);
    rc |= expect_int("cqpsk after an am default: the default configured", opts.analog_am_bandwidth_hz, 0);
    submit_cqpsk_toggle();
    rc |= expect_int("cqpsk after an am default: toggled", dsd_app_drain_cmds(&opts, &state), 1);
    g_fake_cqpsk = 1;
    demod_thread_refuses_leave_keeping(1, 0, DSD_ANALOG_DEMOD_FM, 16000);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_leave_left_to_a_later_request("cqpsk after an am default: left to CQPSK", &opts, &state,
                                               DSD_ANALOG_DEMOD_AM, 0, 2);
    finish_scan_row_on_analog(&opts, &state);

    /* Refused at once while the row's own symbol profile, queued through the hooks, was still on its way, with CQPSK
       toggled on in the same drain: the leave's record names that profile, and the toggle is queued after it. */
    rc |= init_scan_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, DSD_ANALOG_DEMOD_FM, 16000,
                                  DSD_SCAN_MODE_NXDN48, NULL, "cqpsk after a leave at once");
    g_fake_analog_req_max_hz = 13200;
    ++g_fake_rx_seq; /* the row's symbol profile, not taken yet */
    rc |= expect_int("cqpsk after a leave at once: stop queued", dsd_app_command_action(DSD_APP_CMD_SCANNER_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    submit_cqpsk_toggle();
    state.ui_msg[0] = '\0';
    rc |= expect_int("cqpsk after a leave at once: drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_int("cqpsk after a leave at once: stopped", opts.scanner_mode, 0);
    rc |=
        expect_int("cqpsk after a leave at once: CQPSK on queued", g_demod_req_calls == 1 && g_demod_req_cqpsk == 1, 1);
    demod_thread_lands(1);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_leave_left_to_a_later_request("cqpsk after a leave at once: left to CQPSK", &opts, &state,
                                               DSD_ANALOG_DEMOD_FM, 16000, 1);
    finish_scan_row_on_analog(&opts, &state);
    return rc;
}

/*
 * Issue #578: the refused scan leave a later request decides, with a switch onto Analog armed before it. A DMR session
 * on an RTL-SDR at a 48 kHz DSP bandwidth, with NFM 16 kHz stored, scans a typed NXDN48 row and changes its configured
 * mode to Analog: the row keeps its own profile on the digital family, so the switch is armed with no monitor request
 * yet. Stopping the scanner queues the leave's return to the monitor, and CQPSK toggled on before the demod thread
 * takes it queues its profile after it; a retune had moved the rate to 16 kHz, so the demod thread refuses the return
 * where it lands, keeping the digital family, and turns CQPSK on. Going back to DMR then would publish its FSK profile
 * over that CQPSK and toast a failure for a return the later request decided, so the refusal is left to it, the armed
 * switch included: the decoder stays on Analog with CQPSK on, and nothing is toasted. The front end has made the
 * switch no more than before, so it stays armed for the next monitor request to settle: CQPSK toggled off asks for the
 * monitor, and the front end refusing that one as well puts the decoder back on DMR.
 */
static int
test_superseded_scan_leave_keeps_an_armed_switch(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    reset_rx_family_wrap();
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:48");
    opts.rtl_dsp_bw_khz = 48;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_DMR, "armed leave: DMR");
    opts.analog_nfm_bandwidth_hz = 16000;
    demod_thread_lands(0);
    (void)dsd_app_drain_cmds(&opts, &state);
    dsd_rtl_stream_metrics_hooks_set(&g_scan_leave_hooks);
    opts.scanner_mode = 1;
    rc |= expect_int("armed leave: typed row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NXDN48), 0);
    rc |= expect_int("armed leave: row options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    reset_rx_family_wrap();
    g_fake_analog_family = 0;

    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_ANALOG, "armed leave: Analog under the row");
    rc |= expect_int("armed leave: configured Analog",
                     dsd_scan_mode_configured_preset(&opts, &state) == DSDCFG_MODE_ANALOG, 1);
    rc |= expect_int("armed leave: no monitor request under the row", g_analog_req_calls, 0);
    g_fake_demod_rate_hz = 16000;
    g_fake_request_rate_hz = 16000;
    rc |= submit_scanner_stop(&opts, &state, "armed leave: stopped");
    rc |= expect_last_monitor_request("armed leave: the leave queued", 1, DSD_ANALOG_DEMOD_FM, 16000);
    submit_cqpsk_toggle();
    rc |= expect_int("armed leave: toggled", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("armed leave: CQPSK on queued", g_demod_req_cqpsk, 1);
    const int demod_requests = g_demod_req_calls;
    g_fake_cqpsk = 1;
    demod_thread_refuses_leave_keeping(0, 0, DSD_ANALOG_DEMOD_FM, 16000);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_leave_left_to_a_later_request("armed leave: left to CQPSK", &opts, &state, DSD_ANALOG_DEMOD_FM, 16000,
                                               1);
    rc |= expect_int("armed leave: configured Analog still", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_ANALOG,
                     1);
    rc |= expect_int("armed leave: no profile over CQPSK", g_demod_req_calls, demod_requests);

    /* Still armed: CQPSK off asks for the monitor, which the front end refuses where it lands as well. */
    submit_cqpsk_toggle();
    rc |= expect_int("armed leave: CQPSK off", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_last_monitor_request("armed leave: the monitor asked for", 2, DSD_ANALOG_DEMOD_FM, 16000);
    demod_thread_refuses_analog_keeping(0, 0);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("armed leave: back on DMR",
                     opts.analog_only == 0 && dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_DMR, 1);
    rc |= expect_toast("armed leave: says why", &state, "Failed: Analog -> ");

    dsd_rtl_stream_metrics_hooks_set(NULL);
    reset_rx_family_wrap();
    g_fake_cqpsk = 0;
    g_fake_demod_rate_hz = 0;
    g_fake_request_rate_hz = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #578: a refused scan leave whose scan keeps running, with a switch onto Analog armed before it. A DMR session
 * on an RTL-SDR at a 48 kHz DSP bandwidth, with NFM 16 kHz stored, scans a typed NXDN48 row and changes its configured
 * mode to Analog: the row keeps its own profile on the digital family, so the switch is armed with no monitor request
 * yet. A channel map with two rows is then imported, which leaves the row and asks the front end back onto the monitor,
 * while the scanner stays on to visit the new rows; a retune had moved the rate to 16 kHz, so the demod thread refuses
 * that return where it lands, keeping the digital family. The next row's tune decides the front end, so the refusal is
 * left to it, the armed switch included: the decoder stays on Analog and nothing is toasted, rather than go back to DMR
 * for a return the scan has moved on from. The switch stays armed for the next monitor request to settle: once a row is
 * on air and the scanner stops, the front end refusing that return as well puts the decoder back on DMR.
 */
static int
test_continuing_scan_leave_keeps_an_armed_switch(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_ui_cmd_queue_leave_map") != 0) {
        DSD_FPRINTF(stderr, "temp working directory setup failed: %s\n", strerror(errno));
        return 1;
    }
    init_decode_mode_context(&opts, &state);
    reset_rx_family_wrap();
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:48");
    opts.rtl_dsp_bw_khz = 48;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_DMR, "scan goes on: DMR");
    opts.analog_nfm_bandwidth_hz = 16000;
    demod_thread_lands(0);
    (void)dsd_app_drain_cmds(&opts, &state);
    dsd_rtl_stream_metrics_hooks_set(&g_scan_leave_hooks);
    opts.scanner_mode = 1;
    rc |= expect_int("scan goes on: typed row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NXDN48), 0);
    rc |= expect_int("scan goes on: row options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    reset_rx_family_wrap();
    g_fake_analog_family = 0;

    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_ANALOG, "scan goes on: Analog under the row");
    rc |= expect_int("scan goes on: no monitor request under the row", g_analog_req_calls, 0);
    g_fake_demod_rate_hz = 16000;
    g_fake_request_rate_hz = 16000;
    static const char map[] = "channel,frequency_hz,name,mode,options\n"
                              "1,461000000,one,dmr,\n"
                              "2,462000000,two,dmr,\n";
    rc |= write_file_bytes("leave_map.csv", map, strlen(map));
    post_string(DSD_APP_CMD_IMPORT_CHANNEL_MAP, "leave_map.csv");
    rc |= expect_int("scan goes on: map imported", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("scan goes on: the scanner stays on", opts.scanner_mode == 1 && state.lcn_freq_count == 2, 1);
    rc |= expect_last_monitor_request("scan goes on: the leave queued", 1, DSD_ANALOG_DEMOD_FM, 16000);
    const int demod_requests = g_demod_req_calls;
    demod_thread_refuses_leave_keeping(0, 0, DSD_ANALOG_DEMOD_FM, 16000);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("scan goes on: still Analog",
                     opts.analog_only == 1 && dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_ANALOG, 1);
    rc |= expect_int("scan goes on: width kept", opts.analog_nfm_bandwidth_hz, 16000);
    rc |= expect_int("scan goes on: nothing more asked", g_analog_req_calls == 1 && g_demod_req_calls == demod_requests,
                     1);
    rc |= expect_str("scan goes on: no toast", state.ui_msg, "");

    /* Still armed: a row of the new map on air, the scanner stopped, and that return refused as well. */
    rc |= expect_int("scan goes on: next row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR), 0);
    rc |= expect_int("scan goes on: next row options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    rc |= submit_scanner_stop(&opts, &state, "scan goes on: stopped");
    rc |= expect_last_monitor_request("scan goes on: the stop's return queued", 2, DSD_ANALOG_DEMOD_FM, 16000);
    demod_thread_refuses_leave_keeping(0, 0, DSD_ANALOG_DEMOD_FM, 16000);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("scan goes on: back on DMR",
                     opts.analog_only == 0 && dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_DMR, 1);
    rc |= expect_toast("scan goes on: says why", &state, "Failed: Analog -> ");

    dsd_rtl_stream_metrics_hooks_set(NULL);
    reset_rx_family_wrap();
    g_fake_demod_rate_hz = 0;
    g_fake_request_rate_hz = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    (void)remove("leave_map.csv");
    freeState(&state);
    if (dsd_test_temp_cwd_leave(&cwd) != 0) {
        rc = 1;
    }
    return rc;
}

/* An -fA session scanning a typed NXDN48 row on the analog family (init_scan_row_on_analog(), NFM 16 kHz at a 16 kHz
   rate) changes its configured mode to DMR and back to Analog under the row: the row keeps its own profile, so the
   switch back onto Analog is armed from DMR with no monitor request. */
static int
init_switch_armed_under_a_row_on_analog(dsd_opts* opts, dsd_state* state, RtlSdrContext* fake_ctx, const char* label) {
    int rc =
        init_scan_row_on_analog(opts, state, fake_ctx, DSD_ANALOG_DEMOD_FM, 16000, DSD_SCAN_MODE_NXDN48, NULL, label);
    rc |= submit_decode_mode(opts, state, DSDCFG_MODE_DMR, label);
    rc |= submit_decode_mode(opts, state, DSDCFG_MODE_ANALOG, label);
    rc |= expect_int(label, dsd_scan_mode_configured_preset(opts, state) == DSDCFG_MODE_ANALOG, 1);
    rc |= expect_int(label, g_analog_req_calls, 0);
    return rc;
}

/* The switch armed by init_switch_armed_under_a_row_on_analog() is still armed: an AM row on air (its monitor taken),
   the scanner stopped, and the front end refusing that return to NFM 16 kHz where it lands, keeping the AM monitor,
   puts the decoder back on DMR, rather than asking for the NFM default. */
static int
expect_switch_still_armed_off_an_am_row(dsd_opts* opts, dsd_state* state, const char* label) {
    opts->scanner_mode = 1;
    int rc = expect_int(label, dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_AM), 0);
    rc |= expect_int(label, dsd_scan_mode_options(opts, state, NULL), 0);
    demod_thread_lands(0);
    g_fake_monitor_kind = DSD_ANALOG_DEMOD_AM;
    g_fake_monitor_width_hz = 6000;
    g_fake_monitor_lpf_on = 1;
    rc |= submit_scanner_stop(opts, state, label);
    rc |= expect_int(label,
                     g_analog_req_family == DSD_RX_FAMILY_ANALOG && g_analog_req_kind == DSD_ANALOG_DEMOD_FM
                         && g_analog_req_width_hz == 16000,
                     1);
    demod_thread_refuses_leave_keeping(1, 1, DSD_ANALOG_DEMOD_AM, 0);
    state->ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(opts, state);
    rc |= expect_int(label, opts->analog_only == 0 && dsd_infer_decode_mode_preset(opts) == DSDCFG_MODE_DMR, 1);
    rc |= expect_int(label, opts->analog_nfm_bandwidth_hz, 16000);
    rc |= expect_toast(label, state, "Failed: Analog -> ");
    return rc;
}

/*
 * Issue #578: a switch onto Analog armed before a scan leave whose refusal is left to what came after it, on a front end
 * that stayed on the analog family. An -fA session scanning a typed NXDN48 row runs the row's profile on the analog
 * family, so a configured mode changed to DMR and back to Analog under the row arms the switch from DMR while the front
 * end stays on analog FM. The leave's return to NFM 16 kHz is refused where it lands, keeping analog FM off the monitor
 * (the kind asked for), and either a CQPSK toggle queued after it decides the front end, or the scan goes on after a
 * channel-map import with the scanner on: the refusal is left to that, and the armed switch with it, as when the front
 * end kept the digital family. Nothing is asked or toasted, and the switch stays armed for the next monitor request: a
 * later AM row on air, the scanner stopped, and that return refused as well keeping the AM monitor puts the decoder
 * back on DMR.
 */
static int
test_scan_leave_left_to_later_on_analog_keeps_an_armed_switch(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_ui_cmd_queue_leave_analog") != 0) {
        DSD_FPRINTF(stderr, "temp working directory setup failed: %s\n", strerror(errno));
        return 1;
    }

    /* Superseded by CQPSK toggled on before the demod thread took the return. */
    rc |= init_switch_armed_under_a_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, "armed on analog: cqpsk");
    rc |= submit_scanner_stop(&opts, &state, "armed on analog: cqpsk: stopped");
    rc |= expect_last_monitor_request("armed on analog: cqpsk: the leave queued", 1, DSD_ANALOG_DEMOD_FM, 16000);
    submit_cqpsk_toggle();
    rc |= expect_int("armed on analog: cqpsk: toggled", dsd_app_drain_cmds(&opts, &state), 1);
    g_fake_cqpsk = 1;
    demod_thread_refuses_leave_keeping(1, 0, DSD_ANALOG_DEMOD_FM, 16000);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_leave_left_to_a_later_request("armed on analog: cqpsk: left to CQPSK", &opts, &state,
                                               DSD_ANALOG_DEMOD_FM, 16000, 1);
    rc |= expect_switch_still_armed_off_an_am_row(&opts, &state, "armed on analog: cqpsk: still armed");
    finish_scan_row_on_analog(&opts, &state);

    /* The scan goes on after a channel-map import with the scanner on. */
    rc |= init_switch_armed_under_a_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, "armed on analog: map");
    static const char map[] = "channel,frequency_hz,name,mode,options\n"
                              "1,461000000,one,dmr,\n"
                              "2,462000000,two,am,\n";
    rc |= write_file_bytes("leave_analog_map.csv", map, strlen(map));
    post_string(DSD_APP_CMD_IMPORT_CHANNEL_MAP, "leave_analog_map.csv");
    rc |= expect_int("armed on analog: map: imported", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("armed on analog: map: the scanner stays on", opts.scanner_mode == 1 && state.lcn_freq_count == 2,
                     1);
    rc |= expect_last_monitor_request("armed on analog: map: the leave queued", 1, DSD_ANALOG_DEMOD_FM, 16000);
    const int demod_requests = g_demod_req_calls;
    demod_thread_refuses_leave_keeping(1, 0, DSD_ANALOG_DEMOD_FM, 16000);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("armed on analog: map: still Analog",
                     opts.analog_only == 1 && dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_ANALOG, 1);
    rc |= expect_int("armed on analog: map: width kept", opts.analog_nfm_bandwidth_hz, 16000);
    rc |= expect_int("armed on analog: map: nothing more asked",
                     g_analog_req_calls == 1 && g_demod_req_calls == demod_requests, 1);
    rc |= expect_str("armed on analog: map: no toast", state.ui_msg, "");
    rc |= expect_switch_still_armed_off_an_am_row(&opts, &state, "armed on analog: map: still armed");
    (void)remove("leave_analog_map.csv");
    finish_scan_row_on_analog(&opts, &state);

    if (dsd_test_temp_cwd_leave(&cwd) != 0) {
        rc = 1;
    }
    return rc;
}

/*
 * Issue #578: a scan leave's return to the monitor that the stream drops before the demod thread takes it, with a
 * switch onto Analog armed before the leave. An -fA session scanning a typed NXDN48 row on the analog family changes
 * its configured mode to DMR and back to Analog under the row, which arms the switch from DMR
 * (init_switch_armed_under_a_row_on_analog()). A channel map with a dmr and an am row is imported with the scanner on:
 * the import leaves the row and queues the return to NFM 16 kHz, and the scan goes on. The am row's tune then lands its
 * receive family before the demod thread takes that return, which retires it: the front end never ran it, so it was
 * not taken, and the row's tune decides the front end. Nothing is asked or toasted, and the switch stays armed for the
 * next monitor request to settle: with the am row on air, the scanner stopped, and the front end refusing that return
 * where it lands, keeping the AM monitor, the decoder goes back on DMR, rather than asking for the NFM default.
 */
static int
test_scan_leave_retired_by_a_row_keeps_an_armed_switch(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_ui_cmd_queue_leave_retired") != 0) {
        DSD_FPRINTF(stderr, "temp working directory setup failed: %s\n", strerror(errno));
        return 1;
    }

    rc |= init_switch_armed_under_a_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, "leave retired");
    static const char map[] = "channel,frequency_hz,name,mode,options\n"
                              "1,461000000,one,dmr,\n"
                              "2,462000000,two,am,\n";
    rc |= write_file_bytes("leave_retired_map.csv", map, strlen(map));
    post_string(DSD_APP_CMD_IMPORT_CHANNEL_MAP, "leave_retired_map.csv");
    rc |= expect_int("leave retired: imported", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("leave retired: the scanner stays on", opts.scanner_mode == 1 && state.lcn_freq_count == 2, 1);
    rc |= expect_last_monitor_request("leave retired: the leave queued", 1, DSD_ANALOG_DEMOD_FM, 16000);
    const int demod_requests = g_demod_req_calls;
    retune_retires_the_queued_requests();
    g_fake_monitor_kind = DSD_ANALOG_DEMOD_AM;
    g_fake_monitor_width_hz = 6000;
    g_fake_monitor_lpf_on = 1;
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("leave retired: still Analog",
                     opts.analog_only == 1 && dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_ANALOG, 1);
    rc |= expect_int("leave retired: width kept", opts.analog_nfm_bandwidth_hz, 16000);
    rc |= expect_int("leave retired: nothing more asked",
                     g_analog_req_calls == 1 && g_demod_req_calls == demod_requests, 1);
    rc |= expect_str("leave retired: no toast", state.ui_msg, "");
    rc |= expect_switch_still_armed_off_an_am_row(&opts, &state, "leave retired: still armed");
    (void)remove("leave_retired_map.csv");
    finish_scan_row_on_analog(&opts, &state);

    if (dsd_test_temp_cwd_leave(&cwd) != 0) {
        rc = 1;
    }
    return rc;
}

/*
 * Issue #578: what became of a scan leave's return to the monitor goes with the stream it was asked of. An -fM session
 * with the AM default leaves a typed NXDN48 row at a 7.5 kHz rate that cannot filter that default, so the front end
 * refuses the return at once, while the row's own symbol profile is still queued: the record waits for that profile to
 * settle. The stream is then restarted (DSD_APP_CMD_RTL_RESTART) and opens on the configured options, running the AM
 * default the old stream refused, which its monitor publishes as the 6 kHz its channel filter runs. That says nothing
 * about the old stream's refusal: the record goes with the stream it was made on, the configured width stays the
 * default, nothing more is asked of the front end, and the restart's toast stands. The same holds for a leave whose
 * return was still queued when the stream was restarted.
 */
static int
test_scan_leave_record_goes_with_its_stream(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    for (int at_once = 1; at_once >= 0; at_once--) {
        const char* label = at_once ? "leave at once, then a restart" : "leave queued, then a restart";
        rc |= init_scan_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, DSD_ANALOG_DEMOD_AM, 0,
                                      DSD_SCAN_MODE_NXDN48, NULL, label);
        g_fake_demod_rate_hz = 7500;
        g_fake_request_rate_hz = 7500;
        g_analog_req_result = at_once ? -1 : 0;
        ++g_fake_rx_seq; /* the row's symbol profile, not taken yet */
        rc |= submit_scanner_stop(&opts, &state, label);
        rc |= expect_last_monitor_request(label, 1, DSD_ANALOG_DEMOD_AM, 0);
        rc |= expect_int(label, strstr(state.ui_msg, "Refused") == NULL, 1);
        g_analog_req_result = 0;
        g_config_rtl_open_ok = 1;
        rc |= expect_int(label, dsd_app_command_action(DSD_APP_CMD_RTL_RESTART), DSD_APP_COMMAND_SUBMIT_QUEUED);
        state.ui_msg[0] = '\0';
        rc |= expect_int(label, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(label, state.rtl_ctx == (RtlSdrContext*)g_config_rtl_ctx, 1);
        rc |= expect_toast(label, &state, "Applied: RTL stream restarted");
        /* The new stream opened on the AM default: the open settled what the old one left queued, and its monitor runs
           the 6 kHz the default's channel filter sets. */
        stream_reopens(0);
        g_fake_demod_rate_hz = 24000;
        g_fake_request_rate_hz = 24000;
        g_fake_monitor_kind = DSD_ANALOG_DEMOD_AM;
        g_fake_monitor_width_hz = 6000;
        g_fake_monitor_lpf_on = 1;
        (void)dsd_app_drain_cmds(&opts, &state);
        rc |= expect_int(label, opts.analog_am_bandwidth_hz, 0);
        rc |= expect_int(label, opts.analog_only == 1 && opts.analog_demod == DSD_ANALOG_DEMOD_AM, 1);
        rc |= expect_last_monitor_request(label, 1, DSD_ANALOG_DEMOD_AM, 0);
        rc |= expect_str(label, state.ui_msg, "Applied: RTL stream restarted");
        finish_scan_row_on_analog(&opts, &state);
    }
    return rc;
}

/*
 * Issue #578: a scan leave refused at once that asked for the AM default, on a stream that runs that default once the
 * requests queued before the leave have settled. An -fM session with the AM default leaves an am row at a 7.5 kHz rate
 * that cannot filter the default, while a request queued before the leave is still on its way; a retune in flight
 * then lands the front end on a 24 kHz rate and the am row's AM default monitor, which publishes the 6 kHz its channel
 * filter runs. That is the default the leave asked for, not a width the monitor kept instead: nothing was refused, so
 * the configured width stays the default and nothing is toasted. A monitor that runs another AM width (8 kHz) did keep
 * one, and the configured width takes it, with the toast.
 */
static int
test_refused_leave_at_once_reads_the_am_default_it_asked_for(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    for (int runs_default = 1; runs_default >= 0; runs_default--) {
        const char* label = runs_default ? "am default leave: the default runs" : "am default leave: 8 kHz runs";
        rc |= init_scan_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, DSD_ANALOG_DEMOD_AM, 0, DSD_SCAN_MODE_AM,
                                      NULL, label);
        g_fake_demod_rate_hz = 7500;
        g_fake_request_rate_hz = 7500;
        g_analog_req_result = -1;
        ++g_fake_rx_seq; /* a request queued before the leave, not taken yet */
        rc |= submit_scanner_stop(&opts, &state, label);
        rc |= expect_last_monitor_request(label, 1, DSD_ANALOG_DEMOD_AM, 0);
        g_analog_req_result = 0;
        g_fake_demod_rate_hz = 24000;
        g_fake_request_rate_hz = 24000;
        g_fake_monitor_kind = DSD_ANALOG_DEMOD_AM;
        g_fake_monitor_width_hz = runs_default ? 6000 : 8000;
        g_fake_monitor_lpf_on = 1;
        demod_thread_lands(0);
        state.ui_msg[0] = '\0';
        (void)dsd_app_drain_cmds(&opts, &state);
        rc |= expect_int(label, opts.analog_am_bandwidth_hz, runs_default ? 0 : 8000);
        rc |= expect_int(label, opts.analog_only == 1 && opts.analog_demod == DSD_ANALOG_DEMOD_AM, 1);
        rc |= expect_last_monitor_request(label, 1, DSD_ANALOG_DEMOD_AM, 0);
        if (runs_default) {
            rc |= expect_str(label, state.ui_msg, "");
        } else {
            rc |= expect_toast(label, &state, "Refused: ");
            rc |= expect_toast(label, &state, "; the monitor keeps AM 8 kHz");
        }
        finish_scan_row_on_analog(&opts, &state);
    }
    return rc;
}

/* Stop the scanner under the typed row init_scan_row_on_analog() entered, with a switch to analog @p to_kind armed
   under it, and have the front end refuse the leave's return to that kind at @p to_width_hz: at once (@p at_once), the
   rate it publishes having already moved, which the stop's drain settles, or where the request lands, keeping the
   typed row's profile over the kind @p kept_kind with the width setting @p kept_width_hz it ran before the switch. */
static int
stop_scanner_refusing_the_leave(dsd_opts* opts, dsd_state* state, int at_once, int to_kind, int to_width_hz,
                                int kept_kind, int kept_width_hz, const char* label) {
    if (at_once) {
        g_fake_analog_req_max_hz = to_width_hz - 1;
    }
    int rc = submit_scanner_stop(opts, state, label);
    if (!at_once) {
        rc |= expect_last_monitor_request(label, 1, to_kind, to_width_hz);
        rc |= expect_str(label, state->ui_msg, "");
        demod_thread_refuses_leave_keeping(1, 0, kept_kind, kept_width_hz);
        state->ui_msg[0] = '\0';
        (void)dsd_app_drain_cmds(opts, state);
    }
    return rc;
}

/* A switch between FM and AM armed under a typed row, whose leave the front end refused off the monitor, went back: the
   decoder is on analog @p kind again (@p preset) with the NFM and AM widths @p nfm_hz and @p am_hz it had, the monitor
   was asked for that kind at @p width_hz after the leave's refused request, and the toast says why (@p toast: the
   switch that failed, naming @p refused), once. */
static int
expect_switch_between_kinds_reverted(const char* label, dsd_opts* opts, dsd_state* state, int kind,
                                     dsdneoUserDecodeMode preset, int nfm_hz, int am_hz, int width_hz,
                                     const char* toast, const char* refused) {
    char tag[160];
    DSD_SNPRINTF(tag, sizeof tag, "%s: back on the kind it ran", label);
    int rc = expect_int(
        tag, opts->analog_only == 1 && opts->analog_demod == kind && dsd_infer_decode_mode_preset(opts) == preset, 1);
    DSD_SNPRINTF(tag, sizeof tag, "%s: NFM width", label);
    rc |= expect_int(tag, opts->analog_nfm_bandwidth_hz, nfm_hz);
    DSD_SNPRINTF(tag, sizeof tag, "%s: AM width", label);
    rc |= expect_int(tag, opts->analog_am_bandwidth_hz, am_hz);
    DSD_SNPRINTF(tag, sizeof tag, "%s: the monitor asked for that kind", label);
    rc |= expect_last_monitor_request(tag, 2, kind, width_hz);
    DSD_SNPRINTF(tag, sizeof tag, "%s: toast", label);
    rc |= expect_toast(tag, state, toast);
    rc |= expect_toast(tag, state, refused);
    state->ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(opts, state);
    DSD_SNPRINTF(tag, sizeof tag, "%s: reported once", label);
    rc |= expect_str(tag, state->ui_msg, "");
    return rc;
}

/*
 * Issue #578: a scan leave's return to the monitor refused off the monitor, with a switch between FM and AM armed
 * before the leave. An -fM session with AM 10 kHz scans a typed NXDN48 row on the analog family, sets NFM 16 kHz and
 * changes its configured mode to Analog under the row: the row keeps its own profile, so the switch is armed with no
 * monitor request. A retune lands the front end on a 16 kHz rate that cannot filter NFM 16 kHz, and stopping the
 * scanner asks it back onto NFM 16 kHz, which it refuses, keeping the row's profile over the AM the family runs. The
 * front end never made the switch, so the decoder goes back to AM with its 10 kHz, as for any switch between FM and AM
 * the front end refused, rather than keep Analog and reset the NFM width to its default. The other way round, an -fA
 * session on the NFM default switched to AM 10 kHz under the row, left at a 7.5 kHz rate that filters neither AM 10 kHz
 * nor the AM default, goes back to Analog on the NFM default, rather than stay on AM over the row's digital filter.
 * Refused at once, what the stream publishes says it kept the other kind off the monitor; refused where it lands, the
 * stream's record says so: both timings read the same, and the decoder ends up the same.
 */
static int
test_refused_leave_off_the_monitor_reverts_a_switch_between_kinds(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    for (int at_once = 1; at_once >= 0; at_once--) {
        char label[96];
        DSD_SNPRINTF(label, sizeof label, "am -> analog under a row, refused %s", at_once ? "at once" : "on landing");
        rc |= init_scan_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, DSD_ANALOG_DEMOD_AM, 10000,
                                      DSD_SCAN_MODE_NXDN48, NULL, label);
        rc |= submit_nfm_width(&opts, &state, 16000, label);
        rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_ANALOG, label);
        rc |= expect_int(label, dsd_scan_mode_configured_preset(&opts, &state) == DSDCFG_MODE_ANALOG, 1);
        rc |= expect_int(label, g_analog_req_calls, 0);
        rc |= stop_scanner_refusing_the_leave(&opts, &state, at_once, DSD_ANALOG_DEMOD_FM, 16000, DSD_ANALOG_DEMOD_AM,
                                              10000, label);
        rc |= expect_switch_between_kinds_reverted(label, &opts, &state, DSD_ANALOG_DEMOD_AM, DSDCFG_MODE_AM, 16000,
                                                   10000, 10000, "Failed: Analog -> ", "NFM 16 kHz");
        finish_scan_row_on_analog(&opts, &state);

        DSD_SNPRINTF(label, sizeof label, "analog -> am under a row, refused %s", at_once ? "at once" : "on landing");
        rc |= init_scan_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, DSD_ANALOG_DEMOD_FM, 0,
                                      DSD_SCAN_MODE_NXDN48, NULL, label);
        rc |= submit_am_width(&opts, &state, 10000, label);
        rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, label);
        rc |= expect_int(label, dsd_scan_mode_configured_preset(&opts, &state) == DSDCFG_MODE_AM, 1);
        rc |= expect_int(label, g_analog_req_calls, 0);
        g_fake_demod_rate_hz = 7500;
        g_fake_request_rate_hz = 7500;
        g_analog_check_result = -1; /* the AM default does not fit the rate either */
        rc |= stop_scanner_refusing_the_leave(&opts, &state, at_once, DSD_ANALOG_DEMOD_AM, 10000, DSD_ANALOG_DEMOD_FM,
                                              0, label);
        rc |= expect_switch_between_kinds_reverted(label, &opts, &state, DSD_ANALOG_DEMOD_FM, DSDCFG_MODE_ANALOG, 0,
                                                   10000, 0, "Failed: AM -> ", "AM 10 kHz");
        finish_scan_row_on_analog(&opts, &state);
    }
    return rc;
}

/* When the front end refuses a request: at once, at the rate it publishes, or where the request lands. */
typedef enum { REFUSED_AT_ONCE, REFUSED_ON_LANDING } refusal_timing;

/* A switch between FM and AM armed under a scan row, whose leave the front end refuses keeping the kind the session
   ran, and whose revert's own monitor request it refuses as well
   (test_refused_leave_revert_refused_gets_the_leave_policy()). */
typedef struct {
    const char* name;
    int kind;               /* the kind the session runs, and the revert goes back to */
    int width_hz;           /* ... with this configured width */
    dsd_scan_mode row_mode; /* the row scanned */
    int row_width_hz;       /* an nfm row's own width (0: none) */
    int to_kind;            /* the kind the switch under the row is onto */
    int to_width_hz;        /* ... with this configured width */
    int pending_width_hz;   /* a width of kind set while the switch is pending (0: none) */
    int rate_hz;            /* the rate the row's retune left the front end on */
    int refuses_above_hz;   /* the widest width the front end queues at that rate */
    int check_refuses;      /* rtl_stream_check_analog_profile() refuses (the AM default, a pending width) */
    int monitor;            /* the front end keeps the monitor of kind at kept_width_hz (0: the row's profile) */
    int kept_width_hz;      /* the width the monitor runs, or the width setting of kind off it */
    int asked_hz;           /* what the revert asks of the front end, the hold applied */
    int end_width_hz;       /* the configured width of kind the decoder ends on */
    int end_calls;          /* monitor requests made in all */
    int end_request_hz;     /* the width of kind the last one asked for */
    const char* toast;
} leave_revert_case;

/* Stop the scanner under the row @p c scans, with the switch armed: the front end refuses the leave's return (@p leave),
   keeping what @p c says, the decoder goes back to @p c's kind, and the front end refuses the monitor request the revert
   makes as well (@p revert), keeping the same. */
static int
stop_scanner_refusing_the_leave_and_its_revert(dsd_opts* opts, dsd_state* state, const leave_revert_case* c,
                                               refusal_timing leave, refusal_timing revert, const char* label) {
    if (leave == REFUSED_AT_ONCE) {
        g_fake_analog_req_max_hz = c->refuses_above_hz;
    }
    int rc = submit_scanner_stop(opts, state, label);
    if (leave == REFUSED_AT_ONCE) {
        return rc;
    }
    rc |= expect_last_monitor_request(label, 1, c->to_kind, c->to_width_hz);
    demod_thread_refuses_leave_keeping(1, c->monitor, c->kind, c->kept_width_hz);
    if (revert == REFUSED_AT_ONCE) {
        g_fake_analog_req_max_hz = c->refuses_above_hz;
    }
    state->ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(opts, state);
    if (revert == REFUSED_ON_LANDING) {
        char tag[192];
        DSD_SNPRINTF(tag, sizeof tag, "%s: the revert asks for the kind it ran", label);
        rc |= expect_last_monitor_request(tag, 2, c->kind, c->asked_hz);
        rc |= expect_toast(tag, state, "Failed: ");
        demod_thread_refuses_leave_keeping(1, c->monitor, c->kind, c->kept_width_hz);
        state->ui_msg[0] = '\0';
        (void)dsd_app_drain_cmds(opts, state);
    }
    return rc;
}

/* The configured width of analog @p kind in @p opts. */
static int
configured_width_of(const dsd_opts* opts, int kind) {
    return kind == DSD_ANALOG_DEMOD_AM ? opts->analog_am_bandwidth_hz : opts->analog_nfm_bandwidth_hz;
}

/* Run @p c in the timings @p leave and @p revert, and check what the decoder ends on. */
static int
run_leave_revert_case(const leave_revert_case* c, refusal_timing leave, refusal_timing revert, const char* timing) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    char label[128];
    char tag[192];
    DSD_SNPRINTF(label, sizeof label, "%s, %s", c->name, timing);
    dsd_scan_option_values row = {0};
    if (c->row_width_hz > 0) {
        row.present = DSD_SCAN_OPT_BANDWIDTH;
        row.channel_bw_hz = c->row_width_hz;
    }
    int rc = init_scan_row_on_analog(&opts, &state, (RtlSdrContext*)fake_ctx, c->kind, c->width_hz, c->row_mode,
                                     c->row_width_hz > 0 ? &row : NULL, label);
    if (c->to_kind == DSD_ANALOG_DEMOD_AM) {
        rc |= submit_am_width(&opts, &state, c->to_width_hz, label);
        rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, label);
    } else {
        rc |= submit_nfm_width(&opts, &state, c->to_width_hz, label);
        rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_ANALOG, label);
    }
    if (c->pending_width_hz > 0) {
        rc |= (c->kind == DSD_ANALOG_DEMOD_AM) ? submit_am_width(&opts, &state, c->pending_width_hz, label)
                                               : submit_nfm_width(&opts, &state, c->pending_width_hz, label);
    }
    rc |= expect_int(label, g_analog_req_calls, 0);
    g_fake_demod_rate_hz = c->rate_hz;
    g_fake_request_rate_hz = c->rate_hz;
    g_analog_check_result = c->check_refuses ? -1 : 0;
    if (c->monitor) {
        g_fake_monitor_kind = c->kind;
        g_fake_monitor_width_hz = c->kept_width_hz;
        g_fake_monitor_lpf_on = 1;
    }
    rc |= stop_scanner_refusing_the_leave_and_its_revert(&opts, &state, c, leave, revert, label);
    DSD_SNPRINTF(tag, sizeof tag, "%s: back on the kind it ran", label);
    rc |= expect_int(tag, opts.analog_only == 1 && opts.analog_demod == c->kind, 1);
    DSD_SNPRINTF(tag, sizeof tag, "%s: its configured width", label);
    rc |= expect_int(tag, configured_width_of(&opts, c->kind), c->end_width_hz);
    DSD_SNPRINTF(tag, sizeof tag, "%s: the other kind's width kept", label);
    rc |= expect_int(tag, configured_width_of(&opts, c->to_kind), c->to_width_hz);
    DSD_SNPRINTF(tag, sizeof tag, "%s: the last monitor request", label);
    rc |= expect_last_monitor_request(tag, c->end_calls, c->kind, c->end_request_hz);
    DSD_SNPRINTF(tag, sizeof tag, "%s: toast", label);
    rc |= expect_toast(tag, &state, c->toast);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    DSD_SNPRINTF(tag, sizeof tag, "%s: reported once", label);
    rc |= expect_str(tag, state.ui_msg, "");
    finish_scan_row_on_analog(&opts, &state);
    return rc;
}

/*
 * Issue #578: a switch between FM and AM armed under a scan row goes back when the scan's leave is refused keeping the
 * other kind, and the monitor request that revert makes, for the kind the front end kept, is refused as well: the rate
 * a row's retune left cannot filter that kind's width either. The revert is part of the leave, so the leave's "kept
 * width, else default" holds for the kind it restores:
 * - an -fA session with NFM 16 kHz switched to AM 10 kHz under an NXDN48 row and left at a 7.5 kHz rate goes back to
 *   Analog on the NFM default, which is asked for and configured, rather than on NFM 16 kHz with the front end still on
 *   the row's digital filter and no monitor it can run;
 * - an -fM session with AM 10 kHz switched to Analog goes back to AM with its 10 kHz, which the AM default cannot
 *   replace at that rate: the toast says so;
 * - an NFM width changed while the switch was pending, which the rate cannot filter, goes back to the 12.5 kHz the
 *   front end holds before the revert asks for it (ui_hold_reverted_analog_width()), and the leave's rule settles what
 *   was asked: the NFM default;
 * - under an nfm row with its own 12.5 kHz, whose monitor the front end keeps, the configured 16 kHz takes the 12.5 kHz
 *   that monitor runs, and nothing more is asked.
 * Every timing ends the same: each refusal at once, at the rate the front end publishes, or where its request lands,
 * and the revert's refused at once after a leave refused where it lands (a retune moved the published rate in
 * between). The toast names the width refused for the kind restored and what the monitor runs, once.
 */
static int
test_refused_leave_revert_refused_gets_the_leave_policy(void) {
    static const leave_revert_case cases[] = {
        {"nfm 16k -> am under a row", DSD_ANALOG_DEMOD_FM, 16000, DSD_SCAN_MODE_NXDN48, 0, DSD_ANALOG_DEMOD_AM, 10000,
         0, 7500, 5000, 0, 0, 16000, 16000, 0, 3, 0,
         "Refused: NFM 16 kHz does not fit the 7.5 kHz DSP rate; the monitor is back on the NFM default"},
        {"am 10k -> analog under a row", DSD_ANALOG_DEMOD_AM, 10000, DSD_SCAN_MODE_NXDN48, 0, DSD_ANALOG_DEMOD_FM,
         16000, 0, 7500, 5000, 1, 0, 10000, 10000, 10000, 2, 10000,
         "Refused: AM 10 kHz does not fit the 7.5 kHz DSP rate; the AM default does not fit it either"},
        {"nfm 12.5k -> am, 16k set while pending", DSD_ANALOG_DEMOD_FM, 12500, DSD_SCAN_MODE_NXDN48, 0,
         DSD_ANALOG_DEMOD_AM, 10000, 16000, 7500, 5000, 1, 0, 12500, 12500, 0, 3, 0,
         "Refused: NFM 12.5 kHz does not fit the 7.5 kHz DSP rate; the monitor is back on the NFM default"},
        {"nfm 16k -> am under an nfm row's 12.5k", DSD_ANALOG_DEMOD_FM, 16000, DSD_SCAN_MODE_NFM, 12500,
         DSD_ANALOG_DEMOD_AM, 15000, 0, 16000, 13200, 0, 1, 12500, 16000, 12500, 2, 16000,
         "Refused: NFM 16 kHz does not fit the 16 kHz DSP rate; the monitor keeps NFM 12.5 kHz"},
    };
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        rc |= run_leave_revert_case(&cases[i], REFUSED_AT_ONCE, REFUSED_AT_ONCE, "both at once");
        rc |= run_leave_revert_case(&cases[i], REFUSED_ON_LANDING, REFUSED_ON_LANDING, "both on landing");
        rc |= run_leave_revert_case(&cases[i], REFUSED_ON_LANDING, REFUSED_AT_ONCE, "leave on landing, revert at once");
    }
    return rc;
}

/* A DMR session on an RTL-SDR input at a 16 kHz DSP bandwidth, with an explicit NFM width stored while digital (the
   front end is asked with the fake's answer, so a width the rate cannot filter gets as far as the request). */
static void
init_dmr_session_with_nfm_width(dsd_opts* opts, dsd_state* state, RtlSdrContext* fake_ctx, int width_hz) {
    init_decode_mode_context(opts, state);
    opts->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:851.375M:0:0:16");
    opts->rtl_dsp_bw_khz = 16;
    state->rtl_ctx = fake_ctx;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR);
    (void)dsd_app_drain_cmds(opts, state);
    opts->analog_nfm_bandwidth_hz = width_hz;
    opts->mod_cli_lock = 1;
    reset_rx_family_wrap();
    g_fake_cqpsk = 0;
    g_fake_analog_family = 0;
}

static int
expect_back_on_dmr(const char* label, const dsd_opts* opts, const dsd_state* state) {
    int rc = expect_int(label, opts->analog_only, 0);
    rc |= expect_int(label, opts->frame_dmr, 1);
    rc |= expect_int(label, dsd_infer_decode_mode_preset(opts) == DSDCFG_MODE_DMR, 1);
    rc |= expect_int(label, opts->mod_cli_lock, 1);
    rc |= expect_toast(label, state,
                       "Failed: Analog -> NFM 16 kHz does not fit the 16 kHz DSP rate (max 13.2 kHz); use a 24 or 48 "
                       "kHz DSP bandwidth");
    return rc;
}

/*
 * A switch to Analog whose explicit NFM width the front end took when asked, then refused: by the request itself, when
 * a retune moved the rate between the check and the request, or by the demod thread where the request landed, when it
 * moved after. Either way the front end stays on the digital family, so the decoder goes back to the mode it had, with
 * its options and modulation lock, and says why, rather than decode Analog from a digital front end. A switch the
 * front end takes stays, and a later mode change is not undone by the refusal of an earlier one. A config's [mode]
 * onto Analog is put back the same way.
 */
static int
test_refused_switch_onto_analog_puts_the_mode_back(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 16000);

    /* Refused by its request. */
    g_analog_req_result = -1;
    state.ui_msg[0] = '\0';
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    rc |= expect_int("request refused: drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("request refused: checked first", g_analog_check_calls, 1);
    rc |= expect_back_on_dmr("request refused: back on DMR", &opts, &state);
    g_analog_req_result = 0;

    /* Refused where it landed: on Analog while pending, back on DMR once the stream says so. */
    reset_rx_family_wrap();
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("landing: on Analog while pending", opts.analog_only, 1);
    rc |= expect_int("landing: switch requested", g_analog_req_calls == 1 && g_analog_req_width_hz == 16000, 1);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("landing: still Analog while pending", opts.analog_only, 1);
    demod_thread_refuses_analog_keeping(0, 0);
    state.ui_msg[0] = '\0';
    rc |= expect_int("landing: settled on an empty drain", dsd_app_drain_cmds(&opts, &state), 0);
    rc |= expect_back_on_dmr("landing: back on DMR", &opts, &state);
    rc |= expect_int("landing: the digital profile republished", g_demod_req_calls >= 1, 1);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("landing: reported once", state.ui_msg[0] == '\0', 1);

    /* Taken: Analog stays. */
    reset_rx_family_wrap();
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    (void)dsd_app_drain_cmds(&opts, &state);
    demod_thread_lands(0);
    g_fake_analog_family = 1;
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("taken: Analog stays", opts.analog_only, 1);

    /* Switched to Analog and on to P25 before either landed: the P25 request replaced the Analog switch in the queue,
       so the stream reports that switch replaced, never taken, whatever it then says of P25's own request, and the
       decoder stays where the operator put it last. */
    freeState(&state);
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 16000);
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    (void)dsd_app_drain_cmds(&opts, &state);
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_P25P1);
    (void)dsd_app_drain_cmds(&opts, &state);
    demod_thread_refuses_analog_keeping(0, 0);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("moved on: P25 stays", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_P25P1, 1);

    /* A config's [mode] onto Analog, refused where it landed. */
    freeState(&state);
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 16000);
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_ANALOG, "config landing");
    rc |= expect_int("config landing: on Analog while pending", opts.analog_only, 1);
    demod_thread_refuses_analog_keeping(0, 0);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_back_on_dmr("config landing: back on DMR", &opts, &state);

    /* ... and refused by its request: the apply fails. */
    freeState(&state);
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 16000);
    g_analog_req_result = -1;
    state.ui_msg[0] = '\0';
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_ANALOG, "config request refused");
    rc |= expect_back_on_dmr("config request refused: back on DMR", &opts, &state);
    g_analog_req_result = 0;

    g_fake_analog_family = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * A width set right after a switch onto Analog, in the same drain, is the width command's, not a later mode change:
 * the front end still refusing the switch where it lands (its analog request now carries that width) puts the decoder
 * back on the mode it had and says so, rather than leave Analog running on a digital front end, and the width the
 * command set stays configured.
 */
static int
test_refused_switch_onto_analog_with_a_width_set_after_it(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 16000);
    rc |= expect_int("switch + width: switch queued",
                     dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("switch + width: width queued", dsd_app_command_set_i32(DSD_APP_CMD_NFM_BANDWIDTH_SET, 12500),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("switch + width: one drain", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_int("switch + width: on Analog while pending", opts.analog_only, 1);
    rc |= expect_int("switch + width: the last request carries the width", g_analog_req_width_hz, 12500);
    demod_thread_refuses_analog_keeping(0, 0);
    state.ui_msg[0] = '\0';
    g_demod_req_calls = 0;
    rc |= expect_int("switch + width: settled on an empty drain", dsd_app_drain_cmds(&opts, &state), 0);
    rc |= expect_int("switch + width: back off Analog", opts.analog_only, 0);
    rc |= expect_int("switch + width: back on DMR", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_DMR, 1);
    rc |= expect_int("switch + width: the modulation lock back", opts.mod_cli_lock, 1);
    rc |= expect_toast("switch + width: says why", &state, "Failed: Analog -> the RTL front end refused NFM 12.5 kHz");
    rc |= expect_int("switch + width: the command's width stays", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("switch + width: the digital profile republished", g_demod_req_calls >= 1, 1);

    g_fake_analog_family = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * The same refusals under a scan row that inherits the configured mode: the switch reaches the front end once the
 * row's constraint is back, and a refusal puts the configured mode back under the row as well, the row still running.
 */
static int
test_refused_switch_onto_analog_under_a_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 16000);
    rc |= expect_int("row: inherit", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_INHERIT), 0);
    rc |= expect_int("row: options", dsd_scan_mode_options(&opts, &state, NULL), 0);

    /* Refused by its request, made after the row's resume: the command fails. */
    reset_rx_family_wrap();
    g_analog_req_result = -1;
    state.ui_msg[0] = '\0';
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("row request refused: asked after the resume", g_analog_req_calls >= 1, 1);
    rc |= expect_back_on_dmr("row request refused: back on DMR", &opts, &state);
    rc |= expect_int("row request refused: configured DMR",
                     dsd_scan_mode_configured_preset(&opts, &state) == DSDCFG_MODE_DMR, 1);
    rc |= expect_int("row request refused: the row stays", dsd_scan_mode_configured_view(&state) != NULL, 1);
    g_analog_req_result = 0;

    /* Refused where it landed. */
    reset_rx_family_wrap();
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("row landing: on Analog while pending", opts.analog_only, 1);
    demod_thread_refuses_analog_keeping(0, 0);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_back_on_dmr("row landing: back on DMR", &opts, &state);
    rc |=
        expect_int("row landing: configured DMR", dsd_scan_mode_configured_preset(&opts, &state) == DSDCFG_MODE_DMR, 1);
    rc |= expect_int("row landing: the row stays", dsd_scan_mode_configured_view(&state) != NULL, 1);

    dsd_scan_mode_leave(&opts, &state);
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* The decoder is back on DMR, timed for the 24 kHz demod rate the front end runs now (5 samples per symbol), and the
   front end was handed that timing with the DMR profile. */
static int
expect_dmr_timed_for_24k(const char* label, const dsd_opts* opts, const dsd_state* state) {
    int rc = expect_int(label, opts->analog_only, 0);
    rc |= expect_int(label, dsd_infer_decode_mode_preset(opts) == DSDCFG_MODE_DMR, 1);
    rc |= expect_int(label, state->samplesPerSymbol, 5);
    rc |= expect_int(label, state->symbolCenter, dsd_opts_symbol_center(5));
    rc |= expect_int(label, g_demod_req_calls >= 1 && g_demod_req_rate == 4800 && g_demod_req_ted_sps == 5, 1);
    return rc;
}

/*
 * The retune that gets a switch onto Analog refused moves the demod rate, here from 48 to 24 kHz. The decoder goes back
 * to the mode it had, but not to the symbol timing it had at 48 kHz: 10 samples per symbol at 24 kHz, handed to the
 * front end with the modulation locked, would keep the SPS hunt from ever correcting it. It is timed for the rate the
 * front end runs now, whether the request itself or the demod thread refused the switch, and under a scan row too.
 */
static int
test_refused_switch_onto_analog_retimes_the_mode(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;

    /* Refused where it landed, after the rate moved. */
    g_fake_output_rate_hz = 48000U;
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 16000);
    rc |= expect_int("retime: DMR timed for 48 kHz", state.samplesPerSymbol, 10);
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("retime landing: on Analog while pending", opts.analog_only, 1);
    g_fake_output_rate_hz = 24000U;
    demod_thread_refuses_analog_keeping(0, 0);
    g_demod_req_calls = g_demod_req_rate = g_demod_req_ted_sps = 0;
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_dmr_timed_for_24k("retime landing: DMR at 24 kHz", &opts, &state);

    /* Refused by its request: the rate moved between the session's timing and the switch. */
    freeState(&state);
    g_fake_output_rate_hz = 48000U;
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 16000);
    g_fake_output_rate_hz = 24000U;
    g_analog_req_result = -1;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_dmr_timed_for_24k("retime request: DMR at 24 kHz", &opts, &state);
    g_analog_req_result = 0;

    /* Under a row that inherits the configured mode, refused where it landed. */
    freeState(&state);
    g_fake_output_rate_hz = 48000U;
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 16000);
    rc |= expect_int("retime row: inherit", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_INHERIT), 0);
    rc |= expect_int("retime row: options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("retime row: on Analog while pending", opts.analog_only, 1);
    g_fake_output_rate_hz = 24000U;
    demod_thread_refuses_analog_keeping(0, 0);
    g_demod_req_calls = g_demod_req_rate = g_demod_req_ted_sps = 0;
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_dmr_timed_for_24k("retime row: DMR at 24 kHz", &opts, &state);
    rc |= expect_int("retime row: the row stays", dsd_scan_mode_configured_view(&state) != NULL, 1);

    dsd_scan_mode_leave(&opts, &state);
    g_fake_output_rate_hz = 0U;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * DSD_NEO_CHANNEL_LPF=0 turns off the channel filter every explicit NFM width needs, and every stream start refuses
 * such a width for it, whatever the rate. A change that would commit to a start with one is refused first and changes
 * nothing, so the running input is never torn down for a start that cannot open: a width with no stream running, a
 * config that reopens an RTL-SDR at a new DSP bandwidth or an Airspy at the rate it delivers, a DSP bandwidth change,
 * and Input > Switch source > RTL-SDR or Airspy. The unset default follows the environment and is never refused. PCM
 * input runs no channel filter, so there the width is only stored, and a switch from PCM to a radio input holds it.
 */
static int
test_nfm_width_changes_held_to_channel_lpf_off(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:48");
    opts.rtl_dsp_bw_khz = 48;
    (void)dsd_setenv("DSD_NEO_CHANNEL_LPF", "0", 1);
    dsd_neo_config_init();

    state.rtl_ctx = NULL;
    rc |= submit_nfm_width(&opts, &state, 12500, "lpf off: width with no stream");
    rc |= expect_int("lpf off: width not stored", opts.analog_nfm_bandwidth_hz, 0);
    rc |= expect_toast("lpf off: width toast", &state,
                       "Refused: NFM 12.5 kHz needs the channel filter, which DSD_NEO_CHANNEL_LPF=0 turns off");
    rc |= submit_nfm_width(&opts, &state, 0, "lpf off: the default");
    rc |= expect_toast("lpf off: the default toast", &state, "Applied: NFM bandwidth -> default");

    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    reset_rx_family_wrap();
    rc |= submit_config_rtl_bw(&opts, &state, 24, 12500, "lpf off: rtl reopen");
    rc |= expect_int("lpf off: rtl reopen width kept", opts.analog_nfm_bandwidth_hz, 0);
    rc |= expect_int("lpf off: rtl reopen rate kept", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_int("lpf off: rtl reopen stream kept", state.rtl_ctx == (RtlSdrContext*)fake_ctx, 1);
    rc |= expect_toast(
        "lpf off: rtl reopen toast", &state,
        "Config not applied: NFM 12.5 kHz needs the channel filter, which DSD_NEO_CHANNEL_LPF=0 turns off");

    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
    reset_rx_family_wrap();
    rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_AIRSPY, 24, 12500, "lpf off: airspy reopen");
    rc |= expect_int("lpf off: airspy reopen width kept", opts.analog_nfm_bandwidth_hz, 0);
    rc |= expect_int("lpf off: airspy reopen stream kept", state.rtl_ctx == (RtlSdrContext*)fake_ctx, 1);
    rc |= expect_toast(
        "lpf off: airspy reopen toast", &state,
        "Config not applied: NFM 12.5 kHz needs the channel filter, which DSD_NEO_CHANNEL_LPF=0 turns off");

    /* An explicit width already configured (a session started before the variable was set): nothing that reopens
       the device for it goes ahead. */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:48");
    opts.analog_nfm_bandwidth_hz = 16000;
    opts.audio_in_type = AUDIO_IN_NULL; /* no restart: the check is on the options alone */
    state.ui_msg[0] = '\0';
    (void)dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_BW, 24);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("lpf off: dsp bandwidth kept", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_toast("lpf off: dsp bandwidth toast", &state,
                       "Refused: NFM 16 kHz needs the channel filter, which DSD_NEO_CHANNEL_LPF=0 turns off");
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
    state.ui_msg[0] = '\0';
    rc |= expect_true("lpf off: rtl input queued", post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT) > 0);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_str("lpf off: rtl input: the running input stays", opts.audio_in_dev, "airspy");
    rc |= expect_int("lpf off: rtl input: stream kept", state.rtl_ctx == (RtlSdrContext*)fake_ctx, 1);
    rc |= expect_toast("lpf off: rtl input toast", &state,
                       "Refused: NFM 16 kHz needs the channel filter, which DSD_NEO_CHANNEL_LPF=0 turns off");
    /* Issue #578: Input > Switch source > Airspy too. The Airspy sets its own rate, but no rate runs the width without
       its filter, so the switch is refused before the running RTL-SDR is torn down. */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:48");
    g_config_rtl_creates = 0;
    rc |= expect_true("lpf off: airspy input queued", post_empty(DSD_APP_CMD_AIRSPY_ENABLE_INPUT) > 0);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_str("lpf off: airspy input: the running input stays", opts.audio_in_dev, "rtl:0:851.375M:0:0:48");
    rc |= expect_int("lpf off: airspy input: still an RTL input", opts.audio_in_type, AUDIO_IN_RTL);
    rc |= expect_int("lpf off: airspy input: stream kept", state.rtl_ctx == (RtlSdrContext*)fake_ctx, 1);
    rc |= expect_int("lpf off: airspy input: nothing opened", g_config_rtl_creates, 0);
    rc |= expect_toast("lpf off: airspy input toast", &state,
                       "Refused: NFM 16 kHz needs the channel filter, which DSD_NEO_CHANNEL_LPF=0 turns off");

    /* PCM input runs no channel filter, so the variable says nothing about a width there, as a PCM start with the same
       width says nothing: a switch to Analog keeps the stored width and goes ahead, and a width set is stored. */
    opts.audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    state.rtl_ctx = NULL;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_DMR);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("lpf off: pcm digital", opts.analog_only, 0);
    state.ui_msg[0] = '\0';
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("lpf off: pcm switch to Analog", opts.analog_only, 1);
    rc |= expect_int("lpf off: pcm width kept", opts.analog_nfm_bandwidth_hz, 16000);
    rc |= expect_toast("lpf off: pcm switch toast", &state, "Decoding Analog");
    rc |= submit_nfm_width(&opts, &state, 12500, "lpf off: pcm width");
    rc |= expect_int("lpf off: pcm width stored", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_toast("lpf off: pcm width toast", &state, "Applied: NFM bandwidth -> 12.5 kHz");
    /* The width stored on PCM holds a switch to the Airspy, which would run it (issue #578): the PCM input stays. */
    g_config_rtl_creates = 0;
    (void)post_empty(DSD_APP_CMD_AIRSPY_ENABLE_INPUT);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("lpf off: pcm to airspy: still PCM", opts.audio_in_type, AUDIO_IN_PULSE);
    rc |= expect_str("lpf off: pcm to airspy: device kept", opts.audio_in_dev, "pulse");
    rc |= expect_int("lpf off: pcm to airspy: no stream", state.rtl_ctx == NULL, 1);
    rc |= expect_int("lpf off: pcm to airspy: nothing opened", g_config_rtl_creates, 0);
    rc |= expect_toast("lpf off: pcm to airspy toast", &state,
                       "Refused: NFM 12.5 kHz needs the channel filter, which DSD_NEO_CHANNEL_LPF=0 turns off");
    /* A start the variable refused anyway is reported with it, whatever rate the device delivered, and the AM default
       as AM's (svc_describe_start_failure()). */
    char why[128];
    reset_config_rtl_wrap();
    g_config_rtl_refused = 1;
    g_config_rtl_refused_kind = DSD_ANALOG_DEMOD_FM;
    g_config_rtl_refused_width_hz = 12500;
    g_config_rtl_refused_rate_hz = 78125;
    rc |= expect_int("lpf off: start refusal", svc_describe_start_failure(&opts, why, sizeof why), 1);
    rc |= expect_str("lpf off: start refusal names the variable", why,
                     "NFM 12.5 kHz needs the channel filter, which DSD_NEO_CHANNEL_LPF=0 turns off");
    g_config_rtl_refused_kind = DSD_ANALOG_DEMOD_AM;
    g_config_rtl_refused_width_hz = 0;
    rc |= expect_int("lpf off: AM start refusal", svc_describe_start_failure(&opts, why, sizeof why), 1);
    rc |= expect_str("lpf off: AM start refusal names the variable", why,
                     "AM needs the channel filter, which DSD_NEO_CHANNEL_LPF=0 turns off");
    reset_config_rtl_wrap();
    opts.audio_in_type = AUDIO_IN_RTL;

    (void)dsd_unsetenv("DSD_NEO_CHANNEL_LPF");
    dsd_neo_config_init();
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* An Analog (NFM) session on an RTL-SDR input whose DSP bandwidth is 16 kHz, the front end on the FM monitor, with the
   AM width @p am_width_hz configured for a switch to AM. */
static void
init_nfm_session_with_am_width(dsd_opts* opts, dsd_state* state, RtlSdrContext* fake_ctx, int am_width_hz) {
    init_decode_mode_context(opts, state);
    opts->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:118.1M:0:0:16");
    opts->rtl_dsp_bw_khz = 16;
    state->rtl_ctx = fake_ctx;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    (void)dsd_app_drain_cmds(opts, state);
    opts->analog_am_bandwidth_hz = am_width_hz;
    reset_rx_family_wrap();
    g_fake_cqpsk = 0;
    g_fake_analog_family = 1;
}

static int
expect_back_on_nfm(const char* label, const dsd_opts* opts, const dsd_state* state, const char* toast) {
    int rc = expect_int(label, opts->analog_only, 1);
    rc |= expect_int(label, opts->analog_demod, DSD_ANALOG_DEMOD_FM);
    rc |= expect_int(label, dsd_infer_decode_mode_preset(opts) == DSDCFG_MODE_ANALOG, 1);
    rc |= expect_toast(label, state, toast);
    return rc;
}

/*
 * Issue #524: a switch between FM and AM on the running monitor, held to the rate first like a switch onto it, that the
 * front end refuses after all (by the request itself, or where it lands, a retune having moved the rate): the front end
 * keeps the FM monitor, so the decoder goes back to Analog and says why, rather than decode AM from an FM front end. A
 * config's [mode] onto AM is put back the same way. A switch the front end takes stays.
 */
static int
test_refused_switch_between_fm_and_am_puts_the_mode_back(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    static const char k_refusal[] =
        "Failed: AM -> AM 15 kHz does not fit the 16 kHz DSP rate (max 13.2 kHz); use a 24 or 48 kHz DSP bandwidth";
    int rc = 0;
    init_nfm_session_with_am_width(&opts, &state, (RtlSdrContext*)fake_ctx, 15000);

    /* Refused by its request. */
    g_analog_req_result = -1;
    state.ui_msg[0] = '\0';
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_AM);
    rc |= expect_int("fm -> am request refused: drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("fm -> am request refused: checked first",
                     g_analog_check_calls == 1 && g_analog_check_kind == DSD_ANALOG_DEMOD_AM, 1);
    rc |= expect_back_on_nfm("fm -> am request refused: back on Analog", &opts, &state, k_refusal);
    /* Back on the analog monitor, whose raw sink it has: no digital voice stream is opened for it. */
    rc |= expect_int("fm -> am request refused: raw sink kept", g_ensure_analog_calls > 0, 1);
    rc |= expect_int("fm -> am request refused: no digital sink opened", g_ensure_digital_calls, 0);
    g_analog_req_result = 0;

    /* Refused where it landed: on AM while pending, back on Analog once the stream says it kept FM. */
    reset_rx_family_wrap();
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_AM);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("fm -> am landing: on AM while pending", opts.analog_demod, DSD_ANALOG_DEMOD_AM);
    rc |= expect_int("fm -> am landing: AM requested",
                     g_analog_req_kind == DSD_ANALOG_DEMOD_AM && g_analog_req_width_hz == 15000, 1);
    demod_thread_refuses_analog_keeping_kind(1, DSD_ANALOG_DEMOD_FM, 0);
    state.ui_msg[0] = '\0';
    g_ensure_analog_calls = g_ensure_digital_calls = 0;
    rc |= expect_int("fm -> am landing: settled on an empty drain", dsd_app_drain_cmds(&opts, &state), 0);
    rc |= expect_back_on_nfm("fm -> am landing: back on Analog", &opts, &state, k_refusal);
    rc |= expect_int("fm -> am landing: raw sink ensured for Analog", g_ensure_analog_calls, 1);
    rc |= expect_int("fm -> am landing: no digital sink opened", g_ensure_digital_calls, 0);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("fm -> am landing: reported once", state.ui_msg[0] == '\0', 1);

    /* Taken: AM stays. */
    reset_rx_family_wrap();
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_AM);
    (void)dsd_app_drain_cmds(&opts, &state);
    demod_thread_lands(0);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("fm -> am taken: AM stays", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_AM, 1);

    /* A config's [mode] onto AM from Analog, refused where it landed. */
    freeState(&state);
    init_nfm_session_with_am_width(&opts, &state, (RtlSdrContext*)fake_ctx, 15000);
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_AM, "config fm -> am");
    rc |= expect_int("config fm -> am: on AM while pending", opts.analog_demod, DSD_ANALOG_DEMOD_AM);
    demod_thread_refuses_analog_keeping_kind(1, DSD_ANALOG_DEMOD_FM, 0);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_back_on_nfm("config fm -> am landing: back on Analog", &opts, &state, k_refusal);

    /* ... and refused by its request: the apply fails. */
    freeState(&state);
    init_nfm_session_with_am_width(&opts, &state, (RtlSdrContext*)fake_ctx, 15000);
    g_analog_req_result = -1;
    state.ui_msg[0] = '\0';
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_AM, "config fm -> am request refused");
    rc |= expect_back_on_nfm("config fm -> am request refused: back on Analog", &opts, &state, k_refusal);
    g_analog_req_result = 0;

    g_fake_analog_family = 0;
    opts.analog_am_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* The toggles a switch between FM and AM can meet before the front end refuses it where it lands: each changes a
   setting the switch is compared by, and none asks the front end for a receive profile of its own. */
static const struct {
    int cmd;
    const char* name;
} k_unpublished_toggles[] = {
    {DSD_APP_CMD_COSINE_FILTER_TOGGLE, "cosine filter toggle"},
    {DSD_APP_CMD_INV_DMR_TOGGLE, "DMR inversion toggle"},
    {DSD_APP_CMD_INVERT_TOGGLE, "inversion toggle"},
    {DSD_APP_CMD_INPUT_MONITOR_TOGGLE, "input monitor toggle"},
};

/* The settings those toggles change, packed so one comparison says whether a toggle's change stood. */
static int
unpublished_toggle_settings(const dsd_opts* opts) {
    return (opts->use_cosine_filter ? 1 : 0) | (opts->inverted_dmr ? 2 : 0) | (opts->inverted_dpmr ? 4 : 0)
           | (opts->inverted_x2tdma ? 8 : 0) | (opts->inverted_ysf ? 16 : 0) | (opts->inverted_m17 ? 32 : 0)
           | (opts->monitor_input_audio ? 64 : 0);
}

/*
 * Issue #582: a switch between FM and AM the front end refuses where it lands, after a toggle drained in the same pass
 * (or any time before the refusal) changed a setting the switch is compared by: the cosine filter, an inversion, the
 * input monitor. Those toggles ask the front end for no receive profile of their own, so the settings did not move on
 * from the switch the way a later mode or modulation command's would: the decoder still goes back to the kind the front
 * end kept, and says why, keeping what the toggle set. It was left on the refused kind with no word, over a front end
 * running the other one, whose monitor getSymbol() then follows no more.
 */
static int
test_refused_switch_after_an_unpublished_toggle_puts_the_mode_back(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    static const char k_refusal[] =
        "Failed: AM -> AM 15 kHz does not fit the 16 kHz DSP rate (max 13.2 kHz); use a 24 or 48 kHz DSP bandwidth";
    int rc = 0;
    for (size_t i = 0; i < sizeof k_unpublished_toggles / sizeof k_unpublished_toggles[0]; i++) {
        char label[128];
        init_nfm_session_with_am_width(&opts, &state, (RtlSdrContext*)fake_ctx, 15000);
        reset_rx_family_wrap();
        const int settings_before = unpublished_toggle_settings(&opts);
        (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_AM);
        (void)dsd_app_command_action(k_unpublished_toggles[i].cmd);
        DSD_SNPRINTF(label, sizeof label, "fm -> am + %s: drained", k_unpublished_toggles[i].name);
        rc |= expect_int(label, dsd_app_drain_cmds(&opts, &state), 2);
        DSD_SNPRINTF(label, sizeof label, "fm -> am + %s: on AM while pending", k_unpublished_toggles[i].name);
        rc |= expect_int(label, opts.analog_demod, DSD_ANALOG_DEMOD_AM);
        const int settings_toggled = unpublished_toggle_settings(&opts);
        DSD_SNPRINTF(label, sizeof label, "fm -> am + %s: toggled", k_unpublished_toggles[i].name);
        rc |= expect_int(label, settings_toggled != settings_before, 1);
        demod_thread_refuses_analog_keeping_kind(1, DSD_ANALOG_DEMOD_FM, 0);
        state.ui_msg[0] = '\0';
        (void)dsd_app_drain_cmds(&opts, &state);
        /* Back on the analog monitor's FM kind (the input monitor toggle turns the monitor itself off, which no preset
           does, so the preset is not inferred), with the refusal said. */
        DSD_SNPRINTF(label, sizeof label, "fm -> am + %s: back on the analog family", k_unpublished_toggles[i].name);
        rc |= expect_int(label, opts.analog_only, 1);
        DSD_SNPRINTF(label, sizeof label, "fm -> am + %s: back on FM", k_unpublished_toggles[i].name);
        rc |= expect_int(label, opts.analog_demod, DSD_ANALOG_DEMOD_FM);
        DSD_SNPRINTF(label, sizeof label, "fm -> am + %s: refusal said", k_unpublished_toggles[i].name);
        rc |= expect_toast(label, &state, k_refusal);
        DSD_SNPRINTF(label, sizeof label, "fm -> am + %s: the toggle stands", k_unpublished_toggles[i].name);
        rc |= expect_int(label, unpublished_toggle_settings(&opts), settings_toggled);
        g_fake_analog_family = 0;
        opts.analog_am_bandwidth_hz = 0;
        opts.use_cosine_filter = 0;
        opts.inverted_dmr = opts.inverted_dpmr = opts.inverted_x2tdma = opts.inverted_ysf = opts.inverted_m17 = 0;
        state.rtl_ctx = NULL;
        freeState(&state);
    }
    return rc;
}

/* A DMR session with explicit NFM and AM widths the front end refuses once a retune has moved its rate, switched to AM
   and on to Analog before the front end took the AM switch: the request queue is last-writer-wins, so the Analog
   request replaced the AM one, which never ran. The Analog switch is a second DECODE_MODE_SET in a later drain, or
   (@p one_drain) a config's [mode] queued with the AM one and applied in the same drain (two DECODE_MODE_SETs queued
   together coalesce into the last). */
static void
switch_dmr_to_am_then_analog(dsd_opts* opts, dsd_state* state, RtlSdrContext* fake_ctx, int one_drain) {
    init_dmr_session_with_nfm_width(opts, state, fake_ctx, 16000);
    opts->analog_am_bandwidth_hz = 15000;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_AM);
    if (one_drain) {
        dsdneoUserConfig cfg;
        DSD_MEMSET(&cfg, 0, sizeof cfg);
        cfg.has_mode = 1;
        cfg.decode_mode = DSDCFG_MODE_ANALOG;
        (void)dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof(cfg));
    } else {
        (void)dsd_app_drain_cmds(opts, state);
        (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    }
    (void)dsd_app_drain_cmds(opts, state);
}

/* The front end's last receive request asked for the digital family, which it runs, with the symbol profile after it. */
static int
expect_digital_family_requested_last(const char* label) {
    int rc = expect_int(label, g_analog_req_family, DSD_RX_FAMILY_DIGITAL);
    rc |= expect_int(label, g_demod_req_calls > 0 && g_demod_req_order > g_analog_req_order, 1);
    return rc;
}

/*
 * Issue #524: switches onto the monitor and between its kinds made back to back, before the front end has taken the
 * first. It never ran the first (its requests are last-writer-wins), so a refusal of the last one puts the decoder back
 * on the mode the front end still runs, the one before the first switch, and republishes it, rather than on the mode
 * in between: that one's own request would be refused too once the rate has moved, leaving the decoder on AM against a
 * digital front end. The same holds when the last is refused at once, and whether the two switches came in one drain
 * or two. A switch the front end took before the next one began stays the one a refusal puts back.
 */
static int
test_refused_switch_after_a_pending_switch_puts_the_running_mode_back(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    static const char k_refusal[] = "Failed: Analog -> NFM 16 kHz does not fit the 16 kHz DSP rate (max 13.2 kHz); use "
                                    "a 24 or 48 kHz DSP bandwidth";
    int rc = 0;

    /* DMR -> AM -> Analog in two drains, the last refused where it landed: the front end stayed digital. */
    switch_dmr_to_am_then_analog(&opts, &state, (RtlSdrContext*)fake_ctx, 0);
    rc |= expect_int("pending chain: on Analog while pending",
                     opts.analog_only == 1 && opts.analog_demod == DSD_ANALOG_DEMOD_FM, 1);
    demod_thread_refuses_analog_keeping(0, 0);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_back_on_dmr("pending chain landing: back on DMR", &opts, &state);
    rc |= expect_digital_family_requested_last("pending chain landing: the digital family republished");

    /* ... in one drain, Analog by a config's [mode]. */
    freeState(&state);
    switch_dmr_to_am_then_analog(&opts, &state, (RtlSdrContext*)fake_ctx, 1);
    rc |= expect_int("one-drain chain: on Analog while pending",
                     opts.analog_only == 1 && opts.analog_demod == DSD_ANALOG_DEMOD_FM, 1);
    rc |= expect_int("one-drain chain: both switches requested", g_analog_req_calls, 2);
    demod_thread_refuses_analog_keeping(0, 0);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_back_on_dmr("one-drain chain landing: back on DMR", &opts, &state);
    rc |= expect_digital_family_requested_last("one-drain chain landing: the digital family republished");

    /* ... the last refused by its request. */
    freeState(&state);
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 16000);
    opts.analog_am_bandwidth_hz = 15000;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_AM);
    (void)dsd_app_drain_cmds(&opts, &state);
    g_analog_req_result = -1;
    state.ui_msg[0] = '\0';
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_back_on_dmr("pending chain request refused: back on DMR", &opts, &state);
    rc |= expect_digital_family_requested_last("pending chain request refused: the digital family republished");
    g_analog_req_result = 0;

    /* ... with an AM width set between the two switches (issue #526 compares the widths of analog settings): the width
       is no part of either switch, so the Analog switch still supersedes the pending AM one, the refusal goes back to
       DMR, and the width stays. */
    freeState(&state);
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 16000);
    opts.analog_am_bandwidth_hz = 15000;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_AM);
    (void)dsd_app_drain_cmds(&opts, &state);
    (void)dsd_app_command_set_i32(DSD_APP_CMD_AM_BANDWIDTH_SET, 10000);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("width between: the AM width set", opts.analog_am_bandwidth_hz, 10000);
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    (void)dsd_app_drain_cmds(&opts, &state);
    demod_thread_refuses_analog_keeping(0, 0);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_back_on_dmr("width between: back on DMR", &opts, &state);
    rc |= expect_int("width between: the AM width stays", opts.analog_am_bandwidth_hz, 10000);
    rc |= expect_digital_family_requested_last("width between: the digital family republished");

    /* The AM switch taken before the Analog one: a refusal of Analog, the front end keeping AM, goes back to AM. */
    freeState(&state);
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 16000);
    opts.analog_am_bandwidth_hz = 15000;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_AM);
    (void)dsd_app_drain_cmds(&opts, &state);
    demod_thread_lands(0);
    g_fake_analog_family = 1;
    (void)dsd_app_drain_cmds(&opts, &state);
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    (void)dsd_app_drain_cmds(&opts, &state);
    demod_thread_refuses_analog_keeping_kind(1, DSD_ANALOG_DEMOD_AM, 15000);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("taken chain: back on AM",
                     opts.analog_only == 1 && opts.analog_demod == DSD_ANALOG_DEMOD_AM
                         && dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_AM,
                     1);
    rc |= expect_toast("taken chain: says why", &state, k_refusal);

    g_fake_analog_family = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    opts.analog_am_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* An Analog session with an explicit NFM 16 kHz width, switched to AM and back to Analog before any drain found the AM
   switch taken. @p take_at_once nonzero: in one drain, the Analog switch a config's [mode] queued with the AM one, and
   the front end takes the AM switch as soon as it is queued (g_fake_take_next_analog_at_once). 0: in the next drain,
   which starts with the AM switch still pending, and the front end takes it while the Analog switch runs. */
static void
switch_nfm_to_am_and_back(dsd_opts* opts, dsd_state* state, RtlSdrContext* fake_ctx, int take_at_once) {
    init_nfm_session_with_am_width(opts, state, fake_ctx, 0);
    opts->analog_nfm_bandwidth_hz = 16000;
    g_fake_take_next_analog_at_once = take_at_once;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_AM);
    if (take_at_once) {
        dsdneoUserConfig cfg;
        DSD_MEMSET(&cfg, 0, sizeof cfg);
        cfg.has_mode = 1;
        cfg.decode_mode = DSDCFG_MODE_ANALOG;
        (void)dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof(cfg));
    } else {
        (void)dsd_app_drain_cmds(opts, state);
        g_fake_take_before_next_analog = 1;
        (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    }
    state->ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(opts, state);
}

/* The decoder runs AM with the AM width @p am_width_hz, and the front end was last asked for that profile. */
static int
expect_on_am_as_asked(const char* label, const dsd_opts* opts, int am_width_hz) {
    int rc = expect_int(label, opts->analog_only == 1 && opts->analog_demod == DSD_ANALOG_DEMOD_AM, 1);
    rc |= expect_int(label, dsd_infer_decode_mode_preset(opts) == DSDCFG_MODE_AM, 1);
    rc |= expect_int(label, opts->analog_am_bandwidth_hz, am_width_hz);
    rc |= expect_int(label, g_analog_req_family == DSD_RX_FAMILY_ANALOG && g_analog_req_kind == DSD_ANALOG_DEMOD_AM, 1);
    rc |= expect_int(label, g_analog_req_width_hz, am_width_hz);
    return rc;
}

/*
 * Issue #524: NFM 16 kHz, then AM, then NFM 16 kHz again, where the front end took the AM switch at a point no drain
 * saw before the second switch was made: between the two commands of one drain, or while the second command ran. A
 * retune then moves the DSP rate to 16 kHz, which filters AM's 6 kHz but not NFM 16 kHz, and the front end refuses the
 * second switch, keeping AM. The decoder goes back to AM, the mode the front end runs, rather than to NFM 16 kHz from
 * before the AM switch, whose own request the moved rate refuses as well: that would leave the decoder on FM against an
 * AM front end. The same holds when the second switch is refused at once by its request.
 */
static int
test_refused_switch_after_a_taken_switch_puts_the_running_mode_back(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    static const char k_refusal[] = "Failed: Analog -> NFM 16 kHz does not fit the 16 kHz DSP rate (max 13.2 kHz); use "
                                    "a 24 or 48 kHz DSP bandwidth";
    int rc = 0;

    /* Taken between the two commands of one drain; the second refused where it landed. */
    switch_nfm_to_am_and_back(&opts, &state, (RtlSdrContext*)fake_ctx, 1);
    rc |= expect_int("taken between commands: both switches requested", g_analog_req_calls, 2);
    rc |= expect_int("taken between commands: on Analog while pending", opts.analog_demod, DSD_ANALOG_DEMOD_FM);
    demod_thread_refuses_analog_keeping_kind(1, DSD_ANALOG_DEMOD_AM, 0);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_on_am_as_asked("taken between commands, refused landing: back on AM", &opts, 0);
    rc |= expect_toast("taken between commands, refused landing: says why", &state, k_refusal);

    /* ... the second refused at once by its request. */
    freeState(&state);
    switch_nfm_to_am_and_back(&opts, &state, (RtlSdrContext*)fake_ctx, 2);
    rc |= expect_int("taken between commands, refused at once: back on AM",
                     opts.analog_only == 1 && opts.analog_demod == DSD_ANALOG_DEMOD_AM
                         && dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_AM,
                     1);
    rc |= expect_toast("taken between commands, refused at once: says why", &state, k_refusal);
    g_analog_req_result = 0;

    /* Taken while the second command ran, after the drain found the AM switch still pending; refused where it landed. */
    freeState(&state);
    switch_nfm_to_am_and_back(&opts, &state, (RtlSdrContext*)fake_ctx, 0);
    rc |= expect_int("taken during the command: on Analog while pending", opts.analog_demod, DSD_ANALOG_DEMOD_FM);
    demod_thread_refuses_analog_keeping_kind(1, DSD_ANALOG_DEMOD_AM, 0);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_on_am_as_asked("taken during the command, refused landing: back on AM", &opts, 0);
    rc |= expect_toast("taken during the command, refused landing: says why", &state, k_refusal);

    g_fake_analog_family = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #524: an AM width changed on the running AM monitor, then a switch to NFM 16 kHz made before the front end took
 * the width: the switch's request replaced the width's (the queue is last-writer-wins), so the front end still runs AM
 * at the width it had. A retune to a 16 kHz DSP rate gets the switch refused where it lands, and the decoder goes back
 * to AM with the width the front end runs when the moved rate cannot filter the one changed (20 kHz), so what it
 * republishes is what the front end runs; a width the rate still filters (10 kHz) stays and is asked for again.
 */
static int
test_refused_switch_after_a_width_change_holds_the_width(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    static const char k_refusal[] = "Failed: Analog -> NFM 16 kHz does not fit the 16 kHz DSP rate (max 13.2 kHz); use "
                                    "a 24 or 48 kHz DSP bandwidth";
    static const int32_t k_widths[2] = {20000, 10000};
    int rc = 0;
    for (int i = 0; i < 2; i++) {
        const int fits = (k_widths[i] == 10000);
        init_nfm_session_with_am_width(&opts, &state, (RtlSdrContext*)fake_ctx, 0);
        opts.analog_nfm_bandwidth_hz = 16000;
        (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_AM);
        (void)dsd_app_drain_cmds(&opts, &state);
        demod_thread_lands(0);
        (void)dsd_app_drain_cmds(&opts, &state);
        rc |= submit_am_width(&opts, &state, k_widths[i], "width then switch: AM width");
        (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
        (void)dsd_app_drain_cmds(&opts, &state);
        rc |= expect_int("width then switch: NFM requested last", g_analog_req_kind, DSD_ANALOG_DEMOD_FM);
        demod_thread_refuses_analog_keeping_kind(1, DSD_ANALOG_DEMOD_AM, 0);
        /* What the front end answers for the changed AM width at the rate the retune moved it to. */
        g_analog_check_result = fits ? 0 : -1;
        state.ui_msg[0] = '\0';
        (void)dsd_app_drain_cmds(&opts, &state);
        rc |= expect_on_am_as_asked(fits ? "width then switch: a width the rate filters stays"
                                         : "width then switch: back to the width the front end runs",
                                    &opts, fits ? k_widths[i] : 0);
        rc |= expect_toast("width then switch: says why", &state, k_refusal);
        g_analog_check_result = 0;
        opts.analog_am_bandwidth_hz = 0;
        state.rtl_ctx = NULL;
        freeState(&state);
    }
    g_fake_analog_family = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    return rc;
}

/* After a refusal of an AM request under a blank scan row, the front end having kept AM at its default: the configured
   AM width, and the one in force, are the default, whatever the NFM width's baseline was; the NFM width is @p nfm_hz. */
static int
expect_am_width_back_to_the_default(dsd_opts* opts, dsd_state* state, int nfm_hz, const char* label) {
    state->ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(opts, state);
    int rc = expect_int(label, dsd_scan_mode_configured_analog_width(opts, state, DSD_ANALOG_DEMOD_AM), 0);
    rc |= expect_int(label, opts->analog_am_bandwidth_hz, 0);
    rc |= expect_int(label, dsd_scan_mode_configured_analog_width(opts, state, DSD_ANALOG_DEMOD_FM), nfm_hz);
    rc |= expect_int(label, opts->analog_only == 1 && opts->analog_demod == DSD_ANALOG_DEMOD_AM, 1);
    rc |= expect_int(label, strncmp(state->ui_msg, "Refused: ", 9) == 0, 1);
    return rc;
}

/*
 * Issue #524: under a blank scan row, a refusal where a request lands puts back the configured width of the kind the
 * front end kept from before the first change of that kind it ran none of; each kind keeps its own. An NFM width change
 * queued ahead of a switch to AM is no baseline for the AM width, and an AM width set while Analog was the mode (which
 * asks the front end for nothing) is one. Both times the front end kept AM at its default, so the configured AM width
 * goes back to the default, not to an NFM width.
 */
static int
test_refused_request_keeps_each_kind_s_width_baseline(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;

    /* NFM 12.5 -> 16 kHz, a switch to AM, then AM 10 and 15 kHz, none of which a drain found taken: the front end took
       the switch just before the 10 kHz width was queued, and a retune gets it to refuse 15 kHz, keeping AM's default. */
    init_nfm_session_with_am_width(&opts, &state, (RtlSdrContext*)fake_ctx, 0);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:118.1M:0:0:48");
    opts.rtl_dsp_bw_khz = 48;
    opts.analog_nfm_bandwidth_hz = 12500;
    rc |= expect_int("nfm then am: row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_INHERIT), 0);
    rc |= expect_int("nfm then am: row options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    rc |= submit_nfm_width(&opts, &state, 16000, "nfm then am: NFM 16 kHz");
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, "nfm then am: AM");
    g_fake_take_before_next_analog = 1;
    rc |= submit_am_width(&opts, &state, 10000, "nfm then am: AM 10 kHz");
    rc |= submit_am_width(&opts, &state, 15000, "nfm then am: AM 15 kHz");
    rc |= expect_int("nfm then am: AM 15 kHz asked for last",
                     g_analog_req_kind == DSD_ANALOG_DEMOD_AM && g_analog_req_width_hz == 15000, 1);
    demod_thread_refuses_analog_keeping_kind(1, DSD_ANALOG_DEMOD_AM, 0);
    rc |= expect_am_width_back_to_the_default(&opts, &state, 16000, "nfm then am: back to AM's default");
    dsd_scan_mode_leave(&opts, &state);
    freeState(&state);

    /* On AM with the configured NFM width at 25 kHz: Analog, NFM 20 kHz, AM 20 kHz (stored only: Analog is the mode),
       then AM again, before the demod thread takes anything; a retune gets it to refuse the last, and it keeps AM's
       default, having run none of them. */
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:118.1M:0:0:48");
    opts.rtl_dsp_bw_khz = 48;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    opts.analog_nfm_bandwidth_hz = 25000;
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, "am width while on analog: AM");
    demod_thread_lands(0);
    g_fake_analog_family = 1;
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("am width while on analog: row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_INHERIT), 0);
    rc |= expect_int("am width while on analog: row options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_ANALOG, "am width while on analog: Analog");
    rc |= submit_nfm_width(&opts, &state, 20000, "am width while on analog: NFM 20 kHz");
    rc |= submit_am_width(&opts, &state, 20000, "am width while on analog: AM 20 kHz");
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, "am width while on analog: AM again");
    rc |= expect_int("am width while on analog: AM 20 kHz asked for last",
                     g_analog_req_kind == DSD_ANALOG_DEMOD_AM && g_analog_req_width_hz == 20000, 1);
    demod_thread_refuses_analog_keeping_kind(1, DSD_ANALOG_DEMOD_AM, 0);
    rc |= expect_am_width_back_to_the_default(&opts, &state, 20000, "am width while on analog: back to AM's default");
    dsd_scan_mode_leave(&opts, &state);

    g_fake_analog_family = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    opts.analog_am_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* A running RTL session on the analog monitor of @p kind with the configured NFM width @p nfm_hz, every request taken,
   under a blank scan row. */
static int
init_blank_row_on_analog_kind(dsd_opts* opts, dsd_state* state, RtlSdrContext* fake_ctx, int kind, int nfm_hz,
                              const char* label) {
    init_decode_mode_context(opts, state);
    reset_rx_family_wrap();
    opts->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:118.1M:0:0:48");
    opts->rtl_dsp_bw_khz = 48;
    state->rtl_ctx = fake_ctx;
    opts->analog_nfm_bandwidth_hz = nfm_hz;
    int rc = submit_decode_mode(opts, state, kind == DSD_ANALOG_DEMOD_AM ? DSDCFG_MODE_AM : DSDCFG_MODE_ANALOG, label);
    demod_thread_lands(0);
    g_fake_analog_family = 1;
    (void)dsd_app_drain_cmds(opts, state);
    rc |= expect_int(label, dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_INHERIT), 0);
    rc |= expect_int(label, dsd_scan_mode_options(opts, state, NULL), 0);
    return rc;
}

/*
 * Issue #524: a config apply's change of an analog width it asks the front end for nothing about (the kind not in
 * force, or the one a switch in the same config leaves) counts, as a width command's does, for a refusal where a later
 * request of that kind lands: under a blank scan row the configured width of the kind the front end kept goes back to
 * the one from before the config, which is the one it ran.
 */
static int
test_refused_request_keeps_a_config_width_baseline(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;

    /* On AM with the configured NFM width at 25 kHz: Analog, a config that sets AM 20 kHz (stored only: Analog is the
       mode), then AM again, before the demod thread takes anything; a retune gets it to refuse the last, and it keeps
       AM's default, having run none of them. */
    rc |= init_blank_row_on_analog_kind(&opts, &state, (RtlSdrContext*)fake_ctx, DSD_ANALOG_DEMOD_AM, 25000,
                                        "config am width while on analog: AM row");
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_ANALOG, "config am width while on analog: Analog");
    rc |= submit_config_widths(&opts, &state, 25000, 20000, "config am width while on analog: AM 20 kHz");
    rc |= expect_int("config am width while on analog: stored",
                     dsd_scan_mode_configured_analog_width(&opts, &state, DSD_ANALOG_DEMOD_AM), 20000);
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, "config am width while on analog: AM again");
    rc |= expect_int("config am width while on analog: AM 20 kHz asked for last",
                     g_analog_req_kind == DSD_ANALOG_DEMOD_AM && g_analog_req_width_hz == 20000, 1);
    demod_thread_refuses_analog_keeping_kind(1, DSD_ANALOG_DEMOD_AM, 0);
    rc |= expect_am_width_back_to_the_default(&opts, &state, 25000,
                                              "config am width while on analog: back to AM's default");
    dsd_scan_mode_leave(&opts, &state);
    freeState(&state);

    /* On Analog at 12.5 kHz: a config that switches to AM and sets NFM 20 kHz, which the AM request it makes does not
       carry, then Analog again before the demod thread takes that request; a retune gets it to refuse NFM 20 kHz, and
       it keeps FM at 12.5 kHz, having run neither. */
    rc |= init_blank_row_on_analog_kind(&opts, &state, (RtlSdrContext*)fake_ctx, DSD_ANALOG_DEMOD_FM, 12500,
                                        "config switch leaving an nfm width: Analog row");
    rc |= submit_config_nfm_width(&opts, &state, DSDCFG_MODE_AM, 20000, "config switch leaving an nfm width: config");
    rc |= expect_int("config switch leaving an nfm width: AM asked for",
                     opts.analog_demod == DSD_ANALOG_DEMOD_AM && g_analog_req_kind == DSD_ANALOG_DEMOD_AM, 1);
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_ANALOG, "config switch leaving an nfm width: Analog again");
    rc |= expect_int("config switch leaving an nfm width: NFM 20 kHz asked for last",
                     g_analog_req_kind == DSD_ANALOG_DEMOD_FM && g_analog_req_width_hz == 20000, 1);
    demod_thread_refuses_analog_keeping_kind(1, DSD_ANALOG_DEMOD_FM, 12500);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("config switch leaving an nfm width: configured NFM back",
                     dsd_scan_mode_configured_analog_width(&opts, &state, DSD_ANALOG_DEMOD_FM), 12500);
    rc |= expect_int("config switch leaving an nfm width: NFM in force back", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("config switch leaving an nfm width: AM untouched",
                     dsd_scan_mode_configured_analog_width(&opts, &state, DSD_ANALOG_DEMOD_AM), 0);
    rc |= expect_int("config switch leaving an nfm width: still Analog",
                     opts.analog_only == 1 && opts.analog_demod == DSD_ANALOG_DEMOD_FM, 1);
    rc |= expect_int("config switch leaving an nfm width: toast", strncmp(state.ui_msg, "Refused: ", 9) == 0, 1);
    dsd_scan_mode_leave(&opts, &state);

    g_fake_analog_family = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    opts.analog_am_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #524: a switch between FM and AM on the running monitor drops the analog monitor block the decoder has
 * part-collected, as a family change does: it holds the old kind's audio (the FM discriminator reading an AM carrier,
 * or the reverse). DECODE_MODE_SET and a config's [mode] both hold to this; staying on the kind, or a new width of it,
 * keeps the block.
 */
static int
test_fm_am_switch_discards_partial_analog_block(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session_with_am_width(&opts, &state, (RtlSdrContext*)fake_ctx, 0);

    seed_partial_analog_block(&state);
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_AM);
    (void)dsd_app_drain_cmds(&opts, &state);
    demod_thread_lands(0);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("kind block: on AM", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_AM, 1);
    rc |= expect_partial_analog_block("kind block: FM to AM drops the block", &state, 0);

    seed_partial_analog_block(&state);
    rc |= submit_am_width(&opts, &state, 10000, "kind block: an AM width");
    demod_thread_lands(0);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("kind block: AM width applied", opts.analog_am_bandwidth_hz, 10000);
    rc |= expect_partial_analog_block("kind block: an AM width keeps the block", &state, 1);

    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_AM);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_partial_analog_block("kind block: AM again keeps the block", &state, 1);

    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_ANALOG);
    (void)dsd_app_drain_cmds(&opts, &state);
    demod_thread_lands(0);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("kind block: back on Analog", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_ANALOG, 1);
    rc |= expect_partial_analog_block("kind block: AM to FM drops the block", &state, 0);

    seed_partial_analog_block(&state);
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_AM, "kind block: config am");
    demod_thread_lands(0);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("kind block: config onto AM", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_AM, 1);
    rc |= expect_partial_analog_block("kind block: config FM to AM drops the block", &state, 0);

    seed_partial_analog_block(&state);
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_AM, "kind block: config am again");
    rc |= expect_partial_analog_block("kind block: config staying AM keeps the block", &state, 1);

    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_ANALOG, "kind block: config analog");
    demod_thread_lands(0);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("kind block: config onto Analog", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_ANALOG, 1);
    rc |= expect_partial_analog_block("kind block: config AM to FM drops the block", &state, 0);

    g_fake_analog_family = 0;
    opts.analog_am_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #524: an AM width the running monitor took when asked but refused where the request landed (a retune moved the
 * rate) is put back to the width the monitor kept, as an NFM width is. The stream records the setting it runs, so AM's
 * unset default goes back as the default, not as an explicit 6 kHz a save would then write, and an explicit 6 kHz,
 * which filters the same, goes back as the explicit width a save keeps.
 */
static int
test_am_width_refused_where_it_lands(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session_with_am_width(&opts, &state, (RtlSdrContext*)fake_ctx, 0);
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_AM);
    (void)dsd_app_drain_cmds(&opts, &state);
    demod_thread_lands(0);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("am width landing: on AM", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_AM, 1);

    reset_rx_family_wrap();
    rc |= submit_am_width(&opts, &state, 10000, "am width landing: 10 kHz");
    rc |= expect_int("am width landing: stored while pending", opts.analog_am_bandwidth_hz, 10000);
    rc |= expect_int("am width landing: requested for AM",
                     g_analog_req_kind == DSD_ANALOG_DEMOD_AM && g_analog_req_width_hz == 10000, 1);
    demod_thread_refuses_analog_keeping_kind(1, DSD_ANALOG_DEMOD_AM, 0);
    state.ui_msg[0] = '\0';
    rc |= expect_int("am width landing: settled on an empty drain", dsd_app_drain_cmds(&opts, &state), 0);
    rc |= expect_int("am width landing: the default put back", opts.analog_am_bandwidth_hz, 0);
    rc |= expect_int("am width landing: still AM", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_AM, 1);
    rc |= expect_toast("am width landing: toast", &state, "Refused: the RTL front end refused AM 10 kHz (see log)");

    /* An explicit 6 kHz running, then 20 kHz refused where it lands: the explicit 6 kHz goes back. */
    opts.analog_am_bandwidth_hz = 6000;
    reset_rx_family_wrap();
    rc |= submit_am_width(&opts, &state, 20000, "am width landing: 20 kHz over an explicit 6 kHz");
    rc |= expect_int("am width landing: 20 kHz stored, pending", opts.analog_am_bandwidth_hz, 20000);
    demod_thread_refuses_analog_keeping_kind(1, DSD_ANALOG_DEMOD_AM, 6000);
    rc |= expect_int("am width landing: explicit settled", dsd_app_drain_cmds(&opts, &state), 0);
    rc |= expect_int("am width landing: explicit 6 kHz put back", opts.analog_am_bandwidth_hz, 6000);

    g_fake_analog_family = 0;
    opts.analog_am_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #524: DSD_NEO_CHANNEL_LPF=0 turns off the channel filter AM always runs, its default width included, so a
 * switch to AM on a radio input is refused with the reason whatever the rate, where Analog with its unset NFM default
 * goes ahead. Issue #578: so is Input > Switch source > Airspy under AM, before the running input is torn down.
 */
static int
test_am_held_to_channel_lpf_off(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session_with_am_width(&opts, &state, (RtlSdrContext*)fake_ctx, 0);
    (void)dsd_setenv("DSD_NEO_CHANNEL_LPF", "0", 1);
    dsd_neo_config_init();
    state.ui_msg[0] = '\0';
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_AM);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("lpf off: AM refused", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_ANALOG, 1);
    rc |= expect_toast("lpf off: AM toast", &state,
                       "Failed: AM -> AM needs the channel filter, which DSD_NEO_CHANNEL_LPF=0 turns off");

    /* An AM session on the running RTL-SDR, started before the variable was set. */
    (void)dsd_unsetenv("DSD_NEO_CHANNEL_LPF");
    dsd_neo_config_init();
    reset_rx_family_wrap();
    (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_AM);
    (void)dsd_app_drain_cmds(&opts, &state);
    demod_thread_lands(0);
    rc |= expect_int("lpf off: AM session", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_AM, 1);
    (void)dsd_setenv("DSD_NEO_CHANNEL_LPF", "0", 1);
    dsd_neo_config_init();
    g_config_rtl_creates = 0;
    (void)post_empty(DSD_APP_CMD_AIRSPY_ENABLE_INPUT);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_str("lpf off: AM to airspy: the running input stays", opts.audio_in_dev, "rtl:0:118.1M:0:0:16");
    rc |= expect_int("lpf off: AM to airspy: stream kept", state.rtl_ctx == (RtlSdrContext*)fake_ctx, 1);
    rc |= expect_int("lpf off: AM to airspy: nothing opened", g_config_rtl_creates, 0);
    rc |= expect_toast("lpf off: AM to airspy toast", &state,
                       "Refused: AM needs the channel filter, which DSD_NEO_CHANNEL_LPF=0 turns off");
    rc |= expect_int("lpf off: AM to airspy: still AM", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_AM, 1);
    (void)dsd_unsetenv("DSD_NEO_CHANNEL_LPF");
    dsd_neo_config_init();
    g_fake_analog_family = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #526: on a digital session an nfm scan row without a width of its own runs the configured NFM width whenever it
 * comes on air, so while the scan has one the configured width is in use, whichever row is on air: the width command
 * and a config holding [analog] hold an edit to the front end's rate, and a DSP bandwidth that cannot filter the width
 * is refused, as they are under -fA. Otherwise the same edit made while the DMR row is on air was accepted and the nfm
 * row skipped at every visit. The rows that set their own width, and a scanner that is not running, hold nothing.
 */
static int
test_scan_list_holds_the_configured_nfm_width(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 12500);
    load_dmr_and_nfm_rows(&state, 0);
    opts.scanner_mode = 1;

    g_analog_check_result = -1; /* 16 kHz does not fit the 16 kHz DSP rate (max 13.2 kHz) */
    rc |= submit_nfm_width(&opts, &state, 16000, "scan list: unfit width");
    rc |= expect_int("scan list: unfit width refused", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("scan list: held to the front end", g_analog_check_calls == 1 && g_analog_check_width_hz == 16000,
                     1);
    rc |= expect_toast("scan list: refusal names the rate", &state,
                       "Refused: NFM 16 kHz does not fit the 16 kHz DSP rate (max 13.2 kHz)");

    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_config_nfm_width(&opts, &state, DSDCFG_MODE_UNSET, 16000, "scan list: config width");
    rc |= expect_int("scan list: config width not applied", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_toast("scan list: config toast", &state,
                       "Config not applied: NFM 16 kHz does not fit the 16 kHz DSP rate (max 13.2 kHz)");
    reset_rx_family_wrap();
    rc |= submit_config_nfm_width(&opts, &state, DSDCFG_MODE_UNSET, 12500, "scan list: config same width");
    rc |= expect_int("scan list: the same width asks nothing", g_analog_check_calls, 0);
    rc |= submit_config_nfm_width(&opts, &state, DSDCFG_MODE_UNSET, 11250, "scan list: config fitting width");
    rc |= expect_int("scan list: fitting config width applied", opts.analog_nfm_bandwidth_hz, 11250);
    rc |= expect_int("scan list: fitting config width held", g_analog_check_width_hz, 11250);
    rc |= expect_int("scan list: digital session kept", opts.analog_only, 0);

    /* The DSP bandwidth is held to the configured width too (on the options alone: no restart). */
    opts.audio_in_type = AUDIO_IN_NULL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M");
    (void)dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_BW, 12);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("scan list: unfit DSP bandwidth refused", opts.rtl_dsp_bw_khz, 16);
    rc |= expect_toast("scan list: DSP bandwidth toast", &state,
                       "Refused: DSP BW 12 kHz cannot filter NFM 11.25 kHz (max 9.6 kHz); narrow the NFM width first");
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:16");

    /* The nfm row with a width of its own does not run the configured one. */
    load_dmr_and_nfm_rows(&state, 12500);
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_nfm_width(&opts, &state, 16000, "own width row: width");
    rc |= expect_int("own width row: not held", g_analog_check_calls, 0);
    rc |= expect_int("own width row: stored", opts.analog_nfm_bandwidth_hz, 16000);

    /* Nor does a map the scanner is not running. */
    load_dmr_and_nfm_rows(&state, 0);
    opts.scanner_mode = 0;
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_nfm_width(&opts, &state, 20000, "scanner off: width");
    rc |= expect_int("scanner off: not held", g_analog_check_calls, 0);
    rc |= expect_int("scanner off: stored", opts.analog_nfm_bandwidth_hz, 20000);

    g_analog_check_result = 0;
    dsd_channel_modes_clear(&state);
    (void)dsd_channel_profile_set(&state, 1, NULL);
    state.lcn_freq_count = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* A -Y map of a DMR row and an am row (issue #526), the am row with its own width when @p row_width_hz > 0. */
static void
load_dmr_and_am_rows(dsd_state* state, int row_width_hz) {
    load_dmr_and_nfm_rows(state, 0);
    state->trunk_lcn_freq[1] = 118300000L;
    (void)dsd_channel_mode_set(state, 1, DSD_SCAN_MODE_AM);
    dsd_scan_row_profile* profile = NULL;
    if (row_width_hz > 0 && dsd_scan_profile_ensure(&profile) == 0) {
        profile->values.present = DSD_SCAN_OPT_BANDWIDTH;
        profile->values.channel_bw_hz = row_width_hz;
        profile->values.channel_bw_kind = DSD_ANALOG_DEMOD_AM;
    }
    if (dsd_channel_profile_set(state, 1, profile) != 0) {
        dsd_scan_profile_free(profile);
    }
}

/*
 * Issue #526: the AM width follows the configured-view rule on scan rows as the NFM width does. A digital session
 * scanning a map with an am row that sets no width of its own runs the configured AM width whenever that row comes on
 * air, so an AM width edit and a DSP bandwidth are held to it. An am row's own --am-bandwidth-hz shadows an AM width
 * edit while it is on air: the row keeps its width, the baseline takes the edit, the front end is asked for nothing and
 * the toast names the row. An NFM width edit under it is no edit of the width it runs. The leave keeps both edits.
 */
static int
test_am_width_under_am_scan_rows(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 0);
    opts.analog_am_bandwidth_hz = 8000;
    load_dmr_and_am_rows(&state, 0);
    opts.scanner_mode = 1;

    g_analog_check_result = -1;
    rc |= submit_am_width(&opts, &state, 16000, "am scan list: unfit width");
    rc |= expect_int("am scan list: unfit width refused", opts.analog_am_bandwidth_hz, 8000);
    rc |= expect_int(
        "am scan list: held to the front end",
        g_analog_check_calls == 1 && g_analog_check_kind == DSD_ANALOG_DEMOD_AM && g_analog_check_width_hz == 16000, 1);
    reset_rx_family_wrap();
    g_analog_check_result = 0;
    rc |= submit_am_width(&opts, &state, 10000, "am scan list: fitting width");
    rc |= expect_int("am scan list: fitting width stored", opts.analog_am_bandwidth_hz, 10000);
    rc |= expect_int("am scan list: fitting width held", g_analog_check_width_hz, 10000);
    rc |= expect_int("am scan list: digital session kept", opts.analog_only == 0 && opts.frame_dmr == 1, 1);

    /* The DSP bandwidth is held to the configured AM width too (on the options alone: no restart). */
    opts.audio_in_type = AUDIO_IN_NULL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M");
    (void)dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_BW, 12);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("am scan list: unfit DSP bandwidth refused", opts.rtl_dsp_bw_khz, 16);
    rc |= expect_toast("am scan list: DSP bandwidth toast", &state,
                       "Refused: DSP BW 12 kHz cannot filter AM 10 kHz (max 9.6 kHz); narrow the AM width first");
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:16");

    /* The am row without a width of its own on air runs the configured AM width: an edit reaches it live. */
    rc |= expect_int("am row: on air", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_AM), 0);
    rc |= expect_int("am row: no width", dsd_scan_mode_options(&opts, &state, NULL), 0);
    rc |= expect_int("am row: configured width in force",
                     opts.analog_demod == DSD_ANALOG_DEMOD_AM && opts.analog_am_bandwidth_hz == 10000, 1);
    reset_rx_family_wrap();
    g_fake_analog_family = 1;
    rc |= submit_am_width(&opts, &state, 9000, "am row: edit in force");
    rc |= expect_int("am row: edit in force stored", opts.analog_am_bandwidth_hz, 9000);
    rc |= expect_int(
        "am row: edit requested live",
        g_analog_req_calls == 1 && g_analog_req_kind == DSD_ANALOG_DEMOD_AM && g_analog_req_width_hz == 9000, 1);
    rc |= expect_toast("am row: edit in force toast", &state, "Applied: AM bandwidth -> 9 kHz");
    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_int("am row: leave keeps the edit", opts.analog_only == 0 && opts.analog_am_bandwidth_hz == 9000, 1);
    reset_rx_family_wrap();
    g_fake_analog_family = 0;
    rc |= submit_am_width(&opts, &state, 10000, "am row: back to 10 kHz");

    /* The am row's own 8.333 kHz on air: an AM width edit is the configured default's, and the row overrides it. */
    load_dmr_and_am_rows(&state, 8333);
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 8333;
    row.channel_bw_kind = DSD_ANALOG_DEMOD_AM;
    rc |= expect_int("am width row: am row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_AM), 0);
    rc |= expect_int("am width row: its width", dsd_scan_mode_options(&opts, &state, &row), 0);
    rc |= expect_int("am width row: in force",
                     opts.analog_demod == DSD_ANALOG_DEMOD_AM && opts.analog_am_bandwidth_hz == 8333, 1);
    reset_rx_family_wrap();
    g_fake_analog_family = 1;
    rc |= submit_am_width(&opts, &state, 12000, "am width row: shadowed edit");
    rc |= expect_int("am width row: row keeps its width", opts.analog_am_bandwidth_hz, 8333);
    rc |= expect_int("am width row: baseline takes the edit",
                     dsd_scan_mode_configured_view(&state)->analog_am_bandwidth_hz, 12000);
    rc |= expect_int("am width row: front end not asked to change", g_analog_req_calls, 0);
    rc |= expect_int("am width row: row not suspended",
                     dsd_scan_mode_active(&state) == DSD_SCAN_MODE_AM && !dsd_scan_mode_updating(&state), 1);
    rc |= expect_toast("am width row: toast names the row", &state,
                       "Default AM bandwidth -> 12 kHz; this channel overrides it (8.333 kHz)");
    /* An NFM edit under the am row is not shadowed: it is stored, and nothing runs it until an nfm row does. */
    reset_rx_family_wrap();
    g_fake_analog_family = 1;
    rc |= submit_nfm_width(&opts, &state, 12500, "am width row: NFM edit");
    rc |= expect_int("am width row: NFM edit stored", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("am width row: NFM edit requests nothing", g_analog_req_calls, 0);
    rc |= expect_int("am width row: NFM edit keeps the row", opts.analog_am_bandwidth_hz, 8333);
    dsd_scan_mode_leave(&opts, &state);
    rc |= expect_int("am width row: leave keeps the edits",
                     opts.analog_only == 0 && opts.frame_dmr == 1 && opts.analog_am_bandwidth_hz == 12000
                         && opts.analog_nfm_bandwidth_hz == 12500,
                     1);

    g_analog_check_result = 0;
    g_fake_analog_family = 0;
    opts.scanner_mode = 0;
    dsd_channel_modes_clear(&state);
    (void)dsd_channel_profile_set(&state, 1, NULL);
    state.lcn_freq_count = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    opts.analog_am_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #524 with #526: an AM session scanning a list with an nfm row that sets no width of its own runs the configured
 * NFM width whenever that row comes on air, as a digital session does. A config holding [analog] and a DSP bandwidth
 * are held to that width as well as to the AM width the preset runs, whichever row is on air: a DSP bandwidth that
 * filters the AM default but not the NFM width is refused, rather than the nfm row skipped at every visit.
 */
static int
test_scan_list_holds_the_configured_nfm_width_on_am(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 11250);
    opts.analog_am_bandwidth_hz = 0;
    rc |= submit_decode_mode(&opts, &state, DSDCFG_MODE_AM, "am scan list: AM session");
    rc |= expect_int("am scan list: on AM", opts.analog_only == 1 && opts.analog_demod == DSD_ANALOG_DEMOD_AM, 1);
    load_dmr_and_nfm_rows(&state, 0);
    opts.scanner_mode = 1;

    reset_rx_family_wrap();
    g_analog_check_result = -1; /* 16 kHz does not fit the 16 kHz DSP rate (max 13.2 kHz) */
    rc |= submit_config_nfm_width(&opts, &state, DSDCFG_MODE_UNSET, 16000, "am scan list: config width");
    rc |= expect_int("am scan list: config width not applied", opts.analog_nfm_bandwidth_hz, 11250);
    rc |= expect_int(
        "am scan list: config width held",
        g_analog_check_calls >= 1 && g_analog_check_kind == DSD_ANALOG_DEMOD_FM && g_analog_check_width_hz == 16000, 1);
    rc |= expect_toast("am scan list: config toast", &state,
                       "Config not applied: NFM 16 kHz does not fit the 16 kHz DSP rate (max 13.2 kHz)");
    reset_rx_family_wrap();
    rc |= submit_config_nfm_width(&opts, &state, DSDCFG_MODE_UNSET, 12500, "am scan list: config fitting width");
    rc |= expect_int("am scan list: fitting config width applied", opts.analog_nfm_bandwidth_hz, 12500);
    rc |= expect_int("am scan list: AM session kept", opts.analog_only == 1 && opts.analog_demod == DSD_ANALOG_DEMOD_AM,
                     1);

    /* The DSP bandwidth: 12 kHz filters the AM default (max 9.6 kHz) but not the NFM 12.5 kHz the nfm row runs. With
       no stream running, one it takes restarts through the wrapped stream create. */
    state.rtl_ctx = NULL;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_BW, 12);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("am scan list: unfit DSP bandwidth refused", opts.rtl_dsp_bw_khz, 16);
    rc |= expect_toast("am scan list: DSP bandwidth toast", &state,
                       "Refused: DSP BW 12 kHz cannot filter NFM 12.5 kHz (max 9.6 kHz); narrow the NFM width first");
    /* Without the scanner the nfm row runs nothing, and 12 kHz filters the AM default. */
    opts.scanner_mode = 0;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_BW, 12);
    state.ui_msg[0] = '\0';
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("am scan list: scanner off takes 12 kHz", opts.rtl_dsp_bw_khz, 12);

    g_analog_check_result = 0;
    opts.rtl_dsp_bw_khz = 16;
    dsd_channel_modes_clear(&state);
    (void)dsd_channel_profile_set(&state, 1, NULL);
    state.lcn_freq_count = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #526: a config apply is scoped, so the options it sees are the configured ones, but the stream its [input]
 * reopens runs the nfm scan row on air again once the scope resumes. The row's own width is therefore held to the rate
 * the reopened RTL-SDR runs at, as RTL_SET_BW holds it: a DSP bandwidth that cannot filter it leaves the whole config
 * unapplied and the running stream in place, rather than the row refused where it lands. One that can reopens.
 */
static int
test_config_apply_holds_a_scan_row_width_to_a_reopen(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 0);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:154.43M:0:0:24");
    opts.rtl_dsp_bw_khz = 24;
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 20000;
    rc |= expect_int("row reopen: nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM), 0);
    rc |= expect_int("row reopen: its width", dsd_scan_mode_options(&opts, &state, &row), 0);
    rc |= expect_int("row reopen: in force", opts.analog_nfm_bandwidth_hz, 20000);

    char dev_before[sizeof opts.audio_in_dev];
    DSD_SNPRINTF(dev_before, sizeof dev_before, "%s", opts.audio_in_dev);
    reset_rx_family_wrap();
    rc |= submit_config_rtl_bw(&opts, &state, 16, -1, "row reopen: 24->16");
    rc |= expect_int("row reopen 24->16: rate kept", opts.rtl_dsp_bw_khz, 24);
    rc |= expect_str("row reopen 24->16: input unchanged", opts.audio_in_dev, dev_before);
    rc |= expect_int("row reopen 24->16: stream kept", state.rtl_ctx == (RtlSdrContext*)fake_ctx, 1);
    rc |= expect_toast("row reopen 24->16 toast", &state,
                       "Config not applied: NFM 20 kHz does not fit the 16 kHz DSP rate (max 13.2 kHz); use a 24 or 48 "
                       "kHz DSP bandwidth");
    rc |= expect_int("row reopen 24->16: the row keeps its width",
                     dsd_scan_mode_active(&state) == DSD_SCAN_MODE_NFM && opts.analog_nfm_bandwidth_hz == 20000, 1);
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(&state);
    rc |= expect_int("row reopen 24->16: the configured session kept",
                     configured && configured->analog_only == 0 && configured->analog_nfm_bandwidth_hz == 0, 1);

    /* No stream (the reopen's failure is the wrapped stream create's, not the width's): 24 -> 48 kHz applies. */
    state.rtl_ctx = NULL;
    rc |= submit_config_rtl_bw(&opts, &state, 48, -1, "row reopen: 24->48");
    rc |= expect_int("row reopen 24->48: rate applied", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_int("row reopen 24->48: not refused", strstr(state.ui_msg, "Config not applied") == NULL, 1);

    dsd_scan_mode_leave(&opts, &state);
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #578: a config apply runs inside a scan row's suspended scope, so a stream its [input] reopens opens on the
 * configured settings, not the row's, and one started again on the input a failed reopen replaced opens on the row the
 * stream it replaced ran, which the scope's options do not hold either. The row compares unchanged once the scope
 * resumes, so the resume asks the front end for the row's profile anyway: an nfm row on a DMR-configured scanner gets
 * its analog monitor back, whether the reopen failed and put the running input back or started (the successful reopen
 * is issue #583's item 1).
 */
static int
test_config_reopen_under_a_scan_row_republishes_the_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 0);
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 12500;
    rc |= expect_int("row republish: nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM), 0);
    rc |= expect_int("row republish: its width", dsd_scan_mode_options(&opts, &state, &row), 0);
    g_fake_analog_family = 1; /* the front end runs the row's monitor */

    static const char* const labels[] = {"row republish: reopen fails", "row republish: reopen starts"};
    for (int starts = 0; starts <= 1; ++starts) {
        const char* label = labels[starts];
        reset_rx_family_wrap();
        g_config_rtl_open_ok = 1;
        g_config_rtl_fail_starts = starts ? 0 : 1;
        rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_SOAPY, 0, 0, label);
        rc |= expect_int(label, dsd_app_command_test_last_failed(), starts ? 0 : 1);
        /* The reopened SoapySDR device runs the configured DMR; the RTL-SDR put back runs the row's 12.5 kHz monitor,
           as its stream did. */
        rc |= expect_int(label, g_config_rtl_creates == (starts ? 1 : 2) && state.rtl_ctx != NULL, 1);
        rc |= expect_int(label, g_config_rtl_create_analog_only, starts ? 0 : 1);
        rc |= expect_int(label, g_config_rtl_create_width_hz, starts ? 0 : 12500);
        rc |= expect_str(label, opts.audio_in_dev, starts ? "soapy:driver=airspy" : "rtl:0:851.375M:0:0:16");
        /* The row is back in force, and the front end is asked for its monitor. */
        rc |=
            expect_int(label, dsd_scan_mode_active(&state) == DSD_SCAN_MODE_NFM && !dsd_scan_mode_updating(&state), 1);
        rc |= expect_int(label, opts.analog_only == 1 && opts.analog_nfm_bandwidth_hz == 12500, 1);
        rc |= expect_int(label,
                         g_analog_req_calls == 1 && g_analog_req_family == DSD_RX_FAMILY_ANALOG
                             && g_analog_req_kind == DSD_ANALOG_DEMOD_FM && g_analog_req_width_hz == 12500,
                         1);
    }

    dsd_scan_mode_leave(&opts, &state);
    reset_config_rtl_wrap();
    g_fake_analog_family = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #578: the resume republishes the row on air to a stream the config apply started under its suspended scope. A
 * CQPSK row the decoder runs at the stream's symbol-rate output keeps one sample per symbol, which is no timing for the
 * new stream's Gardner loop (the request would clamp it to 2): the row's profile is timed for the rate the new stream
 * runs it at, the demod rate the stream published at its start, which the CQPSK loop runs at on the digital family. A
 * P25 CQPSK row on a DMR-configured scanner gets 10 samples per 4800 Bd symbol from the RTL-SDR a failed reopen put
 * back at 48 kHz, and 4 from a SoapySDR device whose 19,531 Hz demod rate the stream opened on DMR resamples to a
 * 48 kHz FSK output.
 */
static int
test_config_reopen_under_a_cqpsk_row_times_the_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 0);
    rc |= expect_int("cqpsk row: p25 row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_P25), 0);
    dsd_scan_mode_target_modulation(&state, DSD_SCAN_MODULATION_CQPSK);
    dsd_scan_mode_apply_modulation(&opts, DSD_SCAN_MODE_P25, DSD_SCAN_MODULATION_CQPSK);
    state.rf_mod = 1;
    g_fake_cqpsk = 1;
    g_fake_output_rate_hz = 48000U;

    static const char* const labels[] = {"cqpsk row: reopen fails", "cqpsk row: reopen starts"};
    for (int starts = 0; starts <= 1; ++starts) {
        const char* label = labels[starts];
        const int sps = starts ? 4 : 10;
        /* The decoder reads the CQPSK stream's symbol-rate output. */
        state.samplesPerSymbol = 1;
        state.symbolCenter = 0;
        reset_rx_family_wrap();
        g_config_rtl_open_ok = 1;
        g_config_rtl_fail_starts = starts ? 0 : 1;
        g_fake_request_rate_hz = starts ? 19531 : 48000;
        rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_SOAPY, 0, 0, label);
        rc |= expect_int(label, dsd_app_command_test_last_failed(), starts ? 0 : 1);
        rc |= expect_int(label, g_config_rtl_creates == (starts ? 1 : 2) && state.rtl_ctx != NULL, 1);
        rc |= expect_int(label, dsd_scan_mode_active(&state) == DSD_SCAN_MODE_P25 && state.rf_mod == 1, 1);
        rc |= expect_int(label, g_demod_req_calls >= 1 && g_demod_req_cqpsk == 1 && g_demod_req_rate == 4800, 1);
        rc |= expect_int(label, g_demod_req_ted_sps, sps);
        rc |= expect_int(label, state.samplesPerSymbol, sps);
    }

    dsd_scan_mode_leave(&opts, &state);
    reset_rx_family_wrap();
    g_fake_cqpsk = 0;
    g_fake_digital_rate = 0U;
    g_fake_output_rate_hz = 0U;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #578: the same republish on an Analog-configured session. A typed P25 row there runs its symbol profile under
 * the analog family the reopened stream opened on (the configured monitor), with no family switch asked of the stream;
 * the RTL-SDR a failed reopen put back opens on the row itself, as the stream it replaced ran it, on the digital
 * family. A CQPSK row's timing loop then runs at the demod rate the stream published at its start, not at the monitor's
 * 48 kHz resampled audio: 4 samples per 4800 Bd symbol at a SoapySDR device's 19,531 Hz, and 5 at the 24 kHz DSP
 * bandwidth of the RTL-SDR put back. A C4FM row reads the audio the stream delivers, and keeps 10.
 */
static int
test_config_reopen_under_a_cqpsk_row_on_an_analog_session_times_the_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    static const char* const labels[2][2] = {{"analog c4fm row: reopen fails", "analog c4fm row: reopen starts"},
                                             {"analog cqpsk row: reopen fails", "analog cqpsk row: reopen starts"}};
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    for (int cqpsk = 1; cqpsk >= 0; --cqpsk) {
        DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:24");
        opts.rtl_dsp_bw_khz = 24;
        rc |= expect_int(labels[cqpsk][0], dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_P25), 0);
        const dsd_scan_modulation modulation = cqpsk ? DSD_SCAN_MODULATION_CQPSK : DSD_SCAN_MODULATION_C4FM;
        dsd_scan_mode_target_modulation(&state, modulation);
        dsd_scan_mode_apply_modulation(&opts, DSD_SCAN_MODE_P25, modulation);
        state.rf_mod = cqpsk;
        g_fake_cqpsk = cqpsk;
        g_fake_output_rate_hz = 48000U; /* the monitor's resampled audio */
        for (int starts = 0; starts <= 1; ++starts) {
            const char* label = labels[cqpsk][starts];
            const int sps = !cqpsk ? 10 : (starts ? 4 : 5);
            /* The decoder reads a CQPSK row's symbol-rate output. */
            state.samplesPerSymbol = cqpsk ? 1 : 10;
            state.symbolCenter = 0;
            reset_rx_family_wrap();
            /* The reopened stream opened on the configured monitor, the one put back on the row's P25. */
            g_fake_analog_family = starts;
            g_config_rtl_open_ok = 1;
            g_config_rtl_fail_starts = starts ? 0 : 1;
            g_fake_request_rate_hz = starts ? 19531 : 24000;
            rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_SOAPY, 0, 0, label);
            rc |= expect_int(label, dsd_app_command_test_last_failed(), starts ? 0 : 1);
            rc |= expect_int(label, g_config_rtl_creates == (starts ? 1 : 2) && state.rtl_ctx != NULL, 1);
            rc |= expect_int(label, g_config_rtl_create_analog_only, starts);
            rc |= expect_int(label, dsd_scan_mode_active(&state) == DSD_SCAN_MODE_P25 && state.rf_mod == cqpsk, 1);
            rc |= expect_int(label, g_analog_req_calls, 0);
            rc |=
                expect_int(label, g_demod_req_calls >= 1 && g_demod_req_cqpsk == cqpsk && g_demod_req_rate == 4800, 1);
            rc |= expect_int(label, g_demod_req_ted_sps, sps);
            rc |= expect_int(label, state.samplesPerSymbol, sps);
        }
        dsd_scan_mode_leave(&opts, &state);
    }

    reset_rx_family_wrap();
    g_fake_cqpsk = 0;
    g_fake_analog_family = 0;
    g_fake_output_rate_hz = 0U;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* The output rate the stream delivers now: once it runs CQPSK, one sample per 4800 Bd symbol; on the analog monitor,
   its resampled audio. */
static unsigned int g_hook_output_rate_hz;

static unsigned int
hook_output_rate(void) {
    return g_hook_output_rate_hz;
}

/*
 * Issue #578: a config reopen that also changes what the row on air runs has the row republished to the new stream as
 * a changed row, timed for that stream as an unchanged one is. A P25 session scanning a P25 row that takes the
 * configured modulation loads a config that reopens the input as a SoapySDR device and sets [demod] QPSK: the row now
 * runs CQPSK, which the stream the reopen opened on the configured QPSK delivers at one sample per 4800 Bd symbol,
 * while its timing loop runs at the device's 19,531 Hz demod rate. The row is republished for 4 samples per symbol,
 * not for the 1 of the symbol-rate output (which the request would clamp to 2). The same row on an Analog-configured
 * session runs CQPSK under the analog family the stream opened on, at the demod rate the stream published, not at the
 * monitor's 48 kHz audio: 4 as well.
 */
static int
test_config_reopen_that_changes_the_row_modulation_times_the_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    static const char* const labels[] = {"row modulation", "analog row modulation"};
    int rc = 0;
    const dsd_rtl_stream_metrics_hooks hooks = {.output_rate_hz = hook_output_rate,
                                                .analog_family_active = rtl_stream_analog_family_active,
                                                .output_rate_for_family = rtl_stream_output_rate_for_family};
    for (int analog_session = 0; analog_session <= 1; ++analog_session) {
        const char* label = labels[analog_session];
        if (analog_session) {
            init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
        } else {
            init_decode_mode_context(&opts, &state);
            opts.audio_in_type = AUDIO_IN_RTL;
            DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:48");
            opts.rtl_dsp_bw_khz = 48;
            state.rtl_ctx = (RtlSdrContext*)fake_ctx;
            (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_P25P1);
            (void)dsd_app_drain_cmds(&opts, &state);
        }
        rc |= expect_int(label, dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_P25), 0);
        rc |= expect_int(label, state.rf_mod, 0);
        dsd_rtl_stream_metrics_hooks_set(&hooks);
        reset_rx_family_wrap();
        /* The stream the reopen opened on the configured settings: CQPSK on the P25 session, the monitor on the
           Analog one. */
        g_fake_analog_family = analog_session;
        g_hook_output_rate_hz = analog_session ? 48000U : 4800U;
        g_fake_output_rate_hz = g_hook_output_rate_hz;
        g_fake_digital_rate = analog_session ? 0U : 19531U;
        g_fake_request_rate_hz = 19531;
        g_config_rtl_open_ok = 1;
        dsdneoUserConfig cfg;
        DSD_MEMSET(&cfg, 0, sizeof cfg);
        cfg.has_input = 1;
        cfg.input_source = DSDCFG_INPUT_SOAPY;
        DSD_SNPRINTF(cfg.soapy_args, sizeof cfg.soapy_args, "%s", "driver=airspy");
        cfg.has_demod = 1;
        cfg.demod_path = DSDCFG_DEMOD_QPSK;
        rc |= submit_config(&opts, &state, &cfg, label);
        rc |= expect_int(label, dsd_app_command_test_last_failed(), 0);
        rc |= expect_int(label, g_config_rtl_creates == 1 && state.rtl_ctx != NULL, 1);
        rc |= expect_int(label, g_config_rtl_create_analog_only, analog_session);
        rc |= expect_int(label, dsd_scan_mode_active(&state) == DSD_SCAN_MODE_P25 && state.rf_mod == 1, 1);
        rc |= expect_int(label, g_demod_req_calls >= 1 && g_demod_req_cqpsk == 1 && g_demod_req_rate == 4800, 1);
        rc |= expect_int(label, g_demod_req_ted_sps, 4);
        rc |= expect_int(label, state.samplesPerSymbol, 4);

        dsd_rtl_stream_metrics_hooks_set(NULL);
        dsd_scan_mode_leave(&opts, &state);
        reset_rx_family_wrap();
        g_fake_analog_family = 0;
        g_fake_digital_rate = 0U;
        g_fake_output_rate_hz = g_hook_output_rate_hz = 0U;
        state.rtl_ctx = NULL;
        freeState(&state);
    }
    return rc;
}

/*
 * Issue #578: the row republished to a restarted stream is timed for the modulation the stream will run it with. The
 * stream a digital session's reopen started runs the digital family already, and there a symbol profile request applies
 * the row's own CQPSK state, whatever DSD_NEO_CQPSK says (the override decides the CQPSK family only of an open and of
 * a switch out of the analog family), and keeps the output chain the stream opened on. Each session here reopens as a
 * SoapySDR device at a 19,531 Hz demod rate, whose FSK output an open of an FSK mode resamples to 48 kHz. Under
 * DSD_NEO_CQPSK=0 a DMR-configured scanner's stream opens on the FSK discriminator, and a P25 CQPSK row runs CQPSK
 * there, its timing loop at the demod rate: 4 samples per 4800 Bd symbol, not the 10 of the FSK output the override
 * would land a switch on. A P25 session configured for QPSK opens on CQPSK, with no resampler, and a P25 row with its
 * own C4FM runs the FSK discriminator there at the 19,531 Hz output the stream delivers: 4, not the 10 of the 48 kHz a
 * switch onto C4FM would resample to. Under DSD_NEO_CQPSK=1 the DMR scanner's stream opens on CQPSK as well, and a C4FM
 * row reads the same 19,531 Hz: 4.
 */
static int
test_config_reopen_times_the_row_for_the_modulation_it_runs(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];

    static const struct {
        const char* label;
        int cqpsk_env;       /* DSD_NEO_CQPSK: -1 unset */
        int configured_qpsk; /* a P25 session configured for QPSK, else a DMR one */
        int row_cqpsk;
    } legs[] = {
        {"DSD_NEO_CQPSK=0, dmr session, cqpsk row", 0, 0, 1},
        {"p25 qpsk session, c4fm row", -1, 1, 0},
        {"DSD_NEO_CQPSK=1, dmr session, c4fm row", 1, 0, 0},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof legs / sizeof legs[0]; ++i) {
        const char* label = legs[i].label;
        const int cqpsk = legs[i].row_cqpsk;
        init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 0);
        if (legs[i].configured_qpsk) {
            (void)dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, (int32_t)DSDCFG_MODE_P25P1);
            (void)dsd_app_drain_cmds(&opts, &state);
            dsd_scan_mode_apply_modulation(&opts, DSD_SCAN_MODE_P25, DSD_SCAN_MODULATION_CQPSK);
            state.rf_mod = 1;
        }
        rc |= expect_int(label, dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_P25), 0);
        const dsd_scan_modulation modulation = cqpsk ? DSD_SCAN_MODULATION_CQPSK : DSD_SCAN_MODULATION_C4FM;
        dsd_scan_mode_target_modulation(&state, modulation);
        dsd_scan_mode_apply_modulation(&opts, DSD_SCAN_MODE_P25, modulation);
        state.rf_mod = cqpsk;
        reset_rx_family_wrap();
        /* The stream the reopen opened on the configured settings: CQPSK when the override or the QPSK configuration
           picks it (its output at the demod rate), else the FSK discriminator resampled to 48 kHz. A switch out of the
           analog family would land on the family the override picks (rtl_stream_output_rate_for_family()). */
        const int opened_cqpsk = legs[i].cqpsk_env >= 0 ? legs[i].cqpsk_env : legs[i].configured_qpsk;
        g_fake_cqpsk_env = legs[i].cqpsk_env;
        g_fake_cqpsk = opened_cqpsk;
        g_fake_analog_family = 0;
        g_fake_output_rate_hz = opened_cqpsk ? 19531U : 48000U;
        g_fake_request_rate_hz = 19531;
        g_fake_digital_rate = 19531U;
        g_fake_digital_fsk_rate = 48000U;
        g_config_rtl_open_ok = 1;
        state.samplesPerSymbol = cqpsk ? 1 : 10;
        state.symbolCenter = 0;
        rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_SOAPY, 0, 0, label);
        rc |= expect_int(label, dsd_app_command_test_last_failed(), 0);
        rc |= expect_int(label, g_config_rtl_creates == 1 && state.rtl_ctx != NULL, 1);
        rc |= expect_int(label, dsd_scan_mode_active(&state) == DSD_SCAN_MODE_P25 && state.rf_mod == cqpsk, 1);
        /* The digital family asked for with it is the one the stream runs: no switch lands. */
        rc |= expect_int(label, g_analog_req_calls == 0 || g_analog_req_family == DSD_RX_FAMILY_DIGITAL, 1);
        rc |= expect_int(label, g_demod_req_calls >= 1 && g_demod_req_cqpsk == cqpsk && g_demod_req_rate == 4800, 1);
        rc |= expect_int(label, g_demod_req_ted_sps, 4);
        rc |= expect_int(label, state.samplesPerSymbol, 4);

        dsd_scan_mode_leave(&opts, &state);
        reset_rx_family_wrap();
        g_fake_cqpsk = 0;
        g_fake_digital_rate = 0U;
        g_fake_output_rate_hz = 0U;
        state.rtl_ctx = NULL;
        freeState(&state);
    }
    return rc;
}

/*
 * Issue #583: a scoped command under a trunk-scan P25 target whose retune, carrying the digital family, is still
 * outstanding on a front end left digital (the nfm retune the target was timed behind failed, so the stream still runs
 * CQPSK at 78125 Hz). The resume times the target for the digital family's landing, where that retune lands it, and the
 * republish that follows queues a live family request, which supersedes the retune: that request is the marked landing
 * (rtl_stream_request_digital_family_landing()), so the front end lands where the decoder was timed whichever of the two
 * lands first, and says whether the CQPSK state is the target's own choice by the scope's rule
 * (dsd_scan_mode_symbol_timing_rate_hz()). Under -mq and DSD_NEO_CQPSK=0 a target with no modulation is timed and
 * published for the FSK discriminator at 48 kHz (10 samples per 4800 Bd symbol), leaving the CQPSK state to the
 * override; one with modulation=cqpsk keeps CQPSK at 78125 Hz (16) as its own choice. A target with modulation=auto
 * that learned CQPSK is timed, and published, for the CQPSK it runs, not for the preset's C4FM the resume applies on the
 * way: under DSD_NEO_CQPSK=1 its own choice would otherwise put it on the FSK discriminator's 48 kHz. A config apply
 * whose reopen starts leaves the row unchanged, and the resume publishes it to the new stream (ui_started_stream_rate()
 * times what lands nothing): the publish decides the landing, and times it, as for a changed row. With nothing
 * outstanding the row is timed at the live rate and the republish asks for the digital family as it always has.
 *
 * A DMR target with no modulation under -mq keeps the lock's CQPSK in its decoder, but its CQPSK choice is its own,
 * CQPSK off: its GFSK retune lands the FSK discriminator whatever DSD_NEO_CQPSK says. The resume and the republish time
 * it, and publish its profile, for that FSK (10 samples per symbol at 48 kHz, the 12.5 kHz filter), with a landing
 * whose CQPSK state is the target's own, and with nothing outstanding the republish still asks for CQPSK off rather
 * than put the front end its retune landed on FSK onto CQPSK.
 *
 * A stream a command starts with nothing outstanding lands no family, so the command times the row for the rate that
 * stream runs it at (ui_started_stream_rate()), with the same CQPSK state the republish asks for: a config apply whose
 * reopen starts under the suspended scope, and an explicit restart, which leaves the scope in force. Under -mq and
 * DSD_NEO_CQPSK=0 the new stream opens on the FSK discriminator, its 78125 Hz demod rate resampled to 48 kHz. The DMR
 * target is asked for CQPSK off there, so it reads that 48 kHz output: 10 samples per symbol, not the 16 of the demod
 * rate its decoder's inherited CQPSK would pick. A P25 target with modulation=cqpsk is asked for its own CQPSK, whose
 * timing loop runs at the demod rate: 16.
 */
static int
test_scoped_republish_lands_where_the_row_was_timed(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];

    static const struct {
        const char* label;
        dsd_scan_mode mode;
        dsd_scan_modulation modulation;
        int cqpsk_env;   /* DSD_NEO_CQPSK: -1 unset */
        int outstanding; /* a retune that carries the digital family is still outstanding */
        int sps;         /* the timing the decoder and the published profile get */
        int landing;     /* the family request is the marked landing */
        int explicit_choice;
        int restart; /* the command: 0 the DMR inversion, 1 a config apply whose reopen starts, 2 an explicit restart */
        int published_cqpsk; /* the CQPSK state the published profile asks for */
        int channel;         /* the channel filter it names */
    } legs[] = {
        {"landing: no modulation under -mq", DSD_SCAN_MODE_P25, DSD_SCAN_MODULATION_INHERIT, 0, 1, 10, 1, 0, 0, 1,
         DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK},
        {"landing: modulation=cqpsk", DSD_SCAN_MODE_P25, DSD_SCAN_MODULATION_CQPSK, 0, 1, 16, 1, 1, 0, 1,
         DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK},
        {"landing: auto that learned cqpsk", DSD_SCAN_MODE_P25, DSD_SCAN_MODULATION_AUTO, 1, 1, 16, 1, 1, 0, 1,
         DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK},
        {"landing: a reopen that starts", DSD_SCAN_MODE_P25, DSD_SCAN_MODULATION_INHERIT, 0, 1, 10, 1, 0, 1, 1,
         DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK},
        {"landing: nothing outstanding", DSD_SCAN_MODE_P25, DSD_SCAN_MODULATION_INHERIT, 0, 0, 16, 0, -1, 0, 1,
         DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK},
        {"landing: dmr target under -mq", DSD_SCAN_MODE_DMR, DSD_SCAN_MODULATION_INHERIT, 0, 1, 10, 1, 1, 0, 0,
         DSD_RTL_STREAM_CHANNEL_PROFILE_12K5},
        {"landing: dmr target, nothing outstanding", DSD_SCAN_MODE_DMR, DSD_SCAN_MODULATION_INHERIT, 0, 0, 16, 0, -1, 0,
         0, DSD_RTL_STREAM_CHANNEL_PROFILE_12K5},
        {"reopen: dmr target under -mq, nothing outstanding", DSD_SCAN_MODE_DMR, DSD_SCAN_MODULATION_INHERIT, 0, 0, 10,
         0, -1, 1, 0, DSD_RTL_STREAM_CHANNEL_PROFILE_12K5},
        {"reopen: modulation=cqpsk, nothing outstanding", DSD_SCAN_MODE_P25, DSD_SCAN_MODULATION_CQPSK, 0, 0, 16, 0, -1,
         1, 1, DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK},
        {"restart: dmr target under -mq, nothing outstanding", DSD_SCAN_MODE_DMR, DSD_SCAN_MODULATION_INHERIT, 0, 0, 10,
         0, -1, 2, 0, DSD_RTL_STREAM_CHANNEL_PROFILE_12K5},
        {"restart: modulation=cqpsk, nothing outstanding", DSD_SCAN_MODE_P25, DSD_SCAN_MODULATION_CQPSK, 0, 0, 16, 0,
         -1, 2, 1, DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK},
    };

    const dsd_rtl_stream_metrics_hooks hooks = {.output_rate_hz = hook_output_rate,
                                                .analog_family_active = rtl_stream_analog_family_active,
                                                .family_landing_after_pending = rtl_stream_family_landing_after_pending,
                                                .output_rate_for_family = rtl_stream_output_rate_for_family};
    int rc = 0;
    for (size_t i = 0; i < sizeof legs / sizeof legs[0]; ++i) {
        const char* label = legs[i].label;
        init_decode_mode_context(&opts, &state);
        opts.audio_in_type = AUDIO_IN_RTL;
        DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:48");
        opts.rtl_dsp_bw_khz = 48;
        state.rtl_ctx = (RtlSdrContext*)fake_ctx;
        (void)dsd_app_command_set_i32(
            DSD_APP_CMD_DECODE_MODE_SET,
            (int32_t)(legs[i].mode == DSD_SCAN_MODE_DMR ? DSDCFG_MODE_DMR : DSDCFG_MODE_P25P1));
        (void)dsd_app_drain_cmds(&opts, &state);
        /* -mq */
        dsd_scan_mode_apply_modulation(&opts, DSD_SCAN_MODE_P25, DSD_SCAN_MODULATION_CQPSK);
        state.rf_mod = 1;
        opts.trunk_scan_enabled = 1;
        dsd_rtl_stream_metrics_hooks_set(&hooks);
        reset_rx_family_wrap();
        /* The stream still runs CQPSK at the device's forced 78125 Hz, whose FSK output a landing resamples to 48 kHz. */
        g_fake_cqpsk = 1;
        g_fake_cqpsk_env = legs[i].cqpsk_env;
        g_hook_output_rate_hz = 78125U;
        g_fake_output_rate_hz = 78125U;
        g_fake_digital_rate = 78125U;
        g_fake_digital_fsk_rate = 48000U;
        g_fake_family_landing_outstanding = legs[i].outstanding;
        rc |= expect_int(label, dsd_scan_mode_enter(&opts, &state, legs[i].mode), 0);
        dsd_scan_mode_target_modulation(&state, legs[i].modulation);
        rc |= expect_int(label, dsd_scan_mode_options(&opts, &state, NULL), 0);
        if (legs[i].modulation == DSD_SCAN_MODULATION_AUTO) {
            /* The target learned CQPSK on its control channel (trunk_scan.c decides it for auto), and is timed for it. */
            dsd_scan_mode_apply_modulation(&opts, DSD_SCAN_MODE_P25, DSD_SCAN_MODULATION_AUTO);
            state.rf_mod = 1;
        }
        state.samplesPerSymbol = legs[i].sps;
        state.symbolCenter = dsd_opts_symbol_center(legs[i].sps);

        if (legs[i].restart) {
            /* The new stream opens under DSD_NEO_CQPSK=0 on the FSK discriminator, which resamples the 78125 Hz demod
               rate it publishes to 48 kHz; a CQPSK row that lands nothing is timed for that demod rate. */
            g_config_rtl_open_ok = 1;
            g_fake_cqpsk = 0;
            g_hook_output_rate_hz = 48000U;
            g_fake_output_rate_hz = 48000U;
            g_fake_request_rate_hz = 78125;
            if (legs[i].restart == 1) {
                rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_SOAPY, 0, 0, label);
            } else {
                rc |= expect_true(label, post_empty(DSD_APP_CMD_RTL_RESTART) > 0);
                rc |= expect_int(label, dsd_app_drain_cmds(&opts, &state), 1);
            }
            rc |= expect_int(label, dsd_app_command_test_last_failed(), 0);
            rc |= expect_int(label, g_config_rtl_creates == 1 && state.rtl_ctx != NULL, 1);
        } else {
            rc |= expect_int(label, dsd_app_command_action(DSD_APP_CMD_INV_DMR_TOGGLE), DSD_APP_COMMAND_SUBMIT_QUEUED);
            rc |= expect_int(label, dsd_app_drain_cmds(&opts, &state), 1);
        }
        rc |= expect_int(label, dsd_scan_mode_active(&state) == legs[i].mode && state.rf_mod == 1, 1);
        rc |= expect_int(label, state.samplesPerSymbol, legs[i].sps);
        rc |= expect_int(label, g_demod_req_calls >= 1 && g_demod_req_rate == 4800, 1);
        rc |= expect_int(label, g_demod_req_cqpsk, legs[i].published_cqpsk);
        rc |= expect_int(label, g_demod_req_chan, legs[i].channel);
        rc |= expect_int(label, g_demod_req_ted_sps, legs[i].sps);
        /* The republish's own requests: the family first, then the row's profile. */
        rc |= expect_int(label,
                         (legs[i].restart || (g_analog_req_calls == 1 && g_demod_req_calls == 1))
                             && g_analog_req_family == DSD_RX_FAMILY_DIGITAL && g_analog_req_order < g_demod_req_order,
                         1);
        rc |= expect_int(label, g_analog_req_landing, legs[i].landing);
        rc |= expect_int(label, g_analog_req_landing_explicit, legs[i].explicit_choice);

        dsd_scan_mode_leave(&opts, &state);
        dsd_rtl_stream_metrics_hooks_set(NULL);
        reset_rx_family_wrap();
        g_fake_cqpsk = 0;
        g_fake_digital_rate = 0U;
        g_fake_output_rate_hz = g_hook_output_rate_hz = 0U;
        state.rtl_ctx = NULL;
        freeState(&state);
    }
    reset_config_rtl_wrap();
    return rc;
}

/*
 * Issue #578: a config reopen that starts under a scan row's suspended scope opens the new stream on the configured
 * settings, so that start never held the row's own width to the rate the new device delivers, which a config that
 * reopens an Airspy or SoapySDR device cannot know up front. A DMR-configured scanner on an nfm row with its own 25 kHz
 * width loads a config that reopens the input as an Airspy at a 12 kHz DSP bandwidth, whose 19,531 Hz cannot filter
 * 25 kHz: the resume would ask the new stream for a row it refuses, leaving the decoder on the row's NFM over a digital
 * front end. The apply fails as a failed start does: the RTL-SDR that ran is back, with the toast naming the width and
 * the new rate, and the row's monitor is asked of it once the row is back. A rate that fits the row keeps the reopen.
 * With no stream running before, there is nothing to put back: the config stays applied on the new stream, and the
 * toast says the row cannot run there.
 */
static int
test_config_reopen_whose_scan_row_the_new_stream_refuses(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 0);
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 25000;
    rc |= expect_int("reopen row refused: nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM), 0);
    rc |= expect_int("reopen row refused: its width", dsd_scan_mode_options(&opts, &state, &row), 0);
    g_fake_analog_family = 1; /* the front end runs the row's monitor */

    reset_rx_family_wrap();
    g_config_rtl_open_ok = 1;
    g_analog_check_result = -1; /* the new stream cannot filter the row's 25 kHz */
    g_fake_request_rate_hz = 19531;
    rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_AIRSPY, 12, 0, "reopen row refused");
    rc |= expect_int("reopen row refused: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_int(
        "reopen row refused: the row held to the new stream",
        g_analog_check_calls >= 1 && g_analog_check_kind == DSD_ANALOG_DEMOD_FM && g_analog_check_width_hz == 25000, 1);
    rc |= expect_int("reopen row refused: the Airspy started, then the RTL-SDR again",
                     g_config_rtl_creates == 2 && state.rtl_ctx != NULL, 1);
    rc |= expect_str("reopen row refused: RTL-SDR back", opts.audio_in_dev, "rtl:0:851.375M:0:0:16");
    rc |= expect_int("reopen row refused: DSP bandwidth back", opts.rtl_dsp_bw_khz, 16);
    rc |= expect_toast("reopen row refused toast", &state,
                       "Config not applied: NFM 25 kHz does not fit the 19.531 kHz DSP rate");
    rc |= expect_int("reopen row refused: the row back in force",
                     dsd_scan_mode_active(&state) == DSD_SCAN_MODE_NFM && !dsd_scan_mode_updating(&state)
                         && opts.analog_only == 1 && opts.analog_nfm_bandwidth_hz == 25000,
                     1);
    rc |= expect_last_monitor_request("reopen row refused: the row's monitor asked of the RTL-SDR", 1,
                                      DSD_ANALOG_DEMOD_FM, 25000);
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(&state);
    rc |= expect_int("reopen row refused: the configured DMR kept",
                     configured && configured->analog_only == 0 && configured->frame_dmr == 1, 1);

    /* A rate that filters the row's width keeps the reopened Airspy. */
    reset_rx_family_wrap();
    g_config_rtl_open_ok = 1;
    g_fake_request_rate_hz = 39062;
    rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_AIRSPY, 24, 0, "reopen row fits");
    rc |= expect_int("reopen row fits: applied", dsd_app_command_test_last_failed(), 0);
    rc |= expect_int("reopen row fits: one start", g_config_rtl_creates, 1);
    rc |= expect_str("reopen row fits: on the Airspy", opts.audio_in_dev, "airspy");
    rc |= expect_last_monitor_request("reopen row fits: the row's monitor asked of the Airspy", 1, DSD_ANALOG_DEMOD_FM,
                                      25000);

    /* No stream before: nothing to put back. */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:16");
    opts.rtl_dsp_bw_khz = 16;
    state.rtl_ctx = NULL;
    reset_rx_family_wrap();
    g_config_rtl_open_ok = 1;
    g_analog_check_result = -1;
    g_fake_request_rate_hz = 19531;
    rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_AIRSPY, 12, 0, "reopen row refused, no stream");
    rc |= expect_int("reopen row refused, no stream: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_int("reopen row refused, no stream: the Airspy runs",
                     g_config_rtl_creates == 1 && state.rtl_ctx != NULL, 1);
    rc |= expect_str("reopen row refused, no stream: on the Airspy", opts.audio_in_dev, "airspy");
    rc |= expect_toast("reopen row refused, no stream toast", &state,
                       "Config applied; the scan row cannot run: NFM 25 kHz does not fit the 19.531 kHz DSP rate");

    dsd_scan_mode_leave(&opts, &state);
    reset_rx_family_wrap();
    g_fake_analog_family = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #578: a config apply's rollback starts the input that ran on the receive profile its stream ran. The apply runs
 * under a scan row's suspended scope, whose options are the configured settings, while the stream ran the row over
 * them. An Analog-configured session on an Airspy at a 12 kHz DSP bandwidth (19,531 Hz) scans a typed NXDN48 row, whose
 * digital profile the stream runs, with a configured NFM width of 25 kHz that rate cannot filter (the DSP bandwidth was
 * lowered while the row ran, reopening the stream on the row). A config then reopens the input as a SoapySDR device
 * that does not open: started again on the configured monitor, the Airspy would refuse 25 kHz and leave no input, so
 * it starts on the row's NXDN48 profile, and the configured settings go back for the row to resume over. The same holds
 * when the reopened stream's start refuses the configured width itself: a reopen opens on the configured settings,
 * holding their width to the rate the new stream runs whatever row is on air, as every reopen holds the configured
 * width (the analog monitor the scan leaves back to), so the config is not applied and the Airspy starts on the row.
 */
static int
test_config_rollback_restarts_the_profile_the_stream_ran(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    static const char* const labels[] = {"rollback on the row: device fails", "rollback on the row: width refused"};
    int rc = 0;
    for (int refused = 0; refused <= 1; ++refused) {
        const char* label = labels[refused];
        init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
        DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
        opts.rtl_dsp_bw_khz = 12;
        opts.analog_nfm_bandwidth_hz = 25000;
        opts.scanner_mode = 1;
        rc |= expect_int(label, dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NXDN48), 0);
        rc |= expect_int(label, dsd_scan_mode_options(&opts, &state, NULL), 0);
        reset_rx_family_wrap();
        g_config_rtl_open_ok = 1;
        g_config_rtl_device_rate_hz = 19531;
        g_config_rtl_fail_starts = refused ? 0 : 1; /* the SoapySDR device does not open */
        rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_SOAPY, 0, 25000, label);
        rc |= expect_int(label, dsd_app_command_test_last_failed(), 1);
        rc |= expect_toast(label, &state,
                           refused
                               ? "Config not applied: NFM 25 kHz does not fit the 19.531 kHz DSP rate (max 16.377 kHz)"
                               : "Config not applied: the SoapySDR input did not start (see log)");
        rc |= expect_restarted_on(label, &opts, &state, "airspy");
        rc |= expect_int(label, g_config_rtl_create_analog_only, 0);
        rc |= expect_int(label, opts.rtl_dsp_bw_khz, 12);
        rc |= expect_int(label,
                         dsd_scan_mode_active(&state) == DSD_SCAN_MODE_NXDN48 && !dsd_scan_mode_updating(&state)
                             && opts.analog_only == 0 && opts.frame_nxdn48 == 1,
                         1);
        const dsd_scan_settings* configured = dsd_scan_mode_configured_view(&state);
        rc |= expect_int(label,
                         configured && configured->analog_only == 1 && configured->analog_demod == DSD_ANALOG_DEMOD_FM
                             && configured->analog_nfm_bandwidth_hz == 25000,
                         1);
        dsd_scan_mode_leave(&opts, &state);
        reset_config_rtl_wrap();
        opts.scanner_mode = 0;
        opts.analog_nfm_bandwidth_hz = 0;
        state.rtl_ctx = NULL;
        freeState(&state);
    }
    return rc;
}

/*
 * Issue #578: with no stream running before it, a config apply whose new input runs but refuses the scan row on air
 * keeps that input and the whole config, and says the row cannot run. That holds for a config that also moves the
 * session onto the analog monitor: the new stream opened on it, so the row's refusal when the scope resumes is not the
 * front end refusing the switch, and the decoder does not go back to the mode the session had. A DMR-configured scanner
 * with no stream, on an nfm row with its own 25 kHz width, loads a config with [mode] analog that opens an Airspy at a
 * 12 kHz DSP bandwidth, whose 19,531 Hz refuses 25 kHz: the configured mode is Analog, the Airspy runs, and the toast
 * is the scan row's.
 */
static int
test_config_with_no_stream_keeps_its_mode_when_the_scan_row_cannot_run(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 0);
    opts.scanner_mode = 1;
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 25000;
    rc |= expect_int("no stream mode: nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM), 0);
    rc |= expect_int("no stream mode: its width", dsd_scan_mode_options(&opts, &state, &row), 0);
    state.rtl_ctx = NULL;

    reset_rx_family_wrap();
    g_config_rtl_open_ok = 1;
    g_analog_check_result = -1; /* the new stream cannot filter the row's 25 kHz */
    g_fake_request_rate_hz = 19531;
    g_fake_analog_req_max_hz = 16377; /* and refuses it when asked */
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_AIRSPY;
    dsd_airspy_config_defaults(&cfg.airspy);
    cfg.rtl_bw_khz = 12;
    cfg.has_mode = 1;
    cfg.decode_mode = DSDCFG_MODE_ANALOG;
    rc |= submit_config(&opts, &state, &cfg, "no stream mode");
    rc |= expect_int("no stream mode: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_int("no stream mode: the Airspy runs", g_config_rtl_creates == 1 && state.rtl_ctx != NULL, 1);
    rc |= expect_str("no stream mode: on the Airspy", opts.audio_in_dev, "airspy");
    rc |= expect_int("no stream mode: opened on the configured Analog", g_config_rtl_create_analog_only, 1);
    rc |= expect_toast("no stream mode toast", &state,
                       "Config applied; the scan row cannot run: NFM 25 kHz does not fit the 19.531 kHz DSP rate");
    rc |= expect_int("no stream mode: the configured mode is Analog",
                     dsd_scan_mode_configured_preset_exact(&opts, &state), DSDCFG_MODE_ANALOG);
    rc |= expect_int("no stream mode: the row back in force",
                     dsd_scan_mode_active(&state) == DSD_SCAN_MODE_NFM && !dsd_scan_mode_updating(&state), 1);

    dsd_scan_mode_leave(&opts, &state);
    reset_rx_family_wrap();
    opts.scanner_mode = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* A config that opens an Airspy at a 12 kHz DSP bandwidth (19,531 Hz) and sets both configured analog widths. */
static int
submit_config_airspy_widths(dsd_opts* opts, dsd_state* state, int nfm_hz, int am_hz, const char* label) {
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_AIRSPY;
    dsd_airspy_config_defaults(&cfg.airspy);
    cfg.rtl_bw_khz = 12;
    cfg.has_analog = 1;
    cfg.analog_nfm_bandwidth_hz = nfm_hz;
    cfg.analog_am_bandwidth_hz = am_hz;
    return submit_config(opts, state, &cfg, label);
}

/*
 * Issue #578: with no stream running before it, a config apply whose new input runs but refuses the scan row on air
 * keeps the whole config, its [analog] widths too. A row without a width of its own runs the configured one, so the
 * width that config sets can be the one the new stream refuses: a DMR-configured scanner with no stream, on an nfm row
 * that takes the configured 12.5 kHz, loads a config that opens an Airspy whose 19,531 Hz cannot filter the 25 kHz it
 * sets. The row's refusal when the scope resumes is the one the apply reported: the configured width stays 25 kHz and
 * the toast stays the scan row's, rather than the width going back to 12.5 kHz with "Refused: ...". An am row that
 * takes the configured AM width does the same for a 20 kHz AM width. With a stream running before, the same config is
 * rolled back, the configured 12.5 kHz with it, and the resume leaves the "Config not applied: ..." toast.
 */
static int
test_config_with_no_stream_keeps_its_width_when_the_scan_row_cannot_run(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    for (int am = 0; am <= 1; ++am) {
        const char* label = am ? "no stream am row width" : "no stream nfm row width";
        const int kind = am ? DSD_ANALOG_DEMOD_AM : DSD_ANALOG_DEMOD_FM;
        const int width_hz = am ? 20000 : 25000;
        init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 12500);
        opts.analog_am_bandwidth_hz = 8000;
        opts.scanner_mode = 1;
        rc |= expect_int(label, dsd_scan_mode_enter(&opts, &state, am ? DSD_SCAN_MODE_AM : DSD_SCAN_MODE_NFM), 0);
        rc |= expect_int(label, dsd_opts_analog_width_hz(&opts), am ? 8000 : 12500);
        state.rtl_ctx = NULL;
        reset_rx_family_wrap();
        g_config_rtl_open_ok = 1;
        g_analog_check_result = -1;       /* the new stream cannot filter the width */
        g_fake_request_rate_hz = 19531;   /* ... at the rate it delivers */
        g_fake_analog_req_max_hz = 16377; /* and refuses it when asked */
        rc |= submit_config_airspy_widths(&opts, &state, am ? 12500 : 25000, am ? 20000 : 8000, label);
        rc |= expect_int(label, dsd_app_command_test_last_failed(), 1);
        rc |= expect_int(label, g_config_rtl_creates == 1 && state.rtl_ctx != NULL, 1);
        rc |= expect_str(label, opts.audio_in_dev, "airspy");
        rc |=
            expect_toast(label, &state,
                         am ? "Config applied; the scan row cannot run: AM 20 kHz does not fit the 19.531 kHz DSP rate"
                            : "Config applied; the scan row cannot run: NFM 25 kHz does not fit the 19.531 kHz DSP "
                              "rate");
        rc |= expect_int(label, dsd_scan_mode_configured_analog_width(&opts, &state, kind), width_hz);
        rc |= expect_int(label,
                         dsd_scan_mode_active(&state) == (am ? DSD_SCAN_MODE_AM : DSD_SCAN_MODE_NFM)
                             && !dsd_scan_mode_updating(&state) && dsd_opts_analog_width_hz(&opts) == width_hz,
                         1);
        dsd_scan_mode_leave(&opts, &state);
        reset_rx_family_wrap();
        opts.scanner_mode = 0;
        opts.analog_nfm_bandwidth_hz = opts.analog_am_bandwidth_hz = 0;
        state.rtl_ctx = NULL;
        freeState(&state);
    }

    /* A stream running before: rolled back, the configured width with the rest of the receive side. */
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 12500);
    opts.scanner_mode = 1;
    rc |= expect_int("stream nfm row width", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM), 0);
    reset_rx_family_wrap();
    g_config_rtl_open_ok = 1;
    g_analog_check_result = -1;
    g_fake_request_rate_hz = 19531;
    g_fake_analog_req_max_hz = 16377;
    rc |= submit_config_airspy_widths(&opts, &state, 25000, 0, "stream nfm row width");
    rc |= expect_int("stream nfm row width: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_int("stream nfm row width: the Airspy started, then the RTL-SDR again",
                     g_config_rtl_creates == 2 && state.rtl_ctx != NULL, 1);
    rc |= expect_str("stream nfm row width: RTL-SDR back", opts.audio_in_dev, "rtl:0:851.375M:0:0:16");
    rc |= expect_toast("stream nfm row width toast", &state,
                       "Config not applied: NFM 25 kHz does not fit the 19.531 kHz DSP rate");
    rc |= expect_int("stream nfm row width: the configured width back",
                     dsd_scan_mode_configured_analog_width(&opts, &state, DSD_ANALOG_DEMOD_FM), 12500);
    rc |= expect_last_monitor_request("stream nfm row width: the row's monitor asked of the RTL-SDR", 1,
                                      DSD_ANALOG_DEMOD_FM, 12500);

    dsd_scan_mode_leave(&opts, &state);
    reset_rx_family_wrap();
    opts.scanner_mode = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #578: the row a reopen's new stream is held to is the one the scope resumes. A config that turns the scanner off
 * ([trunking] scanner = false) leaves the scope instead (apply_cmd_leave_scanner_scope()), and the session goes back to
 * the configured settings the new stream opened on, so the row it leaves is not asked of that stream. A DMR-configured
 * scanner on an nfm row with its own 25 kHz width loads a config that reopens the input as an Airspy at a 12 kHz DSP
 * bandwidth, whose 19,531 Hz cannot filter 25 kHz, and stops the scanner: the Airspy runs the configured DMR, and the
 * apply succeeds with the I/Q capture still on.
 */
static int
test_config_reopen_that_stops_the_scanner_skips_the_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 0);
    opts.scanner_mode = 1;
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 25000;
    rc |= expect_int("scanner off: nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM), 0);
    rc |= expect_int("scanner off: its width", dsd_scan_mode_options(&opts, &state, &row), 0);
    g_fake_analog_family = 1; /* the front end runs the row's monitor */
    opts.iq_capture_requested = 1;
    DSD_SNPRINTF(opts.iq_capture_path, sizeof opts.iq_capture_path, "%s", "cap.iq");

    reset_rx_family_wrap();
    g_config_rtl_open_ok = 1;
    g_analog_check_result = -1; /* the new stream cannot filter the row's 25 kHz */
    g_fake_request_rate_hz = 19531;
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_AIRSPY;
    dsd_airspy_config_defaults(&cfg.airspy);
    cfg.rtl_bw_khz = 12;
    cfg.has_trunking = 1;
    cfg.trunk_scanner = 0;
    rc |= submit_config(&opts, &state, &cfg, "scanner off");
    rc |= expect_int("scanner off: applied", dsd_app_command_test_last_failed(), 0);
    rc |= expect_int("scanner off: the Airspy started once, with the capture",
                     g_config_rtl_creates == 1 && g_config_rtl_create_capture == 1 && state.rtl_ctx != NULL, 1);
    rc |= expect_str("scanner off: on the Airspy", opts.audio_in_dev, "airspy");
    rc |= expect_int("scanner off: the capture stays on", opts.iq_capture_requested, 1);
    rc |= expect_int("scanner off: no rollback toast", strstr(state.ui_msg, "Config not applied") == NULL, 1);
    rc |= expect_int("scanner off: the scan left, on the configured DMR",
                     opts.scanner_mode == 0 && dsd_scan_mode_active(&state) == DSD_SCAN_MODE_INHERIT
                         && !dsd_scan_mode_updating(&state) && opts.analog_only == 0 && opts.frame_dmr == 1,
                     1);

    reset_rx_family_wrap();
    g_fake_analog_family = 0;
    opts.iq_capture_requested = 0;
    opts.iq_capture_path[0] = '\0';
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #526: an am scan row or target without a width of its own runs the configured AM width whenever it comes on
 * air, the AM default (6 kHz) included, which always runs its channel filter. So a config apply that leaves the session
 * on a preset that runs no AM -- a digital one, or -fA beside its own NFM width -- still holds its [analog]
 * am_bandwidth_hz to the rate, as the AM width command does: a width the front end cannot filter leaves the whole
 * config unapplied, rather than the am row skipped at every visit. A fitting width applies, and a scanner that is not
 * running holds nothing.
 */
static int
config_am_width_case(int analog_session) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    const char* tag = analog_session ? "-fA am rows" : "digital am rows";
    char label[11][64];
    static const char* const steps[11] = {
        "unfit width refused",   "unfit width held as AM",   "unfit width toast",
        "session kept",          "default refused",          "default held as AM",
        "fitting width applied", "fitting width held",       "fitting width kept the session",
        "scanner off applies",   "scanner off holds nothing"};
    for (int i = 0; i < 11; i++) {
        DSD_SNPRINTF(label[i], sizeof label[i], "%s: %s", tag, steps[i]);
    }
    if (analog_session) {
        init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);
    } else {
        init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 0);
    }
    opts.analog_nfm_bandwidth_hz = 0;
    opts.analog_am_bandwidth_hz = 8000;
    load_dmr_and_am_rows(&state, 0);
    opts.scanner_mode = 1;
    const int analog_only = opts.analog_only;

    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_config_widths(&opts, &state, 0, 16000, tag);
    rc |= expect_int(label[0], opts.analog_am_bandwidth_hz, 8000);
    rc |= expect_int(
        label[1],
        g_analog_check_calls >= 1 && g_analog_check_kind == DSD_ANALOG_DEMOD_AM && g_analog_check_width_hz == 16000, 1);
    /* The fake front end refuses at a rate that would fit, so the refusal is worded as the front end's own. */
    rc |= expect_int(
        label[2], strncmp(state.ui_msg, "Config not applied: ", 20) == 0 && strstr(state.ui_msg, "AM 16 kHz") != NULL,
        1);
    rc |= expect_int(label[3], opts.analog_only == analog_only && opts.analog_nfm_bandwidth_hz == 0, 1);

    /* The unset AM default is held as its 6 kHz. */
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_config_widths(&opts, &state, 0, 0, tag);
    rc |= expect_int(label[4], opts.analog_am_bandwidth_hz, 8000);
    rc |= expect_int(label[5], g_analog_check_kind == DSD_ANALOG_DEMOD_AM && g_analog_check_width_hz == 0, 1);

    reset_rx_family_wrap();
    rc |= submit_config_widths(&opts, &state, 0, 10000, tag);
    rc |= expect_int(label[6], opts.analog_am_bandwidth_hz, 10000);
    rc |= expect_int(label[7], g_analog_check_kind == DSD_ANALOG_DEMOD_AM && g_analog_check_width_hz == 10000, 1);
    rc |= expect_int(label[8], opts.analog_only, analog_only);

    /* Without the scanner the am row runs nothing, so nothing holds the AM width on a digital session. */
    opts.scanner_mode = 0;
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_config_widths(&opts, &state, 0, 20000, tag);
    rc |= expect_int(label[9], opts.analog_am_bandwidth_hz, 20000);
    rc |= expect_int(label[10], g_analog_check_calls, 0);

    if (rc) {
        DSD_FPRINTF(stderr, "%s: config AM width hold failed (toast '%s')\n", tag, state.ui_msg);
    }
    g_analog_check_result = 0;
    dsd_channel_modes_clear(&state);
    (void)dsd_channel_profile_set(&state, 1, NULL);
    state.lcn_freq_count = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    opts.analog_am_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

static int
test_config_apply_holds_the_configured_am_width_under_am_rows(void) {
    return config_am_width_case(0) | config_am_width_case(1);
}

/*
 * Issue #526: the reopen a config's [input] makes runs the am scan row on air again once the scope resumes, so the
 * row's own --am-bandwidth-hz is held to the rate the reopened RTL-SDR runs at, as an AM width, as an nfm row's own
 * width is: a DSP bandwidth that cannot filter it leaves the whole config unapplied, naming AM. One that can reopens.
 */
static int
test_config_apply_holds_an_am_row_width_to_a_reopen(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_dmr_session_with_nfm_width(&opts, &state, (RtlSdrContext*)fake_ctx, 0);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:118.3M:0:0:24");
    opts.rtl_dsp_bw_khz = 24;
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 20000;
    row.channel_bw_kind = DSD_ANALOG_DEMOD_AM;
    rc |= expect_int("am row reopen: am row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_AM), 0);
    rc |= expect_int("am row reopen: its width", dsd_scan_mode_options(&opts, &state, &row), 0);
    rc |= expect_int("am row reopen: in force",
                     opts.analog_demod == DSD_ANALOG_DEMOD_AM && opts.analog_am_bandwidth_hz == 20000, 1);

    char dev_before[sizeof opts.audio_in_dev];
    DSD_SNPRINTF(dev_before, sizeof dev_before, "%s", opts.audio_in_dev);
    reset_rx_family_wrap();
    rc |= submit_config_rtl_bw(&opts, &state, 16, -1, "am row reopen: 24->16");
    rc |= expect_int("am row reopen 24->16: rate kept", opts.rtl_dsp_bw_khz, 24);
    rc |= expect_str("am row reopen 24->16: input unchanged", opts.audio_in_dev, dev_before);
    rc |= expect_int("am row reopen 24->16: stream kept", state.rtl_ctx == (RtlSdrContext*)fake_ctx, 1);
    rc |=
        expect_int("am row reopen 24->16: refusal names AM",
                   strncmp(state.ui_msg, "Config not applied: AM 20 kHz does not fit the 16 kHz DSP rate", 62) == 0, 1);
    rc |= expect_int("am row reopen 24->16: the row keeps its width",
                     dsd_scan_mode_active(&state) == DSD_SCAN_MODE_AM && opts.analog_am_bandwidth_hz == 20000, 1);
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(&state);
    rc |= expect_int("am row reopen 24->16: the configured session kept",
                     configured && configured->analog_only == 0 && configured->analog_am_bandwidth_hz == 0, 1);

    state.rtl_ctx = NULL;
    rc |= submit_config_rtl_bw(&opts, &state, 48, -1, "am row reopen: 24->48");
    rc |= expect_int("am row reopen 24->48: rate applied", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_int("am row reopen 24->48: not refused", strstr(state.ui_msg, "Config not applied") == NULL, 1);

    dsd_scan_mode_leave(&opts, &state);
    opts.analog_am_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    if (rc) {
        DSD_FPRINTF(stderr, "am row reopen failed (toast '%s')\n", state.ui_msg);
    }
    freeState(&state);
    return rc;
}

#ifdef DSD_NEO_TEST_RTL_WRAP
/*
 * Issue #526: Input > Switch source > RTL-SDR opens the device at the RTL DSP bandwidth, and a scan with am rows or
 * targets without a width of their own runs the configured AM width there whenever one comes on air. So the configured
 * AM width is held to that rate beside the preset's own width -- on a digital session and on -fA alike -- and a switch
 * whose rate cannot filter it is refused before the running input is torn down. The AM default (6 kHz) fits a 12 kHz
 * DSP bandwidth; a scanner that is not running holds nothing.
 */
static int
rtl_enable_input_am_case(int analog_session) {
    static dsd_opts opts;
    static dsd_state state;
    const char* tag = analog_session ? "-fA input switch" : "digital input switch";
    init_decode_mode_context(&opts, &state);
    state.rtl_ctx = NULL;
    opts.analog_only = analog_session;
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    opts.analog_nfm_bandwidth_hz = 0;
    opts.analog_am_bandwidth_hz = 10000;
    opts.audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    opts.rtl_dsp_bw_khz = 12;
    load_dmr_and_am_rows(&state, 0);
    opts.scanner_mode = 1;

    g_config_rtl_creates = 0;
    int rc = expect_true(tag, post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT) > 0);
    state.ui_msg[0] = '\0';
    rc |= expect_int(tag, dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int(tag, opts.audio_in_type, AUDIO_IN_PULSE);
    rc |= expect_int(tag, g_config_rtl_creates, 0);
    rc |= expect_int(tag, strncmp(state.ui_msg, "Refused: AM 10 kHz does not fit the 12 kHz DSP rate", 51) == 0, 1);

    /* The AM default fits the same bandwidth. */
    opts.analog_am_bandwidth_hz = 0;
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int(tag, g_config_rtl_creates, 1);

    /* Without the scanner the am row runs nothing. */
    opts.audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    opts.analog_am_bandwidth_hz = 10000;
    opts.scanner_mode = 0;
    g_config_rtl_creates = 0;
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int(tag, g_config_rtl_creates, 1);

    if (rc) {
        DSD_FPRINTF(stderr, "%s: AM width hold failed (toast '%s')\n", tag, state.ui_msg);
    }
    dsd_channel_modes_clear(&state);
    (void)dsd_channel_profile_set(&state, 1, NULL);
    state.lcn_freq_count = 0;
    opts.analog_only = 0;
    opts.analog_am_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

static int
test_rtl_enable_input_holds_the_configured_am_width(void) {
    return rtl_enable_input_am_case(0) | rtl_enable_input_am_case(1);
}
#endif

/*
 * A config saved at the default NFM width carries [analog] with no width under it, and loading it into a session that
 * has an explicit width puts the default back (and hands it to the running monitor), rather than keeping the explicit
 * width and writing it into that file on the next save.
 */
static int
test_config_apply_restores_the_default_nfm_width(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_nfm_session(&opts, &state, (RtlSdrContext*)fake_ctx);

    static dsdneoUserConfig saved;
    dsd_snapshot_opts_to_user_config(&opts, &state, &saved);
    rc |= expect_int("saved default: [analog] present", saved.has_analog, 1);
    rc |= expect_int("saved default: no width", saved.analog_nfm_bandwidth_hz, 0);

    rc |= submit_nfm_width(&opts, &state, 12500, "explicit width before the load");
    rc |= expect_int("explicit width set", opts.analog_nfm_bandwidth_hz, 12500);
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_analog = saved.has_analog;
    cfg.analog_nfm_bandwidth_hz = saved.analog_nfm_bandwidth_hz;
    reset_rx_family_wrap();
    rc |= expect_int("load saved default queued", dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof(cfg)),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("load saved default drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("load saved default: the default is back", opts.analog_nfm_bandwidth_hz, 0);
    rc |=
        expect_int("load saved default: the monitor gets it", g_analog_req_calls == 1 && g_analog_req_width_hz == 0, 1);

    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}
#endif

#ifdef USE_RADIO
/*
 * RTL_SET_BW sets the DSP rate an RTL-SDR or rtl_tcp input runs at, so a bandwidth that cannot filter the explicit
 * NFM width the analog preset uses is refused with a toast that names both, and the bandwidth stays: the width is never
 * clamped to fit, nor left to fail the next start. The unset default, a digital session and a device that picks its
 * own rate are not held to it.
 */
static int
test_rtl_set_bw_refuses_a_rate_the_nfm_width_cannot_run_at(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_NULL; /* no restart: the check is on the options alone */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M");
    opts.analog_only = 1;
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    opts.analog_nfm_bandwidth_hz = 16000;
    opts.rtl_dsp_bw_khz = 48;

    /* A refused bandwidth keeps the running stream and its filter, so the tone received on it (issue #522) stays. */
    seed_received_tone(&state);
    uint32_t seeded = state.analog_rx.generation;
    rc |=
        expect_int("bw 16 queued", dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_BW, 16), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("bw 16 drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("bw 16 refused", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_toast("bw 16 toast", &state,
                       "Refused: DSP BW 16 kHz cannot filter NFM 16 kHz (max 13.2 kHz); narrow the NFM width first");
    rc |= expect_received_tone_kept("bw 16 refused: received tone kept", &state, seeded);

    /* A typed digital scan row does not end the analog session: its leave returns to the monitor at this rate. */
    rc |= expect_int("bw row: typed DMR row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR), 0);
    rc |= expect_int("bw row: typed DMR options", dsd_scan_mode_options(&opts, &state, NULL), 0);
    rc |= expect_int("bw row: the row is digital", opts.analog_only, 0);
    (void)dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_BW, 16);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("bw row: 16 refused under the row", opts.rtl_dsp_bw_khz, 48);
    dsd_scan_mode_leave(&opts, &state);

    rc |=
        expect_int("bw 24 queued", dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_BW, 24), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("bw 24 drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("bw 24 applied", opts.rtl_dsp_bw_khz, 24);

    opts.analog_nfm_bandwidth_hz = 0; /* the default keeps its historical rule at any rate */
    (void)dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_BW, 12);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("default width: bw 12 applied", opts.rtl_dsp_bw_khz, 12);

    opts.analog_nfm_bandwidth_hz = 25000;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "soapy:driver=airspy");
    (void)dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_BW, 16);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("device-picked rate: bw 16 applied", opts.rtl_dsp_bw_khz, 16);

    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtltcp:127.0.0.1:1234");
    opts.analog_only = 0; /* a digital session does not use the width */
    (void)dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_BW, 8);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("digital: bw 8 applied", opts.rtl_dsp_bw_khz, 8);
    opts.analog_only = 1;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_BW, 24);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("rtl_tcp: bw 24 refused for 25 kHz", opts.rtl_dsp_bw_khz, 8);
    rc |=
        expect_toast("rtl_tcp: bw 24 toast", &state, "Refused: DSP BW 24 kHz cannot filter NFM 25 kHz (max 20.4 kHz)");

    /* The terminal's Input > Switch source > RTL-SDR leaves "pulse" on the RTL input it enables, which the stream opens
       as an RTL-SDR at the DSP bandwidth: the bandwidth is held to the width there too, refused before the restart that
       would tear the running stream down and fail to open the next one. */
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    opts.analog_nfm_bandwidth_hz = 16000;
    opts.rtl_dsp_bw_khz = 48;
    seed_received_tone(&state);
    seeded = state.analog_rx.generation;
    rc |= expect_int("pulse on rtl: bw 16 queued", dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_BW, 16),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    state.ui_msg[0] = '\0';
    rc |= expect_int("pulse on rtl: bw 16 drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("pulse on rtl: bw 16 refused", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_toast("pulse on rtl: bw 16 toast", &state,
                       "Refused: DSP BW 16 kHz cannot filter NFM 16 kHz (max 13.2 kHz); narrow the NFM width first");
    rc |= expect_received_tone_kept("pulse on rtl: bw 16 refused: received tone kept", &state, seeded);
    /* The same device string on PCM input is Pulse audio, which no DSP bandwidth filters: nothing to hold. */
    opts.audio_in_type = AUDIO_IN_PULSE;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_BW, 16);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("pulse input: bw 16 applied", opts.rtl_dsp_bw_khz, 16);

    opts.audio_in_type = AUDIO_IN_NULL;
    opts.analog_nfm_bandwidth_hz = 0;
    freeState(&state);
    return rc;
}

/* Import @p csv as the channel map through the command queue and leave the toast in @p state. */
static int
import_channel_map_text(dsd_opts* opts, dsd_state* state, const char* path, const char* csv, const char* label) {
    int rc = write_file_bytes(path, csv, strlen(csv));
    state->ui_msg[0] = '\0';
    post_string(DSD_APP_CMD_IMPORT_CHANNEL_MAP, path);
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/*
 * Issue #526: an nfm row's width is held to the DSP rate when the channel map loads. The map loads either way; a row
 * the rate cannot run (its own --nfm-bandwidth-hz, or the configured NFM width a row without one runs) is skipped at
 * every visit, so the import names the first such row and its reason rather than a plain success. With no stream
 * running an RTL-SDR input's rate is the one its DSP bandwidth sets; DSD_NEO_CHANNEL_LPF=0 refuses every explicit
 * width.
 */
static int
test_channel_map_import_names_rows_the_dsp_rate_skips(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_ui_cmd_queue_nfm_map") != 0) {
        DSD_FPRINTF(stderr, "temp working directory setup failed: %s\n", strerror(errno));
        return 1;
    }
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:16");
    opts.rtl_dsp_bw_khz = 16;
    state.rtl_ctx = NULL;
    static const char mixed[] = "channel,frequency_hz,name,mode,options\n"
                                "1,461000000,dmr,dmr,\n"
                                "2,154430000,wide,nfm,--nfm-bandwidth-hz 20000\n"
                                "3,155475000,narrow,nfm,--nfm-bandwidth-hz 12500\n"
                                "4,155490000,wider,nfm,--nfm-bandwidth-hz 25000\n";
    rc |= import_channel_map_text(&opts, &state, "nfm_map.csv", mixed, "mixed map drained");
    rc |= expect_int("mixed map loaded", state.lcn_freq_count, 4);
    rc |= expect_str("mixed map path", opts.chan_in_file, "nfm_map.csv");
    rc |= expect_toast("mixed map names the skipped rows", &state,
                       "Imported; scan channel 2 and 1 more are skipped at every visit: NFM 20 kHz does not fit the "
                       "16 kHz DSP rate");

    /* A 24 kHz DSP bandwidth runs 20 kHz, not 25. */
    opts.rtl_dsp_bw_khz = 24;
    rc |= import_channel_map_text(&opts, &state, "nfm_map.csv", mixed, "24 kHz map drained");
    rc |= expect_toast("24 kHz names one row", &state,
                       "Imported; scan channel 4 is skipped at every visit: NFM 25 kHz does not fit the 24 kHz DSP "
                       "rate");

    /* A row without a width runs the configured one. */
    static const char inherits[] = "channel,frequency_hz,name,mode,options\n"
                                   "1,154430000,plain,nfm,\n";
    opts.rtl_dsp_bw_khz = 16;
    opts.analog_nfm_bandwidth_hz = 16000;
    rc |= import_channel_map_text(&opts, &state, "nfm_plain.csv", inherits, "configured-width map drained");
    rc |= expect_toast("configured width named", &state,
                       "Imported; scan channel 1 is skipped at every visit: NFM 16 kHz does not fit the 16 kHz DSP "
                       "rate");
    opts.analog_nfm_bandwidth_hz = 0;
    rc |= import_channel_map_text(&opts, &state, "nfm_plain.csv", inherits, "default-width map drained");
    rc |= expect_toast("default width imports as before", &state, "Applied: Channel map imported -> nfm_plain.csv");

    /* Every explicit width needs the filter DSD_NEO_CHANNEL_LPF=0 turns off, whatever the rate. */
    opts.rtl_dsp_bw_khz = 48;
    (void)dsd_setenv("DSD_NEO_CHANNEL_LPF", "0", 1);
    dsd_neo_config_init();
    rc |= import_channel_map_text(&opts, &state, "nfm_map.csv", mixed, "lpf-off map drained");
    rc |= expect_toast("lpf-off names the rows", &state,
                       "Imported; scan channel 2 and 2 more are skipped at every visit: NFM 20 kHz needs the "
                       "filter DSD_NEO_CHANNEL_LPF=0 turns off");
    (void)dsd_unsetenv("DSD_NEO_CHANNEL_LPF");
    dsd_neo_config_init();

    /* At 48 kHz every row runs. */
    rc |= import_channel_map_text(&opts, &state, "nfm_map.csv", mixed, "48 kHz map drained");
    rc |= expect_toast("48 kHz imports as before", &state, "Applied: Channel map imported -> nfm_map.csv");

    /* Audio input applies no width: nothing to name. */
    opts.audio_in_type = AUDIO_IN_NULL;
    opts.rtl_dsp_bw_khz = 16;
    rc |= import_channel_map_text(&opts, &state, "nfm_map.csv", mixed, "audio input map drained");
    rc |= expect_toast("audio input imports as before", &state, "Applied: Channel map imported -> nfm_map.csv");

    (void)remove("nfm_map.csv");
    (void)remove("nfm_plain.csv");
    freeState(&state);
    if (dsd_test_temp_cwd_leave(&cwd) != 0) {
        rc = 1;
    }
    return rc;
}
#endif

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
/*
 * Issue #634: a switch to a PCM input keeps the radio spec it replaced, and Input > Switch source > RTL-SDR goes back to
 * it: the rtl_tcp server or SoapySDR device the session ran, not a USB RTL-SDR the PCM name would make of it. The
 * destination is checked before the running input is touched, and a refusal leaves the input's name as it was. The
 * Airspy row opens its own device whatever was kept, and with no eligible spec the row opens an RTL-SDR, saved as one.
 */
static int
test_rtl_row_returns_to_the_radio_a_switch_left(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;

    state.rtl_ctx = NULL;
    opts.audio_in_type = AUDIO_IN_UDP;
    opts.wav_sample_rate = 96000; /* the symbol timing is in 96 kHz units under this PCM input */
    opts.rtltcp_enabled = 0;      /* as an Airspy run since leaves it */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "udp:127.0.0.1:7355");
    DSD_SNPRINTF(opts.radio_in_dev, sizeof opts.radio_in_dev, "%s", "rtltcp:host.example:1234");
    g_noted_timing_calls = 0;
    g_noted_timing_rate = -1;
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    g_reconfigure_guarded = -1;
#endif
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    rc |= expect_int("back to rtl_tcp drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("back to rtl_tcp: the stream opened on it", g_config_rtl_create_dev, "rtltcp:host.example:1234");
    rc |= expect_str("back to rtl_tcp: named for it", opts.audio_in_dev, "rtltcp:host.example:1234");
    rc |= expect_int("back to rtl_tcp: radio input", opts.audio_in_type, AUDIO_IN_RTL);
    rc |= expect_int("back to rtl_tcp: the engine ends the reception", state.input_boundary, 1);
    rc |= expect_int("back to rtl_tcp: its backend flag follows", opts.rtltcp_enabled, 1);
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    rc |= expect_int("back to rtl_tcp: the output follows under the tick guard", g_reconfigure_guarded, 1);
#endif
    /* The RTL output rescale starts from the rate the PCM input left the timing in (DSP side: RTL_SYMBOL_CACHE_GENERATION). */
    rc |= expect_int("back to rtl_tcp: the timing's rate is noted once", g_noted_timing_calls, 1);
    rc |= expect_int("back to rtl_tcp: at the pcm input's rate", g_noted_timing_rate, 96000);
    opts.wav_sample_rate = 48000;
    /* A radio restarted on the radio notes nothing: its timing is already in the output's units. */
    g_noted_timing_calls = 0;
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    rc |= expect_int("radio to radio drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("radio to radio: nothing noted", g_noted_timing_calls, 0);

    /* The Airspy row opens the Airspy, whatever was kept. */
    state.rtl_ctx = NULL;
    opts.audio_in_type = AUDIO_IN_UDP;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "udp:127.0.0.1:7355");
    DSD_SNPRINTF(opts.radio_in_dev, sizeof opts.radio_in_dev, "%s", "soapy:driver=x");
    (void)post_empty(DSD_APP_CMD_AIRSPY_ENABLE_INPUT);
    rc |= expect_int("airspy row drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("airspy row opens the airspy", strncmp(g_config_rtl_create_dev, "airspy", 6), 0);

    /* No eligible spec (none kept, or an Airspy one): an RTL-SDR, which a saved config names as one. */
    state.rtl_ctx = NULL;
    opts.audio_in_type = AUDIO_IN_UDP;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "udp:127.0.0.1:7355");
    DSD_SNPRINTF(opts.radio_in_dev, sizeof opts.radio_in_dev, "%s", "airspy:serial=1");
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    rc |= expect_int("rtl row with no spec drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("rtl row with no spec: an rtl-sdr", opts.audio_in_dev, "rtl");
    rc |= expect_int("rtl row with no spec: no rtl_tcp backend", opts.rtltcp_enabled, 0);
    dsdneoUserConfig snap;
    DSD_MEMSET(&snap, 0, sizeof snap);
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    rc |= expect_int("a bare rtl saves as a radio input", snap.input_source, DSDCFG_INPUT_RTL);

    /* A width the RTL DSP bandwidth cannot filter refuses an RTL-SDR destination, before anything is touched... */
    state.rtl_ctx = NULL;
    opts.analog_only = 1;
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    opts.analog_nfm_bandwidth_hz = 16000;
    opts.rtl_dsp_bw_khz = 12;
    opts.audio_in_type = AUDIO_IN_UDP;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "udp:127.0.0.1:7355");
    DSD_SNPRINTF(opts.radio_in_dev, sizeof opts.radio_in_dev, "%s", "rtl:0:851.0125M:22:0:12");
    g_config_rtl_creates = 0;
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    rc |= expect_int("unfit rtl destination drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("unfit rtl destination: nothing opened", g_config_rtl_creates, 0);
    rc |= expect_str("unfit rtl destination: the name stays", opts.audio_in_dev, "udp:127.0.0.1:7355");
    rc |= expect_contains("unfit rtl destination toast", state.ui_msg, "Refused: NFM 16 kHz");
    /* ...but not a SoapySDR one, whose device sets its own rate. */
    DSD_SNPRINTF(opts.radio_in_dev, sizeof opts.radio_in_dev, "%s", "soapy:driver=x");
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    rc |= expect_int("soapy destination drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("soapy destination opens", g_config_rtl_creates, 1);
    rc |= expect_str("soapy destination named", opts.audio_in_dev, "soapy:driver=x");
    opts.analog_only = 0;
    opts.analog_nfm_bandwidth_hz = 0;

    /* The M17 stream encoder keeps the input it started on. */
    state.rtl_ctx = NULL;
    opts.audio_in_type = AUDIO_IN_UDP;
    opts.m17encoder = 1;
    g_config_rtl_creates = 0;
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    rc |= expect_int("m17 encoder drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("m17 encoder: nothing opened", g_config_rtl_creates, 0);
    rc |= expect_contains("m17 encoder toast", state.ui_msg, "Unsupported: the M17 stream encoder");
    opts.m17encoder = 0;

    state.rtl_ctx = NULL;
    reset_config_rtl_wrap();
    freeState(&state);
    return rc;
}

/* Whether @p port binds on loopback now: nothing holds it. */
static int
loopback_udp_port_is_free(int port) {
    dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == DSD_INVALID_SOCKET) {
        return 0;
    }
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int bound = dsd_socket_bind(sock, (struct sockaddr*)&addr, sizeof addr) == 0;
    dsd_socket_close(sock);
    return bound;
}

/*
 * Issue #634: once the radio runs, the PCM input it replaced is closed (its file, or its UDP socket and receive thread,
 * with the port), as an input switch closes the input it replaces; a radio that does not start leaves it running.
 */
static int
test_rtl_row_closes_the_pcm_input_it_replaced(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_ui_rtl_row_closes_pcm") != 0) {
        return expect_true("rtl row pcm temp directory", 0);
    }
    rc |= write_pcm16_wav("voice.wav", 48000U, 8U);
    install_udp_input_hooks();
    init_decode_mode_context(&opts, &state);
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    DSD_SNPRINTF(opts.radio_in_dev, sizeof opts.radio_in_dev, "%s", "rtltcp:host.example:1234");

    state.rtl_ctx = NULL;
    (void)post_string(DSD_APP_CMD_INPUT_WAV_SET, "voice.wav");
    rc |= expect_int("wav drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("the wav plays", opts.audio_in_file != NULL && opts.audio_in_file_info != NULL);
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    rc |= expect_int("wav to radio drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("wav to radio: the radio runs", opts.audio_in_type, AUDIO_IN_RTL);
    rc |= expect_true("wav to radio: the wav is closed", opts.audio_in_file == NULL && opts.audio_in_file_info == NULL);

    const int udp_port = free_loopback_udp_port();
    rc |= expect_true("rtl row free udp port", udp_port > 0);
    state.rtl_ctx = NULL;
    (void)post_host_port(DSD_APP_CMD_UDP_INPUT_CFG, "127.0.0.1", udp_port);
    rc |= expect_int("udp drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("udp runs", opts.udp_in_ctx != NULL);
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    rc |= expect_int("udp to radio drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("udp to radio: the udp input is stopped", opts.udp_in_ctx == NULL);
    rc |= expect_true("udp to radio: its port is free again", loopback_udp_port_is_free(udp_port));

    /* A radio that does not start keeps the input. */
    state.rtl_ctx = NULL;
    (void)post_string(DSD_APP_CMD_INPUT_WAV_SET, "voice.wav");
    rc |= expect_int("wav again drained", dsd_app_drain_cmds(&opts, &state), 1);
    SNDFILE* const playing = opts.audio_in_file;
    g_config_rtl_open_ok = 0;
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    rc |= expect_int("failed radio drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("failed radio: the wav plays on", playing != NULL && opts.audio_in_file == playing);

    state.rtl_ctx = NULL;
    reset_config_rtl_wrap();
    closeAudioInDevice(&opts);
    clear_net_audio_input_hooks();
    freeState(&state);
    (void)remove("voice.wav");
    rc |= expect_int("rtl row pcm temp directory removed", dsd_test_temp_cwd_leave(&cwd), 0);
    return rc;
}

/*
 * Input > Switch source > RTL-SDR opens a device at the RTL DSP bandwidth, so an explicit NFM width that bandwidth
 * cannot filter is refused before the running input is rewritten and torn down: the new stream's start would refuse the
 * width and leave no stream, with only a generic failure to show for it. The config path refuses the same move. The
 * unset default, a bandwidth that fits, and inputs whose device or capture sets the rate (SoapySDR, the Airspy enable)
 * are not held to a rate: the Airspy enable is held only to the rules every rate shares (DSD_NEO_CHANNEL_LPF=0).
 */
static int
test_rtl_enable_input_holds_the_nfm_width(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_decode_mode_context(&opts, &state);
    opts.analog_only = 1;
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    state.rtl_ctx = NULL;

    /* An Airspy running an explicit 25 kHz at its forced rate; the switch rewrites it to an RTL-SDR at 24 kHz. */
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
    opts.analog_nfm_bandwidth_hz = 25000;
    opts.rtl_dsp_bw_khz = 24;
    g_config_rtl_creates = 0;
    int rc = expect_true("airspy to rtl queued", post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT) > 0);
    state.ui_msg[0] = '\0';
    rc |= expect_int("airspy to rtl drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("airspy to rtl: input unchanged", opts.audio_in_dev, "airspy");
    rc |= expect_int("airspy to rtl: nothing reopened", g_config_rtl_creates, 0);
    rc |=
        expect_toast("airspy to rtl toast", &state,
                     "Refused: NFM 25 kHz does not fit the 24 kHz DSP rate (max 20.4 kHz); use a 48 kHz DSP bandwidth");

    /* PCM input: the switch opens the RTL-SDR the "pulse" device string becomes on an RTL input. A refused switch
       keeps the input, and with it the tone received on it (issue #522). */
    opts.audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    opts.analog_nfm_bandwidth_hz = 16000;
    opts.rtl_dsp_bw_khz = 12;
    seed_received_tone(&state);
    const uint32_t seeded = state.analog_rx.generation;
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    state.ui_msg[0] = '\0';
    rc |= expect_int("pcm to rtl drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("pcm to rtl: still PCM", opts.audio_in_type, AUDIO_IN_PULSE);
    rc |= expect_int("pcm to rtl: nothing opened", g_config_rtl_creates, 0);
    rc |= expect_toast("pcm to rtl toast", &state,
                       "Refused: NFM 16 kHz does not fit the 12 kHz DSP rate (max 9.6 kHz); use a 24 or 48 kHz DSP "
                       "bandwidth");
    rc |= expect_received_tone_kept("pcm to rtl refused: received tone kept", &state, seeded);

    /* A bandwidth that fits, and the unset default at any bandwidth, open the stream. */
    opts.rtl_dsp_bw_khz = 24;
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("pcm to rtl at 24 kHz: opened", g_config_rtl_creates, 1);
    opts.audio_in_type = AUDIO_IN_PULSE;
    opts.analog_nfm_bandwidth_hz = 0;
    opts.rtl_dsp_bw_khz = 12;
    g_config_rtl_creates = 0;
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("default width at 12 kHz: opened", g_config_rtl_creates, 1);

    /* A SoapySDR device, and the Airspy enable, run at a rate the device sets: their start holds the width to it. */
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.analog_nfm_bandwidth_hz = 25000;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "soapy:driver=airspy");
    g_config_rtl_creates = 0;
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("soapy: not held", g_config_rtl_creates, 1);
    g_config_rtl_creates = 0;
    (void)post_empty(DSD_APP_CMD_AIRSPY_ENABLE_INPUT);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("airspy enable: not held", g_config_rtl_creates, 1);

    /* A digital session does not use the width. */
    opts.analog_only = 0;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl");
    g_config_rtl_creates = 0;
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("digital: not held", g_config_rtl_creates, 1);

    /* Issue #526: the switch is unscoped, so the stream it opens starts on the settings in force, an nfm scan row's
       own width among them, which the configured digital session does not use: that width is held to the rate too. */
    opts.audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    opts.analog_nfm_bandwidth_hz = 0;
    opts.rtl_dsp_bw_khz = 24;
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 25000;
    rc |= expect_int("row width: nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM), 0);
    rc |= expect_int("row width: its width", dsd_scan_mode_options(&opts, &state, &row), 0);
    g_config_rtl_creates = 0;
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    state.ui_msg[0] = '\0';
    rc |= expect_int("row width to rtl drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("row width to rtl: still PCM", opts.audio_in_type, AUDIO_IN_PULSE);
    rc |= expect_int("row width to rtl: nothing opened", g_config_rtl_creates, 0);
    rc |=
        expect_toast("row width to rtl toast", &state,
                     "Refused: NFM 25 kHz does not fit the 24 kHz DSP rate (max 20.4 kHz); use a 48 kHz DSP bandwidth");
    /* The same row at a 48 kHz DSP bandwidth opens. */
    opts.rtl_dsp_bw_khz = 48;
    (void)post_empty(DSD_APP_CMD_RTL_ENABLE_INPUT);
    (void)dsd_app_drain_cmds(&opts, &state);
    rc |= expect_int("row width to rtl at 48 kHz: opened", g_config_rtl_creates, 1);
    dsd_scan_mode_leave(&opts, &state);
    opts.analog_only = 0;

    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* Post an input switch (@p command) from the input the options describe, with a tone received on it, and drain it. */
static int
switch_input_from(dsd_opts* opts, dsd_state* state, int command, const char* label) {
    seed_received_tone(state);
    int rc = expect_true(label, post_empty(command) > 0);
    state->ui_msg[0] = '\0';
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/*
 * Issue #578: Input > Switch source never leaves the session without a working input. A start that fails after the
 * switch rewrote the input, refused an analog width the rate the device delivered cannot filter or failing for the
 * device, puts back the input it replaced: a PCM input, which the switch never closed, is only restored, and an
 * RTL-family input that ran is started again. Neither is reset as a new input would be (the tone received on it stays).
 * The message says why: a width refusal names the width and the rate, anything else points at the log.
 */
static int
test_input_switch_that_fails_keeps_the_running_input(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.analog_only = 1;
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    opts.analog_nfm_bandwidth_hz = 25000;

    /* PCM -fA with an explicit 25 kHz, stored there; the Airspy delivers 19,531 Hz at a 12 kHz DSP bandwidth. */
    opts.audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    opts.rtl_dsp_bw_khz = 12;
    state.rtl_ctx = NULL;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    g_config_rtl_refuse_rate_hz = 19531;
    const uint32_t pcm_generation = state.analog_rx.generation;
    rc |= switch_input_from(&opts, &state, DSD_APP_CMD_AIRSPY_ENABLE_INPUT, "pcm to airspy drained");
    rc |= expect_str("pcm to airspy: the Airspy was tried", g_config_rtl_create_dev, "airspy");
    rc |= expect_int("pcm to airspy: nothing restarted", g_config_rtl_creates, 1);
    rc |= expect_int("pcm to airspy: still PCM", opts.audio_in_type, AUDIO_IN_PULSE);
    rc |= expect_str("pcm to airspy: device kept", opts.audio_in_dev, "pulse");
    rc |= expect_int("pcm to airspy: no stream", state.rtl_ctx == NULL, 1);
    rc |= expect_toast("pcm to airspy toast", &state, "Refused: NFM 25 kHz does not fit the 19.531 kHz DSP rate");
    rc |= expect_received_tone_kept("pcm to airspy: received tone kept", &state, pcm_generation);

    /* A running rtl_tcp input at a 48 kHz DSP bandwidth, which runs the width: put back and started again. */
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtltcp:127.0.0.1:1234");
    opts.rtltcp_enabled = 1;
    DSD_SNPRINTF(opts.rtltcp_hostname, sizeof opts.rtltcp_hostname, "%s", "127.0.0.1");
    opts.rtltcp_portno = 1234;
    opts.rtl_dsp_bw_khz = 48;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    g_config_rtl_refuse_rate_hz = 19531;
    const uint32_t tcp_generation = state.analog_rx.generation;
    rc |= switch_input_from(&opts, &state, DSD_APP_CMD_AIRSPY_ENABLE_INPUT, "rtl_tcp to airspy drained");
    rc |= expect_int("rtl_tcp to airspy: tried, then restarted", g_config_rtl_creates == 2 && g_config_rtl_starts == 2,
                     1);
    rc |= expect_str("rtl_tcp to airspy: restarted on rtl_tcp", g_config_rtl_create_dev, "rtltcp:127.0.0.1:1234");
    rc |= expect_str("rtl_tcp to airspy: device put back", opts.audio_in_dev, "rtltcp:127.0.0.1:1234");
    rc |= expect_int("rtl_tcp to airspy: rtl_tcp put back", opts.rtltcp_enabled, 1);
    rc |= expect_int("rtl_tcp to airspy: stream running", state.rtl_ctx != NULL && opts.rtl_started == 1, 1);
    rc |= expect_toast("rtl_tcp to airspy toast", &state, "Refused: NFM 25 kHz does not fit the 19.531 kHz DSP rate");
    rc |= expect_received_tone_kept("rtl_tcp to airspy: received tone kept", &state, tcp_generation);

    /* A running Airspy switched to an RTL-SDR that fails to open: no width to blame. */
    opts.analog_nfm_bandwidth_hz = 12500;
    opts.rtltcp_enabled = 0;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    const uint32_t airspy_generation = state.analog_rx.generation;
    rc |= switch_input_from(&opts, &state, DSD_APP_CMD_RTL_ENABLE_INPUT, "airspy to rtl drained");
    rc |= expect_int("airspy to rtl: tried, then restarted", g_config_rtl_creates, 2);
    rc |= expect_str("airspy to rtl: restarted on the Airspy", g_config_rtl_create_dev, "airspy");
    rc |= expect_str("airspy to rtl: device put back", opts.audio_in_dev, "airspy");
    rc |= expect_int("airspy to rtl: stream running", state.rtl_ctx != NULL && opts.rtl_started == 1, 1);
    rc |= expect_str("airspy to rtl toast", state.ui_msg, "Failed: the RTL-SDR input did not start (see log)");
    rc |= expect_received_tone_kept("airspy to rtl: received tone kept", &state, airspy_generation);

    /* A start that succeeds switches as before. */
    opts.audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    state.rtl_ctx = NULL;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    rc |= switch_input_from(&opts, &state, DSD_APP_CMD_AIRSPY_ENABLE_INPUT, "pcm to airspy that opens drained");
    rc |= expect_str("pcm to airspy that opens: on the Airspy", opts.audio_in_dev, "airspy");
    rc |= expect_int("pcm to airspy that opens: stream running", state.rtl_ctx != NULL, 1);
    rc |= expect_str("pcm to airspy that opens toast", state.ui_msg, "Applied: Airspy input enabled");

    reset_config_rtl_wrap();
    opts.analog_only = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #578: the P25 SM watchdog reads the input, and may retune it, while it holds its tick guard. Input > Switch
 * source holds that guard from its rewrite of the input to the end of its start, and, when the start fails, on through
 * putting back the input that ran and starting it again: the watchdog never sees the input half rewritten or half put
 * back, whether the switch works or not.
 */
static int
test_input_switch_holds_the_watchdog_guard(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.analog_only = 1;
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    opts.analog_nfm_bandwidth_hz = 25000;
    opts.rtl_dsp_bw_khz = 48;
    opts.rtlsdr_center_freq = 851375000U;

    /* A running rtl_tcp input switched to an Airspy whose start refuses the width: put back and started again. A P25
       watchdog retune that completed just before the switch took the guard stands: the input put back is the one the
       switch found under the guard, not a copy read before it. */
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtltcp:127.0.0.1:1234");
    opts.rtltcp_enabled = 1;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    g_config_rtl_refuse_rate_hz = 19531;
    guard_probe_arm_with_retune(&opts, 851387500U);
    rc |= switch_input_from(&opts, &state, DSD_APP_CMD_AIRSPY_ENABLE_INPUT, "guarded rtl_tcp to airspy drained");
    guard_probe_disarm();
    rc |= expect_int("guarded rtl_tcp to airspy: tried, then restarted", g_config_rtl_creates, 2);
    rc |= expect_str("guarded rtl_tcp to airspy: rtl_tcp put back", opts.audio_in_dev, "rtltcp:127.0.0.1:1234");
    rc |=
        expect_int("guarded rtl_tcp to airspy: the watchdog's retune stands", (int)opts.rtlsdr_center_freq, 851387500);
    rc |= expect_int("guarded rtl_tcp to airspy: one hold", g_guard_probe_holds, 1);
    rc |= expect_int("guarded rtl_tcp to airspy: no write outside the guard", g_guard_probe_unguarded, 0);

    /* A running Airspy switched to an RTL-SDR that does not open. */
    opts.analog_nfm_bandwidth_hz = 12500;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
    opts.rtltcp_enabled = 0;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    guard_probe_arm(&opts);
    rc |= switch_input_from(&opts, &state, DSD_APP_CMD_RTL_ENABLE_INPUT, "guarded airspy to rtl drained");
    guard_probe_disarm();
    rc |= expect_str("guarded airspy to rtl: Airspy put back", opts.audio_in_dev, "airspy");
    rc |= expect_int("guarded airspy to rtl: one hold", g_guard_probe_holds, 1);
    rc |= expect_int("guarded airspy to rtl: no write outside the guard", g_guard_probe_unguarded, 0);

    /* A switch that works rewrites the input under the guard as well. */
    opts.audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    state.rtl_ctx = NULL;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    guard_probe_arm(&opts);
    rc |= switch_input_from(&opts, &state, DSD_APP_CMD_AIRSPY_ENABLE_INPUT, "guarded pcm to airspy drained");
    guard_probe_disarm();
    rc |= expect_str("guarded pcm to airspy: on the Airspy", opts.audio_in_dev, "airspy");
    rc |= expect_int("guarded pcm to airspy: no write outside the guard", g_guard_probe_unguarded, 0);

    reset_config_rtl_wrap();
    opts.analog_only = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/*
 * Issue #578: every stream start opens the I/Q capture (--iq-capture) anew, writing over the file. A switch whose start
 * fails has already stopped the stream that ran, which closed the capture with what it had recorded, so the restart
 * that puts that input back runs without the capture: it stays off for the rest of the session, the log says so, and
 * the message does when it fits. A switch that works opens the capture on the new input as before.
 */
static int
test_input_switch_rollback_keeps_the_iq_capture(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.analog_only = 1;
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    opts.analog_nfm_bandwidth_hz = 12500;
    opts.rtl_dsp_bw_khz = 48;
    opts.iq_capture_requested = 1;
    DSD_SNPRINTF(opts.iq_capture_path, sizeof opts.iq_capture_path, "%s", "cap.iq");

    /* A running Airspy recording I/Q, switched to an RTL-SDR that does not open. */
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    g_capture_log[0] = '\0';
    rc |= switch_input_from(&opts, &state, DSD_APP_CMD_RTL_ENABLE_INPUT, "capture airspy to rtl drained");
    rc |= expect_capture_stopped("capture airspy to rtl: recording kept", &opts, 0);
    rc |= expect_str("capture airspy to rtl: Airspy back", opts.audio_in_dev, "airspy");
    rc |= expect_str("capture airspy to rtl toast", state.ui_msg,
                     "Failed: the RTL-SDR input did not start (see log); I/Q capture stopped");

    /* A start that fails after it opened the capture (an Airspy SDK that does not start) has already written the file
       anew: the log says so, rather than claim the recording is kept, and the capture stops all the same. */
    opts.iq_capture_requested = 1;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:48");
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    g_config_rtl_fail_after_capture = 1;
    g_capture_log[0] = '\0';
    rc |= switch_input_from(&opts, &state, DSD_APP_CMD_AIRSPY_ENABLE_INPUT, "capture rtl to airspy drained");
    rc |= expect_capture_stopped("capture rtl to airspy: recording written over", &opts, 1);
    rc |= expect_str("capture rtl to airspy: RTL-SDR back", opts.audio_in_dev, "rtl:0:851.375M:0:0:48");
    rc |= expect_str("capture rtl to airspy toast", state.ui_msg,
                     "Failed: the Airspy input did not start (see log); I/Q capture stopped");

    /* A recording RTL-SDR session switched to Pulse keeps its stream running in the background, recording. A switch to
       an Airspy that does not open stops that stream, which closes the capture with what it recorded, and puts Pulse
       back, which starts nothing: the capture stops all the same, so that no later radio start writes over the file,
       and the log and the message say so. */
    opts.audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    opts.iq_capture_requested = 1;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    g_capture_log[0] = '\0';
    rc |= switch_input_from(&opts, &state, DSD_APP_CMD_AIRSPY_ENABLE_INPUT, "capture pcm over a stream drained");
    rc |= expect_int("capture pcm over a stream: the Airspy tried with the capture, nothing restarted",
                     g_config_rtl_creates == 1 && g_config_rtl_captures == 1, 1);
    rc |= expect_int("capture pcm over a stream: still Pulse, the stream stopped",
                     opts.audio_in_type == AUDIO_IN_PULSE && state.rtl_ctx == NULL, 1);
    rc |= expect_str("capture pcm over a stream: device put back", opts.audio_in_dev, "pulse");
    rc |= expect_int("capture pcm over a stream: capture off", opts.iq_capture_requested, 0);
    rc |= expect_int("capture pcm over a stream: logged, recording kept",
                     strstr(g_capture_log, "I/Q capture stopped") != NULL && strstr(g_capture_log, "cap.iq") != NULL
                         && strstr(g_capture_log, "which is kept") != NULL,
                     1);
    rc |= expect_str("capture pcm over a stream toast", state.ui_msg,
                     "Failed: the Airspy input did not start (see log); I/Q capture stopped");
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    rc |= switch_input_from(&opts, &state, DSD_APP_CMD_AIRSPY_ENABLE_INPUT, "capture pcm over a stream: then opens");
    rc |= expect_int("capture pcm over a stream: the next start does not record",
                     g_config_rtl_creates == 1 && g_config_rtl_create_capture == 0 && state.rtl_ctx != NULL, 1);

    /* The same with no stream running behind Pulse: none was stopped, so the capture stays on. */
    opts.audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    opts.iq_capture_requested = 1;
    state.rtl_ctx = NULL;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    g_capture_log[0] = '\0';
    rc |= switch_input_from(&opts, &state, DSD_APP_CMD_AIRSPY_ENABLE_INPUT, "capture pcm alone drained");
    rc |= expect_int("capture pcm alone: capture still on", opts.iq_capture_requested, 1);
    rc |= expect_str("capture pcm alone: nothing logged", g_capture_log, "");
    rc |= expect_str("capture pcm alone toast", state.ui_msg, "Failed: the Airspy input did not start (see log)");

    /* A switch that works opens the capture on the new input. */
    opts.audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    opts.iq_capture_requested = 1;
    state.rtl_ctx = NULL;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    rc |= switch_input_from(&opts, &state, DSD_APP_CMD_AIRSPY_ENABLE_INPUT, "capture pcm to airspy drained");
    rc |= expect_int("capture pcm to airspy: the Airspy records", g_config_rtl_create_capture, 1);
    rc |= expect_int("capture pcm to airspy: capture still on", opts.iq_capture_requested, 1);
    rc |= expect_str("capture pcm to airspy toast", state.ui_msg, "Applied: Airspy input enabled");

    reset_config_rtl_wrap();
    opts.iq_capture_requested = 0;
    opts.iq_capture_path[0] = '\0';
    opts.analog_only = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* Submit a DSP bandwidth (DSD_APP_CMD_RTL_SET_BW) of @p khz and drain it, as the decoder does. */
static int
submit_dsp_bandwidth(dsd_opts* opts, dsd_state* state, int khz, const char* label) {
    int rc = expect_int(label, dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_BW, khz), DSD_APP_COMMAND_SUBMIT_QUEUED);
    state->ui_msg[0] = '\0';
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/*
 * Issue #578: a DSP bandwidth change (RTL_SET_BW) reopens the device, and a start that fails anyway puts the bandwidth
 * that ran back and starts the input again, as a failed input switch does, rather than leave the session with no
 * radio input. An Airspy delivers a rate the bandwidth sets, which nothing before the reopen can know: NFM 25 kHz runs
 * at 48 kHz, but the 19,531 Hz of a 12 kHz bandwidth cannot filter it, so that start refuses it and the message says
 * why. An RTL-SDR whose device does not open at the new bandwidth says that instead, and with --iq-capture the restart
 * runs without the capture. The change and its rollback run inside one hold of the watchdog guard. A bandwidth whose
 * start works applies as before.
 */
static int
test_dsp_bandwidth_change_that_fails_keeps_the_running_input(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.analog_only = 1;
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    opts.analog_nfm_bandwidth_hz = 25000;
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
    opts.rtl_dsp_bw_khz = 48;
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    g_config_rtl_refuse_rate_hz = 19531;
    guard_probe_arm(&opts);
    rc |= submit_dsp_bandwidth(&opts, &state, 12, "airspy bw 12");
    guard_probe_disarm();
    rc |= expect_int("airspy bw 12: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_int("airspy bw 12: tried, then restarted", g_config_rtl_creates == 2 && state.rtl_ctx != NULL, 1);
    rc |= expect_int("airspy bw 12: restarted at 48 kHz", g_config_rtl_create_bw_khz, 48);
    rc |= expect_int("airspy bw 12: the bandwidth that ran back", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_str("airspy bw 12: still the Airspy", opts.audio_in_dev, "airspy");
    rc |= expect_toast("airspy bw 12 toast", &state, "Refused: NFM 25 kHz does not fit the 19.531 kHz DSP rate");
    rc |= expect_int("airspy bw 12: one hold", g_guard_probe_holds, 1);
    rc |= expect_int("airspy bw 12: no write outside the guard", g_guard_probe_unguarded, 0);

    /* A DMR session on an RTL-SDR at 48 kHz recording I/Q, whose device does not open at 24 kHz. */
    opts.analog_only = 0;
    opts.analog_nfm_bandwidth_hz = 0;
    opts.frame_dmr = 1;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:48");
    opts.iq_capture_requested = 1;
    DSD_SNPRINTF(opts.iq_capture_path, sizeof opts.iq_capture_path, "%s", "cap.iq");
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    g_capture_log[0] = '\0';
    rc |= submit_dsp_bandwidth(&opts, &state, 24, "rtl bw 24");
    rc |= expect_int("rtl bw 24: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_capture_stopped("rtl bw 24: recording kept", &opts, 0);
    rc |= expect_int("rtl bw 24: the bandwidth that ran back", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_int("rtl bw 24: restarted at 48 kHz", g_config_rtl_create_bw_khz == 48 && state.rtl_ctx != NULL, 1);
    rc |= expect_str("rtl bw 24 toast", state.ui_msg,
                     "Failed: the RTL-SDR input did not start (see log); I/Q capture stopped");

    /* One that starts applies. */
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    rc |= submit_dsp_bandwidth(&opts, &state, 24, "rtl bw 24 starts");
    rc |= expect_int("rtl bw 24 starts: applied", dsd_app_command_test_last_failed(), 0);
    rc |= expect_int("rtl bw 24 starts: at 24 kHz", opts.rtl_dsp_bw_khz == 24 && g_config_rtl_create_bw_khz == 24, 1);
    rc |= expect_str("rtl bw 24 starts toast", state.ui_msg, "Applied: RTL DSP BW -> 24 kHz");

    reset_config_rtl_wrap();
    opts.iq_capture_path[0] = '\0';
    opts.frame_dmr = 0;
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* Submit an Airspy setting (DSD_APP_CMD_AIRSPY_SET) and drain it, as the decoder does. */
static int
submit_airspy_setting(dsd_opts* opts, dsd_state* state, const char* key, const char* value, const char* label) {
    dsd_app_airspy_setting_payload edit;
    DSD_MEMSET(&edit, 0, sizeof edit);
    DSD_SNPRINTF(edit.key, sizeof edit.key, "%s", key);
    DSD_SNPRINTF(edit.value, sizeof edit.value, "%s", value);
    int rc = expect_true(label, dsd_app_command_submit(DSD_APP_CMD_AIRSPY_SET, &edit, sizeof edit) > 0);
    state->ui_msg[0] = '\0';
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/*
 * Issue #578: an Airspy setting the running Airspy takes only when it opens (its sample rate) reopens it, and a start
 * that fails puts the settings it replaced back and starts them again without the I/Q capture, as every rollback
 * restart does. The capture stays off for the rest of the session, so the message says so beside the failure, as the
 * config and Switch source rollbacks do. A setting that reopens and starts records on.
 */
static int
test_airspy_setting_rollback_reports_the_iq_capture_stop(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
    dsd_airspy_config_defaults(&opts.airspy);
    const uint32_t rate_before = opts.airspy.sample_rate;
    opts.rtl_dsp_bw_khz = 48;
    opts.iq_capture_requested = 1;
    DSD_SNPRINTF(opts.iq_capture_path, sizeof opts.iq_capture_path, "%s", "cap.iq");
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    g_capture_log[0] = '\0';
    rc |= submit_airspy_setting(&opts, &state, "airspy_sample_rate", "2500000", "airspy rate fails");
    rc |= expect_int("airspy rate fails: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_capture_stopped("airspy rate fails: recording kept", &opts, 0);
    rc |= expect_int("airspy rate fails: the rate put back", (int)opts.airspy.sample_rate, (int)rate_before);
    rc |= expect_int("airspy rate fails: the Airspy runs", state.rtl_ctx != NULL, 1);
    rc |= expect_str("airspy rate fails toast", state.ui_msg, "Failed: Airspy setting; I/Q capture stopped");

    /* One that starts records on, and says only that it applied. */
    opts.iq_capture_requested = 1;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    rc |= submit_airspy_setting(&opts, &state, "airspy_sample_rate", "2500000", "airspy rate starts");
    rc |=
        expect_int("airspy rate starts: records on", g_config_rtl_create_capture == 1 && opts.iq_capture_requested, 1);
    rc |= expect_str("airspy rate starts toast", state.ui_msg, "Applied: Airspy setting");

    reset_config_rtl_wrap();
    opts.iq_capture_requested = 0;
    opts.iq_capture_path[0] = '\0';
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

/* The input failure the session has latched (dsd_input_failure_get()) is of @p kind. */
static int
expect_input_failure(const char* label, int kind) {
    dsd_input_failure failure;
    dsd_input_failure_get(&failure);
    return expect_int(label, failure.kind, kind);
}

/*
 * Issue #578: a start that fails on the input that replaced the one that ran can latch a failure of that input for the
 * session: an Airspy that does not open latches a device failure, which turns the session's normal end into a failure
 * exit (dsd_engine_run_with_lifecycle()). A rollback that has the input that ran running again puts back the failure
 * the session had latched before the change, whatever that input is: a PCM input only put back, or an RTL-SDR or
 * SoapySDR input started again, whose start neither latches nor clears one. Input > Switch source and a config apply's
 * reopen both do, and so do a DSP bandwidth change and an Airspy setting, whose Airspy opens again; a failure latched
 * before the change (an rtl_tcp server that refused an earlier connect) stands as it was. A failure the restarted input
 * latches itself stands too: an Airspy whose device stops at once, which its monitor thread latches before the start
 * returns. A rollback whose restart fails as well leaves the session with no input, and the failure latched stands,
 * even one the restart latched that reads as the failure put back.
 */
static int
test_rollback_puts_back_the_input_failure(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    dsd_airspy_config_defaults(&opts.airspy);
    opts.rtl_dsp_bw_khz = 48;

    /* PCM switched to an Airspy that does not open: PCM is put back, and so is the latch. */
    opts.audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    state.rtl_ctx = NULL;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    rc |= switch_input_from(&opts, &state, DSD_APP_CMD_AIRSPY_ENABLE_INPUT, "latch pcm to airspy drained");
    rc |= expect_str("latch pcm to airspy: still PCM", opts.audio_in_dev, "pulse");
    rc |= expect_str("latch pcm to airspy toast", state.ui_msg, "Failed: the Airspy input did not start (see log)");
    rc |= expect_input_failure("latch pcm to airspy: nothing latched", DSD_INPUT_FAILURE_NONE);

    /* ... with a failure latched before the switch: that one stands. */
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    dsd_input_failure_report(DSD_INPUT_FAILURE_REFUSED, 111);
    rc |= switch_input_from(&opts, &state, DSD_APP_CMD_AIRSPY_ENABLE_INPUT, "latch pcm to airspy, earlier drained");
    rc |= expect_input_failure("latch pcm to airspy, earlier: the earlier one", DSD_INPUT_FAILURE_REFUSED);

    /* A running RTL-SDR or SoapySDR input switched to that Airspy: started again. */
    static const char* const devs[] = {"rtl:0:851.375M:0:0:48", "soapy:driver=rtlsdr"};
    for (size_t i = 0; i < sizeof devs / sizeof devs[0]; ++i) {
        opts.audio_in_type = AUDIO_IN_RTL;
        DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", devs[i]);
        state.rtl_ctx = (RtlSdrContext*)fake_ctx;
        reset_config_rtl_wrap();
        g_config_rtl_open_ok = 1;
        g_config_rtl_fail_starts = 1;
        rc |= switch_input_from(&opts, &state, DSD_APP_CMD_AIRSPY_ENABLE_INPUT, devs[i]);
        rc |= expect_int(devs[i], g_config_rtl_creates == 2 && state.rtl_ctx != NULL, 1);
        rc |= expect_str(devs[i], opts.audio_in_dev, devs[i]);
        rc |= expect_input_failure(devs[i], DSD_INPUT_FAILURE_NONE);
    }

    /* A config whose [input] opens that Airspy over the running RTL-SDR: the RTL-SDR runs again. */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:0:0:48");
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_AIRSPY, 0, 0, "latch cfg airspy over rtl");
    rc |= expect_int("latch cfg airspy over rtl: failed", dsd_app_command_test_last_failed(), 1);
    rc |= expect_str("latch cfg airspy over rtl: RTL-SDR back", opts.audio_in_dev, "rtl:0:851.375M:0:0:48");
    rc |= expect_int("latch cfg airspy over rtl: running", g_config_rtl_creates == 2 && state.rtl_ctx != NULL, 1);
    rc |= expect_input_failure("latch cfg airspy over rtl: nothing latched", DSD_INPUT_FAILURE_NONE);

    /* A running Airspy whose DSP bandwidth, or sample rate, reopens it on a start that does not open: the Airspy that
       ran opens again. */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
    state.rtl_ctx = (RtlSdrContext*)fake_ctx;
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    rc |= submit_dsp_bandwidth(&opts, &state, 24, "latch airspy bw 24");
    rc |= expect_int("latch airspy bw 24: back at 48 kHz", opts.rtl_dsp_bw_khz == 48 && state.rtl_ctx != NULL, 1);
    rc |= expect_input_failure("latch airspy bw 24: nothing latched", DSD_INPUT_FAILURE_NONE);
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 1;
    rc |= submit_airspy_setting(&opts, &state, "airspy_sample_rate", "2500000", "latch airspy rate");
    rc |= expect_int("latch airspy rate: running", g_config_rtl_creates == 2 && state.rtl_ctx != NULL, 1);
    rc |= expect_input_failure("latch airspy rate: nothing latched", DSD_INPUT_FAILURE_NONE);

    /* The Airspy that ran, opened again by each rollback (a DSP bandwidth change, an Airspy setting, a switch to an
       RTL-SDR and a config's SoapySDR [input], none of which start), stops at once: its monitor thread latches a device
       failure before the restart returns. That is the session's failure, and it stands. */
    for (int caller = 0; caller < 4; ++caller) {
        static const char* const labels[] = {"latch airspy bw 24, lost again", "latch airspy rate, lost again",
                                             "latch airspy to rtl, lost again",
                                             "latch cfg soapy over airspy, lost again"};
        const char* label = labels[caller];
        DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
        opts.audio_in_type = AUDIO_IN_RTL;
        opts.rtl_dsp_bw_khz = 48;
        state.rtl_ctx = (RtlSdrContext*)fake_ctx;
        reset_config_rtl_wrap();
        g_config_rtl_open_ok = 1;
        g_config_rtl_fail_starts = 1;
        g_config_rtl_airspy_stops_after_open = 1;
        if (caller == 0) {
            rc |= submit_dsp_bandwidth(&opts, &state, 24, label);
        } else if (caller == 1) {
            rc |= submit_airspy_setting(&opts, &state, "airspy_sample_rate", "2500000", label);
        } else if (caller == 2) {
            rc |= switch_input_from(&opts, &state, DSD_APP_CMD_RTL_ENABLE_INPUT, label);
        } else {
            rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_SOAPY, 0, 0, label);
        }
        rc |= expect_str(label, opts.audio_in_dev, "airspy");
        rc |= expect_int(label, g_config_rtl_creates == 2 && g_config_rtl_airspy_stops_after_open == 0, 1);
        rc |= expect_input_failure(label, DSD_INPUT_FAILURE_DEVICE);
    }

    /* The Airspy switched to an RTL-SDR that does not open, and its own restart does not open either: no input runs,
       and the failure the Airspy latched stands. */
    reset_config_rtl_wrap();
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 2;
    rc |= switch_input_from(&opts, &state, DSD_APP_CMD_RTL_ENABLE_INPUT, "latch airspy to rtl, both fail drained");
    rc |= expect_int("latch airspy to rtl, both fail: no input", state.rtl_ctx == NULL, 1);
    rc |= expect_input_failure("latch airspy to rtl, both fail: the Airspy's", DSD_INPUT_FAILURE_DEVICE);

    /* A device failure latched before a change that reopens the Airspy that ran (a DSP bandwidth change, an Airspy
       setting): the Airspy opens, which clears the latch, and then fails, and the Airspy that ran does not open again,
       latching that same failure itself. No input runs, and that failure stands though it reads as the one put back. */
    for (int caller = 0; caller < 2; ++caller) {
        const char* label = caller ? "latch airspy rate, fails as before" : "latch airspy bw 24, fails as before";
        DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
        opts.audio_in_type = AUDIO_IN_RTL;
        opts.rtl_dsp_bw_khz = 48;
        state.rtl_ctx = (RtlSdrContext*)fake_ctx;
        reset_config_rtl_wrap();
        g_config_rtl_open_ok = 1;
        g_config_rtl_fail_starts = 2;
        g_config_rtl_next_fail_opens = 1;
        dsd_input_failure_report(DSD_INPUT_FAILURE_DEVICE, -5);
        if (caller == 0) {
            rc |= submit_dsp_bandwidth(&opts, &state, 24, label);
        } else {
            rc |= submit_airspy_setting(&opts, &state, "airspy_sample_rate", "2500000", label);
        }
        rc |= expect_int(label, g_config_rtl_starts == 2 && g_config_rtl_next_fail_opens == 0, 1);
        rc |= expect_int(label, state.rtl_ctx == NULL, 1);
        rc |= expect_input_failure(label, DSD_INPUT_FAILURE_DEVICE);
    }

    reset_config_rtl_wrap();
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

#if defined(DSD_NEO_TEST_ANALOG_WRAP) && defined(DSD_NEO_TEST_AUDIO_ENSURE_WRAP)
/*
 * Issue #578: a DMR-configured scanner on @p dev at a 24 kHz DSP bandwidth, with a P25 row on air (@p row; 0: the
 * configured DMR, no row) that runs its own modulation (@p row_cqpsk: CQPSK, else C4FM), which the decoder runs
 * (state->rf_mod) and the stream ran, the row's tune having applied it whatever DSD_NEO_CQPSK says. Every stream a
 * start opens here runs the CQPSK state an open picks, DSD_NEO_CQPSK's (@p cqpsk_env; -1, unset, as 0 for the settings
 * here): CQPSK, whose output is the 24 kHz demod rate, or the FSK discriminator resampled to 48 kHz.
 */
static void
init_row_restart_session(dsd_opts* opts, dsd_state* state, RtlSdrContext* fake_ctx, const char* dev, int row,
                         int row_cqpsk, int cqpsk_env) {
    init_dmr_session_with_nfm_width(opts, state, fake_ctx, 0);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", dev);
    opts->rtl_dsp_bw_khz = 24;
    dsd_airspy_config_defaults(&opts->airspy);
    if (row) {
        (void)dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_P25);
        (void)dsd_scan_mode_options(opts, state, NULL);
        const dsd_scan_modulation modulation = row_cqpsk ? DSD_SCAN_MODULATION_CQPSK : DSD_SCAN_MODULATION_C4FM;
        dsd_scan_mode_target_modulation(state, modulation);
        dsd_scan_mode_apply_modulation(opts, DSD_SCAN_MODE_P25, modulation);
    }
    state->rf_mod = row ? row_cqpsk : 0;
    /* The decoder reads a CQPSK row's symbol-rate output, and a C4FM row's samples at the 24 kHz the stream ran. */
    state->samplesPerSymbol = (row && row_cqpsk) ? 1 : 5;
    state->symbolCenter = 0;
    reset_rx_family_wrap();
    g_config_rtl_open_ok = 1;
    g_fake_cqpsk_env = cqpsk_env;
    g_fake_cqpsk = cqpsk_env > 0 ? 1 : 0;
    g_fake_analog_family = 0;
    g_fake_output_rate_hz = cqpsk_env > 0 ? 24000U : 48000U;
    g_fake_request_rate_hz = 24000;
}

static void
finish_row_restart_session(dsd_opts* opts, dsd_state* state) {
    dsd_scan_mode_leave(opts, state);
    reset_rx_family_wrap();
    g_fake_cqpsk = 0;
    g_fake_output_rate_hz = 0U;
    state->rtl_ctx = NULL;
    freeState(state);
}

/* Submit @p command, one that restarts the radio stream the options describe, and drain it: Input > Switch source,
   a DSP bandwidth of @p khz, an Airspy sample rate, an explicit restart, a gain or a device index. */
static int
submit_stream_restart(dsd_opts* opts, dsd_state* state, int command, int khz, const char* label) {
    switch (command) {
        case DSD_APP_CMD_RTL_ENABLE_INPUT:
        case DSD_APP_CMD_AIRSPY_ENABLE_INPUT: return switch_input_from(opts, state, command, label);
        case DSD_APP_CMD_RTL_SET_BW: return submit_dsp_bandwidth(opts, state, khz, label);
        case DSD_APP_CMD_AIRSPY_SET: return submit_airspy_setting(opts, state, "airspy_sample_rate", "2500000", label);
        default: break;
    }
    int rc = expect_true(label, (command == DSD_APP_CMD_RTL_RESTART ? post_empty(command) : post_i32(command, 1)) > 0);
    state->ui_msg[0] = '\0';
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/* The row's profile, a P25 one of @p cqpsk, was asked of the stream running now, timed for @p sps samples per symbol,
   and the row is still on air with that modulation. */
static int
expect_row_republished(const char* label, const dsd_state* state, int cqpsk, int sps) {
    int rc = expect_int(label, dsd_scan_mode_active(state) == DSD_SCAN_MODE_P25 && state->rf_mod == cqpsk, 1);
    rc |= expect_int(label, g_demod_req_calls >= 1 && g_demod_req_cqpsk == cqpsk && g_demod_req_rate == 4800, 1);
    rc |= expect_int(label, g_demod_req_ted_sps, sps);
    rc |= expect_int(label, state->samplesPerSymbol, sps);
    rc |= expect_int(label, g_analog_req_calls == 0 || g_analog_req_family == DSD_RX_FAMILY_DIGITAL, 1);
    return rc;
}

/*
 * Issue #578: Input > Switch source, a DSP bandwidth change and an Airspy setting leave a scan row's scope in force, so
 * a stream they start opens on the row's settings, but with the CQPSK state an open picks, which DSD_NEO_CQPSK decides
 * when set, while the row's tune applied the row's own. When the start fails, the rollback restarts the input that ran
 * the same way: under DSD_NEO_CQPSK=1 a P25 C4FM row the stream ran on the FSK discriminator comes back on CQPSK, and
 * under DSD_NEO_CQPSK=0 a CQPSK row comes back on the FSK discriminator. The input is back, but it would not run the
 * row until another profile request, so the row's profile is asked of the restarted stream, as a scoped command's
 * resume asks it of a stream it started, and timed for the rate that stream runs it at: a C4FM row at the 24 kHz the
 * CQPSK open delivers (5 samples per 4800 Bd symbol), a CQPSK row at the 24 kHz demod rate its timing loop runs at, not
 * the 48 kHz of the FSK output (10). Nothing is reset as for a new input. With no scan row the rollback asks nothing,
 * as before.
 */
static int
test_rollback_restart_republishes_the_scan_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];

    static const struct {
        const char* label;
        const char* dev;
        const char* toast;
        int command;
        int row;
        int row_cqpsk;
        int cqpsk_env;
    } legs[] = {
        {"airspy switch fails, c4fm row, DSD_NEO_CQPSK=1", "rtl:0:851.375M:0:0:24",
         "Failed: the Airspy input did not start (see log)", DSD_APP_CMD_AIRSPY_ENABLE_INPUT, 1, 0, 1},
        {"dsp bw fails, c4fm row, DSD_NEO_CQPSK=1", "rtl:0:851.375M:0:0:24",
         "Failed: the RTL-SDR input did not start (see log)", DSD_APP_CMD_RTL_SET_BW, 1, 0, 1},
        {"airspy setting fails, c4fm row, DSD_NEO_CQPSK=1", "airspy", "Failed: Airspy setting", DSD_APP_CMD_AIRSPY_SET,
         1, 0, 1},
        {"rtl switch fails, cqpsk row, DSD_NEO_CQPSK=0", "airspy", "Failed: the RTL-SDR input did not start (see log)",
         DSD_APP_CMD_RTL_ENABLE_INPUT, 1, 1, 0},
        {"dsp bw fails, cqpsk row, DSD_NEO_CQPSK=0", "rtl:0:851.375M:0:0:24",
         "Failed: the RTL-SDR input did not start (see log)", DSD_APP_CMD_RTL_SET_BW, 1, 1, 0},
        {"airspy setting fails, cqpsk row, DSD_NEO_CQPSK=0", "airspy", "Failed: Airspy setting", DSD_APP_CMD_AIRSPY_SET,
         1, 1, 0},
        {"dsp bw fails, no scan row", "rtl:0:851.375M:0:0:24", "Failed: the RTL-SDR input did not start (see log)",
         DSD_APP_CMD_RTL_SET_BW, 0, 0, 1},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof legs / sizeof legs[0]; ++i) {
        const char* label = legs[i].label;
        init_row_restart_session(&opts, &state, (RtlSdrContext*)fake_ctx, legs[i].dev, legs[i].row, legs[i].row_cqpsk,
                                 legs[i].cqpsk_env);
        g_config_rtl_fail_starts = 1; /* the new device, bandwidth or rate does not start */
        const uint32_t generation = state.analog_rx.generation;
        rc |= submit_stream_restart(&opts, &state, legs[i].command, 12, label);
        rc |= expect_int(label, dsd_app_command_test_last_failed(), 1);
        rc |= expect_int(label, g_config_rtl_creates == 2 && g_config_rtl_starts == 2 && state.rtl_ctx != NULL, 1);
        rc |= expect_str(label, g_config_rtl_create_dev, legs[i].dev);
        rc |= expect_str(label, opts.audio_in_dev, legs[i].dev);
        rc |= expect_int(label, opts.rtl_dsp_bw_khz, 24);
        rc |= expect_str(label, state.ui_msg, legs[i].toast);
        if (legs[i].command == DSD_APP_CMD_AIRSPY_ENABLE_INPUT || legs[i].command == DSD_APP_CMD_RTL_ENABLE_INPUT) {
            rc |= expect_received_tone_kept(label, &state, generation);
        }
        if (legs[i].row) {
            rc |= expect_row_republished(label, &state, legs[i].row_cqpsk, 5);
        } else {
            rc |= expect_int(label, g_demod_req_calls == 0 && g_analog_req_calls == 0, 1);
        }
        finish_row_restart_session(&opts, &state);
    }
    return rc;
}

/*
 * Issue #578: the same rollback under an analog row. The restart opens on the row's monitor, as the stream it replaced
 * ran it (the options in force are the row's), and the row's profile asked of it is that monitor: an nfm row's own
 * 12.5 kHz, an am row's own 20 kHz, and no symbol profile.
 */
static int
test_rollback_restart_republishes_an_analog_scan_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];
    static const char* const labels[] = {"airspy switch fails, nfm row", "airspy switch fails, am row"};
    int rc = 0;
    for (int am = 0; am <= 1; ++am) {
        const char* label = labels[am];
        const int kind = am ? DSD_ANALOG_DEMOD_AM : DSD_ANALOG_DEMOD_FM;
        const int width_hz = am ? 20000 : 12500;
        init_row_restart_session(&opts, &state, (RtlSdrContext*)fake_ctx, "rtl:0:851.375M:0:0:24", 0, 0, -1);
        dsd_scan_option_values row = {0};
        row.present = DSD_SCAN_OPT_BANDWIDTH;
        row.channel_bw_hz = width_hz;
        row.channel_bw_kind = kind;
        rc |= expect_int(label, dsd_scan_mode_enter(&opts, &state, am ? DSD_SCAN_MODE_AM : DSD_SCAN_MODE_NFM), 0);
        rc |= expect_int(label, dsd_scan_mode_options(&opts, &state, &row), 0);
        g_fake_analog_family = 1; /* the front end runs the row's monitor */
        g_fake_output_rate_hz = 48000U;
        g_config_rtl_fail_starts = 1;
        const uint32_t generation = state.analog_rx.generation;
        rc |= switch_input_from(&opts, &state, DSD_APP_CMD_AIRSPY_ENABLE_INPUT, label);
        rc |= expect_int(label, dsd_app_command_test_last_failed(), 1);
        rc |= expect_restarted_on(label, &opts, &state, "rtl:0:851.375M:0:0:24");
        rc |= expect_int(label,
                         g_config_rtl_create_analog_only == 1 && g_config_rtl_create_kind == kind
                             && g_config_rtl_create_width_hz == width_hz,
                         1);
        rc |= expect_received_tone_kept(label, &state, generation);
        rc |= expect_int(label, dsd_scan_mode_active(&state) == (am ? DSD_SCAN_MODE_AM : DSD_SCAN_MODE_NFM), 1);
        rc |= expect_last_monitor_request(label, 1, kind, width_hz);
        rc |= expect_int(label, g_demod_req_calls, 0);
        finish_row_restart_session(&opts, &state);
    }
    return rc;
}

/*
 * Issue #578: a stream such a command starts that works opens the same way, on the row's settings with the CQPSK state
 * the open picks, and is asked for the row's profile the same way, timed for the rate it runs the row at. Under
 * DSD_NEO_CQPSK=1 a P25 C4FM row runs the FSK discriminator on the new Airspy, the RTL-SDR switched back to, the RTL-SDR
 * reopened at a 48 kHz DSP bandwidth (10 samples per symbol at the 48 kHz CQPSK output), the Airspy reopened at a new
 * sample rate, and the RTL-SDR an explicit restart, a gain or a device index reopens. With no scan row a restart asks
 * nothing, as before.
 */
static int
test_restart_under_a_scan_row_republishes_the_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static void* fake_ctx[2];

    static const struct {
        const char* label;
        const char* dev;
        const char* dev_after;
        int command;
        int khz;
        int row;
    } legs[] = {
        {"airspy switch", "rtl:0:851.375M:0:0:24", "airspy", DSD_APP_CMD_AIRSPY_ENABLE_INPUT, 24, 1},
        {"rtl switch", "airspy", "rtl", DSD_APP_CMD_RTL_ENABLE_INPUT, 24, 1},
        {"dsp bw 48", "rtl:0:851.375M:0:0:24", "rtl:0:851.375M:0:0:24", DSD_APP_CMD_RTL_SET_BW, 48, 1},
        {"airspy setting", "airspy", "airspy", DSD_APP_CMD_AIRSPY_SET, 24, 1},
        {"explicit restart", "rtl:0:851.375M:0:0:24", "rtl:0:851.375M:0:0:24", DSD_APP_CMD_RTL_RESTART, 24, 1},
        {"gain", "rtl:0:851.375M:0:0:24", "rtl:0:851.375M:0:0:24", DSD_APP_CMD_RTL_SET_GAIN, 24, 1},
        {"device index", "rtl:0:851.375M:0:0:24", "rtl:0:851.375M:0:0:24", DSD_APP_CMD_RTL_SET_DEV, 24, 1},
        {"dsp bw 48, no scan row", "rtl:0:851.375M:0:0:24", "rtl:0:851.375M:0:0:24", DSD_APP_CMD_RTL_SET_BW, 48, 0},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof legs / sizeof legs[0]; ++i) {
        char label[96];
        DSD_SNPRINTF(label, sizeof label, "%s that starts, c4fm row, DSD_NEO_CQPSK=1", legs[i].label);
        init_row_restart_session(&opts, &state, (RtlSdrContext*)fake_ctx, legs[i].dev, legs[i].row, 0, 1);
        g_fake_output_rate_hz = (unsigned int)legs[i].khz * 1000U; /* the new CQPSK stream's output */
        rc |= submit_stream_restart(&opts, &state, legs[i].command, legs[i].khz, label);
        rc |= expect_int(label, dsd_app_command_test_last_failed(), 0);
        rc |= expect_int(label, g_config_rtl_creates == 1 && state.rtl_ctx != NULL, 1);
        rc |= expect_str(label, opts.audio_in_dev, legs[i].dev_after);
        if (legs[i].row) {
            rc |= expect_row_republished(label, &state, 0, legs[i].khz == 48 ? 10 : 5);
        } else {
            rc |= expect_int(label, g_demod_req_calls == 0 && g_analog_req_calls == 0, 1);
        }
        finish_row_restart_session(&opts, &state);
    }
    return rc;
}
#endif

#endif

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
static int g_dmr_policy_returns;

static dsd_trunk_tune_result
dmr_policy_return(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    ++g_dmr_policy_returns;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static int
test_dmr_policy_command_ticks_owner(int command) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    opts.trunk_enable = opts.trunk_is_tuned = opts.frame_dmr = 1;
    opts.audio_in_type = AUDIO_IN_NULL;
    opts.audio_out_type = 9;
    state.synctype = state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.trunk_cc_freq = 451000000;
    dsd_trunk_recovery_note_protocol(&state, DSD_TRUNK_RECOVERY_DMR);
    dsd_trunk_tuning_requests_reset();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.return_to_cc_request = dmr_policy_return});
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    dmr_sm_init_ctx(ctx, &opts, &state);
    ctx->state = DMR_SM_TUNED;
    ctx->vc_freq_hz = 452000000;
    ctx->t_tune_m = dsd_decode_now_mono_s() - ctx->grant_timeout_s - 1.0;
    const dsd_call_observation call = {.protocol = DSD_SYNC_DMR_BS_VOICE_POS,
                                       .slot = 0,
                                       .kind = DSD_CALL_KIND_GROUP_VOICE,
                                       .ota_target_id = 1234,
                                       .policy_target_id = 1234,
                                       .ota_source_id = 42,
                                       .frequency_hz = 452000000,
                                       .observed_m = dsd_decode_now_mono_s()};
    int rc = expect_true("expired DMR call seeded", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) > 0);
    g_dmr_policy_returns = 0;
    rc |= expect_true("DMR policy command queued", dsd_app_command_set_u8(command, 0) > 0);
    rc |= expect_int("DMR policy command drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("DMR deadline tick returned to CC", g_dmr_policy_returns, 1);
    rc |= expect_int("DMR deadline tick leaves TUNED", dmr_sm_get_state(ctx), DMR_SM_ON_CC);
    rc |= expect_true("DMR deadline tick starts acquisition", ctx->cc_acquiring && ctx->cc_tune_request_id == 0U);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_trunk_tuning_requests_reset();
    dmr_sm_init_ctx(ctx, NULL, NULL);
    freeState(&state);
    return rc;
}
#endif

static int
test_config_group_path_reloads_before_persist(int scoped) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    char first[DSD_TEST_PATH_MAX], second[DSD_TEST_PATH_MAX], missing[DSD_TEST_PATH_MAX];
    const int first_fd = dsd_test_mkstemp(first, sizeof first, "dsd_config_groups_a");
    const int second_fd = dsd_test_mkstemp(second, sizeof second, "dsd_config_groups_b");
    const int missing_fd = dsd_test_mkstemp(missing, sizeof missing, "dsd_config_groups_missing");
    if (first_fd < 0 || second_fd < 0 || missing_fd < 0) {
        return 1;
    }
    dsd_close(first_fd);
    dsd_close(second_fd);
    dsd_close(missing_fd);
    remove(missing);
    const char* a = "id,mode,name\n111,A,First\n";
    const char* b = "id,mode,name\n222,A,Second\n";
    int rc = write_file_bytes(first, a, strlen(a));
    rc |= write_file_bytes(second, b, strlen(b));
    DSD_SNPRINTF(opts.group_in_file, sizeof opts.group_in_file, "%s", first);
    rc |= expect_int("config seed first groups", dsd_tg_policy_reload_group_file(&opts, &state), 0);
    rc |= expect_int("config seed old avoid", dsd_tg_policy_session_avoid_add(&state, 111), 0);
    dsd_scan_row_profile* row = NULL;
    dsd_key_set keys = {0};
    if (scoped) {
        dsd_scan_options parsed = {0};
        parsed.values.present = DSD_SCAN_OPT_GROUP;
        DSD_SNPRINTF(parsed.values.group_file, sizeof parsed.values.group_file, "%s", second);
        rc |= expect_int("config load row groups", dsd_scan_profile_load(&parsed, 0, &row, &keys), 0);
        rc |= expect_int("config begin row scope", dsd_scan_groups_begin(&state), 0);
        dsd_scan_groups_enter(&state, row);
        rc |= expect_int("config seed row avoid", dsd_tg_policy_session_avoid_add(&state, 777), 0);
    }
    dsdneoUserConfig cfg = {0};
    cfg.has_trunking = 1;
    cfg.trunk_persist_tg_lockouts = 1;
    DSD_SNPRINTF(cfg.trunk_group_csv, sizeof cfg.trunk_group_csv, "%s", second);
    rc |= expect_true("config new groups queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("config new groups drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("config new groups path", opts.group_in_file, second);
    if (scoped) {
        rc |= expect_true("config preserves active row avoid", dsd_tg_policy_session_avoid_contains(&state, 777));
        dsd_scan_groups_leave(&state);
    }
    rc |= expect_int("config resets old avoids", (int)dsd_tg_policy_session_avoid_count(&state, 0, UINT32_MAX), 0);
    dsd_app_tg_listen_payload edit = {222, 222, 0};
    rc |= expect_true("config persisted edit queued", dsd_app_command_set_tg_listen(&edit) > 0);
    rc |= expect_int("config persisted edit drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_file_bytes(first, a);
    rc |= expect_file_bytes(second, "id,mode,name\n222,B,Second\n");

    rc |= expect_int("config seed retained avoid", dsd_tg_policy_session_avoid_add(&state, 222), 0);
    rc |= expect_true("config unchanged groups queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("config unchanged groups drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("config unchanged path retains avoid", dsd_tg_policy_session_avoid_contains(&state, 222));
    DSD_SNPRINTF(cfg.trunk_group_csv, sizeof cfg.trunk_group_csv, "%s", missing);
    cfg.trunk_persist_tg_lockouts = 0;
    rc |= expect_true("config missing groups queued", dsd_app_command_apply_config(&cfg) > 0);
    rc |= expect_int("config missing groups drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("config failure retains groups path", opts.group_in_file, second);
    rc |= expect_true("config failure retains avoid", dsd_tg_policy_session_avoid_contains(&state, 222));
    rc |= expect_int("config failure does not apply remaining options", opts.persist_tg_lockouts, 1);
    rc |= write_file_bytes(missing, a, strlen(a));
    edit.listen = 1;
    rc |= expect_true("config edit after failure queued", dsd_app_command_set_tg_listen(&edit) > 0);
    rc |= expect_int("config edit after failure drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_file_bytes(missing, a);
    rc |= expect_file_bytes(second, b);
    // A failed explicit import must preserve the same table/path pairing as a failed config load.
    remove(missing);
    post_string(DSD_APP_CMD_IMPORT_GROUP_LIST, missing);
    rc |= expect_int("explicit missing import drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("explicit missing import keeps destination", opts.group_in_file, second);
    rc |= expect_true("explicit missing import keeps avoid", dsd_tg_policy_session_avoid_contains(&state, 222));
    dsd_scan_profile_free(row);
    dsd_key_set_free(&keys);
    freeState(&state);
    remove(first);
    remove(second);
    remove(missing);
    return rc;
}

/*
 * AM on a PCM input (issue #524): the audio arrives demodulated, so DECODE_MODE_SET AM is refused with the reason and
 * the -fA alternative, and the session keeps its mode; a config whose [mode] is am applies with the Analog monitor in
 * its place. The preset ids end at AM.
 */
static int
test_am_refused_on_pcm_input(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    init_decode_mode_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_PULSE;
    rc |= expect_int("pcm am: dmr queued", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, DSDCFG_MODE_DMR),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("pcm am: dmr drained", dsd_app_drain_cmds(&opts, &state), 1);

    state.ui_msg[0] = '\0';
    rc |= expect_int("pcm am: am queued", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, DSDCFG_MODE_AM),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("pcm am: am drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("pcm am: still DMR", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_DMR, 1);
    rc |= expect_int("pcm am: detector left alone", opts.analog_demod, DSD_ANALOG_DEMOD_FM);
    rc |= expect_int("pcm am: toast gives the reason",
                     strstr(state.ui_msg, "AM demodulation needs an IQ radio input") != NULL, 1);

    /* A config's decode = am applies, and the session falls back to the Analog monitor with the reason, as a start
     * with it does: a shared config must not break a PCM session, nor be refused whole for it. */
    state.ui_msg[0] = '\0';
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_AM, "pcm am: config am");
    rc |=
        expect_int("pcm am: config fell back to Analog", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_ANALOG, 1);
    rc |= expect_int("pcm am: config detector is FM", opts.analog_demod, DSD_ANALOG_DEMOD_FM);
    rc |= expect_int("pcm am: config toast",
                     strstr(state.ui_msg, "Decoding Analog: AM demodulation needs an IQ radio input") != NULL, 1);

    /* Config > Load makes the loaded file the autosave target, then applies it. Its decode = am must survive the
     * session, as it does a start with it: autosave is turned off, with a toast, rather than writing the Analog
     * fallback over it when the session ends. */
    dsd_app_config_metadata_payload meta;
    DSD_MEMSET(&meta, 0, sizeof meta);
    meta.autosave_enabled = 1;
    DSD_SNPRINTF(meta.path, sizeof meta.path, "%s", "shared-am.ini");
    rc |= expect_int("pcm am: load metadata queued",
                     dsd_app_command_submit(DSD_APP_CMD_CONFIG_METADATA_SET, &meta, sizeof meta),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("pcm am: load metadata drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("pcm am: autosave on for the loaded file", state.config_autosave_enabled, 1);
    state.ui_msg[0] = '\0';
    rc |= submit_config_mode(&opts, &state, DSDCFG_MODE_AM, "pcm am: loaded config am");
    rc |= expect_int("pcm am: loaded config fell back to Analog",
                     dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_ANALOG, 1);
    rc |= expect_int("pcm am: loaded config turns autosave off", state.config_autosave_enabled, 0);
    rc |= expect_str("pcm am: the autosave path stays", state.config_autosave_path, "shared-am.ini");
    rc |= expect_int("pcm am: loaded config toast says why",
                     strstr(state.ui_msg, "Autosave is off this session to keep decode = am") != NULL, 1);
    state.config_autosave_enabled = 0;
    state.config_autosave_path[0] = '\0';

    /* Analog stays available on PCM: it monitors the audio as it arrives. */
    rc |= expect_int("pcm analog queued", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, DSDCFG_MODE_ANALOG),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("pcm analog drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("pcm analog applied", dsd_infer_decode_mode_preset(&opts) == DSDCFG_MODE_ANALOG, 1);

    /* AM is the last preset id: one past it is not a mode. */
    state.ui_msg[0] = '\0';
    rc |= expect_int("past-am queued", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, DSDCFG_MODE_AM + 1),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("past-am drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("past-am refused", strstr(state.ui_msg, "Decode mode not available") != NULL, 1);
    freeState(&state);
    return rc;
}

/* The AM width command stores whole Hz from 5000 to 20000, or 0 for the default, and refuses anything else with the
 * range, as the NFM one does; outside the AM preset it is only configuration, used when AM runs. */
static int
test_am_bandwidth_set_validates(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    init_decode_mode_context(&opts, &state);

    const struct {
        int32_t hz;
        int want_hz;
        const char* toast;
    } cases[] = {
        {8000, 8000, "Applied: AM bandwidth -> 8 kHz"},
        {25000, 8000, "Refused: AM bandwidth 25 kHz is outside 5 kHz to 20 kHz"},
        {4999, 8000, "Refused: AM bandwidth 4.999 kHz is outside 5 kHz to 20 kHz"},
        {-1, 8000, "Refused: AM bandwidth -1 Hz is outside 5 kHz to 20 kHz"},
        {5000, 5000, "Applied: AM bandwidth -> 5 kHz"},
        {0, 0, "Applied: AM bandwidth -> default"},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        state.ui_msg[0] = '\0';
        rc |= expect_int("am width queued", dsd_app_command_set_i32(DSD_APP_CMD_AM_BANDWIDTH_SET, cases[i].hz),
                         DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int("am width drained", dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(cases[i].toast, opts.analog_am_bandwidth_hz, cases[i].want_hz);
        rc |= expect_int(cases[i].toast, strstr(state.ui_msg, cases[i].toast) != NULL, 1);
    }
    freeState(&state);
    return rc;
}

/* --- Issue #518: "this channel" session edits (DSD_APP_CMD_SCAN_ROW_EDIT) --- */

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
/* Submit a session edit of the row @p view names and drain it. */
static int
submit_scan_row_edit(dsd_opts* opts, dsd_state* state, const dsd_app_scan_row_view* view, uint32_t field, int action,
                     const dsd_app_scan_row_edit_payload* values, const char* tag) {
    dsd_app_scan_row_edit_payload p;
    int rc = expect_int(tag, dsd_app_scan_row_view_payload(view, field, action, &p), 0);
    if (values) {
        p.squelch_db = values->squelch_db;
        p.width_hz = values->width_hz;
        p.tone_mode = values->tone_mode;
        p.gain_db = values->gain_db;
        DSD_SNPRINTF(p.tone_list, sizeof p.tone_list, "%s", values->tone_list);
    }
    state->ui_msg[0] = '\0';
    rc |= expect_true(tag, dsd_app_command_scan_row_edit(&p) > 0);
    rc |= expect_int(tag, dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/* Under --trunk-scan: a squelch and a gain edit of the parked target run at once (the gain through a stream restart),
   a restart that fails puts the previous gain back, an editor opened on the target before the rotation stores its edit
   for the next visit, an editor from an earlier scan is refused, and a save keeps writing the configured gain. */
static int
test_scan_row_edit_commands_under_trunk_scan(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.audio_out_type = 9;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "rtl:0:851000000:22:0:48:0:2");
    opts.rtl_gain_value = 22;
    opts.trunk_scan_enabled = 1;
    opts.rtl_squelch_level = dsd_squelch_level_from_sql(-80.0);
    g_config_rtl_open_ok = 1;
    g_config_rtl_fail_starts = 0;
    state.rtl_ctx = (RtlSdrContext*)g_config_rtl_ctx;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_cc_request = gain_scan_tune_to_cc_ok});

    char dir[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    int rc = expect_true("row edit dir", dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_row_edit") != NULL);
    rc |= expect_int("row edit csv path", dsd_test_path_join(csv, sizeof csv, dir, "targets.csv"), 0);
    static const char k_targets[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,rtl_gain,options\n"
                                    "own,p25-trunk,851000000,,3000,,own gain,10,--squelch-db -60\n"
                                    "inherit,dmr-trunk,452000000,,3000,,inherits,,\n";
    rc |= write_file_bytes(csv, k_targets, sizeof k_targets - 1U);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", csv);
    char err[256] = {0};
    const int init_rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    if (init_rc != 0) {
        DSD_FPRINTF(stderr, "row edit scan init: %s\n", err);
    }
    rc |= expect_int("row edit scan init", init_rc, 0);

    dsd_app_scan_row_view view;
    rc |= expect_int("row view", dsd_app_scan_row_view_get(&opts, &state, &view), 0);
    rc |= expect_int("row on air", view.active, 1);
    rc |= expect_str("row label", view.label, "own");
    rc |= expect_int("row offers squelch and gain",
                     dsd_app_scan_row_view_offers(&view, DSD_SCAN_ROW_FIELD_SQUELCH)
                         && dsd_app_scan_row_view_offers(&view, DSD_SCAN_ROW_FIELD_GAIN)
                         && !dsd_app_scan_row_view_offers(&view, DSD_SCAN_ROW_FIELD_WIDTH),
                     1);

    dsd_app_scan_row_edit_payload values = {0};
    values.squelch_db = -45;
    rc |= submit_scan_row_edit(&opts, &state, &view, DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_SET, &values,
                               "squelch edit");
    rc |= expect_str("squelch edit toast", state.ui_msg, "This channel (own): squelch -45 dB for this session");
    rc |=
        expect_true("squelch edit in force", fabs(opts.rtl_squelch_level - dsd_squelch_level_from_sql(-45.0)) < 1e-12);

    const int starts_before = g_config_rtl_starts;
    values.gain_db = 20;
    rc |= submit_scan_row_edit(&opts, &state, &view, DSD_SCAN_ROW_FIELD_GAIN, DSD_SCAN_ROW_EDIT_SET, &values,
                               "gain edit");
    rc |= expect_str("gain edit toast", state.ui_msg, "This channel (own): RTL gain 20 dB for this session");
    rc |= expect_int("gain edit in force", opts.rtl_gain_value, 20);
    rc |= expect_true("gain edit restarted the stream", g_config_rtl_starts > starts_before);
    rc |= expect_int("configured gain untouched", state.trunk_scan_configured_gain, 22);

    /* The restart fails with an I/Q capture on: the previous gain is put back, and the input that ran starts again on
       it without the capture, whose reopen would write over what it recorded. */
    opts.iq_capture_requested = 1;
    DSD_SNPRINTF(opts.iq_capture_path, sizeof opts.iq_capture_path, "%s", "cap.iq");
    g_config_rtl_creates = 0;
    g_config_rtl_captures = 0;
    g_config_rtl_fail_starts = 1;
    values.gain_db = 30;
    rc |= submit_scan_row_edit(&opts, &state, &view, DSD_SCAN_ROW_FIELD_GAIN, DSD_SCAN_ROW_EDIT_SET, &values,
                               "failed gain edit");
    g_config_rtl_fail_starts = 0;
    rc |= expect_true("failed gain edit toast", strncmp(state.ui_msg, "Failed: this channel's RTL gain", 31) == 0);
    rc |= expect_int("failed gain edit puts the previous gain back", opts.rtl_gain_value, 20);
    rc |= expect_int("the recovery start opened no capture",
                     g_config_rtl_creates == 2 && g_config_rtl_captures == 1 && g_config_rtl_create_capture == 0
                         && g_config_rtl_create_gain == 20 && state.rtl_ctx != NULL,
                     1);
    rc |= expect_int("the capture stays off", opts.iq_capture_requested, 0);

    /* The editor stays open across a rotation: its edit waits for the target's next visit. */
    rc |= expect_int("advance", dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_ADVANCE), 0);
    rc |= expect_int("inheriting target runs the configured gain", opts.rtl_gain_value, 22);
    rc |= submit_scan_row_edit(&opts, &state, &view, DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_INHERIT, NULL,
                               "stored inherit");
    rc |= expect_str("stored inherit toast", state.ui_msg,
                     "This channel (own): squelch follows the default from its next visit");
    rc |= expect_true("stored inherit leaves the parked target alone",
                      fabs(opts.rtl_squelch_level - dsd_squelch_level_from_sql(-80.0)) < 1e-12);

    /* An editor from another scan session. */
    dsd_app_scan_row_view stale = view;
    stale.session++;
    values.squelch_db = -50;
    rc |= submit_scan_row_edit(&opts, &state, &stale, DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_SET, &values,
                               "stale edit");
    rc |= expect_str("stale edit toast", state.ui_msg, "Refused: the scan changed; nothing applied");

    dsdneoUserConfig saved;
    dsd_snapshot_opts_to_user_config(&opts, &state, &saved);
    rc |= expect_int("save writes the configured gain", saved.rtl_gain, 22);

    rc |= expect_int("back to own", dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_ADVANCE), 0);
    rc |= expect_int("own runs its edited gain", opts.rtl_gain_value, 20);
    rc |= expect_true("own follows the default squelch",
                      fabs(opts.rtl_squelch_level - dsd_squelch_level_from_sql(-80.0)) < 1e-12);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    (void)remove(csv);
    (void)dsd_test_rmdir(dir);
    state.rtl_ctx = NULL;
    freeState(&state);
    return rc;
}

#endif

/* With no scan running, an edit is refused for either scanner and changes nothing; a payload whose strings are not
   terminated is rejected at submit, and one with an unknown action is a malformed command. */
static int
test_scan_row_edit_commands_without_a_scan(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    opts.rtl_squelch_level = dsd_squelch_level_from_sql(-80.0);
    dsd_app_scan_row_edit_payload p = {0};
    p.session = 7U;
    p.scanner = DSD_SCAN_ROW_SCANNER_CHANNEL_SCAN;
    p.row = 1;
    p.field = (int32_t)DSD_SCAN_ROW_FIELD_SQUELCH;
    p.action = DSD_SCAN_ROW_EDIT_SET;
    p.squelch_db = -50;
    int rc = expect_true("-Y edit queued", dsd_app_command_scan_row_edit(&p) > 0);
    rc |= expect_int("-Y edit drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("-Y edit refused", state.ui_msg, "Refused: no scan is running");
    p.scanner = DSD_SCAN_ROW_SCANNER_TRUNK_SCAN;
    DSD_SNPRINTF(p.target_id, sizeof p.target_id, "%s", "county");
    rc |= expect_true("trunk edit queued", dsd_app_command_scan_row_edit(&p) > 0);
    rc |= expect_int("trunk edit drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("trunk edit refused", state.ui_msg, "Refused: no scan is running");
    rc |= expect_true("nothing changed", fabs(opts.rtl_squelch_level - dsd_squelch_level_from_sql(-80.0)) < 1e-12);

    dsd_app_scan_row_edit_payload unterminated = p;
    DSD_MEMSET(unterminated.target_id, 'x', sizeof unterminated.target_id);
    rc |= expect_int("unterminated payload rejected", dsd_app_command_scan_row_edit(&unterminated),
                     DSD_APP_COMMAND_SUBMIT_REJECTED);
    rc |= expect_int("NULL payload rejected", dsd_app_command_scan_row_edit(NULL), DSD_APP_COMMAND_SUBMIT_REJECTED);
    p.action = 9;
    state.ui_msg[0] = '\0';
    rc |= expect_true("bad action queued", dsd_app_command_scan_row_edit(&p) > 0);
    rc |= expect_int("bad action drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("bad action is a malformed command", dsd_app_command_test_last_invalid_payload(), 1);
    rc |= expect_str("bad action leaves no toast", state.ui_msg, "");
    freeState(&state);
    return rc;
}

int
main(void) {
    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "dsd_socket_init failed\n");
        return 1;
    }
    dsd_neo_log_set_tap(record_test_log, NULL);
    int rc = test_session_queue_cancellation();
    rc |= test_scan_row_edit_commands_without_a_scan();
#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
    rc |= test_scan_row_edit_commands_under_trunk_scan();
#endif
    rc |= test_config_refuses_scanner_under_trunk_scan();
    rc |= test_config_keeps_trunk_scan_lifecycle();
    rc |= test_nfm_bandwidth_set_on_pcm_input();
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    rc |= test_am_width_asks_the_rigctl_peer();
    rc |= test_nfm_width_and_setmod_bw_ask_the_rigctl_peer();
    rc |= test_config_apply_asks_the_rigctl_peer();
    rc |= test_config_mode_switch_refused_by_the_rigctl_peer();
    rc |= test_a_new_rigctl_peer_is_asked_for_a_width_only();
    rc |= test_leaving_the_monitor_undoes_the_rigctl_passband();
    rc |= test_a_width_retry_reaches_the_rigctl_peer();
#endif
    rc |= test_squelch_commands_on_pcm_input();
#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
    rc |= test_rtl_gain_commands_edit_the_trunk_scan_default();
    rc |= test_config_gain_applies_when_the_spec_is_unchanged();
    rc |= test_config_gain_restart_holds_the_running_width();
    rc |= test_config_rtl_restart_under_policy_guard();
    rc |= test_rtl_bandwidth_held_to_am_width();
    rc |= test_squelch_commands_edit_the_configured_default();
    rc |= test_squelch_setting_command();
    rc |= test_squelch_edit_keeps_live_acquisition();
#endif
#ifdef DSD_NEO_TEST_AUDIO_ENSURE_WRAP
    rc |= test_decode_mode_set_ensures_family_sink();
    rc |= test_config_apply_keeps_session_output_layout();
#endif
    rc |= test_family_change_discards_partial_analog_block();
#if defined(USE_RADIO) && defined(DSD_NEO_TEST_ANALOG_WRAP) && defined(DSD_NEO_TEST_AUDIO_ENSURE_WRAP)
    rc |= test_decode_mode_set_switches_rtl_receive_family();
    rc |= test_decode_mode_set_refused_analog_profile_changes_nothing();
    rc |= test_decode_mode_set_leaves_analog_family_off_the_monitor();
    rc |= test_typed_row_republish_follows_configured_family();
    rc |= test_mode_change_under_row_notes_configured_modes();
    rc |= test_config_apply_switches_rtl_receive_family();
    rc |= test_config_apply_refused_analog_profile_changes_nothing();
    rc |= test_decode_mode_set_am_switches_live();
    rc |= test_decode_mode_set_am_clears_received_tone();
    rc |= test_am_bandwidth_set_applies_live();
    rc |= test_config_apply_am_within_the_analog_family();
    rc |= test_am_bandwidth_set_under_scan_rows();
    rc |= test_input_switch_to_pcm_leaves_am();
    rc |= test_tcp_switch_leaves_am_with_unfit_nfm_width();
    rc |= test_nfm_bandwidth_set_applies_live_and_refuses();
    rc |= test_nfm_bandwidth_set_replaces_a_queued_switch();
    rc |= test_nfm_bandwidth_set_after_a_queued_cqpsk_toggle();
    rc |= test_cqpsk_off_under_am_refused_keeps_cqpsk();
    rc |= test_nfm_bandwidth_set_after_a_queued_switch_from_cqpsk();
    rc |= test_nfm_bandwidth_set_under_scan_rows();
    rc |= test_nfm_width_edit_keeps_live_acquisition();
    rc |= test_nfm_bandwidth_set_under_a_width_row();
    rc |= test_refused_width_under_a_scan_row_keeps_the_configured_width();
    rc |= test_refused_row_width_request_puts_the_edit_back();
    rc |= test_decode_mode_analog_under_a_row_holds_the_nfm_width();
    rc |= test_config_apply_holds_nfm_width_to_the_front_end();
    rc |= test_config_apply_width_under_scan_rows();
    rc |= test_config_apply_idle_width_under_scan_rows();
    rc |= test_refused_width_under_a_row_returns_to_the_width_run();
    rc |= test_config_apply_holds_nfm_width_to_a_new_dsp_bandwidth();
    rc |= test_config_apply_holds_nfm_width_to_the_rate_the_reopen_runs_at();
    rc |= test_config_apply_leaves_a_soapy_or_airspy_reopen_to_its_start();
    rc |= test_config_volume_only_reopen_holds_the_unchanged_width();
    rc |= test_config_reopen_that_fails_keeps_the_running_input();
    rc |= test_config_rollback_keeps_the_iq_capture();
    rc |= test_config_rollback_restores_mode_owned_settings();
    rc |= test_config_reopen_with_no_stream_reports_the_failed_start();
    rc |= test_config_apply_restores_the_default_nfm_width();
    rc |= test_scan_list_holds_the_configured_nfm_width();
    rc |= test_scan_list_holds_the_configured_nfm_width_on_am();
    rc |= test_am_width_under_am_scan_rows();
    rc |= test_config_apply_holds_a_scan_row_width_to_a_reopen();
    rc |= test_config_reopen_under_a_scan_row_republishes_the_row();
    rc |= test_config_reopen_under_a_cqpsk_row_times_the_row();
    rc |= test_config_reopen_under_a_cqpsk_row_on_an_analog_session_times_the_row();
    rc |= test_config_reopen_that_changes_the_row_modulation_times_the_row();
    rc |= test_config_reopen_times_the_row_for_the_modulation_it_runs();
    rc |= test_scoped_republish_lands_where_the_row_was_timed();
    rc |= test_config_reopen_whose_scan_row_the_new_stream_refuses();
    rc |= test_config_rollback_restarts_the_profile_the_stream_ran();
    rc |= test_config_reopen_that_stops_the_scanner_skips_the_row();
    rc |= test_config_with_no_stream_keeps_its_mode_when_the_scan_row_cannot_run();
    rc |= test_config_with_no_stream_keeps_its_width_when_the_scan_row_cannot_run();
    rc |= test_config_apply_holds_the_configured_am_width_under_am_rows();
    rc |= test_config_apply_holds_an_am_row_width_to_a_reopen();
#ifdef DSD_NEO_TEST_RTL_WRAP
    rc |= test_rtl_enable_input_holds_the_configured_am_width();
#endif
    rc |= test_nfm_width_waits_for_an_unsettled_cqpsk_toggle();
    rc |= test_nfm_width_refused_after_the_check();
    rc |= test_nfm_width_follows_a_queued_scan_leave();
    rc |= test_refused_scan_leave_is_reported_and_reconciled();
    rc |= test_refused_scan_leave_is_left_to_a_later_request();
    rc |= test_superseded_scan_leave_keeps_an_armed_switch();
    rc |= test_continuing_scan_leave_keeps_an_armed_switch();
    rc |= test_scan_leave_left_to_later_on_analog_keeps_an_armed_switch();
    rc |= test_scan_leave_retired_by_a_row_keeps_an_armed_switch();
    rc |= test_scan_leave_record_goes_with_its_stream();
    rc |= test_refused_leave_at_once_reads_the_am_default_it_asked_for();
    rc |= test_refused_leave_off_the_monitor_reverts_a_switch_between_kinds();
    rc |= test_refused_leave_revert_refused_gets_the_leave_policy();
    rc |= test_refused_switch_onto_analog_puts_the_mode_back();
    rc |= test_refused_switch_onto_analog_with_a_width_set_after_it();
    rc |= test_refused_switch_onto_analog_under_a_row();
    rc |= test_refused_switch_onto_analog_retimes_the_mode();
    rc |= test_nfm_width_changes_held_to_channel_lpf_off();
    rc |= test_refused_switch_between_fm_and_am_puts_the_mode_back();
    rc |= test_refused_switch_after_an_unpublished_toggle_puts_the_mode_back();
    rc |= test_refused_switch_after_a_pending_switch_puts_the_running_mode_back();
    rc |= test_refused_switch_after_a_taken_switch_puts_the_running_mode_back();
    rc |= test_refused_switch_after_a_width_change_holds_the_width();
    rc |= test_refused_request_keeps_each_kind_s_width_baseline();
    rc |= test_refused_request_keeps_a_config_width_baseline();
    rc |= test_fm_am_switch_discards_partial_analog_block();
    rc |= test_am_width_refused_where_it_lands();
    rc |= test_am_held_to_channel_lpf_off();
#endif
#ifdef USE_RADIO
    rc |= test_rtl_set_bw_refuses_a_rate_the_nfm_width_cannot_run_at();
    rc |= test_channel_map_import_names_rows_the_dsp_rate_skips();
#endif
#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
    rc |= test_rtl_enable_input_holds_the_nfm_width();
    rc |= test_rtl_row_returns_to_the_radio_a_switch_left();
    rc |= test_rtl_row_closes_the_pcm_input_it_replaced();
    rc |= test_input_switch_that_fails_keeps_the_running_input();
    rc |= test_input_switch_holds_the_watchdog_guard();
    rc |= test_input_switch_rollback_keeps_the_iq_capture();
    rc |= test_airspy_setting_rollback_reports_the_iq_capture_stop();
    rc |= test_rollback_puts_back_the_input_failure();
    rc |= test_dsp_bandwidth_change_that_fails_keeps_the_running_input();
#if defined(DSD_NEO_TEST_ANALOG_WRAP) && defined(DSD_NEO_TEST_AUDIO_ENSURE_WRAP)
    rc |= test_rollback_restart_republishes_the_scan_row();
    rc |= test_rollback_restart_republishes_an_analog_scan_row();
    rc |= test_restart_under_a_scan_row_republishes_the_row();
#endif
#endif
    rc |= test_am_refused_on_pcm_input();
    rc |= test_am_bandwidth_set_validates();
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    rc |= test_dmr_policy_command_ticks_owner(DSD_APP_CMD_LOCKOUT_SLOT);
    rc |= test_dmr_policy_command_ticks_owner(DSD_APP_CMD_SKIP_SLOT);
#endif
    rc |= test_producer_stop_restart();
    rc |= test_export_disposal();
    rc |= test_direct_key_preserves_active_keyring(0);
    rc |= test_direct_key_preserves_active_keyring(1);
    rc |= test_direct_key_and_force_scope();
    rc |= test_direct_key_updates_preserve_fifo();
    rc |= test_coalesced_setter_erases_old_tail();
    rc |= test_foundation_commands();
    rc |= test_talkgroup_list_commands();
    rc |= test_patched_call_user_blocks_target_supergroup();
    rc |= test_config_group_path_reloads_before_persist(0);
    rc |= test_config_group_path_reloads_before_persist(1);
    rc |= test_skip_commands(0);
    rc |= test_skip_commands(1);
    rc |= test_temporary_lockout_commands(0);
    rc |= test_temporary_lockout_commands(1);
    rc |= test_talkgroup_row_commands();
    rc |= test_talkgroup_export_result();
    rc |= test_source_alias_commands();
    rc |= test_scoped_direct_key_mutes();
    rc |= test_scoped_row_option_commands();
    rc |= test_scoped_setting_toggles();
    rc |= test_scoped_mode_commands_and_config();
    rc |= test_command_api();
    rc |= test_manual_tune_queue_semantics();
    rc |= test_setter_coalescing_preserves_fifo_boundaries();
    rc |= test_visibility_and_queue_overflow();
    rc |= test_key_and_runtime_state_commands();
    rc |= test_file_network_and_import_commands();
    rc |= test_p25_bandplan_commands();
    rc |= test_io_and_state_commands();
    rc |= test_compact_visualizer_toast();
    rc |= test_modulation_and_decode_mode_setters();
    rc |= test_decode_mode_change_clears_received_tone();
    rc |= test_tone_filter_warns_when_a_command_leaves_it_unheard();
    rc |= test_tone_filter_warns_when_a_channel_map_leaves_it_unheard();
    rc |= test_tone_filter_set_edits_the_configured_policy();
    rc |= test_tone_filter_set_under_a_tone_row();
    rc |= test_tone_filter_mode_keeps_the_decoder_list();
    rc |= test_tone_filter_edit_keeps_live_acquisition();
    rc |= test_tone_filter_set_warns_when_unheard();
    rc |= test_input_switch_clears_received_tone();
    rc |= test_playback_switches_clear_received_tone();
    rc |= test_symbol_in_open_replaces_the_playback();
    rc |= test_input_switches_refused_while_a_scan_is_in_force();
    rc |= test_capture_close_records_the_last_capture();
    rc |= test_a_refused_udp_bind_restarts_the_old_input();
    rc |= test_config_reopen_failures_keep_the_input();
    rc |= test_config_apply_input_change_clears_received_tone();
    rc |= test_config_apply_mode_change_clears_received_tone();
    rc |= test_failed_config_reopen_stages_no_file_rate();
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    rc |= test_config_apply_pulse_device_is_a_new_stream();
    rc |= test_tcp_connect_clears_received_tone();
    rc |= test_rigctl_reconnect_key_uses_the_connect_service();
    rc |= test_stop_playback_pulse_failure_keeps_the_playback();
    rc |= test_stop_playback_reopens_pulse_as_a_new_stream();
    rc |= test_output_reconfigure_failure_keeps_the_switch();
#endif
#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP) && defined(DSD_NEO_TEST_IO_CONTROL_WRAP)
    rc |= test_replay_leave_keeps_decode_stamps_ageing();
    rc |= test_replay_kept_by_a_failed_stop();
#endif
    rc |= test_trunk_set();
    rc |= test_scan_voice_gate_commands();
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    rc |= test_manual_tune_commands_commit_only_after_acceptance();
    rc |= test_lockout_hands_p25_sm_back_to_cc(1);
    rc |= test_lockout_hands_p25_sm_back_to_cc(0);
    rc |= test_skip_hands_p25_sm_back_to_cc();
    rc |= test_skip_command_reader_stress();
    rc |= test_channel_cycle_hands_p25_sm_back_to_cc();
    rc |= test_manual_scan_steps_clear_received_tone();
#ifdef USE_RADIO
    rc |= test_manual_tune_trunking_gate_and_reacquisition();
    rc |= test_retune_commands_clear_received_tone();
#endif
    rc |= test_tuner_release();
    rc |= test_replay_refuses_tunes_and_release();
    rc |= test_scan_hold_avoid_commands();
    rc |= test_replay_refuses_channel_cycle_and_return_cc();
    rc |= test_accepted_retunes_forget_the_carrier_codes();
    rc |= test_scan_row_keys_commands();
#endif
#ifdef DSD_NEO_TEST_AUDIO_ENSURE_WRAP
    rc |= expect_int("every case reaching the sink helpers plays to the null output", g_ensure_off_null_calls, 0);
#endif
    dsd_socket_cleanup();
    if (rc == 0) {
        printf("DSD_APP_CMD_QUEUE: OK\n");
    }
    return rc;
}
