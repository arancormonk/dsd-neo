// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests for remaining demod helpers: deemph_filter, low_pass_real, and dsd_fm_demod plumbing. */

#include <cmath>
#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/runtime/analog_channel.h>
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
channel_lpf_tone_gain_at(demod_state* s, int profile, int analog_width_hz, double tone_hz) {
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
    if (analog_width_hz > 0) {
        s->analog_family = 1;
        s->analog_demod = DSD_ANALOG_DEMOD_FM;
        s->channel_lpf_width_hz = analog_width_hz;
    }

    for (int n = 0; n < complex_samples; n++) {
        double phase = two_pi * tone_hz * (double)n / (double)sample_rate;
        s->input_cb_buf[(size_t)(n << 1) + 0] = amp * (float)cos(phase);
        s->input_cb_buf[(size_t)(n << 1) + 1] = amp * (float)sin(phase);
    }

    full_demod(s);

    double power = 0.0;
    int count = 0;
    const int start = 512; /* Skip FIR startup transient. */
    /* The channel FIR is centred (zero delay) and pads its look-ahead past the
     * block end with the last sample, so the final taps/2 outputs are not a
     * steady-state reading; keep them out of the measurement. */
    const int pairs = (s->result_len >> 1) - (DSD_CHANNEL_LPF_MAX_TAPS / 2);
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

static double
channel_lpf_tone_gain(demod_state* s, int profile, double tone_hz) {
    return channel_lpf_tone_gain_at(s, profile, 0, tone_hz);
}

/*
 * The analog width is the protected passband: a tone at +/- W/2 passes within
 * 1 dB for every NFM width, while the profile (WIDE here) no longer decides it.
 */
static int
check_analog_protected_edges(demod_state* s) {
    const int widths[] = {8000, 12500, 16000, 20000, 25000};
    for (int w : widths) {
        const double edge = (double)w * 0.5;
        const double gain = channel_lpf_tone_gain_at(s, DSD_CH_LPF_PROFILE_WIDE, w, edge);
        const double gain_neg = channel_lpf_tone_gain_at(s, DSD_CH_LPF_PROFILE_WIDE, w, -edge);
        if (gain < 0.891 || gain_neg < 0.891) {
            DSD_FPRINTF(stderr, "analog width %d edge gain %.3f/%.3f below -1 dB\n", w, gain, gain_neg);
            return 1;
        }
        const double outside = channel_lpf_tone_gain_at(s, DSD_CH_LPF_PROFILE_WIDE, w, edge + 1600.0);
        if (outside > 0.01) {
            DSD_FPRINTF(stderr, "analog width %d passes %.4f at W/2 + 1600 Hz\n", w, outside);
            return 1;
        }
    }
    /* A narrow width really narrows the channel the WIDE profile would pass. */
    if (channel_lpf_tone_gain_at(s, DSD_CH_LPF_PROFILE_WIDE, 8000, 7000.0) > 0.01
        || channel_lpf_tone_gain(s, DSD_CH_LPF_PROFILE_WIDE, 7000.0) < 0.9) {
        DSD_FPRINTF(stderr, "8 kHz analog width does not reject a 7 kHz tone that WIDE passes\n");
        return 1;
    }
    return 0;
}

/*
 * The plan cache is keyed on (rate_out, profile, width). A width change alone
 * must redesign; an unchanged key must not.
 */
static int
check_analog_plan_cache(demod_state* s) {
    (void)channel_lpf_tone_gain_at(s, DSD_CH_LPF_PROFILE_WIDE, 16000, 1000.0);
    if (s->channel_lpf_plan_width_hz != 16000 || s->channel_lpf_plan_taps_len != 135) {
        DSD_FPRINTF(stderr, "16 kHz plan: width %d taps %d\n", s->channel_lpf_plan_width_hz,
                    s->channel_lpf_plan_taps_len);
        return 1;
    }
    const float center_16k = s->channel_lpf_plan_taps[67];
    s->channel_lpf_width_hz = 8000;
    s->lowpassed = s->input_cb_buf;
    s->lp_len = 1024;
    full_demod(s);
    if (s->channel_lpf_plan_width_hz != 8000) {
        DSD_FPRINTF(stderr, "plan kept width %d after a width change\n", s->channel_lpf_plan_width_hz);
        return 1;
    }
    /* Center tap = 2 * cutoff / rate before DC normalization; the narrower filter's is smaller. */
    if (!(s->channel_lpf_plan_taps[67] < center_16k - 0.05f)) {
        DSD_FPRINTF(stderr, "8 kHz plan center tap %.4f not below 16 kHz %.4f\n", (double)s->channel_lpf_plan_taps[67],
                    (double)center_16k);
        return 1;
    }
    /* Leaving the analog family returns the design to the profile. */
    s->analog_family = 0;
    s->channel_lpf_width_hz = 0;
    s->lowpassed = s->input_cb_buf;
    s->lp_len = 1024;
    full_demod(s);
    if (s->channel_lpf_plan_width_hz != 0 || s->channel_lpf_plan_taps_len != 135
        || !approx_eq(s->channel_lpf_plan_taps[67], center_16k, 1e-7f)) {
        DSD_FPRINTF(stderr, "profile plan after analog: width %d taps %d\n", s->channel_lpf_plan_width_hz,
                    s->channel_lpf_plan_taps_len);
        return 1;
    }
    return 0;
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

/*
 * The channel, not the filter.
 *
 * P25 CQPSK runs a deliberately roomier 7250 Hz cutoff than the other 12.5 kHz
 * profiles, but the channel it protects is still 12.5 kHz. Reporting the cutoff
 * would widen the spectrum screen's channel column by 2 kHz the moment the
 * modulation flipped C4FM->CQPSK on the same system, which reads as the receiver
 * changing its mind about what it is listening to.
 */
static int
test_channel_lpf_protected_edge(void) {
    const float tol = 0.5f;

    struct {
        int profile;
        float want;
    } cases[] = {
        {DSD_CH_LPF_PROFILE_WIDE, 8000.0f},
        {DSD_CH_LPF_PROFILE_6K25, 3125.0f},
        {DSD_CH_LPF_PROFILE_12K5, 6250.0f},
        {DSD_CH_LPF_PROFILE_PROVOICE, 6250.0f},
        {DSD_CH_LPF_PROFILE_P25_C4FM, 6250.0f},
        {DSD_CH_LPF_PROFILE_P25_CQPSK, 6250.0f},
        /* An id from a newer build must fall back, not widen unpredictably. */
        {99, 8000.0f},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        float got = (float)dsd_channel_lpf_protected_edge_hz(cases[i].profile);
        if (!approx_eq(got, cases[i].want, tol)) {
            DSD_FPRINTF(stderr, "protected_edge(%d): got %f want %f\n", cases[i].profile, (double)got,
                        (double)cases[i].want);
            return 0;
        }
    }
    return 1;
}

int
main(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return 1;
    }
    DSD_MEMSET(s, 0, sizeof(*s));

    if (!test_channel_lpf_protected_edge()) {
        free(s);
        return 1;
    }

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

    // dsd_fm_demod: differential phase
    {
        /* Three complex samples rotating +90 deg each step. */
        static float iq[6] = {0.5f, 0.0f, 0.0f, 0.5f, -0.5f, 0.0f};
        s->lowpassed = iq;
        s->lp_len = 6; // 3 complex samples
        s->pre_r = 0.0f;
        s->pre_j = 0.0f;
        s->fm_demod_history_valid = 0; /* force seeding path */
        dsd_fm_demod(s);
        if (s->result_len != 3) {
            DSD_FPRINTF(stderr, "dsd_fm_demod: result_len=%d want 3\n", s->result_len);
            free(s);
            return 1;
        }
        /* With the first sample seeded from history the delta is zero, and
         * subsequent samples are +pi/2 per step. */
        const float pi_2 = 1.5707963f;
        if (fabsf(s->result[0]) > 0.01f) {
            DSD_FPRINTF(stderr, "dsd_fm_demod: result[0]=%f want ~0\n", s->result[0]);
            free(s);
            return 1;
        }
        for (int i = 1; i < s->result_len; i++) {
            float expect = pi_2;
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

    if (check_analog_protected_edges(s) != 0 || check_analog_plan_cache(s) != 0) {
        free(s);
        return 1;
    }

    free(s);
    return 0;
}
