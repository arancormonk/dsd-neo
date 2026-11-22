// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR Tier III channel provenance (discussion #900 F2): one over-the-air writer with an origin per row, FILE and USER
 * rows never overwritten over the air, SITE over the weaker origins, estimates redone when an anchor moves, a network
 * change dropping what the air taught, and the learned notice on the slot that carried it. Real emitter, real CSBK
 * handler; frequencies are invented.
 */

#include <assert.h>
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "dmr_abs_params.h"
#include "dmr_bptc_test_encoder.h"
#include "dmr_crc_provenance.h"
#include "dmr_t3_channels_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

extern void dmr_cspdu(dsd_opts*, dsd_state*, uint8_t*, uint8_t*, uint32_t, uint32_t);

static const long CC_HZ = 451000000L;

static dsd_opts g_opts;
static dsd_state g_state;
static Event_History_I g_history[2];
/* The System Identity Code (bits 40-53) send_c_bcast_mbc() puts in its header; reset() makes it 0. */
static uint16_t g_bcast_syscode;

static void
reset(void) {
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof g_opts);
    DSD_MEMSET(&g_state, 0, sizeof g_state);
    DSD_MEMSET(g_history, 0, sizeof g_history);
    g_state.event_history_s = g_history;
    init_event_history(&g_state.event_history_s[0], 0, 255);
    init_event_history(&g_state.event_history_s[1], 0, 255);
    g_state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    g_opts.trunk_enable = 1;
    g_opts.trunk_tune_group_calls = 1;
    g_opts.trunk_tune_private_calls = 1;
    g_opts.trunk_tune_data_calls = 1;
    g_opts.trunk_is_tuned = 0;
    g_state.trunk_cc_freq = CC_HZ;
    g_bcast_syscode = 0U;
}

static uint64_t
pushes(uint8_t slot) {
    return g_state.event_history_s[slot].push_seq;
}

static const char*
newest_row(uint8_t slot) {
    return g_state.event_history_s[slot].Event_History_Items[1].event_string;
}

static void
set_user(uint16_t lpcn, long hz) {
    dmr_ext_t* ext = dmr_ext_get_or_create(&g_state);
    assert(ext != NULL);
    g_state.trunk_chan_map[lpcn] = hz;
    ext->origin[lpcn] = (uint8_t)DMR_CHAN_ORIGIN_USER;
}

static void
put_bits(uint8_t* bits, size_t start, uint32_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        bits[start + i] = (uint8_t)((value >> (nbits - 1U - i)) & 1U);
    }
}

static void
build_pf0(uint8_t* bits, uint8_t* bytes, uint8_t opcode) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bits[0] = 1U; /* last block */
    bytes[0] = (uint8_t)(0x80U | (opcode & 0x3FU));
    put_bits(bits, 2, opcode & 0x3FU, 6);
}

/* The PDU in @p bits / @p bytes as a two-block MBC of @p opcode: header LB 0, one continuation carrying LB 1 and the
   header's CSBKO, as the assembler hands it over. */
static void
dispatch_mbc(uint8_t* bits, uint8_t* bytes, uint8_t opcode) {
    bits[0] = 0U;
    bytes[0] = (uint8_t)(opcode & 0x3FU);
    bits[96] = 1U;
    put_bits(bits, 98, opcode & 0x3FU, 6);
    bytes[12] = (uint8_t)(0x80U | (opcode & 0x3FU));
    const dmr_cspdu_shape shape = {1U, 1U, (uint8_t)(opcode & 0x3FU), (uint8_t)(opcode & 0x3FU), 1U, 0U};
    dmr_cspdu_ex(&g_opts, &g_state, bits, bytes, 1U, 0U, &shape);
}

static void
send_aloha(uint16_t syscode) {
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pf0(bits, bytes, 25U);
    put_bits(bits, 40, syscode & 0x3FFFU, 14);
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1U, 0U);
}

/* A C_BCAST MBC of announcement @p type: header channel @p header_ch (bits 68-79), BC_AP CHAN @p bcap_chan at
   @p rx_int MHz plus @p rx_step 125 Hz steps (TX the same), AC-info-available bit (56) @p ac_info. */
static void
send_c_bcast_mbc(uint8_t type, uint16_t header_ch, uint16_t bcap_chan, uint16_t rx_int, uint16_t rx_step,
                 uint8_t ac_info) {
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pf0(bits, bytes, 40U);
    put_bits(bits, 16, type & 0x1FU, 5);
    put_bits(bits, 40, g_bcast_syscode & 0x3FFFU, 14);
    bits[56] = (uint8_t)(ac_info & 1U);
    put_bits(bits, 68, header_ch & 0x0FFFU, 12);
    put_bits(bits, 112, 0U, 4);
    put_bits(bits, 118, bcap_chan & 0x0FFFU, 12);
    put_bits(bits, 130, rx_int & 0x03FFU, 10);
    put_bits(bits, 140, rx_step & 0x1FFFU, 13);
    put_bits(bits, 153, rx_int & 0x03FFU, 10);
    put_bits(bits, 163, rx_step & 0x1FFFU, 13);
    dispatch_mbc(bits, bytes, 40U);
}

/* C_BCAST Chan_Freq (type 5) for @p channel. */
static void
send_chan_freq(uint16_t channel, uint16_t rx_int, uint16_t rx_step) {
    send_c_bcast_mbc(5U, channel, channel, rx_int, rx_step, 0U);
}

/* C_BCAST Adjacent_Site (type 6) with absolute parameters for @p channel. */
static void
send_adjacent(uint16_t channel, uint16_t rx_int, uint16_t rx_step) {
    send_c_bcast_mbc(6U, 0x0FFFU, channel, rx_int, rx_step, 1U);
}

static int
candidate_count(long freq) {
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(&g_state);
    int n = 0;
    for (int i = 0; cc != NULL && i < cc->count; i++) {
        n += cc->candidates[i] == freq ? 1 : 0;
    }
    return n;
}

static int
hunt_list_count(long freq) {
    int n = 0;
    for (int i = 0; i < g_state.lcn_freq_count; i++) {
        n += *dsd_state_trunk_lcn_slot_const(&g_state, i) == freq ? 1 : 0;
    }
    return n;
}

/* TV_GRANT (opcode 49) to the absolute escape, its MBC naming @p channel. */
static void
send_absolute_grant(uint16_t channel, uint16_t rx_int, uint16_t rx_step) {
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pf0(bits, bytes, 49U);
    put_bits(bits, 16, 0x0FFFU, 12);
    put_bits(bits, 32, 1001U, 24);
    put_bits(bits, 56, 2002U, 24);
    put_bits(bits, 112, 0U, 4);
    put_bits(bits, 118, channel & 0x0FFFU, 12);
    put_bits(bits, 153, rx_int & 0x03FFU, 10);
    put_bits(bits, 163, rx_step & 0x1FFFU, 13);
    dispatch_mbc(bits, bytes, 49U);
}

static dsd_trunk_tune_result
tune_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    (void)request_id;
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
return_request(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    opts->trunk_is_tuned = 0;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
test_site_overrides_estimate_and_unverified(void) {
    reset();
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 10U, 451012500L, DMR_CHAN_ORIGIN_UNVERIFIED) == 1);
    assert(dmr_t3_chan_origin(&g_state, 10U) == DMR_CHAN_ORIGIN_UNVERIFIED);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 10U, 451025000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.trunk_chan_map[10] == 451025000L);
    assert(dmr_t3_chan_origin(&g_state, 10U) == DMR_CHAN_ORIGIN_SITE);

    assert(dmr_t3_chan_learn(&g_opts, &g_state, 20U, 451100000L, DMR_CHAN_ORIGIN_ESTIMATE) == 1);
    assert(dmr_t3_chan_origin(&g_state, 20U) == DMR_CHAN_ORIGIN_ESTIMATE);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 20U, 451112500L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.trunk_chan_map[20] == 451112500L);
    assert(dmr_t3_chan_origin(&g_state, 20U) == DMR_CHAN_ORIGIN_SITE);

    assert(dmr_t3_chan_learn(&g_opts, &g_state, 30U, 451200000L, DMR_CHAN_ORIGIN_NEIGHBOUR) == 1);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 30U, 451212500L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.trunk_chan_map[30] == 451212500L);
    assert(dmr_t3_chan_origin(&g_state, 30U) == DMR_CHAN_ORIGIN_SITE);

    /* Not channels: CHNULL, the absolute escape, and no frequency. */
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 0U, 451000000L, DMR_CHAN_ORIGIN_SITE) == 0);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 0xFFFU, 451000000L, DMR_CHAN_ORIGIN_SITE) == 0);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 40U, 0L, DMR_CHAN_ORIGIN_SITE) == 0);
    assert(g_state.trunk_chan_map[0xFFF] == 0L);
}

static void
test_file_and_user_never_overwritten_ota(void) {
    reset();
    g_state.trunk_chan_map[30] = 452000000L;
    dmr_t3_chan_note_map_loaded(&g_state);
    assert(dmr_t3_chan_origin(&g_state, 30U) == DMR_CHAN_ORIGIN_FILE);
    assert(dmr_t3_chan_origin(&g_state, 31U) == DMR_CHAN_ORIGIN_NONE);
    set_user(32U, 452025000L);

    /* A row set before anything recorded an origin (a -C map at startup) is FILE too. */
    g_state.trunk_chan_map[33] = 452050000L;
    assert(dmr_t3_chan_origin(&g_state, 33U) == DMR_CHAN_ORIGIN_FILE);

    static const dmr_chan_origin_e ota[] = {DMR_CHAN_ORIGIN_SITE, DMR_CHAN_ORIGIN_UNVERIFIED, DMR_CHAN_ORIGIN_NEIGHBOUR,
                                            DMR_CHAN_ORIGIN_ESTIMATE};
    for (size_t i = 0; i < sizeof ota / sizeof ota[0]; i++) {
        assert(dmr_t3_chan_learn(&g_opts, &g_state, 30U, 453000000L, ota[i]) == 0);
        assert(dmr_t3_chan_learn(&g_opts, &g_state, 32U, 453000000L, ota[i]) == 0);
        assert(dmr_t3_chan_learn(&g_opts, &g_state, 33U, 453000000L, ota[i]) == 0);
    }
    assert(g_state.trunk_chan_map[30] == 452000000L);
    assert(g_state.trunk_chan_map[32] == 452025000L);
    assert(g_state.trunk_chan_map[33] == 452050000L);
    assert(dmr_t3_chan_origin(&g_state, 32U) == DMR_CHAN_ORIGIN_USER);

    /* The writer is over the air only. */
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 34U, 453000000L, DMR_CHAN_ORIGIN_FILE) == 0);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 34U, 453000000L, DMR_CHAN_ORIGIN_USER) == 0);
    assert(g_state.trunk_chan_map[34] == 0L);

    dmr_t3_chan_note_map_cleared(&g_state);
    g_state.trunk_chan_map[30] = 0L;
    g_state.trunk_chan_map[32] = 0L;
    g_state.trunk_chan_map[33] = 0L;
    assert(dmr_t3_chan_origin(&g_state, 32U) == DMR_CHAN_ORIGIN_NONE);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 32U, 453000000L, DMR_CHAN_ORIGIN_SITE) == 1);
}

static void
test_site_change_notice_on_new_hz(void) {
    reset();
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 40U, 451012500L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(strstr(newest_row(0), "DMR TIII: Learned channel 40 -> 451.012500 MHz;") != NULL);
    const uint64_t before = pushes(0);
    const uint32_t rev = dmr_t3_chan_revision(&g_state);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 40U, 451037500L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.trunk_chan_map[40] == 451037500L);
    assert(pushes(0) == before + 1U);
    assert(strstr(newest_row(0), "DMR TIII: Channel 40 changed 451.012500 -> 451.037500 MHz;") != NULL);
    assert(dmr_t3_chan_revision(&g_state) != rev);

    /* The same frequency again is no change and no notice. */
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 40U, 451037500L, DMR_CHAN_ORIGIN_SITE) == 0);
    assert(pushes(0) == before + 1U);
}

static void
test_identical_hz_origin_promotion_bumps_revision(void) {
    reset();
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 50U, 451300000L, DMR_CHAN_ORIGIN_NEIGHBOUR) == 1);
    assert(g_state.dmr_lcn_trust[50] == 1U);
    const uint32_t rev = dmr_t3_chan_revision(&g_state);
    const uint64_t before = pushes(0);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 50U, 451300000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(dmr_t3_chan_origin(&g_state, 50U) == DMR_CHAN_ORIGIN_SITE);
    assert(dmr_t3_chan_revision(&g_state) != rev);
    assert(g_state.dmr_lcn_trust[50] == 2U);
    assert(pushes(0) == before); /* nothing the user sees changed */
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 50U, 451300000L, DMR_CHAN_ORIGIN_UNVERIFIED) == 0);
    assert(dmr_t3_chan_origin(&g_state, 50U) == DMR_CHAN_ORIGIN_SITE);
}

static void
test_ras_unverified_learn_fills_empty_only(void) {
    reset();
    dmr_sm_init(&g_opts, &g_state);
    g_opts.aggressive_framesync = 0; /* -F */
    g_state.currentslot = 0;
    g_state.dmr_ras_signatures = 2U;
    g_state.event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED;

    send_chan_freq(60U, 451U, 100U);
    assert(g_state.trunk_chan_map[60] == 451012500L);
    assert(dmr_t3_chan_origin(&g_state, 60U) == DMR_CHAN_ORIGIN_UNVERIFIED);
    assert(g_state.event_history_s[0].Event_History_Items[1].crc_invalid == DSD_CRC_RAS_UNVERIFIED);
    assert(strstr(newest_row(0), "Learned channel 60") != NULL);

    send_chan_freq(60U, 451U, 300U);
    assert(g_state.trunk_chan_map[60] == 451012500L);

    /* A checked one replaces it. */
    g_state.event_crc_invalid[0] = 0U;
    send_chan_freq(60U, 451U, 300U);
    assert(g_state.trunk_chan_map[60] == 451037500L);
    assert(dmr_t3_chan_origin(&g_state, 60U) == DMR_CHAN_ORIGIN_SITE);

    /* RAS not confirmed on this carrier, or a failed CRC: nothing is learned. */
    g_state.event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED;
    g_state.dmr_ras_signatures = 0U;
    send_chan_freq(61U, 451U, 500U);
    assert(g_state.trunk_chan_map[61] == 0L);
    g_state.dmr_ras_signatures = 2U;
    g_state.event_crc_invalid[0] = DSD_CRC_FAILED;
    send_chan_freq(62U, 451U, 500U);
    assert(g_state.trunk_chan_map[62] == 0L);
    dsd_state_trunk_lcn_free(&g_state);
}

static void
test_neighbour_never_trust2(void) {
    reset();
    dmr_sm_init(&g_opts, &g_state);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 70U, 451400000L, DMR_CHAN_ORIGIN_NEIGHBOUR) == 1);
    assert(g_state.dmr_lcn_trust[70] == 1U);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 71U, 451412500L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.dmr_lcn_trust[71] == 2U);
    g_opts.trunk_is_tuned = 1; /* off the control channel */
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 72U, 451425000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.dmr_lcn_trust[72] == 1U);
    g_opts.trunk_is_tuned = 0;

    send_adjacent(73U, 451U, 400U);
    assert(g_state.trunk_chan_map[73] == 451050000L);
    assert(dmr_t3_chan_origin(&g_state, 73U) == DMR_CHAN_ORIGIN_NEIGHBOUR);
    assert(g_state.dmr_lcn_trust[73] == 1U);
}

static void
test_estimates_dropped_when_anchor_changes(void) {
    reset();
    g_opts.dmr_t3_heuristic_fill = 1;
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 10U, 451000000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 12U, 451025000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.trunk_chan_map[11] == 451012500L);
    assert(dmr_t3_chan_origin(&g_state, 11U) == DMR_CHAN_ORIGIN_ESTIMATE);
    assert(g_state.dmr_lcn_trust[11] == 0U);

    assert(dmr_t3_chan_learn(&g_opts, &g_state, 12U, 451050000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.trunk_chan_map[11] != 451012500L);
    assert(g_state.trunk_chan_map[11] == 0L || g_state.trunk_chan_map[11] == 451025000L);
    if (g_state.trunk_chan_map[11] != 0L) {
        assert(dmr_t3_chan_origin(&g_state, 11U) == DMR_CHAN_ORIGIN_ESTIMATE);
    }

    /* A new anchor fills again, from the anchors alone. */
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 14U, 451100000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.trunk_chan_map[13] == 451075000L);
    assert(g_state.trunk_chan_map[11] == 451025000L);
}

/* An estimate a SITE PDU confirms at the frequency it already holds is promoted in place: the model through the anchors
   is unchanged, so no estimate is dropped and filled again. One revision, no map write (the map sequence stays), no
   notice. */
