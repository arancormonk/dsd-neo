// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Opt-in microbenchmark for the P25 Phase 1 1/2-rate list decoder.
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dsd-neo/protocol/p25/p25_12.h>
#include <stdint.h>
#include <vector>
#include "dsd-neo/core/safe_api.h"

namespace {

volatile double g_bench_sink = 0.0;

enum class OutputFormat : uint8_t { Text, Csv };

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
    double checksum = 0.0;
};

static const uint8_t k_reference_dibits[98] = {
    0U, 1U, 2U, 1U, 0U, 2U, 3U, 1U, 3U, 0U, 2U, 2U, 0U, 2U, 0U, 0U, 0U, 3U, 1U, 1U, 3U, 3U, 0U, 0U, 1U,
    1U, 1U, 3U, 0U, 1U, 3U, 0U, 2U, 1U, 0U, 3U, 2U, 2U, 3U, 3U, 0U, 0U, 1U, 1U, 0U, 2U, 2U, 0U, 3U, 1U,
    0U, 0U, 1U, 0U, 3U, 2U, 3U, 0U, 2U, 0U, 2U, 1U, 1U, 2U, 0U, 0U, 0U, 2U, 0U, 1U, 1U, 1U, 2U, 2U, 3U,
    1U, 1U, 1U, 3U, 0U, 3U, 2U, 1U, 2U, 0U, 2U, 1U, 3U, 0U, 0U, 3U, 3U, 2U, 1U, 3U, 0U, 1U, 0U,
};

static void
print_usage(const char* argv0) {
    std::printf("Usage: %s [--iters N] [--warmup N] [--repeat N] [--case NAME] [--format text|csv] [--list]\n", argv0);
}

