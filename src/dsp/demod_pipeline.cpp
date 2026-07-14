// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief DSP demodulation pipeline (FM/complex baseband) implementation.
 *
 * Provides decimation, CQPSK recovery, discrimination, deemphasis, DC block,
 * and audio filtering. Public APIs are declared in `dsp/demod_pipeline.h`.
 */

#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/firdes.h>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/math_utils.h>
#include <dsd-neo/dsp/simd_fir.h>
#include <dsd-neo/dsp/ted.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/mem.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <math.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/dsp/fsk_modem.h"

#ifndef DSD_NEO_RESTRICT
#if defined(_MSC_VER)
#define DSD_NEO_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define DSD_NEO_RESTRICT __restrict__
#else
#define DSD_NEO_RESTRICT
#endif
#endif

static inline int
debug_cqpsk_enabled(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init();
        cfg = dsd_neo_get_config();
    }
    return (cfg && cfg->debug_cqpsk_enable) ? 1 : 0;
}

/* Platform-specific aligned pointer assumption */
template <typename T>
static inline T*
assume_aligned_ptr(T* p, size_t /*align_unused*/) {
    return p;
}

template <typename T>
static inline const T*
assume_aligned_ptr(const T* p, size_t /*align_unused*/) {
    return p;
}

static inline float
phase_delta_small_angle_or_atan2(float im, float re) {
    float abs_im = fabsf(im);
    if (re > 1.0e-7f && abs_im <= (0.35f * re)) {
        float x = im / re;
        float x2 = x * x;
        return x * (1.0f + x2 * (-0.3333333333333333f + x2 * 0.2f));
    }
    return atan2f(im, re);
}

static inline float
atan_unit_approx(float x) {
    float ax = fabsf(x);
    return x * (0.78539816339744830962f - (ax - 1.0f) * (0.2447f + 0.0663f * ax));
}

static inline float
atan2_qpsk_approx(float y, float x) {
    if (x == 0.0f && y == 0.0f) {
        return 0.0f;
    }

    float ax = fabsf(x);
    float ay = fabsf(y);
    if (ax >= ay) {
        float angle = atan_unit_approx(y / x);
        if (x < 0.0f) {
            angle += (y < 0.0f) ? -3.14159265358979323846f : 3.14159265358979323846f;
        }
        return angle;
    }

    float angle = atan_unit_approx(x / y);
    return (y > 0.0f) ? (1.57079632679489661923f - angle) : (-1.57079632679489661923f - angle);
}

