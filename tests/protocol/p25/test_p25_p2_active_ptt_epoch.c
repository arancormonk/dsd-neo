// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A tune can land so a SACCH GVCU (MAC_ACTIVE) decodes before one of the
 * transmission-start MAC_PTT repeats. The ACTIVE observation begins the
 * canonical epoch; the first-seen PTT of the same transmission previously
 * force-minted a second epoch, committing the observation epoch's freshly
 * built row as a duplicate event -- surfaced in the field as a same-second
 * pair of rows for one encrypted call: "ENC" with no ALG/KID from the ended
 * ACTIVE epoch, then "ENC; ALG; KID" with the lockout notice from the PTT
 * epoch the lockout finalized. The PTT must fold into the young ACTIVE-begun
 * epoch, while a second, differently-signed PTT is the next transmission and
 * must still begin its own epoch.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../src/protocol/p25/p25_trunk_sm_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#define TEST_TG    21001
#define TEST_SRC   1011308
#define TEST_ALGID 0x84
#define TEST_KEYID 0x2710

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
reset_test_state(int tune_enc_calls) {
    if (g_state.event_history_s != NULL) {
        free(g_state.event_history_s);
        g_state.event_history_s = NULL;
    }
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    g_opts.trunk_enable = 1;
    g_opts.trunk_hangtime = 2.0f;
    g_opts.trunk_tune_group_calls = 1;
    g_opts.trunk_tune_enc_calls = tune_enc_calls;
    g_state.p25_cc_freq = 851000000;
    g_state.lastsynctype = DSD_SYNC_P25P2_POS;
    g_state.synctype = DSD_SYNC_P25P2_POS;
    g_state.event_history_s = calloc(2, sizeof(Event_History_I));
    if (g_state.event_history_s != NULL) {
        for (int i = 0; i < 2; i++) {
            init_event_history(&g_state.event_history_s[i], 0, 255);
        }
    }
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
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
    return dsd_call_state_get(&g_state, 0U, &call) > 0 ? call.epoch : 0U;
}

static int
slot0_phase_is(dsd_call_phase phase) {
    dsd_call_snapshot call;
    return dsd_call_state_get(&g_state, 0U, &call) > 0 && call.phase == phase;
}

static void
tune_grant(int svc) {
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    // The SM defers TDMA grants until the descrambler seed (WACN/SYSID/NAC)
    // has been decoded from the control channel.
    g_state.p2_wacn = 0xBEE00;
    g_state.p2_sysid = 0x1A2;
    g_state.p2_cc = 0x293;
    g_opts.trunk_is_tuned = 1;
    p25_sm_event_t grant = p25_sm_ev_group_grant(0x1234, 851500000, TEST_TG, TEST_SRC, svc);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
}

static void
mac_ptt(uint8_t sig_fill) {
    uint8_t signature[P25_SM_PTT_SIGNATURE_BYTES];
    DSD_MEMSET(signature, sig_fill, sizeof(signature));
    (void)p25_sm_emit_ptt_call_metadata(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, P25_SM_SVC_UNKNOWN, signature,
                                        dsd_time_now_monotonic_s(), 0);
}

static void
resolve_ess(void) {
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE2, 0, TEST_ALGID, TEST_KEYID, 0x1111ULL, TEST_TG);
}

/* The field capture: enc lockout on, tune catches a MAC_ACTIVE before one of
 * the transmission-start MAC_PTT repeats, the PTT's ESS locks the call out.
 * One transmission must commit exactly one history row. */
static int
test_active_then_ptt_lockout_single_row(void) {
    int rc = 0;
    reset_test_state(0);
    tune_grant(0x00);
    event_ticks();

    rc |= expect("lockout fixture ACTIVE accepted",
                 p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x40) > 0);
    event_ticks();
    const uint64_t active_epoch = slot0_epoch();
    rc |= expect("lockout fixture ACTIVE begins epoch", active_epoch != 0U);

    mac_ptt(0x33U);
    rc |= expect("first PTT folds into ACTIVE epoch", slot0_epoch() == active_epoch);
    rc |= expect("first PTT keeps epoch live", slot0_phase_is(DSD_CALL_PHASE_ACTIVE));

    resolve_ess();
    event_ticks();
    rc |= expect("lockout ends the transmission", slot0_phase_is(DSD_CALL_PHASE_ENDED));

    /* Remaining PTT repeats and the release follow on the air. */
    mac_ptt(0x33U);
    resolve_ess();
    event_ticks();
    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG, TEST_SRC, dsd_time_now_monotonic_s());
    p25_crypto_reset_slot(&g_state, 0);
    event_ticks();

    rc |= expect("one transmission commits one row", committed_event_count() == 1);
    rc |= expect("committed row carries the resolved ALG", committed_events_contain("ALG: 84"));
    return rc;
}

