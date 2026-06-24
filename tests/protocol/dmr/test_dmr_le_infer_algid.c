// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static uint8_t
bits_to_u4_msb(const unsigned char bits4[4]) {
    uint8_t v = 0;
    for (int i = 0; i < 4; i++) {
        v = (uint8_t)((v << 1) | (bits4[i] & 1U));
    }
    return v & 0xF;
}

static void
fill_le_fragments_for_mi(dsd_state* state, uint8_t slot, uint32_t mi32) {
    // Build 36-bit MI+CRC (MSB-first): 32-bit MI then CRC4.
    uint8_t mi_bits32[32];
    for (int i = 0; i < 32; i++) {
        mi_bits32[i] = (uint8_t)((mi32 >> (31 - i)) & 1U);
    }
    const uint8_t crc = crc4(mi_bits32, 32);

    unsigned char msg36[36];
    DSD_MEMSET(msg36, 0, sizeof(msg36));
    for (int i = 0; i < 32; i++) {
        msg36[i] = (unsigned char)(mi_bits32[i] & 1U);
    }
    for (int i = 0; i < 4; i++) {
        msg36[32 + i] = (unsigned char)((crc >> (3 - i)) & 1U);
    }

    // Golay(24,12) parity bits for each 12-bit chunk (MSB-first).
    unsigned char go36[36];
    DSD_MEMSET(go36, 0, sizeof(go36));
    for (int chunk = 0; chunk < 3; chunk++) {
        unsigned char orig[12];
        unsigned char enc[24];
        DSD_MEMSET(orig, 0, sizeof(orig));
        DSD_MEMSET(enc, 0, sizeof(enc));
        for (int i = 0; i < 12; i++) {
            orig[i] = msg36[chunk * 12 + i] & 1U;
        }
        Golay_24_12_encode(orig, enc);
        for (int i = 0; i < 12; i++) {
            assert((enc[i] & 1U) == (orig[i] & 1U)); // systematic encoding assumed by LE split
            go36[chunk * 12 + i] = enc[12 + i] & 1U;
        }
    }

    // Pack 12-bit chunks into 3 nibbles across vc=1..3 (MI) and vc=4..6 (GO), per column.
    for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) {
            const int bit_base = col * 12 + row * 4;
            const uint8_t mi_nib = bits_to_u4_msb(&msg36[bit_base]);
            const uint8_t go_nib = bits_to_u4_msb(&go36[bit_base]);
            state->late_entry_mi_fragment[slot][1 + row][col] = mi_nib;
            state->late_entry_mi_fragment[slot][4 + row][col] = go_nib;
        }
    }
}

static void
write_nibble_to_ambe(uint8_t frame[4][24], uint8_t nibble) {
    DSD_MEMSET(frame, 0, 4U * 24U * sizeof(frame[0][0]));
    for (int i = 0; i < 4; i++) {
        frame[3][i] = (uint8_t)((nibble >> (3 - i)) & 1U);
    }
}

static void
run_case(uint8_t slot, unsigned long long key, uint32_t mi, int expect_algid) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = slot;
    state.M = 0;
    state.R = key;
    state.RR = key;
    state.dmr_so = 0x40U;
    state.dmr_soR = 0x40U;

    fill_le_fragments_for_mi(&state, slot, mi);
    dmr_late_entry_mi(&opts, &state);

    if (slot == 0U) {
        assert(state.payload_algid == expect_algid);
        assert(state.payload_keyid == 0xFF);
        if (expect_algid == 0x21) {
            assert((uint32_t)state.payload_mi == mi);
        } else if (expect_algid == 0x22) {
            assert(state.payload_miP != 0);
            assert((uint32_t)(state.payload_miP >> 32) == mi);
            assert((uint32_t)state.payload_mi == (uint32_t)state.payload_miP);
        } else {
            assert(0 && "unexpected algid");
        }
        assert(state.payload_algidR == 0);
        return;
    }

    assert(state.payload_algidR == expect_algid);
    assert(state.payload_keyidR == 0xFF);
    if (expect_algid == 0x21) {
        assert((uint32_t)state.payload_miR == mi);
    } else if (expect_algid == 0x22) {
        assert(state.payload_miN != 0);
        assert((uint32_t)(state.payload_miN >> 32) == mi);
        assert((uint32_t)state.payload_miR == (uint32_t)state.payload_miN);
    } else {
        assert(0 && "unexpected algid");
    }
    assert(state.payload_algid == 0);
}

