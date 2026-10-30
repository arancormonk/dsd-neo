// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic contracts for terminal UI command action registries.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "command_dispatch.h"
#include "services.h"

#include "dsd-neo/core/csv_validate.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/runtime/log.h>

void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    (void)level;
    (void)format;
}

/* The scan key swap links here through actions_trunk.c; this suite never loads
 * per-row key files, so the path wrappers stay refused. */
int
csvKeyImportHexPath(const char* path, int show_keys, dsd_state* state, dsd_csv_validation* stats) {
    (void)path;
    (void)show_keys;
    (void)state;
    (void)stats;
    return -1;
}

int
csvKeyImportDecPath(const char* path, int show_keys, dsd_state* state, dsd_csv_validation* stats) {
    (void)path;
    (void)show_keys;
    (void)state;
    (void)stats;
    return -1;
}

static int g_open_audio_calls;
static int g_guard_enters;
static int g_guard_leaves;
static int g_guard_depth;
static int g_guard_errors;
static dsd_opts* g_guard_opts;
static dsd_state* g_guard_state;

void
p25_sm_tick_guard_enter(void) {
    ++g_guard_enters;
    g_guard_errors += g_guard_depth++ != 0;
    if (g_guard_state) {
        g_guard_errors += dsd_scan_mode_configured_view(g_guard_state) == NULL;
    }
}

void
p25_sm_tick_guard_leave(void) {
    ++g_guard_leaves;
    g_guard_errors += --g_guard_depth != 0;
    if (g_guard_state) {
        g_guard_errors += dsd_scan_mode_configured_view(g_guard_state) != NULL;
        g_guard_errors += g_guard_opts->scanner_mode != 0 || g_guard_opts->trunk_enable != 1;
    }
}

static int g_close_audio_calls;
static int g_open_audio_rc;
static int g_reset_history_calls;
static int g_disable_event_log_calls;
static int g_set_event_log_calls;
static int g_set_event_log_rc;
static char g_set_event_log_path[256];

int
openAudioOutput(dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    g_open_audio_calls++;
    return g_open_audio_rc;
}

/* The analog sink Input Monitor turns on: a session that did not start with -8 opened none (Auto included). */
static int g_ensure_analog_calls;
static int g_ensure_analog_rc;

