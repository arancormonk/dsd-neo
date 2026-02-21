// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/bptc.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static void
load_single_burst_value(dsd_state* state, uint8_t slot, uint16_t sb_value) {
    uint8_t info[11];
    uint8_t encoded[16];
    uint8_t data_matrix[32];
    uint8_t interleaved[32];
    memset(info, 0, sizeof(info));
    memset(encoded, 0, sizeof(encoded));
    memset(data_matrix, 0, sizeof(data_matrix));
    memset(interleaved, 0, sizeof(interleaved));

    for (int i = 0; i < 11; i++) {
        info[i] = (uint8_t)((sb_value >> (10 - i)) & 1U);
    }

    Hamming_16_11_4_encode(info, encoded);
    for (int i = 0; i < 16; i++) {
        data_matrix[i] = encoded[i] & 1U;
        data_matrix[16 + i] = data_matrix[i];
    }

    for (int i = 0; i < 32; i++) {
        interleaved[i] = data_matrix[DeInterleaveReverseChannelBptcPlacement[DeInterleaveReverseChannelBptc[i]]];
    }

    for (int i = 0; i < 32; i++) {
        state->dmr_embedded_signalling[slot][5][i + 8] = interleaved[i];
    }
}

static void
test_pi_kirisun_slot0_sets_fields_and_le_mode(void) {
    static dsd_opts opts;
    static dsd_state state;
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));

    state.currentslot = 0;
    uint8_t pi[10] = {0x36, 0x0A, 0x40, 0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x01};
    dmr_pi(&opts, &state, pi, 1, 0);

    assert(state.dmr_so == 0x40);
    assert(state.payload_algid == 0x36);
    assert(state.payload_mi == 0x11223344ULL);
    assert(state.payload_keyid == (uint8_t)((0x36U * 0x000001U) & 0xFFU));
    assert(opts.dmr_le == 3);
}

static void
test_pi_kirisun_requires_crc_ok(void) {
    static dsd_opts opts;
    static dsd_state state;
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));

    opts.dmr_le = 1;
    state.currentslot = 0;
    uint8_t pi[10] = {0x36, 0x0A, 0x40, 0xAA, 0xBB, 0xCC, 0xDD, 0x00, 0x00, 0x01};
    dmr_pi(&opts, &state, pi, 0, 0);

    assert(state.payload_algid == 0);
    assert(state.payload_keyid == 0);
    assert(state.payload_mi == 0);
    assert(opts.dmr_le == 1);
}

static void
test_alg_refresh_advances_kirisun_mi(void) {
    static dsd_opts opts;
    static dsd_state state;
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));

    state.currentslot = 0;
    state.payload_algid = 0x36;
    state.payload_keyid = 0x12;
    state.payload_mi = 0x11223344ULL;
    state.DMRvcL = 9;

    const uint32_t expected = kirisun_lfsr(0x11223344ULL);
    dmr_alg_refresh(&opts, &state);

    assert((uint32_t)state.payload_mi == expected);
    assert(state.DMRvcL == 0);
    assert(state.dropL == 256);
}

static void
test_sbrc_kirisun_gate_rejects_non_kirisun_calls(void) {
    static dsd_opts opts;
    static dsd_state state;
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));

    opts.dmr_le = 3;
    state.currentslot = 1;
    state.dmr_soR = 0x40U;
    state.dmr_fidR = 0x10;
    state.payload_algidR = 0;
    load_single_burst_value(&state, 1, 0x008U);

    dmr_sbrc(&opts, &state, 0);

    assert(state.payload_algidR != 0x35);
}

static void
test_sbrc_kirisun_gate_accepts_kirisun_calls(void) {
    static dsd_opts opts;
    static dsd_state state;
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));

    opts.dmr_le = 3;
    state.currentslot = 1;
    state.dmr_soR = 0x40U;
    state.dmr_fidR = 0x0A;
    state.payload_algidR = 0;
    load_single_burst_value(&state, 1, 0x008U);

    dmr_sbrc(&opts, &state, 0);

    assert(state.payload_algidR == 0x35);
}

static void
test_sbrc_kirisun_gate_ignores_stale_kirisun_alg(void) {
    static dsd_opts opts;
    static dsd_state state;
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));

    opts.dmr_le = 3;
    state.currentslot = 1;
    state.dmr_soR = 0x40U;
    state.dmr_fidR = 0x10;
    state.payload_algidR = 0x35;
    state.payload_keyidR = 0xAA;
    load_single_burst_value(&state, 1, 0x094U);

    dmr_sbrc(&opts, &state, 0);

    assert(state.payload_algidR == 0x24);
    assert(state.payload_keyidR == 0x12);
}

int
main(void) {
    InitAllFecFunction();

    test_pi_kirisun_slot0_sets_fields_and_le_mode();
    test_pi_kirisun_requires_crc_ok();
    test_alg_refresh_advances_kirisun_mi();
    test_sbrc_kirisun_gate_rejects_non_kirisun_calls();
    test_sbrc_kirisun_gate_accepts_kirisun_calls();
    test_sbrc_kirisun_gate_ignores_stale_kirisun_alg();
    printf("DMR PI Kirisun: OK\n");
    return 0;
}
