// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Unit tests for the TETRA trunking state machine (Phase 13).
 *
 * Uses dsd_trunk_tuning_hooks_set() to install counting stubs that track
 * whether tune_to_freq / return_to_cc were actually called.
 *
 * Link requirements:
 *   tetra_trunk_sm.c, dsd-neo_runtime, dsd-neo_core
 */

#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Test infrastructure
 * ----------------------------------------------------------------------- */
static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        g_failures++; \
    } \
} while (0)

/* -----------------------------------------------------------------------
 * Mock hooks
 * ----------------------------------------------------------------------- */
static int g_tune_calls    = 0;
static long g_tuned_freq   = 0;
static int g_release_calls = 0;
static uint64_t g_last_request_id = 0U;
static dsd_trunk_tune_result g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
static dsd_trunk_tune_result g_release_result = DSD_TRUNK_TUNE_RESULT_OK;

static dsd_trunk_tune_result
mock_tune_to_freq(dsd_opts *opts, dsd_state *state, long int freq, int ted_sps,
                  uint64_t request_id)
{
    (void)ted_sps;
    g_tune_calls++;
    g_tuned_freq = freq;
    g_last_request_id = request_id;
    if (opts && state && dsd_trunk_tune_result_is_ok(g_tune_result)) {
        opts->trunk_is_tuned = 1;
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
        state->last_vc_sync_time = state->last_cc_sync_time = 999;
        state->last_vc_sync_time_m = state->last_cc_sync_time_m = 999.0;
        state->p25_last_vc_tune_time = 999;
        state->p25_last_vc_tune_time_m = 999.0;
    }
    return g_tune_result;
}

static dsd_trunk_tune_result
mock_return_to_cc(dsd_opts *opts, dsd_state *state, uint64_t request_id)
{
    g_release_calls++;
    g_last_request_id = request_id;
    if (opts && state && dsd_trunk_tune_result_is_ok(g_release_result)) {
        opts->trunk_is_tuned = 0;
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
        state->last_vc_sync_time = 0;
        state->last_vc_sync_time_m = 0.0;
    }
    return g_release_result;
}

static void
install_hooks(void)
{
    dsd_trunk_tuning_hooks h = {0};
    h.tune_to_freq_request = mock_tune_to_freq;
    h.return_to_cc_request = mock_return_to_cc;
    dsd_trunk_tuning_hooks_set(h);
}

static void
reset_counters(void)
{
    g_tune_calls    = 0;
    g_tuned_freq    = 0;
    g_release_calls = 0;
    g_last_request_id = 0U;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_release_result = DSD_TRUNK_TUNE_RESULT_OK;
    dsd_trunk_tuning_requests_reset();
}

/* Build a minimal opts/state pair ready for trunking (heap-allocated). */
static void
make_pair(dsd_opts **opts_out, dsd_state **state_out)
{
    dsd_opts  *opts  = (dsd_opts  *)calloc(1, sizeof(dsd_opts));
    dsd_state *state = (dsd_state *)calloc(1, sizeof(dsd_state));
    opts->trunk_enable   = 1;
    opts->trunk_hangtime = 5.0f; /* 5 second hangtime */
    state->trunk_cc_freq = 380000000L; /* 380 MHz CC */
    state->tetra_dl_carrier_hz = state->trunk_cc_freq;
    state->samplesPerSymbol = 10;
    *opts_out  = opts;
    *state_out = state;
}

/* -----------------------------------------------------------------------
 * Test 1: on_grant with trunking enabled → tune hook called
 * ----------------------------------------------------------------------- */
static void
test_grant_tunes(void)
{
    dsd_opts *opts; dsd_state *state;
    make_pair(&opts, &state);
    reset_counters();
    tetra_sm_init();

    tetra_sm_on_grant(opts, state, 390000000L, 1);

    CHECK(g_tune_calls == 1,   "tune hook must be called once on grant");
    CHECK(g_tuned_freq == 390000000L, "tuned to correct VC frequency");
    CHECK(tetra_sm_get_state() == TETRA_SM_TUNED, "SM must be TUNED");
    CHECK(state->tetra_trunk_state == TETRA_SM_TUNED, "snapshot state must mirror TUNED");
    free(opts); free(state);
    fprintf(stderr, "  PASS test_grant_tunes\n");
}