static void
test_fragment_wrapper_maps_slot2_to_slot1_and_triggers_decode(void) {
    static dsd_opts opts;
    static dsd_state state;
    static dsd_state encoded;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&encoded, 0, sizeof(encoded));

    const uint32_t mi = 0x55667788U;
    state.currentslot = 2;
    state.M = 0;
    state.RR = 0x0123456789ABCDULL;
    state.dmr_soR = 0x40U;

    fill_le_fragments_for_mi(&encoded, 1, mi);
    for (uint8_t vc = 1; vc <= 6; vc++) {
        uint8_t ambe_fr[4][24];
        uint8_t ambe_fr2[4][24];
        uint8_t ambe_fr3[4][24];
        write_nibble_to_ambe(ambe_fr, (uint8_t)encoded.late_entry_mi_fragment[1][vc][0]);
        write_nibble_to_ambe(ambe_fr2, (uint8_t)encoded.late_entry_mi_fragment[1][vc][1]);
        write_nibble_to_ambe(ambe_fr3, (uint8_t)encoded.late_entry_mi_fragment[1][vc][2]);
        if (vc == 6U) {
            state.currentslot = 1;
        }
        dmr_late_entry_mi_fragment(&opts, &state, vc, ambe_fr, ambe_fr2, ambe_fr3);
    }

    assert(state.late_entry_mi_fragment[0][1][0] == 0);
    assert(state.late_entry_mi_fragment[1][1][0] == encoded.late_entry_mi_fragment[1][1][0]);
    assert(state.payload_algidR == 0x22);
    assert(state.payload_keyidR == 0xFF);
    assert(state.payload_miN != 0);
    assert((uint32_t)(state.payload_miN >> 32) == mi);
}

static void
test_verified_mi_updates_existing_slot_mi(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    const uint32_t mi = 0xA1B2C3D4U;
    state.currentslot = 0;
    state.M = 0;
    state.payload_algid = 0x21;
    state.payload_keyid = 0x34;
    state.payload_mi = 0x01020304ULL;

    fill_le_fragments_for_mi(&state, 0, mi);
    dmr_late_entry_mi(&opts, &state);

    assert(state.payload_algid == 0x21);
    assert(state.payload_keyid == 0x34);
    assert((uint32_t)state.payload_mi == mi);
}

static void
test_alg_refresh_on_error_refreshes_each_encrypted_slot(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.K1 = 1;
    state.payload_algid = 0x21;
    state.payload_mi = 0x11223344ULL;
    state.DMRvcL = 7;
    state.payload_algidR = 0x22;
    state.payload_miR = 0x55667788ULL;
    state.DMRvcR = 9;

    dmr_refresh_algids_on_error(&opts, &state);

    assert(state.dropL == 256);
    assert(state.dropR == 256);
    assert(state.DMRvcL == 0);
    assert(state.DMRvcR == 0);
    assert(state.currentslot == 1);
}

static void
test_alg_refresh_ignores_invalid_current_slot(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 2;
    state.payload_algid = 0x21;
    state.DMRvcL = 5;
    state.DMRvcR = 6;

    dmr_alg_refresh(&opts, &state);

    assert(state.dropL == 0);
    assert(state.dropR == 0);
    assert(state.DMRvcL == 5);
    assert(state.DMRvcR == 6);
}

int
main(void) {
    InitAllFecFunction();

    // RC4 key (<=40-bit) should infer ALG ID 0x21.
    run_case(0, 0xE3AE36E22AULL, 0xEC60C8BEU, 0x21);

    // DES key (>40-bit) should infer ALG ID 0x22.
    run_case(0, 0x0123456789ABCDULL, 0x11223344U, 0x22);

    // Slot 2 should infer through the right-slot state and LFSR fields.
    run_case(1, 0x0123456789ABCDULL, 0x22334455U, 0x22);

    test_fragment_wrapper_maps_slot2_to_slot1_and_triggers_decode();
    test_verified_mi_updates_existing_slot_mi();
    test_alg_refresh_on_error_refreshes_each_encrypted_slot();
    test_alg_refresh_ignores_invalid_current_slot();

    printf("DMR LE infer algid: OK\n");
    return 0;
}

// NOLINTEND(bugprone-implicit-widening-of-multiplication-result)
