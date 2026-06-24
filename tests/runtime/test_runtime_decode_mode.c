// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(clang-analyzer-optin.performance.Padding)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/runtime/config.h"

static int
test_auto_profile_differences(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};

    opts.frame_dstar = 0;
    opts.frame_dmr = 0;
    opts.pulse_digi_out_channels = 7;
    state.rf_mod = 2;

    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_AUTO, DSD_DECODE_PRESET_PROFILE_INTERACTIVE, &opts, &state) != 0) {
        DSD_FPRINTF(stderr, "interactive AUTO apply failed\n");
        return 1;
    }
    if (opts.frame_dstar != 0 || opts.frame_dmr != 0 || opts.pulse_digi_out_channels != 7 || state.rf_mod != 2) {
        DSD_FPRINTF(stderr, "interactive AUTO should preserve existing mode flags/channels\n");
        return 1;
    }
    if (strcmp(opts.output_name, "AUTO") != 0) {
        DSD_FPRINTF(stderr, "interactive AUTO output_name mismatch: %s\n", opts.output_name);
        return 1;
    }

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.frame_dstar = 0;
    opts.frame_dmr = 0;
    opts.frame_ysf = 0;
    opts.frame_provoice = 0;
    opts.pulse_digi_out_channels = 1;
    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_AUTO, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state) != 0) {
        DSD_FPRINTF(stderr, "cli AUTO apply failed\n");
        return 1;
    }
    if (!(opts.frame_dstar == 1 && opts.frame_x2tdma == 1 && opts.frame_p25p1 == 1 && opts.frame_p25p2 == 1
          && opts.frame_nxdn48 == 1 && opts.frame_nxdn96 == 1 && opts.frame_dmr == 1 && opts.frame_dpmr == 1
          && opts.frame_provoice == 1 && opts.frame_ysf == 1 && opts.frame_m17 == 1)) {
        DSD_FPRINTF(stderr, "cli AUTO should enable all digital frame flags\n");
        return 1;
    }
    if (opts.pulse_digi_out_channels != 2 || opts.dmr_stereo != 1 || opts.dmr_mono != 0) {
        DSD_FPRINTF(stderr, "cli AUTO audio settings mismatch channels=%d stereo=%d mono=%d\n",
                    opts.pulse_digi_out_channels, opts.dmr_stereo, opts.dmr_mono);
        return 1;
    }

    return 0;
}

static int
test_p25p2_prefers_qpsk(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};

    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_P25P2, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state) != 0) {
        DSD_FPRINTF(stderr, "cli P25P2 apply failed\n");
        return 1;
    }
    if (!(opts.frame_p25p2 == 1 && opts.frame_p25p1 == 0 && opts.frame_x2tdma == 0)) {
        DSD_FPRINTF(stderr, "cli P25P2 frame flags mismatch\n");
        return 1;
    }
    if (!(opts.mod_c4fm == 0 && opts.mod_qpsk == 1 && opts.mod_gfsk == 0 && state.rf_mod == 1)) {
        DSD_FPRINTF(stderr, "cli P25P2 should select QPSK demod (mod=%d/%d/%d rf_mod=%d)\n", opts.mod_c4fm,
                    opts.mod_qpsk, opts.mod_gfsk, state.rf_mod);
        return 1;
    }
    if (!(state.samplesPerSymbol == 8 && state.symbolCenter == 3)) {
        DSD_FPRINTF(stderr, "cli P25P2 symbol timing mismatch sps=%d center=%d\n", state.samplesPerSymbol,
                    state.symbolCenter);
        return 1;
    }
    return 0;
}

static int
test_dmr_prefers_gfsk(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};

    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state) != 0) {
        DSD_FPRINTF(stderr, "cli DMR apply failed\n");
        return 1;
    }
    if (!(opts.frame_dmr == 1 && opts.frame_dstar == 0 && opts.frame_p25p1 == 0 && opts.frame_p25p2 == 0)) {
        DSD_FPRINTF(stderr, "cli DMR frame flags mismatch\n");
        return 1;
    }
    if (!(opts.mod_c4fm == 0 && opts.mod_qpsk == 0 && opts.mod_gfsk == 1 && state.rf_mod == 2)) {
        DSD_FPRINTF(stderr, "cli DMR should select GFSK demod (mod=%d/%d/%d rf_mod=%d)\n", opts.mod_c4fm, opts.mod_qpsk,
                    opts.mod_gfsk, state.rf_mod);
        return 1;
    }
    return 0;
}

