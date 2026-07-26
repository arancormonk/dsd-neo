// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25p1 encryption lockout ends the canonical call directly, without the TDU
 * path that arms p25_p1_identity_pending. ESS repeats that follow on the same
 * carrier (LDU2 every superframe until the release retunes) previously minted
 * an identity-less epoch that carried the resolved ALG/KID, surfacing a
 * phantom "TGT: 00000000; SRC: 00000000; ENC; ALG: .." event row when the
 * channel released.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"

#define TEST_TG     57111
#define TEST_ALGID  0x84
#define TEST_KEYID  0x023F
// A talkgroup of its own: the encrypted-call cache that lockouts populate is not
// cleared by reset_test_state(), so reusing TEST_TG would block a later grant.
#define TEST_TG_ALT (TEST_TG + 100)

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

static void
reset_test_state(void) {
    if (g_state.event_history_s != NULL) {
        free(g_state.event_history_s);
        g_state.event_history_s = NULL;
    }
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    g_opts.trunk_enable = 1;
    g_opts.trunk_tune_group_calls = 1;
    g_opts.trunk_tune_enc_calls = 0;
    g_state.p25_cc_freq = 851000000;
    g_state.lastsynctype = DSD_SYNC_P25P1_POS;
    g_state.synctype = DSD_SYNC_P25P1_POS;
    g_state.event_history_s = calloc(2, sizeof(Event_History_I));
    if (g_state.event_history_s != NULL) {
        for (int i = 0; i < 2; i++) {
            init_event_history(&g_state.event_history_s[i], 0, 255);
        }
    }
    p25_sm_init_ctx(p25_sm_get_ctx(), &g_opts, &g_state);
}

static void
event_ticks(void) {
    for (int i = 0; i < 3; i++) {
        watchdog_event_current(&g_opts, &g_state, 0);
        watchdog_event_history(&g_opts, &g_state, 0);
    }
}

static int
committed_event_count(void) {
    if (g_state.event_history_s == NULL) {
        return -1;
    }
    int count = 0;
    for (int i = 1; i < 255; i++) {
        if (g_state.event_history_s[0].Event_History_Items[i].event_string[0] != '\0') {
            count++;
        }
    }
    return count;
}

static int
committed_events_contain(const char* needle) {
    if (g_state.event_history_s == NULL) {
        return 0;
    }
    for (int i = 1; i < 255; i++) {
        if (strstr(g_state.event_history_s[0].Event_History_Items[i].event_string, needle) != NULL) {
            return 1;
        }
    }
    return 0;
}

static uint64_t
slot0_epoch(void) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(&g_state, 0U, &call) <= 0) {
        return 0U;
    }
    return call.epoch;
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
tune_group_grant(int tg) {
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
    g_state.trunk_chan_map[0x1234] = 851500000;
    p25_sm_init_ctx(ctx, &g_opts, &g_state);
    g_opts.trunk_is_tuned = 1;
    p25_sm_event_t grant = p25_sm_ev_group_grant(0x1234, 851500000, tg, 0, 0x00);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
}

static void
begin_identified_call(void) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = TEST_TG,
        .policy_target_id = TEST_TG,
        .service_options = 0x40U,
        .has_service_metadata = 1U,
    };
    (void)dsd_call_state_observe(&g_state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    event_ticks();
}

/* ESS repeats after the lockout ended the call must reuse the recorded
 * transmission instead of minting an identity-less epoch. */
