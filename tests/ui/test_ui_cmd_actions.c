// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic contracts for terminal UI command action registries.
 */

#include <dsd-neo/app_control/command_dispatch.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/services.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int g_open_audio_calls;
static int g_close_audio_calls;
static int g_open_audio_rc;
static int g_reset_history_calls;
static int g_disable_event_log_calls;
static int g_set_event_log_calls;
static int g_set_event_log_rc;
static char g_set_event_log_path[256];

int
openAudioOutput(dsd_opts* opts) {
    (void)opts;
    g_open_audio_calls++;
    return g_open_audio_rc;
}

void
closeAudioOutput(dsd_opts* opts) {
    (void)opts;
    g_close_audio_calls++;
}

void
svc_reset_event_history(dsd_state* state) {
    g_reset_history_calls++;
    if (state) {
        state->eh_index = 0;
        state->eh_slot = 0;
    }
}

void
svc_disable_event_log(dsd_opts* opts) {
    (void)opts;
    g_disable_event_log_calls++;
}

int
svc_set_event_log(dsd_opts* opts, const char* path) {
    (void)opts;
    g_set_event_log_calls++;
    DSD_SNPRINTF(g_set_event_log_path, sizeof(g_set_event_log_path), "%s", path ? path : "");
    return g_set_event_log_rc;
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
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
dispatch_one(const struct dsd_app_command_reg* regs, dsd_opts* opts, dsd_state* state,
             const struct dsd_app_command* cmd) {
    for (const struct dsd_app_command_reg* r = regs; r && r->fn; r++) {
        if (r->id == cmd->id) {
            return r->fn(opts, state, cmd);
        }
    }
    return 0;
}

static struct dsd_app_command
cmd_i32(int id, int32_t value) {
    struct dsd_app_command cmd;
    DSD_MEMSET(&cmd, 0, sizeof(cmd));
    cmd.id = id;
    cmd.n = sizeof(value);
    DSD_MEMCPY(cmd.data, &value, sizeof(value));
    return cmd;
}

static struct dsd_app_command
cmd_double(int id, double value) {
    struct dsd_app_command cmd;
    DSD_MEMSET(&cmd, 0, sizeof(cmd));
    cmd.id = id;
    cmd.n = sizeof(value);
    DSD_MEMCPY(cmd.data, &value, sizeof(value));
    return cmd;
}

static struct dsd_app_command
cmd_slot(int id, uint8_t slot) {
    struct dsd_app_command cmd;
    DSD_MEMSET(&cmd, 0, sizeof(cmd));
    cmd.id = id;
    cmd.n = 1;
    cmd.data[0] = slot;
    return cmd;
}

static struct dsd_app_command
cmd_path(int id, const char* path) {
    struct dsd_app_command cmd;
    size_t n = strlen(path);
    DSD_MEMSET(&cmd, 0, sizeof(cmd));
    cmd.id = id;
    cmd.n = n;
    DSD_MEMCPY(cmd.data, path, n);
    return cmd;
}

static int
test_audio_actions(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    struct dsd_app_command cmd;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    cmd = cmd_i32(DSD_APP_CMD_GAIN_SET, 60);
    rc |= expect_int("gain set dispatch", dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd), 1);
    rc |= expect_int("gain set clamps opts", opts.audio_gain, 50);
    rc |= expect_int("gain set mirrors right", opts.audio_gainR, 50);
    rc |= expect_int("gain set mirrors state", state.aout_gain, 50);
    cmd = cmd_i32(DSD_APP_CMD_GAIN_SET, -4);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("gain set clamps low opts", opts.audio_gain, 0);
    rc |= expect_int("gain set low keeps playback default", state.aout_gain, 25);
    opts.audio_gain = 49;
    cmd = cmd_i32(DSD_APP_CMD_GAIN_DELTA, 5);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("gain delta clamps high", opts.audio_gain, 50);

    cmd = cmd_i32(DSD_APP_CMD_GAIN_DELTA, -80);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("gain delta clamps opts", opts.audio_gain, 0);
    rc |= expect_int("zero gain keeps playback gain default left", state.aout_gain, 25);
    rc |= expect_int("zero gain keeps playback gain default right", state.aout_gainR, 25);

    opts.audio_gainA = 49;
    cmd = cmd_i32(DSD_APP_CMD_AGAIN_DELTA, 7);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("analog gain delta clamps high", opts.audio_gainA, 50);
    cmd = cmd_i32(DSD_APP_CMD_AGAIN_DELTA, -90);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("analog gain delta clamps low", opts.audio_gainA, 0);
    cmd = cmd_i32(DSD_APP_CMD_AGAIN_SET, -9);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("analog gain set clamps low", opts.audio_gainA, 0);
    cmd = cmd_i32(DSD_APP_CMD_AGAIN_SET, 60);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("analog gain set clamps high", opts.audio_gainA, 50);

    cmd = cmd_double(DSD_APP_CMD_INPUT_WARN_DB_SET, 12.5);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_true("input warn clamps high", opts.input_warn_db == 0.0);
    cmd = cmd_double(DSD_APP_CMD_INPUT_WARN_DB_SET, -500.0);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_true("input warn clamps low", opts.input_warn_db == -200.0);

    cmd.id = DSD_APP_CMD_INPUT_MONITOR_TOGGLE;
    cmd.n = 0;
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("input monitor toggle enables", opts.monitor_input_audio, 1);
    cmd.id = DSD_APP_CMD_COSINE_FILTER_TOGGLE;
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("cosine filter toggle enables", opts.use_cosine_filter, 1);

    cmd = cmd_i32(DSD_APP_CMD_INPUT_VOL_SET, 0);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("input volume set clamps low", opts.input_volume_multiplier, 1);
    cmd = cmd_i32(DSD_APP_CMD_INPUT_VOL_SET, 99);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("input volume set clamps high", opts.input_volume_multiplier, 16);
    opts.audio_in_type = 0;
    opts.input_volume_multiplier = 1;
    cmd.id = DSD_APP_CMD_INPUT_VOL_CYCLE;
    cmd.n = 0;
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("standard input volume cycles", opts.input_volume_multiplier, 2);
    rc |= expect_true("standard input volume toast", strstr(state.ui_msg, "Input Volume: 2X") != NULL);
    opts.input_volume_multiplier = 9;
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("standard input volume resets", opts.input_volume_multiplier, 1);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_volume_multiplier = 2;
    cmd.id = DSD_APP_CMD_INPUT_VOL_CYCLE;
    cmd.n = 0;
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("rtl input volume cycles", opts.rtl_volume_multiplier, 3);
    rc |= expect_true("rtl input volume toast", strstr(state.ui_msg, "RTL Monitor Gain: 3X") != NULL);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("rtl input volume resets", opts.rtl_volume_multiplier, 1);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_out = 0;
    opts.audio_out_type = 0;
    g_open_audio_calls = 0;
    g_close_audio_calls = 0;
    g_open_audio_rc = -1;
    cmd.id = DSD_APP_CMD_TOGGLE_MUTE;
    cmd.n = 0;
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("mute failed open leaves output off", opts.audio_out, 0);
    rc |= expect_int("mute failed open calls close", g_close_audio_calls, 1);
    rc |= expect_int("mute failed open calls open", g_open_audio_calls, 1);
    rc |= expect_true("mute failed open toast", strstr(state.ui_msg, "open failed") != NULL);
    g_open_audio_calls = 0;
    g_close_audio_calls = 0;
    g_open_audio_rc = 0;
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("mute successful open enables output", opts.audio_out, 1);
    rc |= expect_int("mute successful open calls close", g_close_audio_calls, 1);
    rc |= expect_int("mute successful open calls open", g_open_audio_calls, 1);
    rc |= expect_true("mute successful open toast", strstr(state.ui_msg, "Output: On") != NULL);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("mute toggle disables output", opts.audio_out, 0);
    rc |= expect_true("mute disabled toast", strstr(state.ui_msg, "Output: Muted") != NULL);

    return rc;
}