/* A differently-signed PTT after the epoch has accepted a PTT is the next
 * transmission: it must still begin its own canonical epoch. */
static int
test_second_ptt_still_begins_new_epoch(void) {
    int rc = 0;
    reset_test_state(1);
    tune_grant(0x00);
    event_ticks();

    rc |= expect("clear fixture ACTIVE accepted",
                 p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x00) > 0);
    const uint64_t active_epoch = slot0_epoch();

    mac_ptt(0x44U);
    rc |= expect("clear first PTT folds into ACTIVE epoch", slot0_epoch() == active_epoch);

    /* Same signature repeats stay in the epoch. */
    mac_ptt(0x44U);
    rc |= expect("PTT retransmit keeps the epoch", slot0_epoch() == active_epoch);

    /* A new signature is a new transmission even without a decoded END. */
    mac_ptt(0x55U);
    rc |= expect("differently-signed PTT begins a new epoch", slot0_epoch() != active_epoch);
    rc |= expect("new transmission epoch is live", slot0_phase_is(DSD_CALL_PHASE_ACTIVE));
    return rc;
}

/* Guard the already-correct paths: a PTT with no preceding ACTIVE observation
 * still commits exactly one row under enc lockout. */
static int
test_ptt_without_active_lockout_single_row(void) {
    int rc = 0;
    reset_test_state(0);
    tune_grant(0x00);
    event_ticks();

    mac_ptt(0x66U);
    resolve_ess();
    event_ticks();
    mac_ptt(0x66U);
    resolve_ess();
    event_ticks();

    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG, TEST_SRC, dsd_time_now_monotonic_s());
    p25_crypto_reset_slot(&g_state, 0);
    event_ticks();

    rc |= expect("ptt-only lockout commits one row", committed_event_count() == 1);
    return rc;
}

/* Conversation turnaround: after the accepted MAC_END_PTT, END repeats
 * interleave with SACCH voice-user copies still naming the completed talker.
 * Such a copy inside the retention tail must not reopen the ended call --
 * previously it minted a phantom epoch that committed a duplicate row when
 * the next talker keyed up. */
static int
test_post_end_active_repeat_does_not_reopen(void) {
    int rc = 0;
    reset_test_state(1);
    tune_grant(0x00);
    event_ticks();

    mac_ptt(0x77U);
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x00);
    event_ticks();
    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG, TEST_SRC, dsd_time_now_monotonic_s());
    event_ticks();
    const uint64_t ended_epoch = slot0_epoch();

    /* Delayed SACCH copy still naming the completed talker. */
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x00);
    event_ticks();
    rc |= expect("post-END repeat does not reopen", slot0_phase_is(DSD_CALL_PHASE_ENDED));
    rc |= expect("post-END repeat mints no epoch", slot0_epoch() == ended_epoch);

    /* Next talker keys up; their transmission runs and ends. */
    uint8_t sig[P25_SM_PTT_SIGNATURE_BYTES];
    DSD_MEMSET(sig, 0x88, sizeof(sig));
    (void)p25_sm_emit_ptt_call_metadata(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC + 7, 1, P25_SM_SVC_UNKNOWN, sig,
                                        dsd_time_now_monotonic_s(), 0);
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC + 7, 1, 0x00);
    event_ticks();
    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG, TEST_SRC + 7, dsd_time_now_monotonic_s());
    event_ticks();

    rc |= expect("two transmissions commit two rows", committed_event_count() == 2);
    return rc;
}

/* A SACCH MAC_PTT repeat whose four-burst assembly straddled the accepted END
 * re-arrives inside the retention tail carrying the signature the completed
 * transmission already refreshed. It must not reopen the ended call --
 * previously it force-minted a phantom epoch (the END had invalidated the
 * retransmission marker) that committed an identical duplicate row. */
static int
test_post_end_ptt_repeat_does_not_reopen(void) {
    int rc = 0;
    reset_test_state(1);
    tune_grant(0x00);
    event_ticks();

    mac_ptt(0xC3U);
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x00);
    event_ticks();
    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG, TEST_SRC, dsd_time_now_monotonic_s());
    event_ticks();
    const uint64_t ended_epoch = slot0_epoch();

    /* Delayed SACCH copy of one of the transmission-start PTT repeats. */
    mac_ptt(0xC3U);
    event_ticks();
    rc |= expect("post-END PTT repeat does not reopen", slot0_phase_is(DSD_CALL_PHASE_ENDED));
    rc |= expect("post-END PTT repeat mints no epoch", slot0_epoch() == ended_epoch);

    /* The next transmission keys up inside the tail with a fresh signature
     * (a new MI): that is the conversation continuing, not retention. */
    uint8_t sig[P25_SM_PTT_SIGNATURE_BYTES];
    DSD_MEMSET(sig, 0xD4, sizeof(sig));
    (void)p25_sm_emit_ptt_call_metadata(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC + 7, 1, P25_SM_SVC_UNKNOWN, sig,
                                        dsd_time_now_monotonic_s(), 0);
    event_ticks();
    rc |= expect("fresh-signature PTT reopens", slot0_phase_is(DSD_CALL_PHASE_ACTIVE));
    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG, TEST_SRC + 7, dsd_time_now_monotonic_s());
    event_ticks();

    rc |= expect("two transmissions commit two rows", committed_event_count() == 2);
    return rc;
}

