// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A manual tune whose RTL wait timed out, followed to its completion (issue #671, B4). Before, a late failure left the
 * decoder's tuner frequency (rtlsdr_center_freq) on the frequency asked for, with no notice: the synchronous reconcile
 * covers only a result inside the wait, the controller dropped the completion of an untagged tune, and the engine's
 * failure reconcile follows the trunk-tuning ledger only.
 *
 * The tunes run io_control_set_freq_tagged() into the real RTL controller, stood in for without a device or a
 * controller thread (rtl_stream_test_manual_tune_standin_*()): each one waits out the controller's synchronous
 * deadline and times out, and the test lands it later as the controller thread does. The completion reaches the
 * engine through the callback the engine registers (dsd_engine_trunk_tuning_hooks_install()). The orchestrator's
 * started-stream check is the one step stepped over: rtl_stream_tune_tagged() is renamed at compile time to the
 * stand-in's tune.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/manual_tune.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/io/rtl_stream_fwd.h"

#include "engine_hooks_install.h"
#include "manual_tune_pending.h"
#include "rtl_stream_test_support.h"

#define APPLIED_HZ 851000000U
#define ENTRY_A_HZ 853000000U
#define ENTRY_B_HZ 855000000U

/* The stream's own copy of the options, which the controller's tune writes. */
static dsd_opts* g_stream_opts;

/* rtl_stream_tune_tagged(), renamed here and in the io/control source this test compiles (see the CMake target). */
int test_rtl_stream_tune_tagged(RtlSdrContext* ctx, uint32_t center_freq_hz, uint64_t request_id);

int
test_rtl_stream_tune_tagged(RtlSdrContext* ctx, uint32_t center_freq_hz, uint64_t request_id) {
    (void)ctx;
    return rtl_stream_test_manual_tune_standin_tune(g_stream_opts, center_freq_hz, request_id);
}

/* The trunk-tuning hooks the installer sets; nothing here tunes through them. */
dsd_trunk_tune_result
dsd_engine_trunk_tune_to_freq_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                      uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)freq;
    (void)ted_sps;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_FAILED;
}

dsd_trunk_tune_result
dsd_engine_trunk_tune_to_cc_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)freq;
    (void)ted_sps;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_FAILED;
}

dsd_trunk_tune_result
dsd_engine_return_to_cc_request(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_FAILED;
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} fixture;

static void
fixture_begin(fixture* f) {
    dsd_engine_manual_tune_supersede(f->state);
    DSD_MEMSET(f->opts, 0, sizeof *f->opts);
    DSD_MEMSET(f->state, 0, sizeof *f->state);
    DSD_MEMSET(g_stream_opts, 0, sizeof *g_stream_opts);
    f->opts->audio_in_type = AUDIO_IN_RTL;
    f->opts->rtlsdr_center_freq = APPLIED_HZ;
    f->state->rtl_ctx = (RtlSdrContext*)f->state; /* never dereferenced: the stand-in takes the tune */
    assert(rtl_stream_test_manual_tune_standin_begin(APPLIED_HZ) == 0);
}

static void
fixture_end(void) {
    rtl_stream_test_manual_tune_standin_end();
}

/* A frequency entry as svc_rtl_tune() makes it: tune, then note what timed out. Returns the pending ID. */
static uint64_t
enter_frequency(fixture* f, uint32_t hz) {
    const uint32_t prior_hz = f->opts->rtlsdr_center_freq;
    uint64_t pending_id = 0U;
    assert(io_control_set_freq_tagged(f->opts, f->state, hz, &pending_id) == RTL_STREAM_TUNE_TIMEOUT);
    assert((pending_id & RTL_STREAM_TUNE_ID_MANUAL_BIT) != 0U);
    assert(f->opts->rtlsdr_center_freq == hz);
    dsd_engine_manual_tune_note_pending(f->state, DSD_MANUAL_TUNE_ENTRY, pending_id, hz, prior_hz, -1, 0U, 0U);
    return pending_id;
}

/* Take the queued request off the controller, as its thread does: the tag and the target it carries. */
static void
take_request(uint64_t expect_token, uint32_t expect_hz) {
    uint64_t token = 0U;
    uint32_t hz = 0U;
    assert(rtl_stream_test_manual_tune_standin_take(&token, &hz) == 0);
    assert(token == expect_token);
    assert(hz == expect_hz);
}

