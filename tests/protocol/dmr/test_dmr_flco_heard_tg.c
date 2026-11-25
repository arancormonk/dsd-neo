// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR verified talkgroups (issue #903): a voice LC header or terminator whose own check passed (never one accepted under
 * relaxed RAS handling) proves the group its slot's call names, when it is a group voice channel user in a layout the
 * decoder reads positively for its raw FID:
 *
 * - FID 0 FLCO 0x00 (and the Kenwood scrambler and Hytera FID 0x68 forms of it): Tier II/III (DMR), Connect Plus on a
 *   carrier whose checked messages named Connect Plus, Hytera XPT (16-bit target) where checked messages named XPT
 *   and the XPT layout is the one the LC was read with. A Capacity Plus carrier's FID 0 LC is not read.
 * - Capacity Plus FID 0x10 FLCO 0x04 (8-bit group) only on a carrier checked messages named Capacity Plus.
 * - Capacity Max FID 0x10 FLCO 0x10 only on a carrier a checked Tier III system code named.
 *
 * Embedded LCs, grants and voice syncs never prove a group, unit-to-unit calls are never noted, and a terminator
 * promotes only the open transmission on its own slot that it names.
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/heard_talkgroup.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

enum { LC_HEADER = 1, LC_TERMINATOR = 2, LC_EMBEDDED = 3 };

static void
write_bits(uint8_t* bits, size_t start, uint64_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        bits[start + i] = (uint8_t)((value >> ((nbits - 1U) - i)) & 1U);
    }
}

/* A standard layout LC: 24-bit target and source. */
static void
build_lc(uint8_t bits[80], uint8_t flco, uint8_t fid, uint32_t target, uint32_t source) {
    DSD_MEMSET(bits, 0, 80U);
    write_bits(bits, 2U, flco, 6U);
    write_bits(bits, 8U, fid, 8U);
    write_bits(bits, 24U, target, 24U);
    write_bits(bits, 48U, source, 24U);
}

/* A Capacity Plus group voice LC (FID 0x10 FLCO 0x04): 8-bit group at 40, rest LSN at 51, 16-bit source at 56. Bits
   24..39 carry no part of the group. */
static void
build_capplus_lc(uint8_t bits[80], uint8_t tg, uint8_t rest_lsn, uint16_t source) {
    DSD_MEMSET(bits, 0, 80U);
    write_bits(bits, 2U, 0x04U, 6U);
    write_bits(bits, 8U, 0x10U, 8U);
    write_bits(bits, 24U, 0xA5A5U, 16U);
    write_bits(bits, 40U, tg, 8U);
    write_bits(bits, 51U, rest_lsn, 5U);
    write_bits(bits, 56U, source, 16U);
}

static dsd_opts*
new_opts(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    assert(opts != NULL);
    return opts;
}

static dsd_state*
new_state(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->currentslot = 0;
    return state;
}

static void
free_state(dsd_state* state) {
    dsd_state_ext_free_all(state);
    free(state);
}

/* Run @p bits as an LC of @p type on @p slot with check verdict @p crc and slot CRC flags @p crc_flags. */
static void
run_bits(dsd_opts* opts, dsd_state* state, uint8_t bits[80], uint8_t type, int slot, uint32_t crc, uint8_t crc_flags) {
    state->currentslot = slot;
    const int idx = slot >= 1 ? 1 : 0;
    state->event_crc_invalid[idx] = crc_flags;
    uint32_t irr = 0U;
    dmr_flco(opts, state, bits, crc, &irr, type);
    state->event_crc_invalid[idx] = 0U;
}

static void
run_lc(dsd_opts* opts, dsd_state* state, uint8_t type, int slot, uint8_t flco, uint8_t fid, uint32_t target,
       uint32_t source) {
    uint8_t bits[80];
    build_lc(bits, flco, fid, target, source);
    run_bits(opts, state, bits, type, slot, 1U, 0U);
}

static dsd_call_snapshot
slot_call(const dsd_state* state, uint8_t slot) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof call);
    (void)dsd_call_state_get(state, slot, &call);
    return call;
}

static int
proved(const dsd_state* state, uint8_t slot, dsd_heard_tg_variant variant, uint32_t id) {
    const dsd_call_snapshot call = slot_call(state, slot);
    return call.heard_noted == 1U && call.heard_variant == (uint8_t)variant && call.heard_id == id;
}

