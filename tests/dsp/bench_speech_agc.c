// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Opt-in micro-benchmark of the speech level AGC (<dsd-neo/dsp/speech_agc.h>); not a test.
 *
 * Times dsd_speech_agc_process() on synthetic speech (talkers with real pauses over a -50 dBFS hiss) at 8, 22.05 and
 * 48 kHz, and on open-squelch hiss at 48 kHz, which runs the pitch cue on every frame; at 48 kHz also with the
 * coloured-noise veto the analog chain runs. Each case runs whole frames per call and single samples per call, from a
 * reset each repeat, and prints the fastest and the median ns per sample.
 * Build and run it as docs/dsp-benchmarking.md does the others:
 *   cmake --preset perf-bench
 *   cmake --build --preset perf-bench --target dsd-neo_bench_speech_agc
 *   build/perf-bench/tests/dsd-neo_bench_speech_agc [--seconds S] [--repeat N]
 */

#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/speech_agc.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "speech_synth.h"

#define BENCH_REPEAT_MAX 64

typedef struct {
    int rate_hz;
    int hiss; /* open-squelch hiss alone, else speech */
    int veto; /* the coloured-noise veto, as the analog chain runs it */
} bench_case;

static const bench_case k_cases[] = {{8000, 0, 0},  {22050, 0, 0}, {48000, 0, 0},
                                     {48000, 0, 1}, {48000, 1, 0}, {48000, 1, 1}};

static int
bench_fill(float* x, size_t n, int rate_hz, int hiss) {
    if (hiss) {
        return dsd_test_synth_noise(x, n, rate_hz, 2, DSD_TEST_NOISE_VOICE_BAND, -30.0);
    }
    const size_t seg = (size_t)rate_hz * 10U;
    for (size_t at = 0, t = 0; at < n; at += seg, t++) {
        dsd_test_speech_params p;
        dsd_test_speech_defaults(&p, 10U + t);
        p.level_dbfs = -36.0 + 4.0 * (double)(t % 4U);
        p.f0_hz = 100.0 + 40.0 * (double)(t % 5U);
        if (dsd_test_synth_speech(x + at, at + seg <= n ? seg : n - at, rate_hz, &p) != 0) {
            return -1;
        }
    }
    float* w = (float*)malloc(n * sizeof(float));
    if (w == NULL || dsd_test_synth_noise(w, n, rate_hz, 3, DSD_TEST_NOISE_VOICE_BAND, -50.0) != 0) {
        free(w);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        x[i] += w[i];
    }
    free(w);
    return 0;
}

static int
cmp_double(const void* a, const void* b) {
    const double x = *(const double*)a;
    const double y = *(const double*)b;
    return (x > y) - (x < y);
}

/* ns per sample of one pass over @p x, @p chunk samples per call. */
static double
bench_pass(dsd_speech_agc* agc, const float* x, float* w, size_t n, size_t chunk) {
    DSD_MEMCPY(w, x, n * sizeof(float));
    dsd_speech_agc_reset(agc);
    const uint64_t t0 = dsd_realtime_mono_ns();
    for (size_t p = 0; p < n; p += chunk) {
        dsd_speech_agc_process(agc, w + p, p + chunk <= n ? chunk : n - p, 1, 1, 0);
    }
    const uint64_t t1 = dsd_realtime_mono_ns();
    return (double)(t1 - t0) / (double)n;
}

static int
bench_run(const bench_case* c, int seconds, int repeat) {
    const size_t n = (size_t)c->rate_hz * (size_t)seconds;
    float* x = (float*)malloc(n * sizeof(float));
    float* w = (float*)malloc(n * sizeof(float));
    dsd_speech_agc* agc = (dsd_speech_agc*)calloc(1U, sizeof *agc);
    int rc = -1;
    if (x != NULL && w != NULL && agc != NULL && bench_fill(x, n, c->rate_hz, c->hiss) == 0
        && dsd_speech_agc_init(agc, c->rate_hz, 0.0f, DSD_SPEECH_AGC_TARGET_DBFS, DSD_SPEECH_AGC_CEILING_DBFS) == 0) {
        dsd_speech_agc_set_noise_veto(agc, c->veto);
        const size_t chunks[2] = {(size_t)agc->frame_len, 1U};
        for (size_t k = 0; k < 2; k++) {
            double ns[BENCH_REPEAT_MAX];
            for (int r = 0; r < repeat; r++) {
                ns[r] = bench_pass(agc, x, w, n, chunks[k]);
            }
            qsort(ns, (size_t)repeat, sizeof ns[0], cmp_double);
            DSD_FPRINTF(stdout, "speech_agc %5d Hz %-6s %-4s %4zu samples/call: best %6.2f ns/sample, median %6.2f\n",
                        c->rate_hz, c->hiss ? "hiss" : "speech", c->veto ? "veto" : "", chunks[k], ns[0],
                        ns[repeat / 2]);
        }
        rc = 0;
    }
    free(agc);
    free(w);
    free(x);
    return rc;
}

int
main(int argc, char** argv) {
    int seconds = 60;
    int repeat = 7;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            if (dsd_parse_int_strict(argv[++i], 10, 1, 600, &seconds) != 0) {
                DSD_FPRINTF(stderr, "--seconds takes 1 to 600\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            if (dsd_parse_int_strict(argv[++i], 10, 1, BENCH_REPEAT_MAX, &repeat) != 0) {
                DSD_FPRINTF(stderr, "--repeat takes 1 to %d\n", BENCH_REPEAT_MAX);
                return 2;
            }
        } else {
            DSD_FPRINTF(stderr, "usage: %s [--seconds S] [--repeat N]\n", argv[0]);
            return 2;
        }
    }
    DSD_FPRINTF(stdout, "state %zu bytes; %d s of audio per pass, %d repeats\n", sizeof(dsd_speech_agc), seconds,
                repeat);
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        if (bench_run(&k_cases[i], seconds, repeat) != 0) {
            DSD_FPRINTF(stderr, "case %zu failed\n", i);
            return 1;
        }
    }
    return 0;
}