static void
test_estimate_confirmed_in_place(void) {
    reset();
    g_opts.dmr_t3_heuristic_fill = 1;
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 10U, 451000000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 20U, 451125000L, DMR_CHAN_ORIGIN_SITE) == 1);
    for (uint16_t l = 11U; l <= 19U; l++) {
        assert(dmr_t3_chan_origin(&g_state, l) == DMR_CHAN_ORIGIN_ESTIMATE);
        assert(g_state.trunk_chan_map[l] == 451000000L + (long)(l - 10U) * 12500L);
    }
    const uint32_t rev = dmr_t3_chan_revision(&g_state);
    const uint64_t seq = g_state.trunk_chan_map_seq;
    const uint64_t before0 = pushes(0);
    const uint64_t before1 = pushes(1);

    assert(dmr_t3_chan_learn(&g_opts, &g_state, 15U, 451062500L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(dmr_t3_chan_revision(&g_state) == rev + 1U);
    assert(g_state.trunk_chan_map_seq == seq);
    assert(pushes(0) == before0 && pushes(1) == before1);
    assert(dmr_t3_chan_origin(&g_state, 15U) == DMR_CHAN_ORIGIN_SITE && g_state.dmr_lcn_trust[15] == 2U);
    for (uint16_t l = 11U; l <= 19U; l++) {
        assert(g_state.trunk_chan_map[l] == 451000000L + (long)(l - 10U) * 12500L);
        assert(l == 15U || dmr_t3_chan_origin(&g_state, l) == DMR_CHAN_ORIGIN_ESTIMATE);
    }

    /* A confirmation at another frequency is a real anchor change: the estimates are redone. */
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 16U, 451080000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.trunk_chan_map_seq != seq);
}

/* A row another writer sets after the air (P25's channel cache, its IDEN invalidation, an import) is no longer the
   air's, at any frequency: it reads as FILE, DMR does not replace it, and a network change leaves it. */
static void
test_foreign_write_disowns_row(void) {
    reset();
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 30U, 452000000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 32U, 452025000L, DMR_CHAN_ORIGIN_SITE) == 1);
    dsd_state_set_trunk_chan_freq(&g_state, 30U, 853012500L); /* another protocol's frequency */
    dsd_state_set_trunk_chan_freq(&g_state, 32U, 452025000L); /* the same frequency, written by another */
    assert(dmr_t3_chan_origin(&g_state, 30U) == DMR_CHAN_ORIGIN_FILE);
    assert(dmr_t3_chan_origin(&g_state, 32U) == DMR_CHAN_ORIGIN_FILE);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 30U, 452050000L, DMR_CHAN_ORIGIN_SITE) == 0);
    assert(g_state.trunk_chan_map[30] == 853012500L);
    dmr_t3_chan_network_changed(&g_opts, &g_state);
    assert(g_state.trunk_chan_map[30] == 853012500L && dmr_t3_chan_origin(&g_state, 30U) == DMR_CHAN_ORIGIN_FILE);
    assert(g_state.trunk_chan_map[32] == 452025000L && dmr_t3_chan_origin(&g_state, 32U) == DMR_CHAN_ORIGIN_FILE);

    /* P25's IDEN 0 update clears channels 0x000-0xFFF; the channel cache then fills one of them. */
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 31U, 452012500L, DMR_CHAN_ORIGIN_SITE) == 1);
    dsd_state_p25_invalidate_iden_chan_map(&g_state, 0);
    assert(g_state.trunk_chan_map[31] == 0L && dmr_t3_chan_origin(&g_state, 31U) == DMR_CHAN_ORIGIN_NONE);
    dsd_state_set_trunk_chan_freq(&g_state, 31U, 853025000L);
    assert(dmr_t3_chan_origin(&g_state, 31U) == DMR_CHAN_ORIGIN_FILE);
    dmr_t3_chan_network_changed(&g_opts, &g_state);
    assert(g_state.trunk_chan_map[31] == 853025000L && dmr_t3_chan_origin(&g_state, 31U) == DMR_CHAN_ORIGIN_FILE);

    /* A row the air learns into an empty row afterwards is the air's again. */
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 33U, 452037500L, DMR_CHAN_ORIGIN_SITE) == 1);
    dmr_t3_chan_network_changed(&g_opts, &g_state);
    assert(g_state.trunk_chan_map[33] == 0L);
}

static void
test_network_change_drops_ota_rows_keeps_file_user(void) {
    reset();
    g_state.trunk_chan_map[1] = 452000000L;
    dmr_t3_chan_note_map_loaded(&g_state);
    set_user(2U, 452012500L);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 3U, 452025000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 4U, 452037500L, DMR_CHAN_ORIGIN_UNVERIFIED) == 1);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 5U, 452050000L, DMR_CHAN_ORIGIN_NEIGHBOUR) == 1);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 6U, 452062500L, DMR_CHAN_ORIGIN_ESTIMATE) == 1);
    for (uint16_t l = 1U; l <= 6U; l++) {
        g_state.dmr_lcn_trust[l] = 2U;
    }
    const uint32_t gen = dmr_t3_chan_generation(&g_state);
    const uint32_t rev = dmr_t3_chan_revision(&g_state);

    dmr_t3_chan_network_changed(&g_opts, &g_state);
    assert(dmr_t3_chan_generation(&g_state) == gen + 1U);
    assert(dmr_t3_chan_revision(&g_state) != rev);
    assert(g_state.trunk_chan_map[1] == 452000000L && dmr_t3_chan_origin(&g_state, 1U) == DMR_CHAN_ORIGIN_FILE);
    assert(g_state.trunk_chan_map[2] == 452012500L && dmr_t3_chan_origin(&g_state, 2U) == DMR_CHAN_ORIGIN_USER);
    assert(g_state.dmr_lcn_trust[1] == 2U && g_state.dmr_lcn_trust[2] == 2U);
    for (uint16_t l = 3U; l <= 6U; l++) {
        assert(g_state.trunk_chan_map[l] == 0L);
        assert(dmr_t3_chan_origin(&g_state, l) == DMR_CHAN_ORIGIN_NONE);
        assert(g_state.dmr_lcn_trust[l] == 0U);
    }
}

static void
test_learned_notice_on_receiving_slot(void) {
    reset();
    g_opts.receive.filters[DSD_RECEIVE_SLOT].enabled = 1;
    g_opts.receive.filters[DSD_RECEIVE_SLOT].allow.bits[0] = UINT64_C(1) << 2; /* --slot-allow 2 */
    g_state.currentslot = 1;
    g_state.event_crc_invalid[1] = DSD_CRC_RAS_UNVERIFIED;
    uint64_t before0 = pushes(0);
    uint64_t before1 = pushes(1);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 80U, 451012500L, DMR_CHAN_ORIGIN_UNVERIFIED) == 1);
    assert(pushes(0) == before0);
    assert(pushes(1) == before1 + 1U);
    assert(strstr(newest_row(1), "DMR TIII: Learned channel 80 -> 451.012500 MHz;") != NULL);
    assert(g_state.event_history_s[1].Event_History_Items[1].crc_invalid == DSD_CRC_RAS_UNVERIFIED);

    g_opts.receive.filters[DSD_RECEIVE_SLOT].allow.bits[0] = UINT64_C(1) << 1; /* --slot-allow 1 */
    before0 = pushes(0);
    before1 = pushes(1);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 81U, 451025000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.trunk_chan_map[81] == 451025000L);
    assert(pushes(0) == before0);
    assert(pushes(1) == before1);
}

static void
test_cc_vc_cc_trip_keeps_learned_rows(void) {
    reset();
    dmr_sm_init(&g_opts, &g_state);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = tune_request,
        .tune_to_cc_request = tune_request,
        .return_to_cc_request = return_request,
    });
    send_aloha(0x1234U);
    send_absolute_grant(90U, 451U, 600U);
    assert(g_state.trunk_chan_map[90] == 451075000L);
    assert(dmr_t3_chan_origin(&g_state, 90U) == DMR_CHAN_ORIGIN_SITE);
    assert(g_opts.trunk_is_tuned == 1);
    const uint32_t gen = dmr_t3_chan_generation(&g_state);

    /* The machine's own release (a forced one, as a P_CLEAR for the call arms it) takes the receiver back to the
       control channel. */
    g_state.trunk_sm_force_release = 1;
    dmr_sm_emit_release(&g_opts, &g_state, 0);
    assert(g_opts.trunk_is_tuned == 0);
    send_aloha(0x1234U);
    assert(g_state.trunk_chan_map[90] == 451075000L);
    assert(dmr_t3_chan_origin(&g_state, 90U) == DMR_CHAN_ORIGIN_SITE);
    assert(dmr_t3_chan_generation(&g_state) == gen);

    /* A RAS-accepted C_ALOHA with another code is no checked change. */
    g_opts.aggressive_framesync = 0;
    g_state.dmr_ras_signatures = 2U;
    g_state.event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED;
    send_aloha(0x0567U);
    assert(g_state.trunk_chan_map[90] == 451075000L);
    assert(dmr_t3_chan_generation(&g_state) == gen);

    /* A checked C_ALOHA with another code is a network change. */
    g_state.event_crc_invalid[0] = 0U;
    send_aloha(0x0567U);
    assert(g_state.trunk_chan_map[90] == 0L);
    assert(dmr_t3_chan_generation(&g_state) == gen + 1U);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* The channel plan is the network's (TS 102 361-4 §6.1.2): a checked C_ALOHA from another site of the same network
   (MODEL and NET equal) keeps what the air taught; another NET drops it. Small model: 5 SITE bits under 7 NET bits. */
static void
test_other_site_same_network_keeps_rows(void) {
    reset();
    dmr_sm_init(&g_opts, &g_state);
    send_aloha(0x1234U); /* Small, NET 0x11, SITE 0x14 */
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 95U, 451087500L, DMR_CHAN_ORIGIN_SITE) == 1);
    const uint32_t gen = dmr_t3_chan_generation(&g_state);

    send_aloha(0x1223U); /* Small, NET 0x11, SITE 0x03 */
    assert(g_state.trunk_chan_map[95] == 451087500L);
    assert(dmr_t3_chan_origin(&g_state, 95U) == DMR_CHAN_ORIGIN_SITE);
    assert(dmr_t3_chan_generation(&g_state) == gen);

    send_aloha(0x1254U); /* Small, NET 0x12, SITE 0x14 */
    assert(g_state.trunk_chan_map[95] == 0L);
    assert(dmr_t3_chan_generation(&g_state) == gen + 1U);

    /* The same NET under another MODEL is another network. */
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 95U, 451087500L, DMR_CHAN_ORIGIN_SITE) == 1);
    send_aloha(0x2254U); /* Large: another MODEL */
    assert(g_state.trunk_chan_map[95] == 0L);
    assert(dmr_t3_chan_generation(&g_state) == gen + 2U);
}

static void
test_save_restore_round_trip(void) {
    reset();
    static dmr_t3_chan_target_snapshot a;
    static dmr_t3_chan_target_snapshot b;
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 12U, 451012500L, DMR_CHAN_ORIGIN_SITE) == 1);
    dmr_t3_chan_save(&g_state, &a);
    assert(a.origin[12] == (uint8_t)DMR_CHAN_ORIGIN_SITE);
    dmr_t3_chan_network_changed(&g_opts, &g_state);
    dmr_t3_chan_save(&g_state, &b);
    const uint32_t rev = dmr_t3_chan_revision(&g_state);
    dmr_t3_chan_restore(&g_state, &a);
    assert(dmr_t3_chan_generation(&g_state) == a.generation);
    assert(dmr_t3_chan_revision(&g_state) > rev);
    g_state.trunk_chan_map[12] = 451012500L;
    assert(dmr_t3_chan_origin(&g_state, 12U) == DMR_CHAN_ORIGIN_SITE);
    dmr_t3_chan_restore(&g_state, &b);
    assert(dmr_t3_chan_generation(&g_state) == b.generation);
    assert(dmr_t3_chan_origin(&g_state, 12U) == DMR_CHAN_ORIGIN_FILE); /* a row with no recorded origin */
}

/* ---- C_BCAST announcements (B4, B6) ---- */

/* Chan_Freq always carries a BC_AP (TS 102 361-4 §7.2.19.6): a header CH of 0xFFF leaves the channel to the BC_AP's
   CHAN, learned as any other. It names a logical channel's frequency, not a TSCC (§6.7.1.6), so the positional list
   never takes it (discussion #900). */
static void
test_chan_freq_header_fff_uses_bcap_chan(void) {
    reset();
    dmr_sm_init(&g_opts, &g_state);
    send_c_bcast_mbc(5U, 0x0FFFU, 130U, 451U, 1000U, 0U);
    assert(g_state.trunk_chan_map[130] == 451125000L);
    assert(dmr_t3_chan_origin(&g_state, 130U) == DMR_CHAN_ORIGIN_SITE);
    assert(g_state.trunk_chan_map[0xFFF] == 0L);
    assert(hunt_list_count(451125000L) == 0 && g_state.lcn_freq_count == 0);
    assert(candidate_count(451125000L) == 1);

    /* Neither the header nor the BC_AP names a channel: nothing is learned. */
    send_c_bcast_mbc(5U, 0x0FFFU, 0x0FFFU, 451U, 1100U, 0U);
    send_c_bcast_mbc(5U, 0x0FFFU, 0U, 451U, 1100U, 0U);
    assert(hunt_list_count(451137500L) == 0);
    assert(g_state.trunk_chan_map[0] == 0L && g_state.trunk_chan_map[0xFFF] == 0L);
    dsd_state_trunk_lcn_free(&g_state);
}

/* A header CH and a BC_AP CHAN that both name channels and differ say two things: the announcement is ignored. A BC_AP
   CHAN of 0 or 4095 leaves the header's. */
static void
test_chan_freq_inconsistent_channels_ignored(void) {
    reset();
    dmr_sm_init(&g_opts, &g_state);
    send_c_bcast_mbc(5U, 131U, 132U, 451U, 1200U, 0U);
    assert(g_state.trunk_chan_map[131] == 0L && g_state.trunk_chan_map[132] == 0L);
    assert(hunt_list_count(451150000L) == 0);
    assert(candidate_count(451150000L) == 0);

    send_c_bcast_mbc(5U, 133U, 0U, 451U, 1300U, 0U);
    assert(g_state.trunk_chan_map[133] == 451162500L);
    send_c_bcast_mbc(5U, 134U, 0x0FFFU, 451U, 1400U, 0U);
    assert(g_state.trunk_chan_map[134] == 451175000L);
    dsd_state_trunk_lcn_free(&g_state);
}

/* A checked Chan_Freq is a SITE row with its learned notice; announced again it changes nothing. The positional list
   never holds it. */
static void
test_chan_freq_learned_notice_and_origin(void) {
    reset();
    dmr_sm_init(&g_opts, &g_state);
    const uint64_t before = pushes(0);
    send_chan_freq(135U, 451U, 1500U);
    assert(g_state.trunk_chan_map[135] == 451187500L);
    assert(dmr_t3_chan_origin(&g_state, 135U) == DMR_CHAN_ORIGIN_SITE);
    assert(g_state.dmr_lcn_trust[135] == 2U);
    assert(pushes(0) == before + 1U);
    assert(strstr(newest_row(0), "DMR TIII: Learned channel 135 -> 451.187500 MHz;") != NULL);
    send_chan_freq(135U, 451U, 1500U);
    assert(pushes(0) == before + 1U);
    assert(hunt_list_count(451187500L) == 0 && g_state.lcn_freq_count == 0);
    assert(candidate_count(451187500L) == 1);
    dsd_state_trunk_lcn_free(&g_state);
}

/* A user's list (-C) is never appended to: the channel is still learned, the hunt list keeps the user's rows. */
static void
test_chan_freq_user_list_not_appended(void) {
    reset();
    dmr_sm_init(&g_opts, &g_state);
    DSD_SNPRINTF(g_opts.chan_in_file, sizeof g_opts.chan_in_file, "%s", "user_channels.csv");
    assert(dsd_state_trunk_lcn_freq_write(&g_state, 0, CC_HZ) == 0);
    dsd_state_trunk_lcn_count_raise(&g_state, 1);
    assert(dsd_state_trunk_lcn_user_list_present(&g_opts, &g_state));
    send_chan_freq(141U, 451U, 2300U);
    assert(g_state.trunk_chan_map[141] == 451287500L);
    assert(g_state.lcn_freq_count == 1);
    assert(hunt_list_count(451287500L) == 0);
    dsd_state_trunk_lcn_free(&g_state);
}

/* A C_BCAST Ann_WD_TSCC (type 0) as a single CSBK: BCAST_CH1 @p ch1 marked add, BCAST_CH2 CHNULL. */
static void
send_ann_wd_logical(uint16_t ch1) {
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pf0(bits, bytes, 40U);
    put_bits(bits, 16, 0U, 5);
    put_bits(bits, 56, ch1 & 0x0FFFU, 12);
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1U, 0U);
}

/* A TSCC an Ann_WD_TSCC, Vote_Now or Adjacent_Site names becomes a hunt candidate only from a checked or RAS-accepted
   PDU: a RAS signature on a carrier where RAS is not confirmed adds none. */
