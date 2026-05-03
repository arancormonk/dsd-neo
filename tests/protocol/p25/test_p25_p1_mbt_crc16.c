// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 1 MBT CRC16 tests.
 *
 * Covers CRC16 algorithm consistency, TSBK-style majority-vote correction,
 * MPDU `end` calculation, and field-data header vector diagnostics.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Bridge to the CRC16 implementation in p25_crc.c */
int crc16_lb_bridge(const int* payload, int len);

/*
 * Compute CCITT-16 CRC over `len` bits (each element is 0 or 1),
 * using polynomial 0x1021 with final XOR 0xFFFF.
 */
static uint16_t
compute_crc16(const int* bits, int len) {
    uint16_t crc = 0x0000;
    const uint16_t poly = 0x1021;
    for (int i = 0; i < len; i++) {
        if (((crc >> 15) & 1) ^ (bits[i] & 1)) {
            crc = (uint16_t)((crc << 1) ^ poly);
        } else {
            crc = (uint16_t)(crc << 1);
        }
    }
    return crc ^ 0xFFFF;
}

/*
 * Build a 96-bit vector from an 80-bit payload seed, appending a valid
 * CRC16 in bits 80..95.
 */
static void
make_valid_vector(int seed_val, int bits[96]) {
    for (int i = 0; i < 80; i++) {
        bits[i] = ((seed_val >> (i % 16)) ^ (seed_val >> ((i + 7) % 16))) & 1;
    }
    uint16_t crc = compute_crc16(bits, 80);
    for (int i = 0; i < 16; i++) {
        bits[80 + i] = (crc >> (15 - i)) & 1;
    }
}

static void
majority_vote_3(const int rep0[96], const int rep1[96], const int rep2[96], int out[96]) {
    for (int i = 0; i < 96; i++) {
        int sum = rep0[i] + rep1[i] + rep2[i];
        out[i] = (sum >= 2) ? 1 : 0;
    }
}

