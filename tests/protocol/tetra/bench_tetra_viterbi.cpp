// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Opt-in microbenchmark for the TETRA 16-state Viterbi decoders: the rate-1/4 signalling mother code and the
 * rate-1/3 speech channel.
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
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

enum class BenchKind : uint8_t { Signalling, SpeechFull, SpeechHalf };

struct BenchCase {
    const char* name;
    BenchKind kind;
    int len;      /* soft costs per call */
    double steps; /* trellis steps per call */
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

/* Noisy soft costs around a pseudo-random bit pattern: mostly confident, some marginal, a few wrong. */
static void
make_noisy_costs(uint16_t* costs, int len, uint32_t seed) {
    for (int i = 0; i < len; i++) {
        seed = (seed * 1664525U) + 1013904223U;
        const int bit = (int)((seed >> 31) & 1U);
        const uint32_t confidence = (seed >> 8) & 0x7FFFU;
        const int flip = ((seed >> 4) & 15U) == 0U;
        const uint32_t distance = (bit ^ flip) ? 0x8000U + confidence : 0x7FFFU - confidence;
        costs[i] = (uint16_t)distance;
    }
}

static double
decode_checksum(const BenchCase& c, const uint16_t* costs) {
    uint8_t bits[512];
    double checksum = 0.0;
    int count = 0;
    if (c.kind == BenchKind::Signalling) {
        count = tetra_viterbi_decode_soft(costs, c.len, bits, (int)sizeof(bits));
    } else {
        int crc_ok = 0;
        int mismatches = 0;
        count = c.kind == BenchKind::SpeechFull ? tetra_speech_full_decode(costs, bits, &crc_ok, &mismatches)
                                                : tetra_speech_half_decode(costs, bits, &crc_ok, &mismatches);
        checksum += (double)crc_ok + (double)mismatches;
    }
    checksum += (double)count;
    for (int i = 0; i < count; i++) {
        checksum += (double)bits[i] * (double)((i & 31) + 1);
    }
    return checksum;
}

static void
print_result(const BenchOptions& opts, const BenchCase& c, const BenchStats& stats) {
    if (opts.format == OutputFormat::Csv) {
        std::printf("%s,%d,%d,%d,%.0f,trellis_step,%.3f,%.3f,%.3f,%.6f,%.3f,%.9e\n", c.name, opts.repeat,
                    opts.iterations, opts.warmup, c.steps, stats.median_ns_per_call, stats.min_ns_per_call,
                    stats.mean_ns_per_call, stats.median_ns_per_item, stats.items_per_second, stats.checksum);
        return;
    }
    std::printf("%-28s repeat=%2d median=%9.2f us/call min=%9.2f mean=%9.2f ns/step=%9.3f checksum=% .6e\n", c.name,
                opts.repeat, stats.median_ns_per_call / 1000.0, stats.min_ns_per_call / 1000.0,
                stats.mean_ns_per_call / 1000.0, stats.median_ns_per_item, stats.checksum);
}

static int
run_case(const BenchOptions& opts, const BenchCase& c) {
    if (opts.case_filter != NULL && std::strcmp(opts.case_filter, c.name) != 0) {
        return 0;
    }
    if (opts.list_cases) {
        std::printf("%s\n", c.name);
        return 1;
    }

    std::vector<uint16_t> costs((size_t)c.len);
    make_noisy_costs(costs.data(), c.len, 0x7E7A0001U + (uint32_t)c.len);

    std::vector<double> ns_per_call;
    ns_per_call.reserve((size_t)opts.repeat);
    double checksum = 0.0;
    for (int repeat = 0; repeat < opts.repeat; repeat++) {
        for (int i = 0; i < opts.warmup; i++) {
            checksum += decode_checksum(c, costs.data());
        }
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < opts.iterations; i++) {
            checksum += decode_checksum(c, costs.data());
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
    stats.median_ns_per_item = stats.median_ns_per_call / c.steps;
    stats.items_per_second = 1000000000.0 / stats.median_ns_per_item;

    g_bench_sink += checksum;
    print_result(opts, c, stats);
    return 1;
}

} // namespace

int
main(int argc, char** argv) {
    BenchOptions opts = parse_options(argc, argv);
    /* BSCH (80 type-2 bits), SCH/HD (144), SCH/F (288), the decoder's 512-bit limit, and the two speech channels
       (184 and 94 trellis inputs). */
    static const BenchCase k_cases[] = {
        {"tetra_viterbi_bsch", BenchKind::Signalling, 80 * 4, 80.0},
        {"tetra_viterbi_sch_hd", BenchKind::Signalling, 144 * 4, 144.0},
        {"tetra_viterbi_sch_f", BenchKind::Signalling, 288 * 4, 288.0},
        {"tetra_viterbi_max", BenchKind::Signalling, 512 * 4, 512.0},
        {"tetra_speech_full", BenchKind::SpeechFull, 432, 184.0},
        {"tetra_speech_half", BenchKind::SpeechHalf, 216, 94.0},
    };

    if (opts.format == OutputFormat::Text && !opts.list_cases) {
        std::printf("DSD-neo TETRA Viterbi benchmark\n");
        std::printf("iterations=%d warmup=%d repeat=%d\n\n", opts.iterations, opts.warmup, opts.repeat);
    } else if (opts.format == OutputFormat::Csv && !opts.list_cases) {
        std::printf("case,repeat,iterations,warmup,work_items,item_unit,median_ns_per_call,min_ns_per_call,"
                    "mean_ns_per_call,median_ns_per_item,items_per_second,checksum\n");
    }

    int ran = 0;
    for (const BenchCase& c : k_cases) {
        ran += run_case(opts, c);
    }
    if (ran == 0) {
        DSD_FPRINTF(stderr, "No benchmark case matched. Use --list to see available cases.\n");
        return 2;
    }
    return (g_bench_sink == -1.0) ? 1 : 0;
}
