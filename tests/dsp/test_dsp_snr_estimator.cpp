// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/snr_estimator.h>

#include <cmath>
#include <cstdint>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

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

} // namespace

int
main(void) {
    int rc = 0;
    rc |= expect_good_four_level_estimate();
    rc |= expect_good_binary_estimate();
    rc |= expect_invalid_for_insufficient_data();
    rc |= expect_nonfinite_samples_dropped();
    rc |= expect_invalid_for_nonfinite_bias();
    if (rc == 0) {
        printf("DSP_SNR_ESTIMATOR: OK\n");
    }
    return rc;
}
