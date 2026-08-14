// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 MAC_ACTIVE is positive transmission evidence. Hangtime voice
 * users are filtered by their outer MAC PDU type in the VPDU decoder, so the
 * state machine must not infer hangtime from a recently ended identity --
 * with one time-bounded exception: inside the short post-END retention tail,
 * copies still naming the completed talker on the unchanged target re-describe
 * the transmission that just ended and must not reopen it (see
 * test_p25_p2_active_ptt_epoch.c).
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "../../../src/protocol/p25/p25_trunk_sm_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#define TEST_TG  40602
#define TEST_SRC 600205

static dsd_opts g_opts;
static dsd_state g_state;

static int
expect(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

static dsd_trunk_tune_result
test_tune_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)ted_sps;
    (void)request_id;
    return freq > 0 ? DSD_TRUNK_TUNE_RESULT_OK : DSD_TRUNK_TUNE_RESULT_FAILED;
}

static dsd_trunk_tune_result
test_return_request(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
reset_test_state(void) {
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    g_opts.trunk_enable = 1;
    g_opts.trunk_hangtime = 2.0f;
    g_opts.trunk_tune_group_calls = 1;
    g_opts.trunk_tune_private_calls = 1;
    g_opts.trunk_tune_enc_calls = 1;
    g_state.p25_cc_freq = 851000000;
    g_state.lastsynctype = DSD_SYNC_P25P2_POS;
    g_state.synctype = DSD_SYNC_P25P2_POS;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
}

static uint64_t
slot0_epoch(void) {
    dsd_call_snapshot call;
    return dsd_call_state_get(&g_state, 0U, &call) > 0 ? call.epoch : 0U;
}

static int
slot0_matches(dsd_call_phase phase, uint64_t target, uint64_t source) {
    dsd_call_snapshot call;
    return dsd_call_state_get(&g_state, 0U, &call) > 0 && call.phase == phase && call.ota_target_id == target
           && call.ota_source_id == source;
}

static void
start_tuned_tdma_src(int grant_src) {
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    p25_sm_init_ctx(ctx, &g_opts, &g_state);
    g_opts.trunk_is_tuned = 1;
    p25_sm_event_t grant = p25_sm_ev_group_grant(0x1234, 851500000, TEST_TG, grant_src, 0);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
}

static void
start_tuned_tdma(void) {
    start_tuned_tdma_src(TEST_SRC);
}

static void
transmit_clear(uint8_t signature_fill, int source) {
    uint8_t signature[P25_SM_PTT_SIGNATURE_BYTES];
    DSD_MEMSET(signature, signature_fill, sizeof(signature));
    (void)p25_sm_emit_ptt_call_metadata(&g_opts, &g_state, 0, TEST_TG, 0, source, 1, P25_SM_SVC_UNKNOWN, signature,
                                        dsd_time_now_monotonic_s(), 0);
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE2, 0, 0x80, 0, 0, TEST_TG);
}

static void
end_transmission(int source) {
    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG, source, dsd_time_now_monotonic_s());
    p25_crypto_reset_slot(&g_state, 0);
}

static int
test_same_identity_mac_active_reopens_call(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma();
    transmit_clear(0x11U, TEST_SRC);
    end_transmission(TEST_SRC);
    const uint64_t ended_epoch = slot0_epoch();

    // Past the post-END retention tail: END repeats and delayed SACCH copies
    // of the ended transmission arrive inside it, a re-keyed talker after it.
    p25_sm_get_ctx()->slots[0].last_end_m = dsd_time_now_monotonic_s() - 1.1;

    rc |= expect("same-source ACTIVE accepted",
                 p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x00) > 0);
    rc |= expect("same-source ACTIVE reopens call", slot0_matches(DSD_CALL_PHASE_ACTIVE, TEST_TG, TEST_SRC));
    rc |= expect("same-source ACTIVE starts epoch", slot0_epoch() != ended_epoch);
    rc |= expect("same-source ACTIVE remains clear", g_state.p25_crypto_state[0] == DSD_P25_CRYPTO_CLEAR);
    rc |= expect("same-source ACTIVE clears hangtime", fabs(p25_sm_hangtime_started_m(p25_sm_get_ctx())) <= 1.0e-9);
    return rc;
}

