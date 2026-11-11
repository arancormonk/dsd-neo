// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The analog monitor's audio chain (issue #518): the gain each source's reference signal needs, the fixed gain and the
 * speech AGC on top of it (start gains per band, what it learns from, the new-reception rule on the decode clock, the
 * limiter), which band-pass runs, the legacy filter flags, resets, int16 output, and the state-extension slot that holds
 * it. The speech cases run the synthetic talkers of test_support/speech_synth.h.
 */

#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/dsp/analog_audio.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/analog_voice.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "p56.h"
#include "speech_synth.h"
#include "test_support.h"

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
    /* The decode clock stands still unless a case runs it (clock_start()): the new-reception rule then counts samples
       alone, whatever the host's speed. */
    dsd_decode_clock_use_test(0U);
    return f;
}

static void
fixture_free(fixture* f) {
    dsd_state_ext_free_all(f->state);
    free(f->state);
    free(f->opts);
    dsd_decode_clock_use_system();
}

/* The start gains over -n 50 (spec D6), dB. */
static const double k_fm_narrow_db = 4.537;
static const double k_fm_wide_db = -1.482;
static const double k_am_db = -2.553;

/* The decode clock as a replay runs it: each call reads the time of its last sample, from the samples fed since
   clock_start() and the pauses since. Frozen, samples go by with no time passing (input read faster than real time). */
static uint64_t g_clock_origin_ns;
static uint64_t g_clock_fed;
static int g_clock_rate;
static int g_clock_frozen;

static void
clock_start(int rate) {
    g_clock_origin_ns = 1000000000000ULL;
    g_clock_fed = 0U;
    g_clock_rate = rate;
    g_clock_frozen = 0;
    dsd_decode_clock_test_set_ns(g_clock_origin_ns);
}

/* The input stops for @p seconds (a source that stops sending): time passes with no samples. */
static void
clock_pause(double seconds) {
    g_clock_origin_ns += (uint64_t)llround(seconds * 1e9);
}

static void
clock_note_call(size_t len) {
    if (g_clock_rate <= 0) {
        return;
    }
    if (g_clock_frozen) {
        g_clock_origin_ns -= (uint64_t)((len * 1000000000ULL) / (uint64_t)g_clock_rate);
    }
    g_clock_fed += len;
    dsd_decode_clock_test_set_ns(g_clock_origin_ns + (((g_clock_fed - 1U) * 1000000000ULL) / (uint64_t)g_clock_rate));
}

static uint32_t g_block_rng = 12345U;

/* @p n samples of @p x, in place, through @p chain on @p source at @p rate, in calls of @p block samples (0: random
   sizes from 1 to 960), each sample learning as @p learn says (NULL: every one as DSD_ANALOG_AUDIO_LEARN in @p flags).
   @p gains, when set, gets the published gain after each call. */
static void
chain_feed_on(fixture* f, dsd_analog_audio_chain chain, dsd_analog_audio_source source, int rate, float* x, size_t n,
              size_t block, const uint8_t* learn, unsigned int flags, float* gains) {
    size_t done = 0;
    size_t call = 0;
    while (done < n) {
        size_t len = block;
        if (len == 0U) {
            g_block_rng = (g_block_rng * 1103515245U) + 12345U;
            len = 1U + (size_t)((g_block_rng >> 8) % 960U);
        }
        len = len < n - done ? len : n - done;
        clock_note_call(len);
        /* As the monitor notes each block's reception when it starts filling. */
        dsd_analog_audio_block_begin(f->opts, f->state, chain);
        CHECK(dsd_analog_audio_process_runs_f(f->opts, f->state, chain, x + done, len, source, rate, flags,
                                              learn ? learn + done : NULL)
                  == 0,
              "process");
        done += len;
        if (gains) {
            gains[call] = f->state->aout_gainA;
        }
        call++;
    }
}

static void
chain_feed(fixture* f, dsd_analog_audio_source source, int rate, float* x, size_t n, size_t block, const uint8_t* learn,
           unsigned int flags, float* gains) {
    chain_feed_on(f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, source, rate, x, n, block, learn, flags, gains);
}

static float*
alloc_f(size_t n) {
    float* p = n > 0U ? (float*)calloc(n, sizeof(float)) : NULL;
    if (!p) {
        DSD_FPRINTF(stderr, "cannot allocate %zu samples\n", n);
        exit(2);
    }
    return p;
}

static size_t
secs(int rate, double s) {
    return (size_t)llround(s * (double)rate);
}

/* @p n samples of the synthetic talker @p seed at a nominal P.56 level of @p level_dbfs, int16 units. */
static float*
talker(int rate, size_t n, double level_dbfs, uint64_t seed) {
    float* x = alloc_f(n);
    dsd_test_speech_params p;
    dsd_test_speech_defaults(&p, seed);
    p.level_dbfs = level_dbfs;
    CHECK(dsd_test_synth_speech(x, n, rate, &p) == 0, "speech");
    return x;
}

/* Adds @p kind noise at @p rms_dbfs to @p x. */
static void
add_noise(float* x, size_t n, int rate, uint64_t seed, dsd_test_noise_kind kind, double rms_dbfs) {
    float* w = alloc_f(n);
    CHECK(dsd_test_synth_noise(w, n, rate, seed, kind, rms_dbfs) == 0, "noise");
    for (size_t i = 0; i < n; i++) {
        x[i] += w[i];
    }
    free(w);
}

static double
rms_dbfs(const float* x, size_t n) {
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        sum += (double)x[i] * (double)x[i];
    }
    return 10.0 * log10((sum / (double)(n > 0U ? n : 1U)) / (32768.0 * 32768.0) + 1e-30);
}

static double
peak_of(const float* x, size_t n) {
    double peak = 0.0;
    for (size_t i = 0; i < n; i++) {
        peak = fabs((double)x[i]) > peak ? fabs((double)x[i]) : peak;
    }
    return peak;
}

/* The monitor chain on @p source learns 4 s of a quiet talker (-40 dBFS at -n 50, where the chain's gain over the
   source's is @p made_up): the gain climbs well above the start gain. Returns it. */
static float
adapt_quiet_over(fixture* f, dsd_analog_audio_source source, double made_up) {
    const int rate = 48000;
    const size_t n = secs(rate, 4.0);
    float* x = talker(rate, n, -40.0, 7U);
    const double scale = 1.0 / (dsd_analog_audio_source_gain(source) * made_up);
    for (size_t i = 0; i < n; i++) {
        x[i] = (float)((double)x[i] * scale);
    }
    chain_feed(f, source, rate, x, n, 960U, NULL, DSD_ANALOG_AUDIO_LEARN, NULL);
    free(x);
    return f->state->aout_gainA;
}

