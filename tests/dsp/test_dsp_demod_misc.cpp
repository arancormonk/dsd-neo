// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests for remaining demod helpers: deemph_filter, low_pass_real, and dsd_fm_demod plumbing. */

#include <cmath>
#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static int
approx_eq(float a, float b, float tol) {
    return std::fabs(a - b) <= tol;
}

static int
monotonic_nondecreasing(const float* x, int n) {
    for (int i = 1; i < n; i++) {
        if (x[i] < x[i - 1]) {
            return 0;
        }
    }
    return 1;
}

static double
channel_lpf_tone_gain(demod_state* s, int profile, double tone_hz) {
    const int sample_rate = 48000;
    const int complex_samples = 4096;
    const float amp = 0.75f;
    const double two_pi = 6.28318530717958647692;

    DSD_MEMSET(s, 0, sizeof(*s));
    s->rate_in = sample_rate;
    s->rate_out = sample_rate;
    s->rate_out2 = 0;
    s->mode_demod = &raw_demod;
    s->lowpassed = s->input_cb_buf;
    s->lp_len = complex_samples * 2;
    s->channel_lpf_enable = 1;
    s->channel_lpf_profile = profile;

    for (int n = 0; n < complex_samples; n++) {
        double phase = two_pi * tone_hz * (double)n / (double)sample_rate;
        s->input_cb_buf[(size_t)(n << 1) + 0] = amp * (float)cos(phase);
        s->input_cb_buf[(size_t)(n << 1) + 1] = amp * (float)sin(phase);
    }

    full_demod(s);

    double power = 0.0;
    int count = 0;
    const int start = 512; /* Skip FIR startup transient. */
    const int pairs = s->result_len >> 1;
    for (int n = start; n < pairs; n++) {
        double i = (double)s->result[(size_t)(n << 1) + 0];
        double q = (double)s->result[(size_t)(n << 1) + 1];
        power += i * i + q * q;
        count++;
    }
    if (count <= 0) {
        return 0.0;
    }
    return sqrt(power / (double)count) / (double)amp;
}

static int
check_channel_lpf_protected_edges(demod_state* s) {
    struct edge_case {
        int profile;
        double edge_hz;
        const char* name;
    };
    const struct edge_case cases[] = {
        {DSD_CH_LPF_PROFILE_6K25, 3125.0, "6K25"},           {DSD_CH_LPF_PROFILE_12K5, 6250.0, "12K5"},
        {DSD_CH_LPF_PROFILE_PROVOICE, 6250.0, "PROVOICE"},   {DSD_CH_LPF_PROFILE_P25_C4FM, 6250.0, "P25_C4FM"},
        {DSD_CH_LPF_PROFILE_P25_CQPSK, 6250.0, "P25_CQPSK"}, {DSD_CH_LPF_PROFILE_WIDE, 8000.0, "WIDE"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        double gain = channel_lpf_tone_gain(s, cases[i].profile, cases[i].edge_hz);
        if (gain < 0.90) {
            DSD_FPRINTF(stderr, "channel_lpf %s edge %.0f Hz gain %.3f below passband threshold\n", cases[i].name,
                        cases[i].edge_hz, gain);
            return 1;
        }
    }
    return 0;
}

int
main(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return 1;
    }
    DSD_MEMSET(s, 0, sizeof(*s));

    // deemph_filter: step response
    {
        const int N = 64;
        s->result_len = N;
        for (int i = 0; i < N; i++) {
            s->result[i] = 1.0f;
        }
        s->deemph_a = 0.25f;
        s->deemph_avg = 0.0f;
        deemph_filter(s);
        if (!monotonic_nondecreasing(s->result, N)) {
            DSD_FPRINTF(stderr, "deemph_filter: non-monotonic step response\n");
            free(s);
            return 1;
        }
        if (!approx_eq(s->result[N - 1], 1.0f, 1e-4f)) {
            DSD_FPRINTF(stderr, "deemph_filter: final=%f not near 1.0\n", s->result[N - 1]);
            free(s);
            return 1;
        }
    }

    // low_pass_real: average 2:1 from 48k to 24k on constant signal
    {
        const int N = 32;
        s->result_len = N;
        for (int i = 0; i < N; i++) {
            s->result[i] = 0.5f;
        }
        s->rate_in = 48000;
        s->rate_out2 = 24000;
        s->now_lpr = 0.0f;
        s->prev_lpr_index = 0;
        low_pass_real(s);
        if (s->result_len != N / 2) {
            DSD_FPRINTF(stderr, "low_pass_real: result_len=%d want %d\n", s->result_len, N / 2);
            free(s);
            return 1;
        }
        for (int i = 0; i < s->result_len; i++) {
            if (!approx_eq(s->result[i], 0.5f, 1e-4f)) {
                DSD_FPRINTF(stderr, "low_pass_real: out[%d]=%f not ~0.5\n", i, s->result[i]);
                free(s);
                return 1;
            }
        }
    }

    // dsd_fm_demod: differential phase + FLL offset
    {
        /* Three complex samples rotating +90 deg each step. */
        static float iq[6] = {0.5f, 0.0f, 0.0f, 0.5f, -0.5f, 0.0f};
        s->lowpassed = iq;
        s->lp_len = 6; // 3 complex samples
        s->fll_enabled = 1;
        s->fll_freq = 0.003f; // small FLL offset in rad/sample (native float)
        s->pre_r = 0.0f;
        s->pre_j = 0.0f;
        s->fm_demod_history_valid = 0; /* force seeding path */
        dsd_fm_demod(s);
        if (s->result_len != 3) {
            DSD_FPRINTF(stderr, "dsd_fm_demod: result_len=%d want 3\n", s->result_len);
            free(s);
            return 1;
        }
        /* Output is the differential phase of the already-mixed I/Q plus the
         * FLL's per-sample phase advance added back (see dsd_fm_demod comment):
         * the sum represents the absolute instantaneous frequency of the
         * unmixed signal. With the first sample seeded from history the delta
         * is zero, and subsequent samples are +π/2 per step. */
        const float pi_2 = 1.5707963f;
        const float fll_offset = 0.003f; /* full fll_freq contribution */
        if (fabsf(s->result[0] - fll_offset) > 0.01f) {
            DSD_FPRINTF(stderr, "dsd_fm_demod: result[0]=%f want ~%f (fll offset)\n", s->result[0], fll_offset);
            free(s);
            return 1;
        }
        for (int i = 1; i < s->result_len; i++) {
            float expect = pi_2 + fll_offset;
            if (fabsf(s->result[i] - expect) > 0.01f) {
                DSD_FPRINTF(stderr, "dsd_fm_demod: result[%d]=%f want ~%f\n", i, s->result[i], expect);
                free(s);
                return 1;
            }
        }
    }

    if (check_channel_lpf_protected_edges(s) != 0) {
        free(s);
        return 1;
    }

    free(s);
    return 0;
}