static int
test_identity_decode_failure_still_reopens_call(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma();
    transmit_clear(0x22U, TEST_SRC);
    end_transmission(TEST_SRC);
    const uint64_t ended_epoch = slot0_epoch();

    // Past the post-END retention tail: inside it an identity-less ACTIVE is
    // retention of the ended call and must not reopen (covered by
    // P25_P2_ACTIVE_PTT_EPOCH); after it, a live-typed PDU whose identity
    // failed to decode is the next transmission arriving.
    p25_sm_get_ctx()->slots[0].last_end_m = dsd_time_now_monotonic_s() - 1.1;

    rc |= expect("anonymous ACTIVE accepted", p25_sm_emit_active(&g_opts, &g_state, 0) > 0);
    rc |= expect("anonymous ACTIVE opens retained assignment", slot0_matches(DSD_CALL_PHASE_ACTIVE, TEST_TG, 0U));
    rc |= expect("anonymous ACTIVE starts epoch", slot0_epoch() != ended_epoch);
    return rc;
}

static int
test_conventional_same_identity_active_reopens_call(void) {
    int rc = 0;
    reset_test_state();
    g_opts.trunk_enable = 0;
    p25_sm_init_ctx(p25_sm_get_ctx(), &g_opts, &g_state);

    (void)p25_sm_emit_ptt_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x00);
    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG, TEST_SRC, dsd_time_now_monotonic_s());
    const uint64_t ended_epoch = slot0_epoch();

    rc |= expect("conventional ACTIVE accepted",
                 p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x00) > 0);
    rc |= expect("conventional ACTIVE reopens call", slot0_matches(DSD_CALL_PHASE_ACTIVE, TEST_TG, TEST_SRC));
    rc |= expect("conventional ACTIVE starts epoch", slot0_epoch() != ended_epoch);
    return rc;
}

static int
test_active_coalesces_with_live_vpdu_observation(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma();

    const dsd_call_observation gvcu = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = TEST_TG,
        .policy_target_id = TEST_TG,
        .ota_source_id = TEST_SRC,
        .service_options = 0x00U,
        .has_service_metadata = 1U,
    };
    rc |= expect("live VPDU fixture begins", dsd_call_state_observe(&g_state, &gvcu, DSD_CALL_BOUNDARY_CONTINUE) > 0);
    const uint64_t vpdu_epoch = slot0_epoch();

    rc |= expect("following ACTIVE accepted",
                 p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x00) > 0);
    rc |= expect("following ACTIVE coalesces", slot0_epoch() == vpdu_epoch);
    rc |= expect("following ACTIVE remains live", slot0_matches(DSD_CALL_PHASE_ACTIVE, TEST_TG, TEST_SRC));
    return rc;
}

static int
test_post_end_source_less_ptt_does_not_inherit_talker(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma();
    transmit_clear(0x33U, TEST_SRC);
    end_transmission(TEST_SRC);

    uint8_t signature[P25_SM_PTT_SIGNATURE_BYTES];
    DSD_MEMSET(signature, 0x44, sizeof(signature));
    rc |= expect("source-less PTT accepted",
                 p25_sm_emit_ptt_call_metadata(&g_opts, &g_state, 0, TEST_TG, 0, 0, 1, P25_SM_SVC_UNKNOWN, signature,
                                               dsd_time_now_monotonic_s(), 0)
                     > 0);
    rc |= expect("source-less PTT does not inherit talker", slot0_matches(DSD_CALL_PHASE_ACTIVE, TEST_TG, 0U));
    return rc;
}

/* 0xFFFFFD is the fixed-network call-processing address and 0xFFFFFF is the
 * all-subscribers broadcast address. Neither identifies a talker, so neither
 * may be published as one -- but neither is evidence that the talker the grant
 * already named has stopped, so a known assignment source must survive. */