/* A live-typed MAC_ACTIVE whose payload carries no decodable voice-user block
 * emits an identity-less start. Inside the tail it re-fills the retained
 * assignment identity with unknown service options, reopening the ended call
 * as a crypto-pending phantom -- the field's trailing "SRC 00000000; ENC"
 * rows. It must be treated as retention like its identity-bearing siblings. */
static int
test_post_end_identityless_active_does_not_reopen(void) {
    int rc = 0;
    reset_test_state(1);
    tune_grant(0x00);
    event_ticks();

    mac_ptt(0xE5U);
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x00);
    event_ticks();
    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG, TEST_SRC, dsd_time_now_monotonic_s());
    event_ticks();
    const uint64_t ended_epoch = slot0_epoch();

    /* Live-typed PDU with non-voice MAC content inside the retention tail. */
    (void)p25_sm_emit_active(&g_opts, &g_state, 0);
    event_ticks();
    rc |= expect("post-END identity-less ACTIVE does not reopen", slot0_phase_is(DSD_CALL_PHASE_ENDED));
    rc |= expect("post-END identity-less ACTIVE mints no epoch", slot0_epoch() == ended_epoch);
    rc |= expect("one transmission commits one row", committed_event_count() == 1);
    return rc;
}

/* MAC_END_PTT often names the fixed-network placeholder (0xFFFFFF) instead of
 * the completed talker. Recording the placeholder as the ended source made a
 * post-END voice-user copy naming the real talker look like a changed source,
 * defeating the retention tail and reopening the ended call between END
 * repeats -- one transmission, two rows. */
static int
test_placeholder_end_src_keeps_tail_guard(void) {
    int rc = 0;
    reset_test_state(1);
    tune_grant(0x00);
    event_ticks();

    mac_ptt(0xB1U);
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x00);
    event_ticks();
    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG, 0xFFFFFF, dsd_time_now_monotonic_s());
    event_ticks();
    const uint64_t ended_epoch = slot0_epoch();

    rc |= expect("placeholder END records the real talker",
                 p25_sm_voice_user_repeats_recent_end(0, TEST_TG, TEST_SRC, dsd_time_now_monotonic_s()) == 1);

    /* Delayed SACCH copy still naming the completed talker. */
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x00);
    event_ticks();
    rc |= expect("sourced repeat after placeholder END does not reopen", slot0_phase_is(DSD_CALL_PHASE_ENDED));
    rc |= expect("sourced repeat after placeholder END mints no epoch", slot0_epoch() == ended_epoch);
    rc |= expect("placeholder END transmission commits one row", committed_event_count() == 1);
    return rc;
}

/* A first-seen MAC_PTT can decode several seconds into a transmission whose
 * GVCU observation began the epoch. With voice decoded continuously and no
 * END between them, it names the transmission already on the air and must
 * fold in; the young-epoch window alone forced a split row. After an activity
 * gap the same late PTT is a fresh start and still forks. */
