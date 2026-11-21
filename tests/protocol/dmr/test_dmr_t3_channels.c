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
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static void
send_aloha(uint16_t syscode) {
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pf0(bits, bytes, 25U);
    put_bits(bits, 40, syscode & 0x3FFFU, 14);
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1U, 0U);
}

/* C_BCAST Chan_Freq (type 5) for @p channel. */
static void
send_chan_freq(uint16_t channel, uint16_t rx_int, uint16_t rx_step) {
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pf0(bits, bytes, 40U);
    put_bits(bits, 16, 5U, 5);
    put_bits(bits, 68, channel & 0x0FFFU, 12);
    put_bits(bits, 112, 0U, 4);
    put_bits(bits, 118, channel & 0x0FFFU, 12);
    put_bits(bits, 130, rx_int & 0x03FFU, 10);
    put_bits(bits, 140, rx_step & 0x1FFFU, 13);
    put_bits(bits, 153, rx_int & 0x03FFU, 10);
    put_bits(bits, 163, rx_step & 0x1FFFU, 13);
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1U, 0U);
}

/* C_BCAST Adjacent_Site (type 6) with absolute parameters for @p channel. */
static void
send_adjacent(uint16_t channel, uint16_t rx_int, uint16_t rx_step) {
    uint8_t bits[256];
    uint8_t bytes[48];
    build_pf0(bits, bytes, 40U);
    put_bits(bits, 16, 6U, 5);
    bits[56] = 1U;
    put_bits(bits, 68, 0x0FFFU, 12);
    put_bits(bits, 112, 0U, 4);
    put_bits(bits, 118, channel & 0x0FFFU, 12);
    put_bits(bits, 130, rx_int & 0x03FFU, 10);
    put_bits(bits, 140, rx_step & 0x1FFFU, 13);
    put_bits(bits, 153, rx_int & 0x03FFU, 10);
    put_bits(bits, 163, rx_step & 0x1FFFU, 13);
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1U, 0U);
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
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1U, 0U);
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
    dsd_state_ext_free_all(&g_state);
    printf("DMR_T3_CHANNELS: OK\n");
    return 0;
}
