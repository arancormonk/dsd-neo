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
#include <dsd-neo/fec/dmr_late_entry.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>
#include <stdio.h>
#include "dmr_vcrypt.h"
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
    const uint8_t crc = dsd_dmr_crc4(mi_bits32, 32);

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
    // All six bursts of the superframe delivered their fragments (issue #729: only a complete set decodes).
    state->dmr_le_frag_mask[slot] = 0x7EU;
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
test_forced_algid_preserves_pi_header_keyid(void) {
    // Issue #351: --dmr-force-algid is applied on every LE fragment. The KEY ID a
    // CRC-verified DMRA PI header stored for the call must survive that application,
    // or key lookup selects rkey_array[0xFF] instead of the header's key.
    static dsd_opts opts;
    static dsd_state state;
    static dsd_state encoded;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&encoded, 0, sizeof(encoded));

    const uint32_t mi = 0xA1B2C3D4U;
    state.currentslot = 0;
    state.M = 0x21;             // --dmr-force-algid 21
    state.dmr_so = 0x40U;       // service options carry the privacy bit
    state.payload_algid = 0x21; // CRC-verified PI header already decoded for this call
    state.payload_keyid = 0x03;
    state.payload_mi = 0x01020304ULL;

    fill_le_fragments_for_mi(&encoded, 0, mi);
    for (uint8_t vc = 1; vc <= 6; vc++) {
        uint8_t ambe_fr[4][24];
        uint8_t ambe_fr2[4][24];
        uint8_t ambe_fr3[4][24];
        write_nibble_to_ambe(ambe_fr, (uint8_t)encoded.late_entry_mi_fragment[0][vc][0]);
        write_nibble_to_ambe(ambe_fr2, (uint8_t)encoded.late_entry_mi_fragment[0][vc][1]);
        write_nibble_to_ambe(ambe_fr3, (uint8_t)encoded.late_entry_mi_fragment[0][vc][2]);
        dmr_late_entry_mi_fragment(&opts, &state, vc, ambe_fr, ambe_fr2, ambe_fr3);
    }

    assert(state.payload_algid == 0x21);
    assert(state.payload_keyid == 0x03);
    assert((uint32_t)state.payload_mi == mi);
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

/* The RC4 32-bit MI step, written out here so the decoder's helpers are checked against it rather than themselves. */
static uint32_t
expected_rc4_next(uint32_t mi) {
    uint64_t lfsr = mi;
    for (int i = 0; i < 32; i++) {
        const uint64_t bit = ((lfsr >> 31) ^ (lfsr >> 3) ^ (lfsr >> 1)) & 1U;
        lfsr = (lfsr << 1) | bit;
    }
    return (uint32_t)lfsr;
}

/* The DES 64-bit IV: the MI, then 32 more bits of the x^32 + x^22 + x^2 + x + 1 recurrence. */
static uint64_t
expected_des_iv(uint32_t mi) {
    uint64_t lfsr = mi;
    for (int i = 0; i < 32; i++) {
        const uint64_t bit = ((lfsr >> 31) ^ (lfsr >> 21) ^ (lfsr >> 1) ^ lfsr) & 1U;
        lfsr = (lfsr << 1) | bit;
    }
    return lfsr;
}

static void
test_dropout_catch_up_advances_each_encrypted_slot(void) {
    // Issue #729: the superframe whose VC6 a burst-loop drop-out cost is caught up at the slot's next voice sync, on
    // each encrypted slot, with its keystream rebuilt; the old error-time refresh guessed one step for RC4 only.
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.payload_algid = 0x21;
    state.payload_mi = 0x11223344ULL;
    state.DMRvcL = 7;
    state.payload_algidR = 0x22;
    state.payload_miR = 0x55667788ULL;
    state.DMRvcR = 9;
    state.currentslot = 1;

    state.symbol_clock = 10000U;
    for (uint8_t slot = 0; slot < 2; slot++) {
        dmr_vcrypt_note_first(&state, slot);
        dmr_vcrypt_superframe_start(&state, slot);
        assert(state.dmr_vc_ref_state[slot] == DMR_VC_REF_VALID);
    }
    // Neither slot's VC6 ran; their next voice syncs arrive one superframe on.
    state.symbol_clock += DMR_VC_SUPERFRAME_SYMBOLS;
    dmr_vcrypt_superframe_start(&state, 0);
    dmr_vcrypt_superframe_start(&state, 1);

    assert((uint32_t)state.payload_mi == expected_rc4_next(0x11223344U));
    const uint64_t des_iv = expected_des_iv(0x55667788U);
    assert(state.payload_miN == des_iv);
    assert((uint32_t)state.payload_miR == (uint32_t)des_iv);
    assert(state.dropL == 256);
    assert(state.dropR == 256);
    assert(state.DMRvcL == 0);
    assert(state.DMRvcR == 0);
    assert(state.currentslot == 1);
    assert(state.dmr_vc_untrusted[0] == 0U && state.dmr_vc_untrusted[1] == 0U);
}

static void
test_incomplete_fragment_set_is_not_an_mi(void) {
    // Issue #729: the gate skipped a burst (or the loop lost one), so the array mixes two superframes' fragments.
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 0;
    state.payload_algid = 0x21;
    state.payload_keyid = 0x34;
    state.payload_mi = 0x01020304ULL;
    fill_le_fragments_for_mi(&state, 0, 0xA1B2C3D4U);
    state.dmr_le_frag_mask[0] = 0x7EU & ~(1U << 2); // VC2 never arrived this superframe
    dmr_late_entry_mi(&opts, &state);
    assert((uint32_t)state.payload_mi == 0x01020304U);
}