static float
adapt_quiet(fixture* f, dsd_analog_audio_source source) {
    return adapt_quiet_over(f, source, 1.0);
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
                                         DSD_ANALOG_AUDIO_LEARN);
        CHECK(fabs((double)x[0] - 8231.0) < 0.05 && fabs((double)x[1] + 8231.0) < 0.05,
              "source %d at -n 50: %.3f / %.3f, want +/-8231", (int)refs[r].source, (double)x[0], (double)x[1]);
        CHECK(fabs((double)f.state->aout_gainA) < 1e-4, "-n 50 publishes 0 dB, got %.4f", (double)f.state->aout_gainA);
        f.opts->audio_gainA = 100.0f;
        float y[1] = {refs[r].in};
        (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, y, 1U, refs[r].source, 48000,
                                         DSD_ANALOG_AUDIO_LEARN);
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
                                 1.0, DSD_ANALOG_AUDIO_LEARN, &phase);
    dsd_voice_bandpass bp;
    (void)dsd_voice_bandpass_design(&bp, DSD_VOICE_BAND_FM, 48000);
    const double want = 8231.0 * pow(10.0, dsd_voice_bandpass_gain_db(&bp, 1000.0) / 20.0);
    CHECK(fabs(db(peak / want)) < 0.05, "reference tone through the band-pass peaks at %.1f, want %.1f", peak, want);
    fixture_free(&f);
}

/* On auto a steady tone is no speech (issue #518, D9): the AGC turns it away as a tone within 12 frames (240 ms),
   having moved at most the 7 dB its first commits allow from the start gain; then, with nothing committed for a second,
   the attenuator eases it down to -36 dBFS and holds it there, whatever the tone's level. The old AGC normalised any
   tone to -10 dBFS peak (-13 dBFS RMS), a quiet one with +18 dB. */
static void
test_auto_tone_turned_away(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 1;
    const double amps[] = {0.25, 0.025};
    for (size_t a = 0; a < sizeof amps / sizeof amps[0]; a++) {
        float zeros[960];
        DSD_MEMSET(zeros, 0, sizeof zeros);
        (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, zeros, 960U,
                                         DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 48000, DSD_ANALOG_AUDIO_RESET);
        CHECK(fabs((double)f.state->aout_gainA - k_fm_narrow_db) < 1e-3, "starts at %.3f dB",
              (double)f.state->aout_gainA);
        double phase = 0.0;
        (void)run_tone(&f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 1000.0, amps[a], 0.3,
                       DSD_ANALOG_AUDIO_LEARN, &phase);
        CHECK(fabs((double)f.state->aout_gainA - k_fm_narrow_db) <= 7.0, "tone %.3f moved the gain to %.2f dB", amps[a],
              (double)f.state->aout_gainA);
        const double peak = run_tone(&f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 1000.0,
                                     amps[a], 6.0, DSD_ANALOG_AUDIO_LEARN, &phase);
        const float settled = f.state->aout_gainA;
        /* RMS from the last half second's peak: a sine's crest is 3.01 dB. */
        const double rms = db(peak / 32768.0) - 3.0103;
        CHECK(rms > -37.0 && rms < -35.0, "tone %.3f eased to %.2f dBFS RMS", amps[a], rms);
        (void)run_tone(&f, DSD_ANALOG_AUDIO_CHAIN_MONITOR, DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 1000.0, amps[a], 2.0,
                       DSD_ANALOG_AUDIO_LEARN, &phase);
        CHECK(fabsf(f.state->aout_gainA - settled) < 1e-4f, "tone %.3f kept moving the gain: %.3f -> %.3f dB", amps[a],
              (double)settled, (double)f.state->aout_gainA);
    }
    fixture_free(&f);
}

/* On auto, established speech plays at the AGC's -24 dBFS speech target (P.56, within 3 dB), from a quiet talker and
   from a hot one, and the published gain says how far above -n 50. */
static void
test_auto_levels_speech(void) {
    const int rate = 48000;
    const double levels[] = {-38.0, -18.0};
    for (size_t l = 0; l < sizeof levels / sizeof levels[0]; l++) {
        fixture f = fixture_new();
        f.opts->use_pbf = 1;
        const size_t n = secs(rate, 10.0);
        float* x = talker(rate, n, levels[l], 11U);
        float* in = alloc_f(n);
        DSD_MEMCPY(in, x, n * sizeof(float));
        chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, x, n, 960U, NULL, DSD_ANALOG_AUDIO_LEARN, NULL);
        const size_t from = secs(rate, 5.0);
        const double out = dsd_test_p56_level_dbfs(x + from, n - from, rate, NULL);
        CHECK(fabs(out + 24.0) <= 3.0, "talker at %.0f dBFS plays at %.2f dBFS P.56", levels[l], out);
        CHECK(peak_of(x, n) <= 29204.0, "talker at %.0f dBFS peaks at %.1f", levels[l], peak_of(x, n));
        const double applied = out - dsd_test_p56_level_dbfs(in + from, n - from, rate, NULL);
        CHECK(fabs((double)f.state->aout_gainA - applied) < 3.0, "published %.2f dB, applied about %.2f dB",
              (double)f.state->aout_gainA, applied);
        free(in);
        free(x);
        fixture_free(&f);
    }
}

/* Not learning: the AGC holds its gain whatever arrives, and the limiter still holds the output under -1 dBFS. */
static void
test_not_learning_holds(void) {
    fixture f = fixture_new();
    const int rate = 48000;
    const float adapted = adapt_quiet(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16);
    CHECK(adapted > (float)k_fm_narrow_db + 8.0f, "adapted to a quiet talker (%.2f dB)", (double)adapted);
    const size_t n = secs(rate, 3.0);
    float* x = talker(rate, n, -30.0, 21U);
    chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, x, n, 960U, NULL, 0U, NULL);
    CHECK(fabsf(f.state->aout_gainA - adapted) < 1e-4f, "speech that does not learn moved the gain %.3f -> %.3f dB",
          (double)adapted, (double)f.state->aout_gainA);
    free(x);
    x = talker(rate, n, -3.0, 21U);
    chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, x, n, 960U, NULL, 0U, NULL);
    CHECK(peak_of(x, n) <= 29204.0, "output peak %.1f above the -1 dBFS ceiling", peak_of(x, n));
    free(x);
    fixture_free(&f);
}

