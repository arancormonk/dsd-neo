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
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dmr_abs_params.h"
#include "dmr_bptc_test_encoder.h"
#include "dmr_t3_channels_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

extern void dmr_cspdu(dsd_opts*, dsd_state*, uint8_t*, uint8_t*, uint32_t, uint32_t);

static const long CC_HZ = 451000000L;

static dsd_opts g_opts;
static dsd_state g_state;
static Event_History_I g_history[2];

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
    const dmr_cspdu_shape shape = {1U, 1U, (uint8_t)(opcode & 0x3FU), (uint8_t)(opcode & 0x3FU), 1U};
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

    (void)return_request(&g_opts, &g_state, 0U);
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
   CHAN, learned and appended to the hunt list as any other. */
static void
test_chan_freq_header_fff_uses_bcap_chan(void) {
    reset();
    dmr_sm_init(&g_opts, &g_state);
    send_c_bcast_mbc(5U, 0x0FFFU, 130U, 451U, 1000U, 0U);
    assert(g_state.trunk_chan_map[130] == 451125000L);
    assert(dmr_t3_chan_origin(&g_state, 130U) == DMR_CHAN_ORIGIN_SITE);
    assert(g_state.trunk_chan_map[0xFFF] == 0L);
    assert(hunt_list_count(451125000L) == 1);
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

/* A checked Chan_Freq is a SITE row with its learned notice; announced again it changes nothing and the hunt list
   holds its frequency once. A user's list is never appended to. */
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
    assert(hunt_list_count(451187500L) == 1 && g_state.lcn_freq_count == 1);
    assert(candidate_count(451187500L) == 1);
    dsd_state_trunk_lcn_free(&g_state);
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

static void
send_burst(uint8_t slot, const uint8_t bits[96], uint8_t databurst) {
    uint8_t info[196];
    g_state.currentslot = slot;
    dmr_test_encode_bptc_196x96(bits, k_no_reserved, info);
    dmr_data_burst_handler(&g_opts, &g_state, info, databurst, NULL);
}

/* A CSBK (@p mbc_header 0: LB 1, mask 0xA5A5, data type 3) or an MBC header (LB 0, mask 0xAAAA, data type 4) of
   opcode @p csbko naming channel @p chan (bits 16..27), talkgroup 1001 and source 2002. */
static void
send_csbk_or_header(uint8_t slot, uint8_t csbko, uint16_t chan, int mbc_header) {
    uint8_t bytes[12] = {0};
    uint8_t bits[96];
    bytes[0] = (uint8_t)((mbc_header ? 0x00U : 0x80U) | (csbko & 0x3FU));
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

/* The other slot's bursts between the header and its continuation leave the assembly alone. */
static void
test_companion_slot_traffic_does_not_disarm(void) {
    reset_bursts();
    uint8_t block[1][96];
    uint8_t idle[96];
    DSD_MEMSET(idle, 0, sizeof idle);
    send_csbk_or_header(0, 49U, 0x0FFFU, 1);
    send_csbk_or_header(1, 25U, 0U, 0);
    send_burst(1, idle, 0x09U);
    build_abs_block(block[0], 49U, 121U, 451U, 200U);
    send_continuations(0, block, 1U, 0);
    assert(g_state.trunk_vc_freq[0] == 451025000L);
    assert(g_state.trunk_chan_map[121] == 451025000L);
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

/* A malformed MBC has no side effect at all: no control channel heartbeat, no anchor, nothing published. */
static void
test_malformed_mbc_no_cc_heartbeat(void) {
    reset_bursts();
    g_state.trunk_cc_freq = 0L;
    g_opts.audio_in_type = AUDIO_IN_RTL;
    g_opts.rtlsdr_center_freq = 451500000U;
    g_opts.dmr_crc_relaxed_default = 1;
    dsd_decode_clock_test_set_ns(UINT64_C(5000) * UINT64_C(1000000000));
    g_state.last_cc_sync_time = 0;
    g_state.last_cc_sync_time_m = 0.0;
    send_abs_grant_mbc(0, 0U, 127U, 451U, 800U);
    assert(g_state.last_cc_sync_time == 0);
    assert(g_state.last_cc_sync_time_m < 0.5 && g_state.last_cc_sync_time_m > -0.5);
    assert(g_state.trunk_cc_freq == 0L);
    assert(activity_rows() == 0);
    assert(g_state.trunk_chan_map[127] == 0L);

    /* The same MBC well formed does keep the control channel warm and anchors it. */
    send_abs_grant_mbc(0, 49U, 127U, 451U, 800U);
    assert(g_state.last_cc_sync_time_m > 1.0);
    assert(g_state.trunk_cc_freq == 451500000L);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

int
main(void) {
    test_site_overrides_estimate_and_unverified();
    test_file_and_user_never_overwritten_ota();
    test_site_change_notice_on_new_hz();
    test_identical_hz_origin_promotion_bumps_revision();
    test_ras_unverified_learn_fills_empty_only();
    test_neighbour_never_trust2();
    test_estimates_dropped_when_anchor_changes();
    test_network_change_drops_ota_rows_keeps_file_user();
    test_learned_notice_on_receiving_slot();
    test_cc_vc_cc_trip_keeps_learned_rows();
    test_other_site_same_network_keeps_rows();
    test_save_restore_round_trip();
    test_mbc_header_then_other_same_slot_burst_disarms();
    test_companion_slot_traffic_does_not_disarm();
    test_single_csbk_0xfff_is_malformed();
    test_continuation_csbko_mismatch_rejected();
    test_four_continuations_rejected();
    test_abs_rx_outside_table_c7_no_frequency();
    test_abs_rx_valid_chan_zero_tunes_without_learning();
    test_absolute_grant_two_block_mbc_still_follows();
    test_malformed_mbc_no_cc_heartbeat();
    test_chan_freq_header_fff_uses_bcap_chan();
    test_chan_freq_inconsistent_channels_ignored();
    test_chan_freq_learned_notice_and_origin();
    test_vote_now_ac_unavailable_still_decodes_channel();
    test_adjacent_absolute_is_neighbour_fill_only();
    dsd_state_ext_free_all(&g_state);
    printf("DMR_T3_CHANNELS: OK\n");
    return 0;
}
