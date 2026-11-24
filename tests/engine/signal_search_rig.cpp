// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* A Signal search run the way the Qt/Android app starts one, for the paced rtl_tcp rigs (issue #748): the decoder is
 * configured from the command line like any session (-i rtltcp:...), the app-control frontend runtime and the native
 * history store are opened around it, and the search is submitted through the app-control command queue, so the real
 * coordinator, the app-control observer and the store all run. Not a CTest by itself: it reads its search from the
 * environment and runs until told how many passes to finish. It keeps its files in its working directory: the history
 * journal (history.jsonl) and the stop file (stop).
 *
 *   DSD_SEARCH_RIG_RANGES  ranges, ';' between them, each "name,start_hz,end_hz,step_hz,bandwidth_hz,digital,probe_ms,
 *                          max_visit_ms" (digital as dsd_signal_search_range.digital; 0 analog, 1 Auto, 10 TETRA)
 *   DSD_SEARCH_RIG_PASSES  stop once this many passes have ended (default 1; a positive whole number)
 *   DSD_SEARCH_RIG_SECONDS stop after this long whatever the passes (default 120; 1 to 86400)
 *
 * The run also stops once a file named stop exists in the working directory (a driver that knows when it has seen
 * enough; a signal would end the run without the history on every platform but POSIX).
 *
 * stdout gets one JSON line per change of the store's status ({"rig":"status",...}, with the hit being shown), then,
 * once the run ends, one per history row ({"rig":"history",...}) and the same rows read back after the store is
 * closed and opened again ({"rig":"reload",...}). Time stamps are dsd_realtime_mono_ms(). */
#include <atomic>
#include <cstdlib>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/signal_search.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t kMaxRigSeconds = 86400;
/* The history journal and the stop file, both in the working directory (tests/signal_search_tetra_rig_check.py runs
   the rig in its folder and uses the same names). */
constexpr const char* kRigJournal = "history.jsonl";
constexpr const char* kRigStopFile = "stop";

struct Rig {
    dsd_signal_search_config config{};
    uint64_t passes = 1;
    uint64_t seconds = 120;
    std::atomic<bool> started{false};
    std::atomic<bool> done{false};
    bool failed = false;
};

/* A JSON string of @p text (UTF-8 kept as it is). */
std::string
quoted(const char* text) {
    std::string out = "\"";
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        if (*p == '"' || *p == '\\') {
            out += '\\';
            out += static_cast<char>(*p);
        } else if (*p < 0x20) {
            char escaped[8];
            DSD_SNPRINTF(escaped, sizeof(escaped), "\\u%04x", *p);
            out += escaped;
        } else {
            out += static_cast<char>(*p);
        }
    }
    return out + "\"";
}

std::string
hit_json(const dsd_signal_search_hit& h) {
    char numbers[512];
    DSD_SNPRINTF(numbers, sizeof(numbers),
                 "\"hit_id\":%llu,\"frequency_hz\":%u,\"range_index\":%u,\"started_ms\":%llu,\"ended_ms\":%llu,"
                 "\"confirmed\":%d,\"ended\":%d,\"unresolved\":%d,\"am\":%d,\"power_dbfs\":%.1f,\"noise_dbfs\":%.1f,"
                 "\"slot\":%d,\"target\":%u,\"source\":%u",
                 (unsigned long long)h.hit_id, h.frequency_hz, h.range_index, (unsigned long long)h.started_ms,
                 (unsigned long long)h.ended_ms, h.confirmed, h.ended, h.unresolved, h.am, (double)h.power_dbfs,
                 (double)h.noise_dbfs, h.slot, h.target, h.source);
    return std::string("{") + numbers + ",\"protocol\":" + quoted(h.protocol) + ",\"access\":" + quoted(h.access)
           + ",\"site\":" + quoted(h.site) + ",\"crypt\":" + quoted(h.crypt) + ",\"tone\":" + quoted(h.tone) + "}";
}

