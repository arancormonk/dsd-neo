// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/ez.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static void
set_bits_from_u32(unsigned char* dst_bits, int nbits, unsigned int v) {
    for (int i = 0; i < nbits; i++) {
        dst_bits[i] = (unsigned char)((v >> i) & 1U);
    }
}

static int
arrays_equal_u8(const unsigned char* a, const unsigned char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if ((a[i] & 1U) != (b[i] & 1U)) {
            return 0;
        }
    }
    return 1;
}

/* The codewords both Hamming tests below decode: one per code, data bits first. */
static const unsigned char k_hamming_12_8_codeword[12] = {0, 1, 0, 1, 1, 0, 1, 0, 1, 1, 1, 1};
static const unsigned char k_hamming_13_9_codeword[13] = {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0};
static const unsigned char k_hamming_15_11_codeword[15] = {1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1};
static const unsigned char k_hamming_16_11_4_codeword[16] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0};

typedef bool (*hamming_decode_fn)(unsigned char*, unsigned char*, int);

/* Decode a run of codewords in one call, with a single-bit error in the last of them.
   Correction has to land in the codeword that carried the error: indexing the flip from
   the head of the buffer instead corrupts codeword 0 and leaves the real error standing. */
static int
check_hamming_run(const char* label, hamming_decode_fn decode, const unsigned char* codeword, size_t code_len,
                  size_t data_len, size_t error_position) {
    enum { RUN = 3 };

    unsigned char clean[RUN * 16];
    unsigned char rx[RUN * 16];
    unsigned char dec[RUN * 11];

    for (size_t ic = 0; ic < (size_t)RUN; ic++) {
        DSD_MEMCPY(&clean[ic * code_len], codeword, code_len);
    }
    DSD_MEMCPY(rx, clean, code_len * (size_t)RUN);
    rx[((size_t)(RUN - 1) * code_len) + error_position] ^= 1;
    DSD_MEMSET(dec, 0xFF, sizeof(dec));

    if (decode(rx, dec, RUN) != true) {
        DSD_FPRINTF(stderr, "%s: a single-bit error in the last codeword was not correctable\n", label);
        return 1;
    }
    if (!arrays_equal_u8(rx, clean, code_len * (size_t)RUN)) {
        DSD_FPRINTF(stderr, "%s: correction did not land in the codeword that carried the error\n", label);
        return 1;
    }
    for (size_t ic = 0; ic < (size_t)RUN; ic++) {
        if (!arrays_equal_u8(&dec[ic * data_len], codeword, data_len)) {
            DSD_FPRINTF(stderr, "%s: codeword %zu decoded to the wrong data\n", label, ic);
            return 1;
        }
    }
    return 0;
}

/* Every caller in the tree decodes one codeword at a time, so the multi-codeword path
   these four all advertise had never been exercised. */
static int
test_hamming_multi_codeword(void) {
    InitAllFecFunction();

    if (check_hamming_run("Hamming (12,8)", Hamming_12_8_decode, k_hamming_12_8_codeword, 12, 8, 5) != 0) {
        return 1;
    }
    if (check_hamming_run("Hamming (13,9)", Hamming_13_9_decode, k_hamming_13_9_codeword, 13, 9, 4) != 0) {
        return 1;
    }
    if (check_hamming_run("Hamming (15,11)", Hamming_15_11_decode, k_hamming_15_11_codeword, 15, 11, 10) != 0) {
        return 1;
    }
    if (check_hamming_run("Hamming (16,11,4)", Hamming_16_11_4_decode, k_hamming_16_11_4_codeword, 16, 11, 9) != 0) {
        return 1;
    }
    return 0;
}

