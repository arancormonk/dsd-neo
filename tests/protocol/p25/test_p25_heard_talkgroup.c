// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The P25 producers of the verified talkgroup proof (issue #903): which of the voice channel's own checked messages
 * prove the group of a transmission, and which never do.
 *
 * Phase 1: an LDU1 group voice-user link control from a hard RS decode, trunked or conventional; a soft-erasure retry
 * proves nothing, and a Motorola regroup voice user (LCGRGR) proves a supergroup, which is not remembered.
 *
 * Phase 2: a CRC-passed MAC_PTT naming the group; or a CRC-passed group voice-user message once traffic of the same
 * call is decoded on the slot. A voice user alone (a setup that fails, TIA-102.BBAC §7.3.1), one in an IDLE or
 * HANGTIME PDU, a repeat of a recent end, and a clean end followed by an addressless voice start prove nothing. A
 * regroup voice user's own supergroup evidence counts on its first packet, before its handler updates the patch table,
 * and the frozen verdict survives the patch expiring.
 *
 * The messages run through the real link control and MAC decoders, trunk state machine and call state; traffic is a
 * 4V burst through the frame decoder's DUID dispatch, whose DUID must match its codeword exactly.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/heard_talkgroup.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_crc.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/protocol/p25/p25_xcch.h>
#include <dsd-neo/protocol/p25/p25p2_frame.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25p2_frame_internal.h"

/* The frame decoder's window bits and their LLRs (p25p2_frame.c). */
extern int p2bit[4320];
extern int16_t p2llr[1400];

#define TEST_TG      0x1234
#define TEST_SG      0x2345
#define TEST_SRC     0x00ABCD
#define TEST_CHANNEL 0x1234
#define TEST_FREQ    851500000L
#define TEST_T0_NS   UINT64_C(1000000000000)

static dsd_opts g_opts;
static dsd_state g_state;
static uint64_t g_now_ms = 0U;

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
advance_ms(uint64_t ms) {
    g_now_ms += ms;
    dsd_decode_clock_test_set_ns(TEST_T0_NS + g_now_ms * UINT64_C(1000000));
}

static void
reset_session(int tdma) {
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    g_now_ms = 0U;
    dsd_decode_clock_use_test(TEST_T0_NS);
    g_state.p2_wacn = 0xBEE00;
    g_state.p2_sysid = 0x1A2;
    g_state.p2_cc = 0x293;
    g_state.synctype = g_state.lastsynctype = tdma ? DSD_SYNC_P25P2_POS : DSD_SYNC_P25P1_POS;
    p25_p2_frame_reset();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
}

/* A trunked session tuned to the assignment for @p tg: FDMA (iden 1 announced as such) or TDMA. */
static void
follow_grant(int tdma, int tg) {
    reset_session(tdma);
    g_opts.trunk_enable = 1;
    g_opts.trunk_hangtime = 2.0f;
    g_opts.trunk_tune_group_calls = 1;
    g_state.p25_cc_freq = 851000000;
    g_state.trunk_chan_map[TEST_CHANNEL] = TEST_FREQ;
    g_state.p25_chan_tdma_explicit[1] = tdma ? 2 : 1;
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(ctx, &g_opts, &g_state);
    g_opts.trunk_is_tuned = 1;
    p25_sm_event_t grant = p25_sm_ev_group_grant(TEST_CHANNEL, TEST_FREQ, tg, TEST_SRC, 0x00);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
}

/* A conventional Phase 1 session: no trunking. */
static void
conventional_p1(void) {
    reset_session(0);
    p25_sm_init_ctx(p25_sm_get_ctx(), &g_opts, &g_state);
}

static dsd_call_snapshot
slot_call(uint8_t slot) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    (void)dsd_call_state_get(&g_state, slot, &call);
    return call;
}

/* Slot @p slot holds an ACTIVE call naming @p tg. */
static int
active_on(uint8_t slot, uint64_t tg) {
    const dsd_call_snapshot call = slot_call(slot);
    return call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_target_id == tg;
}

/* The call on @p slot is noted and remembers @p tg as a P25 talkgroup. */
static int
remembered(uint8_t slot, uint32_t tg) {
    const dsd_call_snapshot call = slot_call(slot);
    return call.heard_noted == 1U && call.heard_variant == (uint8_t)DSD_HEARD_TG_P25 && call.heard_id == tg;
}

