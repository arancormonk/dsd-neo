// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief AVX2+FMA implementations of SIMD FIR filter functions.
 *
 * Compiled with -mavx2 -mfma (GCC/Clang) or /arch:AVX2 (MSVC).
 * Processes 8 floats at a time using 256-bit YMM registers.
 * Uses FMA (fused multiply-add) for efficiency.
 */

#if defined(__clang_analyzer__)
extern "C" void
simd_fir_complex_apply_avx2(const float* in, int in_len, float* out, float* hist_i, float* hist_q, const float* taps,
                            int taps_len) {
    (void)in;
    (void)in_len;
    (void)out;
    (void)hist_i;
    (void)hist_q;
    (void)taps;
    (void)taps_len;
}

extern "C" int
simd_hb_decim2_complex_avx2(const float* in, int in_len, float* out, float* hist_i, float* hist_q, const float* taps,
                            int taps_len) {
    (void)in;
    (void)in_len;
    (void)out;
    (void)hist_i;
    (void)hist_q;
    (void)taps;
    (void)taps_len;
    return 0;
}

extern "C" int
simd_hb_decim2_real_avx2(const float* in, int in_len, float* out, float* hist, const float* taps, int taps_len) {
    (void)in;
    (void)in_len;
    (void)out;
    (void)hist;
    (void)taps;
    (void)taps_len;
    return 0;
}
#else

#include <cstring>
#include <immintrin.h>
#include <vector>
#include "dsd-neo/core/safe_api.h"

// NOLINTBEGIN(portability-simd-intrinsics)

static thread_local std::vector<float> tls_scratch_iq;
static thread_local std::vector<float> tls_scratch_real;

