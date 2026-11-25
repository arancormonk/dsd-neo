// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P_CLEAR on a payload channel (TS 102 361-4 Table 7.30): it releases the followed call only when received on that
 * call's slot and addressed to it (its talkgroup, either party of a private call, or ALLMSI); a busy companion slot
 * keeps the carrier and becomes the slot followed, where its own clear then applies; the slot's call is its live call
 * (the one its latest checked LC names), never an in-place grant still awaiting its call; the call record itself ends
 * on its terminator or timeout, not with the clear; a checked clear naming another TSCC makes it a hunt candidate; TG
 * Hold forces the release, and the encryption lockout's synthesized clear always applies, arming the forced release
 * only while a call is followed. Frequencies are invented.
 */

#include <dsd-neo/protocol/dmr/dmr_utils_api.h>

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "dmr_abs_params.h"
#include "dmr_crc_provenance.h"
#include "dmr_t3_channels_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Stubs and helpers (same pattern as other DMR tests)
void
watchdog_event_history(dsd_opts* o, dsd_state* s, uint8_t slot) {
    (void)o;
    (void)s;
    (void)slot;
}

void
watchdog_event_current(const dsd_opts* o, dsd_state* s, uint8_t slot) {
    (void)o;
    (void)s;
    (void)slot;
}

int
dsd_event_emit_data_notice(dsd_opts* o, dsd_state* s, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    (void)o;
    (void)s;
    (void)observation->ota_source_id;
    (void)observation->ota_target_id;
    (void)notice;
    (void)slot;
    return 0;
}

int
dsd_event_emit_system_notice(dsd_opts* o, dsd_state* s, uint8_t slot, const char* notice) {
    (void)o;
    (void)s;
    (void)slot;
    (void)notice;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
rotate_symbol_out_file(dsd_opts* o, dsd_state* s) {
    (void)o;
    (void)s;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_reset_blocks(dsd_opts* o, dsd_state* s) {
    (void)o;
    (void)s;
}

uint8_t
crc8(uint8_t bits[], unsigned int len) {
    (void)bits;
    (void)len;
    return 0xFF;
}

static dsd_trunk_tune_result
test_tune_to_freq(dsd_opts* o, dsd_state* s, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)ted_sps;
    if (!o || !s || freq <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    s->trunk_vc_freq[0] = s->trunk_vc_freq[1] = freq;
    o->trunk_is_tuned = 1;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result g_return_result = DSD_TRUNK_TUNE_RESULT_OK;

static dsd_trunk_tune_result
test_return_to_cc(dsd_opts* o, dsd_state* s, uint64_t request_id) {
    (void)request_id;
    if (g_return_result != DSD_TRUNK_TUNE_RESULT_OK) {
        return g_return_result;
    }
    if (o) {
        o->trunk_is_tuned = 0;
    }
    if (s) {
        s->trunk_vc_freq[0] = s->trunk_vc_freq[1] = 0;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

extern void dmr_cspdu(dsd_opts*, dsd_state*, uint8_t*, uint8_t*, uint32_t, uint32_t);

static const long CC_HZ = 851000000L;
static const long VC_HZ = 852000000L;

static dsd_opts opts;
static dsd_state state;

static const uint64_t k_t0_ns = 1000000000000000000ULL; // the TEST clock's origin, far from any platform reading

/* The decode clock's TEST source at @p ms after its origin. */
static void
clock_at_ms(uint64_t ms) {
    dsd_decode_clock_test_set_ns(k_t0_ns + (ms * 1000000ULL));
}

static void
init_env(void) {
    dsd_state_ext_free_all(&state);
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.trunk_enable = 1;
    opts.trunk_tune_group_calls = 1;
    opts.trunk_tune_private_calls = 1;
    state.trunk_cc_freq = CC_HZ;
    dmr_sm_init(&opts, &state);
}

static void
put_bits(uint8_t* bits, size_t start, uint32_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        bits[start + i] = (uint8_t)((value >> (nbits - 1U - i)) & 1U);
    }
}

/* A P_CLEAR (TS 102 361-4 Table 7.30) of feature set @p fid: LPCN @p lpcn (bits 16-27), G/I @p gi (bit 31), target
   @p target (32-55), source @p source (56-79). */
static void
build_pclear_fid(uint8_t* bits, uint8_t* bytes, uint8_t fid, uint16_t lpcn, uint8_t gi, uint32_t target,
                 uint32_t source) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bits[0] = 1U; /* last block */
    bytes[0] = (uint8_t)(0x80U | 46U);
    bytes[1] = fid;
    put_bits(bits, 2, 46U, 6);
    put_bits(bits, 8, fid, 8);
    put_bits(bits, 16, lpcn & 0x0FFFU, 12);
    bits[31] = (uint8_t)(gi & 1U);
    put_bits(bits, 32, target & 0xFFFFFFU, 24);
    put_bits(bits, 56, source & 0xFFFFFFU, 24);
}

static void
send_pclear(uint8_t slot, uint16_t lpcn, uint8_t gi, uint32_t target) {
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pclear_fid(bits, bytes, 0U, lpcn, gi, target, 77U);
    state.currentslot = slot;
    dmr_cspdu(&opts, &state, bits, bytes, 1, 0);
}

/* The canonical call the decoder would have opened on @p slot: grants are recent activity only. */
static void
observe_call(uint8_t slot, dsd_call_kind kind, uint32_t target, uint32_t source) {
    const dsd_call_observation call = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = slot,
        .kind = kind,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = source,
        .frequency_hz = VC_HZ,
    };
    assert(dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) > 0);
}

/* A voice sync on a slot whose own voice passed the colour-code gate, as an accepted voice burst leaves it. */
static void
accepted_voice_sync(uint8_t slot) {
    state.dmr_confidence_voice_open[slot] = 1;
    state.dmr_confidence_voice_m[slot] = dsd_decode_now_mono_s();
    dmr_sm_emit_voice_sync(&opts, &state, slot);
}

/* Follow group 1234 (source 42) on TS1 of VC_HZ, the companion slot idle. */
static void
follow_group_ts1(void) {
    init_env();
    dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*tg*/ 1234, /*src*/ 42);
    assert(opts.trunk_is_tuned == 1);
    observe_call(0U, DSD_CALL_KIND_GROUP_VOICE, 1234U, 42U);
    state.dmrburstL = 16;
    state.dmrburstR = 9;
}