/* RESET, and a change of source, rate or band, start the chain over at its start gain. */
static void
test_resets(void) {
    fixture f = fixture_new();
    float zeros[960];
    const dsd_analog_audio_source pcm = DSD_ANALOG_AUDIO_SOURCE_PCM16;

    CHECK(adapt_quiet(&f, pcm) > (float)k_fm_narrow_db + 8.0f, "adapted (%.2f dB)", (double)f.state->aout_gainA);
    DSD_MEMSET(zeros, 0, sizeof zeros);
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, zeros, 960U, pcm, 48000,
                                     DSD_ANALOG_AUDIO_LEARN | DSD_ANALOG_AUDIO_RESET);
    CHECK(fabs((double)f.state->aout_gainA - k_fm_narrow_db) < 1e-3, "RESET left %.3f dB", (double)f.state->aout_gainA);

    (void)adapt_quiet(&f, pcm);
    DSD_MEMSET(zeros, 0, sizeof zeros);
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, zeros, 960U, pcm, 24000,
                                     DSD_ANALOG_AUDIO_LEARN);
    CHECK(fabs((double)f.state->aout_gainA - k_fm_narrow_db) < 1e-3, "a rate change left %.3f dB",
          (double)f.state->aout_gainA);

    (void)adapt_quiet(&f, pcm);
    DSD_MEMSET(zeros, 0, sizeof zeros);
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, zeros, 960U,
                                     DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 48000, DSD_ANALOG_AUDIO_LEARN);
    CHECK(fabs((double)f.state->aout_gainA - k_fm_narrow_db) < 1e-3, "a source change left %.3f dB",
          (double)f.state->aout_gainA);

    (void)adapt_quiet(&f, pcm);
    f.opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    DSD_MEMSET(zeros, 0, sizeof zeros);
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, zeros, 960U, pcm, 48000,
                                     DSD_ANALOG_AUDIO_LEARN);
    CHECK(fabs((double)f.state->aout_gainA - k_am_db) < 1e-3, "an FM -> AM change left %.3f dB",
          (double)f.state->aout_gainA);
    fixture_free(&f);
}

/* The gain a fresh reception starts at, over -n 50, after one block that teaches nothing: the band's (D6), FM
   narrowband up to a 16 kHz channel and wideband above it (D7), less the RTL monitor trim over its default 2. The
   start gain follows a setting from the next reset. Before issue #518's speech AGC every one started at 0 dB. */
static double
start_gain_of(fixture* f, dsd_analog_audio_chain chain, dsd_analog_audio_source source) {
    float zeros[960];
    DSD_MEMSET(zeros, 0, sizeof zeros);
    (void)dsd_analog_audio_process_f(f->opts, f->state, chain, zeros, 960U, source, 48000, DSD_ANALOG_AUDIO_RESET);
    return (double)f->state->aout_gainA;
}

static void
test_start_gains(void) {
    fixture f = fixture_new();
    const dsd_analog_audio_chain mon = DSD_ANALOG_AUDIO_CHAIN_MONITOR;
    const dsd_analog_audio_source pcm = DSD_ANALOG_AUDIO_SOURCE_PCM16;
    const dsd_analog_audio_source rtl = DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR;

    const struct {
        int width_hz;
        double want;
    } widths[] = {{0, k_fm_narrow_db},     {8000, k_fm_narrow_db}, {12500, k_fm_narrow_db},
                  {16000, k_fm_narrow_db}, {16001, k_fm_wide_db},  {25000, k_fm_wide_db}};

    for (size_t w = 0; w < sizeof widths / sizeof widths[0]; w++) {
        f.opts->analog_nfm_bandwidth_hz = widths[w].width_hz;
        const double got = start_gain_of(&f, mon, pcm);
        CHECK(fabs(got - widths[w].want) < 1e-3, "FM at %d Hz starts at %.3f dB, want %.3f", widths[w].width_hz, got,
              widths[w].want);
    }
    /* A width change takes effect at the next reception, not inside this one. */
    f.opts->analog_nfm_bandwidth_hz = 12500;
    (void)start_gain_of(&f, mon, pcm);
    f.opts->analog_nfm_bandwidth_hz = 25000;
    float zeros[960];
    DSD_MEMSET(zeros, 0, sizeof zeros);
    (void)dsd_analog_audio_process_f(f.opts, f.state, mon, zeros, 960U, pcm, 48000, 0U);
    CHECK(fabs((double)f.state->aout_gainA - k_fm_narrow_db) < 1e-3, "a width change moved this reception to %.3f",
          (double)f.state->aout_gainA);
    f.opts->analog_nfm_bandwidth_hz = 0;
    /* The FSK discriminator output and EDACS are FM whatever the monitor kind; AM by the AM monitor. */
    CHECK(fabs(start_gain_of(&f, mon, DSD_ANALOG_AUDIO_SOURCE_RTL_FSK) - k_fm_narrow_db) < 1e-3, "FSK source");
    f.opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    CHECK(fabs(start_gain_of(&f, mon, pcm) - k_am_db) < 1e-3, "AM monitor starts at %.3f dB",
          (double)f.state->aout_gainA);
    CHECK(fabs(start_gain_of(&f, DSD_ANALOG_AUDIO_CHAIN_EDACS, pcm) - k_fm_narrow_db) < 1e-3,
          "EDACS under the AM monitor starts at %.3f dB", (double)f.state->aout_gainA);

    /* The RTL monitor trim scales the samples ahead of the chain; the start gain takes it back out. */
    const struct {
        int vol;
        double trim_db;
    } vols[] = {{0, 0.0}, {1, -6.0206}, {2, 0.0}, {3, 3.5218}};

    for (int am = 0; am <= 1; am++) {
        f.opts->analog_demod = am ? DSD_ANALOG_DEMOD_AM : DSD_ANALOG_DEMOD_FM;
        const double band = am ? k_am_db : k_fm_narrow_db;
        for (size_t v = 0; v < sizeof vols / sizeof vols[0]; v++) {
            f.opts->rtl_volume_multiplier = vols[v].vol;
            const double got = start_gain_of(&f, mon, rtl);
            CHECK(fabs(got - (band - vols[v].trim_db)) < 1e-3, "%s vol %d starts at %.3f dB", am ? "AM" : "FM",
                  vols[v].vol, got);
            CHECK(fabs(start_gain_of(&f, mon, pcm) - band) < 1e-3, "PCM moved with vol %d", vols[v].vol);
        }
    }
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
    (void)dsd_analog_audio_process_f(f->opts, f->state, chain, buf, 960U, source, 48000, DSD_ANALOG_AUDIO_LEARN);
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
   it, collected wholly on the new channel, plays and starts the chain at its start gain. */
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
        (void)adapt_quiet(&f, rtl);
        CHECK(f.state->aout_gainA > (float)k_fm_narrow_db + 8.0f, "%s: adapted (%.2f dB)", names[b],
              (double)f.state->aout_gainA);
        CHECK(one_block_peak(&f, rtl, 2.0, &phase, boundaries[b]) < 1e-12, "%s: a block across it played", names[b]);
        const double next = one_block_peak(&f, rtl, 0.025, &phase, NULL);
        CHECK(next > 0.0 && fabs((double)f.state->aout_gainA - k_fm_narrow_db) < 1e-3,
              "%s: the next block played at %.2f dB", names[b], (double)f.state->aout_gainA);
    }
    /* A block collected wholly after the boundary is the new reception's: it plays, from the start gain. */
    (void)adapt_quiet(&f, rtl);
    dsd_trunk_tuning_generation_advance();
    const double clean = one_block_peak(&f, rtl, 0.25, &phase, NULL);
    CHECK(clean > 1000.0 && fabs((double)f.state->aout_gainA - k_fm_narrow_db) < 1e-3,
          "a block after the boundary played %.1f at %.2f dB", clean, (double)f.state->aout_gainA);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    fixture_free(&f);
}