namespace {

struct ComplexIq {
    float i;
    float q;
};

static inline void
copy_iq_history(float* scratch, const float* hist_i, const float* hist_q, int hist_len) {
    for (int k = 0; k < hist_len; k++) {
        const size_t kk = (size_t)k;
        scratch[2 * kk] = hist_i[k];
        scratch[2 * kk + 1] = hist_q[k];
    }
}

static inline void
copy_iq_input(float* scratch, int hist_len, const float* in, int sample_count) {
    DSD_MEMCPY(scratch + (size_t)hist_len * 2, in, (size_t)sample_count * 2 * sizeof(float));
}

static inline void
pad_iq_tail(float* scratch, int hist_len, int sample_count, int pad, float last_i, float last_q) {
    for (int k = 0; k < pad; k++) {
        const size_t kk = (size_t)hist_len + (size_t)sample_count + (size_t)k;
        scratch[2 * kk] = last_i;
        scratch[2 * kk + 1] = last_q;
    }
}

static inline ComplexIq
load_iq(const float* scratch, int idx) {
    const size_t ii = (size_t)idx;
    return {scratch[2 * ii], scratch[2 * ii + 1]};
}

static inline __m256
pack_iq4(const float* scratch, int i0, int i1, int i2, int i3) {
    const ComplexIq s0 = load_iq(scratch, i0);
    const ComplexIq s1 = load_iq(scratch, i1);
    const ComplexIq s2 = load_iq(scratch, i2);
    const ComplexIq s3 = load_iq(scratch, i3);
    return _mm256_set_ps(s3.q, s3.i, s2.q, s2.i, s1.q, s1.i, s0.q, s0.i);
}

static inline __m256
fir_complex_accumulate4(const float* scratch, const float* taps, int hist_len, int center, int n) {
    __m256 acc = _mm256_setzero_ps();
    const __m256 tap_c = _mm256_set1_ps(taps[center]);
    const size_t center_offset = ((size_t)(hist_len + n)) << 1;
    const __m256 center_val = _mm256_loadu_ps(scratch + center_offset);
    acc = _mm256_fmadd_ps(tap_c, center_val, acc);

    for (int k = 0; k < center; k++) {
        const float ce = taps[k];
        if (ce == 0.0f) {
            continue;
        }
        const int d = center - k;
        const size_t minus_offset = ((size_t)(hist_len + n - d)) << 1;
        const size_t plus_offset = ((size_t)(hist_len + n + d)) << 1;
        const __m256 sum_m = _mm256_loadu_ps(scratch + minus_offset);
        const __m256 sum_p = _mm256_loadu_ps(scratch + plus_offset);
        const __m256 sum = _mm256_add_ps(sum_m, sum_p);
        acc = _mm256_fmadd_ps(_mm256_set1_ps(ce), sum, acc);
    }
    return acc;
}

static inline ComplexIq
fir_complex_accumulate_scalar(const float* scratch, const float* taps, int center, int center_idx) {
    ComplexIq center_sample = load_iq(scratch, center_idx);
    ComplexIq acc = {taps[center] * center_sample.i, taps[center] * center_sample.q};

    for (int k = 0; k < center; k++) {
        const float ce = taps[k];
        if (ce == 0.0f) {
            continue;
        }
        const int d = center - k;
        const ComplexIq minus = load_iq(scratch, center_idx - d);
        const ComplexIq plus = load_iq(scratch, center_idx + d);
        acc.i += ce * (minus.i + plus.i);
        acc.q += ce * (minus.q + plus.q);
    }
    return acc;
}

static inline __m256
hb_complex_accumulate4(const float* scratch, const float* taps, int left_len, int center, int n) {
    const int ci0 = left_len + (n << 1);
    const int ci1 = left_len + ((n + 1) << 1);
    const int ci2 = left_len + ((n + 2) << 1);
    const int ci3 = left_len + ((n + 3) << 1);

    __m256 acc = _mm256_setzero_ps();
    acc = _mm256_fmadd_ps(_mm256_set1_ps(taps[center]), pack_iq4(scratch, ci0, ci1, ci2, ci3), acc);

    for (int e = 0; e < center; e += 2) {
        const float ce = taps[e];
        if (ce == 0.0f) {
            continue;
        }
        const int d = center - e;
        const __m256 sum_m = pack_iq4(scratch, ci0 - d, ci1 - d, ci2 - d, ci3 - d);
        const __m256 sum_p = pack_iq4(scratch, ci0 + d, ci1 + d, ci2 + d, ci3 + d);
        const __m256 sum = _mm256_add_ps(sum_m, sum_p);
        acc = _mm256_fmadd_ps(_mm256_set1_ps(ce), sum, acc);
    }
    return acc;
}

static inline ComplexIq
hb_complex_accumulate_scalar(const float* scratch, const float* taps, int left_len, int center, int n) {
    const int center_idx = left_len + (n << 1);
    const ComplexIq center_sample = load_iq(scratch, center_idx);
    ComplexIq acc = {taps[center] * center_sample.i, taps[center] * center_sample.q};

    for (int e = 0; e < center; e += 2) {
        const float ce = taps[e];
        if (ce == 0.0f) {
            continue;
        }
        const int d = center - e;
        const ComplexIq minus = load_iq(scratch, center_idx - d);
        const ComplexIq plus = load_iq(scratch, center_idx + d);
        acc.i += ce * (minus.i + plus.i);
        acc.q += ce * (minus.q + plus.q);
    }
    return acc;
}

static inline void
update_iq_history(const float* in, int sample_count, float* hist_i, float* hist_q, int hist_len) {
    if (sample_count >= hist_len) {
        const int start = sample_count - hist_len;
        for (int k = 0; k < hist_len; k++) {
            const int rel = start + k;
            hist_i[k] = in[rel << 1];
            hist_q[k] = in[(rel << 1) + 1];
        }
        return;
    }

    const int keep = hist_len - sample_count;
    DSD_MEMMOVE(hist_i, hist_i + sample_count, (size_t)keep * sizeof(float));
    DSD_MEMMOVE(hist_q, hist_q + sample_count, (size_t)keep * sizeof(float));
    for (int k = 0; k < sample_count; k++) {
        hist_i[keep + k] = in[k << 1];
        hist_q[keep + k] = in[(k << 1) + 1];
    }
}

} // namespace

/**
 * AVX2+FMA complex symmetric FIR filter (no decimation).
 * Processes 4 complex samples (8 floats) at a time.
 * Uses pre-concatenated scratch buffer to eliminate branching in hot loop.
 */
