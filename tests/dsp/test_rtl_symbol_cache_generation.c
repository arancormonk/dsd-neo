// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/analog_audio.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/shutdown.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <math.h>
#include <sndfile.h>
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
/* Every sample a read returns is g_constant_value while this is set, instead of the batch's base plus its index. */
static int g_constant_samples = 0;
static float g_constant_value = 0.0f;
static int g_read_calls = 0;
/* The samples one read returns at most: one demod block's batch. */
static int g_batch_samples = 4;
/* A change lands while the decoder waits in its next read, before the batch it returns: a generation bump (a retune, a
   replay RESET, a CQPSK toggle), or a profile the front end applies without one (the *_after_bump values, which a bump
   applies too). */
static int g_bump_generation_during_read = 0;
static int g_change_profile_during_read = 0;
/* The generation moves after the next read's batch was published: the decoder flushed the stream since. */
static int g_flush_after_publish_during_read = 0;
static float g_read_base_after_bump = 0.0f;
static int g_output_kind_after_bump = -1;
static int g_symbol_rate_hz_after_bump = 0;
static int g_symbol_levels_after_bump = 0;
static int g_channel_profile_after_bump = -1;
static int g_cleanup_calls = 0;
static int g_fail_reads = 0;
static int g_failed_read_calls = 0;
static int g_max_read_calls = 0;
/* The analog receive profile the front end publishes: the analog family, and on it the monitor of a kind, which is
   published only while the output is the monitor (dsd_rtl_stream_metrics_hook_analog_profile()). Both 0 is what a
   table without these hooks reads. */
static int g_analog_family = 0;
static int g_monitor_published = 0;
static int g_monitor_kind = DSD_ANALOG_DEMOD_FM;
/* A paced I/Q replay (1) tags each read's batch with the generation and profile it was published under; a live stream
   (0) tags nothing (fake_replay_batch()). */
static int g_replay = 0;
/* An RTL rate round trip (issue #633): the read with this index (counted from 1) lands a retune to 48 kHz and the next
   one a retune back to 24 kHz, each a new stream generation; 0 lands none. */
static int g_round_trip_at = 0;
static int g_have_last_batch = 0;
static dsd_rtl_stream_replay_batch g_last_batch;

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

/* The RTL output rescale's requests: from which rate to which (issue #634). */
static int g_rescale_calls = 0;
static int g_rescale_from_hz = 0;
static int g_rescale_to_hz = 0;

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_audio_rescale_symbol_timing(dsd_state* state, int old_rate_hz, int new_rate_hz) {
    (void)state;
    g_rescale_calls++;
    g_rescale_from_hz = old_rate_hz;
    g_rescale_to_hz = new_rate_hz;
}

double
// NOLINTNEXTLINE(misc-use-internal-linkage)
pwr_to_dB(double mean_power) {
    (void)mean_power;
    return 0.0;
}

/* The source each block reached the monitor's audio chain with: the -8 source monitor under digital decoding reads
   the FSK discriminator output, the analog monitor its audio. */