static int
test_trunk_actions(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    struct dsd_app_command cmd;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    cmd.id = DSD_APP_CMD_TRUNK_TOGGLE;
    cmd.n = 0;
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("trunk toggle enables p25 trunk", opts.p25_trunk, 1);
    rc |= expect_int("trunk toggle enables trunk tune", opts.trunk_enable, 1);
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("trunk toggle disables p25 trunk", opts.p25_trunk, 0);
    rc |= expect_int("trunk toggle disables trunk tune", opts.trunk_enable, 0);

    opts.p25_trunk = 1;
    opts.trunk_tune_group_calls = 1;
    cmd.id = DSD_APP_CMD_TRUNK_GROUP_TOGGLE;
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("group toggle works only while trunking", opts.trunk_tune_group_calls, 0);

    state.lasttg = 1234;
    cmd = cmd_slot(DSD_APP_CMD_TG_HOLD_TOGGLE, 0);
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("tg hold slot 0 uses lasttg", (int)state.tg_hold, 1234);
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("tg hold slot 0 clears", (int)state.tg_hold, 0);

    opts.frame_nxdn48 = 1;
    state.lasttg = 0;
    state.tg_hold = 0;
    state.nxdn_last_tg = 2345;
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("tg hold nxdn fallback", (int)state.tg_hold, 2345);

    opts.frame_nxdn48 = 0;
    opts.frame_provoice = 1;
    state.ea_mode = 0;
    state.tg_hold = 0;
    state.lastsrc = 2346;
    cmd = cmd_slot(DSD_APP_CMD_TG_HOLD_TOGGLE, 0);
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("tg hold provoice slot 0 fallback", (int)state.tg_hold, 2346);

    state.tg_hold = 0;
    state.lastsrcR = 3456;
    cmd = cmd_slot(DSD_APP_CMD_TG_HOLD_TOGGLE, 1);
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("tg hold provoice slot 1 fallback", (int)state.tg_hold, 3456);

    state.lasttgR = 4567;
    state.tg_hold = 0;
    opts.frame_provoice = 0;
    cmd = cmd_slot(DSD_APP_CMD_TG_HOLD_TOGGLE, 3);
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("tg hold slot 1 uses lasttgR", (int)state.tg_hold, 4567);
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("tg hold slot 1 clears", (int)state.tg_hold, 0);

    opts.frame_nxdn96 = 1;
    state.lasttgR = 0;
    state.tg_hold = 0;
    state.nxdn_last_tg = 5678;
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("tg hold slot 1 nxdn fallback", (int)state.tg_hold, 5678);
    opts.frame_nxdn96 = 0;

    cmd.id = DSD_APP_CMD_SCANNER_TOGGLE;
    cmd.n = 0;
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("scanner toggle enables scanner", opts.scanner_mode, 1);
    rc |= expect_int("scanner toggle disables trunking", opts.p25_trunk, 0);

    opts.trunk_use_allow_list = 0;
    opts.trunk_tune_private_calls = 1;
    opts.trunk_tune_data_calls = 0;
    opts.trunk_tune_enc_calls = 1;
    cmd.n = 0;
    cmd.id = DSD_APP_CMD_TRUNK_WLIST_TOGGLE;
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("trunk allow-list toggle enables", opts.trunk_use_allow_list, 1);
    cmd.id = DSD_APP_CMD_TRUNK_PRIV_TOGGLE;
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("trunk private toggle disables", opts.trunk_tune_private_calls, 0);
    cmd.id = DSD_APP_CMD_TRUNK_DATA_TOGGLE;
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("trunk data toggle enables", opts.trunk_tune_data_calls, 1);
    cmd.id = DSD_APP_CMD_TRUNK_ENC_TOGGLE;
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("trunk encrypted toggle disables", opts.trunk_tune_enc_calls, 0);

    return rc;
}

