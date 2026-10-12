// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Manual hardware benchmark, deliberately not a CTest: opens the explicitly supplied SDR.
 * DSD_SEARCH_BENCH_MODE=search (default), serial or sweep. Range starts at -i's frequency,
 * extends 10 MHz at 12.5 kHz spacing. Serial reads 40 ms of fresh NFM; sweep uses the
 * existing 3.5-second dwell without rendering. Neither reference identifies off-centre hits. */
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/engine/signal_search.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/signal_search.h>
#include <stdint.h>
#include <stdio.h>
#include <thread>

namespace {
struct Bench {
    uint64_t started = 0, last = 0, phases[8]{}, hits = 0, spans = 0;
    uint64_t max_latency_us = 0;
    dsd_signal_search_phase phase = DSD_SEARCH_STOPPED;
    bool success = false;
    bool first_hit = false;
};

void
observe(const dsd_signal_search_status* s, const dsd_signal_search_hit* hit, void* ptr) {
    auto& b = *static_cast<Bench*>(ptr);
    if (b.success) {
        return;
    }
    const uint64_t now = dsd_realtime_mono_ms();
    b.phases[b.phase] += now - b.last;
    b.last = now;
    b.phase = s->phase;
    if (s->measurement_latency_us > b.max_latency_us) {
        b.max_latency_us = s->measurement_latency_us;
    }
    if (hit && hit->hit_id > b.hits) {
        b.hits = hit->hit_id;
    }
    if (s->phase == DSD_SEARCH_TUNING && s->pass == 1) {
        ++b.spans;
    }
    if (s->phase == DSD_SEARCH_ERROR) {
        DSD_FPRINTF(stderr, "BENCH ERROR: %s\n", s->message);
        dsd_exitflag_store(1);
    } else if (s->pass >= 2 || (b.first_hit && b.hits)) {
        DSD_FPRINTF(stdout,
                    "BENCH search elapsed_ms=%llu acquisition_ms=%llu measurement_ms=%llu reception_ms=%llu "
                    "visits=%llu tunes=%llu rate_hz=%u max_report_latency_us=%llu hit_frequency_hz=%u\n",
                    (unsigned long long)(now - b.started), (unsigned long long)b.phases[DSD_SEARCH_TUNING],
                    (unsigned long long)b.phases[DSD_SEARCH_SEARCHING],
                    (unsigned long long)(b.phases[DSD_SEARCH_IDENTIFYING] + b.phases[DSD_SEARCH_LISTENING]),
                    (unsigned long long)b.hits, (unsigned long long)b.spans, s->capture_rate_hz,
                    (unsigned long long)b.max_latency_us, s->hit.frequency_hz);
        b.success = true;
        dsd_exitflag_store(1);
    }
}

int
start(dsd_opts* opts, dsd_state* state, void* ptr) {
    auto& b = *static_cast<Bench*>(ptr);
    b.started = b.last = dsd_realtime_mono_ms();
    const char* mode = std::getenv("DSD_SEARCH_BENCH_MODE");
    b.first_hit = mode && std::strcmp(mode, "hit") == 0;
    uint32_t first = opts->rtlsdr_center_freq;
    if (first > UINT32_MAX - 10000000) {
        return -1;
    }
    if (mode && (std::strcmp(mode, "serial") == 0 || std::strcmp(mode, "sweep") == 0)) {
        float audio[2048];
        int got = 0;
        if (rtl_stream_read(state->rtl_ctx, audio, 2048, &got) < 0) {
            return -1;
        }
        dsd_signal_capture capture{};
        if (!rtl_signal_capture_get(&capture)) {
            return -1;
        }
        uint32_t step = std::strcmp(mode, "serial") == 0 ? 12500 : capture.rate_hz;
        uint32_t count = 0;
        for (uint32_t hz = first; hz <= first + 10000000 && !dsd_exitflag_load(); hz += step) {
            if (rtl_stream_tune(state->rtl_ctx, hz) != 0) {
                return -1;
            }
            const uint64_t until = dsd_realtime_mono_ms() + (step == 12500 ? 40 : 3500);
            size_t samples = 0;
            while (!dsd_exitflag_load()
                   && (dsd_realtime_mono_ms() < until
                       || (step == 12500 && samples < (size_t)rtl_stream_output_rate(state->rtl_ctx) * 40 / 1000))) {
                if (rtl_stream_read(state->rtl_ctx, audio, 2048, &got) < 0) {
                    return -1;
                }
                samples += (size_t)got;
            }
            ++count;
        }
        DSD_FPRINTF(stdout, "BENCH %s elapsed_ms=%llu tunes=%u rate_hz=%u\n", mode,
                    (unsigned long long)(dsd_realtime_mono_ms() - b.started), count, capture.rate_hz);
        b.success = !dsd_exitflag_load();
        dsd_exitflag_store(1);
        return 0;
    }
    dsd_signal_search_config c{};
    c.session_id = 42;
    c.range_count = 1;
    dsd_signal_search_range_default(&c.ranges[0]);
    c.ranges[0].start_hz = first;
    c.ranges[0].end_hz = first + 10000000;
    c.ranges[0].digital = 0;
    c.ranges[0].max_visit_ms = 1000;
    char error[192];
    int rc = dsd_engine_signal_search_start(opts, state, &c, observe, &b, error, sizeof(error));
    if (rc) {
        DSD_FPRINTF(stderr, "BENCH ERROR: %s\n", error);
    }
    return rc;
}
} // namespace

int
main(int argc, char** argv) {
    auto* opts = static_cast<dsd_opts*>(calloc(1, sizeof(dsd_opts)));
    auto* state = static_cast<dsd_state*>(calloc(1, sizeof(dsd_state)));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    initOpts(opts);
    initState(state);
    int rc = 1;
    Bench b;
    if (dsd_runtime_bootstrap(argc, argv, opts, state, nullptr, &rc) == DSD_BOOTSTRAP_CONTINUE) {
        std::atomic<bool> done{false};
        std::thread timer([&] {
            const auto deadline = dsd_realtime_mono_ms() + 300000;
            while (!done.load() && dsd_realtime_mono_ms() < deadline) {
                dsd_sleep_ms(25);
            }
            if (!done.load()) {
                dsd_exitflag_store(1);
            }
        });
        const dsd_engine_lifecycle_hooks hooks{start, nullptr, &b};
        rc = dsd_engine_run_with_lifecycle(opts, state, &hooks);
        done.store(true);
        timer.join();
        if (!b.success) {
            rc = 1;
        }
    }
    freeState(state);
    free(state);
    free(opts);
    return rc;
}
