// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Opt-in RTL live pipeline performance counters.
 */

#include "rtl_perf.h"
#include <atomic>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/timing.h>
#include <errno.h>
#include <inttypes.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

namespace {

struct RtlPerfCounters {
    std::atomic<uint64_t> ingest_blocks{0};
    std::atomic<uint64_t> ingest_samples{0};
    std::atomic<uint64_t> ingest_drops{0};
    std::atomic<uint64_t> ingest_ns{0};

    std::atomic<uint64_t> demod_blocks{0};
    std::atomic<uint64_t> demod_input_samples{0};
    std::atomic<uint64_t> demod_output_samples{0};
    std::atomic<uint64_t> full_demod_ns{0};
    std::atomic<uint64_t> post_metrics_ns{0};
    std::atomic<uint64_t> output_write_ns{0};

    std::atomic<uint64_t> consumer_reads{0};
    std::atomic<uint64_t> consumer_samples{0};
    std::atomic<uint64_t> consumer_read_ns{0};
};

std::mutex g_perf_mutex;
std::atomic<int> g_perf_state{0}; /* 0 = uninitialized, 1 = disabled, 2 = enabled */
FILE* g_perf_file = nullptr;
uint64_t g_perf_interval_ns = 1000000000ULL;
uint64_t g_perf_next_log_ns = 0;
RtlPerfCounters g_perf;
const char* kRtlPerfCsvPath = "dsd-neo-rtl-perf.csv";

uint64_t
exchange_counter(std::atomic<uint64_t>& counter) {
    return counter.exchange(0, std::memory_order_acq_rel);
}

uint64_t
parse_interval_ns(void) {
    const char* env = getenv("DSD_NEO_RTL_PERF_INTERVAL_MS");
    if (!env || !env[0]) {
        return 1000000000ULL;
    }
    char* end = nullptr;
    errno = 0;
    unsigned long value = strtoul(env, &end, 10);
    if (errno != 0 || end == env || value == 0UL) {
        return 1000000000ULL;
    }
    if (value < 100UL) {
        value = 100UL;
    } else if (value > 60000UL) {
        value = 60000UL;
    }
    return (uint64_t)value * 1000000ULL;
}

void
write_header(FILE* f) {
    DSD_FPRINTF(f,
                "time_ms,source,rate_hz,output_kind,input_used,input_capacity,input_drops,output_used,output_capacity,"
                "symbol_cache_pending,ingest_blocks,ingest_samples,ingest_drops,ingest_ns,demod_blocks,"
                "demod_input_samples,demod_output_samples,full_demod_ns,post_metrics_ns,output_write_ns,"
                "consumer_reads,consumer_samples,consumer_read_ns,snr_db,cfo_hz,carrier_lock\n");
}

void
init_locked(void) {
    if (g_perf_state.load(std::memory_order_acquire) != 0) {
        return;
    }
    const char* path = getenv("DSD_NEO_RTL_PERF_CSV");
    if (!path || !path[0]) {
        g_perf_state.store(1, std::memory_order_release);
        return;
    }

    FILE* f = dsd_fopen_private(kRtlPerfCsvPath, "a");
    if (!f) {
        DSD_FPRINTF(stderr, "RTL PERF: failed to open '%s': %s\n", kRtlPerfCsvPath, strerror(errno));
        g_perf_state.store(1, std::memory_order_release);
        return;
    }

    int needs_header = 1;
    dsd_stat_t st;
    if (dsd_fstat(dsd_fileno(f), &st) == 0) {
        needs_header = (st.st_size <= 0) ? 1 : 0;
    }
    if (needs_header) {
        write_header(f);
    }
    setvbuf(f, nullptr, _IOLBF, 0);

    g_perf_file = f;
    g_perf_interval_ns = parse_interval_ns();
    g_perf_next_log_ns = dsd_time_monotonic_ns() + g_perf_interval_ns;
    g_perf_state.store(2, std::memory_order_release);
}

void
ensure_initialized(void) {
    if (g_perf_state.load(std::memory_order_acquire) != 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_perf_mutex);
    init_locked();
}

} // namespace

extern "C" int
rtl_perf_enabled(void) {
    ensure_initialized();
    return g_perf_state.load(std::memory_order_acquire) == 2 ? 1 : 0;
}

extern "C" uint64_t
rtl_perf_now_ns(void) {
    return dsd_time_monotonic_ns();
}