int
dsd_audio_ensure_analog_output(dsd_opts* opts) {
    (void)opts;
    g_ensure_analog_calls++;
    return g_ensure_analog_rc;
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

/* The trunk-scan pause and resume services (menu_services.c, not linked here; APP_COMMAND_QUEUE drives the real ones
   over the coordinator): a coordinator stand-in with targets while state->trunk_scan_target_count is non-zero, paused
   and resumed through the flags the real one publishes. A resume answers g_trunk_resume_rc (0 running, 1 running with
   the park in flight, -3 refused and still paused). */
static int g_trunk_pause_calls;
static int g_trunk_resume_calls;
static int g_trunk_resume_rc;
static int g_trunk_pause_fails; /* a coordinator whose pause fails: it names no target to leave */

int
svc_pause_trunk_scan_in_place(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    g_trunk_pause_calls++;
    if (state->trunk_scan_target_count == 0U || g_trunk_pause_fails) {
        return -1;
    }
    state->trunk_scan_paused = 1U;
    state->operator_tuned = 1U;
    return 0;
}

int
svc_resume_trunk_scan(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    g_trunk_resume_calls++;
    if (g_trunk_resume_rc == 0 || g_trunk_resume_rc == 1) {
        state->trunk_scan_paused = 0U;
        state->operator_tuned = 0U;
    }
    return g_trunk_resume_rc;
}

/* The next-start check (menu_services.c, not linked here; APP_COMMAND_QUEUE drives the real one): passes unless
   g_next_start_why is set, and records the save the change leaves from an empty one. */
static const char* g_next_start_why;
static int g_next_start_calls;
static dsdneoUserConfig g_next_start_save;

int
svc_check_next_start(const dsd_opts* opts, const dsd_state* state, svc_next_start_change_fn change, const void* ctx,
                     char* why, size_t why_size) {
    (void)opts;
    (void)state;
    g_next_start_calls++;
    DSD_MEMSET(&g_next_start_save, 0, sizeof g_next_start_save);
    change(&g_next_start_save, ctx);
    if (!g_next_start_why) {
        return 0;
    }
    DSD_SNPRINTF(why, why_size, "%s", g_next_start_why);
    return -1;
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

    /* The control API can send any int32 as a delta: no overflow on the way to the clamp. */
    cmd = cmd_i32(DSD_APP_CMD_GAIN_DELTA, INT32_MAX);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("gain delta INT32_MAX clamps high", opts.audio_gain, 50);
    cmd = cmd_i32(DSD_APP_CMD_GAIN_DELTA, INT32_MIN);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("gain delta INT32_MIN clamps low", opts.audio_gain, 0);
    opts.audio_gain = 10;

    cmd = cmd_i32(DSD_APP_CMD_GAIN_DELTA, -80);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("gain delta clamps opts", opts.audio_gain, 0);
    rc |= expect_int("zero gain keeps playback gain default left", state.aout_gain, 25);
    rc |= expect_int("zero gain keeps playback gain default right", state.aout_gainR, 25);

    /* Analog gain is 0..100 everywhere (-n, the menu prompt, the * and / keys): a keypress from -n 100 used to land
       on 50 (issue #518 follow-up). */
    opts.audio_gainA = 100.0f;
    cmd = cmd_i32(DSD_APP_CMD_AGAIN_DELTA, 1);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_true("analog gain delta keeps 100 at the top", fabsf(opts.audio_gainA - 100.0f) < 1e-6f);
    cmd = cmd_i32(DSD_APP_CMD_AGAIN_DELTA, -1);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_true("analog gain delta steps down from 100", fabsf(opts.audio_gainA - 99.0f) < 1e-6f);
    opts.audio_gainA = 97.0f;
    cmd = cmd_i32(DSD_APP_CMD_AGAIN_DELTA, 7);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_true("analog gain delta clamps high", fabsf(opts.audio_gainA - 100.0f) < 1e-6f);
    opts.audio_gainA = 37.5f;
    cmd = cmd_i32(DSD_APP_CMD_AGAIN_DELTA, 1);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_true("analog gain delta keeps a fractional -n", fabsf(opts.audio_gainA - 38.5f) < 1e-6f);
    cmd = cmd_i32(DSD_APP_CMD_AGAIN_DELTA, -90);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_true("analog gain delta below 1 goes to auto", fabsf(opts.audio_gainA) < 1e-6f);
    /* 0 is auto (the AGC): a step from it starts at the reference setting, 50. */
    cmd = cmd_i32(DSD_APP_CMD_AGAIN_DELTA, 1);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_true("analog gain step up from auto starts at 50", fabsf(opts.audio_gainA - 51.0f) < 1e-6f);
    opts.audio_gainA = 0.0f;
    cmd = cmd_i32(DSD_APP_CMD_AGAIN_DELTA, -1);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_true("analog gain step down from auto starts at 50", fabsf(opts.audio_gainA - 49.0f) < 1e-6f);
    opts.audio_gainA = 1.0f;
    cmd = cmd_i32(DSD_APP_CMD_AGAIN_DELTA, -1);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_true("analog gain step down from 1 goes to auto", fabsf(opts.audio_gainA) < 1e-6f);
    cmd = cmd_i32(DSD_APP_CMD_AGAIN_SET, 0);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_true("analog gain set 0 is auto", fabsf(opts.audio_gainA) < 1e-6f);
    cmd = cmd_i32(DSD_APP_CMD_AGAIN_SET, -9);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_true("analog gain set clamps low", fabsf(opts.audio_gainA) < 1e-6f);
    cmd = cmd_i32(DSD_APP_CMD_AGAIN_SET, 80);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_true("analog gain set accepts 80", fabsf(opts.audio_gainA - 80.0f) < 1e-6f);
    cmd = cmd_i32(DSD_APP_CMD_AGAIN_SET, 150);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_true("analog gain set clamps high", fabsf(opts.audio_gainA - 100.0f) < 1e-6f);

    cmd = cmd_double(DSD_APP_CMD_INPUT_WARN_DB_SET, 12.5);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_true("input warn clamps high", opts.input_warn_db == 0.0);
    cmd = cmd_double(DSD_APP_CMD_INPUT_WARN_DB_SET, -500.0);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_true("input warn clamps low", opts.input_warn_db == -200.0);

    cmd.id = DSD_APP_CMD_INPUT_MONITOR_TOGGLE;
    cmd.n = 0;
    g_ensure_analog_calls = 0;
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("input monitor toggle enables", opts.monitor_input_audio, 1);
    rc |= expect_int("input monitor enable opens its sink", g_ensure_analog_calls, 1);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("input monitor toggle disables", opts.monitor_input_audio, 0);
    rc |= expect_int("input monitor disable opens nothing", g_ensure_analog_calls, 1);
    /* A sink that cannot open leaves the monitor off and says so. */
    g_ensure_analog_rc = -1;
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    g_ensure_analog_rc = 0;
    rc |= expect_int("input monitor stays off without a sink", opts.monitor_input_audio, 0);
    rc |= expect_true("input monitor sink failure is said", strstr(state.ui_msg, "Input Monitor") != NULL);
    dispatch_one(dsd_app_actions_audio, &opts, &state, &cmd);
    rc |= expect_int("input monitor enables once the sink opens", opts.monitor_input_audio, 1);
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
seed_voice_call_ids(dsd_state* state, uint8_t slot, int protocol, uint64_t target, uint64_t source) {
    dsd_call_observation observation = {0};
    observation.protocol = protocol;
    observation.slot = slot;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = target;
    observation.policy_target_id = target;
    observation.ota_source_id = source;
    return dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN);
}

static int
seed_voice_call(dsd_state* state, uint8_t slot, int protocol, uint64_t target) {
    return seed_voice_call_ids(state, slot, protocol, target, 0U);
}

static int
test_trunk_ownership_guard(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    for (int toggle = 0; toggle <= 1; ++toggle) {
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.scanner_mode = 1;
        rc |= expect_int("seed conventional row scope", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_P25), 0);
        g_guard_enters = g_guard_leaves = g_guard_depth = g_guard_errors = 0;
        g_guard_opts = &opts;
        g_guard_state = &state;
        struct dsd_app_command cmd = cmd_i32(toggle ? DSD_APP_CMD_TRUNK_TOGGLE : DSD_APP_CMD_TRUNK_SET, 1);
        dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
        rc |= expect_int("trunk on enters once", g_guard_enters, 1);
        rc |= expect_int("trunk on leaves once", g_guard_leaves, 1);
        rc |= expect_int("trunk on guards row leave and ownership assignments", g_guard_errors, 0);
        rc |= expect_true("trunk on clears scanner", opts.trunk_enable == 1 && opts.scanner_mode == 0);
        cmd = cmd_i32(toggle ? DSD_APP_CMD_TRUNK_TOGGLE : DSD_APP_CMD_TRUNK_SET, 0);
        dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
        rc |= expect_int("trunk off never enters", g_guard_enters, 1);
        rc |= expect_int("trunk off never leaves", g_guard_leaves, 1);
        rc |= expect_int("trunk off disables trunking", opts.trunk_enable, 0);
        g_guard_opts = NULL;
        g_guard_state = NULL;
    }
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
    rc |= expect_int("trunk toggle enables trunking", opts.trunk_enable, 1);
    rc |= expect_int("trunk toggle enables trunk tune", opts.trunk_enable, 1);
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("trunk toggle disables trunking", opts.trunk_enable, 0);
    rc |= expect_int("trunk toggle disables trunk tune", opts.trunk_enable, 0);

    opts.trunk_enable = 1;
    opts.trunk_tune_group_calls = 1;
    cmd.id = DSD_APP_CMD_TRUNK_GROUP_TOGGLE;
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("group toggle works only while trunking", opts.trunk_tune_group_calls, 0);

    rc |= !seed_voice_call(&state, 0, DSD_SYNC_DMR_BS_VOICE_POS, 1234);
    cmd = cmd_slot(DSD_APP_CMD_TG_HOLD_TOGGLE, 0);
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("tg hold slot 0 uses lasttg", (int)state.tg_hold, 1234);
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("tg hold slot 0 clears", (int)state.tg_hold, 0);

    opts.frame_nxdn48 = 1;
    state.tg_hold = 0;
    rc |= !seed_voice_call(&state, 0, DSD_SYNC_NXDN_POS, 2345);
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("tg hold nxdn fallback", (int)state.tg_hold, 2345);

    opts.frame_nxdn48 = 0;
    opts.frame_provoice = 1;
    state.ea_mode = 0;
    state.tg_hold = 0;
    rc |= !seed_voice_call_ids(&state, 0, DSD_SYNC_PROVOICE_POS, 0U, 2346U);
    cmd = cmd_slot(DSD_APP_CMD_TG_HOLD_TOGGLE, 0);
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("tg hold provoice slot 0 source fallback", (int)state.tg_hold, 2346);

    state.tg_hold = 0;
    rc |= !seed_voice_call_ids(&state, 1, DSD_SYNC_PROVOICE_POS, 0U, 3456U);
    cmd = cmd_slot(DSD_APP_CMD_TG_HOLD_TOGGLE, 1);
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("tg hold provoice slot 1 source fallback", (int)state.tg_hold, 3456);

    state.tg_hold = 0;
    opts.frame_provoice = 0;
    rc |= !seed_voice_call(&state, 1, DSD_SYNC_DMR_BS_VOICE_POS, 4567);
    cmd = cmd_slot(DSD_APP_CMD_TG_HOLD_TOGGLE, 3);
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("tg hold slot 1 uses lasttgR", (int)state.tg_hold, 4567);
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("tg hold slot 1 clears", (int)state.tg_hold, 0);

    opts.frame_nxdn96 = 1;
    state.tg_hold = 0;
    rc |= !seed_voice_call(&state, 1, DSD_SYNC_NXDN_POS, 5678);
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("tg hold slot 1 nxdn fallback", (int)state.tg_hold, 5678);
    opts.frame_nxdn96 = 0;

    cmd.id = DSD_APP_CMD_SCANNER_TOGGLE;
    cmd.n = 0;
    dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd);
    rc |= expect_int("scanner toggle enables scanner", opts.scanner_mode, 1);
    rc |= expect_int("scanner toggle disables trunking", opts.trunk_enable, 0);

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
    /* Any int32 delta saturates rather than overflowing. */
    opts.rtlsdr_ppm_error = INT32_MAX - 1;
    cmd = cmd_i32(DSD_APP_CMD_PPM_DELTA, INT32_MAX);
    dispatch_one(dsd_app_actions_radio, &opts, &state, &cmd);
    rc |= expect_int("ppm delta saturates high", opts.rtlsdr_ppm_error, INT32_MAX);
    opts.rtlsdr_ppm_error = INT32_MIN + 1;
    cmd = cmd_i32(DSD_APP_CMD_PPM_DELTA, INT32_MIN);
    dispatch_one(dsd_app_actions_radio, &opts, &state, &cmd);
    rc |= expect_int("ppm delta saturates low", opts.rtlsdr_ppm_error, INT32_MIN);
    opts.rtlsdr_ppm_error = -7;

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

    /* The generic hotkey must leave D-STAR's same-rate binary gate immediately. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.mod_gfsk = 1;
    opts.mod_p25p2_c4fm = 1;
    opts.mod_p25p2_profile_lock = 1;
    state.rf_mod = 2;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state.sps_hunt_counter = 9;
    dispatch_one(dsd_app_actions_radio, &opts, &state, &cmd);
    rc |= expect_int("generic mod toggle returns c4fm", opts.mod_c4fm, 1);
    rc |= expect_int("generic mod toggle selects four-level profile", state.sps_hunt_idx,
                     DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    rc |= expect_int("generic mod toggle resets profile dwell", state.sps_hunt_counter, 0);
    rc |= expect_int("generic mod toggle clears p25p2 c4fm mode", opts.mod_p25p2_c4fm, 0);
    rc |= expect_int("generic mod toggle clears p25p2 profile lock", opts.mod_p25p2_profile_lock, 0);

    /* Generic modulation transitions must retain timing from the active PCM source. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 96000;
    opts.rtl_dsp_bw_khz = 48;
    opts.mod_qpsk = 1;
    state.rf_mod = 1;
    dispatch_one(dsd_app_actions_radio, &opts, &state, &cmd);
    rc |= expect_int("96k WAV generic toggle returns c4fm", opts.mod_c4fm, 1);
    rc |= expect_int("96k WAV generic toggle timing", state.samplesPerSymbol, 20);
    rc |= expect_int("96k WAV generic toggle center", state.symbolCenter, 9);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.sps_hunt_counter = 11;
    cmd.id = DSD_APP_CMD_MOD_P2_TOGGLE;
    dispatch_one(dsd_app_actions_radio, &opts, &state, &cmd);
    rc |= expect_int("mod p2 toggle selects qpsk", opts.mod_qpsk, 1);
    rc |= expect_int("mod p2 toggle sets rf_mod", state.rf_mod, 1);
    rc |= expect_int("mod p2 toggle locks explicit qpsk", opts.mod_cli_lock, 1);
    rc |= expect_int("mod p2 toggle locks p25p2 profile", opts.mod_p25p2_profile_lock, 1);
    rc |= expect_int("mod p2 toggle qpsk sps", state.samplesPerSymbol, 8);
    rc |= expect_int("mod p2 toggle qpsk center", state.symbolCenter, 3);
    rc |= expect_int("mod p2 toggle selects profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    rc |= expect_int("mod p2 toggle resets profile dwell", state.sps_hunt_counter, 0);
    opts.mod_cli_lock = 0;
    dispatch_one(dsd_app_actions_radio, &opts, &state, &cmd);
    rc |= expect_int("mod p2 toggle returns c4fm", opts.mod_c4fm, 1);
    rc |= expect_int("mod p2 toggle locks explicit c4fm", opts.mod_cli_lock, 1);
    rc |= expect_int("mod p2 toggle p25p2 sps", state.samplesPerSymbol, 8);
    rc |= expect_int("mod p2 toggle p25p2 center", state.symbolCenter, 3);

    /* At low rates P25P1 and P25P2 can round to the same SPS, so the explicit
     * profile selection—not timing inference—must carry the hotkey choice. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_dsp_bw_khz = 16;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.sps_hunt_counter = 7;
    dispatch_one(dsd_app_actions_radio, &opts, &state, &cmd);
    rc |= expect_int("low-rate mod p2 toggle sps", state.samplesPerSymbol, 3);
    rc |= expect_int("low-rate mod p2 toggle profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    rc |= expect_int("low-rate mod p2 toggle dwell", state.sps_hunt_counter, 0);

    /* The generic modulation hotkey fully exits the P25p2 helper. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    cmd.id = DSD_APP_CMD_MOD_P2_TOGGLE;
    dispatch_one(dsd_app_actions_radio, &opts, &state, &cmd);
    cmd.id = DSD_APP_CMD_MOD_TOGGLE;
    dispatch_one(dsd_app_actions_radio, &opts, &state, &cmd);
    rc |= expect_int("generic toggle exits p25p2 helper lock", opts.mod_cli_lock, 0);
    rc |= expect_int("generic toggle exits p25p2 helper profile lock", opts.mod_p25p2_profile_lock, 0);
    rc |= expect_int("generic toggle exits p25p2 c4fm helper", opts.mod_p25p2_c4fm, 0);
    rc |= expect_int("generic toggle restores p25p1 profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    rc |= expect_int("generic toggle restores p25p1 timing", state.samplesPerSymbol, 10);
    rc |= expect_int("generic toggle restores p25p1 center", state.symbolCenter, 4);
    rc |= expect_int("generic toggle restores c4fm", state.rf_mod, 0);

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

/* Issue #671: `y` and DSD_APP_CMD_SCAN_RESUME. A running trunk scan pauses where it is and resumes on `y`; a paused or
   resume-pending -Y scan resumes (a second `y` while its tune is pending changes nothing); a running -Y scan still
   stops and a stopped one starts, the tuner going back to the scan. SCAN_RESUME resumes only a paused scan. */