static int
not_noted(const dsd_state* state, uint8_t slot) {
    const dsd_call_snapshot call = slot_call(state, slot);
    return call.heard_noted == 0U && call.heard_variant == 0U;
}

/* A checked FID 0 group header on Tier II proves its 24-bit group as DMR; on Tier III too. */
static void
header_fid0_group_is_dmr(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 0x123456U, 0x456U);
    assert(slot_call(state, 0U).phase == DSD_CALL_PHASE_ACTIVE);
    assert(proved(state, 0U, DSD_HEARD_TG_DMR, 0x123456U));
    free_state(state);

    state = new_state();
    dsd_rxid_note_dmr_t3_syscode(state, 0x1234U);
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 0x000321U, 0x456U);
    assert(proved(state, 0U, DSD_HEARD_TG_DMR, 0x000321U));
    free_state(state);

    // A Hytera FID 0x68 group voice user on Tier II reads the standard layout too.
    state = new_state();
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x68U, 1001U, 2002U);
    assert(proved(state, 0U, DSD_HEARD_TG_DMR, 1001U));
    free_state(state);

    // So does a Kenwood scrambler LC (raw FID 0x20, PF and the privacy bit set, check passed).
    state = new_state();
    uint8_t bits[80];
    build_lc(bits, 0x00U, 0x20U, 0x123U, 0x456U);
    bits[0] = 1U;
    write_bits(bits, 16U, 0x40U, 8U);
    run_bits(opts, state, bits, LC_HEADER, 0, 1U, 0U);
    assert(proved(state, 0U, DSD_HEARD_TG_DMR, 0x123U));
    free_state(state);

    // An all-call group is checked but is no ordinary talkgroup: noted, nothing remembered.
    state = new_state();
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 0xFFFFFFU, 0x456U);
    const dsd_call_snapshot call = slot_call(state, 0U);
    assert(call.heard_noted == 1U && call.heard_variant == (uint8_t)DSD_HEARD_TG_NONE);
    free_state(state);
    free(opts);
}

/* Unit-to-unit and unknown FLCOs, a failed check, an FEC failure and a protected LC prove nothing. */
static void
header_not_group_or_unchecked_is_not_noted(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    run_lc(opts, state, LC_HEADER, 0, 0x03U, 0x00U, 0x123U, 0x456U);
    assert(slot_call(state, 0U).phase == DSD_CALL_PHASE_ACTIVE && not_noted(state, 0U));
    free_state(state);

    // Capacity Max unit-to-unit (FID 0x10 FLCO 0x13) on a Tier III carrier.
    state = new_state();
    dsd_rxid_note_dmr_t3_syscode(state, 0x1234U);
    run_lc(opts, state, LC_HEADER, 0, 0x13U, 0x10U, 0x123U, 0x456U);
    assert(slot_call(state, 0U).phase == DSD_CALL_PHASE_ACTIVE && not_noted(state, 0U));
    free_state(state);

    // A Motorola FLCO dsd-neo reads without a published layout (0x00 under FID 0x10), and an undefined Hytera one.
    state = new_state();
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x10U, 0x123U, 0x456U);
    assert(not_noted(state, 0U));
    free_state(state);
    state = new_state();
    run_lc(opts, state, LC_HEADER, 0, 0x01U, 0x68U, 0x123U, 0x456U);
    assert(not_noted(state, 0U));
    free_state(state);

    uint8_t bits[80];
    state = new_state();
    build_lc(bits, 0x00U, 0x00U, 0x123U, 0x456U);
    run_bits(opts, state, bits, LC_HEADER, 0, 0U, DSD_CRC_FAILED);
    assert(not_noted(state, 0U));
    free_state(state);

    state = new_state();
    build_lc(bits, 0x00U, 0x00U, 0x123U, 0x456U);
    state->currentslot = 0;
    uint32_t irr = 1U;
    dmr_flco(opts, state, bits, 1U, &irr, LC_HEADER);
    assert(not_noted(state, 0U));
    free_state(state);

    state = new_state();
    build_lc(bits, 0x00U, 0x00U, 0x123U, 0x456U);
    bits[0] = 1U; // PF: the payload is ciphertext
    run_bits(opts, state, bits, LC_HEADER, 0, 1U, 0U);
    assert(not_noted(state, 0U));
    free_state(state);
    free(opts);
}

