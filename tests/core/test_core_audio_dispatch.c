// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int g_ms_calls = 0;
static int g_ss_calls = 0;

void
playSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    g_ms_calls++;
}

void
playSynthesizedVoiceSS(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    g_ss_calls++;
}

static void
reset_counters(void) {
    g_ms_calls = 0;
    g_ss_calls = 0;
}

static int
expect_counts(int ms, int ss, const char* label) {
    if (g_ms_calls != ms || g_ss_calls != ss) {
        DSD_FPRINTF(stderr, "FAIL %s: got ms=%d ss=%d expected ms=%d ss=%d\n", label, g_ms_calls, g_ss_calls, ms, ss);
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
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    // Both output formats play through the same mixers (issue #652): float output only changes how samples are written.
    struct {
        int floating_point;
        int channels;
        int ms;
        int ss;
        const char* label;
    } cases[] = {
        {0, 1, 1, 0, "short_mono"},   {0, 2, 0, 1, "short_stereo"},     {1, 1, 1, 0, "float_mono"},
        {1, 2, 0, 1, "float_stereo"}, {0, 0, 0, 0, "invalid_channels"}, {1, 3, 0, 0, "invalid_float_channels"},
    };

    for (size_t i = 0; i < (sizeof cases / sizeof cases[0]); i++) {
        reset_counters();
        opts->floating_point = cases[i].floating_point;
        opts->pulse_digi_out_channels = cases[i].channels;
        dsd_play_synthesized_voice(opts, state);
        if (expect_counts(cases[i].ms, cases[i].ss, cases[i].label) != 0) {
            free(opts);
            free(state);
            return 1;
        }
    }

    reset_counters();
    dsd_play_synthesized_voice(NULL, state);
    dsd_play_synthesized_voice(opts, NULL);
    if (expect_counts(0, 0, "null_guard") != 0) {
        free(opts);
        free(state);
        return 1;
    }

    DSD_FPRINTF(stderr, "Core audio dispatch: OK\n");
    free(opts);
    free(state);
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