extern "C" void
rtl_perf_record_ingest(uint64_t elapsed_ns, size_t input_samples, uint64_t dropped_samples) {
    if (!rtl_perf_enabled()) {
        return;
    }
    g_perf.ingest_blocks.fetch_add(1, std::memory_order_relaxed);
    g_perf.ingest_samples.fetch_add((uint64_t)input_samples, std::memory_order_relaxed);
    g_perf.ingest_drops.fetch_add(dropped_samples, std::memory_order_relaxed);
    g_perf.ingest_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
}

extern "C" void
rtl_perf_record_demod_block(uint64_t full_demod_ns, uint64_t post_metrics_ns, uint64_t output_write_ns,
                            size_t input_samples, size_t output_samples) {
    if (!rtl_perf_enabled()) {
        return;
    }
    g_perf.demod_blocks.fetch_add(1, std::memory_order_relaxed);
    g_perf.demod_input_samples.fetch_add((uint64_t)input_samples, std::memory_order_relaxed);
    g_perf.demod_output_samples.fetch_add((uint64_t)output_samples, std::memory_order_relaxed);
    g_perf.full_demod_ns.fetch_add(full_demod_ns, std::memory_order_relaxed);
    g_perf.post_metrics_ns.fetch_add(post_metrics_ns, std::memory_order_relaxed);
    g_perf.output_write_ns.fetch_add(output_write_ns, std::memory_order_relaxed);
}

extern "C" void
rtl_perf_record_consumer_read(uint64_t elapsed_ns, size_t output_samples) {
    if (!rtl_perf_enabled()) {
        return;
    }
    g_perf.consumer_reads.fetch_add(1, std::memory_order_relaxed);
    g_perf.consumer_samples.fetch_add((uint64_t)output_samples, std::memory_order_relaxed);
    g_perf.consumer_read_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
}

extern "C" void
rtl_perf_maybe_log(const rtl_perf_log_snapshot* snapshot) {
    if (!snapshot || !rtl_perf_enabled()) {
        return;
    }

    uint64_t now_ns = dsd_time_monotonic_ns();
    if (now_ns < g_perf_next_log_ns) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_perf_mutex);
    if (g_perf_state.load(std::memory_order_acquire) != 2 || !g_perf_file) {
        return;
    }
    now_ns = dsd_time_monotonic_ns();
    if (now_ns < g_perf_next_log_ns) {
        return;
    }

    uint64_t ingest_blocks = exchange_counter(g_perf.ingest_blocks);
    uint64_t ingest_samples = exchange_counter(g_perf.ingest_samples);
    uint64_t ingest_drops = exchange_counter(g_perf.ingest_drops);
    uint64_t ingest_ns = exchange_counter(g_perf.ingest_ns);
    uint64_t demod_blocks = exchange_counter(g_perf.demod_blocks);
    uint64_t demod_input_samples = exchange_counter(g_perf.demod_input_samples);
    uint64_t demod_output_samples = exchange_counter(g_perf.demod_output_samples);
    uint64_t full_demod_ns = exchange_counter(g_perf.full_demod_ns);
    uint64_t post_metrics_ns = exchange_counter(g_perf.post_metrics_ns);
    uint64_t output_write_ns = exchange_counter(g_perf.output_write_ns);
    uint64_t consumer_reads = exchange_counter(g_perf.consumer_reads);
    uint64_t consumer_samples = exchange_counter(g_perf.consumer_samples);
    uint64_t consumer_read_ns = exchange_counter(g_perf.consumer_read_ns);

    DSD_FPRINTF(g_perf_file,
                "%" PRIu64 ",%s,%" PRIu32 ",%d,%zu,%zu,%" PRIu64 ",%zu,%zu,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%" PRIu64 ",%.3f,%.3f,%d\n",
                (uint64_t)(now_ns / 1000000ULL), snapshot->source ? snapshot->source : "unknown",
                snapshot->sample_rate_hz, snapshot->output_kind, snapshot->input_used, snapshot->input_capacity,
                snapshot->input_drops, snapshot->output_used, snapshot->output_capacity, snapshot->symbol_cache_pending,
                ingest_blocks, ingest_samples, ingest_drops, ingest_ns, demod_blocks, demod_input_samples,
                demod_output_samples, full_demod_ns, post_metrics_ns, output_write_ns, consumer_reads, consumer_samples,
                consumer_read_ns, snapshot->snr_db, snapshot->cfo_hz, snapshot->carrier_lock);
    fflush(g_perf_file);
    g_perf_next_log_ns = now_ns + g_perf_interval_ns;
}

extern "C" void
rtl_perf_shutdown(void) {
    std::lock_guard<std::mutex> lock(g_perf_mutex);
    if (g_perf_file) {
        fflush(g_perf_file);
        fclose(g_perf_file);
        g_perf_file = nullptr;
    }
    g_perf_state.store(0, std::memory_order_release);
}
