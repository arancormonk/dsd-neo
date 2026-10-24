// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* BPTC(196,96): failed column decodes keep the received bits, and the full
 * codeword decoder reports the codeword it reaches and its distance. */

#include <assert.h>
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/bptc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dmr_bptc_test_encoder.h"

/* Encodes payload and reserved bits into a deinterleaved 196-bit word. */
static void
encode_deinterleaved(const uint8_t payload[96], const uint8_t reserved[3], uint8_t out[196]) {
    uint8_t info[196];
    dmr_test_encode_bptc_196x96(payload, reserved, info);
    BPTCDeInterleaveDMRData(info, out);
}

/* Two errors in the column-parity rows of one column give an uncorrectable
 * syndrome on both passes. The extractor must keep that column as received
 * instead of copying the previous column's (or uninitialised) decoder output. */
static void
test_extract_keeps_failed_column(void) {
    static const uint8_t r0[3] = {0, 0, 0};
    uint8_t payload[96] = {0};
    uint8_t word[196];
    uint8_t out[96];
    uint8_t r[3];
    payload[8] = 1; // matrix row 1, column 0

    encode_deinterleaved(payload, r0, word);
    word[137] ^= 1U; // matrix row 9, column 1
    word[182] ^= 1U; // matrix row 12, column 1
    uint32_t irr = BPTC_196x96_Extract_Data(word, out, r);
    assert(irr > 0);
    assert(out[8] == 1);
    assert(out[9] == 0); // stale copy of column 0 set this bit
    assert(memcmp(out, payload, sizeof out) == 0);
    assert(memcmp(r, r0, sizeof r) == 0);

    // The same pattern in the first column has no previous column to copy.
    encode_deinterleaved(payload, r0, word);
    word[136] ^= 1U; // matrix row 9, column 0
    word[181] ^= 1U; // matrix row 12, column 0
    irr = BPTC_196x96_Extract_Data(word, out, r);
    assert(irr > 0);
    assert(memcmp(out, payload, sizeof out) == 0);
    assert(memcmp(r, r0, sizeof r) == 0);
}

/* Clean CSBK from the dmr_t3_cc fixture (R = 0) and the RAS reference burst
 * from the encoder self-check (R = 4). */
static const uint8_t k_csbk_bytes[12] = {0x81, 0x06, 0x04, 0x05, 0x07, 0x0C, 0x0D, 0x00, 0x00, 0x02, 0xA7, 0xC7};
static const uint8_t k_csbk_r[3] = {0, 0, 0};
static const uint8_t k_ras_bytes[12] = {0x02, 0x50, 0x00, 0x00, 0x2A, 0x00, 0x00, 0x18, 0x81, 0x00, 0x00, 0x00};
static const uint8_t k_ras_r[3] = {0, 0, 1};

static unsigned
r_value(const uint8_t r[3]) {
    return (unsigned)r[0] | ((unsigned)r[1] << 1) | ((unsigned)r[2] << 2);
}

static size_t
matrix_bit(size_t row, size_t col) {
    return 1U + (row * 15U) + col;
}

static void
build_word(const uint8_t bytes[12], const uint8_t reserved[3], uint8_t payload[96], uint8_t word[196]) {
    dsd_unpack_bytes_to_bits(bytes, 12, payload, 96, 12);
    encode_deinterleaved(payload, reserved, word);
}

/* Decodes and checks that the call left the input alone. */
static int
decode(const uint8_t word[196], uint8_t out[96], uint8_t r[3], uint32_t* distance) {
    uint8_t before[196];
    DSD_MEMCPY(before, word, sizeof before);
    int ok = BPTC_196x96_Decode_Codeword(word, out, r, distance);
    assert(memcmp(before, word, sizeof before) == 0);
    return ok;
}

static void
test_codeword_clean(void) {
    const uint8_t* bytes[2] = {k_csbk_bytes, k_ras_bytes};
    const uint8_t* reserved[2] = {k_csbk_r, k_ras_r};
    for (size_t w = 0; w < 2; w++) {
        uint8_t payload[96];
        uint8_t word[196];
        uint8_t out[96];
        uint8_t r[3];
        uint32_t distance = 99;
        build_word(bytes[w], reserved[w], payload, word);
        assert(decode(word, out, r, &distance) == 1);
        assert(distance == 0);
        assert(memcmp(out, payload, sizeof out) == 0);
        assert(memcmp(r, reserved[w], sizeof r) == 0);

        // The reserved bit 0 is outside the product code.
        word[0] ^= 1U;
        assert(decode(word, out, r, &distance) == 1);
        assert(distance == 0);
        assert(memcmp(out, payload, sizeof out) == 0);
    }
}