static int g_chain_last_source = -1;

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_analog_audio_process_f(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain, float* buf, size_t n,
                           dsd_analog_audio_source source, int rate_hz, unsigned int flags) {
    (void)opts;
    (void)state;
    (void)chain;
    (void)buf;
    (void)n;
    (void)rate_hz;
    (void)flags;
    g_chain_last_source = (int)source;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_analog_audio_block_begin(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain) {
    (void)opts;
    (void)state;
    (void)chain;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_analog_audio_note_reception(const dsd_state* state) {
    (void)state;
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
    g_chain_last_source = -1;
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

/* The change a read lands before its batch (g_bump_generation_during_read, g_change_profile_during_read), if any. */
static void
apply_change_during_read(void) {
    if (!g_bump_generation_during_read && !g_change_profile_during_read) {
        return;
    }
    if (g_bump_generation_during_read) {
        g_stream_generation++;
    }
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
        g_read_base_after_bump = 0.0f;
    }
    g_bump_generation_during_read = 0;
    g_change_profile_during_read = 0;
}

static int
fake_rtl_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    assert(rtl_ctx != NULL);
    assert(out != NULL);
    assert(out_got != NULL);
    /* The direct outputs fill the symbol cache (at least four samples); the monitor output is read one sample at a
       time. */
    assert(count >= 1U);
    const int n = count < (size_t)g_batch_samples ? (int)count : g_batch_samples;

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
    apply_change_during_read();
    if (g_round_trip_at > 0 && (g_read_calls == g_round_trip_at || g_read_calls == g_round_trip_at + 1)) {
        g_output_rate_hz = g_read_calls == g_round_trip_at ? 48000U : 24000U;
        g_stream_generation++;
    }
    const float read_base = g_read_base;
    for (int i = 0; i < n; i++) {
        out[i] = g_constant_samples ? g_constant_value : read_base + (float)i;
    }
    g_read_base += g_read_base_step;
    /* The batch is published after the change, with what the front end then runs. */
    g_last_batch = (dsd_rtl_stream_replay_batch){
        .generation = g_stream_generation,
        .output_kind = g_output_kind,
        .channel_profile = g_channel_profile,
        .symbol_rate_hz = g_symbol_rate_hz,
        .levels = g_symbol_levels,
    };
    g_have_last_batch = 1;
    if (g_flush_after_publish_during_read) {
        g_stream_generation++;
        g_flush_after_publish_during_read = 0;
    }
    *out_got = n;
    return 0;
}

static int
fake_replay_batch(dsd_rtl_stream_replay_batch* out) {
    if (!g_replay || !g_have_last_batch) {
        return 0;
    }
    *out = g_last_batch;
    return 1;
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

/* A constant width and filter, so the received-tone tap sees a boundary only where the published kind or the
   generation moves. */
static int
fake_analog_profile(int* out_kind, int* out_width_hz, int* out_lpf_on) {
    const int published = g_analog_family && g_monitor_published && g_output_kind == RTL_STREAM_OUTPUT_AUDIO_MONITOR;
    if (out_kind) {
        *out_kind = published ? g_monitor_kind : 0;
    }
    if (out_width_hz) {
        *out_width_hz = published ? 12500 : 0;
    }
    if (out_lpf_on) {
        *out_lpf_on = published ? 1 : 0;
    }
    return published;
}

static int
fake_analog_family_active(void) {
    return g_analog_family;
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
    g_constant_samples = 0;
    g_constant_value = 0.0f;
    g_read_calls = 0;
    g_batch_samples = 4;
    g_bump_generation_during_read = 0;
    g_change_profile_during_read = 0;
    g_flush_after_publish_during_read = 0;
    g_read_base_after_bump = 0.0f;
    g_output_kind_after_bump = -1;
    g_symbol_rate_hz_after_bump = 0;
    g_symbol_levels_after_bump = 0;
    g_channel_profile_after_bump = -1;
    g_cleanup_calls = 0;
    g_fail_reads = 0;
    g_failed_read_calls = 0;
    g_max_read_calls = 0;
    g_analog_family = 0;
    g_monitor_published = 0;
    g_monitor_kind = DSD_ANALOG_DEMOD_FM;
    g_replay = 0;
    g_round_trip_at = 0;
    g_have_last_batch = 0;
    g_last_batch = (dsd_rtl_stream_replay_batch){0};
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
}

static void
reset_decoder_fixture(dsd_opts* opts, dsd_state* state, void* rtl_context) {
    /* The analog monitor starts received-tone detection, which keeps its state in a state extension (issue #522):
       released before the state is wiped, so no case leaks what the previous one left attached. */
    dsd_state_ext_free_all(state);
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
    /* Its gain stage takes it as monitor audio (issue #518). */
    assert(g_chain_last_source == DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR);
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
    /* ... and the gain stage takes it as FSK discriminator output (+/-30000), not monitor audio: scaled as monitor
       audio, the -8 source monitor clipped every sample (issue #518). */
    assert(g_chain_last_source == DSD_ANALOG_AUDIO_SOURCE_RTL_FSK);
    assert(g_cleanup_calls == 0);
}

/* Read @p count more samples of the front end's output. The monitor output is read one sample per read call. */
static void
read_monitor_samples(dsd_opts* opts, dsd_state* state, int count) {
    const int target = g_read_calls + count;
    while (g_read_calls < target) {
        (void)getSymbol(opts, state, 0);
    }
}

/* The first block the decoder played after the landing, read far enough that one past the landing's boundary (which
   the received-tone tap mutes as the old profile's) completes: it holds only @p value, the new monitor's audio. */
static void
assert_first_block_after_landing(dsd_opts* opts, dsd_state* state, const char* label, short value) {
    read_until_analog_block(opts, state, 3000);
    if (g_analog_blocks != 1 || g_first_analog_block_samples != 960U || g_first_analog_block_min != value
        || g_first_analog_block_max != value) {
        DSD_FPRINTF(stderr, "%s: first block after the landing: blocks=%d samples=%zu min=%d max=%d, want all %d\n",
                    label, g_analog_blocks, g_first_analog_block_samples, g_first_analog_block_min,
                    g_first_analog_block_max, value);
    }
    assert(g_analog_blocks == 1);
    assert(g_first_analog_block_samples == 960U);
    assert(g_first_analog_block_min == value);
    assert(g_first_analog_block_max == value);
    assert(g_cleanup_calls == 0);
}

/* The audio each analog kind's detector delivers in these cases. */
static float
monitor_value_for_kind(int kind) {
    return kind == DSD_ANALOG_DEMOD_AM ? 3000.0f : -2000.0f;
}

/*
 * A live switch between the FM and AM monitors (issue #582) reaches the decoder as a family switch does: the decoder
 * commits the new kind at once, and the front end follows at the demod thread's next block boundary, clearing the
 * output ring, moving the stream generation and publishing the new kind before its first sample. Both kinds are the
 * monitor output, so nothing in the output kind tells the decoder the samples it reads in between are the old
 * detector's: the published kind does. A backlog of a whole block is ordinary (synchronous playback holds the decoder
 * for a block's playing time), and a whole block of the old detector's audio holds no boundary for the received-tone
 * tap to mute it by. None of it may be played as the new kind's, whether or not the change emptied the block
 * (@p drop_block: DECODE_MODE_SET does, dsd_symbol_analog_block_reset()).
 */
static void
run_kind_switch(dsd_opts* opts, dsd_state* state, void* rtl_context, int from_kind, int to_kind, int in_flight,
                int drop_block) {
    char label[96];
    DSD_SNPRINTF(label, sizeof label, "%s -> %s, %d in flight%s", from_kind == DSD_ANALOG_DEMOD_AM ? "AM" : "FM",
                 to_kind == DSD_ANALOG_DEMOD_AM ? "AM" : "FM", in_flight, drop_block ? "" : ", block kept");
    reset_stream_fixture();
    reset_decoder_fixture(opts, state, rtl_context);
    reset_analog_block_capture();
    set_analog_block_output(opts, 1);
    state->rf_mod = 0;
    opts->analog_demod = from_kind;
    g_output_kind = RTL_STREAM_OUTPUT_AUDIO_MONITOR;
    g_analog_family = 1;
    g_monitor_published = 1;
    g_monitor_kind = from_kind;
    g_read_base = monitor_value_for_kind(from_kind);

    /* The monitor plays its kind's audio, then part-collects a block. */
    read_until_analog_block(opts, state, 3000);
    assert(g_analog_blocks == 1);
    reset_analog_block_capture();
    read_monitor_samples(opts, state, 300);
    assert(state->analog_sample_counter > 0);

    /* The decoder commits the new kind; the front end still delivers the old detector's audio. */
    opts->analog_demod = to_kind;
    if (drop_block) {
        dsd_symbol_analog_block_reset(state);
    }
    dsd_analog_rx_reset(state);
    read_monitor_samples(opts, state, in_flight);
    if (state->analog_sample_counter != 0 || g_analog_blocks != 0) {
        DSD_FPRINTF(
            stderr, "%s: before the landing: collected %d old-kind samples, played %d blocks (first min=%d max=%d)\n",
            label, state->analog_sample_counter, g_analog_blocks, g_first_analog_block_min, g_first_analog_block_max);
    }
    assert(g_analog_blocks == 0);
    assert(state->analog_sample_counter == 0);

    /* The switch lands: the ring is cleared, the generation moves (twice) and the new kind is published. */
    g_stream_generation += 2U;
    g_monitor_kind = to_kind;
    g_read_base = monitor_value_for_kind(to_kind);
    assert_first_block_after_landing(opts, state, label, (short)monitor_value_for_kind(to_kind));
}

static void
test_analog_block_follows_kind_switch(dsd_opts* opts, dsd_state* state, void* rtl_context) {
    static const int kinds[2][2] = {
        {DSD_ANALOG_DEMOD_FM, DSD_ANALOG_DEMOD_AM},
        {DSD_ANALOG_DEMOD_AM, DSD_ANALOG_DEMOD_FM},
    };
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        run_kind_switch(opts, state, rtl_context, kinds[k][0], kinds[k][1], 1440, 1);
        run_kind_switch(opts, state, rtl_context, kinds[k][0], kinds[k][1], 480, 1);
        run_kind_switch(opts, state, rtl_context, kinds[k][0], kinds[k][1], 1440, 0);
    }
}

/*
 * Leaving a -Y scan that sits on a typed digital row, back to the configured Analog or AM decoder, is the same gap by
 * another route: the row keeps the front end on the analog family and the monitor output with the row's channel, where
 * the stream publishes no monitor, and the leave is no retune. The decoder is the configured analog one at once; the
 * front end puts its monitor back at the demod thread's next block boundary. The leave does not empty the block (the
 * front end's family did not change) and resets the received-tone tap (dsd_frame_sync_reset_acquisition()). None of
 * the row's audio may be played as the monitor's.
 */
static void
run_typed_row_leave(dsd_opts* opts, dsd_state* state, void* rtl_context, int kind) {
    const char* label = kind == DSD_ANALOG_DEMOD_AM ? "typed row leave to AM" : "typed row leave to Analog";
    reset_stream_fixture();
    reset_decoder_fixture(opts, state, rtl_context);
    reset_analog_block_capture();
    set_analog_block_output(opts, 0);
    state->rf_mod = 0;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    g_output_kind = RTL_STREAM_OUTPUT_AUDIO_MONITOR;
    g_analog_family = 1;
    g_monitor_published = 0;
    g_read_base = 1500.0f;

    /* The row's digital decoder collects the discriminator audio of its channel, as a digital session collects any
       source it reads. */
    read_monitor_samples(opts, state, 300);
    assert(state->analog_sample_counter > 0);

    /* The leave: the configured decoder is back, the front end is still on the row's channel. Two and a half blocks
       of it arrive before the monitor request lands. */
    opts->analog_only = 1;
    opts->analog_demod = kind;
    dsd_analog_rx_reset(state);
    read_monitor_samples(opts, state, 2400);
    if (state->analog_sample_counter != 0 || g_analog_blocks != 0) {
        DSD_FPRINTF(stderr,
                    "%s: before the landing: collected %d row samples, played %d blocks (first min=%d max=%d)\n", label,
                    state->analog_sample_counter, g_analog_blocks, g_first_analog_block_min, g_first_analog_block_max);
    }
    assert(g_analog_blocks == 0);
    assert(state->analog_sample_counter == 0);

    /* The monitor request lands: the ring is cleared, the generation moves and the monitor is published. */
    g_stream_generation += 2U;
    g_monitor_published = 1;
    g_monitor_kind = kind;
    g_read_base = monitor_value_for_kind(kind);
    assert_first_block_after_landing(opts, state, label, (short)monitor_value_for_kind(kind));
}

static void
test_analog_block_follows_typed_row_leave(dsd_opts* opts, dsd_state* state, void* rtl_context) {
    run_typed_row_leave(opts, state, rtl_context, DSD_ANALOG_DEMOD_AM);
    run_typed_row_leave(opts, state, rtl_context, DSD_ANALOG_DEMOD_FM);
}

/* Only an analog-family decoder follows the monitor it is configured for. A digital decoder on the monitor output
   (a typed row's decoder, or one a switch from AM has put back to FM while the monitor still runs AM) collects what
   it reads, as the -8 source monitor always has. */
static void
test_digital_decoder_collects_any_monitor(dsd_opts* opts, dsd_state* state, void* rtl_context) {
    for (int published = 0; published <= 1; published++) {
        reset_stream_fixture();
        reset_decoder_fixture(opts, state, rtl_context);
        reset_analog_block_capture();
        set_analog_block_output(opts, 0);
        state->rf_mod = 0;
        opts->analog_demod = DSD_ANALOG_DEMOD_FM;
        g_output_kind = RTL_STREAM_OUTPUT_AUDIO_MONITOR;
        g_analog_family = 1;
        g_monitor_published = published;
        g_monitor_kind = DSD_ANALOG_DEMOD_AM;
        g_read_base = 1500.0f;
        read_monitor_samples(opts, state, 300);
        if (state->analog_sample_counter <= 0) {
            DSD_FPRINTF(stderr, "digital decoder on the monitor (published %d) collected nothing\n", published);
        }
        assert(state->analog_sample_counter > 0);
    }
}

/*
 * Under a paced I/Q replay (issue #572) the decoder paces the demod, so a RESET, a profile the SPS hunt asked for and a
 * CQPSK toggle all land while the decoder waits in its read, and the first batch after each carries labels the decoder
 * has not seen yet. The replay tags that batch with what it ran on; the decoder keeps all of it and consumes it with
 * the new profile. A live read carries no labels, so there the conservative drop stays: a batch read across a
 * generation bump is dropped whole, and one read across a profile change is dropped from the next refresh on.
 */
static int g_failures = 0;

static void
expect_int(const char* label, const char* what, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: %s: got %ld, want %ld\n", label, what, got, want);
        g_failures++;
    }
}

