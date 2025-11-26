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
static const float channel_lpf_wide[kChannelLpfTaps] = {
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

/* Digital-narrow profile taps (fc≈5 kHz @ 24 kHz, 63 taps). Designed as
   Blackman-windowed sinc and normalized to unity DC gain. */
static const float channel_lpf_digital[kChannelLpfTaps] = {
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

/* P25 Hann LPF (≈7 kHz cutoff @ 24 kHz Fs, 63 taps). Hann-windowed sinc
   inspired by OP25 channel filter: ~0 dB through ~6.25 kHz, ~-6 dB near 7 kHz,
   >60 dB attenuation by ~8 kHz. */
static const float channel_lpf_p25_hann[kChannelLpfTaps] = {
    0.0f,
    -1.0f / 32768.0f,
    1.0f / 32768.0f,
    7.0f / 32768.0f,
    -11.0f / 32768.0f,
    -13.0f / 32768.0f,
    36.0f / 32768.0f,
    0.0f,
    -68.0f / 32768.0f,
    46.0f / 32768.0f,
    83.0f / 32768.0f,
    -126.0f / 32768.0f,
    -46.0f / 32768.0f,
    217.0f / 32768.0f,
    -67.0f / 32768.0f,
    -268.0f / 32768.0f,
    258.0f / 32768.0f,
    214.0f / 32768.0f,
    -485.0f / 32768.0f,
    0.0f,
    660.0f / 32768.0f,
    -399.0f / 32768.0f,
    -661.0f / 32768.0f,
    954.0f / 32768.0f,
    339.0f / 32768.0f,
    -1583.0f / 32768.0f,
    506.0f / 32768.0f,
    2167.0f / 32768.0f,
    -2402.0f / 32768.0f,
    -2581.0f / 32768.0f,
    10049.0f / 32768.0f,
    19114.0f / 32768.0f,
    10049.0f / 32768.0f,
    -2581.0f / 32768.0f,
    -2402.0f / 32768.0f,
    2167.0f / 32768.0f,
    506.0f / 32768.0f,
    -1583.0f / 32768.0f,
    339.0f / 32768.0f,
    954.0f / 32768.0f,
    -661.0f / 32768.0f,
    -399.0f / 32768.0f,
    660.0f / 32768.0f,
    0.0f,
    -485.0f / 32768.0f,
    214.0f / 32768.0f,
    258.0f / 32768.0f,
    -268.0f / 32768.0f,
    -67.0f / 32768.0f,
    217.0f / 32768.0f,
    -46.0f / 32768.0f,
    -126.0f / 32768.0f,
    83.0f / 32768.0f,
    46.0f / 32768.0f,
    -68.0f / 32768.0f,
    0.0f,
    36.0f / 32768.0f,
    -13.0f / 32768.0f,
    -11.0f / 32768.0f,
    7.0f / 32768.0f,
    1.0f / 32768.0f,
    -1.0f / 32768.0f,
    0.0f,
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
        void* mem_ptr = dsd_neo_aligned_malloc((size_t)K * sizeof(float));
        d->post_polydecim_taps = (float*)mem_ptr;
    }
    {
        void* mem_ptr = dsd_neo_aligned_malloc((size_t)K * sizeof(float));
        d->post_polydecim_hist = (float*)mem_ptr;
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
    memset(d->post_polydecim_hist, 0, (size_t)K * sizeof(float));
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
        d->post_polydecim_taps[n] = (float)t;
    }
    d->post_polydecim_M = M;
    d->post_polydecim_K = K;
    d->post_polydecim_enabled = 1;
}

static int
audio_polydecim_process(struct demod_state* d, const float* in, int in_len, float* out) {
    if (!d || !d->post_polydecim_enabled || !d->post_polydecim_taps || !d->post_polydecim_hist || in_len <= 0) {
        if (in && out && in_len > 0 && in != out) {
            memcpy(out, in, (size_t)in_len * sizeof(float));
        }
        return in_len;
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
            float acc = 0.0f;
            for (int k = 0; k < K; k++) {
                acc += d->post_polydecim_hist[idx] * taps[k];
                idx--;
                if (idx < 0) {
                    idx += K;
                }
            }
            out[out_len++] = acc;
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
    const float* taps = channel_lpf_wide;
    switch (d->channel_lpf_profile) {
        case DSD_CH_LPF_PROFILE_DIGITAL: taps = channel_lpf_digital; break;
        case DSD_CH_LPF_PROFILE_P25_HANN: taps = channel_lpf_p25_hann; break;
        default: break;
    }
    const float* in = assume_aligned_ptr(d->lowpassed, DSD_NEO_ALIGN);
    float* out = (d->lowpassed == d->hb_workbuf) ? d->timing_buf : d->hb_workbuf;
    float* hi = assume_aligned_ptr(d->channel_lpf_hist_i, DSD_NEO_ALIGN);
    float* hq = assume_aligned_ptr(d->channel_lpf_hist_q, DSD_NEO_ALIGN);
    float lastI = (N > 0) ? in[(size_t)((N - 1) << 1)] : 0.0f;
    float lastQ = (N > 0) ? in[(size_t)(((N - 1) << 1) + 1)] : 0.0f;

    for (int n = 0; n < N; n++) {
        int center_idx = hist_len + n; /* index into combined hist+current stream */
        auto get_iq = [&](int src_idx, float& xi, float& xq) {
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
        float accI = 0.0f;
        float accQ = 0.0f;
        /* center tap */
        {
            float ci, cq;
            get_iq(center_idx, ci, cq);
            float cc = taps[center];
            accI += cc * ci;
            accQ += cc * cq;
        }
        /* symmetric pairs */
        for (int k = 0; k < center; k++) {
            float ce = taps[k];
            if (ce == 0.0f) {
                continue;
            }
            int dists = center - k;
            float xmI, xmQ, xpI, xpQ;
            get_iq(center_idx - dists, xmI, xmQ);
            get_iq(center_idx + dists, xpI, xpQ);
            accI += ce * (xmI + xpI);
            accQ += ce * (xmQ + xpQ);
        }
        out[(size_t)(n << 1)] = accI;
        out[(size_t)(n << 1) + 1] = accQ;
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
                memmove(hi, hi + (hist_len - need), (size_t)need * sizeof(float));
                memmove(hq, hq + (hist_len - need), (size_t)need * sizeof(float));
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
static const float cic_9_tables[][10] = {
    /* ds_p=0: no compensation needed */
    {0.0f},
    /* ds_p=1: single stage */
    {0.0f, 8192.0f / 32768.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    /* ds_p=2: two stages */
    {0.0f, 4096.0f / 32768.0f, 4096.0f / 32768.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    /* ds_p=3: three stages */
    {0.0f, 2730.0f / 32768.0f, 2730.0f / 32768.0f, 2730.0f / 32768.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    /* ds_p=4: four stages */
    {0.0f, 2048.0f / 32768.0f, 2048.0f / 32768.0f, 2048.0f / 32768.0f, 2048.0f / 32768.0f, 0.0f, 0.0f, 0.0f, 0.0f,
     0.0f},
    /* ds_p=5: five stages */
    {0.0f, 1638.0f / 32768.0f, 1638.0f / 32768.0f, 1638.0f / 32768.0f, 1638.0f / 32768.0f, 1638.0f / 32768.0f, 0.0f,
     0.0f, 0.0f, 0.0f},
    /* ds_p=6: six stages */
    {0.0f, 1365.0f / 32768.0f, 1365.0f / 32768.0f, 1365.0f / 32768.0f, 1365.0f / 32768.0f, 1365.0f / 32768.0f,
     1365.0f / 32768.0f, 0.0f, 0.0f, 0.0f},
    /* ds_p=7: seven stages */
    {0.0f, 1170.0f / 32768.0f, 1170.0f / 32768.0f, 1170.0f / 32768.0f, 1170.0f / 32768.0f, 1170.0f / 32768.0f,
     1170.0f / 32768.0f, 1170.0f / 32768.0f, 0.0f, 0.0f},
    /* ds_p=8: eight stages */
    {0.0f, 1024.0f / 32768.0f, 1024.0f / 32768.0f, 1024.0f / 32768.0f, 1024.0f / 32768.0f, 1024.0f / 32768.0f,
     1024.0f / 32768.0f, 1024.0f / 32768.0f, 1024.0f / 32768.0f, 0.0f},
    /* ds_p=9: nine stages */
    {0.0f, 910.0f / 32768.0f, 910.0f / 32768.0f, 910.0f / 32768.0f, 910.0f / 32768.0f, 910.0f / 32768.0f,
     910.0f / 32768.0f, 910.0f / 32768.0f, 910.0f / 32768.0f, 910.0f / 32768.0f},
    /* ds_p=10: ten stages */
    {0.0f, 819.0f / 32768.0f, 819.0f / 32768.0f, 819.0f / 32768.0f, 819.0f / 32768.0f, 819.0f / 32768.0f,
     819.0f / 32768.0f, 819.0f / 32768.0f, 819.0f / 32768.0f, 819.0f / 32768.0f}};

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
hb_decim2_complex_interleaved_ex(const float* DSD_NEO_RESTRICT in, int in_len, float* DSD_NEO_RESTRICT out,
                                 float* DSD_NEO_RESTRICT hist_i, float* DSD_NEO_RESTRICT hist_q,
                                 const float* DSD_NEO_RESTRICT taps_q15, int taps_len) {
    if (taps_len < 3 || (taps_len & 1) == 0) {
        return 0; /* invalid */
    }
    int ch_len = in_len >> 1;     /* per-channel samples */
    int out_ch_len = ch_len >> 1; /* decimated per-channel */
    if (out_ch_len <= 0) {
        return 0;
    }
    const float* DSD_NEO_RESTRICT in_al = assume_aligned_ptr(in, DSD_NEO_ALIGN);
    float* DSD_NEO_RESTRICT out_al = assume_aligned_ptr(out, DSD_NEO_ALIGN);
    float* DSD_NEO_RESTRICT hi = assume_aligned_ptr(hist_i, DSD_NEO_ALIGN);
    float* DSD_NEO_RESTRICT hq = assume_aligned_ptr(hist_q, DSD_NEO_ALIGN);
    float lastI = (ch_len > 0) ? in_al[in_len - 2] : 0.0f;
    float lastQ = (ch_len > 0) ? in_al[in_len - 1] : 0.0f;
    const int center = (taps_len - 1) >> 1;
    const int left_len = taps_len - 1;
    for (int n = 0; n < out_ch_len; n++) {
        int center_idx = (taps_len - 1) + (n << 1); /* per-channel index with left history */
        auto get_iq = [&](int src_idx, float& xi, float& xq) {
            if (src_idx < left_len) {
                xi = hi[src_idx];
                xq = hq[src_idx];
            } else {
                int rel = src_idx - left_len;
                if (rel < ch_len) {
                    xi = in_al[(size_t)(rel << 1)];
                    xq = in_al[(size_t)(rel << 1) + 1];
                } else {
                    xi = lastI;
                    xq = lastQ;
                }
            }
        };
        float accI = 0.0f;
        float accQ = 0.0f;
        /* center tap */
        {
            float ci, cq;
            get_iq(center_idx, ci, cq);
            float cc = taps_q15[center];
            accI += cc * ci;
            accQ += cc * cq;
        }
        /* symmetric pairs: only even indices in the full taps array are non-zero */
        for (int e = 0; e < center; e += 2) {
            int d = center - e; /* distance from center */
            float ce = taps_q15[e];
            if (ce == 0.0f) {
                continue;
            }
            float xmI, xmQ, xpI, xpQ;
            get_iq(center_idx - d, xmI, xmQ);
            get_iq(center_idx + d, xpI, xpQ);
            accI += ce * (xmI + xpI);
            accQ += ce * (xmQ + xpQ);
        }
        out_al[(size_t)(n << 1)] = accI;
        out_al[(size_t)(n << 1) + 1] = accQ;
    }
    /* Update histories with last (taps_len-1) per-channel input samples */
    if (ch_len >= left_len) {
        int start = ch_len - left_len;
        for (int k = 0; k < left_len; k++) {
            int rel = start + k;
            hi[k] = in_al[(size_t)(rel << 1)];
            hq[k] = in_al[(size_t)(rel << 1) + 1];
        }
    } else {
        int existing = ch_len;
        for (int k = 0; k < left_len; k++) {
            if (k < existing) {
                int rel = k;
                hi[k] = in_al[(size_t)(rel << 1)];
                hq[k] = in_al[(size_t)(rel << 1) + 1];
            } else {
                hi[k] = 0.0f;
                hq[k] = 0.0f;
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
    DSD_NEO_IVDEP
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
 * @brief Deferred low-pass: sums and decimates with saturation on writeback.
 *
 * @param d Demodulator state (uses lowpassed buffer and decimation state).
 */
void
low_pass(struct demod_state* d) {
    int i = 0, i2 = 0;
    float* DSD_NEO_RESTRICT lp = assume_aligned_ptr(d->lowpassed, DSD_NEO_ALIGN);
    while (i < d->lp_len) {
        d->now_r += lp[i];
        d->now_j += lp[i + 1];
        i += 2;
        d->prev_index++;
        if (d->prev_index < d->downsample) {
            continue;
        }
        lp[i2] = d->now_r;
        lp[i2 + 1] = d->now_j;
        d->prev_index = 0;
        d->now_r = 0.0f;
        d->now_j = 0.0f;
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
fifth_order(float* data, int length, float* hist) {
    int i;
    float a, b, c, d, e, f;
    a = hist[1];
    b = hist[2];
    c = hist[3];
    d = hist[4];
    e = hist[5];
    f = data[0];
    /* a downsample should improve resolution, so don't fully shift */
    data[0] = (a + (b + e) * 5.0f + (c + d) * 10.0f + f) * (1.0f / 16.0f);
    for (i = 4; i < length; i += 4) {
        a = c;
        b = d;
        c = e;
        d = f;
        e = data[i - 2];
        f = data[i];
        data[i / 2] = (a + (b + e) * 5.0f + (c + d) * 10.0f + f) * (1.0f / 16.0f);
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
generic_fir(float* data, int length, const float* fir, float* hist) {
    int d;
    for (d = 0; d < length; d += 2) {
        float temp = data[d];
        float sum = 0.0f;
        sum += (hist[0] + hist[8]) * fir[1];
        sum += (hist[1] + hist[7]) * fir[2];
        sum += (hist[2] + hist[6]) * fir[3];
        sum += (hist[3] + hist[5]) * fir[4];
        sum += hist[4] * fir[5];
        data[d] = sum;
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

    const float* iq = assume_aligned_ptr(fm->lowpassed, DSD_NEO_ALIGN);
    float* out = assume_aligned_ptr(fm->result, DSD_NEO_ALIGN);

    float prev_r = fm->pre_r;
    float prev_j = fm->pre_j;
    /* Seed history on first use to keep the first phase delta well-defined. */
    if (prev_r == 0.0f && prev_j == 0.0f) {
        prev_r = iq[0];
        prev_j = iq[1];
    }

    for (int n = 0; n < pairs; n++) {
        float cr = iq[(size_t)(n << 1) + 0];
        float cj = iq[(size_t)(n << 1) + 1];
        /* z_n * conj(z_{n-1}) => phase delta; amplitude cancels inside atan2 */
        float re = cr * prev_r + cj * prev_j;
        float im = cj * prev_r - cr * prev_j;
        float angle = atan2f(im, re);
        if (fm->fll_enabled) {
            angle += 0.5f * fm->fll_freq;
        }
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
 * through CQPSK processing (front-end filtering, optional acquisition FLL/TED, Costas).
 * Produces a single real stream (I only) to feed the legacy symbol sampler path.
 */
/**
 * @brief Phase extractor for CQPSK differential phasors.
 *
 * Expects fm->lowpassed to hold the output of cqpsk_diff_phasor (already
 * differenced). Converts each complex phasor to a Q14 phase angle:
 *   theta_n = arg(z_n)
 *
 * @param fm Demodulator state (reads interleaved I/Q in lowpassed, writes phase to result).
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

    for (int n = 0; n < pairs; n++) {
        float phase = atan2f(iq[(size_t)(n << 1) + 1], iq[(size_t)(n << 1) + 0]);
        out[n] = phase;
    }

    fm->result_len = pairs;
}

/* RMS AGC for CQPSK paths (mirrors OP25 rms_agc placement/behavior). */
static inline void
cqpsk_rms_agc(struct demod_state* d) {
    if (!d || !d->cqpsk_rms_agc_enable || !d->lowpassed || d->lp_len < 2) {
        return;
    }
    const int pairs = d->lp_len >> 1;
    const float kAlpha = 0.45f; /* OP25 default */
    const float kRef = 0.85f;   /* target RMS (normalized) */
    const float kEps = 1e-6f;

    float rms = d->cqpsk_rms_agc_rms;
    if (rms <= 0.0f) {
        rms = kRef;
    }

    float* out = d->lowpassed;
    for (int n = 0; n < pairs; n++) {
        float I = out[(size_t)(n << 1)];
        float Q = out[(size_t)(n << 1) + 1];
        float mag2 = I * I + Q * Q;
        float rms2 = rms * rms;
        rms2 = (1.0f - kAlpha) * rms2 + kAlpha * mag2;
        rms = sqrtf(rms2);
        float g = kRef / (rms + kEps);
        if (g > 8.0f) {
            g = 8.0f;
        }
        if (g < 0.125f) {
            g = 0.125f;
        }
        out[(size_t)(n << 1)] = I * g;
        out[(size_t)(n << 1) + 1] = Q * g;
    }

    d->cqpsk_rms_agc_rms = rms;
}

/**
 * @brief Apply post-demod deemphasis IIR filter with Q15 coefficient.
 *
 * @param fm Demodulator state (reads/writes result, updates deemph_avg).
 */
void
deemph_filter(struct demod_state* fm) {
    float avg = fm->deemph_avg; /* per-instance state */
    float* res = assume_aligned_ptr(fm->result, DSD_NEO_ALIGN);
    float alpha = (float)fm->deemph_a * (1.0f / 32768.0f);
    if (alpha < 0.0f) {
        alpha = 0.0f;
    }
    if (alpha > 1.0f) {
        alpha = 1.0f;
    }
    /* Single-pole IIR: avg += (x - avg) * alpha */
    DSD_NEO_IVDEP
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
    const int k = 11; /* cutoff ~ Fs / 2^k (k in 10..12) */
    float k_scale = 1.0f / (float)(1 << k);
    float* res = assume_aligned_ptr(fm->result, DSD_NEO_ALIGN);
    DSD_NEO_IVDEP
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
    float alpha = (float)fm->audio_lpf_alpha * (1.0f / 32768.0f);
    if (alpha < 0.0f) {
        alpha = 0.0f;
    }
    if (alpha > 1.0f) {
        alpha = 1.0f;
    }
    DSD_NEO_IVDEP
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
mean_power(float* samples, int len, int step) {
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
    d->fll_state.freq = d->fll_freq;
    d->fll_state.phase = d->fll_phase;
    d->fll_state.prev_r = d->fll_prev_r;
    d->fll_state.prev_j = d->fll_prev_j;

    fll_config_t cfg;
    cfg.enabled = d->fll_enabled;
    cfg.alpha = d->fll_alpha;
    cfg.beta = d->fll_beta;
    cfg.deadband = d->fll_deadband;
    cfg.slew_max = d->fll_slew_max;

    /* Use QPSK-friendly symbol-spaced update when CQPSK path is active.
       Skip FLL band-edge if non-integer SPS detected (requires integer SPS). */
    if (d->cqpsk_enable && d->ted_sps >= 2 && d->sps_is_integer) {
        fll_update_error_qpsk(&cfg, &d->fll_state, d->lowpassed, d->lp_len, d->ted_sps);
    } else if (d->cqpsk_enable && d->ted_sps >= 2 && !d->sps_is_integer) {
        /* Non-integer SPS: fall back to standard FLL to avoid band-edge malfunction */
        fll_update_error(&cfg, &d->fll_state, d->lowpassed, d->lp_len);
    } else {
        fll_update_error(&cfg, &d->fll_state, d->lowpassed, d->lp_len);
    }

    /* Sync back to demod_state */
    d->fll_freq = d->fll_state.freq;
    d->fll_phase = d->fll_state.phase;
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
    d->fll_state.freq = d->fll_freq;
    d->fll_state.phase = d->fll_phase;
    d->fll_state.prev_r = d->fll_prev_r;
    d->fll_state.prev_j = d->fll_prev_j;

    fll_config_t cfg;
    cfg.enabled = d->fll_enabled;
    cfg.alpha = d->fll_alpha;
    cfg.beta = d->fll_beta;
    cfg.deadband = d->fll_deadband;
    cfg.slew_max = d->fll_slew_max;

    fll_mix_and_update(&cfg, &d->fll_state, d->lowpassed, d->lp_len);

    /* Sync back to demod_state */
    d->fll_freq = d->fll_state.freq;
    d->fll_phase = d->fll_state.phase;
    d->fll_prev_r = d->fll_state.prev_r;
    d->fll_prev_j = d->fll_state.prev_j;
}

/*
 * FM envelope AGC/limiter (per-sample)
 *
 * Normalizes complex I/Q magnitude toward a target RMS to reduce amplitude
 * bounce from low-cost front-ends (e.g., RTL-SDR). Matches the OP25/GNU Radio
 * style of per-sample RMS tracking and gain update (attack/decay), rather than
 * a block-stepped gain. Disabled for CQPSK paths to leave constellation
 * amplitude untouched.
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
    /* Guard: avoid boosting pure noise when input is below configured minimum */
    double acc = 0.0;
    const float* iq_pre = d->lowpassed;
    for (int n = 0; n < pairs; n++) {
        double I = (double)iq_pre[(size_t)(n << 1) + 0];
        double Q = (double)iq_pre[(size_t)(n << 1) + 1];
        acc += I * I + Q * Q;
    }
    if (acc == 0) {
        return;
    }
    double rms_pre = sqrt(acc / (double)pairs);
    float min_rms = (d->fm_agc_min_rms > 0.0f) ? d->fm_agc_min_rms : 0.06f;
    if (rms_pre < (double)min_rms) {
        return;
    }
    float target = (d->fm_agc_target_rms > 0.0f) ? d->fm_agc_target_rms : 0.30f;
    if (target < 0.05f) {
        target = 0.05f;
    }
    if (target > 2.5f) {
        target = 2.5f;
    }
    float alpha_up = (d->fm_agc_alpha_up > 0.0f) ? d->fm_agc_alpha_up : 0.25f;
    float alpha_dn = (d->fm_agc_alpha_down > 0.0f) ? d->fm_agc_alpha_down : 0.75f;
    if (alpha_up < 0.0f) {
        alpha_up = 0.0f;
    }
    if (alpha_up > 1.0f) {
        alpha_up = 1.0f;
    }
    if (alpha_dn < 0.0f) {
        alpha_dn = 0.0f;
    }
    if (alpha_dn > 1.0f) {
        alpha_dn = 1.0f;
    }
    const float kEps = 1e-6f;

    float rms = (float)d->fm_agc_ema_rms;
    if (rms <= 0.0f) {
        rms = target; /* seed estimator near target */
    }
    float rms2 = rms * rms;

    float* out = d->lowpassed;
    float last_g = 1.0f;
    for (int n = 0; n < pairs; n++) {
        float I = (float)out[(size_t)(n << 1)];
        float Q = (float)out[(size_t)(n << 1) + 1];
        float mag2 = I * I + Q * Q;

        /* Faster update when we need to back off gain; slower when ramping up. */
        float alpha = (mag2 > rms2) ? alpha_dn : alpha_up;
        if (alpha < 0.0f) {
            alpha = 0.0f;
        }
        if (alpha > 1.0f) {
            alpha = 1.0f;
        }
        rms2 = (1.0f - alpha) * rms2 + alpha * mag2;
        if (rms2 < 0.0f) {
            rms2 = 0.0f;
        }
        rms = sqrtf(rms2);

        float g = target / (rms + kEps);
        if (g > 8.0f) {
            g = 8.0f;
        }
        if (g < 0.125f) {
            g = 0.125f;
        }
        last_g = g;

        out[(size_t)(n << 1)] = I * g;
        out[(size_t)(n << 1) + 1] = Q * g;
    }
    d->fm_agc_gain = last_g;
    d->fm_agc_ema_rms = (double)rms;
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

/* Optional constant-envelope limiter: per-sample normalization around target magnitude */
static inline void
fm_constant_envelope_limiter(struct demod_state* d) {
    if (!d || !d->fm_limiter_enable || d->cqpsk_enable || !d->lowpassed || d->lp_len < 2) {
        return;
    }
    float target = (d->fm_agc_target_rms > 0.0f) ? d->fm_agc_target_rms : 0.30f;
    if (target < 0.05f) {
        target = 0.05f;
    }
    if (target > 2.5f) {
        target = 2.5f;
    }
    const float t2 = target * target;
    const float lo2 = t2 * 0.25f; /* 0.5^2 */
    const float hi2 = t2 * 4.0f;  /* 2.0^2 */
    float* iq = d->lowpassed;
    const int pairs = d->lp_len >> 1;
    for (int n = 0; n < pairs; n++) {
        float I = iq[(size_t)(n << 1) + 0];
        float Q = iq[(size_t)(n << 1) + 1];
        float m2 = I * I + Q * Q;
        if (m2 <= 0.0f) {
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
        float yI = (float)((double)I * g);
        float yQ = (float)((double)Q * g);
        iq[(size_t)(n << 1) + 0] = yI;
        iq[(size_t)(n << 1) + 1] = yQ;
    }
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
    d->ted_state.mu = d->ted_mu;

    ted_config_t cfg = {d->ted_enabled, d->ted_force, d->ted_gain, d->ted_sps};

    gardner_timing_adjust(&cfg, &d->ted_state, d->lowpassed, &d->lp_len, d->timing_buf);

    /* Sync back to demod_state */
    d->ted_mu = d->ted_state.mu;
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
            float* src = d->lowpassed;
            float* dst = d->hb_workbuf;
            for (i = 0; i < ds_p; i++) {
                /* Stage-aware HB selection: heavier early, light later */
                const float* taps = hb_q15_taps;
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
                generic_fir(d->lowpassed, d->lp_len, cic_9_tables[ds_p], d->droop_i_hist);
                generic_fir(d->lowpassed + 1, d->lp_len - 1, cic_9_tables[ds_p], d->droop_q_hist);
            }
        }
    } else {
        low_pass(d);
    }
    /* Bound channel noise when running at higher Fs (24 kHz default) */
    channel_lpf_apply(d);
    /* Reset per-block CQPSK rotation marker; set inside CQPSK branch when FLL applies rotation. */
    d->cqpsk_fll_rot_applied = 0;
    /* Branch early by mode to simplify ordering. */
    if (d->cqpsk_enable) {
        /* OP25 CQPSK chain (gardner_costas_cc signal flow):
           channel LPF (above) -> RMS AGC -> band-edge FLL -> Gardner -> [Costas+diff combined] -> arg/rescale -> slicer

           Key insight from OP25's gardner_costas_cc:
           - NCO rotation, differential decoding, and phase error detection happen
             in a SINGLE per-sample loop with immediate feedback
           - Each sample sees the loop correction from the previous sample
           - This is implemented in cqpsk_costas_diff_and_update()
        */
        cqpsk_rms_agc(d);
        /* Band-edge FLL (always active when enabled, OP25 style).
           Requires integer SPS; skip band-edge if non-integer. */
        if (d->fll_enabled && d->cqpsk_acq_fll_enable && d->ted_sps >= 2 && d->sps_is_integer) {
            /* Sync from demod_state into modular FLL state so that any
               external tweaks to fll_freq/phase (e.g., spectrum-assisted
               correction) are honored. */
            float prev_freq = d->fll_state.freq;
            d->fll_state.freq = d->fll_freq;
            d->fll_state.phase = d->fll_phase;
            d->fll_state.prev_r = d->fll_prev_r;
            d->fll_state.prev_j = d->fll_prev_j;
            if (prev_freq != d->fll_freq) {
                d->fll_state.integrator = d->fll_freq;
            }

            fll_config_t cfg;
            cfg.enabled = 1;
            /* OP25 default gains converted to native float (rad/sample units) */
            cfg.alpha = (d->fll_alpha > 0.0f) ? d->fll_alpha : 0.0046f;          /* ~150/32768 */
            cfg.beta = (d->fll_beta > 0.0f) ? d->fll_beta : 0.00046f;            /* ~15/32768 */
            cfg.deadband = (d->fll_deadband > 0.0f) ? d->fll_deadband : 0.0086f; /* ~45/16384 * pi */
            cfg.slew_max = (d->fll_slew_max > 0.0f) ? d->fll_slew_max : 0.012f;  /* ~64/32768 * 2pi */

            /* Band-edge FLL (OP25 settings): updates loop and rotates in-place. */
            fll_update_error_qpsk(&cfg, &d->fll_state, d->lowpassed, d->lp_len, d->ted_sps);
            d->cqpsk_fll_rot_applied = 1;

            /* Sync back minimal state */
            d->fll_freq = d->fll_state.freq;
            d->fll_phase = d->fll_state.phase;
            d->fll_prev_r = d->fll_state.prev_r;
            d->fll_prev_j = d->fll_state.prev_j;

            /* Simple lock detector: when |freq| stays small for several blocks, mark locked */
            float fmag = fabsf(d->fll_state.freq);
            const float kLockFreqThr = 0.002f; /* small residual in rad/sample */
            const int kLockBlocks = 6;         /* consecutive blocks */
            if (fmag <= kLockFreqThr) {
                d->cqpsk_acq_quiet_runs++;
            } else {
                d->cqpsk_acq_quiet_runs = 0;
            }
            d->cqpsk_acq_fll_locked = (d->cqpsk_acq_quiet_runs >= kLockBlocks);
        } else {
            d->cqpsk_acq_quiet_runs = 0;
            d->cqpsk_acq_fll_locked = 0;
        }
        /* Timing error correction after FLL (Gardner), before Costas+diff.
           Requires integer SPS; auto-skipped if non-integer. */
        if (d->ted_enabled && d->ted_sps >= 2 && d->sps_is_integer
            && (d->mode_demod != &dsd_fm_demod || d->ted_force)) {
            gardner_timing_adjust(d);
        }
        /* OP25-style combined Costas + differential decode with per-sample feedback.
           This performs NCO rotation -> diff decode -> phase error -> loop update
           in a single pass, ensuring each sample sees the correction from previous samples.
           Output is differential phasors ready for phase extraction.
           Skip during unit tests that use raw_demod. */
        if (d->mode_demod != &raw_demod) {
            cqpsk_costas_diff_and_update(d);
        } else {
            /* For raw_demod tests, just do differential decoding without Costas */
            cqpsk_diff_phasor(d);
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
            float ar_f = (float)ar_q15 * (1.0f / 32768.0f);
            float ai_f = (float)ai_q15 * (1.0f / 32768.0f);
            float* out = d->lowpassed;
            for (int n = 0; n < N; n++) {
                float I = out[(size_t)(n << 1) + 0];
                float Q = out[(size_t)(n << 1) + 1];
                float tI = ar_f * I + ai_f * Q;
                float tQ = -ar_f * Q + ai_f * I;
                float yI = I - tI;
                float yQ = Q - tQ;
                out[(size_t)(n << 1) + 0] = yI;
                out[(size_t)(n << 1) + 1] = yQ;
            }
        }
    }
    /* Apply Gardner TED here only for non-CQPSK paths (e.g., C4FM) and avoid
       analog FM unless explicitly forced. Requires integer SPS. */
    if (d->ted_enabled && !d->cqpsk_enable && d->sps_is_integer && (d->mode_demod != &dsd_fm_demod || d->ted_force)) {
        gardner_timing_adjust(d);
    }
    /* Power squelch (sqrt-free): compare pair power mean (I^2+Q^2) against a threshold.
	   Threshold is specified as per-component mean power on normalized float samples.
	   Since block_mean estimates E[I^2+Q^2], the equivalent threshold in this
	   domain is 2 * squelch_level. Samples are decimated by `squelch_decim_stride`;
	   an EMA smooths block power. The sampling phase advances by lp_len % stride
	   per block to cover all offsets. */
    if (d->squelch_level > 0.0f) {
        /* Decimated block power estimate (no DC correction; EMA smooths) */
        int stride = (d->squelch_decim_stride > 0) ? d->squelch_decim_stride : 16;
        int phase = d->squelch_decim_phase;
        double p = 0.0;
        int count = 0;
        /* Ensure even I/Q alignment and accumulate pair power I^2+Q^2 */
        int start = phase & ~1;
        for (int j = start; j + 1 < d->lp_len; j += stride) {
            double ir = (double)d->lowpassed[j];
            double jq = (double)d->lowpassed[j + 1];
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
            double block_mean = p / (double)count; /* mean of I^2+Q^2 per complex sample */
            if (d->squelch_running_power == 0.0) {
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
                double delta = (block_mean - d->squelch_running_power);
                d->squelch_running_power += delta / (double)(1 << shift);
            }
        }
        /* Convert per-component mean power threshold -> pair domain */
        double thr_pair = 2.0 * (double)d->squelch_level;
        if (d->squelch_running_power < thr_pair) {
            d->squelch_hits++;
            for (i = 0; i < d->lp_len; i++) {
                d->lowpassed[i] = 0.0f;
            }
            d->squelch_gate_open = 0;
        } else {
            d->squelch_hits = 0;
            d->squelch_gate_open = 1;
        }
    }
    /*
     * For CQPSK, produce a single real stream of differential phase symbols
     * (arg(z_n), where z_n was differenced pre-Costas) to feed the legacy
     * symbol sampler instead of FM discriminating. For other paths, use the
     * configured demodulator.
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
                memcpy(d->result, d->timing_buf, (size_t)out_n * sizeof(float));
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
            float alpha_f = (float)a;
            if (alpha_f < 0.0f) {
                alpha_f = 0.0f;
            }
            if (alpha_f > 1.0f) {
                alpha_f = 1.0f;
            }
            float y = (d->result_len > 0) ? d->result[0] : 0.0f;
            for (int k = 0; k < d->result_len; k++) {
                float x = d->result[k];
                float dlt = x - y;
                y += dlt * alpha_f;
                d->result[k] = y;
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
        float env = d->squelch_env;
        float target = d->squelch_gate_open ? 1.0f : 0.0f;
        float alpha = d->squelch_gate_open ? (d->squelch_env_attack > 0.0f ? d->squelch_env_attack : 0.125f)
                                           : (d->squelch_env_release > 0.0f ? d->squelch_env_release : 0.03125f);
        float err = target - env;
        env += alpha * err;
        if (env < 0.0f) {
            env = 0.0f;
        }
        if (env > 1.0f) {
            env = 1.0f;
        }
        d->squelch_env = env;
        /* Multiply audio by envelope */
        float* res = d->result;
        for (int k = 0; k < d->result_len; k++) {
            res[k] = res[k] * env;
        }
    }
}