extern "C" void
simd_fir_complex_apply_avx2(const float* in, int in_len, float* out, float* hist_i, float* hist_q, const float* taps,
                            int taps_len) {
    if (taps_len < 3 || (taps_len & 1) == 0 || in_len < 2) {
        return;
    }

    const int sample_count = in_len >> 1; /* complex samples */
    const int hist_len = taps_len - 1;
    const int center = (taps_len - 1) >> 1;
    const int total_len = hist_len + sample_count;
    const int pad = center + 8;                    /* for n+7+d lookups */
    const int scratch_len = (total_len + pad) * 2; /* *2 for complex (I, Q) */

    /* Resize thread-local buffer if needed (amortized O(1)) */
    if (tls_scratch_iq.size() < (size_t)scratch_len) {
        tls_scratch_iq.resize((size_t)scratch_len);
    }
    float* scratch = tls_scratch_iq.data();

    copy_iq_history(scratch, hist_i, hist_q, hist_len);
    copy_iq_input(scratch, hist_len, in, sample_count);
    const float last_i = in[(sample_count - 1) << 1];
    const float last_q = in[((sample_count - 1) << 1) + 1];
    pad_iq_tail(scratch, hist_len, sample_count, pad, last_i, last_q);

    /* Process 8 complex samples at a time (16 floats). */
    int n = 0;
    for (; n + 7 < sample_count; n += 8) {
        const __m256 acc0 = fir_complex_accumulate4(scratch, taps, hist_len, center, n);
        const __m256 acc1 = fir_complex_accumulate4(scratch, taps, hist_len, center, n + 4);
        _mm256_storeu_ps(out + (n << 1), acc0);
        _mm256_storeu_ps(out + ((n + 4) << 1), acc1);
    }

    /* Process 4 complex samples at a time (8 floats) */
    for (; n + 3 < sample_count; n += 4) {
        const __m256 acc = fir_complex_accumulate4(scratch, taps, hist_len, center, n);
        _mm256_storeu_ps(out + (n << 1), acc);
    }

    /* Scalar epilogue for remaining samples */
    for (; n < sample_count; n++) {
        const int center_idx = hist_len + n;
        const ComplexIq acc = fir_complex_accumulate_scalar(scratch, taps, center, center_idx);
        out[n << 1] = acc.i;
        out[(n << 1) + 1] = acc.q;
    }

    update_iq_history(in, sample_count, hist_i, hist_q, hist_len);
    _mm256_zeroupper(); /* Avoid AVX-SSE transition penalty */
}

/**
 * AVX2+FMA complex half-band decimator by 2.
 * Processes 4 output samples (8 floats) at a time.
 * Uses pre-concatenated scratch buffer to eliminate branching in hot loop.
 */
extern "C" int
simd_hb_decim2_complex_avx2(const float* in, int in_len, float* out, float* hist_i, float* hist_q, const float* taps,
                            int taps_len) {
    if (taps_len < 3 || (taps_len & 1) == 0) {
        return 0;
    }

    const int ch_len = in_len >> 1;
    if (ch_len <= 0) {
        return 0;
    }
    const int out_ch_len = ch_len >> 1;

    const int center = (taps_len - 1) >> 1;
    const int left_len = taps_len - 1;
    const int total_len = left_len + ch_len;
    const int pad = center + 1;                    /* stride-2 centers, 4-output vectorization */
    const int scratch_len = (total_len + pad) * 2; /* *2 for complex (I, Q) */

    /* Resize thread-local buffer if needed (amortized O(1)) */
    if (tls_scratch_iq.size() < (size_t)scratch_len) {
        tls_scratch_iq.resize((size_t)scratch_len);
    }
    float* scratch = tls_scratch_iq.data();

    copy_iq_history(scratch, hist_i, hist_q, left_len);
    copy_iq_input(scratch, left_len, in, ch_len);
    const float last_i = in[in_len - 2];
    const float last_q = in[in_len - 1];
    pad_iq_tail(scratch, left_len, ch_len, pad, last_i, last_q);

    /* Process 4 output samples at a time */
    int n = 0;
    for (; n + 3 < out_ch_len; n += 4) {
        const __m256 acc = hb_complex_accumulate4(scratch, taps, left_len, center, n);
        _mm256_storeu_ps(out + (n << 1), acc);
    }

    /* Scalar epilogue */
    for (; n < out_ch_len; n++) {
        const ComplexIq acc = hb_complex_accumulate_scalar(scratch, taps, left_len, center, n);
        out[n << 1] = acc.i;
        out[(n << 1) + 1] = acc.q;
    }

    update_iq_history(in, ch_len, hist_i, hist_q, left_len);
    _mm256_zeroupper();
    return out_ch_len << 1;
}

/**
 * AVX2+FMA real half-band decimator by 2.
 * Processes 8 output samples at a time.
 * Uses pre-concatenated scratch buffer to eliminate branching in hot loop.
 */
