// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "simd_kmeans_internal.h"

#if defined(__clang_analyzer__)
void
simd_kmeans4_avx2(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    simd_kmeans4_scalar(vals, count, centers, with_noise, out);
}

void
simd_kmeans2_avx2(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    simd_kmeans2_scalar(vals, count, centers, with_noise, out);
}
#else

#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <xmmintrin.h>

// This translation unit is selected only after the AVX2 CPU/OS probe succeeds.
// NOLINTBEGIN(portability-simd-intrinsics)
static inline double
hsum_pd(__m256d v) {
    double lanes[4];
    _mm256_storeu_pd(lanes, v);
    return (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
}

static inline size_t
hsum_epi64(__m256i v) {
    int64_t lanes[4];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(lanes), v);
    return (size_t)(lanes[0] + lanes[1] + lanes[2] + lanes[3]);
}

static inline __m256d
abs_diff(__m256d x, __m256d c) {
    return _mm256_and_pd(_mm256_sub_pd(x, c), _mm256_castsi256_pd(_mm256_set1_epi64x(INT64_MAX)));
}

static inline void
accumulate(__m256d is, __m256d x, __m256d* sum, __m256i* cnt) {
    *sum = _mm256_add_pd(*sum, _mm256_and_pd(is, x));
    *cnt = _mm256_sub_epi64(*cnt, _mm256_castpd_si256(is)); /* an all-ones lane is -1 */
}

static inline void
finish(const __m256d* sum, const __m256i* cnt, int levels, bool with_noise, __m256d noise, simd_kmeans_pass* out) {
    for (int j = 0; j < levels; j++) {
        out->sum[j] = hsum_pd(sum[j]);
        out->count[j] = hsum_epi64(cnt[j]);
    }
    if (with_noise) {
        out->noise = hsum_pd(noise);
    }
}

/* won_j: center j is strictly nearer than every lower center, as in simd_kmeans_nearest(); the highest center
 * that won is the nearest one. */
template <bool WithNoise>
static void
kmeans4_avx2(const float* vals, size_t count, const double* centers, simd_kmeans_pass* out) {
    *out = simd_kmeans_pass{};
    const __m256d c0 = _mm256_set1_pd(centers[0]);
    const __m256d c1 = _mm256_set1_pd(centers[1]);
    const __m256d c2 = _mm256_set1_pd(centers[2]);
    const __m256d c3 = _mm256_set1_pd(centers[3]);
    __m256d sum[4] = {_mm256_setzero_pd(), _mm256_setzero_pd(), _mm256_setzero_pd(), _mm256_setzero_pd()};
    __m256i cnt[4] = {_mm256_setzero_si256(), _mm256_setzero_si256(), _mm256_setzero_si256(), _mm256_setzero_si256()};
    __m256d noise = _mm256_setzero_pd();
    size_t i = 0;
    for (; count - i >= 4; i += 4) {
        const __m256d x = _mm256_cvtps_pd(_mm_loadu_ps(vals + i));
        const __m256d d0 = abs_diff(x, c0);
        const __m256d d1 = abs_diff(x, c1);
        const __m256d won1 = _mm256_cmp_pd(d1, d0, _CMP_LT_OQ);
        const __m256d best1 = _mm256_blendv_pd(d0, d1, won1);
        const __m256d d2 = abs_diff(x, c2);
        const __m256d won2 = _mm256_cmp_pd(d2, best1, _CMP_LT_OQ);
        const __m256d best2 = _mm256_blendv_pd(best1, d2, won2);
        const __m256d d3 = abs_diff(x, c3);
        const __m256d won3 = _mm256_cmp_pd(d3, best2, _CMP_LT_OQ);
        const __m256d above2 = won3;
        const __m256d is2 = _mm256_andnot_pd(above2, won2);
        const __m256d above1 = _mm256_or_pd(above2, won2);
        const __m256d is1 = _mm256_andnot_pd(above1, won1);
        const __m256d above0 = _mm256_or_pd(above1, won1);
        sum[0] = _mm256_add_pd(sum[0], _mm256_andnot_pd(above0, x)); /* center 0 counted from the total below */
        accumulate(is1, x, &sum[1], &cnt[1]);
        accumulate(is2, x, &sum[2], &cnt[2]);
        accumulate(won3, x, &sum[3], &cnt[3]);
        if (WithNoise) {
            __m256d center = _mm256_blendv_pd(c0, c1, won1);
            center = _mm256_blendv_pd(center, c2, won2);
            center = _mm256_blendv_pd(center, c3, won3);
            const __m256d e = _mm256_sub_pd(x, center);
            noise = _mm256_add_pd(noise, _mm256_mul_pd(e, e));
        }
    }
    cnt[0] = _mm256_sub_epi64(_mm256_set1_epi64x((int64_t)(i / 4)),
                              _mm256_add_epi64(_mm256_add_epi64(cnt[1], cnt[2]), cnt[3]));
    finish(sum, cnt, 4, WithNoise, noise, out);
    simd_kmeans_tail<4>(vals, i, count, centers, WithNoise, out);
}

template <bool WithNoise>
static void
kmeans2_avx2(const float* vals, size_t count, const double* centers, simd_kmeans_pass* out) {
    *out = simd_kmeans_pass{};
    const __m256d c0 = _mm256_set1_pd(centers[0]);
    const __m256d c1 = _mm256_set1_pd(centers[1]);
    __m256d sum[2] = {_mm256_setzero_pd(), _mm256_setzero_pd()};
    __m256i cnt[2] = {_mm256_setzero_si256(), _mm256_setzero_si256()};
    __m256d noise = _mm256_setzero_pd();
    size_t i = 0;
    for (; count - i >= 4; i += 4) {
        const __m256d x = _mm256_cvtps_pd(_mm_loadu_ps(vals + i));
        const __m256d won1 = _mm256_cmp_pd(abs_diff(x, c1), abs_diff(x, c0), _CMP_LT_OQ);
        sum[0] = _mm256_add_pd(sum[0], _mm256_andnot_pd(won1, x)); /* center 0 counted from the total below */
        accumulate(won1, x, &sum[1], &cnt[1]);
        if (WithNoise) {
            const __m256d e = _mm256_sub_pd(x, _mm256_blendv_pd(c0, c1, won1));
            noise = _mm256_add_pd(noise, _mm256_mul_pd(e, e));
        }
    }
    cnt[0] = _mm256_sub_epi64(_mm256_set1_epi64x((int64_t)(i / 4)), cnt[1]);
    finish(sum, cnt, 2, WithNoise, noise, out);
    simd_kmeans_tail<2>(vals, i, count, centers, WithNoise, out);
}

void
simd_kmeans4_avx2(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    if (with_noise) {
        kmeans4_avx2<true>(vals, count, centers, out);
    } else {
        kmeans4_avx2<false>(vals, count, centers, out);
    }
}

void
simd_kmeans2_avx2(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    if (with_noise) {
        kmeans2_avx2<true>(vals, count, centers, out);
    } else {
        kmeans2_avx2<false>(vals, count, centers, out);
    }
}

// NOLINTEND(portability-simd-intrinsics)
#endif