static void
expect_near(const char* label, const char* what, float got, float want) {
    if (fabsf(got - want) >= 0.01f) {
        DSD_FPRINTF(stderr, "FAIL: %s: %s: got %.4f, want %.4f\n", label, what, got, want);
        g_failures++;
    }
}

/* A symbol made only of the samples [@p base, @p base + @p span): a batch's samples are its base plus their index. */
static void
expect_from_batch(const char* label, const char* what, float symbol, float base, int span) {
    if (symbol < base || symbol >= base + (float)span) {
        DSD_FPRINTF(stderr, "FAIL: %s: %s: symbol %.4f is not from samples [%.0f, %.0f)\n", label, what, symbol, base,
                    base + (float)span);
        g_failures++;
    }
}

/*
 * Issue #634: an input switch back to the radio tells the RTL output rescale which rate the symbol timing is in. Here the
 * monitor runs at 48 kHz, where the rescale already is; a 96 kHz PCM input moved the timing to its own units; the
 * return to the radio notes 96 kHz (dsd_symbol_note_timing_rate()), so the first symbol on the 48 kHz monitor rescales
 * the timing from 96 kHz to 48 kHz. A rescale that still believed the timing was in 48 kHz units would leave it at
 * twice the monitor's samples a symbol.
 */
static void
test_return_to_radio_rescales_from_the_pcm_rate(dsd_opts* opts, dsd_state* state, void* rtl_context) {
    reset_stream_fixture();
    reset_decoder_fixture(opts, state, rtl_context);
    g_output_kind = RTL_STREAM_OUTPUT_AUDIO_MONITOR;
    g_output_rate_hz = 48000U;
    state->rf_mod = 0;
    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    dsd_symbol_note_timing_rate(48000);
    g_rescale_calls = 0;
    (void)getSymbol(opts, state, 0);
    expect_int("radio return", "no rescale at the rate the timing is in", g_rescale_calls, 0);

    /* The 96 kHz PCM input, and the return to the radio. */
    dsd_symbol_note_timing_rate(96000);
    (void)getSymbol(opts, state, 0);
    expect_int("radio return", "one rescale", g_rescale_calls, 1);
    expect_int("radio return", "from the pcm input's rate", g_rescale_from_hz, 96000);
    expect_int("radio return", "to the monitor's", g_rescale_to_hz, 48000);
    (void)getSymbol(opts, state, 0);
    expect_int("radio return", "and only once", g_rescale_calls, 1);
}

