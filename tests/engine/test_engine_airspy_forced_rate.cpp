// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * End to end, live: an Airspy at a rate it forces decodes as the native-rate capture does (issue #806).
 *
 * The real engine runs a live Airspy session the way the CLI does (-i airspy:F:BW), its stream workers and the native
 * adapter on the in-process SDK double (tests/io/airspy_sdk_double.h) at one forced rate, 312,500, 375,000 or
 * 2,500,000 Hz. The half-band passes leave 78,125 or 93,750 Hz, and the I/Q rate step takes that to the DSP bandwidth
 * rate. The samples are a committed 48 or 72 kHz fixture upsampled to the device rate (forced_rate_input.h), fed as the
 * SDK delivers them, in transfers of the size a replay of the capture reads (dsd_iq_replay_chunk_bytes_for()).
 *
 * The feed is paced by acknowledgement, never by time: a transfer goes in once the demod came back for input with the
 * one before it released and the decoder found the output ring empty with everything published before it read
 * (rtl_stream_test_get_live_sequence()), the three boundaries the replay's decoder pacing holds. So each transfer is one
 * demod block, and the decoder takes its output before the next block starts, as in a replay.
 *
 * Each case asserts, live:
 * - the counters the protocol's own CRC or FEC checked, at least the native-rate value of the same samples (the same
 *   excerpt and noise at 48 or 72 kHz, no step) less an allowance stated per case for frames at the excerpt's edges;
 * - no producer drop, and the chain: the device's rate through the half-band passes and the step to the DSP rate, the
 *   capture-domain consumers on the device's rate;
 * - the capture (--iq-capture, cf32) records that chain.
 * Then the engine replays that capture (--iq-replay), and the replay must run the same chain (plan key), read the
 * capture in the live transfers' partition, hand the demod the same samples after the step bit for bit, and print the
 * same stable decoded fields in order (stable_lines(): the decoder's records with their HH:MM:SS stamps left out, the
 * live stamp being the wall clock and the replay's the capture's, and without the lines a decode-clock throttle gates).
 * It also meets the same thresholds.
 *
 * The RETUNE case retunes the running stream half way: the step starts over with the half-band cascade (its clear
 * count moves), keeps its plan, and the capture-domain consumers keep the device's rate.
 *
 *   dsd-neo_test_engine_airspy_forced_rate <case>
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/exitflag.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "airspy_sdk_double.h"
#include "demod_pipeline_test_support.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"
#include "forced_rate_input.h"
#include "live_iq_step_test_support.h"
#include "rtl_stream_test_support.h"
#include "test_support.h"

#ifndef DSD_NEO_TEST_IQ_FIXTURE_DIR
#error "DSD_NEO_TEST_IQ_FIXTURE_DIR must name tests/fixtures/iq"
#endif

