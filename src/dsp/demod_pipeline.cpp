// SPDX-License-Identifier: GPL-2.0-or-later
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

#include <dsd-neo/dsp/cqpsk_path.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/fll.h>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/math_utils.h>
#include <dsd-neo/dsp/ted.h>
#include <dsd-neo/runtime/mem.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#ifndef MAX_BANDWIDTH_MULTIPLIER
#define MAX_BANDWIDTH_MULTIPLIER 8
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
extern int fll_lut_enabled;

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
static int
hb_decim2_complex_interleaved(const int16_t* DSD_NEO_RESTRICT in, int in_len, int16_t* DSD_NEO_RESTRICT out,
                              int16_t* DSD_NEO_RESTRICT hist_i, int16_t* DSD_NEO_RESTRICT hist_q) {
    const int hist_len = HB_TAPS - 1;
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
    /* Hoist half-band coefficients out of the loop */
    const int16_t c0 = hb_q15_taps[0];
    const int16_t c2 = hb_q15_taps[2];
    const int16_t c4 = hb_q15_taps[4];
    const int16_t c6 = hb_q15_taps[6];
    const int16_t c7 = hb_q15_taps[7];
    for (int n = 0; n < out_ch_len; n++) {
        int center_idx = hist_len + (n << 1); /* per-channel index */
        /* Half-band optimization: only even taps and the center tap contribute (symmetric). */
        auto get_iq = [&](int src_idx, int16_t& xi, int16_t& xq) {
            if (src_idx < hist_len) {
                xi = hi[src_idx];
                xq = hq[src_idx];
            } else {
                int rel = src_idx - hist_len;
                if (rel < ch_len) {
                    xi = in_al[(size_t)(rel << 1)];
                    xq = in_al[(size_t)(rel << 1) + 1];
                } else {
                    xi = lastI;
                    xq = lastQ;
                }
            }
        };
        int16_t ci, cq;
        int16_t im1, qm1, ip1, qp1;
        int16_t im3, qm3, ip3, qp3;
        int16_t im5, qm5, ip5, qp5;
        int16_t im7, qm7, ip7, qp7;
        get_iq(center_idx, ci, cq);
        get_iq(center_idx - 1, im1, qm1);
        get_iq(center_idx + 1, ip1, qp1);
        get_iq(center_idx - 3, im3, qm3);
        get_iq(center_idx + 3, ip3, qp3);
        get_iq(center_idx - 5, im5, qm5);
        get_iq(center_idx + 5, ip5, qp5);
        get_iq(center_idx - 7, im7, qm7);
        get_iq(center_idx + 7, ip7, qp7);
        int64_t accI = 0;
        int64_t accQ = 0;
        accI += (int32_t)c7 * (int32_t)ci;
        accQ += (int32_t)c7 * (int32_t)cq;
        accI += (int32_t)c6 * (int32_t)(im1 + ip1);
        accQ += (int32_t)c6 * (int32_t)(qm1 + qp1);
        accI += (int32_t)c4 * (int32_t)(im3 + ip3);
        accQ += (int32_t)c4 * (int32_t)(qm3 + qp3);
        accI += (int32_t)c2 * (int32_t)(im5 + ip5);
        accQ += (int32_t)c2 * (int32_t)(qm5 + qp5);
        accI += (int32_t)c0 * (int32_t)(im7 + ip7);
        accQ += (int32_t)c0 * (int32_t)(qm7 + qp7);
        accI += (1 << 14);
        accQ += (1 << 14);
        int32_t yI = (int32_t)(accI >> 15);
        int32_t yQ = (int32_t)(accQ >> 15);
        out_al[(size_t)(n << 1)] = sat16(yI);
        out_al[(size_t)(n << 1) + 1] = sat16(yQ);
    }
    /* Update histories with last HB_TAPS-1 per-channel input samples */
    if (ch_len >= hist_len) {
        int start = ch_len - hist_len;
        for (int k = 0; k < hist_len; k++) {
            int rel = start + k;
            hi[k] = in_al[(size_t)(rel << 1)];
            hq[k] = in_al[(size_t)(rel << 1) + 1];
        }
    } else {
        /* Not enough input samples; pad with zeros */
        int existing = ch_len;
        for (int k = 0; k < hist_len; k++) {
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
    return out_ch_len << 1; /* Return total elements (2 * complex samples) */
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
        data[d] = sum >> 15;
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
    int i, pcm;
    int16_t* lp = assume_aligned_ptr(fm->lowpassed, DSD_NEO_ALIGN);
    int16_t* res = assume_aligned_ptr(fm->result, DSD_NEO_ALIGN);
    /* Use selected discriminator from the very first sample */
    pcm = fm->discriminator(lp[0], lp[1], fm->pre_r, fm->pre_j);
    /* Remove known NCO injection from FLL rotation (demod sees -dphi per sample).
	   Scale Q15 (2*pi==1<<15) to Q14 (pi==1<<14) by >>1. */
    if (fm->fll_enabled) {
        pcm += (fm->fll_freq_q15 >> 1);
    }
    res[0] = (int16_t)pcm;
    DSD_NEO_IVDEP
    for (i = 2; i < (fm->lp_len - 1); i += 2) {
        pcm = fm->discriminator(lp[i], lp[i + 1], lp[i - 2], lp[i - 1]);
        if (fm->fll_enabled) {
            pcm += (fm->fll_freq_q15 >> 1);
        }
        res[i / 2] = (int16_t)pcm;
    }
    fm->pre_r = lp[fm->lp_len - 2];
    fm->pre_j = lp[fm->lp_len - 1];
    fm->result_len = fm->lp_len / 2;
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
    cfg.use_lut = fll_lut_enabled;

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
    cfg.use_lut = fll_lut_enabled;

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
 * CQPSK/LSM paths where constant-modulus EQ handles amplitude.
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

    /* Auto-tune AGC parameters heuristically (optional) */
    if (d->fm_agc_auto_enable) {
        /* Track envelope ripple and clipping over time */
        enum { MAXC = 8 };

        static int clip_run = 0;
        static int under_run = 0;
        static double ema_rms = 0.0;
        static int init = 0;
        if (!init) {
            ema_rms = rms;
            init = 1;
        } else {
            ema_rms = 0.9 * ema_rms + 0.1 * rms;
        }
        int clip_thresh = pairs / 512; /* ~0.2% of complex samples */
        if (clip_thresh < 1) {
            clip_thresh = 1;
        }
        if (clip_cnt > clip_thresh) {
            if (clip_run < MAXC) {
                clip_run++;
            }
        } else if (clip_run > 0) {
            clip_run--;
        }
        /* If we're far from full-scale with no clipping for a while, nudge target up */
        int desired_peak = 28000; /* aim for ~85% FS */
        if (max_abs < 20000 && clip_cnt == 0) {
            if (under_run < MAXC) {
                under_run++;
            }
        } else if (under_run > 0) {
            under_run--;
        }
        /* Adjust target based on persistent conditions */
        int tgt = d->fm_agc_target_rms;
        if (clip_run >= 4) {
            tgt -= 400;
            if (tgt < 6000) {
                tgt = 6000;
            }
            d->fm_agc_target_rms = tgt;
            clip_run = 0;
        } else if (under_run >= 8) {
            tgt += 300;
            if (tgt > 14000) {
                tgt = 14000;
            }
            d->fm_agc_target_rms = tgt;
            under_run = 0;
        }
        /* Adapt response speed based on ripple (how much rms deviates) */
        double dev = fabs(rms - ema_rms);
        double ripple = (ema_rms > 1e-6) ? (dev / ema_rms) : 0.0;
        int au = d->fm_agc_alpha_up_q15;
        int ad = d->fm_agc_alpha_down_q15;
        if (ripple > 0.10) { /* >10% short-term change */
            if (au < 24576) {
                au += 1024;
            }
            if (ad < 28672) {
                ad += 1024;
            }
        } else if (ripple < 0.03) {
            if (au > 4096) {
                au -= 512;
            }
            if (ad > 16384) {
                ad -= 512;
            }
        }
        if (au < 1) {
            au = 1;
        }
        if (au > 32768) {
            au = 32768;
        }
        if (ad < 1) {
            ad = 1;
        }
        if (ad > 32768) {
            ad = 32768;
        }
        d->fm_agc_alpha_up_q15 = au;
        d->fm_agc_alpha_down_q15 = ad;
        /* Keep min_rms as a fraction of target so it engages when useful */
        d->fm_agc_min_rms = d->fm_agc_target_rms / 4;
    }
}

/* Optional complex DC blocker prior to FM discrimination */
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
    /* Estimate DC using block mean, then smooth the applied offset */
    int64_t sumI = 0, sumQ = 0;
    for (int n = 0; n < pairs; n++) {
        sumI += (int)iq[(size_t)(n << 1) + 0];
        sumQ += (int)iq[(size_t)(n << 1) + 1];
    }
    int meanI = (pairs > 0) ? (int)(sumI / pairs) : 0;
    int meanQ = (pairs > 0) ? (int)(sumQ / pairs) : 0;
    int dcI = d->iq_dc_avg_r + ((meanI - d->iq_dc_avg_r) >> k);
    int dcQ = d->iq_dc_avg_i + ((meanQ - d->iq_dc_avg_i) >> k);
    /* Apply constant subtraction across this block */
    for (int n = 0; n < pairs; n++) {
        int yI = (int)iq[(size_t)(n << 1) + 0] - dcI;
        int yQ = (int)iq[(size_t)(n << 1) + 1] - dcQ;
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
    int span = (d->cqpsk_rrc_span_syms > 0) ? d->cqpsk_rrc_span_syms : 6;
    int taps_len = span * 2 * sps + 1;
    if (taps_len < 7) {
        taps_len = 7;
    }
    if (taps_len > 257) {
        taps_len = 257;
    }
    static int last_sps = 0;
    static int last_span = 0;
    static int last_alpha_q15 = 0;
    static int last_taps_len = 0;
    static int16_t taps_q15[257];
    static int taps_ready = 0;
    if (!taps_ready || sps != last_sps || span != last_span || d->cqpsk_rrc_alpha_q15 != last_alpha_q15
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
        last_span = span;
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

    ted_config_t cfg = {
        .enabled = d->ted_enabled, .force = d->ted_force, .gain_q20 = d->ted_gain_q20, .sps = d->ted_sps};

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
                /* Fused complex HB decimation on interleaved I/Q */
                int out_len_interleaved =
                    hb_decim2_complex_interleaved(src, in_len, dst, d->hb_hist_i[i], d->hb_hist_q[i]);
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
    /* Baseband conditioning order (FM/C4FM):
       1) Remove DC offset on I/Q to avoid biasing AGC and discriminator
       2) Block-based envelope AGC to normalize |z|
       3) Optional per-sample limiter to clamp fast AM ripple */
    iq_dc_block(d);
    fm_envelope_agc(d);
    fm_constant_envelope_limiter(d);
    /* Mode-aware generic IQ balance (image suppression): engage for non-QPSK paths */
    if (d->iqbal_enable && !d->cqpsk_enable && d->lowpassed && d->lp_len >= 2) {
        /* Estimate s2 = E[z^2], p2 = E[|z|^2] over this block */
        double s2r = 0.0, s2i = 0.0, p2 = 0.0;
        int N = d->lp_len >> 1; /* complex pairs */
        const int16_t* iq = d->lowpassed;
        for (int n = 0; n < N; n++) {
            double I = (double)iq[(size_t)(n << 1) + 0];
            double Q = (double)iq[(size_t)(n << 1) + 1];
            /* z^2 = (I^2 - Q^2) + j*2IQ; |z|^2 = I^2 + Q^2 */
            s2r += I * I - Q * Q;
            s2i += 2.0 * I * Q;
            p2 += I * I + Q * Q;
        }
        if (p2 <= 1e-9) {
            p2 = 1e-9;
        }
        double ar = s2r / p2;
        double ai = s2i / p2;
        /* Smooth alpha via EMA in Q15 */
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
        /* Gate by magnitude threshold */
        int thr = d->iqbal_thr_q15 > 0 ? d->iqbal_thr_q15 : 655; /* ~0.02 */
        int mag2 = (er * er + ei * ei) >> 15;                    /* approximate with Q15 scaling */
        int thr2 = (thr * thr) >> 15;
        if (mag2 >= thr2) {
            /* Apply y = z - alpha * conj(z) with alpha = er/ei (Q15) */
            int ar_q15 = er;
            int ai_q15 = ei;
            int16_t* out = d->lowpassed;
            for (int n = 0; n < N; n++) {
                int32_t I = out[(size_t)(n << 1) + 0];
                int32_t Q = out[(size_t)(n << 1) + 1];
                /* tI = ar*I + ai*Q; tQ = -ar*Q + ai*I (Q15) */
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
    /* Residual CFO loop: estimate error then rotate */
    fll_update_error(d);
    fll_mix_and_update(d);
    /* Optional CQPSK/LSM pre-processing on complex baseband for QPSK paths */
    if (d->cqpsk_enable) {
        if (d->cqpsk_mf_enable) {
            if (d->cqpsk_rrc_enable) {
                mf_rrc_complex_interleaved(d);
            } else {
                mf5_complex_interleaved(d);
            }
        }
        cqpsk_process_block(d);
    }
    /* Lightweight timing error correction (optional, avoid for analog FM demod) */
    if (d->ted_enabled && (d->mode_demod != &dsd_fm_demod || d->ted_force)) {
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
        } else {
            d->squelch_hits = 0;
        }
    }
    d->mode_demod(d); /* lowpassed -> result */
    if (d->mode_demod == &raw_demod) {
        return;
    }
    /* todo, fm noise squelch */
    // use nicer filter here too?
    if (d->post_downsample > 1) {
        d->result_len = low_pass_simple(d->result, d->result_len, d->post_downsample);
    }
    if (d->deemph) {
        deemph_filter(d);
    }
    /* Optional post-demod audio LPF */
    audio_lpf_filter(d);
    if (d->dc_block) {
        dc_block_filter(d);
    }
    if (d->rate_out2 > 0) {
        low_pass_real(d);
        //arbitrary_resample(d->result, d->result, d->result_len, d->result_len * d->rate_out2 / d->rate_out);
    }
}