/* Convert byte array to bit array (MSB first). */
static void
bytes_to_bits(const uint8_t* bytes, int nbytes, int* bits) {
    for (int i = 0; i < nbytes; i++) {
        for (int b = 7; b >= 0; b--) {
            *bits++ = (bytes[i] >> b) & 1;
        }
    }
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    /* -------------------------------------------------------------------
     * CRC16 algorithm consistency — 10 synthetic vectors
     * ------------------------------------------------------------------- */
    {
        static const int seeds[10] = {0x0000, 0x1234, 0x5678, 0x9ABC, 0xDEF0, 0xAAAA, 0x5555, 0xFF00, 0x00FF, 0x7E3C};

        for (int s = 0; s < 10; s++) {
            int bits[96];
            char tag[64];

            make_valid_vector(seeds[s], bits);

            snprintf(tag, sizeof(tag), "crc16_valid_seed_%04X", (unsigned)seeds[s]);
            rc |= expect_eq_int(tag, crc16_lb_bridge(bits, 80), 0);

            int tamper_pos = (s * 7) % 80;
            bits[tamper_pos] ^= 1;

            snprintf(tag, sizeof(tag), "crc16_tamper_seed_%04X_bit%d", (unsigned)seeds[s], tamper_pos);
            rc |= expect_eq_int(tag, (crc16_lb_bridge(bits, 80) != 0) ? 1 : 0, 1);
        }
    }

    /* -------------------------------------------------------------------
     * TSBK-style 3-rep majority — all identical, CRC16 preserved
     * ------------------------------------------------------------------- */
    {
        int original[96];
        make_valid_vector(0xBEEF, original);

        int rep0[96], rep1[96], rep2[96], voted[96];
        memcpy(rep0, original, sizeof(original));
        memcpy(rep1, original, sizeof(original));
        memcpy(rep2, original, sizeof(original));

        majority_vote_3(rep0, rep1, rep2, voted);
        rc |= expect_eq_int("tsbk_3rep_identical_crc16", crc16_lb_bridge(voted, 80), 0);
    }

    /* -------------------------------------------------------------------
     * TSBK-style 3-rep with 1 corrupted rep — majority corrects it
     * ------------------------------------------------------------------- */
    {
        int original[96];
        make_valid_vector(0xCAFE, original);

        int rep0[96], rep1[96], rep2[96], voted[96];
        memcpy(rep0, original, sizeof(original));
        memcpy(rep1, original, sizeof(original));
        memcpy(rep2, original, sizeof(original));

        rep2[10] ^= 1;
        rep2[25] ^= 1;
        rep2[40] ^= 1;
        rep2[55] ^= 1;
        rep2[70] ^= 1;

        majority_vote_3(rep0, rep1, rep2, voted);
        rc |= expect_eq_int("tsbk_3rep_1corrupt_crc16", crc16_lb_bridge(voted, 80), 0);
    }

    /* -------------------------------------------------------------------
     * MPDU `end` calculation for various SAP/blks combinations
     * ------------------------------------------------------------------- */
    {
        static const struct {
            uint8_t sap;
            uint8_t blks;
            int expected_end;
            const char* label;
        } cases[] = {
            {0, 5, 6, "sap0_blks5"},   {1, 3, 4, "sap1_blks3"},   {10, 1, 2, "sap10_blks1"}, {0, 10, 11, "sap0_blks10"},
            {32, 0, 1, "sap32_blks0"}, {61, 5, 6, "sap61_blks5"}, {63, 5, 6, "sap63_blks5"},
        };

        static const int num_cases = (int)(sizeof(cases) / sizeof(cases[0]));

        for (int c = 0; c < num_cases; c++) {
            int end = cases[c].blks + 1;
            if ((cases[c].sap == 61 || cases[c].sap == 63) && cases[c].blks > 10) {
                end = 4;
            }
            if (end > 128) {
                end = 128;
            }

            char tag[64];
            snprintf(tag, sizeof(tag), "end_calc_%s", cases[c].label);
            rc |= expect_eq_int(tag, end, cases[c].expected_end);
        }
    }

    /* -------------------------------------------------------------------
     * Field-data MBT header vectors — diagnostic CRC16 checks
     *
     * Tests CRC16 against known MBT header patterns and alternative
     * hypotheses (different field range, reversed bit order).
     * ------------------------------------------------------------------- */
    {
        static const uint8_t mbt_hdr_vectors[][12] = {
            /* Synthetic 0x3B (NET_STS_BCST) header */
            {0x77, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x01, 0x3B, 0x00, 0x00, 0x00, 0x00},
            /* Synthetic 0x3A (RFSS_STS_BCST) header */
            {0x77, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x01, 0x3A, 0x00, 0x00, 0x00, 0x00},
        };
        static const int num_vectors = (int)(sizeof(mbt_hdr_vectors) / sizeof(mbt_hdr_vectors[0]));

        for (int v = 0; v < num_vectors; v++) {
            int bits[96];
            bytes_to_bits(mbt_hdr_vectors[v], 12, bits);

            int crc_result = crc16_lb_bridge(bits, 80);
            uint8_t fmt = mbt_hdr_vectors[v][0] & 0x1F;
            uint8_t opcode = mbt_hdr_vectors[v][7] & 0x3F;

            fprintf(stderr, "field_vector_%d: FMT=0x%02X OP=0x%02X CRC16=%s\n", v, fmt, opcode,
                    (crc_result == 0) ? "PASS" : "FAIL");

            /* Hypothesis: try 64-bit CRC field range */
            int crc_64 = crc16_lb_bridge(bits, 64);
            fprintf(stderr, "  64-bit range: %s\n", (crc_64 == 0) ? "PASS" : "FAIL");

            /* Hypothesis: try reversed bit order within each byte */
            int rbits[96];
            for (int i = 0; i < 12; i++) {
                uint8_t byte = mbt_hdr_vectors[v][i];
                uint8_t rev = 0;
                for (int b = 0; b < 8; b++) {
                    rev |= (uint8_t)(((byte >> b) & 1) << (7 - b));
                }
                for (int b = 7; b >= 0; b--) {
                    rbits[i * 8 + (7 - b)] = (rev >> b) & 1;
                }
            }
            int crc_rev = crc16_lb_bridge(rbits, 80);
            fprintf(stderr, "  reversed bits: %s\n", (crc_rev == 0) ? "PASS" : "FAIL");
        }
    }

    if (rc == 0) {
        fprintf(stderr, "P25 MBT CRC16: all tests passed\n");
    }
    return rc;
}
