// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic queue-level contracts for app-control commands.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_types.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/app_control/commands_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

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
expect_command_status(const char* tag, dsd_app_command_token token, dsd_app_command_result_status want) {
    dsd_app_command_result result;
    DSD_MEMSET(&result, 0, sizeof(result));
    if (dsd_app_command_result_get(token, &result) != 0) {
        DSD_FPRINTF(stderr, "%s: result for token %llu not found\n", tag, (unsigned long long)token);
        return 1;
    }
    if (result.status != want) {
        DSD_FPRINTF(stderr, "%s: got status %d want %d (%s)\n", tag, (int)result.status, (int)want, result.message);
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
    return dsd_app_post_cmd(id, NULL, 0U);
}

static int
post_i32(int id, int32_t value) {
    return dsd_app_post_cmd(id, &value, sizeof(value));
}

static int
post_u32(int id, uint32_t value) {
    return dsd_app_post_cmd(id, &value, sizeof(value));
}

static int
post_u64(int id, uint64_t value) {
    return dsd_app_post_cmd(id, &value, sizeof(value));
}

static int
post_double(int id, double value) {
    return dsd_app_post_cmd(id, &value, sizeof(value));
}

static int
post_float(int id, float value) {
    return dsd_app_post_cmd(id, &value, sizeof(value));
}

static int
post_string(int id, const char* value) {
    return dsd_app_post_cmd(id, value, strlen(value) + 1U);
}

static int
post_host_port(int id, const char* host, int32_t port) {
    uint8_t payload[256 + sizeof(port)];
    DSD_MEMSET(payload, 0, sizeof(payload));
    DSD_SNPRINTF((char*)payload, 256U, "%s", host);
    DSD_MEMCPY(payload + 256U, &port, sizeof(port));
    return dsd_app_post_cmd(id, payload, sizeof(payload));
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
    return dsd_app_post_cmd(DSD_APP_CMD_KEY_HYTERA_SET, &payload, sizeof(payload));
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
    return dsd_app_post_cmd(DSD_APP_CMD_KEY_AES_SET, &payload, sizeof(payload));
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
    return dsd_app_post_cmd(DSD_APP_CMD_P25_P2_PARAMS_SET, &payload, sizeof(payload));
}

static int
post_call_alert_events(uint8_t events) {
    return dsd_app_post_cmd(DSD_APP_CMD_CALL_ALERT_EVENTS_SET, &events, sizeof(events));
}

static int
test_typed_command_api_wrappers(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);

    rc |= expect_int("typed action rejects setter", dsd_app_command_action(DSD_APP_CMD_GAIN_SET), -1);
    rc |= expect_int("typed i32 rejects action", dsd_app_command_set_i32(DSD_APP_CMD_TOGGLE_MUTE, 1), -1);
    rc |= expect_int("typed string rejects null", dsd_app_command_set_string(DSD_APP_CMD_INPUT_WAV_SET, NULL), -1);
    rc |= expect_int("typed endpoint rejects action",
                     dsd_app_command_set_endpoint(DSD_APP_CMD_TOGGLE_MUTE, "127.0.0.1", -1), -1);
    rc |= expect_int("typed udp input rejects null", dsd_app_command_set_udp_input(NULL, 0), -1);
    rc |= expect_int("typed p25 payload rejects null", dsd_app_command_set_p25_p2_params(NULL), -1);
    rc |= expect_int("typed hytera payload rejects null", dsd_app_command_set_hytera_key(NULL), -1);
    rc |= expect_int("typed aes payload rejects null", dsd_app_command_set_aes_key(NULL), -1);
    rc |= expect_int("typed dsp payload rejects null", dsd_app_command_dsp_op(NULL), -1);
    rc |= expect_int("typed config payload rejects null", dsd_app_command_apply_config(NULL), -1);

    dsd_app_p25_p2_params_payload p2 = {0xABCDEU, 0x123U, 0x456U};
    dsd_app_hytera_key_payload hytera = {0xAU, 1U, 2U, 3U, 4U};
    dsd_app_aes_key_payload aes = {9U, 10U, 11U, 12U};
    dsd_app_dsp_payload dsp = {0};

    rc |= expect_int("typed action posts", dsd_app_command_action(DSD_APP_CMD_UI_SHOW_CHANNELS_TOGGLE), 0);
    rc |= expect_int("typed gain posts", dsd_app_command_set_i32(DSD_APP_CMD_GAIN_SET, 5), 0);
    rc |= expect_int("typed gain coalesces to latest", dsd_app_command_set_i32(DSD_APP_CMD_GAIN_SET, 9), 0);
    rc |= expect_int("typed u8 posts", dsd_app_command_set_u8(DSD_APP_CMD_CALL_ALERT_EVENTS_SET, 3U), 0);
    rc |= expect_int("typed u32 posts", dsd_app_command_set_u32(DSD_APP_CMD_TG_HOLD_SET, 2468U), 0);
    rc |= expect_int("typed u64 posts", dsd_app_command_set_u64(DSD_APP_CMD_KEY_RC4DES_SET, 0x55U), 0);
    rc |= expect_int("typed double posts", dsd_app_command_set_double(DSD_APP_CMD_HANGTIME_SET, 3.5), 0);
    rc |= expect_int("typed float posts", dsd_app_command_set_float(DSD_APP_CMD_CONST_GATE_DELTA, 1.0f), 0);
    rc |= expect_int("typed string posts", dsd_app_command_set_string(DSD_APP_CMD_M17_USER_DATA_SET, "0,DST,SRC"), 0);
    rc |= expect_int("typed endpoint posts",
                     dsd_app_command_set_endpoint(DSD_APP_CMD_RIGCTL_CONNECT_CFG, "127.0.0.1", -1), 0);
    rc |= expect_int("typed p25 payload posts", dsd_app_command_set_p25_p2_params(&p2), 0);
    rc |= expect_int("typed hytera payload posts", dsd_app_command_set_hytera_key(&hytera), 0);
    rc |= expect_int("typed aes payload posts", dsd_app_command_set_aes_key(&aes), 0);
    rc |= expect_int("typed dsp payload posts", dsd_app_command_dsp_op(&dsp), 0);

    rc |= expect_int("typed wrappers applied with coalescing", dsd_app_drain_cmds(&opts, &state), 13);
    rc |= expect_int("typed action toggled channels", opts.frontend_display.show_channels, 1);
    rc |= expect_int("typed gain applied latest", (int)opts.audio_gain, 9);
    rc |= expect_u64("typed tg hold set", state.tg_hold, 2468ULL);
    rc |= expect_u64("typed rc4des key set", state.R, 0x55ULL);
    rc |= expect_true("typed hangtime set", opts.trunk_hangtime > 3.49 && opts.trunk_hangtime < 3.51);
    rc |= expect_str("typed m17 payload copied", state.m17dat, "0,DST,SRC");
    rc |= expect_u64("typed p2 wacn set", state.p2_wacn, 0xABCDEULL);
    rc |= expect_u64("typed p2 sysid set", state.p2_sysid, 0x123ULL);
    rc |= expect_u64("typed p2 cc set", state.p2_cc, 0x456ULL);
    rc |= expect_u64("typed aes key loaded", state.A1[0], 9ULL);
    rc |= expect_int("typed aes key load flag", state.aes_key_loaded[0], 1);

    freeState(&state);
    return rc;
}