static int
toast_is(const dsd_state* state, const char* want) {
    if (strcmp(state->ui_msg, want) != 0) {
        DSD_FPRINTF(stderr, "toast: got \"%s\" want \"%s\"\n", state->ui_msg, want);
        return 0;
    }
    return 1;
}

/* Nothing pending: the poll returns NONE and touches nothing, whatever completion the slot holds. */
static void
test_no_record_polls_none(fixture* f) {
    fixture_begin(f);
    dsd_engine_manual_tune_completion(RTL_STREAM_TUNE_ID_MANUAL_BIT | 7U, RTL_STREAM_TUNE_FAILED);
    f->opts->rtlsdr_center_freq = ENTRY_A_HZ;
    assert(dsd_engine_manual_tune_poll(f->opts, f->state) == DSD_MANUAL_TUNE_NONE);
    assert(f->opts->rtlsdr_center_freq == ENTRY_A_HZ);
    assert(f->state->ui_msg[0] == '\0');
    fixture_end();
}

/* B4: timeout, then a late failure. The cached centre goes back to the one the controller applied, and a toast names
   the frequency that failed. */
static void
test_late_failure_reverts_the_cached_centre(fixture* f) {
    fixture_begin(f);
    const uint64_t id = enter_frequency(f, ENTRY_A_HZ);
    assert(dsd_engine_manual_tune_poll(f->opts, f->state) == DSD_MANUAL_TUNE_NONE); /* not landed yet */
    assert(f->opts->rtlsdr_center_freq == ENTRY_A_HZ);

    take_request(id, ENTRY_A_HZ);
    assert(rtl_stream_test_manual_tune_standin_finish(RTL_STREAM_TUNE_FAILED, APPLIED_HZ) == 0);
    const int polled = dsd_engine_manual_tune_poll(f->opts, f->state);
    if (polled != DSD_MANUAL_TUNE_FAILED || f->opts->rtlsdr_center_freq != APPLIED_HZ) {
        DSD_FPRINTF(stderr, "late failure: poll %d, cached centre %u Hz (want %d, %u Hz)\n", polled,
                    f->opts->rtlsdr_center_freq, DSD_MANUAL_TUNE_FAILED, APPLIED_HZ);
    }
    assert(polled == DSD_MANUAL_TUNE_FAILED);
    assert(f->opts->rtlsdr_center_freq == APPLIED_HZ);
    assert(toast_is(f->state, "Failed: frequency -> 853.000000 MHz"));

    /* Consumed: a second poll finds nothing. */
    f->state->ui_msg[0] = '\0';
    assert(dsd_engine_manual_tune_poll(f->opts, f->state) == DSD_MANUAL_TUNE_NONE);
    assert(f->state->ui_msg[0] == '\0');
    fixture_end();
}

/* Timeout, then a late success: the cached centre stays on the frequency asked for, with no toast. */
static void
test_late_success_keeps_the_cached_centre(fixture* f) {
    fixture_begin(f);
    const uint64_t id = enter_frequency(f, ENTRY_A_HZ);
    take_request(id, ENTRY_A_HZ);
    assert(rtl_stream_test_manual_tune_standin_finish(RTL_STREAM_TUNE_OK, ENTRY_A_HZ) == 0);
    assert(dsd_engine_manual_tune_poll(f->opts, f->state) == DSD_MANUAL_TUNE_LANDED);
    assert(f->opts->rtlsdr_center_freq == ENTRY_A_HZ);
    assert(f->state->ui_msg[0] == '\0');
    assert(dsd_engine_manual_tune_poll(f->opts, f->state) == DSD_MANUAL_TUNE_NONE);
    fixture_end();
}

/* Timeout A, then entry B while A is in flight: B supersedes A's record, so A's late failure changes nothing. B's own
   completion still lands. */