/* -----------------------------------------------------------------------
 * Test 2: on_grant with trunk_enable=0 → no tuning
 * ----------------------------------------------------------------------- */
static void
test_grant_disabled(void)
{
    dsd_opts *opts; dsd_state *state;
    make_pair(&opts, &state);
    opts->trunk_enable = 0;
    reset_counters();
    tetra_sm_init();

    tetra_sm_on_grant(opts, state, 390000000L, 1);

    CHECK(g_tune_calls == 0, "tune hook must NOT be called when trunking disabled");
    CHECK(tetra_sm_get_state() == TETRA_SM_IDLE, "SM must remain IDLE");
    free(opts); free(state);
    fprintf(stderr, "  PASS test_grant_disabled\n");
}

/* -----------------------------------------------------------------------
 * Test 3: duplicate grant same freq → no second tune, hangtime refreshed
 * ----------------------------------------------------------------------- */
static void
test_double_grant_same_freq(void)
{
    dsd_opts *opts; dsd_state *state;
    make_pair(&opts, &state);
    reset_counters();
    tetra_sm_init();

    tetra_sm_on_grant(opts, state, 390000000L, 1);
    int first_calls = g_tune_calls;
    tetra_sm_on_grant(opts, state, 390000000L, 1);

    CHECK(first_calls == 1, "first grant must tune");
    CHECK(g_tune_calls == 1, "duplicate grant (same freq) must NOT re-tune");
    CHECK(tetra_sm_get_state() == TETRA_SM_TUNED, "SM must remain TUNED");
    opts->trunk_is_tuned = 0; /* Generic no-carrier return completed. */
    g_tune_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    tetra_sm_on_grant(opts, state, 390000000L, 1);
    CHECK(g_tune_calls == 2 && opts->trunk_is_tuned == 0,
          "failed reacquisition must not claim shared tuned state");
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    tetra_sm_on_grant(opts, state, 390000000L, 1);
    CHECK(g_tune_calls == 3, "same-frequency grant must retry after failed reacquisition");
    CHECK(opts->trunk_is_tuned == 1, "successful reacquisition restores shared tuned state");
    free(opts); free(state);
    fprintf(stderr, "  PASS test_double_grant_same_freq\n");
}

/* -----------------------------------------------------------------------
 * Test 4: on_release after grant → return_to_cc hook called, SM on ON_CC
 * ----------------------------------------------------------------------- */
static void
test_release_returns_to_cc(void)
{
    dsd_opts *opts; dsd_state *state;
    make_pair(&opts, &state);
    reset_counters();
    tetra_sm_init();

    tetra_sm_on_grant(opts, state, 390000000L, 1);
    reset_counters();
    tetra_sm_on_release(opts, state);

    CHECK(g_release_calls == 1,  "return_to_cc hook must be called on release");
    CHECK(g_tune_calls    == 0,  "no tune during release");
    CHECK(tetra_sm_get_state() == TETRA_SM_ON_CC, "SM must be ON_CC after release");
    free(opts); free(state);
    fprintf(stderr, "  PASS test_release_returns_to_cc\n");
}

/* -----------------------------------------------------------------------
 * Test 5: trunk_cc_freq=0 → grant blocked even if trunk_enable=1
 * ----------------------------------------------------------------------- */
static void
test_no_cc_freq_blocks_grant(void)
{
    dsd_opts *opts; dsd_state *state;
    make_pair(&opts, &state);
    state->trunk_cc_freq = 0L; /* no CC known */
    reset_counters();
    tetra_sm_init();

    tetra_sm_on_grant(opts, state, 390000000L, 1);

    CHECK(g_tune_calls == 0, "grant must be blocked when CC freq unknown");
    CHECK(tetra_sm_get_state() == TETRA_SM_IDLE, "SM must remain IDLE");
    state->trunk_cc_freq = -1L;
    tetra_sm_on_grant(opts, state, 390000000L, 1);
    CHECK(g_tune_calls == 0, "negative CC must not allow a tune without a valid return frequency");
    CHECK(tetra_sm_get_state() == TETRA_SM_IDLE, "invalid CC keeps SM IDLE");
    free(opts); free(state);
    fprintf(stderr, "  PASS test_no_cc_freq_blocks_grant\n");
}

