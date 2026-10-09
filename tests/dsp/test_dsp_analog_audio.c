// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The analog monitor's audio chain (issue #518): the gain each source's reference signal needs, the fixed gain and the
 * AGC on top of it, which band-pass runs, the legacy filter flags, resets, the playing flag, int16 saturation, and the
 * state-extension slot that holds it.
 */

#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/dsp/analog_audio.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/analog_voice.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_failures;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                   \
            DSD_FPRINTF(stderr, __VA_ARGS__);                                                                          \
            DSD_FPRINTF(stderr, "\n");                                                                                 \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static double
db(double v) {
    return 20.0 * log10(v > 1e-300 ? v : 1e-300);
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} fixture;

static fixture
fixture_new(void) {
    fixture f;
    f.opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    f.state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!f.opts || !f.state) {
        DSD_FPRINTF(stderr, "out of memory\n");
        exit(2);
    }
    f.opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    f.opts->audio_gainA = 0.0f;
    init_audio_filters(f.state, 48000);
    return f;
}

static void
fixture_free(fixture* f) {
    dsd_state_ext_free_all(f->state);
    free(f->state);
    free(f->opts);
}

/* @p seconds of a @p hz tone of peak @p amp through @p chain in 20 ms blocks; returns the output peak over the last
   half second. */
static double
run_tone(fixture* f, dsd_analog_audio_chain chain, dsd_analog_audio_source source, double hz, double amp,
         double seconds, unsigned int flags, double* phase) {
    const int fs = 48000;
    const int total = (int)(seconds * fs);
    double peak = 0.0;
    float buf[960];
    for (int n = 0; n < total; n += 960) {
        for (int i = 0; i < 960; i++) {
            buf[i] = (float)(amp * sin(*phase));
            *phase += 2.0 * M_PI * hz / fs;
        }
        CHECK(dsd_analog_audio_process_f(f->opts, f->state, chain, buf, 960U, source, fs, flags) == 0, "process");
        if (n >= total - fs / 2) {
            for (int i = 0; i < 960; i++) {
                peak = fabs((double)buf[i]) > peak ? fabs((double)buf[i]) : peak;
            }
        }
    }
    return peak;
}

static void
test_source_gains(void) {
    CHECK(fabs(dsd_analog_audio_source_gain(DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR) - 32924.0) < 1e-6, "RTL monitor %g",
          dsd_analog_audio_source_gain(DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR));
    CHECK(fabs(dsd_analog_audio_source_gain(DSD_ANALOG_AUDIO_SOURCE_RTL_FSK) - (8231.0 / 30000.0)) < 1e-9, "RTL FSK %g",
          dsd_analog_audio_source_gain(DSD_ANALOG_AUDIO_SOURCE_RTL_FSK));
    CHECK(fabs(dsd_analog_audio_source_gain(DSD_ANALOG_AUDIO_SOURCE_PCM16) - 1.0) < 1e-12, "PCM %g",
          dsd_analog_audio_source_gain(DSD_ANALOG_AUDIO_SOURCE_PCM16));
    CHECK(dsd_analog_gain_is_auto(0.0f), "0 is auto");
    CHECK(dsd_analog_gain_is_auto(-3.0f), "negative is auto");
    CHECK(dsd_analog_gain_is_auto(NAN), "NaN is auto");
    CHECK(!dsd_analog_gain_is_auto(1.0f), "1 is fixed");
    CHECK(!dsd_analog_gain_is_auto(50.0f), "50 is fixed");
}

/* The -6 raw WAV's int16 scale (issue #643): RTL monitor audio, FM or AM, goes to the level a PCM input at the
   reference has (its 0.25 reference to the 8231 reference peak); the FSK discriminator output and PCM are at int16
   scale already and go through exactly as they are. The source gains, which test_source_gains() pins, stay as they
   were. */
