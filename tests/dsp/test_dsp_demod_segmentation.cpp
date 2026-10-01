// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief full_demod()'s linear front end does not depend on how its input is cut into blocks (issue #572).
 *
 * The RTL chain at 1.536 Msps: five half-band passes to 48 kHz, then the 48 kHz channel filter, in the 12K5 profile and
 * on the analog NFM 16 kHz channel, through the pass-through and the FM demodulators. The stages that still decide per
 * block are off or held open: I/Q balance off, the squelch level 0 with its envelope open, and no CQPSK (so no adaptive
 * timing gain). A synthetic stream (an FM tone, a carrier on the adjacent 25 kHz channel and noise) in one block and
 * the same stream cut many ways (odd sizes, sizes below the cascade's 32, a run of 1-sample blocks, large random
 * blocks) must give the same output count and the same values within float tolerance.
 *
 * Each split's deviation from the whole stream is printed whether it passes or not.
 */

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/mem.h>
#include <vector>
#include "dsd-neo/core/safe_api.h"

namespace {

constexpr int kCaptureRateHz = 1536000;
constexpr int kPasses = 5; /* 1.536 Msps / 32 = 48 kHz */
constexpr int kDemodRateHz = kCaptureRateHz >> kPasses;
constexpr int kStreamSamples = 98304;  /* complex, 64 ms */
constexpr double kRawTolOfPeak = 2e-5; /* the pass-through output, relative to its peak magnitude */
constexpr double kFmTolRad = 2e-4;     /* the FM discriminator output, in radians per sample */

struct Config {
    const char* name;
    int analog; /* 1: the analog NFM 16 kHz channel, 0: the 12K5 profile */
    int fm;     /* 1: the FM discriminator, 0: the pass-through */
};

struct Split {
    const char* name;
    std::vector<int> sizes; /* complex samples per block, in order; the rest of the stream in one block after them */
};

struct DemodBox {
    demod_state* s;

    DemodBox() : s(static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)))) {
        if (s) {
            DSD_MEMSET(s, 0, sizeof(*s));
        }
    }

    ~DemodBox() {
        if (s) {
            dsd_neo_aligned_free(s);
        }
    }

    DemodBox(const DemodBox&) = delete;
    DemodBox& operator=(const DemodBox&) = delete;
};

} // namespace

static uint32_t g_rand_state = 0x0DE30D5Eu;

static uint32_t
rand_u32(void) {
    uint32_t x = g_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rand_state = x;
    return x;
}

/* Uniform in [-1, 1]. */
static double
rand_unit(void) {
    return ((double)(rand_u32() >> 8) / 16777215.0) * 2.0 - 1.0;
}

/* The capture: an FM tone at 0 Hz (1 kHz at 3 kHz deviation), an unmodulated carrier at +25 kHz and noise. */
static std::vector<float>
make_capture(void) {
    const double two_pi = 6.28318530717958647692;
    const double fs = (double)kCaptureRateHz;
    std::vector<float> iq((size_t)kStreamSamples * 2U);
    g_rand_state = 0x0DE30D5Eu;
    for (int n = 0; n < kStreamSamples; n++) {
        const double t = (double)n / fs;
        const double phase = (3000.0 / 1000.0) * std::sin(two_pi * 1000.0 * t);
        const double adjacent = two_pi * 25000.0 * t;
        const double i = 0.5 * std::cos(phase) + 0.3 * std::cos(adjacent) + 0.02 * rand_unit();
        const double q = 0.5 * std::sin(phase) + 0.3 * std::sin(adjacent) + 0.02 * rand_unit();
        iq[(size_t)n * 2U] = (float)i;
        iq[(size_t)n * 2U + 1U] = (float)q;
    }
    return iq;
}

static void
configure(demod_state* s, const Config& config) {
    s->rate_in = kCaptureRateHz;
    s->rate_out = kDemodRateHz;
    s->downsample_passes = kPasses;
    s->mode_demod = config.fm ? &dsd_fm_demod : &raw_demod;
    s->output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    s->channel_lpf_enable = 1;
    if (config.analog) {
        s->analog_family = 1;
        s->analog_demod = DSD_ANALOG_DEMOD_FM;
        s->channel_lpf_profile = DSD_CH_LPF_PROFILE_WIDE;
        s->channel_lpf_width_hz = 16000;
    } else {
        s->channel_lpf_profile = DSD_CH_LPF_PROFILE_12K5;
    }
    /* The per-block stages: I/Q balance off, no squelch, the envelope open. */
    s->iqbal_enable = 0;
    s->channel_squelch_level.store(0.0f);
    s->squelch_gate_open = 1;
    s->squelch_env = 1.0f;
}