/* The call on @p slot is noted but remembers nothing (a supergroup). */
static int
noted_not_remembered(uint8_t slot) {
    const dsd_call_snapshot call = slot_call(slot);
    return call.heard_noted == 1U && call.heard_variant == (uint8_t)DSD_HEARD_TG_NONE && call.heard_id == 0U;
}

static int
not_noted(uint8_t slot) {
    const dsd_call_snapshot call = slot_call(slot);
    return call.heard_noted == 0U && call.heard_variant == (uint8_t)DSD_HEARD_TG_NONE;
}

/* ---- Phase 1 link control ---- */

static void
put_bits(uint8_t bits[72], int first, int count, uint32_t value) {
    for (int i = 0; i < count; i++) {
        bits[first + i] = (uint8_t)((value >> (count - 1 - i)) & 1U);
    }
}

/* An LDU1 link control naming @p group from TEST_SRC: LCO $00 Group Voice Channel User (@p mfid 0), or the Motorola
 * Group Regroup Channel User (LCGRGR, @p mfid 0x90), with the same layout. @p soft: it passed only a soft-erasure RS
 * retry (p25p1_ldu1.c sets dsd_state::p25_p1_lcw_soft around the call). */
static void
p1_ldu1_voice_user(uint8_t mfid, uint32_t group, int soft) {
    uint8_t bits[72] = {0};
    put_bits(bits, 0, 8, 0x00);
    put_bits(bits, 8, 8, mfid);
    put_bits(bits, 16, 8, 0x00); /* service options: clear */
    put_bits(bits, 32, 16, group);
    put_bits(bits, 48, 24, TEST_SRC);
    g_state.p25_p1_lcw_soft = soft ? 1U : 0U;
    p25_lcw(&g_opts, &g_state, bits, 0);
    g_state.p25_p1_lcw_soft = 0U;
}

static int
test_p1_hard_lcw_proves_group(void) {
    int rc = 0;
    follow_grant(0, TEST_TG);
    p1_ldu1_voice_user(0x00, TEST_TG, 0);
    rc |= expect("P1 hard LCW publishes the call", active_on(0U, TEST_TG));
    rc |= expect("P1 hard LCW $00 proves the group", remembered(0U, TEST_TG));
    return rc;
}

static int
test_p1_soft_lcw_proves_nothing(void) {
    int rc = 0;
    follow_grant(0, TEST_TG);
    p1_ldu1_voice_user(0x00, TEST_TG, 1);
    rc |= expect("P1 soft LCW still publishes the call", active_on(0U, TEST_TG));
    rc |= expect("P1 soft-erasure LCW proves nothing", not_noted(0U));
    p1_ldu1_voice_user(0x00, TEST_TG, 0);
    rc |= expect("P1 hard LCW after a soft one proves the group", remembered(0U, TEST_TG));
    return rc;
}

/* A TDULC's link control names the call its terminator ends: never a proof, hard-decoded or not. */
static int
test_p1_tdulc_voice_user_proves_nothing(void) {
    int rc = 0;
    follow_grant(0, TEST_TG);
    p1_ldu1_voice_user(0x00, TEST_TG, 1);
    rc |= expect("P1 soft LCW opens the call unproven", active_on(0U, TEST_TG) && not_noted(0U));
    uint8_t bits[72] = {0};
    put_bits(bits, 32, 16, TEST_TG);
    put_bits(bits, 48, 24, TEST_SRC);
    p25_lcw_from_tdulc(&g_opts, &g_state, bits, 0, NULL, 0U);
    rc |= expect("P1 TDULC group voice user proves nothing", slot_call(0U).heard_noted == 0U);
    return rc;
}

static int
test_p1_conventional_hard_lcw_proves_group(void) {
    int rc = 0;
    conventional_p1();
    p1_ldu1_voice_user(0x00, TEST_TG, 1);
    rc |= expect("conventional P1 soft LCW proves nothing", active_on(0U, TEST_TG) && not_noted(0U));
    p1_ldu1_voice_user(0x00, TEST_TG, 0);
    rc |= expect("conventional P1 hard LCW proves the group", remembered(0U, TEST_TG));
    return rc;
}

