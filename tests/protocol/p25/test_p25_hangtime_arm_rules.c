// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Who may arm, restart, or cancel the P25 hangtime countdown.
 *
 * The countdown is one channel-wide deadline -- release this carrier one
 * hangtime after the last *followed* traffic on it -- and several unrelated
 * paths write it. These cases pin the ones that are easy to get backwards:
 *
 *  - a Phase 1 identity wait is followed traffic and keeps re-arming, so an
 *    over that keys up late in the window is not cut off mid-voice;
 *  - a followed voice start cancels it, a lockout-suppressed one does not;
 *  - an encryption classification still in flight owns the release, so the
 *    countdown does not tear a call down before it can resolve clear;
 *  - a lockout reprobe keeps the countdown but still answers to the grant
 *    timeout, so it can neither be released instantly nor ride out a long
 *    configured hangtime;
 *  - a target re-admitted on a clear-claiming grant that re-locks does not get
 *    another re-admission per grant update, while corroborated clear key
 *    material is not penalized at all.
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/enc_lockout.h>
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

#define CLEAR_TG      40601
#define CLEAR_SRC     600205
#define ENC_TG        40699
#define ENC_SRC       600299
#define ENC_ALGID     0x84
#define ENC_KEYID     0x1234
#define TDMA_VC_FREQ  851500000L
#define FDMA_VC_FREQ  852500000L
#define TDMA_CH_SLOT0 0x1234
#define TDMA_CH_SLOT1 0x1235
#define FDMA_CH       0x100A

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
reset_test_state(float hangtime_s, int tune_enc_calls) {
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    g_opts.trunk_enable = 1;
    g_opts.trunk_hangtime = hangtime_s;
    g_opts.trunk_tune_group_calls = 1;
    g_opts.trunk_tune_private_calls = 1;
    g_opts.trunk_tune_enc_calls = tune_enc_calls;
    g_state.p25_cc_freq = 851000000;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
}

static void
start_tuned_tdma(void) {
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    g_state.lastsynctype = DSD_SYNC_P25P2_POS;
    g_state.synctype = DSD_SYNC_P25P2_POS;
    g_state.trunk_chan_map[TDMA_CH_SLOT0] = TDMA_VC_FREQ;
    g_state.trunk_chan_map[TDMA_CH_SLOT1] = TDMA_VC_FREQ;
    g_state.p25_chan_tdma_explicit[1] = 2;
    p25_sm_init_ctx(ctx, &g_opts, &g_state);
    g_opts.trunk_is_tuned = 1;
    p25_sm_event_t grant = p25_sm_ev_group_grant(TDMA_CH_SLOT0, TDMA_VC_FREQ, CLEAR_TG, CLEAR_SRC, 0);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
}

static void
start_tuned_fdma(void) {
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    g_state.lastsynctype = DSD_SYNC_P25P1_POS;
    g_state.synctype = DSD_SYNC_P25P1_POS;
    g_state.trunk_chan_map[FDMA_CH] = FDMA_VC_FREQ;
    p25_sm_init_ctx(ctx, &g_opts, &g_state);
    g_opts.trunk_is_tuned = 1;
    p25_sm_event_t grant = p25_sm_ev_group_grant(FDMA_CH, FDMA_VC_FREQ, CLEAR_TG, CLEAR_SRC, 0);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
}

static void
grant_slot1(int tg, int src, int svc_bits) {
    p25_sm_event_t grant = p25_sm_ev_group_grant(TDMA_CH_SLOT1, TDMA_VC_FREQ, tg, src, svc_bits);
    p25_sm_event(p25_sm_get_ctx(), &g_opts, &g_state, &grant);
}

static void
grant_slot1_update(int tg, int src, int svc_bits) {
    p25_sm_event_t grant = p25_sm_ev_group_grant_update(TDMA_CH_SLOT1, TDMA_VC_FREQ, tg, src, svc_bits);
    p25_sm_event(p25_sm_get_ctx(), &g_opts, &g_state, &grant);
}