static int
test_network_controller_source_is_not_a_talker(void) {
    int rc = 0;
    static const int controller_sources[] = {0xFFFFFD, 0xFFFFFF};

    for (size_t i = 0U; i < sizeof(controller_sources) / sizeof(controller_sources[0]); i++) {
        reset_test_state();
        start_tuned_tdma_src(0);

        rc |= expect("controller-source ACTIVE accepted",
                     p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, controller_sources[i], 1, 0x00) > 0);
        rc |= expect("controller source not published as talker", slot0_matches(DSD_CALL_PHASE_ACTIVE, TEST_TG, 0U));
        rc |= expect("controller source not retained on slot", p25_sm_get_ctx()->slots[0].src == 0);
    }
    return rc;
}

/* A controller placeholder in MAC_ACTIVE says "the network sent this", not
 * "the talker is gone". Erasing the subscriber the grant named would drop the
 * only identity the call has. */
static int
test_controller_source_keeps_granted_talker(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma();

    rc |= expect("granted-talker controller ACTIVE accepted",
                 p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, 0xFFFFFD, 1, 0x00) > 0);
    rc |= expect("granted talker survives controller source",
                 slot0_matches(DSD_CALL_PHASE_ACTIVE, TEST_TG, (uint64_t)TEST_SRC));
    rc |= expect("granted talker retained on slot", p25_sm_get_ctx()->slots[0].src == TEST_SRC);
    return rc;
}

/* Telephone interconnect carries no source field at all, so src 0 means "none
 * exists" rather than "not decoded" and must not inherit the grant's talker. */
static int
test_source_absent_active_does_not_inherit_talker(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma();

    rc |= expect("source-absent ACTIVE accepted",
                 p25_sm_emit_active_call_source_absent(&g_opts, &g_state, 0, TEST_TG, 0, 1, 0x00) > 0);
    rc |= expect("source-absent ACTIVE keeps no talker", slot0_matches(DSD_CALL_PHASE_ACTIVE, TEST_TG, 0U));
    return rc;
}

/* A source-less grant repeat is retention only while it re-describes the call
 * that just ended. When the canonical slot holds a different call -- ended out
 * of band, e.g. by a carrier drop, while the assignment moved on -- the repeat
 * is fresh evidence and must open its own epoch instead of being dropped and
 * having its crypto written onto the unrelated ended call. */
static int
test_assignment_repeat_does_not_borrow_mismatched_ended_call(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma_src(0);

    // Park an unrelated ended call on the slot, with clear crypto.
    const dsd_call_observation other_call = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = TEST_TG + 11,
        .policy_target_id = TEST_TG + 11,
        .service_options = 0x00U,
        .has_service_metadata = 1U,
    };
    rc |= expect("mismatch fixture begins", dsd_call_state_observe(&g_state, &other_call, DSD_CALL_BOUNDARY_BEGIN) > 0);
    rc |= expect("mismatch fixture ends", dsd_call_state_end(&g_state, 0U, 0.0) > 0);
    const uint64_t stale_epoch = slot0_epoch();

    // A source-less ACTIVE for the assignment's own target.
    rc |= expect("mismatch repeat accepted", p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, 0, 1, 0x00) > 0);

    dsd_call_snapshot call;
    rc |= expect("mismatch repeat opens its own call", dsd_call_state_get(&g_state, 0U, &call) > 0
                                                           && call.phase == DSD_CALL_PHASE_ACTIVE
                                                           && call.ota_target_id == (uint64_t)TEST_TG);
    rc |= expect("mismatch repeat starts new epoch", slot0_epoch() != stale_epoch);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_same_identity_mac_active_reopens_call();
    rc |= test_assignment_repeat_does_not_borrow_mismatched_ended_call();
    rc |= test_identity_decode_failure_still_reopens_call();
    rc |= test_conventional_same_identity_active_reopens_call();
    rc |= test_active_coalesces_with_live_vpdu_observation();
    rc |= test_post_end_source_less_ptt_does_not_inherit_talker();
    rc |= test_network_controller_source_is_not_a_talker();
    rc |= test_controller_source_keeps_granted_talker();
    rc |= test_source_absent_active_does_not_inherit_talker();
    dsd_state_ext_free_all(&g_state);

    if (rc == 0) {
        DSD_FPRINTF(stderr, "P25 P2 HANGTIME EVENTS: OK\n");
    }
    return rc;
}
