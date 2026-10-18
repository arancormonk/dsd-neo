// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "simd_stats_internal.h"

#if defined(__clang_analyzer__)
simd_stats_extrema
simd_stats_extrema_neon(const float* samples, size_t count) {
    return simd_stats_extrema_scalar(samples, count);
}

simd_stats_iq_abs
simd_stats_iq_abs_neon(const float* iq, size_t pairs) {
    return simd_stats_iq_abs_scalar(iq, pairs);
}

void
simd_stats_rank5_neon(const float* samples, size_t count, float* lmn, float* lmx) {
    simd_stats_rank5_scalar(samples, count, lmn, lmx);
}
#else

#include <arm_neon.h>
#include <cfloat>
#include <cmath>
#include <stddef.h>
#include <stdint.h>

// This translation unit is built only for ARM64, where NEON is guaranteed.
// NOLINTBEGIN(portability-simd-intrinsics)
static inline uint32x4_t
nonfinite_mask(float32x4_t x) {
    const uint32x4_t exponent = vdupq_n_u32(UINT32_C(0x7f800000));
    return vceqq_u32(vandq_u32(vreinterpretq_u32_f32(x), exponent), exponent);
}

static inline uint32x4_t
extrema_special_mask(float32x4_t x) {
    return vorrq_u32(nonfinite_mask(x), vceqq_u32(vreinterpretq_u32_f32(x), vdupq_n_u32(UINT32_C(0x80000000))));
}

