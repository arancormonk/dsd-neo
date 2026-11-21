// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Simulate grant → C_MOVE → P_CLEAR and verify return-to-CC. C_MOVE is Move TSCC (TS 102 361-4 Table 7.17), which
 * tells one MS to move to another control channel: the followed call stays where it is (carrier, burst indicators),
 * and the P_CLEAR on its slot still releases it.
 */

#include <dsd-neo/protocol/dmr/dmr_utils_api.h>

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dmr_abs_params.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// --- Stubs for external symbols referenced by dmr_csbk.c ---

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

static dsd_trunk_tune_result
test_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)ted_sps;
    if (!opts || !state || freq <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    opts->trunk_is_tuned = 1;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
test_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    if (opts) {
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

// CRC8 helper used in CSBK path (provide a trivial non-zero value)
uint8_t
crc8(uint8_t bits[], unsigned int len) {
    (void)bits;
    (void)len;
    return 0xFF;
}

// --- Helpers ---
static void
init_env(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_hangtime = 0.0f;      // immediate release on clear
    state->trunk_cc_freq = 851000000; // mock CC
}

// Compose a Move TSCC (Table 7.17): Physical Channel Number 0xFFF (bits 44..55), MS address (bits 56..79), and its
// MV_AP block (Table 7.18) naming the TSCC: CHAN @p apcn on @p rx_int_mhz plus @p rx_step_125hz steps.
static void
build_move_tscc(uint8_t* bits, uint8_t* bytes, uint32_t ms, uint16_t apcn, uint16_t rx_int_mhz,
                uint16_t rx_step_125hz) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = (uint8_t)(57 & 0x3F);
    for (int i = 0; i < 12; i++) {
        bits[44 + i] = ((0xFFFu >> (11 - i)) & 1u);
    }
    for (int i = 0; i < 24; i++) {
        bits[56 + i] = (uint8_t)((ms >> (23 - i)) & 1u);
    }
    // MBC CDEFTYPE (bits 112..115) = 0 (absolute channel parms)
    for (int i = 0; i < 12; i++) {
        bits[118 + i] = ((apcn >> (11 - i)) & 1u);
    }
    for (int i = 0; i < 10; i++) {
        bits[153 + i] = ((rx_int_mhz >> (9 - i)) & 1u);
    }
    for (int i = 0; i < 13; i++) {
        bits[163 + i] = ((rx_step_125hz >> (12 - i)) & 1u);
    }
}

/* A P_CLEAR (TS 102 361-4 Table 7.30) to group @p tg: G/I bit 31, target bits 32-55. */
static void
build_pclear(uint8_t* bits, uint8_t* bytes, uint32_t tg) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    // Opcode = 46
    bytes[0] = (uint8_t)(46 & 0x3F);
    bits[31] = 1U;
    for (int i = 0; i < 24; i++) {
        bits[32 + i] = (uint8_t)((tg >> (23 - i)) & 1U);
    }
}

extern void dmr_cspdu(dsd_opts*, dsd_state*, uint8_t*, uint8_t*, uint32_t, uint32_t);

/* The PDU in @p bits / @p bytes as the assembler hands over a two-block MBC: header LB 0, one continuation carrying
   LB 1 and the header's CSBKO (TS 102 361-4 CG_AP/MV_AP/BC_AP). */
static void
cspdu_mbc(dsd_opts* opts, dsd_state* state, uint8_t* bits, uint8_t* bytes) {
    const uint8_t csbko = (uint8_t)(bytes[0] & 0x3FU);
    bytes[0] = csbko;
    bytes[12] = (uint8_t)(0x80U | csbko);
    bits[0] = 0U;
    bits[96] = 1U;
    for (int i = 0; i < 6; i++) {
        bits[98 + i] = (uint8_t)((csbko >> (5 - i)) & 1U);
    }
    const dmr_cspdu_shape shape = {1U, 1U, csbko, csbko, 1U};
    dmr_cspdu_ex(opts, state, bits, bytes, 1U, 0U, &shape);
}

int
main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    static dsd_opts opts;
    static dsd_state state;
    init_env(&opts, &state);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_to_freq,
        .return_to_cc_request = test_return_to_cc,
    });

    // Step 1: tune to initial VC via SM grant (pretend TS2 voice ongoing)
    long f1 = 852000000;
    dmr_sm_emit_group_grant(&opts, &state, f1, /*lpcn*/ 0x0010, /*tg*/ 1001, /*src*/ 222);
    assert(opts.trunk_is_tuned == 1);
    state.currentslot = 1; // slot 2 context in data path
    state.dmrburstR = 16;  // voice on TS2
    state.dmrburstL = 9;   // idle on TS1

    // Step 2: a Move TSCC for some MS names another TSCC (453.500000 MHz): the channel map learns it, and the followed
    // call on TS2 keeps its carrier and burst indicators.
    uint8_t bits[256];
    uint8_t bytes[48];
    const uint16_t apcn = 0x0456;
    const long tscc = 453000000 + 4000 * 125;
    build_move_tscc(bits, bytes, /*ms*/ 4321, apcn, /*rx_int*/ 453, /*rx_step*/ 4000);
    cspdu_mbc(&opts, &state, bits, bytes);
    assert(opts.trunk_is_tuned == 1);
    assert(state.trunk_vc_freq[0] == f1);
    assert(state.trunk_chan_map[apcn] == tscc);
    assert(state.dmrburstR == 16);
    assert(state.dmrburstL == 9);

    // Step 3: P_CLEAR on the followed call's slot, then SM should return to CC immediately
    state.currentslot = 1;
    build_pclear(bits, bytes, 1001U);
    dmr_cspdu(&opts, &state, bits, bytes, /*crc ok*/ 1, /*errs*/ 0);

    assert(opts.trunk_is_tuned == 0);
    assert(state.trunk_vc_freq[0] == 0);
    assert(state.trunk_vc_freq[1] == 0);

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    printf("DMR_T3_MOVE_RELEASE: OK\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
