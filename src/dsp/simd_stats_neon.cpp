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
#else

#include <arm_neon.h>
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

// NOLINTEND(portability-simd-intrinsics)
#endif