/* An LC accepted under relaxed RAS handling opens and follows the call but proves nothing, header or terminator; a
   later checked header of the same transmission does. */
static void
ras_relaxed_lc_is_not_noted(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    uint8_t bits[80];
    build_lc(bits, 0x00U, 0x00U, 0x123U, 0x456U);
    run_bits(opts, state, bits, LC_HEADER, 0, 1U, DSD_CRC_RAS_UNVERIFIED);
    assert(slot_call(state, 0U).phase == DSD_CALL_PHASE_ACTIVE && not_noted(state, 0U));
    run_bits(opts, state, bits, LC_TERMINATOR, 0, 1U, DSD_CRC_RAS_UNVERIFIED);
    assert(not_noted(state, 0U));
    free_state(state);

    state = new_state();
    run_bits(opts, state, bits, LC_HEADER, 0, 1U, DSD_CRC_RAS_UNVERIFIED);
    run_bits(opts, state, bits, LC_HEADER, 0, 1U, 0U);
    assert(proved(state, 0U, DSD_HEARD_TG_DMR, 0x123U));
    free_state(state);
    free(opts);
}

/* An embedded LC opens a late-entry call but proves nothing; the transmission's checked terminator promotes it before
   the slot ends. */
static void
embedded_only_then_terminator(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    run_lc(opts, state, LC_EMBEDDED, 0, 0x00U, 0x00U, 0x123U, 0x456U);
    assert(slot_call(state, 0U).phase == DSD_CALL_PHASE_ACTIVE && not_noted(state, 0U));
    run_lc(opts, state, LC_EMBEDDED, 0, 0x00U, 0x00U, 0x123U, 0x456U);
    assert(not_noted(state, 0U));
    run_lc(opts, state, LC_TERMINATOR, 0, 0x00U, 0x00U, 0x123U, 0x456U);
    const dsd_call_snapshot call = slot_call(state, 0U);
    assert(call.phase == DSD_CALL_PHASE_ENDED);
    assert(proved(state, 0U, DSD_HEARD_TG_DMR, 0x123U));
    free_state(state);
    free(opts);
}

/* A terminator promotes only the open transmission it names, on its own slot: another group or source, the other
   slot, an unknown slot or a failed check end the slot (or not) without a proof. */
static void
terminator_promotes_only_its_transmission(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    run_lc(opts, state, LC_EMBEDDED, 0, 0x00U, 0x00U, 0x123U, 0x456U);
    run_lc(opts, state, LC_TERMINATOR, 0, 0x00U, 0x00U, 0x124U, 0x456U);
    assert(not_noted(state, 0U));
    free_state(state);

    state = new_state();
    run_lc(opts, state, LC_EMBEDDED, 0, 0x00U, 0x00U, 0x123U, 0x456U);
    run_lc(opts, state, LC_TERMINATOR, 0, 0x00U, 0x00U, 0x123U, 0x457U);
    assert(not_noted(state, 0U));
    free_state(state);

    // A unit-to-unit terminator naming the same numbers.
    state = new_state();
    run_lc(opts, state, LC_EMBEDDED, 0, 0x00U, 0x00U, 0x123U, 0x456U);
    run_lc(opts, state, LC_TERMINATOR, 0, 0x03U, 0x00U, 0x123U, 0x456U);
    assert(not_noted(state, 0U));
    free_state(state);

    // The other slot's terminator.
    state = new_state();
    run_lc(opts, state, LC_EMBEDDED, 0, 0x00U, 0x00U, 0x123U, 0x456U);
    run_lc(opts, state, LC_TERMINATOR, 1, 0x00U, 0x00U, 0x123U, 0x456U);
    assert(slot_call(state, 0U).phase == DSD_CALL_PHASE_ACTIVE && not_noted(state, 0U));
    free_state(state);

    // A terminator whose slot is unknown.
    state = new_state();
    run_lc(opts, state, LC_EMBEDDED, 0, 0x00U, 0x00U, 0x123U, 0x456U);
    run_lc(opts, state, LC_TERMINATOR, 2, 0x00U, 0x00U, 0x123U, 0x456U);
    assert(not_noted(state, 0U) && not_noted(state, 1U));
    free_state(state);

    // A terminator whose check failed.
    state = new_state();
    run_lc(opts, state, LC_EMBEDDED, 0, 0x00U, 0x00U, 0x123U, 0x456U);
    uint8_t bits[80];
    build_lc(bits, 0x00U, 0x00U, 0x123U, 0x456U);
    run_bits(opts, state, bits, LC_TERMINATOR, 0, 0U, DSD_CRC_FAILED);
    assert(not_noted(state, 0U));
    free_state(state);

    // A checked header on slot 1 proves slot 1's call only.
    state = new_state();
    run_lc(opts, state, LC_EMBEDDED, 0, 0x00U, 0x00U, 0x123U, 0x456U);
    run_lc(opts, state, LC_HEADER, 1, 0x00U, 0x00U, 0x123U, 0x456U);
    assert(not_noted(state, 0U) && proved(state, 1U, DSD_HEARD_TG_DMR, 0x123U));
    free_state(state);

    // A header whose slot is unknown proves nothing.
    state = new_state();
    run_lc(opts, state, LC_HEADER, 2, 0x00U, 0x00U, 0x123U, 0x456U);
    assert(not_noted(state, 0U) && not_noted(state, 1U));
    free_state(state);
    free(opts);
}

