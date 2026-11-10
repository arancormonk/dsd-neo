// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The test P.56 meter (test_support/p56.h) against what the method must read: sines 3.01 dB under their peak at every
 * rate, levels that follow amplitude scaling, the activity of on/off bursts (on time plus the 200 ms hangover and the
 * envelope's fall), short gaps bridged and longer ones not, one reading of one waveform at 8, 22.05 and 48 kHz, and the
 * silent, unmeasurable and invalid cases.
 */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "p56.h"
#include "speech_synth.h"

/* After a burst the meter stays active for the 200 ms hangover plus the time the two 30 ms envelopes take to fall to
   the threshold it settles on (about 14 dB under the burst's mean magnitude: some 90 ms), less the time they take to
   rise to it at the onset (some 20 ms). */
#define BURST_TAIL_S 0.275

static float*
alloc_samples(size_t n) {
    float* x = (float*)calloc(n > 0 ? n : 1, sizeof(float));
    assert(x != NULL);
    return x;
}

static size_t
samples(double seconds, int rate_hz) {
    return (size_t)lround(seconds * (double)rate_hz);
}

static void
test_sine_reads_rms(void) {
    static const int k_rates[3] = {8000, 22050, 48000};
    static const double k_peaks[3] = {-10.0, -30.0, -50.0};
    for (int r = 0; r < 3; r++) {
        const size_t n = samples(4.0, k_rates[r]);
        float* x = alloc_samples(n);
        for (int p = 0; p < 3; p++) {
            const double rms = k_peaks[p] - (20.0 * log10(sqrt(2.0)));
            assert(dsd_test_synth_tone(x, n, k_rates[r], 1000.0, rms) == 0);
            double act = 0.0;
            const double level = dsd_test_p56_level_dbfs(x, n, k_rates[r], &act);
            if (fabs(level - (k_peaks[p] - 3.0103)) > 0.05) {
                DSD_FPRINTF(stderr, "sine peak %.0f dBFS at %d Hz reads %.4f\n", k_peaks[p], k_rates[r], level);
                assert(0);
            }
            assert(act > 0.99 && act <= 1.0);
        }
        free(x);
    }
}

static void
test_scaling_tracks_level(void) {
    static const int k_rates[2] = {8000, 48000};
    static const double k_steps_db[7] = {-40.0, -30.0, -20.0, -10.0, -3.0, 3.0, 10.0};
    for (int r = 0; r < 2; r++) {
        const size_t n = samples(12.0, k_rates[r]);
        float* x = alloc_samples(n);
        float* y = alloc_samples(n);
        dsd_test_speech_params sp;
        dsd_test_speech_defaults(&sp, 11U);
        sp.level_dbfs = -24.0;
        assert(dsd_test_synth_speech(x, n, k_rates[r], &sp) == 0);
        double act0 = 0.0;
        const double base = dsd_test_p56_level_dbfs(x, n, k_rates[r], &act0);
        for (int s = 0; s < 7; s++) {
            const double k = pow(10.0, k_steps_db[s] / 20.0);
            for (size_t i = 0; i < n; i++) {
                y[i] = (float)((double)x[i] * k);
            }
            double act = 0.0;
            const double level = dsd_test_p56_level_dbfs(y, n, k_rates[r], &act);
            if (fabs((level - base) - k_steps_db[s]) > 0.02 || fabs(act - act0) > 0.005) {
                DSD_FPRINTF(stderr, "scaled %+.0f dB at %d Hz: level moved %+.4f, activity %.4f -> %.4f\n",
                            k_steps_db[s], k_rates[r], level - base, act0, act);
                assert(0);
            }
        }
        free(x);
        free(y);
    }
}

/* @p count bursts of @p on_s seconds at -26 dBFS RMS (voice-band noise, or a 500 Hz sine), each followed by @p off_s
   of exact zeros. Returns the meter's activity, and checks that its level is the bursts' RMS over that activity. */
static double
burst_activity(int rate_hz, int sine, double on_s, double off_s, int count) {
    const size_t on = samples(on_s, rate_hz);
    const size_t period = on + samples(off_s, rate_hz);
    const size_t n = period * (size_t)count;
    float* x = alloc_samples(n);
    for (int b = 0; b < count; b++) {
        float* burst = x + (period * (size_t)b);
        if (sine) {
            assert(dsd_test_synth_tone(burst, on, rate_hz, 500.0, -26.0) == 0);
        } else {
            assert(dsd_test_synth_noise(burst, on, rate_hz, 100U + (uint64_t)b, DSD_TEST_NOISE_VOICE_BAND, -26.0) == 0);
        }
    }
    double act = 0.0;
    const double level = dsd_test_p56_level_dbfs(x, n, rate_hz, &act);
    const double on_fraction = on_s / (on_s + off_s);
    assert(act > 0.0 && act <= 1.0);
    assert(fabs(level - (-26.0 + (10.0 * log10(on_fraction / act)))) < 0.02);
    free(x);
    return act;
}

static void
test_burst_activity(void) {
    static const int k_rates[2] = {8000, 48000};
    static const double k_on_off[4][2] = {{1.0, 1.0}, {0.5, 1.5}, {2.0, 0.5}, {0.3, 3.0}};
    for (int r = 0; r < 2; r++) {
        for (int sine = 0; sine <= 1; sine++) {
            for (int c = 0; c < 4; c++) {
                const double on = k_on_off[c][0];
                const double off = k_on_off[c][1];
                const double act = burst_activity(k_rates[r], sine, on, off, 8);
                const double want = (on + BURST_TAIL_S) / (on + off);
                if (fabs(act - want) > 0.03) {
                    DSD_FPRINTF(stderr, "%s bursts %.1f/%.1f s at %d Hz: activity %.4f, model %.4f\n",
                                sine ? "sine" : "noise", on, off, k_rates[r], act, want);
                    assert(0);
                }
            }
        }
    }
}

/* 100 ms bursts: gaps up to 0.28 s are bridged; past about 0.3 s each burst stays active for its hangover and envelope
   tail only, which ends before 0.33 s. */
static void
test_gap_bridging(void) {
    static const int k_rates[2] = {8000, 48000};
    static const double k_bridged[4] = {0.1, 0.2, 0.25, 0.28};
    static const double k_open[4] = {0.34, 0.4, 0.5, 1.0};
    for (int r = 0; r < 2; r++) {
        for (int sine = 0; sine <= 1; sine++) {
            for (int g = 0; g < 4; g++) {
                const double act = burst_activity(k_rates[r], sine, 0.1, k_bridged[g], 20);
                if (act < 0.98) {
                    DSD_FPRINTF(stderr, "gap %.2f s at %d Hz not bridged: activity %.4f\n", k_bridged[g], k_rates[r],
                                act);
                    assert(0);
                }
            }
            for (int g = 0; g < 4; g++) {
                const double act = burst_activity(k_rates[r], sine, 0.1, k_open[g], 20);
                const double tail = (act * (0.1 + k_open[g])) - 0.1;
                if (tail < 0.25 || tail > 0.33) {
                    DSD_FPRINTF(stderr, "gap %.2f s at %d Hz: active %.3f s after each burst\n", k_open[g], k_rates[r],
                                tail);
                    assert(0);
                }
            }
        }
    }
}

static void
test_rates_read_one_waveform(void) {
    static const int k_rates[3] = {8000, 22050, 48000};
    double levels[3];
    double acts[3];
    for (int r = 0; r < 3; r++) {
        const size_t n = samples(12.0, k_rates[r]);
        float* x = alloc_samples(n);
        dsd_test_speech_params sp;
        dsd_test_speech_defaults(&sp, 5U);
        sp.f0_hz = 190.0;
        assert(dsd_test_synth_speech(x, n, k_rates[r], &sp) == 0);
        levels[r] = dsd_test_p56_level_dbfs(x, n, k_rates[r], &acts[r]);
        free(x);
    }
    for (int r = 1; r < 3; r++) {
        assert(fabs(levels[r] - levels[0]) < 0.02);
        assert(fabs(acts[r] - acts[0]) < 0.002);
    }
}

static void
test_silence_and_invalid(void) {
    const int rate = 8000;
    const size_t n = samples(1.0, rate);
    float* x = alloc_samples(n);
    double act = -1.0;

    /* Exact zeros, and a signal whose envelope never reaches 1 int16 unit: silence. */
    const double zeros = dsd_test_p56_level_dbfs(x, n, rate, &act);
    assert(isinf(zeros) && zeros < 0.0 && fabs(act) < 1e-12);
    assert(dsd_test_synth_tone(x, n, rate, 1000.0, -100.0) == 0);
    assert(isinf(dsd_test_p56_level_dbfs(x, n, rate, &act)) && fabs(act) < 1e-12);
    assert(isinf(dsd_test_p56_level_dbfs(x, 0, rate, NULL)));

    /* An isolated impulse: its power is far above every threshold its envelope reaches. */
    for (size_t i = 0; i < n; i++) {
        x[i] = 0.0f;
    }
    x[n / 2] = 1e7f;
    assert(isnan(dsd_test_p56_level_dbfs(x, n, rate, &act)) && isnan(act));

    assert(isnan(dsd_test_p56_level_dbfs(NULL, n, rate, &act)) && isnan(act));
    assert(isnan(dsd_test_p56_level_dbfs(x, n, 0, NULL)));
    free(x);
}

int
main(void) {
    test_sine_reads_rms();
    test_scaling_tracks_level();
    test_burst_activity();
    test_gap_bridging();
    test_rates_read_one_waveform();
    test_silence_and_invalid();
    printf("TEST_SUPPORT_P56: OK\n");
    return 0;
}
