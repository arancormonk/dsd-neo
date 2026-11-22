// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression checks for NXDN deinterleave and depuncture primitives shared by
 * SACCH, FACCH, CAC, FACCH2, and FACCH3 soft-decision paths.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/nxdn/nxdn_const.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "nxdn_confirm.h"
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

/* The NXDN access code a call history row takes from the state now (issue #575): RAN `want_ran` when `want_valid`,
 * else none. */
static int
expect_nxdn_access_code(const char* tag, const dsd_state* state, int want_valid, uint16_t want_ran) {
    uint8_t kind = 0xA5U;
    uint16_t value = 0xBEEFU;
    const int valid = dsd_access_code_current(state, DSD_SYNC_NXDN_POS, 0U, 0U, &kind, &value);
    const uint8_t want_kind = want_valid ? (uint8_t)DSD_ACCESS_CODE_RAN : (uint8_t)DSD_ACCESS_CODE_NONE;
    const uint16_t want_value = want_valid ? want_ran : 0U;
    if (valid != want_valid || kind != want_kind || value != want_value) {
        DSD_FPRINTF(stderr, "%s: got valid=%d kind=%u value=%u want valid=%d kind=%u value=%u\n", tag, valid,
                    (unsigned)kind, (unsigned)value, want_valid, (unsigned)want_kind, (unsigned)want_value);
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
    state.nxdn_payload_mi = 0x11U;
    state.R = 0x12345U;
    nxdn_reset_payload_seed_if_forced(&state);
    rc |= expect_u8_at("seed-not-forced", 0U, (uint8_t)state.nxdn_payload_mi, 0x11U);
    state.nxdn_cipher_type = 1;
    nxdn_reset_payload_seed_if_forced(&state);
    rc |= expect_u8_at("seed-forced-cipher-low", 0U, (uint8_t)state.nxdn_payload_mi, 0x45U);
    state.nxdn_payload_mi = 0x22U;
    state.nxdn_cipher_type = 0;
    state.M = 1;
    nxdn_reset_payload_seed_if_forced(&state);
    rc |= expect_u8_at("seed-forced-m-low", 0U, (uint8_t)state.nxdn_payload_mi, 0x45U);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.nxdn_payload_mi = 0x33U;
    state.R = 0x2468U;
    state.M = 1;
    nxdn_prepare_sacch_payload_seed(&state, 0);
    rc |= expect_int("prepare-part0-forced-seed", (int)state.nxdn_payload_mi, 0x2468);

    state.nxdn_payload_mi = 0x44U;
    state.nxdn_cipher_type = 0;
    state.M = 1;
    nxdn_prepare_sacch_payload_seed(&state, 2);
    rc |= expect_int("prepare-noncipher-nonzero-part-preserves-seed", (int)state.nxdn_payload_mi, 0x44);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.R = 0x2468U;
    state.nxdn_cipher_type = 1;
    nxdn_prepare_sacch_payload_seed(&state, 2);
    rc |= expect_int("prepare-cipher-part-advances-seed", (int)state.nxdn_payload_mi,
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

    /* A 6-bit CRC is not on its own evidence that this is a transmission, so an
     * unconfirmed frame decodes without publishing a RAN (issue #398). */
    nxdn_handle_sacch(&opts, &state, trellis, m_data, 0x2AU, 0x2AU);
    rc |= expect_int("sacch-sf-unconfirmed-part", state.nxdn_part_of_frame, 1);
    rc |= expect_int("sacch-sf-unconfirmed-ran", (int)state.nxdn_ran, 0);
    rc |= expect_int("sacch-sf-unconfirmed-last-ran", (int)state.nxdn_last_ran, 0);

    /* A second consecutive SACCH confirms the transmission, and the RAN goes out.
     * nxdn_confirm_begin_frame() is the frame boundary nxdn_frame() supplies around each
     * call; without it several short CRCs would count as one frame's worth of evidence. */
    nxdn_confirm_begin_frame(&state);
    state.nxdn_part_of_frame = 0;
    nxdn_handle_sacch(&opts, &state, trellis, m_data, 0x2AU, 0x2AU);
    rc |= expect_int("sacch-sf-good-part", state.nxdn_part_of_frame, 1);
    rc |= expect_int("sacch-sf-good-ran", (int)state.nxdn_ran, 0x15);
    rc |= expect_int("sacch-sf-good-last-ran", (int)state.nxdn_last_ran, 0x15);
    rc |= expect_nxdn_access_code("sacch-sf-good-access-code", &state, 1, 0x15U);
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
    state.nxdn_payload_mi = 0x11U;
    make_sacch_trellis(trellis, 3U, 0x3FU);

    /* A failed SACCH CRC must keep the frame out of the element decoder entirely: the caller is
     * the gate, since the decoder itself carries no CRC verdict (issue #411). The sentinel is
     * what the decoder would overwrite with the real message type had it run. */
    state.NxdnElementsContent.MessageType = 0x22U;

    nxdn_handle_sacch(&opts, &state, trellis, m_data, 0x01U, 0x00U);
    rc |= expect_int("sacch-nsf-bad-crc-part-reset", state.nxdn_part_of_frame, 0);
    rc |= expect_int("sacch-nsf-bad-crc-forced-seed", (int)state.nxdn_payload_mi, 0x2468);
    rc |= expect_u8_at("sacch-nsf-bad-crc-skips-elements", 0U, state.NxdnElementsContent.MessageType, 0x22U);
    rc |= expect_sacch_reset_to_ones(&state);

    /* The same frame with a passing CRC does reach the decoder. IDLE (0x10) sits at bits 2..7 of
     * the 24-bit NSF payload the handler copies from trellis[8..31]. */
    DSD_MEMSET(&state, 0, sizeof(state));
    state.nxdn_sacch_non_superframe = 1;
    make_sacch_trellis(trellis, 3U, 0x3FU);
    write_bits_msb(trellis, 10U, 6U, 0x10U);
    state.NxdnElementsContent.MessageType = 0x22U;

    nxdn_handle_sacch(&opts, &state, trellis, m_data, 0x2AU, 0x2AU);
    rc |= expect_u8_at("sacch-nsf-good-message-type", 0U, state.NxdnElementsContent.MessageType, 0x10U);

    /* The 26 elements a non-superframe SACCH carries hold no SRV_INFO (72 elements), and one passing 6-bit CRC has
     * not confirmed the transmission: the decoder keeps no location and no RAN from it. */
    DSD_MEMSET(&state, 0, sizeof(state));
    state.nxdn_sacch_non_superframe = 1;
    state.nxdn_last_ran = (unsigned int)-1;
    DSD_SNPRINTF(state.nxdn_location_category, sizeof(state.nxdn_location_category), "%s", " ");
    make_sacch_trellis(trellis, 3U, 0x3FU);
    write_bits_msb(trellis, 10U, 6U, 0x19U);
    write_bits_msb(trellis, 16U, 16U, 0x1234U);

    nxdn_handle_sacch(&opts, &state, trellis, m_data, 0x2AU, 0x2AU);
    rc |= expect_u8_at("sacch-nsf-srv-info-message-type", 0U, state.NxdnElementsContent.MessageType, 0x19U);
    rc |= expect_int("sacch-nsf-srv-info-no-ran", (int)state.nxdn_last_ran, -1);
    rc |= expect_int("sacch-nsf-srv-info-no-site", state.nxdn_location_site_code, 0);
    rc |= expect_int("sacch-nsf-srv-info-no-sys", (int)state.nxdn_location_sys_code, 0);
    rc |= expect_int("sacch-nsf-srv-info-no-category", state.nxdn_location_category[0] == ' ' ? 1 : 0, 1);
    return rc;
}

/* One confirmed DCR SB0 frame (sf_mes 0x01) on @p state. */
static void
dcr_sb0(dsd_state* state) {
    static const uint8_t m_data[5] = {0xA1U, 0xB2U, 0xC3U, 0xD4U, 0xE5U};
    static dsd_opts opts;
    uint8_t trellis[32];
    make_sacch2_trellis(trellis, 1U, 0U, 0x01U);
    nxdn_confirm_begin_frame(state);
    nxdn_handle_sacch2(&opts, state, trellis, m_data, 0x2AU, 0x2AU);
}

/* A DCR call ("JPN DCR", no numeric party) and a named NXDN call are two calls (issue #903), however they meet: DCR
 * after a named call whose release was missed, DCR within the 0.5 s reacquisition window after a named call ended by
 * sync loss, and a named VCALL after a DCR call. The SB0s of one DCR call continue it. */
static int
test_sacch2_dcr_and_named_calls_stay_apart(void) {
    static Event_History_I histories[2];
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (state == NULL) {
        return 1;
    }
    int rc = 0;
    const dsd_call_observation named = {
        .protocol = DSD_SYNC_NXDN_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 500U,
        .policy_target_id = 500U,
        .ota_source_id = 123U,
    };
    dsd_call_snapshot call;
    for (int sync_loss = 0; sync_loss <= 1; sync_loss++) {
        init_sacch2_state(state, histories);
        dsd_decode_clock_test_set_ns(1000000000000ULL);
        rc |= expect_int("dcr-named-opens", dsd_call_state_observe(state, &named, DSD_CALL_BOUNDARY_BEGIN), 1);
        (void)dsd_call_state_get(state, 0U, &call);
        const uint64_t named_epoch = call.epoch;
        if (sync_loss) {
            (void)dsd_call_state_end_ex(state, 0U, dsd_decode_now_mono_s(), DSD_CALL_END_SYNC_LOSS);
            dsd_decode_clock_test_set_ns(1000200000000ULL); /* 0.2 s later: inside the reacquisition window */
        }
        dcr_sb0(state);
        dcr_sb0(state);
        const char* tag = sync_loss ? "dcr-after-sync-loss" : "dcr-after-missed-release";
        char label[96];
        DSD_SNPRINTF(label, sizeof label, "%s-call", tag);
        rc |= expect_int(label, dsd_call_state_get(state, 0U, &call), 1);
        DSD_SNPRINTF(label, sizeof label, "%s-new-epoch", tag);
        rc |= expect_int(label, call.epoch != named_epoch && call.phase == DSD_CALL_PHASE_ACTIVE, 1);
        DSD_SNPRINTF(label, sizeof label, "%s-target", tag);
        rc |= expect_int(label, (int)call.ota_target_id, 0);
        DSD_SNPRINTF(label, sizeof label, "%s-source", tag);
        rc |= expect_int(label, (int)call.ota_source_id, 0);
        DSD_SNPRINTF(label, sizeof label, "%s-text", tag);
        rc |= expect_str(label, call.target_text, "JPN DCR");
        const uint64_t dcr_epoch = call.epoch;
        dcr_sb0(state);
        (void)dsd_call_state_get(state, 0U, &call);
        DSD_SNPRINTF(label, sizeof label, "%s-continues", tag);
        rc |= expect_int(label, call.epoch == dcr_epoch, 1);

        /* A named call after the DCR call is another call too. */
        rc |= expect_int("named-after-dcr-observed",
                         dsd_call_state_observe(state, &named, DSD_CALL_BOUNDARY_CONTINUE) >= 0, 1);
        (void)dsd_call_state_get(state, 0U, &call);
        rc |= expect_int("named-after-dcr-new-epoch", call.epoch != dcr_epoch && call.ota_target_id == 500U, 1);
        rc |= expect_str("named-after-dcr-no-dcr-text", call.target_text, "");
        dsd_state_ext_free_all(state);
    }
    dsd_decode_clock_use_system();
    free(state);
    return rc;
}

/* A DCR call and an NXDN call known only by its source (an IDAS INFO3's talker, a VCALL to the null destination) are two
 * calls too (issue #903), each way: DCR after such a call whose release was missed or that sync loss ended 0.2 s
 * before, and such a call after a DCR call, never carry the other's talker or "JPN DCR". A provisional voice call that
 * names no party at all still takes DCR's name in its own epoch. */
static int
test_sacch2_dcr_and_source_only_calls_stay_apart(void) {
    static Event_History_I histories[2];
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (state == NULL) {
        return 1;
    }
    int rc = 0;
    static const dsd_call_kind k_kinds[] = {DSD_CALL_KIND_GROUP_VOICE, DSD_CALL_KIND_VOICE};
    dsd_call_snapshot call;
    char label[96];
    for (size_t k = 0U; k < sizeof(k_kinds) / sizeof(k_kinds[0]); k++) {
        const dsd_call_observation source_only = {
            .protocol = DSD_SYNC_NXDN_POS,
            .slot = 0U,
            .kind = k_kinds[k],
            .ota_source_id = 123U,
        };
        for (int sync_loss = 0; sync_loss <= 1; sync_loss++) {
            init_sacch2_state(state, histories);
            dsd_decode_clock_test_set_ns(1000000000000ULL);
            (void)dsd_call_state_observe(state, &source_only, DSD_CALL_BOUNDARY_BEGIN);
            (void)dsd_call_state_get(state, 0U, &call);
            const uint64_t named_epoch = call.epoch;
            if (sync_loss) {
                (void)dsd_call_state_end_ex(state, 0U, dsd_decode_now_mono_s(), DSD_CALL_END_SYNC_LOSS);
                dsd_decode_clock_test_set_ns(1000200000000ULL); /* 0.2 s later: inside the reacquisition window */
            }
            dcr_sb0(state);
            dcr_sb0(state);
            const char* tag = sync_loss ? "dcr-after-source-only-sync-loss" : "dcr-after-source-only-missed-release";
            DSD_SNPRINTF(label, sizeof label, "%s-%zu-call", tag, k);
            rc |= expect_int(label, dsd_call_state_get(state, 0U, &call), 1);
            DSD_SNPRINTF(label, sizeof label, "%s-%zu-new-epoch", tag, k);
            rc |= expect_int(label, call.epoch != named_epoch && call.phase == DSD_CALL_PHASE_ACTIVE, 1);
            DSD_SNPRINTF(label, sizeof label, "%s-%zu-source", tag, k);
            rc |= expect_int(label, (int)call.ota_source_id, 0);
            DSD_SNPRINTF(label, sizeof label, "%s-%zu-text", tag, k);
            rc |= expect_str(label, call.target_text, "JPN DCR");

            /* A source-only call after the DCR call is another call. */
            const uint64_t dcr_epoch = call.epoch;
            (void)dsd_call_state_observe(state, &source_only, DSD_CALL_BOUNDARY_CONTINUE);
            (void)dsd_call_state_get(state, 0U, &call);
            DSD_SNPRINTF(label, sizeof label, "%s-%zu-source-only-after-dcr-new-epoch", tag, k);
            rc |= expect_int(label, call.epoch != dcr_epoch && call.ota_source_id == 123U, 1);
            DSD_SNPRINTF(label, sizeof label, "%s-%zu-source-only-after-dcr-no-dcr-text", tag, k);
            rc |= expect_str(label, call.target_text, "");
            dsd_state_ext_free_all(state);
        }
    }

    /* A provisional voice call naming nobody takes DCR's name as its own. */
    init_sacch2_state(state, histories);
    dsd_decode_clock_test_set_ns(1000000000000ULL);
    const dsd_call_observation provisional = {
        .protocol = DSD_SYNC_NXDN_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    (void)dsd_call_state_observe(state, &provisional, DSD_CALL_BOUNDARY_BEGIN);
    rc |= expect_int("dcr-provisional-call", dsd_call_state_get(state, 0U, &call), 1);
    const uint64_t provisional_epoch = call.epoch;
    dcr_sb0(state);
    dcr_sb0(state);
    (void)dsd_call_state_get(state, 0U, &call);
    rc |= expect_int("dcr-provisional-same-epoch", call.epoch == provisional_epoch, 1);
    rc |= expect_str("dcr-provisional-text", call.target_text, "JPN DCR");
    dsd_state_ext_free_all(state);
    dsd_decode_clock_use_system();
    free(state);
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
    state.nxdn_payload_mi = 0x1234U;
    make_sacch2_trellis(trellis, 1U, 2U, 0x01U);
    write_bits_msb(trellis, 8U, 2U, 0x01U);
    write_bits_msb(trellis, 10U, 9U, 0x123U);

    /* First short-CRC frame: decoded, but no RAN and no call row yet (issue #398). */
    nxdn_handle_sacch2(&opts, &state, trellis, m_data, 0x2AU, 0x2AU);
    dsd_call_snapshot unconfirmed_call;
    rc |= expect_int("sacch2-unconfirmed-last-ran", (int)state.nxdn_last_ran, 0);
    rc |= expect_int("sacch2-unconfirmed-canonical", dsd_call_state_get(&state, 0U, &unconfirmed_call), 0);
    /* Nor its scrambler: a single-frame message's 7-bit CRC passes noise one time in 128. */
    rc |= expect_int("sacch2-unconfirmed-cipher", state.nxdn_cipher_type, 0);
    rc |= expect_int("sacch2-unconfirmed-enc-lockout", state.dmr_encL, 0);

    nxdn_confirm_begin_frame(&state);
    nxdn_handle_sacch2(&opts, &state, trellis, m_data, 0x2AU, 0x2AU);
    rc |= expect_u8_at("sacch2-good-message-type", 0U, state.nxdn_dcr_sf_message_type, 0x01U);
    rc |= expect_u8_at("sacch2-good-segcrc", 2U, state.nxdn_sacch_frame_segcrc[2], 0U);
    rc |= expect_u8_at("sacch2-single-copy-cipher-msb", 0U, state.dmr_pdu_sf[0][0], 0U);
    rc |= expect_u8_at("sacch2-single-copy-cipher-lsb", 1U, state.dmr_pdu_sf[0][1], 1U);
    rc |= expect_int("sacch2-single-payload-seed-clear", (int)state.nxdn_payload_mi, 0);
    rc |= expect_int("sacch2-single-last-ran", (int)state.nxdn_last_ran, 7);
    /* The 7 is DCR's stand-in, which the terminal shows where a RAN goes; DCR carries no RAN, so the call has no
     * access code (issue #575). */
    rc |= expect_nxdn_access_code("sacch2-single-no-access-code", &state, 0, 0U);
    dsd_call_snapshot call;
    rc |= expect_int("sacch2-single-canonical", dsd_call_state_get(&state, 0U, &call), 1);
    /* DCR carries no decoded group or radio: the call is identity-less voice, never a made-up group (issue #903). */
    rc |= expect_int("sacch2-single-kind", call.kind, DSD_CALL_KIND_VOICE);
    /* Named by text, as a callsign names a call: shown and kept, and never another call's (issue #903). */
    rc |= expect_str("sacch2-single-target-text", call.target_text, "JPN DCR");
    rc |= expect_int("sacch2-single-target", (int)call.ota_target_id, 0);
    rc |= expect_int("sacch2-single-policy-target", (int)call.policy_target_id, 0);
    rc |= expect_int("sacch2-single-source", (int)call.ota_source_id, 0);
    rc |= expect_int("sacch2-single-canonical-crypto", call.crypto, DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_int("sacch2-single-canonical-algid", call.algid, 1);
    rc |= expect_int("sacch2-single-canonical-key", call.kid, 0);
    rc |= expect_int("sacch2-single-canonical-mi", (int)call.mi, 0);
    rc |= expect_int("sacch2-single-canonical-audio", call.audio_permitted, 0);
    rc |= expect_str("sacch2-single-alias", state.generic_talker_alias[0], "JPN DCR");
    rc |= expect_int("sacch2-single-event-protocol", histories[0].Event_History_Items[0].systype, DSD_SYNC_NXDN_POS);
    rc |= expect_int("sacch2-single-event-target", histories[0].Event_History_Items[0].target_id, 0);
    rc |= expect_str("sacch2-single-event-target-text", histories[0].Event_History_Items[0].tgt_str, "JPN DCR");
    rc |= expect_int("sacch2-single-event-source", histories[0].Event_History_Items[0].source_id, 0);
    rc |= expect_int("sacch2-single-event-encrypted", histories[0].Event_History_Items[0].enc, 1);
    rc |= expect_int("sacch2-single-event-algid", histories[0].Event_History_Items[0].enc_alg, 1);
    rc |= expect_int("sacch2-single-event-key", histories[0].Event_History_Items[0].enc_key, 0);
    rc |= expect_int("sacch2-single-event-mi", (int)histories[0].Event_History_Items[0].mi, 0);
    rc |= expect_str("sacch2-single-event-alias", histories[0].Event_History_Items[0].alias, "JPN DCR");
    rc |= expect_int("sacch2-single-event-no-access-code", histories[0].Event_History_Items[0].access_code_kind,
                     DSD_ACCESS_CODE_NONE);
    rc |= expect_int("sacch2-single-event-no-access-code-value", histories[0].Event_History_Items[0].access_code, 0);
    rc |= expect_int("sacch2-single-event-revision", histories[0].revision > 1U, 1);
    rc |= expect_int("sacch2-single-cipher", state.nxdn_cipher_type, 1);
    rc |= expect_int("sacch2-single-enc-lockout", state.dmr_encL, 1);

    const uint64_t call_epoch = call.epoch;
    make_sacch2_trellis(trellis, 1U, 2U, 0x1EU);
    nxdn_handle_sacch2(&opts, &state, trellis, m_data, 0x15U, 0x15U);
    rc |= expect_u8_at("sacch2-end-message-type", 0U, state.nxdn_dcr_sf_message_type, 0x1EU);
    rc |= expect_int("sacch2-end-canonical", dsd_call_state_get(&state, 0U, &call), 1);
    rc |= expect_int("sacch2-end-phase", call.phase, DSD_CALL_PHASE_ENDED);
    rc |= expect_int("sacch2-end-preserves-epoch", call.epoch == call_epoch, 1);

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
    rc |= expect_int("sacch2-pdu-no-call", dsd_call_state_get(&state, 0U, &call), 0);
    rc |= expect_str("sacch2-pdu-no-alias", state.generic_talker_alias[0], "");

    dsd_state_ext_free_all(&state);
    init_sacch2_state(&state, histories);
    make_sacch2_trellis(trellis, 1U, 2U, 0x00U);
    nxdn_handle_sacch2(&opts, &state, trellis, m_data, 0x0CU, 0x0CU);
    rc |= expect_u8_at("sacch2-idle-message-type", 0U, state.nxdn_dcr_sf_message_type, 0x00U);
    rc |= expect_int("sacch2-idle-no-call", dsd_call_state_get(&state, 0U, &call), 0);
    rc |= expect_str("sacch2-idle-no-alias", state.generic_talker_alias[0], "");

    dsd_state_ext_free_all(&state);
    init_sacch2_state(&state, histories);
    state.M = 1;
    state.nxdn_payload_mi = 0x55U;
    make_sacch2_trellis(trellis, 1U, 2U, 0x1EU);

    nxdn_handle_sacch2(&opts, &state, trellis, m_data, 0x01U, 0x00U);
    rc |= expect_u8_at("sacch2-bad-crc-message-type", 0U, state.nxdn_dcr_sf_message_type, 0xFFU);
    rc |= expect_u8_at("sacch2-bad-crc-segcrc", 2U, state.nxdn_sacch_frame_segcrc[2], 1U);
    rc |= expect_int("sacch2-bad-crc-seed-clear", (int)state.nxdn_payload_mi, 0);
    rc |= expect_int("sacch2-bad-crc-no-call", dsd_call_state_get(&state, 0U, &call), 0);
    rc |= expect_str("sacch2-bad-crc-no-alias", state.generic_talker_alias[0], "");
    dsd_state_ext_free_all(&state);
    return rc;
}

/* A confirmed DCR release ends an NXDN call only: another protocol's call on slot 0 stays. */
static int
test_sacch2_release_ends_an_nxdn_call_only(void) {
    static const uint8_t m_data[5] = {0};
    static Event_History_I histories[2];
    static dsd_opts opts;
    static dsd_state state;
    uint8_t trellis[32];
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    init_sacch2_state(&state, histories);
    const dsd_call_observation p25 = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 1U,
        .policy_target_id = 1U,
        .ota_source_id = 3840U,
    };
    (void)dsd_call_state_observe(&state, &p25, DSD_CALL_BOUNDARY_BEGIN);
    state.nxdn_confirmed = 1;
    make_sacch2_trellis(trellis, 1U, 2U, 0x1EU);
    nxdn_handle_sacch2(&opts, &state, trellis, m_data, 0x15U, 0x15U);

    dsd_call_snapshot call;
    rc |= expect_int("sacch2-release-p25-call", dsd_call_state_get(&state, 0U, &call), 1);
    rc |= expect_int("sacch2-release-p25-call-active", call.phase, DSD_CALL_PHASE_ACTIVE);
    dsd_state_ext_free_all(&state);
    return rc;
}

/* A RAN field decoded after DCR's stand-in is a RAN again (issue #575): the stand-in marks only the value it wrote. */
static int
test_ran_after_dcr_stand_in_is_an_access_code(void) {
    static const uint8_t sacch2_m_data[5] = {0};
    static const uint8_t facch2_m_data[26] = {0};
    static Event_History_I histories[2];
    static dsd_opts opts;
    static dsd_state state;
    uint8_t sacch2[32];
    uint8_t facch2[208];
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    init_sacch2_state(&state, histories);
    make_sacch2_trellis(sacch2, 1U, 2U, 0x01U);
    nxdn_handle_sacch2(&opts, &state, sacch2, sacch2_m_data, 0x2AU, 0x2AU);
    nxdn_confirm_begin_frame(&state);
    nxdn_handle_sacch2(&opts, &state, sacch2, sacch2_m_data, 0x2AU, 0x2AU);
    rc |= expect_int("dcr-stand-in-written", (int)state.nxdn_last_ran, 7);
    rc |= expect_nxdn_access_code("dcr-stand-in-no-access-code", &state, 0, 0U);

    make_facch2_trellis(facch2, 1U, 0x2AU, 0x10U);
    nxdn_handle_facch2_udch(&opts, &state, facch2, facch2_m_data, 0x456U, 0x456U, 1U);
    rc |= expect_int("facch2-after-dcr-ran", (int)state.nxdn_last_ran, 0x2A);
    rc |= expect_nxdn_access_code("facch2-after-dcr-access-code", &state, 1, 0x2AU);
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

    /* Same caller-gate contract as SACCH above: a non-zero CAC CRC remainder never reaches the
     * element decoder (issue #411). */
    state.NxdnElementsContent.MessageType = 0x22U;

    /* The frame sync's join histories, stamped on the count the reset rewinds: an M17 word at 100 and 292 would pair
     * with one at 484 after it, and an NXDN96 lock would time the next FSW from 99. */
    state.m17_join_hits.symbolcnt[0] = 100U;
    state.m17_join_hits.kind[0] = 1U;
    state.m17_join_hits.symbolcnt[1] = 292U;
    state.m17_join_hits.kind[1] = 1U;
    state.m17_join_hits.next = 2U;
    state.nxdn96_sync.lock_type = (uint8_t)DSD_SYNC_NXDN_NEG;
    state.nxdn96_sync.lock_symbolcnt = 99U;

    for (int i = 0; i < 10; i++) {
        nxdn_handle_cac(&opts, &state, trellis, m_data, 1U);
    }
    rc |= expect_u8_at("cac-bad-crc-skips-elements", 0U, state.NxdnElementsContent.MessageType, 0x22U);
    rc |= expect_int("cac-before-threshold-carrier", state.carrier, 1);
    rc |= expect_int("cac-before-threshold-format", state.data_header_format[0], 9);

    nxdn_handle_cac(&opts, &state, trellis, m_data, 1U);
    rc |= expect_int("cac-reset-synctype", state.synctype, DSD_SYNC_NONE);
    rc |= expect_int("cac-reset-last-sync", state.lastsynctype, DSD_SYNC_NONE);
    rc |= expect_int("cac-reset-carrier", state.carrier, 0);
    rc |= expect_int("cac-reset-jitter", state.jitter, -1);
    rc |= expect_int("cac-reset-symbolcnt", (int)state.symbolcnt, 0);
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
    rc |= expect_int(
        "cac-reset-forgets-the-m17-join-words",
        state.m17_join_hits.kind[0] == 0U && state.m17_join_hits.kind[1] == 0U && state.m17_join_hits.next == 0U, 1);
    rc |= expect_int("cac-reset-forgets-the-nxdn96-lock", state.nxdn96_sync.lock_type, 0);

    nxdn_handle_cac(&opts, &state, trellis, m_data, 1U);
    rc |= expect_int("cac-counter-cleared-after-reset", state.carrier, 0);

    /* A clean CAC (zero remainder) does reach the decoder: IDLE sits at bits 2..7 of the 147-bit
     * element buffer the handler copies from trellis[8..154]. */
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(trellis, 0, sizeof(trellis));
    write_bits_msb(trellis, 10U, 6U, 0x10U);
    state.NxdnElementsContent.MessageType = 0x22U;

    nxdn_handle_cac(&opts, &state, trellis, m_data, 0U);
    rc |= expect_u8_at("cac-good-message-type", 0U, state.NxdnElementsContent.MessageType, 0x10U);
    return rc;
}

/* A run of failed CACs on a conventional -Y row leaves the clock the -Y step reads alone (issue #834): they are noise
   there, and pushing it would keep the scan on the row. A trunked receiver keeps the push that holds it on its control
   channel, and a plain conventional one keeps it as before. */
static int
cac_fail_run_moves_anchor(int scanner_mode, int trunk_enable) {
    static dsd_opts opts;
    static dsd_state state;
    static const uint8_t m_data[22] = {0};
    uint8_t trellis[176];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(trellis, 0, sizeof(trellis));
    opts.scanner_mode = scanner_mode;
    opts.trunk_enable = trunk_enable;
    state.last_cc_sync_time = 1;
    for (int i = 0; i < 11; i++) {
        nxdn_handle_cac(&opts, &state, trellis, m_data, 1U);
    }
    const int reset = state.lastsynctype == DSD_SYNC_NONE && state.carrier == 0 && state.symbolcnt == 0U;
    if (!reset) {
        return -1;
    }
    return state.last_cc_sync_time != 1;
}

static int
test_cac_fail_run_on_conventional_scan_keeps_anchor(void) {
    int rc = 0;
    rc |= expect_int("cac-fail-y-scan-keeps-the-anchor", cac_fail_run_moves_anchor(1, 0), 0);
    rc |= expect_int("cac-fail-trunked-pushes-the-anchor", cac_fail_run_moves_anchor(0, 1), 1);
    rc |= expect_int("cac-fail-trunked-y-pushes-the-anchor", cac_fail_run_moves_anchor(1, 1), 1);
    rc |= expect_int("cac-fail-conventional-pushes-the-anchor", cac_fail_run_moves_anchor(0, 0), 1);
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

    /* A CSM decoded before any call is observed: the live display takes it, the history row does not. */
    nxdn_handle_pich_tch(&opts, &state, trellis, m_data, 0x123U, 0x123U, 0x08U);
    rc |= expect_str("pich-csm-alias", state.generic_talker_alias[0], "CSM 123456789");
    rc |= expect_str("pich-csm-no-call-event-alias", histories[0].Event_History_Items[0].alias, "");
    rc |= expect_int("pich-csm-no-call-event-revision", (int)histories[0].revision, 0);

    /* Once the call is open the repeat lands on its row, with the "; " suffix. */
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_NXDN_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    rc |= expect_int("pich-csm-call-opens", dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    const uint64_t revision = histories[0].revision;
    nxdn_handle_pich_tch(&opts, &state, trellis, m_data, 0x123U, 0x123U, 0x08U);
    rc |= expect_str("pich-csm-event-alias", histories[0].Event_History_Items[0].alias, "CSM 123456789; ");
    rc |= expect_int("pich-csm-event-revision", (int)(histories[0].revision - revision), 1);

    DSD_SNPRINTF(state.generic_talker_alias[0], sizeof(state.generic_talker_alias[0]), "%s", "KEEP");
    DSD_SNPRINTF(histories[0].Event_History_Items[0].alias, sizeof(histories[0].Event_History_Items[0].alias), "%s",
                 "KEEP; ");
    state.nxdn_dcr_sf_message_type = 0x02U;

    nxdn_handle_pich_tch(&opts, &state, trellis, m_data, 0x123U, 0x123U, 0x08U);
    rc |= expect_str("pich-non-sb0-keeps-alias", state.generic_talker_alias[0], "KEEP");
    rc |= expect_str("pich-non-sb0-keeps-event-alias", histories[0].Event_History_Items[0].alias, "KEEP; ");
    dsd_state_ext_free_all(&state);
    return rc;
}

/*
 * Build the 144 soft bits an over-the-air FACCH1 burst would present, by running the decoder's
 * own transform chain backwards: CRC-12 over the 80-bit payload, rate-1/2 K=5 convolutional
 * encode, 16/9 puncture, then permute. The decoder has no encoder to borrow -- trellis_encode()
 * in src/core/util/dsd_misc.c is static and nxdn_convolution.h is decode-only -- so the eight
 * lines of the rate-1/2 encoder are restated here. nxdn_hard_fallback_decode() runs
 * trellis_decode() over the very stream nxdn_conv_decode_soft() consumes, so both speak this code.
 */
static void
make_facch1_soft_bits(uint8_t bits[144], const uint8_t payload[80], int corrupt_crc) {
    static const uint8_t PARITY[32] = {0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
                                       1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1};
    uint8_t trellis[96];
    uint8_t coded[192];
    uint8_t deperm[144];

    DSD_MEMSET(trellis, 0, sizeof(trellis));
    for (size_t i = 0U; i < 80U; i++) {
        trellis[i] = (uint8_t)(payload[i] & 1U);
    }

    /* CRC-12 over the payload, MSB first, exactly where the decoder reads its check field. */
    uint16_t crc = crc12f(trellis, 80);
    if (corrupt_crc) {
        crc ^= 1U;
    }
    write_bits_msb(trellis, 80U, 12U, crc);
    /* trellis[92..95] stay zero: the chainback returns 92 data bits and four flush bits. */

    unsigned int reg = 0U;
    for (size_t i = 0U; i < sizeof(coded); i += 2U) {
        reg = ((reg << 1U) | trellis[i >> 1U]) & 0x1FU;
        coded[i] = PARITY[reg & 0x19U];
        coded[i + 1U] = PARITY[reg & 0x17U];
    }

    /* Puncture: nxdn_depuncture_16_9_rel() reinserts a zero at each 4k+1, so drop it here. */
    size_t out = 0U;
    for (size_t k = 0U; k < 48U; k++) {
        deperm[out++] = coded[(k * 4U) + 0U];
        deperm[out++] = coded[(k * 4U) + 2U];
        deperm[out++] = coded[(k * 4U) + 3U];
    }

    /* Permute: nxdn_depermute_rel_u8() does deperm[PERM_16_9[i]] = bits[i]. */
    for (size_t i = 0U; i < 144U; i++) {
        bits[i] = deperm[PERM_16_9[i]];
    }
}

/*
 * FACCH1 carries call setup, so it is the most consequential of the caller gates: the element
 * decoder records no CRC verdict of its own, and a frame whose CRC-12 failed must never reach it
 * (issue #411).
 */
static int
test_facch1_crc_gates_element_decode(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t payload[80];
    uint8_t bits[144];
    uint8_t reliab[144];
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(payload, 0, sizeof(payload));
    DSD_MEMSET(reliab, 255, sizeof(reliab));
    write_bits_msb(payload, 2U, 6U, 0x10U); /* IDLE */

    DSD_MEMSET(&state, 0, sizeof(state));
    state.NxdnElementsContent.MessageType = 0x22U;
    make_facch1_soft_bits(bits, payload, 0);
    nxdn_deperm_facch_soft(&opts, &state, bits, reliab, 1U);
    rc |= expect_u8_at("facch1-good-message-type", 0U, state.NxdnElementsContent.MessageType, 0x10U);
    rc |= expect_int("facch1-good-confirmed", nxdn_confirm_is_confirmed(&state), 1);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.NxdnElementsContent.MessageType = 0x22U;
    make_facch1_soft_bits(bits, payload, 1);
    nxdn_deperm_facch_soft(&opts, &state, bits, reliab, 1U);
    rc |= expect_u8_at("facch1-bad-crc-skips-elements", 0U, state.NxdnElementsContent.MessageType, 0x22U);
    rc |= expect_int("facch1-bad-crc-unconfirmed", nxdn_confirm_is_confirmed(&state), 0);
    return rc;
}

/* The VCALLs the decoder published (the conventional-activity hook fires with the publication, once confirmed). */
static int g_vcall_publications;

static void
count_vcall_publication(const dsd_opts* opts, const dsd_state* state, uint32_t target, uint32_t source, int is_private,
                        int encrypted, int data_call) {
    (void)opts;
    (void)state;
    (void)target;
    (void)source;
    (void)is_private;
    (void)encrypted;
    (void)data_call;
    g_vcall_publications++;
}

/* A frame's two FACCH1 copies, the same VCALL with its CRC passing in both. On a frame that runs on from the last, the
   first copy confirms the transmission and publishes the call, and the second, a repeat, is not decoded again. On an
   isolated NXDN48 frame whose SACCH failed, the first copy's check waits for a second check and its call is withheld;
   the second copy is that check and confirms the transmission, so its content publishes the call: the setup is not
   lost to the repeat. Nothing publishes before the confirmation. */
static int
test_facch1_copy_that_confirms_publishes(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t payload[80];
    uint8_t bits[144];
    uint8_t reliab[144];
    int rc = 0;
    const dsd_trunk_scan_hooks hooks = {.nxdn_conventional_activity = count_vcall_publication};
    dsd_trunk_scan_hooks_set(&hooks);
    DSD_MEMSET(payload, 0, sizeof(payload));
    DSD_MEMSET(reliab, 255, sizeof(reliab));
    write_bits_msb(payload, 2U, 6U, 0x01U); /* VCALL */
    make_facch1_soft_bits(bits, payload, 0);

    for (int isolated = 0; isolated <= 1; isolated++) {
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        state.synctype = DSD_SYNC_NXDN_POS;
        nxdn_confirm_reset(&state);
        if (!isolated) {
            nxdn_confirm_note_frame_start(&state, 1000U, 1, 1);
            state.nxdn_confirmed = 1;
        }
        nxdn_confirm_note_frame_start(&state, isolated ? 5000U : 1192U, 0, 0);
        nxdn_confirm_begin_frame(&state);
        g_vcall_publications = 0;
        nxdn_deperm_facch_soft(&opts, &state, bits, reliab, 1U);
        rc |= expect_int(isolated ? "isolated-first-copy-withheld" : "running-first-copy-publishes",
                         g_vcall_publications, isolated ? 0 : 1);
        rc |= expect_int(isolated ? "isolated-first-copy-unconfirmed" : "running-first-copy-confirmed",
                         nxdn_confirm_is_confirmed(&state), isolated ? 0 : 1);
        nxdn_deperm_facch_soft(&opts, &state, bits, reliab, 2U);
        rc |= expect_int(isolated ? "isolated-second-copy-confirms" : "running-second-copy-confirmed",
                         nxdn_confirm_is_confirmed(&state), 1);
        rc |= expect_int(isolated ? "isolated-second-copy-publishes" : "running-repeat-not-decoded-again",
                         g_vcall_publications, 1);
        nxdn_confirm_end_frame(NULL, &state);
        dsd_state_ext_free_all(&state);
    }
    dsd_trunk_scan_hooks_set(NULL);
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

    state.nxdn_last_ran = 0x33U;
    state.nxdn_part_of_frame = 7;
    state.data_header_format[0] = 9U;
    state.NxdnElementsContent.MessageType = 0x22U;

    nxdn_handle_facch2_udch(&opts, &state, trellis, m_data, 0x456U, 0x457U, 1U);
    rc |= expect_int("facch2-bad-keeps-ran", (int)state.nxdn_last_ran, 0x33);
    rc |= expect_int("facch2-bad-part-reset", state.nxdn_part_of_frame, 0);
    rc |= expect_u8_at("facch2-bad-keeps-format", 0U, state.data_header_format[0], 9U);
    rc |= expect_u8_at("facch2-bad-skips-elements", 0U, state.NxdnElementsContent.MessageType, 0x22U);
    return rc;
}

/* What the signal search's confirmation can read when the evidence hook fires: the app-control observer copies the
 * carrier's RAN inside the hook, from state, and only on that confirmation. */
static struct {
    int calls;
    int access_valid;
    uint16_t ran;
} g_search_evidence;

/* Reports naming anything but NXDN, across every case below. */
static int g_search_other_protocol;

static void
capture_search_evidence(const dsd_state* state, const char* protocol, int sync, int slot, uint32_t target,
                        uint32_t source, const char* access) {
    (void)sync;
    (void)slot;
    (void)target;
    (void)source;
    (void)access;
    uint8_t kind = 0U;
    uint16_t value = 0U;
    g_search_evidence.calls++;
    g_search_other_protocol |= protocol == NULL || strcmp(protocol, "NXDN") != 0;
    g_search_evidence.access_valid = dsd_access_code_current(state, DSD_SYNC_NXDN_POS, 0U, 0U, &kind, &value) != 0
                                     && kind == (uint8_t)DSD_ACCESS_CODE_RAN;
    g_search_evidence.ran = value;
}

static void
reset_search_evidence(dsd_state* state) {
    DSD_MEMSET(&g_search_evidence, 0, sizeof(g_search_evidence));
    state->nxdn_last_ran = (unsigned int)-1;
}

/* The channel whose CRC confirms a transmission also carries its RAN. The signal search hears the confirmation once the
 * frame has published that RAN, so one confirming frame followed by silence still names it; a frame that proved
 * nothing tells the search nothing. */
static int
test_confirming_frame_reports_its_ran_to_search(void) {
    static const uint8_t facch2_m_data[26] = {0};
    static const uint8_t cac_m_data[22] = {0};
    static const uint8_t sacch_m_data[5] = {0};
    static dsd_opts opts;
    static dsd_state state;
    uint8_t facch2[208];
    uint8_t cac[176];
    uint8_t sacch[32];
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    g_search_other_protocol = 0;
    dsd_signal_search_evidence_set(capture_search_evidence);

    /* FACCH2/UDCH: a 15-bit CRC confirms on its own. */
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_search_evidence(&state);
    make_facch2_trellis(facch2, 1U, 0x2AU, 0x10U);
    nxdn_confirm_begin_frame(&state);
    nxdn_handle_facch2_udch(&opts, &state, facch2, facch2_m_data, 0x456U, 0x457U, 1U);
    nxdn_confirm_end_frame(NULL, &state);
    rc |= expect_int("search-facch2-bad-crc-reports-nothing", g_search_evidence.calls, 0);
    nxdn_confirm_begin_frame(&state);
    nxdn_handle_facch2_udch(&opts, &state, facch2, facch2_m_data, 0x456U, 0x456U, 1U);
    nxdn_confirm_end_frame(NULL, &state);
    rc |= expect_int("search-facch2-one-confirmation", g_search_evidence.calls, 1);
    rc |= expect_int("search-facch2-sees-the-ran", g_search_evidence.access_valid, 1);
    rc |= expect_int("search-facch2-ran", g_search_evidence.ran, 0x2A);

    /* CAC: a 16-bit CRC confirms on its own. */
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_search_evidence(&state);
    DSD_MEMSET(cac, 0, sizeof(cac));
    write_bits_msb(cac, 2U, 6U, 0x2DU);
    write_bits_msb(cac, 10U, 6U, 0x10U);
    nxdn_confirm_begin_frame(&state);
    nxdn_handle_cac(&opts, &state, cac, cac_m_data, 0U);
    nxdn_confirm_end_frame(NULL, &state);
    rc |= expect_int("search-cac-one-confirmation", g_search_evidence.calls, 1);
    rc |= expect_int("search-cac-sees-the-ran", g_search_evidence.access_valid, 1);
    rc |= expect_int("search-cac-ran", g_search_evidence.ran, 0x2D);

    /* SACCH: a 6-bit CRC confirms only on the second frame running, which is also the frame that publishes the RAN. */
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_search_evidence(&state);
    state.nxdn_sacch_non_superframe = 1;
    make_sacch_trellis(sacch, 3U, 0x15U);
    write_bits_msb(sacch, 10U, 6U, 0x10U);
    nxdn_confirm_begin_frame(&state);
    nxdn_handle_sacch(&opts, &state, sacch, sacch_m_data, 0x2AU, 0x2AU);
    nxdn_confirm_end_frame(NULL, &state);
    rc |= expect_int("search-sacch-first-frame-reports-nothing", g_search_evidence.calls, 0);
    nxdn_confirm_begin_frame(&state);
    nxdn_handle_sacch(&opts, &state, sacch, sacch_m_data, 0x2AU, 0x2AU);
    nxdn_confirm_end_frame(NULL, &state);
    rc |= expect_int("search-sacch-one-confirmation", g_search_evidence.calls, 1);
    rc |= expect_int("search-sacch-sees-the-ran", g_search_evidence.access_valid, 1);
    rc |= expect_int("search-sacch-ran", g_search_evidence.ran, 0x15);

    dsd_signal_search_evidence_set(NULL);
    rc |= expect_int("search-reports-name-nxdn", g_search_other_protocol, 0);
    dsd_state_ext_free_all(&state);
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

    state.data_header_format[0] = 9U;
    state.NxdnElementsContent.MessageType = 0x22U;

    message.check[0] = 0x124U;
    nxdn_handle_facch3_udch2_soft(&opts, &state, &message, 0U);
    rc |= expect_u8_at("udch2-bad-keeps-format", 0U, state.data_header_format[0], 9U);
    rc |= expect_u8_at("udch2-bad-skips-elements", 0U, state.NxdnElementsContent.MessageType, 0x22U);
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

    state.data_header_format[0] = 9U;
    state.NxdnElementsContent.MessageType = 0x22U;
    message.check[1] = 0x223U;
    nxdn_handle_facch3_udch2_soft(&opts, &state, &message, 0U);
    rc |= expect_u8_at("udch2-split-bad-keeps-format", 0U, state.data_header_format[0], 9U);
    rc |= expect_u8_at("udch2-split-bad-keeps-message-type", 0U, state.NxdnElementsContent.MessageType, 0x22U);
    return rc;
}

/*
 * The 60 soft bits of an SCCH (NXDN TS 1-E Fig 4.5-1) carrying @p payload's 25 bits: CRC-7 over them where the decoder
 * reads its check (bits 25-31), four flush bits, the rate-1/2 K=5 code make_facch1_soft_bits() restates, the 12/5
 * puncture nxdn_depuncture_12_5_rel() undoes (each 12 coded bits lose their 6th and 12th), then PERM_12_5.
 */
static void
make_scch_soft_bits(uint8_t bits[60], const uint8_t payload[25], int corrupt_crc) {
    static const uint8_t PARITY[32] = {0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
                                       1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1};
    uint8_t trellis[36];
    uint8_t coded[72];
    uint8_t deperm[60];

    DSD_MEMSET(trellis, 0, sizeof(trellis));
    for (size_t i = 0U; i < 25U; i++) {
        trellis[i] = (uint8_t)(payload[i] & 1U);
    }
    uint8_t crc = crc7_scch(trellis, 25);
    if (corrupt_crc) {
        crc ^= 1U;
    }
    write_bits_msb(trellis, 25U, 7U, crc);

    unsigned int reg = 0U;
    for (size_t i = 0U; i < sizeof(coded); i += 2U) {
        reg = ((reg << 1U) | trellis[i >> 1U]) & 0x1FU;
        coded[i] = PARITY[reg & 0x19U];
        coded[i + 1U] = PARITY[reg & 0x17U];
    }

    size_t out = 0U;
    for (size_t i = 0U; i < sizeof(coded); i++) {
        const size_t k = i % 12U;
        if (k != 5U && k != 11U) {
            deperm[out++] = coded[i];
        }
    }

    for (size_t i = 0U; i < 60U; i++) {
        bits[i] = deperm[PERM_12_5[i]];
    }
}

/* A heap-allocated decoder fixture (AGENTS.md: dsd_state and dsd_opts live on the heap). */
static int
alloc_fixture(dsd_opts** opts, dsd_state** state) {
    *opts = (dsd_opts*)calloc(1, sizeof(**opts));
    *state = (dsd_state*)calloc(1, sizeof(**state));
    if (*opts == NULL || *state == NULL) {
        free(*state);
        free(*opts);
        *opts = NULL;
        *state = NULL;
        return 1;
    }
    return 0;
}

static void
free_fixture(dsd_opts* opts, dsd_state* state) {
    if (state != NULL) {
        dsd_state_ext_free_all(state);
    }
    free(state);
    free(opts);
}

/*
 * A scrambled Type-D call's register is loaded from the key at each superframe's first SCCH and runs on across its four
 * frames (NXDN TS 1-F §16.4.2, Fig 16.4-1). Only an SCCH whose CRC-7 passed may say where in the superframe the frame
 * is: a damaged one that reads as the first (Structure 11) records no position. A verified one records its frame's
 * position, which the frame applies once its signalling chose the cipher and key (nxdn_frame()); the channel decoder
 * itself never touches the register. The placement the frame applies (nxdn_prepare_sacch_payload_seed()) puts the
 * register at the frame's start: the third frame two frames (eight VCH slots) on from the key.
 */
static int
test_scch_crc_gates_scrambler_reload(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    uint8_t payload[25];
    uint8_t bits[60];
    uint8_t reliab[60];
    int rc = 0;
    const unsigned long long key = 0x5A5AULL;

    DSD_MEMSET(reliab, 255, sizeof(reliab));
    DSD_MEMSET(payload, 0, sizeof(payload));
    payload[0] = 1U; /* Structure 11: INFO1, the superframe's first SCCH */
    payload[1] = 1U;
    write_bits_msb(payload, 3U, 5U, 2U);
    write_bits_msb(payload, 8U, 5U, 3U);

    for (int corrupt = 1; corrupt >= 0; corrupt--) {
        if (alloc_fixture(&opts, &state) != 0) {
            return 1;
        }
        state->R = key;
        state->nxdn_cipher_type = 1U;
        state->nxdn_payload_mi = 0x1234ULL; /* partway through the superframe */
        make_scch_soft_bits(bits, payload, corrupt);
        const nxdn_channel_result result = nxdn_deperm_scch_soft(opts, state, bits, reliab, 1U);
        rc |= expect_int(corrupt ? "scch-bad-crc-no-position" : "scch-good-crc-first-frame", result.seed_part,
                         corrupt ? 0 : 1);
        rc |= expect_int("scch-decoder-leaves-register", state->nxdn_payload_mi == 0x1234ULL ? 1 : 0, 1);
        free_fixture(opts, state);
    }

    /* A verified third SCCH (Structure 01, 3/4). */
    payload[0] = 0U;
    payload[1] = 1U;
    if (alloc_fixture(&opts, &state) != 0) {
        return 1;
    }
    state->R = key;
    state->nxdn_cipher_type = 1U;
    state->nxdn_payload_mi = 0x1234ULL;
    make_scch_soft_bits(bits, payload, 0);
    const nxdn_channel_result third = nxdn_deperm_scch_soft(opts, state, bits, reliab, 1U);
    rc |= expect_int("scch-good-crc-third-frame", third.seed_part, 3);
    nxdn_prepare_sacch_payload_seed(state, third.seed_part - 1);
    rc |= expect_int("scch-placement-two-frames-on",
                     state->nxdn_payload_mi == advance_nxdn_lfsr_seed(key, 2 * 4) ? 1 : 0, 1);
    free_fixture(opts, state);
    return rc;
}

/* One SCCH of test_scch_undefined_content_confirms_nothing(): its fields (NXDN TS 1-E §6.5) and whether TS 1-E defines
   that content. */
typedef struct {
    const char* tag;
    uint16_t id;       /* bits 13-23 */
    uint16_t site;     /* a site ID message: Site Type (2) and Site Code (8) over bits 3-12, else 0xFFFF */
    uint8_t structure; /* INFO4 0, INFO3 1, INFO2 2, INFO1 3 */
    uint8_t bits_3_7;  /* Repeater in Use / Go to / Free Repeater 1; Site Type in bits 3-4 */
    uint8_t bits_8_12; /* Home Repeater / Prefix, Free Repeater 2 or Pass Character; Site Code low bits */
    uint8_t bit_24;    /* G/U or Spare */
    uint8_t valid;
    uint8_t inbound; /* decoded as an inbound SCCH (the rest outbound) */
} scch_content_case;

static const scch_content_case k_scch_content_cases[] = {
    {"scch-reserved-prefix-confirms-nothing", 1234U, 0xFFFFU, 1U, 2U, 31U, 0U, 0U, 0U},
    {"scch-valid-confirms", 1234U, 0xFFFFU, 1U, 2U, 3U, 0U, 1U, 0U},
    {"scch-spare-info1-confirms", 0U, 0xFFFFU, 3U, 0U, 0U, 1U, 1U, 0U},
    {"scch-spare-info3-confirms", 1234U, 0xFFFFU, 1U, 2U, 3U, 1U, 1U, 0U},
    {"scch-site-code-0-confirms-nothing", 2041U, (1U << 8) | 0U, 0U, 0U, 0U, 0U, 0U, 0U},
    {"scch-site-code-251-confirms-nothing", 2041U, (1U << 8) | 251U, 0U, 0U, 0U, 0U, 0U, 0U},
    {"scch-site-type-00-confirms-nothing", 2041U, (0U << 8) | 17U, 0U, 0U, 0U, 0U, 0U, 0U},
    {"scch-site-id-confirms", 2041U, (1U << 8) | 17U, 0U, 0U, 0U, 0U, 1U, 0U},
    {"scch-info1-free-repeater-1-31-confirms-nothing", 0U, 0xFFFFU, 3U, 31U, 3U, 0U, 0U, 0U},
    {"scch-info1-free-repeater-2-31-confirms-nothing", 0U, 0xFFFFU, 3U, 2U, 31U, 0U, 0U, 0U},
    {"scch-info1-confirms", 0U, 0xFFFFU, 3U, 2U, 3U, 0U, 1U, 0U},
    {"scch-free-repeater-31-confirms-nothing", 2044U, 0xFFFFU, 0U, 31U, 3U, 0U, 0U, 0U},
    {"scch-info3-free-repeater-31-confirms-nothing", 1234U, 0xFFFFU, 1U, 31U, 3U, 0U, 0U, 0U},
    {"scch-info3-idle-code-confirms-nothing", 2046U, 0xFFFFU, 1U, 2U, 3U, 0U, 0U, 0U},
    {"scch-info3-all-units-confirms-nothing", 2047U, 0xFFFFU, 1U, 2U, 3U, 0U, 0U, 0U},
    {"scch-info2-reserved-id-confirms-nothing", 2001U, 0xFFFFU, 2U, 5U, 3U, 0U, 0U, 0U},
    {"scch-info4-reserved-id-confirms-nothing", 2040U, 0xFFFFU, 0U, 5U, 3U, 1U, 0U, 0U},
    {"scch-info2-id-31-confirms", 31U, 0xFFFFU, 2U, 5U, 3U, 0U, 1U, 0U},
    {"scch-info4-all-group-confirms", 2047U, 0xFFFFU, 0U, 5U, 3U, 0U, 1U, 0U},
    {"scch-info2-special-id-nonzero-prefix-confirms-nothing", 2043U, 0xFFFFU, 2U, 5U, 6U, 0U, 0U, 0U},
    {"scch-info4-special-id-nonzero-prefix-confirms-nothing", 2042U, 0xFFFFU, 0U, 5U, 6U, 0U, 0U, 0U},
    {"scch-info4-special-id-prefix-0-confirms", 2043U, 0xFFFFU, 0U, 5U, 0U, 0U, 1U, 0U},
    {"scch-inbound-info1-repeater-in-use-0-confirms-nothing", 0U, 0xFFFFU, 3U, 0U, 31U, 0U, 0U, 1U},
    {"scch-inbound-info1-repeater-in-use-31-confirms-nothing", 0U, 0xFFFFU, 3U, 31U, 31U, 0U, 0U, 1U},
    {"scch-inbound-info1-confirms", 0U, 0xFFFFU, 3U, 2U, 31U, 0U, 1U, 1U},
    {"scch-inbound-info3-repeater-in-use-0-confirms-nothing", 1234U, 0xFFFFU, 1U, 0U, 3U, 0U, 0U, 1U},
    {"scch-inbound-info3-confirms", 1234U, 0xFFFFU, 1U, 2U, 3U, 0U, 1U, 1U},
    {"scch-inbound-info4-repeater-in-use-0-confirms-nothing", 1234U, 0xFFFFU, 0U, 0U, 3U, 0U, 0U, 1U},
    {"scch-inbound-info2-repeater-in-use-0-confirms-nothing", 1234U, 0xFFFFU, 2U, 0U, 3U, 0U, 0U, 1U},
    {"scch-inbound-info4-eot-confirms", 1234U, 0xFFFFU, 0U, 31U, 3U, 0U, 1U, 1U},
    {"scch-inbound-info2-eot-confirms", 1234U, 0xFFFFU, 2U, 31U, 3U, 0U, 1U, 1U},
};

static void
make_scch_content_case(uint8_t payload[25], const scch_content_case* c) {
    DSD_MEMSET(payload, 0, 25U);
    write_bits_msb(payload, 0U, 2U, c->structure);
    if (c->site != 0xFFFFU) {
        write_bits_msb(payload, 3U, 2U, (uint32_t)(c->site >> 8));
        write_bits_msb(payload, 5U, 8U, (uint32_t)(c->site & 0xFFU));
    } else {
        write_bits_msb(payload, 3U, 5U, c->bits_3_7);
        write_bits_msb(payload, 8U, 5U, c->bits_8_12);
    }
    write_bits_msb(payload, 13U, 11U, c->id);
    payload[24] = c->bit_24;
}

/*
 * An outbound SCCH whose content NXDN TS 1-E does not define passes its CRC-7 but is no evidence of a transmission
 * (Table 9.1-1: a reserved value is not valid): frames of them alone never confirm one. The cases: the reserved prefix
 * 31 (Table 6.6-4); an inbound INFO1 or INFO3 whose Repeater in Use is 0 or 31 (Table 6.6-9); a site ID message's reserved Site Type 00 or Site Code 0, 251
 * (§6.6.4, §6.6.5); a reserved Free Repeater 31 (Table 6.6-8) in an INFO1, an INFO3 or the free repeater message; an
 * INFO3 sender naming a status code or All Units; a reserved ID 2001-2040 (Tables 6.6-5, 6.6-6); the special IDs 2042
 * and 2043 under a prefix other than 0 (§6.6.7, §6.6.8). Valid SCCHs confirm, an INFO1 with its Spare bit set included
 * (a receiver ignores a Spare bit; D2 refuses only its call option),
 * among them an INFO3 with its Spare bit set, which a receiver ignores, and the standard ID 31.
 */
static int
test_scch_undefined_content_confirms_nothing(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    uint8_t payload[25];
    uint8_t bits[60];
    uint8_t reliab[60];
    int rc = 0;
    DSD_MEMSET(reliab, 255, sizeof(reliab));
    for (size_t k = 0; k < sizeof(k_scch_content_cases) / sizeof(k_scch_content_cases[0]); k++) {
        const scch_content_case* c = &k_scch_content_cases[k];
        make_scch_content_case(payload, c);
        if (alloc_fixture(&opts, &state) != 0) {
            return 1;
        }
        state->synctype = DSD_SYNC_NXDN_POS;
        nxdn_confirm_reset(state);
        make_scch_soft_bits(bits, payload, 0);
        for (uint32_t frame = 0U; frame < 3U; frame++) {
            nxdn_confirm_note_frame_start(state, 1000U + (frame * 192U), frame == 0U ? 1 : 0, 0);
            nxdn_confirm_begin_frame(state);
            (void)nxdn_deperm_scch_soft(opts, state, bits, reliab, c->inbound ? 0U : 1U);
            nxdn_confirm_end_frame(opts, state);
        }
        rc |= expect_int(c->tag, nxdn_confirm_is_confirmed(state), (int)c->valid);
        free_fixture(opts, state);
    }
    return rc;
}

/* A VCALL's 80 elements: type, CC option, call type, voice call option, source, destination, cipher and key. */
static void
make_vcall_elements(uint8_t bits[96], uint8_t message_type, uint16_t source, uint16_t destination, uint8_t cipher,
                    uint8_t key_id) {
    DSD_MEMSET(bits, 0, 96U);
    write_bits_msb(bits, 2U, 6U, message_type);
    write_bits_msb(bits, 16U, 3U, 1U);
    write_bits_msb(bits, 19U, 5U, 2U);
    write_bits_msb(bits, 24U, 16U, source);
    write_bits_msb(bits, 40U, 16U, destination);
    write_bits_msb(bits, 56U, 2U, cipher);
    write_bits_msb(bits, 58U, 6U, key_id);
}

/*
 * The MI a call publishes reaches the canonical call, event history and from there the API feed. An NXDN call's MI is
 * only an IV it received over the air: a scrambled call's (whose working register is the key itself, then key stream,
 * NXDN TS 1-F §16.4.2) is none; a DES call's is none until its VCALL_IV comes (here the working register holds the
 * key an assignment loaded, or the key stream a scrambled call before it left).
 */
static int
test_nxdn_history_mi_only_from_received_iv(void) {
    static Event_History_I histories[2];
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    uint8_t bits[96];
    int rc = 0;
    dsd_call_snapshot call;

    if (alloc_fixture(&opts, &state) != 0) {
        return 1;
    }
    init_sacch2_state(state, histories);
    state->nxdn_confirmed = 1;
    state->synctype = DSD_SYNC_NXDN_POS;
    state->R = 0x3039ULL;
    state->nxdn_payload_mi = state->R;
    make_vcall_elements(bits, 0x01U, 0x0123U, 0x0456U, 1U, 0x01U);
    NXDN_Elements_Content_decode(opts, state, bits, sizeof(bits));
    rc |= expect_int("history-mi-scramble-call", dsd_call_state_get(state, 0U, &call), 1);
    rc |= expect_int("history-mi-scramble-call-mi", call.mi == 0ULL ? 1 : 0, 1);
    rc |= expect_int("history-mi-scramble-event-mi", histories[0].Event_History_Items[0].mi == 0ULL ? 1 : 0, 1);
    free_fixture(opts, state);

    if (alloc_fixture(&opts, &state) != 0) {
        return 1;
    }
    init_sacch2_state(state, histories);
    state->nxdn_confirmed = 1;
    state->synctype = DSD_SYNC_NXDN_POS;
    state->R = 0x3039ULL; /* the DES key a followed assignment loaded, also in the working register */
    state->nxdn_payload_mi = state->R;
    make_vcall_elements(bits, 0x01U, 0x0123U, 0x0456U, 2U, 0x01U);
    NXDN_Elements_Content_decode(opts, state, bits, sizeof(bits));
    rc |= expect_int("history-mi-des-call", dsd_call_state_get(state, 0U, &call), 1);
    rc |= expect_int("history-mi-des-call-mi", call.mi == 0ULL ? 1 : 0, 1);
    rc |= expect_int("history-mi-des-event-mi", histories[0].Event_History_Items[0].mi == 0ULL ? 1 : 0, 1);
    DSD_MEMSET(bits, 0, sizeof(bits));
    write_bits_msb(bits, 2U, 6U, 0x03U); /* VCALL_IV, in the open call */
    write_bits_msb(bits, 8U, 32U, 0x11223344U);
    write_bits_msb(bits, 40U, 32U, 0x55667788U);
    NXDN_Elements_Content_decode(opts, state, bits, sizeof(bits));
    rc |= expect_int("history-mi-des-iv-call", dsd_call_state_get(state, 0U, &call), 1);
    rc |= expect_int("history-mi-des-iv-mi", call.mi == 0x1122334455667788ULL ? 1 : 0, 1);
    rc |= expect_int("history-mi-des-iv-event-mi",
                     histories[0].Event_History_Items[0].mi == 0x1122334455667788ULL ? 1 : 0, 1);
    rc |= expect_int("history-mi-des-iv-published-decryptable", call.crypto, DSD_CALL_CRYPTO_DECRYPTABLE);
    rc |= expect_int("history-mi-des-iv-armed", state->nxdn_new_iv, 1);
    free_fixture(opts, state);

    /* A scrambled call ran its register on (LFSRN()); the call's cipher then reads DES: that key stream is no MI. */
    if (alloc_fixture(&opts, &state) != 0) {
        return 1;
    }
    init_sacch2_state(state, histories);
    state->nxdn_confirmed = 1;
    state->synctype = DSD_SYNC_NXDN_POS;
    state->R = 0x3039ULL;
    state->nxdn_payload_mi = 0x1122334455667788ULL;
    state->nxdn_iv_on_air = 1U; /* an IV an earlier call received */
    {
        const char in[49] = {0};
        char out[49];
        state->nxdn_payload_mi = state->R;
        LFSRN(in, out, state);
    }
    make_vcall_elements(bits, 0x01U, 0x0123U, 0x0456U, 2U, 0x01U);
    NXDN_Elements_Content_decode(opts, state, bits, sizeof(bits));
    rc |= expect_int("history-mi-after-scramble-call", dsd_call_state_get(state, 0U, &call), 1);
    rc |= expect_int("history-mi-after-scramble-mi", call.mi == 0ULL ? 1 : 0, 1);
    free_fixture(opts, state);
    return rc;
}

/*
 * NXDN keeps its working register to itself (dsd_state::nxdn_payload_mi). P25 Phase 2 and DMR keep slot 2's MI in
 * dsd_state::payload_miN, which P25 publishes as that slot's MI: no NXDN key load, placement, scrambler step, VCALL_IV
 * or DCR first block may write it (a key-loaded register there would be published as a P25 MI). And another protocol
 * writing payload_miN changes no NXDN MI: the call's MI stays the IV its VCALL_IV carried.
 */
static int
test_nxdn_keeps_the_shared_register(void) {
    static Event_History_I histories[2];
    static const uint8_t m_data[5] = {0};
    const unsigned long long p25_mi = 0x0A0B0C0D0E0F1011ULL;
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    uint8_t bits[96];
    uint8_t trellis[32];
    int rc = 0;
    dsd_call_snapshot call;

    if (alloc_fixture(&opts, &state) != 0) {
        return 1;
    }
    init_sacch2_state(state, histories);
    state->nxdn_confirmed = 1;
    state->synctype = DSD_SYNC_NXDN_POS;
    state->M = 1; /* forced Scramble (-4) */
    state->R = 0x3039ULL;
    state->nxdn_cipher_type = 1U;
    state->payload_miN = p25_mi;
    nxdn_reset_payload_seed_if_forced(state);
    nxdn_prepare_sacch_payload_seed(state, 2);
    {
        const char in[49] = {0};
        char out[49];
        LFSRN(in, out, state);
    }
    state->nxdn_iv_on_air = 1U;                  /* as if an IV had come */
    make_sacch2_trellis(trellis, 1U, 0U, 0x00U); /* a DCR first block */
    nxdn_handle_sacch2(opts, state, trellis, m_data, 0U, 0U);
    rc |= expect_int("shared-register-kept-by-the-scrambler", state->payload_miN == p25_mi ? 1 : 0, 1);
    rc |= expect_int("dcr-first-block-holds-no-iv", state->nxdn_iv_on_air, 0);

    state->M = 0;
    make_vcall_elements(bits, 0x01U, 0x0123U, 0x0456U, 2U, 0x01U);
    NXDN_Elements_Content_decode(opts, state, bits, sizeof(bits));
    DSD_MEMSET(bits, 0, sizeof(bits));
    write_bits_msb(bits, 2U, 6U, 0x03U); /* VCALL_IV */
    write_bits_msb(bits, 8U, 32U, 0x11223344U);
    write_bits_msb(bits, 40U, 32U, 0x55667788U);
    NXDN_Elements_Content_decode(opts, state, bits, sizeof(bits));
    rc |= expect_int("shared-register-kept-by-the-vcall-iv", state->payload_miN == p25_mi ? 1 : 0, 1);

    state->payload_miN = 0x99ULL; /* another protocol's slot-2 MI */
    make_vcall_elements(bits, 0x01U, 0x0123U, 0x0456U, 2U, 0x01U);
    NXDN_Elements_Content_decode(opts, state, bits, sizeof(bits));
    rc |= expect_int("nxdn-mi-call", dsd_call_state_get(state, 0U, &call), 1);
    rc |= expect_int("nxdn-mi-not-another-protocols", call.mi == 0x1122334455667788ULL ? 1 : 0, 1);
    free_fixture(opts, state);
    return rc;
}

/*
 * A Type-C superframe SACCH segment places nothing itself. A verified one gives its position to the frame, which places
 * the register once those messages chose the cipher and key (nxdn_frame()); a failed one gives none, and the register
 * runs on.
 */
static int
test_sacch_superframe_position_stays_with_the_frame(void) {
    static const uint8_t m_data[5] = {0};
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    uint8_t trellis[32];
    int rc = 0;

    static const struct {
        uint8_t structure;
        uint8_t seed_part;                                     /* expected for a verified segment */
    } k_segments[] = {{3U, 1U}, {2U, 2U}, {1U, 3U}, {0U, 4U}}; /* 1/4 .. 4/4 (TS 1-D) */

    for (size_t k = 0; k < sizeof(k_segments) / sizeof(k_segments[0]) * 2U; k++) {
        const int bad = (int)(k & 1U);
        if (alloc_fixture(&opts, &state) != 0) {
            return 1;
        }
        state->nxdn_sacch_non_superframe = 0;
        state->nxdn_cipher_type = 1U; /* a scrambled call, partway through its superframe */
        state->R = 0x3039ULL;
        state->nxdn_payload_mi = 0x1234ULL;
        make_sacch_trellis(trellis, k_segments[k / 2U].structure, 0x15U);
        const nxdn_channel_result result = nxdn_handle_sacch(opts, state, trellis, m_data, 0x2AU, bad ? 0x2BU : 0x2AU);
        rc |= expect_int(bad ? "sacch-sf-bad-crc-no-position" : "sacch-sf-position-for-the-frame", result.seed_part,
                         bad ? 0 : (int)k_segments[k / 2U].seed_part);
        /* The segment decoder never places the register itself: a failed one places nothing, and the frame places a
           verified one once, after its signalling. */
        rc |= expect_int(bad ? "sacch-sf-bad-crc-register-runs-on" : "sacch-sf-register-placed-by-the-frame",
                         state->nxdn_payload_mi == 0x1234ULL ? 1 : 0, 1);
        free_fixture(opts, state);
    }
    return rc;
}

/* An outbound SCCH (direction 1) from free repeaters 6 and 3: INFO1 (@p sf 3) naming the scrambler, else an INFO3 (@p sf
 * 1) naming source 03-0123. */
static void
send_outbound_scch(dsd_opts* opts, dsd_state* state, uint8_t sf) {
    uint8_t message[32];
    DSD_MEMSET(message, 0, sizeof(message));
    write_bits_msb(message, 0U, 2U, sf);
    write_bits_msb(message, 3U, 5U, 6U);
    write_bits_msb(message, 8U, 5U, 3U);
    if (sf == 3U) {
        write_bits_msb(message, 16U, 2U, 1U); // cipher type: scrambler
    } else {
        write_bits_msb(message, 13U, 11U, 123U);
    }
    NXDN_decode_scch(opts, state, message, 1U);
}

/*
 * An IDAS call's own scrambler survives its numbered party (issue #903): a confirmed DCR call is received with the
 * scrambler (its SB0 messages' cipher field), then an IDAS call's INFO1 names the scrambler and its INFO3 the numbered
 * source. The DCR call's received cipher is not the new call's, but the INFO1's is: the call keeps the scrambler.
 */
static int
test_info1_cipher_survives_the_numbered_party(void) {
    static Event_History_I histories[2];
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (alloc_fixture(&opts, &state) != 0) {
        return 1;
    }
    init_sacch2_state(state, histories);
    dsd_decode_clock_test_set_ns(1000000000000ULL);
    state->R = 0x1234ULL;
    static const uint8_t m_data[5] = {0U};
    uint8_t trellis[32];
    make_sacch2_trellis(trellis, 1U, 0U, 0x01U);
    write_bits_msb(trellis, 8U, 2U, 0x01U); // cipher: scrambler
    for (int i = 0; i < 2; i++) {
        nxdn_confirm_begin_frame(state);
        nxdn_handle_sacch2(opts, state, trellis, m_data, 0x2AU, 0x2AU);
    }
    dsd_call_snapshot call;
    int rc = expect_int("info1-after-dcr-dcr-call", dsd_call_state_get(state, 0U, &call), 1);
    rc |= expect_str("info1-after-dcr-dcr-text", call.target_text, "JPN DCR");
    rc |= expect_int("info1-after-dcr-dcr-scrambler", (int)state->nxdn_cipher_type, 1);

    state->last_vc_sync_time = dsd_decode_time(); /* the carrier's voice is live */
    send_outbound_scch(opts, state, 3U);
    send_outbound_scch(opts, state, 1U);
    (void)dsd_call_state_get(state, 0U, &call);
    rc |= expect_int("info1-after-dcr-numbered-source", (int)call.ota_source_id, 123);
    rc |= expect_int("info1-after-dcr-cipher-kept", (int)state->nxdn_cipher_type, 1);
    rc |= expect_int("info1-after-dcr-received-scrambler", (int)state->nxdn_cipher_rx_class, 2);
    free_fixture(opts, state);
    dsd_decode_clock_use_system();
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
    rc |= test_sacch2_dcr_and_named_calls_stay_apart();
    rc |= test_sacch2_dcr_and_source_only_calls_stay_apart();
    rc |= test_sacch2_release_ends_an_nxdn_call_only();
    rc |= test_ran_after_dcr_stand_in_is_an_access_code();
    rc |= test_cac_crc_failure_reset();
    rc |= test_cac_fail_run_on_conventional_scan_keeps_anchor();
    rc |= test_pich_tch_dcr_csm_alias_state();
    rc |= test_facch1_crc_gates_element_decode();
    rc |= test_facch1_copy_that_confirms_publishes();
    rc |= test_facch2_udch_crc_state_update();
    rc |= test_confirming_frame_reports_its_ran_to_search();
    rc |= test_facch3_udch2_crc_state_update();
    rc |= test_facch3_udch2_split_block_storage_and_crc_gate();
    rc |= test_scch_crc_gates_scrambler_reload();
    rc |= test_scch_undefined_content_confirms_nothing();
    rc |= test_nxdn_history_mi_only_from_received_iv();
    rc |= test_sacch_superframe_position_stays_with_the_frame();
    rc |= test_nxdn_keeps_the_shared_register();
    rc |= test_info1_cipher_survives_the_numbered_party();

    if (rc == 0) {
        printf("NXDN_DEPERM_PRIMITIVES: OK\n");
    }
    return rc;
}