static dsd_trunk_tune_result
test_tune(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    (void)request_id;
    opts->rtlsdr_center_freq = freq;
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
test_return(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    opts->trunk_is_tuned = 0;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

/* A followed group grant whose voice sync opens the call (the burst then lost) proves nothing: the grant's identity is
   copied, not heard. Only the channel's own checked header does. */
static void
grant_then_voice_sync_is_not_noted(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    state->trunk_cc_freq = 851000000L;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune,
        .tune_to_cc_request = test_tune,
        .return_to_cc_request = test_return,
    });
    dmr_sm_init(opts, state);
    dmr_sm_event_t grant = dmr_sm_ev_group_grant(852000000L, 0, 100, 200);
    dmr_sm_event(dmr_sm_get_ctx(), opts, state, &grant);
    // The burst's voice passed its gate, so the sync binds the slotless follow to slot 0 and opens the grant's call.
    state->dmr_confidence_voice_open[0] = 1U;
    state->dmr_confidence_voice_m[0] = dsd_decode_now_mono_s();
    dmr_sm_emit_voice_sync(opts, state, 0);
    dsd_call_snapshot call = slot_call(state, 0U);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_target_id == 100U);
    assert(not_noted(state, 0U));
    // A voice sync of an unknown slot neither.
    dmr_sm_emit_voice_sync(opts, state, -1);
    assert(not_noted(state, 0U) && not_noted(state, 1U));
    // The header of this transmission does.
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 100U, 200U);
    assert(proved(state, 0U, DSD_HEARD_TG_DMR, 100U));
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    free_state(state);
    free(opts);
}

/* Capacity Plus: the 8-bit group of FID 0x10 FLCO 0x04, only where checked messages named Capacity Plus (a rest LSN of
   this checked LC included); a FID 0 LC on such a carrier is not read. */
