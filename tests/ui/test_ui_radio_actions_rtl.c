// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* RTL-specific contracts for terminal UI radio command actions. */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/rtl_stream_fwd.h>
#include <stdint.h>
#include <stdio.h>
#include "command_dispatch.h"

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static const dsd_opts* g_profile_opts;
static int g_profile_calls;
static int g_profile_rate;
static int g_profile_levels;
static int g_channel_profile;
static int g_lock_at_profile_apply;

int
rtl_stream_adjust_ppm(dsd_opts* opts, int delta) {
    if (!opts) {
        return -1;
    }
    opts->rtlsdr_ppm_error += delta;
    return 0;
}

uint32_t
rtl_stream_output_rate(const RtlSdrContext* ctx) {
    return ctx ? 48000U : 0U;
}

int
rtl_stream_set_symbol_profile(int symbol_rate_hz, int levels, int channel_profile) {
    g_profile_calls++;
    g_profile_rate = symbol_rate_hz;
    g_profile_levels = levels;
    g_channel_profile = channel_profile;
    g_lock_at_profile_apply = g_profile_opts ? g_profile_opts->mod_cli_lock : -1;
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
dispatch_one(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* cmd) {
    for (const struct dsd_app_command_reg* r = dsd_app_actions_radio; r && r->fn; r++) {
        if (r->id == cmd->id) {
            return r->fn(opts, state, cmd);
        }
    }
    return 0;
}

static void
reset_profile_capture(const dsd_opts* opts) {
    g_profile_opts = opts;
    g_profile_calls = 0;
    g_profile_rate = 0;
    g_profile_levels = 0;
    g_channel_profile = -1;
    g_lock_at_profile_apply = -1;
}

static int
test_p25p2_toggle_applies_rtl_profile_before_lock(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    struct dsd_app_command cmd;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&cmd, 0, sizeof(cmd));

    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (RtlSdrContext*)&state;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    cmd.id = DSD_APP_CMD_MOD_P2_TOGGLE;
    reset_profile_capture(&opts);

    rc |= expect_int("p25p2 qpsk dispatch", dispatch_one(&opts, &state, &cmd), 1);
    rc |= expect_int("p25p2 qpsk profile call", g_profile_calls, 1);
    rc |= expect_int("p25p2 qpsk profile rate", g_profile_rate, 6000);
    rc |= expect_int("p25p2 qpsk profile levels", g_profile_levels, 4);
    rc |= expect_int("p25p2 qpsk channel profile", g_channel_profile, RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    rc |= expect_int("p25p2 qpsk profile precedes lock", g_lock_at_profile_apply, 0);
    rc |= expect_int("p25p2 qpsk enables lock", opts.mod_cli_lock, 1);
    rc |= expect_int("p25p2 qpsk selects hunt profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_6000_4);

    opts.mod_cli_lock = 0;
    reset_profile_capture(&opts);
    rc |= expect_int("p25p2 c4fm dispatch", dispatch_one(&opts, &state, &cmd), 1);
    rc |= expect_int("p25p2 c4fm profile call", g_profile_calls, 1);
    rc |= expect_int("p25p2 c4fm profile rate", g_profile_rate, 6000);
    rc |= expect_int("p25p2 c4fm profile levels", g_profile_levels, 4);
    rc |= expect_int("p25p2 c4fm channel profile", g_channel_profile, RTL_STREAM_CHANNEL_PROFILE_12K5);
    rc |= expect_int("p25p2 c4fm profile precedes lock", g_lock_at_profile_apply, 0);
    rc |= expect_int("p25p2 c4fm enables lock", opts.mod_cli_lock, 1);

    return rc;
}

static int
test_p25p2_toggle_ignores_non_rtl_input(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    struct dsd_app_command cmd;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&cmd, 0, sizeof(cmd));

    opts.audio_in_type = AUDIO_IN_PULSE;
    state.rtl_ctx = (RtlSdrContext*)&state;
    cmd.id = DSD_APP_CMD_MOD_P2_TOGGLE;
    reset_profile_capture(&opts);

    rc |= expect_int("non-rtl p25p2 dispatch", dispatch_one(&opts, &state, &cmd), 1);
    rc |= expect_int("non-rtl p25p2 profile calls", g_profile_calls, 0);
    rc |= expect_int("non-rtl p25p2 enables lock", opts.mod_cli_lock, 1);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_p25p2_toggle_applies_rtl_profile_before_lock();
    rc |= test_p25p2_toggle_ignores_non_rtl_input();
    if (rc == 0) {
        DSD_FPRINTF(stderr, "APP CONTROL RTL ACTION TESTS PASSED\n");
    }
    return rc;
}
