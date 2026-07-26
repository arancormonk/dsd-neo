// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25p2 hangtime signaling after MAC_END_PTT must not mint phantom canonical
 * call epochs. Regression coverage for duplicate event-history rows and for
 * end-of-transmission events tagged ENC: source-less MAC_ACTIVE/GVCU
 * announcements repeated during hangtime previously began a fresh epoch with
 * SRC 0 classified encrypted-pending, which surfaced as an extra event row
 * when the next PTT arrived or the channel released.
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
#include "../../../src/protocol/p25/p25_trunk_sm_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"

#define TEST_TG             40602
#define TEST_SRC            600205
#define TEST_PRIVATE_TARGET 700205

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
    g_opts.trunk_tune_private_calls = 1;
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
}

static void
event_ticks(void) {
    for (int i = 0; i < 3; i++) {
        watchdog_event_current(&g_opts, &g_state, 0);
        watchdog_event_history(&g_opts, &g_state, 0);
    }
}

static void
make_signature(uint8_t signature[P25_SM_PTT_SIGNATURE_BYTES], uint8_t fill) {
    DSD_MEMSET(signature, fill, P25_SM_PTT_SIGNATURE_BYTES);
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

static int
slot0_matches(dsd_call_phase phase, uint64_t source) {
    dsd_call_snapshot call;
    return dsd_call_state_get(&g_state, 0U, &call) > 0 && call.phase == phase && call.ota_source_id == source;
}

static void
start_tuned_tdma_call(int target, int src, int is_group) {
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    p25_sm_init_ctx(ctx, &g_opts, &g_state);
    g_opts.trunk_is_tuned = 1;
    p25_sm_event_t grant = is_group ? p25_sm_ev_group_grant(0x1234, 851500000, target, src, 0)
                                    : p25_sm_ev_indiv_grant(0x1234, 851500000, target, src, 0);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
}

static void
start_tuned_tdma(void) {
    start_tuned_tdma_call(TEST_TG, TEST_SRC, 1);
}

static void
transmit_clear_call(uint8_t signature_fill, int target, int src, int is_group) {
    uint8_t signature[P25_SM_PTT_SIGNATURE_BYTES];
    make_signature(signature, signature_fill);
    (void)p25_sm_emit_ptt_call_metadata(&g_opts, &g_state, 0, is_group ? target : 0, is_group ? 0 : target, src,
                                        is_group, P25_SM_SVC_UNKNOWN, signature, dsd_time_now_monotonic_s(), 0);
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE2, 0, 0x80, 0, 0, target);
    event_ticks();
}

static void
transmit_clear(uint8_t signature_fill, int src) {
    transmit_clear_call(signature_fill, TEST_TG, src, 1);
}

static void
end_transmission_call(int target, int src, int is_group) {
    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, is_group ? target : 0, src, dsd_time_now_monotonic_s());
    p25_crypto_reset_slot(&g_state, 0);
    event_ticks();
}

static void
end_transmission(int src) {
    end_transmission_call(TEST_TG, src, 1);
}

/* Hangtime announcements after an explicit end must not begin a canonical
 * epoch, restart crypto classification, or add event-history rows. */