static int
test_late_first_ptt_folds_into_continuous_epoch(void) {
    int rc = 0;
    reset_test_state(1);
    tune_grant(0x00);
    event_ticks();

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    const double t0 = dsd_time_now_monotonic_s();
    p25_sm_event_t ev = p25_sm_ev_active_call(0, TEST_TG, 0, TEST_SRC, 1, 0x00);
    ev.observed_m = t0;
    p25_sm_event(ctx, &g_opts, &g_state, &ev);
    event_ticks();
    const uint64_t active_epoch = slot0_epoch();
    rc |= expect("continuity fixture opens the epoch", active_epoch != 0U && slot0_phase_is(DSD_CALL_PHASE_ACTIVE));

    /* Voice-user copies keep the slot continuously active past the window. */
    for (int i = 1; i <= 2; i++) {
        ev = p25_sm_ev_active_call(0, TEST_TG, 0, TEST_SRC, 1, 0x00);
        ev.observed_m = t0 + (double)i;
        p25_sm_event(ctx, &g_opts, &g_state, &ev);
    }

    uint8_t sig[P25_SM_PTT_SIGNATURE_BYTES];
    DSD_MEMSET(sig, 0xF2, sizeof(sig));
    (void)p25_sm_emit_ptt_call_metadata(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, P25_SM_SVC_UNKNOWN, sig,
                                        t0 + 2.4, 0);
    event_ticks();
    rc |= expect("late first PTT folds into the continuous epoch", slot0_epoch() == active_epoch);
    rc |= expect("late first PTT keeps the epoch live", slot0_phase_is(DSD_CALL_PHASE_ACTIVE));

    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG, TEST_SRC, t0 + 3.0);
    event_ticks();
    rc |= expect("continuous transmission commits one row", committed_event_count() == 1);

    /* Same shape with an activity gap: a missed terminator may hide a fresh
     * start, so the late PTT must fork its own epoch. */
    reset_test_state(1);
    tune_grant(0x00);
    event_ticks();
    ctx = p25_sm_get_ctx();
    const double t1 = dsd_time_now_monotonic_s();
    ev = p25_sm_ev_active_call(0, TEST_TG, 0, TEST_SRC, 1, 0x00);
    ev.observed_m = t1;
    p25_sm_event(ctx, &g_opts, &g_state, &ev);
    event_ticks();
    const uint64_t gap_epoch = slot0_epoch();

    DSD_MEMSET(sig, 0xF3, sizeof(sig));
    (void)p25_sm_emit_ptt_call_metadata(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, P25_SM_SVC_UNKNOWN, sig,
                                        t1 + 2.4, 0);
    event_ticks();
    rc |= expect("late PTT after an activity gap forks", slot0_epoch() != gap_epoch);
    return rc;
}

/* A different talker inside the tail is fresh evidence: the conversation's
 * next transmission must not be mistaken for retention. */
static int
test_post_end_changed_source_reopens(void) {
    int rc = 0;
    reset_test_state(1);
    tune_grant(0x00);
    event_ticks();

    mac_ptt(0x99U);
    event_ticks();
    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG, TEST_SRC, dsd_time_now_monotonic_s());
    event_ticks();
    const uint64_t ended_epoch = slot0_epoch();

    rc |= expect("changed-source ACTIVE accepted",
                 p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC + 7, 1, 0x00) > 0);
    rc |= expect("changed-source ACTIVE reopens", slot0_phase_is(DSD_CALL_PHASE_ACTIVE));
    rc |= expect("changed-source ACTIVE begins epoch", slot0_epoch() != ended_epoch);
    return rc;
}

/* The suppression helper the VPDU observation path shares with the state
 * machine: same identity inside the tail repeats the ended call; outside the
 * tail, or with a changed source, it is fresh evidence. */
static int
test_repeat_helper_windows(void) {
    int rc = 0;
    reset_test_state(1);
    tune_grant(0x00);
    event_ticks();

    mac_ptt(0xABU);
    event_ticks();
    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG, TEST_SRC, dsd_time_now_monotonic_s());
    event_ticks();

    const double now_m = dsd_time_now_monotonic_s();
    rc |= expect("helper: same identity in tail repeats",
                 p25_sm_voice_user_repeats_recent_end(0, TEST_TG, TEST_SRC, now_m) == 1);
    rc |= expect("helper: source-less copy in tail repeats",
                 p25_sm_voice_user_repeats_recent_end(0, TEST_TG, 0, now_m) == 1);
    rc |= expect("helper: changed source is fresh",
                 p25_sm_voice_user_repeats_recent_end(0, TEST_TG, TEST_SRC + 7, now_m) == 0);
    rc |= expect("helper: changed target is fresh",
                 p25_sm_voice_user_repeats_recent_end(0, TEST_TG + 1, TEST_SRC, now_m) == 0);
    rc |= expect("helper: outside the tail is fresh",
                 p25_sm_voice_user_repeats_recent_end(0, TEST_TG, TEST_SRC, now_m + 1.5) == 0);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_active_then_ptt_lockout_single_row();
    rc |= test_second_ptt_still_begins_new_epoch();
    rc |= test_ptt_without_active_lockout_single_row();
    rc |= test_post_end_active_repeat_does_not_reopen();
    rc |= test_post_end_ptt_repeat_does_not_reopen();
    rc |= test_post_end_identityless_active_does_not_reopen();
    rc |= test_placeholder_end_src_keeps_tail_guard();
    rc |= test_late_first_ptt_folds_into_continuous_epoch();
    rc |= test_post_end_changed_source_reopens();
    rc |= test_repeat_helper_windows();

    if (g_state.event_history_s != NULL) {
        free(g_state.event_history_s);
        g_state.event_history_s = NULL;
    }
    dsd_state_ext_free_all(&g_state);

    if (rc == 0) {
        DSD_FPRINTF(stderr, "P25 P2 ACTIVE PTT EPOCH: OK\n");
    }
    return rc;
}
