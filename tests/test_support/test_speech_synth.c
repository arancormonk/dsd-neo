// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The test signal generators (test_support/speech_synth.h): one seed gives the same samples every time, the
 * continuous-time models give one waveform at 8, 22.05 and 48 kHz, speech reads its nominal P.56 level and follows its
 * talker-level steps exactly, its pauses are real, vowels and steady signals carry the level they are asked for, and
 * bad arguments are refused.
 */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p56.h"
#include "speech_synth.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Every buffer here holds at least one sample: the allocation is exactly @p n, which the callers index up to. */
static float*
alloc_samples(size_t n) {
    assert(n > 0);
    float* x = (float*)calloc(n, sizeof(float));
    assert(x != NULL);
    return x;
}

static size_t
samples(double seconds, int rate_hz) {
    return (size_t)lround(seconds * (double)rate_hz);
}

static double
rms_dbfs(const float* x, size_t n) {
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        sum += (double)x[i] * (double)x[i];
    }
    return 10.0 * log10(sum / (double)n / (32768.0 * 32768.0));
}

static int
same_samples(const float* a, const float* b, size_t n) {
    return memcmp(a, b, n * sizeof(float)) == 0;
}

/* --------------------------------------------------------------------------------------------- determinism */

static void
test_same_seed_same_samples(void) {
    const int rate = 22050;
    const size_t n = samples(3.0, rate);
    float* a = alloc_samples(n);
    float* b = alloc_samples(n);

    dsd_test_speech_params sp;
    dsd_test_speech_defaults(&sp, 3U);
    assert(dsd_test_synth_speech(a, n, rate, &sp) == 0);
    assert(dsd_test_synth_speech(b, n, rate, &sp) == 0);
    assert(same_samples(a, b, n));
    sp.seed = 4U;
    assert(dsd_test_synth_speech(b, n, rate, &sp) == 0);
    assert(!same_samples(a, b, n));

    dsd_test_vowel_params vp;
    dsd_test_vowel_defaults(&vp, 3U);
    assert(dsd_test_synth_vowel(a, n, rate, &vp) == 0);
    assert(dsd_test_synth_vowel(b, n, rate, &vp) == 0);
    assert(same_samples(a, b, n));
    vp.seed = 4U;
    assert(dsd_test_synth_vowel(b, n, rate, &vp) == 0);
    assert(!same_samples(a, b, n));

    for (int kind = DSD_TEST_NOISE_WHITE; kind <= DSD_TEST_NOISE_RUMBLE; kind++) {
        assert(dsd_test_synth_noise(a, n, rate, 9U, (dsd_test_noise_kind)kind, -40.0) == 0);
        assert(dsd_test_synth_noise(b, n, rate, 9U, (dsd_test_noise_kind)kind, -40.0) == 0);
        assert(same_samples(a, b, n));
        assert(dsd_test_synth_noise(b, n, rate, 10U, (dsd_test_noise_kind)kind, -40.0) == 0);
        assert(!same_samples(a, b, n));
    }
    free(a);
    free(b);
}

/* --------------------------------------------------------------------------------------------- one waveform */

enum { SIG_SPEECH, SIG_VOWEL, SIG_DTMF, SIG_HETERODYNE, SIG_BUZZ, SIG_COUNT };

static const dsd_test_level_step k_steps[2] = {{1.5, -20.0}, {3.0, -36.0}};

static void
make_signal(int sig, float* x, size_t n, int rate_hz) {
    dsd_test_speech_params sp;
    dsd_test_vowel_params vp;
    switch (sig) {
        case SIG_SPEECH:
            dsd_test_speech_defaults(&sp, 21U);
            sp.start_s = 0.75;
            sp.steps = k_steps;
            sp.step_count = 2;
            assert(dsd_test_synth_speech(x, n, rate_hz, &sp) == 0);
            break;
        case SIG_VOWEL:
            dsd_test_vowel_defaults(&vp, 21U);
            vp.f0_hz = 70.0;
            assert(dsd_test_synth_vowel(x, n, rate_hz, &vp) == 0);
            break;
        case SIG_DTMF: assert(dsd_test_synth_dtmf(x, n, rate_hz, '#', -20.0) == 0); break;
        case SIG_HETERODYNE: assert(dsd_test_synth_heterodyne(x, n, rate_hz, 980.0, 15.0, -30.0) == 0); break;
        default: assert(dsd_test_synth_mains(x, n, rate_hz, DSD_TEST_MAINS_BUZZ, 60.0, -30.0) == 0); break;
    }
}

/* Where sample times coincide (every 8 kHz sample is every 6th 48 kHz one; every 147th 22.05 kHz sample is every 320th
   48 kHz one) the rates carry the same value. */
