// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Encryption lockout on a TDMA voice channel shared with a followed clear
 * call: the locked-out call keeps signaling for its whole transmission --
 * clear-claiming grant updates that re-admit it as a probe, suppressed MAC
 * voice starts, ESS repeats. None of that is followed traffic, so none of it
 * may reset or disable the hangtime countdown armed when the clear companion
 * ended; otherwise the channel only releases once the encrypted call stops,
 * instead of returning to the control channel one hangtime after the last
 * followed call ended.
 */

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

#define CLEAR_TG  40601
#define CLEAR_SRC 600205
#define ENC_TG    40699
#define ENC_SRC   600299
#define ENC_ALGID 0x84
#define ENC_KEYID 0x1234
#define VC_FREQ   851500000L

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
    g_opts.trunk_tune_enc_calls = 0; /* encryption lockout enabled */
    g_state.p25_cc_freq = 851000000;
    g_state.lastsynctype = DSD_SYNC_P25P2_POS;
    g_state.synctype = DSD_SYNC_P25P2_POS;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
}

static void
start_tuned_tdma(void) {
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    g_state.trunk_chan_map[0x1234] = VC_FREQ;
    g_state.trunk_chan_map[0x1235] = VC_FREQ;
    g_state.p25_chan_tdma_explicit[1] = 2;
    p25_sm_init_ctx(ctx, &g_opts, &g_state);
    g_opts.trunk_is_tuned = 1;
    p25_sm_event_t grant = p25_sm_ev_group_grant(0x1234, VC_FREQ, CLEAR_TG, CLEAR_SRC, 0);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
}

static void
grant_companion(int tg, int src, int svc_bits) {
    p25_sm_event_t grant = p25_sm_ev_group_grant(0x1235, VC_FREQ, tg, src, svc_bits);
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

static double
backdate(double stamp_m, double by_s) {
    return stamp_m > 0.0 ? stamp_m - by_s : stamp_m;
}

/*
 * Re-stamp the clear companion's stop to now. The re-lock's stay-or-release
 * action compares that stamp against the configured hangtime with the real
 * clock, so without this the "holds within companion gap" assertions would
 * depend on the test itself running in under trunk_hangtime seconds -- which a
 * parallel ctest, a sanitizer build, or a throttled container need not.
 */
static void
restamp_companion_stop_now(int slot) {
    p25_sm_get_ctx()->slots[slot].last_stop_m = dsd_time_now_monotonic_s();
}

/*
 * Age out only the hangtime countdown and the stamps that keep the locked-out
 * slot looking occupied. t_tune_m and crypto_attempt_m stay fresh on purpose:
 * the grant timeout and the crypto-classification timeout can then not fire, so
 * a release observed after this can only have come from the hangtime tick the
 * test names.
 */
static void
expire_hangtime_only(void) {
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    const double by_s = (double)g_opts.trunk_hangtime + 0.5;
    ctx->t_hangtime_m = backdate(ctx->t_hangtime_m, by_s);
    ctx->t_voice_m = backdate(ctx->t_voice_m, by_s);
    ctx->slots[0].last_stop_m = backdate(ctx->slots[0].last_stop_m, by_s);
    ctx->slots[0].last_end_m = backdate(ctx->slots[0].last_end_m, by_s);
    ctx->slots[1].last_stop_m = backdate(ctx->slots[1].last_stop_m, by_s);
    ctx->slots[1].last_grant_m = backdate(ctx->slots[1].last_grant_m, by_s);
    ctx->slots[1].last_enc_suppress_m = backdate(ctx->slots[1].last_enc_suppress_m, by_s);
}

/*
 * Slot 0 follows a clear call while slot 1 carries a locked-out encrypted
 * transmission that outlives it. After the clear END arms the hangtime
 * countdown, the locked-out call's continuing signaling -- a clear-claiming
 * grant update that re-admits it as a probe, plus suppressed MAC voice starts
 * -- must leave the countdown running so the tick can release the channel
 * mid-transmission once the hangtime elapses.
 */
static int
test_enc_signaling_does_not_starve_hangtime_release(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma();
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();

    /* Followed clear call on slot 0. */
    transmit(0, CLEAR_TG, CLEAR_SRC, 0x11U);
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE2, 0, 0x80, 0, 0, CLEAR_TG);

    /* Encrypted call granted on the companion slot of the same carrier; its
     * ESS classifies BLOCKED and the target locks out. */
    grant_companion(ENC_TG, ENC_SRC, 0x40);
    rc |= expect("enc grant assigns companion slot", ctx->slots[1].grant_active == 1);
    transmit(1, ENC_TG, ENC_SRC, 0x22U);
    resolve_enc_blocked();
    rc |= expect("enc slot classifies BLOCKED", g_state.p25_crypto_state[1] == DSD_P25_CRYPTO_BLOCKED);
    rc |= expect("lockout suppression stamped", ctx->slots[1].last_enc_suppress_m > 0.0);
    rc |= expect("clear companion holds channel", g_opts.trunk_is_tuned == 1);

    /* Clear call ends; the locked-out slot holds no grant, so the hangtime
     * countdown arms. */
    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, CLEAR_TG, CLEAR_SRC, dsd_time_now_monotonic_s());
    p25_crypto_reset_slot(&g_state, 0);
    const double armed_m = ctx->t_hangtime_m;
    rc |= expect("clear END arms hangtime", armed_m > 0.0);

    /* The site's next grant update for the still-transmitting encrypted call
     * claims clear service options, which releases the lockout ledger entry
     * and re-admits the call as a probe. The probe may hold the channel, but
     * it must not erase the armed countdown. */
    grant_companion(ENC_TG, ENC_SRC, 0x00);
    rc |= expect("clear-claiming grant update re-admits probe", ctx->slots[1].grant_active == 1);
    rc |= expect("probe re-grant preserves armed hangtime", fabs(ctx->t_hangtime_m - armed_m) <= 1.0e-9);

    /* The re-admitted probe's ESS contradicts the grant's clear claim right
     * away: the slot classifies BLOCKED, the lockout re-arms, and the
     * stay-or-release action holds for the companion's unexpired gap -- with
     * the countdown still armed and still anchored where the clear END put it. */
    restamp_companion_stop_now(0);
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE2, 1, ENC_ALGID, ENC_KEYID, 0, ENC_TG);
    rc |= expect("probe re-classifies BLOCKED", g_state.p25_crypto_state[1] == DSD_P25_CRYPTO_BLOCKED);
    rc |= expect("enc re-lock holds within companion gap", g_opts.trunk_is_tuned == 1);
    rc |= expect("enc re-lock leaves hangtime armed", fabs(ctx->t_hangtime_m - armed_m) <= 1.0e-9);

    /* The re-lock tore the assignment down with the rest of the slot, so the
     * locked-out call's remaining MAC repeats are dropped before they reach any
     * timing at all -- and the countdown keeps running toward its deadline. */
    for (int repeat = 0; repeat < 3; repeat++) {
        (void)p25_sm_emit_active_call(&g_opts, &g_state, 1, ENC_TG, 0, ENC_SRC, 1, 0x40);
        rc |= expect("post-lockout MAC repeat keeps slot inactive", ctx->slots[1].voice_active == 0);
        rc |= expect("post-lockout MAC repeat preserves armed hangtime", fabs(ctx->t_hangtime_m - armed_m) <= 1.0e-9);
    }

    /* Hangtime elapses while the encrypted transmission continues and its ESS
     * stops decoding (no further classification transitions). Only the tick
     * can release the channel now. */
    expire_hangtime_only();
    p25_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect("hangtime expiry releases channel", g_opts.trunk_is_tuned == 0);
    return rc;
}

