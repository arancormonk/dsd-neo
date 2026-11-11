// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests for remaining demod helpers: deemph_filter and its compensation, low_pass_real, and dsd_fm_demod
   plumbing. */

#include <cmath>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/mem.h>
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
    s->complex_rate = sample_rate;
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
    /* The channel FIR makes only outputs whose look-ahead it holds (it keeps the
     * last taps/2 samples' outputs pending), so every output is a steady-state
     * reading up to the block's end. */
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

/* The de-emphasis coefficient the front end runs for @p tau_us at @p rate_hz, quantised to Q15 as
   rtl_demod_refresh_audio_coefficients() quantises it. */
static float
deemph_q15_coefficient(int tau_us, int rate_hz) {
    const double alpha = 1.0 - std::exp(-1.0 / ((double)rate_hz * (double)tau_us * 1e-6));
    return (float)((double)std::lrint(alpha * 32768.0) / 32768.0);
}

/* A 1 kHz sine of amplitude 1 through deemph_filter() at s->rate_out; returns the amplitude of what comes out, fitted
   over whole cycles of its second half, past the filter's settling (750 us is under 4 ms). */
static double
deemph_tone_amplitude(demod_state* s) {
    const double pi = 3.14159265358979323846;
    const int n = s->rate_out / 5 < MAXIMUM_BUF_LENGTH ? s->rate_out / 5 : MAXIMUM_BUF_LENGTH;
    s->deemph_avg = 0.0f;
    s->result_len = n;
    for (int i = 0; i < n; i++) {
        s->result[i] = (float)std::sin(2.0 * pi * 1000.0 * (double)i / (double)s->rate_out);
    }
    deemph_filter(s);
    /* Whole cycles: c cycles of 1 kHz are a whole number of samples when c = 1000 / gcd(rate, 1000). */
    int a = s->rate_out;
    int b = 1000;
    while (b != 0) {
        const int t = a % b;
        a = b;
        b = t;
    }
    const int period = s->rate_out / a; /* samples in 1000 / a cycles */
    const int len = ((n / 2) / period) * period;
    double in_phase = 0.0;
    double quadrature = 0.0;
    for (int i = n - len; i < n; i++) {
        const double w = 2.0 * pi * 1000.0 * (double)i / (double)s->rate_out;
        in_phase += (double)s->result[i] * std::sin(w);
        quadrature += (double)s->result[i] * std::cos(w);
    }
    return 2.0 * std::sqrt((in_phase * in_phase) + (quadrature * quadrature)) / (double)len;
}

/* The FM monitor as full_demod() runs it, de-emphasizing at @p tau_us and @p rate_hz. */
static void
deemph_monitor_seed(demod_state* s, int tau_us, int rate_hz) {
    s->mode_demod = &dsd_fm_demod;
    s->output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    s->cqpsk_enable = 0;
    s->deemph = 1;
    s->rate_out = rate_hz;
    s->deemph_a = deemph_q15_coefficient(tau_us, rate_hz);
}

/* dsd_demod_deemph_compensation() is the inverse of what deemph_filter() does to 1 kHz at the coefficient and rate the
   front end runs (issue #518): the de-emphasized tone times the gain comes back to its own level at every monitor and
   replay rate, and the gain matches the filter's closed form there (the default 48 kHz to 0.001 dB). A block that runs
   no de-emphasis gets 1. */