static void
test_rates_share_one_waveform(void) {
    const size_t n48 = samples(5.0, 48000);
    const size_t n8 = samples(5.0, 8000);
    const size_t n22 = samples(5.0, 22050);
    float* x48 = alloc_samples(n48);
    float* x8 = alloc_samples(n8);
    float* x22 = alloc_samples(n22);
    for (int sig = 0; sig < SIG_COUNT; sig++) {
        make_signal(sig, x48, n48, 48000);
        make_signal(sig, x8, n8, 8000);
        make_signal(sig, x22, n22, 22050);
        double worst = 0.0;
        for (size_t m = 0; m < n8 && 6 * m < n48; m++) {
            worst = fmax(worst, fabs((double)x8[m] - (double)x48[6 * m]));
        }
        for (size_t m = 0; 147 * m < n22 && 320 * m < n48; m++) {
            worst = fmax(worst, fabs((double)x22[147 * m] - (double)x48[320 * m]));
        }
        if (worst > 1e-3) {
            DSD_FPRINTF(stderr, "signal %d: rates differ by %.6f int16 units\n", sig, worst);
            assert(0);
        }
        assert(rms_dbfs(x48, n48) > -60.0);
    }
    free(x48);
    free(x8);
    free(x22);
}

/* A later start time is the same signal, entered part way. */
static void
test_start_inside_speech(void) {
    const int rate = 8000;
    const size_t skip = samples(2.5, rate);
    const size_t n = samples(4.0, rate);
    const size_t total = skip + n;
    float* whole = alloc_samples(total);
    float* late = alloc_samples(n);
    dsd_test_speech_params sp;
    dsd_test_speech_defaults(&sp, 8U);
    assert(dsd_test_synth_speech(whole, total, rate, &sp) == 0);
    sp.start_s = 2.5;
    assert(dsd_test_synth_speech(late, n, rate, &sp) == 0);
    for (size_t m = 0; m < n && skip + m < total; m++) {
        assert(fabs((double)late[m] - (double)whole[skip + m]) < 1e-3);
    }
    free(whole);
    free(late);
}

/* ------------------------------------------------------------------------------------------------ speech level */

/* A set of talkers (two pitches, eight seeds, 20 s each) reads within 1 dB of the nominal level, and centres on it. */
static void
test_talker_set_level(void) {
    static const double k_pitches[2] = {105.0, 215.0};
    const int rate = 8000;
    const size_t n = samples(20.0, rate);
    float* x = alloc_samples(n);
    double sum = 0.0;
    int count = 0;
    for (int f = 0; f < 2; f++) {
        for (uint64_t seed = 1U; seed <= 8U; seed++) {
            dsd_test_speech_params sp;
            dsd_test_speech_defaults(&sp, seed);
            sp.f0_hz = k_pitches[f];
            sp.level_dbfs = -26.0;
            assert(dsd_test_synth_speech(x, n, rate, &sp) == 0);
            double act = 0.0;
            const double level = dsd_test_p56_level_dbfs(x, n, rate, &act);
            if (fabs(level - sp.level_dbfs) > 1.0 || act < 0.6 || act > 0.95) {
                DSD_FPRINTF(stderr, "talker %.0f Hz seed %u: level %.3f, activity %.3f\n", k_pitches[f], (unsigned)seed,
                            level, act);
                assert(0);
            }
            sum += level;
            count++;
        }
    }
    assert(fabs((sum / (double)count) - (-26.0)) < 0.3);
    free(x);
}

/* Talker-level steps: between the steps the stepped signal is the unstepped one scaled by exactly the step, and each
   level reads near its nominal value. Each window opens 0.5 s after its step, past the last syllable that started
   under the previous level. */
static void
test_talker_level_steps(void) {
    static const dsd_test_level_step k_talkers[2] = {{10.0, -18.0}, {20.0, -38.0}};
    static const double k_window[3][3] = {{0.5, 10.0, -30.0}, {10.5, 20.0, -18.0}, {20.5, 30.0, -38.0}};
    const int rates[2] = {8000, 48000};
    for (int r = 0; r < 2; r++) {
        const int rate = rates[r];
        const size_t n = samples(30.0, rate);
        float* flat = alloc_samples(n);
        float* stepped = alloc_samples(n);
        dsd_test_speech_params sp;
        dsd_test_speech_defaults(&sp, 31U);
        sp.level_dbfs = -30.0;
        assert(dsd_test_synth_speech(flat, n, rate, &sp) == 0);
        sp.steps = k_talkers;
        sp.step_count = 2;
        assert(dsd_test_synth_speech(stepped, n, rate, &sp) == 0);
        for (int w = 0; w < 3; w++) {
            const size_t i0 = samples(k_window[w][0], rate);
            const size_t len = samples(k_window[w][1], rate) - i0;
            const double ref = dsd_test_p56_level_dbfs(flat + i0, len, rate, NULL);
            const double level = dsd_test_p56_level_dbfs(stepped + i0, len, rate, NULL);
            const double want = k_window[w][2];
            if (fabs((level - ref) - (want - sp.level_dbfs)) > 0.05 || fabs(level - want) > 1.5) {
                DSD_FPRINTF(stderr, "step window %d at %d Hz: level %.3f (flat %.3f), nominal %.1f\n", w, rate, level,
                            ref, want);
                assert(0);
            }
        }
        free(flat);
        free(stepped);
    }
}

