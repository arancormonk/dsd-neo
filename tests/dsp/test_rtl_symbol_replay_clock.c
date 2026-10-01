// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief An I/Q replay's samples run the decode clock as they reach symbol processing (issue #572).
 *
 * A fake paced replay hands getSymbol() a continuous 4800 symbols/s capture in batches of a chosen size, each read
 * taking samples of one batch only, tagged with the batch's media span, as rtl_stream_read() does under --iq-replay.
 * Through the real symbol cache (the CQPSK fast path), each sample handed out moves the decode clock's media time to
 * its own capture time, whatever the batch boundaries. A call-state heal decision 1199 and 1201 symbols after an
 * unverified terminator, either side of the 250 ms heal window, then comes out the same for every batch size, and
 * right. The analog monitor, read one sample at a time outside the cache, moves the clock per sample too.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

enum {
    kSymbolRateHz = 4800,
    /* The capture, in samples: enough for the call, its end and the gap after it. */
    kCaptureSamples = 6000,
};

/* An anchor well away from any platform clock reading, so a decision on the wrong clock cannot land right. */
static const int64_t kAnchorS = 1788245497LL; /* 2026-09-01T06:51:37Z */

static int g_failures = 0;

/* The fake replay: its output kind, the batch size, and the next sample it hands out. */
static int g_output_kind = RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
static uint32_t g_batch_samples = 512U;
static uint64_t g_next_sample = 0U;
static int g_have_batch = 0;
static dsd_rtl_stream_replay_batch g_last_batch;
static uint64_t g_last_read_first = 0U; /* the capture sample the last read returned first */

static void
check(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        g_failures++;
    }
}

/* Capture time of sample @p n of the capture, in ns, rounded down. */
static uint64_t
capture_media_ns(uint64_t n) {
    return (n * 1000000000ULL) / (uint64_t)kSymbolRateHz;
}

/* Media time the decode clock should read once sample @p n has reached symbol processing: its place in its batch, on
   the batch's span. */
static uint64_t
sample_media_ns(uint64_t n, uint32_t batch) {
    const uint64_t first = (n / batch) * batch;
    const uint64_t start = capture_media_ns(first);
    return dsd_decode_clock_batch_media_ns(start, capture_media_ns(first + batch) - start, batch,
                                           (uint32_t)(n - first));
}

/* rtl_stream_read() under a paced replay: the samples of one batch, at most @p count, never across its end. */
static int
fake_rtl_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    (void)rtl_ctx;
    if (!out || !out_got || count == 0U || g_next_sample >= kCaptureSamples) {
        if (out_got) {
            *out_got = 0;
        }
        return -1;
    }
    const uint64_t batch_first = (g_next_sample / g_batch_samples) * g_batch_samples;
    const uint32_t index = (uint32_t)(g_next_sample - batch_first);
    size_t n = (size_t)(g_batch_samples - index);
    if (n > count) {
        n = count;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = 1.0f;
    }
    const uint64_t start = capture_media_ns(batch_first);
    g_last_batch = (dsd_rtl_stream_replay_batch){
        .generation = 1U,
        .output_kind = g_output_kind,
        .channel_profile = g_output_kind == RTL_STREAM_OUTPUT_SYMBOL_CQPSK ? RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK : 0,
        .symbol_rate_hz = g_output_kind == RTL_STREAM_OUTPUT_SYMBOL_CQPSK ? kSymbolRateHz : 0,
        .levels = 4,
        .media_start_ns = start,
        .media_duration_ns = capture_media_ns(batch_first + g_batch_samples) - start,
        .output_count = g_batch_samples,
        .first_index = index,
    };
    g_have_batch = 1;
    g_last_read_first = g_next_sample;
    g_next_sample += (uint64_t)n;
    *out_got = (int)n;
    return 0;
}

static int
fake_replay_batch(dsd_rtl_stream_replay_batch* out) {
    if (!g_have_batch) {
        return 0;
    }
    *out = g_last_batch;
    return 1;
}

static double
fake_rtl_pwr(const void* rtl_ctx) {
    (void)rtl_ctx;
    return 0.0;
}