static void
test_announced_candidates_need_checked_content(void) {
    reset();
    dmr_sm_init(&g_opts, &g_state);
    g_opts.aggressive_framesync = 0; /* -F */
    g_state.currentslot = 0;
    g_state.trunk_chan_map[142] = 451300000L;
    g_state.event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED;
    g_state.dmr_ras_signatures = 0U;
    send_ann_wd_logical(142U);
    send_c_bcast_mbc(2U, 0x0FFFU, 143U, 451U, 2500U, 1U);
    send_adjacent(144U, 451U, 2600U);
    assert(candidate_count(451300000L) == 0);
    assert(candidate_count(451312500L) == 0);
    assert(candidate_count(451325000L) == 0);

    /* RAS confirmed on the carrier: accepted, so each is a candidate. */
    g_state.dmr_ras_signatures = 2U;
    send_ann_wd_logical(142U);
    send_c_bcast_mbc(2U, 0x0FFFU, 143U, 451U, 2500U, 1U);
    send_adjacent(144U, 451U, 2600U);
    assert(candidate_count(451300000L) == 1);
    assert(candidate_count(451312500L) == 1);
    assert(candidate_count(451325000L) == 1);
}

/* Vote_Now with "Active_connection information not available" (§6.7.1.3) still names its channel: only the
   Active_connection bit is meaningless. Its VN_AP is a neighbour's frequency. */
static void
test_vote_now_ac_unavailable_still_decodes_channel(void) {
    reset();
    dmr_sm_init(&g_opts, &g_state);
    send_c_bcast_mbc(2U, 0x0FFFU, 136U, 451U, 1600U, 0U);
    assert(g_state.trunk_chan_map[136] == 451200000L);
    assert(dmr_t3_chan_origin(&g_state, 136U) == DMR_CHAN_ORIGIN_NEIGHBOUR);
    assert(g_state.dmr_lcn_trust[136] == 1U);
    assert(candidate_count(451200000L) == 1);

    send_c_bcast_mbc(2U, 0x0FFFU, 137U, 451U, 1700U, 1U);
    assert(dmr_t3_chan_origin(&g_state, 137U) == DMR_CHAN_ORIGIN_NEIGHBOUR);
}

/* An absolute Adjacent_Site fills an empty row as NEIGHBOUR and never replaces a row, its own included; its frequency
   stays a hunt candidate. */
static void
test_adjacent_absolute_is_neighbour_fill_only(void) {
    reset();
    dmr_sm_init(&g_opts, &g_state);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 138U, 451212500L, DMR_CHAN_ORIGIN_SITE) == 1);
    send_adjacent(138U, 451U, 1800U);
    assert(g_state.trunk_chan_map[138] == 451212500L);
    assert(dmr_t3_chan_origin(&g_state, 138U) == DMR_CHAN_ORIGIN_SITE);
    assert(candidate_count(451225000L) == 1);

    send_adjacent(139U, 451U, 1900U);
    assert(g_state.trunk_chan_map[139] == 451237500L);
    assert(dmr_t3_chan_origin(&g_state, 139U) == DMR_CHAN_ORIGIN_NEIGHBOUR);
    send_adjacent(139U, 451U, 2000U);
    assert(g_state.trunk_chan_map[139] == 451237500L);
    assert(dmr_t3_chan_origin(&g_state, 139U) == DMR_CHAN_ORIGIN_NEIGHBOUR);

    g_state.trunk_chan_map[140] = 451262500L;
    dmr_t3_chan_note_map_loaded(&g_state);
    send_adjacent(140U, 451U, 2200U);
    assert(g_state.trunk_chan_map[140] == 451262500L);
    assert(dmr_t3_chan_origin(&g_state, 140U) == DMR_CHAN_ORIGIN_FILE);
}

/* ---- MBC assembly and absolute parameters (B7): real burst -> assembler -> dispatch ---- */

static const uint8_t k_no_reserved[3] = {0, 0, 0};

static void
unpack_bytes(const uint8_t* bytes, uint8_t* bits, size_t count) {
    for (size_t i = 0; i < count * 8U; ++i) {
        bits[i] = (uint8_t)((bytes[i / 8U] >> (7U - i % 8U)) & 1U);
    }
}

/* The symbol clock at @p slot's next burst: bursts alternate slots every 144 symbols, TS1 on even burst positions and
   TS2 on odd, so a slot's next burst is one TDMA frame (288 symbols) after its last when no burst is skipped. A burst
   the decoder misses is one this never dispatches: the clock moves past it all the same. */
static void
advance_to_slot(uint8_t slot) {
    uint64_t pos = g_state.symbol_clock + 144U;
    if (((pos / 144U) & 1U) != (slot & 1U)) {
        pos += 144U;
    }
    g_state.symbol_clock = pos;
}

static void
send_burst(uint8_t slot, const uint8_t bits[96], uint8_t databurst) {
    uint8_t info[196];
    advance_to_slot(slot);
    g_state.currentslot = slot;
    dmr_test_encode_bptc_196x96(bits, k_no_reserved, info);
    dmr_data_burst_handler(&g_opts, &g_state, info, databurst, NULL);
}

/* A control burst of opcode @p csbko naming channel @p chan (bits 16..27), talkgroup 1001 and source 2002, with
   last-block bit @p lb: a CSBK (@p mbc_header 0: mask 0xA5A5, data type 3) or an MBC header (mask 0xAAAA, data
   type 4). */
static void
send_control_burst(uint8_t slot, uint8_t csbko, uint16_t chan, int mbc_header, uint8_t lb) {
    uint8_t bytes[12] = {0};
    uint8_t bits[96];
    bytes[0] = (uint8_t)((lb ? 0x80U : 0x00U) | (csbko & 0x3FU));
    bytes[2] = (uint8_t)(chan >> 4);
    bytes[3] = (uint8_t)((chan & 0x0FU) << 4);
    bytes[5] = 0x03; /* target (bits 32..55): TG 1001 */
    bytes[6] = 0xE9;
    bytes[8] = 0x07; /* source (bits 56..79): 2002 */
    bytes[9] = 0xD2;
    unpack_bytes(bytes, bits, sizeof bytes);
    const uint16_t crc = (uint16_t)(ComputeCrcCCITT(bits) ^ (mbc_header ? 0xAAAAU : 0xA5A5U));
    bytes[10] = (uint8_t)(crc >> 8);
    bytes[11] = (uint8_t)crc;
    unpack_bytes(bytes, bits, sizeof bytes);
    send_burst(slot, bits, mbc_header ? 0x04U : 0x03U);
}

/* A CSBK (@p mbc_header 0: LB 1) or an MBC header (LB 0) through send_control_burst(). */
static void
send_csbk_or_header(uint8_t slot, uint8_t csbko, uint16_t chan, int mbc_header) {
    send_control_burst(slot, csbko, chan, mbc_header, mbc_header ? 0U : 1U);
}

/* A continuation carrying CdefParms: CSBKO @p csbko, CHAN @p chan, RX @p rx_mhz plus @p rx_step 125 Hz steps (TX the
   same). */
static void
build_abs_block(uint8_t block[96], uint8_t csbko, uint16_t chan, uint16_t rx_mhz, uint16_t rx_step) {
    DSD_MEMSET(block, 0, 96);
    put_bits(block, 2, csbko & 0x3FU, 6);
    put_bits(block, 22, chan & 0x0FFFU, 12);
    put_bits(block, 34, rx_mhz & 0x03FFU, 10);
    put_bits(block, 44, rx_step & 0x1FFFU, 13);
    put_bits(block, 57, rx_mhz & 0x03FFU, 10);
    put_bits(block, 67, rx_step & 0x1FFFU, 13);
}

/* @p count continuations on @p slot, the last with LB 1 and the CRC16 over every continuation (TS 102 361-1 §7.4.0).
   @p old_span_crc 1 computes it instead over the first three blocks and zeros, as the old fixed-size copy did. */
static void
send_continuations(uint8_t slot, uint8_t blocks[][96], size_t count, int old_span_crc) {
    uint8_t all[4 * 96];
    assert(count >= 1U && count <= 4U);
    put_bits(blocks[count - 1U], 0, 1U, 1);
    DSD_MEMSET(all, 0, sizeof all);
    for (size_t b = 0; b < count; b++) {
        DSD_MEMCPY(all + 96U * b, blocks[b], 96);
    }
    if (old_span_crc && count == 4U) {
        DSD_MEMSET(all + 288, 0, 96);
    }
    put_bits(blocks[count - 1U], 80, dsd_crc_ccitt16_bits(all, count * 96U - 16U), 16);
    for (size_t b = 0; b < count; b++) {
        send_burst(slot, blocks[b], 0x05);
    }
}

/* A two-block absolute TV_GRANT on @p slot: header CSBKO 49, CHAN 0xFFF; continuation CSBKO @p cont_csbko. */
static void
send_abs_grant_mbc(uint8_t slot, uint8_t cont_csbko, uint16_t chan, uint16_t rx_mhz, uint16_t rx_step) {
    uint8_t block[1][96];
    send_csbk_or_header(slot, 49U, 0x0FFFU, 1);
    build_abs_block(block[0], cont_csbko, chan, rx_mhz, rx_step);
    send_continuations(slot, block, 1U, 0);
}

static int
activity_rows(void) {
    dsd_recent_activity_snapshot snap;
    DSD_MEMSET(&snap, 0, sizeof snap);
    (void)dsd_recent_activity_copy_snapshot(&g_state, &snap);
    int rows = 0;
    for (int i = 0; i < DSD_RECENT_ACTIVITY_COUNT; i++) {
        rows += snap.entries[i].notice[0] != '\0' ? 1 : 0;
    }
    return rows;
}

static void
reset_bursts(void) {
    reset();
    g_state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    g_state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    dmr_reset_blocks(&g_opts, &g_state);
    dmr_sm_init(&g_opts, &g_state);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_freq_request = tune_request});
}

static int
not_followed(uint16_t chan) {
    return g_state.trunk_vc_freq[0] == 0L && g_opts.trunk_is_tuned == 0 && g_state.trunk_chan_map[chan] == 0L
           && dmr_sm_get_ctx()->state != DMR_SM_TUNED;
}

/* An MBC header armed on one slot is disarmed by the next burst on that slot, whatever it is: header A, then another
   burst on the slot, then continuation B never assemble into a grant nobody sent. A sync loss disarms it too. */
