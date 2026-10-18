// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "simd_kmeans_internal.h"

#if defined(__clang_analyzer__)
void
simd_kmeans4_neon(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    simd_kmeans4_scalar(vals, count, centers, with_noise, out);
}

void
simd_kmeans2_neon(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    simd_kmeans2_scalar(vals, count, centers, with_noise, out);
}
#else

#include <arm_neon.h>
#include <stddef.h>
#include <stdint.h>

// This translation unit is built only for ARM64, where NEON is guaranteed.
// NOLINTBEGIN(portability-simd-intrinsics)
static inline void
accumulate(uint64x2_t is, float64x2_t x, float64x2_t* sum, uint64x2_t* cnt) {
    *sum = vaddq_f64(*sum, vreinterpretq_f64_u64(vandq_u64(is, vreinterpretq_u64_f64(x))));
    *cnt = vsubq_u64(*cnt, is); /* an all-ones lane is -1 */
}

struct kmeans_neon_acc {
    float64x2_t c[4];
    float64x2_t sum[4];
    uint64x2_t cnt[4]; /* center 0 is counted from the total */
    float64x2_t noise;
};

/* won_j: center j is strictly nearer than every lower center, as in simd_kmeans_nearest(); the highest center
 * that won is the nearest one. FABD rounds x - c once and clears the sign, exactly as fabs(x - c). */
template <bool WithNoise>
static inline void
step4(float64x2_t x, kmeans_neon_acc* a) {
    const float64x2_t d0 = vabdq_f64(x, a->c[0]);
    const float64x2_t d1 = vabdq_f64(x, a->c[1]);
    const uint64x2_t won1 = vcltq_f64(d1, d0);
    const float64x2_t best1 = vbslq_f64(won1, d1, d0);
    const float64x2_t d2 = vabdq_f64(x, a->c[2]);
    const uint64x2_t won2 = vcltq_f64(d2, best1);
    const float64x2_t best2 = vbslq_f64(won2, d2, best1);
    const float64x2_t d3 = vabdq_f64(x, a->c[3]);
    const uint64x2_t won3 = vcltq_f64(d3, best2);
    const uint64x2_t is2 = vbicq_u64(won2, won3);
    const uint64x2_t above1 = vorrq_u64(won3, won2);
    const uint64x2_t is1 = vbicq_u64(won1, above1);
    const uint64x2_t above0 = vorrq_u64(above1, won1);
    a->sum[0] = vaddq_f64(a->sum[0], vreinterpretq_f64_u64(vbicq_u64(vreinterpretq_u64_f64(x), above0)));
    accumulate(is1, x, &a->sum[1], &a->cnt[1]);
    accumulate(is2, x, &a->sum[2], &a->cnt[2]);
    accumulate(won3, x, &a->sum[3], &a->cnt[3]);
    if (WithNoise) {
        float64x2_t center = vbslq_f64(won1, a->c[1], a->c[0]);
        center = vbslq_f64(won2, a->c[2], center);
        center = vbslq_f64(won3, a->c[3], center);
        const float64x2_t e = vsubq_f64(x, center);
        a->noise = vaddq_f64(a->noise, vmulq_f64(e, e));
    }
}

template <bool WithNoise>
static inline void
step2(float64x2_t x, kmeans_neon_acc* a) {
    const uint64x2_t won1 = vcltq_f64(vabdq_f64(x, a->c[1]), vabdq_f64(x, a->c[0]));
    a->sum[0] = vaddq_f64(a->sum[0], vreinterpretq_f64_u64(vbicq_u64(vreinterpretq_u64_f64(x), won1)));
    accumulate(won1, x, &a->sum[1], &a->cnt[1]);
    if (WithNoise) {
        const float64x2_t e = vsubq_f64(x, vbslq_f64(won1, a->c[1], a->c[0]));
        a->noise = vaddq_f64(a->noise, vmulq_f64(e, e));
    }
}

template <int Levels, bool WithNoise>
static void
kmeans_neon(const float* vals, size_t count, const double* centers, simd_kmeans_pass* out) {
    *out = simd_kmeans_pass{};
    kmeans_neon_acc acc;
    for (int j = 0; j < 4; j++) {
        acc.c[j] = vdupq_n_f64(centers[j < Levels ? j : 0]);
        acc.sum[j] = vdupq_n_f64(0.0);
        acc.cnt[j] = vdupq_n_u64(0);
    }
    acc.noise = vdupq_n_f64(0.0);
    size_t i = 0;
    for (; count - i >= 4; i += 4) {
        const float32x4_t x4 = vld1q_f32(vals + i);
        if (Levels == 4) {
            step4<WithNoise>(vcvt_f64_f32(vget_low_f32(x4)), &acc);
            step4<WithNoise>(vcvt_high_f64_f32(x4), &acc);
        } else {
            step2<WithNoise>(vcvt_f64_f32(vget_low_f32(x4)), &acc);
            step2<WithNoise>(vcvt_high_f64_f32(x4), &acc);
        }
    }
    size_t others = 0;
    for (int j = 1; j < Levels; j++) {
        out->sum[j] = vaddvq_f64(acc.sum[j]);
        out->count[j] = (size_t)vaddvq_u64(acc.cnt[j]);
        others += out->count[j];
    }
    out->sum[0] = vaddvq_f64(acc.sum[0]);
    out->count[0] = i - others;
    if (WithNoise) {
        out->noise = vaddvq_f64(acc.noise);
    }
    simd_kmeans_tail<Levels>(vals, i, count, centers, WithNoise, out);
}

void
simd_kmeans4_neon(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    if (with_noise) {
        kmeans_neon<4, true>(vals, count, centers, out);
    } else {
        kmeans_neon<4, false>(vals, count, centers, out);
    }
}

void
simd_kmeans2_neon(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    if (with_noise) {
        kmeans_neon<2, true>(vals, count, centers, out);
    } else {
        kmeans_neon<2, false>(vals, count, centers, out);
    }
}

// NOLINTEND(portability-simd-intrinsics)
#endif