static inline float
clamp_float(float value, float lo, float hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

/* Fixed channel low-pass for high-rate mode.
 *
 * Profiles (see DSD_CH_LPF_PROFILE_*):
 *   - Wide/analog: 8000 Hz protected passband edge.
 *   - 6.25 kHz modes: 3125 Hz protected passband edge (NXDN48/dPMR/D-STAR).
 *   - 12.5 kHz 4FSK modes: 6250 Hz protected passband edge (DMR/NXDN96/X2TDMA/YSF/M17).
 *   - ProVoice: 6250 Hz protected passband edge.
 *   - P25 C4FM: 6250 Hz protected passband edge.
 *   - P25 CQPSK/LSM: 6250 Hz protected passband edge plus guard.
 *
 * GNU Radio firdes::low_pass() interprets cutoff_freq as the center of the
 * transition band, not the last flat passband frequency. The design centers
 * below include half of the transition width as guard so nominal channel edges
 * do not sit on the filter skirt.
 *
 * Fixed 63-tap Blackman prototypes provide the allocation/design fallback; preferred taps are
 * generated per sample rate to preserve the intended spectral shape at any Fs.
 *
 * At 48 kHz with 1200 Hz transition width:
 *   - Hamming: ntaps = (53 * 48000) / (22 * 1200) = 97
 *   - Blackman: ntaps = (74 * 48000) / (22 * 1200) = 135
 * Size 144 provides headroom for higher sample rates. */
static const int kChannelLpfTaps = 144;
static const double kChannelLpfTransitionHz = 1200.0;
static const double kChannelLpfGuardHz = kChannelLpfTransitionHz * 0.5;
static const double kChannelLpfWideCutoffHz = 8000.0 + kChannelLpfGuardHz;
static const double kChannelLpf6k25CutoffHz = 3125.0 + kChannelLpfGuardHz;
static const double kChannelLpf12k5CutoffHz = 6250.0 + kChannelLpfGuardHz;
static const double kChannelLpfProvoiceCutoffHz = 6250.0 + kChannelLpfGuardHz;
static const double kChannelLpfP25C4fmCutoffHz = 6250.0 + kChannelLpfGuardHz;
static const double kChannelLpfP25CqpskCutoffHz = 7250.0;
/* Fixed fallback filters are 63 taps (designed for 24 kHz). Only used when
 * dynamic filter generation fails; prefer dynamically generated taps. */
static const int kChannelLpfFallbackTaps = 63;
static const float channel_lpf_wide[kChannelLpfFallbackTaps] = {
    0.0f,
    0.0f,
    -1.0f / 32768.0f,
    3.0f / 32768.0f,
    0.0f,
    -9.0f / 32768.0f,
    14.0f / 32768.0f,
    0.0f,
    -28.0f / 32768.0f,
    39.0f / 32768.0f,
    0.0f,
    -68.0f / 32768.0f,
    88.0f / 32768.0f,
    0.0f,
    -142.0f / 32768.0f,
    178.0f / 32768.0f,
    0.0f,
    -271.0f / 32768.0f,
    330.0f / 32768.0f,
    0.0f,
    -486.0f / 32768.0f,
    586.0f / 32768.0f,
    0.0f,
    -859.0f / 32768.0f,
    1047.0f / 32768.0f,
    0.0f,
    -1625.0f / 32768.0f,
    2111.0f / 32768.0f,
    0.0f,
    -4441.0f / 32768.0f,
    8995.0f / 32768.0f,
    21845.0f / 32768.0f,
    8995.0f / 32768.0f,
    -4441.0f / 32768.0f,
    0.0f,
    2111.0f / 32768.0f,
    -1625.0f / 32768.0f,
    0.0f,
    1047.0f / 32768.0f,
    -859.0f / 32768.0f,
    0.0f,
    586.0f / 32768.0f,
    -486.0f / 32768.0f,
    0.0f,
    330.0f / 32768.0f,
    -271.0f / 32768.0f,
    0.0f,
    178.0f / 32768.0f,
    -142.0f / 32768.0f,
    0.0f,
    88.0f / 32768.0f,
    -68.0f / 32768.0f,
    0.0f,
    39.0f / 32768.0f,
    -28.0f / 32768.0f,
    0.0f,
    14.0f / 32768.0f,
    -9.0f / 32768.0f,
    0.0f,
    3.0f / 32768.0f,
    -1.0f / 32768.0f,
    0.0f,
    0.0f,
};

/* Fixed digital-profile taps (fc≈5 kHz @ 24 kHz, 63 taps). Designed as
   Blackman-windowed sinc and normalized to unity DC gain. */
static const float channel_lpf_digital[kChannelLpfFallbackTaps] = {
    0.0f,
    0.0f,
    0.0f,
    -3.0f / 32768.0f,
    -4.0f / 32768.0f,
    5.0f / 32768.0f,
    15.0f / 32768.0f,
    0.0f,
    -31.0f / 32768.0f,
    -22.0f / 32768.0f,
    42.0f / 32768.0f,
    68.0f / 32768.0f,
    -26.0f / 32768.0f,
    -130.0f / 32768.0f,
    -43.0f / 32768.0f,
    178.0f / 32768.0f,
    180.0f / 32768.0f,
    -156.0f / 32768.0f,
    -368.0f / 32768.0f,
    0.0f,
    542.0f / 32768.0f,
    339.0f / 32768.0f,
    -579.0f / 32768.0f,
    -859.0f / 32768.0f,
    313.0f / 32768.0f,
    1492.0f / 32768.0f,
    486.0f / 32768.0f,
    -2111.0f / 32768.0f,
    -2367.0f / 32768.0f,
    2564.0f / 32768.0f,
    10033.0f / 32768.0f,
    13654.0f / 32768.0f,
    10033.0f / 32768.0f,
    2564.0f / 32768.0f,
    -2367.0f / 32768.0f,
    -2111.0f / 32768.0f,
    486.0f / 32768.0f,
    1492.0f / 32768.0f,
    313.0f / 32768.0f,
    -859.0f / 32768.0f,
    -579.0f / 32768.0f,
    339.0f / 32768.0f,
    542.0f / 32768.0f,
    0.0f,
    -368.0f / 32768.0f,
    -156.0f / 32768.0f,
    180.0f / 32768.0f,
    178.0f / 32768.0f,
    -43.0f / 32768.0f,
    -130.0f / 32768.0f,
    -26.0f / 32768.0f,
    68.0f / 32768.0f,
    42.0f / 32768.0f,
    -22.0f / 32768.0f,
    -31.0f / 32768.0f,
    0.0f,
    15.0f / 32768.0f,
    5.0f / 32768.0f,
    -4.0f / 32768.0f,
    -3.0f / 32768.0f,
    0.0f,
    0.0f,
    0.0f,
};

/* ---------------- Post-demod audio polyphase decimator (M > 1) -------------- */
/*
 * Lightweight 1/M polyphase decimator for audio. Designs a windowed-sinc
 * prototype (Q15) for the given M and streams with a circular history.
 * Keeps state in demod_state to persist across blocks.
 */
static inline int
audio_polydecim_requires_redesign(const struct demod_state* d, int M, int K) {
    return (!d->post_polydecim_taps || !d->post_polydecim_hist || d->post_polydecim_M != M || d->post_polydecim_K != K);
}

static void
audio_polydecim_release(struct demod_state* d) {
    if (!d) {
        return;
    }
    if (d->post_polydecim_taps) {
        dsd_neo_aligned_free(d->post_polydecim_taps);
        d->post_polydecim_taps = NULL;
    }
    if (d->post_polydecim_hist) {
        dsd_neo_aligned_free(d->post_polydecim_hist);
        d->post_polydecim_hist = NULL;
    }
}

static int
audio_polydecim_allocate(struct demod_state* d, int K) {
    d->post_polydecim_taps = static_cast<float*>(dsd_neo_aligned_malloc((size_t)K * sizeof(float)));
    d->post_polydecim_hist = static_cast<float*>(dsd_neo_aligned_malloc((size_t)K * sizeof(float)));
    if (!d->post_polydecim_taps || !d->post_polydecim_hist) {
        audio_polydecim_release(d);
        return 0;
    }
    DSD_MEMSET(d->post_polydecim_hist, 0, (size_t)K * sizeof(float));
    d->post_polydecim_hist_head = 0;
    d->post_polydecim_phase = 0;
    return 1;
}

static void
audio_polydecim_design_taps(float* taps, int K, int M) {
    const double fc = 0.45 / (double)M;
    const int mid = (K - 1) / 2;
    double gain = 0.0;
    for (int n = 0; n < K; n++) {
        const int m = n - mid;
        const double w = 0.54 - 0.46 * cos(2.0 * 3.14159265358979323846 * (double)n / (double)(K - 1));
        const double h = 2.0 * fc * dsd_neo_sinc(2.0 * fc * (double)m);
        gain += h * w;
    }
    if (gain == 0.0) {
        gain = 1.0;
    }
    for (int n = 0; n < K; n++) {
        const int m = n - mid;
        const double w = 0.54 - 0.46 * cos(2.0 * 3.14159265358979323846 * (double)n / (double)(K - 1));
        const double h = 2.0 * fc * dsd_neo_sinc(2.0 * fc * (double)m);
        taps[n] = (float)((h * w) / gain);
    }
}

static inline int
audio_polydecim_passthrough(const float* in, int in_len, float* out) {
    if (in && out && in_len > 0 && in != out) {
        DSD_MEMCPY(out, in, (size_t)in_len * sizeof(float));
    }
    return in_len;
}

static inline float
audio_polydecim_dot_latest(const float* hist, const float* taps, int K, int head) {
    int idx = head - 1;
    if (idx < 0) {
        idx += K;
    }
    float acc = 0.0f;
    for (int k = 0; k < K; k++) {
        acc += hist[idx] * taps[k];
        idx--;
        if (idx < 0) {
            idx += K;
        }
    }
    return acc;
}

static void
audio_polydecim_ensure(struct demod_state* d, int M) {
    if (!d) {
        return;
    }
    if (M <= 1) {
        d->post_polydecim_enabled = 0;
        return;
    }
    const int K = 16; /* taps */
    if (!audio_polydecim_requires_redesign(d, M, K)) {
        d->post_polydecim_enabled = 1;
        return;
    }
    audio_polydecim_release(d);
    if (!audio_polydecim_allocate(d, K)) {
        d->post_polydecim_enabled = 0;
        return;
    }
    /* Design windowed-sinc low-pass: fc ≈ 0.45 / M (normalized), Hamming window. */
    audio_polydecim_design_taps(d->post_polydecim_taps, K, M);
    d->post_polydecim_M = M;
    d->post_polydecim_K = K;
    d->post_polydecim_enabled = 1;
}

static int
audio_polydecim_process(struct demod_state* d, const float* in, int in_len, float* out) {
    if (!d || !d->post_polydecim_enabled || !d->post_polydecim_taps || !d->post_polydecim_hist || in_len <= 0) {
        return audio_polydecim_passthrough(in, in_len, out);
    }
    const int K = d->post_polydecim_K;
    const int M = d->post_polydecim_M;
    int head = d->post_polydecim_hist_head;
    int phase = d->post_polydecim_phase;
    const float* taps = d->post_polydecim_taps;
    int out_len = 0;
    for (int n = 0; n < in_len; n++) {
        /* push */
        d->post_polydecim_hist[head] = in[n];
        if (++head == K) {
            head = 0;
        }
        /* phase accum */
        if (++phase < M) {
            continue;
        }
        phase -= M;
        out[out_len++] = audio_polydecim_dot_latest(d->post_polydecim_hist, taps, K, head);
    }
    d->post_polydecim_hist_head = head;
    d->post_polydecim_phase = phase;
    return out_len;
}

/* ---------------- Fixed channel LPF (complex, no decimation) ----------------- */

static int
channel_lpf_design_low_pass(double sample_rate, double cutoff_hz, float* taps_out, int max_taps) {
    if (sample_rate <= 0.0 || !taps_out || max_taps <= 0) {
        return -1;
    }

    const double nyquist = sample_rate * 0.5;
    const double max_cutoff = nyquist * 0.90;
    double cutoff = cutoff_hz;
    if (cutoff < 100.0) {
        cutoff = 100.0;
    }
    if (cutoff > max_cutoff) {
        cutoff = max_cutoff;
    }

    return dsd_firdes_low_pass(1.0, sample_rate, cutoff, kChannelLpfTransitionHz, DSD_WIN_BLACKMAN, taps_out, max_taps);
}

static const float*
channel_lpf_fallback_taps(int profile, int* taps_len) {
    if (!taps_len) {
        return channel_lpf_wide;
    }
    switch (profile) {
        case DSD_CH_LPF_PROFILE_6K25:
        case DSD_CH_LPF_PROFILE_12K5: *taps_len = kChannelLpfFallbackTaps; return channel_lpf_digital;
        case DSD_CH_LPF_PROFILE_PROVOICE:
        case DSD_CH_LPF_PROFILE_P25_C4FM:
        case DSD_CH_LPF_PROFILE_P25_CQPSK:
        case DSD_CH_LPF_PROFILE_WIDE:
        default: *taps_len = kChannelLpfFallbackTaps; return channel_lpf_wide;
    }
}

static double
channel_lpf_cutoff_for_profile(int profile) {
    switch (profile) {
        case DSD_CH_LPF_PROFILE_6K25: return kChannelLpf6k25CutoffHz;
        case DSD_CH_LPF_PROFILE_12K5: return kChannelLpf12k5CutoffHz;
        case DSD_CH_LPF_PROFILE_PROVOICE: return kChannelLpfProvoiceCutoffHz;
        case DSD_CH_LPF_PROFILE_P25_C4FM: return kChannelLpfP25C4fmCutoffHz;
        case DSD_CH_LPF_PROFILE_P25_CQPSK: return kChannelLpfP25CqpskCutoffHz;
        case DSD_CH_LPF_PROFILE_WIDE:
        default: return kChannelLpfWideCutoffHz;
    }
}

/**
 * @brief Ensure channel LPF taps are generated for this demodulator state.
 *
 * Uses dsd_firdes_low_pass() which is a direct port of GNU Radio's firdes::low_pass().
 * Keeps absolute cutoffs in Hz constant across sample rates and avoids global
 * per-block profile dispatch.
 */
static void
channel_lpf_ensure_plan(struct demod_state* d) {
    if (!d) {
        return;
    }
    const int profile = d->channel_lpf_profile;
    const int rate_out = d->rate_out;
    if (d->channel_lpf_plan_taps_len > 0 && d->channel_lpf_plan_rate_out == rate_out
        && d->channel_lpf_plan_profile == profile) {
        return;
    }

    int taps_len = 0;
    if (rate_out > 0) {
        taps_len = channel_lpf_design_low_pass((double)rate_out, channel_lpf_cutoff_for_profile(profile),
                                               d->channel_lpf_plan_taps, kChannelLpfTaps);
    }
    if (taps_len <= 0) {
        int fallback_len = 0;
        const float* fallback = channel_lpf_fallback_taps(profile, &fallback_len);
        DSD_MEMCPY(d->channel_lpf_plan_taps, fallback, (size_t)fallback_len * sizeof(float));
        taps_len = fallback_len;
    }
    d->channel_lpf_plan_rate_out = rate_out;
    d->channel_lpf_plan_profile = profile;
    d->channel_lpf_plan_taps_len = taps_len;
}

static void
channel_lpf_apply(struct demod_state* d) {
    if (!d || !d->channel_lpf_enable || d->lp_len < 2) {
        return;
    }

    channel_lpf_ensure_plan(d);
    const float* taps = d->channel_lpf_plan_taps;
    int taps_len = d->channel_lpf_plan_taps_len;
    if (!taps || taps_len < 3) {
        return;
    }

    const int hist_len = taps_len - 1;
    const int N = d->lp_len >> 1; /* complex samples */
    if (hist_len > d->channel_lpf_hist_len) {
        d->channel_lpf_hist_len = hist_len;
    }

    const float* in = assume_aligned_ptr(d->lowpassed, DSD_NEO_ALIGN);
    float* out = (d->lowpassed == d->hb_workbuf) ? d->timing_buf : d->hb_workbuf;
    float* hi = assume_aligned_ptr(d->channel_lpf_hist_i, DSD_NEO_ALIGN);
    float* hq = assume_aligned_ptr(d->channel_lpf_hist_q, DSD_NEO_ALIGN);

    /* Use SIMD-dispatched complex symmetric FIR */
    simd_fir_complex_apply(in, d->lp_len, out, hi, hq, taps, taps_len);

    d->lowpassed = out;
    d->lp_len = N << 1;
}

/**
 * @brief Boxcar low-pass and decimate by step (no wraparound).
 *
 * Length must be a multiple of step.
 *
 * @param signal2 In/out buffer of samples.
 * @param len     Length of input buffer.
 * @param step    Decimation factor.
 * @return New length after decimation.
 */
int
low_pass_simple(float* signal2, int len, int step) {
    int i, i2;
    if (step <= 0) {
        return len;
    }
    for (i = 0; i + (step - 1) < len; i += step) {
        float sum = 0.0f;
        for (i2 = 0; i2 < step; i2++) {
            sum += signal2[i + i2];
        }
        // Normalize by step with rounding. Writes output at i/step index.
        float val = sum / (float)step;
        signal2[i / step] = val;
    }
    /* Duplicate the final sample to provide one-sample lookahead for callers
	   that expect at least one extra element. Only do this when there is
	   capacity (i.e., out_len < len) to avoid writing past the end. */
    int out_len = len / step;
    if (out_len > 0 && out_len < len) {
        signal2[out_len] = signal2[out_len - 1];
    }
    return out_len;
}

/**
 * @brief Simple square window FIR on real samples with decimation to rate_out2.
 *
 * @param s Demodulator state (uses result buffer and decimation state).
 */
void
low_pass_real(struct demod_state* s) {
    int i = 0, i2 = 0;
    float* r = assume_aligned_ptr(s->result, DSD_NEO_ALIGN);
    int fast = (int)s->rate_in;
    int slow = s->rate_out2;
    int decim = (slow != 0) ? (fast / slow) : 1;
    if (decim < 1) {
        decim = 1;
    }
    float recip = 1.0f / (float)decim;
    while (i < s->result_len) {
        s->now_lpr += r[i];
        i++;
        s->prev_lpr_index += slow;
        if (s->prev_lpr_index < fast) {
            continue;
        }
        r[i2] = s->now_lpr * recip;
        s->prev_lpr_index -= fast;
        s->now_lpr = 0.0f;
        i2 += 1;
    }
    s->result_len = i2;
}

/**
 * @brief Perform FM discrimination on interleaved low-passed I/Q to produce audio PCM.
 *
 * @param fm Demodulator state (uses lowpassed as input, writes to result).
 */
void
dsd_fm_demod(struct demod_state* fm) {
    const int pairs = fm->lp_len >> 1; /* complex samples */
    if (pairs <= 0) {
        fm->result_len = 0;
        return;
    }

    const float* iq = assume_aligned_ptr(fm->lowpassed, DSD_NEO_ALIGN);
    float* out = assume_aligned_ptr(fm->result, DSD_NEO_ALIGN);

    float prev_r = fm->pre_r;
    float prev_j = fm->pre_j;
    /* Seed history on first use so the first phase delta is well-defined.
       Uses an explicit validity flag rather than a (prev==0) heuristic, which
       would false-seed when a legitimately-zero sample happened to arrive as
       the first sample of a new stream. */
    if (!fm->fm_demod_history_valid) {
        prev_r = iq[0];
        prev_j = iq[1];
        fm->fm_demod_history_valid = 1;
    }

    for (int n = 0; n < pairs; n++) {
        float cr = iq[(size_t)(n << 1) + 0];
        float cj = iq[(size_t)(n << 1) + 1];
        /* z_n * conj(z_{n-1}) => phase delta; amplitude cancels inside atan2 */
        float re = cr * prev_r + cj * prev_j;
        float im = cj * prev_r - cr * prev_j;
        float angle = phase_delta_small_angle_or_atan2(im, re);
        out[n] = angle;
        prev_r = cr;
        prev_j = cj;
    }

    fm->pre_r = prev_r;
    fm->pre_j = prev_j;
    fm->result_len = pairs;
}

/**
 * @brief Pass-through demodulator: copies low-passed samples to output unchanged.
 *
 * @param fm Demodulator state (copies lowpassed to result).
 */
void
raw_demod(struct demod_state* fm) {
    int i;
    for (i = 0; i < fm->lp_len; i++) {
        fm->result[i] = fm->lowpassed[i];
    }
    fm->result_len = fm->lp_len;
}

/**
 * @brief Differential phasor (OP25-style) applied before the Costas loop.
 *
 * Implements GNU Radio's `digital.diff_phasor_cc`: y[n] = x[n] * conj(x[n-1]).
 * Maintains the previous raw complex sample across blocks via
 * cqpsk_diff_prev_r/j. Treats inputs as Q15 to keep output on the same scale
 * as the incoming complex stream.
 *
 * @param d Demodulator state (in-place transform of interleaved I/Q in lowpassed).
 */
static void
cqpsk_diff_phasor(struct demod_state* d) {
    if (!d || !d->lowpassed || d->lp_len < 2) {
        return;
    }
    const int pairs = d->lp_len >> 1;
    float* iq = assume_aligned_ptr(d->lowpassed, DSD_NEO_ALIGN);
    float prev_r = d->cqpsk_diff_prev_r;
    float prev_j = d->cqpsk_diff_prev_j;

    for (int n = 0; n < pairs; n++) {
        float cr = iq[(size_t)(n << 1) + 0];
        float cj = iq[(size_t)(n << 1) + 1];

        float out_r = cr * prev_r + cj * prev_j;
        float out_j = cj * prev_r - cr * prev_j;

        iq[(size_t)(n << 1) + 0] = out_r;
        iq[(size_t)(n << 1) + 1] = out_j;

        prev_r = cr;
        prev_j = cj;
    }

    d->cqpsk_diff_prev_r = prev_r;
    d->cqpsk_diff_prev_j = prev_j;
}

/**
 * @brief QPSK helper demodulator: copy I channel from interleaved complex baseband.
 *
 * Assumes fm->lowpassed holds interleaved I/Q samples that have already passed
 * through CQPSK processing (front-end filtering, OP25 Gardner timing, Costas).
 * Produces a single real stream (I only) for the symbol sampler path.
 */
/**
 * @brief Phase extractor for CQPSK differential phasors (OP25-compatible).
 *
 * Expects fm->lowpassed to hold symbol-rate CQPSK samples after:
 *   op25_gardner_cc -> op25_diff_phasor_cc -> op25_costas_loop_cc
 * (differenced and carrier-corrected). Converts each complex phasor to a
 * scaled float symbol value:
 *   theta_n = arg(z_n) * (4/pi)
 *
 * The 4/pi scaling (OP25: multiply_const_ff(4.0/pi)) maps CQPSK constellation
 * points at ±45°/±135° to symbol levels ~{-3, -1, +1, +3}, matching OP25's
 * fsk4_slicer_fb input expectation.
 *
 * @param fm Demodulator state (reads interleaved I/Q in lowpassed, writes symbols to result).
 */
void
qpsk_differential_demod(struct demod_state* fm) {
    if (!fm || !fm->lowpassed || fm->lp_len < 2) {
        if (fm) {
            fm->result_len = 0;
        }
        return;
    }

    const int pairs = fm->lp_len >> 1; /* complex samples */
    const float* iq = assume_aligned_ptr(fm->lowpassed, DSD_NEO_ALIGN);
    float* out = assume_aligned_ptr(fm->result, DSD_NEO_ALIGN);

    /* OP25 scaling: 4/pi maps ±pi/4 (±45°) to ±1, ±3*pi/4 (±135°) to ±3 */
    const float k4_over_pi = 4.0f / 3.14159265358979323846f; /* ~1.2732 */

    for (int n = 0; n < pairs; n++) {
        float phase = atan2_qpsk_approx(iq[(size_t)(n << 1) + 1], iq[(size_t)(n << 1) + 0]);
        out[n] = phase * k4_over_pi;
    }

    fm->result_len = pairs;
}

/*
 * OP25-style RMS AGC for CQPSK path.
 *
 * Direct port of op25/gr-op25_repeater/apps/rms_agc.py which uses:
 *   rms_agc.rms_agc(alpha=0.45, reference=0.85)
 *
 * The rms_agc.py algorithm (from Daniel Estevez):
 *   1. Compute RMS of input: rms = blocks.rms_cf(alpha)
 *   2. Scale: scaled_rms = rms / reference
 *   3. Add epsilon to avoid division by zero: scaled_rms += 1e-18
 *   4. Divide input by scaled_rms: out = in / scaled_rms
 *
 * The underlying blocks.rms_cf computes:
 *   d_avg = d_beta * d_avg + d_alpha * mag_sqrd
 *   rms = sqrt(d_avg)
 * where d_beta = 1 - d_alpha
 *
 * OP25 parameters: alpha=0.45, reference=0.85
 *
 * This matches op25/gr-op25_repeater/lib/rmsagc_ff_impl.cc exactly:
 *   d_avg = d_beta*d_avg + d_alpha*mag_sqrd;
 *   if (d_avg > 0)
 *       out[i] = d_gain * in[i] / sqrt(d_avg);
 *   else
 *       out[i] = d_gain * in[i];
 *
 * Copyright 2005,2010,2013 Free Software Foundation, Inc.
 * Copyright 2020 Graham J. Norbury, gnorbury@bondcar.com
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
static inline void
cqpsk_rms_agc(struct demod_state* d) {
    if (!d || !d->lowpassed || d->lp_len < 2) {
        return;
    }

    /* OP25 parameters from p25_demodulator.py line 409:
     *   self.agc = rms_agc.rms_agc(0.45, 0.85)
     * where alpha=0.45, reference=0.85 */
    const float alpha = 0.45f;
    const float beta = 1.0f - alpha; /* 0.55 */
    const float gain = 0.85f;        /* reference/k parameter */

    float* iq = d->lowpassed;
    const int pairs = d->lp_len >> 1;

    /* Get running average from state (initialized to 1.0 to avoid startup spike) */
    float avg = d->cqpsk_agc_avg;
    if (avg <= 0.0f) {
        avg = 1.0f; /* OP25 rmsagc_ff_impl.cc: set_alpha sets d_avg = 1.0 */
    }

    /* Process each complex sample exactly as op25's rmsagc_ff_impl.cc:
     *   d_avg = d_beta*d_avg + d_alpha*mag_sqrd;
     *   out = d_gain * in / sqrt(d_avg); */
    for (int i = 0; i < pairs; i++) {
        float I = iq[(size_t)(i << 1)];
        float Q = iq[(size_t)(i << 1) + 1];

        /* mag_sqrd = I*I + Q*Q (complex magnitude squared) */
        float mag_sqrd = I * I + Q * Q;

        /* Update running average: d_avg = d_beta*d_avg + d_alpha*mag_sqrd */
        avg = beta * avg + alpha * mag_sqrd;

        /* Compute gain and apply: out = gain * in / sqrt(avg) */
        if (avg > 0.0f) {
            float scale = gain / sqrtf(avg);
            iq[(size_t)(i << 1)] = I * scale;
            iq[(size_t)(i << 1) + 1] = Q * scale;
        }
        /* else: pass through unchanged (matches op25 behavior) */
    }

    /* Save state for next block */
    d->cqpsk_agc_avg = avg;
}