static void
test_mbc_header_then_other_same_slot_burst_disarms(void) {
    static const uint8_t kinds[] = {0x03U, 0x09U, 0x06U};
    for (size_t k = 0; k <= sizeof kinds; k++) {
        reset_bursts();
        uint8_t block[1][96];
        send_csbk_or_header(0, 49U, 0x0FFFU, 1);
        if (k < sizeof kinds) {
            uint8_t other[96];
            DSD_MEMSET(other, 0, sizeof other);
            if (kinds[k] == 0x03U) {
                send_csbk_or_header(0, 25U, 0U, 0); /* a C_ALOHA */
            } else {
                send_burst(0, other, kinds[k]);
            }
        } else {
            dmr_reset_blocks(&g_opts, &g_state); /* sync loss */
        }
        build_abs_block(block[0], 49U, 120U, 451U, 100U);
        send_continuations(0, block, 1U, 0);
        assert(not_followed(120U));
    }
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* The other slot's burst between the header and its continuation, and between continuations, leaves the assembly
   alone. */
static void
test_companion_slot_traffic_does_not_disarm(void) {
    reset_bursts();
    uint8_t block[1][96];
    uint8_t idle[96];
    DSD_MEMSET(idle, 0, sizeof idle);
    send_csbk_or_header(0, 49U, 0x0FFFU, 1);
    send_csbk_or_header(1, 25U, 0U, 0);
    build_abs_block(block[0], 49U, 121U, 451U, 200U);
    send_continuations(0, block, 1U, 0);
    assert(g_state.trunk_vc_freq[0] == 451025000L);
    assert(g_state.trunk_chan_map[121] == 451025000L);

    /* A three-block C_ALOHA with idle bursts on TS1 between its bursts on TS2 still assembles and is accepted: it
       counts once as scan activity. */
    reset_bursts();
    send_csbk_or_header(1, 25U, 0U, 1);
    uint8_t blocks[2][96];
    build_abs_block(blocks[0], 25U, 0U, 0U, 0U);
    build_abs_block(blocks[1], 25U, 0U, 0U, 0U);
    put_bits(blocks[1], 0, 1U, 1);
    uint8_t all[2 * 96];
    DSD_MEMCPY(all, blocks[0], 96);
    DSD_MEMCPY(all + 96, blocks[1], 96);
    put_bits(blocks[1], 80, dsd_crc_ccitt16_bits(all, 2U * 96U - 16U), 16);
    const uint32_t seq = g_state.scan_y_content_seq;
    send_burst(0, idle, 0x09U);
    send_burst(1, blocks[0], 0x05);
    send_burst(0, idle, 0x09U);
    send_burst(1, blocks[1], 0x05);
    assert(g_state.scan_y_content_seq == seq + 1U);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* A continuation joins only as its slot's next burst (discussion #900 B7): header A on TS1, A's continuation and B's
   header both missed while TS2's bursts keep arriving, then B's continuation: no grant, no map write, no tune. */
static void
test_mbc_missed_same_slot_bursts_disarm(void) {
    uint8_t idle[96];
    DSD_MEMSET(idle, 0, sizeof idle);
    for (int missed = 1; missed <= 2; missed++) {
        reset_bursts();
        uint8_t block[1][96];
        send_csbk_or_header(0, 49U, 0x0FFFU, 1); /* A's header */
        send_burst(1, idle, 0x09U);
        for (int m = 0; m < missed; m++) {
            send_burst(1, idle, 0x09U); /* TS1's burst between these two never decoded */
        }
        build_abs_block(block[0], 49U, 128U, 451U, 900U); /* B's continuation */
        send_continuations(0, block, 1U, 0);
        assert(not_followed(128U));
        assert(activity_rows() == 0);
    }

    /* The same continuation one frame after the header is followed. */
    reset_bursts();
    uint8_t block[1][96];
    send_csbk_or_header(0, 49U, 0x0FFFU, 1);
    send_burst(1, idle, 0x09U);
    build_abs_block(block[0], 49U, 128U, 451U, 900U);
    send_continuations(0, block, 1U, 0);
    assert(g_state.trunk_vc_freq[0] == 451112500L && g_state.trunk_chan_map[128] == 451112500L);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* A single CSBK naming the absolute escape has no appended block: no activity row, no tune. */
static void
test_single_csbk_0xfff_is_malformed(void) {
    reset_bursts();
    send_csbk_or_header(0, 49U, 0x0FFFU, 0);
    assert(activity_rows() == 0);
    assert(g_state.trunk_vc_freq[0] == 0L && g_opts.trunk_is_tuned == 0);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* A continuation that does not carry its header's CSBKO is not that header's (CG_AP Table 7.16). */
static void
test_continuation_csbko_mismatch_rejected(void) {
    reset_bursts();
    send_abs_grant_mbc(0, 0U, 122U, 451U, 300U);
    assert(not_followed(122U));
    send_abs_grant_mbc(0, 40U, 122U, 451U, 300U);
    assert(not_followed(122U));
    assert(activity_rows() == 0);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* An MBC has at most three continuations (TS 102 361-1 §7.4.0), and absolute parameters come from exactly one: four
   are refused whichever span the CRC covers, three carry no absolute grant. */
static void
test_four_continuations_rejected(void) {
    for (int span = 0; span <= 1; span++) {
        reset_bursts();
        uint8_t blocks[4][96];
        send_csbk_or_header(0, 49U, 0x0FFFU, 1);
        build_abs_block(blocks[0], 49U, 123U, 451U, 400U);
        for (size_t b = 1; b < 4U; b++) {
            build_abs_block(blocks[b], 49U, 0U, 0U, 0U);
        }
        send_continuations(0, blocks, 4U, span);
        assert(not_followed(123U));
    }
    reset_bursts();
    uint8_t three[3][96];
    send_csbk_or_header(0, 49U, 0x0FFFU, 1);
    build_abs_block(three[0], 49U, 124U, 451U, 400U);
    build_abs_block(three[1], 49U, 0U, 0U, 0U);
    build_abs_block(three[2], 49U, 0U, 0U, 0U);
    send_continuations(0, three, 3U, 0);
    assert(not_followed(124U));
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* A receive frequency outside Table C.7 (50..999 MHz, 0..7999 steps) is no frequency: nothing tuned or learned. */
static void
test_abs_rx_outside_table_c7_no_frequency(void) {
    static const struct {
        uint16_t mhz;
        uint16_t step;
    } bad[] = {{49U, 0U}, {1000U, 0U}, {451U, 8000U}};

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        reset_bursts();
        send_abs_grant_mbc(0, 49U, 125U, bad[i].mhz, bad[i].step);
        assert(not_followed(125U));
    }
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* A valid frequency whose CHAN is 0 or 4095 is tuned and shown, but names no channel to learn. */
static void
test_abs_rx_valid_chan_zero_tunes_without_learning(void) {
    static const uint16_t chans[] = {0U, 0x0FFFU};
    for (size_t i = 0; i < sizeof chans / sizeof chans[0]; i++) {
        reset_bursts();
        send_abs_grant_mbc(0, 49U, chans[i], 451U, 500U);
        assert(g_state.trunk_vc_freq[0] == 451062500L);
        assert(g_state.trunk_chan_map[0] == 0L && g_state.trunk_chan_map[0x0FFF] == 0L);
    }
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* Review Focus 1: a site that sends absolute grants only, with no channel map at all, is followed as before. */
static void
test_absolute_grant_two_block_mbc_still_follows(void) {
    reset_bursts();
    send_abs_grant_mbc(0, 49U, 126U, 451U, 700U);
    assert(g_state.trunk_vc_freq[0] == 451087500L);
    assert(g_opts.trunk_is_tuned == 1);
    assert(g_state.trunk_chan_map[126] == 451087500L);
    assert(dmr_t3_chan_origin(&g_state, 126U) == DMR_CHAN_ORIGIN_SITE);
    assert(activity_rows() == 1);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* No control channel heartbeat, anchor or scan activity since prepare_no_effect_check(). */
static void
prepare_no_effect_check(void) {
    g_state.trunk_cc_freq = 0L;
    g_opts.audio_in_type = AUDIO_IN_RTL;
    g_opts.rtlsdr_center_freq = 451500000U;
    g_opts.dmr_crc_relaxed_default = 1;
    dsd_decode_clock_test_set_ns(UINT64_C(5000) * UINT64_C(1000000000));
    g_state.last_cc_sync_time = 0;
    g_state.last_cc_sync_time_m = 0.0;
    g_state.scan_y_content_seq = 0U;
}

static int
no_effect_since_prepare(void) {
    return g_state.last_cc_sync_time == 0 && g_state.last_cc_sync_time_m < 0.5 && g_state.last_cc_sync_time_m > -0.5
           && g_state.trunk_cc_freq == 0L && g_state.scan_y_content_seq == 0U && activity_rows() == 0;
}

/* A malformed MBC has no side effect at all: no control channel heartbeat, no anchor, no scan activity, nothing
   published. Malformed are a continuation of another CSBKO, and absolute parameters that are not usable: a receive
   frequency outside Table C.7 or a reserved Cdeftype (TS 102 361-4 Table 7.103). */
static void
test_malformed_mbc_no_cc_heartbeat(void) {
    for (int variant = 0; variant < 3; variant++) {
        reset_bursts();
        prepare_no_effect_check();
        uint8_t block[1][96];
        send_csbk_or_header(0, 49U, 0x0FFFU, 1);
        build_abs_block(block[0], variant == 0 ? 0U : 49U, 127U, variant == 1 ? 49U : 451U, 800U);
        if (variant == 2) {
            put_bits(block[0], 16, 3U, 4); /* Cdeftype 3 */
        }
        send_continuations(0, block, 1U, 0);
        assert(no_effect_since_prepare());
        assert(g_state.trunk_chan_map[127] == 0L);
    }
    reset_bursts();
    prepare_no_effect_check();

    /* The same MBC well formed does keep the control channel warm, anchors it and counts as scan activity. */
    send_abs_grant_mbc(0, 49U, 127U, 451U, 800U);
    assert(g_state.last_cc_sync_time_m > 1.0);
    assert(g_state.trunk_cc_freq == 451500000L);
    assert(g_state.scan_y_content_seq == 1U);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* An MBC header whose last-block bit is set has no continuation: nothing dispatches, so a header-only C_ALOHA under
   relaxed CRC refreshes nothing, and a continuation after it joins nothing. */
static void
test_header_only_mbc_refreshes_nothing(void) {
    reset_bursts();
    prepare_no_effect_check();
    send_control_burst(0, 25U, 0U, 1, 1U);
    assert(no_effect_since_prepare());
    uint8_t block[1][96];
    build_abs_block(block[0], 25U, 0U, 0U, 0U);
    send_continuations(0, block, 1U, 0);
    assert(no_effect_since_prepare());
}

/* The scan's activity evidence (issue #834) waits for the control PDU's verdict: a CSBK or MBC the gate refuses keeps
   no -Y step on the channel; an accepted one counts once. */
static void
test_scan_activity_only_from_accepted_control(void) {
    reset_bursts();
    g_state.scan_y_content_seq = 0U;
    send_csbk_or_header(0, 49U, 0x0FFFU, 0);      /* single-block grant on the absolute escape */
    send_abs_grant_mbc(1, 0U, 129U, 451U, 1000U); /* continuation of another CSBKO */
    assert(g_state.scan_y_content_seq == 0U);
    send_csbk_or_header(0, 25U, 0U, 0); /* a C_ALOHA */
    assert(g_state.scan_y_content_seq == 1U);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* C_BCAST announcements that name a channel by BC_AP or VN_AP (Ann_WD_TSCC's MBC form, Vote_Now and Adjacent_Site on
   0xFFF, Chan_Freq) are refused before any effect when the parameters are unusable; usable ones keep the control
   channel warm. A single-block Chan_Freq has no BC_AP at all. */
static void
test_c_bcast_bad_abs_params_no_effect(void) {
    static const uint8_t types[] = {0U, 2U, 5U, 6U};
    for (size_t i = 0; i < sizeof types; i++) {
        reset();
        prepare_no_effect_check();
        send_c_bcast_mbc(types[i], 0x0FFFU, 141U, 49U, 0U, 1U);
        assert(no_effect_since_prepare());
        assert(g_state.trunk_chan_map[141] == 0L);
        send_c_bcast_mbc(types[i], 0x0FFFU, 141U, 451U, 0U, 1U);
        assert(g_state.last_cc_sync_time_m > 1.0);
    }
    reset();
    prepare_no_effect_check();
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pf0(bits, bytes, 40U);
    put_bits(bits, 16, 5U, 5);
    put_bits(bits, 68, 142U, 12);
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1U, 0U);
    assert(no_effect_since_prepare());
}

/* A single-block C_BCAST of announcement @p type, feature set @p fid, whose System Identity Code (bits 40-53) is
   @p syscode and whose BParms1 (bits 21-34) is @p bparms1. */
static void
send_c_bcast_single(uint8_t type, uint8_t fid, uint16_t syscode, uint16_t bparms1) {
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pf0(bits, bytes, 40U);
    bytes[1] = fid;
    put_bits(bits, 8, fid, 8);
    put_bits(bits, 16, type & 0x1FU, 5);
    put_bits(bits, 21, bparms1 & 0x3FFFU, 14);
    put_bits(bits, 40, syscode & 0x3FFFU, 14);
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1U, 0U);
}

/* A checked standard C_BCAST names the transmitting site in its header (TS 102 361-4 Table 7.20): network B's arriving
   before B's C_ALOHA drops what network A taught first, and a Chan_Freq it carries is learned under B, which B's
   C_ALOHA keeps. Vote_Now's or Adjacent_Site's BParms1 names another site: no change. A manufacturer's C_BCAST and a
   RAS-accepted one are no checked standard identity. */
static void
test_c_bcast_system_code_is_the_network_baseline(void) {
    reset();
    dmr_sm_init(&g_opts, &g_state);
    send_aloha(0x1234U); /* network A: Small, NET 0x11 */
    g_bcast_syscode = 0x1234U;
    send_chan_freq(150U, 451U, 2400U);
    assert(g_state.trunk_chan_map[150] == 451300000L);
    const uint32_t gen = dmr_t3_chan_generation(&g_state);

    send_c_bcast_single(6U, 0U, 0x1234U, 0x1254U); /* Adjacent_Site naming network B */
    send_c_bcast_single(3U, 0x10U, 0x1254U, 0U);   /* a manufacturer's */
    g_opts.aggressive_framesync = 0;
    g_state.dmr_ras_signatures = 2U;
    g_state.event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED;
    send_c_bcast_single(3U, 0U, 0x1254U, 0U); /* RAS-accepted */
    g_state.event_crc_invalid[0] = 0U;
    assert(g_state.trunk_chan_map[150] == 451300000L);
    assert(dmr_t3_chan_generation(&g_state) == gen);

    send_c_bcast_single(3U, 0U, 0x1254U, 0U); /* network B's Local_Time: Small, NET 0x12 */
    assert(g_state.trunk_chan_map[150] == 0L);
    assert(dmr_t3_chan_generation(&g_state) == gen + 1U);

    g_bcast_syscode = 0x1254U;
    send_chan_freq(151U, 451U, 2500U);
    assert(g_state.trunk_chan_map[151] == 451312500L);
    send_aloha(0x1254U);
    assert(g_state.trunk_chan_map[151] == 451312500L);
    assert(dmr_t3_chan_origin(&g_state, 151U) == DMR_CHAN_ORIGIN_SITE);

    /* B's Chan_Freq arriving first after A also lands under B. */
    reset();
    dmr_sm_init(&g_opts, &g_state);
    send_aloha(0x1234U);
    g_bcast_syscode = 0x1254U;
    send_chan_freq(152U, 451U, 2600U);
    send_aloha(0x1254U);
    assert(g_state.trunk_chan_map[152] == 451325000L);
    dsd_state_trunk_lcn_free(&g_state);
}

/* With the colour-code gate locked, an accepted control PDU from proving bursts stamps the gate's evidence
   (dsd_state::dmr_confidence_lock_m) and counts as activity; MBC headers whose continuation never arrives do neither
   (discussion #900), however many. */
static void
test_unfinished_mbc_headers_note_no_lock_evidence(void) {
    reset_bursts();
    g_state.dmr_confidence_locked = 1;
    g_state.dmr_confidence_color_code = 0U;
    g_state.dmr_confidence_lock_m = 0.0;
    g_state.scan_y_content_seq = 0U;
    dsd_decode_clock_test_set_ns(UINT64_C(40) * UINT64_C(1000000000));
    for (int i = 0; i < 4; i++) {
        send_csbk_or_header(0, 49U, 0x0FFFU, 1);
    }
    assert(g_state.dmr_confidence_lock_m < 0.5);
    assert(g_state.scan_y_content_seq == 0U);
    assert(not_followed(0U));

    send_csbk_or_header(0, 25U, 0U, 0); /* a C_ALOHA */
    assert(g_state.dmr_confidence_lock_m > 1.0);
    assert(g_state.scan_y_content_seq == 1U);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* Usable absolute parameters (CHAN @p chan, 451 MHz plus @p rx_step steps) in the continuation block of @p bits. */
static void
put_abs_params(uint8_t* bits, uint16_t chan, uint16_t rx_step) {
    put_bits(bits, 112, 0U, 4);
    put_bits(bits, 118, chan & 0x0FFFU, 12);
    put_bits(bits, 130, 451U, 10);
    put_bits(bits, 140, rx_step & 0x1FFFU, 13);
    put_bits(bits, 153, 451U, 10);
    put_bits(bits, 163, rx_step & 0x1FFFU, 13);
}

/* A grant, C_MOVE or standard P_CLEAR naming a logical channel is a single CSBK (TS 102 361-4 §7.1.1.1.1.2,
   §7.1.1.1.3, §7.1.1.3.2): the same PDU as an MBC, even with a continuation that matches its header, is the wrong form
   and refused before any effect: no activity row, no tune, no heartbeat or anchor. */
static void
test_logical_channel_mbc_is_malformed(void) {
    reset_bursts();
    g_state.trunk_chan_map[12] = 451150000L;
    prepare_no_effect_check();
    uint8_t block[1][96];
    send_csbk_or_header(0, 49U, 12U, 1);
    build_abs_block(block[0], 49U, 12U, 451U, 9000U);
    put_bits(block[0], 16, 3U, 4); /* Cdeftype 3 */
    send_continuations(0, block, 1U, 0);
    assert(no_effect_since_prepare());
    assert(g_state.trunk_vc_freq[0] == 0L && g_opts.trunk_is_tuned == 0);

    reset_bursts();
    g_state.trunk_chan_map[12] = 451150000L;
    prepare_no_effect_check();
    send_csbk_or_header(0, 49U, 12U, 1);
    build_abs_block(block[0], 49U, 12U, 451U, 1200U);
    send_continuations(0, block, 1U, 0);
    assert(no_effect_since_prepare());
    assert(g_state.trunk_vc_freq[0] == 0L && g_opts.trunk_is_tuned == 0);

    static const struct {
        uint8_t opcode;
        size_t field;
    } others[] = {{57U, 44U}, {46U, 16U}};

    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
        reset();
        dmr_sm_init(&g_opts, &g_state);
        prepare_no_effect_check();
        uint8_t bits[256];
        uint8_t bytes[48];
        build_pf0(bits, bytes, others[i].opcode);
        put_bits(bits, others[i].field, 12U, 12);
        put_abs_params(bits, 12U, 1200U);
        dispatch_mbc(bits, bytes, others[i].opcode);
        assert(no_effect_since_prepare());
    }

    /* The single CSBK naming the logical channel is followed. */
    reset_bursts();
    g_state.trunk_chan_map[12] = 451150000L;
    send_csbk_or_header(0, 49U, 12U, 0);
    assert(g_state.trunk_vc_freq[0] == 451150000L);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* A Chan_Freq whose header CH and BC_AP CHAN name different channels contradicts itself: refused before any effect,
   neither learned nor a heartbeat, an anchor or activity. */
static void
test_chan_freq_contradiction_no_effect(void) {
    reset();
    dmr_sm_init(&g_opts, &g_state);
    prepare_no_effect_check();
    send_c_bcast_mbc(5U, 131U, 132U, 451U, 1200U, 0U);
    assert(no_effect_since_prepare());
    assert(g_state.trunk_chan_map[131] == 0L && g_state.trunk_chan_map[132] == 0L);
    send_c_bcast_mbc(5U, 131U, 131U, 451U, 1200U, 0U);
    assert(g_state.trunk_chan_map[131] == 451150000L);
    assert(g_state.last_cc_sync_time_m > 1.0);
    dsd_state_trunk_lcn_free(&g_state);
}

/* A single CSBK is 96 bits: the gate and the handlers read no continuation field of one, whatever the C_BCAST type
   and whether the CRC checked (an exact-size buffer, which the sanitizer build checks). */
static void
test_single_c_bcast_reads_no_continuation(void) {
    for (uint8_t type = 0U; type < 8U; type++) {
        for (uint32_t crc = 0U; crc < 2U; crc++) {
            reset();
            dmr_sm_init(&g_opts, &g_state);
            uint8_t* bits = (uint8_t*)calloc(96U, 1U);
            uint8_t* bytes = (uint8_t*)calloc(12U, 1U);
            assert(bits != NULL && bytes != NULL);
            bits[0] = 1U; /* last block */
            bytes[0] = (uint8_t)(0x80U | 40U);
            put_bits(bits, 2, 40U, 6);
            put_bits(bits, 16, type, 5);
            put_bits(bits, 68, 143U, 12);
            dmr_cspdu(&g_opts, &g_state, bits, bytes, crc, 0U);
            assert(g_state.trunk_chan_map[143] == 0L);
            free(bits);
            free(bytes);
        }
    }
}

/* Vote_Now and Adjacent_Site naming a logical channel are single CSBKs (TS 102 361-4 §7.2.19.3.0(b), Table 7.71): the
   same announcement as an MBC is the wrong form, refused before any effect, another network's header dropping
   nothing. */
static void
test_logical_vote_now_adjacent_mbc_is_malformed(void) {
    static const uint8_t types[] = {2U, 6U};
    for (size_t i = 0; i < sizeof types; i++) {
        reset();
        dmr_sm_init(&g_opts, &g_state);
        send_aloha(0x1234U);
        assert(dmr_t3_chan_learn(&g_opts, &g_state, 144U, 451400000L, DMR_CHAN_ORIGIN_SITE) == 1);
        const uint32_t gen = dmr_t3_chan_generation(&g_state);
        prepare_no_effect_check();
        g_bcast_syscode = 0x1254U;
        send_c_bcast_mbc(types[i], 123U, 145U, 451U, 1200U, 1U);
        assert(no_effect_since_prepare());
        assert(g_state.trunk_chan_map[145] == 0L && candidate_count(451150000L) == 0);
        assert(dmr_t3_chan_generation(&g_state) == gen && g_state.trunk_chan_map[144] == 451400000L);
    }
    dsd_state_trunk_lcn_free(&g_state);
}

/* A standard C_BCAST single block carries LB 1 (TS 102 361-4 Table 7.20): one with LB 0 is refused before its
   heartbeat or its network identity; a manufacturer's flags are its own. */
static void
test_single_c_bcast_without_last_block_is_malformed(void) {
    reset();
    dmr_sm_init(&g_opts, &g_state);
    send_aloha(0x1234U);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 146U, 451412500L, DMR_CHAN_ORIGIN_SITE) == 1);
    const uint32_t gen = dmr_t3_chan_generation(&g_state);
    prepare_no_effect_check();
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pf0(bits, bytes, 40U);
    bits[0] = 0U;
    bytes[0] = 40U;
    put_bits(bits, 16, 3U, 5);                 /* Local_Time */
    put_bits(bits, 40, 0x1254U & 0x3FFFU, 14); /* another network */
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1U, 0U);
    assert(no_effect_since_prepare());
    assert(dmr_t3_chan_generation(&g_state) == gen && g_state.trunk_chan_map[146] == 451412500L);

    bytes[1] = 0x10U; /* a manufacturer's */
    put_bits(bits, 8, 0x10U, 8);
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1U, 0U);
    assert(g_state.last_cc_sync_time_m > 1.0);
}

/* A P_CLEAR on LPCN 0 sends its parties to the last confirmed TSCC and is a single CSBK; only 0xFFF heads the MBC
   form (TS 102 361-4 §7.1.1.3.2). The MBC on LPCN 0 is refused before any effect; the single block is not. */
static void
test_p_clear_mbc_on_lpcn_zero_is_malformed(void) {
    reset();
    dmr_sm_init(&g_opts, &g_state);
    prepare_no_effect_check();
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pf0(bits, bytes, 46U);
    put_bits(bits, 32, 0xFFFED4U, 24);
    put_abs_params(bits, 147U, 1300U);
    dispatch_mbc(bits, bytes, 46U);
    assert(no_effect_since_prepare());
    assert(g_state.trunk_chan_map[147] == 0L);

    build_pf0(bits, bytes, 46U);
    put_bits(bits, 32, 0xFFFED4U, 24);
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1U, 0U);
    assert(g_state.last_cc_sync_time_m > 1.0);
}

/* Another writer taking a row the air learned takes the trust DMR gave it (an LPCN-only grant off the control channel
   needs that trust), and moves the channel revision even at the row's frequency. A write to a row the air does not
   hold moves neither. */
static void
test_foreign_takeover_drops_trust_and_moves_revision(void) {
    reset();
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 148U, 451425000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.dmr_lcn_trust[148] == 2U);
    uint32_t rev = dmr_t3_chan_revision(&g_state);
    dsd_state_set_trunk_chan_freq(&g_state, 148U, 451425000L);
    assert(dmr_t3_chan_origin(&g_state, 148U) == DMR_CHAN_ORIGIN_FILE);
    assert(g_state.dmr_lcn_trust[148] == 0U);
    assert(dmr_t3_chan_revision(&g_state) != rev);

    rev = dmr_t3_chan_revision(&g_state);
    g_state.dmr_lcn_trust[149] = 1U;
    dsd_state_set_trunk_chan_freq(&g_state, 149U, 451437500L);
    assert(g_state.dmr_lcn_trust[149] == 1U);
    assert(dmr_t3_chan_revision(&g_state) == rev);
}

/* A target snapshot carries the revision a reader sees, the takeover count included: after one learn and one
   same-frequency takeover it saves that revision, and restoring it, into a fresh state or over the state it came
   from, moves the revision past it. */
static void
test_save_restore_keeps_effective_revision(void) {
    reset();
    static dmr_t3_chan_target_snapshot snap;
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 148U, 451425000L, DMR_CHAN_ORIGIN_SITE) == 1);
    dsd_state_set_trunk_chan_freq(&g_state, 148U, 451425000L);
    const uint32_t rev = dmr_t3_chan_revision(&g_state);
    dmr_t3_chan_save(&g_state, &snap);
    assert(snap.revision == rev);
    dmr_t3_chan_restore(&g_state, &snap);
    assert(dmr_t3_chan_revision(&g_state) > rev);

    reset();
    dmr_t3_chan_restore(&g_state, &snap);
    assert(dmr_t3_chan_revision(&g_state) > rev);
}

/* ---- Missing-channel ledger (F1) ---- */

static int g_override_warnings;
static int g_override_cap_warnings;
static int g_missing_notices;
static char g_missing_notice[256];
static int g_missing_summaries;
static char g_missing_summary[600];

static void
missing_log_tap(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)level;
    (void)ctx;
    if (text == NULL) {
        return;
    }
    if (strncmp(text, "WARNING: DMR channel overrides file ", 36) == 0) {
        g_override_warnings++;
        g_override_cap_warnings += strstr(text, " names more than 256 channels; skipping.") != NULL ? 1 : 0;
        return;
    }
    if (strncmp(text, "NOTICE: DMR Tier III: channel ", 30) == 0) {
        g_missing_notices++;
        DSD_SNPRINTF(g_missing_notice, sizeof g_missing_notice, "%s", text);
    } else if (strncmp(text, "NOTICE: DMR Tier III: ", 22) == 0 || strncmp(text, "NOTICE: DMR Tier III (", 22) == 0) {
        g_missing_summaries++;
        DSD_SNPRINTF(g_missing_summary, sizeof g_missing_summary, "%s", text);
    }
}