static void
transmit(int slot, int tg, int source, uint8_t signature_fill) {
    uint8_t signature[P25_SM_PTT_SIGNATURE_BYTES];
    DSD_MEMSET(signature, signature_fill, sizeof(signature));
    (void)p25_sm_emit_ptt_call_metadata(&g_opts, &g_state, slot, tg, 0, source, 1, P25_SM_SVC_UNKNOWN, signature,
                                        dsd_time_now_monotonic_s(), 0);
}

static void
resolve_enc_blocked(void) {
    /* A non-clear tuple contradicting clear service context quarantines as
     * pending on first sight; the FEC-accepted repeat completes it. */
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE2, 1, ENC_ALGID, ENC_KEYID, 0, ENC_TG);
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE2, 1, ENC_ALGID, ENC_KEYID, 0, ENC_TG);
}

/*
 * A Phase 1 voice start whose LCW identity has not decoded yet is a followed
 * call waiting on its identity, not lockout-suppressed signaling: it has no
 * repeat loop behind it, and the TDU that armed the countdown set the pending
 * flag in the same breath. It must keep re-arming, or an over that keys up
 * near the end of the window is released while its voice is on the air.
 *
 * Encryption lockout is disabled here on purpose -- this path is reached in
 * plain follow mode, outside the lockout rules entirely.
 */
static int
test_p1_identity_wait_rearms_hangtime(void) {
    int rc = 0;
    reset_test_state(2.0f, /*tune_enc_calls*/ 1);
    start_tuned_fdma();
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    rc |= expect("fdma grant tunes single carrier", g_opts.trunk_is_tuned == 1 && ctx->vc_is_tdma == 0);

    transmit(0, CLEAR_TG, CLEAR_SRC, 0x11U);
    rc |= expect("identity-bearing start is followed traffic",
                 ctx->slots[0].voice_active == 1 && ctx->t_hangtime_m <= 0.0);

    /* The TDU ends the over: the countdown arms and the next epoch owes an
     * identity before its audio may open. */
    p25_sm_emit_tdu(&g_opts, &g_state);
    rc |= expect("tdu arms hangtime", ctx->t_hangtime_m > 0.0);
    rc |= expect("tdu marks p1 identity pending", g_state.p25_p1_identity_pending == 1);

    /* Most of the window has gone by when the next over keys up, and its first
     * voice frames carry no decoded identity yet. */
    const double stale_m = dsd_time_now_monotonic_s() - ((double)g_opts.trunk_hangtime - 0.1);
    ctx->t_hangtime_m = stale_m;
    p25_sm_event_t identity_less = p25_sm_ev_active(0);
    p25_sm_event(ctx, &g_opts, &g_state, &identity_less);
    rc |= expect("identity wait re-arms hangtime", ctx->t_hangtime_m > stale_m + 0.05);

    p25_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect("identity wait keeps the carrier", g_opts.trunk_is_tuned == 1);
    return rc;
}

/*
 * The other side of the same branch: once the identity decodes, the start is
 * ordinary followed traffic and cancels the countdown outright.
 */
static int
test_followed_voice_start_cancels_hangtime(void) {
    int rc = 0;
    reset_test_state(2.0f, /*tune_enc_calls*/ 1);
    start_tuned_fdma();
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();

    transmit(0, CLEAR_TG, CLEAR_SRC, 0x11U);
    p25_sm_emit_tdu(&g_opts, &g_state);
    rc |= expect("tdu arms hangtime", ctx->t_hangtime_m > 0.0);

    transmit(0, CLEAR_TG, CLEAR_SRC, 0x33U);
    rc |= expect("followed voice start cancels hangtime", ctx->t_hangtime_m <= 0.0);
    rc |= expect("followed voice start marks the slot active", ctx->slots[0].voice_active == 1);
    return rc;
}

/*
 * Followed activity is a per-slot fact. The tick re-stamps the channel-wide
 * t_voice_m to now on every pass while *either* slot is voice_active, so on a
 * busy TDMA carrier a locked-out slot's own repeats each looked like a slot
 * leaving followed activity -- arming a countdown while the companion is still
 * talking, and pushing the release past one hangtime after the last followed
 * traffic.
 */