/**
 * @brief Apply post-demod deemphasis IIR filter.
 *
 * @param fm Demodulator state (reads/writes result, updates deemph_avg).
 */
void
deemph_filter(struct demod_state* fm) {
    float avg = fm->deemph_avg; /* per-instance state */
    float* res = assume_aligned_ptr(fm->result, DSD_NEO_ALIGN);
    float alpha = fm->deemph_a;
    if (alpha < 0.0f) {
        alpha = 0.0f;
    }
    if (alpha > 1.0f) {
        alpha = 1.0f;
    }
    /* Single-pole IIR: avg += (x - avg) * alpha */
    for (int i = 0; i < fm->result_len; i++) {
        float d = res[i] - avg;
        avg += d * alpha;
        res[i] = avg;
    }
    fm->deemph_avg = avg; /* write back state */
}

/**
 * @brief Apply a simple DC blocking (leaky integrator high-pass) filter to audio.
 *
 * @param fm Demodulator state (reads/writes result, updates dc_avg).
 */
void
dc_block_filter(struct demod_state* fm) {
    /* Leaky integrator high-pass: dc += (x - dc) / 2^k; y = x - dc */
    float dc = fm->dc_avg;
    const int k = 11; /* cutoff ~ Fs / (2π * 2^k) for small alpha=2^-k (k in 10..12) */
    float k_scale = 1.0f / (float)(1 << k);
    float* res = assume_aligned_ptr(fm->result, DSD_NEO_ALIGN);
    for (int i = 0; i < fm->result_len; i++) {
        float x = res[i];
        dc += (x - dc) * k_scale;
        float y = x - dc;
        res[i] = y;
    }
    fm->dc_avg = dc;
}

