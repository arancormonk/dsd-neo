// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_DSP_SIMD_FIR_INTERNAL_H_
#define DSD_NEO_SRC_DSP_SIMD_FIR_INTERNAL_H_

#include <cstddef>
#include <cstdint>
#include "dsd-neo/core/safe_api.h"

int simd_fir_complex_apply_scalar(const float* in, int n_in, float* out, int out_cap, float* hist_i, float* hist_q,
                                  int hist_len, int* pending, const float* taps, int taps_len);
int simd_hb_decim2_complex_scalar(const float* in, int in_len, float* out, float* hist_i, float* hist_q,
                                  const float* taps, int taps_len);
int simd_hb_decim2_real_scalar(const float* in, int in_len, float* out, float* hist, const float* taps, int taps_len);

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

#endif /* DSD_NEO_SRC_DSP_SIMD_FIR_INTERNAL_H_ */
