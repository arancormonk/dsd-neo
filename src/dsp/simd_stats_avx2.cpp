// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "simd_stats_internal.h"

#if defined(__clang_analyzer__)
simd_stats_extrema
simd_stats_extrema_avx2(const float* samples, size_t count) {
    return simd_stats_extrema_scalar(samples, count);
}

simd_stats_iq_abs
simd_stats_iq_abs_avx2(const float* iq, size_t pairs) {
    return simd_stats_iq_abs_scalar(iq, pairs);
}
#else

#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>

// This translation unit is selected only after the AVX2 CPU/OS probe succeeds.
// NOLINTBEGIN(portability-simd-intrinsics)
static inline __m256i
nonfinite_mask(__m256 x) {
    const __m256i exponent = _mm256_set1_epi32(0x7f800000);
    return _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_castps_si256(x), exponent), exponent);
}

/* std::fabs here would be an inline instantiation built with AVX flags, which the linker may keep for the
   baseline callers too; clearing the sign bit is the same result and stays local. */
static inline float
abs_bits(float x) {
    return _mm_cvtss_f32(_mm_and_ps(_mm_set_ss(x), _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff))));
}

static inline __m256i
extrema_special_mask(__m256 x) {
    const __m256i negative_zero = _mm256_set1_epi32(INT32_MIN);
    return _mm256_or_si256(nonfinite_mask(x), _mm256_cmpeq_epi32(_mm256_castps_si256(x), negative_zero));
}

simd_stats_extrema
simd_stats_extrema_avx2(const float* samples, size_t count) {
    if (count < 16) {
        return simd_stats_extrema_scalar(samples, count);
    }
    const __m256 first = _mm256_loadu_ps(samples);
    const __m256 second = _mm256_loadu_ps(samples + 8);
    __m256 lo1 = _mm256_min_ps(first, second);
    __m256 lo2 = _mm256_max_ps(first, second);
    __m256 hi1 = lo2;
    __m256 hi2 = lo1;
    __m256i special = _mm256_or_si256(extrema_special_mask(first), extrema_special_mask(second));
    size_t k = 16;
    for (; count - k >= 8; k += 8) {
        const __m256 x = _mm256_loadu_ps(samples + k);
        special = _mm256_or_si256(special, extrema_special_mask(x));
        lo2 = _mm256_min_ps(lo2, _mm256_max_ps(lo1, x));
        lo1 = _mm256_min_ps(lo1, x);
        hi2 = _mm256_max_ps(hi2, _mm256_min_ps(hi1, x));
        hi1 = _mm256_max_ps(hi1, x);
    }
    if (_mm256_movemask_ps(_mm256_castsi256_ps(special)) != 0) {
        return simd_stats_extrema_scalar(samples, count);
    }
    float lows1[8], lows2[8], highs1[8], highs2[8];
    _mm256_storeu_ps(lows1, lo1);
    _mm256_storeu_ps(lows2, lo2);
    _mm256_storeu_ps(highs1, hi1);
    _mm256_storeu_ps(highs2, hi2);
    simd_stats_extrema r = simd_stats_merge_extrema(lows1, lows2, highs1, highs2, 8);
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
simd_stats_iq_abs_avx2(const float* iq, size_t pairs) {
    __m256d sum0 = _mm256_setzero_pd();
    __m256d sum1 = _mm256_setzero_pd();
    __m256 peak = _mm256_setzero_ps();
    __m256i special = _mm256_setzero_si256();
    const __m256 abs_mask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
    size_t k = 0;
    for (; pairs - k >= 4; k += 4) {
        const __m256 x = _mm256_and_ps(_mm256_loadu_ps(iq + 2 * k), abs_mask);
        special = _mm256_or_si256(special, nonfinite_mask(x));
        peak = _mm256_max_ps(peak, x);
        sum0 = _mm256_add_pd(sum0, _mm256_cvtps_pd(_mm256_castps256_ps128(x)));
        sum1 = _mm256_add_pd(sum1, _mm256_cvtps_pd(_mm256_extractf128_ps(x, 1)));
    }
    if (_mm256_movemask_ps(_mm256_castsi256_ps(special)) != 0) {
        return simd_stats_iq_abs_scalar(iq, pairs);
    }
    double sums[4];
    float peaks[8];
    _mm256_storeu_pd(sums, _mm256_add_pd(sum0, sum1));
    _mm256_storeu_ps(peaks, peak);
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
        const float ai = abs_bits(iq[2 * k]);
        const float aq = abs_bits(iq[2 * k + 1]);
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