/* Pauses between phrases are exact zeros, long enough that P.56 does not bridge them. */
static void
test_real_pauses(void) {
    const int rate = 8000;
    const size_t n = samples(12.0, rate);
    float* x = alloc_samples(n);
    dsd_test_speech_params sp;
    dsd_test_speech_defaults(&sp, 2U);
    assert(dsd_test_synth_speech(x, n, rate, &sp) == 0);
    const size_t long_run = samples(0.4, rate);
    size_t run = 0;
    int pauses = 0;
    for (size_t i = 0; i < n; i++) {
        if (x[i] == 0.0f) {
            run++;
            pauses += run == long_run ? 1 : 0;
        } else {
            run = 0;
        }
    }
    assert(pauses >= 3);
    free(x);
}

/* --------------------------------------------------------------------------------------------------- vowels */

static void
test_vowels(void) {
    const int rate = 8000;
    const size_t n = samples(1.2, rate);
    const size_t edge = samples(0.03, rate);
    const size_t len = samples(1.0, rate);
    float* steady = alloc_samples(n);
    float* natural = alloc_samples(n);
    dsd_test_vowel_params vp;
    dsd_test_vowel_defaults(&vp, 5U);
    vp.f0_hz = 125.0; /* 64 samples a period at 8 kHz */
    vp.duration_s = 1.0;
    assert(dsd_test_synth_vowel(natural, n, rate, &vp) == 0);
    vp.steady = 1;
    assert(dsd_test_synth_vowel(steady, n, rate, &vp) == 0);

    /* The level is the RMS between the edges; shimmer moves a natural vowel's a little. */
    assert(fabs(rms_dbfs(steady + edge, len - (2 * edge)) - vp.level_dbfs) < 0.02);
    assert(fabs(rms_dbfs(natural + edge, len - (2 * edge)) - vp.level_dbfs) < 0.5);

    /* A steady vowel repeats exactly every period; jitter and vibrato keep a natural one from doing so. */
    const double amp = 32768.0 * pow(10.0, vp.level_dbfs / 20.0);
    double steady_diff = 0.0;
    double natural_diff = 0.0;
    for (size_t i = edge; i + 64 < len - edge; i++) {
        steady_diff = fmax(steady_diff, fabs((double)steady[i + 64] - (double)steady[i]));
        natural_diff = fmax(natural_diff, fabs((double)natural[i + 64] - (double)natural[i]));
    }
    assert(steady_diff < 1e-3 * amp);
    assert(natural_diff > 0.2 * amp);

    for (size_t i = len; i < n; i++) {
        assert(steady[i] == 0.0f && natural[i] == 0.0f);
    }
    free(steady);
    free(natural);
}

/* --------------------------------------------------------------------------------------------- steady signals */

static void
test_steady_signal_levels(void) {
    static const char k_digits[] = "0123456789*#ABCD";
    const int rate = 48000;
    const size_t n = samples(2.0, rate);
    float* x = alloc_samples(n);

    assert(dsd_test_synth_tone(x, n, rate, 1000.0, -18.0) == 0);
    assert(fabs(rms_dbfs(x, n) - (-18.0)) < 0.01);
    for (int d = 0; k_digits[d] != '\0'; d++) {
        assert(dsd_test_synth_dtmf(x, n, rate, k_digits[d], -20.0) == 0);
        assert(fabs(rms_dbfs(x, n) - (-20.0)) < 0.05);
    }
    assert(dsd_test_synth_heterodyne(x, n, rate, 1500.0, -40.0, -30.0) == 0);
    assert(fabs(rms_dbfs(x, n) - (-30.0)) < 0.05);
    assert(dsd_test_synth_mains(x, n, rate, DSD_TEST_MAINS_HUM, 50.0, -35.0) == 0);
    assert(fabs(rms_dbfs(x, n) - (-35.0)) < 0.05);
    assert(dsd_test_synth_mains(x, n, rate, DSD_TEST_MAINS_BUZZ, 60.0, -35.0) == 0);
    assert(fabs(rms_dbfs(x, n) - (-35.0)) < 0.05);
    for (int kind = DSD_TEST_NOISE_WHITE; kind <= DSD_TEST_NOISE_RUMBLE; kind++) {
        assert(dsd_test_synth_noise(x, n, rate, 77U, (dsd_test_noise_kind)kind, -42.0) == 0);
        assert(fabs(rms_dbfs(x, n) - (-42.0)) < 0.001);
    }
    free(x);
}

