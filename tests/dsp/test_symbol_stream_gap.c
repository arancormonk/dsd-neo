// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief A gap in the radio stream reaches the symbol reader in order (issue #518 D5).
 *
 * A fake RTL stream hands getSymbol() samples numbered from 1, so each symbol read on the CQPSK symbol output, and on
 * the monitor output at one sample a symbol, is the number of the sample it took. A gap sits before one sample:
 *  - an I/Q replay carries it in the batch tag (boundary_seq) of the batch that starts there, as a recorded USB
 *    recovery or rtl_tcp reconnect marks the batch after it; the stream's own count moves once every sample before it
 *    has been read, as the replay applies its events on an idle pipeline;
 *  - a live stream moves its count before the first sample past the gap can be read, and the reader checks it after
 *    every read.
 * Through the symbol cache and the one-sample reader alike, every sample before the gap comes out in order, then the
 * read that would hand out the first sample past it raises the stream-gap interruption instead. While it stands every
 * symbol read is an unusable 0 and the stream is not read. Once the engine has consumed it, reading resumes at the
 * first sample past the gap with none lost, and the count the decoder took is the stream's. A Signal search
 * interruption raised over it does not lower its reason.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

enum {
    kSymbolRateHz = 4800,
    kCaptureSamples = 4000,
};

static int g_failures = 0;

/* The fake stream: replay or live, its output kind, the read size (a replay's batch, a live read's most), the next
   sample it hands out (0-based; the value is one more), the first sample past the gap, and the reads it served. */
static int g_replay = 1;
static int g_output_kind = RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
static uint32_t g_batch = 37U;
static uint64_t g_next = 0U;
static uint64_t g_gap_at = UINT64_MAX;
static int g_reads = 0;
static int g_have_batch = 0;
static dsd_rtl_stream_replay_batch g_last_batch;

/* Whether symbol @p got is sample @p want: the samples are whole numbers, so within a quarter. */
static int
reads_sample(float got, uint64_t want) {
    return fabsf(got - (float)want) < 0.25f;
}

static void
check(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        g_failures++;
    }
}

static int
fake_read_ex(void* rtl_ctx, float* out, uint8_t* flags, size_t count, int* out_got) {
    (void)rtl_ctx;
    if (!out || !out_got || count == 0U || g_next >= kCaptureSamples) {
        if (out_got) {
            *out_got = 0;
        }
        return -1;
    }
    g_reads++;
    const uint64_t batch_first = (g_next / g_batch) * g_batch;
    size_t n = (size_t)(batch_first + g_batch - g_next);
    if (n > count) {
        n = count;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = (float)(g_next + i + 1U);
        if (flags) {
            flags[i] = 0U;
        }
    }
    g_last_batch = (dsd_rtl_stream_replay_batch){
        .generation = 1U,
        .output_kind = g_output_kind,
        .channel_profile = g_output_kind == RTL_STREAM_OUTPUT_SYMBOL_CQPSK ? RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK : 0,
        .symbol_rate_hz = g_output_kind == RTL_STREAM_OUTPUT_SYMBOL_CQPSK ? kSymbolRateHz : 0,
        .levels = 4,
        .output_count = g_batch,
        .first_index = (uint32_t)(g_next - batch_first),
        .center_frequency_hz = 851012500U,
        .boundary_seq = batch_first >= g_gap_at ? 1U : 0U,
    };
    g_have_batch = 1;
    g_next += (uint64_t)n;
    *out_got = (int)n;
    return 0;
}

static int
fake_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    return fake_read_ex(rtl_ctx, out, NULL, count, out_got);
}

static double
fake_pwr(const void* rtl_ctx) {
    (void)rtl_ctx;
    return 0.0;
}

static int
fake_replay_batch(dsd_rtl_stream_replay_batch* out) {
    if (!g_replay || !g_have_batch) {
        return 0;
    }
    *out = g_last_batch;
    return 1;
}

/* The stream's count. A replay applies the gap once every sample before it has been read; a live stream moves it
   before the first sample past the gap can be read, so a read that returned that sample sees it moved. */
