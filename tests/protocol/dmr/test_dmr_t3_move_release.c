// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Simulate grant → C_MOVE (TS2→TS1) → P_CLEAR and verify return-to-CC.
 */

#include <dsd-neo/protocol/dmr/dmr_utils_api.h>

#include <assert.h>
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

void
watchdog_event_datacall(dsd_opts* opts, dsd_state* state, uint32_t src, uint32_t dst, char* data_string, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)src;
    (void)dst;
    (void)data_string;
    (void)slot;
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
    opts->trunk_hangtime = 0.0f;      // immediate release on clear
    state->trunk_cc_freq = 851000000; // mock CC
}

// Compose a minimal C_MOVE CSBK + MBC absolute channel parms
static void
build_cmove_apcn(uint8_t* bits, uint8_t* bytes, uint16_t apcn, uint16_t rx_int_mhz, uint16_t rx_step_125hz,
                 uint8_t slot) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    // Opcode (low 6 bits of first byte) = 57
    bytes[0] = (uint8_t)(57 & 0x3F);
    // LPCN field (bits 16..27) = 0xFFF (absolute)
    for (int i = 0; i < 12; i++) {
        bits[16 + i] = ((0xFFFu >> (11 - i)) & 1u);
    }
    // Slot bit (bit 28) — 0 => TS1, 1 => TS2
    bits[28] = (slot & 1u);
    // MBC CDEFTYPE (bits 112..115) = 0 (absolute channel parms)
    // APCN (bits 118..129)
    for (int i = 0; i < 12; i++) {
        bits[118 + i] = ((apcn >> (11 - i)) & 1u);
    }
    // RX_INT (bits 153..162) 10 bits
    for (int i = 0; i < 10; i++) {
        bits[153 + i] = ((rx_int_mhz >> (9 - i)) & 1u);
    }
    // RX_STEP (bits 163..175) 13 bits
    for (int i = 0; i < 13; i++) {
        bits[163 + i] = ((rx_step_125hz >> (12 - i)) & 1u);
    }
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

    // Step 2: issue C_MOVE to TS1 with new absolute channel (e.g., 853.500000 MHz)
    uint8_t bits[256];
    uint8_t bytes[48];
    uint16_t apcn = 0x0456;
    uint16_t rx_int = 853;                                            // MHz
    uint16_t rx_step = 4000;                                          // 4000 * 125 Hz = 500 kHz
    long f2 = 853000000 + 4000 * 125;                                 // 853.500000 MHz
    build_cmove_apcn(bits, bytes, apcn, rx_int, rx_step, /*slot*/ 0); // TS1
    dmr_cspdu(&opts, &state, bits, bytes, /*crc ok*/ 1, /*errs*/ 0);
    assert(opts.trunk_is_tuned == 1);
    assert(state.trunk_vc_freq[0] == f2);
    // After move, opposite slot should clear; destination slot shows voice
    assert(state.dmrburstL == 16);
    assert(state.dmrburstR == 9);

    // Step 3: P_CLEAR on the active slot, then SM should return to CC immediately
    // Set current slot context to TS1 for P_CLEAR evaluation
    state.currentslot = 0;
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