/* Each LDU1 repeats the voice user: once noted, the transmission is never classified again. */
static int
test_p1_classifies_once(void) {
    int rc = 0;
    follow_grant(0, TEST_TG);
    p1_ldu1_voice_user(0x00, TEST_TG, 0);
    const long evals = dsd_heard_tg_test_eval_count();
    for (int i = 0; i < 8; i++) {
        p1_ldu1_voice_user(0x00, TEST_TG, 0);
    }
    rc |= expect("P1 later LDU1s keep the proof", remembered(0U, TEST_TG));
    rc |= expect("P1 later LDU1s never classify again", dsd_heard_tg_test_eval_count() == evals);
    return rc;
}

/* A regroup voice user (LCGRGR) names a supergroup: noted, not remembered. */
static int
test_p1_regroup_voice_user_is_supergroup(void) {
    int rc = 0;
    follow_grant(0, TEST_SG);
    p1_ldu1_voice_user(0x90, TEST_SG, 0);
    rc |= expect("P1 LCGRGR publishes the call", active_on(0U, TEST_SG));
    rc |= expect("P1 LCGRGR proves a supergroup", noted_not_remembered(0U));
    return rc;
}

/* A standard voice user for a group the patch table names as a supergroup; the patch then ends, and the verdict stays
 * as frozen through the call's end. */
static int
test_p1_patched_group_verdict_frozen(void) {
    int rc = 0;
    follow_grant(0, TEST_SG);
    p25_patch_update(&g_state, TEST_SG, 1, 1);
    p1_ldu1_voice_user(0x00, TEST_SG, 0);
    rc |= expect("P1 LCW $00 naming a patch's supergroup is not remembered",
                 active_on(0U, TEST_SG) && noted_not_remembered(0U));
    p25_patch_clear_sg(&g_state, TEST_SG);
    p1_ldu1_voice_user(0x00, TEST_SG, 0);
    rc |= expect("P1 patch expiring keeps the frozen verdict", noted_not_remembered(0U));
    p25_sm_emit_end(&g_opts, &g_state, 0);
    const dsd_call_snapshot ended = slot_call(0U);
    rc |=
        expect("P1 ended call keeps the frozen verdict", ended.phase == DSD_CALL_PHASE_ENDED && ended.heard_noted == 1U
                                                             && ended.heard_variant == (uint8_t)DSD_HEARD_TG_NONE);
    return rc;
}

/* ---- Phase 2 MAC PDUs ---- */

enum { MAC_PTT = 1, MAC_END_PTT = 2, MAC_IDLE = 3, MAC_ACTIVE = 4, MAC_HANGTIME = 6 };

/* A FACCH MAC PDU of @p opcode for slot @p slot, octets 1.. from @p body (@p body_len octets), with a valid CRC-12. */
static void
facch(uint8_t slot, int opcode, const uint8_t* body, int body_len) {
    uint8_t mac[18] = {0};
    mac[0] = (uint8_t)(opcode << 5);
    for (int i = 0; i < body_len && i + 1 < (int)sizeof(mac); i++) {
        mac[i + 1] = body[i];
    }
    int payload[180] = {0};
    for (int i = 0; i < 144; i++) {
        payload[i] = (mac[i / 8] >> (7 - i % 8)) & 1;
    }
    for (int crc = 0; crc < 4096; crc++) {
        for (int i = 0; i < 12; i++) {
            payload[144 + i] = (crc >> (11 - i)) & 1;
        }
        if (crc12_xb_bridge(payload, 144) == 0) {
            break;
        }
    }
    g_state.currentslot = slot;
    process_FACCH_MAC_PDU(&g_opts, &g_state, payload);
}

/* MAC_PTT naming @p group from TEST_SRC, clear (ALGID 0x80). */
static void
p2_ptt(uint8_t slot, uint32_t group) {
    uint8_t body[17] = {0};
    body[9] = 0x80; /* octet 10: ALGID */
    body[12] = (uint8_t)(TEST_SRC >> 16);
    body[13] = (uint8_t)(TEST_SRC >> 8);
    body[14] = (uint8_t)TEST_SRC;
    body[15] = (uint8_t)(group >> 8);
    body[16] = (uint8_t)group;
    facch(slot, MAC_PTT, body, (int)sizeof(body));
}