static int
candidate_count(long freq) {
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(&state);
    int n = 0;
    for (int i = 0; cc != NULL && i < cc->count; i++) {
        n += cc->candidates[i] == freq ? 1 : 0;
    }
    return n;
}

/* A P_CLEAR forced through TG Hold: its address names the held call. */
static void
test_tg_hold_forces_release(void) {
    init_env();
    // A slotless grant: the clear's slot is the followed call's until its first voice names one.
    dmr_sm_emit_group_grant(&opts, &state, VC_HZ, /*lpcn*/ 0, /*tg*/ 1234, /*src*/ 42);
    assert(opts.trunk_is_tuned == 1);
    observe_call(0U, DSD_CALL_KIND_GROUP_VOICE, 1234U, 42U);
    state.tg_hold = 1234;
    send_pclear(0U, 0U, 1U, 1234U);
    assert(opts.trunk_is_tuned == 0);
}

static void
test_p_clear_matching_group_releases(void) {
    follow_group_ts1();
    send_pclear(0U, 0U, 1U, 1234U);
    assert(opts.trunk_is_tuned == 0);
    assert(state.trunk_cc_freq == CC_HZ);
}

/* A private call is cleared by naming either party. */
static void
test_p_clear_individual_either_party_releases(void) {
    for (int party = 0; party < 2; party++) {
        init_env();
        dmr_sm_emit_indiv_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*dst*/ 5001, /*src*/ 5002);
        assert(opts.trunk_is_tuned == 1);
        observe_call(0U, DSD_CALL_KIND_PRIVATE_VOICE, 5001U, 5002U);
        state.dmrburstR = 9;
        send_pclear(0U, 0U, 1U, party == 0 ? 5001U : 5002U); // a talkgroup of a party's number: not the call
        assert(opts.trunk_is_tuned == 1);
        send_pclear(0U, 0U, 0U, party == 0 ? 5001U : 5002U);
        assert(opts.trunk_is_tuned == 0);
    }
}

/* A clear naming another call, or on the companion slot, changes nothing: the followed call, its slot's burst hint
   and the machine's voice flags stay. */
static void
test_p_clear_unrelated_target_ignored(void) {
    follow_group_ts1();
    accepted_voice_sync(0U);
    const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    assert(ctx->slots[0].voice_active == 1);

    send_pclear(0U, 0U, 1U, 999U);  // another talkgroup
    send_pclear(0U, 0U, 0U, 1234U); // a unit of the talkgroup's number
    send_pclear(0U, 0U, 0U, 6001U); // an unrelated unit
    assert(opts.trunk_is_tuned == 1);
    assert(state.dmrburstL == 16);
    assert(ctx->slots[0].voice_active == 1);

    // The companion slot's clear, even naming the followed talkgroup or every MS, is not for the followed call.
    state.dmrburstL = 9;
    send_pclear(1U, 0U, 1U, 1234U);
    send_pclear(1U, 0U, 0U, 0xFFFED4U);
    assert(opts.trunk_is_tuned == 1);
    assert(state.dmrburstR == 9);
    assert(ctx->slots[0].voice_active == 1);
}

/* ALLMSI (Table A.4, G/I 0) clears every call on the payload channel; so does the all-ones ALLMSID. */
static void
test_p_clear_allmsi_releases(void) {
    follow_group_ts1();
    send_pclear(0U, 0U, 0U, 0xFFFED4U);
    assert(opts.trunk_is_tuned == 0);

    follow_group_ts1();
    send_pclear(0U, 0U, 0U, 0xFFFFFFU);
    assert(opts.trunk_is_tuned == 0);
}

/* The followed call's clear while the companion slot carries a call: only the cleared slot's voice ends, and the
   carrier stays for the companion. The cleared slot's call record is left to its terminator or timeout (decided limit,
   discussion #900); the companion's stays live. */