static void
test_int16_scale(void) {
    CHECK(fabs(DSD_ANALOG_AUDIO_RTL_MONITOR_REFERENCE - 0.25) < 1e-12, "RTL monitor reference %g",
          DSD_ANALOG_AUDIO_RTL_MONITOR_REFERENCE);
    const double rtl = dsd_analog_audio_int16_scale(DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR);
    CHECK(fabs(rtl - 32924.0) <= 1e-9 * 32924.0, "RTL monitor scale %.9g", rtl);
    CHECK(fabs((DSD_ANALOG_AUDIO_RTL_MONITOR_REFERENCE * rtl) - DSD_ANALOG_AUDIO_REFERENCE_PEAK) < 1e-9,
          "RTL monitor reference lands at %.9g", DSD_ANALOG_AUDIO_RTL_MONITOR_REFERENCE * rtl);
    /* Exactly 1: no double but 1.0 lies within DBL_EPSILON / 4 of it, so the samples are written unchanged. */
    const double fsk = dsd_analog_audio_int16_scale(DSD_ANALOG_AUDIO_SOURCE_RTL_FSK);
    const double pcm = dsd_analog_audio_int16_scale(DSD_ANALOG_AUDIO_SOURCE_PCM16);
    CHECK(fabs(fsk - 1.0) < DBL_EPSILON / 4.0, "RTL FSK scale %.17g", fsk);
    CHECK(fabs(pcm - 1.0) < DBL_EPSILON / 4.0, "PCM scale %.17g", pcm);
}

/* The fixed gain takes each source's reference to the -12 dBFS reference peak at -n 50 and scales with N / 50. */
static void
test_fixed_gain(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 0;

    const struct {
        dsd_analog_audio_source source;
        float in;
    } refs[] = {
        {DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 0.25f},
        {DSD_ANALOG_AUDIO_SOURCE_RTL_FSK, 30000.0f},
        {DSD_ANALOG_AUDIO_SOURCE_PCM16, 8231.0f},
    };

    for (size_t r = 0; r < sizeof refs / sizeof refs[0]; r++) {
        f.opts->audio_gainA = 50.0f;
        float x[2] = {refs[r].in, -refs[r].in};
        (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, x, 2U, refs[r].source, 48000,
                                         DSD_ANALOG_AUDIO_PLAYING);
        CHECK(fabs((double)x[0] - 8231.0) < 0.05 && fabs((double)x[1] + 8231.0) < 0.05,
              "source %d at -n 50: %.3f / %.3f, want +/-8231", (int)refs[r].source, (double)x[0], (double)x[1]);
        CHECK(fabs((double)f.state->aout_gainA) < 1e-4, "-n 50 publishes 0 dB, got %.4f", (double)f.state->aout_gainA);
        f.opts->audio_gainA = 100.0f;
        float y[1] = {refs[r].in};
        (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, y, 1U, refs[r].source, 48000,
                                         DSD_ANALOG_AUDIO_PLAYING);
        CHECK(fabs((double)y[0] - 16462.0) < 0.1, "source %d at -n 100: %.3f, want 16462", (int)refs[r].source,
              (double)y[0]);
        CHECK(fabs((double)f.state->aout_gainA - 6.0206) < 1e-3, "-n 100 publishes +6.02 dB, got %.4f",
              (double)f.state->aout_gainA);
    }
    fixture_free(&f);
}

/* With the band-pass on, the reference tone (1 kHz) plays at the reference peak less the passband ripple there. */
static void
test_fixed_gain_through_bandpass(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 1;
    f.opts->audio_gainA = 50.0f;
    double phase = 0.0;
    const double peak = run_tone(&f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 1000.0, 0.25,
                                 1.0, DSD_ANALOG_AUDIO_PLAYING, &phase);
    dsd_voice_bandpass bp;
    (void)dsd_voice_bandpass_design(&bp, DSD_VOICE_BAND_FM, 48000);
    const double want = 8231.0 * pow(10.0, dsd_voice_bandpass_gain_db(&bp, 1000.0) / 20.0);
    CHECK(fabs(db(peak / want)) < 0.05, "reference tone through the band-pass peaks at %.1f, want %.1f", peak, want);
    fixture_free(&f);
}

