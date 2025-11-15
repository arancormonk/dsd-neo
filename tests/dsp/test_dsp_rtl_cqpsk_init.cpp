// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit test: RTL CQPSK demod init defaults for a P25 Phase 2 mode.
 *
 * Exercises rtl_demod_init_for_mode + env/opts config +
 * mode defaults + TED SPS refresh to ensure:
 *  - CQPSK path can be enabled via env.
 *  - TED SPS is derived from the effective complex rate and
 *    P25P2 symbol rate and lands in a sane range.
 *  - CQPSK RRC configuration fields are non-zero and reasonable.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/runtime/ring.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
clear_env_for_test(void) {
    /* Ensure TED and resampler are driven by defaults, not caller env. */
    unsetenv("DSD_NEO_TED");
    unsetenv("DSD_NEO_TED_GAIN");
    unsetenv("DSD_NEO_TED_SPS");
    unsetenv("DSD_NEO_TED_FORCE");
    unsetenv("DSD_NEO_RESAMP");
    /* CQPSK/RRC env toggles are set explicitly below as needed. */
    unsetenv("DSD_NEO_CQPSK_RRC");
    unsetenv("DSD_NEO_CQPSK_RRC_ALPHA");
    unsetenv("DSD_NEO_CQPSK_RRC_SPAN");
    unsetenv("DSD_NEO_CQPSK");
}

int
main(void) {
    clear_env_for_test();

    /* Enable CQPSK path for this test via env, matching runtime usage. */
    if (setenv("DSD_NEO_CQPSK", "1", 1) != 0) {
        perror("setenv DSD_NEO_CQPSK");
        return 1;
    }

    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));
    /* P25 Phase 2 CQPSK mode with a typical 12 kHz DSP baseband. */
    opts.frame_p25p2 = 1;
    opts.rtl_dsp_bw_khz = 12;

    demod_state demod;
    memset(&demod, 0, sizeof(demod));
    struct output_state output;
    memset(&output, 0, sizeof(output));

    const int demod_base_rate_hz = opts.rtl_dsp_bw_khz * 1000;

    rtl_demod_init_for_mode(&demod, &output, &opts, demod_base_rate_hz);

    if (demod.rate_in != demod_base_rate_hz || demod.rate_out != demod_base_rate_hz) {
        fprintf(stderr, "DEM: rate_in/out=%d/%d expected=%d\n", demod.rate_in, demod.rate_out, demod_base_rate_hz);
        return 1;
    }

    /* Apply env/opts-driven configuration and mode defaults. */
    rtl_demod_config_from_env_and_opts(&demod, &opts);
    rtl_demod_select_defaults_for_mode(&demod, &opts, &output);

    if (demod.cqpsk_enable != 1) {
        fprintf(stderr, "DEM: cqpsk_enable=%d expected=1 (env)\n", demod.cqpsk_enable);
        return 1;
    }

    /* Simulate initial rate planning: configure resampler and then refresh TED SPS. */
    rtl_demod_maybe_update_resampler_after_rate_change(&demod, &output, demod_base_rate_hz);
    rtl_demod_maybe_refresh_ted_sps_after_rate_change(&demod, &opts, &output);

    /* For P25P2 at a nominal 48 kHz complex rate we expect SPS ≈ Fs/6000 ≈ 8. */
    if (demod.ted_sps < 4 || demod.ted_sps > 12) {
        fprintf(stderr, "DEM: ted_sps=%d out of expected range [4,12]\n", demod.ted_sps);
        return 1;
    }

    /* Compute the expected SPS using the same rounding rule as the config helper. */
    int Fs_cx = 48000;
    int expected_sps = (Fs_cx + 3000) / 6000;
    if (expected_sps < 2) {
        expected_sps = 2;
    }
    if (expected_sps > 64) {
        expected_sps = 64;
    }
    if (demod.ted_sps != expected_sps) {
        fprintf(stderr, "DEM: ted_sps=%d expected=%d\n", demod.ted_sps, expected_sps);
        return 1;
    }

    /* CQPSK RRC configuration should be non-zero and within a sane range. */
    if (demod.cqpsk_rrc_alpha_q15 <= 0 || demod.cqpsk_rrc_alpha_q15 > 32768) {
        fprintf(stderr, "DEM: cqpsk_rrc_alpha_q15=%d out of range\n", demod.cqpsk_rrc_alpha_q15);
        return 1;
    }
    if (demod.cqpsk_rrc_span_syms < 3 || demod.cqpsk_rrc_span_syms > 16) {
        fprintf(stderr, "DEM: cqpsk_rrc_span_syms=%d out of range [3,16]\n", demod.cqpsk_rrc_span_syms);
        return 1;
    }

    return 0;
}