namespace {

/* A decoder line counted by its text: at least min of them. */
struct Count {
    const char* label;
    const char* text;
    int min;
};

struct Case {
    const char* name;
    dsd_test_forced_rate_spec spec;
    const char* mode[3];
    Count counts[3];
    /* Lines the live-replay comparison takes as the stable decoded fields: any line holding one of these. */
    const char* stable[4];
    /* TSBKs whose CRC passed at least this, and the network's WACN in the state; -1: not checked */
    int min_tsbk_ok;
    int retune;
};

constexpr double kPadS = 0.05;

/*
 * The native-rate values are the same samples at 48 kHz (72 kHz for TETRA) through the same noise, with no half-band
 * pass and no step: p25p1_cqpsk_cc 52 TSBKs whose CRC passed, 41 network status (WACN 92065, SYS 0D5), 18 grants; p25p1_cqpsk_vc 8
 * grants; the simulcast control channel 52 / 41 / 18; p25p2_cc 10 SACCH; dmr_voice_synth's first 2 s 16 voice bursts;
 * tetra_call_real_air_72k's first 2 s 70 cell identities (BSCH) and 25 TCH/FS bursts whose CRC passed. The allowance is
 * 2 of each P25p1 count (a frame cut at an excerpt edge), 2 SACCH, one DMR superframe (6 voice bursts), 4 identities
 * and 2 TCH bursts.
 */
const Case kCases[] = {
    {"P25P1_CQPSK_CC_312K",
     {"p25p1_cqpsk_cc", 0.0, 0.0, 312500U, 48000U, kPadS},
     {"-f1", "-mq", nullptr},
     {{"network status", "WACN: 92065; SYS: 0D5", 39}, {"voice grants", "Group Voice Channel", 16}, {}},
     {"TSBK", "WACN", "Group Voice Channel", "NAC"},
     50,
     0},
    {"P25P1_CQPSK_VC_375K",
     {"p25p1_cqpsk_vc", 0.0, 0.0, 375000U, 48000U, kPadS},
     {"-f1", "-mq", nullptr},
     {{"voice grants", "Group Voice Channel User", 6}, {}, {}},
     {"Group Voice Channel", "HDU", "LDU", "NAC"},
     -1,
     0},
    {"P25P1_CQPSK_SIMULCAST_312K",
     {"p25p1_cqpsk_cc_simulcast", 0.0, 0.0, 312500U, 48000U, kPadS},
     {"-f1", "-mq", nullptr},
     {{"network status", "WACN: 92065; SYS: 0D5", 39}, {"voice grants", "Group Voice Channel", 16}, {}},
     {"TSBK", "WACN", "Group Voice Channel", "NAC"},
     50,
     0},
    {"P25P1_CQPSK_CC_2500K",
     {"p25p1_cqpsk_cc", 0.0, 0.0, 2500000U, 48000U, kPadS},
     {"-f1", "-mq", nullptr},
     {{"network status", "WACN: 92065; SYS: 0D5", 39}, {"voice grants", "Group Voice Channel", 16}, {}},
     {"TSBK", "WACN", "Group Voice Channel", "NAC"},
     50,
     0},
    /* P25 Phase 2: p25p2_cc is remodulated audio with no RS-checkable payload, so this is DUID-FEC-level evidence
       only (weak). */
    {"P25P2_CC_375K",
     {"p25p2_cc", 0.0, 0.0, 375000U, 48000U, kPadS},
     {"-f2", nullptr, nullptr},
     {{"SACCH (DUID level, weak)", "P25p2 SACCH", 8}, {}, {}},
     {"P25p2", nullptr, nullptr, nullptr},
     -1,
     0},
    {"DMR_VOICE_312K",
     {"dmr_voice_synth", 0.0, 2.0, 312500U, 48000U, kPadS},
     {"-fs", nullptr, nullptr},
     {{"voice bursts", "Color Code=02 | VC", 10}, {}, {}},
     {"Color Code", nullptr, nullptr, nullptr},
     -1,
     0},
    {"TETRA_CALL_375K",
     {"tetra_call_real_air_72k", 0.0, 2.0, 375000U, 72000U, kPadS},
     {"-fT", "-Z", nullptr},
     {{"cell identities", "[TETRA BSCH] MCC=228 MNC=8889 CC=0x01", 66},
      {"TCH/FS bursts, CRC passed", "[TETRA TCH/FS] CC=1  dec_bits=274  crc=ok", 23},
      {}},
     {"[TETRA BSCH]", "crc=", "[TETRA CMCE", nullptr},
     -1,
     0},
    {"RETUNE_375K",
     {"p25p1_cqpsk_cc", 0.0, 0.0, 375000U, 48000U, kPadS},
     {"-f1", "-mq", nullptr},
     {{"network status", "WACN: 92065; SYS: 0D5", 1}, {}, {}},
     {nullptr, nullptr, nullptr, nullptr},
     -1,
     1},
};

std::vector<std::string> g_failures;

void
check(const std::string& label, bool cond) {
    if (!cond) {
        g_failures.push_back(label);
    }
}

void
check_min(const std::string& label, long long got, long long min) {
    if (got < min) {
        g_failures.push_back(label + ": got " + std::to_string(got) + ", want at least " + std::to_string(min));
    }
}

void
check_eq(const std::string& label, long long got, long long want) {
    if (got != want) {
        g_failures.push_back(label + ": got " + std::to_string(got) + ", want " + std::to_string(want));
    }
}

/* What the stop hook saw, after live processing and before the engine tears its state down. */
struct RunView {
    int stops = 0;
    unsigned long long wacn = 0ULL;
};

int
observe_start(dsd_opts*, dsd_state*, void*) {
    return 0;
}

void
observe_stop(dsd_opts*, dsd_state* state, void* context) {
    auto* view = static_cast<RunView*>(context);
    view->stops++;
    view->wacn = state->p2_wacn;
}

/* The post-step samples of every block, in order (dsd_demod_test_set_post_step_tap()), on the demod thread. */
void
collect_post_step(const float* iq, int pairs, void* ctx) {
    auto* out = static_cast<std::vector<float>*>(ctx);
    if (iq && pairs > 0) {
        out->insert(out->end(), iq, iq + 2U * static_cast<size_t>(pairs));
    }
}

/* A replay's blocks (rtl_stream_test_set_replay_block_hook()): their sizes in complex samples, and the chain the
   demod runs at the first, read on the demod thread. */
struct ReplayBlocks {
    std::vector<size_t> pairs;
    rtl_stream_test_running_chain chain{};
    int have_chain = 0;
};

void
collect_replay_block(const rtl_stream_test_replay_block* block, void* ctx) {
    auto* out = static_cast<ReplayBlocks*>(ctx);
    out->pairs.push_back(block->float_count / 2U);
    if (!out->have_chain) {
        out->have_chain = rtl_stream_test_get_running_chain(&out->chain) == 0;
    }
}

/* Run the engine on @p args as the CLI would, the decoder's console in @p log. Returns the engine's result. */
int
run_engine(const std::vector<std::string>& args, RunView* view, std::string* log) {
    auto opts = static_cast<dsd_opts*>(calloc(1, sizeof(dsd_opts)));
    auto state = static_cast<dsd_state*>(calloc(1, sizeof(dsd_state)));
    if (!opts || !state) {
        free(opts);
        free(state);
        return -1;
    }
    initOpts(opts);
    initState(state);
    std::vector<std::vector<char>> storage;
    std::vector<char*> argv;
    for (const std::string& a : args) {
        storage.emplace_back(a.begin(), a.end());
        storage.back().push_back('\0');
    }
    argv.reserve(storage.size() + 1U);
    for (auto& s : storage) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    dsd_test_capture_stderr cap;
    int rc = -1;
    if (dsd_test_capture_stderr_begin(&cap, "dsdneo_forced_rate_log") == 0) {
        int boot_rc = 1;
        if (dsd_runtime_bootstrap((int)args.size(), argv.data(), opts, state, nullptr, &boot_rc)
            == DSD_BOOTSTRAP_CONTINUE) {
            const dsd_engine_lifecycle_hooks hooks = {observe_start, observe_stop, view};
            rc = dsd_engine_run_with_lifecycle(opts, state, &hooks);
        }
        (void)dsd_test_capture_stderr_end(&cap);
        FILE* fp = dsd_fopen_existing_regular_file(cap.path, "rb");
        if (fp) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1U, sizeof buf, fp)) > 0U) {
                log->append(buf, n);
            }
            (void)fclose(fp);
        }
        (void)remove(cap.path);
    }
    freeState(state);
    free(state);
    free(opts);
    dsd_exitflag_store(0);
    return rc;
}