static int
test_lockout_ess_repeats_do_not_mint_epochs(void) {
    int rc = 0;
    reset_test_state();

    begin_identified_call();
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE1, 0, TEST_ALGID, TEST_KEYID, 0x1111ULL, TEST_TG);
    event_ticks();
    rc |= expect("lockout fixture blocked", g_state.p25_crypto_state[0] == DSD_P25_CRYPTO_BLOCKED);

    p25_emit_enc_lockout_once_typed(&g_opts, &g_state, 0, TEST_TG, 0x40, 1);
    event_ticks();
    const uint64_t ended_epoch = slot0_epoch();
    dsd_call_snapshot call;
    rc |=
        expect("lockout ends call", dsd_call_state_get(&g_state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect("lockout call keeps crypto", call.algid == TEST_ALGID && call.kid == TEST_KEYID);
    rc |= expect("lockout commits one event", committed_event_count() == 1);

    // LDU2 ESS keeps arriving with fresh MI until the release retunes.
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE1, 0, TEST_ALGID, TEST_KEYID, 0x2222ULL, TEST_TG);
    event_ticks();
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE1, 0, TEST_ALGID, TEST_KEYID, 0x3333ULL, TEST_TG);
    event_ticks();

    rc |= expect("ess repeat no phantom epoch", slot0_epoch() == ended_epoch);
    rc |= expect("ess repeat stays ended",
                 dsd_call_state_get(&g_state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect("ess repeat single event", committed_event_count() == 1);
    rc |= expect("no identity-less rows", !committed_events_contain("TGT: 00000000"));
    rc |= expect("lockout row keeps target", committed_events_contain("TGT: 00057111"));
    return rc;
}

/* The conventional TDU flow arms p25_p1_identity_pending: the next HDU's
 * crypto must still open the next transmission's epoch before its LCW
 * identity arrives. */
static int
test_identity_pending_ess_still_opens_call(void) {
    int rc = 0;
    reset_test_state();

    begin_identified_call();
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE1, 0, TEST_ALGID, TEST_KEYID, 0x1111ULL, TEST_TG);
    event_ticks();
    p25_emit_enc_lockout_once_typed(&g_opts, &g_state, 0, TEST_TG, 0x40, 1);
    event_ticks();
    const uint64_t ended_epoch = slot0_epoch();

    g_state.p25_p1_identity_pending = 1;
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE1, 0, TEST_ALGID, TEST_KEYID, 0x4444ULL, TEST_TG);
    event_ticks();

    dsd_call_snapshot call;
    rc |= expect("identity-pending opens call",
                 dsd_call_state_get(&g_state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE);
    rc |= expect("identity-pending new epoch", slot0_epoch() != ended_epoch);
    rc |= expect("identity-pending epoch started", g_state.p25_p1_identity_epoch_started == 1);
    return rc;
}

/* A later assignment may legitimately reuse the same system ALGID/KID. The
 * ended lockout epoch must not suppress ESS after the assignment generation
 * advances, even if a fresh grant has already cleared identity_pending. */
static int
test_reused_key_after_new_assignment_opens_call(void) {
    int rc = 0;
    reset_test_state();

    begin_identified_call();
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE1, 0, TEST_ALGID, TEST_KEYID, 0x1111ULL, TEST_TG);
    event_ticks();
    p25_emit_enc_lockout_once_typed(&g_opts, &g_state, 0, TEST_TG, 0x40, 1);
    event_ticks();
    const uint64_t ended_epoch = slot0_epoch();
    rc |= expect("lockout epoch recorded", g_state.p25_p1_lockout_epoch.valid != 0U);

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    ctx->grant_count++;
    g_state.p25_p1_identity_pending = 0;
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE1, 0, TEST_ALGID, TEST_KEYID, 0x6666ULL, TEST_TG);
    event_ticks();

    dsd_call_snapshot call;
    rc |= expect("reused-key assignment opens call",
                 dsd_call_state_get(&g_state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE);
    rc |= expect("reused-key assignment starts new epoch", slot0_epoch() != ended_epoch);
    return rc;
}

/* An ended call without recorded crypto is not a continuation context: a
 * fresh ESS observation must still open a call to hold its classification. */
static int
test_ess_after_cryptoless_end_still_opens_call(void) {
    int rc = 0;
    reset_test_state();

    begin_identified_call();
    (void)dsd_call_state_end(&g_state, 0U, 0.0);
    event_ticks();
    const uint64_t ended_epoch = slot0_epoch();

    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE1, 0, TEST_ALGID, TEST_KEYID, 0x5555ULL, TEST_TG);
    event_ticks();

    dsd_call_snapshot call;
    rc |= expect("cryptoless-end ess opens call",
                 dsd_call_state_get(&g_state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE);
    rc |= expect("cryptoless-end ess new epoch", slot0_epoch() != ended_epoch);
    return rc;
}

/* Conventional receivers have neither a grant generation nor necessarily a
 * populated carrier frequency. Bound the post-lockout ESS continuation by
 * time so a later same-key transmission can still begin an epoch. */
static int
test_stale_same_key_ess_opens_conventional_call(void) {
    int rc = 0;
    reset_test_state();

    begin_identified_call();
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE1, 0, TEST_ALGID, TEST_KEYID, 0x1111ULL, TEST_TG);
    p25_emit_enc_lockout_once_typed(&g_opts, &g_state, 0, TEST_TG, 0x40, 1);
    const uint64_t ended_epoch = slot0_epoch();
    rc |= expect("stale fixture lockout recorded", g_state.p25_p1_lockout_epoch.valid != 0U);

    g_state.p25_p1_lockout_epoch.recorded_m = dsd_time_now_monotonic_s() - 1.1;
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE1, 0, TEST_ALGID, TEST_KEYID, 0x7777ULL, TEST_TG);

    dsd_call_snapshot call;
    rc |= expect("stale same-key ESS opens call",
                 dsd_call_state_get(&g_state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE);
    rc |= expect("stale same-key ESS starts epoch", slot0_epoch() != ended_epoch);
    return rc;
}