static int
test_hangtime_announcements_do_not_mint_epochs(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma();

    transmit_clear(0x11, TEST_SRC);
    rc |= expect("tx1 active", slot0_matches(DSD_CALL_PHASE_ACTIVE, TEST_SRC));
    const uint64_t tx1_epoch = slot0_epoch();
    rc |= expect("tx1 epoch nonzero", tx1_epoch != 0U);

    end_transmission(TEST_SRC);
    rc |= expect("tx1 ended", slot0_matches(DSD_CALL_PHASE_ENDED, TEST_SRC));
    rc |= expect("tx1 committed", committed_event_count() == 1);

    // Hangtime chatter: identity without service bits (regroup style), a GVCU
    // with clear service bits, and a MAC_ACTIVE with no decodable identity.
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, 0, 1, P25_SM_SVC_UNKNOWN);
    event_ticks();
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, 0, 1, 0x00);
    event_ticks();
    (void)p25_sm_emit_active(&g_opts, &g_state, 0);
    event_ticks();

    rc |= expect("no phantom epoch", slot0_epoch() == tx1_epoch);
    rc |= expect("still ended", slot0_matches(DSD_CALL_PHASE_ENDED, TEST_SRC));
    // The missed-PTT protection may hold the slot classification at
    // encrypted-pending, but the ended canonical call must keep its real
    // classification so no ENC event can be fabricated from the announcement.
    {
        dsd_call_snapshot call;
        rc |= expect("ended call keeps clear crypto",
                     dsd_call_state_get(&g_state, 0U, &call) > 0 && call.crypto == DSD_CALL_CRYPTO_CLEAR);
    }
    rc |= expect("no extra event", committed_event_count() == 1);

    transmit_clear(0x22, TEST_SRC);
    rc |= expect("tx2 active", slot0_matches(DSD_CALL_PHASE_ACTIVE, TEST_SRC));
    rc |= expect("tx2 new epoch", slot0_epoch() != tx1_epoch);
    rc |= expect("no phantom flush at tx2", committed_event_count() == 1);

    end_transmission(TEST_SRC);
    (void)p25_sm_emit_active(&g_opts, &g_state, 0);
    event_ticks();
    p25_sm_emit_idle_at(&g_opts, &g_state, 0, dsd_time_now_monotonic_s());
    event_ticks();

    rc |= expect("two transmissions two events", committed_event_count() == 2);
    rc |= expect("no src zero rows", !committed_events_contain("SRC: 00000000"));
    rc |= expect("no enc rows", !committed_events_contain("ENC;"));
    return rc;
}

/* MAC_END_PTT repeats several times at end of transmission, and delayed SACCH
 * copies of the sourced group announcement interleave with those repeats still
 * naming the talker whose transmission just completed. Re-announcing the
 * completed talker must not resurrect the epoch, re-arm voice activity, or let
 * the repeated END commit duplicate event-history rows. */
static int
test_same_source_announcement_after_end_is_hangtime(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma();

    transmit_clear(0x11, TEST_SRC);
    end_transmission(TEST_SRC);
    rc |= expect("same-src fixture ended", slot0_matches(DSD_CALL_PHASE_ENDED, TEST_SRC));
    const uint64_t ended_epoch = slot0_epoch();
    rc |= expect("same-src fixture committed", committed_event_count() == 1);

    for (int i = 0; i < 3; i++) {
        (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x00);
        event_ticks();
        (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG, TEST_SRC, dsd_time_now_monotonic_s());
        event_ticks();
    }

    rc |= expect("same-src stays ended", slot0_matches(DSD_CALL_PHASE_ENDED, TEST_SRC));
    rc |= expect("same-src no phantom epoch", slot0_epoch() == ended_epoch);
    rc |= expect("same-src single event", committed_event_count() == 1);
    rc |= expect("same-src no enc rows", !committed_events_contain("ENC;"));

    // A new PTT from the same talker is a real transmission and still opens.
    transmit_clear(0x22, TEST_SRC);
    rc |= expect("same-src rekey active", slot0_matches(DSD_CALL_PHASE_ACTIVE, TEST_SRC));
    rc |= expect("same-src rekey new epoch", slot0_epoch() != ended_epoch);
    end_transmission(TEST_SRC);
    rc |= expect("same-src two transmissions two events", committed_event_count() == 2);
    return rc;
}

/* The traffic channel's GVCU observation begins the canonical epoch a moment
 * before the SM's ACTIVE-driven voice start describes the same transmission;
 * the start must fold into that epoch instead of minting a second one and
 * pushing the first epoch's freshly built row as a duplicate event. */
static int
test_active_start_coalesces_with_traffic_observation(void) {
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
        .has_service_metadata = 1U,
    };
    rc |= expect("coalesce fixture begins", dsd_call_state_observe(&g_state, &gvcu, DSD_CALL_BOUNDARY_CONTINUE) > 0);
    const uint64_t observed_epoch = slot0_epoch();
    event_ticks();

    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x00);
    event_ticks();

    rc |= expect("coalesce keeps epoch", slot0_epoch() == observed_epoch);
    rc |= expect("coalesce stays active", slot0_matches(DSD_CALL_PHASE_ACTIVE, TEST_SRC));
    rc |= expect("coalesce no committed rows", committed_event_count() == 0);

    end_transmission(TEST_SRC);
    rc |= expect("coalesce single event on end", committed_event_count() == 1);
    return rc;
}