extern "C" int
simd_hb_decim2_real_avx2(const float* in, int in_len, float* out, float* hist, const float* taps, int taps_len) {
    if (taps_len < 3 || (taps_len & 1) == 0) {
        return 0;
    }
    if (in_len <= 0) {
        return 0;
    }

    const int hist_len = taps_len - 1;
    const int center = (taps_len - 1) >> 1;
    int out_len = in_len >> 1;

    const int total_len = hist_len + in_len;
    const int pad = center + 1; /* stride-2 centers, 8-output vectorization */
    const int scratch_len = total_len + pad;

    /* Resize thread-local buffer if needed (amortized O(1)) */
    if (tls_scratch_real.size() < (size_t)scratch_len) {
        tls_scratch_real.resize((size_t)scratch_len);
    }
    float* scratch = tls_scratch_real.data();

    /* Copy history */
    DSD_MEMCPY(scratch, hist, (size_t)hist_len * sizeof(float));

    /* Copy input */
    DSD_MEMCPY(scratch + hist_len, in, (size_t)in_len * sizeof(float));

    /* Tail padding to preserve last sample behavior */
    float last = in[in_len - 1];
    for (int k = 0; k < pad; k++) {
        scratch[hist_len + in_len + k] = last;
    }

    /* Branch-free sample access (index is always valid due to padding) */
    auto get_sample = [&](int idx) -> float { return scratch[idx]; };

    /* Process 8 output samples at a time */
    int n = 0;
    for (; n + 7 < out_len; n += 8) {
        __m256 acc = _mm256_setzero_ps();

        float cc = taps[center];
        __m256 tap_c = _mm256_set1_ps(cc);

        /* Center indices for 8 outputs */
        int ci0 = hist_len + (n << 1);
        int ci1 = hist_len + ((n + 1) << 1);
        int ci2 = hist_len + ((n + 2) << 1);
        int ci3 = hist_len + ((n + 3) << 1);
        int ci4 = hist_len + ((n + 4) << 1);
        int ci5 = hist_len + ((n + 5) << 1);
        int ci6 = hist_len + ((n + 6) << 1);
        int ci7 = hist_len + ((n + 7) << 1);

        __m256 center_val = _mm256_set_ps(get_sample(ci7), get_sample(ci6), get_sample(ci5), get_sample(ci4),
                                          get_sample(ci3), get_sample(ci2), get_sample(ci1), get_sample(ci0));
        acc = _mm256_fmadd_ps(tap_c, center_val, acc);

        /* Half-band: only even indices */
        for (int e = 0; e < center; e += 2) {
            float ce = taps[e];
            if (ce == 0.0f) {
                continue;
            }
            int d = center - e;
            __m256 tap_e = _mm256_set1_ps(ce);

            __m256 sum_m =
                _mm256_set_ps(get_sample(ci7 - d), get_sample(ci6 - d), get_sample(ci5 - d), get_sample(ci4 - d),
                              get_sample(ci3 - d), get_sample(ci2 - d), get_sample(ci1 - d), get_sample(ci0 - d));
            __m256 sum_p =
                _mm256_set_ps(get_sample(ci7 + d), get_sample(ci6 + d), get_sample(ci5 + d), get_sample(ci4 + d),
                              get_sample(ci3 + d), get_sample(ci2 + d), get_sample(ci1 + d), get_sample(ci0 + d));
            __m256 sum = _mm256_add_ps(sum_m, sum_p);
            acc = _mm256_fmadd_ps(tap_e, sum, acc);
        }

        _mm256_storeu_ps(out + n, acc);
    }

    /* Scalar epilogue */
    for (; n < out_len; n++) {
        int center_idx = hist_len + (n << 1);
        float acc = 0.0f;

        acc += taps[center] * get_sample(center_idx);

        for (int e = 0; e < center; e += 2) {
            float ce = taps[e];
            if (ce == 0.0f) {
                continue;
            }
            int d = center - e;
            acc += ce * (get_sample(center_idx - d) + get_sample(center_idx + d));
        }

        out[n] = acc;
    }

    /* Update history: read from original `in` buffer, not scratch */
    if (in_len >= hist_len) {
        DSD_MEMCPY(hist, in + (in_len - hist_len), (size_t)hist_len * sizeof(float));
    } else {
        int need = hist_len - in_len;
        DSD_MEMMOVE(hist, hist + in_len, (size_t)need * sizeof(float));
        DSD_MEMCPY(hist + need, in, (size_t)in_len * sizeof(float));
    }

    _mm256_zeroupper();
    return out_len;
}

// NOLINTEND(portability-simd-intrinsics)

#endif
