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
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/dsd_time.h>
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
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/rtl_stream_fwd.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
static int g_open_audio_input_stub_armed = 0;
static int g_open_audio_input_stub_rc = 0;
static int g_open_audio_input_calls = 0;

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol name.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __real_svc_tcp_connect_audio(dsd_opts* opts, const char* host, int port);
int __wrap_svc_tcp_connect_audio(dsd_opts* opts, const char* host, int port);
int __real_openAudioInput(dsd_opts* opts);
int __wrap_openAudioInput(dsd_opts* opts);
int __wrap_io_control_set_freq(dsd_opts* opts, dsd_state* state, long int freq);
dsd_trunk_tune_result __wrap_dsd_trunk_tuning_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq,
                                                              int ted_sps, uint64_t* out_request_id);
int __real_dsd_tg_policy_call_skip_arm(dsd_state* state, uint32_t id, uint32_t src, int fallback, double now_mono_s);
int __wrap_dsd_tg_policy_call_skip_arm(dsd_state* state, uint32_t id, uint32_t src, int fallback, double now_mono_s);

int
__wrap_dsd_tg_policy_call_skip_arm(dsd_state* state, uint32_t id, uint32_t src, int fallback, double now_mono_s) {
    return g_skip_arm_refused ? -1 : __real_dsd_tg_policy_call_skip_arm(state, id, src, fallback, now_mono_s);
}

/* An armed connect succeeds or fails without a socket; a success leaves the options as the
   real service does: TCP input on the requested endpoint. */
int
__wrap_svc_tcp_connect_audio(dsd_opts* opts, const char* host, int port) {
    if (!g_tcp_connect_stub_armed) {
        return __real_svc_tcp_connect_audio(opts, host, port);
    }
    g_tcp_connect_calls++;
    if (g_tcp_connect_stub_rc == 0 && opts && host) {
        DSD_SNPRINTF(opts->tcp_hostname, sizeof opts->tcp_hostname, "%s", host);
        opts->tcp_portno = port;
        opts->audio_in_type = AUDIO_IN_TCP;
    }
    return g_tcp_connect_stub_rc;
}

int
__wrap_openAudioInput(dsd_opts* opts) {
    if (!g_open_audio_input_stub_armed) {
        return __real_openAudioInput(opts);
    }
    g_open_audio_input_calls++;
    return g_open_audio_input_stub_rc;
}