static int
test_dmr_preserves_manual_c4fm_lock(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};

    opts.mod_cli_lock = 1;
    opts.mod_c4fm = 1;
    opts.mod_qpsk = 0;
    opts.mod_gfsk = 0;
    state.rf_mod = 0;

    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state) != 0) {
        DSD_FPRINTF(stderr, "cli DMR apply failed\n");
        return 1;
    }
    if (!(opts.frame_dmr == 1 && opts.mod_cli_lock == 1 && opts.mod_c4fm == 1 && opts.mod_qpsk == 0
          && opts.mod_gfsk == 0 && state.rf_mod == 0)) {
        DSD_FPRINTF(stderr, "cli DMR should preserve manual C4FM lock (mod=%d/%d/%d lock=%d rf_mod=%d)\n",
                    opts.mod_c4fm, opts.mod_qpsk, opts.mod_gfsk, opts.mod_cli_lock, state.rf_mod);
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
        DSD_FPRINTF(stderr, "interactive X2 apply failed\n");
        return 1;
    }
    if (!(opts.frame_x2tdma == 1 && opts.frame_dstar == 0 && opts.frame_dmr == 0)) {
        DSD_FPRINTF(stderr, "interactive X2 frame flags mismatch\n");
        return 1;
    }
    if (!(opts.mod_c4fm == 1 && opts.mod_qpsk == 0 && opts.mod_gfsk == 0 && state.rf_mod == 0)) {
        DSD_FPRINTF(stderr, "interactive X2 should reset demod mode to C4FM (mod=%d/%d/%d rf_mod=%d)\n", opts.mod_c4fm,
                    opts.mod_qpsk, opts.mod_gfsk, state.rf_mod);
        return 1;
    }
    if (opts.pulse_digi_out_channels != 1) {
        DSD_FPRINTF(stderr, "interactive X2 should use 1 output channel, got %d\n", opts.pulse_digi_out_channels);
        return 1;
    }

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_YSF, DSD_DECODE_PRESET_PROFILE_CONFIG, &opts, &state) != 0) {
        DSD_FPRINTF(stderr, "config YSF apply failed\n");
        return 1;
    }
    if (!(opts.pulse_digi_out_channels == 2 && opts.dmr_stereo == 1 && opts.dmr_mono == 0)) {
        DSD_FPRINTF(stderr, "config YSF audio settings mismatch channels=%d stereo=%d mono=%d\n",
                    opts.pulse_digi_out_channels, opts.dmr_stereo, opts.dmr_mono);
        return 1;
    }

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_YSF, DSD_DECODE_PRESET_PROFILE_INTERACTIVE, &opts, &state) != 0) {
        DSD_FPRINTF(stderr, "interactive YSF apply failed\n");
        return 1;
    }
    if (!(opts.pulse_digi_out_channels == 1 && opts.dmr_stereo == 0 && state.dmr_stereo == 0 && opts.dmr_mono == 0)) {
        DSD_FPRINTF(stderr, "interactive YSF audio settings mismatch channels=%d stereo=%d state_stereo=%d mono=%d\n",
                    opts.pulse_digi_out_channels, opts.dmr_stereo, state.dmr_stereo, opts.dmr_mono);
        return 1;
    }
    return 0;
}