/* The log's lines without ANSI colour. */
std::vector<std::string>
log_lines(const std::string& log) {
    std::vector<std::string> lines;
    std::string line;
    for (size_t i = 0; i < log.size(); i++) {
        const char c = log[i];
        if (c == '\x1b') {
            while (i < log.size() && log[i] != 'm') {
                i++;
            }
            continue;
        }
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else if (c != '\r') {
            line.push_back(c);
        }
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

/* TSBKs whose CRC passed: the decoder prints a TSBK's message on the line after its " TSBK" header only when the CRC
   passed (tsbk_dispatch_message()). */
int
count_tsbk_crc_ok(const std::vector<std::string>& lines) {
    int n = 0;
    for (size_t i = 0; i + 1U < lines.size(); i++) {
        const std::string& l = lines[i];
        const size_t end = l.find_last_not_of(' ');
        const bool header = end != std::string::npos && end >= 4U && l.compare(end - 4U, 5U, " TSBK") == 0;
        if (header && lines[i + 1U].find_first_not_of(' ') != std::string::npos) {
            n++;
        }
    }
    return n;
}

int
count_lines(const std::vector<std::string>& lines, const char* text) {
    int n = 0;
    for (const std::string& l : lines) {
        if (l.find(text) != std::string::npos) {
            n++;
        }
    }
    return n;
}

/* Decoder lines whose printing a throttle on the decode clock gates, which a live stream runs on the wall clock and a
   replay on the capture's: the live feed runs faster than real time, so it prints fewer. A DMR CACH short LC prints at
   most once a second a slot (dmr_cach_handle_single_fragment()). */
const char* const kClockThrottled[] = {"(single)"};

bool
clock_throttled(const std::string& l) {
    for (const char* t : kClockThrottled) {
        if (l.find(t) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/* The stable decoded fields: every decoder record (a line stamped HH:MM:SS, its stamp left out, and the indented lines
   that go on with it) and the lines holding one of @p c's stable texts, leaving out the clock-throttled lines. */
std::vector<std::string>
stable_lines(const std::vector<std::string>& lines, const Case& c) {
    std::vector<std::string> out;
    bool in_record = false;
    for (const std::string& l : lines) {
        const bool stamped = l.size() >= 9U && l[2] == ':' && l[5] == ':' && l[8] == ' ';
        const bool goes_on = in_record && !l.empty() && l[0] == ' ';
        in_record = stamped || goes_on;
        bool keep = in_record;
        for (const char* t : c.stable) {
            keep = keep || (t && l.find(t) != std::string::npos);
        }
        if (keep && !clock_throttled(l)) {
            out.push_back(stamped ? l.substr(9U) : l);
        }
    }
    return out;
}

/* What the feeder saw of the live run. */
struct LiveFeed {
    const Case* c = nullptr;
    const dsd_test_forced_rate_input* in = nullptr;
    size_t transfer_pairs = 0U;
    std::vector<size_t> transfers;
    int ok = 0;
    int retuned = 0;
    /* Each transfer before a retune was one demod block (a retune's mute and settling cut the ones after it). */
    int blocks_per_transfer_ok = 1;
    std::string error;
    rtl_stream_test_running_chain chain{};
    rtl_stream_test_running_chain chain_after_retune{};
    uint64_t producer_drops = 0U;
    uint64_t step_clears_before = 0U;
    uint64_t step_clears_after = 0U;
};

/* Wait (10 s at most) until @p done. */
template <typename Pred>
bool
wait_for(Pred done) {
    for (int i = 0; i < 10000; i++) {
        if (done()) {
            return true;
        }
        dsd_sleep_ms(1);
    }
    return done();
}

rtl_stream_test_live_sequence
live_sequence() {
    rtl_stream_test_live_sequence s{};
    rtl_stream_test_get_live_sequence(&s);
    return s;
}

/* One transfer, then the three acknowledgements: the demod released it and came back for input, and the decoder
   found the output ring empty with everything that block published taken. */
bool
feed_transfer(LiveFeed* feed, const float* iq, size_t pairs) {
    const uint64_t blocks_before = live_sequence().blocks;
    if (!wait_for([&] { return airspy_double::deliver(iq, (int)pairs); })) {
        feed->error = "the stream took no transfer";
        return false;
    }
    const uint64_t produced = live_sequence().input_produced;
    if (!wait_for([&] { return live_sequence().demod_idle_consumed >= produced; })) {
        feed->error = "the demod did not come back for input";
        return false;
    }
    const uint64_t published = live_sequence().published;
    if (!wait_for([&] { return live_sequence().decoder_idle_published >= published; })) {
        feed->error = "the decoder did not take the block's output";
        return false;
    }
    if (!feed->retuned) {
        feed->blocks_per_transfer_ok &= live_sequence().blocks == blocks_before + 1U ? 1 : 0;
    }
    feed->transfers.push_back(pairs);
    return true;
}

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
feeder(void* arg) {
    auto* feed = static_cast<LiveFeed*>(arg);
    const dsd_test_forced_rate_input* in = feed->in;
    if (!wait_for([] { return airspy_double::streaming.load(); })) {
        feed->error = "the Airspy stream did not start";
        dsd_exitflag_store(1);
        DSD_THREAD_RETURN;
    }
    feed->step_clears_before = dsd_live_iq_step_test_clears();
    size_t at = 0U;
    while (at < in->pairs) {
        const size_t pairs = (in->pairs - at < feed->transfer_pairs) ? in->pairs - at : feed->transfer_pairs;
        if (!feed_transfer(feed, in->iq + 2U * at, pairs)) {
            dsd_exitflag_store(1);
            DSD_THREAD_RETURN;
        }
        at += pairs;
        if (at == pairs) {
            (void)rtl_stream_test_get_running_chain(&feed->chain);
        }
        if (feed->c->retune && !feed->retuned && at >= in->pairs / 2U) {
            feed->retuned = 1;
            feed->step_clears_before = dsd_live_iq_step_test_clears();
            if (dsd_rtl_stream_test_request_retune((long)in->center_frequency_hz + 12500L, 5000) != 0) {
                feed->error = "the retune did not complete";
                dsd_exitflag_store(1);
                DSD_THREAD_RETURN;
            }
        }
    }
    feed->step_clears_after = dsd_live_iq_step_test_clears();
    (void)rtl_stream_test_get_running_chain(&feed->chain_after_retune);
    feed->producer_drops = live_sequence().producer_drops;
    feed->ok = 1;
    dsd_exitflag_store(1);
    DSD_THREAD_RETURN;
}

std::string
mhz(uint64_t hz) {
    char buf[32];
    DSD_SNPRINTF(buf, sizeof buf, "%.6fM", (double)hz / 1e6);
    return buf;
}

std::vector<std::string>
base_args(const Case& c) {
    std::vector<std::string> args = {"dsd-neo", "--frontend", "none"};
    for (const char* m : c.mode) {
        if (m) {
            args.emplace_back(m);
        }
    }
    args.emplace_back("-o");
    args.emplace_back("null");
    return args;
}

/* The case's thresholds on one run's log and state. */
void
check_thresholds(const Case& c, const char* run, const std::vector<std::string>& lines, const RunView& view) {
    const std::string prefix = std::string(c.name) + " " + run + ": ";
    check_eq(prefix + "the stop hook ran once", view.stops, 1);
    if (c.min_tsbk_ok >= 0) {
        check_min(prefix + "TSBKs whose CRC passed", count_tsbk_crc_ok(lines), c.min_tsbk_ok);
        check_eq(prefix + "the WACN decoded", (long long)view.wacn, 0x92065LL);
    }
    for (const Count& k : c.counts) {
        if (k.label) {
            check_min(prefix + k.label, count_lines(lines, k.text), k.min);
        }
    }
    DSD_FPRINTF(stderr, "%s %s: [TSBKs whose CRC passed]=%d", c.name, run, count_tsbk_crc_ok(lines));
    for (const Count& k : c.counts) {
        if (k.label) {
            DSD_FPRINTF(stderr, " [%s]=%d", k.label, count_lines(lines, k.text));
        }
    }
    DSD_FPRINTF(stderr, "\n");
}

/* The chain the device's rate runs: its passes, the step to the DSP rate, the capture-domain consumers on the device's
   rate. */
void
check_chain(const Case& c, const char* what, const rtl_stream_test_running_chain& chain,
            const dsd_test_forced_rate_input& in) {
    const std::string prefix = std::string(c.name) + " " + what + ": ";
    check_eq(prefix + "half-band passes", chain.passes, in.passes);
    check_eq(prefix + "the step runs", chain.step_enabled, 1);
    check_eq(prefix + "step L", chain.step_l, in.step_l);
    check_eq(prefix + "step M", chain.step_m, in.step_m);
    check_eq(prefix + "the step's target", chain.step_target_hz, (long long)c.spec.dsp_rate_hz);
    check_eq(prefix + "the complex rate after the step", chain.complex_rate_hz, (long long)c.spec.dsp_rate_hz);
    check_eq(prefix + "the capture-domain rate", chain.capture_rate_hz, (long long)c.spec.device_rate_hz);
    check_eq(prefix + "device forced", chain.forced, 1);
}

/* The live capture's sidecar records the chain the live stream ran. */
void
check_capture(const Case& c, const char* meta_path, const dsd_test_forced_rate_input& in) {
    const std::string prefix = std::string(c.name) + " capture: ";
    dsd_iq_replay_config meta;
    DSD_MEMSET(&meta, 0, sizeof(meta));
    char err[256] = {0};
    if (dsd_iq_replay_read_metadata(meta_path, &meta, err, sizeof(err)) != DSD_IQ_OK) {
        check(prefix + "the sidecar reads: " + err, false);
        return;
    }
    const dsd_iq_rate_chain* chain = &meta.rate_chain;
    check_eq(prefix + "sample rate (the device's)", meta.sample_rate_hz, (long long)c.spec.device_rate_hz);
    check_eq(prefix + "base decimation", meta.base_decimation, 1LL << in.passes);
    check(prefix + "records its rate chain", chain->has_nominal && chain->has_forced && chain->has_step);
    check_eq(prefix + "nominal rate", chain->nominal_rate_hz, (long long)c.spec.device_rate_hz);
    check_eq(prefix + "forced", chain->device_forced, 1);
    check_eq(prefix + "DSP rate", chain->dsp_rate_hz, (long long)c.spec.dsp_rate_hz);
    check_eq(prefix + "step L", chain->step_l, in.step_l);
    check_eq(prefix + "step M", chain->step_m, in.step_m);
    check_eq(prefix + "no drop", (long long)meta.input_ring_drops, 0);
    dsd_iq_replay_config_clear(&meta);
}

/* The live run: the engine on -i airspy, the feeder on its own thread, the capture (but for a retune) at
   @p data_path. */
int
run_live(const Case& c, const dsd_test_forced_rate_input& in, const char* data_path, LiveFeed* feed,
         std::vector<float>* post, std::vector<std::string>* lines) {
    feed->c = &c;
    feed->in = &in;
    feed->transfer_pairs =
        dsd_iq_replay_chunk_bytes_for(DSD_IQ_FORMAT_CF32, c.spec.device_rate_hz, c.spec.device_rate_hz)
        / (2U * sizeof(float));
    std::vector<std::string> args = base_args(c);
    char bw[16];
    DSD_SNPRINTF(bw, sizeof bw, "%u", (unsigned)(c.spec.dsp_rate_hz / 1000U));
    args.emplace_back("-i");
    args.emplace_back("airspy:" + mhz(in.center_frequency_hz) + ":" + bw);
    if (!c.retune) {
        args.emplace_back("--iq-capture");
        args.emplace_back(data_path);
        args.emplace_back("--iq-capture-format");
        args.emplace_back("cf32");
    }
    dsd_demod_test_set_post_step_tap(collect_post_step, post);
    dsd_thread_t thread;
    RunView view;
    std::string log;
    int rc = -1;
    if (dsd_thread_create(&thread, feeder, feed) == 0) {
        rc = run_engine(args, &view, &log);
        (void)dsd_thread_join(thread);
        /* A feeder that gave up after the engine had ended raised the exit flag after run_engine() cleared it. */
        dsd_exitflag_store(0);
    }
    dsd_demod_test_set_post_step_tap(nullptr, nullptr);
    *lines = log_lines(log);

    check(std::string(c.name) + " live: the run ended cleanly", rc == 0);
    check(std::string(c.name) + " live: every transfer fed: " + feed->error, feed->ok == 1);
    check(std::string(c.name) + " live: each transfer is one demod block", feed->blocks_per_transfer_ok == 1);
    check_eq(std::string(c.name) + " live: producer drops", (long long)feed->producer_drops, 0);
    check_thresholds(c, "live", *lines, view);
    check_chain(c, "live", feed->chain, in);
    if (c.retune) {
        /* The retune starts the step over with the half-band cascade, on the plan it ran. */
        check(std::string(c.name) + ": the retune clears the step", feed->step_clears_after > feed->step_clears_before);
        check_chain(c, "after the retune", feed->chain_after_retune, in);
    }
    return feed->ok;
}

/* The live and replayed runs print the same stable decoded fields. */
void
check_stable_fields(const Case& c, const std::vector<std::string>& live, const std::vector<std::string>& replay) {
    check(std::string(c.name) + ": live decodes stable fields", !live.empty());
    if (live == replay) {
        return;
    }
    size_t i = 0;
    while (i < live.size() && i < replay.size() && live[i] == replay[i]) {
        i++;
    }
    check(std::string(c.name) + ": the same stable decoded fields; first difference at line " + std::to_string(i)
              + ": live \"" + (i < live.size() ? live[i] : "<end>") + "\", replay \""
              + (i < replay.size() ? replay[i] : "<end>") + "\"",
          false);
}

/* The engine replays the live capture at @p meta_path: the same thresholds, chain, partition, post-step samples and
   stable decoded fields as the live run. */
void
run_replay_and_compare(const Case& c, const dsd_test_forced_rate_input& in, const char* meta_path, const LiveFeed& feed,
                       const std::vector<float>& live_post, const std::vector<std::string>& live_lines) {
    std::vector<float> post;
    ReplayBlocks blocks;
    dsd_demod_test_set_post_step_tap(collect_post_step, &post);
    rtl_stream_test_set_replay_block_hook(collect_replay_block, &blocks);
    std::vector<std::string> args = base_args(c);
    args.emplace_back("--iq-replay");
    args.emplace_back(meta_path);
    RunView view;
    std::string log;
    const int rc = run_engine(args, &view, &log);
    rtl_stream_test_set_replay_block_hook(nullptr, nullptr);
    dsd_demod_test_set_post_step_tap(nullptr, nullptr);
    const std::vector<std::string> lines = log_lines(log);

    check(std::string(c.name) + " replay: the run ended cleanly", rc == 0);
    check_thresholds(c, "replay", lines, view);
    check(std::string(c.name) + " replay: the demod ran a block", blocks.have_chain == 1);
    check_chain(c, "replay", blocks.chain, in);
    /* The plan key: the same chain, field by field. */
    check(std::string(c.name) + ": live and replay run the same chain",
          std::memcmp(&feed.chain, &blocks.chain, sizeof(feed.chain)) == 0);
    check(std::string(c.name) + ": the replay reads the live transfers' partition", blocks.pairs == feed.transfers);
    check(std::string(c.name) + ": the same samples after the step, bit for bit",
          live_post.size() == post.size() && !live_post.empty()
              && std::memcmp(live_post.data(), post.data(), live_post.size() * sizeof(float)) == 0);
    const std::vector<std::string> live_stable = stable_lines(live_lines, c);
    check_stable_fields(c, live_stable, stable_lines(lines, c));
    DSD_FPRINTF(stderr, "%s: %zu transfers of %zu pairs, %zu post-step pairs, %zu stable lines\n", c.name,
                feed.transfers.size(), feed.transfer_pairs, live_post.size() / 2U, live_stable.size());
}

int
run_case(const Case& c) {
    char err[256] = {0};
    dsd_test_forced_rate_input in;
    if (dsd_test_forced_rate_input_make(DSD_NEO_TEST_IQ_FIXTURE_DIR, &c.spec, &in, err, sizeof(err)) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: %s\n", c.name, err);
        return 1;
    }
    char dir[DSD_TEST_PATH_MAX];
    char data_path[DSD_TEST_PATH_MAX];
    char meta_path[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_forced_rate")
        || dsd_test_path_join(data_path, sizeof data_path, dir, "live.iq") != 0
        || dsd_test_path_join(meta_path, sizeof meta_path, dir, "live.iq.json") != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: no temp directory\n", c.name);
        dsd_test_forced_rate_input_free(&in);
        return 1;
    }
    /* The SDK lists only the forced rate, so the adapter opens the device there. */
    airspy_double::rates[0] = c.spec.device_rate_hz;
    airspy_double::rates[1] = c.spec.device_rate_hz;

    LiveFeed feed;
    std::vector<float> live_post;
    std::vector<std::string> live_lines;
    if (run_live(c, in, data_path, &feed, &live_post, &live_lines) && !c.retune) {
        check_capture(c, meta_path, in);
        run_replay_and_compare(c, in, meta_path, feed, live_post, live_lines);
    }
    static const char* const kFiles[] = {"live.iq", "live.iq.json", nullptr};
    (void)dsd_test_remove_temp_dir(dir, kFiles);
    dsd_test_forced_rate_input_free(&in);
    return 0;
}

} // namespace

int
main(int argc, char** argv) {
    if (argc != 2) {
        DSD_FPRINTF(stderr, "usage: %s <case>\n", argc > 0 ? argv[0] : "test_engine_airspy_forced_rate");
        return 2;
    }
    const Case* c = nullptr;
    for (const Case& k : kCases) {
        if (std::strcmp(k.name, argv[1]) == 0) {
            c = &k;
        }
    }
    if (!c) {
        DSD_FPRINTF(stderr, "unknown case %s\n", argv[1]);
        return 2;
    }
    if (run_case(*c) != 0) {
        return 1;
    }
    for (const std::string& f : g_failures) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", f.c_str());
    }
    if (!g_failures.empty()) {
        return 1;
    }
    DSD_FPRINTF(stderr, "ENGINE_AIRSPY_FORCED_RATE_%s: OK\n", c->name);
    return 0;
}
