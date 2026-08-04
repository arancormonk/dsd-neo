// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Validate the P25 Phase 2 frame-level service-option encrypted mute does not
 * force trunk state-machine release before ESS has identified the ALGID.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

void process_2V(dsd_opts* opts, dsd_state* state);

// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_socket_t Connect(char* hostname, int portno);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state);
static int g_return_to_cc_called = 0;

dsd_socket_t
Connect(char* hostname, int portno) {
    (void)hostname;
    (void)portno;
    return DSD_INVALID_SOCKET;
}

static dsd_trunk_tune_result
return_to_cc_result(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    (void)opts;
    (void)state;
    g_return_to_cc_called++;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.return_to_cc_request = return_to_cc_result;
    dsd_trunk_tuning_hooks_set(hooks);
}

void
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

void
LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state) {
    (void)buffer_in;
    (void)buffer_out;
    (void)state;
}

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
seed_call(dsd_state* state, uint8_t slot, dsd_call_kind kind, uint64_t target) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = slot,
        .kind = kind,
        .ota_target_id = target,
        .policy_target_id = target,
        .observed_m = 1.0,
    };
    return dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0;
}

static void
setup_tuned_tdma(dsd_opts* opts, dsd_state* state, p25_sm_ctx_t** ctx) {
    DSD_MEMSET(opts, 0, sizeof *opts);
    DSD_MEMSET(state, 0, sizeof *state);
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->trunk_tune_enc_calls = 0;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    state->currentslot = 0;
    state->p25_vc_cqpsk_pref = -1;
    state->p25_vc_cqpsk_override = -1;
    state->lastsynctype = DSD_SYNC_P25P2_POS;
    (void)seed_call(state, 0U, DSD_CALL_KIND_GROUP_VOICE, 1234);
    (void)seed_call(state, 1U, DSD_CALL_KIND_GROUP_VOICE, 5678);

    p25_sm_init_ctx(p25_sm_get_ctx(), opts, state);
    *ctx = p25_sm_get_ctx();
    (*ctx)->state = P25_SM_TUNED;
    (*ctx)->vc_is_tdma = 1;
    (*ctx)->vc_freq_hz = 851000000;
    (*ctx)->t_tune_m = 1.0;
    (*ctx)->t_voice_m = 1.0;
    for (int slot = 0; slot < 2; slot++) {
        p25_sm_slot_ctx_t* slot_ctx = &(*ctx)->slots[slot];
        const int tg = slot == 0 ? 1234 : 5678;
        slot_ctx->grant_active = 1;
        slot_ctx->freq_hz = (*ctx)->vc_freq_hz;
        slot_ctx->channel = 0x2000 | slot;
        slot_ctx->target_id = tg;
        slot_ctx->ota_tg = tg;
        slot_ctx->tg = tg;
        slot_ctx->is_group = 1;
        slot_ctx->svc_bits = 0;
    }
}