/* What a status line says; a line is printed whenever this changes. */
std::string
status_json(const dsd_signal_search_status& s) {
    char numbers[256];
    DSD_SNPRINTF(numbers, sizeof(numbers),
                 "\"phase\":%d,\"pass\":%llu,\"owned\":%d,\"range_index\":%u,\"channel_index\":%u,"
                 "\"channel_count\":%u,\"capture_rate_hz\":%u,\"identifying\":%d",
                 (int)s.phase, (unsigned long long)s.pass, s.owned, s.range_index, s.channel_index, s.channel_count,
                 s.capture_rate_hz, s.identifying);
    return std::string(numbers) + ",\"message\":" + quoted(s.message) + ",\"hit\":" + hit_json(s.hit);
}

void
print_line(const char* kind, const std::string& body) {
    DSD_FPRINTF(stdout, "{\"rig\":\"%s\",\"t_ms\":%llu,%s}\n", kind, (unsigned long long)dsd_realtime_mono_ms(),
                body.c_str());
    fflush(stdout);
}

/* "name,start,end,step,bandwidth,digital,probe_ms,max_visit_ms" into @p r; 0 when it does not parse. */
int
parse_range(const std::string& text, dsd_signal_search_range* r) {
    std::vector<std::string> fields;
    size_t at = 0;
    for (;;) {
        const size_t comma = text.find(',', at);
        fields.push_back(text.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
        if (comma == std::string::npos) {
            break;
        }
        at = comma + 1;
    }
    if (fields.size() != 8) {
        return 0;
    }
    long long values[7];
    for (int i = 0; i < 7; ++i) {
        char* end = nullptr;
        values[i] = std::strtoll(fields[(size_t)i + 1].c_str(), &end, 10);
        if (!end || end == fields[(size_t)i + 1].c_str() || *end != '\0' || values[i] < 0
            || values[i] > (long long)UINT32_MAX) {
            return 0;
        }
    }
    dsd_signal_search_range_default(r);
    DSD_SNPRINTF(r->name, sizeof(r->name), "%s", fields[0].c_str());
    r->start_hz = (uint32_t)values[0];
    r->end_hz = (uint32_t)values[1];
    r->step_num = (uint32_t)values[2];
    r->step_den = 1;
    r->bandwidth_hz = (uint32_t)values[3];
    r->digital = (int)values[4];
    r->probe_ms = (int)values[5];
    r->max_visit_ms = (int)values[6];
    return 1;
}

int
read_config(Rig* rig) {
    const char* ranges = std::getenv("DSD_SEARCH_RIG_RANGES");
    if (!ranges || !ranges[0]) {
        DSD_FPRINTF(stderr, "RIG ERROR: DSD_SEARCH_RIG_RANGES is not set\n");
        return 0;
    }
    std::string all(ranges);
    size_t at = 0;
    rig->config.session_id = 748;
    while (at <= all.size()) {
        const size_t semi = all.find(';', at);
        const std::string one = all.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
        if (rig->config.range_count >= DSD_SEARCH_MAX_RANGES
            || !parse_range(one, &rig->config.ranges[rig->config.range_count])) {
            DSD_FPRINTF(stderr, "RIG ERROR: bad range \"%s\"\n", one.c_str());
            return 0;
        }
        ++rig->config.range_count;
        if (semi == std::string::npos) {
            break;
        }
        at = semi + 1;
    }

    /* Positive whole numbers; the seconds at most a day, so the deadline in milliseconds cannot wrap. */
    const struct {
        const char* name;
        uint64_t max;
        uint64_t* out;
    } limits[] = {{"DSD_SEARCH_RIG_PASSES", UINT64_MAX, &rig->passes},
                  {"DSD_SEARCH_RIG_SECONDS", kMaxRigSeconds, &rig->seconds}};

    for (const auto& limit : limits) {
        const char* text = std::getenv(limit.name);
        if (!text || !text[0]) {
            continue;
        }
        uint64_t value = 0;
        if (dsd_parse_uint64_strict(text, 10, limit.max, &value) != 0 || value == 0) {
            DSD_FPRINTF(stderr, "RIG ERROR: bad %s \"%s\"\n", limit.name, text);
            return 0;
        }
        *limit.out = value;
    }
    return 1;
}

int
start(dsd_opts* opts, dsd_state* state, void* ptr) {
    auto* rig = static_cast<Rig*>(ptr);
    dsd_app_frontend_runtime_start(opts, state);
    if (dsd_app_signal_search_start(&rig->config) < 0) {
        DSD_FPRINTF(stderr, "RIG ERROR: the search start was not queued\n");
        return -1;
    }
    rig->started.store(true);
    return 0;
}

void
stop(dsd_opts* opts, dsd_state* state, void* ptr) {
    (void)opts;
    (void)state;
    (void)ptr;
    dsd_app_frontend_runtime_stop();
}

/* Print every status change until the passes are done, the search fails or the time runs out. */
void
watch(Rig* rig) {
    const uint64_t deadline = dsd_realtime_mono_ms() + rig->seconds * 1000U;
    std::string last;
    while (!rig->done.load()) {
        dsd_signal_search_status s{};
        dsd_app_signal_search_status_get(&s, nullptr, 0);
        const std::string now = status_json(s);
        if (now != last) {
            print_line("status", now);
            last = now;
        }
        if (rig->started.load() && s.session_id == rig->config.session_id) {
            if (s.phase == DSD_SEARCH_ERROR || (!s.owned && s.phase == DSD_SEARCH_STOPPED && s.pass > 0)) {
                rig->failed = s.phase == DSD_SEARCH_ERROR;
                dsd_exitflag_store(1);
                return;
            }
            if (s.pass > rig->passes) {
                dsd_exitflag_store(1);
                return;
            }
        }
        if (dsd_path_exists_nofollow(kRigStopFile) == 1) {
            dsd_exitflag_store(1);
            return;
        }
        if (dsd_realtime_mono_ms() >= deadline) {
            DSD_FPRINTF(stderr, "RIG ERROR: %llu s passed\n", (unsigned long long)rig->seconds);
            rig->failed = true;
            dsd_exitflag_store(1);
            return;
        }
        dsd_sleep_ms(10);
    }
}

/* Every row the store retains (up to DSD_SEARCH_HISTORY_LIMIT), not only the newest: a dump that left the older
   rows out would miss them in both the live and the reopened copy alike, so the comparison could not tell. */
void
print_history(const char* kind) {
    std::vector<dsd_signal_search_hit> hits(DSD_SEARCH_HISTORY_LIMIT);
    const size_t n = dsd_app_signal_search_history_get(hits.data(), hits.size());
    for (size_t i = 0; i < n; ++i) {
        print_line(kind, std::string("\"row\":") + hit_json(hits[i]));
    }
}

} // namespace