static int
test_locked_out_repeats_do_not_arm_beside_a_live_companion(void) {
    int rc = 0;
    /* Follow mode first, so the encrypted call keeps its assignment and its
     * suppressed voice starts still reach the timing rules once lockout is on. */
    reset_test_state(2.0f, /*tune_enc_calls*/ 1);
    start_tuned_tdma();
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();

    grant_slot1(ENC_TG, ENC_SRC, 0x40);
    transmit(1, ENC_TG, ENC_SRC, 0x22U);
    resolve_enc_blocked();
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 1, ENC_TG, 0, ENC_SRC, 1, 0x40);
    rc |= expect("follow mode keeps the encrypted assignment", ctx->slots[1].grant_active == 1);
    rc |= expect("follow mode marks the encrypted slot active", ctx->slots[1].voice_active == 1);

    /* Slot 0 carries a followed clear call and stays up throughout. */
    transmit(0, CLEAR_TG, CLEAR_SRC, 0x11U);
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE2, 0, 0x80, 0, 0, CLEAR_TG);
    rc |= expect("clear companion is active", ctx->slots[0].voice_active == 1);

    /* Lockout goes on mid-call. The tick keeps advancing the channel-wide voice
     * clock because slot 0 is live, so reading followed activity off it would
     * make every suppressed slot-1 repeat look like a transition. */
    g_opts.trunk_tune_enc_calls = 0;
    p25_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect("live companion advances the channel voice clock", ctx->t_voice_m > 0.0);

    for (int repeat = 0; repeat < 3; repeat++) {
        (void)p25_sm_emit_active_call(&g_opts, &g_state, 1, ENC_TG, 0, ENC_SRC, 1, 0x40);
        rc |= expect("locked-out repeat leaves the countdown unarmed", ctx->t_hangtime_m <= 0.0);
        rc |= expect("locked-out repeat leaves the companion alone", ctx->slots[0].voice_active == 1);
    }
    return rc;
}

/*
 * A crypto classification still inside its budget owns the release. The
 * countdown the companion's END armed keeps running underneath it, but it may
 * not tear the pending call down first: the call may yet resolve clear, and
 * shrinking its budget to whatever is left of the hangtime drops legitimate
 * overs mid-classification.
 */
static int
test_pending_classification_outranks_hangtime(void) {
    int rc = 0;
    reset_test_state(2.0f, /*tune_enc_calls*/ 0);
    start_tuned_tdma();
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();

    /* Followed clear call on slot 0, then its END arms the countdown. */
    transmit(0, CLEAR_TG, CLEAR_SRC, 0x11U);
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE2, 0, 0x80, 0, 0, CLEAR_TG);
    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, CLEAR_TG, CLEAR_SRC, dsd_time_now_monotonic_s());
    p25_crypto_reset_slot(&g_state, 0);
    rc |= expect("clear END arms hangtime", ctx->t_hangtime_m > 0.0);

    /* The companion slot is granted with no service options, which under
     * lockout is admitted as one silent classification probe. */
    grant_slot1(ENC_TG, ENC_SRC, P25_SM_SVC_UNKNOWN);
    rc |= expect("probe grant assigns companion slot", ctx->slots[1].grant_active == 1);
    rc |= expect("probe slot classifies pending", g_state.p25_crypto_state[1] == DSD_P25_CRYPTO_ENCRYPTED_PENDING
                                                      && ctx->slots[1].crypto_attempt_m > 0.0);
    /* Its suppressed voice start leaves the classification deadline in charge
     * and, just as importantly, leaves the countdown alone: cancelling it on a
     * start the SM is not following is what let the site's repeats hold the
     * carrier for the life of the encrypted call. */
    const double armed_m = dsd_time_now_monotonic_s() - 0.25;
    ctx->t_hangtime_m = armed_m;
    for (int repeat = 0; repeat < 3; repeat++) {
        (void)p25_sm_emit_active_call(&g_opts, &g_state, 1, ENC_TG, 0, ENC_SRC, 1, P25_SM_SVC_UNKNOWN);
        rc |= expect("pending MAC start keeps slot inactive", ctx->slots[1].voice_active == 0);
        rc |= expect("pending MAC start neither cancels nor restarts the countdown",
                     fabs(ctx->t_hangtime_m - armed_m) <= 1.0e-9);
    }

    /* The hangtime elapses while the classification budget still runs. */
    ctx->t_hangtime_m = dsd_time_now_monotonic_s() - ((double)g_opts.trunk_hangtime + 0.5);
    p25_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect("classification in flight holds the carrier", g_opts.trunk_is_tuned == 1);

    /* Once the budget lapses the carrier goes back, so nothing is held open. */
    ctx->slots[1].crypto_attempt_m = dsd_time_now_monotonic_s() - (ctx->config.grant_timeout_s + 0.5);
    ctx->t_tune_m = ctx->slots[1].crypto_attempt_m;
    ctx->slots[1].last_grant_m = ctx->slots[1].crypto_attempt_m;
    p25_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect("lapsed classification releases the carrier", g_opts.trunk_is_tuned == 0);
    return rc;
}