static int
test_cli_preset_mapping_and_guards(void) {
    static const struct {
        char preset;
        dsdneoUserDecodeMode mode;
    } cases[] = {
        {'a', DSDCFG_MODE_AUTO},   {'A', DSDCFG_MODE_ANALOG}, {'d', DSDCFG_MODE_DSTAR}, {'x', DSDCFG_MODE_X2TDMA},
        {'t', DSDCFG_MODE_TDMA},   {'1', DSDCFG_MODE_P25P1},  {'2', DSDCFG_MODE_P25P2}, {'s', DSDCFG_MODE_DMR},
        {'i', DSDCFG_MODE_NXDN48}, {'n', DSDCFG_MODE_NXDN96}, {'y', DSDCFG_MODE_YSF},   {'m', DSDCFG_MODE_M17},
    };

    dsdneoUserDecodeMode mode = DSDCFG_MODE_UNSET;
    if (dsd_decode_mode_from_cli_preset('a', NULL) != -1) {
        DSD_FPRINTF(stderr, "NULL output preset parse should fail\n");
        return 1;
    }
    if (dsd_decode_mode_from_cli_preset('?', &mode) != -1 || mode != DSDCFG_MODE_UNSET) {
        DSD_FPRINTF(stderr, "invalid preset parse should fail without changing mode\n");
        return 1;
    }

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        mode = DSDCFG_MODE_UNSET;
        if (dsd_decode_mode_from_cli_preset(cases[i].preset, &mode) != 0 || mode != cases[i].mode) {
            DSD_FPRINTF(stderr, "preset %c mapped to %d, expected %d\n", cases[i].preset, (int)mode,
                        (int)cases[i].mode);
            return 1;
        }
    }

    return 0;
}

static int
test_remaining_preset_modes(void) {
    static const struct {
        dsdneoUserDecodeMode mode;
        dsdDecodePresetProfile profile;
        const char* name;
        int frame_dstar;
        int frame_x2tdma;
        int frame_p25p1;
        int frame_p25p2;
        int frame_nxdn48;
        int frame_nxdn96;
        int frame_dmr;
        int frame_dpmr;
        int frame_provoice;
        int frame_ysf;
        int frame_m17;
        int samples_per_symbol;
        int symbol_center;
        int rf_mod;
        int pulse_channels;
    } cases[] = {
        {DSDCFG_MODE_P25P1, DSD_DECODE_PRESET_PROFILE_CLI, "P25p1", 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
        {DSDCFG_MODE_NXDN96, DSD_DECODE_PRESET_PROFILE_INTERACTIVE, "NXDN96", 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 20, 9, 0,
         1},
        {DSDCFG_MODE_DSTAR, DSD_DECODE_PRESET_PROFILE_CLI, "DSTAR", 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
        {DSDCFG_MODE_EDACS_PV, DSD_DECODE_PRESET_PROFILE_CLI, "EDACS/PV", 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 5, 2, 2, 1},
        {DSDCFG_MODE_DPMR, DSD_DECODE_PRESET_PROFILE_CLI, "dPMR", 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 20, 9, 0, 1},
        {DSDCFG_MODE_M17, DSD_DECODE_PRESET_PROFILE_CLI, "M17", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1},
        {DSDCFG_MODE_TDMA, DSD_DECODE_PRESET_PROFILE_CLI, "TDMA", 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        state.samplesPerSymbol = -1;
        state.symbolCenter = -1;
        opts.use_cosine_filter = 1;

        if (dsd_apply_decode_mode_preset(cases[i].mode, cases[i].profile, &opts, &state) != 0) {
            DSD_FPRINTF(stderr, "preset %s apply failed\n", cases[i].name);
            return 1;
        }
        if (strcmp(opts.output_name, cases[i].name) != 0) {
            DSD_FPRINTF(stderr, "preset output name mismatch: got %s expected %s\n", opts.output_name, cases[i].name);
            return 1;
        }
        if (!(opts.frame_dstar == cases[i].frame_dstar && opts.frame_x2tdma == cases[i].frame_x2tdma
              && opts.frame_p25p1 == cases[i].frame_p25p1 && opts.frame_p25p2 == cases[i].frame_p25p2
              && opts.frame_nxdn48 == cases[i].frame_nxdn48 && opts.frame_nxdn96 == cases[i].frame_nxdn96
              && opts.frame_dmr == cases[i].frame_dmr && opts.frame_dpmr == cases[i].frame_dpmr
              && opts.frame_provoice == cases[i].frame_provoice && opts.frame_ysf == cases[i].frame_ysf
              && opts.frame_m17 == cases[i].frame_m17)) {
            DSD_FPRINTF(stderr, "preset %s frame flags mismatch\n", cases[i].name);
            return 1;
        }
        if (cases[i].samples_per_symbol > 0
            && !(state.samplesPerSymbol == cases[i].samples_per_symbol
                 && state.symbolCenter == cases[i].symbol_center)) {
            DSD_FPRINTF(stderr, "preset %s timing mismatch sps=%d center=%d\n", cases[i].name, state.samplesPerSymbol,
                        state.symbolCenter);
            return 1;
        }
        if (state.rf_mod != cases[i].rf_mod || opts.pulse_digi_out_channels != cases[i].pulse_channels) {
            DSD_FPRINTF(stderr, "preset %s rf/audio mismatch rf_mod=%d channels=%d\n", cases[i].name, state.rf_mod,
                        opts.pulse_digi_out_channels);
            return 1;
        }
        if (cases[i].mode == DSDCFG_MODE_M17 && opts.use_cosine_filter != 0) {
            DSD_FPRINTF(stderr, "M17 should disable cosine filter\n");
            return 1;
        }
        if (cases[i].mode == DSDCFG_MODE_EDACS_PV && (state.ea_mode != 0 || state.esk_mask != 0)) {
            DSD_FPRINTF(stderr, "EDACS/PV should clear EA/ESK state\n");
            return 1;
        }
    }

    static dsd_opts opts = {0};
    static dsd_state state = {0};
    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_P25P1, DSD_DECODE_PRESET_PROFILE_CLI, NULL, &state) != -1
        || dsd_apply_decode_mode_preset(DSDCFG_MODE_P25P1, DSD_DECODE_PRESET_PROFILE_CLI, &opts, NULL) != -1
        || dsd_apply_decode_mode_preset(DSDCFG_MODE_UNSET, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state) != -1) {
        DSD_FPRINTF(stderr, "preset apply guards should reject NULL/unknown modes\n");
        return 1;
    }

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.dmr_mono = 1;
    state.dmr_stereo = 1;
    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_NXDN96, DSD_DECODE_PRESET_PROFILE_CONFIG, &opts, &state) != 0) {
        DSD_FPRINTF(stderr, "config NXDN96 apply failed\n");
        return 1;
    }
    if (opts.dmr_mono != 1 || state.dmr_stereo != 1) {
        DSD_FPRINTF(stderr, "config NXDN96 should preserve mono/state stereo settings\n");
        return 1;
    }

    return 0;
}