int
__wrap_io_control_set_freq(dsd_opts* opts, dsd_state* state, long int freq) {
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

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

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

static int
expect_received_tone_cleared(const char* tag, const dsd_state* state, uint32_t seeded_generation) {
    const int cleared = state->analog_rx.tone_state != DSD_ANALOG_TONE_STATE_LOCKED
                        && state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE
                        && state->analog_rx.ctcss_tenths_hz == 0 && state->analog_rx.carrier_open == 0
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
                     dsd_app_command_set_endpoint(DSD_APP_CMD_UDP_INPUT_CFG, "0.0.0.0", 7355),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed p25 payload posts", dsd_app_command_set_p25_p2_params(&p2), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed hytera payload posts", dsd_app_command_set_hytera_key(&hytera),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed aes payload posts", dsd_app_command_set_aes_key(&aes), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("typed dsp payload posts", dsd_app_command_dsp_op(&dsp), DSD_APP_COMMAND_SUBMIT_QUEUED);

    rc |= expect_int("typed commands applied with coalescing", dsd_app_drain_cmds(&opts, &state), 15);
    rc |= expect_int("typed action toggled channels", opts.frontend_display.show_channels, 1);
    rc |= expect_int("typed gain applied latest", (int)opts.audio_gain, 9);
    rc |= expect_str("typed udp input bind copied", opts.udp_in_bindaddr, "0.0.0.0");
    rc |= expect_int("typed udp input port copied", opts.udp_in_portno, 7355);
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
    post_string(DSD_APP_CMD_PULSE_IN_SET, "source0");
    rc |= expect_int("pulse command group applied", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_str("pulse output selected", opts.audio_out_dev, "pulse");
    rc |= expect_int("pulse output type selected", opts.audio_out_type, 0);
    rc |= expect_str("pulse input selected", opts.audio_in_dev, "pulse");
    rc |= expect_int("pulse input type selected", opts.audio_in_type, AUDIO_IN_PULSE);

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
    rc |= expect_str("udp input selected", opts.audio_in_dev, "udp");
    rc |= expect_int("udp input type", opts.audio_in_type, AUDIO_IN_UDP);
    rc |= expect_str("udp bind copied", opts.udp_in_bindaddr, "0.0.0.0");
    rc |= expect_int("udp port copied", opts.udp_in_portno, 7355);
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
    state.p25_last_cc_msg_time_m = dsd_time_now_monotonic_s() + 0.01;
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
                      dsd_tg_policy_call_skip_active(&state, 1201, dsd_time_now_monotonic_s()));
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
    state.p25_last_cc_msg_time_m = dsd_time_now_monotonic_s() + 0.01;
    ev = p25_sm_ev_group_grant(0x1234, 852000000L, 1201, 1202, 0);
    p25_sm_event(sm, &opts, &state, &ev);
    rc |= expect_true("skipped grant remains blocked", p25_sm_get_state(sm) != P25_SM_TUNED);
    rc |= expect_str("grant refusal identifies skip kind", state.p25_sm_last_reason, "grant-blocked-call-skip");
    const double refresh_m = dsd_time_now_monotonic_s();
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
        reader->failures |=
            left != blocked || right != blocked || decision.audio_allowed != !blocked
            || decision.tune_allowed != !blocked || decision.record_allowed != !blocked
            || decision.stream_allowed != !blocked
            || dsd_tg_policy_call_skip_count(reader->state, dsd_time_now_monotonic_s()) != (size_t)blocked;
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
                                       .observed_m = dsd_time_now_monotonic_s()};
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
                          dsd_tg_policy_call_skip_count(&state, dsd_time_now_monotonic_s()) == 0);
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
    state.p25_last_cc_msg_time_m = dsd_time_now_monotonic_s() + 0.01;
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
    opts.scanner_mode = 0;
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
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        init_test_context(&opts, &state);
        opts.trunk_enable = 0;
        opts.scanner_mode = 0;
        seed_received_tone(&state);
        const uint32_t seeded = state.analog_rx.generation;
        reset_io_control_tune_stub(cases[i].tune_result);
        rc |=
            expect_int(cases[i].tag, dsd_app_command_set_u32(cases[i].cmd, 853125000U), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(cases[i].tag, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(cases[i].tag, g_io_control_tune_calls, 1);
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
#endif

/* A decode-mode change is a boundary the received tone (issue #522) must not cross: it goes
   through the acquisition reset, which forgets the tone. */
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
    return rc;
}

/* A new audio input is a new receiver: the tone the old input carried goes when the input
   switches (issue #522), not when the detector next loses it -- which, between two inputs at
   the same rate, nothing else would tell it to. */
static int
test_input_switch_clears_received_tone(void) {
    static const struct {
        int cmd;
        const char* value; /**< string payload, or NULL for none */
        const char* tag;
    } cases[] = {
        {DSD_APP_CMD_INPUT_WAV_SET, "input.wav", "wav input clears the received tone"},
        {DSD_APP_CMD_INPUT_SET_PULSE, NULL, "pulse input clears the received tone"},
        {DSD_APP_CMD_PULSE_IN_SET, "source0", "named pulse source clears the received tone"},
        {DSD_APP_CMD_UDP_INPUT_CFG, NULL, "udp input clears the received tone"},
        {DSD_APP_CMD_INPUT_SYM_STREAM_SET, "symbols.f32", "symbol stream input clears the received tone"},
    };

    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        init_test_context(&opts, &state);
        seed_received_tone(&state);
        const uint32_t seeded = state.analog_rx.generation;
        int queued = 0;
        if (cases[i].cmd == DSD_APP_CMD_UDP_INPUT_CFG) {
            queued = post_host_port(cases[i].cmd, "0.0.0.0", 7355);
        } else if (cases[i].value) {
            queued = post_string(cases[i].cmd, cases[i].value);
        } else {
            queued = post_empty(cases[i].cmd);
        }
        rc |= expect_int(cases[i].tag, queued, DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(cases[i].tag, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_received_tone_cleared(cases[i].tag, &state, seeded);
        freeState(&state);
    }
    return rc;
}

/*
 * The input switches the table above cannot drive without a real file: replaying the last
 * input as a symbol file, and stopping playback back onto a live input. Both clear the tone
 * the old input carried (issue #522).
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
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", path);
    seed_received_tone(&state);
    uint32_t seeded = state.analog_rx.generation;
    rc |= expect_int("replay last queued", post_empty(DSD_APP_CMD_REPLAY_LAST), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("replay last drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("replay last switches to the symbol file", opts.audio_in_type, AUDIO_IN_SYMBOL_BIN);
    rc |= expect_received_tone_cleared("replay last clears the received tone", &state, seeded);

    /* Stopping that playback moves the input again: onto stdin when an output other than
       Pulse is configured. */
    opts.audio_out_type = 9;
    seed_received_tone(&state);
    seeded = state.analog_rx.generation;
    rc |= expect_int("stop playback queued", post_empty(DSD_APP_CMD_STOP_PLAYBACK), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("stop playback drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("stop playback closes the symbol file", opts.symbolfile == NULL);
    rc |= expect_int("stop playback switches to stdin", opts.audio_in_type, AUDIO_IN_STDIN);
    rc |= expect_received_tone_cleared("stop playback clears the received tone", &state, seeded);
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
        {DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG, -1, 0, "refused tcp connect to an endpoint keeps the received tone"},
        {DSD_APP_CMD_TCP_CONNECT_AUDIO, 0, 1, "tcp reconnect clears the received tone"},
        {DSD_APP_CMD_TCP_CONNECT_AUDIO, -1, 0, "refused tcp reconnect keeps the received tone"},
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
 * Stopping playback back onto live Pulse input clears the playback's tone even when Pulse
 * then fails to open: the playback is gone either way, and its tone does not describe
 * whatever the input becomes next (issue #522).
 */
static int
test_stop_playback_pulse_failure_clears_received_tone(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);
    opts.audio_out_type = 0;
    opts.audio_in_type = AUDIO_IN_WAV;
    seed_received_tone(&state);
    const uint32_t seeded = state.analog_rx.generation;
    arm_open_audio_input_stub(1, -1);
    rc |= expect_int("stop playback onto pulse queued", post_empty(DSD_APP_CMD_STOP_PLAYBACK),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("stop playback onto pulse drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("stop playback tried to open pulse", g_open_audio_input_calls, 1);
    rc |= expect_int("stop playback switched to pulse", opts.audio_in_type, AUDIO_IN_PULSE);
    rc |= expect_received_tone_cleared("failed pulse open still clears the received tone", &state, seeded);
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

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
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
                                              .observed_m = dsd_time_now_monotonic_s()};
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

static int
expect_file_bytes(const char* path, const char* expected) {
    char contents[1024] = {0};
    FILE* file = dsd_fopen_existing_regular_file(path, "rb");
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
        max_align_t alignment;
        unsigned char bytes[1200];
    } storage = {0};

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
    rc |= expect_true("idle skip no-op", dsd_tg_policy_call_skip_count(&state, dsd_time_now_monotonic_s()) == 0);
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
            const double now = dsd_time_now_monotonic_s();
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
    rc |= expect_int("export seed skip", dsd_tg_policy_call_skip_arm(&state, 123, 1, 0, dsd_time_now_monotonic_s()), 0);

    union {
        max_align_t alignment;
        unsigned char bytes[1200];
    } storage = {0};

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

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol name.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
int __wrap_rtl_stream_create(dsd_opts* opts, RtlSdrContext** out_ctx);
int __wrap_rtl_stream_stop(RtlSdrContext* ctx);
int __wrap_rtl_stream_destroy(RtlSdrContext* ctx);

int
__wrap_rtl_stream_create(dsd_opts* opts, RtlSdrContext** out_ctx) {
    g_config_rtl_create_squelch = opts ? opts->rtl_squelch_level : -1.0;
    g_config_rtl_create_frame_p25p1 = opts ? opts->frame_p25p1 : -1;
    *out_ctx = NULL;
    ++g_config_rtl_creates;
    return -1;
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
#endif

/* --- Issue #521: squelch edits beneath a row or target that overrides it --- */

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
    rc |= expect_str("plain squelch toast", state.ui_msg, "Applied: RTL squelch -> -70.0 dB");
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
/* What rtl_stream_analog_family_active() reports: the front end runs the analog family (its monitor output, or a
   symbol profile a CQPSK toggle or typed row applied under it). */
static int g_fake_analog_family;
static unsigned int g_fake_digital_rate;
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
/* What the stream kept when it refused (rtl_stream_receive_request_refusal()): its family, and the analog width. */
static int g_fake_rx_kept_analog;
static int g_fake_rx_kept_width_hz;
/* The CQPSK state the requests queued since the last landing leave the stream on (rtl_stream_requested_cqpsk()). */
static int g_fake_cqpsk_after;
/* What rtl_stream_request_analog_profile() answers (0 queues it): -1 is a front end that refuses the request at the
   rate it publishes now, although rtl_stream_check_analog_profile() took it (a retune in between). */
static int g_analog_req_result;
/* What rtl_stream_get_demod_rate_hz() reports: the demod rate the stream publishes. */
static int g_fake_demod_rate_hz;
/* What rtl_stream_output_rate() reports: the rate the decoder reads and times symbols for. 0, as the real one answers
   for the tests' fake context, has the decoder time for the input's own rate. */
static uint32_t g_fake_output_rate_hz;
/* The CQPSK state of the last demod profile requested. */
static int g_demod_req_cqpsk;
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
unsigned int __wrap_rtl_stream_output_rate_for_family(int family, int cqpsk_enable, int symbol_rate_hz);
void __wrap_rtl_stream_set_digital_decode_modes(const dsd_opts* opts);
uint32_t __wrap_rtl_stream_receive_request_seq(void);
int __wrap_rtl_stream_receive_request_outcome(uint32_t seq);
int __wrap_rtl_stream_receive_request_refusal(uint32_t seq, int* out_analog_family, int* out_width_hz);
int __wrap_rtl_stream_requested_cqpsk(void);
int __wrap_rtl_stream_get_demod_rate_hz(void);
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
    return (seq != 0U && seq == g_fake_rx_refused) ? RTL_STREAM_RX_REQUEST_REFUSED : RTL_STREAM_RX_REQUEST_SETTLED;
}

int
__wrap_rtl_stream_receive_request_refusal(uint32_t seq, int* out_analog_family, int* out_width_hz) {
    if (__wrap_rtl_stream_receive_request_outcome(seq) != RTL_STREAM_RX_REQUEST_REFUSED) {
        return 0;
    }
    if (out_analog_family) {
        *out_analog_family = g_fake_rx_kept_analog;
    }
    if (out_width_hz) {
        *out_width_hz = g_fake_rx_kept_width_hz;
    }
    return 1;
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
    g_analog_req_family = family;
    g_analog_req_kind = kind;
    g_analog_req_width_hz = width_hz;
    g_analog_req_order = ++g_rx_sequence;
    if (g_analog_req_result != 0) {
        return g_analog_req_result;
    }
    g_fake_cqpsk_after = (family == DSD_RX_FAMILY_ANALOG) ? 0 : __wrap_rtl_stream_requested_cqpsk();
    g_fake_rx_analog_seq = ++g_fake_rx_seq;
    return 0;
}

int
__wrap_rtl_stream_request_demod_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile,
                                        int ted_sps, int ted_sps_is_override) {
    (void)levels;
    (void)channel_profile;
    (void)ted_sps_is_override;
    g_demod_req_calls++;
    g_demod_req_cqpsk = cqpsk_enable;
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

unsigned int
__wrap_rtl_stream_output_rate_for_family(int family, int cqpsk_enable, int symbol_rate_hz) {
    (void)family;
    (void)cqpsk_enable;
    (void)symbol_rate_hz;
    return g_fake_digital_rate;
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
    g_fake_rx_settled = g_fake_rx_seq;
}

/* The demod thread has taken everything queued so far, and refused the last analog request where it landed (a retune
   moved the demod rate after it was checked): the front end keeps the receive profile, and the CQPSK state, it had,
   which the stream records as the family it stayed on (@p analog_family) and the analog width it runs. */
static void
demod_thread_refuses_analog_keeping(int analog_family, int width_hz) {
    g_fake_rx_kept_analog = analog_family;
    g_fake_rx_kept_width_hz = width_hz;
    g_fake_rx_refused = g_fake_rx_analog_seq;
    g_fake_rx_settled = g_fake_rx_seq;
}

/* The stream restarts: the open drops what the previous stream left queued and forgets a refusal it recorded. */
static void
stream_reopens(int cqpsk) {
    g_fake_rx_refused = 0U;
    demod_thread_lands(cqpsk);
}

/* Each step starts on a stream that has taken the last step's requests (demod_thread_lands()), so what the step sets
   in g_fake_cqpsk is what the stream publishes. */
static void
reset_rx_family_wrap(void) {
    demod_thread_lands(g_fake_cqpsk);
    g_analog_check_result = 0;
    g_analog_req_result = 0;
    g_analog_check_calls = g_analog_check_family = g_analog_check_kind = g_analog_check_width_hz = 0;
    g_rx_sequence = 0;
    g_analog_req_calls = g_analog_req_family = g_analog_req_kind = g_analog_req_width_hz = g_analog_req_order = 0;
    g_demod_req_calls = g_demod_req_order = g_demod_req_rate = g_demod_req_ted_sps = g_demod_req_cqpsk = 0;
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
    rc |= expect_int("cqpsk off queues its demod profile", g_demod_req_calls, 1);
    rc |= expect_int("cqpsk off requests the analog profile", g_analog_req_calls, 1);
    rc |= expect_int("cqpsk off: analog after the demod profile", g_analog_req_order > g_demod_req_order, 1);
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
    rc |= expect_int("cqpsk off + width: CQPSK requested off", g_demod_req_calls == 1 && g_demod_req_cqpsk == 0, 1);
    rc |= expect_int("cqpsk off + width: toggle and width both requested", g_analog_req_calls, 2);
    rc |= expect_int("cqpsk off + width: the last request carries the new width", g_analog_req_width_hz, 20000);
    rc |= expect_int("cqpsk off + width: after the demod profile", g_analog_req_order > g_demod_req_order, 1);

    /* Two toggles in one drain flip twice: the second reads the first, queued but not taken. */
    reset_rx_family_wrap();
    g_fake_cqpsk = 0;
    submit_cqpsk_toggle();
    submit_cqpsk_toggle();
    rc |= expect_int("two toggles drained", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_int("two toggles: on, then off", g_demod_req_calls == 2 && g_demod_req_cqpsk == 0, 1);
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
    rc |= expect_int("own width row: republished", svc_publish_nfm_bandwidth(&opts, &state, -1), 0);
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

/*
 * A config whose [input] reopens a SoapySDR or Airspy device over the running RTL-SDR runs an explicit NFM width at the
 * rate that device delivers (an Airspy's 78,125 Hz, say), which neither the running input's DSP rate nor rtl_bw_khz
 * gives. It is held to neither: the reopened stream's start checks the width at the delivered rate. An Airspy source
 * over a running Airspy reopens nothing (it applies live), so the running front end still holds it.
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
       stream's start, as it does for any SoapySDR or Airspy reopen. */
    opts.rtl_dsp_bw_khz = 12;
    reset_rx_family_wrap();
    g_analog_check_result = -1;
    rc |= submit_config_device_source(&opts, &state, DSDCFG_INPUT_AIRSPY, 48, 25000, "cfg airspy 12->48 kHz");
    rc |= expect_int("cfg airspy 12->48 kHz: front end not asked", g_analog_check_calls, 0);
    rc |= expect_int("cfg airspy 12->48 kHz: not refused", strstr(state.ui_msg, "Config not applied") == NULL, 1);
    rc |= expect_int("cfg airspy 12->48 kHz: width applied", opts.analog_nfm_bandwidth_hz, 25000);

    g_analog_check_result = 0;
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

    /* Switched to Analog and on to P25 before either landed; the stream then reports the Analog switch refused. The
       P25 request replaced it, so the decoder stays where the operator put it last. */
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
 * and Input > Switch source > RTL-SDR. The unset default follows the environment and is never refused. PCM input runs
 * no channel filter, so there the width is only stored.
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
    opts.audio_in_type = AUDIO_IN_RTL;

    (void)dsd_unsetenv("DSD_NEO_CHANNEL_LPF");
    dsd_neo_config_init();
    opts.analog_nfm_bandwidth_hz = 0;
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
 * Input > Switch source > RTL-SDR opens a device at the RTL DSP bandwidth, so an explicit NFM width that bandwidth
 * cannot filter is refused before the running input is rewritten and torn down: the new stream's start would refuse the
 * width and leave no stream, with only a generic failure to show for it. The config path refuses the same move. The
 * unset default, a bandwidth that fits, and inputs whose device or capture sets the rate (SoapySDR, the Airspy enable)
 * are not held.
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

    /* A SoapySDR device, and the Airspy enable, run at a rate the device sets: their start checks it. */
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
    ctx->t_tune_m = dsd_time_now_monotonic_s() - ctx->grant_timeout_s - 1.0;
    const dsd_call_observation call = {.protocol = DSD_SYNC_DMR_BS_VOICE_POS,
                                       .slot = 0,
                                       .kind = DSD_CALL_KIND_GROUP_VOICE,
                                       .ota_target_id = 1234,
                                       .policy_target_id = 1234,
                                       .ota_source_id = 42,
                                       .frequency_hz = 452000000,
                                       .observed_m = dsd_time_now_monotonic_s()};
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

int
main(void) {
    int rc = test_session_queue_cancellation();
    rc |= test_config_refuses_scanner_under_trunk_scan();
    rc |= test_config_keeps_trunk_scan_lifecycle();
    rc |= test_nfm_bandwidth_set_on_pcm_input();
#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
    rc |= test_config_rtl_restart_under_policy_guard();
    rc |= test_squelch_commands_edit_the_configured_default();
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
    rc |= test_nfm_bandwidth_set_applies_live_and_refuses();
    rc |= test_nfm_bandwidth_set_replaces_a_queued_switch();
    rc |= test_nfm_bandwidth_set_after_a_queued_cqpsk_toggle();
    rc |= test_nfm_bandwidth_set_after_a_queued_switch_from_cqpsk();
    rc |= test_nfm_bandwidth_set_under_scan_rows();
    rc |= test_nfm_width_edit_keeps_live_acquisition();
    rc |= test_nfm_bandwidth_set_under_a_width_row();
    rc |= test_refused_width_under_a_scan_row_keeps_the_configured_width();
    rc |= test_decode_mode_analog_under_a_row_holds_the_nfm_width();
    rc |= test_config_apply_holds_nfm_width_to_the_front_end();
    rc |= test_config_apply_width_under_scan_rows();
    rc |= test_refused_width_under_a_row_returns_to_the_width_run();
    rc |= test_config_apply_holds_nfm_width_to_a_new_dsp_bandwidth();
    rc |= test_config_apply_holds_nfm_width_to_the_rate_the_reopen_runs_at();
    rc |= test_config_apply_leaves_a_soapy_or_airspy_reopen_to_its_start();
    rc |= test_config_apply_restores_the_default_nfm_width();
    rc |= test_scan_list_holds_the_configured_nfm_width();
    rc |= test_config_apply_holds_a_scan_row_width_to_a_reopen();
    rc |= test_nfm_width_waits_for_an_unsettled_cqpsk_toggle();
    rc |= test_nfm_width_refused_after_the_check();
    rc |= test_nfm_width_follows_a_queued_scan_leave();
    rc |= test_refused_switch_onto_analog_puts_the_mode_back();
    rc |= test_refused_switch_onto_analog_with_a_width_set_after_it();
    rc |= test_refused_switch_onto_analog_under_a_row();
    rc |= test_refused_switch_onto_analog_retimes_the_mode();
    rc |= test_nfm_width_changes_held_to_channel_lpf_off();
#endif
#ifdef USE_RADIO
    rc |= test_rtl_set_bw_refuses_a_rate_the_nfm_width_cannot_run_at();
    rc |= test_channel_map_import_names_rows_the_dsp_rate_skips();
#endif
#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
    rc |= test_rtl_enable_input_holds_the_nfm_width();
#endif
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
    rc |= test_input_switch_clears_received_tone();
    rc |= test_playback_switches_clear_received_tone();
    rc |= test_config_apply_input_change_clears_received_tone();
    rc |= test_config_apply_mode_change_clears_received_tone();
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    rc |= test_tcp_connect_clears_received_tone();
    rc |= test_stop_playback_pulse_failure_clears_received_tone();
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
    rc |= test_scan_hold_avoid_commands();
    rc |= test_scan_row_keys_commands();
#endif
#ifdef DSD_NEO_TEST_AUDIO_ENSURE_WRAP
    rc |= expect_int("every case reaching the sink helpers plays to the null output", g_ensure_off_null_calls, 0);
#endif
    if (rc == 0) {
        printf("DSD_APP_CMD_QUEUE: OK\n");
    }
    return rc;
}