static void
p2_end(uint8_t slot, uint32_t group) {
    uint8_t body[17] = {0};
    body[12] = (uint8_t)(TEST_SRC >> 16);
    body[13] = (uint8_t)(TEST_SRC >> 8);
    body[14] = (uint8_t)TEST_SRC;
    body[15] = (uint8_t)(group >> 8);
    body[16] = (uint8_t)group;
    facch(slot, MAC_END_PTT, body, (int)sizeof(body));
}

/* A Group Voice Channel User - Abbreviated (opcode 0x01) for @p group in a MAC PDU of @p opcode. */
static void
p2_voice_user(uint8_t slot, int opcode, uint32_t group) {
    const uint8_t body[7] = {
        0x01,
        0x00,
        (uint8_t)(group >> 8),
        (uint8_t)group,
        (uint8_t)(TEST_SRC >> 16),
        (uint8_t)(TEST_SRC >> 8),
        (uint8_t)TEST_SRC,
    };
    facch(slot, opcode, body, (int)sizeof(body));
}

/* The Motorola MFID 0x90 Group Regroup Voice Channel User (0x80 0x90) for supergroup @p sg in a MAC_ACTIVE. */
static void
p2_regroup_voice_user(uint8_t slot, uint32_t sg) {
    const uint8_t body[8] = {
        0x80,
        0x90,
        0x00,
        (uint8_t)(sg >> 8),
        (uint8_t)sg,
        (uint8_t)(TEST_SRC >> 16),
        (uint8_t)(TEST_SRC >> 8),
        (uint8_t)TEST_SRC,
    };
    facch(slot, MAC_ACTIVE, body, (int)sizeof(body));
}

/* DUID codewords (p25p2_frame.c duid_canonical). */
enum { DUID_4V = 0x00, DUID_SACCH = 0x39 };

/* Window timeslot @p timeslot_index sends DUID bits @p duid: every bit confident, except those @p weak_mask names
   (MSB first), sent with a reliability below the soft-erasure threshold. */
static void
seed_duid(int timeslot_index, uint8_t duid, uint8_t weak_mask) {
    static const int duid_offsets[8] = {0, 1, 74, 75, 244, 245, 318, 319};
    const int base = timeslot_index * 360;
    for (int i = 0; i < 8; i++) {
        const int abs_bit = base + duid_offsets[i];
        const int bit = (duid >> (7 - i)) & 1;
        const int16_t magnitude = (int16_t)(((weak_mask >> (7 - i)) & 1U) ? 5 : 200);
        p2bit[abs_bit] = bit;
        p2llr[abs_bit] = bit ? magnitude : (int16_t)-magnitude;
    }
}

/* One window from superframe timeslot 0 through the frame decoder's DUID dispatch: slot @p slot's first timeslot
   sends a 4V burst as DUID bits @p duid (@p weak_mask as seed_duid()), every other timeslot a SACCH. */
static void
p2_traffic_as(uint8_t slot, uint8_t duid, uint8_t weak_mask) {
    g_state.p2_scramble_offset = 0;
    g_state.currentslot = 0;
    for (int i = 0; i < 4; i++) {
        if (i == (int)slot) {
            seed_duid(i, duid, weak_mask);
        } else {
            seed_duid(i, DUID_SACCH, 0U);
        }
    }
    p25p2_process_duid(&g_opts, &g_state);
}

/* Traffic: a 4V burst on @p slot whose DUID is its codeword exactly, through the real voice path. */
static void
p2_traffic(uint8_t slot) {
    p2_traffic_as(slot, DUID_4V, 0U);
}

static int
test_p2_ptt_proves_group(void) {
    int rc = 0;
    follow_grant(1, TEST_TG);
    p2_ptt(0U, TEST_TG);
    rc |= expect("P2 MAC_PTT publishes the call", active_on(0U, TEST_TG));
    rc |= expect("P2 CRC-passed MAC_PTT proves the group", remembered(0U, TEST_TG));
    return rc;
}

