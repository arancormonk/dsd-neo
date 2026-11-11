// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The engine's side of the P25 proof the CQPSK timing loop's post-lock gain follows: a no-carrier exit withdraws it, and
 * so does every carrier boundary (a tune the user asked for, a scan step, a source or input switch, a replay's recorded
 * retune), a stream gap on the same carrier included: the decoding across it starts over, and the next validated frame
 * proves the carrier again. The real installation forwards it to the demod (ENGINE_RTL_STREAM_METRICS_HOOKS_INSTALL);
 * here the table captures it.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int g_proof_calls;
static uint64_t g_proof_last;

static void
capture_p25_proof(uint64_t proof) {
    g_proof_calls++;
    g_proof_last = proof;
}

/* A proof standing, as a validated frame leaves it, and nothing reported since. */
static void
proven(void) {
    dsd_rtl_stream_metrics_hook_note_p25_proven(41U);
    g_proof_calls = 0;
    g_proof_last = 42U;
}

static int
expect(const char* label, int ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s (calls %d, last %llu)\n", label, g_proof_calls, (unsigned long long)g_proof_last);
        return 1;
    }
    return 0;
}

static int
withdrawn(void) {
    return g_proof_calls > 0 && g_proof_last == 0U;
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        free(opts);
        free(state);
        return 1;
    }
    initOpts(opts);
    initState(state);
    dsd_rtl_stream_metrics_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.p25_timing_proof = capture_p25_proof;
    dsd_rtl_stream_metrics_hooks_set(&hooks);

    int rc = 0;
    proven();
    noCarrier(opts, state);
    rc |= expect("a no-carrier exit withdraws the proof", withdrawn());

    static const struct {
        dsd_carrier_boundary_kind kind;
        const char* label;
    } boundaries[] = {
        {DSD_CARRIER_BOUNDARY_TUNE, "a tune's carrier boundary withdraws the proof"},
        {DSD_CARRIER_BOUNDARY_SCAN_STEP, "a scan step's carrier boundary withdraws the proof"},
        {DSD_CARRIER_BOUNDARY_SOURCE, "a source change's carrier boundary withdraws the proof"},
        {DSD_CARRIER_BOUNDARY_INPUT_SWITCH, "an input switch's carrier boundary withdraws the proof"},
        {DSD_CARRIER_BOUNDARY_REPLAY_RETUNE, "a replay retune's carrier boundary withdraws the proof"},
        {DSD_CARRIER_BOUNDARY_STREAM_GAP, "a stream gap withdraws the proof"},
    };

    for (size_t i = 0; i < sizeof boundaries / sizeof boundaries[0]; i++) {
        proven();
        dsd_engine_carrier_boundary(opts, state, boundaries[i].kind, 0);
        rc |= expect(boundaries[i].label, withdrawn());
    }

    dsd_rtl_stream_metrics_hooks_set(NULL);
    freeState(state);
    free(opts);
    free(state);
    if (rc == 0) {
        DSD_FPRINTF(stderr, "ENGINE_P25_TIMING_PROOF: OK\n");
    }
    return rc;
}
