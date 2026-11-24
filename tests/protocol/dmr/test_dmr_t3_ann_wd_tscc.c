// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Ann_WD_TSCC switches while already on the CC must keep the return-to-CC
 * cleanup semantics, and deferred switches must not advance the CC anchor.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dmr_abs_params.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

extern void dmr_cspdu(dsd_opts*, dsd_state*, uint8_t*, uint8_t*, uint32_t, uint32_t);

static int g_return_to_cc_calls = 0;
static int g_dmr_reset_blocks_calls = 0;
static dsd_trunk_tune_result g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

void
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

int
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    (void)opts;
    (void)state;
    (void)observation->ota_source_id;
    (void)observation->ota_target_id;
    (void)notice;
    (void)slot;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_dmr_reset_blocks_calls++;
}

uint8_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
crc8(uint8_t bits[], unsigned int len) {
    (void)bits;
    (void)len;
    return 0xFF;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_drain_audio_output(dsd_opts* opts) {
    (void)opts;
}

static dsd_trunk_tune_result
test_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    g_return_to_cc_calls++;
    if (g_return_to_cc_result != DSD_TRUNK_TUNE_RESULT_OK) {
        return g_return_to_cc_result;
    }

    if (opts) {
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->trunk_vc_freq[0] = 0;
        state->trunk_vc_freq[1] = 0;
        (void)dsd_call_state_end(state, 0U, 0.0);
        (void)dsd_call_state_end(state, 1U, 0.0);
    }
    dmr_reset_blocks(opts, state);
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
init_env(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 0;
    state->trunk_cc_freq = 851000000L;
    dmr_sm_init(opts, state);
}

static void
write_bits_u32(uint8_t* bits, size_t start, uint32_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        size_t shift = (nbits - 1U) - i;
        bits[start + i] = (uint8_t)((value >> shift) & 1U);
    }
}

static void
build_ann_wd_tscc(uint8_t* bits, uint8_t* bytes, uint16_t ch1, uint16_t ch2, uint8_t ch1_flag, uint8_t ch2_flag) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = (uint8_t)(0x80U | 40U); /* C_BCAST, a single block: LB 1 (Table 7.20) */
    bits[0] = 1U;
    write_bits_u32(bits, 25U, 1U, 4U);
    write_bits_u32(bits, 29U, 2U, 4U);
    bits[33] = (uint8_t)(ch1_flag & 1U);
    bits[34] = (uint8_t)(ch2_flag & 1U);
    write_bits_u32(bits, 56U, ch1 & 0x0FFFU, 12U);
    write_bits_u32(bits, 68U, ch2 & 0x0FFFU, 12U);
}

/* The MBC form (TS 102 361-4 §7.2.19.1): the header (BCAST_CH1 @p bcast_ch1, AW_FLAG1 @p aw_flag1) and one BC_AP
   naming CHAN @p chan at @p rx_mhz plus @p rx_step 125 Hz steps (TX the same), as the assembler hands them over. */
static void
send_ann_wd_mbc(dsd_opts* opts, dsd_state* state, uint16_t bcast_ch1, uint8_t aw_flag1, uint16_t chan, uint16_t rx_mhz,
                uint16_t rx_step) {
    uint8_t bits[256];
    uint8_t bytes[48];
    build_ann_wd_tscc(bits, bytes, bcast_ch1, 0U, aw_flag1, 0U);
    bits[0] = 0U; /* the header is no last block */
    bytes[0] = 40U;
    write_bits_u32(bits, 2U, 40U, 6U);
    bits[96] = 1U;
    write_bits_u32(bits, 98U, 40U, 6U);
    bytes[12] = (uint8_t)(0x80U | 40U);
    write_bits_u32(bits, 118U, chan & 0x0FFFU, 12U);
    write_bits_u32(bits, 130U, rx_mhz & 0x03FFU, 10U);
    write_bits_u32(bits, 140U, rx_step & 0x1FFFU, 13U);
    write_bits_u32(bits, 153U, rx_mhz & 0x03FFU, 10U);
    write_bits_u32(bits, 163U, rx_step & 0x1FFFU, 13U);
    const dmr_cspdu_shape shape = {1U, 1U, 40U, 40U, 1U, 0U};
    dmr_cspdu_ex(opts, state, bits, bytes, 1U, 0U, &shape);
}

