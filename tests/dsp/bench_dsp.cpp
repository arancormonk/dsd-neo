// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Opt-in DSP microbenchmark harness.
 *
 * Build with:
 *   cmake --build --preset dev-debug --target dsd-neo_bench_dsp -j
 *
 * Run from the build tree:
 *   build/dev-debug/tests/dsd-neo_bench_dsp --iters 2000
 */

#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/simd_fir.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/input_ring.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int
dsd_rtl_stream_should_exit(void) {
    return 0;
}

namespace {

constexpr float kPi = 3.14159265358979323846f;
volatile float g_bench_sink = 0.0f;

static int
parse_iterations(int argc, char** argv) {
    int iterations = 2000;
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--iters") == 0) {
            int parsed = std::atoi(argv[i + 1]);
            if (parsed > 0) {
                iterations = parsed;
            }
            i++;
        }
    }
    return iterations;
}

static float
next_lcg_float(uint32_t* state) {
    *state = (*state * 1664525u) + 1013904223u;
    return ((float)((*state >> 8) & 0x00ffffffu) * (1.0f / 8388608.0f)) - 1.0f;
}

static void
fill_noise(std::vector<float>* v, uint32_t seed) {
    for (size_t i = 0; i < v->size(); i++) {
        (*v)[i] = next_lcg_float(&seed);
    }
}

static void
fill_rotating_iq(std::vector<float>* v, float step) {
    float phase = 0.0f;
    const int pairs = (int)(v->size() >> 1);
    for (int i = 0; i < pairs; i++) {
        (*v)[(size_t)i * 2] = 0.7f * std::cos(phase);
        (*v)[(size_t)i * 2 + 1] = 0.7f * std::sin(phase);
        phase += step;
        if (phase > 2.0f * kPi) {
            phase -= 2.0f * kPi;
        }
    }
}

static void
make_symmetric_taps(std::vector<float>* taps) {
    const int n = (int)taps->size();
    const int center = n >> 1;
    for (int i = 0; i < n; i++) {
        int d = std::abs(i - center);
        float window = 0.54f + 0.46f * std::cos(kPi * (float)d / (float)center);
        (*taps)[i] = (d == 0) ? 0.25f : (0.015f * window / (float)(d + 1));
    }
}

template <typename Fn>
static void
run_case(const char* name, int iterations, double work_items, Fn fn) {
    float checksum = 0.0f;
    for (int i = 0; i < 8; i++) {
        checksum += fn();
    }

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; i++) {
        checksum += fn();
    }
    auto end = std::chrono::steady_clock::now();
    g_bench_sink += checksum;

    double ns = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double ns_per_call = ns / (double)iterations;
    double ns_per_item = ns / ((double)iterations * work_items);
    std::printf("%-32s %10.2f us/call %9.3f ns/item  checksum=% .6e\n", name, ns_per_call / 1000.0, ns_per_item,
                (double)checksum);
}

static void
bench_input_ring(int iterations) {
    constexpr size_t kBlock = 8192;
    std::vector<float> in(kBlock);
    std::vector<float> out(kBlock);
    fill_noise(&in, 0x1234u);

    input_ring_state ring;
    if (input_ring_init(&ring, kBlock + 1U) != 0) {
        std::fprintf(stderr, "input_ring_init failed\n");
        return;
    }

    run_case("input_ring_read_block", iterations, (double)kBlock, [&]() -> float {
        input_ring_clear(&ring);
        input_ring_write(&ring, in.data(), kBlock);
        int got = input_ring_read_block(&ring, out.data(), kBlock);
        return out[0] + out[kBlock - 1] + (float)got;
    });

    run_case("input_ring_read_reserve", iterations, (double)kBlock, [&]() -> float {
        input_ring_clear(&ring);
        input_ring_write(&ring, in.data(), kBlock);
        float* p1 = NULL;
        float* p2 = NULL;
        size_t n1 = 0;
        size_t n2 = 0;
        int got = input_ring_read_reserve(&ring, kBlock, &p1, &n1, &p2, &n2);
        float sum = (p1 && n1 > 0) ? p1[0] + p1[n1 - 1] : 0.0f;
        if (p2 && n2 > 0) {
            sum += p2[0] + p2[n2 - 1];
        }
        input_ring_read_commit(&ring, (size_t)((got > 0) ? got : 0));
        return sum + (float)got;
    });

    input_ring_destroy(&ring);
}