static void
test_newer_entry_supersedes_a_late_failure(fixture* f) {
    fixture_begin(f);
    const uint64_t id_a = enter_frequency(f, ENTRY_A_HZ);
    take_request(id_a, ENTRY_A_HZ); /* in flight on the controller */
    const uint64_t id_b = enter_frequency(f, ENTRY_B_HZ);
    assert(id_b != id_a);

    assert(rtl_stream_test_manual_tune_standin_finish(RTL_STREAM_TUNE_FAILED, APPLIED_HZ) == 0);
    assert(dsd_engine_manual_tune_poll(f->opts, f->state) == DSD_MANUAL_TUNE_NONE);
    assert(f->opts->rtlsdr_center_freq == ENTRY_B_HZ);
    assert(f->state->ui_msg[0] == '\0');

    take_request(id_b, ENTRY_B_HZ);
    assert(rtl_stream_test_manual_tune_standin_finish(RTL_STREAM_TUNE_OK, ENTRY_B_HZ) == 0);
    assert(dsd_engine_manual_tune_poll(f->opts, f->state) == DSD_MANUAL_TUNE_LANDED);
    assert(f->opts->rtlsdr_center_freq == ENTRY_B_HZ);
    fixture_end();
}

/* Two entries back to back while the first is still queued: the second coalesces into it and times out as well (a
   manual tune's tag owns no request, so it is not deferred), and the completion comes under the second's ID. */
static void
test_second_entry_coalesces_into_a_queued_one(fixture* f) {
    fixture_begin(f);
    (void)enter_frequency(f, ENTRY_A_HZ);
    const uint64_t id_b = enter_frequency(f, ENTRY_B_HZ);
    take_request(id_b, ENTRY_B_HZ);
    assert(rtl_stream_test_manual_tune_standin_finish(RTL_STREAM_TUNE_FAILED, APPLIED_HZ) == 0);
    assert(dsd_engine_manual_tune_poll(f->opts, f->state) == DSD_MANUAL_TUNE_FAILED);
    assert(f->opts->rtlsdr_center_freq == APPLIED_HZ);
    assert(toast_is(f->state, "Failed: frequency -> 855.000000 MHz"));
    fixture_end();
}

/* The controller thread's part in a failed coalesced request. It waits for the second entry to queue over the first
   (the queued request's tag is no longer the first entry's), then takes the request and fails it while that entry
   still waits. The tag is the handshake: no ordering rests on a sleep. */
typedef struct {
    uint64_t first_id;
    uint64_t taken_token;
} coalesced_failure;

static DSD_THREAD_RETURN_TYPE
fail_the_coalesced_request(void* arg) {
    coalesced_failure* c = (coalesced_failure*)arg;
    uint64_t queued = c->first_id;
    /* Bounded so a broken handshake fails the test instead of hanging it: 10 s, far past the entry's 500 ms wait. */
    for (int tries = 0; tries < 10000; tries++) {
        if (rtl_stream_test_manual_tune_standin_queued(&queued) == 1 && queued != c->first_id) {
            break;
        }
        dsd_sleep_ms(1U);
    }
    uint32_t hz = 0U;
    if (queued != c->first_id && rtl_stream_test_manual_tune_standin_take(&c->taken_token, &hz) == 0) {
        (void)rtl_stream_test_manual_tune_standin_finish(RTL_STREAM_TUNE_FAILED, APPLIED_HZ);
    }
    DSD_THREAD_RETURN;
}

/* Timeout A, then entry B coalesces into A's queued request, which fails inside B's wait: B's synchronous failure
   supersedes A's record, so it must put the cached centre back itself. A's target, which the receiver never reached,
   must not stay. */
static void
test_newer_entry_failing_in_its_wait_reverts_the_cached_centre(fixture* f) {
    fixture_begin(f);
    coalesced_failure controller_side = {0U, 0U};
    controller_side.first_id = enter_frequency(f, ENTRY_A_HZ);
    dsd_thread_t controller_thread;
    assert(dsd_thread_create(&controller_thread, fail_the_coalesced_request, &controller_side) == 0);
    uint64_t pending_id = UINT64_MAX;
    const int rc = io_control_set_freq_tagged(f->opts, f->state, ENTRY_B_HZ, &pending_id);
    assert(dsd_thread_join(controller_thread) == 0);
    assert(rc == RTL_STREAM_TUNE_FAILED);
    assert(pending_id == 0U);
    assert((controller_side.taken_token & RTL_STREAM_TUNE_ID_MANUAL_BIT) != 0U);
    assert(controller_side.taken_token != controller_side.first_id);
    /* As svc_rtl_tune() does with a result that reached the controller: nothing pending, the record superseded. */
    dsd_engine_manual_tune_note_pending(f->state, DSD_MANUAL_TUNE_ENTRY, pending_id, ENTRY_B_HZ, ENTRY_A_HZ, -1, 0U,
                                        0U);
    if (f->opts->rtlsdr_center_freq != APPLIED_HZ) {
        DSD_FPRINTF(stderr, "coalesced failure: cached centre %u Hz (want %u Hz)\n", f->opts->rtlsdr_center_freq,
                    APPLIED_HZ);
    }
    assert(f->opts->rtlsdr_center_freq == APPLIED_HZ);
    assert(dsd_engine_manual_tune_poll(f->opts, f->state) == DSD_MANUAL_TUNE_NONE);
    assert(f->opts->rtlsdr_center_freq == APPLIED_HZ);
    fixture_end();
}

