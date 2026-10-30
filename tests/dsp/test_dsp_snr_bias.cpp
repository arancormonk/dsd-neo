// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/snr_bias.h>
#include <math.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static int
nearly_equal(double got, double expected, double tol) {
    double d = got - expected;
    if (d < 0) {
        d = -d;
    }
    return d <= tol;
}

int
main(void) {
    const int rate_out = 48000;
    const int ted_sps = 10;
    const double tol = 1e-6;

    struct test_case {
        int profile;
        double expected_c4fm;
        double expected_evm;
        const char* name;
    };

    const struct test_case cases[] = {
        {DSD_CH_LPF_PROFILE_WIDE, 8.168128829362555, 4.358128829362554, "WIDE"},
        {DSD_CH_LPF_PROFILE_6K25, 4.407620246502007, 0.5976202465020064, "6K25"},
        {DSD_CH_LPF_PROFILE_12K5, 7.155589163742908, 3.3455891637429076, "12K5"},
        {DSD_CH_LPF_PROFILE_PROVOICE, 7.155589163742908, 3.3455891637429076, "PROVOICE"},
        {DSD_CH_LPF_PROFILE_P25_C4FM, 7.155589163742908, 3.3455891637429076, "P25_C4FM"},
        {DSD_CH_LPF_PROFILE_P25_CQPSK, 7.408709288089902, 3.598709288089902, "P25_CQPSK"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct test_case* tc = &cases[i];
        double got_c4fm = dsd_snr_bias_c4fm_db(rate_out, ted_sps, tc->profile, DSD_SNR_TETRA_STAGE_WIDE);
        if (!nearly_equal(got_c4fm, tc->expected_c4fm, tol)) {
            DSD_FPRINTF(stderr, "SNR bias C4FM mismatch for %s: got %.12f expected %.12f\n", tc->name, got_c4fm,
                        tc->expected_c4fm);
            return 1;
        }
        double got_evm = dsd_snr_bias_evm_db(rate_out, ted_sps, tc->profile, DSD_SNR_TETRA_STAGE_WIDE);
        if (!nearly_equal(got_evm, tc->expected_evm, tol)) {
            DSD_FPRINTF(stderr, "SNR bias EVM mismatch for %s: got %.12f expected %.12f\n", tc->name, got_evm,
                        tc->expected_evm);
            return 1;
        }
    }

    /* Fallback behavior for invalid inputs should be stable. */
    {
        const double fb_c4fm = dsd_snr_bias_c4fm_db(0, ted_sps, DSD_CH_LPF_PROFILE_WIDE, DSD_SNR_TETRA_STAGE_WIDE);
        const double fb_evm = dsd_snr_bias_evm_db(rate_out, 0, DSD_CH_LPF_PROFILE_WIDE, DSD_SNR_TETRA_STAGE_WIDE);
        if (!nearly_equal(fb_c4fm, 7.93, tol)) {
            DSD_FPRINTF(stderr, "SNR bias C4FM fallback mismatch: got %.12f expected %.12f\n", fb_c4fm, 7.93);
            return 1;
        }
        if (!nearly_equal(fb_evm, 2.42, tol)) {
            DSD_FPRINTF(stderr, "SNR bias EVM fallback mismatch: got %.12f expected %.12f\n", fb_evm, 2.42);
            return 1;
        }
    }

    /* The noise bandwidth follows the chain that runs. The TETRA chain's RRC matched filter has an equivalent noise
       bandwidth of the 18000 sym/s symbol rate two-sided, 9000 Hz one-sided as the other profiles' figures are, so
       its EVM bias is the estimator bias plus 10 log10(9000 / 18000). A TETRA profile whose chain is inactive runs the
       WIDE plan and uses WIDE's bandwidth. A chain flag on another profile changes nothing. */
    {
        struct bw_case {
            int profile;
            int tetra_stage;
            double noise_bw_hz;
        };

        const struct bw_case bw_cases[] = {
            {DSD_CH_LPF_PROFILE_TETRA, DSD_SNR_TETRA_STAGE_CHAIN, 9000.0},
            {DSD_CH_LPF_PROFILE_TETRA, DSD_SNR_TETRA_STAGE_WIDE, 8415.0},
            {DSD_CH_LPF_PROFILE_TETRA, DSD_SNR_TETRA_STAGE_CHANNEL, 12114.0},
            {DSD_CH_LPF_PROFILE_WIDE, DSD_SNR_TETRA_STAGE_CHANNEL, 8415.0},
            {DSD_CH_LPF_PROFILE_WIDE, DSD_SNR_TETRA_STAGE_WIDE, 8415.0},
            {DSD_CH_LPF_PROFILE_6K25, DSD_SNR_TETRA_STAGE_WIDE, 3540.0},
            {DSD_CH_LPF_PROFILE_12K5, DSD_SNR_TETRA_STAGE_WIDE, 6665.0},
            {DSD_CH_LPF_PROFILE_PROVOICE, DSD_SNR_TETRA_STAGE_WIDE, 6665.0},
            {DSD_CH_LPF_PROFILE_P25_C4FM, DSD_SNR_TETRA_STAGE_WIDE, 6665.0},
            {DSD_CH_LPF_PROFILE_P25_CQPSK, DSD_SNR_TETRA_STAGE_WIDE, 7065.0},
            {DSD_CH_LPF_PROFILE_P25_CQPSK, DSD_SNR_TETRA_STAGE_CHAIN, 7065.0},
        };
        for (size_t i = 0; i < sizeof(bw_cases) / sizeof(bw_cases[0]); i++) {
            const struct bw_case* bc = &bw_cases[i];
            const double got = dsd_snr_noise_bandwidth_hz(bc->profile, bc->tetra_stage);
            if (!nearly_equal(got, bc->noise_bw_hz, tol)) {
                DSD_FPRINTF(stderr, "noise bandwidth for profile %d TETRA stage %d: got %.3f expected %.3f\n",
                            bc->profile, bc->tetra_stage, got, bc->noise_bw_hz);
                return 1;
            }
        }
        const double tetra_evm = dsd_snr_bias_evm_db(72000, 4, DSD_CH_LPF_PROFILE_TETRA, DSD_SNR_TETRA_STAGE_CHAIN);
        const double want_tetra_evm = 1.92 + 10.0 * log10(9000.0 / 18000.0);
        if (!nearly_equal(tetra_evm, want_tetra_evm, tol)) {
            DSD_FPRINTF(stderr, "TETRA EVM bias: got %.12f expected %.12f\n", tetra_evm, want_tetra_evm);
            return 1;
        }
        const double inactive_evm = dsd_snr_bias_evm_db(72000, 4, DSD_CH_LPF_PROFILE_TETRA, DSD_SNR_TETRA_STAGE_WIDE);
        const double wide_evm = dsd_snr_bias_evm_db(72000, 4, DSD_CH_LPF_PROFILE_WIDE, DSD_SNR_TETRA_STAGE_WIDE);
        if (!nearly_equal(inactive_evm, wide_evm, tol)) {
            DSD_FPRINTF(stderr, "inactive TETRA EVM bias: got %.12f expected WIDE's %.12f\n", inactive_evm, wide_evm);
            return 1;
        }
    }

    return 0;
}