/* A setup the base station gives up on: voice users, then nothing. */
static int
test_p2_failed_setup_proves_nothing(void) {
    int rc = 0;
    follow_grant(1, TEST_TG);
    for (int i = 0; i < 4; i++) {
        p2_voice_user(0U, MAC_ACTIVE, TEST_TG);
        advance_ms(30U);
    }
    rc |= expect("P2 setup voice users publish the call", active_on(0U, TEST_TG));
    rc |= expect("P2 voice user alone proves nothing", not_noted(0U));
    p2_voice_user(0U, MAC_IDLE, TEST_TG);
    const dsd_call_snapshot after = slot_call(0U);
    rc |= expect("P2 failed setup ends unproven", after.phase == DSD_CALL_PHASE_ENDED && after.heard_noted == 0U);
    return rc;
}

static int
test_p2_voice_user_then_traffic_proves_group(void) {
    int rc = 0;
    follow_grant(1, TEST_TG);
    p2_voice_user(0U, MAC_ACTIVE, TEST_TG);
    rc |= expect("P2 voice user waits for traffic", active_on(0U, TEST_TG) && not_noted(0U));
    p2_traffic(1U);
    rc |= expect("P2 traffic on the other slot proves nothing", not_noted(0U));
    p2_traffic(0U);
    rc |= expect("P2 voice user then traffic proves the group", remembered(0U, TEST_TG));
    const long evals = dsd_heard_tg_test_eval_count();
    for (int i = 0; i < 4; i++) {
        p2_voice_user(0U, MAC_ACTIVE, TEST_TG);
        p2_traffic(0U);
    }
    rc |= expect("P2 later voice users and traffic never classify again", dsd_heard_tg_test_eval_count() == evals);
    rc |= expect("P2 later bursts wait on nothing", g_state.p25_p2_heard_vu_epoch[0] == 0U);
    return rc;
}

/* A Group Voice Channel User - Abbreviated for @p group, straight into the MAC message dispatch as a MAC_SIGNAL of
 * CRC verdict @p crc_ok (-F lets a failed one through, process_MAC_VPDU_crc()). */
static void
p2_signal_voice_user(uint8_t slot, uint32_t group, int crc_ok) {
    unsigned long long int mac[24] = {0};
    mac[1] = 0x01;
    mac[3] = (group >> 8) & 0xFFU;
    mac[4] = group & 0xFFU;
    mac[5] = (TEST_SRC >> 16) & 0xFFU;
    mac[6] = (TEST_SRC >> 8) & 0xFFU;
    mac[7] = TEST_SRC & 0xFFU;
    g_state.currentslot = slot;
    process_MAC_VPDU_crc(&g_opts, &g_state, 0, P25_MAC_PDU_SIGNAL, mac, crc_ok);
}

/* A voice user from a MAC PDU whose CRC failed publishes the call, but its traffic proves nothing; one that passed
 * does. */
static int
test_p2_failed_crc_voice_user_proves_nothing(void) {
    int rc = 0;
    follow_grant(1, TEST_TG);
    p2_signal_voice_user(0U, TEST_TG, 0);
    rc |= expect("P2 failed-CRC voice user publishes the call", active_on(0U, TEST_TG));
    p2_traffic(0U);
    rc |= expect("P2 failed-CRC voice user then traffic proves nothing", not_noted(0U));
    p2_signal_voice_user(0U, TEST_TG, 1);
    p2_traffic(0U);
    rc |= expect("P2 CRC-passed voice user then traffic proves the group", remembered(0U, TEST_TG));
    return rc;
}

/* A voice user continuing a call already noted leaves nothing waiting. */
static int
test_p2_voice_user_after_note_waits_on_nothing(void) {
    int rc = 0;
    follow_grant(1, TEST_TG);
    p2_ptt(0U, TEST_TG);
    rc |= expect("P2 MAC_PTT notes the call", remembered(0U, TEST_TG));
    p2_voice_user(0U, MAC_ACTIVE, TEST_TG);
    rc |= expect("P2 voice user after the note waits on nothing", g_state.p25_p2_heard_vu_epoch[0] == 0U);
    return rc;
}

