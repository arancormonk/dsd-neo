// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A followed P25 call keeps its talkgroup and radio ID across a mid-call fade.
 *
 * On a fade longer than the no-sync timeout the engine's no-carrier pass ends both canonical
 * calls as a sync loss (no_carrier_finalize_canonical_calls()) and clears the payload and P25
 * crypto state (dsd_engine_reset_no_carrier_state()), without telling the P25 state machine,
 * which still follows the assignment. Past the reacquire window the clear ESS reopens a Phase 1
 * call from the assignment's target alone, and the same-identity LCs after it changed nothing the
 * state machine held, so the radio ID never came back for the rest of the transmission (and an
 * LC that decoded before the ESS left the call ended). fade_engine() below models that engine
 * work exactly; the engine library is not linked.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25_crc.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_xcch.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"

#define TEST_TG       50651
#define TEST_SRC      615061
#define TEST_SRC_NEXT 615046
#define TEST_CHANNEL  0x1234
#define TEST_FREQ     851500000L
#define TEST_T0_NS    UINT64_C(1000000000000)

static dsd_opts g_opts;
static dsd_state g_state;

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

static int
expect(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

static void
clock_at_ms(uint64_t ms) {
    dsd_decode_clock_test_set_ns(TEST_T0_NS + ms * UINT64_C(1000000));
}

/* A trunked session tuned to the assignment for TEST_TG/TEST_SRC: FDMA (iden 1 announced as such) or TDMA. */
static p25_sm_ctx_t*
follow_grant(int tdma) {
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    dsd_decode_clock_use_test(TEST_T0_NS);
    g_opts.trunk_enable = 1;
    g_opts.trunk_hangtime = 2.0f;
    g_opts.trunk_tune_group_calls = 1;
    g_state.p25_cc_freq = 851000000;
    g_state.trunk_chan_map[TEST_CHANNEL] = TEST_FREQ;
    g_state.p25_chan_tdma_explicit[1] = tdma ? 2 : 1;
    // The SM defers TDMA grants until the descrambler seed (WACN/SYSID/NAC) has been decoded from the control channel.
    g_state.p2_wacn = 0xBEE00;
    g_state.p2_sysid = 0x1A2;
    g_state.p2_cc = 0x293;
    g_state.synctype = g_state.lastsynctype = tdma ? DSD_SYNC_P25P2_POS : DSD_SYNC_P25P1_POS;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(ctx, &g_opts, &g_state);
    g_opts.trunk_is_tuned = 1;
    p25_sm_event_t grant = p25_sm_ev_group_grant(TEST_CHANNEL, TEST_FREQ, TEST_TG, TEST_SRC, 0x00);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
    return ctx;
}

/* The engine's no-carrier pass: no_carrier_finalize_canonical_calls() then the payload and P25 crypto resets of
 * dsd_engine_reset_no_carrier_state(). */
static void
fade_engine(void) {
    for (uint8_t slot = 0; slot < 2; slot++) {
        (void)dsd_call_state_end_ex(&g_state, slot, 0.0, DSD_CALL_END_SYNC_LOSS);
    }
    g_state.payload_algid = 0;
    g_state.payload_keyid = 0;
    g_state.payload_mi = 0;
    p25_crypto_reset_slot(&g_state, 0);
    p25_crypto_reset_slot(&g_state, 1);
}

/* A clear Phase 1 ESS (LDU2/HDU ALGID 0x80). */
static void
p1_clear_ess(void) {
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE1, 0, 0x80, 0, 0ULL, TEST_TG);
}

/* A Group Voice Channel User LC (p25_lcw.c) for TEST_TG from @p src. */
static void
p1_gvcu(int src) {
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, src, 1, 0x00);
}

/* The LDU's per-frame voice activity (p25p1_ldu1.c/p25p1_ldu2.c). */
static void
p1_ldu_activity(void) {
    (void)p25_sm_emit_active(&g_opts, &g_state, 0);
}

static int
slot0(dsd_call_snapshot* call) {
    DSD_MEMSET(call, 0, sizeof(*call));
    return dsd_call_state_get(&g_state, 0U, call) > 0;
}

static int
slot0_named(uint64_t source) {
    dsd_call_snapshot call = {0};
    return slot0(&call) && call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_target_id == TEST_TG
           && call.ota_source_id == source;
}