/* The stream through a fresh demodulator, cut as @p split says; every output in order. */
static std::vector<float>
run_split(const Config& config, const std::vector<float>& iq, const Split& split, int* blocks) {
    std::vector<float> out;
    DemodBox box;
    if (!box.s) {
        return out;
    }
    configure(box.s, config);
    *blocks = 0;
    size_t i = 0U;
    int at = 0;
    while (at < kStreamSamples) {
        int size = i < split.sizes.size() ? split.sizes[i] : kStreamSamples - at;
        i++;
        if (size > kStreamSamples - at) {
            size = kStreamSamples - at;
        }
        DSD_MEMCPY(box.s->input_cb_buf, iq.data() + (size_t)at * 2U, (size_t)size * 2U * sizeof(float));
        box.s->lowpassed = box.s->input_cb_buf;
        box.s->lp_len = size * 2;
        full_demod(box.s);
        out.insert(out.end(), box.s->result, box.s->result + box.s->result_len);
        at += size;
        (*blocks)++;
    }
    return out;
}

static std::vector<Split>
make_splits(void) {
    std::vector<Split> splits;

    Split random_small{"seeded random sizes 1..4096", {}};
    g_rand_state = 0x5EED0001u;
    for (int total = 0; total < kStreamSamples;) {
        const int size = 1 + (int)(rand_u32() % 4096U);
        random_small.sizes.push_back(size);
        total += size;
    }
    splits.push_back(random_small);

    Split odd{"odd sizes cycled", {}};
    static const int kOdd[] = {1, 3, 5, 7, 31, 33, 63, 65, 255, 1023, 4097, 16385};
    for (int total = 0, k = 0; total < kStreamSamples; k++) {
        const int size = kOdd[(size_t)k % (sizeof(kOdd) / sizeof(kOdd[0]))];
        odd.sizes.push_back(size);
        total += size;
    }
    splits.push_back(odd);

    Split short_blocks{"sizes below 32, a run of 300 1-sample blocks, then the rest", {}};
    g_rand_state = 0x5EED0002u;
    for (int total = 0; total < 3000;) {
        const int size = 1 + (int)(rand_u32() % 31U);
        short_blocks.sizes.push_back(size);
        total += size;
    }
    short_blocks.sizes.insert(short_blocks.sizes.end(), 300U, 1);
    splits.push_back(short_blocks);

    Split random_large{"seeded random sizes 1..70000", {}};
    g_rand_state = 0x5EED0003u;
    for (int total = 0; total < kStreamSamples;) {
        const int size = 1 + (int)(rand_u32() % 70000U);
        random_large.sizes.push_back(size);
        total += size;
    }
    splits.push_back(random_large);

    splits.push_back(Split{"1-sample blocks between large ones", {32768, 1, 32767, 1, 1, 16383}});
    return splits;
}

static int
test_config(const Config& config, const std::vector<float>& iq) {
    std::printf("Testing full_demod() across block splits (%s)...\n", config.name);
    int blocks = 0;
    const std::vector<float> whole = run_split(config, iq, Split{"whole", {kStreamSamples}}, &blocks);
    double peak = 0.0;
    for (float v : whole) {
        peak = std::fmax(peak, std::fabs((double)v));
    }
    const double tol = config.fm ? kFmTolRad : kRawTolOfPeak * peak;
    std::printf("  whole stream: %zu outputs, peak %.4f, tolerance %.3e\n", whole.size(), peak, tol);
    if (whole.size() < 1000U || !(peak > 0.0)) {
        DSD_FPRINTF(stderr, "  FAIL: the whole stream made %zu outputs (peak %.4f)\n", whole.size(), peak);
        return 1;
    }
    int rc = 0;
    for (const Split& split : make_splits()) {
        const std::vector<float> got = run_split(config, iq, split, &blocks);
        const size_t common = got.size() < whole.size() ? got.size() : whole.size();
        double worst = 0.0;
        size_t worst_at = 0U;
        for (size_t k = 0; k < common; k++) {
            const double err = std::isnan((double)got[k]) ? INFINITY : std::fabs((double)got[k] - (double)whole[k]);
            if (err > worst) {
                worst = err;
                worst_at = k;
            }
        }
        std::printf("  %s: %d blocks, %zu outputs, deviation %.3e at output %zu\n", split.name, blocks, got.size(),
                    worst, worst_at);
        if (got.size() != whole.size() || !(worst <= tol)) {
            DSD_FPRINTF(stderr, "  FAIL: %s, %s: %zu outputs (the whole stream %zu), deviation %.3e (tolerance %.3e)\n",
                        config.name, split.name, got.size(), whole.size(), worst, tol);
            rc = 1;
        }
    }
    if (rc == 0) {
        std::printf("  PASS\n");
    }
    return rc;
}

int
main(void) {
    const std::vector<float> iq = make_capture();
    const Config configs[] = {
        {"12K5 profile, pass-through", 0, 0},
        {"12K5 profile, FM", 0, 1},
        {"analog NFM 16 kHz, pass-through", 1, 0},
        {"analog NFM 16 kHz, FM", 1, 1},
    };
    int failures = 0;
    for (const Config& config : configs) {
        failures += test_config(config, iq);
    }
    if (failures > 0) {
        std::printf("\n%d configuration(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll tests PASSED\n");
    return 0;
}