static int
test_symbol_timing_and_inference(void) {
    static const struct {
        dsdneoUserDecodeMode mode;
        int expected_sps;
        int expected_center;
    } timing_cases[] = {
        {DSDCFG_MODE_P25P2, 16, 6},    {DSDCFG_MODE_NXDN48, 40, 18}, {DSDCFG_MODE_DPMR, 40, 18},
        {DSDCFG_MODE_EDACS_PV, 10, 4}, {DSDCFG_MODE_DMR, 20, 8},
    };

    static const struct {
        unsigned int bit;
        dsdneoUserDecodeMode expected;
    } infer_cases[] = {
        {1u << 6, DSDCFG_MODE_DMR},    {1u << 2, DSDCFG_MODE_P25P1},  {1u << 3, DSDCFG_MODE_P25P2},
        {1u << 4, DSDCFG_MODE_NXDN48}, {1u << 5, DSDCFG_MODE_NXDN96}, {1u << 1, DSDCFG_MODE_X2TDMA},
        {1u << 9, DSDCFG_MODE_YSF},    {1u << 0, DSDCFG_MODE_DSTAR},  {1u << 8, DSDCFG_MODE_EDACS_PV},
        {1u << 7, DSDCFG_MODE_DPMR},   {1u << 10, DSDCFG_MODE_M17},
    };

    dsd_apply_decode_mode_symbol_timing(DSDCFG_MODE_DMR, 96000, NULL);

    for (size_t i = 0; i < sizeof timing_cases / sizeof timing_cases[0]; i++) {
        static dsd_state state;
        DSD_MEMSET(&state, 0, sizeof state);
        dsd_apply_decode_mode_symbol_timing(timing_cases[i].mode, 96000, &state);
        if (state.samplesPerSymbol != timing_cases[i].expected_sps
            || state.symbolCenter != timing_cases[i].expected_center || state.jitter != -1) {
            DSD_FPRINTF(stderr, "timing mode %d mismatch sps=%d center=%d jitter=%d\n", (int)timing_cases[i].mode,
                        state.samplesPerSymbol, state.symbolCenter, state.jitter);
            return 1;
        }
    }

    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof state);
    dsd_apply_decode_mode_symbol_timing(DSDCFG_MODE_DMR, 48000, &state);
    if (state.samplesPerSymbol != 10 || state.symbolCenter != 4) {
        DSD_FPRINTF(stderr, "48000 timing should keep base DMR timing\n");
        return 1;
    }

    if (dsd_infer_decode_mode_preset(NULL) != DSDCFG_MODE_AUTO) {
        DSD_FPRINTF(stderr, "NULL infer should return AUTO\n");
        return 1;
    }

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.analog_only = 1;
    opts.monitor_input_audio = 1;
    if (dsd_infer_decode_mode_preset(&opts) != DSDCFG_MODE_ANALOG) {
        DSD_FPRINTF(stderr, "analog monitor infer mismatch\n");
        return 1;
    }

    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    if (dsd_infer_decode_mode_preset(&opts) != DSDCFG_MODE_TDMA) {
        DSD_FPRINTF(stderr, "TDMA infer mismatch\n");
        return 1;
    }

    for (size_t i = 0; i < sizeof infer_cases / sizeof infer_cases[0]; i++) {
        DSD_MEMSET(&opts, 0, sizeof opts);
        opts.frame_dstar = (infer_cases[i].bit & (1u << 0)) != 0;
        opts.frame_x2tdma = (infer_cases[i].bit & (1u << 1)) != 0;
        opts.frame_p25p1 = (infer_cases[i].bit & (1u << 2)) != 0;
        opts.frame_p25p2 = (infer_cases[i].bit & (1u << 3)) != 0;
        opts.frame_nxdn48 = (infer_cases[i].bit & (1u << 4)) != 0;
        opts.frame_nxdn96 = (infer_cases[i].bit & (1u << 5)) != 0;
        opts.frame_dmr = (infer_cases[i].bit & (1u << 6)) != 0;
        opts.frame_dpmr = (infer_cases[i].bit & (1u << 7)) != 0;
        opts.frame_provoice = (infer_cases[i].bit & (1u << 8)) != 0;
        opts.frame_ysf = (infer_cases[i].bit & (1u << 9)) != 0;
        opts.frame_m17 = (infer_cases[i].bit & (1u << 10)) != 0;
        if (dsd_infer_decode_mode_preset(&opts) != infer_cases[i].expected) {
            DSD_FPRINTF(stderr, "infer bit %u mismatch\n", infer_cases[i].bit);
            return 1;
        }
    }

    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.frame_dstar = 1;
    opts.frame_dmr = 1;
    if (dsd_infer_decode_mode_preset(&opts) != DSDCFG_MODE_AUTO) {
        DSD_FPRINTF(stderr, "mixed unsupported infer should return AUTO\n");
        return 1;
    }

    return 0;
}

int
main(void) {
    int rc = 0;
    rc |= test_auto_profile_differences();
    rc |= test_p25p2_prefers_qpsk();
    rc |= test_dmr_prefers_gfsk();
    rc |= test_dmr_preserves_manual_c4fm_lock();
    rc |= test_interactive_x2_and_ysf_behavior();
    rc |= test_cli_preset_mapping_and_guards();
    rc |= test_remaining_preset_modes();
    rc |= test_symbol_timing_and_inference();
    return rc;
}

// NOLINTEND(clang-analyzer-optin.performance.Padding)