/* The de-emphasis is the receiver's one-pole: its first output is (1 - a) of a step, it settles to the step, and a 1 kHz
   sine in steady state comes out at the analytic gain (750 us at 48 kHz: -13.65 dB). */
static void
test_deemphasis(void) {
    const int rate = 48000;
    const size_t n = samples(1.0, rate);
    float* x = alloc_samples(n);
    for (size_t i = 0; i < n; i++) {
        x[i] = 1.0f;
    }
    const double a = exp(-1.0 / (48000.0 * 750e-6));
    assert(dsd_test_deemphasis(x, n, rate, 750e-6) == 0);
    assert(fabs((double)x[0] - (1.0 - a)) < 1e-7 && fabs((double)x[n - 1] - 1.0) < 1e-6);
    for (size_t i = 0; i < n; i++) {
        x[i] = (float)sin(2.0 * M_PI * 1000.0 * (double)i / (double)rate);
    }
    assert(dsd_test_deemphasis(x, n, rate, 750e-6) == 0);
    const size_t half = n / 2;
    const double w = 2.0 * M_PI * 1000.0 / (double)rate;
    const double want = 20.0 * log10((1.0 - a) / sqrt(1.0 - (2.0 * a * cos(w)) + (a * a)));
    /* A unit sine reads 20 log10(sqrt(0.5) / 32768) dBFS. */
    const double got = rms_dbfs(x + half, n - half) - (20.0 * log10(sqrt(0.5) / 32768.0));
    assert(fabs(want + 13.65) < 0.01 && fabs(got - want) < 0.01);
    assert(dsd_test_deemphasis(NULL, n, rate, 750e-6) == -1);
    assert(dsd_test_deemphasis(x, n, 7999, 750e-6) == -1);
    assert(dsd_test_deemphasis(x, n, rate, 0.0) == -1);
    assert(dsd_test_deemphasis(x, n, rate, NAN) == -1);
    free(x);
}

static void
test_bad_arguments(void) {
    float* x = alloc_samples(16000);
    dsd_test_speech_params sp;
    dsd_test_vowel_params vp;
    dsd_test_speech_defaults(&sp, 1U);
    dsd_test_vowel_defaults(&vp, 1U);
    assert(dsd_test_synth_speech(NULL, 16, 8000, &sp) == -1);
    assert(dsd_test_synth_speech(x, 16, 7999, &sp) == -1);
    assert(dsd_test_synth_speech(x, 16, 8000, NULL) == -1);
    sp.f0_hz = 40.0;
    assert(dsd_test_synth_speech(x, 16, 8000, &sp) == -1);
    sp.f0_hz = 120.0;
    sp.jitter = 0.5;
    assert(dsd_test_synth_speech(x, 16, 8000, &sp) == -1);
    sp.jitter = 0.01;
    sp.step_count = 1;
    assert(dsd_test_synth_speech(x, 16, 8000, &sp) == -1);
    vp.duration_s = 0.01;
    assert(dsd_test_synth_vowel(x, 16, 8000, &vp) == -1);
    assert(dsd_test_synth_tone(x, 16, 8000, 4000.0, -20.0) == -1);
    assert(dsd_test_synth_dtmf(x, 16, 8000, 'X', -20.0) == -1);
    assert(dsd_test_synth_heterodyne(x, 16000, 8000, 3000.0, 1000.0, -20.0) == -1);
    assert(dsd_test_synth_mains(x, 16, 8000, DSD_TEST_MAINS_HUM, 100.0, -20.0) == -1);
    free(x);
}

int
main(void) {
    test_same_seed_same_samples();
    test_rates_share_one_waveform();
    test_start_inside_speech();
    test_talker_set_level();
    test_talker_level_steps();
    test_real_pauses();
    test_vowels();
    test_steady_signal_levels();
    test_deemphasis();
    test_bad_arguments();
    printf("TEST_SUPPORT_SPEECH_SYNTH: OK\n");
    return 0;
}