static int
parse_positive_int(const char* value, int fallback) {
    if (value == NULL) {
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
    if (value == NULL) {
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
            const char* format = argv[++i];
            opts.format = (std::strcmp(format, "csv") == 0) ? OutputFormat::Csv : OutputFormat::Text;
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
    size_t middle = values.size() / 2U;
    if ((values.size() & 1U) != 0U) {
        return values[middle];
    }
    return 0.5 * (values[middle - 1U] + values[middle]);
}

static void
make_clean_llr(int16_t bit_llr[196]) {
    for (int i = 0; i < 98; i++) {
        bit_llr[(i * 2) + 0] = ((k_reference_dibits[i] >> 1) & 1U) ? 200 : -200;
        bit_llr[(i * 2) + 1] = (k_reference_dibits[i] & 1U) ? 200 : -200;
    }
}

static void
make_marginal_llr(int16_t bit_llr[196]) {
    uint32_t seed = 0x8BADF00DU;
    for (int i = 0; i < 196; i++) {
        seed = (seed * 1664525U) + 1013904223U;
        int dibit_index = i / 2;
        int bit_index = i & 1;
        int expected =
            bit_index == 0 ? ((k_reference_dibits[dibit_index] >> 1) & 1U) : (k_reference_dibits[dibit_index] & 1U);
        int16_t magnitude = (int16_t)(1U + ((seed >> 24) % 96U));
        if ((i % 13) == 0) {
            expected ^= 1;
        }
        bit_llr[i] = expected ? magnitude : (int16_t)-magnitude;
    }
}

static double
decode_checksum(const int16_t bit_llr[196]) {
    p25_12_candidate_t candidates[P25_12_MAX_CANDIDATES];
    int count = p25_12_soft_llr_list(NULL, bit_llr, candidates, P25_12_MAX_CANDIDATES);
    double checksum = (double)count;
    for (int candidate = 0; candidate < count; candidate++) {
        checksum += (double)candidates[candidate].metric;
        for (int byte_index = 0; byte_index < 12; byte_index++) {
            checksum += (double)candidates[candidate].bytes[byte_index] * (double)(byte_index + 1);
        }
    }
    return checksum;
}

static void
print_result(const BenchOptions& opts, const char* name, const BenchStats& stats) {
    constexpr double kTrellisSymbols = 49.0;
    if (opts.format == OutputFormat::Csv) {
        std::printf("%s,%d,%d,%d,%.0f,trellis_symbol,%.3f,%.3f,%.3f,%.6f,%.3f,%.9e\n", name, opts.repeat,
                    opts.iterations, opts.warmup, kTrellisSymbols, stats.median_ns_per_call, stats.min_ns_per_call,
                    stats.mean_ns_per_call, stats.median_ns_per_item, stats.items_per_second, stats.checksum);
        return;
    }
    std::printf("%-28s repeat=%2d median=%9.2f us/call min=%9.2f mean=%9.2f ns/symbol=%9.3f checksum=% .6e\n", name,
                opts.repeat, stats.median_ns_per_call / 1000.0, stats.min_ns_per_call / 1000.0,
                stats.mean_ns_per_call / 1000.0, stats.median_ns_per_item, stats.checksum);
}

static int
run_case(const BenchOptions& opts, const char* name, const int16_t bit_llr[196]) {
    if (opts.case_filter != NULL && std::strcmp(opts.case_filter, name) != 0) {
        return 0;
    }
    if (opts.list_cases) {
        std::printf("%s\n", name);
        return 1;
    }

    std::vector<double> ns_per_call;
    ns_per_call.reserve((size_t)opts.repeat);
    double checksum = 0.0;
    for (int repeat = 0; repeat < opts.repeat; repeat++) {
        for (int i = 0; i < opts.warmup; i++) {
            checksum += decode_checksum(bit_llr);
        }
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < opts.iterations; i++) {
            checksum += decode_checksum(bit_llr);
        }
        auto end = std::chrono::steady_clock::now();
        double elapsed_ns = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        ns_per_call.push_back(elapsed_ns / (double)opts.iterations);
    }

    BenchStats stats;
    stats.checksum = checksum;
    stats.min_ns_per_call = *std::min_element(ns_per_call.begin(), ns_per_call.end());
    for (double value : ns_per_call) {
        stats.mean_ns_per_call += value;
    }
    stats.mean_ns_per_call /= (double)ns_per_call.size();
    stats.median_ns_per_call = median_of(ns_per_call);
    stats.median_ns_per_item = stats.median_ns_per_call / 49.0;
    stats.items_per_second = 1000000000.0 / stats.median_ns_per_item;

    g_bench_sink += checksum;
    print_result(opts, name, stats);
    return 1;
}

} // namespace

int
main(int argc, char** argv) {
    BenchOptions opts = parse_options(argc, argv);
    int16_t clean_llr[196];
    int16_t marginal_llr[196];
    int16_t tie_llr[196];
    make_clean_llr(clean_llr);
    make_marginal_llr(marginal_llr);
    DSD_MEMSET(tie_llr, 0, sizeof(tie_llr));

    if (opts.format == OutputFormat::Text && !opts.list_cases) {
        std::printf("DSD-neo P25 1/2-rate list-decoder benchmark\n");
        std::printf("iterations=%d warmup=%d repeat=%d\n\n", opts.iterations, opts.warmup, opts.repeat);
    } else if (opts.format == OutputFormat::Csv && !opts.list_cases) {
        std::printf("case,repeat,iterations,warmup,work_items,item_unit,median_ns_per_call,min_ns_per_call,"
                    "mean_ns_per_call,median_ns_per_item,items_per_second,checksum\n");
    }

    int ran = 0;
    ran += run_case(opts, "p25_12_list_clean", clean_llr);
    ran += run_case(opts, "p25_12_list_marginal", marginal_llr);
    ran += run_case(opts, "p25_12_list_ties", tie_llr);
    if (ran == 0) {
        DSD_FPRINTF(stderr, "No benchmark case matched. Use --list to see available cases.\n");
        return 2;
    }
    return (g_bench_sink == -1.0) ? 1 : 0;
}
