// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_DSP_SIMD_FIR_INTERNAL_H_
#define DSD_NEO_SRC_DSP_SIMD_FIR_INTERNAL_H_

#include <cstddef>
#include <cstdint>
#include "dsd-neo/core/safe_api.h"

int simd_fir_complex_apply_scalar(const float* in, int n_in, float* out, int out_cap, float* hist_i, float* hist_q,
                                  int hist_len, int* pending, const float* taps, int taps_len);
int simd_hb_decim2_complex_scalar(const float* in, int in_len, float* out, float* hist_i, float* hist_q, int* pending,
                                  const float* taps, int taps_len);
int simd_hb_decim2_real_scalar(const float* in, int in_len, float* out, float* hist, int* pending, const float* taps,
                               int taps_len);

/*
 * The streaming complex FIR's call contract (simd_fir_complex_apply()), shared by every backend: the outputs a call
 * makes, max(0, pending + n_in - c) with c = (taps_len - 1) / 2, or -1 for a call it refuses without touching any
 * state. It refuses even or short taps, a history shorter than taps_len - 1, a negative pending, a pending whose
 * windows would reach before the history (pending + c > hist_len), a negative n_in, and an out_cap below the outputs.
 */
/* The filter a call runs: odd taps, at least 3, and a history of at least taps_len - 1 samples. */
static inline int
simd_fir_complex_filter_ok(const float* hist_i, const float* hist_q, int hist_len, const float* taps, int taps_len) {
    return hist_i && hist_q && taps && taps_len >= 3 && (taps_len & 1) != 0 && hist_len >= taps_len - 1;
}

static inline int
simd_fir_complex_outputs(const float* in, int n_in, const float* out, int out_cap, const float* hist_i,
                         const float* hist_q, int hist_len, const int* pending, const float* taps, int taps_len) {
    if (!simd_fir_complex_filter_ok(hist_i, hist_q, hist_len, taps, taps_len) || !pending || n_in < 0 || out_cap < 0
        || (n_in > 0 && !in)) {
        return -1;
    }
    const int center = (taps_len - 1) >> 1;
    const int held = *pending;
    if (held < 0 || held > hist_len - center) {
        return -1;
    }
    const int64_t made = (int64_t)held + (int64_t)n_in - (int64_t)center;
    if (made <= 0) {
        return 0;
    }
    if (made > (int64_t)out_cap || !out) {
        return -1;
    }
    return (int)made;
}

/* Keep the newest @p hist_len complex samples of [history | in], right-aligned. */
static inline void
simd_fir_complex_push_history(const float* in, int n_in, float* hist_i, float* hist_q, int hist_len) {
    if (n_in >= hist_len) {
        const size_t start = (size_t)n_in - (size_t)hist_len;
        for (int k = 0; k < hist_len; k++) {
            const size_t rel = start + (size_t)k;
            hist_i[k] = in[2 * rel];
            hist_q[k] = in[2 * rel + 1];
        }
        return;
    }
    if (n_in <= 0) {
        return;
    }
    const int keep = hist_len - n_in;
    DSD_MEMMOVE(hist_i, hist_i + n_in, (size_t)keep * sizeof(float));
    DSD_MEMMOVE(hist_q, hist_q + n_in, (size_t)keep * sizeof(float));
    for (int k = 0; k < n_in; k++) {
        const size_t rel = (size_t)k;
        hist_i[keep + k] = in[2 * rel];
        hist_q[keep + k] = in[2 * rel + 1];
    }
}

/*
 * The half-band decimators' call contract (simd_hb_decim2_complex(), simd_hb_decim2_real()), shared by every backend.
 * With c = (taps_len - 1) / 2 and lt = pending + n_in, a call makes (lt - c + 1) / 2 outputs once lt reaches c + 1 and
 * none before, and leaves pending lt - 2 x outputs. Output k is centred at relative index centre_rel = 2k - pending of
 * the block (negative indices reach into the history, which holds taps_len - 1 samples), so its window spans
 * centre_rel - c .. centre_rel + c: never before the history (pending <= c) and never past the block's last sample
 * (the count). Every backend's prefix condition, vector base and load-footprint guard derives from centre_rel.
 */
/* The decimator a call runs and the state it carries: odd taps, at least 3, a history and pending within 0..c. */
static inline int
simd_hb_decim2_state_ok(const float* hist_a, const float* hist_b, const int* pending, const float* taps, int taps_len) {
    if (!hist_a || !hist_b || !pending || !taps || taps_len < 3 || (taps_len & 1) == 0) {
        return 0;
    }
    return *pending >= 0 && *pending <= ((taps_len - 1) >> 1);
}

/* The outputs a call of @p n_in samples makes, or -1 for a call refused with no state touched. */
static inline int
simd_hb_decim2_outputs(const float* in, int n_in, const float* out, const float* hist_a, const float* hist_b,
                       const int* pending, const float* taps, int taps_len) {
    if (!simd_hb_decim2_state_ok(hist_a, hist_b, pending, taps, taps_len) || n_in < 0 || (n_in > 0 && !in)) {
        return -1;
    }
    const int64_t center = (int64_t)((taps_len - 1) >> 1);
    const int64_t lt = (int64_t)*pending + (int64_t)n_in;
    const int made = lt >= center + 1 ? (int)((lt - center + 1) / 2) : 0;
    if (made > 0 && !out) {
        return -1;
    }
    return made;
}

/* The complex decimator's count: in_len counts floats, so it must be even. */
static inline int
simd_hb_decim2_complex_outputs(const float* in, int in_len, const float* out, const float* hist_i, const float* hist_q,
                               const int* pending, const float* taps, int taps_len) {
    if (in_len < 0 || (in_len & 1) != 0) {
        return -1;
    }
    return simd_hb_decim2_outputs(in, in_len >> 1, out, hist_i, hist_q, pending, taps, taps_len);
}

static inline int
simd_hb_decim2_real_outputs(const float* in, int in_len, const float* out, const float* hist, const int* pending,
                            const float* taps, int taps_len) {
    return simd_hb_decim2_outputs(in, in_len, out, hist, hist, pending, taps, taps_len);
}

/* Keep the newest @p hist_len real samples of [history | in], right-aligned. */
static inline void
simd_hb_real_push_history(const float* in, int n_in, float* hist, int hist_len) {
    if (n_in >= hist_len) {
        DSD_MEMCPY(hist, in + (n_in - hist_len), (size_t)hist_len * sizeof(float));
        return;
    }
    if (n_in <= 0) {
        return;
    }
    const int keep = hist_len - n_in;
    DSD_MEMMOVE(hist, hist + n_in, (size_t)keep * sizeof(float));
    DSD_MEMCPY(hist + keep, in, (size_t)n_in * sizeof(float));
}

#endif /* DSD_NEO_SRC_DSP_SIMD_FIR_INTERNAL_H_ */