/* The de-emphasis gain the fake stream publishes (dsd_rtl_stream_metrics_hook_monitor_deemph_gain()). */
static double g_fake_deemph_gain = 1.0;

static double
fake_deemph_gain(void) {
    return g_fake_deemph_gain;
}

static void
boundary_deemph(fixture* f) {
    (void)f;
    g_fake_deemph_gain = 1.0;
}

/* One block of +/-@p in (a two-sample square wave, which neither filter nor AGC rounds) through the monitor chain on
   @p source with @p flags; returns the output's first sample. */
static double
deemph_square_out(fixture* f, dsd_analog_audio_source source, double in, unsigned int flags) {
    float x[2] = {(float)in, (float)-in};
    dsd_analog_audio_block_begin(f->opts, f->state, DSD_ANALOG_AUDIO_CHAIN_MONITOR);
    (void)dsd_analog_audio_process_f(f->opts, f->state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, x, 2U, source, 48000, flags);
    return (double)x[0];
}

/* B8/B9 (issue #518): on an RTL input the monitor audio arrives de-emphasized, the 1 kHz reference down by the gain
   the stream publishes; the chain makes it up at the gain stage, so the reference plays at the -12 dBFS reference peak
   at -n 50 and the AGC starts from its start gain over that level, whatever DSD_NEO_DEEMPH selects. aout_gainA stays dB
   over -n 50.
   The FSK discriminator output and PCM samples never ran through the de-emphasis, nor does an RTL monitor source off
   an RTL input: their gain is the source gain alone. A new gain starts the chain over, and a block collected across
   one is dropped. */
static void
test_deemph_gain(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 0;
    dsd_rtl_stream_metrics_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.stream_generation = fake_rtl_generation;
    hooks.monitor_deemph_gain = fake_deemph_gain;
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    f.opts->audio_in_type = AUDIO_IN_RTL;
    const dsd_analog_audio_source rtl = DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR;
    /* 75 us at 48 kHz (+0.865 dB) and 750 us (+13.65 dB), the reference 0.25 taken down by each. */
    const double gains[] = {1.104727, 4.810934};
    for (size_t g = 0; g < sizeof gains / sizeof gains[0]; g++) {
        g_fake_deemph_gain = gains[g];
        f.opts->audio_gainA = 50.0f;
        double out = deemph_square_out(&f, rtl, 0.25 / gains[g], DSD_ANALOG_AUDIO_LEARN);
        CHECK(fabs(out - 8231.0) < 0.05, "gain %.4f at -n 50: %.3f, want 8231", gains[g], out);
        CHECK(fabsf(f.state->aout_gainA) < 1e-4f, "gain %.4f at -n 50 publishes %.4f dB, want 0", gains[g],
              (double)f.state->aout_gainA);
        f.opts->audio_gainA = 100.0f;
        out = deemph_square_out(&f, rtl, 0.25 / gains[g], DSD_ANALOG_AUDIO_LEARN);
        CHECK(fabs(out - 16462.0) < 0.1, "gain %.4f at -n 100: %.3f, want 16462", gains[g], out);
        /* Auto: a block the AGC does not learn from plays at its start gain over the same -n 50 gain. */
        f.opts->audio_gainA = 0.0f;
        out = deemph_square_out(&f, rtl, 0.25 / gains[g], DSD_ANALOG_AUDIO_RESET);
        const double start = 8231.0 * pow(10.0, k_fm_narrow_db / 20.0);
        CHECK(fabs(out - start) < 0.1, "gain %.4f on auto starts at %.3f, want %.3f", gains[g], out, start);
        CHECK(fabs((double)f.state->aout_gainA - k_fm_narrow_db) < 1e-3, "gain %.4f on auto publishes %.4f dB",
              gains[g], (double)f.state->aout_gainA);
        /* Never the FSK discriminator nor PCM. */
        f.opts->audio_gainA = 50.0f;
        out = deemph_square_out(&f, DSD_ANALOG_AUDIO_SOURCE_RTL_FSK, 30000.0, DSD_ANALOG_AUDIO_LEARN);
        CHECK(fabs(out - 8231.0) < 0.05, "gain %.4f on the FSK discriminator: %.3f, want 8231", gains[g], out);
        out = deemph_square_out(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, 8231.0, DSD_ANALOG_AUDIO_LEARN);
        CHECK(fabs(out - 8231.0) < 0.05, "gain %.4f on PCM: %.3f, want 8231", gains[g], out);
    }
    /* Off an RTL input the stream's gain is not the samples': the RTL monitor source gets the source gain alone. */
    f.opts->audio_in_type = AUDIO_IN_PULSE;
    const double off_rtl = deemph_square_out(&f, rtl, 0.25, DSD_ANALOG_AUDIO_LEARN);
    CHECK(fabs(off_rtl - 8231.0) < 0.05, "off an RTL input: %.3f, want 8231", off_rtl);
    f.opts->audio_in_type = AUDIO_IN_RTL;

    /* A new gain on the same generation (a gain changes only with one, but the chain keys on both): the block collected
       across it is dropped, and the next starts over at the new reference gain. */
    f.opts->audio_gainA = 0.0f;
    f.opts->use_pbf = 1;
    g_fake_deemph_gain = gains[1];
    double phase = 0.0;
    (void)adapt_quiet_over(&f, rtl, gains[1]);
    CHECK(f.state->aout_gainA > (float)k_fm_narrow_db + 8.0f, "adapted to a quiet talker (%.2f dB)",
          (double)f.state->aout_gainA);
    CHECK(one_block_peak(&f, rtl, 0.25, &phase, boundary_deemph) < 1e-12, "a block across a new gain played");
    (void)one_block_peak(&f, rtl, 0.0025, &phase, NULL);
    CHECK(fabs((double)f.state->aout_gainA - k_fm_narrow_db) < 1e-3,
          "after a new gain the chain played at %.2f dB, not from the start gain", (double)f.state->aout_gainA);

    g_fake_deemph_gain = 1.0;
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
                                             DSD_ANALOG_AUDIO_LEARN);
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
    const dsd_analog_audio_source rtl = DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR;
    const float adapted = adapt_quiet(&f, rtl);
    CHECK(adapted > (float)k_fm_narrow_db + 8.0f, "adapted (%.2f dB)", (double)adapted);
    float block[960];
    for (int i = 0; i < 960; i++) {
        block[i] = (float)(2.0 * sin(2.0 * M_PI * 400.0 * (double)i / 48000.0));
    }
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, block, 960U, rtl, 48000,
                                     DSD_ANALOG_AUDIO_DISCARD | DSD_ANALOG_AUDIO_LEARN);
    double dropped = 0.0;
    for (int i = 0; i < 960; i++) {
        dropped = fabs((double)block[i]) > dropped ? fabs((double)block[i]) : dropped;
    }
    CHECK(dropped < 1e-12, "a discarded block played (peak %.3g)", dropped);
    CHECK(fabsf(f.state->aout_gainA - adapted) < 1e-6f, "a discarded block moved the published gain");
    /* The next block starts from the start gain with clear filters: silence in, silence out. */
    float zeros[960];
    DSD_MEMSET(zeros, 0, sizeof zeros);
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, zeros, 960U, rtl, 48000,
                                     DSD_ANALOG_AUDIO_LEARN);
    CHECK(fabs((double)f.state->aout_gainA - k_fm_narrow_db) < 1e-3, "after a discard the chain kept %.3f dB",
          (double)f.state->aout_gainA);
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
                                     DSD_ANALOG_AUDIO_SOURCE_RTL_FSK, 48000, DSD_ANALOG_AUDIO_LEARN);
    float zeros[960];
    DSD_MEMSET(zeros, 0, sizeof zeros);
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, zeros, 960U,
                                     DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, 48000, DSD_ANALOG_AUDIO_LEARN);
    double peak = 0.0;
    for (int i = 0; i < 960; i++) {
        peak = fabs((double)zeros[i]) > peak ? fabs((double)zeros[i]) : peak;
    }
    CHECK(peak < 1e-9, "the low-pass carried a %.3g transient across the source change", peak);
    fixture_free(&f);
}

