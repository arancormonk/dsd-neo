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

void
simd_stats_rank5_avx2(const float* samples, size_t count, float* lmn, float* lmx) {
    simd_stats_rank5_scalar(samples, count, lmn, lmx);
}
#else

#include <cfloat>
#include <emmintrin.h>
#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <xmmintrin.h>

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

/* Per-lane sorted insertion: l ascending keeps the five smallest, h descending
 * the five largest. Live is the number of ranks already filled; deeper ranks
 * still hold their seeds, which every finite value passes. Without NaN, -0 or
 * infinities, min/max ties return identical bits. */
template <int Live>
static inline void
rank5_insert(__m256* l, __m256* h, __m256 lo, __m256 hi) {
    for (int k = 0; k < Live && k < 4; ++k) {
        const __m256 low = _mm256_min_ps(l[k], lo);
        lo = _mm256_max_ps(l[k], lo);
        l[k] = low;
        const __m256 high = _mm256_max_ps(h[k], hi);
        hi = _mm256_min_ps(h[k], hi);
        h[k] = high;
    }
    if (Live < 5) {
        /* The clamp only keeps the unused Live == 5 instantiation in bounds. */
        l[Live < 5 ? Live : 4] = lo;
        h[Live < 5 ? Live : 4] = hi;
    } else {
        l[4] = _mm256_min_ps(l[4], lo);
        h[4] = _mm256_max_ps(h[4], hi);
    }
}

static inline void
rank5_insert_live(__m256* l, __m256* h, __m256 lo, __m256 hi, int live) {
    switch (live) {
        case 1: rank5_insert<1>(l, h, lo, hi); break;
        case 2: rank5_insert<2>(l, h, lo, hi); break;
        case 3: rank5_insert<3>(l, h, lo, hi); break;
        case 4: rank5_insert<4>(l, h, lo, hi); break;
        default: rank5_insert<5>(l, h, lo, hi); break;
    }
}

/* Five smallest of two ascending lists, ascending, per lane:
 * c[k] = min(a[k], b[k], max(a[i], b[k - 1 - i]) for i < k), reduced as a tree. */
static inline void
rank5_merge(const __m256* a, const __m256* b, __m256* c) {
    c[0] = _mm256_min_ps(a[0], b[0]);
    c[1] = _mm256_min_ps(_mm256_min_ps(a[1], b[1]), _mm256_max_ps(a[0], b[0]));
    c[2] =
        _mm256_min_ps(_mm256_min_ps(a[2], b[2]), _mm256_min_ps(_mm256_max_ps(a[0], b[1]), _mm256_max_ps(a[1], b[0])));
    c[3] = _mm256_min_ps(_mm256_min_ps(_mm256_min_ps(a[3], b[3]), _mm256_max_ps(a[2], b[0])),
                         _mm256_min_ps(_mm256_max_ps(a[0], b[2]), _mm256_max_ps(a[1], b[1])));
    c[4] = _mm256_min_ps(
        _mm256_min_ps(_mm256_min_ps(a[4], b[4]), _mm256_max_ps(a[3], b[0])),
        _mm256_min_ps(_mm256_min_ps(_mm256_max_ps(a[0], b[3]), _mm256_max_ps(a[1], b[2])), _mm256_max_ps(a[2], b[1])));
}

void
simd_stats_rank5_avx2(const float* samples, size_t count, float* lmn, float* lmx) {
    if (count < SIMD_STATS_RANK5_AVX2_MIN_COUNT) {
        simd_stats_rank5_sse2(samples, count, lmn, lmx);
        return;
    }
    const __m256 seed_low = _mm256_set1_ps(FLT_MAX);
    const __m256 seed_high = _mm256_set1_ps(-FLT_MAX);
    __m256 l[5] = {seed_low, seed_low, seed_low, seed_low, seed_low};
    __m256 h[5] = {seed_high, seed_high, seed_high, seed_high, seed_high};
    __m256 x = _mm256_loadu_ps(samples);
    __m256i special = extrema_special_mask(x);
    l[0] = x;
    h[0] = x;
    size_t k = 8;
    int live = 1;
    for (; live < 5 && count - k >= 8; k += 8, ++live) {
        x = _mm256_loadu_ps(samples + k);
        special = _mm256_or_si256(special, extrema_special_mask(x));
        rank5_insert_live(l, h, x, x, live);
    }
    for (; count - k >= 8; k += 8) {
        x = _mm256_loadu_ps(samples + k);
        special = _mm256_or_si256(special, extrema_special_mask(x));
        rank5_insert<5>(l, h, x, x);
    }
    const size_t tail = count - k;
    if (tail > 0) {
        /* Reload the last eight samples and seed the lanes already inserted. */
        x = _mm256_loadu_ps(samples + count - 8);
        const __m256 fresh = _mm256_castsi256_ps(
            _mm256_cmpgt_epi32(_mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7), _mm256_set1_epi32((int)(7 - tail))));
        special = _mm256_or_si256(special, extrema_special_mask(x));
        rank5_insert_live(l, h, _mm256_blendv_ps(seed_low, x, fresh), _mm256_blendv_ps(seed_high, x, fresh), live);
    }
    if (_mm256_movemask_ps(_mm256_castsi256_ps(special)) != 0) {
        simd_stats_rank5_scalar(samples, count, lmn, lmx);
        return;
    }
    /* Negate the largest values so one merge network reduces both lists:
     * lanes 0-3 carry low candidates and lanes 4-7 negated high candidates. */
    const __m256 sign = _mm256_set1_ps(-0.0f);
    __m256 a[5], b[5], c[5];
    for (int i = 0; i < 5; ++i) {
        const __m256 n = _mm256_xor_ps(h[i], sign);
        a[i] = _mm256_permute2f128_ps(l[i], n, 0x20);
        b[i] = _mm256_permute2f128_ps(l[i], n, 0x31);
    }
    rank5_merge(a, b, c);
    for (int i = 0; i < 5; ++i) {
        a[i] = _mm256_permute_ps(c[i], _MM_SHUFFLE(1, 0, 3, 2));
    }
    rank5_merge(c, a, b);
    for (int i = 0; i < 5; ++i) {
        a[i] = _mm256_permute_ps(b[i], _MM_SHUFFLE(2, 3, 0, 1));
    }
    rank5_merge(b, a, c);
    /* Gather lane 0 (low ranks) and lane 4 (negated high ranks) of each rank. */
    __m128 lo[5], hi[5];
    for (int i = 0; i < 5; ++i) {
        lo[i] = _mm256_castps256_ps128(c[i]);
        hi[i] = _mm256_extractf128_ps(c[i], 1);
    }
    const __m128 neg = _mm_set1_ps(-0.0f);
    _mm_storeu_ps(lmn, _mm_movelh_ps(_mm_unpacklo_ps(lo[0], lo[1]), _mm_unpacklo_ps(lo[2], lo[3])));
    _mm_store_ss(lmn + 4, lo[4]);
    _mm_storeu_ps(lmx, _mm_xor_ps(_mm_movelh_ps(_mm_unpacklo_ps(hi[0], hi[1]), _mm_unpacklo_ps(hi[2], hi[3])), neg));
    _mm_store_ss(lmx + 4, _mm_xor_ps(hi[4], neg));
}

// NOLINTEND(portability-simd-intrinsics)
#endif
