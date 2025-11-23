// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief DSP demodulation pipeline (FM/complex baseband) implementation.
 *
 * Provides decimation, optional FLL/TED, discrimination, deemphasis, DC block,
 * and audio filtering. Public APIs are declared in `dsp/demod_pipeline.h`.
 */

#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/fll.h>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/math_utils.h>
#include <dsd-neo/dsp/ted.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/mem.h>

/* Provide weak references so unit tests that do not link RTL stream still link. */
extern "C" {
#ifdef __GNUC__
__attribute__((weak)) int rtl_stream_dsp_get(int*, int*, int*);
#endif
}
#include <algorithm>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

/* demod_state now provided by include/dsp/demod_state.h */

/* Macros and constants from the original file */
#ifndef MAXIMUM_OVERSAMPLE
#define MAXIMUM_OVERSAMPLE 16
#endif
#ifndef DEFAULT_BUF_LENGTH
#define DEFAULT_BUF_LENGTH 16384
#endif
#ifndef MAXIMUM_BUF_LENGTH
#define MAXIMUM_BUF_LENGTH (MAXIMUM_OVERSAMPLE * DEFAULT_BUF_LENGTH)
#endif

#ifndef DSD_NEO_ALIGN
#define DSD_NEO_ALIGN 64
#endif

#ifndef DSD_NEO_RESTRICT
#define DSD_NEO_RESTRICT __restrict__
#endif

#ifndef DSD_NEO_IVDEP
#define DSD_NEO_IVDEP
#endif

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

/* HB_TAPS and hb_q15_taps provided by dsp/halfband.h */

/* Fixed channel low-pass for high-rate (24 kHz) mode.
 *
 * Profile 0 (wide/analog): ~8 kHz cutoff @ 24 kHz Fs.
 *   Designed as Blackman-windowed sinc, 63 taps; passband ripple ~0 dB through 6 kHz,
 *   ~-6 dB at 8 kHz, stopband < -65 dB by 9 kHz.
 *
 * Profile 1 (digital-narrow): ~5 kHz cutoff @ 24 kHz Fs.
 *   Designed as Blackman-windowed sinc, 63 taps; passband ~0 dB through ~3.6 kHz,
 *   ~-3 dB at 4.8 kHz, stopband < -60 dB by 6 kHz; tailored for 4.8 ksps 4FSK/CQPSK. */
static const int kChannelLpfTaps = 63;
static const int kChannelLpfHistLen = kChannelLpfTaps - 1;
static const int16_t channel_lpf_wide_q15[kChannelLpfTaps] = {
    0,    0, -1,   3,    0, -9,    14,   0, -28,   39,   0,     -68,  88,    0, -142, 178,   0, -271, 330,  0, -486,
    586,  0, -859, 1047, 0, -1625, 2111, 0, -4441, 8995, 21845, 8995, -4441, 0, 2111, -1625, 0, 1047, -859, 0, 586,
    -486, 0, 330,  -271, 0, 178,   -142, 0, 88,    -68,  0,     39,   -28,   0, 14,   -9,    0, 3,    -1,   0, 0,
};

/* Digital-narrow profile taps (fc≈5 kHz @ 24 kHz, 63 taps, Q15). Designed as
   Blackman-windowed sinc and normalized to unity DC gain. */
static const int16_t channel_lpf_digital_q15[kChannelLpfTaps] = {
    0,     0,    0,     -3,    -4,  5,    15,   0,    -31,  -22,  42,  68,    -26,   -130, -43,   178,
    180,   -156, -368,  0,     542, 339,  -579, -859, 313,  1492, 486, -2111, -2367, 2564, 10033, 13654,
    10033, 2564, -2367, -2111, 486, 1492, 313,  -859, -579, 339,  542, 0,     -368,  -156, 180,   178,
    -43,   -130, -26,   68,    42,  -22,  -31,  0,    15,   5,    -4,  -3,    0,     0,    0,
};

/* ---------------- Post-demod audio polyphase decimator (M > 2) -------------- */
/*
 * Lightweight 1/M polyphase decimator for audio. Designs a windowed-sinc
 * prototype (Q15) for the given M and streams with a circular history.
 * Keeps state in demod_state to persist across blocks.
 */