/* A 4V burst whose DUID decoded only after a correction is voice for the decoder, but not traffic that proves a voice
 * user: one bit off the codeword (the hard table corrects it), then two weak bits off (only the soft lookup matches
 * it). The next burst whose DUID matched exactly proves the group. */
static int
test_p2_corrected_duid_traffic_proves_nothing(void) {
    int rc = 0;
    follow_grant(1, TEST_TG);
    p2_voice_user(0U, MAC_ACTIVE, TEST_TG);
    const int fourv_before = g_state.fourv_counter[0];
    p2_traffic_as(0U, 0x01U, 0x01U);
    rc |= expect("P2 hard-corrected DUID still decodes a 4V", g_state.fourv_counter[0] != fourv_before);
    rc |= expect("P2 hard-corrected DUID burst proves nothing", active_on(0U, TEST_TG) && not_noted(0U));
    p2_voice_user(0U, MAC_ACTIVE, TEST_TG);
    const int fourv_soft = g_state.fourv_counter[0];
    p2_traffic_as(0U, 0x03U, 0x03U);
    rc |= expect("P2 soft-matched DUID still decodes a 4V", g_state.fourv_counter[0] != fourv_soft);
    rc |= expect("P2 soft-matched DUID burst proves nothing", active_on(0U, TEST_TG) && not_noted(0U));
    p2_voice_user(0U, MAC_ACTIVE, TEST_TG);
    p2_traffic(0U);
    rc |= expect("P2 exact DUID burst then proves the group", remembered(0U, TEST_TG));
    return rc;
}

/* A voice burst run outside the DUID dispatch carries no DUID verdict and proves nothing. */
static int
test_p2_voice_outside_dispatch_proves_nothing(void) {
    int rc = 0;
    follow_grant(1, TEST_TG);
    p2_voice_user(0U, MAC_ACTIVE, TEST_TG);
    g_state.currentslot = 0;
    process_2V(&g_opts, &g_state);
    rc |= expect("P2 2V outside the DUID dispatch proves nothing", active_on(0U, TEST_TG) && not_noted(0U));
    return rc;
}

/* Voice users an IDLE or HANGTIME PDU carries re-describe the call; traffic after them proves nothing. */
static int
test_p2_tracking_only_voice_users_prove_nothing(void) {
    int rc = 0;
    static const int kinds[2] = {MAC_IDLE, MAC_HANGTIME};
    for (int k = 0; k < 2; k++) {
        follow_grant(1, TEST_TG);
        p2_voice_user(0U, kinds[k], TEST_TG);
        p2_traffic(0U);
        rc |= expect(k == 0 ? "P2 IDLE voice user then traffic proves nothing"
                            : "P2 HANGTIME voice user then traffic proves nothing",
                     active_on(0U, TEST_TG) && not_noted(0U));
    }
    return rc;
}

/* A setup that ended unproven: a voice user, then a clean END. */
static dsd_call_snapshot
p2_unproven_call_ended(void) {
    follow_grant(1, TEST_TG);
    p2_voice_user(0U, MAC_ACTIVE, TEST_TG);
    advance_ms(30U);
    p2_end(0U, TEST_TG);
    return slot_call(0U);
}

/* A voice user repeating the call a clean END just ended (its SACCH assembly straddled the END), and traffic after it:
 * the ended call stays ended and unproven. */
static int
test_p2_recent_end_repeat_proves_nothing(void) {
    int rc = 0;
    const dsd_call_snapshot ended = p2_unproven_call_ended();
    rc |= expect("P2 END ends the unproven call", ended.phase == DSD_CALL_PHASE_ENDED && ended.heard_noted == 0U);
    advance_ms(30U);
    p2_voice_user(0U, MAC_ACTIVE, TEST_TG);
    advance_ms(30U);
    p2_traffic(0U);
    const dsd_call_snapshot after = slot_call(0U);
    rc |= expect("P2 recent-end repeat and its traffic prove nothing",
                 after.epoch == ended.epoch && after.heard_noted == 0U);
    return rc;
}

/* A clean END, then an addressless voice start without a fresh grant: it reopens the assignment's call, which the
 * ended call's voice user never proves. A fresh MAC_PTT does. */