/* The two chains keep their own state: adapting the monitor leaves EDACS at its start gain. */
static void
test_chains_are_independent(void) {
    fixture f = fixture_new();
    CHECK(adapt_quiet(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16) > (float)k_fm_narrow_db + 8.0f, "monitor adapted (%.2f dB)",
          (double)f.state->aout_gainA);
    float zeros[960];
    DSD_MEMSET(zeros, 0, sizeof zeros);
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_EDACS, zeros, 960U,
                                     DSD_ANALOG_AUDIO_SOURCE_PCM16, 48000, DSD_ANALOG_AUDIO_LEARN);
    CHECK(fabs((double)f.state->aout_gainA - k_fm_narrow_db) < 1e-3, "EDACS chain starts at %.3f dB",
          (double)f.state->aout_gainA);
    fixture_free(&f);
}

/* RMS of a @p hz tone through @p chain at the fixed gain, after a settling second. */
static double
tone_rms_out_on(fixture* f, dsd_analog_audio_chain chain, dsd_analog_audio_source source, double hz) {
    double phase = 0.0;
    f->opts->audio_gainA = 50.0f;
    (void)run_tone(f, chain, source, hz, 0.25, 1.0, DSD_ANALOG_AUDIO_LEARN, &phase);
    float buf[960];
    double sum = 0.0;
    for (int b = 0; b < 25; b++) {
        for (int i = 0; i < 960; i++) {
            buf[i] = (float)(0.25 * sin(phase));
            phase += 2.0 * M_PI * hz / 48000.0;
        }
        (void)dsd_analog_audio_process_f(f->opts, f->state, chain, buf, 960U, source, 48000, DSD_ANALOG_AUDIO_LEARN);
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

/* The int16 entry rounds, and the limiter holds a fixed gain's output at -1 dBFS (29204) where it would have clipped. */
static void
test_short_rounds_and_limits(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 0;
    f.opts->audio_gainA = 100.0f;
    short x[2] = {100, -101};
    CHECK(dsd_analog_audio_process_s(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_EDACS, x, 2U,
                                     DSD_ANALOG_AUDIO_SOURCE_PCM16, 48000, DSD_ANALOG_AUDIO_LEARN)
              == 0,
          "process_s");
    CHECK(x[0] == 200 && x[1] == -202, "rounded to %d %d", x[0], x[1]);
    short y[2] = {30000, -30000};
    (void)dsd_analog_audio_process_s(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_EDACS, y, 2U,
                                     DSD_ANALOG_AUDIO_SOURCE_PCM16, 48000, DSD_ANALOG_AUDIO_LEARN);
    CHECK(y[0] == 29204 && y[1] >= -29204 && y[1] < -29000, "limited to %d %d", y[0], y[1]);
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

/* B2 (issue #518): hiss in a pause is not lifted. A talker over steady FM hiss 20 dB under it, the squelch open
   throughout (the default -110 dB squelch), then 3 s of the hiss alone: the gain at the end of the speech holds over
   the hiss, and the hiss plays no louder than at that gain. The old AGC released through the pause (3 to 20 dB/s, up
   to its +18 dB cap), lifting the hiss toward the speech. */
static void
test_hiss_in_pauses_not_lifted(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 1;
    const int rate = 48000;
    const size_t n_speech = secs(rate, 6.0);
    const size_t n_hiss = secs(rate, 3.0);
    float* x = alloc_f(n_speech + n_hiss);
    float* speech = talker(rate, n_speech, -30.0, 31U);
    DSD_MEMCPY(x, speech, n_speech * sizeof(float));
    add_noise(x, n_speech + n_hiss, rate, 32U, DSD_TEST_NOISE_DEEMPH, -50.0);
    float* hiss_in = alloc_f(n_hiss);
    DSD_MEMCPY(hiss_in, x + n_speech, n_hiss * sizeof(float));
    float gains[(9 * 48000) / 960];
    chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, x, n_speech + n_hiss, 960U, NULL, DSD_ANALOG_AUDIO_LEARN,
               gains);
    const size_t speech_blocks = n_speech / 960U;
    const size_t blocks = (n_speech + n_hiss) / 960U;
    const float at_speech_end = gains[speech_blocks - 1U];
    float most = at_speech_end;
    for (size_t b = speech_blocks; b < blocks; b++) {
        most = gains[b] > most ? gains[b] : most;
    }
    CHECK(most <= at_speech_end + 0.5f, "the hiss lifted the gain from %.2f to %.2f dB", (double)at_speech_end,
          (double)most);
    /* The hiss through the band-pass at the gain the speech left, against what played (the last second). */
    float* ref = alloc_f(n_hiss);
    DSD_MEMCPY(ref, hiss_in, n_hiss * sizeof(float));
    dsd_voice_bandpass bp;
    (void)dsd_voice_bandpass_design(&bp, DSD_VOICE_BAND_FM, rate);
    dsd_voice_bandpass_process(&bp, ref, n_hiss);
    const size_t last = secs(rate, 1.0);
    const double want = rms_dbfs(ref + n_hiss - last, last) + (double)at_speech_end;
    const double got = rms_dbfs(x + n_speech + n_hiss - last, last);
    CHECK(got <= want + 0.5, "the hiss played at %.2f dBFS, %.2f dB over the gain the speech left", got, got - want);
    free(ref);
    free(hiss_in);
    free(speech);
    free(x);
    fixture_free(&f);
}

/* @p n samples of open-squelch noise at @p rms_dbfs (FM hiss after de-emphasis, as noise_floor is). */
static float*
open_squelch_noise(int rate, size_t n, double rms_dbfs, uint64_t seed) {
    float* x = alloc_f(n);
    CHECK(dsd_test_synth_noise(x, n, rate, seed, DSD_TEST_NOISE_DEEMPH, rms_dbfs) == 0, "noise");
    return x;
}

/* B2 (issue #518): open-squelch noise is not normalised to speech level. 10 s of hot receiver noise (noise_floor reads
   -12.8 dBFS RMS at -n 50) on the default open squelch: no speech in it, so the AGC's attenuator eases it down to
   -30 dBFS or less. The old AGC brought it to its -10 dBFS peak target (-21 dBFS RMS), louder than speech. */
static void
test_open_squelch_noise_eased_down(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 1;
    const int rate = 48000;
    const size_t n = secs(rate, 10.0);
    float* x = open_squelch_noise(rate, n, -13.0, 41U);
    chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, x, n, 960U, NULL, DSD_ANALOG_AUDIO_LEARN, NULL);
    const size_t last = secs(rate, 2.0);
    const double got = rms_dbfs(x + n - last, last);
    CHECK(got <= -30.0, "open-squelch noise plays at %.2f dBFS RMS", got);
    CHECK(peak_of(x, n) <= 29204.0, "open-squelch noise peaked at %.1f", peak_of(x, n));
    free(x);
    fixture_free(&f);
}

/* B2 (issue #518): the transmission after open-squelch noise does not start low. A talker for 6 s, 10 s of hot noise on
   the open squelch, then the same talker again (the carrier quiets the noise): over its first second the second
   transmission gets within 2 dB of the gain the first one had established. The old AGC carried the noise's gain (-8 dB)
   into it, about 10 dB low for a second. */
static void
test_next_transmission_after_noise(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 1;
    const int rate = 48000;
    const size_t n_a = secs(rate, 6.0);
    const size_t n_noise = secs(rate, 10.0);
    const size_t n_b = secs(rate, 1.5);
    float* a = talker(rate, n_a, -30.0, 51U);
    float* a_in = alloc_f(n_a);
    DSD_MEMCPY(a_in, a, n_a * sizeof(float));
    chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, a, n_a, 960U, NULL, DSD_ANALOG_AUDIO_LEARN, NULL);
    const size_t est = secs(rate, 3.0);
    const double gain_a = dsd_test_p56_level_dbfs(a + n_a - est, est, rate, NULL)
                          - dsd_test_p56_level_dbfs(a_in + n_a - est, est, rate, NULL);
    float* noise = open_squelch_noise(rate, n_noise, -13.0, 52U);
    chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, noise, n_noise, 960U, NULL, DSD_ANALOG_AUDIO_LEARN, NULL);
    float* b = talker(rate, n_b, -30.0, 53U);
    float* b_in = alloc_f(n_b);
    DSD_MEMCPY(b_in, b, n_b * sizeof(float));
    chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, b, n_b, 960U, NULL, DSD_ANALOG_AUDIO_LEARN, NULL);
    const size_t first = secs(rate, 1.0);
    const double gain_b =
        dsd_test_p56_level_dbfs(b, first, rate, NULL) - dsd_test_p56_level_dbfs(b_in, first, rate, NULL);
    CHECK(gain_b >= gain_a - 2.0, "after the noise the next transmission played %.2f dB, the first %.2f dB", gain_b,
          gain_a);
    free(b_in);
    free(b);
    free(noise);
    free(a_in);
    free(a);
    fixture_free(&f);
}