static int
test_radio_actions(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    struct dsd_app_command cmd;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    cmd = cmd_i32(DSD_APP_CMD_PPM_DELTA, -7);
    dispatch_one(dsd_app_actions_radio, &opts, &state, &cmd);
    rc |= expect_int("ppm delta applies without radio backend", opts.rtlsdr_ppm_error, -7);

    cmd.id = DSD_APP_CMD_INVERT_TOGGLE;
    cmd.n = 0;
    dispatch_one(dsd_app_actions_radio, &opts, &state, &cmd);
    rc |= expect_int("invert dmr", opts.inverted_dmr, 1);
    rc |= expect_int("invert dpmr", opts.inverted_dpmr, 1);
    rc |= expect_int("invert x2", opts.inverted_x2tdma, 1);
    rc |= expect_int("invert ysf", opts.inverted_ysf, 1);
    rc |= expect_int("invert m17", opts.inverted_m17, 1);

    cmd.id = DSD_APP_CMD_MOD_TOGGLE;
    dispatch_one(dsd_app_actions_radio, &opts, &state, &cmd);
    rc |= expect_int("mod toggle selects qpsk", opts.mod_qpsk, 1);
    rc |= expect_int("mod toggle clears c4fm", opts.mod_c4fm, 0);
    rc |= expect_int("mod toggle updates rf_mod", state.rf_mod, 1);
    rc |= expect_int("mod toggle p25p1 sps", state.samplesPerSymbol, 10);
    rc |= expect_int("mod toggle p25p1 center", state.symbolCenter, 4);
    dispatch_one(dsd_app_actions_radio, &opts, &state, &cmd);
    rc |= expect_int("mod toggle returns c4fm", opts.mod_c4fm, 1);
    rc |= expect_int("mod toggle clears qpsk", opts.mod_qpsk, 0);
    rc |= expect_int("mod toggle clears rf_mod", state.rf_mod, 0);

    cmd.id = DSD_APP_CMD_MOD_P2_TOGGLE;
    dispatch_one(dsd_app_actions_radio, &opts, &state, &cmd);
    rc |= expect_int("mod p2 toggle selects qpsk", opts.mod_qpsk, 1);
    rc |= expect_int("mod p2 toggle sets rf_mod", state.rf_mod, 1);
    rc |= expect_int("mod p2 toggle qpsk sps", state.samplesPerSymbol, 8);
    rc |= expect_int("mod p2 toggle qpsk center", state.symbolCenter, 3);
    dispatch_one(dsd_app_actions_radio, &opts, &state, &cmd);
    rc |= expect_int("mod p2 toggle returns c4fm", opts.mod_c4fm, 1);
    rc |= expect_int("mod p2 toggle p25p2 sps", state.samplesPerSymbol, 8);
    rc |= expect_int("mod p2 toggle p25p2 center", state.symbolCenter, 3);

    return rc;
}