static void
follow_ts1_with_busy_companion(void) {
    follow_group_ts1();
    observe_call(1U, DSD_CALL_KIND_GROUP_VOICE, 4321U, 43U);
    state.dmrburstR = 16;
    accepted_voice_sync(0U);
    accepted_voice_sync(1U);
    const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    assert(ctx->slots[0].voice_active == 1 && ctx->slots[1].voice_active == 1);

    send_pclear(0U, 0U, 1U, 1234U);
    assert(opts.trunk_is_tuned == 1);
    assert(ctx->slots[0].voice_active == 0);
    assert(ctx->slots[1].voice_active == 1);
    assert(state.dmrburstL == 9);
    assert(state.dmrburstR == 16);
    dsd_call_snapshot ts1;
    dsd_call_snapshot ts2;
    assert(dsd_call_state_get(&state, 0U, &ts1) > 0 && ts1.phase == DSD_CALL_PHASE_ACTIVE);
    assert(dsd_call_state_get(&state, 1U, &ts2) > 0 && ts2.phase == DSD_CALL_PHASE_ACTIVE);
    assert(ts2.policy_target_id == 4321U);
    // The cleared slot's terminator LC ends its record.
    assert(dsd_call_state_end_ex(&state, 0U, 0.0, DSD_CALL_END_EXPLICIT) > 0);
}

/* The receiver stays for the companion's call, but the followed slot stays the cleared one (decided limit, discussion
   #900): a later clear on the companion, for its talkgroup or to every MS, is not for the followed call, and the
   receiver waits for the hangtime as on main. */
static void
test_p_clear_busy_companion_kept(void) {
    follow_ts1_with_busy_companion();
    const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    assert(ctx->vc_slot == 0);
    send_pclear(1U, 0U, 1U, 4321U);
    send_pclear(1U, 0U, 0U, 0xFFFED4U);
    assert(opts.trunk_is_tuned == 1);
    assert(ctx->vc_slot == 0);
    assert(state.dmrburstR == 16);
    assert(ctx->slots[1].voice_active == 1);
}

/* The clear is matched to the call as the machine followed it: a checked voice LC naming another call on the followed
   slot since the grant does not rename it, so a clear for the LC's talkgroup changes nothing and one for the grant's
   applies (decided limit, discussion #900). */
static void
test_p_clear_matches_followed_grant(void) {
    follow_group_ts1();
    observe_call(0U, DSD_CALL_KIND_GROUP_VOICE, 5678U, 51U);
    dmr_sm_note_lc_identity(&opts, &state, 0, DSD_CALL_KIND_GROUP_VOICE, 5678U, 51U);
    const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    assert(ctx->slots[0].prov == (uint8_t)DMR_SM_ID_LC && ctx->slots[0].target == 5678U);

    send_pclear(0U, 0U, 1U, 5678U); // the LC's talkgroup
    assert(opts.trunk_is_tuned == 1);
    assert(state.dmrburstL == 16);

    send_pclear(0U, 0U, 1U, 1234U); // the followed grant's
    assert(opts.trunk_is_tuned == 0);
}

/* A grant for another call on the followed slot ends that assignment (issue #903: the receiver returns to the
   control channel, nothing holding the companion slot), and its repeat is followed as any grant is. That follow is the
   call a clear is matched to: one for the replaced talkgroup changes nothing, and one for the new grant's applies. */
static void
test_p_clear_follows_in_place_grant(void) {
    follow_group_ts1();
    dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*tg*/ 6789, /*src*/ 61);
    assert(opts.trunk_is_tuned == 0);
    dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*tg*/ 6789, /*src*/ 61);
    const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    assert(ctx->vc_tg == 6789 && ctx->vc_is_group == 1);
    assert(opts.trunk_is_tuned == 1);
    state.dmrburstL = 16;
    state.dmrburstR = 9;

    send_pclear(0U, 0U, 1U, 1234U); // the replaced grant's talkgroup
    assert(opts.trunk_is_tuned == 1);
    assert(state.dmrburstL == 16);

    send_pclear(0U, 0U, 1U, 6789U);
    assert(opts.trunk_is_tuned == 0);
}

/* A private data grant from 3003 to 2002 on the followed slot of a private voice call from 1001 to 2002 is another call,
   not a repeat: it ends that assignment (issue #903), and its repeat is followed as the data call it is, so a clear
   naming its sender applies and one naming the voice call's sender does not. A repeat of the voice grant, its parties
   in either order, keeps the call as it was. */
static void
test_p_clear_private_data_grant_is_not_voice_repeat(void) {
    init_env();
    dmr_sm_emit_indiv_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*dst*/ 2002, /*src*/ 1001);
    assert(opts.trunk_is_tuned == 1);
    const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    dmr_sm_event_t ev = dmr_sm_ev_indiv_grant(VC_HZ, 20, /*dst*/ 2002, /*src*/ 3003);
    ev.slot = 0;
    ev.kind = DSD_CALL_KIND_DATA;
    assert(dmr_sm_submit_grant(&opts, &state, &ev) == DMR_SM_GRANT_REFUSED);
    assert(opts.trunk_is_tuned == 0);
    assert(dmr_sm_submit_grant(&opts, &state, &ev) == DMR_SM_GRANT_FOLLOWED);
    assert(ctx->vc_src == 3003 && ctx->vc_dst == 2002 && ctx->vc_data_call == 1);
    state.dmrburstL = 16;
    state.dmrburstR = 9;
    send_pclear(0U, 0U, 0U, 1001U); // the voice call's sender: not the followed call now
    assert(opts.trunk_is_tuned == 1);
    send_pclear(0U, 0U, 0U, 3003U);
    assert(opts.trunk_is_tuned == 0);

    init_env();
    dmr_sm_emit_indiv_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*dst*/ 2002, /*src*/ 1001);
    dmr_sm_emit_indiv_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*dst*/ 1001, /*src*/ 2002);
    assert(ctx->vc_src == 1001 && ctx->vc_dst == 2002 && ctx->vc_data_call == 0);
    assert(opts.trunk_is_tuned == 1);
    // A private voice grant from a third radio to the same target is another call: it ends the assignment.
    dmr_sm_emit_indiv_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*dst*/ 2002, /*src*/ 3003);
    assert(opts.trunk_is_tuned == 0);
}