/* -----------------------------------------------------------------------
 * Test 6: on_cc_sync transitions IDLE→ON_CC
 * ----------------------------------------------------------------------- */
static void
test_cc_sync_transitions_state(void)
{
    dsd_opts *opts; dsd_state *state;
    make_pair(&opts, &state);
    reset_counters();
    tetra_sm_init();

    CHECK(tetra_sm_get_state() == TETRA_SM_IDLE, "SM starts IDLE");
    tetra_sm_on_cc_sync(opts, state);
    CHECK(tetra_sm_get_state() == TETRA_SM_ON_CC, "SM transitions to ON_CC after cc_sync");
    CHECK(state->tetra_trunk_state == TETRA_SM_ON_CC, "snapshot state must mirror ON_CC");
    /* subsequent cc_sync while ON_CC must keep ON_CC state */
    tetra_sm_on_cc_sync(opts, state);
    CHECK(tetra_sm_get_state() == TETRA_SM_ON_CC, "SM stays ON_CC on repeated cc_sync");
    tetra_sm_set_hangtime(7);
    tetra_sm_on_grant(opts, state, 390000000L, 1);
    tetra_sm_on_cc_sync(opts, state);
    CHECK(tetra_sm_get_state() == TETRA_SM_TUNED,
          "sync while shared state is tuned must retain traffic tracking");
    opts->trunk_is_tuned = 0;
    tetra_sm_on_cc_sync(opts, state);
    CHECK(tetra_sm_get_state() == TETRA_SM_ON_CC,
          "confirmed CC sync reconciles an external return");
    CHECK(g_release_calls == 0, "reconciliation must not retune a second time");
    CHECK(tetra_sm_get_hangtime() == 7, "reconciliation preserves configured hangtime");
    free(opts); free(state);
    fprintf(stderr, "  PASS test_cc_sync_transitions_state\n");
}

static void
test_external_return_clears_traffic_state(void)
{
    dsd_opts *opts; dsd_state *state;
    make_pair(&opts, &state);
    reset_counters(); tetra_sm_init();
    tetra_sm_on_grant(opts, state, 390000000L, 8);
    state->tetra_call_active = 1;
    state->tetra_tx_granted_valid = 1;
    state->tetra_tx_granted_ssi = 0x112233u;
    state->tetra_cmce_tx_granted_party_type_valid = 1;
    state->tetra_tx_event_party_ssi_valid = 1;
    state->tetra_tx_continue = state->tetra_tx_interrupted = state->tetra_tx_wait = 1;
    state->tetra_vc_assignment_valid = 1;
    state->tetra_vc_assignment_type = 2;
    state->tetra_vc_timeslot_bitmap = state->tetra_vc_slot = 8;
    state->tetra_vc_carrier = 12;
    state->tetra_vc_freq_hz = 390000000L;
    reset_counters();
    tetra_sm_on_external_cc_return(state);
    CHECK(tetra_sm_get_state() == TETRA_SM_ON_CC, "external return reconciles SM");
    CHECK(state->tetra_trunk_state == TETRA_SM_ON_CC, "external return mirrors ON_CC");
    CHECK(g_tune_calls == 0 && g_release_calls == 0, "external return issues no tuner request");
    CHECK(!state->tetra_call_active && !state->tetra_tx_granted_valid,
          "external return clears call and floor grant");
    CHECK(!state->tetra_tx_granted_ssi && !state->tetra_cmce_tx_granted_party_type_valid
          && !state->tetra_tx_event_party_ssi_valid,
          "external return clears floor-control identities");
    CHECK(!state->tetra_tx_continue && !state->tetra_tx_interrupted && !state->tetra_tx_wait,
          "external return clears floor events");
    CHECK(!state->tetra_vc_assignment_valid && !state->tetra_vc_timeslot_bitmap
          && !state->tetra_vc_freq_hz && !state->tetra_vc_carrier,
          "external return clears traffic allocation");
    state->trunk_cc_freq = 0;
    tetra_sm_on_external_cc_return(state);
    CHECK(tetra_sm_get_state() == TETRA_SM_IDLE,
          "traffic end without a usable CC returns SM to IDLE");
    CHECK(state->tetra_trunk_state == TETRA_SM_IDLE, "missing CC mirrors IDLE");
    free(opts); free(state);
}