/**
 * @brief Apply a simple one-pole low-pass filter to audio.
 *
 * @param fm Demodulator state (reads/writes result, updates audio_lpf_state).
 */
void
audio_lpf_filter(struct demod_state* fm) {
    if (!fm->audio_lpf_enable) {
        return;
    }
    float* res = assume_aligned_ptr(fm->result, DSD_NEO_ALIGN);
    float y = fm->audio_lpf_state;
    float alpha = fm->audio_lpf_alpha;
    if (alpha < 0.0f) {
        alpha = 0.0f;
    }
    if (alpha > 1.0f) {
        alpha = 1.0f;
    }
    for (int i = 0; i < fm->result_len; i++) {
        float x = res[i];
        float d = x - y;
        y += d * alpha;
        res[i] = y;
    }
    fm->audio_lpf_state = y;
}

/**
 * @brief Calculate mean power (squared RMS) with DC bias removed.
 *
 * @param samples Input samples buffer.
 * @param len     Number of samples.
 * @param step    Step size for sampling.
 * @return Mean power (squared RMS) with DC bias removed.
 */
float
mean_power(const float* samples, int len, int step) {
    double p = 0.0;
    double t = 0.0;
    for (int i = 0; i < len; i += step) {
        double s = (double)samples[i];
        t += s;
        p += s * s;
    }
    /* DC-corrected energy ≈ p - (t^2)/len */
    double dc_corr = 0.0;
    if (len > 0) {
        dc_corr = (t * t) / (double)len;
    }
    double energy = p - dc_corr;
    if (energy < 0.0) {
        energy = 0.0;
    }
    return (float)(energy / (double)(len > 0 ? len : 1));
}