/* A clear call the LC names, faded for @p fade_ms. */
static void
p1_call_then_fade(uint64_t fade_ms) {
    (void)follow_grant(0);
    p1_clear_ess();
    p1_gvcu(TEST_SRC);
    fade_engine();
    clock_at_ms(fade_ms);
    p1_ldu_activity();
}

/* Past the reacquire window the ESS reopens the call from the assignment's target; the LC after it names the talker
 * again, on the same epoch (one call, not two). */
static int
test_p1_fade_ess_then_lc_restores_source(void) {
    int rc = 0;
    p1_call_then_fade(1000U);
    p1_clear_ess();
    dsd_call_snapshot reopened = {0};
    rc |= expect("P1 ESS reopens the faded call",
                 slot0(&reopened) && reopened.phase == DSD_CALL_PHASE_ACTIVE && reopened.ota_target_id == TEST_TG);
    p1_gvcu(TEST_SRC);
    dsd_call_snapshot named = {0};
    rc |= expect("P1 LC after the fade names the radio again", slot0_named(TEST_SRC));
    rc |= expect("P1 LC names it on the ESS's epoch", slot0(&named) && named.epoch == reopened.epoch);
    p1_gvcu(TEST_SRC);
    rc |= expect("P1 radio ID stays on the next LC", slot0_named(TEST_SRC));
    return rc;
}

/* An LC that decodes before the ESS reopens the call with its identity; the ESS then classifies that call. */
static int
test_p1_fade_lc_before_ess_reopens_call(void) {
    int rc = 0;
    p1_call_then_fade(1000U);
    p1_gvcu(TEST_SRC);
    dsd_call_snapshot reopened = {0};
    rc |= expect("P1 LC before the ESS reopens the faded call", slot0_named(TEST_SRC) && slot0(&reopened));
    p1_clear_ess();
    dsd_call_snapshot classified = {0};
    rc |= expect("P1 ESS after the LC keeps the call and its radio",
                 slot0_named(TEST_SRC) && slot0(&classified) && classified.epoch == reopened.epoch);
    rc |= expect("P1 ESS after the LC classifies it clear", classified.crypto == DSD_CALL_CRYPTO_CLEAR);
    return rc;
}

/* A short fade is the reacquire path's: the faded call comes back whole. */
static int
test_p1_short_fade_reacquires(void) {
    int rc = 0;
    p1_call_then_fade(300U);
    p1_clear_ess();
    p1_gvcu(TEST_SRC);
    rc |= expect("P1 short fade reacquires the call", slot0_named(TEST_SRC));
    return rc;
}

/* A different talker after the fade is named by its own LC; the previous talker is never published onto it. */
static int
test_p1_fade_new_talker_named_by_its_lc(void) {
    int rc = 0;
    p1_call_then_fade(1000U);
    p1_clear_ess();
    p1_gvcu(TEST_SRC_NEXT);
    rc |= expect("P1 new talker after the fade is named", slot0_named(TEST_SRC_NEXT));
    return rc;
}

/* An LC that decoded no subscriber source (0, or the 0xFFFFFD placeholder) does not lend the reopened call the
 * assignment's source, which could be the talker before the fade. */
static int
test_p1_fade_sourceless_lc_inherits_nothing(void) {
    int rc = 0;
    static const int sources[] = {0, 0xFFFFFD};
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        p1_call_then_fade(1000U);
        p1_clear_ess();
        p1_gvcu(sources[i]);
        dsd_call_snapshot call = {0};
        rc |= expect("P1 source-less LC leaves the reopened call's source unknown",
                     slot0(&call) && call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_target_id == TEST_TG
                         && call.ota_source_id == 0U);
    }
    return rc;
}

/* A regroup whose key is KEY=0 runs as clear despite its encrypted service bit. Restoring its radio ID after the fade
 * keeps the classification the reopening ESS made instead of starting it over. */