/* The stream stops with the tune queued: the stop fails it under its tag, but the session it was asked of is gone, so
   the record is dropped and the cached centre stays for the next stream to open on. */
static void
test_stream_stop_drops_the_record(fixture* f) {
    fixture_begin(f);
    (void)enter_frequency(f, ENTRY_A_HZ);
    assert(rtl_stream_test_manual_tune_standin_stop() == 1);
    assert(dsd_engine_manual_tune_poll(f->opts, f->state) == DSD_MANUAL_TUNE_NONE);
    assert(f->opts->rtlsdr_center_freq == ENTRY_A_HZ);
    assert(f->state->ui_msg[0] == '\0');
    assert(dsd_engine_manual_tune_poll(f->opts, f->state) == DSD_MANUAL_TUNE_NONE);
    fixture_end();
}

/* A manual tune never enters the trunk-tuning ledger, so its late failure gates no frame; a ledger request's failure
   still reaches the ledger through the same callback. */
static void
test_manual_ids_stay_out_of_the_ledger(fixture* f) {
    fixture_begin(f);
    dsd_trunk_tuning_requests_reset();
    const uint64_t id = enter_frequency(f, ENTRY_A_HZ);
    take_request(id, ENTRY_A_HZ);
    assert(rtl_stream_test_manual_tune_standin_finish(RTL_STREAM_TUNE_FAILED, APPLIED_HZ) == 0);
    assert(dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()) == 1);
    assert(dsd_engine_manual_tune_poll(f->opts, f->state) == DSD_MANUAL_TUNE_FAILED);
    fixture_end();

    fixture_begin(f);
    const uint64_t ledger_id = dsd_trunk_tuning_request_begin();
    assert(ledger_id != 0U && (ledger_id & RTL_STREAM_TUNE_ID_MANUAL_BIT) == 0U);
    assert(rtl_stream_test_manual_tune_standin_tune(g_stream_opts, ENTRY_B_HZ, ledger_id) == RTL_STREAM_TUNE_TIMEOUT);
    take_request(ledger_id, ENTRY_B_HZ);
    assert(rtl_stream_test_manual_tune_standin_finish(RTL_STREAM_TUNE_FAILED, APPLIED_HZ) == 0);
    assert(dsd_trunk_tuning_request_status(ledger_id, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()) == 0);
    dsd_trunk_tuning_requests_reset();
    fixture_end();
}

int
main(void) {
    fixture f = {calloc(1, sizeof(dsd_opts)), calloc(1, sizeof(dsd_state))};
    g_stream_opts = calloc(1, sizeof(dsd_opts));
    if (!f.opts || !f.state || !g_stream_opts) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        free(g_stream_opts);
        free(f.state);
        free(f.opts);
        return 1;
    }
    dsd_engine_trunk_tuning_hooks_install();

    test_no_record_polls_none(&f);
    test_late_failure_reverts_the_cached_centre(&f);
    test_late_success_keeps_the_cached_centre(&f);
    test_newer_entry_supersedes_a_late_failure(&f);
    test_second_entry_coalesces_into_a_queued_one(&f);
    test_newer_entry_failing_in_its_wait_reverts_the_cached_centre(&f);
    test_stream_stop_drops_the_record(&f);
    test_manual_ids_stay_out_of_the_ledger(&f);

    rtl_stream_register_tune_completion_callback(NULL, NULL);
    free(g_stream_opts);
    free(f.state);
    free(f.opts);
    printf("ENGINE_MANUAL_TUNE_PENDING: OK\n");
    return 0;
}