/* An announcement that names a talker after the end is a real transmission
 * (late entry with a missed MAC_PTT) and must still open a call. */
static int
test_source_bearing_announcement_opens_call(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma();

    transmit_clear(0x11, TEST_SRC);
    end_transmission(TEST_SRC);
    const uint64_t ended_epoch = slot0_epoch();

    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, 777001, 1, 0x00);
    event_ticks();

    rc |= expect("late entry opens call", slot0_matches(DSD_CALL_PHASE_ACTIVE, 777001U));
    rc |= expect("late entry new epoch", slot0_epoch() != ended_epoch);
    return rc;
}

/* A source-less announcement for a different group is a new call, not
 * hangtime signaling for the completed assignment. */
static int
test_source_less_different_target_opens_call(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma();

    transmit_clear(0x11, TEST_SRC);
    end_transmission(TEST_SRC);
    const uint64_t ended_epoch = slot0_epoch();
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();

    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG + 1, 0, 0, 1, 0x00);
    event_ticks();

    dsd_call_snapshot call = {0};
    rc |= expect("source-less changed target call exists", dsd_call_state_get(&g_state, 0U, &call) > 0);
    rc |= expect("source-less changed target active", call.phase == DSD_CALL_PHASE_ACTIVE);
    rc |= expect("source-less changed target identity", call.kind == DSD_CALL_KIND_GROUP_VOICE
                                                            && call.ota_target_id == (uint64_t)(TEST_TG + 1)
                                                            && call.ota_source_id == 0U);
    rc |= expect("source-less changed target new epoch", call.epoch != ended_epoch);
    rc |= expect("source-less changed target clears hangtime", ctx->t_hangtime_m == 0.0);
    (void)p25_sm_emit_decoded_voice(&g_opts, &g_state, 0);
    event_ticks();
    rc |= expect("decoded voice keeps changed target",
                 dsd_call_state_get(&g_state, 0U, &call) > 0 && call.ota_target_id == (uint64_t)(TEST_TG + 1));
    return rc;
}

/* Standard Unit-to-Unit Voice User carries a subscriber source field, so a
 * zeroed post-END repeat remains hangtime. Telephone-interconnect voice has
 * no source field and must be allowed to open a new epoch for the same target. */
static int
test_source_optional_private_voice_opens_call(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma_call(TEST_PRIVATE_TARGET, TEST_SRC, 0);

    transmit_clear_call(0x11, TEST_PRIVATE_TARGET, TEST_SRC, 0);
    end_transmission_call(TEST_PRIVATE_TARGET, TEST_SRC, 0);
    const uint64_t ended_epoch = slot0_epoch();
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();

    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, 0, TEST_PRIVATE_TARGET, 0, 0, 0x00);
    event_ticks();

    dsd_call_snapshot call = {0};
    rc |= expect("standard private repeat exists", dsd_call_state_get(&g_state, 0U, &call) > 0);
    rc |= expect("standard private repeat stays ended", call.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect("standard private repeat keeps epoch", call.epoch == ended_epoch);

    (void)p25_sm_emit_active_call_ex(&g_opts, &g_state, 0, 0, TEST_PRIVATE_TARGET, 0, 0, 0x00, 1);
    event_ticks();

    rc |= expect("source-optional private call exists", dsd_call_state_get(&g_state, 0U, &call) > 0);
    rc |= expect("source-optional private active", call.phase == DSD_CALL_PHASE_ACTIVE);
    rc |= expect("source-optional private identity", call.kind == DSD_CALL_KIND_PRIVATE_VOICE
                                                         && call.ota_target_id == TEST_PRIVATE_TARGET
                                                         && call.ota_source_id == 0U);
    rc |= expect("source-optional private new epoch", call.epoch != ended_epoch);
    rc |= expect("source-optional private clears hangtime", ctx->t_hangtime_m == 0.0);
    (void)p25_sm_emit_decoded_voice(&g_opts, &g_state, 0);
    event_ticks();
    rc |= expect("decoded voice keeps private target", dsd_call_state_get(&g_state, 0U, &call) > 0
                                                           && call.kind == DSD_CALL_KIND_PRIVATE_VOICE
                                                           && call.ota_target_id == TEST_PRIVATE_TARGET);
    return rc;
}