static int
test_scanner_toggle_pauses_and_resumes(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    struct dsd_app_command y = {0};
    y.id = DSD_APP_CMD_SCANNER_TOGGLE;
    struct dsd_app_command resume = {0};
    resume.id = DSD_APP_CMD_SCAN_RESUME;

    /* Trunk scan: y pauses the running scan in place, then resumes it. */
    opts.trunk_scan_enabled = 1;
    state.trunk_scan_target_count = 2U;
    g_trunk_pause_calls = g_trunk_resume_calls = 0;
    g_trunk_resume_rc = 0;
    rc |= expect_int("y pauses a running trunk scan", dispatch_one(dsd_app_actions_trunk, &opts, &state, &y), 1);
    rc |= expect_true("trunk scan paused", g_trunk_pause_calls == 1 && state.trunk_scan_paused == 1U);
    rc |= expect_true("trunk scan paused toast", strcmp(state.ui_msg, "Trunk scan paused") == 0);
    rc |= expect_int("y resumes the paused trunk scan", dispatch_one(dsd_app_actions_trunk, &opts, &state, &y), 1);
    rc |= expect_true("trunk scan resumed", g_trunk_resume_calls == 1 && state.trunk_scan_paused == 0U);
    rc |= expect_true("trunk scan resumed toast", strcmp(state.ui_msg, "Trunk scan resumed") == 0);
    /* A resume whose park is still in flight runs: the same answer. */
    state.trunk_scan_paused = 1U;
    g_trunk_resume_rc = 1;
    rc |= expect_int("pending park resume", dispatch_one(dsd_app_actions_trunk, &opts, &state, &resume), 1);
    rc |= expect_true("pending park resume toast", strcmp(state.ui_msg, "Trunk scan resumed") == 0);
    /* A refused resume says why itself (the coordinator's toast) and fails the command. */
    state.trunk_scan_paused = 1U;
    g_trunk_resume_rc = -3;
    state.ui_msg[0] = '\0';
    rc |= expect_int("refused trunk resume", dispatch_one(dsd_app_actions_trunk, &opts, &state, &resume), 2);
    rc |= expect_true("refused trunk resume stays paused", state.trunk_scan_paused == 1U && state.ui_msg[0] == '\0');
    state.trunk_scan_paused = 0U;
    rc |= expect_int("resume of a running trunk scan", dispatch_one(dsd_app_actions_trunk, &opts, &state, &resume), 2);
    rc |= expect_true("running trunk scan toast", strcmp(state.ui_msg, "Not paused: the scan is running") == 0);
    /* Final review, Minor 5: a coordinator whose pause fails is still a trunk scan, and `y` says so; it used to toast
       the conventional scanner, which `y` never asked for here. */
    g_trunk_pause_fails = 1;
    rc |=
        expect_int("y over a coordinator that cannot pause", dispatch_one(dsd_app_actions_trunk, &opts, &state, &y), 2);
    rc |= expect_true("coordinator that cannot pause toast",
                      strcmp(state.ui_msg, "Trunk scan active: pause unavailable") == 0);
    rc |= expect_true("coordinator that cannot pause changes nothing",
                      state.trunk_scan_paused == 0U && opts.scanner_mode == 0);
    g_trunk_pause_fails = 0;
    /* No coordinator behind the flag: the toggle is refused as before. */
    state.trunk_scan_target_count = 0U;
    rc |= expect_int("y with no coordinator", dispatch_one(dsd_app_actions_trunk, &opts, &state, &y), 2);
    rc |= expect_true("no coordinator toast",
                      strcmp(state.ui_msg, "Trunk scan active: conventional scanner unavailable") == 0);
    rc |= expect_int("no -Y under trunk scan", opts.scanner_mode, 0);
    opts.trunk_scan_enabled = 0;

    /* -Y: a resume still waiting on its tune answers again without another resume. */
    state.lcn_scan_paused = 1U;
    state.lcn_scan_resume_pending = 1U;
    rc |= expect_int("y while a resume is pending", dispatch_one(dsd_app_actions_trunk, &opts, &state, &y), 1);
    rc |= expect_true("pending toast", strcmp(state.ui_msg, "Resuming scan: waiting for the tune") == 0);
    rc |= expect_true("still pending", state.lcn_scan_resume_pending == 1U && opts.scanner_mode == 0);
    /* A paused list resumes through the engine (here, the stubs' list has no row: the scan stays paused). */
    state.lcn_scan_resume_pending = 0U;
    rc |= expect_int("y on a paused list", dispatch_one(dsd_app_actions_trunk, &opts, &state, &y), 2);
    rc |= expect_true("no row: still paused", state.lcn_scan_paused == 1U && opts.scanner_mode == 0);

    /* Not paused: y starts the scanner and stops it. The tuner stays the operator's in the record: only a resume, or a
       new session, clears that (issue #671 ruling). */
    state.lcn_scan_paused = 0U;
    state.operator_tuned = 1U;
    rc |= expect_int("resume with nothing paused", dispatch_one(dsd_app_actions_trunk, &opts, &state, &resume), 2);
    rc |= expect_true("nothing paused toast", strcmp(state.ui_msg, "Not scanning: no paused scan to resume") == 0);
    rc |= expect_int("y starts", dispatch_one(dsd_app_actions_trunk, &opts, &state, &y), 1);
    rc |= expect_true("started; operator_tuned kept", opts.scanner_mode == 1 && state.operator_tuned == 1U);
    rc |= expect_int("resume of a running list", dispatch_one(dsd_app_actions_trunk, &opts, &state, &resume), 2);
    rc |= expect_true("running list toast", strcmp(state.ui_msg, "Not paused: the scan is running") == 0);
    rc |= expect_int("y stops", dispatch_one(dsd_app_actions_trunk, &opts, &state, &y), 1);
    rc |= expect_int("stopped", opts.scanner_mode, 0);
    return rc;
}

