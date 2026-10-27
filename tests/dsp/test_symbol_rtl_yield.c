// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief A live RTL read that gave way to a queued command reaches the symbol reader as a yield (issue #518 D4).
 *
 * A fake RTL stream hands getSymbol() samples numbered from 1 and, while told to, returns DSD_RTL_READ_YIELD: what a
 * live read returns when it found nothing, the device has no stream (a USB stall recovery runs, rtl_tcp is not
 * connected) and a command is queued. Through the CQPSK symbol cache, the FSK discriminator's cache and the one-sample
 * reader alike:
 *  - the yield is not a failure: nothing asks for shutdown, and a Signal search is not involved;
 *  - it raises the yield interruption, which stands for every symbol read until the engine loop consumes it: each is an
 *    unusable 0 and none reaches the stream. A search interruption raised over it keeps it; a stream gap outranks it;
 *  - once consumed, reading resumes at the next sample, none lost.
 * The contracts beside it stay: a failed read (-1) asks for shutdown outside a search and is a search interruption
 * inside one, and an idle NFM monitor's empty read (0 samples) interrupts the hunt without standing.
 * An rtl_tcp server that comes back between two reads (its reconnect moved the stream's discontinuity count) does not
 * cut the yield short, and the gap it left is not lost behind it.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

enum {
    kSymbolRateHz = 4800,
    kFskRateHz = 48000,
    kCaptureSamples = 200000,
};

static int g_failures = 0;

/* The shared stubs' count of shutdown requests (frame_sync_dsp_stubs.c). */
extern int g_frame_sync_dsp_stub_shutdown_requests;

/* The fake live stream: its output kind, the most a read returns, the next sample it hands out (0-based; the value is
   one more), what a read returns instead of samples while set (DSD_RTL_READ_YIELD, -1, or 0 for an empty read), and
   the reads that reached it. */
static int g_output_kind = RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
static uint32_t g_read_size = 64U;
static uint64_t g_next = 0U;
static int g_instead = 1;
static int g_reads = 0;
static int g_search_running = 0;
/* The stream's discontinuity count: a reconnect moves it before any sample after it can be read. */
static uint64_t g_discontinuity = 0U;

static void
check(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        g_failures++;
    }
}

/* Whether symbol @p got is sample @p want: the samples are whole numbers, so within a quarter. */
static int
reads_sample(float got, uint64_t want) {
    return fabsf(got - (float)want) < 0.25f;
}

static int
fake_read_ex(void* rtl_ctx, float* out, uint8_t* flags, size_t count, int* out_got) {
    (void)rtl_ctx;
    if (out_got) {
        *out_got = 0;
    }
    if (!out || !out_got || count == 0U || g_next >= kCaptureSamples) {
        return -1;
    }
    g_reads++;
    if (g_instead != 1) {
        return g_instead;
    }
    size_t n = g_read_size < count ? g_read_size : count;
    for (size_t i = 0; i < n; i++) {
        out[i] = (float)(g_next + i + 1U);
        if (flags) {
            flags[i] = 0U;
        }
    }
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
fake_output_kind(void) {
    return g_output_kind;
}

static int
fake_symbol_profile(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    const int direct = g_output_kind != RTL_STREAM_OUTPUT_AUDIO_MONITOR;
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = direct ? kSymbolRateHz : 0;
    }
    if (out_levels) {
        *out_levels = 4;
    }
    if (out_channel_profile) {
        *out_channel_profile =
            g_output_kind == RTL_STREAM_OUTPUT_SYMBOL_CQPSK ? RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK : 0;
    }
    return 0;
}

static uint32_t
fake_generation(void) {
    return 1U;
}

/* One sample a symbol on the CQPSK and monitor outputs; ten on the FSK discriminator's. */
static unsigned int
fake_output_rate_hz(void) {
    return g_output_kind == RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR ? (unsigned int)kFskRateHz
                                                                : (unsigned int)kSymbolRateHz;
}

static int
fake_search_running(const dsd_state* state) {
    (void)state;
    return g_search_running;
}

static uint64_t
fake_discontinuity_seq(void) {
    return g_discontinuity;
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
        .discontinuity_seq = fake_discontinuity_seq,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    dsd_signal_search_active_set(fake_search_running);
}