static void
reset_missing(void) {
    reset();
    g_state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    g_missing_notices = 0;
    g_missing_notice[0] = '\0';
    g_missing_summaries = 0;
    g_missing_summary[0] = '\0';
}

/* Grant @p opcode for LPCN @p lpcn, destination slot @p slot, target 1001 and source 2002, feature set @p fid. */
static void
send_grant_fid(uint8_t opcode, uint16_t lpcn, uint8_t slot, uint8_t fid) {
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pf0(bits, bytes, opcode);
    bytes[1] = fid;
    put_bits(bits, 8, fid, 8);
    put_bits(bits, 16, lpcn & 0x0FFFU, 12);
    bits[28] = (uint8_t)(slot & 1U);
    put_bits(bits, 32, 1001U, 24);
    put_bits(bits, 56, 2002U, 24);
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1U, 0U);
}

static void
send_grant(uint8_t opcode, uint16_t lpcn, uint8_t slot) {
    send_grant_fid(opcode, lpcn, slot, 0U);
}

static dmr_t3_missing_ledger*
missing_copy(void) {
    static dmr_t3_missing_ledger out;
    dmr_t3_missing_copy(&g_state, &out);
    return &out;
}

static int
bit_set(const uint8_t* set, uint16_t lpcn) {
    return (set[lpcn >> 3] >> (lpcn & 7U)) & 1U;
}

static void
test_unmapped_grant_recorded_once_per_channel(void) {
    reset_missing();
    dsd_decode_clock_use_test(UINT64_C(7000) * UINT64_C(1000000));
    send_grant(49U, 12U, 1U);
    assert(dmr_t3_missing_count(&g_state) == 1U);
    assert(g_missing_notices == 1);
    assert(strcmp(g_missing_notice,
                  "NOTICE: DMR Tier III: channel 12 has no frequency; dsd-neo cannot retune to its calls\n")
           == 0);
    dmr_t3_missing_ledger* l = missing_copy();
    assert(l->detail_count == 1U);
    assert(l->details[0].lpcn == 12U && l->details[0].slot == 1U);
    assert(l->details[0].group == 1U && l->details[0].data == 0U && l->details[0].unverified == 0U);
    assert(l->details[0].recipient == 1001U && l->details[0].caller == 2002U);
    assert(l->details[0].last_heard_ms == UINT64_C(7000));
    assert(bit_set(l->missing, 12U) && bit_set(l->checked, 12U) && bit_set(l->reported, 12U));
    const uint32_t rev = l->revision;

    /* The same channel again, on the other slot as a private data grant: one channel, one notice, the newest
     * details. */
    dsd_decode_clock_test_set_ns(UINT64_C(9000) * UINT64_C(1000000));
    send_grant(51U, 12U, 0U);
    assert(dmr_t3_missing_count(&g_state) == 1U);
    assert(g_missing_notices == 1);
    l = missing_copy();
    assert(l->detail_count == 1U && l->revision != rev);
    assert(l->details[0].slot == 0U && l->details[0].group == 0U && l->details[0].data == 1U);
    assert(l->details[0].last_heard_ms == UINT64_C(9000));

    /* The exit summary names it. */
    dmr_t3_missing_log_summary(&g_state);
    assert(g_missing_summaries == 1);
    assert(strstr(g_missing_summary, "1 channel") != NULL && strstr(g_missing_summary, " 12") != NULL);
    dsd_decode_clock_use_system();
}

static void
test_count_exact_past_16_details(void) {
    reset_missing();
    for (uint16_t l = 100U; l < 120U; l++) {
        send_grant(49U, l, 0U);
    }
    assert(dmr_t3_missing_count(&g_state) == 20U);
    assert(g_missing_notices == 20);
    dmr_t3_missing_ledger* l = missing_copy();
    assert(l->detail_count == 16U);
    assert(l->details[0].lpcn == 119U && l->details[15].lpcn == 104U);
    for (uint16_t c = 100U; c < 120U; c++) {
        assert(bit_set(l->missing, c));
    }

    /* A repeat moves its channel to the front and drops nothing else. */
    send_grant(49U, 110U, 0U);
    l = missing_copy();
    assert(dmr_t3_missing_count(&g_state) == 20U && l->detail_count == 16U);
    assert(l->details[0].lpcn == 110U && l->details[1].lpcn == 119U && l->details[15].lpcn == 104U);
    /* One evicted from the details comes back at the front, the oldest goes. */
    send_grant(49U, 100U, 0U);
    l = missing_copy();
    assert(l->details[0].lpcn == 100U && l->details[15].lpcn == 105U);
    assert(g_missing_notices == 20);

    dmr_t3_missing_log_summary(&g_state);
    assert(g_missing_summaries == 1 && strstr(g_missing_summary, "20 channels") != NULL);
    assert(strstr(g_missing_summary, "(+4 more)") != NULL);

    /* A target named at such length that the summary does not fit says it was cut. */
    char target[400];
    DSD_MEMSET(target, 'x', sizeof target - 1U);
    target[sizeof target - 1U] = '\0';
    dmr_t3_missing_log_summary_for(l, target);
    assert(g_missing_summaries == 2 && strstr(g_missing_summary, "20 channels") != NULL);
    const size_t len = strlen(g_missing_summary);
    assert(len > 4U && strcmp(g_missing_summary + len - 4U, "...\n") == 0);
    assert(strstr(g_missing_summary, "(+4 more)") == NULL);
}

static void
test_mapped_grant_records_nothing(void) {
    reset_missing();
    g_state.trunk_chan_map[12] = 451012500L;
    dmr_t3_chan_note_map_loaded(&g_state);
    dmr_sm_init(&g_opts, &g_state);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_freq_request = tune_request});
    send_grant(49U, 12U, 0U);
    assert(g_state.trunk_vc_freq[0] == 451012500L); /* followed as before */
    assert(dmr_t3_missing_count(&g_state) == 0U);
    assert(missing_copy()->detail_count == 0U && !bit_set(missing_copy()->reported, 12U));
    assert(g_missing_notices == 0);
    /* Not channels at all: CHNULL, and 4095 with no absolute block. */
    g_opts.trunk_is_tuned = 0;
    send_grant(49U, 0U, 0U);
    assert(dmr_t3_missing_count(&g_state) == 0U);
    dmr_t3_missing_log_summary(&g_state);
    assert(g_missing_summaries == 0);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

static void
test_ras_grant_ledger_unverified(void) {
    reset_missing();
    g_opts.aggressive_framesync = 0; /* -F */
    g_state.currentslot = 0;
    g_state.dmr_ras_signatures = 2U;
    g_state.event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED;
    send_grant(49U, 40U, 0U);
    assert(dmr_t3_missing_count(&g_state) == 1U);
    dmr_t3_missing_ledger* l = missing_copy();
    assert(l->details[0].unverified == 1U && !bit_set(l->checked, 40U));
    assert(g_missing_notices == 1);
    assert(strcmp(g_missing_notice, "NOTICE: DMR Tier III: channel 40 has no frequency; dsd-neo cannot retune to "
                                    "its calls (unchecked)\n")
           == 0);
    dmr_t3_missing_log_summary(&g_state);
    assert(strstr(g_missing_summary, "40 (unchecked)") != NULL);

    /* A checked repeat clears the mark, and no second notice; a later RAS one does not bring the mark back. */
    g_state.event_crc_invalid[0] = 0U;
    send_grant(49U, 40U, 0U);
    l = missing_copy();
    assert(l->details[0].unverified == 0U && bit_set(l->checked, 40U));
    g_state.event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED;
    send_grant(49U, 40U, 0U);
    assert(missing_copy()->details[0].unverified == 0U);
    assert(g_missing_notices == 1);

    /* RAS not confirmed on this carrier, or a failed CRC: nothing recorded. */
    g_state.dmr_ras_signatures = 0U;
    send_grant(49U, 41U, 0U);
    g_state.dmr_ras_signatures = 2U;
    g_state.event_crc_invalid[0] = DSD_CRC_FAILED;
    send_grant(49U, 42U, 0U);
    assert(dmr_t3_missing_count(&g_state) == 1U);
    assert(g_missing_notices == 1);
}

static void
test_synthesized_and_ms_data_56_ignored(void) {
    reset_missing();
    dmr_cspdu_release_channel(&g_opts, &g_state);
    assert(dmr_t3_missing_count(&g_state) == 0U);

    /* MS-sourced: 56 is BS_Dwn_Act there, and an inbound grant opcode is no grant the site sent. */
    g_state.synctype = DSD_SYNC_DMR_MS_DATA;
    send_grant(56U, 43U, 0U);
    send_grant(49U, 44U, 0U);
    assert(dmr_t3_missing_count(&g_state) == 0U);

    /* A manufacturer's feature set is not the standard grant. */
    g_state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    send_grant_fid(49U, 45U, 0U, 0x10U);
    assert(dmr_t3_missing_count(&g_state) == 0U);

    /* Outbound 56 is a group data grant. */
    send_grant(56U, 46U, 0U);
    assert(dmr_t3_missing_count(&g_state) == 1U);
    assert(missing_copy()->details[0].data == 1U && missing_copy()->details[0].group == 1U);
    assert(g_missing_notices == 1);
}

static void
test_reconcile_clears_only_mapped_bits(void) {
    reset_missing();
    send_grant(49U, 10U, 0U);
    send_grant(49U, 11U, 0U);
    send_grant(49U, 12U, 0U);
    const uint32_t rev = missing_copy()->revision;

    /* A map load that names 11 (and 13, never granted) clears 11 only and sets nothing. */
    g_state.trunk_chan_map[11] = 451025000L;
    g_state.trunk_chan_map[13] = 451037500L;
    dmr_t3_chan_note_map_loaded(&g_state);
    dmr_t3_missing_ledger* l = missing_copy();
    assert(dmr_t3_missing_count(&g_state) == 2U && l->revision != rev);
    assert(bit_set(l->missing, 10U) && !bit_set(l->missing, 11U) && bit_set(l->missing, 12U));
    assert(!bit_set(l->missing, 13U) && !bit_set(l->checked, 11U));
    assert(bit_set(l->reported, 11U)); /* reported survives everything but a network change */
    assert(l->detail_count == 2U && l->details[0].lpcn == 12U && l->details[1].lpcn == 10U);

    /* A row learned over the air clears its channel too. */
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 12U, 451050000L, DMR_CHAN_ORIGIN_SITE) == 1);
    l = missing_copy();
    assert(dmr_t3_missing_count(&g_state) == 1U && l->detail_count == 1U && l->details[0].lpcn == 10U);

    /* An explicit reconcile with nothing newly mapped changes nothing. */
    const uint32_t rev2 = l->revision;
    dmr_t3_missing_reconcile(&g_state);
    assert(missing_copy()->revision == rev2);

    /* The map emptied: 11 granted again is recorded, with no second notice. */
    DSD_MEMSET(g_state.trunk_chan_map, 0, sizeof g_state.trunk_chan_map);
    dmr_t3_chan_note_map_cleared(&g_state);
    send_grant(49U, 11U, 0U);
    assert(dmr_t3_missing_count(&g_state) == 2U);
    assert(g_missing_notices == 3);
}

static void
test_missing_network_change_resets(void) {
    reset_missing();
    send_grant(49U, 10U, 0U);
    send_grant(49U, 11U, 1U);
    assert(dmr_t3_missing_count(&g_state) == 2U && g_missing_notices == 2);
    const uint32_t rev = missing_copy()->revision;
    dmr_t3_chan_network_changed(&g_opts, &g_state);
    dmr_t3_missing_ledger* l = missing_copy();
    assert(dmr_t3_missing_count(&g_state) == 0U && l->detail_count == 0U && l->revision != rev);
    assert(!bit_set(l->reported, 10U) && !bit_set(l->checked, 10U));
    send_grant(49U, 10U, 0U);
    assert(dmr_t3_missing_count(&g_state) == 1U && g_missing_notices == 3);

    /* The save and restore carry the ledger: a restore brings it back with a revision past both. */
    static dmr_t3_chan_target_snapshot a;
    dmr_t3_chan_save(&g_state, &a);
    dmr_t3_chan_network_changed(&g_opts, &g_state);
    const uint32_t rev2 = missing_copy()->revision;
    dmr_t3_chan_restore(&g_state, &a);
    l = missing_copy();
    assert(dmr_t3_missing_count(&g_state) == 1U && l->details[0].lpcn == 10U);
    assert(l->revision > rev2 && l->revision > a.missing.revision);
}

/* ---- The user's channels (F3 overlay) ---- */

