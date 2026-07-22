// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression checks for NXDN deinterleave and depuncture primitives shared by
 * SACCH, FACCH, CAC, FACCH2, and FACCH3 soft-decision paths.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/nxdn/nxdn_const.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "nxdn_const_reinclude.h" // IWYU pragma: keep
#include "nxdn_internal.h"

static int
expect_u8_at(const char* tag, size_t index, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s[%zu]: got 0x%02X want 0x%02X\n", tag, index, got, want);
        return 1;
    }
    return 0;
}

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u16(const char* tag, uint16_t got, uint16_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%04X want 0x%04X\n", tag, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
fill_input(uint8_t* input, uint8_t* reliab, size_t len) {
    for (size_t i = 0U; i < len; i++) {
        input[i] = (uint8_t)((i * 3U + 1U) & 0xFFU);
        reliab[i] = (uint8_t)(255U - ((i * 5U) & 0xFFU));
    }
}

static void
write_bits_msb(uint8_t* bits, size_t start, size_t nbits, uint32_t value) {
    for (size_t i = 0U; i < nbits; i++) {
        const size_t shift = nbits - 1U - i;
        bits[start + i] = (uint8_t)((value >> shift) & 1U);
    }
}

static unsigned long long
advance_nxdn_lfsr_seed(unsigned long long seed, int calls) {
    int lfsr = (int)(seed & 0x7FFFULL);
    for (int call = 0; call < calls; call++) {
        for (int i = 0; i < 49; i++) {
            const int bit = ((lfsr >> 1) ^ (lfsr >> 0)) & 1;
            lfsr = ((lfsr >> 1) | (bit << 14));
        }
    }
    return (unsigned long long)(lfsr & 0x7FFF);
}

static int
expect_matrix_deperm(const char* tag, const uint8_t* input, const uint8_t* reliab, const uint8_t* deperm,
                     const uint8_t* deperm_rel, size_t len, size_t columns) {
    int rc = 0;
    const size_t rows = len / columns;

    for (size_t i = 0U; i < len; i++) {
        const size_t out = (i % rows) * columns + (i / rows);
        rc |= expect_u8_at(tag, out, deperm[out], input[i]);
        rc |= expect_u8_at("deperm reliability", out, deperm_rel[out], reliab[i]);
    }
    return rc;
}

static int
test_depermute_matrices(void) {
    int rc = 0;

    {
        uint8_t input[60];
        uint8_t reliab[60];
        uint8_t deperm[60];
        uint8_t deperm_rel[60];
        fill_input(input, reliab, sizeof(input));
        DSD_MEMSET(deperm, 0xA5, sizeof(deperm));
        DSD_MEMSET(deperm_rel, 0x5A, sizeof(deperm_rel));

        nxdn_depermute_rel_u8(input, reliab, 60U, PERM_12_5, deperm, deperm_rel);
        rc |= expect_matrix_deperm("deperm-12x5", input, reliab, deperm, deperm_rel, sizeof(input), 12U);
    }

    {
        uint8_t input[144];
        uint8_t reliab[144];
        uint8_t deperm[144];
        uint8_t deperm_rel[144];
        fill_input(input, reliab, sizeof(input));
        DSD_MEMSET(deperm, 0xA5, sizeof(deperm));
        DSD_MEMSET(deperm_rel, 0x5A, sizeof(deperm_rel));

        nxdn_depermute_rel_u8(input, reliab, 144U, PERM_16_9, deperm, deperm_rel);
        rc |= expect_matrix_deperm("deperm-16x9", input, reliab, deperm, deperm_rel, sizeof(input), 16U);
    }

    {
        uint8_t input[300];
        uint8_t reliab[300];
        uint8_t deperm[300];
        uint8_t deperm_rel[300];
        fill_input(input, reliab, sizeof(input));
        DSD_MEMSET(deperm, 0xA5, sizeof(deperm));
        DSD_MEMSET(deperm_rel, 0x5A, sizeof(deperm_rel));

        nxdn_depermute_rel(input, reliab, 300U, PERM_12_25, deperm, deperm_rel);
        rc |= expect_matrix_deperm("deperm-12x25", input, reliab, deperm, deperm_rel, sizeof(input), 12U);
    }

    {
        uint8_t input[348];
        uint8_t reliab[348];
        uint8_t deperm[348];
        uint8_t deperm_rel[348];
        fill_input(input, reliab, sizeof(input));
        DSD_MEMSET(deperm, 0xA5, sizeof(deperm));
        DSD_MEMSET(deperm_rel, 0x5A, sizeof(deperm_rel));

        nxdn_depermute_rel(input, reliab, 348U, PERM_12_29, deperm, deperm_rel);
        rc |= expect_matrix_deperm("deperm-12x29", input, reliab, deperm, deperm_rel, sizeof(input), 12U);
    }

    return rc;
}

static int
expect_depuncture_12_5_group(const uint8_t* deperm, const uint8_t* deperm_rel, const uint8_t* depunc,
                             const uint8_t* depunc_rel, size_t in_base, size_t out_base) {
    static const uint8_t map[] = {0, 1, 2, 3, 4, 0xFFU, 5, 6, 7, 8, 9, 0xFFU};
    int rc = 0;

    for (size_t i = 0U; i < sizeof(map); i++) {
        if (map[i] == 0xFFU) {
            rc |= expect_u8_at("depunc-12-5-erasure-bit", out_base + i, depunc[out_base + i], 0U);
            rc |= expect_u8_at("depunc-12-5-erasure-rel", out_base + i, depunc_rel[out_base + i], 0U);
        } else {
            rc |= expect_u8_at("depunc-12-5-bit", out_base + i, depunc[out_base + i], deperm[in_base + map[i]]);
            rc |= expect_u8_at("depunc-12-5-rel", out_base + i, depunc_rel[out_base + i], deperm_rel[in_base + map[i]]);
        }
    }
    return rc;
}

static int
test_depuncture_12_5(void) {
    uint8_t deperm[60];
    uint8_t deperm_rel[60];
    uint8_t depunc[72];
    uint8_t depunc_rel[72];
    int rc = 0;

    fill_input(deperm, deperm_rel, sizeof(deperm));
    DSD_MEMSET(depunc, 0xA5, sizeof(depunc));
    DSD_MEMSET(depunc_rel, 0x5A, sizeof(depunc_rel));

    nxdn_depuncture_12_5_rel(deperm, deperm_rel, depunc, depunc_rel);
    for (size_t group = 0U; group < 6U; group++) {
        rc |= expect_depuncture_12_5_group(deperm, deperm_rel, depunc, depunc_rel, group * 10U, group * 12U);
    }
    return rc;
}

static int
test_depuncture_16_9(void) {
    uint8_t deperm[144];
    uint8_t deperm_rel[144];
    uint8_t depunc[192];
    uint8_t depunc_rel[192];
    int rc = 0;

    fill_input(deperm, deperm_rel, sizeof(deperm));
    DSD_MEMSET(depunc, 0xA5, sizeof(depunc));
    DSD_MEMSET(depunc_rel, 0x5A, sizeof(depunc_rel));

    nxdn_depuncture_16_9_rel(deperm, deperm_rel, depunc, depunc_rel);
    for (size_t i = 0U; i < 144U; i += 3U) {
        const size_t out = (i / 3U) * 4U;
        rc |= expect_u8_at("depunc-16-9-bit-a", out, depunc[out], deperm[i]);
        rc |= expect_u8_at("depunc-16-9-rel-a", out, depunc_rel[out], deperm_rel[i]);
        rc |= expect_u8_at("depunc-16-9-erasure-bit", out + 1U, depunc[out + 1U], 0U);
        rc |= expect_u8_at("depunc-16-9-erasure-rel", out + 1U, depunc_rel[out + 1U], 0U);
        rc |= expect_u8_at("depunc-16-9-bit-b", out + 2U, depunc[out + 2U], deperm[i + 1U]);
        rc |= expect_u8_at("depunc-16-9-rel-b", out + 2U, depunc_rel[out + 2U], deperm_rel[i + 1U]);
        rc |= expect_u8_at("depunc-16-9-bit-c", out + 3U, depunc[out + 3U], deperm[i + 2U]);
        rc |= expect_u8_at("depunc-16-9-rel-c", out + 3U, depunc_rel[out + 3U], deperm_rel[i + 2U]);
    }
    return rc;
}

static int
test_depuncture_12_group(void) {
    static const uint8_t map[] = {0, 1, 2, 0xFFU, 3, 4, 5, 6, 7, 8, 9, 0xFFU, 10, 11};
    uint8_t deperm[24];
    uint8_t deperm_rel[24];
    uint8_t depunc[28];
    uint8_t depunc_rel[28];
    int rc = 0;

    fill_input(deperm, deperm_rel, sizeof(deperm));
    DSD_MEMSET(depunc, 0xA5, sizeof(depunc));
    DSD_MEMSET(depunc_rel, 0x5A, sizeof(depunc_rel));

    nxdn_depuncture_12_group_rel(deperm, deperm_rel, 2U, depunc, depunc_rel);
    for (size_t group = 0U; group < 2U; group++) {
        for (size_t i = 0U; i < sizeof(map); i++) {
            const size_t out = (group * sizeof(map)) + i;
            if (map[i] == 0xFFU) {
                rc |= expect_u8_at("depunc-12-group-erasure-bit", out, depunc[out], 0U);
                rc |= expect_u8_at("depunc-12-group-erasure-rel", out, depunc_rel[out], 0U);
            } else {
                const size_t in = (group * 12U) + map[i];
                rc |= expect_u8_at("depunc-12-group-bit", out, depunc[out], deperm[in]);
                rc |= expect_u8_at("depunc-12-group-rel", out, depunc_rel[out], deperm_rel[in]);
            }
        }
    }
    return rc;
}

static int
test_crc_helpers(void) {
    static const uint8_t empty[1] = {0U};
    static const uint8_t zeros6[6] = {0U, 0U, 0U, 0U, 0U, 0U};
    static const uint8_t ones6[6] = {1U, 1U, 1U, 1U, 1U, 1U};
    static const uint8_t pattern8[8] = {1U, 0U, 1U, 1U, 0U, 0U, 1U, 0U};
    uint8_t pattern25[25];
    uint8_t pattern26[26];
    uint8_t pattern171[171];
    int rc = 0;

    for (size_t i = 0U; i < sizeof(pattern25); i++) {
        pattern25[i] = (uint8_t)((i * 5U + 1U) & 1U);
    }
    for (size_t i = 0U; i < sizeof(pattern26); i++) {
        pattern26[i] = (uint8_t)((i * 3U + 1U) & 1U);
    }
    for (size_t i = 0U; i < sizeof(pattern171); i++) {
        pattern171[i] = (uint8_t)((i * 7U + 3U) & 1U);
    }

    rc |= expect_u8_at("crc6 empty", 0U, crc6(empty, 0), 63U);
    rc |= expect_u8_at("crc6 zeros6", 0U, crc6(zeros6, (int)sizeof(zeros6)), 24U);
    rc |= expect_u8_at("crc6 ones6", 0U, crc6(ones6, (int)sizeof(ones6)), 0U);
    rc |= expect_u8_at("crc6 pattern26", 0U, crc6(pattern26, (int)sizeof(pattern26)), 46U);

    rc |= expect_u8_at("crc7 empty", 0U, crc7_scch(empty, 0), 127U);
    rc |= expect_u8_at("crc7 pattern25", 0U, crc7_scch(pattern25, (int)sizeof(pattern25)), 16U);

    rc |= expect_u16("crc16cac empty", crc16cac(empty, 0), 0x3C11U);
    rc |= expect_u16("crc16cac pattern8", crc16cac(pattern8, (int)sizeof(pattern8)), 0xF862U);
    rc |= expect_u16("crc16cac pattern171", crc16cac(pattern171, (int)sizeof(pattern171)), 0x2608U);
    return rc;
}

static int
test_bit_window_and_state_helpers(void) {
    uint8_t trellis[8];
    static dsd_state state;
    int rc = 0;

    rc |= expect_u8_at("dcr-sb0-call", 0U, (uint8_t)nxdn_dcr_is_sb0_message_type(0x01U), 1U);
    rc |= expect_u8_at("dcr-sb0-idle", 0U, (uint8_t)nxdn_dcr_is_sb0_message_type(0x02U), 0U);
    rc |= expect_u8_at("sacch-sf2-part", 0U, (uint8_t)nxdn_sacch_part_of_frame(2U), 1U);
    rc |= expect_u8_at("sacch-sf1-part", 0U, (uint8_t)nxdn_sacch_part_of_frame(1U), 2U);
    rc |= expect_u8_at("sacch-sf0-part", 0U, (uint8_t)nxdn_sacch_part_of_frame(0U), 3U);
    rc |= expect_u8_at("sacch-sf3-invalid", 0U, (uint8_t)nxdn_sacch_part_of_frame(3U), 0U);

    DSD_MEMSET(trellis, 0, sizeof(trellis));
    trellis[2] = 1U;
    trellis[4] = 1U;
    trellis[7] = 1U;
    rc |= expect_u8_at("ran-from-trellis", 0U, (uint8_t)nxdn_ran_from_trellis(trellis), 0x29U);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.payload_miN = 0x11U;
    state.R = 0x12345U;
    nxdn_reset_payload_seed_if_forced(&state);
    rc |= expect_u8_at("seed-not-forced", 0U, (uint8_t)state.payload_miN, 0x11U);
    state.nxdn_cipher_type = 1;
    nxdn_reset_payload_seed_if_forced(&state);
    rc |= expect_u8_at("seed-forced-cipher-low", 0U, (uint8_t)state.payload_miN, 0x45U);
    state.payload_miN = 0x22U;
    state.nxdn_cipher_type = 0;
    state.M = 1;
    nxdn_reset_payload_seed_if_forced(&state);
    rc |= expect_u8_at("seed-forced-m-low", 0U, (uint8_t)state.payload_miN, 0x45U);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.payload_miN = 0x33U;
    state.R = 0x2468U;
    state.M = 1;
    nxdn_prepare_sacch_payload_seed(&state, 0);
    rc |= expect_int("prepare-part0-forced-seed", (int)state.payload_miN, 0x2468);

    state.payload_miN = 0x44U;
    state.nxdn_cipher_type = 0;
    state.M = 1;
    nxdn_prepare_sacch_payload_seed(&state, 2);
    rc |= expect_int("prepare-noncipher-nonzero-part-preserves-seed", (int)state.payload_miN, 0x44);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.R = 0x2468U;
    state.nxdn_cipher_type = 1;
    nxdn_prepare_sacch_payload_seed(&state, 2);
    rc |= expect_int("prepare-cipher-part-advances-seed", (int)state.payload_miN,
                     (int)advance_nxdn_lfsr_seed(0x2468U, 8));
    return rc;
}

static void
init_sacch2_state(dsd_state* state, Event_History_I histories[2]) {
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(histories, 0, sizeof(Event_History_I) * 2U);
    DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
    state->event_history_s = histories;
}

static void
make_sacch2_trellis(uint8_t trellis[32], uint8_t sf_fb, uint8_t sf_num, uint8_t sf_mes) {
    DSD_MEMSET(trellis, 0, 32U);
    trellis[0] = (uint8_t)(sf_fb & 1U);
    write_bits_msb(trellis, 1U, 2U, sf_num & 0x03U);
    write_bits_msb(trellis, 3U, 5U, sf_mes & 0x1FU);
}

static void
make_sacch_trellis(uint8_t trellis[32], uint8_t sf, uint8_t ran) {
    DSD_MEMSET(trellis, 0, 32U);
    write_bits_msb(trellis, 0U, 2U, sf & 0x03U);
    write_bits_msb(trellis, 2U, 6U, ran & 0x3FU);
    for (size_t i = 8U; i < 26U; i++) {
        trellis[i] = (uint8_t)((i + ran) & 1U);
    }
}

static void
make_csm_trellis(uint8_t trellis[96], const uint8_t digits[9]) {
    DSD_MEMSET(trellis, 0, 96U);
    for (size_t i = 0U; i < 9U; i++) {
        write_bits_msb(trellis, i * 4U, 4U, digits[i] & 0x0FU);
    }
}

static void
make_facch2_trellis(uint8_t trellis[208], uint8_t sf, uint8_t ran, uint8_t message_type) {
    DSD_MEMSET(trellis, 0, 208U);
    write_bits_msb(trellis, 0U, 2U, sf & 0x03U);
    write_bits_msb(trellis, 2U, 6U, ran & 0x3FU);
    write_bits_msb(trellis, 10U, 6U, message_type & 0x3FU);
}

static void
make_facch3_bits(uint8_t bits[160], uint8_t message_type) {
    DSD_MEMSET(bits, 0, 160U);
    write_bits_msb(bits, 2U, 6U, message_type & 0x3FU);
}

static int
expect_sacch_reset_to_ones(const dsd_state* state) {
    int rc = 0;
    for (size_t frame = 0U; frame < 4U; frame++) {
        rc |= expect_u8_at("sacch2-reset-crc", frame, state->nxdn_sacch_frame_segcrc[frame], 1U);
        for (size_t bit = 0U; bit < 18U; bit++) {
            rc |= expect_u8_at("sacch2-reset-segment", (frame * 18U) + bit, state->nxdn_sacch_frame_segment[frame][bit],
                               1U);
        }
    }
    return rc;
}

static int
test_sacch_state_update(void) {
    static const uint8_t m_data[5] = {0x11U, 0x22U, 0x33U, 0x44U, 0x55U};
    static dsd_opts opts;
    static dsd_state state;
    uint8_t trellis[32];
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(state.nxdn_sacch_frame_segment, 0xA5, sizeof(state.nxdn_sacch_frame_segment));
    DSD_MEMSET(state.nxdn_sacch_frame_segcrc, 0, sizeof(state.nxdn_sacch_frame_segcrc));
    state.nxdn_sacch_non_superframe = 0;
    state.nxdn_part_of_frame = 0;
    make_sacch_trellis(trellis, 2U, 0x15U);

    nxdn_handle_sacch(&opts, &state, trellis, m_data, 0x2AU, 0x2AU);
    rc |= expect_int("sacch-sf-good-part", state.nxdn_part_of_frame, 1);
    rc |= expect_int("sacch-sf-good-ran", (int)state.nxdn_ran, 0x15);
    rc |= expect_int("sacch-sf-good-last-ran", (int)state.nxdn_last_ran, 0x15);
    rc |= expect_u8_at("sacch-sf-good-segcrc", 1U, state.nxdn_sacch_frame_segcrc[1], 0U);
    for (size_t i = 0U; i < 18U; i++) {
        rc |= expect_u8_at("sacch-sf-good-segment-copy", i, state.nxdn_sacch_frame_segment[1][i], trellis[i + 8U]);
    }

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(state.nxdn_sacch_frame_segment, 0, sizeof(state.nxdn_sacch_frame_segment));
    DSD_MEMSET(state.nxdn_sacch_frame_segcrc, 0, sizeof(state.nxdn_sacch_frame_segcrc));
    state.nxdn_sacch_non_superframe = 0;
    state.nxdn_part_of_frame = 0;
    make_sacch_trellis(trellis, 1U, 0x09U);

    nxdn_handle_sacch(&opts, &state, trellis, m_data, 0x10U, 0x10U);
    rc |= expect_int("sacch-sf-out-of-order-part", state.nxdn_part_of_frame, 2);
    rc |= expect_u8_at("sacch-sf-out-of-order-reset-prior-segcrc", 0U, state.nxdn_sacch_frame_segcrc[0], 1U);
    rc |= expect_u8_at("sacch-sf-out-of-order-current-segcrc", 2U, state.nxdn_sacch_frame_segcrc[2], 0U);
    for (size_t i = 0U; i < 18U; i++) {
        rc |= expect_u8_at("sacch-sf-out-of-order-current-copy", i, state.nxdn_sacch_frame_segment[2][i],
                           trellis[i + 8U]);
    }

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(state.nxdn_sacch_frame_segment, 0, sizeof(state.nxdn_sacch_frame_segment));
    DSD_MEMSET(state.nxdn_sacch_frame_segcrc, 0, sizeof(state.nxdn_sacch_frame_segcrc));
    state.nxdn_sacch_non_superframe = 0;
    state.nxdn_part_of_frame = 1;
    make_sacch_trellis(trellis, 1U, 0x2AU);

    nxdn_handle_sacch(&opts, &state, trellis, m_data, 0x01U, 0x00U);
    rc |= expect_int("sacch-sf-bad-crc-part", state.nxdn_part_of_frame, 2);
    rc |= expect_u8_at("sacch-sf-bad-crc-segcrc", 2U, state.nxdn_sacch_frame_segcrc[2], 1U);
    rc |= expect_u8_at("sacch-sf-bad-crc-reset-other", 0U, state.nxdn_sacch_frame_segcrc[0], 1U);

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(state.nxdn_sacch_frame_segment, 0, sizeof(state.nxdn_sacch_frame_segment));
    DSD_MEMSET(state.nxdn_sacch_frame_segcrc, 0, sizeof(state.nxdn_sacch_frame_segcrc));
    state.nxdn_sacch_non_superframe = 1;
    state.nxdn_part_of_frame = 3;
    state.M = 1;
    state.R = 0x2468U;
    state.payload_miN = 0x11U;
    make_sacch_trellis(trellis, 3U, 0x3FU);

    nxdn_handle_sacch(&opts, &state, trellis, m_data, 0x01U, 0x00U);
    rc |= expect_int("sacch-nsf-bad-crc-part-reset", state.nxdn_part_of_frame, 0);
    rc |= expect_int("sacch-nsf-bad-crc-forced-seed", (int)state.payload_miN, 0x2468);
    rc |= expect_sacch_reset_to_ones(&state);
    return rc;
}

static int
test_sacch2_state_update(void) {
    static const uint8_t m_data[5] = {0xA1U, 0xB2U, 0xC3U, 0xD4U, 0xE5U};
    static Event_History_I histories[2];
    static dsd_opts opts;
    static dsd_state state;
    uint8_t trellis[32];
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    init_sacch2_state(&state, histories);
    state.M = 1;
    state.payload_miN = 0x1234U;
    make_sacch2_trellis(trellis, 1U, 2U, 0x01U);
    write_bits_msb(trellis, 8U, 2U, 0x01U);
    write_bits_msb(trellis, 10U, 9U, 0x123U);

    nxdn_handle_sacch2(&opts, &state, trellis, m_data, 0x2AU, 0x2AU);
    rc |= expect_u8_at("sacch2-good-message-type", 0U, state.nxdn_dcr_sf_message_type, 0x01U);
    rc |= expect_u8_at("sacch2-good-segcrc", 2U, state.nxdn_sacch_frame_segcrc[2], 0U);
    rc |= expect_u8_at("sacch2-single-copy-cipher-msb", 0U, state.dmr_pdu_sf[0][0], 0U);
    rc |= expect_u8_at("sacch2-single-copy-cipher-lsb", 1U, state.dmr_pdu_sf[0][1], 1U);
    rc |= expect_int("sacch2-single-payload-seed-clear", (int)state.payload_miN, 0);
    rc |= expect_int("sacch2-single-last-ran", (int)state.nxdn_last_ran, 7);
    dsd_call_snapshot call;
    rc |= expect_int("sacch2-single-canonical", dsd_call_state_get(&state, 0U, &call), 1);
    rc |= expect_int("sacch2-single-target", (int)call.ota_target_id, 777);
    rc |= expect_int("sacch2-single-source", (int)call.ota_source_id, 777);
    rc |= expect_str("sacch2-single-alias", state.generic_talker_alias[0], "JPN DCR");
    rc |= expect_str("sacch2-single-event-alias", histories[0].Event_History_Items[0].alias, "JPN DCR; ");
    rc |= expect_int("sacch2-single-event-revision", (int)histories[0].revision, 1);
    rc |= expect_int("sacch2-single-cipher", state.nxdn_cipher_type, 1);
    rc |= expect_int("sacch2-single-enc-lockout", state.dmr_encL, 1);

    dsd_state_ext_free_all(&state);
    init_sacch2_state(&state, histories);
    DSD_MEMSET(state.nxdn_sacch_frame_segment, 0, sizeof(state.nxdn_sacch_frame_segment));
    DSD_MEMSET(state.nxdn_sacch_frame_segcrc, 0, sizeof(state.nxdn_sacch_frame_segcrc));
    DSD_MEMSET(state.dmr_pdu_sf[0], 0xA5, sizeof(state.dmr_pdu_sf[0]));
    make_sacch2_trellis(trellis, 0U, 0U, 0x02U);
    write_bits_msb(trellis, 8U, 2U, 0x00U);
    write_bits_msb(trellis, 10U, 9U, 0x055U);

    nxdn_handle_sacch2(&opts, &state, trellis, m_data, 0x11U, 0x11U);
    rc |= expect_u8_at("sacch2-complete-message-type", 0U, state.nxdn_dcr_sf_message_type, 0x02U);
    rc |= expect_sacch_reset_to_ones(&state);
    rc |= expect_u8_at("sacch2-complete-clears-pdu", 0U, state.dmr_pdu_sf[0][0], 0U);
    rc |= expect_str("sacch2-complete-alias", state.generic_talker_alias[0], "JPN DCR");

    dsd_state_ext_free_all(&state);
    init_sacch2_state(&state, histories);
    state.M = 1;
    state.payload_miN = 0x55U;
    make_sacch2_trellis(trellis, 1U, 2U, 0x1EU);

    nxdn_handle_sacch2(&opts, &state, trellis, m_data, 0x01U, 0x00U);
    rc |= expect_u8_at("sacch2-bad-crc-message-type", 0U, state.nxdn_dcr_sf_message_type, 0xFFU);
    rc |= expect_u8_at("sacch2-bad-crc-segcrc", 2U, state.nxdn_sacch_frame_segcrc[2], 1U);
    rc |= expect_int("sacch2-bad-crc-seed-clear", (int)state.payload_miN, 0);
    rc |= expect_int("sacch2-bad-crc-no-call", dsd_call_state_get(&state, 0U, &call), 0);
    rc |= expect_str("sacch2-bad-crc-no-alias", state.generic_talker_alias[0], "");
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_cac_crc_failure_reset(void) {
    static const uint8_t m_data[22] = {0};
    static dsd_opts opts;
    static dsd_state state;
    uint8_t trellis[176];
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(trellis, 0, sizeof(trellis));
    write_bits_msb(trellis, 2U, 6U, 0x2DU);

    state.synctype = DSD_SYNC_NXDN_POS;
    state.lastsynctype = DSD_SYNC_NXDN_POS;
    state.carrier = 1;
    state.center = 5.0f;
    state.jitter = 9;
    state.min = -9.0f;
    state.max = 9.0f;
    state.dibit_buf_p = state.dibit_buf + 42;
    state.symbolcnt = 99;
    state.offset = 7;
    state.data_header_blocks[0] = 7;
    state.data_header_padding[0] = 3;
    state.data_header_format[0] = 9;
    state.data_header_valid[0] = 1;
    state.payload_algid = 0xAAU;
    state.payload_keyid = 0xBBBBU;
    state.payload_mi = 0xCCCCU;
    state.aes_ivR[0] = 0x5AU;

    for (int i = 0; i < 10; i++) {
        nxdn_handle_cac(&opts, &state, trellis, m_data, 1U);
    }
    rc |= expect_int("cac-before-threshold-carrier", state.carrier, 1);
    rc |= expect_int("cac-before-threshold-format", state.data_header_format[0], 9);

    nxdn_handle_cac(&opts, &state, trellis, m_data, 1U);
    rc |= expect_int("cac-reset-synctype", state.synctype, DSD_SYNC_NONE);
    rc |= expect_int("cac-reset-last-sync", state.lastsynctype, DSD_SYNC_NONE);
    rc |= expect_int("cac-reset-carrier", state.carrier, 0);
    rc |= expect_int("cac-reset-jitter", state.jitter, -1);
    rc |= expect_int("cac-reset-symbolcnt", state.symbolcnt, 0);
    rc |= expect_int("cac-reset-offset", state.offset, 0);
    rc |= expect_int("cac-reset-dibit-pointer", (int)(state.dibit_buf_p - state.dibit_buf), 200);
    rc |= expect_int("cac-reset-data-blocks", state.data_header_blocks[0], 1);
    rc |= expect_int("cac-reset-data-padding", state.data_header_padding[0], 0);
    rc |= expect_int("cac-reset-data-format", state.data_header_format[0], 0);
    rc |= expect_int("cac-reset-data-valid", state.data_header_valid[0], 0);
    rc |= expect_int("cac-reset-payload-algid", state.payload_algid, 0);
    rc |= expect_int("cac-reset-payload-keyid", state.payload_keyid, 0);
    rc |= expect_int("cac-reset-payload-mi", (int)state.payload_mi, 0);
    rc |= expect_u8_at("cac-reset-aes-iv", 0U, state.aes_ivR[0], 0U);

    nxdn_handle_cac(&opts, &state, trellis, m_data, 1U);
    rc |= expect_int("cac-counter-cleared-after-reset", state.carrier, 0);
    return rc;
}

static int
test_pich_tch_dcr_csm_alias_state(void) {
    static const uint8_t csm_digits[9] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U};
    static const uint8_t m_data[12] = {0};
    static Event_History_I histories[2];
    static dsd_opts opts;
    static dsd_state state;
    uint8_t trellis[96];
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(histories, 0, sizeof(histories));
    state.event_history_s = histories;
    state.nxdn_dcr_sf_message_type = 0x01U;
    make_csm_trellis(trellis, csm_digits);

    nxdn_handle_pich_tch(&opts, &state, trellis, m_data, 0x123U, 0x123U, 0x08U);
    rc |= expect_str("pich-csm-alias", state.generic_talker_alias[0], "CSM 123456789");
    rc |= expect_str("pich-csm-event-alias", histories[0].Event_History_Items[0].alias, "CSM 123456789; ");
    rc |= expect_int("pich-csm-event-revision", (int)histories[0].revision, 1);

    DSD_SNPRINTF(state.generic_talker_alias[0], sizeof(state.generic_talker_alias[0]), "%s", "KEEP");
    DSD_SNPRINTF(histories[0].Event_History_Items[0].alias, sizeof(histories[0].Event_History_Items[0].alias), "%s",
                 "KEEP; ");
    state.nxdn_dcr_sf_message_type = 0x02U;

    nxdn_handle_pich_tch(&opts, &state, trellis, m_data, 0x123U, 0x123U, 0x08U);
    rc |= expect_str("pich-non-sb0-keeps-alias", state.generic_talker_alias[0], "KEEP");
    rc |= expect_str("pich-non-sb0-keeps-event-alias", histories[0].Event_History_Items[0].alias, "KEEP; ");
    return rc;
}

static int
test_facch2_udch_crc_state_update(void) {
    static const uint8_t m_data[26] = {0};
    static dsd_opts opts;
    static dsd_state state;
    uint8_t trellis[208];
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    make_facch2_trellis(trellis, 1U, 0x2AU, 0x10U);

    nxdn_handle_facch2_udch(&opts, &state, trellis, m_data, 0x456U, 0x456U, 1U);
    rc |= expect_int("facch2-good-ran", (int)state.nxdn_last_ran, 0x2A);
    rc |= expect_int("facch2-good-part", state.nxdn_part_of_frame, 2);
    rc |= expect_u8_at("facch2-good-format", 0U, state.data_header_format[0], 1U);
    rc |= expect_u8_at("facch2-good-message-type", 0U, state.NxdnElementsContent.MessageType, 0x10U);
    rc |= expect_u8_at("facch2-good-crc", 0U, state.NxdnElementsContent.VCallCrcIsGood, 1U);

    state.nxdn_last_ran = 0x33U;
    state.nxdn_part_of_frame = 7;
    state.data_header_format[0] = 9U;
    state.NxdnElementsContent.MessageType = 0x22U;
    state.NxdnElementsContent.VCallCrcIsGood = 0U;

    nxdn_handle_facch2_udch(&opts, &state, trellis, m_data, 0x456U, 0x457U, 1U);
    rc |= expect_int("facch2-bad-keeps-ran", (int)state.nxdn_last_ran, 0x33);
    rc |= expect_int("facch2-bad-part-reset", state.nxdn_part_of_frame, 0);
    rc |= expect_u8_at("facch2-bad-keeps-format", 0U, state.data_header_format[0], 9U);
    rc |= expect_u8_at("facch2-bad-skips-elements", 0U, state.NxdnElementsContent.MessageType, 0x22U);
    return rc;
}

static int
test_facch3_udch2_crc_state_update(void) {
    struct nxdn_facch3_udch2_message message;
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&message, 0, sizeof(message));
    for (size_t i = 0U; i < sizeof(message.bytes); i++) {
        message.bytes[i] = (uint8_t)(0x30U + i);
    }

    DSD_MEMSET(&state, 0, sizeof(state));
    make_facch3_bits(message.bits, 0x10U);

    message.crc[0] = message.check[0] = 0x123U;
    message.crc[1] = message.check[1] = 0x456U;
    nxdn_handle_facch3_udch2_soft(&opts, &state, &message, 1U);
    rc |= expect_u8_at("facch3-good-format", 0U, state.data_header_format[0], 1U);
    rc |= expect_u8_at("facch3-good-message-type", 0U, state.NxdnElementsContent.MessageType, 0x10U);
    rc |= expect_u8_at("facch3-good-crc", 0U, state.NxdnElementsContent.VCallCrcIsGood, 1U);

    state.data_header_format[0] = 9U;
    state.NxdnElementsContent.MessageType = 0x22U;
    state.NxdnElementsContent.VCallCrcIsGood = 0U;

    message.check[0] = 0x124U;
    nxdn_handle_facch3_udch2_soft(&opts, &state, &message, 0U);
    rc |= expect_u8_at("udch2-bad-keeps-format", 0U, state.data_header_format[0], 9U);
    rc |= expect_u8_at("udch2-bad-skips-elements", 0U, state.NxdnElementsContent.MessageType, 0x22U);
    rc |= expect_u8_at("udch2-bad-keeps-crc", 0U, state.NxdnElementsContent.VCallCrcIsGood, 0U);
    return rc;
}