/* On auto, the reference tone's peak settles at the AGC target and the published gain says how far above -n 50. */
static void
test_auto_gain(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 1;
    double phase = 0.0;
    const double peak = run_tone(&f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 1000.0, 0.25,
                                 2.0, DSD_ANALOG_AUDIO_PLAYING, &phase);
    CHECK(fabs(db(peak / (double)DSD_VOICE_AGC_TARGET_PEAK)) < 0.1, "auto settles at %.1f, want %.1f", peak,
          (double)DSD_VOICE_AGC_TARGET_PEAK);
    /* The reference reaches the band-pass at about -12 dBFS peak, so the AGC adds about 2 dB. */
    CHECK(f.state->aout_gainA > 1.5f && f.state->aout_gainA < 2.8f, "published auto gain %.2f dB",
          (double)f.state->aout_gainA);

    /* The same tone 20 dB quieter: the AGC adds its most, 18 dB, and no more. */
    phase = 0.0;
    (void)run_tone(&f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 1000.0, 0.025, 8.0,
                   DSD_ANALOG_AUDIO_PLAYING, &phase);
    CHECK(fabs((double)f.state->aout_gainA - DSD_VOICE_AGC_MAX_BOOST_DB) < 0.05, "quiet tone boosted %.2f dB",
          (double)f.state->aout_gainA);
    fixture_free(&f);
}

/* Not playing: the AGC holds its gain whatever arrives, and the output stays limited. */
static void
test_not_playing_freezes(void) {
    fixture f = fixture_new();
    double phase = 0.0;
    (void)run_tone(&f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 1000.0, 0.25, 2.0,
                   DSD_ANALOG_AUDIO_PLAYING, &phase);
    const float before = f.state->aout_gainA;
    const double peak =
        run_tone(&f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 1000.0, 2.5, 1.0, 0U, &phase);
    CHECK(fabsf(f.state->aout_gainA - before) < 1e-5f, "a muted span moved the gain %.3f -> %.3f dB", (double)before,
          (double)f.state->aout_gainA);
    CHECK(peak <= (double)DSD_VOICE_AGC_TARGET_PEAK * 1.0001, "muted output peak %.1f above the target", peak);
    fixture_free(&f);
}

/* RESET, and a change of source, rate or band, start the chain over at the source's reference gain. */
static void
test_resets(void) {
    fixture f = fixture_new();
    double phase = 0.0;
    float zeros[960];
    const unsigned int playing = DSD_ANALOG_AUDIO_PLAYING;
    const dsd_analog_audio_source rtl = DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR;

    (void)run_tone(&f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, rtl, 1000.0, 0.025, 8.0, playing, &phase);
    CHECK(f.state->aout_gainA > 17.0f, "adapted to a quiet tone (%.2f dB)", (double)f.state->aout_gainA);
    DSD_MEMSET(zeros, 0, sizeof zeros);
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, zeros, 960U, rtl, 48000,
                                     playing | DSD_ANALOG_AUDIO_RESET);
    CHECK(fabsf(f.state->aout_gainA) < 1e-4f, "RESET left %.3f dB", (double)f.state->aout_gainA);

    (void)run_tone(&f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, rtl, 1000.0, 0.025, 8.0, playing, &phase);
    DSD_MEMSET(zeros, 0, sizeof zeros);
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, zeros, 960U, rtl, 24000, playing);
    CHECK(fabsf(f.state->aout_gainA) < 1e-4f, "a rate change left %.3f dB", (double)f.state->aout_gainA);

    (void)run_tone(&f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, rtl, 1000.0, 0.025, 8.0, playing, &phase);
    DSD_MEMSET(zeros, 0, sizeof zeros);
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, zeros, 960U,
                                     DSD_ANALOG_AUDIO_SOURCE_PCM16, 48000, playing);
    CHECK(fabsf(f.state->aout_gainA) < 1e-4f, "a source change left %.3f dB", (double)f.state->aout_gainA);

    (void)run_tone(&f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, rtl, 1000.0, 0.025, 8.0, playing, &phase);
    f.opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    DSD_MEMSET(zeros, 0, sizeof zeros);
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, zeros, 960U, rtl, 48000, playing);
    CHECK(fabsf(f.state->aout_gainA) < 1e-4f, "an FM -> AM change left %.3f dB", (double)f.state->aout_gainA);
    fixture_free(&f);
}

static uint32_t g_fake_rtl_generation = 1U;

static uint32_t
fake_rtl_generation(void) {
    return g_fake_rtl_generation;
}

/* Feed one block of a 1 kHz tone at @p amp through @p chain; returns its output peak. @p boundary, when set, runs
   between the block's start (dsd_analog_audio_block_begin()) and its processing, as a retune landing while the block
   fills. */