/* An edit checked against the channel as it is now (what an editor that just read it sends: the frequency DMR uses). */
static dmr_t3_set_result
overlay_edit(uint16_t lpcn, dmr_t3_overlay_op op, long hz) {
    return dmr_t3_overlay_apply(&g_opts, &g_state, lpcn, op, hz, dmr_t3_chan_effective_hz(&g_state, lpcn),
                                (uint8_t)dmr_t3_chan_origin(&g_state, lpcn), dmr_t3_chan_generation(&g_state), 1);
}

static void
test_overlay_set_hide_restore_live(void) {
    reset();
    g_state.trunk_chan_map[12] = 451012500L;
    dmr_t3_chan_note_map_loaded(&g_state);
    uint32_t rev = dmr_t3_chan_revision(&g_state);

    /* SET: the user's frequency is live and the user's; the file's row is kept for RESTORE. */
    assert(overlay_edit(12U, DMR_T3_OVERLAY_SET, 451137500L) == DMR_T3_SET_OK);
    assert(g_state.trunk_chan_map[12] == 451137500L && dmr_t3_chan_origin(&g_state, 12U) == DMR_CHAN_ORIGIN_USER);
    long hz = 0;
    long base_hz = 0;
    assert(dmr_t3_overlay_get(&g_state, 12U, &hz, &base_hz) == DMR_T3_OVERLAY_SET);
    assert(hz == 451137500L && base_hz == 451012500L && dmr_t3_overlay_count(&g_state) == 1U);
    assert(dmr_t3_chan_revision(&g_state) != rev);
    /* Nothing over the air replaces it. */
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 12U, 451250000L, DMR_CHAN_ORIGIN_SITE) == 0);
    assert(g_state.trunk_chan_map[12] == 451137500L);

    /* HIDE: no frequency live, still the user's (listed, never filled over the air), the same base. */
    assert(overlay_edit(12U, DMR_T3_OVERLAY_HIDE, 0L) == DMR_T3_SET_OK);
    assert(g_state.trunk_chan_map[12] == 0L && dmr_t3_chan_origin(&g_state, 12U) == DMR_CHAN_ORIGIN_USER);
    assert(dmr_t3_overlay_get(&g_state, 12U, &hz, &base_hz) == DMR_T3_OVERLAY_HIDE && hz == 0L);
    assert(base_hz == 451012500L && dmr_t3_overlay_count(&g_state) == 1U);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 12U, 451250000L, DMR_CHAN_ORIGIN_SITE) == 0);
    assert(g_state.trunk_chan_map[12] == 0L);

    /* RESTORE: the file's row comes back as the file's. */
    assert(overlay_edit(12U, DMR_T3_OVERLAY_RESTORE, 0L) == DMR_T3_SET_OK);
    assert(g_state.trunk_chan_map[12] == 451012500L && dmr_t3_chan_origin(&g_state, 12U) == DMR_CHAN_ORIGIN_FILE);
    assert(dmr_t3_overlay_get(&g_state, 12U, NULL, NULL) == 0 && dmr_t3_overlay_count(&g_state) == 0U);

    /* A channel with no row: SET adds it, RESTORE empties it again, the trust byte back too. */
    g_state.dmr_lcn_trust[20] = 0U;
    assert(overlay_edit(20U, DMR_T3_OVERLAY_SET, 451250000L) == DMR_T3_SET_OK);
    assert(g_state.trunk_chan_map[20] == 451250000L && dmr_t3_chan_origin(&g_state, 20U) == DMR_CHAN_ORIGIN_USER);
    assert(overlay_edit(20U, DMR_T3_OVERLAY_RESTORE, 0L) == DMR_T3_SET_OK);
    assert(g_state.trunk_chan_map[20] == 0L && dmr_t3_chan_origin(&g_state, 20U) == DMR_CHAN_ORIGIN_NONE);

    /* RESTORE with no entry and a SET to the same frequency change nothing. */
    rev = dmr_t3_chan_revision(&g_state);
    assert(overlay_edit(21U, DMR_T3_OVERLAY_RESTORE, 0L) == DMR_T3_SET_OK);
    assert(overlay_edit(22U, DMR_T3_OVERLAY_SET, 451262500L) == DMR_T3_SET_OK);
    const uint32_t rev2 = dmr_t3_chan_revision(&g_state);
    assert(rev2 != rev);
    assert(overlay_edit(22U, DMR_T3_OVERLAY_SET, 451262500L) == DMR_T3_SET_OK);
    assert(dmr_t3_chan_revision(&g_state) == rev2);

    /* Not channels, operations or frequencies. */
    assert(overlay_edit(0U, DMR_T3_OVERLAY_SET, 451000000L) == DMR_T3_SET_INVALID);
    assert(overlay_edit(4095U, DMR_T3_OVERLAY_HIDE, 0L) == DMR_T3_SET_INVALID);
    assert(overlay_edit(23U, DMR_T3_OVERLAY_SET, 0L) == DMR_T3_SET_INVALID);
    assert(overlay_edit(23U, DMR_T3_OVERLAY_SET, 99999L) == DMR_T3_SET_INVALID);
    assert(g_state.trunk_chan_map[23] == 0L && dmr_t3_chan_revision(&g_state) == rev2);
}

/* An edit made against a row that changed since the editor read it is refused and changes nothing. */
/* An entry over a row the air taught is DMR's own write: no takeover is counted, and RESTORE gives the row back to the
   air (its dsd_state::dmr_lcn_ota bit), so a network change drops it again. A row another writer took from the air
   reads FILE, and RESTORE keeps it FILE, which a network change leaves. */
static void
test_overlay_restore_gives_the_air_its_row(void) {
    reset();
    g_opts.dmr_t3_heuristic_fill = 0;
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 30U, 451300000L, DMR_CHAN_ORIGIN_SITE) == 1);
    const uint8_t trust = g_state.dmr_lcn_trust[30];
    const uint32_t takeovers = g_state.dmr_lcn_ota_seq;
    assert(overlay_edit(30U, DMR_T3_OVERLAY_SET, 451312500L) == DMR_T3_SET_OK);
    assert(g_state.dmr_lcn_ota_seq == takeovers && !dsd_state_dmr_lcn_ota_get(&g_state, 30U));
    assert(dmr_t3_chan_origin(&g_state, 30U) == DMR_CHAN_ORIGIN_USER);
    assert(overlay_edit(30U, DMR_T3_OVERLAY_RESTORE, 0L) == DMR_T3_SET_OK);
    assert(g_state.trunk_chan_map[30] == 451300000L && dmr_t3_chan_origin(&g_state, 30U) == DMR_CHAN_ORIGIN_SITE);
    assert(dsd_state_dmr_lcn_ota_get(&g_state, 30U) && g_state.dmr_lcn_trust[30] == trust);
    assert(g_state.dmr_lcn_ota_seq == takeovers);

    assert(dmr_t3_chan_learn(&g_opts, &g_state, 31U, 451400000L, DMR_CHAN_ORIGIN_SITE) == 1);
    dsd_state_set_trunk_chan_freq(&g_state, 31U, 451400000L); /* another writer takes it at its frequency */
    assert(dmr_t3_chan_origin(&g_state, 31U) == DMR_CHAN_ORIGIN_FILE);
    assert(overlay_edit(31U, DMR_T3_OVERLAY_HIDE, 0L) == DMR_T3_SET_OK);
    assert(overlay_edit(31U, DMR_T3_OVERLAY_RESTORE, 0L) == DMR_T3_SET_OK);
    assert(g_state.trunk_chan_map[31] == 451400000L && dmr_t3_chan_origin(&g_state, 31U) == DMR_CHAN_ORIGIN_FILE);

    dmr_t3_chan_network_changed(&g_opts, &g_state);
    assert(g_state.trunk_chan_map[30] == 0L && dmr_t3_chan_origin(&g_state, 30U) == DMR_CHAN_ORIGIN_NONE);
    assert(g_state.trunk_chan_map[31] == 451400000L && dmr_t3_chan_origin(&g_state, 31U) == DMR_CHAN_ORIGIN_FILE);
}

static void
test_overlay_stale_expectations_refused(void) {
    reset();
    g_state.trunk_chan_map[12] = 451012500L;
    dmr_t3_chan_note_map_loaded(&g_state);
    const uint32_t gen = dmr_t3_chan_generation(&g_state);
    const uint32_t rev = dmr_t3_chan_revision(&g_state);
    const uint8_t file = (uint8_t)DMR_CHAN_ORIGIN_FILE;
    assert(dmr_t3_overlay_apply(&g_opts, &g_state, 12U, DMR_T3_OVERLAY_SET, 451137500L, 451025000L, file, gen, 1)
           == DMR_T3_SET_STALE);
    assert(dmr_t3_overlay_apply(&g_opts, &g_state, 12U, DMR_T3_OVERLAY_SET, 451137500L, 451012500L,
                                (uint8_t)DMR_CHAN_ORIGIN_SITE, gen, 1)
           == DMR_T3_SET_STALE);
    assert(dmr_t3_overlay_apply(&g_opts, &g_state, 12U, DMR_T3_OVERLAY_SET, 451137500L, 451012500L, file, gen + 1U, 1)
           == DMR_T3_SET_STALE);
    assert(g_state.trunk_chan_map[12] == 451012500L && dmr_t3_overlay_count(&g_state) == 0U);
    assert(dmr_t3_chan_revision(&g_state) == rev);

    /* A network change moves the generation: what the editor read on the old network is stale. */
    dmr_t3_chan_network_changed(&g_opts, &g_state);
    assert(dmr_t3_overlay_apply(&g_opts, &g_state, 12U, DMR_T3_OVERLAY_HIDE, 0L, 451012500L, file, gen, 1)
           == DMR_T3_SET_STALE);
    assert(g_state.trunk_chan_map[12] == 451012500L);
    /* The expectations hold: applied. Unchecked (a start-up file): applied whatever they say. */
    assert(dmr_t3_overlay_apply(&g_opts, &g_state, 12U, DMR_T3_OVERLAY_HIDE, 0L, 451012500L, file,
                                dmr_t3_chan_generation(&g_state), 1)
           == DMR_T3_SET_OK);
    assert(dmr_t3_overlay_apply(&g_opts, &g_state, 13U, DMR_T3_OVERLAY_SET, 451150000L, 7L, 9U, 99U, 0)
           == DMR_T3_SET_OK);
    assert(g_state.trunk_chan_map[12] == 0L && g_state.trunk_chan_map[13] == 451150000L);
}

static void
test_overlay_refused_under_trunk_scan(void) {
    reset();
    g_opts.trunk_scan_enabled = 1;
    assert(overlay_edit(12U, DMR_T3_OVERLAY_SET, 451137500L) == DMR_T3_SET_TRUNK_SCAN);
    assert(overlay_edit(12U, DMR_T3_OVERLAY_HIDE, 0L) == DMR_T3_SET_TRUNK_SCAN);
    assert(g_state.trunk_chan_map[12] == 0L && dmr_t3_overlay_count(&g_state) == 0U);
    assert(dmr_t3_overlay_load_csv(&g_opts, &g_state, "never-opened.csv") == -1);
}

/* Review Focus 4: a reload of the saved map (an RR refresh writes the file and reloads it) lays the overlay over the
   new rows, the new file's row becomes the base, a clear keeps the overlay over the empty map. */
static void
test_overlay_reapplied_after_reload(void) {
    reset();
    g_state.trunk_chan_map[12] = 451012500L;
    g_state.trunk_chan_map[13] = 451025000L;
    dmr_t3_chan_note_map_loaded(&g_state);
    assert(overlay_edit(12U, DMR_T3_OVERLAY_SET, 451137500L) == DMR_T3_SET_OK);
    assert(overlay_edit(13U, DMR_T3_OVERLAY_HIDE, 0L) == DMR_T3_SET_OK);

    /* The reload replaces the map wholesale (chan_map_adopt()), then notes it and lays the overlay again. */
    DSD_MEMSET(g_state.trunk_chan_map, 0, sizeof g_state.trunk_chan_map);
    g_state.trunk_chan_map[12] = 451012600L;
    g_state.trunk_chan_map[13] = 451025100L;
    g_state.trunk_chan_map[14] = 451037500L;
    dmr_t3_chan_note_map_loaded(&g_state);
    dmr_t3_overlay_reapply(&g_state);
    long base_hz = 0;
    assert(g_state.trunk_chan_map[12] == 451137500L && dmr_t3_chan_origin(&g_state, 12U) == DMR_CHAN_ORIGIN_USER);
    assert(g_state.trunk_chan_map[13] == 0L && dmr_t3_chan_origin(&g_state, 13U) == DMR_CHAN_ORIGIN_USER);
    assert(dmr_t3_chan_origin(&g_state, 14U) == DMR_CHAN_ORIGIN_FILE);
    assert(dmr_t3_overlay_get(&g_state, 12U, NULL, &base_hz) == DMR_T3_OVERLAY_SET && base_hz == 451012600L);
    /* A second re-apply with nothing replaced keeps the bases. */
    dmr_t3_overlay_reapply(&g_state);
    assert(dmr_t3_overlay_get(&g_state, 13U, NULL, &base_hz) == DMR_T3_OVERLAY_HIDE && base_hz == 451025100L);

    /* RESTORE returns the new file's row. */
    assert(overlay_edit(13U, DMR_T3_OVERLAY_RESTORE, 0L) == DMR_T3_SET_OK);
    assert(g_state.trunk_chan_map[13] == 451025100L && dmr_t3_chan_origin(&g_state, 13U) == DMR_CHAN_ORIGIN_FILE);

    /* A clear empties the base; the user's row stays. */
    DSD_MEMSET(g_state.trunk_chan_map, 0, sizeof g_state.trunk_chan_map);
    dmr_t3_chan_note_map_cleared(&g_state);
    dmr_t3_overlay_reapply(&g_state);
    assert(g_state.trunk_chan_map[12] == 451137500L && dmr_t3_chan_origin(&g_state, 12U) == DMR_CHAN_ORIGIN_USER);
    assert(dmr_t3_overlay_get(&g_state, 12U, NULL, &base_hz) == DMR_T3_OVERLAY_SET && base_hz == 0L);
    assert(overlay_edit(12U, DMR_T3_OVERLAY_RESTORE, 0L) == DMR_T3_SET_OK);
    assert(g_state.trunk_chan_map[12] == 0L && dmr_t3_chan_origin(&g_state, 12U) == DMR_CHAN_ORIGIN_NONE);
}

/* A network change drops what the air taught, from the overlay's bases too; the user's rows stay. */
static void
test_overlay_network_change_keeps_user_rows(void) {
    reset();
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 12U, 451012500L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(overlay_edit(12U, DMR_T3_OVERLAY_SET, 451137500L) == DMR_T3_SET_OK);
    assert(overlay_edit(13U, DMR_T3_OVERLAY_HIDE, 0L) == DMR_T3_SET_OK);
    dmr_t3_chan_network_changed(&g_opts, &g_state);
    long base_hz = 1;
    assert(g_state.trunk_chan_map[12] == 451137500L && dmr_t3_chan_origin(&g_state, 12U) == DMR_CHAN_ORIGIN_USER);
    assert(dmr_t3_chan_origin(&g_state, 13U) == DMR_CHAN_ORIGIN_USER && dmr_t3_overlay_count(&g_state) == 2U);
    assert(dmr_t3_overlay_get(&g_state, 12U, NULL, &base_hz) == DMR_T3_OVERLAY_SET && base_hz == 0L);
    assert(overlay_edit(12U, DMR_T3_OVERLAY_RESTORE, 0L) == DMR_T3_SET_OK);
    assert(g_state.trunk_chan_map[12] == 0L && dmr_t3_chan_origin(&g_state, 12U) == DMR_CHAN_ORIGIN_NONE);
    assert(g_state.dmr_lcn_trust[12] == 0U);
}

/* Review Focus 3: the overlay is the global map's, never a trunk-scan target's. A save never carries it and a restore
   never brings one back or takes the live one away (a Signal search session restores its own provenance over the same
   overlay); the trunk-scan target switch empties it with dmr_t3_overlay_discard(). */
static void
test_overlay_cleared_by_target_restore(void) {
    reset();
    assert(overlay_edit(12U, DMR_T3_OVERLAY_SET, 451137500L) == DMR_T3_SET_OK);
    static dmr_t3_chan_target_snapshot a;
    static dmr_t3_chan_target_snapshot b;
    dmr_t3_chan_save(&g_state, &a);
    DSD_MEMSET(&b, 0, sizeof b);
    dmr_t3_chan_restore(&g_state, &b);
    assert(dmr_t3_overlay_count(&g_state) == 1U && dmr_t3_overlay_get(&g_state, 12U, NULL, NULL) == DMR_T3_OVERLAY_SET);
    const uint32_t rev = dmr_t3_chan_revision(&g_state);
    dmr_t3_overlay_discard(&g_state);
    assert(dmr_t3_overlay_count(&g_state) == 0U && dmr_t3_overlay_get(&g_state, 12U, NULL, NULL) == 0);
    assert(dmr_t3_chan_revision(&g_state) != rev);
    assert(overlay_edit(13U, DMR_T3_OVERLAY_HIDE, 0L) == DMR_T3_SET_OK);
    dmr_t3_chan_restore(&g_state, &a);
    assert(dmr_t3_overlay_count(&g_state) == 1U && dmr_t3_overlay_get(&g_state, 12U, NULL, NULL) == 0);
    assert(dmr_t3_overlay_get(&g_state, 13U, NULL, NULL) == DMR_T3_OVERLAY_HIDE);
    dmr_t3_overlay_discard(&g_state);
    assert(dmr_t3_overlay_count(&g_state) == 0U && dmr_t3_overlay_get(&g_state, 13U, NULL, NULL) == 0);
}

