// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Opt-in DSP microbenchmark harness.
 *
 * Build with:
 *   cmake --preset perf-bench
 *   cmake --build --preset perf-bench --target dsd-neo_bench_dsp -j
 *
 * Run from the build tree:
 *   build/perf-bench/tests/dsd-neo_bench_dsp --iters 3000 --repeat 5
 */

#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/equalizer.h>
#include <dsd-neo/dsp/fsk_modem.h>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/resampler.h>
#include <dsd-neo/dsp/simd_fir.h>
#include <dsd-neo/dsp/ted.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/mem.h>
#include <dsd-neo/runtime/ring.h>

#include <algorithm>
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

enum class OutputFormat { Text, Csv };

struct BenchOptions {
    int iterations = 2000;
    int warmup = 8;
    int repeat = 1;
    const char* case_filter = NULL;
    OutputFormat format = OutputFormat::Text;
    int list_cases = 0;
};

struct BenchStats {
    double min_ns_per_call = 0.0;
    double mean_ns_per_call = 0.0;
    double median_ns_per_call = 0.0;
    double median_ns_per_item = 0.0;
    double items_per_second = 0.0;
    float checksum = 0.0f;
};

struct DemodHolder {
    demod_state* s;

    DemodHolder() : s((demod_state*)dsd_neo_aligned_malloc(sizeof(demod_state))) {
        if (s) {
            std::memset(s, 0, sizeof(*s));
        }
    }

    ~DemodHolder() {
        if (!s) {
            return;
        }
        dsd_fsk_modem_release(&s->fsk_modem_state);
        if (s->post_polydecim_taps) {
            dsd_neo_aligned_free(s->post_polydecim_taps);
        }
        if (s->post_polydecim_hist) {
            dsd_neo_aligned_free(s->post_polydecim_hist);
        }
        if (s->resamp_taps) {
            dsd_neo_aligned_free(s->resamp_taps);
        }
        if (s->resamp_hist) {
            dsd_neo_aligned_free(s->resamp_hist);
        }
        dsd_neo_aligned_free(s);
    }

    operator bool() const { return s != NULL; }
};

static void
print_usage(const char* argv0) {
    std::printf("Usage: %s [--iters N] [--warmup N] [--repeat N] [--case NAME] [--format text|csv] [--list]\n", argv0);
}

static int
parse_positive_int(const char* value, int fallback) {
    if (!value) {
        return fallback;
    }

    char* end = NULL;
    long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0 || parsed > INT32_MAX) {
        return fallback;
    }
    return (int)parsed;
}

static int
parse_non_negative_int(const char* value, int fallback) {
    if (!value) {
        return fallback;
    }

    char* end = NULL;
    long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 0 || parsed > INT32_MAX) {
        return fallback;
    }
    return (int)parsed;
}

static BenchOptions
parse_options(int argc, char** argv) {
    BenchOptions opts;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            opts.iterations = parse_positive_int(argv[++i], opts.iterations);
        } else if (std::strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            opts.warmup = parse_non_negative_int(argv[++i], opts.warmup);
        } else if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            opts.repeat = parse_positive_int(argv[++i], opts.repeat);
        } else if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            opts.case_filter = argv[++i];
            if (std::strcmp(opts.case_filter, "all") == 0) {
                opts.case_filter = NULL;
            }
        } else if (std::strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            const char* fmt = argv[++i];
            if (std::strcmp(fmt, "csv") == 0) {
                opts.format = OutputFormat::Csv;
            } else {
                opts.format = OutputFormat::Text;
            }
        } else if (std::strcmp(argv[i], "--list") == 0) {
            opts.list_cases = 1;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            std::exit(0);
        }
    }
    return opts;
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

static double
median_of(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    size_t mid = values.size() / 2U;
    if ((values.size() & 1U) != 0U) {
        return values[mid];
    }
    return 0.5 * (values[mid - 1U] + values[mid]);
}

