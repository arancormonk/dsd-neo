// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Opt-in micro-benchmark of an NFM monitor block's way into its recording (issue #668).
 *
 * Times the unsynced analog finalize step on whole 960-sample blocks of audio input under the level squelch, with the
 * engine's analog_call_block hook installed: the tap's reads (and the tone eligibility each read tracks), the voice
 * filters, the hook's block, and the engine's call with, under -P, the conversion to 48 kHz, the pre-roll and the WAV
 * writes. Two streams: "quiet" never opens the squelch (no call: with a pre-roll every block is buffered), "calls"
 * alternates 25 open blocks and 25 closed ones (a call, and a recording under -P, every 50 blocks). bench_dsp measures
 * the receive core only.
 *
 * The source builds against trees without the pre-roll too (it reads opts->analog_record_preroll_ms only where
 * DSD_ANALOG_RECORD_PREROLL_DEFAULT_MS exists), so one run on each tree compares them. Build and run it as
 * docs/dsp-benchmarking.md does the others:
 *   cmake --preset perf-bench
 *   cmake --build --preset perf-bench --target dsd-neo_bench_analog_record
 *   build/perf-bench/tests/dsd-neo_bench_analog_record [--blocks N] [--repeat N] [--case K]
 * One line per case (--case K runs case K alone, for a profiler): its median ns per block over the repeats, and the
 * fastest.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/analog_call.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../test_support/test_support.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "symbol_test_support.h"

#define BENCH_BLOCK      960U
#define BENCH_REPEAT_MAX 32
#define BENCH_RUN        25 /* blocks open, then blocks closed, in the "calls" stream */
#define BENCH_PI         3.14159265358979323846

typedef struct {
    int rate_hz;
    int record;     /* -P */
    int preroll_ms; /* -1: the tree's own default */
    int calls;      /* the "calls" stream, else "quiet" */
} bench_case;

static const bench_case k_cases[] = {
    {48000, 0, -1, 0}, {48000, 1, 0, 0}, {48000, 1, 200, 0}, {48000, 0, -1, 1}, {48000, 1, 0, 1}, {48000, 1, 200, 1},
    {44100, 0, -1, 0}, {44100, 1, 0, 0}, {44100, 1, 200, 0}, {44100, 0, -1, 1}, {44100, 1, 0, 1}, {44100, 1, 200, 1},
};

static float g_open[BENCH_BLOCK];
static float g_closed[BENCH_BLOCK];
static char g_dir[DSD_TEST_PATH_MAX];

/* An open block: 700 and 1300 Hz near -14 dBFS; a closed one: the same 50 dB down. Scaled as audio input is. */
static void
bench_fill(int rate_hz) {
    for (unsigned int i = 0; i < BENCH_BLOCK; i++) {
        const double t = (double)i / (double)rate_hz;
        const double v = (0.6 * sin(2.0 * BENCH_PI * 700.0 * t)) + (0.4 * sin(2.0 * BENCH_PI * 1300.0 * t));
        g_open[i] = (float)(6000.0 * v);
        g_closed[i] = (float)(19.0 * v);
    }
}

static int
bench_remove_entry(const char* name, void* user) {
    (void)user;
    char path[DSD_TEST_PATH_MAX * 2];
    if (dsd_test_path_join(path, sizeof path, g_dir, name) == 0) {
        (void)remove(path);
    }
    return 0;
}

/* The monitor on audio input at the case's rate, the level squelch at -30 dB, the engine's call hooked in. */
static void
bench_setup(dsd_opts* opts, dsd_state* state, const bench_case* c) {
    initOpts(opts);
    initState(state);
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = c->rate_hz;
    opts->input_volume_multiplier = 1;
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->audio_out = 0;
    opts->rtl_squelch_level = dsd_squelch_level_from_sql(-30.0);
    opts->dmr_stereo_wav = c->record;
    DSD_SNPRINTF(opts->wav_out_dir, sizeof opts->wav_out_dir, "%s", g_dir);
#ifdef DSD_ANALOG_RECORD_PREROLL_DEFAULT_MS
    if (c->preroll_ms >= 0) {
        opts->analog_record_preroll_ms = c->preroll_ms;
    }
#endif
}