/* Optional complex DC blocker prior to FM discrimination (per-sample leaky integrator) */
static inline void
iq_dc_block(struct demod_state* d) {
    if (!d || !d->iq_dc_block_enable || !d->lowpassed || d->lp_len < 2) {
        return;
    }
    int k = d->iq_dc_shift;
    if (k < 6) {
        k = 6;
    }
    if (k > 15) {
        k = 15;
    }
    float* iq = d->lowpassed;
    const int pairs = d->lp_len >> 1;
    float dcI = d->iq_dc_avg_r;
    float dcQ = d->iq_dc_avg_i;
    float alpha = 1.0f / (float)(1 << k);
    for (int n = 0; n < pairs; n++) {
        float I = iq[(size_t)(n << 1) + 0];
        float Q = iq[(size_t)(n << 1) + 1];
        /* Update leaky integrator toward current sample */
        dcI += (I - dcI) * alpha;
        dcQ += (Q - dcQ) * alpha;
        float yI = I - dcI;
        float yQ = Q - dcQ;
        iq[(size_t)(n << 1) + 0] = yI;
        iq[(size_t)(n << 1) + 1] = yQ;
    }
    d->iq_dc_avg_r = dcI;
    d->iq_dc_avg_i = dcQ;
}

/**
 * @brief Apply stage-wise half-band decimation for complex baseband.
 */