static void
print_result(const BenchOptions& opts, const char* name, const char* item_unit, double work_items,
             const BenchStats& stats) {
    if (opts.format == OutputFormat::Csv) {
        std::printf("%s,%d,%d,%d,%.0f,%s,%.3f,%.3f,%.3f,%.6f,%.3f,%.9e,%s\n", name, opts.repeat, opts.iterations,
                    opts.warmup, work_items, item_unit, stats.median_ns_per_call, stats.min_ns_per_call,
                    stats.mean_ns_per_call, stats.median_ns_per_item, stats.items_per_second, (double)stats.checksum,
                    simd_fir_get_impl_name());
        return;
    }

    std::printf("%-36s repeat=%2d median=%9.2f us/call min=%9.2f mean=%9.2f ns/item=%9.3f item=%-8s ips=%10.1f "
                "checksum=% .6e\n",
                name, opts.repeat, stats.median_ns_per_call / 1000.0, stats.min_ns_per_call / 1000.0,
                stats.mean_ns_per_call / 1000.0, stats.median_ns_per_item, item_unit, stats.items_per_second,
                (double)stats.checksum);
}

template <typename Fn>
static int
run_case(const BenchOptions& opts, const char* name, const char* item_unit, double work_items, Fn fn) {
    if (opts.case_filter && std::strcmp(opts.case_filter, name) != 0) {
        return 0;
    }
    if (opts.list_cases) {
        std::printf("%s\n", name);
        return 1;
    }

    std::vector<double> ns_per_call;
    ns_per_call.reserve((size_t)opts.repeat);
    float checksum = 0.0f;

    for (int r = 0; r < opts.repeat; r++) {
        for (int i = 0; i < opts.warmup; i++) {
            checksum += fn();
        }

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < opts.iterations; i++) {
            checksum += fn();
        }
        auto end = std::chrono::steady_clock::now();

        double ns = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        ns_per_call.push_back(ns / (double)opts.iterations);
    }

    BenchStats stats;
    stats.checksum = checksum;
    stats.min_ns_per_call = *std::min_element(ns_per_call.begin(), ns_per_call.end());
    double sum = 0.0;
    for (double v : ns_per_call) {
        sum += v;
    }
    stats.mean_ns_per_call = sum / (double)ns_per_call.size();
    stats.median_ns_per_call = median_of(ns_per_call);
    stats.median_ns_per_item = (work_items > 0.0) ? (stats.median_ns_per_call / work_items) : 0.0;
    stats.items_per_second = (stats.median_ns_per_item > 0.0) ? (1000000000.0 / stats.median_ns_per_item) : 0.0;

    g_bench_sink += checksum;
    print_result(opts, name, item_unit, work_items, stats);
    return 1;
}