static void
bench_fir(int iterations) {
    constexpr int kPairs = 4096;
    constexpr int kInLen = kPairs * 2;
    constexpr int kTaps = 63;

    std::vector<float> in(kInLen);
    std::vector<float> out(kInLen);
    std::vector<float> hist_i(kTaps - 1, 0.0f);
    std::vector<float> hist_q(kTaps - 1, 0.0f);
    std::vector<float> taps(kTaps);
    fill_noise(&in, 0x2468u);
    make_symmetric_taps(&taps);

    run_case("simd_fir_complex_apply", iterations, (double)kPairs, [&]() -> float {
        simd_fir_complex_apply(in.data(), kInLen, out.data(), hist_i.data(), hist_q.data(), taps.data(), kTaps);
        return out[0] + out[kInLen - 1] + hist_i[0] + hist_q[0];
    });

    std::vector<float> hb_hist_i(HB_TAPS - 1, 0.0f);
    std::vector<float> hb_hist_q(HB_TAPS - 1, 0.0f);
    run_case("simd_hb_decim2_complex", iterations, (double)kPairs, [&]() -> float {
        int got = simd_hb_decim2_complex(in.data(), kInLen, out.data(), hb_hist_i.data(), hb_hist_q.data(), hb_q15_taps,
                                         HB_TAPS);
        return out[0] + out[(got > 0) ? got - 1 : 0] + (float)got;
    });

    std::vector<float> real_in(kInLen);
    std::vector<float> real_out(kInLen / 2);
    std::vector<float> real_hist(HB_TAPS - 1, 0.0f);
    fill_noise(&real_in, 0x3579u);
    run_case("simd_hb_decim2_real", iterations, (double)kInLen, [&]() -> float {
        int got = simd_hb_decim2_real(real_in.data(), kInLen, real_out.data(), real_hist.data(), hb_q15_taps, HB_TAPS);
        return real_out[0] + real_out[(got > 0) ? got - 1 : 0] + (float)got;
    });
}

static void
bench_carrier_loops(int iterations) {
    constexpr int kPairs = 4096;
    constexpr int kInLen = kPairs * 2;

    std::vector<float> fll_iq(kInLen);
    std::vector<float> costas_iq(kInLen);
    fill_rotating_iq(&fll_iq, 0.0125f);
    fill_rotating_iq(&costas_iq, 0.018f);

    demod_state* fll_state = (demod_state*)std::calloc(1, sizeof(*fll_state));
    demod_state* costas_state = (demod_state*)std::calloc(1, sizeof(*costas_state));
    if (!fll_state || !costas_state) {
        std::fprintf(stderr, "demod_state allocation failed\n");
        std::free(fll_state);
        std::free(costas_state);
        return;
    }

    fll_state->cqpsk_enable = 1;
    fll_state->lowpassed = fll_iq.data();
    fll_state->lp_len = kInLen;
    fll_state->ted_sps = 5;
    fll_state->rate_out = 24000;

    run_case("op25_fll_band_edge_cc", iterations, (double)kPairs, [&]() -> float {
        op25_fll_band_edge_cc(fll_state);
        return fll_iq[0] + fll_iq[kInLen - 1] + fll_state->fll_band_edge_state.freq;
    });

    costas_state->cqpsk_enable = 1;
    costas_state->lowpassed = costas_iq.data();
    costas_state->lp_len = kInLen;
    costas_state->ted_sps = 5;
    costas_state->rate_out = 24000;

    run_case("op25_costas_loop_cc", iterations, (double)kPairs, [&]() -> float {
        op25_costas_loop_cc(costas_state);
        return costas_iq[0] + costas_iq[kInLen - 1] + costas_state->costas_state.freq;
    });

    std::free(fll_state);
    std::free(costas_state);
}

} /* namespace */

int
main(int argc, char** argv) {
    int iterations = parse_iterations(argc, argv);
    dsd_neo_config_init(NULL);

    std::printf("DSD-neo DSP benchmark\n");
    std::printf("iterations=%d simd_fir_impl=%s\n\n", iterations, simd_fir_get_impl_name());

    bench_input_ring(iterations);
    bench_fir(iterations);
    bench_carrier_loops(iterations);

    std::printf("\nsink=% .6e\n", (double)g_bench_sink);
    return 0;
}
