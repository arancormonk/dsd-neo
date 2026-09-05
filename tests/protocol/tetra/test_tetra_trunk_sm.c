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
static dsd_trunk_tune_result g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
static dsd_trunk_tune_result g_release_result = DSD_TRUNK_TUNE_RESULT_OK;

static dsd_trunk_tune_result
mock_tune_to_freq(dsd_opts *opts, dsd_state *state, long int freq, int ted_sps,
                  uint64_t request_id)
{
    (void)state; (void)ted_sps; (void)request_id;
    g_tune_calls++;
    g_tuned_freq = freq;
    if (opts && dsd_trunk_tune_result_is_ok(g_tune_result)) opts->trunk_is_tuned = 1;
    return g_tune_result;
}

static dsd_trunk_tune_result
mock_return_to_cc(dsd_opts *opts, dsd_state *state, uint64_t request_id)
{
    (void)state; (void)request_id;
    g_release_calls++;
    if (opts && dsd_trunk_tune_result_is_ok(g_release_result)) opts->trunk_is_tuned = 0;
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
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_release_result = DSD_TRUNK_TUNE_RESULT_OK;
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
    /* subsequent cc_sync while ON_CC must keep ON_CC state */
    tetra_sm_on_cc_sync(opts, state);
    CHECK(tetra_sm_get_state() == TETRA_SM_ON_CC, "SM stays ON_CC on repeated cc_sync");
    free(opts); free(state);
    fprintf(stderr, "  PASS test_cc_sync_transitions_state\n");
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
    CHECK(tetra_sm_get_state() == TETRA_SM_ON_CC,
          "successful disabled return must finish ON_CC");

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
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    tetra_sm_on_grant(opts, state, 390000000L, 1);
    CHECK(tetra_sm_get_state() == TETRA_SM_TUNED,
          "pending grant must stage the TUNED state");
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
    test_no_cc_freq_blocks_grant();
    test_cc_sync_transitions_state();
    test_cc_sync_requires_enabled_valid_cc();
    test_hangtime_expires();
    test_disable_while_tuned_returns_to_cc();
    test_grant_result_matrix();
    test_release_result_matrix();
    fprintf(stderr, "=== %d failure(s) ===\n", g_failures);

    /* Reset hooks so other tests are not affected */
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});

    return g_failures != 0 ? 1 : 0;
}
