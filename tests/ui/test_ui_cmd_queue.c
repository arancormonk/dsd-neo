// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic queue-level contracts for app-control commands.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/app_control/commands_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/runtime/config.h"

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
static int g_io_control_tune_result = RTL_STREAM_TUNE_OK;
static int g_io_control_tune_calls = 0;
static long int g_io_control_tune_freq = 0;
static dsd_trunk_tune_result g_cc_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
static int g_cc_tune_calls = 0;
static long int g_cc_tune_freq = 0;
static int g_cc_tune_ted_sps = 0;
static int g_cc_profile_at_tune = -1;

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol name.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __wrap_io_control_set_freq(dsd_opts* opts, dsd_state* state, long int freq);
dsd_trunk_tune_result __wrap_dsd_trunk_tuning_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq,
                                                              int ted_sps);

int
__wrap_io_control_set_freq(dsd_opts* opts, dsd_state* state, long int freq) {
    (void)opts;
    (void)state;
    g_io_control_tune_calls++;
    g_io_control_tune_freq = freq;
    return g_io_control_tune_result;
}

dsd_trunk_tune_result
__wrap_dsd_trunk_tuning_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
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

static int
enc_lockout_inert(const dsd_state* state) {
    return state != NULL && dsd_enc_lockout_active_count(state) == 0;
}

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got \"%s\" want \"%s\"\n", tag, got, want);
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

    remove(symbol_out);
    remove(symbol_in);
    remove(missing_csv);
    remove(key_csv);

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
    rc |= expect_str("group import path copied", opts.group_in_file, missing_csv);
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
static void
seed_active_p25_voice(dsd_opts* opts, dsd_state* state, long cc_freq, long vc_freq, int tg) {
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
        .policy_target_id = (uint32_t)tg,
        .ota_source_id = (uint32_t)(tg + 1),
        .frequency_hz = vc_freq,
        .observed_m = 1.0,
    };
    if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0) {
        dsd_event_sync_slot(opts, state, 0U);
    }
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
    rc |= expect_str("deferred lockout policy name", lockout_name, "LOCKOUT");
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
    init_test_context(&opts, &state);
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
    init_test_context(&opts, &state);
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
    init_test_context(&opts, &state);
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
    init_test_context(&opts, &state);
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
    init_test_context(&opts, &state);
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
    init_test_context(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dmr = 0;
    rc |= expect_int("out-of-range mode queued", dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, 260),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("out-of-range mode drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("out-of-range mode does not alias onto dmr", opts.frame_dmr, 0);
    rc |= expect_int("out-of-range mode leaves p25 alone", opts.frame_p25p1, 1);
    freeState(&state);

    /* Back to auto re-enables the set the engine starts with. */
    init_test_context(&opts, &state);
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
    (void)state;
    g_scan_control_calls++;
    g_scan_control_last_op = op;
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
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);

    /* Hold: flips the flag, leaves the dwell alone; release restarts the dwell. */
    rc |= expect_int("scan hold queued", dsd_app_command_action(DSD_APP_CMD_SCAN_HOLD_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("scan hold drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("scan hold sets flag", state.lcn_scan_hold, 1);
    rc |= expect_true("scan hold keeps dwell", state.last_cc_sync_time_m == 42.0);
    rc |= expect_contains("scan hold toast", state.ui_msg, "hold on");
    rc |= expect_int("scan hold release queued", dsd_app_command_action(DSD_APP_CMD_SCAN_HOLD_TOGGLE),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("scan hold release drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("scan hold release clears flag", state.lcn_scan_hold, 0);
    rc |= expect_true("scan hold release restarts dwell", state.last_cc_sync_time_m > 42.0);
    rc |= expect_contains("scan hold release toast", state.ui_msg, "hold off");

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
    dsd_trunk_scan_hooks_set(hooks);
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
    rc |= expect_int("trunk-scan avoid queued", dsd_app_command_action(DSD_APP_CMD_SCAN_AVOID),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("trunk-scan avoid drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("trunk-scan avoid op", g_scan_control_last_op, DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE);
    rc |= expect_int("trunk-scan avoid leaves -Y count", (int)state.lcn_avoid_count, 0);
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
    dsd_trunk_scan_hooks none = {0};
    dsd_trunk_scan_hooks_set(none);
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
    init_test_context(opts, state);
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
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", "global.csv");
    state->M = 0;
    int rc = expect_int("enter option row", dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR), 0);
    const dsd_scan_option_values row = {.present = DSD_SCAN_OPT_FORCE | DSD_SCAN_OPT_CRC | DSD_SCAN_OPT_VOICE
                                                   | DSD_SCAN_OPT_HOLD | DSD_SCAN_OPT_GROUP | DSD_SCAN_OPT_MUTE_DMR,
                                        .force = 0x21,
                                        .strict_crc = 0,
                                        .voice_only = 1,
                                        .hold_ms = 4000,
                                        .mute_dmr = 1,
                                        .group_file = "row.csv"};
    rc |= expect_int("install row options", dsd_scan_mode_options(opts, state, &row), 0);
    rc |= expect_int("queue force default", post_empty(DSD_APP_CMD_FORCE_PRIV_TOGGLE), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("queue CRC default", post_empty(DSD_APP_CMD_AGGR_SYNC_TOGGLE), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("queue hold default", dsd_app_command_set_i32(DSD_APP_CMD_SCAN_VOICE_HOLD_MS_SET, 3000),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("queue mute default", post_empty(DSD_APP_CMD_ALL_MUTES_TOGGLE), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("option commands drained", dsd_app_drain_cmds(opts, state), 4);
    rc |= expect_int("row keeps force", state->M, 0x21);
    rc |= expect_int("row keeps hold", opts->scan_voice_hold_ms, 4000);
    rc |= expect_int("row keeps mute", opts->dmr_mute_encL, 1);
    dsd_scan_settings configured;
    dsd_scan_mode_configured(opts, state, &configured);
    rc |= expect_int("configured force updated", configured.force_key, 1);
    rc |= expect_int("configured mute updated", configured.dmr_mute_encL, 0);
    rc |= expect_int("configured CRC updated", configured.aggressive_framesync, 0);
    rc |= expect_int("configured CRC default updated", configured.dmr_crc_relaxed_default, 1);
    dsdneoUserConfig saved;
    dsd_snapshot_opts_to_user_config(opts, state, &saved);
    rc |= expect_int("saved configured hold", saved.trunk_scan_voice_hold_ms, 3000);
    rc |= expect_int("saved configured gate", saved.trunk_scan_voice_only, 0);
    rc |= expect_str("saved configured groups", saved.trunk_group_csv, "global.csv");
    rc |= expect_int("clear row options", dsd_scan_mode_options(opts, state, NULL), 0);
    rc |= expect_int("clear options restores force", state->M, 1);
    rc |= expect_int("clear options restores hold", opts->scan_voice_hold_ms, 3000);
    rc |= expect_int("clear options restores mute", opts->dmr_mute_encL, 0);
    rc |= expect_str("clear options restores groups", opts->group_in_file, "global.csv");
    dsd_scan_mode_leave(opts, state);
    freeState(state);
    free(state);
    free(opts);
    return rc;
}

int
main(void) {
    int rc = 0;
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
    rc |= test_trunk_set();
    rc |= test_scan_voice_gate_commands();
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    rc |= test_manual_tune_commands_commit_only_after_acceptance();
#ifdef USE_RADIO
    rc |= test_manual_tune_trunking_gate_and_reacquisition();
#endif
    rc |= test_tuner_release();
    rc |= test_scan_hold_avoid_commands();
    rc |= test_scan_row_keys_commands();
#endif
    if (rc == 0) {
        printf("DSD_APP_CMD_QUEUE: OK\n");
    }
    return rc;
}