static int
test_deemph_compensation(demod_state* s) {
    static const int kRates[] = {8000, 12000, 16000, 24000, 48000, 72000, 78125};
    static const int kTaus[] = {50, 75, 750};
    int ok = 1;
    DSD_MEMSET(s, 0, sizeof(*s));
    for (int r : kRates) {
        for (int tau : kTaus) {
            deemph_monitor_seed(s, tau, r);
            const double gain = dsd_demod_deemph_compensation(s);
            const double amplitude = deemph_tone_amplitude(s);
            if (std::fabs(amplitude * gain - 1.0) > 1e-4) {
                DSD_FPRINTF(stderr, "deemph compensation %d us @%d: tone %.6f x gain %.6f = %.6f, want 1\n", tau, r,
                            amplitude, gain, amplitude * gain);
                ok = 0;
            }
        }
    }

    /* The closed form at the default 48 kHz, with the Q15 codes the front end runs (11166, 7947, 898). */
    static const struct {
        int tau_us;
        int q15;
        double want_db;
    } kAt48k[] = {{50, 11166, 0.403}, {75, 7947, 0.865}, {750, 898, 13.647}};

    deemph_monitor_seed(s, 75, 48000);
    for (const auto& row : kAt48k) {
        s->deemph_a = (float)row.q15 / 32768.0f;
        if (std::lrint(deemph_q15_coefficient(row.tau_us, 48000) * 32768.0f) != row.q15) {
            DSD_FPRINTF(stderr, "deemph compensation: %d us quantises off %d\n", row.tau_us, row.q15);
            ok = 0;
        }
        const double got_db = 20.0 * std::log10(dsd_demod_deemph_compensation(s));
        if (std::fabs(got_db - row.want_db) > 1e-3) {
            DSD_FPRINTF(stderr, "deemph compensation %d us @48k: %.4f dB, want %.3f\n", row.tau_us, got_db,
                        row.want_db);
            ok = 0;
        }
    }

    /* No de-emphasis runs: off (and AM, which clears it), the FSK discriminator, CQPSK symbols, the raw pass-through,
       and no coefficient or rate. */
    struct {
        const char* name;
        void (*apply)(demod_state*);
    } const kNone[] = {
        {"off", [](demod_state* d) { d->deemph = 0; }},
        {"FSK discriminator", [](demod_state* d) { d->output_kind = DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR; }},
        {"CQPSK", [](demod_state* d) { d->cqpsk_enable = 1; }},
        {"raw pass-through", [](demod_state* d) { d->mode_demod = &raw_demod; }},
        {"no coefficient", [](demod_state* d) { d->deemph_a = 0.0f; }},
        {"no rate", [](demod_state* d) { d->rate_out = 0; }},
    };

    for (const auto& c : kNone) {
        deemph_monitor_seed(s, 75, 48000);
        c.apply(s);
        if (std::fabs(dsd_demod_deemph_compensation(s) - 1.0) > 1e-12) {
            DSD_FPRINTF(stderr, "deemph compensation: %s gave %.6f, want 1\n", c.name,
                        dsd_demod_deemph_compensation(s));
            ok = 0;
        }
    }
    if (std::fabs(dsd_demod_deemph_compensation(NULL) - 1.0) > 1e-12) {
        DSD_FPRINTF(stderr, "deemph compensation: NULL is not 1\n");
        ok = 0;
    }
    DSD_MEMSET(s, 0, sizeof(*s));
    return ok;
}

int
main(void) {
    demod_state* s = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (!s) {
        return 1;
    }
    DSD_MEMSET(s, 0, sizeof(*s));

    if (!test_deemph_compensation(s)) {
        dsd_neo_aligned_free(s);
        return 1;
    }

    if (!test_channel_lpf_protected_edge()) {
        dsd_neo_aligned_free(s);
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
            dsd_neo_aligned_free(s);
            return 1;
        }
        if (!approx_eq(s->result[N - 1], 1.0f, 1e-4f)) {
            DSD_FPRINTF(stderr, "deemph_filter: final=%f not near 1.0\n", s->result[N - 1]);
            dsd_neo_aligned_free(s);
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
            dsd_neo_aligned_free(s);
            return 1;
        }
        for (int i = 0; i < s->result_len; i++) {
            if (!approx_eq(s->result[i], 0.5f, 1e-4f)) {
                DSD_FPRINTF(stderr, "low_pass_real: out[%d]=%f not ~0.5\n", i, s->result[i]);
                dsd_neo_aligned_free(s);
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
            dsd_neo_aligned_free(s);
            return 1;
        }
        /* With the first sample seeded from history the delta is zero, and
         * subsequent samples are +pi/2 per step. */
        const float pi_2 = 1.5707963f;
        if (fabsf(s->result[0]) > 0.01f) {
            DSD_FPRINTF(stderr, "dsd_fm_demod: result[0]=%f want ~0\n", s->result[0]);
            dsd_neo_aligned_free(s);
            return 1;
        }
        for (int i = 1; i < s->result_len; i++) {
            float expect = pi_2;
            if (fabsf(s->result[i] - expect) > 0.01f) {
                DSD_FPRINTF(stderr, "dsd_fm_demod: result[%d]=%f want ~%f\n", i, s->result[i], expect);
                dsd_neo_aligned_free(s);
                return 1;
            }
        }
    }

    if (check_channel_lpf_protected_edges(s) != 0) {
        dsd_neo_aligned_free(s);
        return 1;
    }

    if (check_analog_protected_edges(s) != 0 || check_analog_plan_cache(s) != 0) {
        dsd_neo_aligned_free(s);
        return 1;
    }

    dsd_neo_aligned_free(s);
    return 0;
}