/* The VPDU observer can legitimately begin the next same-identity epoch after
 * hangtime expires while a companion slot retains the carrier. Its following
 * MAC_ACTIVE must not be mistaken for the preceding slot-local END. */
static int
test_fresh_vpdu_epoch_not_suppressed(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma();

    transmit_clear(0x11, TEST_SRC);
    end_transmission(TEST_SRC);
    const uint64_t ended_epoch = slot0_epoch();

    const dsd_call_observation vpdu = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = TEST_TG,
        .policy_target_id = TEST_TG,
        .ota_source_id = TEST_SRC,
        .service_options = 0x00U,
        .has_service_metadata = 1U,
    };
    rc |= expect("fresh vpdu begins call", dsd_call_state_observe(&g_state, &vpdu, DSD_CALL_BOUNDARY_CONTINUE) > 0);
    p25_crypto_begin_voice_call(&g_state, DSD_P25_CRYPTO_PHASE2, 0, 0x00, 0);
    g_state.p25_p2_audio_allowed[0] = 1;

    const uint64_t vpdu_epoch = slot0_epoch();
    rc |= expect("fresh vpdu advances epoch", vpdu_epoch != ended_epoch);
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x00);
    event_ticks();

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    dsd_call_snapshot call = {0};
    rc |= expect("fresh vpdu active accepted", ctx->slots[0].voice_active == 1);
    rc |= expect("fresh vpdu epoch coalesced", dsd_call_state_get(&g_state, 0U, &call) > 0
                                                   && call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch == vpdu_epoch);
    rc |= expect("fresh vpdu clear crypto preserved", g_state.p25_crypto_state[0] == DSD_P25_CRYPTO_CLEAR);
    rc |= expect("fresh vpdu clear audio preserved", g_state.p25_p2_audio_allowed[0] == 1);

    (void)p25_sm_emit_decoded_voice(&g_opts, &g_state, 0);
    event_ticks();
    rc |= expect("fresh vpdu decoded voice stays clear", g_state.p25_crypto_state[0] == DSD_P25_CRYPTO_CLEAR);
    rc |= expect("fresh vpdu decoded voice stays audible", g_state.p25_p2_audio_allowed[0] == 1);
    return rc;
}

/* Decoded voice frames are live transmission evidence even without a talker
 * ID. They must reopen the retained assignment when MAC_PTT was missed. */
static int
test_decoded_voice_opens_late_entry(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma();

    transmit_clear(0x11, TEST_SRC);
    end_transmission(TEST_SRC);
    const uint64_t ended_epoch = slot0_epoch();
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    rc |= expect("late entry fixture hangtime", ctx->t_hangtime_m > 0.0);

    (void)p25_sm_emit_decoded_voice(&g_opts, &g_state, 0);
    event_ticks();

    rc |= expect("decoded voice opens anonymous call", slot0_matches(DSD_CALL_PHASE_ACTIVE, 0U));
    rc |= expect("decoded voice starts new epoch", slot0_epoch() != ended_epoch);
    rc |= expect("decoded voice keeps traffic channel", ctx->state == P25_SM_TUNED);
    rc |= expect("decoded voice clears hangtime", ctx->t_hangtime_m == 0.0);
    return rc;
}

/* A post-end voice start with no source of its own must not inherit the
 * previous talker from the retained assignment. */
static int
test_post_end_ptt_does_not_inherit_stale_source(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma();

    transmit_clear(0x11, TEST_SRC);
    end_transmission(TEST_SRC);

    uint8_t signature[P25_SM_PTT_SIGNATURE_BYTES];
    make_signature(signature, 0x33);
    (void)p25_sm_emit_ptt_call_metadata(&g_opts, &g_state, 0, TEST_TG, 0, 0, 1, P25_SM_SVC_UNKNOWN, signature,
                                        dsd_time_now_monotonic_s(), 0);
    event_ticks();

    rc |= expect("post-end ptt opens call", slot0_matches(DSD_CALL_PHASE_ACTIVE, 0U));
    return rc;
}

