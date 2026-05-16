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
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/fsk_modem.h>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/simd_fir.h>
#include <dsd-neo/dsp/ted.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/ring.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>
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
fill_fsk_iq(std::vector<float>* v, int sps, float deviation_per_level) {
    static const float levels[] = {-3.0f, -1.0f, 1.0f, 3.0f, 1.0f, -1.0f};
    float phase = 0.19f;
    const int pairs = (int)(v->size() >> 1);
    for (int n = 0; n < pairs; n++) {
        float sym = levels[(n / sps) % (int)(sizeof(levels) / sizeof(levels[0]))];
        phase += deviation_per_level * sym;
        if (phase > kPi) {
            phase -= 2.0f * kPi;
        } else if (phase < -kPi) {
            phase += 2.0f * kPi;
        }
        (*v)[(size_t)n * 2] = 0.85f * std::cos(phase);
        (*v)[(size_t)n * 2 + 1] = 0.85f * std::sin(phase);
    }
}

static void
fill_cqpsk_iq(std::vector<float>* v, int sps) {
    static const float symbols[] = {-3.0f, -1.0f, 1.0f, 3.0f};
    float phase = 0.0f;
    const int pairs = (int)(v->size() >> 1);
    for (int n = 0; n < pairs; n++) {
        if ((n % sps) == 0) {
            phase += symbols[(n / sps) & 3] * (kPi / 4.0f);
            while (phase > kPi) {
                phase -= 2.0f * kPi;
            }
            while (phase < -kPi) {
                phase += 2.0f * kPi;
            }
        }
        (*v)[(size_t)n * 2] = std::cos(phase);
        (*v)[(size_t)n * 2 + 1] = std::sin(phase);
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
bench_output_ring(int iterations) {
    constexpr size_t kBlock = 8192;
    std::vector<float> in(kBlock);
    std::vector<float> out(kBlock);
    fill_noise(&in, 0x4567u);

    output_state ring = {};
    ring.rate = 48000;
    ring.capacity = kBlock + 1U;
    ring.buffer = (float*)std::calloc(ring.capacity, sizeof(float));
    if (!ring.buffer) {
        std::fprintf(stderr, "output ring allocation failed\n");
        return;
    }
    dsd_cond_init(&ring.ready);
    dsd_cond_init(&ring.space);
    dsd_mutex_init(&ring.ready_m);
    ring.head.store(0);
    ring.tail.store(0);
    ring.write_timeouts.store(0);
    ring.read_timeouts.store(0);

    run_case("output_ring_read_one", iterations, (double)kBlock, [&]() -> float {
        ring_clear(&ring);
        ring_write_no_signal(&ring, in.data(), kBlock);
        float sum = 0.0f;
        for (size_t i = 0; i < kBlock; i++) {
            float sample = 0.0f;
            if (ring_read_one(&ring, &sample) == 0) {
                sum += sample;
            }
        }
        return sum;
    });

    run_case("output_ring_read_batch", iterations, (double)kBlock, [&]() -> float {
        ring_clear(&ring);
        ring_write_no_signal(&ring, in.data(), kBlock);
        int got = ring_read_batch(&ring, out.data(), kBlock);
        return out[0] + out[(got > 0) ? (size_t)got - 1U : 0U] + (float)got;
    });

    dsd_mutex_destroy(&ring.ready_m);
    dsd_cond_destroy(&ring.ready);
    dsd_cond_destroy(&ring.space);
    std::free(ring.buffer);
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

static void
bench_full_demod(int iterations) {
    constexpr int kFskSps = 10;
    constexpr int kFskSymbols = 512;
    constexpr int kFskInLen = kFskSymbols * kFskSps * 2;
    constexpr int kCqpskSps = 5;
    constexpr int kCqpskSymbols = 512;
    constexpr int kCqpskInLen = kCqpskSymbols * kCqpskSps * 2;

    std::vector<float> fsk_in(kFskInLen);
    std::vector<float> cqpsk_in(kCqpskInLen);
    fill_fsk_iq(&fsk_in, kFskSps, 0.028f);
    fill_cqpsk_iq(&cqpsk_in, kCqpskSps);

    demod_state* fsk_state = (demod_state*)std::calloc(1, sizeof(*fsk_state));
    demod_state* cqpsk_state = (demod_state*)std::calloc(1, sizeof(*cqpsk_state));
    if (!fsk_state || !cqpsk_state) {
        std::fprintf(stderr, "demod_state allocation failed\n");
        std::free(fsk_state);
        std::free(cqpsk_state);
        return;
    }

    fsk_state->rate_in = 48000;
    fsk_state->rate_out = 48000;
    fsk_state->rate_out2 = 12000;
    fsk_state->lowpassed = fsk_state->input_cb_buf;
    fsk_state->mode_demod = &dsd_fm_demod;
    fsk_state->output_kind = DSD_DEMOD_OUTPUT_SYMBOL_FSK;
    fsk_state->symbol_rate_hz = 4800;
    fsk_state->symbol_levels = 4;
    fsk_state->channel_lpf_enable = 1;
    fsk_state->channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_C4FM;
    {
        dsd_fsk_modem_config cfg = {};
        cfg.sample_rate_hz = fsk_state->rate_out;
        cfg.symbol_rate_hz = fsk_state->symbol_rate_hz;
        cfg.levels = fsk_state->symbol_levels;
        cfg.channel_profile = fsk_state->channel_lpf_profile;
        dsd_fsk_modem_init(&fsk_state->fsk_modem_state, &cfg);
    }

    cqpsk_state->rate_in = 24000;
    cqpsk_state->rate_out = 24000;
    cqpsk_state->lowpassed = cqpsk_state->input_cb_buf;
    cqpsk_state->mode_demod = &dsd_fm_demod;
    cqpsk_state->output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    cqpsk_state->cqpsk_enable = 1;
    cqpsk_state->symbol_rate_hz = 4800;
    cqpsk_state->symbol_levels = 4;
    cqpsk_state->ted_sps = kCqpskSps;
    cqpsk_state->sps_is_integer = 1;
    cqpsk_state->channel_lpf_enable = 1;
    cqpsk_state->channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_CQPSK;
    cqpsk_state->cqpsk_diff_prev_r = 1.0f;
    cqpsk_state->cqpsk_diff_prev_j = 0.0f;
    ted_init_state(&cqpsk_state->ted_state);

    run_case("full_demod_symbol_fsk", iterations, (double)kFskSymbols, [&]() -> float {
        std::memcpy(fsk_state->input_cb_buf, fsk_in.data(), fsk_in.size() * sizeof(float));
        fsk_state->lowpassed = fsk_state->input_cb_buf;
        fsk_state->lp_len = kFskInLen;
        full_demod(fsk_state);
        return fsk_state->result[0] + fsk_state->result[(fsk_state->result_len > 0) ? fsk_state->result_len - 1 : 0]
               + (float)fsk_state->result_len;
    });

    run_case("full_demod_symbol_cqpsk", iterations, (double)kCqpskSymbols, [&]() -> float {
        std::memcpy(cqpsk_state->input_cb_buf, cqpsk_in.data(), cqpsk_in.size() * sizeof(float));
        cqpsk_state->lowpassed = cqpsk_state->input_cb_buf;
        cqpsk_state->lp_len = kCqpskInLen;
        full_demod(cqpsk_state);
        return cqpsk_state->result[0]
               + cqpsk_state->result[(cqpsk_state->result_len > 0) ? cqpsk_state->result_len - 1 : 0]
               + (float)cqpsk_state->result_len;
    });

    std::free(fsk_state);
    std::free(cqpsk_state);
}

} /* namespace */

int
main(int argc, char** argv) {
    int iterations = parse_iterations(argc, argv);
    dsd_neo_config_init(NULL);

    std::printf("DSD-neo DSP benchmark\n");
    std::printf("iterations=%d simd_fir_impl=%s\n\n", iterations, simd_fir_get_impl_name());

    bench_input_ring(iterations);
    bench_output_ring(iterations);
    bench_fir(iterations);
    bench_carrier_loops(iterations);
    bench_full_demod(iterations);

    std::printf("\nsink=% .6e\n", (double)g_bench_sink);
    return 0;
}