static double
one_block_peak_on(fixture* f, dsd_analog_audio_chain chain, dsd_analog_audio_source source, double amp, double* phase,
                  int begin, void (*boundary)(fixture*)) {
    float buf[960];
    if (begin) {
        dsd_analog_audio_block_begin(f->opts, f->state, chain);
    }
    for (int i = 0; i < 960; i++) {
        buf[i] = (float)(amp * sin(*phase));
        *phase += 2.0 * M_PI * 1000.0 / 48000.0;
    }
    if (boundary) {
        boundary(f);
    }
    (void)dsd_analog_audio_process_f(f->opts, f->state, chain, buf, 960U, source, 48000, DSD_ANALOG_AUDIO_PLAYING);
    double peak = 0.0;
    for (int i = 0; i < 960; i++) {
        peak = fabs((double)buf[i]) > peak ? fabs((double)buf[i]) : peak;
    }
    return peak;
}

static double
one_block_peak(fixture* f, dsd_analog_audio_source source, double amp, double* phase, void (*boundary)(fixture*)) {
    return one_block_peak_on(f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, source, amp, phase, 1, boundary);
}

static void
boundary_tuning(fixture* f) {
    (void)f;
    dsd_trunk_tuning_generation_advance();
}

static void
boundary_stream(fixture* f) {
    (void)f;
    g_fake_rtl_generation++;
}

static void
boundary_announced(fixture* f) {
    dsd_analog_audio_note_reception(f->state);
}

/* What the legacy -Y rigctl retune, a scan row commit or a reconnect calls: the receive tap's reset announces the
   boundary to the audio chain too, whether or not the tap runs. */
static void
boundary_rx_reset(fixture* f) {
    dsd_analog_rx_reset(f->state);
}

/* A block that fills across a boundary -- a new trunk-tuning generation on any input, a new RTL stream generation on
   an RTL input, or a boundary dsd_analog_rx_reset() announces (the legacy -Y rigctl retune moves neither generation)
   -- is partly the channel before it, whichever path collected it (the -8 source monitor has no tap to report it): it
   is dropped, so a loud tail of the old channel neither plays nor pulls the new channel's gain down. The block after
   it, collected wholly on the new channel, plays and starts the chain at the reference gain. */
static void
test_new_reception_starts_over(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 1;
    double phase = 0.0;
    const dsd_analog_audio_source rtl = DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR;
    dsd_rtl_stream_metrics_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.stream_generation = fake_rtl_generation;
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    f.opts->audio_in_type = AUDIO_IN_RTL;
    void (*const boundaries[])(fixture*) = {boundary_tuning, boundary_stream, boundary_announced, boundary_rx_reset};
    const char* const names[] = {"tuning generation", "stream generation", "announced reception", "tap reset"};
    for (size_t b = 0; b < sizeof boundaries / sizeof boundaries[0]; b++) {
        for (int blk = 0; blk < 400; blk++) {
            (void)one_block_peak(&f, rtl, 0.025, &phase, NULL);
        }
        CHECK(f.state->aout_gainA > 17.0f, "%s: adapted (%.2f dB)", names[b], (double)f.state->aout_gainA);
        CHECK(one_block_peak(&f, rtl, 2.0, &phase, boundaries[b]) < 1e-12, "%s: a block across it played", names[b]);
        const double next = one_block_peak(&f, rtl, 0.025, &phase, NULL);
        CHECK(next > 0.0 && fabsf(f.state->aout_gainA) < 0.5f, "%s: the next block played at %.2f dB", names[b],
              (double)f.state->aout_gainA);
    }
    /* A block collected wholly after the boundary is the new reception's: it plays, from the reference gain. */
    for (int blk = 0; blk < 400; blk++) {
        (void)one_block_peak(&f, rtl, 0.025, &phase, NULL);
    }
    dsd_trunk_tuning_generation_advance();
    const double clean = one_block_peak(&f, rtl, 0.25, &phase, NULL);
    CHECK(clean > 1000.0 && fabsf(f.state->aout_gainA) < 2.5f, "a block after the boundary played %.1f at %.2f dB",
          clean, (double)f.state->aout_gainA);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    fixture_free(&f);
}