static int
candidate_present(const dsd_state* state, long freq) {
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    for (int i = 0; cc != NULL && i < cc->count; i++) {
        if (cc->candidates[i] == freq) {
            return 1;
        }
    }
    return 0;
}

static int
candidates_count(const dsd_state* state) {
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    return cc != NULL ? cc->count : 0;
}

/* The MBC form announces one TSCC whatever BCAST_CH1 holds: its BC_AP teaches CHAN its frequency (SITE) and, announced,
   the TSCC is a hunt candidate. */
static int
test_ann_wd_absolute_learns_and_adds(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    init_env(&opts, &state);
    send_ann_wd_mbc(&opts, &state, 0x0FFFU, 0U, 20U, 853U, 4000U);
    rc |= expect_true("ann_wd absolute: CHAN learned", state.trunk_chan_map[20] == 853500000L);
    rc |= expect_true("ann_wd absolute: SITE origin", dmr_t3_chan_origin(&state, 20U) == DMR_CHAN_ORIGIN_SITE);
    rc |= expect_true("ann_wd absolute: candidate added", candidate_present(&state, 853500000L));
    rc |= expect_true("ann_wd absolute: no switch", state.trunk_cc_freq == 851000000L);

    send_ann_wd_mbc(&opts, &state, 5U, 0U, 21U, 853U, 4100U);
    rc |= expect_true("ann_wd absolute: BCAST_CH1 ignored", state.trunk_chan_map[21] == 853512500L);
    rc |= expect_true("ann_wd absolute: BCAST_CH1 not learned", state.trunk_chan_map[5] == 0L);
    rc |= expect_true("ann_wd absolute: second candidate", candidate_present(&state, 853512500L));

    /* A BC_AP with a frequency outside Table C.7 is no TSCC. */
    send_ann_wd_mbc(&opts, &state, 0x0FFFU, 0U, 22U, 1000U, 0U);
    rc |= expect_true("ann_wd absolute: bad frequency ignored", state.trunk_chan_map[22] == 0L);
    rc |= expect_true("ann_wd absolute: bad frequency no candidate", candidates_count(&state) == 2);
    dsd_state_ext_free_all(&state);
    return rc;
}

