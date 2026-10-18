// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_DSP_SIMD_STATS_INTERNAL_H
#define DSD_NEO_DSP_SIMD_STATS_INTERNAL_H

#include <dsd-neo/core/safe_api.h>
#include <stddef.h>
#include <stdint.h>

struct simd_stats_extrema {
    float min1;
    float min2;
    float max1;
    float max2;
};

struct simd_stats_iq_abs {
    double sum_abs;
    float max_abs;
};

static inline simd_stats_extrema
simd_stats_extrema_seed(float first, float second) {
    simd_stats_extrema r = {first, second, first, second};
    if (second < first) {
        r.min1 = second;
        r.min2 = first;
    }
    if (second > first) {
        r.max1 = second;
        r.max2 = first;
    }
    return r;
}

static inline void
simd_stats_track_low(simd_stats_extrema* r, float x) {
    if (x < r->min1) {
        r->min2 = r->min1;
        r->min1 = x;
    } else if (x < r->min2) {
        r->min2 = x;
    }
}

static inline void
simd_stats_track_high(simd_stats_extrema* r, float x) {
    if (x > r->max1) {
        r->max2 = r->max1;
        r->max1 = x;
    } else if (x > r->max2) {
        r->max2 = x;
    }
}

/* Vector min/max can choose a different signed zero or NaN on a tie. Those
 * uncommon inputs use the original ordered comparisons instead. */
static inline bool
simd_stats_extrema_needs_scalar(float x) {
    uint32_t bits = 0;
    DSD_MEMCPY(&bits, &x, sizeof bits);
    return (bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) || bits == UINT32_C(0x80000000);
}

static inline simd_stats_extrema
simd_stats_merge_extrema(const float* lo1, const float* lo2, const float* hi1, const float* hi2, size_t lanes) {
    simd_stats_extrema r = {lo1[0], lo2[0], hi1[0], hi2[0]};
    for (size_t k = 1; k < lanes; ++k) {
        simd_stats_track_low(&r, lo1[k]);
        simd_stats_track_low(&r, lo2[k]);
        simd_stats_track_high(&r, hi1[k]);
        simd_stats_track_high(&r, hi2[k]);
    }
    return r;
}

/* Backend contracts: extrema has at least two samples; I/Q has at least one
 * complete pair. Inputs are non-null and exactly sized, with arbitrary alignment. */
simd_stats_extrema simd_stats_extrema_scalar(const float* samples, size_t count);
simd_stats_iq_abs simd_stats_iq_abs_scalar(const float* iq, size_t pairs);
simd_stats_extrema simd_stats_extrema_sse2(const float* samples, size_t count);
simd_stats_iq_abs simd_stats_iq_abs_sse2(const float* iq, size_t pairs);
simd_stats_extrema simd_stats_extrema_avx2(const float* samples, size_t count);
simd_stats_iq_abs simd_stats_iq_abs_avx2(const float* iq, size_t pairs);
simd_stats_extrema simd_stats_extrema_neon(const float* samples, size_t count);
simd_stats_iq_abs simd_stats_iq_abs_neon(const float* iq, size_t pairs);

/* Rank-5 contract: count >= 1; samples, lmn and lmx (five floats each) are
 * non-null, do not overlap, and may be unaligned. lmn receives the five smallest
 * ascending and lmx the five largest descending, seeded as in the public API. */
/* AVX2's extra 256-bit merge round only pays off from four vectors; the
 * dispatcher sends shorter windows to SSE2. */
#define SIMD_STATS_RANK5_AVX2_MIN_COUNT 32U
void simd_stats_rank5_scalar(const float* samples, size_t count, float* lmn, float* lmx);
void simd_stats_rank5_sse2(const float* samples, size_t count, float* lmn, float* lmx);
void simd_stats_rank5_avx2(const float* samples, size_t count, float* lmn, float* lmx);
void simd_stats_rank5_neon(const float* samples, size_t count, float* lmn, float* lmx);

#endif