/* EDACS collects three blocks and processes them afterwards, so it notes the reception once for the triplet: a retune
   landing during the collection drops all three, the old channel's samples in the first two included. */
static void
test_edacs_triplet_across_a_boundary(void) {
    fixture f = fixture_new();
    double phase = 0.0;
    const dsd_analog_audio_source pcm = DSD_ANALOG_AUDIO_SOURCE_PCM16;
    float blocks[3][960];
    for (int t = 0; t < 2; t++) {
        dsd_analog_audio_block_begin(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_EDACS);
        for (int b = 0; b < 3; b++) {
            for (int i = 0; i < 960; i++) {
                blocks[b][i] = (float)(3000.0 * sin(phase));
                phase += 2.0 * M_PI * 1000.0 / 48000.0;
            }
        }
        if (t == 1) {
            dsd_trunk_tuning_generation_advance();
        }
        double peak = 0.0;
        for (int b = 0; b < 3; b++) {
            (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_EDACS, blocks[b], 960U, pcm, 48000,
                                             DSD_ANALOG_AUDIO_PLAYING);
            for (int i = 0; i < 960; i++) {
                peak = fabs((double)blocks[b][i]) > peak ? fabs((double)blocks[b][i]) : peak;
            }
        }
        if (t == 0) {
            CHECK(peak > 1000.0, "a triplet within one reception did not play (peak %.1f)", peak);
        } else {
            CHECK(peak < 1e-12, "a triplet across a retune played (peak %.1f)", peak);
        }
    }
    fixture_free(&f);
}

/* A dropped block (one that straddles a retune) comes out as silence, filters nothing into any state, the voice
   band-pass's or the legacy filters', and the chain starts over with the samples after it. */
static void
test_discard_starts_over_after(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 1;
    f.opts->use_lpf = 1;
    f.opts->use_hpf = 1;
    double phase = 0.0;
    const dsd_analog_audio_source rtl = DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR;
    (void)run_tone(&f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, rtl, 1000.0, 0.025, 8.0, DSD_ANALOG_AUDIO_PLAYING, &phase);
    const float adapted = f.state->aout_gainA;
    float block[960];
    for (int i = 0; i < 960; i++) {
        block[i] = (float)(2.0 * sin(2.0 * M_PI * 400.0 * (double)i / 48000.0));
    }
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, block, 960U, rtl, 48000,
                                     DSD_ANALOG_AUDIO_DISCARD | DSD_ANALOG_AUDIO_PLAYING);
    double dropped = 0.0;
    for (int i = 0; i < 960; i++) {
        dropped = fabs((double)block[i]) > dropped ? fabs((double)block[i]) : dropped;
    }
    CHECK(dropped < 1e-12, "a discarded block played (peak %.3g)", dropped);
    CHECK(fabsf(f.state->aout_gainA - adapted) < 1e-6f, "a discarded block moved the published gain");
    /* The next block starts from the reference gain with clear filters: silence in, silence out. */
    float zeros[960];
    DSD_MEMSET(zeros, 0, sizeof zeros);
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, zeros, 960U, rtl, 48000,
                                     DSD_ANALOG_AUDIO_PLAYING);
    CHECK(fabsf(f.state->aout_gainA) < 1e-4f, "after a discard the chain kept %.3f dB", (double)f.state->aout_gainA);
    double peak = 0.0;
    for (int i = 0; i < 960; i++) {
        peak = fabs((double)zeros[i]) > peak ? fabs((double)zeros[i]) : peak;
    }
    CHECK(peak < 1e-9, "the filters carried a %.3g tail past the discard", peak);
    fixture_free(&f);
}

/* The legacy 960 Hz filters are the chain's own and start over with it: a source change from the FSK output
   (+/-30000) to monitor audio leaves no charge in the low-pass for the new source to play. */
static void
test_legacy_filters_start_over_with_the_chain(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 0;
    f.opts->use_lpf = 1;
    f.opts->audio_gainA = 50.0f;
    float block[960];
    for (int i = 0; i < 960; i++) {
        block[i] = 30000.0f;
    }
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, block, 960U,
                                     DSD_ANALOG_AUDIO_SOURCE_RTL_FSK, 48000, DSD_ANALOG_AUDIO_PLAYING);
    float zeros[960];
    DSD_MEMSET(zeros, 0, sizeof zeros);
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, zeros, 960U,
                                     DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 48000, DSD_ANALOG_AUDIO_PLAYING);
    double peak = 0.0;
    for (int i = 0; i < 960; i++) {
        peak = fabs((double)zeros[i]) > peak ? fabs((double)zeros[i]) : peak;
    }
    CHECK(peak < 1e-9, "the low-pass carried a %.3g transient across the source change", peak);
    fixture_free(&f);
}