static const char*
mode_label(int replay) {
    return replay ? "paced replay" : "live";
}

static void
start_cqpsk_stream(dsd_opts* opts, dsd_state* state, void* rtl_context, int replay, int batch_samples) {
    reset_stream_fixture();
    reset_decoder_fixture(opts, state, rtl_context);
    g_replay = replay;
    g_batch_samples = batch_samples;
    g_output_kind = RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    state->rf_mod = 1;
}

static void
start_fsk_stream(dsd_opts* opts, dsd_state* state, void* rtl_context, int replay, int batch_samples) {
    reset_stream_fixture();
    reset_decoder_fixture(opts, state, rtl_context);
    g_replay = replay;
    g_batch_samples = batch_samples;
    g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_output_rate_hz = 48000U;
    g_symbol_rate_hz = 4800;
    g_symbol_levels = 4;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    state->rf_mod = 0;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
}

/* CQPSK, one sample a symbol, four-sample batches from 1000: the first batch is consumed, then a change lands in the
   next read, whose batch starts at 3000 (the next at 3100). */
static void
cqpsk_consume_first_batch(dsd_opts* opts, dsd_state* state) {
    g_read_base = 1000.0f;
    for (int i = 0; i < 4; i++) {
        (void)getSymbol(opts, state, 1);
    }
    assert(g_read_calls == 1);
    g_read_base = 3000.0f;
    g_read_base_step = 100.0f;
}