static void
full_demod_apply_halfband_decimation(struct demod_state* d) {
    if (!d || d->downsample_passes <= 0) {
        return;
    }
    int in_len = d->lp_len;
    float* src = d->lowpassed;
    float* dst = d->hb_workbuf;
    for (int i = 0; i < d->downsample_passes; i++) {
        const float* taps = (i == 0) ? hb31_q15_taps : hb_q15_taps;
        int taps_len = (i == 0) ? 31 : HB_TAPS;
        int out_len = simd_hb_decim2_complex(src, in_len, dst, d->hb_hist_i[i], d->hb_hist_q[i], taps, taps_len);
        src = dst;
        in_len = out_len;
        dst = (src == d->hb_workbuf) ? d->lowpassed : d->hb_workbuf;
    }
    d->lowpassed = src;
    d->lp_len = in_len;
}

static void
full_demod_update_channel_state(struct demod_state* d) {
    channel_lpf_apply(d);
    if (d->lowpassed && d->lp_len >= 2) {
        int n = (d->lp_len > 512) ? 512 : d->lp_len;
        d->channel_pwr = mean_power(d->lowpassed, n, 1);
    }
    if (d->lowpassed && d->lp_len > 0 && d->channel_squelch_level > 0.0f && d->channel_pwr < d->channel_squelch_level) {
        d->channel_squelched = 1;
        d->squelch_gate_open = 0;
        for (int k = 0; k < d->lp_len; k++) {
            d->lowpassed[k] = 0.0f;
        }
        return;
    }
    d->channel_squelched = 0;
    d->squelch_gate_open = 1;
}

static int
full_demod_emit_zero_cqpsk_symbols(struct demod_state* d) {
    if (!d->cqpsk_enable || !d->channel_squelched) {
        return 0;
    }
    int in_pairs = d->lp_len >> 1;
    int sps = (d->ted_sps > 0) ? d->ted_sps : 5;
    int out_syms = (in_pairs + sps - 1) / sps;
    if (out_syms < 1) {
        out_syms = 1;
    } else if (out_syms > MAXIMUM_BUF_LENGTH) {
        out_syms = MAXIMUM_BUF_LENGTH;
    }
    for (int k = 0; k < out_syms; k++) {
        d->result[k] = 0.0f;
    }
    d->result_len = out_syms;
    return 1;
}