static void
test_cc_sync_requires_enabled_valid_cc(void)
{
    dsd_opts *opts; dsd_state *state;
    make_pair(&opts, &state);
    tetra_sm_init();

    opts->trunk_enable = 0;
    tetra_sm_on_cc_sync(opts, state);
    CHECK(tetra_sm_get_state() == TETRA_SM_IDLE,
          "disabled trunking must not enter ON_CC");

    opts->trunk_enable = 1;
    state->trunk_cc_freq = 0;
    tetra_sm_on_cc_sync(opts, state);
    CHECK(tetra_sm_get_state() == TETRA_SM_IDLE,
          "invalid CC frequency must not enter ON_CC");

    free(opts); free(state);
    fprintf(stderr, "  PASS test_cc_sync_requires_enabled_valid_cc\n");
}

/* -----------------------------------------------------------------------
 * Test 7: tick while TUNED with expired hangtime → release
 *
 * Use hangtime=0.0f so that any real elapsed time triggers the release.
 * ----------------------------------------------------------------------- */
static void
test_hangtime_expires(void)
{
    dsd_opts *opts; dsd_state *state;
    make_pair(&opts, &state);
    opts->trunk_hangtime = 0.0f; /* expire immediately */
    reset_counters();
    tetra_sm_init();

    tetra_sm_on_grant(opts, state, 390000000L, 1);
    CHECK(tetra_sm_get_state() == TETRA_SM_TUNED, "SM must be TUNED before tick");

    reset_counters();
    tetra_sm_tick(opts, state); /* hangtime=0 → should release */

    CHECK(g_release_calls == 1, "return_to_cc must be called when hangtime expires");
    CHECK(tetra_sm_get_state() == TETRA_SM_ON_CC, "SM must be ON_CC after hangtime");
    free(opts); free(state);
    fprintf(stderr, "  PASS test_hangtime_expires\n");
}

static void
test_active_call_suspends_hangtime(void)
{
    dsd_opts *opts; dsd_state *state;
    make_pair(&opts, &state);
    opts->trunk_hangtime = 0.0f;
    reset_counters();
    tetra_sm_init();

    tetra_sm_on_grant(opts, state, 390000000L, 1);
    state->tetra_call_active = 1;
    tetra_sm_tick(opts, state);

    CHECK(g_release_calls == 0,
          "active call must suspend the idle hangtime");
    CHECK(tetra_sm_get_state() == TETRA_SM_TUNED,
          "active call must remain tuned after hangtime would expire");

    state->tetra_call_active = 0;
    tetra_sm_tick(opts, state);
    CHECK(g_release_calls == 1,
          "hangtime must resume after the call becomes inactive");
    CHECK(tetra_sm_get_state() == TETRA_SM_ON_CC,
          "inactive call must return to CC after hangtime");

    free(opts); free(state);
    fprintf(stderr, "  PASS test_active_call_suspends_hangtime\n");
}

static void
test_disable_while_tuned_returns_to_cc(void)
{
    dsd_opts *opts; dsd_state *state;
    make_pair(&opts, &state);
    reset_counters();
    tetra_sm_init();
    tetra_sm_on_grant(opts, state, 390000000L, 1);
    CHECK(tetra_sm_get_state() == TETRA_SM_TUNED,
          "SM must be TUNED before disabling trunking");

    reset_counters();
    opts->trunk_enable = 0;
    tetra_sm_tick(opts, state);
    CHECK(g_release_calls == 1,
          "disabling trunking while tuned must request return to CC");
    CHECK(tetra_sm_get_state() == TETRA_SM_IDLE,
          "successful disabled cleanup must finish IDLE");
    CHECK(state->tetra_trunk_state == TETRA_SM_IDLE,
          "disabled cleanup snapshot must mirror IDLE");

    free(opts); free(state);
    fprintf(stderr, "  PASS test_disable_while_tuned_returns_to_cc\n");
}

