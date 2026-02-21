// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/decode_mode.h>

#include <stdio.h>
#include <string.h>

static int
test_auto_profile_differences(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};

    opts.frame_dstar = 0;
    opts.frame_dmr = 0;
    opts.pulse_digi_out_channels = 7;
    state.rf_mod = 2;

    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_AUTO, DSD_DECODE_PRESET_PROFILE_INTERACTIVE, &opts, &state) != 0) {
        fprintf(stderr, "interactive AUTO apply failed\n");
        return 1;
    }
    if (opts.frame_dstar != 0 || opts.frame_dmr != 0 || opts.pulse_digi_out_channels != 7 || state.rf_mod != 2) {
        fprintf(stderr, "interactive AUTO should preserve existing mode flags/channels\n");
        return 1;
    }
    if (strcmp(opts.output_name, "AUTO") != 0) {
        fprintf(stderr, "interactive AUTO output_name mismatch: %s\n", opts.output_name);
        return 1;
    }

    memset(&opts, 0, sizeof opts);
    memset(&state, 0, sizeof state);
    opts.frame_dstar = 0;
    opts.frame_dmr = 0;
    opts.frame_ysf = 0;
    opts.frame_provoice = 0;
    opts.pulse_digi_out_channels = 1;
    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_AUTO, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state) != 0) {
        fprintf(stderr, "cli AUTO apply failed\n");
        return 1;
    }
    if (!(opts.frame_dstar == 1 && opts.frame_x2tdma == 1 && opts.frame_p25p1 == 1 && opts.frame_p25p2 == 1
          && opts.frame_nxdn48 == 1 && opts.frame_nxdn96 == 1 && opts.frame_dmr == 1 && opts.frame_dpmr == 1
          && opts.frame_provoice == 1 && opts.frame_ysf == 1 && opts.frame_m17 == 1)) {
        fprintf(stderr, "cli AUTO should enable all digital frame flags\n");
        return 1;
    }
    if (opts.pulse_digi_out_channels != 2 || opts.dmr_stereo != 1 || opts.dmr_mono != 0) {
        fprintf(stderr, "cli AUTO audio settings mismatch channels=%d stereo=%d mono=%d\n",
                opts.pulse_digi_out_channels, opts.dmr_stereo, opts.dmr_mono);
        return 1;
    }

    return 0;
}

static int
test_interactive_x2_and_ysf_behavior(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};

    opts.mod_c4fm = 0;
    opts.mod_qpsk = 1;
    opts.mod_gfsk = 1;
    state.rf_mod = 2;

    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_X2TDMA, DSD_DECODE_PRESET_PROFILE_INTERACTIVE, &opts, &state) != 0) {
        fprintf(stderr, "interactive X2 apply failed\n");
        return 1;
    }
    if (!(opts.frame_x2tdma == 1 && opts.frame_dstar == 0 && opts.frame_dmr == 0)) {
        fprintf(stderr, "interactive X2 frame flags mismatch\n");
        return 1;
    }
    if (!(opts.mod_c4fm == 1 && opts.mod_qpsk == 0 && opts.mod_gfsk == 0 && state.rf_mod == 0)) {
        fprintf(stderr, "interactive X2 should reset demod mode to C4FM (mod=%d/%d/%d rf_mod=%d)\n", opts.mod_c4fm,
                opts.mod_qpsk, opts.mod_gfsk, state.rf_mod);
        return 1;
    }
    if (opts.pulse_digi_out_channels != 1) {
        fprintf(stderr, "interactive X2 should use 1 output channel, got %d\n", opts.pulse_digi_out_channels);
        return 1;
    }

    memset(&opts, 0, sizeof opts);
    memset(&state, 0, sizeof state);
    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_YSF, DSD_DECODE_PRESET_PROFILE_CONFIG, &opts, &state) != 0) {
        fprintf(stderr, "config YSF apply failed\n");
        return 1;
    }
    if (!(opts.pulse_digi_out_channels == 2 && opts.dmr_stereo == 1 && opts.dmr_mono == 0)) {
        fprintf(stderr, "config YSF audio settings mismatch channels=%d stereo=%d mono=%d\n",
                opts.pulse_digi_out_channels, opts.dmr_stereo, opts.dmr_mono);
        return 1;
    }

    memset(&opts, 0, sizeof opts);
    memset(&state, 0, sizeof state);
    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_YSF, DSD_DECODE_PRESET_PROFILE_INTERACTIVE, &opts, &state) != 0) {
        fprintf(stderr, "interactive YSF apply failed\n");
        return 1;
    }
    if (!(opts.pulse_digi_out_channels == 1 && opts.dmr_stereo == 0 && state.dmr_stereo == 0 && opts.dmr_mono == 0)) {
        fprintf(stderr, "interactive YSF audio settings mismatch channels=%d stereo=%d state_stereo=%d mono=%d\n",
                opts.pulse_digi_out_channels, opts.dmr_stereo, state.dmr_stereo, opts.dmr_mono);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    rc |= test_auto_profile_differences();
    rc |= test_interactive_x2_and_ysf_behavior();
    return rc;
}