/*
 * Enabling encryption lockout mid-call is the one way a BLOCKED slot keeps its
 * assignment: follow mode already classified and accepted the call, so nothing
 * tore it down. Its voice starts are suppressed from that moment on, and they
 * keep arriving for the whole transmission. The first one is a real transition
 * out of followed activity and arms the countdown; every repeat after it must
 * leave that countdown where it is, or the deadline never arrives.
 */
static int
test_lockout_enabled_mid_call_arms_once(void) {
    int rc = 0;
    reset_test_state(2.0f, /*tune_enc_calls*/ 1);
    start_tuned_tdma();
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();

    /* Follow mode accepts the encrypted call and keeps its assignment. */
    grant_slot1(ENC_TG, ENC_SRC, 0x40);
    transmit(1, ENC_TG, ENC_SRC, 0x22U);
    resolve_enc_blocked();
    rc |= expect("follow mode classifies BLOCKED", g_state.p25_crypto_state[1] == DSD_P25_CRYPTO_BLOCKED);
    rc |= expect("follow mode keeps the assignment", ctx->slots[1].grant_active == 1);
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 1, ENC_TG, 0, ENC_SRC, 1, 0x40);
    rc |= expect("follow mode marks the slot active", ctx->slots[1].voice_active == 1);
    rc |= expect("followed start cancels any countdown", ctx->t_hangtime_m <= 0.0);

    /* The user turns encryption lockout on while the call is up. */
    g_opts.trunk_tune_enc_calls = 0;

    /* The next repeat is suppressed, and clearing the now-stale activity arms
     * the countdown so the tuned timer can release the carrier. */
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 1, ENC_TG, 0, ENC_SRC, 1, 0x40);
    const double armed_m = ctx->t_hangtime_m;
    rc |= expect("suppressing stale activity arms the countdown", armed_m > 0.0);
    rc |= expect("suppressed start clears the stale activity", ctx->slots[1].voice_active == 0);

    /* Every repeat after it is just the same transmission still going. */
    for (int repeat = 0; repeat < 3; repeat++) {
        (void)p25_sm_emit_active_call(&g_opts, &g_state, 1, ENC_TG, 0, ENC_SRC, 1, 0x40);
        rc |= expect("suppressed repeat does not restart the countdown", fabs(ctx->t_hangtime_m - armed_m) <= 1.0e-9);
    }

    /* So the deadline actually arrives. */
    ctx->t_hangtime_m = dsd_time_now_monotonic_s() - ((double)g_opts.trunk_hangtime + 0.5);
    p25_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect("countdown releases the carrier", g_opts.trunk_is_tuned == 0);
    return rc;
}

/*
 * A lockout reprobe re-admitted on the retained carrier keeps the armed
 * countdown, so it cannot extend the channel hold. Its acquisition window is
 * therefore whichever deadline comes first, and the grant timeout still has to
 * run: with a long configured hangtime, a reprobe whose voice never arrives
 * would otherwise hold the carrier for the whole hangtime instead of giving it
 * up after grant_timeout_s.
 */