/* The suppression window is measured from the last accepted repeat, not from
 * the lockout instant. LDU2 carries the ESS every other LDU, so a lockout that
 * holds the carrier through hangtime keeps re-resolving well past one second;
 * a window anchored to the lockout would expire mid-hangtime and let the next
 * repeat mint the phantom epoch this test file exists to prevent. */
static int
test_lockout_ess_window_slides_with_repeats(void) {
    int rc = 0;
    reset_test_state();

    begin_identified_call();
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE1, 0, TEST_ALGID, TEST_KEYID, 0x1111ULL, TEST_TG);
    event_ticks();
    p25_emit_enc_lockout_once_typed(&g_opts, &g_state, 0, TEST_TG, 0x40, 1);
    event_ticks();
    const uint64_t ended_epoch = slot0_epoch();
    rc |= expect("sliding fixture lockout recorded", g_state.p25_p1_lockout_epoch.valid != 0U);

    // Four repeats, each 0.9 s after the previous one. Total elapsed since the
    // lockout is 3.6 s, far past the 1.0 s window, yet no gap ever exceeds it.
    for (int i = 0; i < 4; i++) {
        g_state.p25_p1_lockout_epoch.recorded_m = dsd_time_now_monotonic_s() - 0.9;
        (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE1, 0, TEST_ALGID, TEST_KEYID,
                                 0x2000ULL + (unsigned long long)i, TEST_TG);
        event_ticks();
        rc |= expect("sliding repeat mints no epoch", slot0_epoch() == ended_epoch);
    }

    dsd_call_snapshot call;
    rc |= expect("sliding repeats leave call ended",
                 dsd_call_state_get(&g_state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect("sliding repeats commit one event", committed_event_count() == 1);
    rc |= expect("sliding repeats emit no identity-less row", !committed_events_contain("TGT: 00000000"));
    return rc;
}

/* p25_emit_enc_lockout_once_typed() only records the lockout epoch when it
 * finalizes a matching call, but handle_enc() ends the slot unconditionally.
 * A lockout whose kind does not match the canonical call -- a group lockout
 * raised while the slot holds a private call -- must still leave the following
 * ESS repeats with a suppression context. */
static int
test_non_matching_lockout_still_records_epoch(void) {
    int rc = 0;
    reset_test_state();
    tune_group_grant(TEST_TG_ALT);
    rc |= expect("non-matching fixture tuned", p25_sm_get_ctx()->state == P25_SM_TUNED);

    // Put a private call on the slot so the group lockout below cannot match
    // it: p25_lockout_get_call_context() compares kind as well as target, so
    // it reports no match and the emit path alone records nothing.
    const dsd_call_observation private_call = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_PRIVATE_VOICE,
        .ota_target_id = TEST_TG_ALT,
        .policy_target_id = TEST_TG_ALT,
        .service_options = 0x40U,
        .has_service_metadata = 1U,
    };
    (void)dsd_call_state_observe(&g_state, &private_call, DSD_CALL_BOUNDARY_BEGIN);
    const uint64_t private_epoch = slot0_epoch();

    // The precondition handle_enc() checks before locking out.
    g_state.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    p25_sm_emit_enc(&g_opts, &g_state, 0, TEST_ALGID, TEST_KEYID, TEST_TG_ALT);

    dsd_call_snapshot call;
    rc |= expect("non-matching lockout ends call",
                 dsd_call_state_get(&g_state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect("non-matching lockout records epoch", g_state.p25_p1_lockout_epoch.valid != 0U);
    rc |= expect("non-matching lockout records the ended epoch",
                 g_state.p25_p1_lockout_epoch.call_epoch == private_epoch);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_lockout_ess_repeats_do_not_mint_epochs();
    rc |= test_lockout_ess_window_slides_with_repeats();
    rc |= test_non_matching_lockout_still_records_epoch();
    rc |= test_identity_pending_ess_still_opens_call();
    rc |= test_reused_key_after_new_assignment_opens_call();
    rc |= test_ess_after_cryptoless_end_still_opens_call();
    rc |= test_stale_same_key_ess_opens_conventional_call();

    if (g_state.event_history_s != NULL) {
        free(g_state.event_history_s);
        g_state.event_history_s = NULL;
    }
    dsd_state_ext_free_all(&g_state);

    if (rc == 0) {
        DSD_FPRINTF(stderr, "P25 P1 LOCKOUT EVENTS: OK\n");
    }
    return rc;
}