static int
test_logging_actions(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    struct dsd_app_command cmd;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.eh_index = 254;
    cmd.id = DSD_APP_CMD_EH_NEXT;
    cmd.n = 0;
    dispatch_one(dsd_app_actions_logging, &opts, &state, &cmd);
    rc |= expect_int("event history next clamps", state.eh_index, 254);
    state.eh_index = 10;
    dispatch_one(dsd_app_actions_logging, &opts, &state, &cmd);
    rc |= expect_int("event history next increments", state.eh_index, 11);
    cmd.id = DSD_APP_CMD_EH_PREV;
    dispatch_one(dsd_app_actions_logging, &opts, &state, &cmd);
    rc |= expect_int("event history prev decrements", state.eh_index, 10);

    state.eh_slot = 0;
    state.eh_index = 9;
    cmd.id = DSD_APP_CMD_EH_TOGGLE_SLOT;
    dispatch_one(dsd_app_actions_logging, &opts, &state, &cmd);
    rc |= expect_int("event history slot cycles to slot 1", state.eh_slot, 1);
    rc |= expect_int("event history slot 1 resets index", state.eh_index, 0);
    state.eh_index = 8;
    dispatch_one(dsd_app_actions_logging, &opts, &state, &cmd);
    rc |= expect_int("event history slot cycles to slot 2", state.eh_slot, 2);
    rc |= expect_int("event history slot 2 resets index", state.eh_index, 0);
    state.eh_index = 7;
    dispatch_one(dsd_app_actions_logging, &opts, &state, &cmd);
    rc |= expect_int("event history slot cycles to all", state.eh_slot, 0);
    rc |= expect_int("event history slot all resets index", state.eh_index, 0);

    DSD_SNPRINTF(state.ui_msg, sizeof(state.ui_msg), "stale");
    state.ui_msg_expire = 99;
    cmd.id = DSD_APP_CMD_UI_MSG_CLEAR;
    dispatch_one(dsd_app_actions_logging, &opts, &state, &cmd);
    rc |= expect_true("ui message clear empties toast", state.ui_msg[0] == '\0');
    rc |= expect_int("ui message clear expires", (int)state.ui_msg_expire, 0);

    g_reset_history_calls = 0;
    cmd.id = DSD_APP_CMD_EH_RESET;
    dispatch_one(dsd_app_actions_logging, &opts, &state, &cmd);
    rc |= expect_int("event history reset service called", g_reset_history_calls, 1);
    rc |= expect_true("event history reset toast", strstr(state.ui_msg, "Event history reset") != NULL);

    g_disable_event_log_calls = 0;
    cmd.id = DSD_APP_CMD_EVENT_LOG_DISABLE;
    dispatch_one(dsd_app_actions_logging, &opts, &state, &cmd);
    rc |= expect_int("event log disable service called", g_disable_event_log_calls, 1);
    rc |= expect_true("event log disable toast", strstr(state.ui_msg, "Event log disabled") != NULL);

    g_set_event_log_calls = 0;
    g_set_event_log_rc = 0;
    cmd = cmd_path(DSD_APP_CMD_EVENT_LOG_SET, "/tmp/ui-actions.log");
    dispatch_one(dsd_app_actions_logging, &opts, &state, &cmd);
    rc |= expect_int("event log set service called", g_set_event_log_calls, 1);
    rc |= expect_true("event log set path passed", strcmp(g_set_event_log_path, "/tmp/ui-actions.log") == 0);
    rc |= expect_true("event log set toast", strstr(state.ui_msg, "Event log -> /tmp/ui-actions.log") != NULL);

    g_set_event_log_rc = -1;
    cmd = cmd_path(DSD_APP_CMD_EVENT_LOG_SET, "/bad");
    dispatch_one(dsd_app_actions_logging, &opts, &state, &cmd);
    rc |= expect_true("event log set failure toast", strstr(state.ui_msg, "path invalid") != NULL);

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_audio_actions();
    rc |= test_trunk_actions();
    rc |= test_radio_actions();
    rc |= test_logging_actions();
    if (rc == 0) {
        printf("DSD_APP_CMD_ACTIONS: OK\n");
    }
    return rc;
}
