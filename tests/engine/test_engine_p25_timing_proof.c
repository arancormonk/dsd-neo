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
 *
 * The trunk tuning paths withdraw it too (trunk_tuning.c): a return to the control channel that retunes nothing keeps
 * the carrier and withdraws it only when the control channel's timing moves the chain, and where the linker can
 * replace the rigctl client, the real voice channel, control channel and scanner tunes withdraw it once accepted and
 * keep it when refused.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#ifdef DSD_NEO_TEST_RIGCTL_TUNE_WRAP
#include <dsd-neo/platform/sockets.h>
#include <stdbool.h>
#endif

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

/* A P25 control channel known with trunking off: a return to it retunes nothing, and its timing decides the chain
   (dsd_engine_apply_cc_symbol_timing()). The control channel is TDMA, so its chain is CQPSK at 6000 sym/s. */
static int
test_return_without_retune(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    opts->trunk_enable = 0;
    state->p25_cc_freq = 851012500L;
    state->p25_cc_is_tdma = 1;
    state->rf_mod = 0;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    proven();
    (void)dsd_engine_return_to_cc_request(opts, state, 0U);
    rc |= expect("the return's timing moved the chain",
                 state->rf_mod == 1 && state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    rc |= expect("a return that moves the chain withdraws the proof", withdrawn());
    proven();
    (void)dsd_engine_return_to_cc_request(opts, state, 0U);
    rc |= expect("a return that restates the chain keeps the proof", g_proof_calls == 0);
    state->p25_cc_freq = 0;
    state->p25_cc_is_tdma = 0;
    return rc;
}

#ifdef DSD_NEO_TEST_RIGCTL_TUNE_WRAP
/* The rigctl peer the tunes reach: it takes the frequency unless a case refuses it. */
static int g_set_freq_ok = 1;

// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
bool __wrap_SetFreq(dsd_socket_t sockfd, long int freq);
bool __wrap_SetModulationKind(dsd_socket_t sockfd, int kind, int bandwidth);

bool
__wrap_SetFreq(dsd_socket_t sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return g_set_freq_ok != 0;
}

bool
__wrap_SetModulationKind(dsd_socket_t sockfd, int kind, int bandwidth) {
    (void)sockfd;
    (void)kind;
    (void)bandwidth;
    return true;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

/* The real tunes (trunk_tuning.c) through a rigctl peer: each accepted one withdraws the proof, by its
   dsd_frame_sync_reset_mod_state(); a refused one moves nothing and keeps it. */
static int
test_trunk_tunes(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    opts->trunk_enable = 1;

    g_set_freq_ok = 0;
    proven();
    rc |= expect("a refused voice channel tune fails",
                 dsd_engine_trunk_tune_to_freq_request(opts, state, 852012500L, 0, 0U) == DSD_TRUNK_TUNE_RESULT_FAILED);
    rc |= expect("a refused voice channel tune keeps the proof", g_proof_calls == 0);
    proven();
    rc |= expect("a refused control channel tune fails",
                 dsd_engine_trunk_tune_to_cc_request(opts, state, 851012500L, 0, 0U) == DSD_TRUNK_TUNE_RESULT_FAILED);
    rc |= expect("a refused control channel tune keeps the proof", g_proof_calls == 0);

    g_set_freq_ok = 1;
    proven();
    rc |= expect("a voice channel tune is accepted",
                 dsd_engine_trunk_tune_to_freq_request(opts, state, 852012500L, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect("a voice channel tune withdraws the proof", withdrawn());
    proven();
    rc |= expect("a control channel tune is accepted",
                 dsd_engine_trunk_tune_to_cc_request(opts, state, 851012500L, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect("a control channel tune withdraws the proof", withdrawn());
    proven();
    uint64_t request_id = 0U;
    rc |= expect("a scanner tune is accepted",
                 dsd_engine_scan_tune_to_freq(opts, state, 853012500L, 0, &request_id) == DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect("a scanner tune withdraws the proof", withdrawn());

    opts->use_rigctl = 0;
    opts->trunk_enable = 0;
    return rc;
}
#endif

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

    dsd_engine_set_decoder_thread(1);
    rc |= test_return_without_retune(opts, state);
#ifdef DSD_NEO_TEST_RIGCTL_TUNE_WRAP
    rc |= test_trunk_tunes(opts, state);
#endif
    dsd_engine_set_decoder_thread(0);

    dsd_rtl_stream_metrics_hooks_set(NULL);
    freeState(state);
    free(opts);
    free(state);
    if (rc == 0) {
        DSD_FPRINTF(stderr, "ENGINE_P25_TIMING_PROOF: OK\n");
    }
    return rc;
}