static void
test_grant_result_matrix(void)
{
    const dsd_trunk_tune_result rejected[] = {
        DSD_TRUNK_TUNE_RESULT_DEFERRED,
        DSD_TRUNK_TUNE_RESULT_FAILED,
        DSD_TRUNK_TUNE_RESULT_TIMEOUT,
    };

    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        dsd_opts *opts; dsd_state *state;
        make_pair(&opts, &state);
        reset_counters();
        tetra_sm_init();
        tetra_sm_on_cc_sync(opts, state);
        g_tune_result = rejected[i];

        tetra_sm_on_grant(opts, state, 390000000L, 1);
        CHECK(g_tune_calls == 1, "rejected grant must call tune hook once");
        CHECK(tetra_sm_get_state() == TETRA_SM_ON_CC,
              "rejected grant must keep the previous state");

        g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
        tetra_sm_on_grant(opts, state, 390000000L, 1);
        CHECK(g_tune_calls == 2, "grant must be retryable after rejection");
        CHECK(tetra_sm_get_state() == TETRA_SM_TUNED,
              "successful retry must advance to TUNED");
        free(opts); free(state);
    }

    dsd_opts *opts; dsd_state *state;
    make_pair(&opts, &state);
    reset_counters();
    tetra_sm_init();
    tetra_sm_on_cc_sync(opts, state);
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    tetra_sm_on_grant(opts, state, 390000000L, 1);
    CHECK(tetra_sm_get_state() == TETRA_SM_TUNED,
          "pending grant must stage the TUNED state");
    const uint64_t failed_request = g_last_request_id;
    CHECK(failed_request != 0U, "pending grant must retain a correlated request");
    tetra_sm_on_grant(opts, state, 391000000L, 2);
    CHECK(g_tune_calls == 1, "a second grant must not overtake a pending tune");
    dsd_trunk_tuning_request_publish(failed_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    CHECK(!dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()),
          "failed asynchronous grant must keep frames gated before owner rollback");
    tetra_sm_poll_tuning(opts, state);
    CHECK(tetra_sm_get_state() == TETRA_SM_ON_CC && !opts->trunk_is_tuned,
          "failed asynchronous grant must roll back to ON_CC");
    CHECK(state->trunk_vc_freq[0] == 0 && state->p25_vc_freq[0] == 0
              && state->last_vc_sync_time == 0 && state->last_vc_sync_time_m == 0.0,
          "failed asynchronous grant must restore shared tune caches and timers");
    CHECK(dsd_trunk_tuning_pending_request() == 0U,
          "failed asynchronous grant must retire its frame gate");

    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    tetra_sm_on_grant(opts, state, 392000000L, 4);
    const uint64_t successful_request = g_last_request_id;
    dsd_trunk_tuning_request_publish(successful_request, DSD_TRUNK_TUNE_RESULT_OK);
    tetra_sm_poll_tuning(opts, state);
    CHECK(tetra_sm_get_state() == TETRA_SM_TUNED && opts->trunk_is_tuned,
          "successful asynchronous grant must commit TUNED");
    CHECK(state->trunk_vc_freq[0] == 392000000L && state->p25_vc_freq[1] == 392000000L
              && state->last_vc_sync_time > 0 && state->last_vc_sync_time_m > 0.0,
          "successful asynchronous grant must publish its VC and completion time");
    CHECK(dsd_trunk_tuning_pending_request() == 0U,
          "successful asynchronous grant must open the frame gate");

    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    tetra_sm_on_grant(opts, state, 393000000L, 8);
    const uint64_t reassignment_request = g_last_request_id;
    dsd_trunk_tuning_request_publish(reassignment_request, DSD_TRUNK_TUNE_RESULT_TIMEOUT);
    tetra_sm_poll_tuning(opts, state);
    CHECK(tetra_sm_get_state() == TETRA_SM_TUNED && opts->trunk_is_tuned,
          "failed asynchronous reassignment must retain the old VC state");
    CHECK(state->trunk_vc_freq[0] == 392000000L && state->p25_vc_freq[1] == 392000000L,
          "failed asynchronous reassignment must restore the old shared VC caches");
    const int calls_before_old_grant = g_tune_calls;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    tetra_sm_on_grant(opts, state, 392000000L, 4);
    CHECK(g_tune_calls == calls_before_old_grant,
          "rollback must preserve the old VC identity for duplicate suppression");
    free(opts); free(state);
    fprintf(stderr, "  PASS test_grant_result_matrix\n");
}

