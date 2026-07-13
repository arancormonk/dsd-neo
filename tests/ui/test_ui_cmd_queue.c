// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic queue-level contracts for app-control commands.
 */

#include <dsd-neo/app_control/commands.h>
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
p25_encrypted_call_cache_empty(const dsd_state* state) {
    if (!state || state->p25_enc_tg_cache_next != 0U) {
        return 0;
    }
    for (int i = 0; i < DSD_P25_ENC_TG_CACHE_DEPTH; i++) {
        if (state->p25_enc_tg_cache_until[i] != 0 || state->p25_enc_tg_cache_tg[i] != 0U
            || state->p25_enc_tg_cache_is_group[i] != 0U) {
            return 0;
        }
    }
    return 1;
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
    state.p25_enc_tg_cache_until[0] = 1234567890;
    state.p25_enc_tg_cache_tg[0] = 2468U;
    state.p25_enc_tg_cache_is_group[0] = 1U;
    state.p25_enc_tg_cache_next = 1U;

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
    rc |=
        expect_true("manual RC4/AES key changes clear P25 blocked-call cache", p25_encrypted_call_cache_empty(&state));

    state.p25_enc_tg_cache_until[0] = 1234567890;
    state.p25_enc_tg_cache_tg[0] = 9753U;
    state.p25_enc_tg_cache_is_group[0] = 0U;
    state.p25_enc_tg_cache_next = 2U;
    rc |=
        expect_int("standalone AES key change posts", dsd_app_command_set_aes_key(&aes), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("standalone AES key change applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |=
        expect_true("standalone AES key change clears P25 blocked-call cache", p25_encrypted_call_cache_empty(&state));

    state.p25_enc_tg_cache_until[0] = 1234567890;
    state.p25_enc_tg_cache_tg[0] = 8642U;
    state.p25_enc_tg_cache_is_group[0] = 1U;
    state.p25_enc_tg_cache_next = 3U;
    rc |= expect_int("standalone RC4/DES key change posts", dsd_app_command_set_u64(DSD_APP_CMD_KEY_RC4DES_SET, 0xAAU),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("standalone RC4/DES key change applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("standalone RC4/DES key change clears P25 blocked-call cache",
                      p25_encrypted_call_cache_empty(&state));

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

    state.p25_enc_tg_cache_until[0] = 1234567890;
    state.p25_enc_tg_cache_tg[0] = 3579U;
    state.p25_enc_tg_cache_is_group[0] = 0U;
    state.p25_enc_tg_cache_next = 4U;
    post_string(DSD_APP_CMD_IMPORT_KEYS_DEC, key_csv);
    rc |= expect_int("successful runtime key import applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_true("successful runtime key import clears P25 blocked-call cache",
                      p25_encrypted_call_cache_empty(&state));

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
    state->lasttg = tg;
    state->lastsrc = tg + 1;
    state->last_cc_sync_time = 123;
    state->last_cc_sync_time_m = 42.0;
    state->synctype = DSD_SYNC_P25P1_POS;
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    state->samplesPerSymbol = 7;
    state->symbolCenter = 3;
    state->p25_cc_is_tdma = 0;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state->sps_hunt_counter = 17;
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
    rc |= expect_int("deferred return-to-CC keeps TG", state.lasttg, 1201);
    rc |= expect_true("deferred return-to-CC keeps VC", state.p25_vc_freq[0] == 852000000L);
    rc |= expect_true("deferred return-to-CC keeps CC sync", state.last_cc_sync_time_m == 42.0);
    rc |= expect_int("deferred return-to-CC keeps SPS", state.samplesPerSymbol, 7);
    rc |= expect_int("deferred return-to-CC keeps SPS profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_4800_2);
    rc |= expect_int("deferred return-to-CC keeps SPS hunt counter", state.sps_hunt_counter, 17);

    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_PENDING);
    rc |= expect_int("accepted timeout return-to-CC queued", dsd_app_command_action(DSD_APP_CMD_RETURN_CC),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("accepted timeout return-to-CC drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("accepted timeout clears trunk tuned", opts.trunk_is_tuned, 0);
    rc |= expect_int("accepted timeout clears TG", state.lasttg, 0);
    rc |= expect_true("accepted timeout clears VC", state.p25_vc_freq[0] == 0L);
    rc |= expect_true("accepted timeout refreshes CC sync", state.last_cc_sync_time_m > 42.0);
    rc |= expect_int("accepted timeout selects P25 SPS profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    rc |= expect_int("accepted timeout resets SPS hunt counter", state.sps_hunt_counter, 0);
    rc |= expect_int("accepted timeout used profile-aware CC tune", g_cc_tune_calls, 1);
    rc |= expect_int("accepted timeout profile was staged before commit", g_cc_profile_at_tune,
                     DSD_FRAME_SYNC_SPS_PROFILE_4800_2);
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
    rc |= expect_int("deferred lockout queued", dsd_app_command_set_u8(DSD_APP_CMD_LOCKOUT_SLOT, 0U),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("deferred lockout drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("deferred lockout tune calls", g_cc_tune_calls, 1);
    rc |= expect_int("deferred lockout keeps trunk tuned", opts.trunk_is_tuned, 1);
    rc |= expect_int("deferred lockout keeps TG", state.lasttg, 2201);
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
    rc |= expect_true("deferred lockout reports cleanup separately",
                      strstr(state.ui_msg, "TG 2201 locked out; return-to-CC tune failed") != NULL);
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
    rc |= expect_int("no-CC lockout clears TG", state.lasttg, 0);
    rc |= expect_true("no-CC lockout clears P25 VC", state.p25_vc_freq[0] == 0L);
    rc |= expect_true("no-CC lockout clears trunk VC", state.trunk_vc_freq[0] == 0L);
    rc |= expect_int("no-CC lockout runs no-carrier cleanup", state.carrier, 0);
    rc |= expect_true("no-CC lockout keeps CC unknown", state.trunk_cc_freq == 0L && state.p25_cc_freq == 0L);
    freeState(&state);

    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 855000000L, 856000000L, 3201);
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
    rc |= expect_int("deferred channel cycle keeps TG", state.lasttg, 3201);
    rc |= expect_true("deferred channel cycle keeps VC", state.p25_vc_freq[0] == 856000000L);
    rc |= expect_true("deferred channel cycle keeps CC sync", state.last_cc_sync_time_m == 42.0);

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
    rc |= expect_int("accepted channel cycle clears TG", state.lasttg, 0);
    rc |= expect_true("accepted channel cycle clears P25 VC", state.p25_vc_freq[0] == 0L);
    rc |= expect_true("accepted channel cycle refreshes CC sync", state.last_cc_sync_time_m > 42.0);
    rc |= expect_int("accepted channel cycle uses raw tune", g_io_control_tune_calls, 1);
    rc |= expect_true("accepted channel cycle frequency", g_io_control_tune_freq == 857000000L);
    rc |= expect_int("accepted channel cycle skips CC tune", g_cc_tune_calls, 0);
    rc |= expect_int("accepted channel cycle keeps SPS", state.samplesPerSymbol, 8);
    rc |= expect_int("accepted channel cycle keeps symbol center", state.symbolCenter, 3);
    rc |= expect_int("accepted channel cycle keeps SPS profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    rc |= expect_int("accepted channel cycle keeps SPS hunt counter", state.sps_hunt_counter, 23);

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
#endif

int
main(void) {
    int rc = 0;
    rc |= test_command_api();
    rc |= test_setter_coalescing_preserves_fifo_boundaries();
    rc |= test_visibility_and_queue_overflow();
    rc |= test_key_and_runtime_state_commands();
    rc |= test_file_network_and_import_commands();
    rc |= test_io_and_state_commands();
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    rc |= test_manual_tune_commands_commit_only_after_acceptance();
#endif
    if (rc == 0) {
        printf("DSD_APP_CMD_QUEUE: OK\n");
    }
    return rc;
}