static int
test_p2_addressless_start_after_end_proves_nothing(void) {
    int rc = 0;
    const dsd_call_snapshot ended = p2_unproven_call_ended();
    advance_ms(1500U);
    p2_traffic(0U);
    advance_ms(30U);
    p2_traffic(0U);
    const dsd_call_snapshot after = slot_call(0U);
    rc |= expect("P2 addressless voice start reopens another call",
                 after.phase == DSD_CALL_PHASE_ACTIVE && after.ota_target_id == TEST_TG && after.epoch != ended.epoch);
    rc |= expect("P2 addressless voice start after a clean END proves nothing", after.heard_noted == 0U);
    p2_ptt(0U, TEST_TG);
    rc |= expect("P2 a fresh MAC_PTT after it proves the group", remembered(0U, TEST_TG));
    return rc;
}

/* A clean END, then an addressless MAC_ACTIVE (no voice user) without a fresh grant, then traffic: whatever the
 * receiver reopens is not proven by the ended call's voice user. */
static int
test_p2_addressless_mac_active_after_end_proves_nothing(void) {
    int rc = 0;
    const dsd_call_snapshot ended = p2_unproven_call_ended();
    advance_ms(1500U);
    const uint8_t none[1] = {0};
    facch(0U, MAC_ACTIVE, none, 0);
    advance_ms(30U);
    p2_traffic(0U);
    const dsd_call_snapshot after = slot_call(0U);
    rc |= expect("P2 addressless MAC_ACTIVE reopens another call",
                 after.phase == DSD_CALL_PHASE_ACTIVE && after.ota_target_id == TEST_TG && after.epoch != ended.epoch);
    rc |= expect("P2 addressless MAC_ACTIVE after a clean END proves nothing", after.heard_noted == 0U);
    return rc;
}

/* The MFID 0x90 regroup voice user's first packet, before any patch is known: its traffic proves a supergroup. */
static int
test_p2_regroup_voice_user_first_packet_is_supergroup(void) {
    int rc = 0;
    follow_grant(1, TEST_SG);
    rc |= expect("P2 no patch known before the regroup voice user", g_state.p25_patch_count == 0);
    p2_regroup_voice_user(0U, TEST_SG);
    rc |= expect("P2 regroup voice user publishes the call", active_on(0U, TEST_SG) && not_noted(0U));
    /* The patch entry its handler noted expires before the traffic: the evidence is the message's own. */
    p25_patch_clear_sg(&g_state, TEST_SG);
    p2_traffic(0U);
    rc |= expect("P2 regroup voice user then traffic proves a supergroup", noted_not_remembered(0U));
    advance_ms(30U);
    p2_end(0U, TEST_SG);
    const dsd_call_snapshot ended = slot_call(0U);
    rc |= expect("P2 regroup call commits its frozen verdict",
                 ended.phase == DSD_CALL_PHASE_ENDED && ended.heard_noted == 1U
                     && ended.heard_variant == (uint8_t)DSD_HEARD_TG_NONE);
    return rc;
}

/* Run @p deliver (a regroup voice user for TEST_SG, no patch known), clear the patch entry its handler noted, then
 * traffic: the message's own supergroup evidence makes the proof a supergroup, which is not remembered. */
static int
expect_regroup_voice_user_is_supergroup(const char* what, void (*deliver)(void)) {
    int rc = 0;
    char tag[128];
    follow_grant(1, TEST_SG);
    deliver();
    DSD_SNPRINTF(tag, sizeof tag, "P2 %s publishes the call", what);
    rc |= expect(tag, active_on(0U, TEST_SG) && not_noted(0U));
    p25_patch_clear_sg(&g_state, TEST_SG);
    p2_traffic(0U);
    DSD_SNPRINTF(tag, sizeof tag, "P2 %s then traffic proves a supergroup", what);
    rc |= expect(tag, noted_not_remembered(0U));
    return rc;
}

/* The standard Group Regroup Voice Channel User - Abbreviated (opcode 0x90, standard MFID) in a MAC_ACTIVE. */
static void
p2_standard_regroup_voice_user(void) {
    const uint8_t body[7] = {
        0x90,
        0x00,
        (uint8_t)(TEST_SG >> 8),
        (uint8_t)TEST_SG,
        (uint8_t)(TEST_SRC >> 16),
        (uint8_t)(TEST_SRC >> 8),
        (uint8_t)TEST_SRC,
    };
    facch(0U, MAC_ACTIVE, body, (int)sizeof(body));
}

