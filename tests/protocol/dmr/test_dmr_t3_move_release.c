// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Simulate grant → C_MOVE → P_CLEAR and verify return-to-CC. A C_MOVE (TS 102 361-4 Table 7.17) moves radios to
 * another TSCC and names no call, so the followed call stays where it is, burst state included, and its P_CLEAR still
 * releases it.
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

static void
put_bits(uint8_t* bits, size_t start, uint32_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        bits[start + i] = (uint8_t)((value >> ((nbits - 1U) - i)) & 1U);
    }
}

/* An MBC C_MOVE (Table 7.17) with the absolute escape (PCN 0xFFF) and its MV_AP block (Table 7.18): Mask 0, so every
   radio, to the TSCC on @p rx_int MHz plus @p rx_step 125 Hz steps, CdefParms LPCN @p apcn. */
static void
build_cmove_mbc(uint8_t* bits, uint8_t* bytes, uint16_t apcn, uint16_t rx_int_mhz, uint16_t rx_step_125hz) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = 57U; // LB 0: an MBC header
    put_bits(bits, 0U, bytes[0], 8U);
    put_bits(bits, 36U, 2U, 4U);      // Backoff
    put_bits(bits, 44U, 0xFFFU, 12U); // PCN: the absolute escape
    put_bits(bits, 96U, 0x80U | 57U, 8U);
    put_bits(bits, 118U, apcn, 12U);
    put_bits(bits, 153U, rx_int_mhz, 10U);
    put_bits(bits, 163U, rx_step_125hz, 13U);
}

static void
build_pclear(uint8_t* bits, uint8_t* bytes) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    // Opcode = 46
    bytes[0] = (uint8_t)(46 & 0x3F);
}

extern void dmr_cspdu(dsd_opts*, dsd_state*, uint8_t*, uint8_t*, uint32_t, uint32_t);

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

    // Step 2: a C_MOVE to a TSCC on another frequency (853.500000 MHz) leaves the followed call alone.
    uint8_t bits[256];
    uint8_t bytes[48];
    const unsigned int tunes = state.p25_sm_tune_count;
    build_cmove_mbc(bits, bytes, 0x0456, 853, 4000);
    dmr_cspdu(&opts, &state, bits, bytes, /*crc ok*/ 1, /*errs*/ 0);
    assert(opts.trunk_is_tuned == 1);
    assert(state.trunk_vc_freq[0] == f1);
    assert(state.p25_sm_tune_count == tunes);
    assert(dmr_sm_get_ctx()->vc_tg == 1001);
    assert(state.trunk_chan_map[0x0456] == 0);
    assert(state.dmrburstL == 9);
    assert(state.dmrburstR == 16);

    // Step 3: P_CLEAR on the active slot, then SM should return to CC immediately
    state.currentslot = 1;
    build_pclear(bits, bytes);
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
