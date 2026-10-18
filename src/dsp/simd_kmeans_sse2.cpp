// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "simd_kmeans_internal.h"

#if defined(__clang_analyzer__)
void
simd_kmeans4_sse2(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    simd_kmeans4_scalar(vals, count, centers, with_noise, out);
}

void
simd_kmeans2_sse2(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    simd_kmeans2_scalar(vals, count, centers, with_noise, out);
}
#else

#include <emmintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <xmmintrin.h>

// This translation unit is built only for x86-64, where SSE2 is guaranteed.
// NOLINTBEGIN(portability-simd-intrinsics)
static inline double
hsum_pd(__m128d v) {
    double lanes[2];
    _mm_storeu_pd(lanes, v);
    return lanes[0] + lanes[1];
}

static inline size_t
hsum_epi64(__m128i v) {
    int64_t lanes[2];
    _mm_storeu_si128(reinterpret_cast<__m128i*>(lanes), v);
    return (size_t)(lanes[0] + lanes[1]);
}

static inline __m128d
select_pd(__m128d mask, __m128d if_set, __m128d if_clear) {
    return _mm_or_pd(_mm_and_pd(mask, if_set), _mm_andnot_pd(mask, if_clear));
}

static inline __m128d
abs_diff(__m128d x, __m128d c) {
    return _mm_and_pd(_mm_sub_pd(x, c), _mm_castsi128_pd(_mm_set1_epi64x(INT64_MAX)));
}

static inline void
accumulate(__m128d is, __m128d x, __m128d* sum, __m128i* cnt) {
    *sum = _mm_add_pd(*sum, _mm_and_pd(is, x));
    *cnt = _mm_sub_epi64(*cnt, _mm_castpd_si128(is)); /* an all-ones lane is -1 */
}

struct kmeans_sse2_acc {
    __m128d c[4];
    __m128d sum[4];
    __m128i cnt[4]; /* center 0 is counted from the total */
    __m128d noise;
};

/* won_j: center j is strictly nearer than every lower center, as in simd_kmeans_nearest(); the highest center
 * that won is the nearest one. */
template <bool WithNoise>
static inline void
step4(__m128d x, kmeans_sse2_acc* a) {
    const __m128d d0 = abs_diff(x, a->c[0]);
    const __m128d d1 = abs_diff(x, a->c[1]);
    const __m128d won1 = _mm_cmplt_pd(d1, d0);
    const __m128d best1 = select_pd(won1, d1, d0);
    const __m128d d2 = abs_diff(x, a->c[2]);
    const __m128d won2 = _mm_cmplt_pd(d2, best1);
    const __m128d best2 = select_pd(won2, d2, best1);
    const __m128d d3 = abs_diff(x, a->c[3]);
    const __m128d won3 = _mm_cmplt_pd(d3, best2);
    const __m128d is2 = _mm_andnot_pd(won3, won2);
    const __m128d above1 = _mm_or_pd(won3, won2);
    const __m128d is1 = _mm_andnot_pd(above1, won1);
    const __m128d above0 = _mm_or_pd(above1, won1);
    a->sum[0] = _mm_add_pd(a->sum[0], _mm_andnot_pd(above0, x));
    accumulate(is1, x, &a->sum[1], &a->cnt[1]);
    accumulate(is2, x, &a->sum[2], &a->cnt[2]);
    accumulate(won3, x, &a->sum[3], &a->cnt[3]);
    if (WithNoise) {
        __m128d center = select_pd(won1, a->c[1], a->c[0]);
        center = select_pd(won2, a->c[2], center);
        center = select_pd(won3, a->c[3], center);
        const __m128d e = _mm_sub_pd(x, center);
        a->noise = _mm_add_pd(a->noise, _mm_mul_pd(e, e));
    }
}

template <bool WithNoise>
static inline void
step2(__m128d x, kmeans_sse2_acc* a) {
    const __m128d won1 = _mm_cmplt_pd(abs_diff(x, a->c[1]), abs_diff(x, a->c[0]));
    a->sum[0] = _mm_add_pd(a->sum[0], _mm_andnot_pd(won1, x));
    accumulate(won1, x, &a->sum[1], &a->cnt[1]);
    if (WithNoise) {
        const __m128d e = _mm_sub_pd(x, select_pd(won1, a->c[1], a->c[0]));
        a->noise = _mm_add_pd(a->noise, _mm_mul_pd(e, e));
    }
}

template <int Levels, bool WithNoise>
static void
kmeans_sse2(const float* vals, size_t count, const double* centers, simd_kmeans_pass* out) {
    *out = simd_kmeans_pass{};
    kmeans_sse2_acc acc;
    for (int j = 0; j < 4; j++) {
        acc.c[j] = _mm_set1_pd(centers[j < Levels ? j : 0]);
        acc.sum[j] = _mm_setzero_pd();
        acc.cnt[j] = _mm_setzero_si128();
    }
    acc.noise = _mm_setzero_pd();
    size_t i = 0;
    /* Whole four-float loads only, so the last load never reads past vals[count - 1]. */
    for (; count - i >= 4; i += 4) {
        const __m128 x4 = _mm_loadu_ps(vals + i);
        if (Levels == 4) {
            step4<WithNoise>(_mm_cvtps_pd(x4), &acc);
            step4<WithNoise>(_mm_cvtps_pd(_mm_movehl_ps(x4, x4)), &acc);
        } else {
            step2<WithNoise>(_mm_cvtps_pd(x4), &acc);
            step2<WithNoise>(_mm_cvtps_pd(_mm_movehl_ps(x4, x4)), &acc);
        }
    }
    size_t others = 0;
    for (int j = 1; j < Levels; j++) {
        out->sum[j] = hsum_pd(acc.sum[j]);
        out->count[j] = hsum_epi64(acc.cnt[j]);
        others += out->count[j];
    }
    out->sum[0] = hsum_pd(acc.sum[0]);
    out->count[0] = i - others;
    if (WithNoise) {
        out->noise = hsum_pd(acc.noise);
    }
    simd_kmeans_tail<Levels>(vals, i, count, centers, WithNoise, out);
}

void
simd_kmeans4_sse2(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    if (with_noise) {
        kmeans_sse2<4, true>(vals, count, centers, out);
    } else {
        kmeans_sse2<4, false>(vals, count, centers, out);
    }
}

void
simd_kmeans2_sse2(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    if (with_noise) {
        kmeans_sse2<2, true>(vals, count, centers, out);
    } else {
        kmeans_sse2<2, false>(vals, count, centers, out);
    }
}

// NOLINTEND(portability-simd-intrinsics)
#endif