static void
reset_stream(dsd_opts* opts, dsd_state* state, int output_kind) {
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtl_volume_multiplier = 1;
    state->rtl_ctx = (struct RtlSdrContext*)&g_fake_rtl_context;
    state->rf_mod = output_kind == RTL_STREAM_OUTPUT_SYMBOL_CQPSK ? 1 : 2;
    state->samplesPerSymbol = output_kind == RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR ? 10 : 1;
    state->symbolCenter = 0;
    g_output_kind = output_kind;
    g_read_size = 64U;
    g_next = 0U;
    g_instead = 1;
    g_reads = 0;
    g_search_running = 0;
    g_discontinuity = 0U;
    g_frame_sync_dsp_stub_shutdown_requests = 0;
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    dsd_decode_clock_use_replay(1788245497LL);
}

/* The engine loop's consumption of the interruption (live_scanner_main_loop()). */
static void
engine_consumes(dsd_state* state) {
    state->input_interrupted = 0;
    state->input_interrupt_reason = DSD_INTERRUPT_NONE;
}

/* yield_not_taken_as_failure_by_caches and yield_is_sticky_until_consumed, on one output: read on, then the stream
   yields part way through, then resume. */
static void
yield_across(dsd_opts* opts, dsd_state* state, int output_kind, const char* name) {
    char label[192];
    reset_stream(opts, state, output_kind);
    for (int i = 0; i < 200; i++) {
        (void)getSymbol(opts, state, 0);
    }
    DSD_SNPRINTF(label, sizeof label, "%s: the stream reads before the yield", name);
    check(label, state->input_interrupted == 0 && g_next > 0U);

    /* Every sample the reader took comes out before the yield; the read that yields is the one that finds none. */
    g_instead = DSD_RTL_READ_YIELD;
    int raised = 0;
    for (int i = 0; i < 400 && !raised; i++) {
        (void)getSymbol(opts, state, 1);
        raised = state->input_interrupted;
    }
    const uint64_t taken = g_next;
    DSD_SNPRINTF(label, sizeof label, "%s: the yield is no failure (no shutdown)", name);
    check(label, g_frame_sync_dsp_stub_shutdown_requests == 0);
    DSD_SNPRINTF(label, sizeof label, "%s: the yield raises the yield interruption", name);
    check(label, raised && state->input_interrupt_reason == DSD_INTERRUPT_YIELD);
    DSD_SNPRINTF(label, sizeof label, "%s: the symbol the yield cut is unusable", name);
    check(label, state->symbol_replay_symbol_unusable == 1);

    /* Sticky until consumed, the stream yielding or not: every read an unusable 0, none reaching the stream. */
    const int reads = g_reads;
    int sticky = 1;
    for (int i = 0; i < 300; i++) {
        g_instead = (i & 1) ? 1 : DSD_RTL_READ_YIELD;
        state->symbol_replay_symbol_unusable = 0;
        const float symbol = getSymbol(opts, state, 1);
        sticky &= reads_sample(symbol, 0U) && state->symbol_replay_symbol_unusable == 1 && state->input_interrupted == 1
                  && state->input_interrupt_reason == DSD_INTERRUPT_YIELD;
    }
    DSD_SNPRINTF(label, sizeof label, "%s: every read until the engine consumes the yield stands an erasure in", name);
    check(label, sticky);
    DSD_SNPRINTF(label, sizeof label, "%s: no read reaches the stream while the yield stands", name);
    check(label, g_reads == reads && g_next == taken);
    DSD_SNPRINTF(label, sizeof label, "%s: no shutdown while the yield stands", name);
    check(label, g_frame_sync_dsp_stub_shutdown_requests == 0);

    /* A search interruption raised over it keeps the yield; a stream gap outranks it. */
    dsd_state_raise_input_interrupt(state, DSD_INTERRUPT_SEARCH);
    DSD_SNPRINTF(label, sizeof label, "%s: a search interruption does not replace the yield", name);
    check(label, state->input_interrupt_reason == DSD_INTERRUPT_YIELD);

    /* Consumed: reading resumes on the stream (sample by sample in yield_resumes_at_the_next_sample()). */
    g_instead = 1;
    engine_consumes(state);
    int resumed = 1;
    for (int i = 0; i < 40; i++) {
        (void)getSymbol(opts, state, 0);
        resumed &= state->input_interrupted == 0;
    }
    DSD_SNPRINTF(label, sizeof label, "%s: once consumed, reading resumes", name);
    check(label, resumed && g_next > taken);
}

