// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A lapsed crypto classification on one slot of a shared TDMA carrier must
 * not tear the channel down while the other slot's conversation sits inside
 * its hangtime gap: the retains-carrier signals (voice_active, audio gate,
 * ring) all read a talker gap as inactive, but the gap is exactly what the
 * hangtime window promises to bridge, and classification is itself evidence
 * a call may still resolve clear. The hold mirrors the hangtime bridge the
 * enc-lockout release path already honors; the hangtime tick still owns the
 * release once the gap outlives the window. The plain pending-voice-grant
 * timeout keeps its immediate release by design (the carrier's stay is the
 * shorter of the companion bridge and the assignment's acquisition window),
 * which the idle-companion case below exercises.
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int g_return_to_cc_called = 0;

static dsd_trunk_tune_result
return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    (void)opts;
    (void)state;
    g_return_to_cc_called++;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.return_to_cc_request = return_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
}

static int
expect_eq(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

// Tuned TDMA carrier whose slot-0 conversation just went quiet: the talker
// un-keyed a moment ago, so voice_active, the audio gate, and the ring are all
// clear, but the stop is fresh inside the hangtime window the SM promises to
// bridge.
static void
setup_clear_gap_on_slot0(dsd_opts* opts, dsd_state* state, p25_sm_ctx_t** out_ctx, double now_m) {
    DSD_MEMSET(opts, 0, sizeof *opts);
    DSD_MEMSET(state, 0, sizeof *state);
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->trunk_tune_enc_calls = 0;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_hangtime = 2.0f;
    state->currentslot = 0;
    state->synctype = DSD_SYNC_P25P2_POS;
    state->lastsynctype = DSD_SYNC_P25P2_POS;

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(ctx, opts, state);
    *out_ctx = ctx;
    ctx->state = P25_SM_TUNED;
    ctx->vc_is_tdma = 1;
    ctx->vc_freq_hz = 851500000;
    ctx->config.hangtime_s = 2.0;
    ctx->config.grant_timeout_s = 3.0;
    ctx->t_tune_m = now_m - 60.0;
    ctx->vc_activity_seen = 1;

    p25_sm_slot_ctx_t* clear_slot = &ctx->slots[0];
    clear_slot->grant_active = 0;
    clear_slot->voice_active = 0;
    clear_slot->is_group = 1;
    clear_slot->ota_tg = 1234;
    clear_slot->target_id = 1234;
    clear_slot->last_grant_m = now_m - 10.0;
    clear_slot->last_start_m = now_m - 8.0;
    clear_slot->last_stop_m = now_m - 0.3;
    clear_slot->last_followed_m = now_m - 0.3;
}

static void
age_clear_gap_past_hangtime(p25_sm_ctx_t* ctx, double now_m) {
    const double past_m = now_m - (ctx->config.hangtime_s + 1.0);
    ctx->slots[0].last_stop_m = past_m;
    ctx->slots[0].last_followed_m = past_m;
    ctx->slots[0].last_start_m = past_m - 1.0;
    ctx->slots[0].last_grant_m = past_m - 1.0;
}

// Crypto-classification timeout: slot 1's encryption classification never
// resolved (no FEC-accepted ESS) and its budget lapses inside slot 0's gap.
static int
test_crypto_timeout_holds_through_companion_gap(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    const double now_m = dsd_time_now_monotonic_s();
    int rc = 0;

    setup_clear_gap_on_slot0(&opts, &state, &ctx, now_m);
    p25_sm_slot_ctx_t* classifying_slot = &ctx->slots[1];
    classifying_slot->grant_active = 1;
    classifying_slot->is_group = 1;
    classifying_slot->ota_tg = 30601;
    classifying_slot->target_id = 30601;
    classifying_slot->last_grant_m = now_m - (ctx->config.grant_timeout_s + 0.5);
    classifying_slot->crypto_attempt_m = now_m - (ctx->config.grant_timeout_s + 0.5);
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;
    g_return_to_cc_called = 0;

    p25_sm_tick_ctx(ctx, &opts, &state);

    rc |= expect_eq("crypto timeout in gap: no cc return", g_return_to_cc_called, 0);
    rc |= expect_eq("crypto timeout in gap: still tuned", ctx->state == P25_SM_TUNED, 1);
    rc |= expect_eq("crypto timeout in gap: classification dismantled", state.p25_crypto_state[1],
                    DSD_P25_CRYPTO_UNKNOWN);
    rc |= expect_eq("crypto timeout in gap: stale assignment cleared", ctx->slots[1].grant_active, 0);

    age_clear_gap_past_hangtime(ctx, dsd_time_now_monotonic_s());
    p25_sm_tick_ctx(ctx, &opts, &state);
    rc |= expect_eq("crypto timeout past gap: released to cc", g_return_to_cc_called, 1);

    dsd_state_ext_free_all(&state);
    return rc;
}

// With no companion history at all (the other slot never carried a call), both
// timeouts keep their immediate release: nothing is waiting out a gap.
static int
test_timeouts_still_release_without_companion_history(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    const double now_m = dsd_time_now_monotonic_s();
    int rc = 0;

    setup_clear_gap_on_slot0(&opts, &state, &ctx, now_m);
    // Erase slot 0's conversation history: idle companion.
    ctx->slots[0].last_grant_m = 0.0;
    ctx->slots[0].last_start_m = 0.0;
    ctx->slots[0].last_stop_m = 0.0;
    ctx->slots[0].last_followed_m = 0.0;
    ctx->slots[0].ota_tg = 0;
    ctx->slots[0].target_id = 0;

    p25_sm_slot_ctx_t* pending_slot = &ctx->slots[1];
    pending_slot->grant_active = 1;
    pending_slot->is_group = 1;
    pending_slot->ota_tg = 30601;
    pending_slot->target_id = 30601;
    pending_slot->last_grant_m = now_m - (ctx->config.grant_timeout_s + 0.5);
    g_return_to_cc_called = 0;

    p25_sm_tick_ctx(ctx, &opts, &state);
    rc |= expect_eq("idle companion: released to cc", g_return_to_cc_called, 1);

    dsd_state_ext_free_all(&state);
    return rc;
}

int
main(void) {
    install_trunk_tuning_hooks();

    int rc = 0;
    rc |= test_crypto_timeout_holds_through_companion_gap();
    rc |= test_timeouts_still_release_without_companion_history();
    if (rc == 0) {
        DSD_FPRINTF(stderr, "P25 P2 TICK COMPANION HOLD: OK\n");
    }
    return rc;
}