/* One error per distinct row and column is corrected by the row pass. */
static void
test_codeword_scattered_errors(void) {
    static const size_t rows[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    static const size_t cols[8] = {3, 5, 7, 9, 11, 13, 0, 2};
    uint8_t payload[96];
    uint8_t clean[196];
    build_word(k_ras_bytes, k_ras_r, payload, clean);
    for (size_t n = 1; n <= 8; n++) {
        uint8_t word[196];
        uint8_t out[96];
        uint8_t r[3];
        uint32_t distance = 0;
        DSD_MEMCPY(word, clean, sizeof word);
        for (size_t i = 0; i < n; i++) {
            word[matrix_bit(rows[i], cols[i])] ^= 1U;
        }
        assert(decode(word, out, r, &distance) == 1);
        assert(distance == n);
        assert(memcmp(out, payload, sizeof out) == 0);
        assert(memcmp(r, k_ras_r, sizeof r) == 0);
    }
}

/* A column-parity bit is corrected by its column and counts once. */
static void
test_codeword_column_parity_error(void) {
    uint8_t payload[96];
    uint8_t word[196];
    uint8_t out[96];
    uint8_t r[3];
    uint32_t distance = 0;
    build_word(k_csbk_bytes, k_csbk_r, payload, word);
    word[matrix_bit(10, 6)] ^= 1U;
    assert(decode(word, out, r, &distance) == 1);
    assert(distance == 1);
    assert(memcmp(out, payload, sizeof out) == 0);
    assert(memcmp(r, k_csbk_r, sizeof r) == 0);
}

/* Bits 29/74/75/90 on the R = 4 word: the full decoder returns the original
 * codeword at distance 4 while the extractor's payload differs at 18/51/62. */
static void
test_codeword_differs_from_extractor(void) {
    static const size_t flips[4] = {29, 74, 75, 90};
    static const size_t extractor_diffs[3] = {18, 51, 62};
    uint8_t payload[96];
    uint8_t word[196];
    uint8_t out[96];
    uint8_t r[3];
    uint32_t distance = 0;
    build_word(k_ras_bytes, k_ras_r, payload, word);
    for (size_t i = 0; i < 4; i++) {
        word[flips[i]] ^= 1U;
    }
    assert(decode(word, out, r, &distance) == 1);
    assert(distance == 4);
    assert(memcmp(out, payload, sizeof out) == 0);
    assert(memcmp(r, k_ras_r, sizeof r) == 0);

    uint8_t extracted[96];
    uint8_t extracted_r[3];
    uint8_t copy[196];
    DSD_MEMCPY(copy, word, sizeof copy);
    (void)BPTC_196x96_Extract_Data(copy, extracted, extracted_r);
    size_t next = 0;
    for (size_t i = 0; i < 96; i++) {
        if (extracted[i] != payload[i]) {
            assert(next < 3 && extractor_diffs[next] == i);
            next++;
        }
    }
    assert(next == 3);
}

/* Bits 1/2/16/17 (two errors in each of rows 0-1 and columns 0-1) lead the
 * iterative decoder to a wrong codeword at distance 5, beyond a radius-4 gate. */
static void
test_codeword_wrong_beyond_radius(void) {
    const uint8_t* bytes[2] = {k_csbk_bytes, k_ras_bytes};
    const uint8_t* reserved[2] = {k_csbk_r, k_ras_r};
    static const unsigned wrong_r[2] = {6, 2};
    static const size_t flips[4] = {1, 2, 16, 17};
    for (size_t w = 0; w < 2; w++) {
        uint8_t payload[96];
        uint8_t word[196];
        uint8_t out[96];
        uint8_t r[3];
        uint32_t distance = 0;
        build_word(bytes[w], reserved[w], payload, word);
        for (size_t i = 0; i < 4; i++) {
            word[flips[i]] ^= 1U;
        }
        assert(decode(word, out, r, &distance) == 1);
        assert(distance == 5);
        assert(distance > 4);
        assert(r_value(r) == wrong_r[w]);
        assert(memcmp(out, payload, sizeof out) != 0);
    }
}

static uint32_t
xorshift32(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Random words rarely decode at all, and never close to a codeword. */
static void
test_codeword_random_words(void) {
    uint32_t state = 0x5EED1234U;
    unsigned successes = 0;
    for (unsigned t = 0; t < 10000; t++) {
        uint8_t word[196];
        uint8_t out[96];
        uint8_t r[3];
        uint32_t distance = 0;
        for (size_t i = 0; i < 196; i++) {
            word[i] = (uint8_t)(xorshift32(&state) & 1U);
        }
        if (decode(word, out, r, &distance) == 1) {
            successes++;
            assert(distance >= 17);
        }
    }
    printf("random words decoded to a codeword: %u of 10000\n", successes);
}

/* A row-parity and a column-parity error in one column: the extractor copies
 * back data bits only, so that column still fails on the second pass although
 * the payload is right. The extractor then falls back to the full codeword
 * decoder, whose codeword within the unique radius is the nearest one. */
static void
test_extract_falls_back_to_nearest_codeword(void) {
    uint8_t payload[96];
    uint8_t word[196];
    uint8_t out[96];
    uint8_t r[3];
    build_word(k_csbk_bytes, k_csbk_r, payload, word);
    word[matrix_bit(4, 11)] ^= 1U; // row-parity bit
    word[matrix_bit(9, 11)] ^= 1U; // column-parity bit
    assert(BPTC_196x96_Extract_Data(word, out, r) == 0);
    assert(memcmp(out, payload, sizeof out) == 0);
    assert(memcmp(r, k_csbk_r, sizeof r) == 0);

    // Capacity Plus channel status CSBK from the dmr_voice fixture, CRC good:
    // errors at matrix (4,11), (6,10) and (9,11), distance 3.
    static const char k_voice_csbk[] = "0000101111100000000100001110111000110000001001000"
                                       "0000000000000000000000010000000000000000000000000"
                                       "0001000000000011000111000110110011101100000111001"
                                       "1111000111000010000001010100101011001110011010010";
    static const uint8_t k_voice_csbk_bytes[12] = {0xBE, 0x10, 0xE3, 0x00, 0x00, 0x00,
                                                   0x00, 0x00, 0x00, 0x00, 0xC1, 0xB3};
    _Static_assert(sizeof k_voice_csbk == 197, "196 bits and the terminating NUL");
    for (size_t i = 0; i < 196; i++) {
        word[i] = (uint8_t)(k_voice_csbk[i] == '1');
    }
    dsd_unpack_bytes_to_bits(k_voice_csbk_bytes, 12, payload, 96, 12);
    uint32_t distance = 0;
    assert(decode(word, out, r, &distance) == 1);
    assert(distance == 3);
    assert(memcmp(out, payload, sizeof out) == 0);
    assert(BPTC_196x96_Extract_Data(word, out, r) == 0);
    assert(memcmp(out, payload, sizeof out) == 0);
    assert(memcmp(r, k_csbk_r, sizeof r) == 0);

    // Beyond the radius the extractor keeps its own result and its count:
    // these five errors fail the second pass and reach a codeword at distance 5.
    static const size_t flips[5] = {88, 127, 134, 144, 178};
    build_word(k_csbk_bytes, k_csbk_r, payload, word);
    for (size_t i = 0; i < 5; i++) {
        word[flips[i]] ^= 1U;
    }
    uint8_t codeword_out[96];
    uint8_t codeword_r[3];
    assert(decode(word, codeword_out, codeword_r, &distance) == 1);
    assert(distance == 5);
    uint8_t copy[196];
    DSD_MEMCPY(copy, word, sizeof copy);
    assert(BPTC_196x96_Extract_Data(copy, out, r) > 0);
    assert(distance > DSD_BPTC_196x96_UNIQUE_RADIUS);
}

/* A word the decoder cannot finish returns 0 with zeroed outputs. */
static void
test_codeword_rejects(void) {
    uint8_t payload[96];
    uint8_t word[196];
    uint8_t out[96];
    uint8_t r[3];
    uint32_t distance = 7;
    build_word(k_csbk_bytes, k_csbk_r, payload, word);
    for (size_t row = 0; row < 5; row++) {
        for (size_t col = 0; col < 5; col++) {
            word[matrix_bit(row, col)] ^= 1U;
        }
    }
    DSD_MEMSET(out, 0xAA, sizeof out);
    DSD_MEMSET(r, 0xAA, sizeof r);
    assert(decode(word, out, r, &distance) == 0);
    assert(distance == 0);
    for (size_t i = 0; i < 96; i++) {
        assert(out[i] == 0);
    }
    assert(r[0] == 0 && r[1] == 0 && r[2] == 0);

    assert(BPTC_196x96_Decode_Codeword(NULL, out, r, &distance) == 0);
    assert(BPTC_196x96_Decode_Codeword(word, NULL, r, &distance) == 0);
    assert(BPTC_196x96_Decode_Codeword(word, out, NULL, &distance) == 0);
    assert(BPTC_196x96_Decode_Codeword(word, out, r, NULL) == 0);
}

int
main(void) {
    InitAllFecFunction();
    assert(dmr_test_check_reference_burst() == 1);
    test_extract_keeps_failed_column();
    test_codeword_clean();
    test_codeword_scattered_errors();
    test_codeword_column_parity_error();
    test_codeword_differs_from_extractor();
    test_codeword_wrong_beyond_radius();
    test_codeword_random_words();
    test_codeword_rejects();
    test_extract_falls_back_to_nearest_codeword();
    printf("FEC BPTC codeword tests passed.\n");
    return 0;
}