static void
capplus_group(void) {
    dsd_opts* opts = new_opts();
    uint8_t bits[80];

    dsd_state* state = new_state();
    build_capplus_lc(bits, 0x2AU, 0U, 0x1234U); // no rest LSN, nothing else names Capacity Plus
    run_bits(opts, state, bits, LC_HEADER, 0, 1U, 0U);
    assert(slot_call(state, 0U).phase == DSD_CALL_PHASE_ACTIVE && slot_call(state, 0U).ota_target_id == 0x2AU);
    assert(not_noted(state, 0U));
    free_state(state);

    state = new_state();
    build_capplus_lc(bits, 0x2AU, 3U, 0x1234U); // the checked LC names its rest LSN
    run_bits(opts, state, bits, LC_HEADER, 0, 1U, 0U);
    assert(proved(state, 0U, DSD_HEARD_TG_CAPPLUS, 0x2AU));
    free_state(state);

    // Late entry: the embedded LC opens the call, the terminator proves it on a carrier already named Capacity Plus.
    state = new_state();
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    build_capplus_lc(bits, 0x07U, 0U, 0x1234U);
    run_bits(opts, state, bits, LC_EMBEDDED, 0, 1U, 0U);
    assert(not_noted(state, 0U));
    run_bits(opts, state, bits, LC_TERMINATOR, 0, 1U, 0U);
    assert(proved(state, 0U, DSD_HEARD_TG_CAPPLUS, 0x07U));
    free_state(state);

    // A RAS-accepted Capacity Plus LC names no rest LSN to the record and proves nothing.
    state = new_state();
    build_capplus_lc(bits, 0x2AU, 3U, 0x1234U);
    run_bits(opts, state, bits, LC_HEADER, 0, 1U, DSD_CRC_RAS_UNVERIFIED);
    assert(not_noted(state, 0U));
    free_state(state);

    // A standard LC on a Capacity Plus carrier.
    state = new_state();
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 0x2AU, 0x1234U);
    assert(not_noted(state, 0U));
    free_state(state);

    // A trunk-scan revisit restores the Capacity Plus branding while the received identity starts over: the standard
    // layout the LC is read with until fresh evidence arrives proves nothing.
    state = new_state();
    DSD_SNPRINTF(state->dmr_branding_sub, sizeof(state->dmr_branding_sub), "%s", "Cap+ ");
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 0x12002AU, 0x1234U);
    assert(slot_call(state, 0U).ota_target_id == 0x12002AU);
    assert(not_noted(state, 0U));
    free_state(state);
    free(opts);
}

/* Connect Plus: the 24-bit group of a standard LC, where checked messages named Connect Plus. */
static void
conplus_group(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    dsd_rxid_note_dmr_brand(state, "Connect Plus");
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 0x00ABCDU, 0x456U);
    assert(proved(state, 0U, DSD_HEARD_TG_CONPLUS, 0x00ABCDU));
    free_state(state);
    free(opts);
}

/* A Hytera XPT call alert LC on slot 0, whose own check passed unless @p ras. */
static void
xpt_alert(dsd_opts* opts, dsd_state* state, int ras) {
    uint8_t bits[80];
    DSD_MEMSET(bits, 0, 80U);
    bits[1] = 1U;
    write_bits(bits, 2U, 0x09U, 6U);
    write_bits(bits, 8U, 0x68U, 8U);
    write_bits(bits, 32U, 0x0005U, 16U);
    run_bits(opts, state, bits, LC_HEADER, 0, 1U, ras ? DSD_CRC_RAS_UNVERIFIED : 0U);
}

/* Hytera XPT: where a checked message named XPT, the standard LC is read with the 16-bit XPT target and proves it as
   XPT. A relaxed XPT indication changes nothing: a later clean standard header keeps its 24-bit target as DMR. Branding
   that outlived its carrier's checked evidence leaves the LC read with the standard layout, which may have misread a
   16-bit target, so nothing is proved. */
static void
xpt_group(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    xpt_alert(opts, state, 0);
    assert(strcmp(state->dmr_branding_sub, "XPT ") == 0);
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 0x120007U, 0x3456U);
    assert(slot_call(state, 0U).ota_target_id == 0x0007U);
    assert(proved(state, 0U, DSD_HEARD_TG_XPT, 0x0007U));
    free_state(state);

    state = new_state();
    xpt_alert(opts, state, 0);
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x68U, 0x000009U, 0x3456U);
    assert(proved(state, 0U, DSD_HEARD_TG_XPT, 0x0009U));
    free_state(state);

    state = new_state();
    xpt_alert(opts, state, 1);
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 0x123456U, 0x456U);
    assert(slot_call(state, 0U).ota_target_id == 0x123456U);
    assert(proved(state, 0U, DSD_HEARD_TG_DMR, 0x123456U));
    free_state(state);

    state = new_state();
    xpt_alert(opts, state, 0);
    dsd_rxid_reset(state); // a trunk-scan revisit; the restored branding text stays
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 0x120007U, 0x3456U);
    assert(slot_call(state, 0U).ota_target_id == 0x120007U); // read with the standard layout
    assert(not_noted(state, 0U));
    free_state(state);
    free(opts);
}