static int
test_reprobe_still_answers_to_grant_timeout(void) {
    int rc = 0;
    reset_test_state(30.0f, /*tune_enc_calls*/ 0);
    start_tuned_tdma();
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();

    transmit(0, CLEAR_TG, CLEAR_SRC, 0x11U);
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE2, 0, 0x80, 0, 0, CLEAR_TG);
    grant_slot1(ENC_TG, ENC_SRC, 0x40);
    transmit(1, ENC_TG, ENC_SRC, 0x22U);
    resolve_enc_blocked();
    rc |= expect("enc slot classifies BLOCKED", g_state.p25_crypto_state[1] == DSD_P25_CRYPTO_BLOCKED);

    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, CLEAR_TG, CLEAR_SRC, dsd_time_now_monotonic_s());
    p25_crypto_reset_slot(&g_state, 0);
    const double armed_m = ctx->t_hangtime_m;
    rc |= expect("clear END arms hangtime", armed_m > 0.0);

    grant_slot1(ENC_TG, ENC_SRC, 0x00);
    rc |= expect("clear-claiming grant re-admits the reprobe", ctx->slots[1].grant_active == 1);
    rc |= expect("reprobe preserves the armed countdown", fabs(ctx->t_hangtime_m - armed_m) <= 1.0e-9);

    /* No voice follows. The 30s hangtime is nowhere near expiring, so only the
     * grant timeout can end this. */
    const double timed_out_m = dsd_time_now_monotonic_s() - (ctx->config.grant_timeout_s + 0.5);
    ctx->t_tune_m = timed_out_m;
    ctx->slots[1].last_grant_m = timed_out_m;
    rc |= expect("hangtime is nowhere near expiry",
                 (dsd_time_now_monotonic_s() - ctx->t_hangtime_m) < (double)g_opts.trunk_hangtime);
    p25_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect("reprobe without voice gives the carrier up", g_opts.trunk_is_tuned == 0);
    return rc;
}

/*
 * The site repeats a clear-claiming grant update for the life of the encrypted
 * call it is wrong about. The first one buys a reprobe -- that is the designed
 * recovery for a talkgroup that stopped encrypting -- but once that reprobe
 * re-locks, the next identical update must not buy another. The ledger entry
 * the reprobe consumed is gone by then, so the backoff is what tells the two
 * apart, and without it the carrier cycles tune/classify/release once per
 * update.
 */
static int
test_failed_reprobe_is_not_repeated_per_grant_update(void) {
    int rc = 0;
    reset_test_state(2.0f, /*tune_enc_calls*/ 0);
    start_tuned_tdma();
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();

    transmit(0, CLEAR_TG, CLEAR_SRC, 0x11U);
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE2, 0, 0x80, 0, 0, CLEAR_TG);
    grant_slot1(ENC_TG, ENC_SRC, 0x40);
    transmit(1, ENC_TG, ENC_SRC, 0x22U);
    resolve_enc_blocked();
    rc |= expect("enc target locks out", dsd_enc_lockout_lookup(&g_state, (uint32_t)ENC_TG, 1, NULL) == 1);

    /* First clear-claiming update: admitted, and the ledger entry is spent. */
    grant_slot1(ENC_TG, ENC_SRC, 0x00);
    rc |= expect("first clear-claiming update re-admits the target", ctx->slots[1].grant_active == 1);
    rc |= expect("first re-admission consumes the ledger entry",
                 dsd_enc_lockout_lookup(&g_state, (uint32_t)ENC_TG, 1, NULL) == 0);

    /* The reprobe's own voice re-locks it: the grant bit was wrong. */
    resolve_enc_blocked();
    rc |=
        expect("failed reprobe re-locks the target", dsd_enc_lockout_lookup(&g_state, (uint32_t)ENC_TG, 1, NULL) == 1);
    rc |= expect("failed reprobe drops the assignment", ctx->slots[1].grant_active == 0);

    /* The site repeats the same claim a moment later, first as another
     * assignment and then as the grant update the control channel actually
     * favours -- the provenance the reported stall was driven by. */
    grant_slot1(ENC_TG, ENC_SRC, 0x00);
    rc |= expect("repeat assignment does not re-admit the target", ctx->slots[1].grant_active == 0);
    rc |= expect("repeat assignment leaves the ledger entry armed",
                 dsd_enc_lockout_lookup(&g_state, (uint32_t)ENC_TG, 1, NULL) == 1);

    grant_slot1_update(ENC_TG, ENC_SRC, 0x00);
    rc |= expect("repeat grant update does not re-admit the target", ctx->slots[1].grant_active == 0);
    rc |= expect("repeat grant update leaves the ledger entry armed",
                 dsd_enc_lockout_lookup(&g_state, (uint32_t)ENC_TG, 1, NULL) == 1);
    return rc;
}