/* A generation bump with a new rate, levels and channel profile, and a bump alone (a RESET). */
static void
run_cqpsk_bump(dsd_opts* opts, dsd_state* state, void* rtl_context, int replay, int profile_change) {
    char label[96];
    DSD_SNPRINTF(label, sizeof label, "%s, CQPSK, generation bump%s", mode_label(replay),
                 profile_change ? " with a rate, levels and profile change" : " alone");
    start_cqpsk_stream(opts, state, rtl_context, replay, 4);
    cqpsk_consume_first_batch(opts, state);
    g_bump_generation_during_read = 1;
    if (profile_change) {
        g_symbol_rate_hz_after_bump = 2400;
        g_symbol_levels_after_bump = 2;
        g_channel_profile_after_bump = RTL_STREAM_CHANNEL_PROFILE_6K25;
    }
    float first = getSymbol(opts, state, 1);
    if (!replay) {
        expect_near(label, "first symbol after the change (the batch read across it is dropped)", first, 3100.0f);
        expect_int(label, "reads", g_read_calls, 3);
        return;
    }
    expect_near(label, "first symbol after the change", first, 3000.0f);
    expect_int(label, "reads", g_read_calls, 2);
    expect_int(label, "cache generation", (long)state->rtl_symbol_cache_generation, 2);
    expect_int(label, "cache symbol rate", state->rtl_symbol_cache_symbol_rate_hz, profile_change ? 2400 : 4800);
    expect_int(label, "cache levels", state->rtl_symbol_cache_levels, profile_change ? 2 : 4);
    expect_int(label, "cache channel profile", state->rtl_symbol_cache_channel_profile,
               profile_change ? RTL_STREAM_CHANNEL_PROFILE_6K25 : RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    /* The first sample is sliced on the new levels' thresholds. */
    expect_near(label, "slicer minimum", state->min, profile_change ? -1.0f : -3.0f);
    expect_int(label, "cached samples pending", dsd_rtl_stream_metrics_hook_symbol_cache_pending(), 3);
    for (int i = 1; i < 4; i++) {
        expect_near(label, "rest of the first batch", getSymbol(opts, state, 1), 3000.0f + (float)i);
    }
    expect_int(label, "reads after the whole first batch", g_read_calls, 2);
    expect_near(label, "next batch", getSymbol(opts, state, 1), 3100.0f);
    expect_int(label, "cleanup calls", g_cleanup_calls, 0);
}

/* The front end applies new levels without moving the generation (a profile the decoder asked for). */
static void
run_cqpsk_levels_change(dsd_opts* opts, dsd_state* state, void* rtl_context, int replay) {
    char label[96];
    DSD_SNPRINTF(label, sizeof label, "%s, CQPSK, levels change without a generation bump", mode_label(replay));
    start_cqpsk_stream(opts, state, rtl_context, replay, 4);
    cqpsk_consume_first_batch(opts, state);
    g_change_profile_during_read = 1;
    g_symbol_levels_after_bump = 2;
    float first = getSymbol(opts, state, 1);
    expect_near(label, "first symbol after the change", first, 3000.0f);
    /* Live, the first sample goes out on the old levels, and the refresh before the next drops the rest. */
    expect_near(label, "slicer minimum on the first sample", state->min, replay ? -1.0f : -3.0f);
    expect_near(label, "second symbol after the change", getSymbol(opts, state, 1), replay ? 3001.0f : 3100.0f);
    expect_int(label, "reads", g_read_calls, replay ? 2 : 3);
    expect_int(label, "cache levels", state->rtl_symbol_cache_levels, 2);
    expect_near(label, "slicer minimum", state->min, -1.0f);
}

/* FSK at 48 kHz and 4800 symbols/s (10 samples a symbol), batches of @p batch_samples from 1000, each 1000 above the
   last: the first four symbols are read, then a change lands in the next read, whose batch starts at 9000. */
static void
fsk_consume_four_symbols(dsd_opts* opts, dsd_state* state, const char* label) {
    g_read_base = 1000.0f;
    g_read_base_step = 1000.0f;
    for (int i = 0; i < 4; i++) {
        expect_from_batch(label, "symbol before the change", getSymbol(opts, state, 1), 1000.0f, g_batch_samples);
    }
    assert(g_read_calls == 1);
    assert(state->samplesPerSymbol == 10);
    g_read_base_after_bump = 9000.0f;
}

/* A generation bump alone (a RESET) with no profile change, landing at a symbol boundary (40-sample batches, four
   symbols each) or part-way through one (45-sample batches: the fifth symbol has taken five samples when it lands). */
static void
run_fsk_bump(dsd_opts* opts, dsd_state* state, void* rtl_context, int replay, int batch_samples) {
    char label[96];
    DSD_SNPRINTF(label, sizeof label, "%s, FSK, generation bump alone, %d-sample batches", mode_label(replay),
                 batch_samples);
    start_fsk_stream(opts, state, rtl_context, replay, batch_samples);
    fsk_consume_four_symbols(opts, state, label);
    /* A slicer the decoder has moved since, which the change resets. */
    state->min = -1.0f;
    g_bump_generation_during_read = 1;
    float after[4];
    for (int i = 0; i < 4; i++) {
        after[i] = getSymbol(opts, state, 1);
    }
    const int reads_after_four = g_read_calls;
    expect_near(label, "slicer reset", state->min, -30000.0f);
    expect_int(label, "samples per symbol", state->samplesPerSymbol, 10);
    if (!replay) {
        expect_from_batch(label, "first symbol after the change (the batch read across it is dropped)", after[0],
                          10000.0f, 10);
        expect_int(label, "reads after four symbols", reads_after_four, 3);
        return;
    }
    /* The new stream's symbols start on its first sample and take 40 samples of its first batch: one read. */
    expect_from_batch(label, "first symbol after the change", after[0], 9000.0f, 10);
    for (int i = 1; i < 4; i++) {
        expect_from_batch(label, "symbols after the change", after[i], 9000.0f + (float)(10 * i), 10);
    }
    expect_int(label, "reads after four symbols", reads_after_four, 2);
    expect_int(label, "cache generation", (long)state->rtl_symbol_cache_generation, 2);
    expect_int(label, "cleanup calls", g_cleanup_calls, 0);
}

/* The SPS hunt steps to 2400 symbols/s and the front end follows at its next block without a generation bump: a new
   rate and channel profile on the batch the next read returns (20 samples a symbol, two symbols a batch). */
static void
run_fsk_hunt_rate_change(dsd_opts* opts, dsd_state* state, void* rtl_context, int replay) {
    char label[96];
    DSD_SNPRINTF(label, sizeof label, "%s, FSK, hunt rate change without a generation bump", mode_label(replay));
    start_fsk_stream(opts, state, rtl_context, replay, 40);
    fsk_consume_four_symbols(opts, state, label);
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    g_change_profile_during_read = 1;
    g_symbol_rate_hz_after_bump = 2400;
    g_channel_profile_after_bump = RTL_STREAM_CHANNEL_PROFILE_6K25;
    float first = getSymbol(opts, state, 1);
    float second = getSymbol(opts, state, 1);
    expect_int(label, "samples per symbol", state->samplesPerSymbol, 20);
    expect_from_batch(label, "first symbol after the change", first, 9000.0f, 20);
    expect_int(label, "cache symbol rate", state->rtl_symbol_cache_symbol_rate_hz, 2400);
    expect_int(label, "cache channel profile", state->rtl_symbol_cache_channel_profile,
               RTL_STREAM_CHANNEL_PROFILE_6K25);
    if (!replay) {
        /* The refresh before the second symbol finds the old profile's labels on the batch and drops the rest. */
        expect_from_batch(label, "second symbol after the change (the rest of the batch is dropped)", second, 10000.0f,
                          20);
        expect_int(label, "reads", g_read_calls, 3);
        return;
    }
    expect_from_batch(label, "second symbol after the change", second, 9020.0f, 20);
    expect_int(label, "reads", g_read_calls, 2);
}

/* FSK -> CQPSK: the front end toggles CQPSK (a generation bump and a new output kind) while the FSK path waits in its
   read. The symbol in flight ends there, as it does after a dropped batch, and the new output's first sample opens the
   next one. */
static void
run_fsk_to_cqpsk(dsd_opts* opts, dsd_state* state, void* rtl_context, int replay) {
    char label[96];
    DSD_SNPRINTF(label, sizeof label, "%s, FSK to CQPSK", mode_label(replay));
    start_fsk_stream(opts, state, rtl_context, replay, 4);
    g_read_base = 4000.0f;
    g_read_base_step = 100.0f;
    g_bump_generation_during_read = 1;
    g_output_kind_after_bump = RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
    g_symbol_rate_hz_after_bump = 4800;
    g_symbol_levels_after_bump = 4;
    g_channel_profile_after_bump = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    expect_near(label, "the FSK symbol in flight", getSymbol(opts, state, 1), 0.0f);
    expect_int(label, "output kind", g_output_kind, RTL_STREAM_OUTPUT_SYMBOL_CQPSK);
    state->rf_mod = 1;
    float first = getSymbol(opts, state, 1);
    if (!replay) {
        expect_near(label, "first CQPSK symbol (the batch read across the toggle is dropped)", first, 4100.0f);
        expect_int(label, "reads", g_read_calls, 2);
        return;
    }
    expect_near(label, "first CQPSK symbol", first, 4000.0f);
    for (int i = 1; i < 4; i++) {
        expect_near(label, "rest of the first CQPSK batch", getSymbol(opts, state, 1), 4000.0f + (float)i);
    }
    expect_int(label, "reads", g_read_calls, 1);
    expect_int(label, "cleanup calls", g_cleanup_calls, 0);
}

/* CQPSK -> FSK: the toggle lands while the CQPSK path waits in its read (40-sample batches). */
static void
run_cqpsk_to_fsk(dsd_opts* opts, dsd_state* state, void* rtl_context, int replay) {
    char label[96];
    DSD_SNPRINTF(label, sizeof label, "%s, CQPSK to FSK", mode_label(replay));
    start_cqpsk_stream(opts, state, rtl_context, replay, 40);
    g_read_base = 1000.0f;
    g_read_base_step = 1000.0f;
    for (int i = 0; i < 40; i++) {
        (void)getSymbol(opts, state, 1);
    }
    assert(g_read_calls == 1);
    state->rf_mod = 0;
    g_bump_generation_during_read = 1;
    g_output_kind_after_bump = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_symbol_rate_hz_after_bump = 4800;
    g_symbol_levels_after_bump = 4;
    g_channel_profile_after_bump = RTL_STREAM_CHANNEL_PROFILE_12K5;
    g_read_base_after_bump = 9000.0f;
    float after[4];
    for (int i = 0; i < 4; i++) {
        after[i] = getSymbol(opts, state, 1);
    }
    expect_int(label, "samples per symbol", state->samplesPerSymbol, 10);
    if (!replay) {
        expect_from_batch(label, "first FSK symbol (the batch read across the toggle is dropped)", after[0], 10000.0f,
                          10);
        expect_int(label, "reads after four symbols", g_read_calls, 3);
        return;
    }
    for (int i = 0; i < 4; i++) {
        expect_from_batch(label, "FSK symbols of the first batch", after[i], 9000.0f + (float)(10 * i), 10);
    }
    expect_int(label, "reads after four symbols", g_read_calls, 2);
    expect_int(label, "cleanup calls", g_cleanup_calls, 0);
}

/* The stream's first read already runs on a new generation (the demod applied a queued profile before its first
   block). */
static void
run_first_batch_of_stream(dsd_opts* opts, dsd_state* state, void* rtl_context, int replay) {
    char label[96];
    DSD_SNPRINTF(label, sizeof label, "%s, first batch of the stream", mode_label(replay));
    start_cqpsk_stream(opts, state, rtl_context, replay, 4);
    g_read_base = 1000.0f;
    g_read_base_step = 100.0f;
    g_bump_generation_during_read = 1;
    expect_near(label, "first symbol", getSymbol(opts, state, 1), replay ? 1000.0f : 1100.0f);
    expect_int(label, "reads", g_read_calls, replay ? 1 : 2);
}

/* A batch the stream moved on from after it was published (a flush the decoder asked for) is dropped in a replay too:
   the pop checks the batch's generation, not the stream's. */
static void
run_flush_after_publish(dsd_opts* opts, dsd_state* state, void* rtl_context, int replay) {
    char label[96];
    DSD_SNPRINTF(label, sizeof label, "%s, flush after the batch was published", mode_label(replay));
    start_cqpsk_stream(opts, state, rtl_context, replay, 4);
    g_read_base = 3000.0f;
    g_read_base_step = 100.0f;
    g_flush_after_publish_during_read = 1;
    expect_near(label, "first symbol", getSymbol(opts, state, 1), 3100.0f);
    expect_int(label, "reads", g_read_calls, 2);
}

/* A direct output's switch to the monitor output: the monitor's batch is dropped, as a live read's is (that output is
   read a sample at a time outside the cache), and the move off the direct output still drops the part-collected
   analog block. */
static void
run_direct_to_monitor(dsd_opts* opts, dsd_state* state, void* rtl_context, int replay) {
    char label[96];
    DSD_SNPRINTF(label, sizeof label, "%s, FSK to the monitor output", mode_label(replay));
    start_fsk_stream(opts, state, rtl_context, replay, 40);
    g_read_base = 1000.0f;
    g_read_base_step = 1000.0f;
    for (int i = 0; i < 4; i++) {
        (void)getSymbol(opts, state, 0);
    }
    assert(state->analog_sample_counter > 0);
    g_bump_generation_during_read = 1;
    g_output_kind_after_bump = RTL_STREAM_OUTPUT_AUDIO_MONITOR;
    g_read_base_after_bump = 9000.0f;
    expect_near(label, "the FSK symbol in flight", getSymbol(opts, state, 0), 0.0f);
    expect_int(label, "part-collected analog samples", state->analog_sample_counter, 0);
    expect_int(label, "cached samples pending", dsd_rtl_stream_metrics_hook_symbol_cache_pending(), 0);
    expect_int(label, "cache output kind", state->rtl_symbol_cache_output_kind, 0);
    expect_int(label, "cleanup calls", g_cleanup_calls, 0);
}

/* End of input: a failed read ends the stream in a replay as it does live. */
static void
run_read_failure(dsd_opts* opts, dsd_state* state, void* rtl_context, int replay) {
    char label[96];
    DSD_SNPRINTF(label, sizeof label, "%s, failed read", mode_label(replay));
    start_cqpsk_stream(opts, state, rtl_context, replay, 4);
    g_fail_reads = 1;
    expect_near(label, "symbol", getSymbol(opts, state, 1), 0.0f);
    expect_int(label, "failed reads", g_failed_read_calls, 1);
    expect_int(label, "cleanup calls", g_cleanup_calls, 1);
}

static void
test_replay_keeps_first_batch_after_change(dsd_opts* opts, dsd_state* state, void* rtl_context) {
    for (int replay = 0; replay <= 1; replay++) {
        run_cqpsk_bump(opts, state, rtl_context, replay, 1);
        run_cqpsk_bump(opts, state, rtl_context, replay, 0);
        run_cqpsk_levels_change(opts, state, rtl_context, replay);
        run_fsk_bump(opts, state, rtl_context, replay, 40);
        run_fsk_bump(opts, state, rtl_context, replay, 45);
        run_fsk_hunt_rate_change(opts, state, rtl_context, replay);
        run_fsk_to_cqpsk(opts, state, rtl_context, replay);
        run_cqpsk_to_fsk(opts, state, rtl_context, replay);
        run_first_batch_of_stream(opts, state, rtl_context, replay);
        run_flush_after_publish(opts, state, rtl_context, replay);
        run_direct_to_monitor(opts, state, rtl_context, replay);
        run_read_failure(opts, state, rtl_context, replay);
    }
    reset_stream_fixture();
    reset_decoder_fixture(opts, state, rtl_context);
}

/* Read until the stream has handed the decoder @p reads samples. */
static void
read_until_reads(dsd_opts* opts, dsd_state* state, int reads) {
    for (int i = 0; i < 100000 && g_read_calls < reads; i++) {
        (void)getSymbol(opts, state, 0);
    }
}

/* A -6 raw WAV at the analog sink rate (48 kHz mono) in a private file made from the dsd_mkstemp() template @p path. */
static SNDFILE*
open_raw_wav(char* path) {
    const int fd = dsd_mkstemp(path);
    assert(fd >= 0);
    (void)dsd_close(fd);
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.samplerate = 48000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* wav = sf_open(path, SFM_WRITE, &info);
    assert(wav != NULL);
    return wav;
}

/*
 * Issue #633: an RTL rate round trip (24, 48, 24 kHz with the demod resampler off) inside the samples one getSymbol()
 * call reads, each step a new stream generation. The block they land in finishes at the rate it started at, but one of
 * its samples ran at 48 kHz: the -6 raw WAV, which records every other block, leaves it out. The generation is read
 * with each sample, not once a symbol, or the round trip would pass unseen.
 */
static void
test_rtl_rate_round_trip_inside_one_symbol(dsd_opts* opts, dsd_state* state, void* rtl_context) {
    reset_stream_fixture();
    reset_decoder_fixture(opts, state, rtl_context);
    g_output_kind = RTL_STREAM_OUTPUT_AUDIO_MONITOR;
    g_output_rate_hz = 24000U;
    g_analog_family = 1;
    g_monitor_published = 1;
    g_batch_samples = 1;
    set_analog_block_output(opts, 1);
    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    char raw_path[] = "dsdneo_rtl_round_trip_XXXXXX";
    opts->wav_out_raw = open_raw_wav(raw_path);

    /* Two 20 ms blocks (480 samples at 24 kHz), then the round trip three samples into the next getSymbol() call. */
    read_until_reads(opts, state, 960);
    g_round_trip_at = g_read_calls + 3;
    (void)getSymbol(opts, state, 0);
    expect_int("rtl-633", "round trip inside one call", g_read_calls >= g_round_trip_at + 1, 1);
    read_until_reads(opts, state, 6 * 480);
    const int blocks = g_read_calls / 480;
    sf_close(opts->wav_out_raw);
    opts->wav_out_raw = NULL;
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    SNDFILE* wav = sf_open(raw_path, SFM_READ, &info);
    assert(wav != NULL);
    sf_close(wav);
    (void)remove(raw_path);
    /* Every block but the mixed one, each 480 samples at 24 kHz as 960 at 48 kHz. */
    expect_int("rtl-633", "round trip block left out of -6", (long)info.frames, (long)(blocks - 1) * 960L);
}

/* The 20 ms blocks each raw WAV level case reads, and the frames they come to at 48 kHz. */
#define RAW_WAV_LEVEL_BLOCKS 6
#define RAW_WAV_LEVEL_FRAMES (RAW_WAV_LEVEL_BLOCKS * 960)

/* One raw WAV level case: what the front end delivers, and the frames the -6 WAV must hold. */
typedef struct {
    const char* label;
    /* Every frame from @p settle on is @p want within @p tolerance; the ones before it, the converter's start from
       silence, are no larger than @p settle_max. */
    long want;
    long tolerance;
    long settle_max;
    int output_kind;
    unsigned int rate_hz;
    float value;
    int settle;
} raw_wav_level_case;

/* Runs @p c: RAW_WAV_LEVEL_BLOCKS blocks of @p c->value samples, the -6 WAV recording them, then reads it back. */
static void
run_raw_wav_level(dsd_opts* opts, dsd_state* state, void* rtl_context, const raw_wav_level_case* c) {
    reset_stream_fixture();
    reset_decoder_fixture(opts, state, rtl_context);
    reset_analog_block_capture();
    const int monitor = c->output_kind == RTL_STREAM_OUTPUT_AUDIO_MONITOR;
    g_output_kind = c->output_kind;
    g_output_rate_hz = c->rate_hz;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    g_analog_family = monitor;
    g_monitor_published = monitor;
    g_constant_samples = 1;
    g_constant_value = c->value;
    /* The monitor output is read one sample at a time; the discriminator output fills the symbol cache four at a
       time. */
    g_batch_samples = monitor ? 1 : 4;
    set_analog_block_output(opts, monitor);
    state->rf_mod = 0;
    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    char raw_path[] = "dsdneo_rtl_raw_level_XXXXXX";
    opts->wav_out_raw = open_raw_wav(raw_path);
    const int block_samples = (int)c->rate_hz / 50;
    read_until_reads(opts, state, (RAW_WAV_LEVEL_BLOCKS * block_samples) / g_batch_samples);
    sf_close(opts->wav_out_raw);
    opts->wav_out_raw = NULL;

    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    SNDFILE* wav = sf_open(raw_path, SFM_READ, &info);
    assert(wav != NULL);
    static short frames[RAW_WAV_LEVEL_FRAMES];
    const sf_count_t got = sf_read_short(wav, frames, (sf_count_t)RAW_WAV_LEVEL_FRAMES);
    sf_close(wav);
    (void)remove(raw_path);
    /* The last block may still be filling when the reads stop. */
    expect_int(c->label, "frames written", got >= (sf_count_t)(RAW_WAV_LEVEL_FRAMES - 960), 1);
    int bad = 0;
    for (sf_count_t i = 0; i < got; i++) {
        const long v = frames[i];
        const int settled = i >= c->settle;
        const int ok = settled ? labs(v - c->want) <= c->tolerance : labs(v) <= c->settle_max;
        if (!ok && bad++ == 0) {
            if (settled) {
                DSD_FPRINTF(stderr, "FAIL: %s: frame %lld of %lld is %ld, want %ld +/- %ld\n", c->label, (long long)i,
                            (long long)got, v, c->want, c->tolerance);
            } else {
                DSD_FPRINTF(stderr, "FAIL: %s: settling frame %lld is %ld, want at most %ld either way\n", c->label,
                            (long long)i, v, c->settle_max);
            }
        }
    }
    expect_int(c->label, "frames off the level", bad, 0);
    g_constant_samples = 0;
}

/*
 * Issue #643: the -6 raw WAV on RTL monitor input at the int16 level a PCM input at the reference has. The monitor's
 * samples run at its 1/pi output scale, where the reference signal (1 kHz at 3 kHz deviation, or AM at 50 %) peaks at
 * 0.25 with the default volume trim, so written as they were they rounded to 0 or +/-1: a silent WAV. The WAV scales
 * them by 8231 / 0.25 = 32924, so 0.125 is written as 4116 (within 1 %), at the sink rate (48 kHz, written as it is)
 * and converted from 24 kHz. The converter starts from silence: with 16 taps a phase when upsampling
 * (rate_converter_taps_per_phase()) every tap holds the level from the 16th input, frame 30, on, and before that its
 * filter's step response rings either side of zero and overshoots the level by 12.6 % (4632), bounded here at 25 %:
 * the scale is applied once, to the converted samples. The FSK discriminator output, normalised to +/-30000 already, is
 * written exactly as it is at the sink rate, and converted unscaled from 24 kHz.
 */
static void
test_rtl_raw_wav_level(dsd_opts* opts, dsd_state* state, void* rtl_context) {
    static const raw_wav_level_case cases[] = {
        {"rtl-643 monitor at 48 kHz", 4116L, 41L, 0L, RTL_STREAM_OUTPUT_AUDIO_MONITOR, 48000U, 0.125f, 0},
        {"rtl-643 monitor at 24 kHz", 4116L, 41L, 5145L, RTL_STREAM_OUTPUT_AUDIO_MONITOR, 24000U, 0.125f, 30},
        {"rtl-643 FSK discriminator at 48 kHz", 12000L, 0L, 0L, RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR, 48000U, 12000.25f,
         0},
        {"rtl-643 FSK discriminator at 24 kHz", 12000L, 120L, 15000L, RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR, 24000U,
         12000.25f, 30},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_raw_wav_level(opts, state, rtl_context, &cases[i]);
    }
    reset_stream_fixture();
    reset_decoder_fixture(opts, state, rtl_context);
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
        .analog_profile = fake_analog_profile,
        .analog_family_active = fake_analog_family_active,
        .replay_batch = fake_replay_batch,
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

    test_replay_keeps_first_batch_after_change(&opts, &state, &fake_rtl_context);

    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast_analog = fake_blast_analog});
    test_analog_block_follows_family_switch(&opts, &state, &fake_rtl_context);
    test_analog_block_follows_kind_switch(&opts, &state, &fake_rtl_context);
    test_analog_block_follows_typed_row_leave(&opts, &state, &fake_rtl_context);
    test_digital_decoder_collects_any_monitor(&opts, &state, &fake_rtl_context);
    test_return_to_radio_rescales_from_the_pcm_rate(&opts, &state, &fake_rtl_context);
    test_rtl_rate_round_trip_inside_one_symbol(&opts, &state, &fake_rtl_context);
    test_rtl_raw_wav_level(&opts, &state, &fake_rtl_context);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});

    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    dsd_state_ext_free_all(&state);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d RTL symbol cache check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