/* Fail closed (decided limit, issue #903): a clear is matched to the followed grant's own call only. An in-place grant
   for the call a checked LC named on the followed slot keeps the slot's record (and its follow time) the LC's and the
   followed grant's identity as it was, so a clear naming the LC's call matches nothing, and one naming the followed
   grant's call applies. Group, then a private pair named by the LC in the other order than the grant. */
static void
test_p_clear_in_place_grant_matching_slot_lc(void) {
    follow_group_ts1();
    observe_call(0U, DSD_CALL_KIND_GROUP_VOICE, 5678U, 51U);
    dmr_sm_note_lc_identity(&opts, &state, 0, DSD_CALL_KIND_GROUP_VOICE, 5678U, 51U);
    const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    const double lc_follow_m = ctx->slots[0].follow_m;
    dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*tg*/ 5678, /*src*/ 51);
    assert(ctx->vc_tg == 1234 && ctx->vc_is_group == 1);
    assert(ctx->slots[0].prov == (uint8_t)DMR_SM_ID_LC && fabs(ctx->slots[0].follow_m - lc_follow_m) < 1e-9);
    send_pclear(0U, 0U, 1U, 5678U); // the LC's call: not the followed grant's
    assert(opts.trunk_is_tuned == 1 && state.dmrburstL == 16);
    send_pclear(0U, 0U, 1U, 1234U);
    assert(opts.trunk_is_tuned == 0);

    for (int party = 0; party < 2; party++) {
        init_env();
        dmr_sm_emit_indiv_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*dst*/ 101, /*src*/ 102);
        assert(opts.trunk_is_tuned == 1);
        observe_call(0U, DSD_CALL_KIND_PRIVATE_VOICE, 2002U, 3003U);
        dmr_sm_note_lc_identity(&opts, &state, 0, DSD_CALL_KIND_PRIVATE_VOICE, 2002U, 3003U);
        dsd_call_snapshot call;
        assert(dsd_call_state_get(&state, 0U, &call) > 0);
        const uint64_t lc_epoch = dsd_call_snapshot_transmission(&call);
        dmr_sm_emit_indiv_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*dst*/ 3003, /*src*/ 2002);
        assert(ctx->vc_dst == 101 && ctx->vc_src == 102 && ctx->vc_is_group == 0);
        assert(ctx->slots[0].prov == (uint8_t)DMR_SM_ID_LC);
        // The first voice sync publishes nothing over the LC: its talker 3003 stays, in the same transmission.
        accepted_voice_sync(0U);
        assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE);
        assert(dsd_call_snapshot_transmission(&call) == lc_epoch);
        assert(call.ota_source_id == 3003U && call.ota_target_id == 2002U);
        state.dmrburstL = 16;
        state.dmrburstR = 9;
        send_pclear(0U, 0U, 0U, party == 0 ? 2002U : 3003U); // a party of the LC's call
        assert(opts.trunk_is_tuned == 1);
        send_pclear(0U, 0U, 0U, party == 0 ? 101U : 102U);
        assert(opts.trunk_is_tuned == 0);
    }
}

/* Fail closed (decided limit, issue #903): a grant for another call on the followed TS1 retires it while TS2 carries a
   call, so the receiver stays for TS2. The adopted companion's call matches no addressed clear, whether or not an
   earlier grant named it (TG 700): a clear for TS1's retired TG 300 and one for TG 700 on TS2 both change nothing; the
   call ends on its terminator, timeout or hangtime. */
static void
test_p_clear_after_companion_adoption(void) {
    for (int variant = 0; variant < 2; variant++) {
        init_env();
        dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*tg*/ 300, /*src*/ 30);
        observe_call(0U, DSD_CALL_KIND_GROUP_VOICE, 300U, 30U);
        accepted_voice_sync(0U);
        if (variant == 0) {
            dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 1, /*tg*/ 700, /*src*/ 70);
        }
        observe_call(1U, DSD_CALL_KIND_GROUP_VOICE, 700U, 70U);
        accepted_voice_sync(1U);
        dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*tg*/ 400, /*src*/ 40);
        const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
        assert(opts.trunk_is_tuned == 1 && ctx->vc_slot == 1 && ctx->vc_identity_known == 0);
        state.dmrburstL = 9;
        state.dmrburstR = 16;
        send_pclear(1U, 0U, 1U, 300U); // the retired slot's talkgroup
        assert(opts.trunk_is_tuned == 1 && state.dmrburstR == 16 && ctx->slots[1].voice_active == 1);
        send_pclear(1U, 0U, 1U, 700U);
        assert(opts.trunk_is_tuned == 1 && state.dmrburstR == 16 && ctx->slots[1].voice_active == 1);
    }
}

