// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic queue-level contracts for app-control commands.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
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
    rc |= expect_str("channel import path copied", opts.chan_in_file, missing_csv);
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
 */
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

    /* And it is the whole point that a tap now works where it was refused. */
    rc |= expect_int("tune after release queued", dsd_app_command_set_u32(DSD_APP_CMD_MANUAL_TUNE, 853125000U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tune after release drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("tune after release reaches the tuner", g_io_control_tune_calls, 1);
    rc |= expect_contains("tune after release reports applied", state.ui_msg, "Applied: tuned -> 853125000 Hz");
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

    /* Both carry a payload, so the payload-less action API must refuse them. */
    rc |= expect_int("mod set rejects an action submit", dsd_app_command_action(DSD_APP_CMD_MOD_SET),
                     DSD_APP_COMMAND_SUBMIT_REJECTED);
    rc |= expect_int("decode mode rejects an action submit", dsd_app_command_action(DSD_APP_CMD_DECODE_MODE_SET),
                     DSD_APP_COMMAND_SUBMIT_REJECTED);
    freeState(&state);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_command_api();
    rc |= test_manual_tune_queue_semantics();
    rc |= test_setter_coalescing_preserves_fifo_boundaries();
    rc |= test_visibility_and_queue_overflow();
    rc |= test_key_and_runtime_state_commands();
    rc |= test_file_network_and_import_commands();
    rc |= test_io_and_state_commands();
    rc |= test_compact_visualizer_toast();
    rc |= test_modulation_and_decode_mode_setters();
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    rc |= test_manual_tune_commands_commit_only_after_acceptance();
    rc |= test_manual_tune_trunking_gate_and_reacquisition();
    rc |= test_tuner_release();
#endif
    if (rc == 0) {
        printf("DSD_APP_CMD_QUEUE: OK\n");
    }
    return rc;
}