/* Issue #671 (B5): trunking is not the operator's to switch while a trunk-scan coordinator exists, running or paused;
   each target sets its own. A paused -Y scan ends when trunking takes the tuner, and stays paused when it is switched
   off. */
static int
test_trunking_under_a_scan(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    for (int paused = 0; paused <= 1; paused++) {
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.trunk_scan_enabled = 1;
        state.trunk_scan_target_count = 2U;
        opts.trunk_enable = paused ? 0 : 1;
        state.trunk_scan_paused = (uint8_t)paused;
        const int before = opts.trunk_enable;
        struct dsd_app_command cmd = {0};
        cmd.id = DSD_APP_CMD_TRUNK_TOGGLE;
        rc |= expect_int("t refused under trunk scan", dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd), 2);
        rc |= expect_int("t leaves trunking", opts.trunk_enable, before);
        rc |=
            expect_true("t refusal toast", strcmp(state.ui_msg, "Trunk scan: each target sets its own trunking") == 0);
        for (int32_t on = 0; on <= 1; on++) {
            cmd = cmd_i32(DSD_APP_CMD_TRUNK_SET, on);
            state.ui_msg[0] = '\0';
            rc |= expect_int("trunk set refused under trunk scan",
                             dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd), 2);
            rc |= expect_int("trunk set leaves trunking", opts.trunk_enable, before);
            rc |= expect_true("trunk set refusal toast",
                              strcmp(state.ui_msg, "Trunk scan: each target sets its own trunking") == 0);
        }
    }

    /* The flag with no coordinator behind it (none was set up): trunking is the operator's to switch. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.trunk_scan_enabled = 1;
    struct dsd_app_command t = {0};
    t.id = DSD_APP_CMD_TRUNK_TOGGLE;
    rc |= expect_int("t with no coordinator", dispatch_one(dsd_app_actions_trunk, &opts, &state, &t), 1);
    rc |= expect_int("t with no coordinator switches trunking", opts.trunk_enable, 1);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.lcn_scan_paused = 1U;
    state.operator_tuned = 1U;
    struct dsd_app_command cmd = cmd_i32(DSD_APP_CMD_TRUNK_SET, 0);
    rc |= expect_int("trunk off under a paused list", dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd), 1);
    rc |= expect_true("trunk off keeps the pause", state.lcn_scan_paused == 1U && opts.trunk_enable == 0);
    cmd = cmd_i32(DSD_APP_CMD_TRUNK_SET, 1);
    rc |= expect_int("trunk on under a paused list", dispatch_one(dsd_app_actions_trunk, &opts, &state, &cmd), 1);
    /* The calls followed from here belong to the system explored, not a scan's saved name: operator_tuned stays. */
    rc |= expect_true("trunk on ends the pause; operator_tuned kept",
                      state.lcn_scan_paused == 0U && opts.trunk_enable == 1 && state.operator_tuned == 1U);
    return rc;
}

