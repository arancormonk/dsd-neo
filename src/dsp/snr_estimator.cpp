// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/snr_estimator.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdlib.h>

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
circular_phase_distance(int a, int b, int samples_per_symbol) {
    int d = std::abs(a - b);
    int wrapped = samples_per_symbol - d;
    return (d < wrapped) ? d : wrapped;
}

int
collect_phase_samples(const float* samples, int sample_count, int samples_per_symbol, int phase, int phase_window,
                      float* vals, int max_vals) {
    int count = 0;
    for (int i = 0; i < sample_count && count < max_vals; i++) {
        if (samples_per_symbol > 1) {
            int sample_phase = i % samples_per_symbol;
            if (circular_phase_distance(sample_phase, phase, samples_per_symbol) > phase_window) {
                continue;
            }
        }
        float v = samples[i];
        if (std::isfinite(v)) {
            vals[count++] = v;
        }
    }
    return count;
}

double
percentile_value(float* vals, int count, int index) {
    if (index < 0) {
        index = 0;
    } else if (index >= count) {
        index = count - 1;
    }
    std::nth_element(vals, vals + index, vals + count);
    return vals[index];
}

template <int Levels>
int
nearest_center(double sample, const double (&centers)[Levels]) {
    int best = 0;
    double best_dist = std::abs(sample - centers[0]);
    for (int i = 1; i < Levels; i++) {
        double dist = std::abs(sample - centers[i]);
        if (dist < best_dist) {
            best = i;
            best_dist = dist;
        }
    }
    return best;
}

template <int Levels>
int
kmeans_assign(const float* vals, int count, const double (&centers)[Levels], int* assignments, double (&sum)[Levels],
              int (&cnt)[Levels]) {
    for (int i = 0; i < Levels; i++) {
        sum[i] = 0.0;
        cnt[i] = 0;
    }
    for (int i = 0; i < count; i++) {
        int b = nearest_center((double)vals[i], centers);
        assignments[i] = b;
        sum[b] += vals[i];
        cnt[b]++;
    }
    for (int i = 0; i < Levels; i++) {
        if (cnt[i] <= 0) {
            return 0;
        }
    }
    return 1;
}

template <int Levels>
int
kmeans_refine(const float* vals, int count, double (&centers)[Levels], int* assignments, int (&cnt)[Levels]) {
    double sum[Levels];
    for (int iter = 0; iter < 8; iter++) {
        if (!kmeans_assign(vals, count, centers, assignments, sum, cnt)) {
            return 0;
        }
        for (int i = 0; i < Levels; i++) {
            centers[i] = sum[i] / (double)cnt[i];
        }
        std::sort(centers, centers + Levels);
    }
    return kmeans_assign(vals, count, centers, assignments, sum, cnt);
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

    double mu[4] = {percentile_value(vals, count, count / 8), percentile_value(vals, count, (3 * count) / 8),
                    percentile_value(vals, count, (5 * count) / 8), percentile_value(vals, count, (7 * count) / 8)};
    std::sort(mu, mu + 4);
    std::array<int, kMaxSnrSamples> assignments{};
    int cnt[4] = {0, 0, 0, 0};
    if (!kmeans_refine(vals, count, mu, assignments.data(), cnt)) {
        return kInvalidSnrDb;
    }

    double noise = 0.0;
    for (int i = 0; i < count; i++) {
        double e = (double)vals[i] - mu[assignments[(size_t)i]];
        noise += e * e;
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

    double mu[2] = {percentile_value(vals, count, count / 4), percentile_value(vals, count, (3 * count) / 4)};
    std::sort(mu, mu + 2);
    std::array<int, kMaxSnrSamples> assignments{};
    int cnt[2] = {0, 0};
    if (!kmeans_refine(vals, count, mu, assignments.data(), cnt)) {
        return kInvalidSnrDb;
    }

    double noise = 0.0;
    for (int i = 0; i < count; i++) {
        double e = (double)vals[i] - mu[assignments[(size_t)i]];
        noise += e * e;
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
    if (!samples || sample_count <= 64 || !valid_sps(samples_per_symbol) || !std::isfinite(bias_db)) {
        return kInvalidSnrDb;
    }

    int win = normalized_phase_window(samples_per_symbol, phase_window);
    int phases = samples_per_symbol;
    std::array<float, kMaxSnrSamples> vals{};
    double best = kInvalidSnrDb;
    for (int phase = 0; phase < phases; phase++) {
        int count =
            collect_phase_samples(samples, sample_count, samples_per_symbol, phase, win, vals.data(), (int)vals.size());
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