/* At most DMR_T3_OVERLAY_MAX_ROWS channels have an entry: one more SET or HIDE is FULL and changes nothing; an edit of
   a channel that has one, a RESTORE, and a new entry after a RESTORE still apply. */
static void
test_overlay_bounded(void) {
    reset();
    for (uint16_t l = 1U; l <= (uint16_t)DMR_T3_OVERLAY_MAX_ROWS; l++) {
        assert(overlay_edit(l, DMR_T3_OVERLAY_SET, 451000000L + (long)l * 12500L) == DMR_T3_SET_OK);
    }
    assert(dmr_t3_overlay_count(&g_state) == (size_t)DMR_T3_OVERLAY_MAX_ROWS);
    const uint16_t next = (uint16_t)(DMR_T3_OVERLAY_MAX_ROWS + 1);
    const uint32_t rev = dmr_t3_chan_revision(&g_state);
    assert(overlay_edit(next, DMR_T3_OVERLAY_SET, 455000000L) == DMR_T3_SET_FULL);
    assert(overlay_edit(next, DMR_T3_OVERLAY_HIDE, 0L) == DMR_T3_SET_FULL);
    assert(g_state.trunk_chan_map[next] == 0L && dmr_t3_chan_origin(&g_state, next) == DMR_CHAN_ORIGIN_NONE);
    assert(dmr_t3_chan_revision(&g_state) == rev && dmr_t3_overlay_count(&g_state) == (size_t)DMR_T3_OVERLAY_MAX_ROWS);
    assert(overlay_edit(1U, DMR_T3_OVERLAY_HIDE, 0L) == DMR_T3_SET_OK);
    assert(overlay_edit(2U, DMR_T3_OVERLAY_RESTORE, 0L) == DMR_T3_SET_OK);
    assert(overlay_edit(next, DMR_T3_OVERLAY_HIDE, 0L) == DMR_T3_SET_OK);
    assert(dmr_t3_overlay_count(&g_state) == (size_t)DMR_T3_OVERLAY_MAX_ROWS);
}

/* Review Focus 5: hiding the channel of the call being followed leaves the call on its carrier; the next grant naming
   the channel is not followed, and it is no missing channel either. */
static void
test_hide_followed_channel_keeps_call(void) {
    reset_missing();
    g_state.trunk_chan_map[12] = 451012500L;
    g_state.trunk_chan_map[13] = 451025000L;
    dmr_t3_chan_note_map_loaded(&g_state);
    dmr_sm_init(&g_opts, &g_state);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_freq_request = tune_request});
    send_grant(49U, 12U, 0U);
    assert(g_opts.trunk_is_tuned == 1 && g_state.trunk_vc_freq[0] == 451012500L);
    assert(dmr_sm_get_ctx()->state == DMR_SM_TUNED);

    assert(overlay_edit(12U, DMR_T3_OVERLAY_HIDE, 0L) == DMR_T3_SET_OK);
    assert(g_opts.trunk_is_tuned == 1 && g_state.trunk_vc_freq[0] == 451012500L);
    assert(dmr_sm_get_ctx()->state == DMR_SM_TUNED);

    /* Back on the control channel: a grant for 12 is not followed and not recorded; 13's still is. */
    g_opts.trunk_is_tuned = 0;
    g_state.trunk_vc_freq[0] = g_state.trunk_vc_freq[1] = 0;
    dmr_sm_init(&g_opts, &g_state);
    send_grant(49U, 12U, 0U);
    assert(g_opts.trunk_is_tuned == 0 && g_state.trunk_vc_freq[0] == 0L);
    assert(dmr_t3_missing_count(&g_state) == 0U && g_missing_notices == 0);
    send_grant(49U, 13U, 0U);
    assert(g_opts.trunk_is_tuned == 1 && g_state.trunk_vc_freq[0] == 451025000L);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* A hidden channel leaves the ledger and stays out of it; restored, its grants are recorded again (one notice). */
static void
test_overlay_hidden_channel_leaves_ledger(void) {
    reset_missing();
    send_grant(49U, 40U, 0U);
    send_grant(49U, 41U, 0U);
    assert(dmr_t3_missing_count(&g_state) == 2U && g_missing_notices == 2);
    assert(overlay_edit(40U, DMR_T3_OVERLAY_HIDE, 0L) == DMR_T3_SET_OK);
    assert(overlay_edit(41U, DMR_T3_OVERLAY_SET, 451512500L) == DMR_T3_SET_OK);
    assert(dmr_t3_missing_count(&g_state) == 0U && missing_copy()->detail_count == 0U);
    send_grant(49U, 40U, 0U);
    assert(dmr_t3_missing_count(&g_state) == 0U);
    assert(overlay_edit(40U, DMR_T3_OVERLAY_RESTORE, 0L) == DMR_T3_SET_OK);
    send_grant(49U, 40U, 0U);
    assert(dmr_t3_missing_count(&g_state) == 1U && g_missing_notices == 2);
}

/* Review gate b-r1 E1/E3: the user's entry decides a grant's channel before the shared map does. A P25 IDEN 0
   invalidation (another protocol's writer) empties rows 0..4095, the user's set row among them: a logical grant for it
   is still followed on the user's frequency, never recorded as missing, and the same SET again lays the entry over the
   live row once more. An absolute grant for a hidden channel is not followed (nor learned into the row), and one for a
   set channel follows the user's frequency, not the advertised one. */
static void
test_overlay_governs_grants_over_the_shared_map(void) {
    reset_bursts();
    assert(overlay_edit(12U, DMR_T3_OVERLAY_SET, 451137500L) == DMR_T3_SET_OK);
    dsd_state_p25_invalidate_iden_chan_map(&g_state, 0);
    assert(g_state.trunk_chan_map[12] == 0L && dmr_t3_chan_origin(&g_state, 12U) == DMR_CHAN_ORIGIN_USER);
    send_grant(49U, 12U, 0U);
    assert(g_opts.trunk_is_tuned == 1 && g_state.trunk_vc_freq[0] == 451137500L);
    assert(dmr_t3_missing_count(&g_state) == 0U);
    /* Nothing over the air takes the emptied row while the entry holds it. */
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 12U, 451250000L, DMR_CHAN_ORIGIN_SITE) == 0);
    assert(g_state.trunk_chan_map[12] == 0L);

    /* The editor's repeat of the same SET repairs the live row. */
    const uint32_t rev = dmr_t3_chan_revision(&g_state);
    assert(overlay_edit(12U, DMR_T3_OVERLAY_SET, 451137500L) == DMR_T3_SET_OK);
    assert(g_state.trunk_chan_map[12] == 451137500L && dmr_t3_chan_origin(&g_state, 12U) == DMR_CHAN_ORIGIN_USER);
    assert(dmr_t3_chan_revision(&g_state) != rev);

    /* Absolute grants: hidden 126 is not followed and its row stays empty; set 127 follows the user's frequency. */
    reset_bursts();
    assert(overlay_edit(126U, DMR_T3_OVERLAY_HIDE, 0L) == DMR_T3_SET_OK);
    send_abs_grant_mbc(0, 49U, 126U, 451U, 700U);
    assert(not_followed(126U) && dmr_t3_chan_origin(&g_state, 126U) == DMR_CHAN_ORIGIN_USER);
    assert(dmr_t3_missing_count(&g_state) == 0U);
    reset_bursts();
    assert(overlay_edit(127U, DMR_T3_OVERLAY_SET, 451162500L) == DMR_T3_SET_OK);
    send_abs_grant_mbc(0, 49U, 127U, 451U, 700U);
    assert(g_opts.trunk_is_tuned == 1 && g_state.trunk_vc_freq[0] == 451162500L);
    assert(g_state.trunk_chan_map[127] == 451162500L && dmr_t3_chan_origin(&g_state, 127U) == DMR_CHAN_ORIGIN_USER);
    /* With no entry the advertised frequency is followed, as before. */
    reset_bursts();
    send_abs_grant_mbc(0, 49U, 128U, 451U, 700U);
    assert(g_opts.trunk_is_tuned == 1 && g_state.trunk_vc_freq[0] == 451087500L);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* Review gate b-r1 E2: a hidden channel (the user's row at no frequency) is no heuristic anchor. With channel 10 hidden
   and 12 the only mapped row, the fill has one anchor and invents nothing for 11. */
static void
test_hidden_channel_anchors_no_estimate(void) {
    reset();
    g_opts.dmr_t3_heuristic_fill = 1;
    assert(overlay_edit(10U, DMR_T3_OVERLAY_HIDE, 0L) == DMR_T3_SET_OK);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 12U, 450000000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.trunk_chan_map[11] == 0L && dmr_t3_chan_origin(&g_state, 11U) == DMR_CHAN_ORIGIN_NONE);
    /* Two real anchors still fill the gap between them. */
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 14U, 450025000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.trunk_chan_map[13] == 450012500L && dmr_t3_chan_origin(&g_state, 13U) == DMR_CHAN_ORIGIN_ESTIMATE);
    assert(g_state.trunk_chan_map[11] == 0L);
}

/* Review gate b-r1 E6: another writer of the shared map (P25's frequency cache, a -Y visit, a replay) fills a missing
   channel's row without a reconcile. The next map change DMR sees reconciles every outstanding channel, not only its
   own: a learned row, the exit summary, and a save each catch up. A mapped grant does no reconcile. */
static void
test_missing_reconciled_after_other_writers(void) {
    reset_missing();
    send_grant(49U, 12U, 0U);
    send_grant(49U, 14U, 0U);
    send_grant(49U, 16U, 0U);
    assert(dmr_t3_missing_count(&g_state) == 3U);
    dsd_state_set_trunk_chan_freq(&g_state, 12U, 451150000L);
    /* DMR learns another channel: 12 goes with it. */
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 13U, 451162500L, DMR_CHAN_ORIGIN_SITE) == 1);
    dmr_t3_missing_ledger* l = missing_copy();
    assert(dmr_t3_missing_count(&g_state) == 2U && !bit_set(l->missing, 12U) && bit_set(l->missing, 14U));
    assert(l->detail_count == 2U);

    /* The exit summary reconciles first: 14 filled by another writer is not reported. */
    dsd_state_set_trunk_chan_freq(&g_state, 14U, 451175000L);
    g_missing_summaries = 0;
    dmr_t3_missing_log_summary(&g_state);
    assert(g_missing_summaries == 1 && strstr(g_missing_summary, " 14") == NULL);
    assert(strstr(g_missing_summary, ": 16\n") != NULL && dmr_t3_missing_count(&g_state) == 1U);

    /* A save does too, so a parked target's ledger matches its map. */
    dsd_state_set_trunk_chan_freq(&g_state, 16U, 451200000L);
    static dmr_t3_chan_target_snapshot saved;
    dmr_t3_chan_save(&g_state, &saved);
    assert(!bit_set(saved.missing.missing, 16U) && dmr_t3_missing_count(&g_state) == 0U);

    /* Nothing moved: a sync changes nothing. */
    const uint32_t rev = dmr_t3_missing_revision(&g_state);
    dmr_t3_missing_sync(&g_state);
    assert(dmr_t3_missing_revision(&g_state) == rev);
}

/* Review gate b-r2 C2: an edit's expected frequency is the one DMR uses for the channel (the entry over the shared
   row), not the shared row, which another protocol's writer can empty or fill under an entry. A set channel whose row
   P25 emptied and a hidden one whose row P25 filled both take an edit naming the entry's frequency; the shared row's
   value is stale for both. A channel with no entry is judged by its shared row. */
static void
test_overlay_edit_expects_the_effective_frequency(void) {
    reset();
    assert(overlay_edit(12U, DMR_T3_OVERLAY_SET, 451137500L) == DMR_T3_SET_OK);
    assert(overlay_edit(13U, DMR_T3_OVERLAY_HIDE, 0L) == DMR_T3_SET_OK);
    dsd_state_set_trunk_chan_freq(&g_state, 12U, 0L);         /* P25's IDEN invalidation empties it */
    dsd_state_set_trunk_chan_freq(&g_state, 13U, 451162500L); /* P25's frequency cache fills it */
    assert(dmr_t3_chan_effective_hz(&g_state, 12U) == 451137500L && dmr_t3_chan_effective_hz(&g_state, 13U) == 0L);
    const uint8_t user = (uint8_t)DMR_CHAN_ORIGIN_USER;
    assert(dmr_t3_chan_origin(&g_state, 12U) == DMR_CHAN_ORIGIN_USER);
    assert(dmr_t3_chan_origin(&g_state, 13U) == DMR_CHAN_ORIGIN_USER);
    const uint32_t gen = dmr_t3_chan_generation(&g_state);

    assert(dmr_t3_overlay_apply(&g_opts, &g_state, 12U, DMR_T3_OVERLAY_SET, 451150000L, 0L, user, gen, 1)
           == DMR_T3_SET_STALE);
    assert(dmr_t3_overlay_apply(&g_opts, &g_state, 13U, DMR_T3_OVERLAY_RESTORE, 0L, 451162500L, user, gen, 1)
           == DMR_T3_SET_STALE);
    assert(dmr_t3_overlay_count(&g_state) == 2U);

    assert(dmr_t3_overlay_apply(&g_opts, &g_state, 12U, DMR_T3_OVERLAY_SET, 451150000L, 451137500L, user, gen, 1)
           == DMR_T3_SET_OK);
    assert(g_state.trunk_chan_map[12] == 451150000L && dmr_t3_chan_effective_hz(&g_state, 12U) == 451150000L);
    assert(dmr_t3_overlay_apply(&g_opts, &g_state, 13U, DMR_T3_OVERLAY_RESTORE, 0L, 0L, user, gen, 1) == DMR_T3_SET_OK);
    assert(dmr_t3_overlay_get(&g_state, 13U, NULL, NULL) == 0 && dmr_t3_overlay_count(&g_state) == 1U);

    dsd_state_set_trunk_chan_freq(&g_state, 14U, 451175000L);
    assert(dmr_t3_chan_effective_hz(&g_state, 14U) == 451175000L);
    assert(dmr_t3_chan_effective_hz(&g_state, 0U) == 0L && dmr_t3_chan_effective_hz(&g_state, 4095U) == 0L);
}

/* Review gate b-r2 C3: the fill anchors on the frequency DMR uses. A hidden channel whose shared row another protocol
   filled anchors nothing (one anchor left: no estimate); a set channel whose row another protocol emptied anchors at
   the user's frequency (two anchors: the gap between them fills). */
static void
test_anchors_resolve_the_overlay(void) {
    reset();
    g_opts.dmr_t3_heuristic_fill = 1;
    assert(overlay_edit(10U, DMR_T3_OVERLAY_HIDE, 0L) == DMR_T3_SET_OK);
    dsd_state_set_trunk_chan_freq(&g_state, 10U, 450000000L);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 12U, 450025000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.trunk_chan_map[11] == 0L && dmr_t3_chan_origin(&g_state, 11U) == DMR_CHAN_ORIGIN_NONE);

    reset();
    g_opts.dmr_t3_heuristic_fill = 1;
    assert(overlay_edit(20U, DMR_T3_OVERLAY_SET, 450000000L) == DMR_T3_SET_OK);
    dsd_state_set_trunk_chan_freq(&g_state, 20U, 0L);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 22U, 450025000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.trunk_chan_map[21] == 450012500L && dmr_t3_chan_origin(&g_state, 21U) == DMR_CHAN_ORIGIN_ESTIMATE);
    assert(g_state.trunk_chan_map[20] == 0L && dmr_t3_chan_origin(&g_state, 20U) == DMR_CHAN_ORIGIN_USER);
}

/* Review gate b-r2 C4: a fill the user's edit caused (or the --dmr-chan-overrides file) has no received burst behind
   it: its notice is a system notice, with no protocol, slot or CRC flags. A fill a learned row caused stays received
   data on that burst's slot. */