/*
 * Same channel, worse phase alignment: the clear-claiming grant update lands
 * before the clear call ends, so the clear END reads the reprobe assignment as
 * pending companion activity and never arms the countdown. The re-lock's hold
 * must arm it from the companion's real stop time so the tick can still
 * release the channel once the gap outlives the hangtime.
 */
static int
test_enc_relock_hold_arms_hangtime_for_ended_companion(void) {
    int rc = 0;
    reset_test_state();
    start_tuned_tdma();
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();

    /* Followed clear call on slot 0; locked-out encrypted call on slot 1. */
    transmit(0, CLEAR_TG, CLEAR_SRC, 0x11U);
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE2, 0, 0x80, 0, 0, CLEAR_TG);
    grant_companion(ENC_TG, ENC_SRC, 0x40);
    transmit(1, ENC_TG, ENC_SRC, 0x22U);
    resolve_enc_blocked();
    rc |= expect("enc slot classifies BLOCKED", g_state.p25_crypto_state[1] == DSD_P25_CRYPTO_BLOCKED);

    /* The clear-claiming grant update re-admits the probe while the clear
     * call is still up; the clear call then ends with that assignment
     * pending, so the END cannot arm the countdown. */
    grant_companion(ENC_TG, ENC_SRC, 0x00);
    rc |= expect("clear-claiming grant update re-admits probe", ctx->slots[1].grant_active == 1);
    (void)p25_sm_emit_end_call_at(&g_opts, &g_state, 0, CLEAR_TG, CLEAR_SRC, dsd_time_now_monotonic_s());
    p25_crypto_reset_slot(&g_state, 0);

    /* The probe's ESS classifies BLOCKED again; the re-lock holds for the
     * companion's unexpired gap and must arm the countdown from the
     * companion's stop, because no further transition will revisit it.
     *
     * The anchor is the assertion: arming from now instead would give the
     * locked-out call a full extra hangtime on every re-lock, which is the
     * failure this arm exists to avoid. */
    restamp_companion_stop_now(0);
    rc |= expect("clear END left the countdown unarmed", ctx->t_hangtime_m <= 0.0);
    resolve_enc_blocked();
    rc |= expect("enc re-lock holds within companion gap", g_opts.trunk_is_tuned == 1);
    rc |= expect("enc re-lock arms hangtime from companion stop",
                 ctx->t_hangtime_m > 0.0 && fabs(ctx->t_hangtime_m - ctx->slots[0].last_stop_m) <= 1.0e-9);

    /* Hangtime elapses with the encrypted transmission still up and its ESS
     * quiet; the tick must release the channel. */
    expire_hangtime_only();
    p25_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect("hangtime expiry releases channel after re-lock", g_opts.trunk_is_tuned == 0);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_enc_signaling_does_not_starve_hangtime_release();
    rc |= test_enc_relock_hold_arms_hangtime_for_ended_companion();
    dsd_state_ext_free_all(&g_state);

    if (rc == 0) {
        DSD_FPRINTF(stderr, "P25 P2 ENC LOCKOUT HANGTIME RELEASE: OK\n");
    }
    return rc;
}