static int
test_p1_fade_clear_override_keeps_ess_classification(void) {
    int rc = 0;
    (void)follow_grant(0);
    p25_patch_set_kas(&g_state, TEST_TG, 0, 0x80, 1);
    p1_clear_ess();
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x40);
    fade_engine();
    clock_at_ms(1000U);
    p1_ldu_activity();
    p1_clear_ess();
    const int algid = g_state.payload_algid;
    const dsd_p25_crypto_state classified = g_state.p25_crypto_state[0];
    rc |= expect("override ESS classifies the reopened call clear", classified == DSD_P25_CRYPTO_CLEAR);
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x40);
    rc |= expect("override LC after the fade names the radio again", slot0_named(TEST_SRC));
    rc |= expect("override LC keeps the ESS classification",
                 g_state.p25_crypto_state[0] == classified && g_state.payload_algid == algid);
    return rc;
}

/* A fresh HDU after a missed terminator leaves the identity pending; the LC that completes it is that transmission's
 * start, not a recovery, so a regroup KEY=0 clear override still applies to its encrypted HDU and keeps the call. */
static int
test_p1_pending_hdu_identity_keeps_clear_override(void) {
    int rc = 0;
    p25_sm_ctx_t* ctx = follow_grant(0);
    p25_patch_set_kas(&g_state, TEST_TG, 0, 0x80, 1);
    p1_clear_ess();
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x40);
    p1_ldu_activity();
    // hdu_maybe_enc_lockout() (p25p1_hdu.c) for an encrypted HDU with no key loaded.
    g_state.p25_p1_identity_pending = 1;
    g_state.p25_p1_identity_epoch_started = 0;
    g_state.p25_p2_audio_allowed[0] = 0;
    g_state.p25_p1_hdu_crypto_fresh = 1;
    (void)p25_crypto_resolve(&g_opts, &g_state, DSD_P25_CRYPTO_PHASE1, 0, 0x84, 0x1234, UINT64_C(0x1112131415161718),
                             TEST_TG);
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, TEST_TG, 0, TEST_SRC, 1, 0x40);
    rc |= expect("pending HDU identity under a clear override keeps the call",
                 ctx->state == P25_SM_TUNED && ctx->slots[0].grant_active);
    rc |= expect("pending HDU identity completes", g_state.p25_p1_identity_pending == 0);
    return rc;
}

/* An identity-less voice epoch on the followed carrier (the vocoder's media mark) takes the identity the next LC
 * decodes, in place. */
static int
test_p1_identityless_epoch_named_by_lc(void) {
    int rc = 0;
    p1_call_then_fade(1000U);
    const dsd_call_observation media = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    (void)dsd_call_state_observe(&g_state, &media, DSD_CALL_BOUNDARY_BEGIN);
    dsd_call_snapshot anonymous = {0};
    rc |= expect("identity-less voice epoch opens",
                 slot0(&anonymous) && anonymous.phase == DSD_CALL_PHASE_ACTIVE && anonymous.ota_target_id == 0U);
    p1_gvcu(TEST_SRC);
    dsd_call_snapshot named = {0};
    rc |= expect("P1 LC names the identity-less epoch", slot0_named(TEST_SRC));
    rc |= expect("P1 LC names it in place", slot0(&named) && named.epoch == anonymous.epoch);
    return rc;
}

/* A talkgroup avoided during the fade is not republished: the LC's policy check refuses it. */
static int
test_p1_fade_avoided_talkgroup_not_republished(void) {
    int rc = 0;
    p1_call_then_fade(1000U);
    rc |= expect("session avoid accepted", dsd_tg_policy_session_avoid_add(&g_state, TEST_TG) == 0);
    p1_gvcu(TEST_SRC);
    rc |= expect("P1 avoided talkgroup is not republished after the fade", !slot0_named(TEST_SRC));
    return rc;
}

/* A transmission its terminator ended is not reopened from the state machine's memory: the next one waits for its
 * own identity (the existing identity-pending rule). */
static int
test_p1_terminated_call_waits_for_identity(void) {
    int rc = 0;
    p25_sm_ctx_t* ctx = follow_grant(0);
    p1_clear_ess();
    p1_gvcu(TEST_SRC);
    p25_sm_event_t end = p25_sm_ev_end(0);
    p25_sm_event(ctx, &g_opts, &g_state, &end);
    clock_at_ms(1000U);
    p1_ldu_activity();
    dsd_call_snapshot call = {0};
    rc |= expect("P1 identity stays pending after a terminator", g_state.p25_p1_identity_pending != 0);
    rc |= expect("P1 terminated call is not reopened by anonymous voice",
                 !(slot0(&call) && call.phase == DSD_CALL_PHASE_ACTIVE));
    return rc;
}