static void
test_late_entry_against_trusted_chain_waits_for_confirmation(void) {
    // Issue #729: against a chain a PI header anchored, one disagreeing late entry (a Golay miscorrection passes CRC4
    // one time in 16) is held; the next superframe's late entry, its successor, confirms it.
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 0;
    state.payload_algid = 0x21;
    state.payload_keyid = 0x34;
    state.payload_mi = 0x01020304ULL;
    state.symbol_clock = 5000U;
    dmr_vcrypt_note_first(&state, 0);
    dmr_vcrypt_superframe_start(&state, 0);
    assert(state.dmr_vc_ref_state[0] == DMR_VC_REF_VALID && state.dmr_vc_untrusted[0] == 0U);

    const uint32_t wrong = 0xA1B2C3D4U;
    fill_le_fragments_for_mi(&state, 0, wrong);
    dmr_late_entry_mi(&opts, &state);
    assert((uint32_t)state.payload_mi == 0x01020304U);
    assert(state.dmr_le_candidate_valid[0] != 0U);

    const uint32_t successor = expected_rc4_next(wrong);
    fill_le_fragments_for_mi(&state, 0, successor);
    dmr_late_entry_mi(&opts, &state);
    assert((uint32_t)state.payload_mi == successor);
    assert(state.dmr_le_candidate_valid[0] == 0U);
}

static void
test_held_late_entry_mi_expires_and_a_pi_drops_it(void) {
    // Issue #729: a held late-entry MI is confirmed only by the very next superframe's; one superframe later it has
    // expired, and a PI that re-keys the slot drops it at once, so neither can overwrite the chain the PI anchored.
    static dsd_opts opts;
    static dsd_state state;
    for (int pi_rekey = 0; pi_rekey <= 1; pi_rekey++) {
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        state.currentslot = 0;
        state.payload_algid = 0x21;
        state.payload_keyid = 0x34;
        state.payload_mi = 0x01020304ULL;
        state.symbol_clock = 5000U;
        dmr_vcrypt_note_first(&state, 0);
        dmr_vcrypt_superframe_start(&state, 0);

        const uint32_t wrong = 0xA1B2C3D4U;
        fill_le_fragments_for_mi(&state, 0, wrong);
        dmr_late_entry_mi(&opts, &state);
        assert(state.dmr_le_candidate_valid[0] != 0U);
        dmr_vcrypt_superframe_end(&state, 0);
        if (pi_rekey) {
            dmr_vcrypt_note_first(&state, 0); // a PI header: the slot is re-keyed
        } else {
            dmr_vcrypt_superframe_end(&state, 0); // the next superframe's late entry never completed
        }
        assert(state.dmr_le_candidate_valid[0] == 0U);
    }
}

static void
test_late_entry_without_trusted_chain_is_adopted_at_once(void) {
    // A late entrant (no PI header seen, chain unknown) keeps today's immediate adoption, and the chain is trusted
    // from the next superframe on.
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 0;
    state.payload_algid = 0x21;
    state.payload_keyid = 0x34;
    state.payload_mi = 0x01020304ULL;
    state.symbol_clock = 7000U;
    dmr_vcrypt_superframe_start(&state, 0);
    assert(state.dmr_vc_untrusted[0] == 1U);

    fill_le_fragments_for_mi(&state, 0, 0xA1B2C3D4U);
    dmr_late_entry_mi(&opts, &state);
    assert((uint32_t)state.payload_mi == 0xA1B2C3D4U);
    assert(state.dmr_vc_ref_state[0] == DMR_VC_REF_VALID);
    dmr_vcrypt_superframe_end(&state, 0);
    state.symbol_clock += DMR_VC_SUPERFRAME_SYMBOLS;
    dmr_vcrypt_superframe_start(&state, 0);
    assert(state.dmr_vc_untrusted[0] == 0U);
}

static void
test_kirisun_late_entry_candidate_is_confirmed_by_its_successor(void) {
    // Kirisun steps its MI with kirisun_lfsr() each superframe: two successive genuine late-entry MIs that disagree
    // with a stale trusted chain confirm one another (issue #729).
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 0;
    state.payload_algid = 0x36;
    state.payload_keyid = 0x01;
    state.payload_mi = 0x01020304ULL;
    state.symbol_clock = 9000U;
    dmr_vcrypt_note_first(&state, 0);
    dmr_vcrypt_superframe_start(&state, 0);

    const uint32_t genuine = 0x5A6B7C8DU;
    fill_le_fragments_for_mi(&state, 0, genuine);
    dmr_late_entry_mi(&opts, &state);
    assert(state.dmr_le_candidate_valid[0] != 0U);

    const uint32_t successor = kirisun_lfsr(genuine);
    fill_le_fragments_for_mi(&state, 0, successor);
    dmr_late_entry_mi(&opts, &state);
    assert(state.dmr_le_candidate_valid[0] == 0U);
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
    test_forced_algid_preserves_pi_header_keyid();
    test_verified_mi_updates_existing_slot_mi();
    test_dropout_catch_up_advances_each_encrypted_slot();
    test_incomplete_fragment_set_is_not_an_mi();
    test_late_entry_against_trusted_chain_waits_for_confirmation();
    test_late_entry_without_trusted_chain_is_adopted_at_once();
    test_held_late_entry_mi_expires_and_a_pi_drops_it();
    test_kirisun_late_entry_candidate_is_confirmed_by_its_successor();
    test_alg_refresh_ignores_invalid_current_slot();

    printf("DMR LE infer algid: OK\n");
    return 0;
}

// NOLINTEND(bugprone-implicit-widening-of-multiplication-result)
