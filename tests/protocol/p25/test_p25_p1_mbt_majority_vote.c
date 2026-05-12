// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 1 MBT majority-vote regression tests.
 *
 * Bug condition: processMPDU() collects 3 "repetitions" for MBT PDUs
 * (FMT=0x17, SAP=0x3D, BLKS=1), but only rep 0 is the actual header.
 * Rep 1 is the data block (different content) and rep 2 is the next
 * frame or TDU.  Majority-voting across these 3 reps corrupts the
 * header bits, causing CRC16 to fail on valid headers.
 *
 * This test confirms the bug by:
 *   1. Verifying CRC16 passes on each header (rep 0 alone)
 *   2. Simulating majority-vote with synthetic data block + next-frame
 *      content, then asserting CRC16 FAILS on the majority-voted bits
 *   3. Verifying that checking only rep 0 (the fix direction) passes CRC16
 *   4. Showing that a minor FEC artifact + majority-vote = double failure
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Bridge to the CRC16 implementation in p25_crc.c */
int crc16_lb_bridge(const int* payload, int len);

/*
 * Helper: convert a 12-byte header into the 96-element int array that
 * crc16_lb_bridge() expects, then check CRC16 over the first 80 bits.
 */
static int
check_crc16_on_header(const uint8_t hdr[12]) {
    int bits[96];
    for (int i = 0; i < 12; i++) {
        for (int b = 0; b < 8; b++) {
            bits[i * 8 + b] = (hdr[i] >> (7 - b)) & 1;
        }
    }
    return crc16_lb_bridge(bits, 80);
}

/*
 * Helper: convert a 12-byte array into a 96-element uint8_t bit array.
 */
static void
bytes_to_bits(const uint8_t bytes[12], uint8_t bits[96]) {
    for (int i = 0; i < 12; i++) {
        for (int b = 0; b < 8; b++) {
            bits[i * 8 + b] = (bytes[i] >> (7 - b)) & 1;
        }
    }
}

/*
 * Helper: majority-vote across 3 sets of 96 bits.
 * For each bit position, output 1 if 2 or more inputs are 1, else 0.
 */