static int
test_facch3_udch2_split_block_storage_and_crc_gate(void) {
    struct nxdn_facch3_udch2_message message;
    uint8_t trellis0[96];
    uint8_t trellis1[96];
    uint8_t m_data0[12];
    uint8_t m_data1[12];
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&message, 0, sizeof(message));
    DSD_MEMSET(message.bits, 0xA5, sizeof(message.bits));
    DSD_MEMSET(message.bytes, 0x5A, sizeof(message.bytes));
    for (size_t i = 0U; i < sizeof(trellis0); i++) {
        trellis0[i] = (uint8_t)((i + 1U) & 1U);
        trellis1[i] = (uint8_t)(i & 1U);
    }
    write_bits_msb(trellis0, 2U, 6U, 0x10U);
    for (size_t i = 0U; i < sizeof(m_data0); i++) {
        m_data0[i] = (uint8_t)(0x20U + i);
        m_data1[i] = (uint8_t)(0x80U + i);
    }

    nxdn_store_facch3_udch2_block(&message, 1U, trellis1, m_data1);
    for (size_t i = 0U; i < 80U; i++) {
        rc |= expect_u8_at("facch3-split-first-half-untouched", i, message.bits[i], 0xA5U);
        rc |= expect_u8_at("facch3-split-second-half", i, message.bits[i + 80U], trellis1[i]);
    }
    for (size_t i = 0U; i < 12U; i++) {
        rc |= expect_u8_at("facch3-split-first-bytes-untouched", i, message.bytes[i], 0x5AU);
        rc |= expect_u8_at("facch3-split-second-bytes", i, message.bytes[i + 12U], m_data1[i]);
    }

    nxdn_store_facch3_udch2_block(&message, 0U, trellis0, m_data0);
    for (size_t i = 0U; i < 80U; i++) {
        rc |= expect_u8_at("facch3-split-first-half", i, message.bits[i], trellis0[i]);
        rc |= expect_u8_at("facch3-split-second-half-preserved", i, message.bits[i + 80U], trellis1[i]);
    }
    for (size_t i = 0U; i < 12U; i++) {
        rc |= expect_u8_at("facch3-split-first-bytes", i, message.bytes[i], m_data0[i]);
        rc |= expect_u8_at("facch3-split-second-bytes-preserved", i, message.bytes[i + 12U], m_data1[i]);
    }

    DSD_MEMSET(&state, 0, sizeof(state));
    message.crc[0] = message.check[0] = 0x111U;
    message.crc[1] = message.check[1] = 0x222U;
    nxdn_handle_facch3_udch2_soft(&opts, &state, &message, 1U);
    rc |= expect_u8_at("facch3-split-good-format", 0U, state.data_header_format[0], 1U);
    rc |= expect_u8_at("facch3-split-good-message-type", 0U, state.NxdnElementsContent.MessageType, 0x10U);
    rc |= expect_u8_at("facch3-split-good-crc", 0U, state.NxdnElementsContent.VCallCrcIsGood, 1U);

    state.data_header_format[0] = 9U;
    state.NxdnElementsContent.MessageType = 0x22U;
    state.NxdnElementsContent.VCallCrcIsGood = 0U;
    message.check[1] = 0x223U;
    nxdn_handle_facch3_udch2_soft(&opts, &state, &message, 0U);
    rc |= expect_u8_at("udch2-split-bad-keeps-format", 0U, state.data_header_format[0], 9U);
    rc |= expect_u8_at("udch2-split-bad-keeps-message-type", 0U, state.NxdnElementsContent.MessageType, 0x22U);
    rc |= expect_u8_at("udch2-split-bad-keeps-crc", 0U, state.NxdnElementsContent.VCallCrcIsGood, 0U);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_depermute_matrices();
    rc |= test_depuncture_12_5();
    rc |= test_depuncture_16_9();
    rc |= test_depuncture_12_group();
    rc |= test_crc_helpers();
    rc |= test_bit_window_and_state_helpers();
    rc |= test_sacch_state_update();
    rc |= test_sacch2_state_update();
    rc |= test_cac_crc_failure_reset();
    rc |= test_pich_tch_dcr_csm_alias_state();
    rc |= test_facch2_udch_crc_state_update();
    rc |= test_facch3_udch2_crc_state_update();
    rc |= test_facch3_udch2_split_block_storage_and_crc_gate();

    if (rc == 0) {
        printf("NXDN_DEPERM_PRIMITIVES: OK\n");
    }
    return rc;
}