/* At a squelch close the AGC undoes what the reception's tail taught (dsd_speech_agc_gate_closed()): a talker learned
   for 5 s, a 100 ms pause, then a 280 ms vowel 15 dB louder that pulls the gain down while the gate is still open,
   then the gate closes: the gain goes back to where it stood before the vowel, and the next transmission, 0.5 s later
   (one reception still), starts from there. */
static void
test_rollback_at_close(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 1;
    const int rate = 48000;
    clock_start(rate);
    const size_t n_a = secs(rate, 5.0);
    float* a = talker(rate, n_a, -30.0, 61U);
    chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, a, n_a, 960U, NULL, DSD_ANALOG_AUDIO_LEARN, NULL);
    float block[960];
    for (int b = 0; b < 5; b++) {
        DSD_MEMSET(block, 0, sizeof block);
        chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, block, 960U, 960U, NULL, DSD_ANALOG_AUDIO_LEARN, NULL);
    }
    const float before = f.state->aout_gainA;
    /* The tail: a natural vowel at -15 dBFS RMS, in 20 ms calls. */
    const size_t n_tail = secs(rate, 0.28);
    float* tail = alloc_f(n_tail);
    dsd_test_vowel_params v;
    dsd_test_vowel_defaults(&v, 62U);
    v.level_dbfs = -15.0;
    v.duration_s = 0.28;
    CHECK(dsd_test_synth_vowel(tail, n_tail, rate, &v) == 0, "vowel");
    float least = before;
    for (size_t at = 0; at < n_tail; at += 960U) {
        const size_t len = n_tail - at < 960U ? n_tail - at : 960U;
        chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, tail + at, len, 960U, NULL, DSD_ANALOG_AUDIO_LEARN, NULL);
        least = f.state->aout_gainA < least ? f.state->aout_gainA : least;
    }
    CHECK(least < before - 0.5f, "the tail did not pull the gain down (%.2f -> %.2f dB)", (double)before,
          (double)least);
    DSD_MEMSET(block, 0, sizeof block);
    chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, block, 960U, 960U, NULL, 0U, NULL);
    CHECK(fabsf(f.state->aout_gainA - before) < 0.05f, "after the close the gain is %.2f dB, before the tail %.2f dB",
          (double)f.state->aout_gainA, (double)before);
    for (int b = 0; b < 25; b++) {
        DSD_MEMSET(block, 0, sizeof block);
        chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, block, 960U, 960U, NULL, 0U, NULL);
    }
    DSD_MEMSET(block, 0, sizeof block);
    chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, block, 960U, 960U, NULL, DSD_ANALOG_AUDIO_LEARN, NULL);
    CHECK(fabsf(f.state->aout_gainA - before) < 0.05f, "the next transmission starts at %.2f dB, not %.2f dB",
          (double)f.state->aout_gainA, (double)before);
    free(tail);
    free(a);
    fixture_free(&f);
}

/* One run of the new-reception rule: a talker learned at @p rate, then @p closed_s of samples that do not learn
   (processed, with the clock frozen when @p fast: input read faster than real time), then @p pause_s with no samples at
   all (a source that stops sending), then samples that learn again, all in calls of @p block samples. Returns 1 when
   the AGC started over before them (back at the start gain), 0 when it kept its gain. */