/* The two chains keep their own state: adapting the monitor leaves EDACS at its reference gain. */
static void
test_chains_are_independent(void) {
    fixture f = fixture_new();
    double phase = 0.0;
    (void)run_tone(&f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, DSD_ANALOG_AUDIO_SOURCE_PCM16, 1000.0, 300.0, 8.0,
                   DSD_ANALOG_AUDIO_PLAYING, &phase);
    CHECK(f.state->aout_gainA > 17.0f, "monitor adapted (%.2f dB)", (double)f.state->aout_gainA);
    float zeros[960];
    DSD_MEMSET(zeros, 0, sizeof zeros);
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_EDACS, zeros, 960U,
                                     DSD_ANALOG_AUDIO_SOURCE_PCM16, 48000, DSD_ANALOG_AUDIO_PLAYING);
    CHECK(fabsf(f.state->aout_gainA) < 1e-4f, "EDACS chain starts at %.3f dB", (double)f.state->aout_gainA);
    fixture_free(&f);
}

/* RMS of a @p hz tone through @p chain at the fixed gain, after a settling second. */
static double
tone_rms_out_on(fixture* f, dsd_analog_audio_chain chain, dsd_analog_audio_source source, double hz) {
    double phase = 0.0;
    f->opts->audio_gainA = 50.0f;
    (void)run_tone(f, chain, source, hz, 0.25, 1.0, DSD_ANALOG_AUDIO_PLAYING, &phase);
    float buf[960];
    double sum = 0.0;
    for (int b = 0; b < 25; b++) {
        for (int i = 0; i < 960; i++) {
            buf[i] = (float)(0.25 * sin(phase));
            phase += 2.0 * M_PI * hz / 48000.0;
        }
        (void)dsd_analog_audio_process_f(f->opts, f->state, chain, buf, 960U, source, 48000, DSD_ANALOG_AUDIO_PLAYING);
        for (int i = 0; i < 960; i++) {
            sum += (double)buf[i] * (double)buf[i];
        }
    }
    return sqrt(sum / (25.0 * 960.0));
}

static double
tone_rms_out(fixture* f, dsd_analog_audio_source source, double hz) {
    return tone_rms_out_on(f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, source, hz);
}

/* The monitor under AM runs the AM band-pass (200 Hz high-pass), whether the RTL detector or a receiver ahead of PCM
   input demodulated it; everything else runs the FM one, which takes CTCSS 40 dB down. */
static void
test_band_follows_the_monitor(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 1;
    const double fm = tone_rms_out(&f, DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 250.0);
    f.opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    const double am = tone_rms_out(&f, DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 250.0);
    const double ref = 8231.0 / sqrt(2.0);
    CHECK(db(fm / ref) < -39.0, "FM monitor: 250 Hz only %.1f dB down", -db(fm / ref));
    CHECK(db(am / ref) > -6.0, "AM monitor: 250 Hz %.1f dB down", -db(am / ref));
    /* AM through a receiver ahead of PCM input (an AM scan row on rigctl) runs the AM band too. */
    const double pcm = tone_rms_out(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, 250.0);
    CHECK(db(pcm / (0.25 / sqrt(2.0))) > -6.0, "PCM under an AM monitor: 250 Hz %.1f dB down",
          -db(pcm / (0.25 / sqrt(2.0))));
    /* The FSK discriminator output, and EDACS on any source, run the FM band whatever the monitor kind. */
    const double fsk = tone_rms_out(&f, DSD_ANALOG_AUDIO_SOURCE_RTL_FSK, 250.0);
    CHECK(db(fsk / (0.25 * dsd_analog_audio_source_gain(DSD_ANALOG_AUDIO_SOURCE_RTL_FSK) / sqrt(2.0))) < -39.0,
          "FSK source under an AM monitor not on the FM band");
    const double edacs = tone_rms_out_on(&f, DSD_ANALOG_AUDIO_CHAIN_EDACS, DSD_ANALOG_AUDIO_SOURCE_PCM16, 250.0);
    CHECK(db(edacs / (0.25 / sqrt(2.0))) < -39.0, "EDACS under an AM monitor not on the FM band");
    fixture_free(&f);
}