/* A clear of the followed TS1 before its first voice, while TS2 carries a call, forces nothing: the receiver stays for
   TS2. It retires TS1's pending grant all the same: the grant no longer counts as acquiring the slot, and the next
   voice there (its header missed) is a new transmission the cleared grant does not name. */
static void
test_p_clear_before_first_voice_retires_the_grant(void) {
    init_env();
    dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*tg*/ 1234, /*src*/ 42);
    observe_call(1U, DSD_CALL_KIND_GROUP_VOICE, 4321U, 43U);
    accepted_voice_sync(1U);
    state.dmrburstL = 9;
    state.dmrburstR = 16;
    send_pclear(0U, 0U, 1U, 1234U);
    const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    assert(opts.trunk_is_tuned == 1 && ctx->slots[1].voice_active == 1 && ctx->vc_slot == 0);
    assert(ctx->vc_identity_published == 1 && ctx->vc_media_heard == 1);
    dmr_sm_busy_view_t view;
    (void)dmr_sm_describe_held_calls(dmr_sm_get_ctx(), &opts, &state, dsd_decode_now_mono_s(), &view);
    assert(view.held && view.members == 0x2);
    accepted_voice_sync(0U);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0U, &call) <= 0 || call.ota_target_id != 1234U);
}

/* A clear of the followed TS1 before its first voice, while the clear's own slot status calls TS2 busy but no voice
   was accepted there, forces nothing; the cleared TS1 is no slot of the carrier's acquisition all the same, so the
   carrier holds nothing against other candidates and the next tick returns to the control channel, never waiting out
   the cleared grant's timeout (variant 0). A grant naming a new call on TS1 before that tick is acquired as any grant
   is, and holds the carrier (variant 1). */
static void
test_p_clear_cleared_slot_holds_no_acquisition(void) {
    for (int regrant = 0; regrant < 2; regrant++) {
        init_env();
        dsd_decode_clock_use_test(k_t0_ns);
        clock_at_ms(0U);
        dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*tg*/ 1234, /*src*/ 42);
        state.dmrburstL = 9;
        state.dmrburstR = 16;
        send_pclear(0U, 0U, 1U, 1234U);
        assert(opts.trunk_is_tuned == 1 && state.trunk_sm_force_release == 0);
        if (regrant) {
            clock_at_ms(100U);
            dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*tg*/ 5555, /*src*/ 55);
            assert(dmr_sm_get_ctx()->vc_tg == 5555 && dmr_sm_get_ctx()->vc_slot == 0);
        }
        dmr_sm_busy_view_t view;
        (void)dmr_sm_describe_held_calls(dmr_sm_get_ctx(), &opts, &state, dsd_decode_now_mono_s(), &view);
        assert(view.held == regrant);
        clock_at_ms(1000U); // past any settle after the follow, well within the grant timeout
        opts.frame_dmr = 1;
        dmr_sm_tick_ctx(dmr_sm_get_ctx(), &opts, &state);
        assert(opts.trunk_is_tuned == regrant);
        dsd_decode_clock_use_system();
    }
}

/* A clear of the followed TS1 before its first voice forces the return, which the tuner defers (the latch retries it):
   the cleared slot is retired all the same, so voice heard there before the retry (its header missed) is never
   published as the cleared grant's call. */
static void
test_p_clear_deferred_return_retires_the_slot(void) {
    init_env();
    dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*tg*/ 1234, /*src*/ 42);
    state.dmrburstL = 16;
    state.dmrburstR = 9;
    g_return_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    send_pclear(0U, 0U, 1U, 1234U);
    g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
    const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    assert(opts.trunk_is_tuned == 1 && ctx->state == DMR_SM_TUNED && state.trunk_sm_force_release == 1);
    assert(ctx->vc_identity_known == 0 && ctx->vc_identity_published == 1);
    observe_call(0U, DSD_CALL_KIND_VOICE, 0U, 0U);
    accepted_voice_sync(0U);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0U, &call) <= 0 || call.ota_target_id != 1234U);
    state.trunk_sm_force_release = 0;
}

/* A slotless grant's clear (it names the grant's call on either slot before its first voice) that forces nothing, the
   clear's slot status calling TS2 busy, retires the grant: voice on TS2 afterwards (its header missed) is never
   published as the cleared grant's call. */
static void
test_p_clear_retires_a_slotless_grant(void) {
    init_env();
    dmr_sm_emit_group_grant(&opts, &state, VC_HZ, /*lpcn*/ 0, /*tg*/ 1234, /*src*/ 42);
    const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    assert(opts.trunk_is_tuned == 1 && ctx->vc_slot == -1 && ctx->vc_identity_known == 1);
    state.dmrburstL = 9;
    state.dmrburstR = 16;
    send_pclear(0U, 0U, 1U, 1234U);
    assert(opts.trunk_is_tuned == 1 && state.trunk_sm_force_release == 0);
    assert(ctx->vc_identity_known == 0 && ctx->vc_identity_published == 1);
    observe_call(1U, DSD_CALL_KIND_VOICE, 0U, 0U);
    accepted_voice_sync(1U);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 1U, &call) <= 0 || call.ota_target_id != 1234U);
}