static int
reception_restarts(int rate, double closed_s, double pause_s, size_t block, int fast) {
    fixture f = fixture_new();
    clock_start(rate);
    const size_t n_a = secs(rate, 4.0);
    float* a = talker(rate, n_a, -40.0, 71U);
    chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, a, n_a, block, NULL, DSD_ANALOG_AUDIO_LEARN, NULL);
    const float adapted = f.state->aout_gainA;
    CHECK(adapted > (float)k_fm_narrow_db + 8.0f, "%d Hz: adapted (%.2f dB)", rate, (double)adapted);
    const size_t n_closed = secs(rate, closed_s);
    float* closed = n_closed > 0U ? alloc_f(n_closed) : NULL;
    g_clock_frozen = fast;
    if (n_closed > 0U) {
        chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, closed, n_closed, block, NULL, 0U, NULL);
    }
    g_clock_frozen = 0;
    /* The close rolled back what the reception's last frames taught: kept means kept from here. */
    const float held = f.state->aout_gainA;
    clock_pause(pause_s);
    const size_t n_b = secs(rate, 0.1);
    float* b = alloc_f(n_b);
    chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, b, n_b, block, NULL, DSD_ANALOG_AUDIO_LEARN, NULL);
    const double got = (double)f.state->aout_gainA;
    int restarted = -1;
    if (fabs(got - k_fm_narrow_db) < 1e-3) {
        restarted = 1;
    } else if (fabs(got - (double)held) < 1e-3) {
        restarted = 0;
    }
    CHECK(restarted >= 0, "%d Hz: after the gap the gain is %.3f dB, neither kept (%.3f) nor the start gain", rate, got,
          (double)held);
    free(b);
    free(closed);
    free(a);
    fixture_free(&f);
    return restarted;
}

/* The new-reception rule (issue #518): a gap of a second or more between two samples that learn starts the AGC over,
   whether the samples in between were processed without learning (a closed squelch, a rejected tone) or none came at
   all (stdin, UDP or TCP input whose sender stops sending; the tap's pause detector), or both; a shorter one keeps
   what it learned. The times come from block-level decode-clock readings spread by sample position, and from the
   samples processed, so the outcome does not depend on how the samples are split into calls, nor on the rate: 48 kHz
   and 8 kHz (the lowest rate the AGC runs at), in 20 ms calls, odd sizes, random sizes and the 960-sample blocks that
   span 120 ms at 8 kHz. Input read faster than real time (the clock frozen while the closed samples go by) counts by
   its samples. */
static void
test_new_reception_after_a_second(void) {
    const int rates[] = {48000, 8000};
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++) {
        const int rate = rates[r];
        const size_t blocks[] = {(size_t)rate / 50U, 960U, 37U, 0U};
        for (size_t b = 0; b < sizeof blocks / sizeof blocks[0]; b++) {
            const size_t block = blocks[b];
            CHECK(reception_restarts(rate, 0.98, 0.0, block, 0) == 0, "%d Hz/%zu: 0.98 s closed restarted", rate,
                  block);
            CHECK(reception_restarts(rate, 1.02, 0.0, block, 0) == 1, "%d Hz/%zu: 1.02 s closed kept", rate, block);
            CHECK(reception_restarts(rate, 0.0, 0.98, block, 0) == 0, "%d Hz/%zu: a 0.98 s pause restarted", rate,
                  block);
            CHECK(reception_restarts(rate, 0.0, 1.02, block, 0) == 1, "%d Hz/%zu: a 1.02 s pause kept", rate, block);
            CHECK(reception_restarts(rate, 0.5, 0.52, block, 0) == 1, "%d Hz/%zu: 0.5 s closed + 0.52 s pause kept",
                  rate, block);
            CHECK(reception_restarts(rate, 0.5, 0.46, block, 0) == 0,
                  "%d Hz/%zu: 0.5 s closed + 0.46 s pause restarted", rate, block);
            CHECK(reception_restarts(rate, 1.02, 0.0, block, 1) == 1, "%d Hz/%zu: 1.02 s read fast kept", rate, block);
        }
    }
}

/* The new-reception rule with the closed run and the next learning samples in one buffer, a learn flag per sample, fed
   in calls of @p block samples (0: random sizes), so the call boundaries fall inside the closed run and around its end
   and a call can hold both. Returns the gain after it; the output goes to @p out (n_out samples). */
static float
joined_reception_gain(int rate, double closed_s, size_t block, float* out, size_t n_out) {
    fixture f = fixture_new();
    clock_start(rate);
    const size_t n_a = secs(rate, 4.0);
    const size_t n_closed = secs(rate, closed_s);
    const size_t n_b = secs(rate, 0.1);
    const size_t n = n_a + n_closed + n_b;
    float* x = alloc_f(n);
    float* a = talker(rate, n_a, -40.0, 71U);
    DSD_MEMCPY(x, a, n_a * sizeof(float));
    uint8_t* learn = (uint8_t*)calloc(n, 1U);
    if (!learn) {
        DSD_FPRINTF(stderr, "out of memory\n");
        exit(2);
    }
    for (size_t i = 0; i < n; i++) {
        learn[i] = (i < n_a || i >= n_a + n_closed) ? 1U : 0U;
    }
    g_block_rng = 777U;
    chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_PCM16, rate, x, n, block, learn, 0U, NULL);
    const float got = f.state->aout_gainA;
    DSD_MEMCPY(out, x, (n < n_out ? n : n_out) * sizeof(float));
    free(learn);
    free(a);
    free(x);
    fixture_free(&f);
    return got;
}

/* The same rule inside one buffer: a closure just under a second keeps the gain, one just over starts it over, and the
   output is the same, sample for sample, whether the buffer goes in 20 ms calls, 37-sample calls or random ones. */
static void
test_new_reception_inside_one_buffer(void) {
    const int rates[] = {48000, 8000};
    const double closures[] = {0.98, 1.02};
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++) {
        const int rate = rates[r];
        const size_t n_out = secs(rate, 5.2);
        float* want = alloc_f(n_out);
        float* got = alloc_f(n_out);
        for (size_t c = 0; c < sizeof closures / sizeof closures[0]; c++) {
            const float g = joined_reception_gain(rate, closures[c], (size_t)rate / 50U, want, n_out);
            const int restarted = fabs((double)g - k_fm_narrow_db) < 1e-3;
            CHECK(restarted == (closures[c] > 1.0), "%d Hz: %.2f s closed in one buffer: gain %.3f dB", rate,
                  closures[c], (double)g);
            CHECK(restarted || g > (float)k_fm_narrow_db + 8.0f, "%d Hz: %.2f s closed lost the gain (%.3f dB)", rate,
                  closures[c], (double)g);
            const size_t blocks[] = {37U, 0U};
            for (size_t b = 0; b < sizeof blocks / sizeof blocks[0]; b++) {
                const float gb = joined_reception_gain(rate, closures[c], blocks[b], got, n_out);
                int same = fabsf(gb - g) < 1e-4f;
                for (size_t i = 0; same && i < n_out; i++) {
                    same = fabsf(got[i] - want[i]) <= 1e-3f;
                }
                CHECK(same, "%d Hz: %.2f s closed in calls of %zu: not the 20 ms calls' output", rate, closures[c],
                      blocks[b]);
            }
        }
        free(got);
        free(want);
    }
}