static void
audio_polydecim_ensure(struct demod_state* d, int M) {
    if (!d || M <= 2) {
        d->post_polydecim_enabled = 0;
        return;
    }
    const int K = 16; /* taps */
    int redesign = 0;
    if (!d->post_polydecim_taps || !d->post_polydecim_hist || d->post_polydecim_M != M || d->post_polydecim_K != K) {
        redesign = 1;
    }
    if (!redesign) {
        d->post_polydecim_enabled = 1;
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
    {
        void* mem_ptr = dsd_neo_aligned_malloc((size_t)K * sizeof(int16_t));
        d->post_polydecim_taps = (int16_t*)mem_ptr;
    }
    {
        void* mem_ptr = dsd_neo_aligned_malloc((size_t)K * sizeof(int16_t));
        d->post_polydecim_hist = (int16_t*)mem_ptr;
    }
    if (!d->post_polydecim_taps || !d->post_polydecim_hist) {
        if (d->post_polydecim_taps) {
            free(d->post_polydecim_taps);
            d->post_polydecim_taps = NULL;
        }
        if (d->post_polydecim_hist) {
            free(d->post_polydecim_hist);
            d->post_polydecim_hist = NULL;
        }
        d->post_polydecim_enabled = 0;
        return;
    }
    memset(d->post_polydecim_hist, 0, (size_t)K * sizeof(int16_t));
    d->post_polydecim_hist_head = 0;
    d->post_polydecim_phase = 0;

    /* Design windowed-sinc low-pass: fc ≈ 0.45 / M (normalized), Hamming window */
    double fc = 0.45 / (double)M;
    int N = K;
    int mid = (N - 1) / 2;
    double gain = 0.0;
    for (int n = 0; n < N; n++) {
        int m = n - mid;
        double w = 0.54 - 0.46 * cos(2.0 * 3.14159265358979323846 * (double)n / (double)(N - 1));
        double h = 2.0 * fc * dsd_neo_sinc(2.0 * fc * (double)m);
        double t = h * w;
        gain += t;
    }
    if (gain == 0.0) {
        gain = 1.0;
    }
    for (int n = 0; n < N; n++) {
        int m = n - mid;
        double w = 0.54 - 0.46 * cos(2.0 * 3.14159265358979323846 * (double)n / (double)(N - 1));
        double h = 2.0 * fc * dsd_neo_sinc(2.0 * fc * (double)m);
        double t = (h * w) / gain;
        int v = (int)lrint(t * 32768.0);
        if (v > 32767) {
            v = 32767;
        }
        if (v < -32768) {
            v = -32768;
        }
        d->post_polydecim_taps[n] = (int16_t)v;
    }
    d->post_polydecim_M = M;
    d->post_polydecim_K = K;
    d->post_polydecim_enabled = 1;
}

static int
audio_polydecim_process(struct demod_state* d, const int16_t* in, int in_len, int16_t* out) {
    if (!d || !d->post_polydecim_enabled || !d->post_polydecim_taps || !d->post_polydecim_hist || in_len <= 0) {
        if (in && out && in_len > 0) {
            memcpy(out, in, (size_t)in_len * sizeof(int16_t));
        }
        return in_len;
    }
    const int K = d->post_polydecim_K;
    const int M = d->post_polydecim_M;
    int head = d->post_polydecim_hist_head;
    int phase = d->post_polydecim_phase;
    const int16_t* taps = d->post_polydecim_taps;
    int out_len = 0;
    for (int n = 0; n < in_len; n++) {
        /* push */
        d->post_polydecim_hist[head] = in[n];
        head++;
        if (head == K) {
            head = 0;
        }
        /* phase accum */
        phase++;
        if (phase >= M) {
            phase -= M;
            /* dot product over K most recent samples */
            int idx = head - 1;
            if (idx < 0) {
                idx += K;
            }
            int64_t acc = 0;
            for (int k = 0; k < K; k++) {
                acc += (int32_t)d->post_polydecim_hist[idx] * (int32_t)taps[k];
                idx--;
                if (idx < 0) {
                    idx += K;
                }
            }
            acc += (1 << 14);
            int32_t y = (int32_t)(acc >> 15);
            out[out_len++] = sat16(y);
        }
    }
    d->post_polydecim_hist_head = head;
    d->post_polydecim_phase = phase;
    return out_len;
}

/* ---------------- Fixed channel LPF (complex, no decimation) ----------------- */
static void
channel_lpf_apply(struct demod_state* d) {
    if (!d || !d->channel_lpf_enable || d->lp_len < 2) {
        return;
    }
    const int taps_len = kChannelLpfTaps;
    const int hist_len = kChannelLpfHistLen;
    const int center = (taps_len - 1) >> 1;
    const int N = d->lp_len >> 1; /* complex samples */
    if (hist_len > d->channel_lpf_hist_len) {
        d->channel_lpf_hist_len = hist_len;
    }
    const int16_t* taps = (d->channel_lpf_profile == 1) ? channel_lpf_digital_q15 : channel_lpf_wide_q15;
    const int16_t* in = assume_aligned_ptr(d->lowpassed, DSD_NEO_ALIGN);
    int16_t* out = (d->lowpassed == d->hb_workbuf) ? d->timing_buf : d->hb_workbuf;
    int16_t* hi = assume_aligned_ptr(d->channel_lpf_hist_i, DSD_NEO_ALIGN);
    int16_t* hq = assume_aligned_ptr(d->channel_lpf_hist_q, DSD_NEO_ALIGN);
    int16_t lastI = (N > 0) ? in[(size_t)((N - 1) << 1)] : 0;
    int16_t lastQ = (N > 0) ? in[(size_t)(((N - 1) << 1) + 1)] : 0;

    for (int n = 0; n < N; n++) {
        int center_idx = hist_len + n; /* index into combined hist+current stream */
        auto get_iq = [&](int src_idx, int16_t& xi, int16_t& xq) {
            if (src_idx < hist_len) {
                xi = hi[src_idx];
                xq = hq[src_idx];
            } else {
                int rel = src_idx - hist_len;
                if (rel < N) {
                    xi = in[(size_t)(rel << 1)];
                    xq = in[(size_t)(rel << 1) + 1];
                } else {
                    xi = lastI;
                    xq = lastQ;
                }
            }
        };
        int64_t accI = 0;
        int64_t accQ = 0;
        /* center tap */
        {
            int16_t ci, cq;
            get_iq(center_idx, ci, cq);
            int16_t cc = taps[center];
            accI += (int32_t)cc * (int32_t)ci;
            accQ += (int32_t)cc * (int32_t)cq;
        }
        /* symmetric pairs */
        for (int k = 0; k < center; k++) {
            int16_t ce = taps[k];
            if (ce == 0) {
                continue;
            }
            int dists = center - k;
            int16_t xmI, xmQ, xpI, xpQ;
            get_iq(center_idx - dists, xmI, xmQ);
            get_iq(center_idx + dists, xpI, xpQ);
            accI += (int32_t)ce * (int32_t)(xmI + xpI);
            accQ += (int32_t)ce * (int32_t)(xmQ + xpQ);
        }
        accI += (1 << 14);
        accQ += (1 << 14);
        out[(size_t)(n << 1)] = sat16((int32_t)(accI >> 15));
        out[(size_t)(n << 1) + 1] = sat16((int32_t)(accQ >> 15));
    }

    /* Update history with the last (hist_len) complex samples of this block */
    if (hist_len > 0) {
        if (N >= hist_len) {
            for (int k = 0; k < hist_len; k++) {
                int rel = N - hist_len + k;
                hi[k] = in[(size_t)(rel << 1)];
                hq[k] = in[(size_t)(rel << 1) + 1];
            }
        } else {
            int need = hist_len - N;
            if (need > 0) {
                memmove(hi, hi + (hist_len - need), (size_t)need * sizeof(int16_t));
                memmove(hq, hq + (hist_len - need), (size_t)need * sizeof(int16_t));
            }
            for (int k = 0; k < N; k++) {
                hi[need + k] = in[(size_t)(k << 1)];
                hq[need + k] = in[(size_t)(k << 1) + 1];
            }
        }
    }
    d->lowpassed = out;
    d->lp_len = N << 1;
}

/* CIC compensation filter tables */
#define CIC_TABLE_MAX 10
static const int cic_9_tables[][10] = {
    /* ds_p=0: no compensation needed */
    {0},
    /* ds_p=1: single stage */
    {0, 8192, 0, 0, 0, 0, 0, 0, 0, 0},
    /* ds_p=2: two stages */
    {0, 4096, 4096, 0, 0, 0, 0, 0, 0, 0},
    /* ds_p=3: three stages */
    {0, 2730, 2730, 2730, 0, 0, 0, 0, 0, 0},
    /* ds_p=4: four stages */
    {0, 2048, 2048, 2048, 2048, 0, 0, 0, 0, 0},
    /* ds_p=5: five stages */
    {0, 1638, 1638, 1638, 1638, 1638, 0, 0, 0, 0},
    /* ds_p=6: six stages */
    {0, 1365, 1365, 1365, 1365, 1365, 1365, 0, 0, 0},
    /* ds_p=7: seven stages */
    {0, 1170, 1170, 1170, 1170, 1170, 1170, 1170, 0, 0},
    /* ds_p=8: eight stages */
    {0, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 0},
    /* ds_p=9: nine stages */
    {0, 910, 910, 910, 910, 910, 910, 910, 910, 910},
    /* ds_p=10: ten stages */
    {0, 819, 819, 819, 819, 819, 819, 819, 819, 819}};

/* Global flags provided by rtl front-end */
extern int use_halfband_decimator;

/**
 * Decimate one real channel by 2 using a half-band FIR with persistent left history.
 *
 * @param in   Pointer to real input samples.
 * @param in_len Number of real input samples.
 * @param out  Pointer to output buffer (size >= in_len/2).
 * @param hist Persistent history of length HB_TAPS-1 (left wing).
 * @return Number of output samples written (in_len/2).
 */
/* hb_decim2_real provided by dsp/halfband.h */

/**
 * @brief Half-band decimator for complex interleaved I/Q data.
 *
 * Decimates by 2:1 using symmetric FIR filter.
 *
 * @param in      Input complex samples (interleaved I/Q).
 * @param in_len  Number of complex samples (total elements = 2 * in_len).
 * @param out     Output buffer for decimated complex samples.
 * @param hist_i  Persistent I-channel history of length HB_TAPS-1.
 * @param hist_q  Persistent Q-channel history of length HB_TAPS-1.
 * @return Number of output complex samples.
 */
/* Generalized complex half-band decimator by 2 that accepts arbitrary Q15 tap sets.
   Falls back to an optimized unrolled path for 15-tap filters. */
static int
hb_decim2_complex_interleaved_ex(const int16_t* DSD_NEO_RESTRICT in, int in_len, int16_t* DSD_NEO_RESTRICT out,
                                 int16_t* DSD_NEO_RESTRICT hist_i, int16_t* DSD_NEO_RESTRICT hist_q,
                                 const int16_t* DSD_NEO_RESTRICT taps_q15, int taps_len) {
    const int hist_len = HB_TAPS - 1;
    (void)hist_len; /* not used in generalized path */
    if (taps_len < 3 || (taps_len & 1) == 0) {
        return 0; /* invalid */
    }
    int ch_len = in_len >> 1;     /* per-channel samples */
    int out_ch_len = ch_len >> 1; /* decimated per-channel */
    if (out_ch_len <= 0) {
        return 0;
    }
    const int16_t* DSD_NEO_RESTRICT in_al = assume_aligned_ptr(in, DSD_NEO_ALIGN);
    int16_t* DSD_NEO_RESTRICT out_al = assume_aligned_ptr(out, DSD_NEO_ALIGN);
    int16_t* DSD_NEO_RESTRICT hi = assume_aligned_ptr(hist_i, DSD_NEO_ALIGN);
    int16_t* DSD_NEO_RESTRICT hq = assume_aligned_ptr(hist_q, DSD_NEO_ALIGN);
    int16_t lastI = (ch_len > 0) ? in_al[in_len - 2] : 0;
    int16_t lastQ = (ch_len > 0) ? in_al[in_len - 1] : 0;
    const int center = (taps_len - 1) >> 1;
    for (int n = 0; n < out_ch_len; n++) {
        int center_idx = (taps_len - 1) + (n << 1); /* per-channel index with left history */
        auto get_iq = [&](int src_idx, int16_t& xi, int16_t& xq) {
            if (src_idx < (taps_len - 1)) {
                xi = hi[src_idx];
                xq = hq[src_idx];
            } else {
                int rel = src_idx - (taps_len - 1);
                if (rel < ch_len) {
                    xi = in_al[(size_t)(rel << 1)];
                    xq = in_al[(size_t)(rel << 1) + 1];
                } else {
                    xi = lastI;
                    xq = lastQ;
                }
            }
        };
        int64_t accI = 0;
        int64_t accQ = 0;
        /* center tap */
        {
            int16_t ci, cq;
            get_iq(center_idx, ci, cq);
            int16_t cc = taps_q15[center];
            accI += (int32_t)cc * (int32_t)ci;
            accQ += (int32_t)cc * (int32_t)cq;
        }
        /* symmetric pairs: only even indices in the full taps array are non-zero */
        for (int e = 0; e < center; e += 2) {
            int d = center - e; /* distance from center */
            int16_t ce = taps_q15[e];
            if (ce == 0) {
                continue;
            }
            int16_t xmI, xmQ, xpI, xpQ;
            get_iq(center_idx - d, xmI, xmQ);
            get_iq(center_idx + d, xpI, xpQ);
            accI += (int32_t)ce * (int32_t)(xmI + xpI);
            accQ += (int32_t)ce * (int32_t)(xmQ + xpQ);
        }
        accI += (1 << 14);
        accQ += (1 << 14);
        out_al[(size_t)(n << 1)] = sat16((int32_t)(accI >> 15));
        out_al[(size_t)(n << 1) + 1] = sat16((int32_t)(accQ >> 15));
    }
    /* Update histories with last (taps_len-1) per-channel input samples */
    if (ch_len >= (taps_len - 1)) {
        int start = ch_len - (taps_len - 1);
        for (int k = 0; k < (taps_len - 1); k++) {
            int rel = start + k;
            hi[k] = in_al[(size_t)(rel << 1)];
            hq[k] = in_al[(size_t)(rel << 1) + 1];
        }
    } else {
        int existing = ch_len;
        for (int k = 0; k < (taps_len - 1); k++) {
            if (k < existing) {
                int rel = k;
                hi[k] = in_al[(size_t)(rel << 1)];
                hq[k] = in_al[(size_t)(rel << 1) + 1];
            } else {
                hi[k] = 0;
                hq[k] = 0;
            }
        }
    }
    return out_ch_len << 1;
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
low_pass_simple(int16_t* signal2, int len, int step) {
    int i, i2, sum;
    if (step <= 0) {
        return len;
    }
    for (i = 0; i + (step - 1) < len; i += step) {
        sum = 0;
        for (i2 = 0; i2 < step; i2++) {
            sum += (int)signal2[i + i2];
        }
        // Normalize by step with rounding. Writes output at i/step index.
        int val = (sum >= 0) ? (sum + step / 2) / step : -(((-sum) + step / 2) / step);
        signal2[i / step] = (int16_t)val;
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
    int16_t* r = assume_aligned_ptr(s->result, DSD_NEO_ALIGN);
    int fast = (int)s->rate_in;
    int slow = s->rate_out2;
    /* Precompute fixed-point reciprocal of decimation factor to avoid per-sample division */
    int decim = (slow != 0) ? (fast / slow) : 1;
    if (decim < 1) {
        decim = 1;
    }
    const int kShiftLPR = 15; /* Q15 reciprocal */
    int recip_decim_q = (1 << kShiftLPR) / decim;
    DSD_NEO_IVDEP
    while (i < s->result_len) {
        s->now_lpr += r[i];
        i++;
        s->prev_lpr_index += slow;
        if (s->prev_lpr_index < fast) {
            continue;
        }
        /* Multiply by reciprocal and shift instead of dividing by (fast/slow) */
        int64_t scaled = ((int64_t)s->now_lpr * recip_decim_q);
        r[i2] = (int16_t)(scaled >> kShiftLPR);
        s->prev_lpr_index -= fast;
        s->now_lpr = 0;
        i2 += 1;
    }
    s->result_len = i2;
}

/**
 * @brief Deferred low-pass: sums and decimates with saturation on writeback.
 *
 * @param d Demodulator state (uses lowpassed buffer and decimation state).
 */
void
low_pass(struct demod_state* d) {
    int i = 0, i2 = 0;
    int16_t* DSD_NEO_RESTRICT lp = assume_aligned_ptr(d->lowpassed, DSD_NEO_ALIGN);
    while (i < d->lp_len) {
        d->now_r += lp[i];
        d->now_j += lp[i + 1];
        i += 2;
        d->prev_index++;
        if (d->prev_index < d->downsample) {
            continue;
        }
        /* Saturate accumulated sums when writing back to int16 */
        lp[i2] = sat16(d->now_r);
        lp[i2 + 1] = sat16(d->now_j);
        d->prev_index = 0;
        d->now_r = 0;
        d->now_j = 0;
        i2 += 2;
    }
    d->lp_len = i2;
}

/**
 * @brief Fifth-order half-band-like decimator operating on a single real sequence.
 *
 * Caller applies this separately to I and Q streams. Uses 6-tap state in
 * `hist` and writes decimated output in-place.
 *
 * @param data   In/out real data buffer (single channel).
 * @param length Input length (elements), processed in-place.
 * @param hist   Persistent history buffer of length >= 6.
 */
void
fifth_order(int16_t* data, int length, int16_t* hist) {
    int i;
    int16_t a, b, c, d, e, f;
    a = hist[1];
    b = hist[2];
    c = hist[3];
    d = hist[4];
    e = hist[5];
    f = data[0];
    /* a downsample should improve resolution, so don't fully shift */
    data[0] = (a + (b + e) * 5 + (c + d) * 10 + f) >> 4;
    for (i = 4; i < length; i += 4) {
        a = c;
        b = d;
        c = e;
        d = f;
        e = data[i - 2];
        f = data[i];
        data[i / 2] = (a + (b + e) * 5 + (c + d) * 10 + f) >> 4;
    }
    /* archive */
    hist[0] = a;
    hist[1] = b;
    hist[2] = c;
    hist[3] = d;
    hist[4] = e;
    hist[5] = f;
}

/**
 * @brief FIR filter with symmetric 9-tap coefficients (phase-saving implementation).
 *
 * @param data   In/out data buffer (interleaved step of 2 assumed).
 * @param length Number of input samples.
 * @param fir    Coefficient array (expects layout for length 9).
 * @param hist   History buffer used across calls.
 */
void
generic_fir(int16_t* data, int length, int* fir, int16_t* hist) {
    int d, temp, sum;
    for (d = 0; d < length; d += 2) {
        temp = data[d];
        sum = 0;
        sum += (hist[0] + hist[8]) * fir[1];
        sum += (hist[1] + hist[7]) * fir[2];
        sum += (hist[2] + hist[6]) * fir[3];
        sum += (hist[3] + hist[5]) * fir[4];
        sum += hist[4] * fir[5];
        sum += (1 << 14); /* Round */
        /* Saturate on writeback to guard future coefficient changes */
        data[d] = sat16((int32_t)(sum >> 15));
        hist[0] = hist[1];
        hist[1] = hist[2];
        hist[2] = hist[3];
        hist[3] = hist[4];
        hist[4] = hist[5];
        hist[5] = hist[6];
        hist[6] = hist[7];
        hist[7] = hist[8];
        hist[8] = temp;
    }
}

/**
 * @brief Perform FM discriminator on interleaved low-passed I/Q to produce audio PCM.
 *
 * Uses the active discriminator configured in fm->discriminator.
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

    const int16_t* iq = assume_aligned_ptr(fm->lowpassed, DSD_NEO_ALIGN);
    int16_t* out = assume_aligned_ptr(fm->result, DSD_NEO_ALIGN);

    int prev_r = fm->pre_r;
    int prev_j = fm->pre_j;
    /* Seed history on first use to keep the first phase delta well-defined. */
    if (prev_r == 0 && prev_j == 0) {
        prev_r = iq[0];
        prev_j = iq[1];
    }

    for (int n = 0; n < pairs; n++) {
        int cr = iq[(size_t)(n << 1) + 0];
        int cj = iq[(size_t)(n << 1) + 1];
        /* z_n * conj(z_{n-1}) => phase delta; amplitude cancels inside atan2 */
        int64_t re = (int64_t)cr * (int64_t)prev_r + (int64_t)cj * (int64_t)prev_j;
        int64_t im = (int64_t)cj * (int64_t)prev_r - (int64_t)cr * (int64_t)prev_j;
        int angle_q14 = dsd_neo_fast_atan2(im, re);
        if (fm->fll_enabled) {
            angle_q14 += (fm->fll_freq_q15 >> 1);
        }
        out[n] = sat16(angle_q14);
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
        fm->result[i] = (int16_t)fm->lowpassed[i];
    }
    fm->result_len = fm->lp_len;
}

/**
 * @brief QPSK helper demodulator: copy I channel from interleaved complex baseband.
 *
 * Assumes fm->lowpassed holds interleaved I/Q samples that have already passed
 * through CQPSK processing (matched filter, optional acquisition FLL/TED, Costas).
 * Produces a single real stream (I only) to feed the legacy symbol sampler path.
 */
/**
 * @brief Differential QPSK demodulator for CQPSK/LSM paths.
 *
 * Computes the phase difference between consecutive complex samples using:
 *   delta_n = arg(z_n * conj(z_{n-1}))
 *
 * Phase is returned in Q14 units where pi == 1<<14. The differential history
 * is maintained across blocks via cqpsk_diff_prev_r/j in demod_state.
 *
 * @param fm Demodulator state (reads interleaved I/Q in lowpassed, writes phase deltas to result).
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
    const int16_t* iq = assume_aligned_ptr(fm->lowpassed, DSD_NEO_ALIGN);
    int16_t* out = assume_aligned_ptr(fm->result, DSD_NEO_ALIGN);

    int prev_r = fm->cqpsk_diff_prev_r;
    int prev_j = fm->cqpsk_diff_prev_j;

    /* Initialize history on first use with the first sample to keep the
       first phase delta well-defined (zero rotation). */
    if (prev_r == 0 && prev_j == 0) {
        prev_r = iq[0];
        prev_j = iq[1];
    }

    for (int n = 0; n < pairs; n++) {
        int cr = iq[(size_t)(n << 1) + 0];
        int cj = iq[(size_t)(n << 1) + 1];

        int64_t mr = (int64_t)cr * (int64_t)prev_r + (int64_t)cj * (int64_t)prev_j; /* Re{z_n * conj(z_{n-1})} */
        int64_t mj = (int64_t)cj * (int64_t)prev_r - (int64_t)cr * (int64_t)prev_j; /* Im{z_n * conj(z_{n-1})} */

        int phase_q14 = dsd_neo_fast_atan2(mj, mr);
        out[n] = (int16_t)phase_q14;

        prev_r = cr;
        prev_j = cj;
    }

    fm->cqpsk_diff_prev_r = prev_r;
    fm->cqpsk_diff_prev_j = prev_j;
    fm->result_len = pairs;
}

/**
 * @brief Apply post-demod deemphasis IIR filter with Q15 coefficient.
 *
 * @param fm Demodulator state (reads/writes result, updates deemph_avg).
 */
void
deemph_filter(struct demod_state* fm) {
    int avg = fm->deemph_avg; /* per-instance state */
    int i, d;
    int16_t* res = assume_aligned_ptr(fm->result, DSD_NEO_ALIGN);
    /* Q15 alpha = (1 - a), where a = exp(-1/(Fs*tau)) */
    const int kShiftDeemph = 15; /* Q15 */
    int alpha_q15 = fm->deemph_a;
    if (alpha_q15 < 0) {
        alpha_q15 = 0;
    }
    if (alpha_q15 > (1 << kShiftDeemph)) {
        alpha_q15 = (1 << kShiftDeemph);
    }
    /* Single-pole IIR: avg += (x - avg) * alpha */
    DSD_NEO_IVDEP
    for (i = 0; i < fm->result_len; i++) {
        d = res[i] - avg;
        int64_t delta = (int64_t)d * (int64_t)alpha_q15;
        /* symmetric rounding */
        if (d > 0) {
            delta += (1LL << (kShiftDeemph - 1));
        } else if (d < 0) {
            delta -= (1LL << (kShiftDeemph - 1));
        }
        avg += (int)(delta >> kShiftDeemph);
        res[i] = (int16_t)avg;
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
    int i;
    /* Leaky integrator high-pass: dc += (x - dc) >> k; y = x - dc */
    int dc = fm->dc_avg;
    const int k = 11; /* cutoff ~ Fs / 2^k (k in 10..12) */
    int16_t* res = assume_aligned_ptr(fm->result, DSD_NEO_ALIGN);
    DSD_NEO_IVDEP
    for (i = 0; i < fm->result_len; i++) {
        int x = (int)res[i];
        dc += (x - dc) >> k;
        int y = x - dc;
        res[i] = sat16(y);
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
    int i;
    int16_t* res = assume_aligned_ptr(fm->result, DSD_NEO_ALIGN);
    int y = fm->audio_lpf_state;               /* Q0 */
    const int alpha_q15 = fm->audio_lpf_alpha; /* Q15 */
    const int kShift = 15;
    DSD_NEO_IVDEP
    for (i = 0; i < fm->result_len; i++) {
        int x = (int)res[i];
        int d = x - y;
        int64_t delta = (int64_t)d * (int64_t)alpha_q15;
        /* symmetric rounding */
        if (d >= 0) {
            delta += (1LL << (kShift - 1));
        } else {
            delta -= (1LL << (kShift - 1));
        }
        y += (int)(delta >> kShift);
        res[i] = (int16_t)y;
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
long int
mean_power(int16_t* samples, int len, int step) {
    int64_t p = 0;
    int64_t t = 0;
    for (int i = 0; i < len; i += step) {
        int64_t s = (int64_t)samples[i];
        t += s;
        p += s * s;
    }
    /* DC-corrected energy ≈ p - (t^2)/len with rounded division */
    int64_t dc_corr = 0;
    if (len > 0) {
        int64_t tt = t * t;
        dc_corr = (tt + (len / 2)) / len;
    }
    int64_t energy = p - dc_corr;
    if (energy < 0) {
        energy = 0;
    }
    return (long int)(energy / (len > 0 ? len : 1));
}

/**
 * @brief Estimate frequency error using the configured discriminator and update the
 * FLL loop control variables in Q15.
 *
 * Mirrors the modular FLL path used by the RTL front-end.
 *
 * @param d Demodulator state (syncs to/from `fll_state`, updates loop vars).
 */
static void
fll_update_error(struct demod_state* d) {
    if (!d->fll_enabled) {
        return;
    }
    /* Sync from demod_state to module state */
    d->fll_state.freq_q15 = d->fll_freq_q15;
    d->fll_state.phase_q15 = d->fll_phase_q15;
    d->fll_state.prev_r = d->fll_prev_r;
    d->fll_state.prev_j = d->fll_prev_j;

    fll_config_t cfg;
    cfg.enabled = d->fll_enabled;
    cfg.alpha_q15 = d->fll_alpha_q15;
    cfg.beta_q15 = d->fll_beta_q15;
    cfg.deadband_q14 = d->fll_deadband_q14;
    cfg.slew_max_q15 = d->fll_slew_max_q15;

    /* Use QPSK-friendly symbol-spaced update when CQPSK path is active */
    if (d->cqpsk_enable && d->ted_sps >= 2) {
        fll_update_error_qpsk(&cfg, &d->fll_state, d->lowpassed, d->lp_len, d->ted_sps);
    } else {
        fll_update_error(&cfg, &d->fll_state, d->lowpassed, d->lp_len);
    }

    /* Sync back to demod_state */
    d->fll_freq_q15 = d->fll_state.freq_q15;
    d->fll_phase_q15 = d->fll_state.phase_q15;
    d->fll_prev_r = d->fll_state.prev_r;
    d->fll_prev_j = d->fll_state.prev_j;
}

/**
 * @brief Mix low-passed I/Q by the FLL NCO and advance the loop accumulators.
 *
 * Rotates the complex baseband to reduce residual CFO and synchronizes the
 * demod state with the modular FLL implementation.
 *
 * @param d Demodulator state (reads/writes `lowpassed`, updates `fll_state`).
 */
static void
fll_mix_and_update(struct demod_state* d) {
    if (!d->fll_enabled) {
        return;
    }

    /* Sync from demod_state to module state */
    d->fll_state.freq_q15 = d->fll_freq_q15;
    d->fll_state.phase_q15 = d->fll_phase_q15;
    d->fll_state.prev_r = d->fll_prev_r;
    d->fll_state.prev_j = d->fll_prev_j;

    fll_config_t cfg;
    cfg.enabled = d->fll_enabled;
    cfg.alpha_q15 = d->fll_alpha_q15;
    cfg.beta_q15 = d->fll_beta_q15;
    cfg.deadband_q14 = d->fll_deadband_q14;
    cfg.slew_max_q15 = d->fll_slew_max_q15;

    fll_mix_and_update(&cfg, &d->fll_state, d->lowpassed, d->lp_len);

    /* Sync back to demod_state */
    d->fll_freq_q15 = d->fll_state.freq_q15;
    d->fll_phase_q15 = d->fll_state.phase_q15;
    d->fll_prev_r = d->fll_state.prev_r;
    d->fll_prev_j = d->fll_state.prev_j;
}

/*
 * FM envelope AGC/limiter (block-based)
 *
 * Normalizes complex I/Q magnitude toward a target RMS to reduce amplitude
 * bounce from low-cost front-ends (e.g., RTL-SDR). Uses per-block RMS with a
 * smoothed gain (separate attack/decay alphas) to avoid pumping. Disabled for
 * CQPSK paths to leave constellation amplitude untouched.
 */
static inline void
fm_envelope_agc(struct demod_state* d) {
    if (!d || !d->fm_agc_enable || d->cqpsk_enable || !d->lowpassed || d->lp_len < 2) {
        return;
    }
    const int pairs = d->lp_len >> 1;
    if (pairs <= 0) {
        return;
    }
    /* Compute block RMS of |z| */
    uint64_t acc = 0;
    const int16_t* iq = d->lowpassed;
    for (int n = 0; n < pairs; n++) {
        int32_t I = iq[(size_t)(n << 1) + 0];
        int32_t Q = iq[(size_t)(n << 1) + 1];
        acc += (uint64_t)((int64_t)I * (int64_t)I + (int64_t)Q * (int64_t)Q);
    }
    if (acc == 0) {
        return;
    }
    double mean_r2 = (double)acc / (double)pairs;
    double rms = sqrt(mean_r2);
    if (rms < (double)(d->fm_agc_min_rms > 0 ? d->fm_agc_min_rms : 2000)) {
        /* Hold gain on very weak input to avoid boosting noise */
        return;
    }
    int target = (d->fm_agc_target_rms > 0) ? d->fm_agc_target_rms : 10000;
    if (target < 1000) {
        target = 1000;
    }
    if (target > 20000) {
        target = 20000;
    }
    double g_raw = (double)target / rms;
    /* Clamp extreme steps */
    if (g_raw > 8.0) {
        g_raw = 8.0;
    }
    if (g_raw < 0.125) {
        g_raw = 0.125;
    }
    int g_tgt_q15 = (int)(g_raw * 32768.0 + 0.5);
    if (g_tgt_q15 < 512) {
        g_tgt_q15 = 512;
    }
    if (g_tgt_q15 > 262144) {
        g_tgt_q15 = 262144; /* allow >1 in Q15 via later clamp */
    }
    int g_q15 = d->fm_agc_gain_q15 > 0 ? d->fm_agc_gain_q15 : 32768;
    int alpha_up = d->fm_agc_alpha_up_q15 > 0 ? d->fm_agc_alpha_up_q15 : 8192;      /* ~0.25 */
    int alpha_dn = d->fm_agc_alpha_down_q15 > 0 ? d->fm_agc_alpha_down_q15 : 24576; /* ~0.75 */
    int diff = g_tgt_q15 - g_q15;
    int alpha = (diff > 0) ? alpha_up : alpha_dn;
    g_q15 += (int)(((int64_t)alpha * (int64_t)diff) >> 15);
    if (g_q15 < 1024) {
        g_q15 = 1024; /* >= 1/32 */
    }
    if (g_q15 > 262144) {
        g_q15 = 262144; /* <= 8x */
    }
    /* Apply smoothed gain and collect simple post-AGC stats for auto-tune */
    int16_t* out = d->lowpassed;
    int clip_cnt = 0;
    int max_abs = 0;
    for (int n = 0; n < pairs; n++) {
        int32_t I = out[(size_t)(n << 1) + 0];
        int32_t Q = out[(size_t)(n << 1) + 1];
        int32_t yI = (int32_t)(((int64_t)I * (int64_t)g_q15) >> 15);
        int32_t yQ = (int32_t)(((int64_t)Q * (int64_t)g_q15) >> 15);
        if (yI > 32767) {
            yI = 32767;
            clip_cnt++;
        }
        if (yI < -32768) {
            yI = -32768;
            clip_cnt++;
        }
        if (yQ > 32767) {
            yQ = 32767;
            clip_cnt++;
        }
        if (yQ < -32768) {
            yQ = -32768;
            clip_cnt++;
        }
        int aI = yI >= 0 ? yI : -yI;
        int aQ = yQ >= 0 ? yQ : -yQ;
        if (aI > max_abs) {
            max_abs = aI;
        }
        if (aQ > max_abs) {
            max_abs = aQ;
        }
        out[(size_t)(n << 1) + 0] = (int16_t)yI;
        out[(size_t)(n << 1) + 1] = (int16_t)yQ;
    }
    d->fm_agc_gain_q15 = g_q15;
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
    int16_t* iq = d->lowpassed;
    const int pairs = d->lp_len >> 1;
    int dcI = d->iq_dc_avg_r;
    int dcQ = d->iq_dc_avg_i;
    for (int n = 0; n < pairs; n++) {
        int I = (int)iq[(size_t)(n << 1) + 0];
        int Q = (int)iq[(size_t)(n << 1) + 1];
        /* Update leaky integrator toward current sample */
        dcI += (I - dcI) >> k;
        dcQ += (Q - dcQ) >> k;
        int yI = I - dcI;
        int yQ = Q - dcQ;
        iq[(size_t)(n << 1) + 0] = sat16(yI);
        iq[(size_t)(n << 1) + 1] = sat16(yQ);
    }
    d->iq_dc_avg_r = dcI;
    d->iq_dc_avg_i = dcQ;
}

/* Optional constant-envelope limiter: per-sample normalization around target magnitude */
static inline void
fm_constant_envelope_limiter(struct demod_state* d) {
    if (!d || !d->fm_limiter_enable || d->cqpsk_enable || !d->lowpassed || d->lp_len < 2) {
        return;
    }
    int target = (d->fm_agc_target_rms > 0) ? d->fm_agc_target_rms : 10000;
    if (target < 1000) {
        target = 1000;
    }
    if (target > 20000) {
        target = 20000;
    }
    const int64_t t2 = (int64_t)target * (int64_t)target;
    const int64_t lo2 = (t2 >> 2); /* 0.5^2 */
    const int64_t hi2 = (t2 << 2); /* 2.0^2 */
    int16_t* iq = d->lowpassed;
    const int pairs = d->lp_len >> 1;
    for (int n = 0; n < pairs; n++) {
        int32_t I = iq[(size_t)(n << 1) + 0];
        int32_t Q = iq[(size_t)(n << 1) + 1];
        int64_t m2 = (int64_t)I * I + (int64_t)Q * Q;
        if (m2 <= 0) {
            continue;
        }
        if (m2 >= lo2 && m2 <= hi2) {
            /* within tolerance, leave unchanged */
            continue;
        }
        double mag = sqrt((double)m2);
        if (mag < 1e-6) {
            continue;
        }
        double g = (double)target / mag;
        if (g > 8.0) {
            g = 8.0;
        }
        if (g < 0.125) {
            g = 0.125;
        }
        int32_t yI = (int32_t)lrint((double)I * g);
        int32_t yQ = (int32_t)lrint((double)Q * g);
        iq[(size_t)(n << 1) + 0] = sat16(yI);
        iq[(size_t)(n << 1) + 1] = sat16(yQ);
    }
}

/* Small symmetric 5-tap matched-like FIR on interleaved I/Q (in-place via workbuf). */
static void
mf5_complex_interleaved(struct demod_state* d) {
    if (!d || !d->lowpassed || d->lp_len < 10) {
        return;
    }
    const int N = d->lp_len >> 1; /* complex samples */
    int16_t* in = d->lowpassed;
    /* Choose an output buffer that does not alias input. If lowpassed already
       points to hb_workbuf (odd HB decim passes), use timing_buf as scratch. */
    int16_t* out = (in == d->hb_workbuf) ? d->timing_buf : d->hb_workbuf;
    /* taps ~ [1, 4, 6, 4, 1] / 16 in Q15 */
    const int t0 = 2048;  /* 1/16 */
    const int t1 = 8192;  /* 4/16 */
    const int t2 = 12288; /* 6/16 */
    for (int n = 0; n < N; n++) {
        int idxm2 = (n - 2);
        int idxm1 = (n - 1);
        int idxp1 = (n + 1 < N) ? (n + 1) : (N - 1);
        int idxp2 = (n + 2 < N) ? (n + 2) : (N - 1);
        if (idxm2 < 0) {
            idxm2 = 0;
        }
        if (idxm1 < 0) {
            idxm1 = 0;
        }
        int16_t im2 = in[(size_t)(idxm2 << 1)];
        int16_t jm2 = in[(size_t)(idxm2 << 1) + 1];
        int16_t im1 = in[(size_t)(idxm1 << 1)];
        int16_t jm1 = in[(size_t)(idxm1 << 1) + 1];
        int16_t i0 = in[(size_t)(n << 1)];
        int16_t j0 = in[(size_t)(n << 1) + 1];
        int16_t ip1 = in[(size_t)(idxp1 << 1)];
        int16_t jp1 = in[(size_t)(idxp1 << 1) + 1];
        int16_t ip2 = in[(size_t)(idxp2 << 1)];
        int16_t jp2 = in[(size_t)(idxp2 << 1) + 1];
        int64_t accI = (int64_t)im2 * t0 + (int64_t)im1 * t1 + (int64_t)i0 * t2 + (int64_t)ip1 * t1 + (int64_t)ip2 * t0;
        int64_t accQ = (int64_t)jm2 * t0 + (int64_t)jm1 * t1 + (int64_t)j0 * t2 + (int64_t)jp1 * t1 + (int64_t)jp2 * t0;
        out[(size_t)(n << 1)] = sat16((int32_t)((accI + (1 << 14)) >> 15));
        out[(size_t)(n << 1) + 1] = sat16((int32_t)((accQ + (1 << 14)) >> 15));
    }
    /* swap buffers */
    memcpy(d->lowpassed, out, (size_t)(N << 1) * sizeof(int16_t));
}

/* RRC matched filter on interleaved I/Q using current TED SPS and configured alpha/span. */
static void
mf_rrc_complex_interleaved(struct demod_state* d) {
    if (!d || !d->lowpassed || d->lp_len < 10) {
        return;
    }
    if (d->ted_sps <= 1) {
        return;
    }
    int sps = d->ted_sps;
    double alpha = (d->cqpsk_rrc_alpha_q15 > 0) ? ((double)d->cqpsk_rrc_alpha_q15 / 32768.0) : 0.25;
    int taps_len;
    int span_used = (d->cqpsk_rrc_span_syms > 0) ? d->cqpsk_rrc_span_syms : 0;
    if (span_used > 0) {
        taps_len = span_used * 2 * sps + 1;
    } else {
        /* Default to ntaps = 11*sps + 1 (e.g., 89 for sps=8) */
        taps_len = 11 * sps + 1;
    }
    if (taps_len < 7) {
        taps_len = 7;
    }
    if (taps_len > 257) {
        taps_len = 257;
    }
    if ((taps_len & 1) == 0) {
        taps_len += 1; /* enforce odd length for symmetry */
        if (taps_len > 257) {
            taps_len = 257;
        }
    }
    static int last_sps = 0;
    static int last_span = 0;
    static int last_alpha_q15 = 0;
    static int last_taps_len = 0;
    static int16_t taps_q15[257];
    static int taps_ready = 0;
    if (!taps_ready || sps != last_sps || span_used != last_span || d->cqpsk_rrc_alpha_q15 != last_alpha_q15
        || taps_len != last_taps_len) {
        /* Design RRC taps */
        int mid = taps_len / 2;
        double sum = 0.0;
        for (int n = 0; n < taps_len; n++) {
            double t_over_T = ((double)n - (double)mid) / (double)sps;
            double tau = t_over_T;
            double h;
            double pi = 3.14159265358979323846;
            double four_a_tau = 4.0 * alpha * tau;
            double denom = pi * tau * (1.0 - (four_a_tau * four_a_tau));
            if (fabs(tau) < 1e-8) {
                /* Limit at tau=0 */
                h = (1.0 + alpha * (4.0 / pi - 1.0));
            } else if (fabs(fabs(tau) - (1.0 / (4.0 * alpha))) < 1e-6) {
                /* Limit at tau = ±1/(4α) */
                double a = alpha;
                double term1 = (1.0 + 2.0 / pi) * sin(pi / (4.0 * a));
                double term2 = (1.0 - 2.0 / pi) * cos(pi / (4.0 * a));
                h = (a / sqrt(2.0)) * (term1 + term2);
            } else {
                double num = sin(pi * tau * (1.0 - alpha)) + four_a_tau * cos(pi * tau * (1.0 + alpha));
                h = num / denom;
            }
            sum += h;
            /* store temporarily in taps_q15 for second pass */
            taps_q15[n] = (int16_t)h; /* placeholder */
        }
        /* Normalize DC gain to 1.0 (sum taps = 1) */
        if (fabs(sum) < 1e-9) {
            sum = 1.0;
        }
        for (int n = 0; n < taps_len; n++) {
            double t_over_T = ((double)n - (double)mid) / (double)sps;
            double tau = t_over_T;
            double h;
            double pi = 3.14159265358979323846;
            double four_a_tau = 4.0 * alpha * tau;
            double denom = pi * tau * (1.0 - (four_a_tau * four_a_tau));
            if (fabs(tau) < 1e-8) {
                h = (1.0 + alpha * (4.0 / pi - 1.0));
            } else if (fabs(fabs(tau) - (1.0 / (4.0 * alpha))) < 1e-6) {
                double a = alpha;
                double term1 = (1.0 + 2.0 / pi) * sin(pi / (4.0 * a));
                double term2 = (1.0 - 2.0 / pi) * cos(pi / (4.0 * a));
                h = (a / sqrt(2.0)) * (term1 + term2);
            } else {
                double num = sin(pi * tau * (1.0 - alpha)) + four_a_tau * cos(pi * tau * (1.0 + alpha));
                h = num / denom;
            }
            h = h / sum;
            long v = lrint(h * 32768.0);
            if (v > 32767) {
                v = 32767;
            }
            if (v < -32768) {
                v = -32768;
            }
            taps_q15[n] = (int16_t)v;
        }
        last_sps = sps;
        last_span = span_used;
        last_alpha_q15 = d->cqpsk_rrc_alpha_q15;
        last_taps_len = taps_len;
        taps_ready = 1;
    }

    const int N = d->lp_len >> 1; /* complex length */
    int16_t* in = d->lowpassed;
    /* Avoid in-place aliasing with hb_workbuf; use timing_buf when needed. */
    int16_t* out = (in == d->hb_workbuf) ? d->timing_buf : d->hb_workbuf;
    int mid = last_taps_len / 2;
    for (int n = 0; n < N; n++) {
        int64_t accI = 0;
        int64_t accQ = 0;
        for (int k = 0; k < last_taps_len; k++) {
            int idx = n + (k - mid);
            if (idx < 0) {
                idx = 0;
            }
            if (idx >= N) {
                idx = N - 1;
            }
            int16_t ir = in[(size_t)(idx << 1)];
            int16_t iq = in[(size_t)(idx << 1) + 1];
            int16_t t = taps_q15[k];
            accI += (int32_t)ir * t;
            accQ += (int32_t)iq * t;
        }
        out[(size_t)(n << 1)] = sat16((int32_t)((accI + (1 << 14)) >> 15));
        out[(size_t)(n << 1) + 1] = sat16((int32_t)((accQ + (1 << 14)) >> 15));
    }
    /* swap buffers */
    memcpy(d->lowpassed, out, (size_t)(N << 1) * sizeof(int16_t));
}

/**
 * @brief Apply a lightweight Gardner timing correction to complex baseband.
 *
 * When enabled, this may adjust `lowpassed` and `lp_len` via the modular TED API.
 * Intended primarily for digital modes; typically disabled for analog FM.
 *
 * @param d Demodulator state (syncs `ted_state`, may modify samples/length).
 */
static void
gardner_timing_adjust(struct demod_state* d) {
    if (!d->ted_enabled || d->ted_sps <= 1) {
        return;
    }

    /* Sync from demod_state to module state */
    d->ted_state.mu_q20 = d->ted_mu_q20;

    ted_config_t cfg = {d->ted_enabled, d->ted_force, d->ted_gain_q20, d->ted_sps};

    gardner_timing_adjust(&cfg, &d->ted_state, d->lowpassed, &d->lp_len, d->timing_buf);

    /* Sync back to demod_state */
    d->ted_mu_q20 = d->ted_state.mu_q20;
}

/**
 * @brief Full demodulation pipeline for one block.
 *
 * Applies decimation (HB cascade or legacy), optional FLL and timing
 * correction, followed by the configured discriminator and post-processing.
 *
 * @param d Demodulator state (consumes lowpassed, produces result).
 */
void
full_demod(struct demod_state* d) {
    int i, ds_p;
    ds_p = d->downsample_passes;
    if (ds_p) {
        /* Choose decimator: half-band cascade (default) or legacy path */
        if (use_halfband_decimator) {
            /* Apply ds_p stages of 2:1 half-band decimation on interleaved lowpassed */
            int in_len = d->lp_len;
            int16_t* src = d->lowpassed;
            int16_t* dst = d->hb_workbuf;
            for (i = 0; i < ds_p; i++) {
                /* Stage-aware HB selection: heavier early, light later */
                const int16_t* taps = hb_q15_taps;
                int taps_len = HB_TAPS;
                if (i == 0) {
                    taps = hb31_q15_taps;
                    taps_len = 31;
                }
                /* Fused complex HB decimation on interleaved I/Q */
                int out_len_interleaved = hb_decim2_complex_interleaved_ex(src, in_len, dst, d->hb_hist_i[i],
                                                                           d->hb_hist_q[i], taps, taps_len);
                /* Next stage */
                src = dst;
                in_len = out_len_interleaved;
                dst = (src == d->hb_workbuf) ? d->lowpassed : d->hb_workbuf;
            }
            /* Final output resides in 'src' with length in_len; consume in-place (no copy) */
            d->lowpassed = src;
            d->lp_len = in_len;
            /* No droop compensation for half-band cascade */
        } else {
            for (i = 0; i < ds_p; i++) {
                fifth_order(d->lowpassed, (d->lp_len >> i), d->lp_i_hist[i]);
                fifth_order(d->lowpassed + 1, (d->lp_len >> i) - 1, d->lp_q_hist[i]);
            }
            d->lp_len = d->lp_len >> ds_p;
            /* droop compensation */
            if (d->comp_fir_size == 9 && ds_p <= CIC_TABLE_MAX) {
                generic_fir(d->lowpassed, d->lp_len, (int*)cic_9_tables[ds_p], d->droop_i_hist);
                generic_fir(d->lowpassed + 1, d->lp_len - 1, (int*)cic_9_tables[ds_p], d->droop_q_hist);
            }
        }
    } else {
        low_pass(d);
    }
    /* Bound channel noise when running at higher Fs (24 kHz default) */
    channel_lpf_apply(d);
    /* Reset per-block CQPSK rotation marker; set inside CQPSK branch when FLL applies rotation. */
    d->cqpsk_fll_rot_applied = 0;
    /*
     * Branch early by mode to simplify ordering.
     *
     * CQPSK (QPSK-like):
     *   DC block (optional) -> Matched Filter (RRC/MF) -> FLL (optional) -> Gardner TED (optional) -> Costas -> diff QPSK
     *
     * FM/C4FM and others:
     *   DC block -> AGC/limiter (when allowed) -> FLL (if enabled)
     */
    if (d->cqpsk_enable) {
        /* Optional complex DC removal */
        iq_dc_block(d);
        /* CQPSK matched filter before timing/carrier recovery */
        if (d->cqpsk_mf_enable) {
            if (d->cqpsk_rrc_enable) {
                mf_rrc_complex_interleaved(d);
            } else {
                mf5_complex_interleaved(d);
            }
        }
        /* Optional acquisition-only FLL for CQPSK (pre-Costas).
           Run after DC/MF so the symbol-spaced detector sees shaped, DC-free samples. */
        if (d->fll_enabled && d->cqpsk_acq_fll_enable && !d->cqpsk_acq_fll_locked && d->ted_sps >= 2) {
            int acq_updated = 0;
            /* Sync from demod_state into modular FLL state so that any
               external tweaks to fll_freq_q15/phase (e.g., spectrum-assisted
               correction) are honored by the CQPSK acquisition loop. */
            int prev_freq = d->fll_state.freq_q15;
            d->fll_state.freq_q15 = d->fll_freq_q15;
            d->fll_state.phase_q15 = d->fll_phase_q15;
            d->fll_state.prev_r = d->fll_prev_r;
            d->fll_state.prev_j = d->fll_prev_j;
            /* If the shared freq was externally adjusted (e.g., Costas), realign the
               integrator to that new setpoint so the acquisition loop does not pull
               back toward a stale integral state. Preserve the integrator otherwise. */
            if (prev_freq != d->fll_freq_q15) {
                d->fll_state.int_q15 = d->fll_freq_q15;
            }

            /* Build FLL config from current demod state */
            fll_config_t cfg;
            cfg.enabled = 1;
            cfg.alpha_q15 = (d->fll_alpha_q15 > 0) ? d->fll_alpha_q15 : 150;
            cfg.beta_q15 = (d->fll_beta_q15 > 0) ? d->fll_beta_q15 : 15;
            cfg.deadband_q14 = (d->fll_deadband_q14 > 0) ? d->fll_deadband_q14 : 45;
            cfg.slew_max_q15 = (d->fll_slew_max_q15 > 0) ? d->fll_slew_max_q15 : 64;

            /* Update only when we likely have a carrier; use the squelch gate as proxy. */
            if (d->squelch_gate_open && d->lp_len <= MAXIMUM_BUF_LENGTH) {
                /* Band-edge FLL (OP25 settings): updates loop and rotates in-place. */
                fll_update_error_qpsk(&cfg, &d->fll_state, d->lowpassed, d->lp_len, d->ted_sps);
                d->cqpsk_fll_rot_applied = 1;
                acq_updated = 1;
            }

            /* Sync back minimal state */
            d->fll_freq_q15 = d->fll_state.freq_q15;
            d->fll_phase_q15 = d->fll_state.phase_q15;
            d->fll_prev_r = d->fll_state.prev_r;
            d->fll_prev_j = d->fll_state.prev_j;

            /* Simple lock detector: when |freq| stays small for several blocks, stop acquisition FLL */
            if (acq_updated) {
                int fmag = d->fll_state.freq_q15;
                if (fmag < 0) {
                    fmag = -fmag;
                }
                const int kLockFreqThr = 64; /* small residual */
                const int kLockBlocks = 6;   /* consecutive blocks */
                if (fmag <= kLockFreqThr) {
                    d->cqpsk_acq_quiet_runs++;
                } else {
                    d->cqpsk_acq_quiet_runs = 0;
                }
                if (d->cqpsk_acq_quiet_runs >= kLockBlocks) {
                    d->cqpsk_acq_fll_locked = 1;
                }
            } else {
                d->cqpsk_acq_quiet_runs = 0;
            }
        }
        /* Lightweight timing error correction after FLL for CQPSK paths.
           Reuse the same FM guard to keep TED off for pure analog FM. */
        if (d->ted_enabled && (d->mode_demod != &dsd_fm_demod || d->ted_force)) {
            gardner_timing_adjust(d);
        }
        /* Carrier recovery (always run Costas for CQPSK),
           but skip during unit tests that use raw_demod. */
        if (d->mode_demod != &raw_demod) {
            cqpsk_costas_mix_and_update(d);
        }
    } else {
        /* Baseband conditioning order (FM/C4FM):
           1) Remove DC offset on I/Q to avoid biasing AGC and discriminator
           2) Block-based envelope AGC to normalize |z|
           3) Optional per-sample limiter to clamp fast AM ripple */
        iq_dc_block(d);
        /* Avoid running both AGC and limiter simultaneously to reduce gain "pumping". */
        if (d->fm_agc_enable) {
            fm_envelope_agc(d);
        } else if (d->fm_limiter_enable) {
            fm_constant_envelope_limiter(d);
        }
        /* Residual-CFO FLL when enabled */
        if (d->fll_enabled) {
            /* Update control only when squelch indicates a carrier; always apply rotation. */
            if (d->squelch_gate_open) {
                fll_update_error(d);
            }
            fll_mix_and_update(d);
        }
    }
    /* Mode-aware generic IQ balance (image suppression) after CFO rotation */
    if (d->iqbal_enable && !d->cqpsk_enable && d->lowpassed && d->lp_len >= 2) {
        /* Estimate s2 = E[z^2], p2 = E[|z|^2] over this block */
        double s2r = 0.0, s2i = 0.0, p2 = 0.0;
        int N = d->lp_len >> 1; /* complex pairs */
        const int16_t* iq = d->lowpassed;
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
        double ar = s2r / p2;
        double ai = s2i / p2;
        int a_q15 = d->iqbal_alpha_ema_a_q15 > 0 ? d->iqbal_alpha_ema_a_q15 : 6553; /* ~0.2 */
        int r_q15 = (int)(ar * 32768.0 + 0.5);
        int i_q15 = (int)(ai * 32768.0 + 0.5);
        if (r_q15 > 32767) {
            r_q15 = 32767;
        }
        if (r_q15 < -32768) {
            r_q15 = -32768;
        }
        if (i_q15 > 32767) {
            i_q15 = 32767;
        }
        if (i_q15 < -32768) {
            i_q15 = -32768;
        }
        int er = d->iqbal_alpha_ema_r_q15;
        int ei = d->iqbal_alpha_ema_i_q15;
        er += (int)(((int64_t)a_q15 * (int64_t)(r_q15 - er)) >> 15);
        ei += (int)(((int64_t)a_q15 * (int64_t)(i_q15 - ei)) >> 15);
        d->iqbal_alpha_ema_r_q15 = er;
        d->iqbal_alpha_ema_i_q15 = ei;
        int thr = d->iqbal_thr_q15 > 0 ? d->iqbal_thr_q15 : 655; /* ~0.02 */
        int mag2 = (er * er + ei * ei) >> 15;
        int thr2 = (thr * thr) >> 15;
        if (mag2 >= thr2) {
            int ar_q15 = er;
            int ai_q15 = ei;
            int16_t* out = d->lowpassed;
            for (int n = 0; n < N; n++) {
                int32_t I = out[(size_t)(n << 1) + 0];
                int32_t Q = out[(size_t)(n << 1) + 1];
                int32_t tI = (int32_t)(((int64_t)ar_q15 * I + (int64_t)ai_q15 * Q) >> 15);
                int32_t tQ = (int32_t)((-(int64_t)ar_q15 * Q + (int64_t)ai_q15 * I) >> 15);
                int32_t yI = I - tI;
                int32_t yQ = Q - tQ;
                if (yI > 32767) {
                    yI = 32767;
                }
                if (yI < -32768) {
                    yI = -32768;
                }
                if (yQ > 32767) {
                    yQ = 32767;
                }
                if (yQ < -32768) {
                    yQ = -32768;
                }
                out[(size_t)(n << 1) + 0] = (int16_t)yI;
                out[(size_t)(n << 1) + 1] = (int16_t)yQ;
            }
        }
    }
    /* CQPSK matched filtering handled earlier in the CQPSK branch.
       Apply Gardner TED here only for non-CQPSK paths (e.g., C4FM) and avoid
       analog FM unless explicitly forced. */
    if (d->ted_enabled && !d->cqpsk_enable && (d->mode_demod != &dsd_fm_demod || d->ted_force)) {
        gardner_timing_adjust(d);
    }
    /* Power squelch (sqrt-free): compare pair power mean (I^2+Q^2) against a threshold.
	   Threshold is specified as per-component mean power (RMS^2 on int16).
	   Since block_mean estimates E[I^2+Q^2], the equivalent threshold in this
	   domain is 2 * squelch_level. Samples are decimated by `squelch_decim_stride`;
	   an EMA smooths block power. The sampling phase advances by lp_len % stride
	   per block to cover all offsets. */
    if (d->squelch_level) {
        /* Decimated block power estimate (no DC correction; EMA smooths) */
        int stride = (d->squelch_decim_stride > 0) ? d->squelch_decim_stride : 16;
        int phase = d->squelch_decim_phase;
        int64_t p = 0;
        int count = 0;
        /* Ensure even I/Q alignment and accumulate pair power I^2+Q^2 */
        int start = phase & ~1;
        for (int j = start; j + 1 < d->lp_len; j += stride) {
            int64_t ir = (int64_t)d->lowpassed[j];
            int64_t jq = (int64_t)d->lowpassed[j + 1];
            p += ir * ir + jq * jq;
            count++;
        }
        /* Advance phase to sample different positions next block */
        if (stride > 0) {
            int adv = d->lp_len % stride;
            /* keep even alignment for I/Q pairing */
            if (adv & 1) {
                adv++;
            }
            if (adv >= stride) {
                adv %= stride;
            }
            d->squelch_decim_phase = (phase + adv) % stride;
        }
        if (count > 0) {
            int64_t block_mean = p / count; /* mean of I^2+Q^2 per complex sample */
            if (d->squelch_running_power == 0) {
                /* Initialize on first measurement to avoid long ramp */
                d->squelch_running_power = block_mean;
            } else {
                /* EMA: running += (block_mean - running) / window, window ~ 2^shift */
                int w = (d->squelch_window > 0) ? d->squelch_window : 2048;
                int shift = 0;
                /* approximate log2(window), prefer power-of-two windows */
                while ((1 << shift) < w && shift < 30) {
                    shift++;
                }
                int64_t delta = (block_mean - d->squelch_running_power);
                d->squelch_running_power += (delta >> shift);
            }
        }
        /* Convert per-component mean power threshold -> pair domain */
        int64_t thr_pair = 2LL * (int64_t)d->squelch_level;
        if (d->squelch_running_power < thr_pair) {
            d->squelch_hits++;
            for (i = 0; i < d->lp_len; i++) {
                d->lowpassed[i] = 0;
            }
            d->squelch_gate_open = 0;
        } else {
            d->squelch_hits = 0;
            d->squelch_gate_open = 1;
        }
    }
    /*
     * For CQPSK, produce a single real stream of differential phase symbols
     * (arg(z_n * conj(z_{n-1}))) to feed the legacy symbol sampler instead of
     * FM discriminating. For other paths, use the configured demodulator.
     */
    if (d->cqpsk_enable) {
        qpsk_differential_demod(d);
    } else {
        d->mode_demod(d); /* lowpassed -> result */
    }
    if (d->mode_demod == &raw_demod) {
        return;
    }
    /* todo, fm noise squelch */
    // use nicer filter here too?
    if (!d->cqpsk_enable && d->post_downsample > 1) {
        int decim = d->post_downsample;
        if (decim > 2) {
            /* Higher-quality polyphase decimator for larger factors */
            audio_polydecim_ensure(d, decim);
            int out_n = audio_polydecim_process(d, d->result, d->result_len, d->timing_buf);
            if (out_n > 0) {
                memcpy(d->result, d->timing_buf, (size_t)out_n * sizeof(int16_t));
                d->result_len = out_n;
            }
        } else {
            /* Pre-filter with one-pole and simple decimation for small factor */
            int Fs = (d->rate_out > 0) ? d->rate_out : 48000;
            double fc = 0.2 * ((double)Fs / (double)decim);
            if (fc < 50.0) {
                fc = 50.0;
            }
            double a = 1.0 - exp(-2.0 * 3.14159265358979323846 * fc / (double)Fs);
            if (a < 0.0) {
                a = 0.0;
            }
            if (a > 1.0) {
                a = 1.0;
            }
            int alpha_q15 = (int)lrint(a * 32768.0);
            if (alpha_q15 < 1) {
                alpha_q15 = 1;
            }
            if (alpha_q15 > 32767) {
                alpha_q15 = 32767;
            }
            int y = (d->result_len > 0) ? d->result[0] : 0;
            for (int k = 0; k < d->result_len; k++) {
                int x = (int)d->result[k];
                int dlt = x - y;
                int64_t delta = (int64_t)dlt * alpha_q15;
                if (dlt >= 0) {
                    delta += (1 << 14);
                } else {
                    delta -= (1 << 14);
                }
                y += (int)(delta >> 15);
                d->result[k] = sat16(y);
            }
            d->result_len = low_pass_simple(d->result, d->result_len, decim);
        }
    }
    if (!d->cqpsk_enable) {
        if (d->deemph) {
            deemph_filter(d);
        }
        /* Optional post-demod audio LPF */
        audio_lpf_filter(d);
        if (d->dc_block) {
            dc_block_filter(d);
        }
    }
    /* Skip post-demod audio decimator for CQPSK symbol streams. */
    if (!d->cqpsk_enable && d->rate_out2 > 0) {
        low_pass_real(d);
        //arbitrary_resample(d->result, d->result, d->result_len, d->result_len * d->rate_out2 / d->rate_out);
    }

    /* Apply soft squelch envelope on audio (skip for CQPSK symbol stream) */
    if (!d->cqpsk_enable) {
        int env = d->squelch_env_q15;
        int target = d->squelch_gate_open ? 32768 : 0;
        int alpha = d->squelch_gate_open ? (d->squelch_env_attack_q15 > 0 ? d->squelch_env_attack_q15 : 4096)
                                         : (d->squelch_env_release_q15 > 0 ? d->squelch_env_release_q15 : 1024);
        int err = target - env;
        int64_t delta = (int64_t)alpha * (int64_t)err;
        env += (int)(delta >> 15);
        if (env < 0) {
            env = 0;
        }
        if (env > 32768) {
            env = 32768;
        }
        d->squelch_env_q15 = env;
        /* Multiply audio by envelope */
        int16_t* res = d->result;
        for (int k = 0; k < d->result_len; k++) {
            int32_t v = (int32_t)((int64_t)res[k] * env >> 15);
            if (v > 32767) {
                v = 32767;
            }
            if (v < -32768) {
                v = -32768;
            }
            res[k] = (int16_t)v;
        }
    }
}