/* A voice sync whose burst supplied no accepted voice (the confidence gate has not corroborated it) publishes nothing
   and spends nothing; the first sync with accepted voice publishes the followed grant's call. */
static void
test_unaccepted_sync_publishes_no_grant(void) {
    init_env();
    dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*tg*/ 1234, /*src*/ 42);
    dmr_sm_emit_voice_sync(&opts, &state, 0);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE);
    assert(dmr_sm_get_ctx()->vc_identity_published == 0);
    accepted_voice_sync(0U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
           && call.ota_target_id == 1234U);
}

/* A grant is published only on voice the gate accepted since the slot's previous sync, never on the timestamp of voice
   an earlier sync already handled. TS2's call (its header missed, naming no one) is adopted for a retired TS1; a grant
   for TG 700 on TS2 then names the followed call, its publication pending. A bare TS2 sync after it, with no voice accepted
   since TS2's last sync, publishes nothing; the next sync after accepted voice publishes the grant. */
static void
test_publication_takes_the_bursts_own_verdict(void) {
    init_env();
    dsd_decode_clock_use_test(k_t0_ns);
    clock_at_ms(0U);
    dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*tg*/ 300, /*src*/ 30);
    observe_call(0U, DSD_CALL_KIND_GROUP_VOICE, 300U, 30U);
    accepted_voice_sync(0U);
    observe_call(1U, DSD_CALL_KIND_VOICE, 0U, 0U); // TS2's call, its header missed: it names no one yet
    clock_at_ms(10U);
    accepted_voice_sync(1U);
    clock_at_ms(20U);
    dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*tg*/ 400, /*src*/ 40);
    const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    assert(opts.trunk_is_tuned == 1 && ctx->vc_slot == 1 && ctx->vc_identity_known == 0);
    clock_at_ms(30U);
    dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 1, /*tg*/ 700, /*src*/ 70);
    assert(ctx->vc_identity_known == 1 && ctx->vc_tg == 700 && ctx->vc_identity_published == 0);
    clock_at_ms(40U);
    dmr_sm_emit_voice_sync(&opts, &state, 1); // TS2's voice was last accepted at 10 ms, before its sync then
    assert(ctx->vc_identity_published == 0);
    clock_at_ms(50U);
    accepted_voice_sync(1U);
    assert(ctx->vc_identity_published == 1);
    dsd_decode_clock_use_system();
}

/* A clear naming either party of the followed private grant applies while the grant's own transmission is on the slot.
   Fail closed (decided limit, issue #903): once that transmission ends and the reply begins (an embedded LC naming the
   other party as the talker, a new transmission), the grant identity names no call, so no addressed clear matches, even
   one naming a party of the same pair; the reply ends on its terminator, timeout or hangtime. */
static void
test_p_clear_private_reverse_leg(void) {
    for (int reply = 0; reply < 2; reply++) {
        for (int party = 0; party < 2; party++) {
            init_env();
            dmr_sm_emit_indiv_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*dst*/ 2002, /*src*/ 1001);
            assert(opts.trunk_is_tuned == 1);
            state.dmrburstL = 16;
            state.dmrburstR = 9;
            accepted_voice_sync(0U); // publishes the grant's 1001 -> 2002
            if (reply) {
                assert(dsd_call_state_end_ex(&state, 0U, dsd_decode_now_mono_s(), DSD_CALL_END_TERMINATOR) > 0);
                observe_call(0U, DSD_CALL_KIND_PRIVATE_VOICE, 1001U, 2002U); // the reply: 2002 -> 1001
                dmr_sm_note_lc_identity(&opts, &state, 0, DSD_CALL_KIND_PRIVATE_VOICE, 1001U, 2002U);
                accepted_voice_sync(0U);
            }
            send_pclear(0U, 0U, 0U, 3003U); // neither party
            assert(opts.trunk_is_tuned == 1);
            send_pclear(0U, 0U, 0U, party == 0 ? 1001U : 2002U);
            assert(opts.trunk_is_tuned == (reply ? 1 : 0));
            assert(state.dmrburstL == (reply ? 16 : 9));
        }
    }
}

/* Decided limit (discussion #900): under DMR mono playout (-fr) a clear of the followed TS1 call while TS2's accepted
   voice keeps the receiver on the carrier leaves playout on the followed slot. TS2 is not played until the receiver
   moves, and the followed slot stays TS1 for P_CLEAR addressing. */
static void
test_p_clear_mono_stays_on_followed_slot(void) {
    dsd_decode_clock_use_test(k_t0_ns);
    clock_at_ms(0U);
    follow_group_ts1();
    opts.dmr_mono = 1;
    observe_call(1U, DSD_CALL_KIND_GROUP_VOICE, 4321U, 43U);
    state.dmrburstR = 16;
    accepted_voice_sync(0U);
    accepted_voice_sync(1U);
    const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    assert(state.dmr_mono_slot == 0);
    send_pclear(0U, 0U, 1U, 1234U);
    assert(opts.trunk_is_tuned == 1 && ctx->vc_slot == 0);
    assert(state.dmr_mono_slot == 0);
    clock_at_ms(30U);
    accepted_voice_sync(1U);
    assert(opts.trunk_is_tuned == 1 && state.dmr_mono_slot == 0);
    send_pclear(1U, 0U, 1U, 4321U); // still not the followed call's slot
    assert(opts.trunk_is_tuned == 1 && ctx->vc_slot == 0 && state.dmr_mono_slot == 0);
    dsd_decode_clock_use_system();
}

