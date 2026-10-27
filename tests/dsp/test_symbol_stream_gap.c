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
#include <stdlib.h>
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

/* A live gap that lands right after a read: the read that returns sample g_race_at (0-based) took it off the stream
   before the gap's output clear, and the gap then completes at once, before the reader checks the count. The clear
   moves the generation and drops what was left before the gap, so the stream goes on at kPostGapFirst. */
enum { kPostGapFirst = 2001 };

static uint64_t g_race_at = UINT64_MAX;
static int g_race_landed = 0;
static uint32_t g_generation = 1U;
/* An I/Q replay whose read of the first sample past a recorded gap began before the generation moved for it. */
static int g_replay_gap_moves_generation = 0;
/* A live generation advance (a queued retune, another reconnect) right after the reader's g_advance_armed-th next read
   of the generation; g_advance_after_gap_read arms it for the first read after the one that returns the first sample
   past the gap (the reader validating that sample). */
static int g_advance_after_gap_read = 0;
static int g_advance_armed = 0;

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
    if (g_replay && g_replay_gap_moves_generation && g_next == g_gap_at) {
        g_generation++;
    }
    if (!g_replay && g_advance_after_gap_read && g_next == g_gap_at) {
        g_advance_armed = 1;
    }
    const uint64_t batch_first = (g_next / g_batch) * g_batch;
    size_t n = (size_t)(batch_first + g_batch - g_next);
    if (n > count) {
        n = count;
    }
    if (g_next < g_gap_at && g_gap_at - g_next < n) {
        n = (size_t)(g_gap_at - g_next);
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
        .boundary_seq = g_next >= g_gap_at ? 1U : 0U,
    };
    g_have_batch = 1;
    g_next += (uint64_t)n;
    if (!g_race_landed && g_race_at != UINT64_MAX && g_next > g_race_at) {
        g_race_landed = 1;
        g_generation++;
        g_next = (uint64_t)kPostGapFirst - 1U;
    }
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
    if (g_race_landed) {
        return 1U;
    }
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
    const uint32_t generation = g_generation;
    if (g_advance_armed > 0) {
        g_advance_armed--;
        if (g_advance_armed == 0) {
            g_generation++;
        }
    }
    return generation;
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
    g_race_at = UINT64_MAX;
    g_race_landed = 0;
    g_generation = 1U;
    g_replay_gap_moves_generation = 0;
    g_advance_after_gap_read = 0;
    g_advance_armed = 0;
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
    reset_stream(opts, state, 1, RTL_STREAM_OUTPUT_SYMBOL_CQPSK, 37U, (uint64_t)3U * 37U);
    read_across_the_gap(opts, state, "replay, cached reader", (uint64_t)3U * 37U);
}

/* The same through the one-sample reader (the monitor output): the read that returns the batch's first sample sees the
   gap, and that sample is handed out first once the engine has consumed it. */
static void
test_recovery_events_reset_decoder_one_sample_reader(dsd_opts* opts, dsd_state* state) {
    reset_stream(opts, state, 1, RTL_STREAM_OUTPUT_AUDIO_MONITOR, 37U, (uint64_t)3U * 37U);
    read_across_the_gap(opts, state, "replay, one-sample reader", (uint64_t)3U * 37U);
}

/* The same with the generation moving while the read of the first sample past the gap waits: the batch label places
   that sample past the gap, so it is still held and handed out first once the gap is consumed. */
static void
test_replay_one_sample_reader_keeps_labelled_sample_across_generation(dsd_opts* opts, dsd_state* state) {
    reset_stream(opts, state, 1, RTL_STREAM_OUTPUT_AUDIO_MONITOR, 37U, (uint64_t)3U * 37U);
    g_replay_gap_moves_generation = 1;
    read_across_the_gap(opts, state, "replay, one-sample reader, generation moved", (uint64_t)3U * 37U);
}

/* A live stream through the cache: the count is read after every read, so the read that returns the first sample past
   the gap raises it, before any of that read's samples is handed out. They follow once it is consumed; the fake, like the real output ring, ends each read at the gap. */
static void
test_live_gap_checked_after_each_cached_read(dsd_opts* opts, dsd_state* state) {
    reset_stream(opts, state, 0, RTL_STREAM_OUTPUT_SYMBOL_CQPSK, 64U, 100U);
    read_across_the_gap(opts, state, "live, cached reader", 100U);
}

static void
test_live_gap_checked_after_each_one_sample_read(dsd_opts* opts, dsd_state* state) {
    reset_stream(opts, state, 0, RTL_STREAM_OUTPUT_AUDIO_MONITOR, 64U, 100U);
    read_across_the_gap(opts, state, "live, one-sample reader", 100U);
}

/* A live gap that completes between a one-sample read and the count check after it (a recovery or reconnect finishing
   meanwhile): that read's sample came off the stream before the gap, under the generation before it. The gap is raised,
   but the sample is dropped rather than held, so once the engine consumes the gap the first sample read is the first
   past it, and nothing from before the gap reaches the reset decoder. */