static int
fake_output_kind(void) {
    return g_output_kind;
}

static int
fake_symbol_profile(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    const int cqpsk = g_output_kind == RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = cqpsk ? kSymbolRateHz : 0;
    }
    if (out_levels) {
        *out_levels = 4;
    }
    if (out_channel_profile) {
        *out_channel_profile = cqpsk ? RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK : 0;
    }
    return 0;
}

static uint32_t
fake_stream_generation(void) {
    return 1U;
}

static unsigned int
fake_output_rate_hz(void) {
    return g_output_kind == RTL_STREAM_OUTPUT_SYMBOL_CQPSK ? (unsigned int)kSymbolRateHz : 48000U;
}

static int g_fake_rtl_context = 0;

static void
install_fake_replay(void) {
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){.read = fake_rtl_read, .return_pwr = fake_rtl_pwr});
    static const dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .output_kind = fake_output_kind,
        .symbol_profile = fake_symbol_profile,
        .stream_generation = fake_stream_generation,
        .replay_batch = fake_replay_batch,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
}

static void
reset_replay(dsd_opts* opts, dsd_state* state, int output_kind, uint32_t batch) {
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtl_volume_multiplier = 1;
    state->rtl_ctx = (struct RtlSdrContext*)&g_fake_rtl_context;
    state->rf_mod = output_kind == RTL_STREAM_OUTPUT_SYMBOL_CQPSK ? 1 : 0;
    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    g_output_kind = output_kind;
    g_batch_samples = batch;
    g_next_sample = 0U;
    g_have_batch = 0;
    g_last_read_first = 0U;
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    dsd_decode_clock_use_replay(kAnchorS);
}

/* Read CQPSK symbols until sample @p last has been handed out, checking after each that the decode clock reads the
   capture time of the sample just handed out. */
static void
read_symbols_through(dsd_opts* opts, dsd_state* state, uint64_t* handed_out, uint64_t last, const char* label) {
    const uint64_t anchor_ns = (uint64_t)kAnchorS * 1000000000ULL;
    for (; *handed_out <= last; (*handed_out)++) {
        (void)getSymbol(opts, state, 0);
        const uint64_t want = anchor_ns + sample_media_ns(*handed_out, g_batch_samples);
        const uint64_t got = dsd_decode_now_mono_ns();
        if (got != want) {
            DSD_FPRINTF(stderr,
                        "FAIL: %s, %u-sample batches: after sample %llu the decode clock reads %llu, want %llu\n",
                        label, g_batch_samples, (unsigned long long)*handed_out, (unsigned long long)got,
                        (unsigned long long)want);
            g_failures++;
            *handed_out = last + 1U;
            return;
        }
    }
}

static dsd_call_observation
voice_observation(uint64_t target, uint64_t source, dsd_call_kind kind) {
    dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_P25P1_POS, 0U, source, target);
    observation.kind = kind;
    observation.policy_target_id = target;
    /* observed_m 0.0: the call store stamps it from the decode clock, as the protocol paths that pass none do. */
    observation.observed_m = 0.0;
    return observation;
}

/* An identified P25 call on a CQPSK replay ends on an unverified terminator at sample @p end; an identity-less
   continuation follows @p gap samples later. Returns 1 when it healed the ended call, 0 when it opened a new one, -1 on
   a fixture failure. */
static int
heal_after_gap(dsd_opts* opts, dsd_state* state, uint32_t batch, uint64_t end, uint64_t gap) {
    reset_replay(opts, state, RTL_STREAM_OUTPUT_SYMBOL_CQPSK, batch);
    if (!dsd_call_state_ensure(state)) {
        return -1;
    }
    char label[96];
    DSD_SNPRINTF(label, sizeof(label), "heal case, gap %llu", (unsigned long long)gap);
    uint64_t handed_out = 0U;
    read_symbols_through(opts, state, &handed_out, 100U, label);
    dsd_call_observation call = voice_observation(1234U, 5678U, DSD_CALL_KIND_GROUP_VOICE);
    if (dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN) <= 0) {
        return -1;
    }
    read_symbols_through(opts, state, &handed_out, end, label);
    if (dsd_call_state_end_ex(state, 0U, 0.0, DSD_CALL_END_UNVERIFIED_TERMINATOR) <= 0) {
        return -1;
    }
    read_symbols_through(opts, state, &handed_out, end + gap, label);
    dsd_call_observation continuation = voice_observation(0U, 0U, DSD_CALL_KIND_VOICE);
    (void)dsd_call_state_observe(state, &continuation, DSD_CALL_BOUNDARY_BEGIN);
    dsd_call_snapshot snapshot;
    if (dsd_call_state_get(state, 0U, &snapshot) <= 0) {
        return -1;
    }
    return snapshot.ota_target_id == 1234U ? 1 : 0;
}