static int
test_tracked_command_results(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);

    dsd_app_command_token gain_first = 0;
    dsd_app_command_token gain_second = 0;
    rc |= expect_int("tracked gain queued", dsd_app_command_set_i32_tracked(DSD_APP_CMD_GAIN_SET, 5, &gain_first),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_command_status("tracked gain first queued", gain_first, DSD_APP_COMMAND_RESULT_QUEUED);
    rc |= expect_int("tracked gain coalesced", dsd_app_command_set_i32_tracked(DSD_APP_CMD_GAIN_SET, 9, &gain_second),
                     DSD_APP_COMMAND_SUBMIT_COALESCED);
    rc |= expect_command_status("tracked gain second coalesced", gain_second, DSD_APP_COMMAND_RESULT_COALESCED);
    rc |= expect_int("tracked coalesced drain count", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("tracked coalesced latest value", (int)opts.audio_gain, 9);
    rc |= expect_command_status("tracked gain first completed", gain_first, DSD_APP_COMMAND_RESULT_COMPLETED);
    rc |= expect_command_status("tracked gain second remains coalesced", gain_second, DSD_APP_COMMAND_RESULT_COALESCED);

    uint8_t short_payload = 0xAAU;
    dsd_app_command_token invalid = 0;
    rc |= expect_int(
        "tracked invalid raw queued",
        dsd_app_command_submit_tracked(DSD_APP_CMD_KEY_AES_SET, &short_payload, sizeof short_payload, &invalid),
        DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tracked invalid drain count", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_command_status("tracked invalid payload result", invalid, DSD_APP_COMMAND_RESULT_INVALID_PAYLOAD);

    dsd_app_command_token unsupported = 0;
    rc |= expect_int("tracked unsupported raw queued", dsd_app_command_submit_tracked(999999, NULL, 0U, &unsupported),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tracked unsupported drain count", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_command_status("tracked unsupported result", unsupported, DSD_APP_COMMAND_RESULT_UNSUPPORTED);

    dsd_app_command_token backend_fail = 0;
    rc |=
        expect_int("tracked backend failure queued",
                   dsd_app_command_set_endpoint_tracked(DSD_APP_CMD_RIGCTL_CONNECT_CFG, "127.0.0.1", -1, &backend_fail),
                   DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tracked backend failure drain count", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_command_status("tracked backend failure result", backend_fail, DSD_APP_COMMAND_RESULT_FAILED);

    dsd_app_command_capability caps[8];
    size_t cap_count = 0;
    rc |= expect_int("capability count query", dsd_app_command_capabilities_get(NULL, 0U, &cap_count), 0);
    rc |= expect_true("capability count nonzero", cap_count > 8U);
    rc |= expect_int("capability truncated query", dsd_app_command_capabilities_get(caps, 8U, &cap_count), 1);

    dsd_app_command_descriptor descs[160];
    size_t desc_count = 0;
    rc |= expect_int("descriptor count query", dsd_app_command_descriptors_get(NULL, 0U, &desc_count), 0);
    rc |= expect_int("descriptor count parity", (int)desc_count, (int)cap_count);
    rc |= expect_true("descriptor test buffer large enough", desc_count <= sizeof descs / sizeof descs[0]);
    rc |= expect_int("descriptor full query", dsd_app_command_descriptors_get(descs, desc_count, &desc_count), 0);
    const dsd_app_command_descriptor* gain_desc = NULL;
    const dsd_app_command_descriptor* bw_desc = NULL;
    const dsd_app_command_descriptor* config_desc = NULL;
    const dsd_app_command_descriptor* metadata_desc = NULL;
    for (size_t i = 0; i < desc_count; i++) {
        rc |= expect_true("descriptor label present", descs[i].label != NULL && descs[i].label[0] != '\0');
        if (descs[i].command_id == DSD_APP_CMD_GAIN_SET) {
            gain_desc = &descs[i];
        } else if (descs[i].command_id == DSD_APP_CMD_RTL_SET_BW) {
            bw_desc = &descs[i];
        } else if (descs[i].command_id == DSD_APP_CMD_CONFIG_APPLY) {
            config_desc = &descs[i];
        } else if (descs[i].command_id == DSD_APP_CMD_CONFIG_METADATA_SET) {
            metadata_desc = &descs[i];
        }
    }
    rc |= expect_true("gain descriptor present", gain_desc != NULL);
    if (gain_desc) {
        rc |= expect_true("gain descriptor range", gain_desc->min_value == 0.0 && gain_desc->max_value == 50.0);
    }
    rc |= expect_true("rtl bandwidth descriptor present", bw_desc != NULL);
    if (bw_desc) {
        rc |= expect_true("rtl bandwidth enum options", bw_desc->enum_option_count >= 7U);
        rc |= expect_true("rtl bandwidth radio availability",
                          (bw_desc->availability_flags & DSD_APP_COMMAND_AVAIL_RADIO) != 0U);
        rc |= expect_int("rtl bandwidth restart hint", bw_desc->may_require_restart, 1);
    }
    rc |= expect_true("config descriptor present", config_desc != NULL);
    if (config_desc) {
        rc |= expect_int("config restart hint", config_desc->may_require_restart, 1);
    }
    rc |= expect_true("config metadata descriptor present", metadata_desc != NULL);
    if (metadata_desc) {
        rc |= expect_int("config metadata payload kind", metadata_desc->payload_kind, DSD_APP_COMMAND_PAYLOAD_STRUCT);
        rc |= expect_true("config metadata payload size",
                          metadata_desc->payload_size == sizeof(dsd_app_config_metadata_payload));
    }

    dsd_app_config_metadata_payload metadata;
    DSD_MEMSET(&metadata, 0, sizeof metadata);
    metadata.autosave_enabled = 1;
    DSD_SNPRINTF(metadata.path, sizeof metadata.path, "%s", "/tmp/queued-config.toml");
    dsd_app_command_token metadata_token = 0;
    rc |= expect_int("tracked config metadata queued",
                     dsd_app_command_set_config_metadata_tracked(&metadata, &metadata_token),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tracked config metadata drain count", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("tracked config metadata enabled", state.config_autosave_enabled, 1);
    rc |= expect_str("tracked config metadata path", state.config_autosave_path, "/tmp/queued-config.toml");
    rc |= expect_command_status("tracked config metadata result", metadata_token, DSD_APP_COMMAND_RESULT_COMPLETED);

    opts.frontend_kind = DSD_FRONTEND_TERMINAL;
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.frontend_kind = DSD_FRONTEND_NATIVE;
    cfg.frontend_kind_is_set = 1;
    dsd_app_command_token restart = 0;
    rc |= expect_int("tracked config restart queued", dsd_app_command_apply_config_tracked(&cfg, &restart),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tracked config restart drain count", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("tracked config preserves active frontend", opts.frontend_kind, DSD_FRONTEND_TERMINAL);
    rc |= expect_command_status("tracked config restart result", restart, DSD_APP_COMMAND_RESULT_RESTART_REQUIRED);

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

    uint8_t short_payload = 0xFFU;
    state.K = 0x99999999U;
    state.A1[0] = 0x55U;
    post_u32(DSD_APP_CMD_KEY_BASIC_SET, 0x01020304U);
    dsd_app_post_cmd(DSD_APP_CMD_KEY_AES_SET, &short_payload, sizeof(short_payload));
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
    const unsigned char symbol_data[] = {0x12U, 0x34U, 0x56U, 0x78U};

    remove(symbol_out);
    remove(symbol_in);
    remove(missing_csv);

    init_test_context(&opts, &state);

    rc |= write_file_bytes(symbol_in, symbol_data, sizeof(symbol_data));

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
    dsd_app_post_cmd(DSD_APP_CMD_UDP_INPUT_CFG, NULL, 0U);
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

    post_string(DSD_APP_CMD_EVENT_LOG_SET, "events.log");
    rc |= expect_int("event log set applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("event log path set", opts.event_out_file, "events.log");
    post_empty(DSD_APP_CMD_EVENT_LOG_DISABLE);
    rc |= expect_int("event log disable applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("event log disabled", opts.event_out_file, "");

    remove(symbol_out);
    remove(symbol_in);
    freeState(&state);
    return rc;
}

static int
test_io_and_legacy_state_commands(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);

    opts.slot1_on = 1;
    opts.slot2_on = 1;
    opts.slot_preference = 0;
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frontend_display.const_gate_other = 0.05f;
    opts.frontend_display.const_gate_qpsk = 0.10f;
    opts.frontend_display.const_norm_mode = 0;
    opts.frontend_display.eye_unicode = 0;
    opts.frontend_display.eye_color = 0;
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
    post_empty(DSD_APP_CMD_CRC_RELAX_TOGGLE);
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

    rc |= expect_int("io/legacy commands applied", dsd_app_drain_cmds(&opts, &state), 38);
    rc |= expect_str("udp input selected", opts.audio_in_dev, "udp");
    rc |= expect_int("udp input type", opts.audio_in_type, AUDIO_IN_UDP);
    rc |= expect_str("udp bind copied", opts.udp_in_bindaddr, "0.0.0.0");
    rc |= expect_int("udp port copied", opts.udp_in_portno, 7355);
    rc |= expect_int("compact toggled", opts.frontend_display.terminal_compact, 1);
    rc |= expect_int("slot1 disabled", opts.slot1_on, 0);
    rc |= expect_int("slot2 disabled", opts.slot2_on, 0);
    rc |= expect_int("slot preference cycled", opts.slot_preference, 1);
    rc |= expect_int("payload toggled", opts.payload, 1);
    rc |= expect_int("p25 ga toggled", opts.frontend_display.show_p25_group_affiliations, 1);
    rc |= expect_int("lpf toggled", opts.use_lpf, 1);
    rc |= expect_int("hpf toggled", opts.use_hpf, 1);
    rc |= expect_int("pbf toggled", opts.use_pbf, 1);
    rc |= expect_int("hpf-d toggled", opts.use_hpf_d, 1);
    rc |= expect_int("aggressive sync toggled twice", opts.aggressive_framesync, 0);
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
    rc |= expect_int("eye unicode toggled", opts.frontend_display.eye_unicode, 1);
    rc |= expect_int("eye color toggled", opts.frontend_display.eye_color, 1);
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
    rc |= expect_int("second legacy command group applied", dsd_app_drain_cmds(&opts, &state), 10);
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

int
main(void) {
    int rc = 0;
    rc |= test_typed_command_api_wrappers();
    rc |= test_tracked_command_results();
    rc |= test_visibility_and_queue_overflow();
    rc |= test_key_and_runtime_state_commands();
    rc |= test_file_network_and_import_commands();
    rc |= test_io_and_legacy_state_commands();
    if (rc == 0) {
        printf("DSD_APP_CMD_QUEUE: OK\n");
    }
    return rc;
}