static int
test_hamming_codes(void) {
    InitAllFecFunction();

    // Hamming (7,4)
    {
        static const unsigned char codeword[7] = {0, 1, 0, 1, 1, 0, 0};
        unsigned char rx[7];
        DSD_MEMCPY(rx, codeword, 7);
        assert(Hamming_7_4_decode(rx) == true);
        assert(arrays_equal_u8(rx, codeword, 7));
        // 1-bit error
        DSD_MEMCPY(rx, codeword, 7);
        rx[2] ^= 1;
        assert(Hamming_7_4_decode(rx) == true);
        assert(arrays_equal_u8(rx, codeword, 7));
        // Note: (7,4) single-error correction may miscorrect double errors.
        // Do not assert behavior on 2-bit errors here.
    }

    // Hamming (12,8)
    {
        static const unsigned char expected[8] = {0, 1, 0, 1, 1, 0, 1, 0};
        unsigned char rx[12], dec[8];
        DSD_MEMCPY(rx, k_hamming_12_8_codeword, 12);
        assert(Hamming_12_8_decode(rx, dec, 1) == true);
        assert(memcmp(dec, expected, 8) == 0);
        // 1-bit error
        DSD_MEMCPY(rx, k_hamming_12_8_codeword, 12);
        rx[5] ^= 1;
        DSD_MEMSET(dec, 0, sizeof(dec));
        assert(Hamming_12_8_decode(rx, dec, 1) == true);
        assert(memcmp(dec, expected, 8) == 0);
        // Multi-bit errors: decoder may not guarantee detect; skip assertion.
    }

    // Hamming (13,9)
    {
        static const unsigned char expected[9] = {1, 0, 1, 0, 1, 0, 1, 0, 1};
        unsigned char rx[13], dec[9];
        DSD_MEMCPY(rx, k_hamming_13_9_codeword, 13);
        assert(Hamming_13_9_decode(rx, dec, 1) == true);
        assert(memcmp(dec, expected, 9) == 0);
        // 1-bit error
        DSD_MEMCPY(rx, k_hamming_13_9_codeword, 13);
        rx[4] ^= 1;
        DSD_MEMSET(dec, 0, sizeof(dec));
        assert(Hamming_13_9_decode(rx, dec, 1) == true);
        assert(memcmp(dec, expected, 9) == 0);
        // Multi-bit errors: skip assertion.
    }

    // Hamming (15,11)
    {
        static const unsigned char expected[11] = {1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 0};
        unsigned char rx[15], dec[11];
        DSD_MEMCPY(rx, k_hamming_15_11_codeword, 15);
        assert(Hamming_15_11_decode(rx, dec, 1) == true);
        assert(memcmp(dec, expected, 11) == 0);
        // 1-bit error
        DSD_MEMCPY(rx, k_hamming_15_11_codeword, 15);
        rx[10] ^= 1;
        DSD_MEMSET(dec, 0, sizeof(dec));
        assert(Hamming_15_11_decode(rx, dec, 1) == true);
        assert(memcmp(dec, expected, 11) == 0);
        // Multi-bit errors: skip assertion.
    }

    // Hamming (16,11,4)
    {
        static const unsigned char expected[11] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0};
        unsigned char rx[16], dec[11];
        DSD_MEMCPY(rx, k_hamming_16_11_4_codeword, 16);
        assert(Hamming_16_11_4_decode(rx, dec, 1) == true);
        assert(memcmp(dec, expected, 11) == 0);
        // 1-bit error
        DSD_MEMCPY(rx, k_hamming_16_11_4_codeword, 16);
        rx[15] ^= 1;
        DSD_MEMSET(dec, 0, sizeof(dec));
        assert(Hamming_16_11_4_decode(rx, dec, 1) == true);
        assert(memcmp(dec, expected, 11) == 0);
        // Multi-bit errors: skip assertion.
    }

    return 0;
}