/* Under 8 kHz (PCM input at 2.4-8 kHz runs the chain at its own rate) the speech AGC does not run: the chain plays the
   -n 50 gain on auto, the fixed one otherwise, through the -1 dBFS limiter, and publishes it. On auto it logs that once
   per rate; a fixed -n logs nothing. */
static void
test_unsupported_rate_plays_fixed_gain(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 0;
    dsd_test_capture_stderr cap;
    CHECK(dsd_test_capture_stderr_begin(&cap, "dsdneo_analog_audio_log") == 0, "capture stderr");
    float x[4] = {1000.0f, -1000.0f, 31000.0f, -31000.0f};
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, x, 4U,
                                     DSD_ANALOG_AUDIO_SOURCE_PCM16, 4000, DSD_ANALOG_AUDIO_LEARN);
    float again[2] = {0.0f, 0.0f};
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, again, 2U,
                                     DSD_ANALOG_AUDIO_SOURCE_PCM16, 4000, DSD_ANALOG_AUDIO_RESET);
    /* A fixed -n plays as asked at any rate, with nothing to say. */
    f.opts->audio_gainA = 100.0f;
    float y[2] = {1000.0f, -1000.0f};
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, y, 2U,
                                     DSD_ANALOG_AUDIO_SOURCE_PCM16, 6000, DSD_ANALOG_AUDIO_RESET);
    const float fixed_gain_db = f.state->aout_gainA;
    f.opts->audio_gainA = 0.0f;
    float z[2] = {0.0f, 0.0f};
    (void)dsd_analog_audio_process_f(f.opts, f.state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, z, 2U,
                                     DSD_ANALOG_AUDIO_SOURCE_PCM16, 6000, DSD_ANALOG_AUDIO_LEARN);
    (void)dsd_test_capture_stderr_end(&cap);
    char log[1024];
    CHECK(dsd_test_capture_stderr_read(&cap, log, sizeof log) == 0, "read the capture");
    CHECK(fabsf(x[0] - 1000.0f) < 1e-3f && fabsf(x[1] + 1000.0f) < 1e-3f, "auto at 4 kHz: %.3f %.3f", (double)x[0],
          (double)x[1]);
    CHECK(fabsf(x[2]) <= 29204.0f && fabsf(x[3]) <= 29204.0f, "auto at 4 kHz passed the ceiling: %.1f %.1f",
          (double)x[2], (double)x[3]);
    CHECK(fabsf(y[0] - 2000.0f) < 1e-3f && fabsf(y[1] + 2000.0f) < 1e-3f, "-n 100 at 6 kHz: %.3f %.3f", (double)y[0],
          (double)y[1]);
    CHECK(fabs((double)fixed_gain_db - 6.0206) < 1e-3, "-n 100 at 6 kHz publishes %.3f dB", (double)fixed_gain_db);
    /* Auto at 4 kHz says so once over two calls; the fixed gain at 6 kHz says nothing; auto at 6 kHz says so once. */
    const char* const line_4k = "Automatic analog gain inactive: 4000 Hz audio is below 8000 Hz; playing at the -n 50 "
                                "level\n";
    const char* const line_6k = "Automatic analog gain inactive: 6000 Hz audio is below 8000 Hz; playing at the -n 50 "
                                "level\n";
    const char* first = strstr(log, line_4k);
    CHECK(first != NULL && strstr(first + 1, line_4k) == NULL, "the 4 kHz line not logged exactly once: [%s]", log);
    const char* six = strstr(log, line_6k);
    CHECK(six != NULL && strstr(six + 1, line_6k) == NULL && strlen(log) == strlen(line_4k) + strlen(line_6k),
          "the log holds other than one line per rate: [%s]", log);
    fixture_free(&f);
}

/* A fixed -n keeps the limiter: hot open-squelch noise at -n 50 with the FM de-emphasis made up at 750 us (+13.65 dB,
   DSD_NEO_DEEMPH=nfm) never reaches full scale, where the old fixed gain clipped it. */
static void
test_fixed_gain_keeps_the_limiter(void) {
    fixture f = fixture_new();
    f.opts->use_pbf = 1;
    f.opts->audio_gainA = 50.0f;
    dsd_rtl_stream_metrics_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.stream_generation = fake_rtl_generation;
    hooks.monitor_deemph_gain = fake_deemph_gain;
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    f.opts->audio_in_type = AUDIO_IN_RTL;
    g_fake_deemph_gain = 4.810934;
    const int rate = 48000;
    const size_t n = secs(rate, 2.0);
    float* x = open_squelch_noise(rate, n, -13.0, 81U);
    /* At the RTL monitor's scale: -13 dBFS at -n 50 before the de-emphasis is made up, +0.65 dBFS after. */
    for (size_t i = 0; i < n; i++) {
        x[i] = (float)((double)x[i] / 32924.0);
    }
    chain_feed(&f, DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, rate, x, n, 960U, NULL, DSD_ANALOG_AUDIO_LEARN, NULL);
    CHECK(peak_of(x, n) <= 29204.0, "fixed -n 50 noise peaked at %.1f", peak_of(x, n));
    free(x);
    g_fake_deemph_gain = 1.0;
    dsd_rtl_stream_metrics_hooks_set(NULL);
    fixture_free(&f);
}

int
main(void) {
    test_source_gains();
    test_int16_scale();
    test_fixed_gain();
    test_fixed_gain_through_bandpass();
    test_auto_tone_turned_away();
    test_auto_levels_speech();
    test_not_learning_holds();
    test_start_gains();
    test_resets();
    test_chains_are_independent();
    test_new_reception_starts_over();
    test_deemph_gain();
    test_edacs_triplet_across_a_boundary();
    test_discard_starts_over_after();
    test_legacy_filters_start_over_with_the_chain();
    test_band_follows_the_monitor();
    test_legacy_filter_flags();
    test_short_rounds_and_limits();
    test_hiss_in_pauses_not_lifted();
    test_open_squelch_noise_eased_down();
    test_next_transmission_after_noise();
    test_rollback_at_close();
    test_new_reception_after_a_second();
    test_new_reception_inside_one_buffer();
    test_unsupported_rate_plays_fixed_gain();
    test_fixed_gain_keeps_the_limiter();
    test_slot_lifetime_and_args();
    if (g_failures) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("DSP_ANALOG_AUDIO: OK\n");
    return 0;
}
