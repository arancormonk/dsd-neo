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
static int g_request_calls;
static int g_request_cqpsk;
static int g_request_rate;
static int g_request_levels;
static int g_request_channel_profile;
static int g_request_ted_sps;
static int g_request_ted_override;
static int g_lock_at_request;

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
rtl_stream_request_demod_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile, int ted_sps,
                                 int ted_sps_is_override) {
    g_request_calls++;
    g_request_cqpsk = cqpsk_enable;
    g_request_rate = symbol_rate_hz;
    g_request_levels = levels;
    g_request_channel_profile = channel_profile;
    g_request_ted_sps = ted_sps;
    g_request_ted_override = ted_sps_is_override;
    g_lock_at_request = g_profile_opts ? g_profile_opts->mod_cli_lock : -1;
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
    g_request_calls = 0;
    g_request_cqpsk = -2;
    g_request_rate = 0;
    g_request_levels = 0;
    g_request_channel_profile = -2;
    g_request_ted_sps = -2;
    g_request_ted_override = -1;
    g_lock_at_request = -1;
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
    rc |= expect_int("p25p2 qpsk request call", g_request_calls, 1);
    rc |= expect_int("p25p2 qpsk family", g_request_cqpsk, 1);
    rc |= expect_int("p25p2 qpsk profile rate", g_request_rate, 6000);
    rc |= expect_int("p25p2 qpsk profile levels", g_request_levels, 4);
    rc |= expect_int("p25p2 qpsk channel profile", g_request_channel_profile, RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    rc |= expect_int("p25p2 qpsk TED SPS", g_request_ted_sps, 8);
    rc |= expect_int("p25p2 qpsk TED no-override", g_request_ted_override, 0);
    rc |= expect_int("p25p2 qpsk request precedes lock", g_lock_at_request, 0);
    rc |= expect_int("p25p2 qpsk enables lock", opts.mod_cli_lock, 1);
    rc |= expect_int("p25p2 qpsk pins profile", opts.mod_p25p2_profile_lock, 1);
    rc |= expect_int("p25p2 qpsk selects hunt profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_6000_4);

    opts.mod_cli_lock = 0;
    reset_profile_capture(&opts);
    rc |= expect_int("p25p2 c4fm dispatch", dispatch_one(&opts, &state, &cmd), 1);
    rc |= expect_int("p25p2 c4fm request call", g_request_calls, 1);
    rc |= expect_int("p25p2 c4fm family", g_request_cqpsk, 0);
    rc |= expect_int("p25p2 c4fm profile rate", g_request_rate, 6000);
    rc |= expect_int("p25p2 c4fm profile levels", g_request_levels, 4);
    rc |= expect_int("p25p2 c4fm channel profile", g_request_channel_profile, RTL_STREAM_CHANNEL_PROFILE_12K5);
    rc |= expect_int("p25p2 c4fm TED SPS", g_request_ted_sps, 8);
    rc |= expect_int("p25p2 c4fm TED no-override", g_request_ted_override, 0);
    rc |= expect_int("p25p2 c4fm request precedes lock", g_lock_at_request, 0);
    rc |= expect_int("p25p2 c4fm enables lock", opts.mod_cli_lock, 1);

    return rc;
}

static int
test_generic_toggle_restores_rtl_after_p25p2_helper(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    struct dsd_app_command cmd;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&cmd, 0, sizeof(cmd));

    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (RtlSdrContext*)&state;
    cmd.id = DSD_APP_CMD_MOD_P2_TOGGLE;
    reset_profile_capture(&opts);
    rc |= expect_int("p25p2 helper setup dispatch", dispatch_one(&opts, &state, &cmd), 1);

    cmd.id = DSD_APP_CMD_MOD_TOGGLE;
    reset_profile_capture(&opts);
    rc |= expect_int("generic helper exit dispatch", dispatch_one(&opts, &state, &cmd), 1);
    rc |= expect_int("generic helper exit request call", g_request_calls, 1);
    rc |= expect_int("generic helper exit family", g_request_cqpsk, 0);
    rc |= expect_int("generic helper exit profile rate", g_request_rate, 4800);
    rc |= expect_int("generic helper exit profile levels", g_request_levels, 4);
    rc |= expect_int("generic helper exit channel profile", g_request_channel_profile,
                     RTL_STREAM_CHANNEL_PROFILE_P25_C4FM);
    rc |= expect_int("generic helper exit TED SPS", g_request_ted_sps, 10);
    rc |= expect_int("generic helper exit TED no-override", g_request_ted_override, 0);
    rc |= expect_int("generic helper exit request retains lock during transition", g_lock_at_request, 1);
    rc |= expect_int("generic helper exit releases lock", opts.mod_cli_lock, 0);
    rc |= expect_int("generic helper exit clears profile pin", opts.mod_p25p2_profile_lock, 0);
    rc |= expect_int("generic helper exit selects profile 0", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    rc |= expect_int("generic helper exit decoder SPS", state.samplesPerSymbol, 10);
    rc |= expect_int("generic helper exit decoder center", state.symbolCenter, 4);
    return rc;
}

/* One decode mode enabled, as a single-protocol session has it. */
static void
enable_nxdn48(dsd_opts* o) {
    o->frame_nxdn48 = 1;
}

static void
enable_nxdn96(dsd_opts* o) {
    o->frame_nxdn96 = 1;
}

static void
enable_dpmr(dsd_opts* o) {
    o->frame_dpmr = 1;
}

static void
enable_p25p2(dsd_opts* o) {
    o->frame_p25p2 = 1;
}

static void
enable_provoice(dsd_opts* o) {
    o->frame_provoice = 1;
}

/*
 * The symbol rate the modulation control applies comes from the mode being
 * decoded. NXDN48 and dPMR run at 2400 sym/s in a 6.25 kHz channel and NXDN96 at
 * 4800 in 12.5 kHz despite both NXDN variants sharing a preset sps; deriving the
 * rate from the frame flags by hand got the 2400 modes wrong and asked the front
 * end for a filter the DSP's own hunt disagreed with, which is what makes the
 * filter flip every time the hunt re-runs.
 */
static int
test_mod_set_takes_symbol_profile_from_the_decoded_mode(void) {
    static const struct {
        const char* tag;
        void (*enable)(dsd_opts*);
        int rate;
        int levels;
        int channel_profile;
        int ted_sps;
        int hunt_idx;
    } cases[] = {
        {"nxdn48", enable_nxdn48, 2400, 4, RTL_STREAM_CHANNEL_PROFILE_6K25, 20, DSD_FRAME_SYNC_SPS_PROFILE_2400_4},
        {"dpmr", enable_dpmr, 2400, 4, RTL_STREAM_CHANNEL_PROFILE_6K25, 20, DSD_FRAME_SYNC_SPS_PROFILE_2400_4},
        /* 4800 like P25p1, but a 12.5 kHz channel — dsd_opts_uses_wide_4800_profile(). */
        {"nxdn96", enable_nxdn96, 4800, 4, RTL_STREAM_CHANNEL_PROFILE_12K5, 10, DSD_FRAME_SYNC_SPS_PROFILE_4800_4},
        {"p25p2", enable_p25p2, 6000, 4, RTL_STREAM_CHANNEL_PROFILE_12K5, 8, DSD_FRAME_SYNC_SPS_PROFILE_6000_4},
        {"provoice", enable_provoice, 9600, 2, RTL_STREAM_CHANNEL_PROFILE_PROVOICE, 5,
         DSD_FRAME_SYNC_SPS_PROFILE_9600_2},
    };

    int rc = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        static dsd_opts opts;
        static dsd_state state;
        struct dsd_app_command cmd;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        DSD_MEMSET(&cmd, 0, sizeof(cmd));

        opts.audio_in_type = AUDIO_IN_RTL;
        state.rtl_ctx = (RtlSdrContext*)&state;
        cases[i].enable(&opts);
        /* Start on QPSK so the C4FM request below is a real change rather than
         * the no-op a control re-asserting its own state produces. */
        opts.mod_qpsk = 1;
        state.rf_mod = 1;
        /* Deliberately the wrong profile to begin with: leaving it alone is the
         * bug, so the assertion has to be able to tell "set" from "untouched". */
        state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
        state.sps_hunt_counter = 7;

        const int32_t want_c4fm = 0;
        cmd.id = DSD_APP_CMD_MOD_SET;
        cmd.n = (int)sizeof want_c4fm;
        DSD_MEMCPY(cmd.data, &want_c4fm, sizeof want_c4fm);
        reset_profile_capture(&opts);

        rc |= expect_int(cases[i].tag, dispatch_one(&opts, &state, &cmd), 1);
        rc |= expect_int(cases[i].tag, g_request_calls, 1);
        rc |= expect_int(cases[i].tag, g_request_rate, cases[i].rate);
        rc |= expect_int(cases[i].tag, g_request_levels, cases[i].levels);
        rc |= expect_int(cases[i].tag, g_request_channel_profile, cases[i].channel_profile);
        rc |= expect_int(cases[i].tag, g_request_ted_sps, cases[i].ted_sps);
        rc |= expect_int(cases[i].tag, g_request_cqpsk, 0);
        rc |= expect_int(cases[i].tag, state.samplesPerSymbol, cases[i].ted_sps);
        rc |= expect_int(cases[i].tag, state.sps_hunt_idx, cases[i].hunt_idx);
        rc |= expect_int(cases[i].tag, state.sps_hunt_counter, 0);
    }
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
    rc |= expect_int("non-rtl p25p2 request calls", g_request_calls, 0);
    rc |= expect_int("non-rtl p25p2 enables lock", opts.mod_cli_lock, 1);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_p25p2_toggle_applies_rtl_profile_before_lock();
    rc |= test_generic_toggle_restores_rtl_after_p25p2_helper();
    rc |= test_mod_set_takes_symbol_profile_from_the_decoded_mode();
    rc |= test_p25p2_toggle_ignores_non_rtl_input();
    if (rc == 0) {
        DSD_FPRINTF(stderr, "APP CONTROL RTL ACTION TESTS PASSED\n");
    }
    return rc;
}