/* The Motorola MFID 0x90 Group Regroup Voice Channel User - Extended (0xA0 0x90, 16 octets) in a MAC_ACTIVE. */
static void
p2_mfid90_extended_regroup_voice_user(void) {
    const uint8_t body[16] = {
        0xA0,
        0x90,
        0x10,
        0x00,
        (uint8_t)(TEST_SG >> 8),
        (uint8_t)TEST_SG,
        (uint8_t)(TEST_SRC >> 16),
        (uint8_t)(TEST_SRC >> 8),
        (uint8_t)TEST_SRC,
        0xBE,
        0xE0,
        0x01,
        0xA2,
    };
    facch(0U, MAC_ACTIVE, body, (int)sizeof(body));
}

static int
test_p2_other_regroup_voice_users_are_supergroups(void) {
    int rc = 0;
    rc |= expect_regroup_voice_user_is_supergroup("standard regroup voice user", p2_standard_regroup_voice_user);
    rc |= expect_regroup_voice_user_is_supergroup("MFID90 extended regroup voice user",
                                                  p2_mfid90_extended_regroup_voice_user);
    return rc;
}

/* A MAC_PTT for a group a regroup grant put in the patch table; the patch then ends, the verdict stays. */
static int
test_p2_ptt_for_patched_group_verdict_frozen(void) {
    int rc = 0;
    follow_grant(1, TEST_SG);
    p25_patch_update(&g_state, TEST_SG, 1, 1);
    p2_ptt(0U, TEST_SG);
    rc |= expect("P2 MAC_PTT naming a patch's supergroup is not remembered",
                 active_on(0U, TEST_SG) && noted_not_remembered(0U));
    p25_patch_clear_sg(&g_state, TEST_SG);
    p2_voice_user(0U, MAC_ACTIVE, TEST_SG);
    p2_traffic(0U);
    rc |= expect("P2 patch expiring keeps the frozen verdict", noted_not_remembered(0U));
    advance_ms(30U);
    p2_end(0U, TEST_SG);
    const dsd_call_snapshot ended = slot_call(0U);
    rc |= expect("P2 patch expiring before commit keeps the frozen verdict",
                 ended.phase == DSD_CALL_PHASE_ENDED && ended.heard_noted == 1U
                     && ended.heard_variant == (uint8_t)DSD_HEARD_TG_NONE);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_p1_hard_lcw_proves_group();
    rc |= test_p1_soft_lcw_proves_nothing();
    rc |= test_p1_tdulc_voice_user_proves_nothing();
    rc |= test_p1_conventional_hard_lcw_proves_group();
    rc |= test_p1_classifies_once();
    rc |= test_p1_regroup_voice_user_is_supergroup();
    rc |= test_p1_patched_group_verdict_frozen();
    rc |= test_p2_ptt_proves_group();
    rc |= test_p2_failed_setup_proves_nothing();
    rc |= test_p2_voice_user_then_traffic_proves_group();
    rc |= test_p2_failed_crc_voice_user_proves_nothing();
    rc |= test_p2_voice_user_after_note_waits_on_nothing();
    rc |= test_p2_corrected_duid_traffic_proves_nothing();
    rc |= test_p2_voice_outside_dispatch_proves_nothing();
    rc |= test_p2_tracking_only_voice_users_prove_nothing();
    rc |= test_p2_recent_end_repeat_proves_nothing();
    rc |= test_p2_addressless_start_after_end_proves_nothing();
    rc |= test_p2_addressless_mac_active_after_end_proves_nothing();
    rc |= test_p2_regroup_voice_user_first_packet_is_supergroup();
    rc |= test_p2_ptt_for_patched_group_verdict_frozen();
    rc |= test_p2_other_regroup_voice_users_are_supergroups();
    dsd_decode_clock_use_system();
    dsd_state_ext_free_all(&g_state);
    if (rc == 0) {
        printf("P25_HEARD_TALKGROUP: OK\n");
    }
    return rc;
}