static uint64_t
fake_discontinuity_seq(void) {
    if (g_gap_at == UINT64_MAX) {
        return 0U;
    }
    return (g_replay ? g_next >= g_gap_at : g_next > g_gap_at) ? 1U : 0U;
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
fake_generation(void) {
    return 1U;
}

/* One sample a symbol on the monitor output as on the symbol output. */
static unsigned int
fake_output_rate_hz(void) {
    return (unsigned int)kSymbolRateHz;
}

static int g_fake_rtl_context = 0;

static void
install_fake_stream(void) {
    dsd_rtl_stream_io_hooks_set(
        (dsd_rtl_stream_io_hooks){.read = fake_read, .return_pwr = fake_pwr, .read_ex = fake_read_ex});
    static const dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .output_kind = fake_output_kind,
        .symbol_profile = fake_symbol_profile,
        .stream_generation = fake_generation,
        .replay_batch = fake_replay_batch,
        .discontinuity_seq = fake_discontinuity_seq,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
}

static void
reset_stream(dsd_opts* opts, dsd_state* state, int replay, int output_kind, uint32_t batch, uint64_t gap_at) {
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtl_volume_multiplier = 1;
    state->rtl_ctx = (struct RtlSdrContext*)&g_fake_rtl_context;
    state->rf_mod = output_kind == RTL_STREAM_OUTPUT_SYMBOL_CQPSK ? 1 : 2;
    state->samplesPerSymbol = 1;
    state->symbolCenter = 0;
    g_replay = replay;
    g_output_kind = output_kind;
    g_batch = batch;
    g_next = 0U;
    g_gap_at = gap_at;
    g_reads = 0;
    g_have_batch = 0;
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    dsd_decode_clock_use_replay(1788245497LL);
}

/* The engine loop's consumption of the interruption (live_scanner_main_loop()). */
static void
engine_consumes(dsd_state* state) {
    state->input_interrupted = 0;
    state->input_interrupt_reason = DSD_INTERRUPT_NONE;
}

/* Read symbols until the gap is raised, then hold there, then resume: the samples come out numbered 1, 2, 3... with
   the first sample past the gap first after the resume. */
static void
read_across_the_gap(dsd_opts* opts, dsd_state* state, const char* name, uint64_t last_before_gap) {
    char label[192];
    uint64_t want = 1U;
    int raised = 0;
    int analog_collected = 0;
    for (int i = 0; i < 2 * kCaptureSamples && want <= last_before_gap + 40U; i++) {
        analog_collected = state->analog_sample_counter;
        const float symbol = getSymbol(opts, state, 0);
        if (state->input_interrupted) {
            raised = 1;
            break;
        }
        if (!reads_sample(symbol, want)) {
            DSD_SNPRINTF(label, sizeof label, "%s: symbol %llu reads sample %.0f", name, (unsigned long long)want,
                         (double)symbol);
            check(label, 0);
            return;
        }
        want++;
    }
    DSD_SNPRINTF(label, sizeof label, "%s: the gap is raised", name);
    check(label, raised);
    DSD_SNPRINTF(label, sizeof label, "%s: every sample before the gap came out first, in order (next %llu)", name,
                 (unsigned long long)want);
    check(label, want == last_before_gap + 1U);
    DSD_SNPRINTF(label, sizeof label, "%s: the interruption is a stream gap", name);
    check(label, state->input_interrupt_reason == DSD_INTERRUPT_STREAM_GAP);
    DSD_SNPRINTF(label, sizeof label, "%s: the decoder took the stream's count", name);
    check(label, state->rtl_stream_gap_seq == 1U);
    if (g_output_kind == RTL_STREAM_OUTPUT_AUDIO_MONITOR) {
        /* The analog block part-collected before the gap does not run on into the samples after it. */
        DSD_SNPRINTF(label, sizeof label, "%s: the part-collected analog block is dropped (%d samples)", name,
                     analog_collected);
        check(label, analog_collected > 0 && state->analog_sample_counter == 0);
    }

    /* Sticky until consumed: every read is an unusable 0, and none reaches the stream. */
    const int reads = g_reads;
    int sticky = 1;
    for (int i = 0; i < 300; i++) {
        state->symbol_replay_symbol_unusable = 0;
        const float symbol = getSymbol(opts, state, 1);
        sticky &= reads_sample(symbol, 0U) && state->symbol_replay_symbol_unusable == 1 && state->input_interrupted == 1
                  && state->input_interrupt_reason == DSD_INTERRUPT_STREAM_GAP;
    }
    DSD_SNPRINTF(label, sizeof label, "%s: every read until the engine consumes the gap stands an erasure in", name);
    check(label, sticky);
    DSD_SNPRINTF(label, sizeof label, "%s: no read reaches the stream while the gap stands", name);
    check(label, g_reads == reads);

    /* A Signal search interruption raised over it keeps the stream gap. */
    dsd_state_raise_input_interrupt(state, DSD_INTERRUPT_SEARCH);
    DSD_SNPRINTF(label, sizeof label, "%s: a lower reason does not replace the stream gap", name);
    check(label, state->input_interrupt_reason == DSD_INTERRUPT_STREAM_GAP);

    engine_consumes(state);
    int in_order = 1;
    for (int i = 0; i < 60; i++) {
        const float symbol = getSymbol(opts, state, 0);
        in_order &= reads_sample(symbol, want) && state->input_interrupted == 0;
        want++;
    }
    DSD_SNPRINTF(label, sizeof label, "%s: once consumed, reading resumes at the first sample past the gap", name);
    check(label, in_order);
}

/* An I/Q replay through the symbol cache (the CQPSK symbol output): the gap the capture recorded is consumed before
   the first sample of the batch after it, while the samples the cache held before it come out first, in order. */
static void
test_recovery_events_reset_decoder_cached_reader(dsd_opts* opts, dsd_state* state) {
    reset_stream(opts, state, 1, RTL_STREAM_OUTPUT_SYMBOL_CQPSK, 37U, 3U * 37U);
    read_across_the_gap(opts, state, "replay, cached reader", 3U * 37U);
}

/* The same through the one-sample reader (the monitor output): the read that returns the batch's first sample sees the
   gap, and that sample is handed out first once the engine has consumed it. */
static void
test_recovery_events_reset_decoder_one_sample_reader(dsd_opts* opts, dsd_state* state) {
    reset_stream(opts, state, 1, RTL_STREAM_OUTPUT_AUDIO_MONITOR, 37U, 3U * 37U);
    read_across_the_gap(opts, state, "replay, one-sample reader", 3U * 37U);
}

/* A live stream through the cache: the count is read after every read, so the read that returns the first sample past
   the gap raises it, before any of that read's samples is handed out. They follow once it is consumed; the samples of
   that read before the gap with them, never one past the gap before it. */
static void
test_live_gap_checked_after_each_cached_read(dsd_opts* opts, dsd_state* state) {
    reset_stream(opts, state, 0, RTL_STREAM_OUTPUT_SYMBOL_CQPSK, 64U, 100U);
    read_across_the_gap(opts, state, "live, cached reader", 64U);
}

static void
test_live_gap_checked_after_each_one_sample_read(dsd_opts* opts, dsd_state* state) {
    reset_stream(opts, state, 0, RTL_STREAM_OUTPUT_AUDIO_MONITOR, 64U, 100U);
    read_across_the_gap(opts, state, "live, one-sample reader", 100U);
}

/* No gap: the stream's count never moves and nothing is raised. */
static void
test_no_gap_reads_through(dsd_opts* opts, dsd_state* state) {
    reset_stream(opts, state, 1, RTL_STREAM_OUTPUT_SYMBOL_CQPSK, 37U, UINT64_MAX);
    int clean = 1;
    for (int i = 0; i < 500; i++) {
        clean &= reads_sample(getSymbol(opts, state, 0), (uint64_t)i + 1U) && state->input_interrupted == 0;
    }
    check("no gap: every sample reads through", clean);
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_stream();

    test_no_gap_reads_through(&opts, &state);
    test_recovery_events_reset_decoder_cached_reader(&opts, &state);
    test_recovery_events_reset_decoder_one_sample_reader(&opts, &state);
    test_live_gap_checked_after_each_cached_read(&opts, &state);
    test_live_gap_checked_after_each_one_sample_read(&opts, &state);

    dsd_decode_clock_use_system();
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    dsd_state_ext_free_all(&state);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d stream gap check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