/* A checked clear naming another TSCC makes it a hunt candidate; the release still returns to the control channel.
   Neither the control channel itself, an unmapped channel nor an unchecked clear adds one. */
static void
test_p_clear_named_tscc_becomes_candidate(void) {
    const long tscc = 851500000L;
    follow_group_ts1();
    state.trunk_chan_map[30] = tscc;
    state.trunk_chan_map[31] = CC_HZ;
    send_pclear(1U, 31U, 1U, 999U); // the control channel itself; the companion's clear for another call
    send_pclear(1U, 32U, 1U, 999U); // no frequency
    assert(dsd_trunk_cc_candidates_peek(&state) == NULL || dsd_trunk_cc_candidates_peek(&state)->count == 0);

    // RAS signature, RAS not confirmed on this carrier: not checked.
    state.event_crc_invalid[1] = DSD_CRC_RAS_UNVERIFIED;
    send_pclear(1U, 30U, 1U, 999U);
    assert(candidate_count(tscc) == 0);
    state.event_crc_invalid[1] = 0U;

    send_pclear(1U, 30U, 1U, 999U); // checked, not for the followed call: still a candidate
    assert(candidate_count(tscc) == 1);
    assert(opts.trunk_is_tuned == 1);

    send_pclear(0U, 30U, 1U, 1234U);
    assert(opts.trunk_is_tuned == 0);
    assert(state.trunk_cc_freq == CC_HZ);
    assert(candidate_count(tscc) == 1);

    // The absolute form: LPCN 0xFFF and an appended block (TS 102 361-4 §7.1.1.1.2) naming CHAN 40 at 851.625 MHz.
    follow_group_ts1();
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pclear_fid(bits, bytes, 0U, 0x0FFFU, 1U, 1234U, 77U);
    bits[0] = 0U;
    bytes[0] = 46U;
    bits[96] = 1U;
    put_bits(bits, 98, 46U, 6);
    bytes[12] = (uint8_t)(0x80U | 46U);
    put_bits(bits, 112, 0U, 4);    // Cdeftype 0
    put_bits(bits, 118, 40U, 12);  // CHAN
    put_bits(bits, 130, 851U, 10); // TX MHz
    put_bits(bits, 140, 5000U, 13);
    put_bits(bits, 153, 851U, 10); // RX MHz
    put_bits(bits, 163, 5000U, 13);
    const dmr_cspdu_shape shape = {1U, 1U, 46U, 46U, 1U, 0U};
    state.currentslot = 0;
    dmr_cspdu_ex(&opts, &state, bits, bytes, 1U, 0U, &shape);
    assert(candidate_count(851625000L) == 1);
    assert(opts.trunk_is_tuned == 0);
    assert(state.trunk_cc_freq == CC_HZ);
}

/* The user's entry for @p lpcn as dmr_t3_overlay_apply() makes it, beside the shared row: SET at @p hz, HIDE for 0. */
static void
user_entry(dsd_state* s, uint16_t lpcn, long hz) {
    dmr_ext_t* ext = dmr_ext_get_or_create(s);
    assert(ext != NULL && ext->overlay.op[lpcn] == 0U);
    ext->overlay.op[lpcn] = (uint8_t)(hz != 0 ? DMR_T3_OVERLAY_SET : DMR_T3_OVERLAY_HIDE);
    ext->overlay.hz[lpcn] = hz;
    ext->overlay.count++;
}

/* A logical P_CLEAR TSCC reads the frequency DMR uses for it (the user's entry over the shared
   row): a set channel whose row another protocol emptied is still a candidate, at the user's frequency; a hidden one
   whose row another protocol filled is none. */
static void
test_p_clear_tscc_uses_the_users_channels(void) {
    const long set_hz = 851537500L;
    const long other_hz = 851562500L;
    follow_group_ts1();
    user_entry(&state, 33U, set_hz);
    user_entry(&state, 34U, 0L);
    dsd_state_set_trunk_chan_freq(&state, 33U, 0L);
    dsd_state_set_trunk_chan_freq(&state, 34U, other_hz);
    send_pclear(1U, 33U, 1U, 999U);
    assert(candidate_count(set_hz) == 1);
    send_pclear(1U, 34U, 1U, 999U);
    assert(candidate_count(other_hz) == 0);
}

/* The clear the encryption lockout synthesizes names no address and always applies. */
static void
test_synth_lockout_clear_still_releases(void) {
    follow_group_ts1();
    dmr_cspdu_release_channel(&opts, &state);
    assert(opts.trunk_is_tuned == 0);
}

/* A manufacturer's opcode 46 (another feature set) keeps its old handling: it is not read as Table 7.30. */
static void
test_vendor_fid_clear_unchanged(void) {
    follow_group_ts1();
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pclear_fid(bits, bytes, 0x10U, 0U, 1U, 999U, 77U);
    state.currentslot = 0;
    dmr_cspdu(&opts, &state, bits, bytes, 1, 0);
    assert(opts.trunk_is_tuned == 0);
}