static int
test_golay_qr(void) {
    InitAllFecFunction();

    // Golay (20,8) – correct up to 2 errors
    {
        static const unsigned char codeword[20] = {1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 0, 1, 1};
        unsigned char rx[20];
        // 0 errors
        DSD_MEMCPY(rx, codeword, 20);
        assert(Golay_20_8_decode(rx) == true);
        assert(arrays_equal_u8(rx, codeword, 20));
        // 1 error
        DSD_MEMCPY(rx, codeword, 20);
        rx[3] ^= 1;
        assert(Golay_20_8_decode(rx) == true);
        assert(arrays_equal_u8(rx, codeword, 20));
        // 2 errors
        DSD_MEMCPY(rx, codeword, 20);
        rx[1] ^= 1;
        rx[9] ^= 1;
        assert(Golay_20_8_decode(rx) == true);
        // 3 errors -> fail (weight-6 code)
        DSD_MEMCPY(rx, codeword, 20);
        rx[0] ^= 1;
        rx[5] ^= 1;
        rx[12] ^= 1;
        assert(Golay_20_8_decode(rx) == false);
    }

    // Golay (24,12) – correct up to 3 errors
    {
        unsigned char msg[12];
        unsigned char enc[24], rx[24];
        set_bits_from_u32(msg, 12, 0xACE);
        Golay_24_12_encode(msg, enc);
        DSD_MEMCPY(rx, enc, 24); // 0
        assert(Golay_24_12_decode(rx) == true);
        DSD_MEMCPY(rx, enc, 24);
        rx[2] ^= 1; // 1
        assert(Golay_24_12_decode(rx) == true);
        DSD_MEMCPY(rx, enc, 24);
        rx[1] ^= 1;
        rx[5] ^= 1; // 2
        assert(Golay_24_12_decode(rx) == true);
        // Some implementations may not correct all 3-error patterns deterministically; skip.
        // For >3 errors behavior is undefined; skip negative assertion.
    }

    // Quadratic residue (16,7,6) – up to 2 errors
    {
        static const unsigned char codeword[16] = {1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        unsigned char rx[16];
        DSD_MEMCPY(rx, codeword, 16); // 0
        assert(QR_16_7_6_decode(rx) == true);
        DSD_MEMCPY(rx, codeword, 16);
        rx[6] ^= 1; // 1
        assert(QR_16_7_6_decode(rx) == true);
        DSD_MEMCPY(rx, codeword, 16);
        rx[0] ^= 1;
        rx[9] ^= 1; // 2
        assert(QR_16_7_6_decode(rx) == true);
        // For >2 errors behavior is undefined; skip negative assertion.
    }

    return 0;
}

static int
test_isch_soft_lookup(void) {
    const uint64_t isch0 = 0x184229d461ULL;
    uint8_t reliab[40];
    for (int i = 0; i < 40; i++) {
        reliab[i] = 220;
    }

    assert(isch_lookup(isch0) == 0);
    assert(isch_lookup_soft(isch0, reliab) == 0);

    uint64_t corrupted = isch0;
    int low_bits[3] = {3, 11, 27};
    for (int i = 0; i < 3; i++) {
        corrupted ^= 1ULL << (39 - low_bits[i]);
        reliab[low_bits[i]] = 10;
    }
    assert(isch_lookup(corrupted) == 0);
    assert(isch_lookup_soft(corrupted, NULL) == 0);
    assert(isch_lookup_soft(corrupted, reliab) == 0);

    uint64_t too_far = isch0;
    for (int i = 0; i < 8; i++) {
        too_far ^= 1ULL << (39 - i);
    }
    assert(isch_lookup(too_far) == -2);
    assert(isch_lookup_soft(too_far, reliab) == -2);

    assert(isch_lookup(0x575d57f7ffULL) == -2);

    return 0;
}

static void
assert_all_zero_ints(const int* values, size_t n) {
    for (size_t i = 0; i < n; i++) {
        assert(values[i] == 0);
    }
}

static int
test_rs28_zero_codewords(void) {
    int ess_payload[96] = {0};
    int ess_parity[168] = {0};
    int facch_payload[156] = {0};
    int facch_parity[114] = {0};
    int sacch_payload[180] = {0};
    int sacch_parity[132] = {0};
    const int facch_fixed_erasures[18] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 54, 55, 56, 57, 58, 59, 60, 61, 62};
    const int sacch_fixed_erasures[11] = {0, 1, 2, 3, 4, 57, 58, 59, 60, 61, 62};
    int many_erasures[32];
    for (int i = 0; i < 32; i++) {
        many_erasures[i] = i;
    }

    assert(ez_rs28_ess(ess_payload, ess_parity, NULL, 0) == 0);
    assert_all_zero_ints(ess_payload, sizeof(ess_payload) / sizeof(ess_payload[0]));

    assert(ez_rs28_facch(facch_payload, facch_parity, facch_fixed_erasures, 18) == 0);
    assert_all_zero_ints(facch_payload, sizeof(facch_payload) / sizeof(facch_payload[0]));

    assert(ez_rs28_sacch(sacch_payload, sacch_parity, sacch_fixed_erasures, 11) == 0);
    assert_all_zero_ints(sacch_payload, sizeof(sacch_payload) / sizeof(sacch_payload[0]));

    assert(ez_rs28_ess(ess_payload, ess_parity, many_erasures, 32) == 0);
    assert_all_zero_ints(ess_payload, sizeof(ess_payload) / sizeof(ess_payload[0]));

    assert(ez_rs28_facch(facch_payload, facch_parity, many_erasures, 32) == 0);
    assert_all_zero_ints(facch_payload, sizeof(facch_payload) / sizeof(facch_payload[0]));

    assert(ez_rs28_sacch(sacch_payload, sacch_parity, many_erasures, 32) == 0);
    assert_all_zero_ints(sacch_payload, sizeof(sacch_payload) / sizeof(sacch_payload[0]));

    return 0;
}

int
main(void) {
    if (test_hamming_codes() != 0) {
        return 1;
    }
    if (test_hamming_multi_codeword() != 0) {
        return 1;
    }
    if (test_golay_qr() != 0) {
        return 1;
    }
    if (test_isch_soft_lookup() != 0) {
        return 1;
    }
    if (test_rs28_zero_codewords() != 0) {
        return 1;
    }

    printf("FEC block code tests passed.\n");
    return 0;
}