/*
 * In follow mode the ledger is suspended, not erased: entries survive a
 * temporary toggle to following encrypted calls without owing a fresh probe.
 * A clear-claiming grant arriving during that window must not quietly erase
 * one, or turning lockout back on starts from an empty ledger. handle_enc's
 * sibling release is gated the same way.
 */
static int
test_follow_mode_does_not_erase_the_ledger(void) {
    int rc = 0;
    reset_test_state(2.0f, /*tune_enc_calls*/ 0);
    start_tuned_tdma();

    /* Armed while lockout was on; the user then switched to following. */
    p25_sm_note_encrypted_call_typed(&g_opts, &g_state, ENC_TG, 1, ENC_ALGID, ENC_KEYID);
    rc |= expect("seeded entry is present", dsd_enc_lockout_lookup(&g_state, (uint32_t)ENC_TG, 1, NULL) == 1);
    g_opts.trunk_tune_enc_calls = 1;

    grant_slot1(ENC_TG, ENC_SRC, 0x00);
    rc |= expect("follow mode admits the call", p25_sm_get_ctx()->slots[1].grant_active == 1);
    rc |= expect("follow mode leaves the ledger entry alone",
                 dsd_enc_lockout_lookup(&g_state, (uint32_t)ENC_TG, 1, NULL) == 1);
    return rc;
}

/*
 * The reprobe penalty answers "was this target blocking?", which is not what
 * "did the ledger have an entry?" answers. New key material leaves entries
 * behind at a stale epoch: they no longer block anything, so a grant for one is
 * admitted on its own merits and is ordinary followed traffic -- charging it the
 * reprobe penalty releases a call the user can now decrypt one hangtime early.
 */
static int
test_stale_epoch_entry_is_not_a_reprobe(void) {
    int rc = 0;
    reset_test_state(2.0f, /*tune_enc_calls*/ 0);
    start_tuned_tdma();
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();

    transmit(0, CLEAR_TG, CLEAR_SRC, 0x11U);
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE2, 0, 0x80, 0, 0, CLEAR_TG);
    p25_sm_note_encrypted_call_typed(&g_opts, &g_state, ENC_TG, 1, ENC_ALGID, ENC_KEYID);

    /* The user loads key material: the entry survives but stops blocking. */
    dsd_enc_lockout_bump_key_epoch(&g_state);
    rc |= expect("key change leaves the entry stale",
                 dsd_enc_lockout_lookup(&g_state, (uint32_t)ENC_TG, 1, NULL) == 1
                     && dsd_enc_lockout_entry_active(&g_state, (uint32_t)ENC_TG, 1) == 0);

    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, CLEAR_TG, CLEAR_SRC, dsd_time_now_monotonic_s());
    p25_crypto_reset_slot(&g_state, 0);
    rc |= expect("clear END arms hangtime", ctx->t_hangtime_m > 0.0);

    grant_slot1(ENC_TG, ENC_SRC, 0x00);
    rc |= expect("stale-epoch target is admitted", ctx->slots[1].grant_active == 1);
    rc |= expect("nothing was blocking, so nothing is a reprobe", ctx->t_hangtime_m <= 0.0);
    return rc;
}

/*
 * Releasing the ledger on a clear-claiming grant is a one-shot: the entry is
 * gone afterwards, and with it any way to tell that the call was ever locked
 * out. Only the path that goes on to process the assignment may spend it. The
 * policy-only evaluation -- which runs on every Phase 1 MBT group grant while
 * the SM is already tuned -- would otherwise consume the release and drop the
 * reprobe marking on the floor.
 */
