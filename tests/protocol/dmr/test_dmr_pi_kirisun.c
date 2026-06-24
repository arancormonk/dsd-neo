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
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static void
load_single_burst_value(dsd_state* state, uint8_t slot, uint16_t sb_value) {
    uint8_t info[11];
    uint8_t encoded[16];
    uint8_t data_matrix[32];
    uint8_t interleaved[32];
    DSD_MEMSET(info, 0, sizeof(info));
    DSD_MEMSET(encoded, 0, sizeof(encoded));
    DSD_MEMSET(data_matrix, 0, sizeof(data_matrix));
    DSD_MEMSET(interleaved, 0, sizeof(interleaved));

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

static uint16_t
build_rc_command_value(uint8_t rc_value) {
    uint8_t rc_bits[4];
    for (int i = 0; i < 4; i++) {
        rc_bits[i] = (uint8_t)((rc_value >> (3 - i)) & 1U);
    }
    const uint8_t masked_crc = (uint8_t)(crc7(rc_bits, 4U) ^ 0x7AU);
    return (uint16_t)(((uint16_t)(rc_value & 0xFU) << 7U) | masked_crc);
}

static uint16_t
build_txi_value(uint8_t opcode, uint8_t delay) {
    const uint8_t low8 = (uint8_t)(((delay & 0x1FU) << 3U) | (opcode & 0x7U));
    uint8_t low_bits[8];
    for (int i = 0; i < 8; i++) {
        low_bits[i] = (uint8_t)((low8 >> (7 - i)) & 1U);
    }
    return (uint16_t)(((uint16_t)crc3(low_bits, 8U) << 8U) | low8);
}

static uint8_t
hytera_pi_checksum(const uint8_t pi[10]) {
    uint8_t checksum = 0;
    for (int i = 0; i < 9; i++) {
        checksum = (uint8_t)((checksum + pi[i]) & 0xFFU);
    }
    checksum = (uint8_t)(~checksum & 0xFFU);
    checksum++;
    return checksum;
}

static void
test_pi_kirisun_slot0_sets_fields_and_le_mode(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

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
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

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
test_pi_kirisun_slot1_sets_fields_and_le_mode(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 1;
    uint8_t pi[10] = {0x37, 0x0A, 0x55, 0xAA, 0xBB, 0xCC, 0xDD, 0x00, 0x00, 0x09};
    dmr_pi(&opts, &state, pi, 1, 0);

    assert(state.dmr_soR == 0x55);
    assert(state.payload_algidR == 0x37);
    assert(state.payload_miR == 0xAABBCCDDULL);
    assert(state.payload_keyidR == (uint8_t)((0x37U * 0x000009U) & 0xFFU));
    assert(opts.dmr_le == 3);
}

static void
test_pi_kirisun_generic_alg_sets_fields(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 0;
    uint8_t pi[10] = {0x38, 0x0A, 0x66, 0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x05};
    dmr_pi(&opts, &state, pi, 1, 0);

    assert(state.dmr_so == 0x66);
    assert(state.payload_algid == 0x38);
    assert(state.payload_mi == 0x01020304ULL);
    assert(state.payload_keyid == (uint8_t)((0x38U * 0x000005U) & 0xFFU));
    assert(opts.dmr_le == 3);
}

static void
test_pi_irrecoverable_error_is_noop(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.trunk_is_tuned = 1;
    opts.dmr_le = 1;
    state.currentslot = 0;
    uint8_t pi[10] = {0x36, 0x0A, 0x40, 0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x01};
    dmr_pi(&opts, &state, pi, 1, 1);

    assert(state.payload_algid == 0);
    assert(state.payload_keyid == 0);
    assert(state.payload_mi == 0);
    assert(state.last_vc_sync_time == 0);
    assert(state.last_cc_sync_time == 0);
    assert(opts.dmr_le == 1);
}

static void
test_pi_dmra_normalizes_aes128_and_expands_iv(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 0;
    state.DMRvcL = 9;
    uint8_t pi[10] = {0x04, 0x10, 0x12, 0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x00};
    dmr_pi(&opts, &state, pi, 1, 0);

    const unsigned long long next_mi =
        ((unsigned long long)state.aes_iv[4] << 24ULL) | ((unsigned long long)state.aes_iv[5] << 16ULL)
        | ((unsigned long long)state.aes_iv[6] << 8ULL) | ((unsigned long long)state.aes_iv[7] << 0ULL);

    assert(state.payload_algid == 0x24);
    assert(state.payload_keyid == 0x12);
    assert(state.aes_iv[0] == 0x11);
    assert(state.aes_iv[1] == 0x22);
    assert(state.aes_iv[2] == 0x33);
    assert(state.aes_iv[3] == 0x44);
    assert(state.payload_mi == next_mi);
    assert(state.DMRvcL == 0);
}

static void
test_pi_dmra_normalizes_rc4_without_iv_expansion(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 0;
    state.DMRvcL = 9;
    uint8_t pi[10] = {0x01, 0x10, 0x45, 0x10, 0x20, 0x30, 0x40, 0x00, 0x00, 0x00};
    dmr_pi(&opts, &state, pi, 1, 0);

    assert(state.payload_algid == 0x21);
    assert(state.payload_keyid == 0x45);
    assert(state.payload_mi == 0x10203040ULL);
    assert(state.DMRvcL == 9);
}

static void
test_pi_dmra_native_family_algid_preserves_rc4_id(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 0;
    uint8_t pi[10] = {0x21, 0x10, 0x46, 0x55, 0x66, 0x77, 0x88, 0x00, 0x00, 0x00};
    dmr_pi(&opts, &state, pi, 1, 0);

    assert(state.payload_algid == 0x21);
    assert(state.payload_keyid == 0x46);
    assert(state.payload_mi == 0x55667788ULL);
}

static void
test_pi_dmra_normalizes_des_on_slot1_and_expands_iv(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 1;
    state.DMRvcR = 7;
    uint8_t pi[10] = {0x02, 0x10, 0x34, 0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00, 0x00};
    dmr_pi(&opts, &state, pi, 1, 0);

    assert(state.payload_algidR == 0x22);
    assert(state.payload_keyidR == 0x34);
    assert((uint32_t)(state.payload_miN >> 32ULL) == 0xCAFEBABEU);
    assert((uint32_t)state.payload_miR == (uint32_t)state.payload_miN);
    assert(state.DMRvcR == 0);
}

static void
test_pi_dmra_normalizes_aes256_on_slot1_and_expands_iv(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 1;
    state.DMRvcR = 8;
    uint8_t pi[10] = {0x05, 0x10, 0x78, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00, 0x00};
    dmr_pi(&opts, &state, pi, 1, 0);

    const unsigned long long next_mi =
        ((unsigned long long)state.aes_ivR[4] << 24ULL) | ((unsigned long long)state.aes_ivR[5] << 16ULL)
        | ((unsigned long long)state.aes_ivR[6] << 8ULL) | ((unsigned long long)state.aes_ivR[7] << 0ULL);

    assert(state.payload_algidR == 0x25);
    assert(state.payload_keyidR == 0x78);
    assert(state.aes_ivR[0] == 0x22);
    assert(state.aes_ivR[1] == 0x33);
    assert(state.aes_ivR[2] == 0x44);
    assert(state.aes_ivR[3] == 0x55);
    assert(state.payload_miR == next_mi);
    assert(state.DMRvcR == 0);
}

static void
test_pi_dmra_normalizes_aes256_on_slot0_and_expands_iv(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 0;
    state.DMRvcL = 8;
    uint8_t pi[10] = {0x05, 0x10, 0x79, 0x33, 0x44, 0x55, 0x66, 0x00, 0x00, 0x00};
    dmr_pi(&opts, &state, pi, 1, 0);

    const unsigned long long next_mi =
        ((unsigned long long)state.aes_iv[4] << 24ULL) | ((unsigned long long)state.aes_iv[5] << 16ULL)
        | ((unsigned long long)state.aes_iv[6] << 8ULL) | ((unsigned long long)state.aes_iv[7] << 0ULL);

    assert(state.payload_algid == 0x25);
    assert(state.payload_keyid == 0x79);
    assert(state.aes_iv[0] == 0x33);
    assert(state.aes_iv[1] == 0x44);
    assert(state.aes_iv[2] == 0x55);
    assert(state.aes_iv[3] == 0x66);
    assert(state.payload_mi == next_mi);
    assert(state.DMRvcL == 0);
}

static void
test_pi_dmra_rejects_algid_outside_dmra_range(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 0;
    uint8_t pi[10] = {0x26, 0x10, 0x56, 0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x00};
    dmr_pi(&opts, &state, pi, 1, 0);

    assert(state.payload_algid == 0);
    assert(state.payload_keyid == 0);
    assert(state.payload_mi == 0);
}

static void
test_pi_hytera_enhanced_checksum_sets_slot1_fields(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.dmr_le = 1;
    opts.show_keys = 0;
    state.currentslot = 1;
    state.RR = 0x123456789AULL;
    uint8_t pi[10] = {0x02, 0x68, 0x34, 0x01, 0x23, 0x45, 0x67, 0x89, 0x00, 0x00};
    pi[9] = hytera_pi_checksum(pi);
    dmr_pi(&opts, &state, pi, 1, 0);

    assert((state.dmr_soR & 0x40U) != 0U);
    assert(state.payload_algidR == 0x02);
    assert(state.payload_keyidR == 0x34);
    assert(state.payload_miR == 0x0123456789ULL);
    assert(opts.dmr_le == 2);
}

static void
test_pi_hytera_slot0_checksum_key_and_error_paths(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.dmr_le = 1;
    opts.show_keys = 1;
    state.currentslot = 0;
    state.R = 0x0102030405ULL;
    uint8_t pi[10] = {0x02, 0x68, 0x21, 0x10, 0x32, 0x54, 0x76, 0x98, 0x00, 0x00};
    pi[9] = hytera_pi_checksum(pi);
    dmr_pi(&opts, &state, pi, 1, 0);

    assert((state.dmr_so & 0x40U) != 0U);
    assert(state.payload_algid == 0x02);
    assert(state.payload_keyid == 0x21);
    assert(state.payload_mi == 0x1032547698ULL);
    assert(opts.dmr_le == 2);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.dmr_le = 1;
    state.currentslot = 0;
    pi[9] ^= 0x7FU;
    dmr_pi(&opts, &state, pi, 1, 0);

    assert((state.dmr_so & 0x40U) != 0U);
    assert(state.payload_algid == 0x02);
    assert(state.payload_keyid == 0x21);
    assert(state.payload_mi == 0x1032547698ULL);
    assert(opts.dmr_le == 1);
}

static void
test_pi_refreshes_sync_time_when_trunk_tuned(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.trunk_is_tuned = 1;
    state.currentslot = 0;
    uint8_t pi[10] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    dmr_pi(&opts, &state, pi, 1, 0);

    assert(state.last_vc_sync_time > 0);
    assert(state.last_cc_sync_time > 0);
    assert(state.last_vc_sync_time_m > 0.0);
    assert(state.last_cc_sync_time_m > 0.0);
}

static void
test_alg_refresh_advances_kirisun_mi(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

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
test_lfsr_refresh_updates_slot1_variants(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 1;
    state.payload_algidR = 0x21;
    state.payload_keyidR = 0x66;
    state.payload_miR = 0x89ABCDEFULL;
    LFSR(&state);
    assert(state.payload_miR != 0x89ABCDEFULL);

    state.payload_algidR = 0x24;
    state.payload_keyidR = 0x77;
    state.payload_miR = 0x11223344ULL;
    state.DMRvcR = 6;
    LFSR128d(&state);
    assert(state.aes_ivR[0] == 0x11);
    assert(state.aes_ivR[1] == 0x22);
    assert(state.aes_ivR[2] == 0x33);
    assert(state.aes_ivR[3] == 0x44);
    assert(state.payload_miR != 0x11223344ULL);
    assert(state.DMRvcR == 0);
}

static void
test_hytera_refresh_advances_feedback_branch(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 0;
    state.payload_mi = 0x8000000000ULL;
    hytera_enhanced_alg_refresh(&state);
    assert(state.payload_mi != 0x8000000000ULL);
}

static void
test_sbrc_kirisun_gate_rejects_non_kirisun_calls(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

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
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

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
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

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

static void
test_sbrc_standard_slot0_encryption_identifiers(void) {
    static dsd_opts opts;
    static dsd_state state;
    const uint8_t algs[] = {1U, 2U, 4U, 5U};

    for (size_t i = 0; i < sizeof(algs) / sizeof(algs[0]); i++) {
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));

        const uint8_t alg = algs[i];
        const uint8_t key = (uint8_t)(0x20U + alg);
        opts.dmr_le = 1;
        opts.payload = 1;
        state.currentslot = 0;
        state.dmr_so = 0x40U;
        load_single_burst_value(&state, 0, (uint16_t)(((uint16_t)key << 3U) | alg));

        dmr_sbrc(&opts, &state, 0);

        assert(state.payload_algid == (int)(alg + 0x20U));
        assert(state.payload_keyid == key);
        assert(state.payload_algidR == 0);
    }
}

static void
test_sbrc_standard_slot1_encryption_identifier_guards(void) {
    static dsd_opts opts;
    static dsd_state state;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.dmr_le = 1;
    state.currentslot = 1;
    state.dmr_soR = 0x40U;
    state.M = 1;
    load_single_burst_value(&state, 1, (uint16_t)((0x44U << 3U) | 5U));
    dmr_sbrc(&opts, &state, 0);
    assert(state.payload_algidR == 0);
    assert(state.payload_keyidR == 0);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.dmr_le = 1;
    state.currentslot = 1;
    state.dmr_soR = 0x40U;
    load_single_burst_value(&state, 1, (uint16_t)((0x44U << 3U) | 5U));
    dmr_sbrc(&opts, &state, 0);
    assert(state.payload_algidR == 0x25);
    assert(state.payload_keyidR == 0x44);
}

static void
test_sbrc_reverse_channel_commands(void) {
    static dsd_opts opts;
    static dsd_state state;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.dmr_le = 1;
    state.currentslot = 0;
    load_single_burst_value(&state, 0, build_rc_command_value(5U));
    dmr_sbrc(&opts, &state, 1);
    assert(state.payload_algid == 0);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.dmr_le = 1;
    state.currentslot = 0;
    load_single_burst_value(&state, 0, build_rc_command_value(6U));
    dmr_sbrc(&opts, &state, 1);
    assert(state.payload_algid == 0);
}

static void
test_sbrc_txi_commands(void) {
    static dsd_opts opts;
    static dsd_state state;
    const uint8_t delays[] = {2U, 4U, 6U, 8U};

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.dmr_le = 1;
    opts.payload = 1;
    state.currentslot = 0;
    load_single_burst_value(&state, 0, build_txi_value(0U, 5U));
    dmr_sbrc(&opts, &state, 0);
    assert(state.payload_algid == 0);

    for (size_t i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.dmr_le = 1;
        opts.payload = 1;
        state.currentslot = 0;
        load_single_burst_value(&state, 0, build_txi_value(3U, delays[i]));
        dmr_sbrc(&opts, &state, 0);
        assert(state.payload_algid == 0);
    }
}

int
main(void) {
    InitAllFecFunction();

    test_pi_kirisun_slot0_sets_fields_and_le_mode();
    test_pi_kirisun_requires_crc_ok();
    test_pi_kirisun_slot1_sets_fields_and_le_mode();
    test_pi_kirisun_generic_alg_sets_fields();
    test_pi_irrecoverable_error_is_noop();
    test_pi_dmra_normalizes_aes128_and_expands_iv();
    test_pi_dmra_normalizes_rc4_without_iv_expansion();
    test_pi_dmra_native_family_algid_preserves_rc4_id();
    test_pi_dmra_normalizes_des_on_slot1_and_expands_iv();
    test_pi_dmra_normalizes_aes256_on_slot1_and_expands_iv();
    test_pi_dmra_normalizes_aes256_on_slot0_and_expands_iv();
    test_pi_dmra_rejects_algid_outside_dmra_range();
    test_pi_hytera_enhanced_checksum_sets_slot1_fields();
    test_pi_hytera_slot0_checksum_key_and_error_paths();
    test_pi_refreshes_sync_time_when_trunk_tuned();
    test_alg_refresh_advances_kirisun_mi();
    test_lfsr_refresh_updates_slot1_variants();
    test_hytera_refresh_advances_feedback_branch();
    test_sbrc_kirisun_gate_rejects_non_kirisun_calls();
    test_sbrc_kirisun_gate_accepts_kirisun_calls();
    test_sbrc_kirisun_gate_ignores_stale_kirisun_alg();
    test_sbrc_standard_slot0_encryption_identifiers();
    test_sbrc_standard_slot1_encryption_identifier_guards();
    test_sbrc_reverse_channel_commands();
    test_sbrc_txi_commands();
    printf("DMR PI Kirisun: OK\n");
    return 0;
}
