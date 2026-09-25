// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/shutdown.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"

static uint32_t g_stream_generation = 1;
static int g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
static unsigned int g_output_rate_hz = 48000U;
static int g_symbol_rate_hz = 4800;
static int g_symbol_levels = 4;
static int g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
static float g_read_base = 1000.0f;
static float g_read_base_step = 0.0f;
static int g_read_calls = 0;
static int g_bump_generation_during_read = 0;
static float g_read_base_after_bump = 0.0f;
static int g_output_kind_after_bump = -1;
static int g_symbol_rate_hz_after_bump = 0;
static int g_symbol_levels_after_bump = 0;
static int g_channel_profile_after_bump = -1;
static int g_cleanup_calls = 0;
static int g_fail_reads = 0;
static int g_failed_read_calls = 0;
static int g_max_read_calls = 0;

dsd_socket_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
Connect(char* hostname, int portno) {
    (void)hostname;
    (void)portno;
    return (dsd_socket_t)0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
openAudioInput(dsd_opts* opts) {
    (void)opts;
    return -1;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts) {
    (void)opts;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_request_shutdown(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_cleanup_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_audio_rescale_symbol_timing(dsd_state* state, int old_rate_hz, int new_rate_hz) {
    (void)state;
    (void)old_rate_hz;
    (void)new_rate_hz;
}

double
// NOLINTNEXTLINE(misc-use-internal-linkage)
pwr_to_dB(double mean_power) {
    (void)mean_power;
    return 0.0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
lpf_f(dsd_state* state, float* input, int len) {
    (void)state;
    (void)input;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
hpf_f(dsd_state* state, float* input, int len) {
    (void)state;
    (void)input;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
pbf_f(dsd_state* state, float* input, int len) {
    (void)state;
    (void)input;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
analog_gain_f(const dsd_opts* opts, dsd_state* state, float* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
agsm_f(dsd_opts* opts, dsd_state* state, float* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

/* The analog monitor blocks getSymbol() played (through the UDP analog hook): how many, and the first one's samples. */
static int g_analog_blocks = 0;
static size_t g_first_analog_block_samples = 0;
static short g_first_analog_block_min = 0;
static short g_first_analog_block_max = 0;

static void
reset_analog_block_capture(void) {
    g_analog_blocks = 0;
    g_first_analog_block_samples = 0;
    g_first_analog_block_min = 0;
    g_first_analog_block_max = 0;
}

static void
fake_blast_analog(const dsd_opts* opts, dsd_state* state, size_t nbytes, const void* data) {
    (void)opts;
    (void)state;
    const short* samples = (const short*)data;
    const size_t count = nbytes / sizeof(short);
    if (g_analog_blocks == 0 && count > 0U) {
        g_first_analog_block_samples = count;
        g_first_analog_block_min = samples[0];
        g_first_analog_block_max = samples[0];
        for (size_t i = 1; i < count; i++) {
            if (samples[i] < g_first_analog_block_min) {
                g_first_analog_block_min = samples[i];
            }
            if (samples[i] > g_first_analog_block_max) {
                g_first_analog_block_max = samples[i];
            }
        }
    }
    g_analog_blocks++;
}

static int
fake_rtl_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    assert(rtl_ctx != NULL);
    assert(out != NULL);
    assert(out_got != NULL);
    /* The direct outputs fill the symbol cache (at least four samples); the monitor output is read one sample at a
       time. */
    assert(count >= 1U);
    const int n = count < 4U ? (int)count : 4;

    g_read_calls++;
    if (g_max_read_calls > 0 && g_read_calls > g_max_read_calls) {
        DSD_FPRINTF(stderr, "RTL symbol cache exceeded read limit: calls=%d limit=%d\n", g_read_calls,
                    g_max_read_calls);
        exit(3);
    }
    if (g_fail_reads) {
        g_failed_read_calls++;
        if (g_failed_read_calls > 4) {
            DSD_FPRINTF(stderr, "RTL symbol cache retried failed reads instead of returning EMPTY\n");
            exit(2);
        }
        *out_got = 0;
        return -1;
    }
    float read_base = g_read_base;
    if (g_bump_generation_during_read) {
        g_stream_generation++;
        if (g_output_kind_after_bump >= 0) {
            g_output_kind = g_output_kind_after_bump;
            g_output_kind_after_bump = -1;
        }
        if (g_symbol_rate_hz_after_bump > 0) {
            g_symbol_rate_hz = g_symbol_rate_hz_after_bump;
            g_symbol_rate_hz_after_bump = 0;
        }
        if (g_symbol_levels_after_bump > 0) {
            g_symbol_levels = g_symbol_levels_after_bump;
            g_symbol_levels_after_bump = 0;
        }
        if (g_channel_profile_after_bump >= 0) {
            g_channel_profile = g_channel_profile_after_bump;
            g_channel_profile_after_bump = -1;
        }
        if (g_read_base_after_bump > 0.0f) {
            g_read_base = g_read_base_after_bump;
            read_base = g_read_base;
            g_read_base_after_bump = 0.0f;
        }
        g_bump_generation_during_read = 0;
    }
    for (int i = 0; i < n; i++) {
        out[i] = read_base + (float)i;
    }
    g_read_base += g_read_base_step;
    *out_got = n;
    return 0;
}

static double
fake_rtl_pwr(const void* rtl_ctx) {
    assert(rtl_ctx != NULL);
    return 0.0;
}

static int
fake_output_kind(void) {
    return g_output_kind;
}

static int
fake_symbol_profile(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = g_symbol_rate_hz;
    }
    if (out_levels) {
        *out_levels = g_symbol_levels;
    }
    if (out_channel_profile) {
        *out_channel_profile = g_channel_profile;
    }
    return 0;
}

static uint32_t
fake_stream_generation(void) {
    return g_stream_generation;
}

static unsigned int
fake_output_rate_hz(void) {
    return g_output_rate_hz;
}

static void
reset_stream_fixture(void) {
    g_stream_generation = 1U;
    g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_output_rate_hz = 48000U;
    g_symbol_rate_hz = 4800;
    g_symbol_levels = 4;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
    g_read_base = 1000.0f;
    g_read_base_step = 0.0f;
    g_read_calls = 0;
    g_bump_generation_during_read = 0;
    g_read_base_after_bump = 0.0f;
    g_output_kind_after_bump = -1;
    g_symbol_rate_hz_after_bump = 0;
    g_symbol_levels_after_bump = 0;
    g_channel_profile_after_bump = -1;
    g_cleanup_calls = 0;
    g_fail_reads = 0;
    g_failed_read_calls = 0;
    g_max_read_calls = 0;
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
}

static void
reset_decoder_fixture(dsd_opts* opts, dsd_state* state, void* rtl_context) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    state->rf_mod = 2;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state->rtl_ctx = (struct RtlSdrContext*)rtl_context;
}

/* A decoder that plays its analog monitor block over UDP (-o udp), squelch open, reading at the fixture's 48 kHz: the
   block is 960 samples. */
static void
set_analog_block_output(dsd_opts* opts, int analog_family) {
    opts->analog_only = analog_family;
    opts->monitor_input_audio = 1;
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    opts->rtl_volume_multiplier = 1;
    opts->rtl_squelch_level = -1.0;
}

/* Read until the decoder plays an analog block, at most @p max_symbols symbols. */
static void
read_until_analog_block(dsd_opts* opts, dsd_state* state, int max_symbols) {
    for (int i = 0; i < max_symbols && g_analog_blocks == 0; i++) {
        (void)getSymbol(opts, state, 0);
    }
}

/*
 * A live switch between the analog and digital families reaches the decoder in two steps: the decode-mode change
 * commits the decoder (and drops its part-collected analog block) on the decoder thread, and the front end follows when
 * the demod thread applies the family request at its next block boundary, clearing the output ring and bumping the
 * stream generation. Until then the decoder still reads the old family's output. Neither the samples it reads in
 * between nor a block part-collected before the boundary may reach the first block the new family plays.
 */
static void
test_analog_block_follows_family_switch(dsd_opts* opts, dsd_state* state, void* rtl_context) {
    /* Digital -> analog. A digital session collects its unsynced FSK discriminator samples into the block. */
    reset_stream_fixture();
    reset_decoder_fixture(opts, state, rtl_context);
    reset_analog_block_capture();
    g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    g_read_base = 1000.0f;
    state->rf_mod = 0;
    for (int i = 0; i < 5; i++) {
        (void)getSymbol(opts, state, 0);
    }
    assert(state->analog_sample_counter > 0);

    /* The decoder commits to Analog and drops its block; the front end has not switched yet, so the ring still holds
       well over a block of discriminator output. None of it is collected or played as monitor audio. */
    set_analog_block_output(opts, 1);
    dsd_symbol_analog_block_reset(state);
    for (int i = 0; i < 200; i++) {
        (void)getSymbol(opts, state, 0);
    }
    if (state->analog_sample_counter != 0 || g_analog_blocks != 0) {
        DSD_FPRINTF(stderr, "pending analog switch: collected %d discriminator samples, played %d blocks\n",
                    state->analog_sample_counter, g_analog_blocks);
    }
    assert(state->analog_sample_counter == 0);
    assert(g_analog_blocks == 0);

    /* The switch lands: monitor audio from a new stream generation. The first block is all monitor audio. */
    g_output_kind = RTL_STREAM_OUTPUT_AUDIO_MONITOR;
    g_stream_generation++;
    g_read_base = -2000.0f;
    read_until_analog_block(opts, state, 1000);
    if (g_analog_blocks != 1 || g_first_analog_block_min != -2000 || g_first_analog_block_max != -2000) {
        DSD_FPRINTF(stderr, "first monitor block: blocks=%d samples=%zu min=%d max=%d\n", g_analog_blocks,
                    g_first_analog_block_samples, g_first_analog_block_min, g_first_analog_block_max);
    }
    assert(g_analog_blocks == 1);
    assert(g_first_analog_block_samples == 960U);
    assert(g_first_analog_block_min == -2000);
    assert(g_first_analog_block_max == -2000);
    assert(g_cleanup_calls == 0);

    /* Analog -> digital, with the source monitor (-8) still playing the block. The monitor collects its audio. */
    reset_stream_fixture();
    reset_decoder_fixture(opts, state, rtl_context);
    reset_analog_block_capture();
    set_analog_block_output(opts, 1);
    g_output_kind = RTL_STREAM_OUTPUT_AUDIO_MONITOR;
    g_read_base = -2000.0f;
    state->rf_mod = 0;
    for (int i = 0; i < 5; i++) {
        (void)getSymbol(opts, state, 0);
    }
    assert(state->analog_sample_counter > 0);

    /* The decoder commits to a digital mode and drops its block, then reads monitor audio the front end still delivers
       until the switch lands: the digital decoder's block collects it, as it collects any source it reads. */
    opts->analog_only = 0;
    dsd_symbol_analog_block_reset(state);
    for (int i = 0; i < 5; i++) {
        (void)getSymbol(opts, state, 0);
    }
    assert(state->analog_sample_counter > 0);
    assert(g_analog_blocks == 0);

    /* The switch lands on the FSK discriminator: the monitor audio collected before it is dropped there, and the first
       block holds discriminator samples only. */
    g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    g_stream_generation++;
    g_read_base = 1000.0f;
    read_until_analog_block(opts, state, 1000);
    if (g_analog_blocks != 1 || g_first_analog_block_min < 1000 || g_first_analog_block_max > 1003) {
        DSD_FPRINTF(stderr, "first discriminator block: blocks=%d samples=%zu min=%d max=%d\n", g_analog_blocks,
                    g_first_analog_block_samples, g_first_analog_block_min, g_first_analog_block_max);
    }
    assert(g_analog_blocks == 1);
    assert(g_first_analog_block_samples == 960U);
    assert(g_first_analog_block_min >= 1000);
    assert(g_first_analog_block_max <= 1003);
    assert(g_cleanup_calls == 0);
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rf_mod = 1;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;

    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){
        .read = fake_rtl_read,
        .return_pwr = fake_rtl_pwr,
    });
    dsd_rtl_stream_metrics_hooks metrics_hooks = {
        .output_kind = fake_output_kind,
        .output_rate_hz = fake_output_rate_hz,
        .symbol_profile = fake_symbol_profile,
        .stream_generation = fake_stream_generation,
    };
    dsd_rtl_stream_metrics_hooks_set(&metrics_hooks);

    /*
     * Symbol-path setup covers the direct CQPSK output path first, then the
     * discriminator path that seeds min/max state from the active channel
     * profile.
     */
    reset_stream_fixture();
    reset_decoder_fixture(&opts, &state, &fake_rtl_context);
    g_output_kind = RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
    state.rf_mod = 1;
    assert(getSymbol(&opts, &state, 1) == 1000.0f);
    assert(state.min == -3.0f);
    assert(state.max == 3.0f);
    assert(state.lmid == -2.0f);
    assert(state.umid == 2.0f);

    g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_stream_generation = 2U;
    g_output_rate_hz = 48000U;
    g_symbol_rate_hz = 4800;
    g_symbol_levels = 4;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    g_read_base = 30000.0f;
    g_read_base_step = 4.0f;
    state.rf_mod = 0;

    (void)getSymbol(&opts, &state, 1);
    assert(state.min == -30000.0f);
    assert(state.max == 30000.0f);
    assert(state.lmid == -20000.0f);
    assert(state.umid == 20000.0f);
    assert(state.minref == -24000.0f);
    assert(state.maxref == 24000.0f);
    assert(state.minbuf[0] == -30000.0f);
    assert(state.maxbuf[0] == 30000.0f);
    assert(state.minbuf[1023] == -30000.0f);
    assert(state.maxbuf[1023] == 30000.0f);
    assert(state.minmax_sum_window == 0);

    /*
     * Cached CQPSK symbols should be reused until the generation or channel
     * profile changes. Mid-read generation bumps must refresh cached metadata
     * without losing the samples returned by the read hook.
     */
    reset_stream_fixture();
    reset_decoder_fixture(&opts, &state, &fake_rtl_context);

    g_output_kind = RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
    state.rf_mod = 1;
    assert(getSymbol(&opts, &state, 1) == 1000.0f);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 3);
    assert(getSymbol(&opts, &state, 1) == 1001.0f);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 2);
    assert(g_read_calls == 1);
    assert(state.min == -3.0f);
    assert(state.max == 3.0f);
    assert(state.lmid == -2.0f);
    assert(state.umid == 2.0f);

    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    g_read_base = 1250.0f;

    assert(getSymbol(&opts, &state, 1) == 1250.0f);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 3);
    assert(getSymbol(&opts, &state, 1) == 1251.0f);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 2);
    assert(g_read_calls == 2);

    g_symbol_rate_hz = 6000;
    g_read_base = 1500.0f;

    assert(getSymbol(&opts, &state, 1) == 1500.0f);
    assert(getSymbol(&opts, &state, 1) == 1501.0f);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 2);
    assert(g_read_calls == 3);

    g_stream_generation = 2;
    g_read_base = 2000.0f;

    assert(getSymbol(&opts, &state, 1) == 2000.0f);
    assert(getSymbol(&opts, &state, 1) == 2001.0f);
    assert(g_read_calls == 4);

    assert(getSymbol(&opts, &state, 1) == 2002.0f);
    assert(getSymbol(&opts, &state, 1) == 2003.0f);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 0);
    assert(g_read_calls == 4);

    g_read_base = 3000.0f;
    g_read_base_step = 100.0f;
    g_bump_generation_during_read = 1;
    g_symbol_rate_hz_after_bump = 2400;
    g_symbol_levels_after_bump = 2;
    g_channel_profile_after_bump = RTL_STREAM_CHANNEL_PROFILE_6K25;

    assert(getSymbol(&opts, &state, 1) == 3100.0f);
    assert(g_stream_generation == 3U);
    assert(g_read_calls == 6);
    assert(state.rtl_symbol_cache_generation == 3U);
    assert(state.rtl_symbol_cache_symbol_rate_hz == 2400);
    assert(state.rtl_symbol_cache_channel_profile == RTL_STREAM_CHANNEL_PROFILE_6K25);
    assert(state.rtl_symbol_cache_levels == 2);
    assert(state.min == -1.0f);
    assert(state.max == 1.0f);
    assert(getSymbol(&opts, &state, 1) == 3101.0f);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 2);
    assert(getSymbol(&opts, &state, 1) == 3102.0f);
    assert(getSymbol(&opts, &state, 1) == 3103.0f);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 0);
    assert(g_cleanup_calls == 0);

    /*
     * FSK discriminator tests cover nominal sample-per-symbol choices, fractional
     * accumulation for high-rate modes, jitter adjustment, and output-kind changes
     * that happen while a read is in flight.
     */
    reset_stream_fixture();
    reset_decoder_fixture(&opts, &state, &fake_rtl_context);
    g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_output_rate_hz = 48000U;
    g_symbol_rate_hz = 4800;
    g_symbol_levels = 4;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    g_read_base = 1000.0f;
    g_read_base_step = 4.0f;

    assert(getSymbol(&opts, &state, 1) == 1004.0f);
    assert(state.samplesPerSymbol == 10);
    assert(state.symbolCenter == 4);
    assert(state.jitter == -1);
    assert(g_read_calls == 3);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 2);

    g_stream_generation = 4U;
    g_symbol_rate_hz = 2400;
    g_symbol_levels = 2;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_6K25;
    g_read_base = 2000.0f;
    g_read_base_step = 4.0f;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    float nxdn_symbol = getSymbol(&opts, &state, 1);
    if (fabsf(nxdn_symbol - 2009.7778f) >= 0.01f) {
        DSD_FPRINTF(stderr, "FSK discriminator generation-change symbol %.4f\n", nxdn_symbol);
    }
    assert(fabsf(nxdn_symbol - 2009.7778f) < 0.01f);
    assert(state.samplesPerSymbol == 20);
    assert(state.symbolCenter == dsd_opts_symbol_center(20));
    assert(state.rtl_symbol_cache_generation == 4U);
    assert(state.rtl_symbol_cache_symbol_rate_hz == 2400);
    assert(state.rtl_symbol_cache_channel_profile == RTL_STREAM_CHANNEL_PROFILE_6K25);
    assert(state.rtl_symbol_cache_levels == 2);

    /*
     * The SPS hunt owns discriminator timing, not the front end's published rate.
     * A hunt step only queues its RTL profile request for the demod thread, so the
     * published rate lags it -- and under fast I/Q replay of a fixture that fits in
     * the output ring it never moves at all. Slicing on the lagging rate put timing
     * back on the old profile after every hunt step, which
     * frame_sync_ensure_enabled_sps_profile() then read as "the hunt is on the old
     * profile", cancelling the step and pinning AUTO to 4800/4 (issue #374).
     */
    reset_stream_fixture();
    reset_decoder_fixture(&opts, &state, &fake_rtl_context);
    g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_output_rate_hz = 48000U;
    g_symbol_rate_hz = 4800;
    g_symbol_levels = 4;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    g_read_base = 1000.0f;
    g_read_base_step = 4.0f;

    (void)getSymbol(&opts, &state, 1);
    assert(state.samplesPerSymbol == 10);

    /* Hunt steps to 2400/4; the front end has not caught up (same generation, same
       published 4800/12.5 kHz profile). Timing must follow the hunt regardless. */
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    (void)getSymbol(&opts, &state, 1);
    assert(state.samplesPerSymbol == 20);
    assert(state.symbolCenter == dsd_opts_symbol_center(20));
    assert(g_symbol_rate_hz == 4800);

    /* The front end catching up later changes nothing the hunt did not already ask for. */
    g_stream_generation++;
    g_symbol_rate_hz = 2400;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_6K25;
    (void)getSymbol(&opts, &state, 1);
    assert(state.samplesPerSymbol == 20);

    /* And the reverse: a published 2400 rate must not hold timing at 2400 once the
       hunt has rotated on to a 4800 profile. */
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    (void)getSymbol(&opts, &state, 1);
    assert(state.samplesPerSymbol == 10);
    assert(state.symbolCenter == dsd_opts_symbol_center(10));

    reset_stream_fixture();
    reset_decoder_fixture(&opts, &state, &fake_rtl_context);
    g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_output_rate_hz = 24000U;
    g_symbol_rate_hz = 9600;
    g_symbol_levels = 2;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_PROVOICE;
    g_read_base = 5000.0f;
    g_read_base_step = 4.0f;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_9600_2;

    g_max_read_calls = 2;
    assert(getSymbol(&opts, &state, 0) == 5000.0f);
    assert(state.samplesPerSymbol == 2);
    assert(state.symbolCenter == dsd_opts_symbol_center(2));
    assert(state.jitter == -1);
    assert(g_read_calls == 1);
    g_max_read_calls = 0;

    g_read_base = 5000.0f;
    g_read_base_step = 4.0f;
    g_stream_generation++;
    assert(getSymbol(&opts, &state, 1) == 5000.0f);
    assert(state.samplesPerSymbol == 2);
    assert(state.symbolCenter == dsd_opts_symbol_center(2));
    assert(state.rtl_fsk_sps_accum == 4800);
    assert(getSymbol(&opts, &state, 1) == 5003.0f);
    assert(state.samplesPerSymbol == 3);
    assert(state.symbolCenter == dsd_opts_symbol_center(3));
    assert(state.rtl_fsk_sps_accum == 0);
    assert(getSymbol(&opts, &state, 1) == 5005.0f);
    assert(state.samplesPerSymbol == 2);
    assert(state.rtl_fsk_sps_accum == 4800);

    reset_stream_fixture();
    reset_decoder_fixture(&opts, &state, &fake_rtl_context);
    g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_output_rate_hz = 48000U;
    g_symbol_rate_hz = 4800;
    g_symbol_levels = 4;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    g_read_base = 7000.0f;
    g_read_base_step = 4.0f;

    assert(getSymbol(&opts, &state, 1) == 7004.0f);
    assert(state.samplesPerSymbol == 10);
    assert(state.symbolCenter == dsd_opts_symbol_center(10));
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 2);
    state.jitter = state.symbolCenter;
    float shifted_symbol = getSymbol(&opts, &state, 0);
    if (fabsf(shifted_symbol - 7015.0f) >= 0.01f) {
        DSD_FPRINTF(stderr, "FSK discriminator jitter-adjusted symbol %.4f\n", shifted_symbol);
    }
    assert(fabsf(shifted_symbol - 7015.0f) < 0.01f);
    assert(state.jitter == -1);
    assert(g_read_calls == 6);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 3);

    reset_stream_fixture();
    reset_decoder_fixture(&opts, &state, &fake_rtl_context);
    g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_output_rate_hz = 48000U;
    g_symbol_rate_hz = 4800;
    g_symbol_levels = 4;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    g_read_base = 4000.0f;
    g_read_base_step = 100.0f;
    g_bump_generation_during_read = 1;
    g_output_kind_after_bump = RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
    g_symbol_rate_hz_after_bump = 4800;
    g_symbol_levels_after_bump = 4;
    g_channel_profile_after_bump = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;

    assert(getSymbol(&opts, &state, 1) == 0.0f);
    assert(g_cleanup_calls == 0);
    assert(g_stream_generation == 2U);
    assert(g_output_kind == RTL_STREAM_OUTPUT_SYMBOL_CQPSK);
    state.rf_mod = 1;
    assert(getSymbol(&opts, &state, 1) == 4100.0f);
    assert(g_cleanup_calls == 0);

    /*
     * Every retune and replay RESET bumps the stream generation, which flushes
     * whatever the decoder had cached. A symbol in flight when that happens must
     * restart on the new stream instead of averaging samples from both, so what
     * comes out does not depend on how full the cache happened to be -- the
     * decoder-side half of the sampling nondeterminism in issue #404.
     */
    float restart_symbol = 0.0f;
    for (int prefill = 0; prefill < 3; prefill++) {
        reset_stream_fixture();
        reset_decoder_fixture(&opts, &state, &fake_rtl_context);
        g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
        g_output_rate_hz = 48000U;
        g_symbol_rate_hz = 4800;
        g_symbol_levels = 4;
        g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
        state.rf_mod = 0;
        g_read_base = 4000.0f;
        g_read_base_step = 100.0f;

        for (int s = 0; s < prefill; s++) {
            (void)getSymbol(&opts, &state, 1);
        }

        g_bump_generation_during_read = 1;
        g_read_base_after_bump = 9000.0f;
        float symbol = getSymbol(&opts, &state, 1);
        if (symbol < 9000.0f) {
            DSD_FPRINTF(stderr, "generation bump at prefill %d mixed streams: symbol %.4f\n", prefill, symbol);
        }
        assert(symbol >= 9000.0f);
        if (prefill == 0) {
            restart_symbol = symbol;
        } else if (fabsf(symbol - restart_symbol) >= 0.01f) {
            DSD_FPRINTF(stderr, "generation bump at prefill %d gave %.4f, prefill 0 gave %.4f\n", prefill, symbol,
                        restart_symbol);
            assert(fabsf(symbol - restart_symbol) < 0.01f);
        }
    }

    /*
     * Read failures should surface as the existing empty-symbol path, trigger
     * the cleanup hook once, and leave global hooks reset for later tests.
     */
    reset_stream_fixture();
    reset_decoder_fixture(&opts, &state, &fake_rtl_context);
    g_fail_reads = 1;
    g_failed_read_calls = 0;
    assert(getSymbol(&opts, &state, 1) == 0.0f);
    assert(g_failed_read_calls == 1);
    assert(g_cleanup_calls == 1);

    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast_analog = fake_blast_analog});
    test_analog_block_follows_family_switch(&opts, &state, &fake_rtl_context);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});

    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    return 0;
}
