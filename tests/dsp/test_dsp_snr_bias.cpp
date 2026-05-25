// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/snr_bias.h>
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
        double got_c4fm = dsd_snr_bias_c4fm_db(rate_out, ted_sps, tc->profile);
        if (!nearly_equal(got_c4fm, tc->expected_c4fm, tol)) {
            DSD_FPRINTF(stderr, "SNR bias C4FM mismatch for %s: got %.12f expected %.12f\n", tc->name, got_c4fm,
                        tc->expected_c4fm);
            return 1;
        }
        double got_evm = dsd_snr_bias_evm_db(rate_out, ted_sps, tc->profile);
        if (!nearly_equal(got_evm, tc->expected_evm, tol)) {
            DSD_FPRINTF(stderr, "SNR bias EVM mismatch for %s: got %.12f expected %.12f\n", tc->name, got_evm,
                        tc->expected_evm);
            return 1;
        }
    }

    /* Fallback behavior for invalid inputs should be stable. */
    {
        const double fb_c4fm = dsd_snr_bias_c4fm_db(0, ted_sps, DSD_CH_LPF_PROFILE_WIDE);
        const double fb_evm = dsd_snr_bias_evm_db(rate_out, 0, DSD_CH_LPF_PROFILE_WIDE);
        if (!nearly_equal(fb_c4fm, 7.93, tol)) {
            DSD_FPRINTF(stderr, "SNR bias C4FM fallback mismatch: got %.12f expected %.12f\n", fb_c4fm, 7.93);
            return 1;
        }
        if (!nearly_equal(fb_evm, 2.42, tol)) {
            DSD_FPRINTF(stderr, "SNR bias EVM fallback mismatch: got %.12f expected %.12f\n", fb_evm, 2.42);
            return 1;
        }
    }

    return 0;
}