int
main(int argc, char** argv) {
    static Rig rig;
    if (!read_config(&rig)) {
        return 2;
    }
    if (dsd_app_signal_search_store_open(kRigJournal) != 0) {
        DSD_FPRINTF(stderr, "RIG ERROR: the store at %s did not open\n", kRigJournal);
        return 2;
    }
    auto* opts = static_cast<dsd_opts*>(calloc(1, sizeof(dsd_opts)));
    auto* state = static_cast<dsd_state*>(calloc(1, sizeof(dsd_state)));
    if (!opts || !state) {
        free(opts);
        free(state);
        dsd_app_signal_search_store_close();
        return 1;
    }
    initOpts(opts);
    initState(state);
    int rc = 1;
    if (dsd_runtime_bootstrap(argc, argv, opts, state, nullptr, &rc) == DSD_BOOTSTRAP_CONTINUE) {
        std::thread watcher(watch, &rig);
        const dsd_engine_lifecycle_hooks hooks{start, stop, &rig};
        rc = dsd_engine_run_with_lifecycle(opts, state, &hooks);
        rig.done.store(true);
        watcher.join();
        if (rig.failed) {
            rc = 1;
        }
    }
    freeState(state);
    free(state);
    free(opts);
    dsd_app_signal_search_store_flush();
    print_history("history");
    dsd_app_signal_search_store_close();
    if (dsd_app_signal_search_store_open(kRigJournal) != 0) {
        DSD_FPRINTF(stderr, "RIG ERROR: the store at %s did not open again\n", kRigJournal);
        return 1;
    }
    print_history("reload");
    dsd_app_signal_search_store_close();
    return rc;
}