/* The legacy 960 Hz filters still answer their flags, after the band-pass. */
static void
test_legacy_filter_flags(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 0;
    const double hf_plain = tone_rms_out(&f, DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 6000.0);
    f.opts->use_lpf = 1;
    const double hf_lpf = tone_rms_out(&f, DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 6000.0);
    f.opts->use_lpf = 0;
    const double lf_plain = tone_rms_out(&f, DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 120.0);
    f.opts->use_hpf = 1;
    const double lf_hpf = tone_rms_out(&f, DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 120.0);
    CHECK(db(hf_lpf / hf_plain) < -10.0, "use_lpf took 6 kHz only %.1f dB down", -db(hf_lpf / hf_plain));
    CHECK(db(lf_hpf / lf_plain) < -10.0, "use_hpf took 120 Hz only %.1f dB down", -db(lf_hpf / lf_plain));
    fixture_free(&f);
}

/* The int16 entry saturates instead of wrapping, and rounds. */
static void
test_short_saturates(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 0;
    f.opts->audio_gainA = 100.0f;
    short x[4] = {30000, -30000, 100, -101};
    CHECK(dsd_analog_audio_process_s(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_EDACS, x, 4U,
                                     DSD_ANALOG_AUDIO_SOURCE_PCM16, 48000, DSD_ANALOG_AUDIO_PLAYING)
              == 0,
          "process_s");
    CHECK(x[0] == 32767 && x[1] == -32768 && x[2] == 200 && x[3] == -202, "saturated to %d %d %d %d", x[0], x[1], x[2],
          x[3]);
    fixture_free(&f);
}

static void
test_slot_lifetime_and_args(void) {
    fixture f = fixture_new();
    CHECK(dsd_state_ext_get(f.state, DSD_STATE_EXT_DSP_ANALOG_AUDIO) == NULL, "slot allocated before use");
    float x[1] = {0.0f};
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, x, 1U,
                                     DSD_ANALOG_AUDIO_SOURCE_PCM16, 48000, 0U);
    CHECK(dsd_state_ext_get(f.state, DSD_STATE_EXT_DSP_ANALOG_AUDIO) != NULL, "slot not allocated on first use");
    dsd_state_ext_free_all(f.state);
    CHECK(dsd_state_ext_get(f.state, DSD_STATE_EXT_DSP_ANALOG_AUDIO) == NULL, "slot not freed");
    CHECK(dsd_analog_audio_process_f(NULL, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, x, 1U,
                                     DSD_ANALOG_AUDIO_SOURCE_PCM16, 48000, 0U)
              == -1,
          "NULL opts accepted");
    /* An out-of-range chain on purpose: the call refuses it. */
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const dsd_analog_audio_chain bad_chain = (dsd_analog_audio_chain)5;
    CHECK(dsd_analog_audio_process_f(f.opts, f.state, bad_chain, x, 1U, DSD_ANALOG_AUDIO_SOURCE_PCM16, 48000, 0U) == -1,
          "unknown chain accepted");
    CHECK(dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, x, 0U,
                                     DSD_ANALOG_AUDIO_SOURCE_PCM16, 48000, 0U)
              == 0,
          "empty block refused");
    fixture_free(&f);
}

int
main(void) {
    test_source_gains();
    test_int16_scale();
    test_fixed_gain();
    test_fixed_gain_through_bandpass();
    test_auto_gain();
    test_not_playing_freezes();
    test_resets();
    test_chains_are_independent();
    test_new_reception_starts_over();
    test_edacs_triplet_across_a_boundary();
    test_discard_starts_over_after();
    test_legacy_filters_start_over_with_the_chain();
    test_band_follows_the_monitor();
    test_legacy_filter_flags();
    test_short_saturates();
    test_slot_lifetime_and_args();
    if (g_failures) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("DSP_ANALOG_AUDIO: OK\n");
    return 0;
}
