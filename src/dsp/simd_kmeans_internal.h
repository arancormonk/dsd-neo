// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* One k-means pass for the discriminator SNR estimator, private to dsd-neo_dsp. */

#ifndef DSD_NEO_DSP_SIMD_KMEANS_INTERNAL_H
#define DSD_NEO_DSP_SIMD_KMEANS_INTERNAL_H

#include <cmath>
#include <stddef.h>

struct simd_kmeans_pass {
    double sum[4];   /* sum of the samples assigned to each center */
    size_t count[4]; /* number of samples assigned to each center */
    double noise;    /* sum of (x - center)^2 over every sample; 0 unless requested */
};

/* Nearest center by |x - c| in double; strict less-than, so the lowest index wins a tie. */
template <int Levels>
static inline int
simd_kmeans_nearest(double sample, const double* centers) {
    int best = 0;
    double best_dist = std::fabs(sample - centers[0]);
    for (int i = 1; i < Levels; i++) {
        double dist = std::fabs(sample - centers[i]);
        if (dist < best_dist) {
            best = i;
            best_dist = dist;
        }
    }
    return best;
}

/* Continue a pass sequentially over vals[start, count), as the scalar backend does from 0. */
template <int Levels>
static inline void
simd_kmeans_tail(const float* vals, size_t start, size_t count, const double* centers, bool with_noise,
                 simd_kmeans_pass* out) {
    for (size_t i = start; i < count; i++) {
        const double x = (double)vals[i];
        const int b = simd_kmeans_nearest<Levels>(x, centers);
        out->sum[b] += x;
        out->count[b]++;
        if (with_noise) {
            const double e = x - centers[b];
            out->noise += e * e;
        }
    }
}

/* Backend contract: vals holds count finite samples (any alignment); centers holds 4 or 2 values. Each backend
 * assigns every sample exactly as simd_kmeans_nearest() does, so counts match the scalar pass exactly; the vector
 * backends add lanes in a different order, so sums and noise may differ by rounding. Unused entries of out are 0. */
void simd_kmeans4_scalar(const float* vals, size_t count, const double* centers, bool with_noise,
                         simd_kmeans_pass* out);
void simd_kmeans2_scalar(const float* vals, size_t count, const double* centers, bool with_noise,
                         simd_kmeans_pass* out);
void simd_kmeans4_sse2(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out);
void simd_kmeans2_sse2(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out);
void simd_kmeans4_avx2(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out);
void simd_kmeans2_avx2(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out);
void simd_kmeans4_neon(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out);
void simd_kmeans2_neon(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out);

/* Runtime-dispatched passes (AVX2, SSE2, NEON or scalar). */
void simd_kmeans4(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out);
void simd_kmeans2(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out);

#endif