static int
test_pre_ess_single_slot_stays_tuned(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);
    state.currentslot = 0;
    state.p25_p2_audio_allowed[0] = 1;
    state.dmr_so = 0x40;
    g_return_to_cc_called = 0;

    process_2V(&opts, &state);

    int rc = 0;
    rc |= expect_eq("pre-ess single slot: no return", g_return_to_cc_called, 0);
    rc |= expect_eq("pre-ess single slot: still tuned", ctx->state == P25_SM_TUNED, 1);
    rc |= expect_eq("pre-ess single slot: pending voice stays inactive", ctx->slots[0].voice_active, 0);
    rc |= expect_eq("pre-ess single slot: gate closed", state.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq("pre-ess single slot: pending crypto state", state.p25_crypto_state[0],
                    DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_eq("pre-ess single slot: deadline started", ctx->slots[0].crypto_attempt_m > 0.0, 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_clear_voice_to_encrypted_restarts_deadline(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);
    state.currentslot = 0;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.p25_p2_audio_allowed[0] = 1;
    state.dmr_so = 0x40;
    ctx->slots[0].grant_active = 1;
    ctx->slots[0].voice_active = 1;
    ctx->slots[0].last_active_m = 2.0;
    ctx->slots[0].crypto_attempt_m = 1.0;

    process_2V(&opts, &state);

    int rc = 0;
    rc |= expect_eq("clear-to-encrypted: pending crypto state", state.p25_crypto_state[0],
                    DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_eq("clear-to-encrypted: gate closed", state.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq("clear-to-encrypted: voice activity cleared", ctx->slots[0].voice_active, 0);
    rc |= expect_eq("clear-to-encrypted: stale deadline replaced", ctx->slots[0].crypto_attempt_m > 1.0, 1);

    const double started_m = ctx->slots[0].crypto_attempt_m;
    process_2V(&opts, &state);
    rc |= expect_eq("clear-to-encrypted: repeated SVC keeps deadline",
                    fabs(ctx->slots[0].crypto_attempt_m - started_m) <= 1.0e-9, 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_pre_ess_opposite_clear_slot_stays_tuned(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);
    state.currentslot = 1;
    state.p25_p2_audio_allowed[0] = 1;
    state.p25_p2_audio_allowed[1] = 1;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.dmr_soR = 0x40;
    p25_sm_emit_active(&opts, &state, 0);
    g_return_to_cc_called = 0;

    process_2V(&opts, &state);

    int rc = 0;
    rc |= expect_eq("opposite clear slot: no return", g_return_to_cc_called, 0);
    rc |= expect_eq("opposite clear slot: still tuned", ctx->state == P25_SM_TUNED, 1);
    rc |= expect_eq("opposite clear slot: clear active", ctx->slots[0].voice_active, 1);
    rc |= expect_eq("opposite clear slot: pending ess voice inactive", ctx->slots[1].voice_active, 0);
    rc |= expect_eq("opposite clear slot: clear gate open", state.p25_p2_audio_allowed[0], 1);
    rc |= expect_eq("opposite clear slot: locked gate closed", state.p25_p2_audio_allowed[1], 0);
    rc |= expect_eq("opposite clear slot: pending crypto state", state.p25_crypto_state[1],
                    DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_clear_regroup_override_survives_voice_burst(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);
    state.currentslot = 0;
    state.dmr_so = 0x40;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.p25_p2_audio_allowed[0] = 1;
    p25_patch_update(&state, 1234, /*is_patch*/ 1, /*active*/ 1);
    p25_patch_set_kas(&state, 1234, /*key*/ 0, /*alg*/ 0x84, /*ssn*/ 1);

    process_2V(&opts, &state);

    int rc = 0;
    rc |= expect_eq("clear regroup: crypto remains clear", state.p25_crypto_state[0], DSD_P25_CRYPTO_CLEAR);
    rc |= expect_eq("clear regroup: audio gate remains open", state.p25_p2_audio_allowed[0], 1);
    rc |= expect_eq("clear regroup: voice activity emitted", ctx->slots[0].voice_active, 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_private_voice_ignores_regroup_clear_key_collision(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);
    state.currentslot = 0;
    (void)seed_call(&state, 0U, DSD_CALL_KIND_PRIVATE_VOICE, 0x123456);
    state.dmr_so = 0x40;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.p25_p2_audio_allowed[0] = 1;
    p25_patch_add_wgid(&state, 0x2222, 0x3456);
    p25_patch_set_kas(&state, 0x2222, /*key*/ 0, /*alg*/ 0x84, /*ssn*/ 1);

    process_2V(&opts, &state);

    int rc = 0;
    rc |= expect_eq("private patch collision: crypto pending", state.p25_crypto_state[0],
                    DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_eq("private patch collision: audio gate closed", state.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq("private patch collision: voice activity suppressed", ctx->slots[0].voice_active, 0);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_encrypted_follow_tracks_activity_while_media_is_muted(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);
    opts.trunk_tune_enc_calls = 1;
    state.currentslot = 0;
    state.dmr_so = 0x40;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;
    state.p25_p2_audio_allowed[0] = 0;
    ctx->slots[0].svc_bits = 0x40;

    p25_sm_emit_ptt(&opts, &state, 0);

    int rc = 0;
    rc |= expect_eq("encrypted follow PTT marks activity", ctx->slots[0].voice_active, 1);
    rc |= expect_eq("encrypted follow PTT keeps media muted", state.p25_p2_audio_allowed[0], 0);

    ctx->slots[0].voice_active = 0;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    process_2V(&opts, &state);

    rc |= expect_eq("encrypted follow voice frame marks activity", ctx->slots[0].voice_active, 1);
    rc |= expect_eq("encrypted follow voice frame keeps media muted", state.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq("encrypted follow voice frame stays tuned", ctx->state == P25_SM_TUNED, 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_new_transmission_inherits_fresh_clear_grant_service(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);

    // A transmission completed on slot 0 and the CC re-announced the
    // assignment (grant update with explicit-clear service options) after the
    // stop. The retained service bits classify the next transmission clear
    // instead of starting it encryption-pending and muting clear voice until
    // an ESS decodes -- the audible chop under encryption lockout in fades.
    ctx->slots[0].svc_bits = 0x04;
    ctx->slots[0].last_start_m = 1.0;
    ctx->slots[0].last_stop_m = 2.0;
    ctx->slots[0].last_grant_m = 3.0;
    ctx->slots[0].voice_active = 0;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_UNKNOWN;

    p25_sm_emit_ptt(&opts, &state, 0);

    int rc = 0;
    rc |= expect_eq("fresh clear grant: next transmission classified clear", state.p25_crypto_state[0],
                    DSD_P25_CRYPTO_CLEAR);
    rc |= expect_eq("fresh clear grant: voice activity accepted", ctx->slots[0].voice_active, 1);
    rc |= expect_eq("fresh clear grant: no classification deadline", ctx->slots[0].crypto_attempt_m == 0.0, 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_new_transmission_without_grant_revalidation_stays_pending(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);

    // No grant re-validated the assignment since the stop: the next
    // transmission may not inherit the preceding epoch's service options and
    // must wait for ESS/LCW to classify it.
    ctx->slots[0].svc_bits = 0x04;
    ctx->slots[0].last_start_m = 1.0;
    ctx->slots[0].last_stop_m = 3.0;
    ctx->slots[0].last_grant_m = 2.0;
    ctx->slots[0].voice_active = 0;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_UNKNOWN;

    p25_sm_emit_ptt(&opts, &state, 0);

    int rc = 0;
    rc |= expect_eq("stale grant service: next transmission stays pending", state.p25_crypto_state[0],
                    DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_eq("stale grant service: classification deadline armed", ctx->slots[0].crypto_attempt_m > 0.0, 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

// A locked-out encrypted call re-runs its stay-or-release decision on every
// FEC-accepted ESS repeat (~once per superframe). The companion's clear
// conversation is a series of transmissions with gaps that MAC_END opens by
// clearing voice_active and the audio gate -- the exact state the
// retains-carrier check reads as "inactive". The hangtime window owns those
// gaps: a repeat inside it must hold the channel, a repeat after it lapses
// must still release.
static int
test_enc_repeat_holds_channel_through_companion_hangtime(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);
    // The zeroed opts request hangtime 0 (immediate release); this case is
    // about the window itself, so give it the stock op25-aligned value.
    ctx->config.hangtime_s = 2.0;
    state.event_history_s = calloc(2, sizeof(Event_History_I));
    if (state.event_history_s == NULL) {
        return 1;
    }
    for (int i = 0; i < 2; i++) {
        init_event_history(&state.event_history_s[i], 0, 255);
    }
    g_return_to_cc_called = 0;

    int rc = 0;

    // Clear transmission decoding on slot 0.
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    rc |= expect_eq("clear voice start accepted", p25_sm_emit_active_call(&opts, &state, 0, 1234, 0, 42, 1, 0x00), 1);
    state.p25_p2_audio_allowed[0] = 1;

    // The slot-1 ESS classifies BLOCKED and the lockout action runs; the
    // active companion holds the channel.
    (void)p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 1, 0x84, 0x2710, 0x1111222233334444ULL, 5678);
    rc |= expect_eq("companion active: slot1 blocked", state.p25_crypto_state[1] == DSD_P25_CRYPTO_BLOCKED, 1);
    rc |= expect_eq("companion active: still tuned", ctx->state == P25_SM_TUNED, 1);
    rc |= expect_eq("companion active: no cc return", g_return_to_cc_called, 0);

    // The clear talker un-keys; the conversation gap opens well inside the
    // hangtime window.
    rc |= expect_eq("clear transmission end accepted",
                    p25_sm_emit_end_call_at(&opts, &state, 0, 1234, 42, dsd_time_now_monotonic_s()), 1);
    state.p25_p2_audio_allowed[0] = 0;

    // The locked-out call's next ESS repeat re-runs the lockout action inside
    // the companion's gap. It must not tear the channel down.
    p25_sm_emit_enc(&opts, &state, 1, 0x84, 0x2710, 5678);
    rc |= expect_eq("gap inside hangtime: still tuned", ctx->state == P25_SM_TUNED, 1);
    rc |= expect_eq("gap inside hangtime: no cc return", g_return_to_cc_called, 0);
    rc |= expect_eq("gap inside hangtime: tuner still on vc", opts.trunk_is_tuned, 1);

    // Once the companion's gap outlives the hangtime, the repeat releases.
    // Age the whole slot history together: a grant newer than the stop is a
    // pending assignment and retains the carrier on its own.
    const double past_stop_m = dsd_time_now_monotonic_s() - (ctx->config.hangtime_s + 1.0);
    ctx->slots[0].last_start_m = past_stop_m - 1.0;
    ctx->slots[0].last_grant_m = past_stop_m - 1.0;
    ctx->slots[0].last_stop_m = past_stop_m;
    p25_sm_emit_enc(&opts, &state, 1, 0x84, 0x2710, 5678);
    rc |= expect_eq("gap past hangtime: released to cc", g_return_to_cc_called, 1);

    dsd_state_ext_free_all(&state);
    free(state.event_history_s);
    state.event_history_s = NULL;
    return rc;
}

// The lockout release for a carrier whose companion never carried a call (no
// recorded stop) keeps its immediate release: nothing is waiting out a gap.
static int
test_enc_lockout_idle_companion_still_releases(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);
    // Nonzero hangtime proves the release below is not the zero-window case.
    ctx->config.hangtime_s = 2.0;
    state.event_history_s = calloc(2, sizeof(Event_History_I));
    if (state.event_history_s == NULL) {
        return 1;
    }
    for (int i = 0; i < 2; i++) {
        init_event_history(&state.event_history_s[i], 0, 255);
    }
    g_return_to_cc_called = 0;

    // Slot 0 idle with no assignment or call history.
    ctx->slots[0].grant_active = 0;

    (void)p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 1, 0x84, 0x2710, 0x1111222233334444ULL, 5678);

    int rc = 0;
    rc |= expect_eq("idle companion: released to cc", g_return_to_cc_called, 1);

    dsd_state_ext_free_all(&state);
    free(state.event_history_s);
    state.event_history_s = NULL;
    return rc;
}

// The FACCH double-END fast release infers "channel closing" from an
// otherwise idle companion slot, but encryption lockout erases the suppressed
// call's assignment, epoch, and audio gate -- everything that check reads --
// while the site keeps transmitting the locked-out call on the carrier. A
// double END from the clear conversation must hold the channel while the
// companion's suppression stamp is fresh, and must still release once the
// stamp outlives the hangtime.
static int
test_facch_double_end_holds_channel_while_companion_enc_suppressed(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);
    ctx->config.hangtime_s = 2.0;
    state.event_history_s = calloc(2, sizeof(Event_History_I));
    if (state.event_history_s == NULL) {
        return 1;
    }
    for (int i = 0; i < 2; i++) {
        init_event_history(&state.event_history_s[i], 0, 255);
    }
    g_return_to_cc_called = 0;

    int rc = 0;

    // Clear transmission decoding on slot 0.
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    rc |= expect_eq("clear voice start accepted", p25_sm_emit_active_call(&opts, &state, 0, 1234, 0, 42, 1, 0x00), 1);
    state.p25_p2_audio_allowed[0] = 1;

    // Slot 1 carries the locked-out encrypted call: its ESS classifies BLOCKED
    // and the lockout action erases the slot's assignment and audio trace,
    // leaving only the suppression stamp as evidence the carrier is occupied.
    (void)p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 1, 0x84, 0x2710, 0x1111222233334444ULL, 5678);
    rc |= expect_eq("companion suppressed: still tuned", ctx->state == P25_SM_TUNED, 1);
    rc |= expect_eq("companion suppressed: stamp recorded", ctx->slots[1].last_enc_suppress_m > 0.0, 1);

    // The clear talker un-keys; the FACCH END repeat lands 0.2 s later while
    // the locked-out call is still transmitting. Reading that repeat as the
    // channel closing would bounce to the CC and clip the conversation's next
    // over, which the site re-grants on this same channel moments later.
    const double end_m = dsd_time_now_monotonic_s();
    rc |= expect_eq("first facch end applied", p25_sm_emit_facch_end_call_at(&opts, &state, 0, 1234, 42, end_m),
                    P25_SM_END_APPLIED);
    state.p25_p2_audio_allowed[0] = 0;
    (void)p25_sm_emit_facch_end_call_at(&opts, &state, 0, 1234, 42, end_m + 0.2);
    rc |= expect_eq("double end inside suppression: still tuned", ctx->state == P25_SM_TUNED, 1);
    rc |= expect_eq("double end inside suppression: no cc return", g_return_to_cc_called, 0);
    rc |= expect_eq("double end inside suppression: tuner still on vc", opts.trunk_is_tuned, 1);

    // Once the suppression stamp outlives the hangtime -- the locked-out call
    // stopped repeating -- the same double END releases as before.
    ctx->slots[1].last_enc_suppress_m = end_m - (ctx->config.hangtime_s + 1.0);
    (void)p25_sm_emit_facch_end_call_at(&opts, &state, 0, 1234, 42, end_m + 0.4);
    rc |= expect_eq("double end past suppression: released to cc", g_return_to_cc_called, 1);

    dsd_state_ext_free_all(&state);
    free(state.event_history_s);
    state.event_history_s = NULL;
    return rc;
}

int
main(void) {
    install_trunk_tuning_hooks();

    int rc = 0;
    rc |= test_pre_ess_single_slot_stays_tuned();
    rc |= test_clear_voice_to_encrypted_restarts_deadline();
    rc |= test_pre_ess_opposite_clear_slot_stays_tuned();
    rc |= test_clear_regroup_override_survives_voice_burst();
    rc |= test_private_voice_ignores_regroup_clear_key_collision();
    rc |= test_encrypted_follow_tracks_activity_while_media_is_muted();
    rc |= test_new_transmission_inherits_fresh_clear_grant_service();
    rc |= test_new_transmission_without_grant_revalidation_stays_pending();
    rc |= test_enc_repeat_holds_channel_through_companion_hangtime();
    rc |= test_enc_lockout_idle_companion_still_releases();
    rc |= test_facch_double_end_holds_channel_while_companion_enc_suppressed();
    return rc;
}