/* The heal window after an unverified terminator is 250 ms: 1199 symbols (249.79 ms) later a continuation heals, 1201
   (250.21 ms) later it does not. Each sample runs the clock to its own capture time, so both come out the same for
   every batch size: a batch-granular clock would be up to a batch early, 107 ms at 512 samples. */
static void
test_heal_decision_does_not_depend_on_batches(dsd_opts* opts, dsd_state* state) {
    static const uint32_t kBatches[] = {512U, 300U, 127U, 37U, 1U};
    /* The end sits part way into a batch for every size above. */
    const uint64_t end = 2345U;
    for (size_t i = 0; i < sizeof(kBatches) / sizeof(kBatches[0]); i++) {
        char label[128];
        const int inside = heal_after_gap(opts, state, kBatches[i], end, 1199U);
        DSD_SNPRINTF(label, sizeof(label), "%u-sample batches: a continuation 1199 symbols after the end heals it",
                     kBatches[i]);
        check(label, inside == 1);
        const int outside = heal_after_gap(opts, state, kBatches[i], end, 1201U);
        DSD_SNPRINTF(label, sizeof(label),
                     "%u-sample batches: a continuation 1201 symbols after the end opens a new call", kBatches[i]);
        check(label, outside == 0);
    }
}

/* The analog monitor is read one sample at a time outside the cache (symbol_read_sample_rtl()): each read runs the
   clock to the capture time of the sample it returned. */
static void
test_monitor_reads_run_the_clock(dsd_opts* opts, dsd_state* state) {
    reset_replay(opts, state, RTL_STREAM_OUTPUT_AUDIO_MONITOR, 37U);
    const uint64_t anchor_ns = (uint64_t)kAnchorS * 1000000000ULL;
    for (int i = 0; i < 40; i++) {
        (void)getSymbol(opts, state, 0);
        const uint64_t want = anchor_ns + sample_media_ns(g_last_read_first, g_batch_samples);
        if (dsd_decode_now_mono_ns() != want) {
            DSD_FPRINTF(stderr, "FAIL: monitor symbol %d: the decode clock reads %llu, want %llu (sample %llu)\n", i,
                        (unsigned long long)dsd_decode_now_mono_ns(), (unsigned long long)want,
                        (unsigned long long)g_last_read_first);
            g_failures++;
            return;
        }
    }
    check("the monitor read samples one at a time", g_next_sample > 40U && g_next_sample == g_last_read_first + 1U);
}

/* A live read carries no batch, so it moves nothing: the decode clock stays where it was. */
static void
test_live_reads_leave_the_clock(dsd_opts* opts, dsd_state* state) {
    reset_replay(opts, state, RTL_STREAM_OUTPUT_SYMBOL_CQPSK, 512U);
    dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .output_kind = fake_output_kind,
        .symbol_profile = fake_symbol_profile,
        .stream_generation = fake_stream_generation,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    for (int i = 0; i < 600; i++) {
        (void)getSymbol(opts, state, 0);
    }
    check("live samples leave the decode clock at its anchor",
          dsd_decode_now_mono_ns() == (uint64_t)kAnchorS * 1000000000ULL);
    install_fake_replay();
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_replay();

    test_heal_decision_does_not_depend_on_batches(&opts, &state);
    test_monitor_reads_run_the_clock(&opts, &state);
    test_live_reads_leave_the_clock(&opts, &state);

    dsd_decode_clock_use_system();
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    dsd_state_ext_free_all(&state);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d replay clock check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