static void
full_demod_debug_post_agc(const demod_state* d) {
    static int call_count = 0;
    if (!debug_cqpsk_enabled() || (++call_count % 50) != 0 || d->lp_len < 8) {
        return;
    }
    const float* iq = d->lowpassed;
    float mag_sum = 0.0f;
    float max_env = 0.0f;
    int pairs = d->lp_len >> 1;
    int limit = (pairs < 100) ? pairs : 100;
    for (int k = 0; k < limit; k++) {
        float I = iq[(k << 1)];
        float Q = iq[(k << 1) + 1];
        float mag = sqrtf(I * I + Q * Q);
        mag_sum += mag;
        if (mag > max_env) {
            max_env = mag;
        }
    }
    float avg_mag = mag_sum / (float)limit;
    DSD_FPRINTF(stderr, "[POST-AGC] avg_mag:%.3f max_env:%.3f samples:%d\n", avg_mag, max_env, d->lp_len / 2);
}

static void
full_demod_debug_op25_state(const struct demod_state* d, int pre_len) {
    static int call_count = 0;
    if (!debug_cqpsk_enabled() || (++call_count % 50) != 0) {
        return;
    }
    const dsd_costas_loop_state_t* c = &d->costas_state;
    const dsd_fll_band_edge_state_t* f = &d->fll_band_edge_state;
    const ted_state_t* ted = &d->ted_state;
    const float kTwoPi = 6.28318530717958647692f;
    float fll_be_freq_hz = f->freq * ((float)d->rate_out / kTwoPi);
    int sym_rate = (d->ted_sps > 0 && d->rate_out > 0) ? (d->rate_out / d->ted_sps) : 4800;
    float costas_freq_hz = c->freq * ((float)sym_rate / kTwoPi);
    float ted_lock = (ted->lock_count > 0) ? (ted->lock_accum / (float)ted->lock_count) : 0.0f;
    DSD_FPRINTF(stderr,
                "[OP25] in:%d out:%d omega:%.3f ted_gain:%.3f ted_lock:%.3f fll_be_freq:%.1fHz "
                "costas_freq:%.1fHz phase:%.3f\n",
                pre_len / 2, d->lp_len / 2, ted->omega, d->ted_effective_gain, ted_lock, fll_be_freq_hz, costas_freq_hz,
                c->phase);
    if (d->lp_len < 8) {
        return;
    }
    const float* iq = d->lowpassed;
    DSD_FPRINTF(stderr, "[IQ] ");
    for (int k = 0; k < 4 && k < (d->lp_len >> 1); k++) {
        float I = iq[(k << 1)];
        float Q = iq[(k << 1) + 1];
        float phase_deg = atan2f(Q, I) * 57.2957795f;
        float mag = sqrtf(I * I + Q * Q);
        DSD_FPRINTF(stderr, "(%.2f,%.2f|%.0f°,%.2f) ", I, Q, phase_deg, mag);
    }
    DSD_FPRINTF(stderr, "\n");
}

static void
full_demod_run_cqpsk_chain(struct demod_state* d) {
    if (!d->cqpsk_enable || d->channel_squelched) {
        return;
    }
    cqpsk_rms_agc(d);
    full_demod_debug_post_agc(d);
    if (d->mode_demod == &raw_demod) {
        cqpsk_diff_phasor(d);
        return;
    }
    int pre_len = d->lp_len;
    op25_fll_band_edge_cc(d);
    op25_gardner_cc(d);
    op25_diff_phasor_cc(d);
    op25_costas_loop_cc(d);
    full_demod_debug_op25_state(d, pre_len);
}

static void
full_demod_run_non_cqpsk_chain(struct demod_state* d) {
    if (d->cqpsk_enable || d->channel_squelched) {
        return;
    }
    int fsk_direct_output = (d->output_kind == DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR);
    iq_dc_block(d);
    if (fsk_direct_output) {
        return;
    }
}

static void
full_demod_apply_iq_balance(struct demod_state* d) {
    if (!d->iqbal_enable || d->cqpsk_enable || d->channel_squelched || !d->lowpassed || d->lp_len < 2) {
        return;
    }
    double s2r = 0.0, s2i = 0.0, p2 = 0.0;
    int N = d->lp_len >> 1;
    const float* iq = d->lowpassed;
    for (int n = 0; n < N; n++) {
        double I = (double)iq[(size_t)(n << 1) + 0];
        double Q = (double)iq[(size_t)(n << 1) + 1];
        s2r += I * I - Q * Q;
        s2i += 2.0 * I * Q;
        p2 += I * I + Q * Q;
    }
    if (p2 <= 1e-9) {
        p2 = 1e-9;
    }
    float er = d->iqbal_alpha_ema_r;
    float ei = d->iqbal_alpha_ema_i;
    float ar = (float)(s2r / p2);
    float ai = (float)(s2i / p2);
    float ema_alpha = d->iqbal_alpha_ema_a > 0.0f ? d->iqbal_alpha_ema_a : 0.2f;
    er += ema_alpha * (ar - er);
    ei += ema_alpha * (ai - ei);
    d->iqbal_alpha_ema_r = er;
    d->iqbal_alpha_ema_i = ei;
    float thr = d->iqbal_thr > 0.0f ? d->iqbal_thr : 0.02f;
    if ((er * er + ei * ei) < (thr * thr)) {
        return;
    }
    float* out = d->lowpassed;
    for (int n = 0; n < N; n++) {
        float I = out[(size_t)(n << 1) + 0];
        float Q = out[(size_t)(n << 1) + 1];
        float tI = er * I + ei * Q;
        float tQ = -er * Q + ei * I;
        out[(size_t)(n << 1) + 0] = I - tI;
        out[(size_t)(n << 1) + 1] = Q - tQ;
    }
}

static int
full_demod_handle_fsk_output(struct demod_state* d) {
    if (d->output_kind != DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR) {
        return 0;
    }
    int in_pairs = d->lp_len >> 1;
    if (d->channel_squelched) {
        dsd_fsk_modem_reset(&d->fsk_modem_state);
        d->result_len = in_pairs < MAXIMUM_BUF_LENGTH ? in_pairs : MAXIMUM_BUF_LENGTH;
        for (int i = 0; i < d->result_len; i++) {
            d->result[i] = 0.0f;
        }
    } else {
        d->result_len = dsd_fsk_modem_discriminator_process(&d->fsk_modem_state, d->lowpassed, d->lp_len, d->result,
                                                            MAXIMUM_BUF_LENGTH);
    }
    return 1;
}

