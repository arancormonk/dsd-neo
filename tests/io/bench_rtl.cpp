// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Opt-in RTL ingest and post-demod microbenchmark harness.
 *
 * Build with:
 *   cmake --preset perf-bench
 *   cmake --build --preset perf-bench --target dsd-neo_bench_rtl -j
 *
 * Run from the build tree:
 *   build/perf-bench/tests/dsd-neo_bench_rtl --iters 3000 --repeat 5
 */

#include <dsd-neo/dsp/simd_widen.h>
#include <dsd-neo/io/rtl_metrics.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/ring.h>

#include <algorithm>
#include <atomic>
// IWYU pragma: no_include <bits/chrono.h>
#include <chrono> // IWYU pragma: keep
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include <vector>

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
            opts.format = (std::strcmp(fmt, "csv") == 0) ? OutputFormat::Csv : OutputFormat::Text;
        } else if (std::strcmp(argv[i], "--list") == 0) {
            opts.list_cases = 1;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            std::exit(0);
        }
    }
    return opts;
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
        std::printf("%s,%d,%d,%d,%.0f,%s,%.3f,%.3f,%.3f,%.6f,%.3f,%.9e\n", name, opts.repeat, opts.iterations,
                    opts.warmup, work_items, item_unit, stats.median_ns_per_call, stats.min_ns_per_call,
                    stats.mean_ns_per_call, stats.median_ns_per_item, stats.items_per_second, (double)stats.checksum);
        return;
    }
    std::printf("%-38s repeat=%2d median=%9.2f us/call min=%9.2f mean=%9.2f ns/item=%9.3f item=%-8s ips=%10.1f "
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

static void
fill_cu8(std::vector<unsigned char>* v, uint32_t seed) {
    for (size_t i = 0; i < v->size(); i++) {
        seed = seed * 1664525u + 1013904223u;
        (*v)[i] = (unsigned char)((seed >> 24) & 0xffu);
    }
}

static void
fill_cs16(std::vector<int16_t>* v, uint32_t seed) {
    for (size_t i = 0; i < v->size(); i++) {
        seed = seed * 1664525u + 1013904223u;
        (*v)[i] = (int16_t)((int32_t)((seed >> 16) & 0xffffu) - 32768);
    }
}

static void
fill_rotating_iq(std::vector<float>* v, float step) {
    float phase = 0.0f;
    const int pairs = (int)(v->size() >> 1);
    for (int i = 0; i < pairs; i++) {
        (*v)[(size_t)i * 2U] = 0.7f * std::cos(phase);
        (*v)[(size_t)i * 2U + 1U] = 0.7f * std::sin(phase);
        phase += step;
        if (phase > 2.0f * kPi) {
            phase -= 2.0f * kPi;
        }
    }
}

static void
reset_ring_at(input_ring_state* ring, size_t index) {
    ring->tail.store(index);
    ring->head.store(index);
}

static uint32_t
process_u8_chunk(unsigned char* src, float* dst, size_t len, int combined, uint32_t phase) {
    if (combined) {
        return widen_rotate90_u8_to_f32_bias127_phase(src, dst, (uint32_t)len, phase);
    }
    phase = rotate90_u8_inplace_phase(src, (uint32_t)len, phase);
    widen_u8_to_f32_bias128_scalar(src, dst, (uint32_t)len);
    return phase;
}

static float
ingest_u8_block(input_ring_state* ring, unsigned char* src, size_t len, size_t start_index, int combined,
                uint32_t* phase) {
    reset_ring_at(ring, start_index);
    float *p1 = NULL, *p2 = NULL;
    size_t n1 = 0, n2 = 0;
    input_ring_reserve(ring, len, &p1, &n1, &p2, &n2);
    if (n1 & 1U) {
        n1--;
    }
    size_t w1 = (n1 < len) ? n1 : len;
    size_t rem = len - w1;
    if (n2 & 1U) {
        n2--;
    }
    size_t w2 = (n2 < rem) ? n2 : rem;

    if (w1 > 0U) {
        *phase = process_u8_chunk(src, p1, w1, combined, *phase);
    }
    if (w2 > 0U) {
        *phase = process_u8_chunk(src + w1, p2, w2, combined, *phase);
    }
    input_ring_commit(ring, w1 + w2);
    return (p1 ? p1[0] : 0.0f)
           + ((p2 && w2 > 0U)   ? p2[w2 - 1U]
              : (p1 && w1 > 0U) ? p1[w1 - 1U]
                                : 0.0f)
           + (float)(*phase);
}

static float
ingest_cs16_block(input_ring_state* ring, const int16_t* src, size_t complex_count, size_t start_index) {
    reset_ring_at(ring, start_index);
    const float scale = 1.0f / 32768.0f;
    const size_t need = complex_count * 2U;
    float *p1 = NULL, *p2 = NULL;
    size_t n1 = 0, n2 = 0;
    input_ring_reserve(ring, need, &p1, &n1, &p2, &n2);
    if (n1 & 1U) {
        n1--;
    }
    size_t w1 = (n1 < need) ? n1 : need;
    size_t rem = need - w1;
    if (n2 & 1U) {
        n2--;
    }
    size_t w2 = (n2 < rem) ? n2 : rem;

    for (size_t i = 0; i < (w1 / 2U); i++) {
        size_t sample_idx = i * 2U;
        p1[sample_idx + 0U] = (float)src[sample_idx + 0U] * scale;
        p1[sample_idx + 1U] = (float)src[sample_idx + 1U] * scale;
    }
    size_t base = w1;
    for (size_t i = 0; i < (w2 / 2U); i++) {
        size_t src_idx = base + (i * 2U);
        p2[(i * 2U) + 0U] = (float)src[src_idx + 0U] * scale;
        p2[(i * 2U) + 1U] = (float)src[src_idx + 1U] * scale;
    }
    input_ring_commit(ring, w1 + w2);
    return (p1 ? p1[0] : 0.0f) + ((p2 && w2 > 0U) ? p2[w2 - 1U] : (p1 && w1 > 0U) ? p1[w1 - 1U] : 0.0f);
}

static int
bench_rtl_ingest(const BenchOptions& opts) {
    int ran = 0;
    constexpr size_t kBytes = 16384;
    constexpr size_t kRingCap = kBytes + 513U;
    constexpr size_t kWrapStart = kRingCap - 128U;
    std::vector<unsigned char> u8(kBytes);
    std::vector<int16_t> cs16(kBytes);
    fill_cu8(&u8, 0x13579bdfu);
    fill_cs16(&cs16, 0x2468ace0u);

    input_ring_state ring;
    if (input_ring_init(&ring, kRingCap) != 0) {
        std::fprintf(stderr, "input_ring_init failed\n");
        return ran;
    }

    uint32_t combined_phase = 0;
    ran += run_case(opts, "rtl_ingest_u8_combined_contig", "byte", (double)kBytes,
                    [&]() -> float { return ingest_u8_block(&ring, u8.data(), kBytes, 0U, 1, &combined_phase); });

    uint32_t wrap_phase = 0;
    ran += run_case(opts, "rtl_ingest_u8_combined_wrap", "byte", (double)kBytes,
                    [&]() -> float { return ingest_u8_block(&ring, u8.data(), kBytes, kWrapStart, 1, &wrap_phase); });

    uint32_t two_pass_phase = 0;
    ran += run_case(opts, "rtl_ingest_u8_two_pass_contig", "byte", (double)kBytes,
                    [&]() -> float { return ingest_u8_block(&ring, u8.data(), kBytes, 0U, 0, &two_pass_phase); });

    ran += run_case(opts, "rtl_ingest_cs16_contig", "sample", (double)kBytes,
                    [&]() -> float { return ingest_cs16_block(&ring, cs16.data(), kBytes / 2U, 0U); });

    input_ring_destroy(&ring);
    return ran;
}

static int
bench_rtl_metrics(const BenchOptions& opts) {
    int ran = 0;
    std::vector<float> iq_1024(1024);
    std::vector<float> iq_4096(4096);
    fill_rotating_iq(&iq_1024, 0.017f);
    fill_rotating_iq(&iq_4096, 0.011f);

    ran += run_case(opts, "rtl_metrics_spectrum_1024", "sample", (double)iq_1024.size(), [&]() -> float {
        rtl_metrics_update_spectrum_from_iq(iq_1024.data(), (int)iq_1024.size(), 48000);
        return iq_1024[0] + iq_1024[iq_1024.size() - 1U];
    });

    ran += run_case(opts, "rtl_metrics_spectrum_4096", "sample", (double)iq_4096.size(), [&]() -> float {
        rtl_metrics_update_spectrum_from_iq(iq_4096.data(), (int)iq_4096.size(), 48000);
        return iq_4096[0] + iq_4096[iq_4096.size() - 1U];
    });
    return ran;
}

static int
bench_rtl_output(const BenchOptions& opts) {
    int ran = 0;
    constexpr size_t kBlock = 512;
    std::vector<float> in(kBlock);
    std::vector<float> out(kBlock);
    for (size_t i = 0; i < kBlock; i++) {
        in[i] = (float)((int)(i & 31U) - 16) * (1.0f / 16.0f);
    }

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

    ran += run_case(opts, "rtl_symbol_output_read_batch_512", "sample", (double)kBlock, [&]() -> float {
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

} // namespace

int
main(int argc, char** argv) {
    BenchOptions opts = parse_options(argc, argv);
    if (opts.format == OutputFormat::Csv && !opts.list_cases) {
        std::printf("case,repeat,iterations,warmup,work_items,item_unit,median_ns_per_call,min_ns_per_call,"
                    "mean_ns_per_call,median_ns_per_item,items_per_second,checksum\n");
    }

    int ran = 0;
    ran += bench_rtl_ingest(opts);
    ran += bench_rtl_metrics(opts);
    ran += bench_rtl_output(opts);

    if (!opts.list_cases && ran == 0) {
        std::fprintf(stderr, "No benchmark case matched.\n");
        return 2;
    }
    return (g_bench_sink == 1234567.0f) ? 1 : 0;
}
