// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25p1_ldu.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static int g_ms_calls = 0;
static int g_ss_calls = 0;
static int g_fm_calls = 0;
static int g_fs_calls = 0;

void
playSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_ms_calls++;
}

void
playSynthesizedVoiceSS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_ss_calls++;
}

void
playSynthesizedVoiceFM(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_fm_calls++;
}

void
playSynthesizedVoiceFS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_fs_calls++;
}

static void
reset_counters(void) {
    g_ms_calls = 0;
    g_ss_calls = 0;
    g_fm_calls = 0;
    g_fs_calls = 0;
}

static int
expect_counts(int ms, int ss, int fm, int fs, const char* label) {
    if (g_ms_calls != ms || g_ss_calls != ss || g_fm_calls != fm || g_fs_calls != fs) {
        fprintf(stderr, "FAIL %s: got ms=%d ss=%d fm=%d fs=%d expected ms=%d ss=%d fm=%d fs=%d\n", label, g_ms_calls,
                g_ss_calls, g_fm_calls, g_fs_calls, ms, ss, fm, fs);
        return 1;
    }
    return 0;
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    struct {
        int floating_point;
        int channels;
        int ms;
        int ss;
        int fm;
        int fs;
        const char* label;
    } cases[] = {
        {0, 1, 1, 0, 0, 0, "short_mono"},       {0, 2, 0, 1, 0, 0, "short_stereo"},
        {1, 1, 0, 0, 1, 0, "float_mono"},       {1, 2, 0, 0, 0, 1, "float_stereo"},
        {0, 0, 0, 0, 0, 0, "invalid_channels"}, {2, 2, 0, 0, 0, 0, "invalid_float_mode"},
    };

    for (size_t i = 0; i < (sizeof cases / sizeof cases[0]); i++) {
        reset_counters();
        opts->floating_point = cases[i].floating_point;
        opts->pulse_digi_out_channels = cases[i].channels;
        p25p1_play_imbe_audio(opts, state);
        if (expect_counts(cases[i].ms, cases[i].ss, cases[i].fm, cases[i].fs, cases[i].label) != 0) {
            free(opts);
            free(state);
            return 1;
        }
    }

    reset_counters();
    p25p1_play_imbe_audio(NULL, state);
    p25p1_play_imbe_audio(opts, NULL);
    if (expect_counts(0, 0, 0, 0, "null_guard") != 0) {
        free(opts);
        free(state);
        return 1;
    }

    fprintf(stderr, "P25 P1 audio dispatch: OK\n");
    free(opts);
    free(state);
    return 0;
}