static void
full_demod_debug_cqpsk_symbols(const struct demod_state* d) {
    static unsigned int call_count = 0;
    static int hist_p3 = 0, hist_p1 = 0, hist_m1 = 0, hist_m3 = 0, hist_other = 0;
    static int hist_samples = 0;
    static double evm_err_acc = 0.0;
    static double evm_ref_acc = 0.0;
    static int evm_count = 0;
    if (!debug_cqpsk_enabled()) {
        return;
    }
    const float* syms = d->result;
    for (int k = 0; k < d->result_len; k++) {
        float s = syms[k];
        float ideal = 0.0f;
        if (s > 2.0f) {
            hist_p3++;
            ideal = 3.0f;
        } else if (s > 0.0f) {
            hist_p1++;
            ideal = 1.0f;
        } else if (s > -2.0f) {
            hist_m1++;
            ideal = -1.0f;
        } else {
            hist_m3++;
            ideal = -3.0f;
        }
        hist_samples++;
        double err = (double)s - (double)ideal;
        evm_err_acc += err * err;
        evm_ref_acc += (double)ideal * (double)ideal;
        evm_count++;
    }
    if ((++call_count % 25) != 0 || hist_samples <= 0) {
        return;
    }
    float total = (float)hist_samples;
    DSD_FPRINTF(stderr, "[SYM] +3:%.1f%% +1:%.1f%% -1:%.1f%% -3:%.1f%% (n=%d)\n", 100.0f * hist_p3 / total,
                100.0f * hist_p1 / total, 100.0f * hist_m1 / total, 100.0f * hist_m3 / total, hist_samples);
    if (evm_count > 0 && evm_ref_acc > 1e-9) {
        double mse = evm_err_acc / (double)evm_count;
        double ref_pwr = evm_ref_acc / (double)evm_count;
        double evm_rms = sqrt(mse);
        double ref_rms = sqrt(ref_pwr);
        double evm_pct = (ref_rms > 1e-9) ? (evm_rms / ref_rms) * 100.0 : 0.0;
        double bias = dsd_rtl_stream_metrics_hook_snr_bias_evm();
        double snr_db = (mse > 1e-12) ? 10.0 * log10(ref_pwr / mse) - bias : 99.0;
        DSD_FPRINTF(stderr, "[CQPSK] EVM:%.2f%% SNR:%.1f dB ref_rms:%.2f n:%d\n", evm_pct, snr_db, ref_rms, evm_count);
    }
    hist_p3 = hist_p1 = hist_m1 = hist_m3 = hist_other = 0;
    hist_samples = 0;
    evm_err_acc = 0.0;
    evm_ref_acc = 0.0;
    evm_count = 0;
}

static void
full_demod_run_output_demod(struct demod_state* d) {
    if (d->cqpsk_enable) {
        qpsk_differential_demod(d);
        full_demod_debug_cqpsk_symbols(d);
    } else {
        d->mode_demod(d);
    }
}

static void
full_demod_apply_post_audio_decimation(struct demod_state* d) {
    if (d->cqpsk_enable || d->post_downsample <= 1) {
        return;
    }
    int decim = d->post_downsample;
    audio_polydecim_ensure(d, decim);
    if (d->post_polydecim_enabled) {
        int out_n = audio_polydecim_process(d, d->result, d->result_len, d->timing_buf);
        if (out_n > 0) {
            DSD_MEMCPY(d->result, d->timing_buf, (size_t)out_n * sizeof(float));
            d->result_len = out_n;
        }
        return;
    }
    int Fs = (d->rate_out > 0) ? d->rate_out : 48000;
    double fc = 0.2 * ((double)Fs / (double)decim);
    if (fc < 50.0) {
        fc = 50.0;
    }
    double a = 1.0 - exp(-2.0 * 3.14159265358979323846 * fc / (double)Fs);
    if (a < 0.0) {
        a = 0.0;
    } else if (a > 1.0) {
        a = 1.0;
    }
    float alpha_f = clamp_float((float)a, 0.0f, 1.0f);
    float y = (d->result_len > 0) ? d->result[0] : 0.0f;
    for (int k = 0; k < d->result_len; k++) {
        float x = d->result[k];
        y += (x - y) * alpha_f;
        d->result[k] = y;
    }
    d->result_len = low_pass_simple(d->result, d->result_len, decim);
}

static void
full_demod_apply_audio_post_filters(struct demod_state* d) {
    if (!d->cqpsk_enable) {
        if (d->deemph) {
            deemph_filter(d);
        }
        audio_lpf_filter(d);
        if (d->dc_block) {
            dc_block_filter(d);
        }
        if (d->rate_out2 > 0) {
            low_pass_real(d);
        }
    }
}

static void
full_demod_apply_squelch_envelope(struct demod_state* d) {
    if (d->cqpsk_enable) {
        return;
    }
    float env = d->squelch_env;
    float target = d->squelch_gate_open ? 1.0f : 0.0f;
    float alpha = d->squelch_gate_open ? (d->squelch_env_attack > 0.0f ? d->squelch_env_attack : 0.125f)
                                       : (d->squelch_env_release > 0.0f ? d->squelch_env_release : 0.03125f);
    env = clamp_float(env + alpha * (target - env), 0.0f, 1.0f);
    d->squelch_env = env;
    for (int k = 0; k < d->result_len; k++) {
        d->result[k] = d->result[k] * env;
    }
}

/**
 * @brief Full demodulation pipeline for one block.
 */
void
full_demod(struct demod_state* d) {
    full_demod_apply_halfband_decimation(d);
    full_demod_update_channel_state(d);
    if (full_demod_emit_zero_cqpsk_symbols(d)) {
        return;
    }
    full_demod_run_cqpsk_chain(d);
    full_demod_run_non_cqpsk_chain(d);
    full_demod_apply_iq_balance(d);
    if (full_demod_handle_fsk_output(d)) {
        return;
    }
    full_demod_run_output_demod(d);
    if (d->mode_demod == &raw_demod) {
        return;
    }
    full_demod_apply_post_audio_decimation(d);
    full_demod_apply_audio_post_filters(d);
    full_demod_apply_squelch_envelope(d);
}