static int
test_policy_only_evaluation_does_not_spend_the_ledger(void) {
    int rc = 0;
    reset_test_state(2.0f, /*tune_enc_calls*/ 0);
    start_tuned_tdma();

    p25_sm_note_encrypted_call_typed(&g_opts, &g_state, ENC_TG, 1, ENC_ALGID, ENC_KEYID);
    rc |= expect("enc target locks out", dsd_enc_lockout_entry_active(&g_state, (uint32_t)ENC_TG, 1) == 1);

    p25_sm_apply_group_grant_policy(&g_opts, &g_state, TDMA_CH_SLOT1, 0x00, ENC_TG, ENC_SRC);
    rc |= expect("policy-only evaluation leaves the ledger entry armed",
                 dsd_enc_lockout_entry_active(&g_state, (uint32_t)ENC_TG, 1) == 1);
    return rc;
}

/*
 * Corroborated key material is not the same evidence. An active patch clear key
 * is what p25_lcw.c already trusts to reopen the Phase 1 audio gate, so a grant
 * carrying it releases the ledger on any path, owes no reprobe penalty -- the
 * call is followed traffic from its first frame, and cancels the countdown like
 * one -- and never serves the reprobe backoff.
 */
static int
test_patch_clear_key_release_is_not_a_reprobe(void) {
    int rc = 0;
    reset_test_state(2.0f, /*tune_enc_calls*/ 0);
    start_tuned_tdma();
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();

    transmit(0, CLEAR_TG, CLEAR_SRC, 0x11U);
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE2, 0, 0x80, 0, 0, CLEAR_TG);
    p25_sm_note_encrypted_call_typed(&g_opts, &g_state, ENC_TG, 1, ENC_ALGID, ENC_KEYID);
    rc |= expect("enc target locks out", dsd_enc_lockout_lookup(&g_state, (uint32_t)ENC_TG, 1, NULL) == 1);

    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, CLEAR_TG, CLEAR_SRC, dsd_time_now_monotonic_s());
    p25_crypto_reset_slot(&g_state, 0);
    rc |= expect("clear END arms hangtime", ctx->t_hangtime_m > 0.0);

    /* The regroup announces an explicitly clear key for the target, and the
     * grant still claims encryption. */
    p25_patch_set_kas(&g_state, ENC_TG, /*key*/ 0, /*alg*/ ENC_ALGID, /*ssn*/ 1);
    grant_slot1(ENC_TG, ENC_SRC, 0x40);
    rc |= expect("patch clear key admits the call", ctx->slots[1].grant_active == 1);
    rc |= expect("patch clear key releases the ledger entry",
                 dsd_enc_lockout_lookup(&g_state, (uint32_t)ENC_TG, 1, NULL) == 0);
    rc |= expect("patch clear key is followed traffic, not a reprobe", ctx->t_hangtime_m <= 0.0);

    /* And it is not spending a reprobe, so a re-lock does not lock it out of
     * its next grant. */
    p25_sm_note_encrypted_call_typed(&g_opts, &g_state, ENC_TG, 1, ENC_ALGID, ENC_KEYID);
    grant_slot1(ENC_TG, ENC_SRC, 0x40);
    rc |= expect("patch clear key is never held off by the reprobe backoff",
                 dsd_enc_lockout_lookup(&g_state, (uint32_t)ENC_TG, 1, NULL) == 0);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_p1_identity_wait_rearms_hangtime();
    rc |= test_followed_voice_start_cancels_hangtime();
    rc |= test_locked_out_repeats_do_not_arm_beside_a_live_companion();
    rc |= test_lockout_enabled_mid_call_arms_once();
    rc |= test_pending_classification_outranks_hangtime();
    rc |= test_reprobe_still_answers_to_grant_timeout();
    rc |= test_failed_reprobe_is_not_repeated_per_grant_update();
    rc |= test_follow_mode_does_not_erase_the_ledger();
    rc |= test_stale_epoch_entry_is_not_a_reprobe();
    rc |= test_policy_only_evaluation_does_not_spend_the_ledger();
    rc |= test_patch_clear_key_release_is_not_a_reprobe();
    dsd_state_ext_free_all(&g_state);

    if (rc == 0) {
        DSD_FPRINTF(stderr, "P25 HANGTIME ARM RULES: OK\n");
    }
    return rc;
}