/* AW_FLAG 1 withdraws a TSCC from the hunt list: it is never added, in either form. */
static int
test_ann_wd_withdraw_not_added(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[256];
    uint8_t bytes[48];
    int rc = 0;

    init_env(&opts, &state);
    state.trunk_chan_map[10] = 852000000L;
    state.trunk_chan_map[11] = 852500000L;
    build_ann_wd_tscc(bits, bytes, 10U, 11U, 1U, 1U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |= expect_true("ann_wd logical: both withdrawn, none added", candidates_count(&state) == 0);
    build_ann_wd_tscc(bits, bytes, 10U, 11U, 0U, 1U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |= expect_true("ann_wd logical: CH1 added", candidate_present(&state, 852000000L));
    rc |= expect_true("ann_wd logical: CH2 withdrawn", !candidate_present(&state, 852500000L));
    dsd_state_ext_free_all(&state);

    init_env(&opts, &state);
    state.trunk_chan_map[10] = 852000000L;
    state.trunk_chan_map[11] = 852500000L;
    build_ann_wd_tscc(bits, bytes, 10U, 11U, 1U, 0U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |= expect_true("ann_wd logical: CH1 withdrawn", !candidate_present(&state, 852000000L));
    rc |= expect_true("ann_wd logical: CH2 added", candidate_present(&state, 852500000L));
    dsd_state_ext_free_all(&state);

    init_env(&opts, &state);
    send_ann_wd_mbc(&opts, &state, 0x0FFFU, 1U, 23U, 853U, 4200U);
    rc |= expect_true("ann_wd absolute withdraw: no candidate", candidates_count(&state) == 0);
    rc |= expect_true("ann_wd absolute withdraw: nothing learned", state.trunk_chan_map[23] == 0L);

    /* A withdrawal may name a frequency the channel no longer has: the newer mapping stays. */
    send_ann_wd_mbc(&opts, &state, 0x0FFFU, 0U, 24U, 853U, 4400U);
    rc |= expect_true("ann_wd absolute add: learned", state.trunk_chan_map[24] == 853550000L);
    send_ann_wd_mbc(&opts, &state, 0x0FFFU, 1U, 24U, 853U, 4800U);
    rc |= expect_true("ann_wd absolute withdraw: newer mapping kept", state.trunk_chan_map[24] == 853550000L);
    dsd_state_ext_free_all(&state);
    return rc;
}

/* The single-block form names TSCCs as logical channels only: one naming the absolute escape as BCAST_CH1 or
   BCAST_CH2 lacks the block that frequency needs, and the whole announcement is refused before any effect (no control
   channel heartbeat, no candidate for the other half). */
static int
test_ann_wd_single_block_escape_refused(void) {
    static dsd_opts opts;
    static dsd_state state;
    static const uint16_t pairs[2][2] = {{0x0FFFU, 11U}, {11U, 0x0FFFU}};
    uint8_t bits[256];
    uint8_t bytes[48];
    int rc = 0;

    for (size_t i = 0; i < 2U; i++) {
        init_env(&opts, &state);
        state.trunk_chan_map[11] = 852500000L;
        build_ann_wd_tscc(bits, bytes, pairs[i][0], pairs[i][1], 0U, 0U);
        dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
        rc |= expect_true("ann_wd single escape: no heartbeat", state.last_cc_sync_time == 0);
        rc |= expect_true("ann_wd single escape: no candidate", candidates_count(&state) == 0);
        dsd_state_ext_free_all(&state);
    }

    /* Both halves logical: accepted. */
    init_env(&opts, &state);
    state.trunk_chan_map[11] = 852500000L;
    build_ann_wd_tscc(bits, bytes, 0U, 11U, 0U, 0U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |= expect_true("ann_wd single logical: heartbeat", state.last_cc_sync_time != 0);
    rc |= expect_true("ann_wd single logical: candidate", candidate_present(&state, 852500000L));
    dsd_state_ext_free_all(&state);
    return rc;
}

static void
build_t3_grant(uint8_t* bits, uint8_t* bytes, uint8_t opcode, uint16_t lpcn, uint8_t slot, uint32_t target,
               uint32_t source) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = (uint8_t)(opcode & 0x3FU);
    write_bits_u32(bits, 16U, lpcn & 0x0FFFU, 12U);
    bits[28] = (uint8_t)(slot & 1U);
    write_bits_u32(bits, 32U, target & 0x00FFFFFFU, 24U);
    write_bits_u32(bits, 56U, source & 0x00FFFFFFU, 24U);
}

static void
build_c_aloha_small(uint8_t* bits, uint8_t* bytes, uint16_t net, uint16_t site) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = 25U; /* C_ALOHA_SYS_PARMS */
    write_bits_u32(bits, 40U, 1U, 2U);
    write_bits_u32(bits, 42U, net & 0x007FU, 7U);
    write_bits_u32(bits, 49U, site & 0x001FU, 5U);
    write_bits_u32(bits, 54U, 2U, 2U);
}

static int
expect_recent_activity(const char* tag, const dsd_state* state, uint8_t index, dsd_call_kind kind,
                       const char* expected) {
    dsd_recent_activity_snapshot recent;
    if (dsd_recent_activity_copy_snapshot(state, &recent) <= 0 || recent.entries[index].observation.kind != kind
        || strstr(recent.entries[index].notice, expected) == NULL) {
        DSD_FPRINTF(stderr, "FAIL: %s: missing structured activity '%s'\n", tag, expected);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[256];
    uint8_t bytes[48];
    const uint16_t old_ch = 10U;
    const uint16_t next_ch = 11U;
    const long old_cc = 851000000L;
    const long next_cc = 852000000L;

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .return_to_cc_request = test_return_to_cc,
    });

    init_env(&opts, &state);
    state.trunk_chan_map[old_ch] = old_cc;
    state.trunk_chan_map[next_ch] = next_cc;
    state.trunk_vc_freq[0] = 853000000L;
    state.trunk_vc_freq[1] = 853000000L;
    dsd_call_observation active_call = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 1001U,
        .policy_target_id = 1001U,
        .ota_source_id = 2002U,
    };
    rc |=
        expect_true("seed active DMR call", dsd_call_state_observe(&state, &active_call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    g_return_to_cc_calls = 0;
    g_dmr_reset_blocks_calls = 0;
    g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;

    build_ann_wd_tscc(bits, bytes, old_ch, next_ch, 1U, 0U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |= expect_true("tscc switch uses return-to-cc hook", g_return_to_cc_calls == 1);
    rc |= expect_true("tscc switch resets DMR blocks", g_dmr_reset_blocks_calls == 1);
    rc |= expect_true("tscc switch advances CC", state.trunk_cc_freq == next_cc);
    rc |= expect_true("tscc switch clears stale VC", state.trunk_vc_freq[0] == 0 && state.trunk_vc_freq[1] == 0);
    dsd_call_snapshot ended_call;
    rc |= expect_true("tscc switch ends canonical call",
                      dsd_call_state_get(&state, 0U, &ended_call) == 1 && ended_call.phase == DSD_CALL_PHASE_ENDED);

    dsd_state_ext_free_all(&state);
    init_env(&opts, &state);
    state.trunk_chan_map[old_ch] = old_cc;
    state.trunk_chan_map[next_ch] = next_cc;
    g_return_to_cc_calls = 0;
    g_dmr_reset_blocks_calls = 0;
    g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;

    build_ann_wd_tscc(bits, bytes, old_ch, next_ch, 1U, 0U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |= expect_true("deferred tscc switch calls return-to-cc", g_return_to_cc_calls == 1);
    rc |= expect_true("deferred tscc switch does not reset", g_dmr_reset_blocks_calls == 0);
    rc |= expect_true("deferred tscc switch restores old CC", state.trunk_cc_freq == old_cc);

    dsd_state_ext_free_all(&state);
    init_env(&opts, &state);
    build_t3_grant(bits, bytes, 49U, 0x0123U, 0U, 1001U, 2002U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |=
        expect_recent_activity("group grant activity kind", &state, 0U, DSD_CALL_KIND_GROUP_VOICE, "Active Group Ch:");

    dsd_state_ext_free_all(&state);
    init_env(&opts, &state);
    build_t3_grant(bits, bytes, 52U, 0x0124U, 1U, 1002U, 2003U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |= expect_recent_activity("data grant activity kind", &state, 1U, DSD_CALL_KIND_DATA, "Active Data Ch:");

    dsd_state_ext_free_all(&state);
    init_env(&opts, &state);
    build_t3_grant(bits, bytes, 48U, 0x0125U, 0U, 1003U, 2004U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |= expect_recent_activity("private grant activity kind", &state, 0U, DSD_CALL_KIND_PRIVATE_VOICE,
                                 "Active Private Ch:");

    dsd_state_ext_free_all(&state);
    init_env(&opts, &state);
    build_c_aloha_small(bits, bytes, 2U, 27U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    /* No -D: the site is shown whole, network and site one-based (issue #799 D1, D2). */
    rc |= expect_true("C_ALOHA shows no split without -D", strcmp(state.dmr_site_parms, "TIII Small:3-28;105B; ") == 0);

    dsd_state_ext_free_all(&state);
    init_env(&opts, &state);
    opts.dmr_dmrla_is_set = 1;
    opts.dmr_dmrla_n = 0;
    build_c_aloha_small(bits, bytes, 2U, 27U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |= expect_true("C_ALOHA -D 0 shows no split", strcmp(state.dmr_site_parms, "TIII Small:3-28;105B; ") == 0);

    /* -D 2: the top two SITE bits (11 of 11011) are the area, the other three (011) the site in it. */
    dsd_state_ext_free_all(&state);
    init_env(&opts, &state);
    opts.dmr_dmrla_is_set = 1;
    opts.dmr_dmrla_n = 2;
    build_c_aloha_small(bits, bytes, 2U, 27U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |= expect_true("C_ALOHA -D 2 splits the top bits", strcmp(state.dmr_site_parms, "TIII Small:3-4.4;105B; ") == 0);

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_state_ext_free_all(&state);
    rc |= test_ann_wd_absolute_learns_and_adds();
    rc |= test_ann_wd_withdraw_not_added();
    rc |= test_ann_wd_single_block_escape_refused();
    if (rc == 0) {
        printf("DMR_T3_ANN_WD_TSCC: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
