// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/snr_estimator.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "dsd-neo/core/safe_api.h"
#include "dsp/simd_kmeans_internal.h"
#include "dsp/snr_phase_samples.h"
#if defined(__x86_64__) || defined(_M_X64)
#include "dsp/simd_x86_cpu.h"
#endif

namespace {

enum : std::uint16_t { kSps = 10, kSymbols = 512, kSamples = kSps * kSymbols };

float
deterministic_noise(uint32_t* seed, float peak) {
    *seed = (*seed * 1664525u) + 1013904223u;
    int centered = (int)((*seed >> 16) & 0xffffu) - 32768;
    return ((float)centered / 32768.0f) * peak;
}

void
fill_four_level_discriminator(float* out) {
    static const float levels[4] = {-30000.0f, -10000.0f, 10000.0f, 30000.0f};
    uint32_t seed = 0x1234abcdU;
    float prev = levels[0];
    for (int sym = 0; sym < kSymbols; sym++) {
        float level = levels[(sym + (sym / 7)) & 3];
        for (int k = 0; k < kSps; k++) {
            float v = (k == 0 && sym > 0) ? (0.5f * (prev + level)) : level;
            out[(sym * kSps) + k] = v + deterministic_noise(&seed, 350.0f);
        }
        prev = level;
    }
}

void
fill_binary_discriminator(float* out) {
    uint32_t seed = 0x4f1bbc2dU;
    float prev = -22000.0f;
    for (int sym = 0; sym < kSymbols; sym++) {
        float level = (sym & 1) ? 22000.0f : -22000.0f;
        for (int k = 0; k < kSps; k++) {
            float v = (k == 0 && sym > 0) ? (0.5f * (prev + level)) : level;
            out[(sym * kSps) + k] = v + deterministic_noise(&seed, 450.0f);
        }
        prev = level;
    }
}

int
expect_good_four_level_estimate(void) {
    float samples[kSamples];
    fill_four_level_discriminator(samples);

    double c4fm = dsd_snr_estimate_c4fm_real_db(samples, kSamples, kSps, 1, 7.0);
    double binary = dsd_snr_estimate_gfsk_real_db(samples, kSamples, kSps, 1, 3.0);
    if (!(c4fm > 20.0 && c4fm > binary + 12.0)) {
        DSD_FPRINTF(stderr, "four-level estimator c4fm=%.3f binary=%.3f\n", c4fm, binary);
        return 1;
    }
    return 0;
}

int
expect_good_binary_estimate(void) {
    float samples[kSamples];
    fill_binary_discriminator(samples);

    double gfsk = dsd_snr_estimate_gfsk_real_db(samples, kSamples, kSps, 1, 3.0);
    if (!(gfsk > 20.0)) {
        DSD_FPRINTF(stderr, "binary estimator gfsk=%.3f\n", gfsk);
        return 1;
    }
    return 0;
}

int
expect_invalid_for_insufficient_data(void) {
    float samples[16] = {};
    double snr = dsd_snr_estimate_c4fm_real_db(samples, 16, kSps, 1, 0.0);
    if (!(snr <= -50.0)) {
        DSD_FPRINTF(stderr, "insufficient data estimator snr=%.3f\n", snr);
        return 1;
    }
    return 0;
}

/* NaN and infinity from their bit patterns, so the global fast-math option has no NAN or INFINITY literal to warn on. */
float
float_from_bits(uint32_t bits) {
    float value = 0.0f;
    DSD_MEMCPY(&value, &bits, sizeof value);
    return value;
}

int
expect_nonfinite_samples_dropped(void) {
    float samples[kSamples];
    fill_four_level_discriminator(samples);
    double clean = dsd_snr_estimate_c4fm_real_db(samples, kSamples, kSps, 1, 7.0);

    /* A cf32 capture or a Soapy CF32 stream can carry NaN and infinite samples into the discriminator. Without the
     * filter they reach std::nth_element(), whose ordering they break. */
    for (int i = 0; i < kSamples; i += 13) {
        samples[i] = float_from_bits(0x7fc00000U);
    }
    for (int i = 5; i < kSamples; i += 17) {
        samples[i] = float_from_bits(0x7f800000U);
    }
    for (int i = 9; i < kSamples; i += 19) {
        samples[i] = float_from_bits(0xff800000U);
    }
    double dirty = dsd_snr_estimate_c4fm_real_db(samples, kSamples, kSps, 1, 7.0);
    /* This source keeps IEEE semantics in fast-math builds, so the finiteness test below is a real one. */
    if (!std::isfinite(clean) || !std::isfinite(dirty) || !(std::fabs(dirty - clean) < 1.0)) {
        DSD_FPRINTF(stderr, "non-finite samples estimator clean=%.3f dirty=%.3f\n", clean, dirty);
        return 1;
    }
    return 0;
}

int
expect_invalid_for_nonfinite_bias(void) {
    float samples[kSamples];
    fill_four_level_discriminator(samples);
    double nan_bias = 0.0;
    double inf_bias = 0.0;
    const uint64_t nan_bits = 0x7ff8000000000000ULL;
    const uint64_t inf_bits = 0x7ff0000000000000ULL;
    DSD_MEMCPY(&nan_bias, &nan_bits, sizeof nan_bias);
    DSD_MEMCPY(&inf_bias, &inf_bits, sizeof inf_bias);
    double nan_snr = dsd_snr_estimate_c4fm_real_db(samples, kSamples, kSps, 1, nan_bias);
    double inf_snr = dsd_snr_estimate_gfsk_real_db(samples, kSamples, kSps, 1, inf_bias);
    if (!std::isfinite(nan_snr) || !std::isfinite(inf_snr) || !(nan_snr <= -50.0 && inf_snr <= -50.0)) {
        DSD_FPRINTF(stderr, "non-finite bias estimator nan=%.3f inf=%.3f\n", nan_snr, inf_snr);
        return 1;
    }
    return 0;
}

int
expect_phase_collection_matches_modulo_scan(void) {
    std::vector<float> samples(17000);
    uint32_t seed = 187;
    for (size_t i = 0; i < samples.size(); ++i) {
        samples[i] = deterministic_noise(&seed, 1.0f);
        if (i % 127 == 0) {
            samples[i] = float_from_bits(0x7fc00000U);
        } else if (i % 271 == 0) {
            samples[i] = float_from_bits(0x7f800000U);
        } else if (i % 31 == 0) {
            samples[i] = float_from_bits(0x80000000U);
        }
    }
    std::vector<float> expected(8192);
    std::vector<float> guarded(8194);
    const float canary = 123456.0f;
    for (int sps = 1; sps <= 64; ++sps) {
        for (int phase = 0; phase < sps; ++phase) {
            for (int requested : {0, 1, 2, sps / 10, sps / 2}) {
                const int win = std::min(requested, sps / 2);
                for (int length : {0, 31, 256, 8193, 17000}) {
                    for (int cap : {0, 1, 17, 8192}) {
                        int want = 0;
                        for (int i = 0; i < length && want < cap; ++i) {
                            const int distance = std::abs(i % sps - phase);
                            if (std::min(distance, sps - distance) <= win && std::isfinite(samples[(size_t)i])) {
                                expected[(size_t)want++] = samples[(size_t)i];
                            }
                        }
                        guarded[0] = canary;
                        guarded[(size_t)want + 1] = canary;
                        guarded[(size_t)cap + 1] = canary;
                        guarded.back() = canary;
                        const int got = dsd_snr_collect_phase_samples(samples.data(), length, sps, phase, win,
                                                                      guarded.data() + 1, cap);
                        if (got != want
                            || std::memcmp(expected.data(), guarded.data() + 1, (size_t)want * sizeof(float)) != 0
                            || std::fabs(guarded[0] - canary) > 1e-7f
                            || std::fabs(guarded[(size_t)got + 1] - canary) > 1e-7f
                            || std::fabs(guarded[(size_t)cap + 1] - canary) > 1e-7f
                            || std::fabs(guarded.back() - canary) > 1e-7f) {
                            DSD_FPRINTF(stderr, "phase collector mismatch: sps=%d phase=%d win=%d n=%d cap=%d\n", sps,
                                        phase, win, length, cap);
                            return 1;
                        }
                    }
                }
            }
        }
    }
    if (dsd_snr_collect_phase_samples(nullptr, 256, 10, 0, 1, guarded.data(), 8192) != 0
        || dsd_snr_collect_phase_samples(samples.data(), 256, 10, 0, 1, nullptr, 8192) != 0
        || dsd_snr_collect_phase_samples(samples.data(), -1, 10, 0, 1, guarded.data(), 8192) != 0
        || dsd_snr_collect_phase_samples(samples.data(), 256, 0, 0, 1, guarded.data(), 8192) != 0
        || dsd_snr_collect_phase_samples(samples.data(), 256, 65, 0, 1, guarded.data(), 8192) != 0
        || dsd_snr_collect_phase_samples(samples.data(), 256, 10, 10, 1, guarded.data(), 8192) != 0
        || dsd_snr_collect_phase_samples(samples.data(), 256, 10, 0, 6, guarded.data(), 8192) != 0) {
        DSD_FPRINTF(stderr, "phase collector accepted invalid input\n");
        return 1;
    }
    return 0;
}

/* The estimator as it was before its k-means passes were vectorised: full-range percentile selection, a scalar
 * assignment pass with an assignments array, and a separate sequential noise sum. */
namespace legacy {

constexpr double kInvalidSnrDb = -100.0;
constexpr int kMaxSnrSamples = 8192;

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

template <int Levels>
double
estimate_values_db(float* vals, int count, double bias_db) {
    if (count <= 32) {
        return kInvalidSnrDb;
    }
    /* Seed at the centre of each level's share: count/8, 3count/8, ... for four levels, count/4, 3count/4 for two.
     * A loop rather than a Levels == 4 branch, so the two-level instantiation never indexes mu[2]. */
    double mu[Levels];
    for (int b = 0; b < Levels; b++) {
        mu[b] = percentile_value(vals, count, ((2 * b + 1) * count) / (2 * Levels));
    }
    std::sort(mu, mu + Levels);
    std::vector<int> assignments((size_t)kMaxSnrSamples);
    int cnt[Levels] = {};
    if (!kmeans_refine(vals, count, mu, assignments.data(), cnt)) {
        return kInvalidSnrDb;
    }
    double noise = 0.0;
    for (int i = 0; i < count; i++) {
        double e = (double)vals[i] - mu[assignments[(size_t)i]];
        noise += e * e;
    }
    int total = 0;
    for (int b = 0; b < Levels; b++) {
        total += cnt[b];
    }
    double noise_var = noise / (double)total;
    double sig_var = signal_variance(mu, cnt, total);
    if (!(noise_var > 1e-9 && sig_var > 1e-9)) {
        return kInvalidSnrDb;
    }
    return 10.0 * std::log10(sig_var / noise_var) - bias_db;
}

template <int Levels>
double
estimate_db(const float* samples, int sample_count, int sps, int phase_window, double bias_db) {
    if (!samples || sample_count <= 64 || sps < 1 || sps > 64 || !std::isfinite(bias_db)) {
        return kInvalidSnrDb;
    }
    int win = phase_window;
    if (win < 0) {
        win = sps / 10;
    }
    if (sps > 1 && win < 1) {
        win = 1;
    }
    if (win > sps / 2) {
        win = sps / 2;
    }
    std::vector<float> vals((size_t)kMaxSnrSamples, 0.0f);
    double best = kInvalidSnrDb;
    for (int phase = 0; phase < sps; phase++) {
        int count = dsd_snr_collect_phase_samples(samples, sample_count, sps, phase, win, vals.data(), kMaxSnrSamples);
        if (count <= 32) {
            continue;
        }
        double snr = estimate_values_db<Levels>(vals.data(), count, bias_db);
        if (snr > best) {
            best = snr;
        }
    }
    return best;
}

} // namespace legacy

/* Vector backends add each center's samples lane by lane and then across lanes, not in input order, so their sums
 * and noise differ from the sequential ones by rounding alone. Relative to the magnitudes summed that is a few ulps
 * per addition; 1e-12 leaves a wide margin over 8192 samples while still catching any misassigned sample. */
constexpr double kSumRelTolerance = 1e-12;
/* The SNR is a log ratio of such sums, so the same rounding moves it by ~1e-14 dB. */
constexpr double kSnrDbTolerance = 1e-9;

using kmeans_fn = void (*)(const float*, size_t, const double*, bool, simd_kmeans_pass*);

uint32_t
next_random(uint32_t* seed) {
    *seed = (*seed * 1664525u) + 1013904223u;
    return *seed;
}

float
random_unit(uint32_t* seed) {
    return (float)((next_random(seed) >> 8) & 0xffffffU) / 16777216.0f;
}

bool
sum_close(double want, double got, double magnitude) {
    return std::fabs(want - got) <= kSumRelTolerance * (magnitude + 1e-30);
}

template <int Levels>
int
check_kmeans_backend(const char* name, kmeans_fn fn, const float* vals, size_t count, const double* centers,
                     bool exact_sums) {
    double want_sum[4] = {};
    size_t want_count[4] = {};
    double magnitude[4] = {};
    double want_noise = 0.0;
    double noise_magnitude = 0.0;
    for (size_t i = 0; i < count; i++) {
        const double x = (double)vals[i];
        int best = 0;
        double best_dist = std::fabs(x - centers[0]);
        for (int j = 1; j < Levels; j++) {
            const double dist = std::fabs(x - centers[j]);
            if (dist < best_dist) {
                best = j;
                best_dist = dist;
            }
        }
        want_sum[best] += x;
        want_count[best]++;
        magnitude[best] += std::fabs(x);
        const double e = x - centers[best];
        want_noise += e * e;
        noise_magnitude += e * e;
    }
    for (int with_noise = 0; with_noise <= 1; with_noise++) {
        simd_kmeans_pass got;
        DSD_MEMSET(&got, 0x5a, sizeof got);
        fn(vals, count, centers, with_noise != 0, &got);
        bool ok = true;
        for (int j = 0; j < 4; j++) {
            if (j >= Levels) {
                ok = ok && got.count[j] == 0 && std::fabs(got.sum[j]) <= 0.0;
                continue;
            }
            ok = ok && got.count[j] == want_count[j];
            ok = ok
                 && (exact_sums ? std::fabs(got.sum[j] - want_sum[j]) <= 0.0
                                : sum_close(want_sum[j], got.sum[j], magnitude[j]));
        }
        if (with_noise) {
            ok = ok
                 && (exact_sums ? std::fabs(got.noise - want_noise) <= 0.0
                                : sum_close(want_noise, got.noise, noise_magnitude));
        } else {
            ok = ok && std::fabs(got.noise) <= 0.0;
        }
        if (!ok) {
            DSD_FPRINTF(stderr, "k-means %s levels=%d n=%zu noise=%d mismatch: counts", name, Levels, count,
                        with_noise);
            for (int j = 0; j < Levels; j++) {
                DSD_FPRINTF(stderr, " %zu/%zu", got.count[j], want_count[j]);
            }
            DSD_FPRINTF(stderr, " sums");
            for (int j = 0; j < Levels; j++) {
                DSD_FPRINTF(stderr, " %.17g/%.17g", got.sum[j], want_sum[j]);
            }
            DSD_FPRINTF(stderr, " noise %.17g/%.17g\n", got.noise, want_noise);
            return 1;
        }
    }
    return 0;
}

int
check_kmeans_cases(const char* name, kmeans_fn fn4, kmeans_fn fn2) {
    uint32_t seed = 0x9e3779b9U;
    /* Room for the longest block plus an unaligned start. */
    std::vector<float> buf(8192 + 8);
    const double centers4[4] = {-0.75, -0.25, 0.25, 0.75};
    const double centers2[2] = {-0.5, 0.5};
    /* Equal centers: every tie goes to the lower index. */
    const double tied4[4] = {-0.5, -0.5, 0.5, 0.5};
    const double tied2[2] = {0.25, 0.25};
    const size_t lengths[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 31, 33, 63, 64, 65, 255, 256, 257, 2047, 8192};
    for (size_t n : lengths) {
        for (size_t offset = 0; offset < 4; offset++) {
            float* vals = buf.data() + offset;
            /* Random samples: sums need a tolerance. */
            for (size_t i = 0; i < n; i++) {
                vals[i] = 2.0f * random_unit(&seed) - 1.0f;
            }
            if (check_kmeans_backend<4>(name, fn4, vals, n, centers4, false)
                || check_kmeans_backend<2>(name, fn2, vals, n, centers2, false)
                || check_kmeans_backend<4>(name, fn4, vals, n, tied4, false)
                || check_kmeans_backend<2>(name, fn2, vals, n, tied2, false)) {
                return 1;
            }
            /* Eighths, including every midpoint between the centers and signed zeros: each sum and noise term is
             * exact in any order, so the sums must match bit for bit, which also proves each sample's assignment. */
            for (size_t i = 0; i < n; i++) {
                const int step = (int)(next_random(&seed) >> 16) % 17 - 8;
                vals[i] = (step == 0 && (i & 1U)) ? -0.0f : (float)step / 8.0f;
            }
            if (check_kmeans_backend<4>(name, fn4, vals, n, centers4, true)
                || check_kmeans_backend<2>(name, fn2, vals, n, centers2, true)
                || check_kmeans_backend<4>(name, fn4, vals, n, tied4, true)
                || check_kmeans_backend<2>(name, fn2, vals, n, tied2, true)) {
                return 1;
            }
        }
    }
    return 0;
}

/* Every k-means backend on this host, and the dispatcher, against the sequential nearest-center pass. */
int
expect_kmeans_backends_match_scalar(void) {
    int rc = check_kmeans_cases("scalar", simd_kmeans4_scalar, simd_kmeans2_scalar);
    rc |= check_kmeans_cases("dispatch", simd_kmeans4, simd_kmeans2);
#if defined(__x86_64__) || defined(_M_X64)
    rc |= check_kmeans_cases("SSE2", simd_kmeans4_sse2, simd_kmeans2_sse2);
#if defined(DSD_NEO_TEST_HAVE_AVX2_IMPL) && DSD_NEO_X86_AVX2_RUNTIME_PROBE_SUPPORTED
    if (dsd_neo_cpu_has_avx2_with_os_support()) {
        rc |= check_kmeans_cases("AVX2", simd_kmeans4_avx2, simd_kmeans2_avx2);
    }
#endif
#elif defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64) || defined(_M_ARM64EC)
    rc |= check_kmeans_cases("NEON", simd_kmeans4_neon, simd_kmeans2_neon);
#endif
    return rc;
}

int
compare_with_legacy(const char* label, const std::vector<float>& samples, int sps, int window) {
    const int n = (int)samples.size();
    const double bias = 1.5;
    const double want4 = legacy::estimate_db<4>(samples.data(), n, sps, window, bias);
    const double got4 = dsd_snr_estimate_c4fm_real_db(samples.data(), n, sps, window, bias);
    const double want2 = legacy::estimate_db<2>(samples.data(), n, sps, window, bias);
    const double got2 = dsd_snr_estimate_gfsk_real_db(samples.data(), n, sps, window, bias);
    if (!(std::fabs(want4 - got4) <= kSnrDbTolerance) || !(std::fabs(want2 - got2) <= kSnrDbTolerance)) {
        DSD_FPRINTF(stderr, "legacy mismatch %s n=%d sps=%d win=%d: c4fm %.17g/%.17g gfsk %.17g/%.17g\n", label, n, sps,
                    window, got4, want4, got2, want2);
        return 1;
    }
    return 0;
}

/* The public estimators against the embedded legacy algorithm on random, clustered, tied, constant and degenerate
 * blocks. Selection order and summation order changed, so only rounding may separate them. */
int
expect_estimators_match_legacy(void) {
    uint32_t seed = 0x2545f491U;
    int rc = 0;
    int invalid_seen = 0;
    const int sps_list[] = {1, 2, 5, 10, 20, 64};
    const int lengths[] = {65, 100, 257, 2048, 9001};
    for (int sps : sps_list) {
        for (int n : lengths) {
            for (int window : {-1, 0, 1, sps / 2}) {
                std::vector<float> samples((size_t)n);
                /* Uniform noise. */
                for (float& v : samples) {
                    v = 2.0f * random_unit(&seed) - 1.0f;
                }
                rc |= compare_with_legacy("random", samples, sps, window);
                /* Four and two rails with noise. */
                for (int levels : {4, 2}) {
                    for (int i = 0; i < n; i++) {
                        const int symbol = i / sps;
                        const int rail = (int)((uint32_t)(symbol * 2654435761U) >> 28) % levels;
                        samples[(size_t)i] =
                            (float)(2 * rail - levels + 1) * 1000.0f + 300.0f * (random_unit(&seed) - 0.5f);
                    }
                    rc |= compare_with_legacy("clustered", samples, sps, window);
                }
                /* Small integers: many samples equidistant from two centers, and exact sums. */
                for (float& v : samples) {
                    v = (float)((int)(next_random(&seed) >> 16) % 7 - 3);
                }
                rc |= compare_with_legacy("tied", samples, sps, window);
                /* Constant: one rail, so no signal variance. */
                std::fill(samples.begin(), samples.end(), 0.25f);
                rc |= compare_with_legacy("constant", samples, sps, window);
                /* Two values: four centers leave two empty clusters. */
                for (size_t i = 0; i < samples.size(); i++) {
                    samples[i] = (i & 1U) ? 1.0f : -1.0f;
                }
                rc |= compare_with_legacy("two-valued", samples, sps, window);
                if (dsd_snr_estimate_c4fm_real_db(samples.data(), n, sps, window, 0.0) <= -100.0 + 1e-9) {
                    invalid_seen++;
                }
            }
        }
    }
    if (invalid_seen == 0) {
        DSD_FPRINTF(stderr, "legacy comparison never reached an empty cluster\n");
        rc |= 1;
    }
    return rc;
}

} // namespace

int
main(void) {
    int rc = 0;
    rc |= expect_phase_collection_matches_modulo_scan();
    rc |= expect_good_four_level_estimate();
    rc |= expect_good_binary_estimate();
    rc |= expect_invalid_for_insufficient_data();
    rc |= expect_nonfinite_samples_dropped();
    rc |= expect_invalid_for_nonfinite_bias();
    rc |= expect_kmeans_backends_match_scalar();
    rc |= expect_estimators_match_legacy();
    if (rc == 0) {
        printf("DSP_SNR_ESTIMATOR: OK\n");
    }
    return rc;
}