static int
bench_input_ring(const BenchOptions& opts) {
    int ran = 0;
    constexpr size_t kBlock = 8192;
    std::vector<float> in(kBlock);
    std::vector<float> out(kBlock);
    fill_noise(&in, 0x1234u);

    input_ring_state ring;
    if (input_ring_init(&ring, kBlock + 1U) != 0) {
        std::fprintf(stderr, "input_ring_init failed\n");
        return ran;
    }

    ran += run_case(opts, "input_ring_read_block", "sample", (double)kBlock, [&]() -> float {
        input_ring_clear(&ring);
        input_ring_write(&ring, in.data(), kBlock);
        int got = input_ring_read_block(&ring, out.data(), kBlock);
        return out[0] + out[kBlock - 1] + (float)got;
    });

    ran += run_case(opts, "input_ring_read_reserve", "sample", (double)kBlock, [&]() -> float {
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
    return ran;
}

static int
bench_output_ring(const BenchOptions& opts) {
    int ran = 0;
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
        return ran;
    }
    dsd_cond_init(&ring.ready);
    dsd_cond_init(&ring.space);
    dsd_mutex_init(&ring.ready_m);
    ring.head.store(0);
    ring.tail.store(0);
    ring.write_timeouts.store(0);
    ring.read_timeouts.store(0);

    ran += run_case(opts, "output_ring_read_one", "sample", (double)kBlock, [&]() -> float {
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

    ran += run_case(opts, "output_ring_read_batch", "sample", (double)kBlock, [&]() -> float {
        ring_clear(&ring);
        ring_write_no_signal(&ring, in.data(), kBlock);
        int got = ring_read_batch(&ring, out.data(), kBlock);
        return out[0] + out[(got > 0) ? (size_t)got - 1U : 0U] + (float)got;
    });

    dsd_mutex_destroy(&ring.ready_m);
    dsd_cond_destroy(&ring.ready);
    dsd_cond_destroy(&ring.space);
    std::free(ring.buffer);
    return ran;
}

static int
bench_fir(const BenchOptions& opts) {
    int ran = 0;
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

    ran += run_case(opts, "simd_fir_complex_apply", "pair", (double)kPairs, [&]() -> float {
        simd_fir_complex_apply(in.data(), kInLen, out.data(), hist_i.data(), hist_q.data(), taps.data(), kTaps);
        return out[0] + out[kInLen - 1] + hist_i[0] + hist_q[0];
    });

    std::vector<float> hb_hist_i(HB_TAPS - 1, 0.0f);
    std::vector<float> hb_hist_q(HB_TAPS - 1, 0.0f);
    ran += run_case(opts, "simd_hb_decim2_complex", "pair", (double)kPairs, [&]() -> float {
        int got = simd_hb_decim2_complex(in.data(), kInLen, out.data(), hb_hist_i.data(), hb_hist_q.data(), hb_q15_taps,
                                         HB_TAPS);
        return out[0] + out[(got > 0) ? got - 1 : 0] + (float)got;
    });

    std::vector<float> real_in(kInLen);
    std::vector<float> real_out(kInLen / 2);
    std::vector<float> real_hist(HB_TAPS - 1, 0.0f);
    fill_noise(&real_in, 0x3579u);
    ran += run_case(opts, "simd_hb_decim2_real", "sample", (double)kInLen, [&]() -> float {
        int got = simd_hb_decim2_real(real_in.data(), kInLen, real_out.data(), real_hist.data(), hb_q15_taps, HB_TAPS);
        return real_out[0] + real_out[(got > 0) ? got - 1 : 0] + (float)got;
    });

    return ran;
}

static int
bench_kernel_demods(const BenchOptions& opts) {
    int ran = 0;
    constexpr int kPairs = 4096;
    constexpr int kInLen = kPairs * 2;

    std::vector<float> fm_iq(kInLen);
    std::vector<float> qpsk_iq(kInLen);
    std::vector<float> fsk_iq(kInLen);
    std::vector<float> fsk_out(kPairs);
    fill_fsk_iq(&fm_iq, 10, 0.028f);
    fill_cqpsk_iq(&qpsk_iq, 5);
    fill_fsk_iq(&fsk_iq, 10, 0.028f);

    DemodHolder fm_state;
    DemodHolder qpsk_state;
    if (!fm_state || !qpsk_state) {
        std::fprintf(stderr, "demod_state allocation failed\n");
        return ran;
    }

    fm_state.s->lowpassed = fm_iq.data();
    fm_state.s->lp_len = kInLen;
    fm_state.s->fll_enabled = 0;

    ran += run_case(opts, "dsd_fm_demod", "pair", (double)kPairs, [&]() -> float {
        dsd_fm_demod(fm_state.s);
        return fm_state.s->result[0] + fm_state.s->result[(fm_state.s->result_len > 0) ? fm_state.s->result_len - 1 : 0]
               + (float)fm_state.s->result_len;
    });

    qpsk_state.s->lowpassed = qpsk_iq.data();
    qpsk_state.s->lp_len = kInLen;
    ran += run_case(opts, "qpsk_differential_demod", "pair", (double)kPairs, [&]() -> float {
        qpsk_differential_demod(qpsk_state.s);
        return qpsk_state.s->result[0]
               + qpsk_state.s->result[(qpsk_state.s->result_len > 0) ? qpsk_state.s->result_len - 1 : 0]
               + (float)qpsk_state.s->result_len;
    });

    dsd_fsk_modem_state fsk_state = {};
    dsd_fsk_modem_config fsk_cfg = {};
    fsk_cfg.sample_rate_hz = 48000;
    fsk_cfg.symbol_rate_hz = 4800;
    fsk_cfg.levels = 4;
    fsk_cfg.channel_profile = DSD_CH_LPF_PROFILE_P25_C4FM;
    dsd_fsk_modem_init(&fsk_state, &fsk_cfg);
    ran += run_case(opts, "dsd_fsk_modem_process", "pair", (double)kPairs, [&]() -> float {
        int got = dsd_fsk_modem_process(&fsk_state, fsk_iq.data(), kInLen, fsk_out.data(), (int)fsk_out.size());
        return fsk_out[0] + fsk_out[(got > 0) ? got - 1 : 0] + (float)got;
    });
    dsd_fsk_modem_release(&fsk_state);

    dsd_resampler_state resampler = {};
    std::vector<float> resamp_in(kInLen);
    std::vector<float> resamp_out(kInLen * 2);
    fill_noise(&resamp_in, 0x7788u);
    if (dsd_resampler_design(&resampler, 1, 2)) {
        ran += run_case(opts, "dsd_resampler_process_block", "sample", (double)kInLen, [&]() -> float {
            int got = dsd_resampler_process_block(&resampler, resamp_in.data(), (int)resamp_in.size(),
                                                  resamp_out.data(), (int)resamp_out.size());
            return resamp_out[0] + resamp_out[(got > 0) ? got - 1 : 0] + (float)got;
        });
    }
    dsd_resampler_reset(&resampler);

    return ran;
}

static int
bench_carrier_loops(const BenchOptions& opts) {
    int ran = 0;
    constexpr int kPairs = 4096;
    constexpr int kInLen = kPairs * 2;

    std::vector<float> fll_iq(kInLen);
    std::vector<float> costas_iq(kInLen);
    fill_rotating_iq(&fll_iq, 0.0125f);
    fill_rotating_iq(&costas_iq, 0.018f);

    DemodHolder fll_state;
    DemodHolder costas_state;
    if (!fll_state || !costas_state) {
        std::fprintf(stderr, "demod_state allocation failed\n");
        return ran;
    }

    fll_state.s->cqpsk_enable = 1;
    fll_state.s->lowpassed = fll_iq.data();
    fll_state.s->lp_len = kInLen;
    fll_state.s->ted_sps = 5;
    fll_state.s->rate_out = 24000;

    ran += run_case(opts, "op25_fll_band_edge_cc", "pair", (double)kPairs, [&]() -> float {
        op25_fll_band_edge_cc(fll_state.s);
        return fll_iq[0] + fll_iq[kInLen - 1] + fll_state.s->fll_band_edge_state.freq;
    });

    costas_state.s->cqpsk_enable = 1;
    costas_state.s->lowpassed = costas_iq.data();
    costas_state.s->lp_len = kInLen;
    costas_state.s->ted_sps = 5;
    costas_state.s->rate_out = 24000;

    ran += run_case(opts, "op25_costas_loop_cc", "pair", (double)kPairs, [&]() -> float {
        op25_costas_loop_cc(costas_state.s);
        return costas_iq[0] + costas_iq[kInLen - 1] + costas_state.s->costas_state.freq;
    });

    return ran;
}

static void
configure_common_fsk_state(demod_state* s, int sample_rate, int symbol_rate, int channel_lpf_enable, int squelched) {
    s->rate_in = sample_rate;
    s->rate_out = sample_rate;
    s->rate_out2 = 0;
    s->lowpassed = s->input_cb_buf;
    s->mode_demod = &dsd_fm_demod;
    s->output_kind = DSD_DEMOD_OUTPUT_SYMBOL_FSK;
    s->symbol_rate_hz = symbol_rate;
    s->symbol_levels = 4;
    s->ted_sps = sample_rate / symbol_rate;
    s->sps_is_integer = (s->ted_sps * symbol_rate == sample_rate) ? 1 : 0;
    s->channel_lpf_enable = channel_lpf_enable;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_C4FM;
    s->channel_squelch_level = squelched ? 1000.0f : 0.0f;
    s->squelch_env = squelched ? 0.0f : 1.0f;
    s->squelch_env_attack = 0.125f;
    s->squelch_env_release = 0.03125f;

    dsd_fsk_modem_config cfg = {};
    cfg.sample_rate_hz = s->rate_out;
    cfg.symbol_rate_hz = s->symbol_rate_hz;
    cfg.levels = s->symbol_levels;
    cfg.channel_profile = s->channel_lpf_profile;
    dsd_fsk_modem_init(&s->fsk_modem_state, &cfg);
}

static void
configure_common_c4fm_audio_state(demod_state* s, int sample_rate, int symbol_rate, int channel_lpf_enable,
                                  int squelched) {
    s->rate_in = sample_rate;
    s->rate_out = sample_rate;
    s->rate_out2 = 0;
    s->lowpassed = s->input_cb_buf;
    s->mode_demod = &dsd_fm_demod;
    s->output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    s->symbol_rate_hz = symbol_rate;
    s->symbol_levels = 4;
    s->ted_sps = sample_rate / symbol_rate;
    s->sps_is_integer = (s->ted_sps * symbol_rate == sample_rate) ? 1 : 0;
    s->channel_lpf_enable = channel_lpf_enable;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_C4FM;
    s->channel_squelch_level = squelched ? 1000.0f : 0.0f;
    s->squelch_env = squelched ? 0.0f : 1.0f;
    s->squelch_env_attack = 0.125f;
    s->squelch_env_release = 0.03125f;
    s->iq_dc_block_enable = 1;
    s->iq_dc_shift = 11;
    s->fm_agc_enable = 1;
    s->fm_agc_target_rms = 0.30f;
    s->fm_agc_min_rms = 0.06f;
    s->fm_agc_alpha_up = 0.25f;
    s->fm_agc_alpha_down = 0.75f;
    s->output_scale = 1.0f;
}

static void
configure_common_cqpsk_state(demod_state* s, int sample_rate, int symbol_rate, int sps, int eq_enable) {
    s->rate_in = sample_rate;
    s->rate_out = sample_rate;
    s->lowpassed = s->input_cb_buf;
    s->mode_demod = &dsd_fm_demod;
    s->output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    s->cqpsk_enable = 1;
    s->symbol_rate_hz = symbol_rate;
    s->symbol_levels = 4;
    s->ted_sps = sps;
    s->sps_is_integer = 1;
    s->channel_lpf_enable = 1;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_CQPSK;
    s->cqpsk_diff_prev_r = 1.0f;
    s->cqpsk_diff_prev_j = 0.0f;
    s->squelch_env = 1.0f;
    s->squelch_env_attack = 0.125f;
    s->squelch_env_release = 0.03125f;
    s->cqpsk_eq_enable = eq_enable;
    s->cqpsk_eq_taps = DSD_CQPSK_CMA_EQ_DEFAULT_TAPS;
    s->cqpsk_eq_mu = DSD_CQPSK_CMA_EQ_DEFAULT_MU;
    s->cqpsk_eq_modulus = DSD_CQPSK_CMA_EQ_DEFAULT_MODULUS;
    ted_init_state(&s->ted_state);
}

static int
bench_one_fsk_full_demod(const BenchOptions& opts, const char* name, int sample_rate, int symbol_rate,
                         int channel_lpf_enable, int squelched) {
    const int sps = sample_rate / symbol_rate;
    const int symbols = 512;
    const int in_len = symbols * sps * 2;
    std::vector<float> in((size_t)in_len);
    fill_fsk_iq(&in, sps, 0.028f);

    DemodHolder state;
    if (!state) {
        std::fprintf(stderr, "demod_state allocation failed\n");
        return 0;
    }
    configure_common_fsk_state(state.s, sample_rate, symbol_rate, channel_lpf_enable, squelched);

    return run_case(opts, name, "symbol", (double)symbols, [&]() -> float {
        std::memcpy(state.s->input_cb_buf, in.data(), in.size() * sizeof(float));
        state.s->lowpassed = state.s->input_cb_buf;
        state.s->lp_len = in_len;
        full_demod(state.s);
        return state.s->result[0] + state.s->result[(state.s->result_len > 0) ? state.s->result_len - 1 : 0]
               + (float)state.s->result_len;
    });
}

static int
bench_one_c4fm_audio_full_demod(const BenchOptions& opts, const char* name, int sample_rate, int symbol_rate,
                                int channel_lpf_enable, int squelched) {
    const int sps = sample_rate / symbol_rate;
    const int symbols = 512;
    const int in_len = symbols * sps * 2;
    std::vector<float> in((size_t)in_len);
    fill_fsk_iq(&in, sps, 0.028f);

    DemodHolder state;
    if (!state) {
        std::fprintf(stderr, "demod_state allocation failed\n");
        return 0;
    }
    configure_common_c4fm_audio_state(state.s, sample_rate, symbol_rate, channel_lpf_enable, squelched);

    return run_case(opts, name, "symbol", (double)symbols, [&]() -> float {
        std::memcpy(state.s->input_cb_buf, in.data(), in.size() * sizeof(float));
        state.s->lowpassed = state.s->input_cb_buf;
        state.s->lp_len = in_len;
        full_demod(state.s);
        return state.s->result[0] + state.s->result[(state.s->result_len > 0) ? state.s->result_len - 1 : 0]
               + (float)state.s->result_len;
    });
}

static int
bench_one_cqpsk_full_demod(const BenchOptions& opts, const char* name, int sample_rate, int symbol_rate, int sps,
                           int eq_enable) {
    const int symbols = 512;
    const int in_len = symbols * sps * 2;
    std::vector<float> in((size_t)in_len);
    fill_cqpsk_iq(&in, sps);

    DemodHolder state;
    if (!state) {
        std::fprintf(stderr, "demod_state allocation failed\n");
        return 0;
    }
    configure_common_cqpsk_state(state.s, sample_rate, symbol_rate, sps, eq_enable);

    return run_case(opts, name, "symbol", (double)symbols, [&]() -> float {
        std::memcpy(state.s->input_cb_buf, in.data(), in.size() * sizeof(float));
        state.s->lowpassed = state.s->input_cb_buf;
        state.s->lp_len = in_len;
        full_demod(state.s);
        return state.s->result[0] + state.s->result[(state.s->result_len > 0) ? state.s->result_len - 1 : 0]
               + (float)state.s->result_len;
    });
}

static int
bench_full_demod(const BenchOptions& opts) {
    int ran = 0;

    ran += bench_one_c4fm_audio_full_demod(opts, "full_demod_c4fm_24k_lpf_on", 24000, 4800, 1, 0);
    ran += bench_one_c4fm_audio_full_demod(opts, "full_demod_c4fm_24k_lpf_off", 24000, 4800, 0, 0);
    ran += bench_one_c4fm_audio_full_demod(opts, "full_demod_c4fm_24k_squelched", 24000, 4800, 1, 1);
    ran += bench_one_c4fm_audio_full_demod(opts, "full_demod_c4fm_48k_lpf_on", 48000, 4800, 1, 0);
    ran += bench_one_c4fm_audio_full_demod(opts, "full_demod_c4fm_48k_lpf_off", 48000, 4800, 0, 0);
    ran += bench_one_c4fm_audio_full_demod(opts, "full_demod_c4fm_48k_squelched", 48000, 4800, 1, 1);

    ran += bench_one_fsk_full_demod(opts, "full_demod_fsk_24k_lpf_on", 24000, 4800, 1, 0);
    ran += bench_one_fsk_full_demod(opts, "full_demod_fsk_24k_lpf_off", 24000, 4800, 0, 0);
    ran += bench_one_fsk_full_demod(opts, "full_demod_fsk_24k_squelched", 24000, 4800, 1, 1);
    ran += bench_one_fsk_full_demod(opts, "full_demod_fsk_48k_lpf_on", 48000, 4800, 1, 0);
    ran += bench_one_fsk_full_demod(opts, "full_demod_fsk_48k_lpf_off", 48000, 4800, 0, 0);
    ran += bench_one_fsk_full_demod(opts, "full_demod_fsk_48k_squelched", 48000, 4800, 1, 1);

    ran += bench_one_cqpsk_full_demod(opts, "full_demod_cqpsk_p25p1", 24000, 4800, 5, 0);
    ran += bench_one_cqpsk_full_demod(opts, "full_demod_cqpsk_p25p1_eq", 24000, 4800, 5, 1);
    ran += bench_one_cqpsk_full_demod(opts, "full_demod_cqpsk_p25p2", 48000, 6000, 8, 0);
    ran += bench_one_cqpsk_full_demod(opts, "full_demod_cqpsk_p25p2_eq", 48000, 6000, 8, 1);

    return ran;
}

} /* namespace */

int
main(int argc, char** argv) {
    BenchOptions opts = parse_options(argc, argv);
    dsd_neo_config_init(NULL);

    if (opts.format == OutputFormat::Text && !opts.list_cases) {
        std::printf("DSD-neo DSP benchmark\n");
        std::printf("iterations=%d warmup=%d repeat=%d simd_fir_impl=%s\n\n", opts.iterations, opts.warmup, opts.repeat,
                    simd_fir_get_impl_name());
    } else if (opts.format == OutputFormat::Csv && !opts.list_cases) {
        std::printf("case,repeat,iters,warmup,work_items,item_unit,median_ns_per_call,min_ns_per_call,"
                    "mean_ns_per_call,median_ns_per_item,items_per_second,checksum,simd_impl\n");
    }

    int ran = 0;
    ran += bench_input_ring(opts);
    ran += bench_output_ring(opts);
    ran += bench_fir(opts);
    ran += bench_kernel_demods(opts);
    ran += bench_carrier_loops(opts);
    ran += bench_full_demod(opts);

    if (opts.case_filter && ran == 0) {
        std::fprintf(stderr, "No benchmark case matched '%s'. Use --list to see available cases.\n", opts.case_filter);
        return 2;
    }

    if (!opts.list_cases && opts.format == OutputFormat::Text) {
        std::printf("\nsink=% .6e\n", (double)g_bench_sink);
    }
    return 0;
}