/* A vendor variant needs current carrier evidence. The no-carrier pass (dsd_rxid_mark_stale()) leaves the last
   received identity for display, no longer current: the next transmitter on the frequency may be another system's (a
   Tier II repeater, whose 0xFFFCA0 is an ordinary group and outside the Connect Plus range), so a standard LC proves
   nothing while only a stale vendor system is on record, and proves as the vendor's again once a checked message of
   the carrier names it. A stale Tier III record reads as DMR either way, so its standard LC still proves. */
static void
vendor_variant_needs_current_evidence(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    dsd_rxid_note_dmr_brand(state, "Connect Plus");
    dsd_rxid_mark_stale(state);
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 0xFFFCA0U, 0x456U);
    assert(slot_call(state, 0U).phase == DSD_CALL_PHASE_ACTIVE && slot_call(state, 0U).ota_target_id == 0xFFFCA0U);
    assert(not_noted(state, 0U));
    run_lc(opts, state, LC_TERMINATOR, 0, 0x00U, 0x00U, 0xFFFCA0U, 0x456U);
    assert(not_noted(state, 0U));
    free_state(state);

    state = new_state();
    dsd_rxid_note_dmr_brand(state, "Connect Plus");
    dsd_rxid_mark_stale(state);
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 0x00ABCDU, 0x456U);
    assert(not_noted(state, 0U));
    run_lc(opts, state, LC_TERMINATOR, 0, 0x00U, 0x00U, 0x00ABCDU, 0x456U);
    dsd_rxid_note_dmr_brand(state, "Connect Plus");
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 0x00ABCEU, 0x456U);
    assert(proved(state, 0U, DSD_HEARD_TG_CONPLUS, 0x00ABCEU));
    free_state(state);

    state = new_state();
    xpt_alert(opts, state, 0);
    run_lc(opts, state, LC_TERMINATOR, 0, 0x00U, 0x00U, 0x120007U, 0x3456U);
    dsd_rxid_mark_stale(state);
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 0x000009U, 0x3457U);
    assert(slot_call(state, 0U).phase == DSD_CALL_PHASE_ACTIVE && not_noted(state, 0U));
    free_state(state);

    state = new_state();
    dsd_rxid_note_dmr_t3_syscode(state, 0x1234U);
    dsd_rxid_mark_stale(state);
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 0x000321U, 0x456U);
    assert(proved(state, 0U, DSD_HEARD_TG_DMR, 0x000321U));
    free_state(state);
    free(opts);
}

/* Capacity Max group voice (FID 0x10 FLCO 0x10): a 24-bit group as DMR, only on a carrier a checked Tier III system
   code named. The Capacity Max marker alone (which -F may take from a RAS C_ALOHA) is no Tier III evidence. */
static void
capmax_group(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    run_lc(opts, state, LC_HEADER, 0, 0x10U, 0x10U, 0x000321U, 0x456U);
    assert(slot_call(state, 0U).phase == DSD_CALL_PHASE_ACTIVE && not_noted(state, 0U));
    free_state(state);

    state = new_state();
    dsd_rxid_note_dmr_t3_capmax(state, 0U, 1U);
    assert(dsd_rxid_dmr_t3_is_capmax(state, 0U, 1U));
    run_lc(opts, state, LC_HEADER, 0, 0x10U, 0x10U, 0x000321U, 0x456U);
    assert(slot_call(state, 0U).phase == DSD_CALL_PHASE_ACTIVE && not_noted(state, 0U));
    free_state(state);

    state = new_state();
    dsd_rxid_note_dmr_t3_syscode(state, 0x1234U);
    run_lc(opts, state, LC_HEADER, 0, 0x10U, 0x10U, 0x000321U, 0x456U);
    assert(proved(state, 0U, DSD_HEARD_TG_DMR, 0x000321U));
    free_state(state);
    free(opts);
}

typedef enum { CARRIER_XPT, CARRIER_CONPLUS, CARRIER_CAPMAX } trunk_carrier;

/* A fresh state on a carrier whose checked messages named @p carrier. */
static dsd_state*
carrier_state(dsd_opts* opts, trunk_carrier carrier) {
    dsd_state* state = new_state();
    if (carrier == CARRIER_XPT) {
        xpt_alert(opts, state, 0);
    } else if (carrier == CARRIER_CONPLUS) {
        dsd_rxid_note_dmr_brand(state, "Connect Plus");
    } else {
        dsd_rxid_note_dmr_t3_syscode(state, 0x1234U);
    }
    return state;
}