/* A manufacturer's opcode 46 is no Table 7.30 P_CLEAR: 0xFFF in its bits 16-27 is its own content, not the absolute
   escape. The single CSBK keeps the control channel warm and takes the legacy carrier-wide release. */
static void
test_vendor_fid_clear_fff_bits_not_refused(void) {
    follow_group_ts1();
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pclear_fid(bits, bytes, 0x10U, 0x0FFFU, 1U, 999U, 77U);
    state.currentslot = 0;
    state.last_cc_sync_time = 0;
    dmr_cspdu(&opts, &state, bits, bytes, 1, 0);
    assert(state.last_cc_sync_time != 0);
    assert(opts.trunk_is_tuned == 0);
}

/* On the control channel (nothing followed, composite TSCC: the companion slot is the idle TSCC), a P_CLEAR on the
   payload slot, standard (FID @p fid 0, any address) or a manufacturer's, arms no forced release: a latched one would
   release the next followed grant on its first tick. */
static void
check_on_cc_clear_arms_no_forced_release(uint8_t fid) {
    init_env();
    assert(opts.trunk_is_tuned == 0);
    state.dmrburstL = 9;
    state.dmrburstR = 9;
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pclear_fid(bits, bytes, fid, 0U, 0U, 0xFFFED4U, 77U);
    state.currentslot = 1;
    dmr_cspdu(&opts, &state, bits, bytes, 1, 0);
    assert(state.trunk_sm_force_release == 0);

    dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*tg*/ 1234, /*src*/ 42);
    assert(opts.trunk_is_tuned == 1);
    dmr_sm_tick_ctx(dmr_sm_get_ctx(), &opts, &state);
    dmr_sm_tick_ctx(dmr_sm_get_ctx(), &opts, &state);
    assert(opts.trunk_is_tuned == 1);
}

static void
test_on_cc_clear_arms_no_forced_release(void) {
    check_on_cc_clear_arms_no_forced_release(0U);
    check_on_cc_clear_arms_no_forced_release(0x10U);
}

/* An idle XPT status marks the receiver tuned (RTL or rigctl input) while the machine stays on the control channel.
   The encryption lockout's synthesized clear there arms no forced release, so the next followed grant is kept; while a
   call is followed the same clear still releases it. */
static void
test_xpt_idle_tuned_lockout_clear_arms_no_forced_release(void) {
    init_env();
    dmr_sm_note_cc_activity(&opts, &state, CC_HZ);
    assert(dmr_sm_get_ctx()->state == DMR_SM_ON_CC);
    opts.trunk_is_tuned = 1; // as dmr_cspdu_xpt_update_cc_from_input() leaves it
    state.dmrburstL = 9;
    state.dmrburstR = 9;
    state.currentslot = 0;
    dmr_cspdu_release_channel(&opts, &state);
    assert(state.trunk_sm_force_release == 0);
    assert(dmr_sm_get_ctx()->state == DMR_SM_ON_CC);

    opts.trunk_is_tuned = 0;
    dmr_sm_emit_group_grant_slot(&opts, &state, VC_HZ, /*lpcn*/ 20, /*slot*/ 0, /*tg*/ 1234, /*src*/ 42);
    assert(opts.trunk_is_tuned == 1);
    dmr_sm_tick_ctx(dmr_sm_get_ctx(), &opts, &state);
    dmr_sm_tick_ctx(dmr_sm_get_ctx(), &opts, &state);
    assert(opts.trunk_is_tuned == 1);

    dmr_cspdu_release_channel(&opts, &state);
    assert(opts.trunk_is_tuned == 0);
    assert(state.trunk_sm_force_release == 0); // the release it forced took the latch
}

int
main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_to_freq,
        .return_to_cc_request = test_return_to_cc,
    });
    test_tg_hold_forces_release();
    test_p_clear_matching_group_releases();
    test_p_clear_individual_either_party_releases();
    test_p_clear_unrelated_target_ignored();
    test_p_clear_allmsi_releases();
    test_p_clear_busy_companion_kept();
    test_p_clear_matches_followed_grant();
    test_p_clear_follows_in_place_grant();
    test_p_clear_private_data_grant_is_not_voice_repeat();
    test_p_clear_in_place_grant_matching_slot_lc();
    test_p_clear_private_reverse_leg();
    test_p_clear_after_companion_adoption();
    test_p_clear_before_first_voice_retires_the_grant();
    test_p_clear_cleared_slot_holds_no_acquisition();
    test_p_clear_deferred_return_retires_the_slot();
    test_p_clear_retires_a_slotless_grant();
    test_unaccepted_sync_publishes_no_grant();
    test_publication_takes_the_bursts_own_verdict();
    test_p_clear_mono_stays_on_followed_slot();
    test_p_clear_named_tscc_becomes_candidate();
    test_p_clear_tscc_uses_the_users_channels();
    test_synth_lockout_clear_still_releases();
    test_vendor_fid_clear_unchanged();
    test_vendor_fid_clear_fff_bits_not_refused();
    test_on_cc_clear_arms_no_forced_release();
    test_xpt_idle_tuned_lockout_clear_arms_no_forced_release();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_state_ext_free_all(&state);
    printf("DMR_T3_FORCE_RELEASE: OK\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