/* A FACCH MAC_ACTIVE carrying a Group Voice Channel User (TG, SRC, clear service), with a valid CRC-12. */
static void
facch_mac_active_gvcu(int payload[180], int tg, int src) {
    uint8_t mac[23] = {0};
    mac[0] = 0x80; // opcode 4: MAC_ACTIVE
    mac[1] = 0x01; // Group Voice Channel User - Abbreviated
    mac[2] = 0x00; // service options: clear
    mac[3] = (uint8_t)((tg >> 8) & 0xFF);
    mac[4] = (uint8_t)(tg & 0xFF);
    mac[5] = (uint8_t)((src >> 16) & 0xFF);
    mac[6] = (uint8_t)((src >> 8) & 0xFF);
    mac[7] = (uint8_t)(src & 0xFF);
    for (int i = 0; i < 180; i++) {
        payload[i] = (mac[i / 8] >> (7 - i % 8)) & 1;
    }
    for (int crc = 0; crc < 4096; crc++) {
        for (int i = 0; i < 12; i++) {
            payload[144 + i] = (crc >> (11 - i)) & 1;
        }
        if (crc12_xb_bridge(payload, 144) == 0) {
            return;
        }
    }
}

/* Phase 2 recovers through the MAC_ACTIVE's own observation and classification, before the state machine sees it:
 * the 4V activity alone publishes nothing, and the clear MAC_ACTIVE restores the call, its clear classification and
 * the slot's audio. */
static int
test_p2_fade_mac_active_restores_call_and_audio(void) {
    int rc = 0;
    (void)follow_grant(1);
    g_state.currentslot = 0;
    int payload[180];
    facch_mac_active_gvcu(payload, TEST_TG, TEST_SRC);
    process_FACCH_MAC_PDU(&g_opts, &g_state, payload);
    rc |= expect("P2 MAC_ACTIVE opens the call", slot0_named(TEST_SRC));
    rc |= expect("P2 clear MAC_ACTIVE opens the slot's audio", g_state.p25_p2_audio_allowed[0] != 0);

    fade_engine();
    g_state.p25_p2_audio_allowed[0] = 0;
    clock_at_ms(1000U);
    (void)p25_sm_emit_active(&g_opts, &g_state, 0);
    dsd_call_snapshot call = {0};
    rc |= expect("P2 4V activity alone publishes nothing",
                 slot0(&call) && call.phase == DSD_CALL_PHASE_ENDED && call.end_reason == DSD_CALL_END_SYNC_LOSS);

    clock_at_ms(1100U);
    process_FACCH_MAC_PDU(&g_opts, &g_state, payload);
    rc |= expect("P2 MAC_ACTIVE after the fade restores the call", slot0_named(TEST_SRC));
    rc |= expect("P2 MAC_ACTIVE after the fade classifies it clear",
                 slot0(&call) && call.crypto == DSD_CALL_CRYPTO_CLEAR);
    rc |= expect("P2 MAC_ACTIVE after the fade reopens the slot's audio", g_state.p25_p2_audio_allowed[0] != 0);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_p1_fade_ess_then_lc_restores_source();
    rc |= test_p1_fade_lc_before_ess_reopens_call();
    rc |= test_p1_short_fade_reacquires();
    rc |= test_p1_fade_new_talker_named_by_its_lc();
    rc |= test_p1_fade_sourceless_lc_inherits_nothing();
    rc |= test_p1_fade_clear_override_keeps_ess_classification();
    rc |= test_p1_pending_hdu_identity_keeps_clear_override();
    rc |= test_p1_identityless_epoch_named_by_lc();
    rc |= test_p1_fade_avoided_talkgroup_not_republished();
    rc |= test_p1_terminated_call_waits_for_identity();
    rc |= test_p2_fade_mac_active_restores_call_and_audio();
    dsd_decode_clock_use_system();
    dsd_state_ext_free_all(&g_state);
    if (rc == 0) {
        printf("P25_VOICE_RESUME_IDENTITY: OK\n");
    }
    return rc;
}