/* ns per block for @p blocks blocks of the case's stream. */
static double
bench_run(const bench_case* c, int blocks) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (opts == NULL || state == NULL) {
        free(opts);
        free(state);
        return -1.0;
    }
    bench_setup(opts, state, c);
    /* A second of each kind first, so the filters, the squelch and the sinks have settled. */
    for (int i = 0; i < 100; i++) {
        (void)dsd_symbol_test_finalize_unsynced_analog_block(opts, state, i < 50 ? g_closed : g_open, BENCH_BLOCK);
    }
    dsd_engine_analog_call_end(opts, state);
    const uint64_t began = dsd_realtime_mono_ns();
    for (int i = 0; i < blocks; i++) {
        const int open = c->calls && (i / BENCH_RUN) % 2 == 0;
        (void)dsd_symbol_test_finalize_unsynced_analog_block(opts, state, open ? g_open : g_closed, BENCH_BLOCK);
    }
    const uint64_t took = dsd_realtime_mono_ns() - began;
    dsd_engine_analog_call_end(opts, state);
    freeState(state);
    free(state);
    free(opts);
    (void)dsd_dir_list(g_dir, bench_remove_entry, NULL);
    return (double)took / (double)blocks;
}

/* The pre-roll column: "-" without -P, "none" on a tree without the setting, else the case's own. */
static const char*
bench_preroll_label(const bench_case* c, int have_preroll) {
    if (!c->record) {
        return "-";
    }
    if (!have_preroll) {
        return "none";
    }
    return c->preroll_ms > 0 ? "200" : "0";
}

static int
bench_compare(const void* a, const void* b) {
    const double x = *(const double*)a;
    const double y = *(const double*)b;
    return (x > y) - (x < y);
}

static int
bench_arg(int argc, char** argv, const char* name, int fallback, int max_value) {
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            char* end = NULL;
            const long v = strtol(argv[i + 1], &end, 10);
            if (end != argv[i + 1] && *end == '\0' && v > 0 && v <= max_value) {
                return (int)v;
            }
        }
    }
    return fallback;
}

int
main(int argc, char** argv) {
    const int blocks = bench_arg(argc, argv, "--blocks", 5000, 1000000);
    const int repeat = bench_arg(argc, argv, "--repeat", 5, BENCH_REPEAT_MAX);
    const int only = bench_arg(argc, argv, "--case", 0, (int)(sizeof(k_cases) / sizeof(k_cases[0])));
    if (dsd_test_mkdtemp(g_dir, sizeof g_dir, "dsdneo_bench_record") == NULL) {
        return 1;
    }
    dsd_frame_sync_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.analog_call_block = dsd_engine_analog_call_block;
    dsd_frame_sync_hooks_set(&hooks);
#ifdef DSD_ANALOG_RECORD_PREROLL_DEFAULT_MS
    const int have_preroll = 1;
#else
    const int have_preroll = 0;
#endif
    printf("case rate stream -P preroll_ms median_ns_per_block min_ns_per_block (blocks %d, repeats %d)\n", blocks,
           repeat);
    for (size_t k = 0; k < sizeof(k_cases) / sizeof(k_cases[0]); k++) {
        const bench_case* c = &k_cases[k];
        if ((c->record && c->preroll_ms > 0 && !have_preroll) || (only > 0 && (size_t)only != k + 1U)) {
            continue;
        }
        bench_fill(c->rate_hz);
        double ns[BENCH_REPEAT_MAX];
        for (int r = 0; r < repeat; r++) {
            ns[r] = bench_run(c, blocks);
        }
        qsort(ns, (size_t)repeat, sizeof(ns[0]), bench_compare);
        printf("%zu %d %s %s %s %.0f %.0f\n", k + 1U, c->rate_hz, c->calls ? "calls" : "quiet",
               c->record ? "on" : "off", bench_preroll_label(c, have_preroll), ns[repeat / 2], ns[0]);
    }
    static const char* const k_none[] = {NULL};
    (void)dsd_dir_list(g_dir, bench_remove_entry, NULL);
    (void)dsd_test_remove_temp_dir(g_dir, k_none);
    return 0;
}
