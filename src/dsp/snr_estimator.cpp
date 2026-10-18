// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/snr_estimator.h>
#include <dsd-neo/platform/fp_opaque.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stddef.h>
#include "simd_kmeans_internal.h"
#include "snr_phase_samples.h"

namespace {

constexpr double kInvalidSnrDb = -100.0;
constexpr int kMaxSnrSamples = 8192;

int
valid_sps(int samples_per_symbol) {
    return samples_per_symbol >= 1 && samples_per_symbol <= 64;
}

int
normalized_phase_window(int samples_per_symbol, int phase_window) {
    int win = phase_window;
    if (win < 0) {
        win = samples_per_symbol / 10;
    }
    if (samples_per_symbol > 1 && win < 1) {
        win = 1;
    }
    int max_win = samples_per_symbol / 2;
    if (win > max_win) {
        win = max_win;
    }
    return win;
}

int
clamped_index(int count, int index) {
    if (index < 0) {
        return 0;
    }
    if (index >= count) {
        return count - 1;
    }
    return index;
}

/* Select the requested order statistics (each index clamped to [0, count)). An ascending index only partitions
 * the part above the previous selection, which nth_element() has already left holding the larger values, so each
 * result equals a full-range nth_element() at that index. */
template <int N>
void
percentile_values(float* vals, int count, const int (&indices)[N], double (&out)[N]) {
    int prev = -1;
    for (int i = 0; i < N; i++) {
        int index = clamped_index(count, indices[i]);
        if (index > prev) {
            std::nth_element(vals + prev + 1, vals + index, vals + count);
        } else if (index < prev) {
            std::nth_element(vals, vals + index, vals + count);
        }
        out[i] = vals[index];
        prev = index;
    }
}

void
kmeans_pass(const float* vals, int count, const double (&centers)[4], bool with_noise, simd_kmeans_pass* pass) {
    simd_kmeans4(vals, (size_t)count, centers, with_noise, pass);
}

void
kmeans_pass(const float* vals, int count, const double (&centers)[2], bool with_noise, simd_kmeans_pass* pass) {
    simd_kmeans2(vals, (size_t)count, centers, with_noise, pass);
}

/* Assign every sample to its nearest center and gather the per-center sums and counts; 0 when a center is empty. */
template <int Levels>
int
kmeans_assign(const float* vals, int count, const double (&centers)[Levels], bool with_noise, double (&sum)[Levels],
              int (&cnt)[Levels], double* noise) {
    simd_kmeans_pass pass;
    kmeans_pass(vals, count, centers, with_noise, &pass);
    for (int i = 0; i < Levels; i++) {
        sum[i] = pass.sum[i];
        cnt[i] = (int)pass.count[i];
    }
    if (noise) {
        *noise = pass.noise;
    }
    for (int i = 0; i < Levels; i++) {
        if (cnt[i] <= 0) {
            return 0;
        }
    }
    return 1;
}

/* Eight center updates, then a final assignment whose noise is the squared distance to the final centers. */
template <int Levels>
int
kmeans_refine(const float* vals, int count, double (&centers)[Levels], int (&cnt)[Levels], double* noise) {
    double sum[Levels];
    for (int iter = 0; iter < 8; iter++) {
        if (!kmeans_assign(vals, count, centers, false, sum, cnt, nullptr)) {
            return 0;
        }
        for (int i = 0; i < Levels; i++) {
            centers[i] = sum[i] / (double)cnt[i];
        }
        std::sort(centers, centers + Levels);
    }
    return kmeans_assign(vals, count, centers, true, sum, cnt, noise);
}

template <int Levels>
double
signal_variance(const double (&mu)[Levels], const int (&cnt)[Levels], int total) {
    double mean = 0.0;
    for (int b = 0; b < Levels; b++) {
        mean += mu[b] * (double)cnt[b] / (double)total;
    }

    double signal = 0.0;
    for (int b = 0; b < Levels; b++) {
        double d = mu[b] - mean;
        signal += (double)cnt[b] * d * d;
    }
    return signal / (double)total;
}

double
estimate_c4fm_values_db(float* vals, int count, double bias_db) {
    if (!vals || count <= 32) {
        return kInvalidSnrDb;
    }

    const int indices[4] = {count / 8, (3 * count) / 8, (5 * count) / 8, (7 * count) / 8};
    double mu[4];
    percentile_values(vals, count, indices, mu);
    std::sort(mu, mu + 4);
    int cnt[4] = {0, 0, 0, 0};
    double noise = 0.0;
    if (!kmeans_refine(vals, count, mu, cnt, &noise)) {
        return kInvalidSnrDb;
    }

    int total = cnt[0] + cnt[1] + cnt[2] + cnt[3];
    double noise_var = noise / (double)total;
    double sig_var = signal_variance(mu, cnt, total);
    if (!(noise_var > 1e-9 && sig_var > 1e-9)) {
        return kInvalidSnrDb;
    }
    return 10.0 * std::log10(sig_var / noise_var) - bias_db;
}

double
estimate_gfsk_values_db(float* vals, int count, double bias_db) {
    if (!vals || count <= 32) {
        return kInvalidSnrDb;
    }

    const int indices[2] = {count / 4, (3 * count) / 4};
    double mu[2];
    percentile_values(vals, count, indices, mu);
    std::sort(mu, mu + 2);
    int cnt[2] = {0, 0};
    double noise = 0.0;
    if (!kmeans_refine(vals, count, mu, cnt, &noise)) {
        return kInvalidSnrDb;
    }

    int total = cnt[0] + cnt[1];
    double noise_var = noise / (double)total;
    double sig_var = signal_variance(mu, cnt, total);
    if (!(noise_var > 1e-9 && sig_var > 1e-9)) {
        return kInvalidSnrDb;
    }
    return 10.0 * std::log10(sig_var / noise_var) - bias_db;
}

template <typename EstimateFn>
double
estimate_best_phase_db(const float* samples, int sample_count, int samples_per_symbol, int phase_window, double bias_db,
                       EstimateFn estimate_fn) {
    bias_db = dsd_fp_opaque_d(bias_db); /* keep the NaN test when LTO inlines this into fast-math code */
    if (!samples || sample_count <= 64 || !valid_sps(samples_per_symbol) || !std::isfinite(bias_db)) {
        return kInvalidSnrDb;
    }

    int win = normalized_phase_window(samples_per_symbol, phase_window);
    int phases = samples_per_symbol;
    /* Left uninitialized: each phase writes vals[0, count) before the estimator reads that range, and zero-filling
     * the whole buffer per call cost more than the estimate on short blocks. */
    std::array<float, kMaxSnrSamples> vals;
    double best = kInvalidSnrDb;
    for (int phase = 0; phase < phases; phase++) {
        int count = dsd_snr_collect_phase_samples(samples, sample_count, samples_per_symbol, phase, win, vals.data(),
                                                  (int)vals.size());
        if (count <= 32) {
            continue;
        }
        double snr = estimate_fn(vals.data(), count, bias_db);
        if (snr > best) {
            best = snr;
        }
    }
    return best;
}

} // namespace

extern "C" double
dsd_snr_estimate_c4fm_real_db(const float* samples, int sample_count, int samples_per_symbol, int phase_window,
                              double bias_db) {
    return estimate_best_phase_db(samples, sample_count, samples_per_symbol, phase_window, bias_db,
                                  estimate_c4fm_values_db);
}

extern "C" double
dsd_snr_estimate_gfsk_real_db(const float* samples, int sample_count, int samples_per_symbol, int phase_window,
                              double bias_db) {
    return estimate_best_phase_db(samples, sample_count, samples_per_symbol, phase_window, bias_db,
                                  estimate_gfsk_values_db);
}