static void
test_release_result_matrix(void)
{
    const dsd_trunk_tune_result rejected[] = {
        DSD_TRUNK_TUNE_RESULT_DEFERRED,
        DSD_TRUNK_TUNE_RESULT_FAILED,
        DSD_TRUNK_TUNE_RESULT_TIMEOUT,
    };

    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        dsd_opts *opts; dsd_state *state;
        make_pair(&opts, &state);
        reset_counters();
        tetra_sm_init();
        tetra_sm_on_grant(opts, state, 390000000L, 1);
        g_release_result = rejected[i];

        tetra_sm_on_release(opts, state);
        CHECK(g_release_calls == 1, "rejected release must call return hook once");
        CHECK(tetra_sm_get_state() == TETRA_SM_TUNED,
              "rejected release must keep TUNED state");

        g_release_result = DSD_TRUNK_TUNE_RESULT_OK;
        tetra_sm_on_release(opts, state);
        CHECK(g_release_calls == 2, "release must be retryable after rejection");
        CHECK(tetra_sm_get_state() == TETRA_SM_ON_CC,
              "successful retry must advance to ON_CC");
        free(opts); free(state);
    }

    dsd_opts *opts; dsd_state *state;
    make_pair(&opts, &state);
    reset_counters();
    tetra_sm_init();
    tetra_sm_on_grant(opts, state, 390000000L, 1);
    g_release_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    tetra_sm_on_release(opts, state);
    CHECK(tetra_sm_get_state() == TETRA_SM_ON_CC,
          "pending return must stage the ON_CC state");
    const uint64_t failed_request = g_last_request_id;
    CHECK(failed_request != 0U, "pending return must retain a correlated request");
    tetra_sm_on_release(opts, state);
    CHECK(g_release_calls == 1, "a second release must not overtake a pending return");
    dsd_trunk_tuning_request_publish(failed_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    CHECK(!dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()),
          "failed asynchronous return must keep frames gated before owner rollback");
    tetra_sm_poll_tuning(opts, state);
    CHECK(tetra_sm_get_state() == TETRA_SM_TUNED && opts->trunk_is_tuned,
          "failed asynchronous return must roll back to TUNED");
    CHECK(state->trunk_vc_freq[0] == 390000000L && state->p25_vc_freq[1] == 390000000L
              && state->last_vc_sync_time == 999 && state->last_vc_sync_time_m == 999.0,
          "failed asynchronous return must restore shared VC caches and timers");
    CHECK(dsd_trunk_tuning_pending_request() == 0U,
          "failed asynchronous return must retire its frame gate");

    g_release_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    tetra_sm_on_release(opts, state);
    const uint64_t successful_request = g_last_request_id;
    dsd_trunk_tuning_request_publish(successful_request, DSD_TRUNK_TUNE_RESULT_OK);
    tetra_sm_poll_tuning(opts, state);
    CHECK(tetra_sm_get_state() == TETRA_SM_ON_CC && !opts->trunk_is_tuned,
          "successful asynchronous return must commit ON_CC");
    CHECK(dsd_trunk_tuning_pending_request() == 0U,
          "successful asynchronous return must open the frame gate");
    free(opts); free(state);
    fprintf(stderr, "  PASS test_release_result_matrix\n");
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void)
{
    install_hooks();

    fprintf(stderr, "=== TETRA SM tests ===\n");
    test_grant_tunes();
    test_grant_disabled();
    test_double_grant_same_freq();
    test_release_returns_to_cc();
    test_external_return_clears_traffic_state();
    test_no_cc_freq_blocks_grant();
    test_cc_sync_transitions_state();
    test_cc_sync_requires_enabled_valid_cc();
    test_hangtime_expires();
    test_active_call_suspends_hangtime();
    test_disable_while_tuned_returns_to_cc();
    test_grant_result_matrix();
    test_release_result_matrix();
    fprintf(stderr, "=== %d failure(s) ===\n", g_failures);

    /* Reset hooks so other tests are not affected */
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});

    return g_failures != 0 ? 1 : 0;
}
