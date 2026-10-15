// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Retiring failed tune requests once no tune owner is active (issue #703). An asynchronous RTL retune stages the
 * decoder's tuner frequency (rtlsdr_center_freq) when it is issued; if it then fails, the radio never left the
 * frequency the stream last applied. Before the engine retires such a failure and lets frames dispatch again, it
 * reconciles the decoder's tuner frequency with the stream's applied one, so a CRC-good block received afterwards is
 * attributed to the frequency the radio is on, never to the failed target. A request still in flight is left alone.
 *
 * The stream's applied frequency is a link-time fake; the RTL context is a dummy pointer that nothing dereferences. A
 * wrap of the request-status read lets the controller's failure land between the engine's read and its retirement.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#define APPLIED_HZ 450000000U
#define STAGED_HZ  451000000U

static int g_failures;
static uint64_t g_fail_after_read; // the controller publishes this request's failure right after the engine reads it

// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __wrap_rtl_stream_get_last_applied_freq(uint32_t* out_freq_hz);
dsd_trunk_tune_result __real_dsd_trunk_tuning_request_status(uint64_t request_id, double* out_completed_m);
dsd_trunk_tune_result __wrap_dsd_trunk_tuning_request_status(uint64_t request_id, double* out_completed_m);

dsd_trunk_tune_result
__wrap_dsd_trunk_tuning_request_status(uint64_t request_id, double* out_completed_m) {
    const dsd_trunk_tune_result result = __real_dsd_trunk_tuning_request_status(request_id, out_completed_m);
    if (request_id != 0U && request_id == g_fail_after_read) {
        g_fail_after_read = 0U;
        dsd_trunk_tuning_request_publish(request_id, DSD_TRUNK_TUNE_RESULT_FAILED);
    }
    return result;
}

int
__wrap_rtl_stream_get_last_applied_freq(uint32_t* out_freq_hz) {
    if (out_freq_hz) {
        *out_freq_hz = APPLIED_HZ;
    }
    return 0;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

static void
expect(int cond, const char* what) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

static uint64_t
failing_request(void) {
    const uint64_t id = dsd_trunk_tuning_request_begin();
    dsd_trunk_tuning_request_mark_ready(id);
    return id;
}

int
main(void) {
    static int dummy_stream;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    initOpts(opts);
    initState(state);
    opts->audio_in_type = AUDIO_IN_RTL;
    state->rtl_ctx = (struct RtlSdrContext*)(void*)&dummy_stream;

    // The failure arrives, then trunking is turned off.
    dsd_trunk_tuning_requests_reset();
    opts->trunk_enable = 1;
    opts->rtlsdr_center_freq = STAGED_HZ; // staged by the probe on issue
    const uint64_t first = failing_request();
    dsd_trunk_tuning_request_publish(first, DSD_TRUNK_TUNE_RESULT_FAILED);
    opts->trunk_enable = 0;
    dsd_engine_retire_inactive_tune_failures(opts, state);
    expect(dsd_trunk_tuning_pending_request() == 0U, "the failure is retired once no tune owner is active");
    expect(opts->rtlsdr_center_freq == APPLIED_HZ, "the decoder's tuner frequency is the stream's applied one");
    state->trunk_cc_freq = 0;
    dmr_sm_note_cc_activity(opts, state, 0); // a CRC-good control block in monitor mode
    expect(state->trunk_cc_freq == (long)APPLIED_HZ, "decoded control is attributed to the radio's frequency");

    // Trunking is turned off first; the failure arrives afterwards.
    dsd_trunk_tuning_requests_reset();
    opts->trunk_enable = 1;
    opts->rtlsdr_center_freq = STAGED_HZ;
    const uint64_t second = failing_request();
    opts->trunk_enable = 0;
    dsd_engine_retire_inactive_tune_failures(opts, state);
    expect(opts->rtlsdr_center_freq == STAGED_HZ, "a request still in flight leaves the staged frequency alone");
    dsd_trunk_tuning_request_publish(second, DSD_TRUNK_TUNE_RESULT_FAILED);
    dsd_engine_retire_inactive_tune_failures(opts, state);
    expect(dsd_trunk_tuning_pending_request() == 0U, "the late failure is retired");
    expect(opts->rtlsdr_center_freq == APPLIED_HZ, "and the tuner frequency reconciled first");

    // The failure lands between the engine's read of the request and its retirement: it is retired only on a later pass,
    // once the tuner frequency has been reconciled.
    dsd_trunk_tuning_requests_reset();
    opts->trunk_enable = 1;
    opts->rtlsdr_center_freq = STAGED_HZ;
    const uint64_t racing = failing_request();
    opts->trunk_enable = 0;
    g_fail_after_read = racing;
    dsd_engine_retire_inactive_tune_failures(opts, state);
    expect(dsd_trunk_tuning_pending_request() == racing && opts->rtlsdr_center_freq == STAGED_HZ,
           "a failure published after the read is not retired unreconciled");
    dsd_engine_retire_inactive_tune_failures(opts, state);
    expect(dsd_trunk_tuning_pending_request() == 0U && opts->rtlsdr_center_freq == APPLIED_HZ,
           "the next pass reconciles, then retires it");

    // A tune owner still active keeps its failure for its own recovery.
    dsd_trunk_tuning_requests_reset();
    opts->trunk_enable = 1;
    opts->rtlsdr_center_freq = STAGED_HZ;
    const uint64_t third = failing_request();
    dsd_trunk_tuning_request_publish(third, DSD_TRUNK_TUNE_RESULT_FAILED);
    dsd_engine_retire_inactive_tune_failures(opts, state);
    expect(dsd_trunk_tuning_pending_request() == third && opts->rtlsdr_center_freq == STAGED_HZ,
           "an active tune owner's failure is left to it");

    dsd_trunk_tuning_requests_reset();
    state->rtl_ctx = NULL;
    freeState(state);
    free(state);
    free(opts);
    if (g_failures == 0) {
        printf("engine tune failure reconcile: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