/* On the one-sample outputs every symbol is one sample, so the resume can be checked sample by sample. */
static void
yield_resumes_at_the_next_sample(dsd_opts* opts, dsd_state* state, int output_kind, const char* name) {
    char label[192];
    reset_stream(opts, state, output_kind);
    g_read_size = 37U;
    uint64_t want = 1U;
    int in_order = 1;
    for (int i = 0; i < 100; i++) {
        in_order &= reads_sample(getSymbol(opts, state, 0), want++);
    }
    g_instead = DSD_RTL_READ_YIELD;
    int raised = 0;
    for (int i = 0; i < 100 && !raised; i++) {
        const float symbol = getSymbol(opts, state, 0);
        raised = state->input_interrupted;
        if (!raised) {
            in_order &= reads_sample(symbol, want++);
        }
    }
    DSD_SNPRINTF(label, sizeof label, "%s: every sample before the yield came out first, in order", name);
    check(label, raised && in_order && want == g_next + 1U);
    for (int i = 0; i < 50; i++) {
        (void)getSymbol(opts, state, 0);
    }
    /* A stream gap met while the yield stands outranks it. */
    dsd_state_raise_input_interrupt(state, DSD_INTERRUPT_STREAM_GAP);
    DSD_SNPRINTF(label, sizeof label, "%s: a stream gap outranks the yield", name);
    check(label, state->input_interrupt_reason == DSD_INTERRUPT_STREAM_GAP);
    g_instead = 1;
    engine_consumes(state);
    for (int i = 0; i < 60; i++) {
        in_order &= reads_sample(getSymbol(opts, state, 0), want++) && state->input_interrupted == 0;
    }
    DSD_SNPRINTF(label, sizeof label, "%s: once consumed, reading resumes at the next sample, none lost", name);
    check(label, in_order);
}

/* sticky_yield_across_reconnect: an rtl_tcp outage gave way to a queued command (the yield stands), and the server
 * comes back between two reads, its reconnect having moved the stream's discontinuity count. The read after it still
 * stands the yield: an unusable 0, nothing read from the stream. Once the engine consumed the yield, the next read
 * raises the stream gap rather than letting it slip by, before any sample after it; once that is consumed, reading
 * resumes at the next sample, none lost. */
static void
sticky_yield_across_reconnect(dsd_opts* opts, dsd_state* state, int output_kind, const char* name) {
    char label[192];
    reset_stream(opts, state, output_kind);
    g_read_size = 37U;
    uint64_t want = 1U;
    int in_order = 1;
    for (int i = 0; i < 100; i++) {
        in_order &= reads_sample(getSymbol(opts, state, 0), want++);
    }
    g_instead = DSD_RTL_READ_YIELD;
    int raised = 0;
    for (int i = 0; i < 100 && !raised; i++) {
        const float symbol = getSymbol(opts, state, 0);
        raised = state->input_interrupted;
        if (!raised) {
            in_order &= reads_sample(symbol, want++);
        }
    }
    DSD_SNPRINTF(label, sizeof label, "%s: the outage's read raises the yield", name);
    check(label, raised && state->input_interrupt_reason == DSD_INTERRUPT_YIELD);

    /* The server is back: samples flow and the count moved. */
    g_instead = 1;
    g_discontinuity = 1U;
    const int reads = g_reads;
    int sticky = 1;
    for (int i = 0; i < 20; i++) {
        state->symbol_replay_symbol_unusable = 0;
        const float symbol = getSymbol(opts, state, 0);
        sticky &= reads_sample(symbol, 0U) && state->symbol_replay_symbol_unusable == 1
                  && state->input_interrupt_reason == DSD_INTERRUPT_YIELD;
    }
    DSD_SNPRINTF(label, sizeof label, "%s: the yield stands across the reconnect, reading nothing", name);
    check(label, sticky && g_reads == reads);

    engine_consumes(state);
    (void)getSymbol(opts, state, 0);
    DSD_SNPRINTF(label, sizeof label, "%s: the next read raises the reconnect's gap", name);
    check(label, state->input_interrupted == 1 && state->input_interrupt_reason == DSD_INTERRUPT_STREAM_GAP);
    engine_consumes(state);
    for (int i = 0; i < 60; i++) {
        in_order &= reads_sample(getSymbol(opts, state, 0), want++) && state->input_interrupted == 0;
    }
    DSD_SNPRINTF(label, sizeof label, "%s: once consumed, reading resumes at the next sample, none lost", name);
    check(label, in_order);
}