/* Issue #671 (round 4): a -Y start the next start would refuse (a trunk scan a config left waiting) is refused with the
   rule, the scanner left off and trunking as it was; a stop is never asked. */
static int
test_scanner_start_held_to_the_next_start(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    struct dsd_app_command y = {0};
    y.id = DSD_APP_CMD_SCANNER_TOGGLE;
    opts.trunk_enable = 1;
    g_next_start_calls = 0;
    g_next_start_why =
        "trunking.scanner cannot be combined with trunk_scan.enabled; the -Y scanner and a trunk scan both "
        "own the tuner";
    rc |= expect_int("next start: y refused", dispatch_one(dsd_app_actions_trunk, &opts, &state, &y), 2);
    rc |= expect_true("next start: the rule on the status line",
                      strcmp(state.ui_msg, "Scanner not started: trunking.scanner cannot be combined with "
                                           "trunk_scan.enabled")
                          == 0);
    rc |= expect_true("next start: nothing changed", opts.scanner_mode == 0 && opts.trunk_enable == 1);
    rc |= expect_true("next start: the save asked about runs the scanner, trunking off",
                      g_next_start_calls == 1 && g_next_start_save.trunk_scanner == 1
                          && g_next_start_save.trunk_enabled == 0);
    g_next_start_why = NULL;
    rc |= expect_int("next start: y starts", dispatch_one(dsd_app_actions_trunk, &opts, &state, &y), 1);
    rc |= expect_true("next start: scanning", opts.scanner_mode == 1 && opts.trunk_enable == 0);
    rc |= expect_int("next start: y stops", dispatch_one(dsd_app_actions_trunk, &opts, &state, &y), 1);
    rc |= expect_true("next start: a stop asks nothing", g_next_start_calls == 2 && opts.scanner_mode == 0);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_audio_actions();
    rc |= test_trunk_actions();
    rc |= test_trunk_ownership_guard();
    rc |= test_scanner_toggle_pauses_and_resumes();
    rc |= test_scanner_start_held_to_the_next_start();
    rc |= test_trunking_under_a_scan();
    rc |= test_radio_actions();
    rc |= test_logging_actions();
    if (rc == 0) {
        printf("DSD_APP_CMD_ACTIONS: OK\n");
    }
    return rc;
}