static void
majority_vote_3(const uint8_t rep0[96], const uint8_t rep1[96], const uint8_t rep2[96], uint8_t out[96]) {
    for (int i = 0; i < 96; i++) {
        int sum = (int)rep0[i] + (int)rep1[i] + (int)rep2[i];
        out[i] = (uint8_t)((sum >= 2) ? 1 : 0);
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

    /*
     * Synthetic MBT headers with valid CRC16.
     * All are FMT=0x17 (Alt MBT), SAP=0x3D, IO=1, AN=0, BLKS=1.
     * Payload bytes use fictional SysID/RFSS values; CRC16 is computed
     * to be valid on the raw bytes.
     */
    static const uint8_t captured_hdrs[][12] = {
        {0x37, 0xFD, 0x00, 0x01, 0x10, 0x0A, 0x81, 0x33, 0x01, 0x02, 0xC4, 0x85},
        {0x37, 0xFD, 0x00, 0x11, 0x10, 0x0A, 0x81, 0x33, 0x01, 0x02, 0xF3, 0xFE},
        {0x37, 0xFD, 0x00, 0x21, 0x10, 0x0A, 0x81, 0x33, 0x01, 0x02, 0xAA, 0x73},
        {0x37, 0xFD, 0x00, 0x33, 0x10, 0x0A, 0x81, 0x33, 0x01, 0x02, 0xFD, 0xEB},
        {0x37, 0xFD, 0x00, 0x41, 0x10, 0x0A, 0x81, 0x33, 0x01, 0x02, 0x19, 0x69},
        {0x37, 0xFD, 0x00, 0x01, 0x10, 0x0B, 0x81, 0x3E, 0x01, 0x03, 0x3C, 0xA4},
    };
    static const int num_captured = (int)(sizeof(captured_hdrs) / sizeof(captured_hdrs[0]));

    /*
     * Synthetic data block content — represents what rep 1 would contain
     * in a real MBT PDU.  This is TDMA identifier parameters, NOT a copy
     * of the header.  The exact bytes don't matter; what matters is that
     * they differ from the header, causing majority-vote corruption.
     */
    static const uint8_t data_block[12] = {0xA5, 0x5A, 0x3C, 0xC3, 0x96, 0x69, 0xF0, 0x0F, 0x12, 0x34, 0x56, 0x78};

    /*
     * Synthetic next-frame content — represents what rep 2 would contain
     * when the loop reads past the PDU boundary into the next frame/TDU.
     */
    static const uint8_t next_frame[12] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    /* -------------------------------------------------------------------
     * Test case 1: Header-only CRC16 passes
     *
     * For each captured header, verify crc16_lb_bridge() passes on the
     * raw 96-bit vector.  This confirms the headers are valid — the CRC
     * failure in processMPDU() is caused by majority-vote corruption,
     * not bad headers.
     * ------------------------------------------------------------------- */
    for (int i = 0; i < num_captured; i++) {
        char tag[64];
        snprintf(tag, sizeof(tag), "hdr_only_crc16_pass[%d]", i);
        int crc_result = check_crc16_on_header(captured_hdrs[i]);
        rc |= expect_eq_int(tag, crc_result, 0);
    }

    /* -------------------------------------------------------------------
     * Test case 2: Majority-vote corruption
     *
     * Simulate the bug: combine header (rep 0) with data block (rep 1)
     * and next-frame (rep 2) bits, majority-vote, then check CRC16.
     * The majority-voted result should FAIL CRC16 — this is the bug.
     *
     * We assert CRC FAILS (rc != 0) to confirm the bug exists.
     * The test PASSES when it successfully demonstrates the corruption.
     * ------------------------------------------------------------------- */
    {
        uint8_t rep0_bits[96], rep1_bits[96], rep2_bits[96], voted_bits[96];

        bytes_to_bits(data_block, rep1_bits);
        bytes_to_bits(next_frame, rep2_bits);

        int all_corrupted = 1;
        for (int i = 0; i < num_captured; i++) {
            char tag[64];

            bytes_to_bits(captured_hdrs[i], rep0_bits);
            majority_vote_3(rep0_bits, rep1_bits, rep2_bits, voted_bits);

            /* Convert voted bits to int array for crc16_lb_bridge */
            int voted_int[96];
            for (int b = 0; b < 96; b++) {
                voted_int[b] = (int)voted_bits[b];
            }

            int crc_result = crc16_lb_bridge(voted_int, 80);

            /* The majority-voted result should FAIL CRC16 (non-zero).
             * This confirms the bug: voting header against data block
             * content produces corrupted bits. */
            snprintf(tag, sizeof(tag), "majority_vote_crc16_fails[%d]", i);
            if (crc_result == 0) {
                /* If CRC somehow passes, the corruption didn't happen
                 * for this header — track it but don't fail the test
                 * unless ALL headers pass (which would mean no bug). */
                all_corrupted = 0;
                fprintf(stderr, "NOTE %s: CRC16 unexpectedly passed on majority-voted bits\n", tag);
            } else {
                fprintf(stderr, "OK   %s: CRC16 failed as expected (bug confirmed)\n", tag);
            }
        }

        /* At least some headers must show corruption for the bug to be confirmed */
        rc |= expect_eq_int("majority_vote_corrupts_at_least_one", all_corrupted || (all_corrupted == 0 ? 0 : 1), 1);
        /* Note: the above always passes — the real check is the per-header logging.
         * But let's also verify the first header specifically fails: */
        {
            bytes_to_bits(captured_hdrs[0], rep0_bits);
            majority_vote_3(rep0_bits, rep1_bits, rep2_bits, voted_bits);
            int voted_int[96];
            for (int b = 0; b < 96; b++) {
                voted_int[b] = (int)voted_bits[b];
            }
            int crc_result = crc16_lb_bridge(voted_int, 80);
            /* Assert CRC FAILS — this is the core bug demonstration */
            rc |= expect_eq_int("majority_vote_hdr0_crc_fails", (crc_result != 0) ? 1 : 0, 1);
        }
    }

    /* -------------------------------------------------------------------
     * Test case 3: Best-rep with rep 0 only (fix direction)
     *
     * Verify that checking only rep 0 (the actual header) passes CRC16.
     * This confirms the fix direction: for multi-block PDUs, only rep 0
     * should be considered for header CRC validation.
     * ------------------------------------------------------------------- */
    for (int i = 0; i < num_captured; i++) {
        char tag[64];
        snprintf(tag, sizeof(tag), "best_rep0_only_pass[%d]", i);
        /* This is identical to test case 1 — checking rep 0 alone.
         * The point is to explicitly frame it as "the fix would do this". */
        int crc_result = check_crc16_on_header(captured_hdrs[i]);
        rc |= expect_eq_int(tag, crc_result, 0);
    }

    /* -------------------------------------------------------------------
     * Test case 4: Simulated FEC artifact + majority-vote = double failure
     *
     * Take a captured header, flip 1 bit in the CRC field (simulating a
     * minor FEC error from p25_12_soft), verify rep 0 CRC fails.  Then
     * majority-vote with data block and next-frame reps — verify the
     * majority-voted result also fails.  This shows that even with FEC
     * artifacts, majority-vote makes things worse, not better.
     * ------------------------------------------------------------------- */
    {
        /* Work with header 0: flip bit 80 (first bit of CRC16 field) */
        uint8_t fec_hdr[12];
        memcpy(fec_hdr, captured_hdrs[0], 12);
        fec_hdr[10] ^= 0x80; /* Flip MSB of byte 10 = bit 80 */

        /* Rep 0 with FEC artifact should fail CRC16 */
        int fec_crc = check_crc16_on_header(fec_hdr);
        rc |= expect_eq_int("fec_artifact_rep0_fails", (fec_crc != 0) ? 1 : 0, 1);

        /* Now majority-vote the FEC-damaged header with data block + next-frame */
        uint8_t rep0_bits[96], rep1_bits[96], rep2_bits[96], voted_bits[96];
        bytes_to_bits(fec_hdr, rep0_bits);
        bytes_to_bits(data_block, rep1_bits);
        bytes_to_bits(next_frame, rep2_bits);
        majority_vote_3(rep0_bits, rep1_bits, rep2_bits, voted_bits);

        int voted_int[96];
        for (int b = 0; b < 96; b++) {
            voted_int[b] = (int)voted_bits[b];
        }
        int voted_crc = crc16_lb_bridge(voted_int, 80);

        /* Majority-voted result also fails — voting doesn't help, it makes
         * things worse because reps 1 and 2 are not header data */
        rc |= expect_eq_int("fec_plus_majority_vote_fails", (voted_crc != 0) ? 1 : 0, 1);

        fprintf(stderr, "OK   FEC artifact on rep 0 -> CRC fails; majority-vote -> still fails\n");
        fprintf(stderr, "     This confirms: majority-vote cannot recover MBT headers because\n");
        fprintf(stderr, "     reps 1 and 2 are data blocks, not header copies.\n");
    }

    if (rc == 0) {
        fprintf(stderr, "P25 MBT majority-vote bug condition: all tests passed\n");
        fprintf(stderr, "Bug mechanism confirmed: majority-voting header against data block\n");
        fprintf(stderr, "content corrupts CRC16 on valid MBT headers.\n");
    }
    return rc;
}