static void
test_live_gap_after_one_sample_read_drops_pre_gap_sample(dsd_opts* opts, dsd_state* state) {
    reset_stream(opts, state, 0, RTL_STREAM_OUTPUT_AUDIO_MONITOR, 64U, UINT64_MAX);
    g_race_at = 49U;
    int in_order = 1;
    for (uint64_t want = 1U; want <= 49U; want++) {
        in_order &= reads_sample(getSymbol(opts, state, 0), want) && state->input_interrupted == 0;
    }
    check("live race: the samples before the racing read come out in order", in_order);
    (void)getSymbol(opts, state, 0);
    check("live race: the gap is raised", state->input_interrupted == 1
                                              && state->input_interrupt_reason == DSD_INTERRUPT_STREAM_GAP
                                              && state->rtl_stream_gap_seq == 1U);
    engine_consumes(state);
    const float first = getSymbol(opts, state, 0);
    char label[128];
    DSD_SNPRINTF(label, sizeof label, "live race: the first sample after the gap is past it (read %.0f)",
                 (double)first);
    check(label, reads_sample(first, (uint64_t)kPostGapFirst));
    int after = 1;
    for (uint64_t want = (uint64_t)kPostGapFirst + 1U; want < (uint64_t)kPostGapFirst + 40U; want++) {
        after &= reads_sample(getSymbol(opts, state, 0), want) && state->input_interrupted == 0;
    }
    check("live race: the stream goes on in order past the gap", after);
}

/* A live gap, then the generation advances right after the reader validated the first sample past it against the
   generation its read began under, before the sample is held. The sample is held under that validated generation, so
   once the engine consumes the gap it is no longer current and is dropped: the first symbol is the next sample. */
static void
test_live_held_sample_keeps_validated_generation(dsd_opts* opts, dsd_state* state) {
    reset_stream(opts, state, 0, RTL_STREAM_OUTPUT_AUDIO_MONITOR, 64U, 100U);
    g_advance_after_gap_read = 1;
    int in_order = 1;
    for (uint64_t want = 1U; want <= 100U; want++) {
        in_order &= reads_sample(getSymbol(opts, state, 0), want) && state->input_interrupted == 0;
    }
    check("live advance at hold: the samples before the gap come out in order", in_order);
    (void)getSymbol(opts, state, 0);
    check("live advance at hold: the gap is raised",
          state->input_interrupted == 1 && state->input_interrupt_reason == DSD_INTERRUPT_STREAM_GAP);
    check("live advance at hold: the generation advanced at the hold", g_generation == 2U);
    engine_consumes(state);
    const float first = getSymbol(opts, state, 0);
    char label[128];
    DSD_SNPRINTF(label, sizeof label, "live advance at hold: the held sample is dropped (read %.0f)", (double)first);
    check(label, reads_sample(first, 102U));
}

/* A live gap, its sample held, and the generation advances right after the reader validated the held sample at its
   hand-out. The sample is handed out under the generation it was validated against, so the analog block it starts is
   labelled with that one, not the one after the advance. */
static void
test_live_handed_out_sample_keeps_validated_generation(dsd_opts* opts, dsd_state* state) {
    reset_stream(opts, state, 0, RTL_STREAM_OUTPUT_AUDIO_MONITOR, 64U, 100U);
    for (uint64_t want = 1U; want <= 100U; want++) {
        (void)getSymbol(opts, state, 0);
    }
    (void)getSymbol(opts, state, 0);
    check("live advance at hand-out: the gap is raised", state->input_interrupted == 1);
    engine_consumes(state);
    /* A symbol read takes the stream's profile (its generation) first, then validates the held sample. */
    g_advance_armed = 2;
    const float first = getSymbol(opts, state, 0);
    check("live advance at hand-out: the held sample comes out first", reads_sample(first, 101U));
    check("live advance at hand-out: the generation advanced at the hand-out", g_generation == 2U);
    char label[128];
    DSD_SNPRINTF(label, sizeof label, "live advance at hand-out: its block is labelled with generation 1 (got %u)",
                 (unsigned)state->analog_block_rtl_gen);
    check(label, state->analog_block_rtl_gen == 1U);
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

/* A direct reader can acknowledge a gap while an older symbol cache still exists in the same generation. */
static void
test_direct_reader_acknowledgment_drops_old_cache(dsd_opts* opts, dsd_state* state) {
    reset_stream(opts, state, 0, RTL_STREAM_OUTPUT_SYMBOL_CQPSK, 64U, UINT64_MAX);
    check("old cache starts before the gap", reads_sample(getSymbol(opts, state, 0), 1U));
    check("old cache has unread samples", state->rtl_symbol_cache_pos < state->rtl_symbol_cache_len);
    const uint64_t next = g_next + 1U;
    g_gap_at = g_next;
    state->rtl_stream_gap_seq = 1U;
    const float symbol = getSymbol(opts, state, 0);
    check("direct acknowledgment discards old samples", reads_sample(symbol, next));
    check("direct acknowledgment never moves backwards", state->rtl_stream_gap_seq == 1U);
    check("direct acknowledgment is not raised again", state->input_interrupted == 0);
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    install_fake_stream();

    test_direct_reader_acknowledgment_drops_old_cache(opts, state);
    test_no_gap_reads_through(opts, state);
    test_recovery_events_reset_decoder_cached_reader(opts, state);
    test_recovery_events_reset_decoder_one_sample_reader(opts, state);
    test_replay_one_sample_reader_keeps_labelled_sample_across_generation(opts, state);
    test_live_gap_checked_after_each_cached_read(opts, state);
    test_live_gap_checked_after_each_one_sample_read(opts, state);
    test_live_gap_after_one_sample_read_drops_pre_gap_sample(opts, state);
    test_live_held_sample_keeps_validated_generation(opts, state);
    test_live_handed_out_sample_keeps_validated_generation(opts, state);

    dsd_decode_clock_use_system();
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d stream gap check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