simd_stats_extrema
simd_stats_extrema_neon(const float* samples, size_t count) {
    if (count < 8) {
        return simd_stats_extrema_scalar(samples, count);
    }
    const float32x4_t first = vld1q_f32(samples);
    const float32x4_t second = vld1q_f32(samples + 4);
    float32x4_t lo1 = vminq_f32(first, second);
    float32x4_t lo2 = vmaxq_f32(first, second);
    float32x4_t hi1 = lo2;
    float32x4_t hi2 = lo1;
    uint32x4_t special = vorrq_u32(extrema_special_mask(first), extrema_special_mask(second));
    size_t k = 8;
    for (; count - k >= 4; k += 4) {
        const float32x4_t x = vld1q_f32(samples + k);
        special = vorrq_u32(special, extrema_special_mask(x));
        lo2 = vminq_f32(lo2, vmaxq_f32(lo1, x));
        lo1 = vminq_f32(lo1, x);
        hi2 = vmaxq_f32(hi2, vminq_f32(hi1, x));
        hi1 = vmaxq_f32(hi1, x);
    }
    if (vmaxvq_u32(special) != 0) {
        return simd_stats_extrema_scalar(samples, count);
    }
    float lows1[4], lows2[4], highs1[4], highs2[4];
    vst1q_f32(lows1, lo1);
    vst1q_f32(lows2, lo2);
    vst1q_f32(highs1, hi1);
    vst1q_f32(highs2, hi2);
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
simd_stats_iq_abs_neon(const float* iq, size_t pairs) {
    if (pairs < 8) {
        return simd_stats_iq_abs_scalar(iq, pairs);
    }
    float64x2_t sums_low[4] = {};
    float64x2_t sums_high[4] = {};
    float32x4_t peak = vdupq_n_f32(0.0f);
    uint32x4_t special = vdupq_n_u32(0);
    size_t k = 0;
    /* Independent sums hide conversion/add latency without changing any filter
     * state. Every load lies within the eight complete I/Q pairs. */
    for (; pairs - k >= 8; k += 8) {
        for (size_t block = 0; block < 4; ++block) {
            const float32x4_t x = vabsq_f32(vld1q_f32(iq + 2 * k + 4 * block));
            special = vorrq_u32(special, nonfinite_mask(x));
            peak = vmaxq_f32(peak, x);
            sums_low[block] = vaddq_f64(sums_low[block], vcvt_f64_f32(vget_low_f32(x)));
            sums_high[block] = vaddq_f64(sums_high[block], vcvt_f64_f32(vget_high_f32(x)));
        }
    }
    if (vmaxvq_u32(special) != 0) {
        return simd_stats_iq_abs_scalar(iq, pairs);
    }
    float64x2_t sum = vdupq_n_f64(0.0);
    for (size_t block = 0; block < 4; ++block) {
        sum = vaddq_f64(sum, vaddq_f64(sums_low[block], sums_high[block]));
    }
    simd_stats_iq_abs r = {vaddvq_f64(sum), vmaxvq_f32(peak)};
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
rank5_insert(float32x4_t* l, float32x4_t* h, float32x4_t lo, float32x4_t hi) {
    for (int k = 0; k < Live && k < 4; ++k) {
        const float32x4_t low = vminq_f32(l[k], lo);
        lo = vmaxq_f32(l[k], lo);
        l[k] = low;
        const float32x4_t high = vmaxq_f32(h[k], hi);
        hi = vminq_f32(h[k], hi);
        h[k] = high;
    }
    if (Live < 5) {
        /* The clamp only keeps the unused Live == 5 instantiation in bounds. */
        l[Live < 5 ? Live : 4] = lo;
        h[Live < 5 ? Live : 4] = hi;
    } else {
        l[4] = vminq_f32(l[4], lo);
        h[4] = vmaxq_f32(h[4], hi);
    }
}

static inline void
rank5_insert_live(float32x4_t* l, float32x4_t* h, float32x4_t lo, float32x4_t hi, int live) {
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
rank5_merge(const float32x4_t* a, const float32x4_t* b, float32x4_t* c) {
    c[0] = vminq_f32(a[0], b[0]);
    c[1] = vminq_f32(vminq_f32(a[1], b[1]), vmaxq_f32(a[0], b[0]));
    c[2] = vminq_f32(vminq_f32(a[2], b[2]), vminq_f32(vmaxq_f32(a[0], b[1]), vmaxq_f32(a[1], b[0])));
    c[3] = vminq_f32(vminq_f32(vminq_f32(a[3], b[3]), vmaxq_f32(a[2], b[0])),
                     vminq_f32(vmaxq_f32(a[0], b[2]), vmaxq_f32(a[1], b[1])));
    c[4] = vminq_f32(vminq_f32(vminq_f32(a[4], b[4]), vmaxq_f32(a[3], b[0])),
                     vminq_f32(vminq_f32(vmaxq_f32(a[0], b[3]), vmaxq_f32(a[1], b[2])), vmaxq_f32(a[2], b[1])));
}

void
simd_stats_rank5_neon(const float* samples, size_t count, float* lmn, float* lmx) {
    /* Below six samples the merge costs more than scalar insertion saves. */
    if (count < 6) {
        simd_stats_rank5_scalar(samples, count, lmn, lmx);
        return;
    }
    const float32x4_t seed_low = vdupq_n_f32(FLT_MAX);
    const float32x4_t seed_high = vdupq_n_f32(-FLT_MAX);
    float32x4_t l[5] = {seed_low, seed_low, seed_low, seed_low, seed_low};
    float32x4_t h[5] = {seed_high, seed_high, seed_high, seed_high, seed_high};
    float32x4_t x = vld1q_f32(samples);
    uint32x4_t special = extrema_special_mask(x);
    l[0] = x;
    h[0] = x;
    size_t k = 4;
    int live = 1;
    for (; live < 5 && count - k >= 4; k += 4, ++live) {
        x = vld1q_f32(samples + k);
        special = vorrq_u32(special, extrema_special_mask(x));
        rank5_insert_live(l, h, x, x, live);
    }
    for (; count - k >= 4; k += 4) {
        x = vld1q_f32(samples + k);
        special = vorrq_u32(special, extrema_special_mask(x));
        rank5_insert<5>(l, h, x, x);
    }
    const size_t tail = count - k;
    if (tail > 0) {
        /* Reload the last four samples and seed the lanes already inserted. */
        static const uint32_t lane_index[4] = {0, 1, 2, 3};
        x = vld1q_f32(samples + count - 4);
        const uint32x4_t fresh = vcgtq_u32(vld1q_u32(lane_index), vdupq_n_u32((uint32_t)(3 - tail)));
        special = vorrq_u32(special, extrema_special_mask(x));
        rank5_insert_live(l, h, vbslq_f32(fresh, x, seed_low), vbslq_f32(fresh, x, seed_high), live);
    }
    if (vmaxvq_u32(special) != 0) {
        simd_stats_rank5_scalar(samples, count, lmn, lmx);
        return;
    }
    /* Negate the largest values so one merge network reduces both lists:
     * lanes 0-1 carry low candidates and lanes 2-3 negated high candidates. */
    float32x4_t a[5], b[5], c[5];
    for (int i = 0; i < 5; ++i) {
        const float32x4_t n = vnegq_f32(h[i]);
        a[i] = vcombine_f32(vget_low_f32(l[i]), vget_low_f32(n));
        b[i] = vcombine_f32(vget_high_f32(l[i]), vget_high_f32(n));
    }
    rank5_merge(a, b, c);
    for (int i = 0; i < 5; ++i) {
        a[i] = vrev64q_f32(c[i]);
    }
    rank5_merge(c, a, b);
    for (int i = 0; i < 5; ++i) {
        lmn[i] = vgetq_lane_f32(b[i], 0);
        lmx[i] = -vgetq_lane_f32(b[i], 2);
    }
}

// NOLINTEND(portability-simd-intrinsics)
#endif