/* search_failure_contract_kept: a failed read is still a failure, never a yield: outside a search it asks for
   shutdown; inside a search visit it is the search's interruption, which stands only while the search runs. */
static void
test_search_failure_contract_kept(dsd_opts* opts, dsd_state* state) {
    const int kinds[3] = {
        RTL_STREAM_OUTPUT_SYMBOL_CQPSK,
        RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR,
        RTL_STREAM_OUTPUT_AUDIO_MONITOR,
    };
    for (int k = 0; k < 3; k++) {
        char label[160];
        reset_stream(opts, state, kinds[k]);
        g_search_running = 1;
        for (int i = 0; i < 20; i++) {
            (void)getSymbol(opts, state, 0);
        }
        g_instead = -1;
        for (int i = 0; i < 400 && !state->input_interrupted; i++) {
            (void)getSymbol(opts, state, 0);
        }
        DSD_SNPRINTF(label, sizeof label, "search contract (kind %d): a failed read in a search visit interrupts it",
                     kinds[k]);
        check(label, state->input_interrupted == 1 && state->input_interrupt_reason == DSD_INTERRUPT_SEARCH
                         && g_frame_sync_dsp_stub_shutdown_requests == 0);

        reset_stream(opts, state, kinds[k]);
        for (int i = 0; i < 20; i++) {
            (void)getSymbol(opts, state, 0);
        }
        g_instead = -1;
        for (int i = 0; i < 400 && g_frame_sync_dsp_stub_shutdown_requests == 0; i++) {
            (void)getSymbol(opts, state, 0);
        }
        DSD_SNPRINTF(label, sizeof label, "search contract (kind %d): outside a search a failed read ends the stream",
                     kinds[k]);
        check(label,
              g_frame_sync_dsp_stub_shutdown_requests > 0 && state->input_interrupt_reason == DSD_INTERRUPT_NONE);
    }
}

/* analog_zero_read_contract_kept: an idle NFM monitor's empty read (success, 0 samples) leaves the hunt for the engine
   loop without a reason of its own, and does not stand: the next read reaches the stream again. */
static void
test_analog_zero_read_contract_kept(dsd_opts* opts, dsd_state* state) {
    reset_stream(opts, state, RTL_STREAM_OUTPUT_AUDIO_MONITOR);
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    for (int i = 0; i < 20; i++) {
        (void)getSymbol(opts, state, 0);
    }
    g_instead = 0;
    (void)getSymbol(opts, state, 0);
    check("analog contract: the empty read interrupts the hunt",
          state->input_interrupted == 1 && state->input_interrupt_reason == DSD_INTERRUPT_NONE);
    check("analog contract: the empty read asks for no shutdown", g_frame_sync_dsp_stub_shutdown_requests == 0);
    const int reads = g_reads;
    (void)getSymbol(opts, state, 0);
    check("analog contract: the empty read's interruption does not stand", g_reads == reads + 1);
    engine_consumes(state);
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    assert(opts != NULL);
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state != NULL);
    install_fake_stream();

    yield_across(opts, state, RTL_STREAM_OUTPUT_SYMBOL_CQPSK, "CQPSK symbol cache");
    yield_across(opts, state, RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR, "FSK discriminator cache");
    yield_across(opts, state, RTL_STREAM_OUTPUT_AUDIO_MONITOR, "one-sample reader");
    yield_resumes_at_the_next_sample(opts, state, RTL_STREAM_OUTPUT_SYMBOL_CQPSK, "CQPSK symbol cache");
    yield_resumes_at_the_next_sample(opts, state, RTL_STREAM_OUTPUT_AUDIO_MONITOR, "one-sample reader");
    sticky_yield_across_reconnect(opts, state, RTL_STREAM_OUTPUT_SYMBOL_CQPSK, "CQPSK symbol cache");
    sticky_yield_across_reconnect(opts, state, RTL_STREAM_OUTPUT_AUDIO_MONITOR, "one-sample reader");
    test_search_failure_contract_kept(opts, state);
    test_analog_zero_read_contract_kept(opts, state);

    dsd_signal_search_active_set(NULL);
    dsd_decode_clock_use_system();
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    dsd_state_ext_free_all(state);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d RTL yield check(s) failed\n", g_failures);
        free(state);
        free(opts);
        return 1;
    }
    free(state);
    free(opts);
    return 0;
}