/* Late entry on @p carrier: an embedded LC (FLCO @p flco, FID @p fid) opens the call with 24-bit target field
   @p call_target, then a checked terminator with target field @p term_target ends it. */
static dsd_state*
late_entry_terminated(dsd_opts* opts, trunk_carrier carrier, uint8_t flco, uint8_t fid, uint32_t call_target,
                      uint32_t term_target) {
    dsd_state* state = carrier_state(opts, carrier);
    run_lc(opts, state, LC_EMBEDDED, 0, flco, fid, call_target, 0x3456U);
    assert(slot_call(state, 0U).phase == DSD_CALL_PHASE_ACTIVE && not_noted(state, 0U));
    run_lc(opts, state, LC_TERMINATOR, 0, flco, fid, term_target, 0x3456U);
    assert(slot_call(state, 0U).phase == DSD_CALL_PHASE_ENDED);
    return state;
}

/* A terminator on an XPT, Connect Plus or Capacity Max carrier promotes the open transmission it names, its target read
   from its bits in the carrier's layout before the slot ends (XPT: the 16-bit target, whatever bits 24..31 hold); one
   naming another target promotes nothing. */
static void
terminator_on_vendor_carriers(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = late_entry_terminated(opts, CARRIER_XPT, 0x00U, 0x00U, 0x120007U, 0x340007U);
    assert(proved(state, 0U, DSD_HEARD_TG_XPT, 0x0007U));
    free_state(state);
    state = late_entry_terminated(opts, CARRIER_XPT, 0x00U, 0x00U, 0x120007U, 0x120008U);
    assert(not_noted(state, 0U));
    free_state(state);

    state = late_entry_terminated(opts, CARRIER_CONPLUS, 0x00U, 0x00U, 0x00ABCDU, 0x00ABCDU);
    assert(proved(state, 0U, DSD_HEARD_TG_CONPLUS, 0x00ABCDU));
    free_state(state);
    state = late_entry_terminated(opts, CARRIER_CONPLUS, 0x00U, 0x00U, 0x00ABCDU, 0x01ABCDU);
    assert(not_noted(state, 0U));
    free_state(state);

    state = late_entry_terminated(opts, CARRIER_CAPMAX, 0x10U, 0x10U, 0x000321U, 0x000321U);
    assert(proved(state, 0U, DSD_HEARD_TG_DMR, 0x000321U));
    free_state(state);
    state = late_entry_terminated(opts, CARRIER_CAPMAX, 0x10U, 0x10U, 0x000321U, 0x000322U);
    assert(not_noted(state, 0U));
    free_state(state);
    free(opts);
}

/* The first checked LC of a transmission classifies it once; a repeat, its embedded LCs and its terminator change
   nothing. */
static void
noted_once_per_transmission(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 0x123U, 0x456U);
    const dsd_call_snapshot first = slot_call(state, 0U);
    assert(proved(state, 0U, DSD_HEARD_TG_DMR, 0x123U));
    run_lc(opts, state, LC_HEADER, 0, 0x00U, 0x00U, 0x123U, 0x456U);
    run_lc(opts, state, LC_EMBEDDED, 0, 0x00U, 0x00U, 0x123U, 0x456U);
    run_lc(opts, state, LC_TERMINATOR, 0, 0x00U, 0x00U, 0x123U, 0x456U);
    const dsd_call_snapshot last = slot_call(state, 0U);
    assert(last.epoch == first.epoch && proved(state, 0U, DSD_HEARD_TG_DMR, 0x123U));
    free_state(state);
    free(opts);
}

int
main(void) {
    header_fid0_group_is_dmr();
    header_not_group_or_unchecked_is_not_noted();
    ras_relaxed_lc_is_not_noted();
    embedded_only_then_terminator();
    terminator_promotes_only_its_transmission();
    grant_then_voice_sync_is_not_noted();
    capplus_group();
    conplus_group();
    xpt_group();
    vendor_variant_needs_current_evidence();
    capmax_group();
    terminator_on_vendor_carriers();
    noted_once_per_transmission();
    printf("DMR_FLCO_HEARD_TG: OK\n");
    return 0;
}
