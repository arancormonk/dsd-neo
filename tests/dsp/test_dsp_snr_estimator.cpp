// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/snr_estimator.h>

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

} // namespace

int
main(void) {
    int rc = 0;
    rc |= expect_good_four_level_estimate();
    rc |= expect_good_binary_estimate();
    rc |= expect_invalid_for_insufficient_data();
    if (rc == 0) {
        printf("DSP_SNR_ESTIMATOR: OK\n");
    }
    return rc;
}
