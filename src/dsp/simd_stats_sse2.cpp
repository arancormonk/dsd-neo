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

void
simd_stats_rank5_sse2(const float* samples, size_t count, float* lmn, float* lmx) {
    simd_stats_rank5_scalar(samples, count, lmn, lmx);
}
#else

#include <cfloat>
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

/* Per-lane sorted insertion: l ascending keeps the five smallest, h descending
 * the five largest. Live is the number of ranks already filled; deeper ranks
 * still hold their seeds, which every finite value passes. Without NaN, -0 or
 * infinities, min/max ties return identical bits. */
template <int Live>
static inline void
rank5_insert(__m128* l, __m128* h, __m128 lo, __m128 hi) {
    for (int k = 0; k < Live && k < 4; ++k) {
        const __m128 low = _mm_min_ps(l[k], lo);
        lo = _mm_max_ps(l[k], lo);
        l[k] = low;
        const __m128 high = _mm_max_ps(h[k], hi);
        hi = _mm_min_ps(h[k], hi);
        h[k] = high;
    }
    if (Live < 5) {
        /* The clamp only keeps the unused Live == 5 instantiation in bounds. */
        l[Live < 5 ? Live : 4] = lo;
        h[Live < 5 ? Live : 4] = hi;
    } else {
        l[4] = _mm_min_ps(l[4], lo);
        h[4] = _mm_max_ps(h[4], hi);
    }
}

static inline void
rank5_insert_live(__m128* l, __m128* h, __m128 lo, __m128 hi, int live) {
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
rank5_merge(const __m128* a, const __m128* b, __m128* c) {
    c[0] = _mm_min_ps(a[0], b[0]);
    c[1] = _mm_min_ps(_mm_min_ps(a[1], b[1]), _mm_max_ps(a[0], b[0]));
    c[2] = _mm_min_ps(_mm_min_ps(a[2], b[2]), _mm_min_ps(_mm_max_ps(a[0], b[1]), _mm_max_ps(a[1], b[0])));
    c[3] = _mm_min_ps(_mm_min_ps(_mm_min_ps(a[3], b[3]), _mm_max_ps(a[2], b[0])),
                      _mm_min_ps(_mm_max_ps(a[0], b[2]), _mm_max_ps(a[1], b[1])));
    c[4] = _mm_min_ps(_mm_min_ps(_mm_min_ps(a[4], b[4]), _mm_max_ps(a[3], b[0])),
                      _mm_min_ps(_mm_min_ps(_mm_max_ps(a[0], b[3]), _mm_max_ps(a[1], b[2])), _mm_max_ps(a[2], b[1])));
}

/* Gather lane 0 (low ranks) and lane 2 (negated high ranks) of each rank. */
static inline void
rank5_store(const __m128* r, float* lmn, float* lmx) {
    const __m128 sign = _mm_set1_ps(-0.0f);
    const __m128 r01 = _mm_unpacklo_ps(r[0], r[1]);
    const __m128 r23 = _mm_unpacklo_ps(r[2], r[3]);
    const __m128 n01 = _mm_unpackhi_ps(r[0], r[1]);
    const __m128 n23 = _mm_unpackhi_ps(r[2], r[3]);
    _mm_storeu_ps(lmn, _mm_movelh_ps(r01, r23));
    _mm_store_ss(lmn + 4, r[4]);
    _mm_storeu_ps(lmx, _mm_xor_ps(_mm_movelh_ps(n01, n23), sign));
    _mm_store_ss(lmx + 4, _mm_xor_ps(_mm_movehl_ps(r[4], r[4]), sign));
}

void
simd_stats_rank5_sse2(const float* samples, size_t count, float* lmn, float* lmx) {
    /* Below six samples the merge costs more than scalar insertion saves. */
    if (count < 6) {
        simd_stats_rank5_scalar(samples, count, lmn, lmx);
        return;
    }
    const __m128 seed_low = _mm_set1_ps(FLT_MAX);
    const __m128 seed_high = _mm_set1_ps(-FLT_MAX);
    __m128 l[5] = {seed_low, seed_low, seed_low, seed_low, seed_low};
    __m128 h[5] = {seed_high, seed_high, seed_high, seed_high, seed_high};
    __m128 x = _mm_loadu_ps(samples);
    __m128i special = extrema_special_mask(x);
    l[0] = x;
    h[0] = x;
    size_t k = 4;
    int live = 1;
    for (; live < 5 && count - k >= 4; k += 4, ++live) {
        x = _mm_loadu_ps(samples + k);
        special = _mm_or_si128(special, extrema_special_mask(x));
        rank5_insert_live(l, h, x, x, live);
    }
    for (; count - k >= 4; k += 4) {
        x = _mm_loadu_ps(samples + k);
        special = _mm_or_si128(special, extrema_special_mask(x));
        rank5_insert<5>(l, h, x, x);
    }
    const size_t tail = count - k;
    if (tail > 0) {
        /* Reload the last four samples and seed the lanes already inserted. */
        x = _mm_loadu_ps(samples + count - 4);
        const __m128 fresh =
            _mm_castsi128_ps(_mm_cmpgt_epi32(_mm_setr_epi32(0, 1, 2, 3), _mm_set1_epi32((int)(3 - tail))));
        special = _mm_or_si128(special, extrema_special_mask(x));
        const __m128 data = _mm_and_ps(fresh, x);
        rank5_insert_live(l, h, _mm_or_ps(data, _mm_andnot_ps(fresh, seed_low)),
                          _mm_or_ps(data, _mm_andnot_ps(fresh, seed_high)), live);
    }
    if (_mm_movemask_ps(_mm_castsi128_ps(special)) != 0) {
        simd_stats_rank5_scalar(samples, count, lmn, lmx);
        return;
    }
    /* Negate the largest values so one merge network reduces both lists:
     * lanes 0-1 carry low candidates and lanes 2-3 negated high candidates. */
    const __m128 sign = _mm_set1_ps(-0.0f);
    __m128 a[5], b[5], c[5];
    for (int i = 0; i < 5; ++i) {
        const __m128 n = _mm_xor_ps(h[i], sign);
        a[i] = _mm_movelh_ps(l[i], n);
        b[i] = _mm_movehl_ps(n, l[i]);
    }
    rank5_merge(a, b, c);
    for (int i = 0; i < 5; ++i) {
        a[i] = _mm_shuffle_ps(c[i], c[i], _MM_SHUFFLE(2, 3, 0, 1));
    }
    rank5_merge(c, a, b);
    rank5_store(b, lmn, lmx);
}

// NOLINTEND(portability-simd-intrinsics)
#endif