/* Conventional (non-trunked) decode has the same hangtime pattern: a
 * source-less announcement after the end must not begin an epoch, while a
 * source-bearing one must. */
static int
test_conventional_announcements(void) {
    int rc = 0;
    reset_test_state();
    g_opts.trunk_enable = 0;
    p25_sm_init_ctx(p25_sm_get_ctx(), &g_opts, &g_state);

    (void)p25_sm_emit_ptt_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0);
    event_ticks();
    rc |= expect("conventional tx active", slot0_matches(DSD_CALL_PHASE_ACTIVE, TEST_SRC));
    const uint64_t tx_epoch = slot0_epoch();

    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG, TEST_SRC, dsd_time_now_monotonic_s());
    event_ticks();
    rc |= expect("conventional tx ended", slot0_matches(DSD_CALL_PHASE_ENDED, TEST_SRC));

    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, 0, 1, 0x00);
    event_ticks();
    rc |= expect("conventional no phantom epoch", slot0_epoch() == tx_epoch);
    rc |= expect("conventional still ended", slot0_matches(DSD_CALL_PHASE_ENDED, TEST_SRC));

    // A source-less identity for another target is a new call, not a repeat
    // of the completed target's hangtime announcement.
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG + 1, 0, 0, 1, 0x00);
    event_ticks();
    dsd_call_snapshot changed_target = {0};
    rc |= expect("conventional source-less changed target call",
                 dsd_call_state_get(&g_state, 0U, &changed_target) > 0 && changed_target.phase == DSD_CALL_PHASE_ACTIVE
                     && changed_target.kind == DSD_CALL_KIND_GROUP_VOICE
                     && changed_target.ota_target_id == (uint64_t)(TEST_TG + 1) && changed_target.ota_source_id == 0U);
    rc |= expect("conventional source-less changed target epoch", changed_target.epoch != tx_epoch);
    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG + 1, 0, dsd_time_now_monotonic_s());
    event_ticks();

    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, 777002, 1, 0x00);
    event_ticks();
    rc |= expect("conventional late entry opens call", slot0_matches(DSD_CALL_PHASE_ACTIVE, 777002U));

    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, TEST_TG, 777002, dsd_time_now_monotonic_s());
    event_ticks();
    rc |= expect("conventional late entry ended", slot0_matches(DSD_CALL_PHASE_ENDED, 777002U));
    const uint64_t late_epoch = slot0_epoch();

    // Hangtime copies re-announcing the completed talker must not resurrect it.
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, 777002, 1, 0x00);
    event_ticks();
    rc |= expect("conventional same-src suppressed", slot0_matches(DSD_CALL_PHASE_ENDED, 777002U));
    rc |= expect("conventional same-src epoch preserved", slot0_epoch() == late_epoch);

    // A changed talker announced without a PTT is still a new transmission.
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, 777003, 1, 0x00);
    event_ticks();
    rc |= expect("conventional next talker opens call", slot0_matches(DSD_CALL_PHASE_ACTIVE, 777003U));
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_hangtime_announcements_do_not_mint_epochs();
    rc |= test_same_source_announcement_after_end_is_hangtime();
    rc |= test_active_start_coalesces_with_traffic_observation();
    rc |= test_source_bearing_announcement_opens_call();
    rc |= test_source_less_different_target_opens_call();
    rc |= test_source_optional_private_voice_opens_call();
    rc |= test_fresh_vpdu_epoch_not_suppressed();
    rc |= test_decoded_voice_opens_late_entry();
    rc |= test_post_end_ptt_does_not_inherit_stale_source();
    rc |= test_conventional_announcements();

    if (g_state.event_history_s != NULL) {
        free(g_state.event_history_s);
        g_state.event_history_s = NULL;
    }
    dsd_state_ext_free_all(&g_state);

    if (rc == 0) {
        DSD_FPRINTF(stderr, "P25 P2 HANGTIME EVENTS: OK\n");
    }
    return rc;
}
