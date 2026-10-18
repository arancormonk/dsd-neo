// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "simd_stats_internal.h"

#if defined(__clang_analyzer__)
simd_stats_extrema
simd_stats_extrema_sse2(const float* samples, size_t count) {
    return simd_stats_extrema_scalar(samples, count);
}

simd_stats_iq_abs
simd_stats_iq_abs_sse2(const float* iq, size_t pairs) {
    return simd_stats_iq_abs_scalar(iq, pairs);
}
#else

#include <cmath>
#include <emmintrin.h>
#include <mmintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <xmmintrin.h>

// This translation unit is built only for x86-64, where SSE2 is guaranteed.
// NOLINTBEGIN(portability-simd-intrinsics)
static inline __m128i
nonfinite_mask(__m128 x) {
    const __m128i exponent = _mm_set1_epi32(0x7f800000);
    return _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(x), exponent), exponent);
}

static inline __m128i
extrema_special_mask(__m128 x) {
    const __m128i negative_zero = _mm_set1_epi32(INT32_MIN);
    return _mm_or_si128(nonfinite_mask(x), _mm_cmpeq_epi32(_mm_castps_si128(x), negative_zero));
}

simd_stats_extrema
simd_stats_extrema_sse2(const float* samples, size_t count) {
    if (count < 8) {
        return simd_stats_extrema_scalar(samples, count);
    }
    const __m128 first = _mm_loadu_ps(samples);
    const __m128 second = _mm_loadu_ps(samples + 4);
    __m128 lo1 = _mm_min_ps(first, second);
    __m128 lo2 = _mm_max_ps(first, second);
    __m128 hi1 = lo2;
    __m128 hi2 = lo1;
    __m128i special = _mm_or_si128(extrema_special_mask(first), extrema_special_mask(second));
    size_t k = 8;
    for (; count - k >= 4; k += 4) {
        const __m128 x = _mm_loadu_ps(samples + k);
        special = _mm_or_si128(special, extrema_special_mask(x));
        lo2 = _mm_min_ps(lo2, _mm_max_ps(lo1, x));
        lo1 = _mm_min_ps(lo1, x);
        hi2 = _mm_max_ps(hi2, _mm_min_ps(hi1, x));
        hi1 = _mm_max_ps(hi1, x);
    }
    if (_mm_movemask_ps(_mm_castsi128_ps(special)) != 0) {
        return simd_stats_extrema_scalar(samples, count);
    }
    float lows1[4], lows2[4], highs1[4], highs2[4];
    _mm_storeu_ps(lows1, lo1);
    _mm_storeu_ps(lows2, lo2);
    _mm_storeu_ps(highs1, hi1);
    _mm_storeu_ps(highs2, hi2);
    simd_stats_extrema r = simd_stats_merge_extrema(lows1, lows2, highs1, highs2, 4);
    for (; k < count; ++k) {
        if (simd_stats_extrema_needs_scalar(samples[k])) {
            return simd_stats_extrema_scalar(samples, count);
        }
        simd_stats_track_low(&r, samples[k]);
        simd_stats_track_high(&r, samples[k]);
    }
    return r;
}

simd_stats_iq_abs
simd_stats_iq_abs_sse2(const float* iq, size_t pairs) {
    __m128d sum0 = _mm_setzero_pd();
    __m128d sum1 = _mm_setzero_pd();
    __m128 peak = _mm_setzero_ps();
    __m128i special = _mm_setzero_si128();
    const __m128 abs_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
    size_t k = 0;
    for (; pairs - k >= 2; k += 2) {
        const __m128 x = _mm_and_ps(_mm_loadu_ps(iq + 2 * k), abs_mask);
        special = _mm_or_si128(special, nonfinite_mask(x));
        peak = _mm_max_ps(peak, x);
        sum0 = _mm_add_pd(sum0, _mm_cvtps_pd(x));
        sum1 = _mm_add_pd(sum1, _mm_cvtps_pd(_mm_movehl_ps(x, x)));
    }
    if (_mm_movemask_ps(_mm_castsi128_ps(special)) != 0) {
        return simd_stats_iq_abs_scalar(iq, pairs);
    }
    double sums[2];
    float peaks[4];
    _mm_storeu_pd(sums, _mm_add_pd(sum0, sum1));
    _mm_storeu_ps(peaks, peak);
    simd_stats_iq_abs r = {0.0, 0.0f};
    for (double sum : sums) {
        r.sum_abs += sum;
    }
    for (float value : peaks) {
        if (value > r.max_abs) {
            r.max_abs = value;
        }
    }
    for (; k < pairs; ++k) {
        const float ai = std::fabs(iq[2 * k]);
        const float aq = std::fabs(iq[2 * k + 1]);
        const float value = (ai > aq) ? ai : aq;
        if (value > r.max_abs) {
            r.max_abs = value;
        }
        r.sum_abs += (double)ai + (double)aq;
    }
    return r;
}

// NOLINTEND(portability-simd-intrinsics)
#endif