static void
test_user_fill_notice_is_a_system_notice(void) {
    reset();
    g_opts.dmr_t3_heuristic_fill = 1;
    g_state.currentslot = 1;
    g_state.event_crc_invalid[1] = DSD_CRC_RAS_UNVERIFIED;
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 12U, 450000000L, DMR_CHAN_ORIGIN_SITE) == 1);
    const uint64_t before0 = pushes(0);
    uint64_t before1 = pushes(1);
    assert(overlay_edit(14U, DMR_T3_OVERLAY_SET, 450025000L) == DMR_T3_SET_OK);
    assert(g_state.trunk_chan_map[13] == 450012500L && dmr_t3_chan_origin(&g_state, 13U) == DMR_CHAN_ORIGIN_ESTIMATE);
    assert(pushes(1) == before1 && pushes(0) == before0 + 1U);
    const Event_History* row = &g_state.event_history_s[0].Event_History_Items[1];
    assert(strstr(row->event_string, "DMR TIII: Heuristic filled 1 LCNs") != NULL);
    assert(row->category == (uint8_t)DSD_EVENT_CATEGORY_SYSTEM && row->systype == DSD_SYNC_NONE);
    assert(row->crc_invalid == 0U);

    before1 = pushes(1);
    assert(dmr_t3_chan_learn(&g_opts, &g_state, 16U, 450050000L, DMR_CHAN_ORIGIN_SITE) == 1);
    assert(g_state.trunk_chan_map[15] == 450037500L && pushes(1) == before1 + 2U);
    row = &g_state.event_history_s[1].Event_History_Items[1];
    assert(strstr(row->event_string, "DMR TIII: Heuristic filled 2 LCNs") != NULL);
    assert(row->category == (uint8_t)DSD_EVENT_CATEGORY_DATA && row->crc_invalid == DSD_CRC_RAS_UNVERIFIED);
}

/* Write @p text to a new temporary file named into @p path. */
static void
write_temp_csv(char* path, size_t size, const char* text) {
    const int fd = dsd_test_mkstemp(path, size, "dmr_chan_overrides");
    assert(fd >= 0);
    const size_t len = strlen(text);
    assert(dsd_write(fd, text, len) == (ssize_t)len);
    assert(dsd_close(fd) == 0);
}

/* A file naming more channels than the overlay holds: the rows past the cap are skipped with a warning; a later row
   for a channel already taken still applies. */
static void
test_overlay_load_csv_past_the_cap(void) {
    reset_missing();
    static char text[16384];
    size_t used = 0;
    for (unsigned l = 1U; l <= (unsigned)DMR_T3_OVERLAY_MAX_ROWS + 2U; l++) {
        used += (size_t)DSD_SNPRINTF(text + used, sizeof text - used, "%u,%ld\n", l, 451000000L + (long)l * 12500L);
    }
    DSD_SNPRINTF(text + used, sizeof text - used, "1,0\n");
    char path[DSD_TEST_PATH_MAX];
    write_temp_csv(path, sizeof path, text);
    g_override_warnings = 0;
    g_override_cap_warnings = 0;
    assert(dmr_t3_overlay_load_csv(&g_opts, &g_state, path) == DMR_T3_OVERLAY_MAX_ROWS + 1);
    assert(dmr_t3_overlay_count(&g_state) == (size_t)DMR_T3_OVERLAY_MAX_ROWS);
    assert(dmr_t3_overlay_get(&g_state, 1U, NULL, NULL) == DMR_T3_OVERLAY_HIDE);
    assert(dmr_t3_overlay_get(&g_state, (uint16_t)DMR_T3_OVERLAY_MAX_ROWS, NULL, NULL) == DMR_T3_OVERLAY_SET);
    assert(dmr_t3_overlay_get(&g_state, (uint16_t)(DMR_T3_OVERLAY_MAX_ROWS + 1), NULL, NULL) == 0);
    assert(g_state.trunk_chan_map[DMR_T3_OVERLAY_MAX_ROWS + 2] == 0L);
    /* Two rows past the cap skipped by the reader itself, and the repeated channel 1 reported. */
    assert(g_override_warnings == 3 && g_override_cap_warnings == 2);
    (void)remove(path);
}

/* The --dmr-chan-overrides file: a header, set and hide rows, and every malformed row skipped with a warning. */
static void
test_overlay_load_csv_rows(void) {
    reset_missing();
    g_state.trunk_chan_map[12] = 451012500L;
    g_state.trunk_chan_map[13] = 451025000L;
    char path[DSD_TEST_PATH_MAX];
    write_temp_csv(path, sizeof path,
                   "lpcn,frequency_hz\r\n"
                   "12,451137500\r\n"
                   "13,0\n"
                   "\n"
                   " 20 , 451250000 \n"
                   "0,451000000\n"
                   "4095,451000000\n"
                   "21,99999\n"
                   "22,-451000000\n"
                   "23,2147483648\n"
                   "24,451000000,extra\n"
                   "25\n"
                   "0x1A,451000000\n"
                   "26,451.0125\n"
                   "lpcn,frequency_hz\n"
                   "20,451262500\n");
    g_override_warnings = 0;
    assert(dmr_t3_overlay_load_csv(&g_opts, &g_state, path) == 4);
    assert(g_state.trunk_chan_map[12] == 451137500L && dmr_t3_chan_origin(&g_state, 12U) == DMR_CHAN_ORIGIN_USER);
    assert(g_state.trunk_chan_map[13] == 0L && dmr_t3_overlay_get(&g_state, 13U, NULL, NULL) == DMR_T3_OVERLAY_HIDE);
    assert(g_state.trunk_chan_map[20] == 451262500L); /* given twice: the later row */
    for (uint16_t l = 21U; l <= 26U; l++) {
        assert(g_state.trunk_chan_map[l] == 0L && dmr_t3_overlay_get(&g_state, l, NULL, NULL) == 0);
    }
    assert(dmr_t3_overlay_count(&g_state) == 3U);
    /* 0, 4095, 21..25, the hex channel, 26 and the second header: ten skipped, and the repeated 20 reported. */
    assert(g_override_warnings == 11);
    (void)remove(path);

    /* No file: nothing applied. */
    assert(dmr_t3_overlay_load_csv(&g_opts, &g_state, path) == -1);
    assert(dmr_t3_overlay_count(&g_state) == 3U);
}

/* ---- Tier III identity and TSCC evidence (F3 "Use control channel frequency", B4 metrics) ---- */

/* A checked C_ALOHA heard on the carrier the receiver is on, with no call followed, names that carrier the TSCC until
   the receiver leaves it; a RAS-accepted one, one heard while a call is followed, or one with no known tuned frequency
   does not. Tier III stays known for the session. */
static void
test_cc_evidence_from_checked_aloha(void) {
    reset_missing();
    dmr_sm_init(&g_opts, &g_state);
    g_opts.audio_in_type = AUDIO_IN_RTL;
    g_opts.rtlsdr_center_freq = (uint32_t)CC_HZ;
    assert(dmr_t3_cc_confirmed_hz(&g_state) == 0 && !dmr_t3_tier3_seen(&g_state));

    g_opts.aggressive_framesync = 0;
    g_state.dmr_ras_signatures = 2U;
    g_state.event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED;
    send_aloha(0x1234U);
    assert(dmr_t3_cc_confirmed_hz(&g_state) == 0 && !dmr_t3_tier3_seen(&g_state));
    /* RAS-accepted names nothing of the site, but tells how its channel numbers show. */
    assert(dmr_t3_tier3_unchecked_seen(&g_state));
    g_state.event_crc_invalid[0] = 0U;

    g_opts.trunk_is_tuned = 1;
    send_aloha(0x1234U);
    assert(dmr_t3_cc_confirmed_hz(&g_state) == 0);
    assert(dmr_t3_tier3_seen(&g_state));
    g_opts.trunk_is_tuned = 0;

    send_aloha(0x1234U);
    assert(dmr_t3_cc_confirmed_hz(&g_state) == CC_HZ);

    /* The receiver left the carrier: the evidence is stale, the identity is not. */
    g_state.carrier_seq++;
    assert(dmr_t3_cc_confirmed_hz(&g_state) == 0);
    assert(dmr_t3_tier3_seen(&g_state));

    /* Off a radio input there is no frequency to name. */
    g_opts.audio_in_type = 0;
    send_aloha(0x1234U);
    assert(dmr_t3_cc_confirmed_hz(&g_state) == 0);
}

/* Review gate b-r2 C6: a trunk retune (to a voice channel, back, a hunt's next candidate) moves the runtime tuning
   generation, not always the carrier count: the evidence goes stale with it, and none holds while a tune is pending. */
static void
test_cc_evidence_stale_after_trunk_retune(void) {
    reset_missing();
    dmr_sm_init(&g_opts, &g_state);
    g_opts.audio_in_type = AUDIO_IN_RTL;
    g_opts.rtlsdr_center_freq = (uint32_t)CC_HZ;
    send_aloha(0x1234U);
    assert(dmr_t3_cc_confirmed_hz(&g_state) == CC_HZ);
    const uint32_t carrier = g_state.carrier_seq;
    const uint64_t request = dsd_trunk_tuning_request_begin();
    assert(dmr_t3_cc_confirmed_hz(&g_state) == 0);
    dsd_trunk_tuning_request_complete(request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_state.carrier_seq == carrier && dsd_trunk_tuning_pending_request() == 0U);
    assert(dmr_t3_cc_confirmed_hz(&g_state) == 0);
    /* Heard again on the carrier reached: confirmed there. */
    send_aloha(0x1234U);
    assert(dmr_t3_cc_confirmed_hz(&g_state) == CC_HZ);
}

/* The flag rides each trunk-scan target's snapshot; the evidence never crosses to another target. */
static void
test_tier3_and_evidence_per_target(void) {
    reset_missing();
    static dmr_t3_chan_target_snapshot t3;
    static dmr_t3_chan_target_snapshot other;
    g_opts.audio_in_type = AUDIO_IN_RTL;
    g_opts.rtlsdr_center_freq = (uint32_t)CC_HZ;
    send_aloha(0x1234U);
    assert(dmr_t3_cc_confirmed_hz(&g_state) == CC_HZ && dmr_t3_tier3_seen(&g_state));
    dmr_t3_chan_save(&g_state, &t3);
    assert(t3.tier3 == 1U);
    DSD_MEMSET(&other, 0, sizeof other);
    dmr_t3_chan_restore(&g_state, &other);
    assert(!dmr_t3_tier3_seen(&g_state) && dmr_t3_cc_confirmed_hz(&g_state) == 0);
    dmr_t3_chan_restore(&g_state, &t3);
    assert(dmr_t3_tier3_seen(&g_state));
    assert(dmr_t3_cc_confirmed_hz(&g_state) == 0); /* a retune: heard again before it is used */
}

/* A checked standard grant or C_BCAST names the site Tier III; a manufacturer's grant or a RAS-accepted one does
 * not. */
static void
test_tier3_from_checked_grant_and_bcast(void) {
    reset_missing();
    dmr_sm_init(&g_opts, &g_state);
    send_grant_fid(49U, 12U, 0U, 0x10U);
    assert(!dmr_t3_tier3_seen(&g_state));
    g_opts.aggressive_framesync = 0;
    g_state.dmr_ras_signatures = 2U;
    assert(!dmr_t3_tier3_unchecked_seen(&g_state));
    g_state.event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED;
    send_grant(49U, 12U, 0U);
    assert(!dmr_t3_tier3_seen(&g_state) && dmr_t3_tier3_unchecked_seen(&g_state));
    g_state.event_crc_invalid[0] = 0U;
    send_grant(49U, 12U, 0U);
    assert(dmr_t3_tier3_seen(&g_state));

    reset_missing();
    dmr_sm_init(&g_opts, &g_state);
    send_chan_freq(30U, 451U, 400U);
    assert(dmr_t3_tier3_seen(&g_state));
}

/* The grant path never allocates the DMR record: a checked grant for a mapped channel on a session with none (a map
   loaded at start creates none) records nothing, and the next checked C_ALOHA names the site. Once the record exists,
   a grant sets the flag. */
static void
test_mapped_grant_allocates_nothing(void) {
    reset_missing();
    dmr_sm_init(&g_opts, &g_state);
    g_state.trunk_chan_map[12] = 451150000L;
    assert(!dmr_t3_chan_recorded(&g_state));
    send_grant(49U, 12U, 0U);
    assert(!dmr_t3_chan_recorded(&g_state));
    assert(!dmr_t3_tier3_seen(&g_state));

    reset_missing();
    dmr_sm_init(&g_opts, &g_state);
    g_state.trunk_chan_map[12] = 451150000L;
    dmr_t3_chan_note_map_cleared(&g_state); /* creates the record */
    assert(dmr_t3_chan_recorded(&g_state) && !dmr_t3_tier3_seen(&g_state));
    send_grant(49U, 12U, 0U);
    assert(dmr_t3_tier3_seen(&g_state));
}

/* The RAS-only flag rides each target's snapshot as the checked one does. */
static void
test_unchecked_tier3_per_target(void) {
    reset_missing();
    static dmr_t3_chan_target_snapshot ras;
    static dmr_t3_chan_target_snapshot none;
    dmr_t3_note_tier3(&g_state, 0);
    dmr_t3_chan_save(&g_state, &ras);
    assert(ras.tier3_unchecked == 1U && ras.tier3 == 0U);
    DSD_MEMSET(&none, 0, sizeof none);
    dmr_t3_chan_restore(&g_state, &none);
    assert(!dmr_t3_tier3_unchecked_seen(&g_state));
    dmr_t3_chan_restore(&g_state, &ras);
    assert(dmr_t3_tier3_unchecked_seen(&g_state) && !dmr_t3_tier3_seen(&g_state));
}

int
main(void) {
    dsd_neo_log_set_tap(missing_log_tap, NULL);
    test_site_overrides_estimate_and_unverified();
    test_file_and_user_never_overwritten_ota();
    test_site_change_notice_on_new_hz();
    test_identical_hz_origin_promotion_bumps_revision();
    test_ras_unverified_learn_fills_empty_only();
    test_neighbour_never_trust2();
    test_estimates_dropped_when_anchor_changes();
    test_network_change_drops_ota_rows_keeps_file_user();
    test_estimate_confirmed_in_place();
    test_foreign_write_disowns_row();
    test_learned_notice_on_receiving_slot();
    test_cc_vc_cc_trip_keeps_learned_rows();
    test_other_site_same_network_keeps_rows();
    test_save_restore_round_trip();
    test_mbc_header_then_other_same_slot_burst_disarms();
    test_companion_slot_traffic_does_not_disarm();
    test_mbc_missed_same_slot_bursts_disarm();
    test_single_csbk_0xfff_is_malformed();
    test_continuation_csbko_mismatch_rejected();
    test_four_continuations_rejected();
    test_abs_rx_outside_table_c7_no_frequency();
    test_abs_rx_valid_chan_zero_tunes_without_learning();
    test_absolute_grant_two_block_mbc_still_follows();
    test_malformed_mbc_no_cc_heartbeat();
    test_header_only_mbc_refreshes_nothing();
    test_scan_activity_only_from_accepted_control();
    test_c_bcast_bad_abs_params_no_effect();
    test_chan_freq_header_fff_uses_bcap_chan();
    test_unmapped_grant_recorded_once_per_channel();
    test_count_exact_past_16_details();
    test_mapped_grant_records_nothing();
    test_ras_grant_ledger_unverified();
    test_synthesized_and_ms_data_56_ignored();
    test_reconcile_clears_only_mapped_bits();
    test_missing_network_change_resets();
    test_overlay_set_hide_restore_live();
    test_overlay_restore_gives_the_air_its_row();
    test_overlay_stale_expectations_refused();
    test_overlay_refused_under_trunk_scan();
    test_overlay_reapplied_after_reload();
    test_overlay_network_change_keeps_user_rows();
    test_overlay_cleared_by_target_restore();
    test_overlay_bounded();
    test_overlay_load_csv_past_the_cap();
    test_hide_followed_channel_keeps_call();
    test_overlay_hidden_channel_leaves_ledger();
    test_overlay_governs_grants_over_the_shared_map();
    test_hidden_channel_anchors_no_estimate();
    test_missing_reconciled_after_other_writers();
    test_overlay_edit_expects_the_effective_frequency();
    test_anchors_resolve_the_overlay();
    test_user_fill_notice_is_a_system_notice();
    test_overlay_load_csv_rows();
    test_chan_freq_inconsistent_channels_ignored();
    test_chan_freq_learned_notice_and_origin();
    test_chan_freq_user_list_not_appended();
    test_announced_candidates_need_checked_content();
    test_vote_now_ac_unavailable_still_decodes_channel();
    test_adjacent_absolute_is_neighbour_fill_only();
    test_c_bcast_system_code_is_the_network_baseline();
    test_unfinished_mbc_headers_note_no_lock_evidence();
    test_logical_channel_mbc_is_malformed();
    test_chan_freq_contradiction_no_effect();
    test_single_c_bcast_reads_no_continuation();
    test_logical_vote_now_adjacent_mbc_is_malformed();
    test_single_c_bcast_without_last_block_is_malformed();
    test_p_clear_mbc_on_lpcn_zero_is_malformed();
    test_foreign_takeover_drops_trust_and_moves_revision();
    test_save_restore_keeps_effective_revision();
    test_cc_evidence_from_checked_aloha();
    test_cc_evidence_stale_after_trunk_retune();
    test_tier3_and_evidence_per_target();
    test_tier3_from_checked_grant_and_bcast();
    test_mapped_grant_allocates_nothing();
    test_unchecked_tier3_per_target();
    dsd_state_ext_free_all(&g_state);
    printf("DMR_T3_CHANNELS: OK\n");
    return 0;
}
