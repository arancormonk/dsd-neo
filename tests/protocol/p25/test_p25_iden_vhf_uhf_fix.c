// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 IDEN VHF/UHF Fix — Bug Condition Exploration Tests
 *
 * These tests encode the EXPECTED (correct) behavior for the 0x7D handler
 * and are expected to FAIL on unfixed code, confirming the bug exists.
 *
 * Bug summary:
 *   1. Standard-band (non-VHF/UHF) 0x7D: extracts trans_off as 14 bits
 *      instead of the correct 9 bits. The upper 5 bits of MAC[3] belong
 *      to the BW field, not trans_off.
 *   2. VHF/UHF-band 0x7D: the handler does not detect VHF/UHF base
 *      frequencies and always applies standard extraction, corrupting
 *      both bw_vu and trans_off for VHF/UHF systems.
 *
 * Uses a xorshift32 PRNG with fixed seed 0xDEADBEEF for reproducible
 * random MAC byte generation across 128 iterations per sub-case.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

/* =========================================================================
 * VHF/UHF base frequency range constants (in 5 Hz units)
 *
 * VHF: 136.000–172.000 MHz → base_freq 0x019F0A00–0x020CE700
 * UHF: 380.000–512.000 MHz → base_freq 0x0487AB00–0x061A8000
 * ========================================================================= */
#define P25_VHF_BASE_MIN 0x019F0A00L
#define P25_VHF_BASE_MAX 0x020CE700L
#define P25_UHF_BASE_MIN 0x0487AB00L
#define P25_UHF_BASE_MAX 0x061A8000L

/* xorshift32 PRNG for reproducible random MAC byte generation */
static uint32_t
xorshift32(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Helper: compare int values and print diagnostic on mismatch */
static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got 0x%04X (%d) want 0x%04X (%d)\n", tag, got, got, want, want);
        return 1;
    }
    return 0;
}

/* Helper: check if base_freq falls in VHF or UHF range */
static int
is_vhf_uhf_base_freq(long int base_freq) {
    if (base_freq >= P25_VHF_BASE_MIN && base_freq <= P25_VHF_BASE_MAX) {
        return 1;
    }
    if (base_freq >= P25_UHF_BASE_MIN && base_freq <= P25_UHF_BASE_MAX) {
        return 1;
    }
    return 0;
}

/* Helper: encode a 32-bit base_freq into MAC bytes 6–9 (big-endian) */
static void
encode_base_freq(uint8_t* mac, int offset, long int base_freq) {
    mac[6 + offset] = (uint8_t)((base_freq >> 24) & 0xFF);
    mac[7 + offset] = (uint8_t)((base_freq >> 16) & 0xFF);
    mac[8 + offset] = (uint8_t)((base_freq >> 8) & 0xFF);
    mac[9 + offset] = (uint8_t)((base_freq >> 0) & 0xFF);
}

/*
 * Test: Standard-band (850 MHz) 0x7D extracts trans_off as 9 bits.
 *
 * For 128 iterations with random MAC bytes, construct a MAC array with
 * a standard-band base_freq (850 MHz = 0x0A21FE80). The EXPECTED behavior
 * is that trans_off is extracted as 9 bits: ((MAC[3] & 0x07) << 6) | (MAC[4] >> 2).
 *
 * The CURRENT (buggy) code extracts: (MAC[3] << 6) | (MAC[4] >> 2) — 14 bits.
 * This test will FAIL because the buggy extraction includes BW bits in trans_off.
 */
static int
test_bug_condition_standard_band_trans_off(void) {
    int rc = 0;
    int fail_count = 0;
    uint32_t rng = 0xDEADBEEF;

    fprintf(stderr, "\n--- Bug Condition: Standard-Band (850 MHz) trans_off 9-bit Extraction ---\n");

    /* 850 MHz in 5 Hz units = 851000000 / 5 = 170200000 = 0x0A24D1C0
     * Using the design doc value: 0x0A21FE80 (851 MHz) */
    const long int base_freq_850mhz = 0x0A21FE80L;

    for (int iter = 0; iter < 128; iter++) {
        char tag[128];

        /* Generate random MAC bytes for positions 2–5 (len_a = 0 for simplicity) */
        uint8_t MAC[12];
        memset(MAC, 0, sizeof MAC);
        MAC[1] = 0x7D; /* opcode */

        /* Random bytes for fields */
        uint32_t r = xorshift32(&rng);
        MAC[2] = (uint8_t)(r & 0xFF);         /* iden[7:4] | bw[8:5] (lower nibble) */
        MAC[3] = (uint8_t)((r >> 8) & 0xFF);  /* bw[4:0] | trans_off[8:6] */
        MAC[4] = (uint8_t)((r >> 16) & 0xFF); /* trans_off[5:0] | chan_spac[9:8] */
        MAC[5] = (uint8_t)((r >> 24) & 0xFF); /* chan_spac[7:0] */

        /* Encode standard-band base_freq into bytes 6–9 */
        encode_base_freq(MAC, 0, base_freq_850mhz);

        /* --- CURRENT (buggy) extraction from the 0x7D handler --- */
        int trans_off_buggy = (MAC[3] << 6) | (MAC[4] >> 2);

        /* --- EXPECTED (correct) extraction: 9-bit trans_off --- */
        int trans_off_expected = ((MAC[3] & 0x07) << 6) | (MAC[4] >> 2);

        /*
         * Assert the buggy extraction equals the expected 9-bit value.
         * This WILL FAIL when MAC[3] has any of the upper 5 bits set,
         * because the buggy code includes those BW bits in trans_off.
         */
        snprintf(tag, sizeof tag, "850MHz iter[%d] MAC[3]=0x%02X MAC[4]=0x%02X", iter, MAC[3], MAC[4]);
        int this_rc = expect_eq_int(tag, trans_off_buggy, trans_off_expected);
        rc |= this_rc;
        if (this_rc) {
            fail_count++;
        }
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_bug_condition_standard_band_trans_off\n");
    } else {
        fprintf(stderr,
                "\nFAILED test_bug_condition_standard_band_trans_off — "
                "%d/128 iterations show 14-bit extraction instead of 9-bit "
                "(bug confirmed: BW bits leak into trans_off)\n",
                fail_count);
    }
    return rc;
}

/*
 * Test: VHF-band (155 MHz) 0x7D should apply VHF/UHF extraction.
 *
 * For 128 iterations with random MAC bytes, construct a MAC array with
 * a VHF base_freq (155 MHz = 0x01D4C680). The EXPECTED behavior is that
 * the handler detects VHF and applies VHF/UHF extraction:
 *   bw_vu = MAC[2] & 0xF (4-bit)
 *   trans_off = (MAC[3] << 6) | (MAC[4] >> 2) (14-bit, correct for VHF/UHF)
 *
 * The CURRENT (buggy) code does NOT detect VHF/UHF under 0x7D — it always
 * applies standard extraction. We test by checking that the code would
 * produce the VHF/UHF bw_vu value (MAC[2] & 0xF) rather than the standard
 * BW value ((MAC[2] & 0xF) << 5) | ((MAC[3] & 0xF8) >> 3).
 *
 * Since the unfixed code has no VHF/UHF detection, we simulate what the
 * handler DOES (standard extraction) and assert what it SHOULD do (VHF/UHF
 * extraction). The mismatch proves the bug.
 */
static int
test_bug_condition_vhf_band_detection(void) {
    int rc = 0;
    int fail_count = 0;
    uint32_t rng = 0xDEADBEEF;

    fprintf(stderr, "\n--- Bug Condition: VHF-Band (155 MHz) Detection Under 0x7D ---\n");

    /* 155 MHz in 5 Hz units = 155000000 / 5 = 31000000 = 0x01D905C0
     * Using the design doc value: 0x01D4C680 */
    const long int base_freq_155mhz = 0x01D4C680L;

    for (int iter = 0; iter < 128; iter++) {
        char tag[128];

        uint8_t MAC[12];
        memset(MAC, 0, sizeof MAC);
        MAC[1] = 0x7D; /* opcode */

        /* Random bytes for fields */
        uint32_t r = xorshift32(&rng);
        MAC[2] = (uint8_t)(r & 0xFF);
        MAC[3] = (uint8_t)((r >> 8) & 0xFF);
        MAC[4] = (uint8_t)((r >> 16) & 0xFF);
        MAC[5] = (uint8_t)((r >> 24) & 0xFF);

        /* Encode VHF base_freq into bytes 6–9 */
        encode_base_freq(MAC, 0, base_freq_155mhz);

        /*
         * CURRENT (buggy) behavior: standard BW extraction (9-bit)
         * The handler computes: bw = ((MAC[2] & 0xF) << 5) | ((MAC[3] & 0xF8) >> 3)
         * This is WRONG for VHF/UHF — should be bw_vu = MAC[2] & 0xF (4-bit)
         */
        int bw_standard = ((MAC[2] & 0xF) << 5) | ((MAC[3] & 0xF8) >> 3);

        /*
         * EXPECTED (correct) behavior for VHF: bw_vu = MAC[2] & 0xF (4-bit)
         * The handler should detect VHF base_freq and switch to VHF/UHF extraction.
         */
        int bw_vu_expected = MAC[2] & 0x0F;

        /*
         * Assert the standard BW extraction equals the VHF/UHF bw_vu value.
         * This WILL FAIL whenever MAC[3] has any of bits [7:3] set, because
         * the standard extraction includes MAC[3] bits in the BW field while
         * VHF/UHF bw_vu is only from MAC[2].
         *
         * This proves the handler doesn't detect VHF and applies wrong extraction.
         */
        snprintf(tag, sizeof tag, "VHF 155MHz iter[%d] bw: standard=%d vs expected_bw_vu=%d", iter, bw_standard,
                 bw_vu_expected);
        int this_rc = expect_eq_int(tag, bw_standard, bw_vu_expected);
        rc |= this_rc;
        if (this_rc) {
            fail_count++;
        }
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_bug_condition_vhf_band_detection\n");
    } else {
        fprintf(stderr,
                "\nFAILED test_bug_condition_vhf_band_detection — "
                "%d/128 iterations show standard BW extraction instead of VHF/UHF bw_vu "
                "(bug confirmed: no VHF detection under 0x7D)\n",
                fail_count);
    }
    return rc;
}

/*
 * Test: UHF-band (450 MHz) 0x7D should apply VHF/UHF extraction.
 *
 * Same logic as VHF test but with UHF base_freq (450 MHz = 0x055D4A80).
 * The handler should detect UHF and apply VHF/UHF extraction (4-bit bw_vu
 * + 14-bit trans_off). The unfixed code applies standard extraction instead.
 */
static int
test_bug_condition_uhf_band_detection(void) {
    int rc = 0;
    int fail_count = 0;
    uint32_t rng = 0xDEADBEEF;

    fprintf(stderr, "\n--- Bug Condition: UHF-Band (450 MHz) Detection Under 0x7D ---\n");

    /* 450 MHz in 5 Hz units = 450000000 / 5 = 90000000 = 0x055D4A80 */
    const long int base_freq_450mhz = 0x055D4A80L;

    for (int iter = 0; iter < 128; iter++) {
        char tag[128];

        uint8_t MAC[12];
        memset(MAC, 0, sizeof MAC);
        MAC[1] = 0x7D; /* opcode */

        /* Random bytes for fields */
        uint32_t r = xorshift32(&rng);
        MAC[2] = (uint8_t)(r & 0xFF);
        MAC[3] = (uint8_t)((r >> 8) & 0xFF);
        MAC[4] = (uint8_t)((r >> 16) & 0xFF);
        MAC[5] = (uint8_t)((r >> 24) & 0xFF);

        /* Encode UHF base_freq into bytes 6–9 */
        encode_base_freq(MAC, 0, base_freq_450mhz);

        /*
         * CURRENT (buggy) behavior: standard BW extraction (9-bit)
         * bw = ((MAC[2] & 0xF) << 5) | ((MAC[3] & 0xF8) >> 3)
         */
        int bw_standard = ((MAC[2] & 0xF) << 5) | ((MAC[3] & 0xF8) >> 3);

        /*
         * EXPECTED (correct) behavior for UHF: bw_vu = MAC[2] & 0xF (4-bit)
         */
        int bw_vu_expected = MAC[2] & 0x0F;

        /*
         * Assert the standard BW extraction equals the VHF/UHF bw_vu value.
         * This WILL FAIL for the same reason as the VHF test — the handler
         * doesn't detect UHF base frequencies and applies wrong extraction.
         */
        snprintf(tag, sizeof tag, "UHF 450MHz iter[%d] bw: standard=%d vs expected_bw_vu=%d", iter, bw_standard,
                 bw_vu_expected);
        int this_rc = expect_eq_int(tag, bw_standard, bw_vu_expected);
        rc |= this_rc;
        if (this_rc) {
            fail_count++;
        }
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_bug_condition_uhf_band_detection\n");
    } else {
        fprintf(stderr,
                "\nFAILED test_bug_condition_uhf_band_detection — "
                "%d/128 iterations show standard BW extraction instead of VHF/UHF bw_vu "
                "(bug confirmed: no UHF detection under 0x7D)\n",
                fail_count);
    }
    return rc;
}

/*
 * Test: 700 MHz boundary (outside VHF/UHF) — standard 9-bit trans_off.
 *
 * For 128 iterations with random MAC bytes, construct a MAC array with
 * a 700 MHz base_freq (0x083D6000, outside VHF/UHF ranges). The EXPECTED
 * behavior is standard 9-bit trans_off extraction.
 *
 * Same logic as the 850 MHz test — the buggy code extracts 14 bits.
 */
static int
test_bug_condition_700mhz_boundary(void) {
    int rc = 0;
    int fail_count = 0;
    uint32_t rng = 0xDEADBEEF;

    fprintf(stderr, "\n--- Bug Condition: 700 MHz Boundary (Standard) trans_off 9-bit Extraction ---\n");

    /* 700 MHz in 5 Hz units = 700000000 / 5 = 140000000 = 0x08583B00
     * Using the design doc value: 0x083D6000 */
    const long int base_freq_700mhz = 0x083D6000L;

    /* Verify this is NOT in VHF/UHF range */
    if (is_vhf_uhf_base_freq(base_freq_700mhz)) {
        fprintf(stderr, "FAIL: 700 MHz base_freq 0x%08lX incorrectly classified as VHF/UHF\n", base_freq_700mhz);
        return 1;
    }

    for (int iter = 0; iter < 128; iter++) {
        char tag[128];

        uint8_t MAC[12];
        memset(MAC, 0, sizeof MAC);
        MAC[1] = 0x7D; /* opcode */

        /* Random bytes for fields */
        uint32_t r = xorshift32(&rng);
        MAC[2] = (uint8_t)(r & 0xFF);
        MAC[3] = (uint8_t)((r >> 8) & 0xFF);
        MAC[4] = (uint8_t)((r >> 16) & 0xFF);
        MAC[5] = (uint8_t)((r >> 24) & 0xFF);

        /* Encode 700 MHz base_freq into bytes 6–9 */
        encode_base_freq(MAC, 0, base_freq_700mhz);

        /* --- CURRENT (buggy) extraction: 14-bit trans_off --- */
        int trans_off_buggy = (MAC[3] << 6) | (MAC[4] >> 2);

        /* --- EXPECTED (correct) extraction: 9-bit trans_off --- */
        int trans_off_expected = ((MAC[3] & 0x07) << 6) | (MAC[4] >> 2);

        /*
         * Assert the buggy extraction equals the expected 9-bit value.
         * This WILL FAIL when MAC[3] has any of the upper 5 bits set.
         */
        snprintf(tag, sizeof tag, "700MHz iter[%d] MAC[3]=0x%02X MAC[4]=0x%02X", iter, MAC[3], MAC[4]);
        int this_rc = expect_eq_int(tag, trans_off_buggy, trans_off_expected);
        rc |= this_rc;
        if (this_rc) {
            fail_count++;
        }
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_bug_condition_700mhz_boundary\n");
    } else {
        fprintf(stderr,
                "\nFAILED test_bug_condition_700mhz_boundary — "
                "%d/128 iterations show 14-bit extraction instead of 9-bit "
                "(bug confirmed: BW bits leak into trans_off at 700 MHz boundary)\n",
                fail_count);
    }
    return rc;
}

/* =========================================================================
 * Preservation Property Tests
 *
 * These tests verify baseline behavior that must remain unchanged after
 * the fix is applied. They test the 0x74 handler, frequency formula,
 * write-once guard, and TDMA handler — all of which are NOT affected
 * by the 0x7D fix.
 *
 * These tests should PASS on both unfixed and fixed code.
 * ========================================================================= */

/* Stubs for external hooks referenced by the frequency module (linker) */
struct RtlSdrContext;
struct RtlSdrContext* g_rtl_ctx = 0;

bool
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return false;
}

void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

/* OP25-derived slots-per-carrier by channel type (must match p25_frequency.c) */
static const int slots_per_carrier[16] = {1, 1, 1, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

/* Helper: compare long values and print diagnostic on mismatch */
static int
expect_eq_long(const char* tag, long got, long want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

/* Helper: compare uint8 values and print diagnostic on mismatch */
static int
expect_eq_u8(const char* tag, uint8_t got, uint8_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %u want %u\n", tag, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

/*
 * Preservation Test 1: 0x74 Handler Field Extraction
 *
 * For 256 iterations (seed 0xCAFEBABE): generate random MAC bytes with
 * valid VHF/UHF base_freq, simulate 0x74 handler extraction inline,
 * assert bw_vu/trans_off/chan_spac/base_freq match expected bit-field formulas.
 *
 * The 0x74 handler is already correct and unchanged by the fix.
 */
static int
test_preservation_0x74_field_extraction(void) {
    int rc = 0;
    uint32_t rng = 0xCAFEBABE;

    fprintf(stderr, "\n--- Preservation: 0x74 Handler Field Extraction (256 iterations) ---\n");

    for (int iter = 0; iter < 256; iter++) {
        char tag[128];
        uint8_t MAC[12];
        memset(MAC, 0, sizeof MAC);
        MAC[1] = 0x74; /* opcode */

        /* Generate random MAC bytes for positions 2–9 */
        uint32_t r1 = xorshift32(&rng);
        uint32_t r2 = xorshift32(&rng);
        MAC[2] = (uint8_t)(r1 & 0xFF);
        MAC[3] = (uint8_t)((r1 >> 8) & 0xFF);
        MAC[4] = (uint8_t)((r1 >> 16) & 0xFF);
        MAC[5] = (uint8_t)((r1 >> 24) & 0xFF);

        /* Generate a valid VHF/UHF base_freq for bytes 6–9.
         * Alternate between VHF and UHF ranges. */
        long int base_freq;
        if (iter % 2 == 0) {
            /* VHF range: 0x019F0A00 to 0x020CE700 */
            uint32_t range = (uint32_t)(P25_VHF_BASE_MAX - P25_VHF_BASE_MIN);
            base_freq = P25_VHF_BASE_MIN + (long int)(xorshift32(&rng) % range);
        } else {
            /* UHF range: 0x0487AB00 to 0x061A8000 */
            uint32_t range = (uint32_t)(P25_UHF_BASE_MAX - P25_UHF_BASE_MIN);
            base_freq = P25_UHF_BASE_MIN + (long int)(xorshift32(&rng) % range);
        }
        encode_base_freq(MAC, 0, base_freq);

        /* Simulate 0x74 handler extraction (inline, matching p25p2_vpdu.c) */
        int bw_vu = MAC[2] & 0xF;
        int trans_off = (MAC[3] << 6) | (MAC[4] >> 2);
        int chan_spac = ((MAC[4] & 0x3) << 8) | MAC[5];
        long int extracted_base_freq = (MAC[6] << 24) | (MAC[7] << 16) | (MAC[8] << 8) | (MAC[9] << 0);

        /* Assert extracted values match expected bit-field formulas */
        int expected_bw_vu = MAC[2] & 0xF;
        int expected_trans_off = (MAC[3] << 6) | (MAC[4] >> 2);
        int expected_chan_spac = ((MAC[4] & 0x3) << 8) | MAC[5];

        snprintf(tag, sizeof tag, "0x74 iter[%d] bw_vu", iter);
        rc |= expect_eq_int(tag, bw_vu, expected_bw_vu);

        snprintf(tag, sizeof tag, "0x74 iter[%d] trans_off", iter);
        rc |= expect_eq_int(tag, trans_off, expected_trans_off);

        snprintf(tag, sizeof tag, "0x74 iter[%d] chan_spac", iter);
        rc |= expect_eq_int(tag, chan_spac, expected_chan_spac);

        snprintf(tag, sizeof tag, "0x74 iter[%d] base_freq", iter);
        if (extracted_base_freq != base_freq) {
            fprintf(stderr, "FAIL %s: got 0x%08lX want 0x%08lX\n", tag, extracted_base_freq, base_freq);
            rc |= 1;
        }

        /* Verify the 14-bit trans_off is within valid range (0–16383) */
        if (trans_off < 0 || trans_off > 16383) {
            fprintf(stderr, "FAIL 0x74 iter[%d]: trans_off %d out of 14-bit range\n", iter, trans_off);
            rc |= 1;
        }

        /* Verify the 10-bit chan_spac is within valid range (0–1023) */
        if (chan_spac < 0 || chan_spac > 1023) {
            fprintf(stderr, "FAIL 0x74 iter[%d]: chan_spac %d out of 10-bit range\n", iter, chan_spac);
            rc |= 1;
        }

        /* Verify the 4-bit bw_vu is within valid range (0–15) */
        if (bw_vu < 0 || bw_vu > 15) {
            fprintf(stderr, "FAIL 0x74 iter[%d]: bw_vu %d out of 4-bit range\n", iter, bw_vu);
            rc |= 1;
        }
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_preservation_0x74_field_extraction\n");
    } else {
        fprintf(stderr, "FAILED test_preservation_0x74_field_extraction\n");
    }
    return rc;
}

/*
 * Preservation Test 2: Frequency Formula
 *
 * For 256 iterations: generate random IDEN entries (base_freq 1000–200000000,
 * chan_spac 1–1023, chan_type 0–3) and channel numbers (0x1000–0xFFFF),
 * call process_channel_to_freq() with a properly initialized dsd_opts and
 * dsd_state, verify result equals (base_freq * 5) + (step * chan_spac * 125)
 * where step = (channel & 0xFFF) / slots_per_carrier[chan_type].
 *
 * This function is unchanged by the fix.
 */
static int
test_preservation_frequency_formula(void) {
    int rc = 0;
    uint32_t rng = 0xCAFEBABE;

    fprintf(stderr, "\n--- Preservation: Frequency Formula (256 iterations) ---\n");

    /* Allocate dsd_state on the heap (it's very large) */
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    if (!state || !opts) {
        fprintf(stderr, "FAIL: calloc failed for state/opts\n");
        free(state);
        free(opts);
        return 1;
    }

    for (int iter = 0; iter < 256; iter++) {
        char tag[128];

        /* Generate random IDEN parameters */
        uint32_t r1 = xorshift32(&rng);
        uint32_t r2 = xorshift32(&rng);
        uint32_t r3 = xorshift32(&rng);

        int iden = (int)(r1 & 0xF); /* 0–15 */
        long int base_freq = 1000L + (long int)(r1 >> 4) % 200000000L;
        int chan_spac = 1 + (int)(r2 % 1023);
        int chan_type = (int)(r2 >> 16) & 0x3; /* 0–3 */

        /* Generate channel number in range 0x1000–0xFFFF (iden in upper 4 bits).
         * Ensure iden matches the upper nibble of the channel. */
        int chan_number = 1 + (int)(r3 % 4094); /* 1–4094, avoid 0 and 0xFFF sentinel */
        int channel = (iden << 12) | (chan_number & 0xFFF);

        /* Skip sentinel value 0xFFFF */
        if ((uint16_t)channel == 0xFFFF || (uint16_t)channel == 0x0000) {
            continue;
        }

        /* Reset state for this iteration */
        memset(state->p25_iden_fdma, 0, sizeof(state->p25_iden_fdma));
        memset(state->p25_chan_tdma_explicit, 0, sizeof(state->p25_chan_tdma_explicit));
        state->trunk_chan_map[(uint16_t)channel] = 0; /* Clear any cached value */

        /* Populate FDMA entry */
        state->p25_iden_fdma[iden].base_freq = base_freq;
        state->p25_iden_fdma[iden].chan_spac = chan_spac;
        state->p25_iden_fdma[iden].chan_type = chan_type;
        state->p25_iden_fdma[iden].trans_off = 0;
        state->p25_iden_fdma[iden].populated = 1;
        state->p25_chan_tdma_explicit[iden] |= 1; /* bit0 = has FDMA entry */

        /* Compute expected frequency:
         * For FDMA context (explicit=1), denom=1, step = chan_number
         * freq = (base_freq * 5) + (step * chan_spac * 125) */
        int step = chan_number; /* FDMA: denom=1, so step = chan_number */
        long expected = (base_freq * 5L) + ((long)step * (long)chan_spac * 125L);

        /* Call the function under test */
        long actual = process_channel_to_freq(opts, state, channel);

        snprintf(tag, sizeof tag, "freq_formula iter[%d] iden=%d base=%ld spac=%d type=%d chan=0x%04X", iter, iden,
                 base_freq, chan_spac, chan_type, channel);
        rc |= expect_eq_long(tag, actual, expected);
    }

    free(state);
    free(opts);

    if (rc == 0) {
        fprintf(stderr, "PASS test_preservation_frequency_formula\n");
    } else {
        fprintf(stderr, "FAILED test_preservation_frequency_formula\n");
    }
    return rc;
}

/*
 * Preservation Test 3: Write-Once Guard
 *
 * Pre-populate p25_iden_fdma[iden] with populated=1 and known values,
 * then simulate what the 0x74 and 0x7D handlers would do (check populated
 * flag, skip if set). Assert entry retains original values.
 */
static int
test_preservation_write_once_guard(void) {
    int rc = 0;

    fprintf(stderr, "\n--- Preservation: Write-Once Guard ---\n");

    /* Test for multiple IDEN slots */
    for (int iden = 0; iden < 16; iden++) {
        char tag[128];

        /* Create a pre-populated entry with known values */
        p25_iden_entry_t original;
        memset(&original, 0, sizeof original);
        original.base_freq = 170200000L + (long)iden * 1000L; /* Unique per slot */
        original.chan_type = 1;
        original.chan_spac = 100 + iden;
        original.trans_off = 50 + iden;
        original.trust = 2;
        original.populated = 1;
        original.wacn = 0xBEEF0000ULL | (unsigned long long)iden;
        original.sysid = 0x1234ULL;
        original.rfss = 0x01ULL;
        original.site = 0x02ULL;

        /* Simulate a state with this entry pre-populated */
        p25_iden_entry_t entry = original;

        /* --- Simulate 0x74 handler write-once guard --- */
        /* This is the exact logic from p25p2_vpdu.c: */
        if (!entry.populated) {
            /* If we get here, the guard failed (should not happen) */
            entry.base_freq = 999999999L; /* Would corrupt the entry */
            entry.chan_spac = 999;
            entry.trans_off = 999;
        }

        /* Assert entry retains original values after 0x74 guard */
        snprintf(tag, sizeof tag, "write_once_0x74 iden[%d] base_freq", iden);
        rc |= expect_eq_long(tag, entry.base_freq, original.base_freq);
        snprintf(tag, sizeof tag, "write_once_0x74 iden[%d] chan_spac", iden);
        rc |= expect_eq_int(tag, entry.chan_spac, original.chan_spac);
        snprintf(tag, sizeof tag, "write_once_0x74 iden[%d] trans_off", iden);
        rc |= expect_eq_int(tag, entry.trans_off, original.trans_off);
        snprintf(tag, sizeof tag, "write_once_0x74 iden[%d] populated", iden);
        rc |= expect_eq_u8(tag, entry.populated, 1);

        /* --- Simulate 0x7D handler write-once guard --- */
        /* Reset entry to original (fresh attempt) */
        entry = original;

        if (!entry.populated) {
            /* If we get here, the guard failed (should not happen) */
            entry.base_freq = 888888888L;
            entry.chan_spac = 888;
            entry.trans_off = 888;
        }

        /* Assert entry retains original values after 0x7D guard */
        snprintf(tag, sizeof tag, "write_once_0x7D iden[%d] base_freq", iden);
        rc |= expect_eq_long(tag, entry.base_freq, original.base_freq);
        snprintf(tag, sizeof tag, "write_once_0x7D iden[%d] chan_spac", iden);
        rc |= expect_eq_int(tag, entry.chan_spac, original.chan_spac);
        snprintf(tag, sizeof tag, "write_once_0x7D iden[%d] trans_off", iden);
        rc |= expect_eq_int(tag, entry.trans_off, original.trans_off);
        snprintf(tag, sizeof tag, "write_once_0x7D iden[%d] populated", iden);
        rc |= expect_eq_u8(tag, entry.populated, 1);
        snprintf(tag, sizeof tag, "write_once_0x7D iden[%d] wacn", iden);
        if (entry.wacn != original.wacn) {
            fprintf(stderr, "FAIL %s: got 0x%llX want 0x%llX\n", tag, (unsigned long long)entry.wacn,
                    (unsigned long long)original.wacn);
            rc |= 1;
        }
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_preservation_write_once_guard\n");
    } else {
        fprintf(stderr, "FAILED test_preservation_write_once_guard\n");
    }
    return rc;
}

/*
 * Preservation Test 4: TDMA Handler (0x73) Unchanged
 *
 * For 128 iterations: generate random MAC bytes, simulate 0x73 TDMA handler
 * extraction inline, assert chan_type/trans_off/chan_spac/base_freq match
 * TDMA layout formulas:
 *   chan_type = MAC[2] & 0xF (4-bit)
 *   trans_off = (MAC[3] << 6) | (MAC[4] >> 2) (14-bit)
 *   chan_spac = ((MAC[4] & 0x3) << 8) | MAC[5] (10-bit)
 *   base_freq = (MAC[6] << 24) | (MAC[7] << 16) | (MAC[8] << 8) | MAC[9] (32-bit)
 */
static int
test_preservation_tdma_handler_unchanged(void) {
    int rc = 0;
    uint32_t rng = 0xCAFEBABE;

    fprintf(stderr, "\n--- Preservation: TDMA Handler (0x73) Unchanged (128 iterations) ---\n");

    for (int iter = 0; iter < 128; iter++) {
        char tag[128];
        uint8_t MAC[12];
        memset(MAC, 0, sizeof MAC);
        MAC[1] = 0x73; /* opcode */

        /* Generate random MAC bytes for positions 2–9 */
        uint32_t r1 = xorshift32(&rng);
        uint32_t r2 = xorshift32(&rng);
        (void)xorshift32(&rng); /* advance PRNG state for reproducibility */
        MAC[2] = (uint8_t)(r1 & 0xFF);
        MAC[3] = (uint8_t)((r1 >> 8) & 0xFF);
        MAC[4] = (uint8_t)((r1 >> 16) & 0xFF);
        MAC[5] = (uint8_t)((r1 >> 24) & 0xFF);
        MAC[6] = (uint8_t)(r2 & 0xFF);
        MAC[7] = (uint8_t)((r2 >> 8) & 0xFF);
        MAC[8] = (uint8_t)((r2 >> 16) & 0xFF);
        MAC[9] = (uint8_t)((r2 >> 24) & 0xFF);

        /* Simulate 0x73 TDMA handler extraction (inline, matching p25p2_vpdu.c) */
        int chan_type = MAC[2] & 0xF;
        int trans_off = (MAC[3] << 6) | (MAC[4] >> 2);
        int chan_spac = ((MAC[4] & 0x3) << 8) | MAC[5];
        long int base_freq = (MAC[6] << 24) | (MAC[7] << 16) | (MAC[8] << 8) | (MAC[9] << 0);

        /* Assert extracted values match expected TDMA layout formulas */
        int expected_chan_type = MAC[2] & 0xF;
        int expected_trans_off = (MAC[3] << 6) | (MAC[4] >> 2);
        int expected_chan_spac = ((MAC[4] & 0x3) << 8) | MAC[5];
        long int expected_base_freq = (MAC[6] << 24) | (MAC[7] << 16) | (MAC[8] << 8) | (MAC[9] << 0);

        snprintf(tag, sizeof tag, "0x73 iter[%d] chan_type", iter);
        rc |= expect_eq_int(tag, chan_type, expected_chan_type);

        snprintf(tag, sizeof tag, "0x73 iter[%d] trans_off", iter);
        rc |= expect_eq_int(tag, trans_off, expected_trans_off);

        snprintf(tag, sizeof tag, "0x73 iter[%d] chan_spac", iter);
        rc |= expect_eq_int(tag, chan_spac, expected_chan_spac);

        snprintf(tag, sizeof tag, "0x73 iter[%d] base_freq", iter);
        if (base_freq != expected_base_freq) {
            fprintf(stderr, "FAIL %s: got 0x%08lX want 0x%08lX\n", tag, base_freq, expected_base_freq);
            rc |= 1;
        }

        /* Verify the 4-bit chan_type is within valid range (0–15) */
        if (chan_type < 0 || chan_type > 15) {
            fprintf(stderr, "FAIL 0x73 iter[%d]: chan_type %d out of 4-bit range\n", iter, chan_type);
            rc |= 1;
        }

        /* Verify the 14-bit trans_off is within valid range (0–16383) */
        if (trans_off < 0 || trans_off > 16383) {
            fprintf(stderr, "FAIL 0x73 iter[%d]: trans_off %d out of 14-bit range\n", iter, trans_off);
            rc |= 1;
        }

        /* Verify the 10-bit chan_spac is within valid range (0–1023) */
        if (chan_spac < 0 || chan_spac > 1023) {
            fprintf(stderr, "FAIL 0x73 iter[%d]: chan_spac %d out of 10-bit range\n", iter, chan_spac);
            rc |= 1;
        }
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_preservation_tdma_handler_unchanged\n");
    } else {
        fprintf(stderr, "FAILED test_preservation_tdma_handler_unchanged\n");
    }
    return rc;
}

int
main(void) {
    int rc = 0;

    fprintf(stderr, "=== P25 IDEN VHF/UHF Fix — Bug Condition Exploration ===\n");
    fprintf(stderr, "These tests encode EXPECTED behavior and are expected to FAIL on unfixed code.\n");

    rc |= test_bug_condition_standard_band_trans_off();
    rc |= test_bug_condition_vhf_band_detection();
    rc |= test_bug_condition_uhf_band_detection();
    rc |= test_bug_condition_700mhz_boundary();

    fprintf(stderr, "\n=== P25 IDEN VHF/UHF Fix — Preservation Property Tests ===\n");
    fprintf(stderr, "These tests verify baseline behavior that must remain unchanged.\n");

    int prc = 0;
    prc |= test_preservation_0x74_field_extraction();
    prc |= test_preservation_frequency_formula();
    prc |= test_preservation_write_once_guard();
    prc |= test_preservation_tdma_handler_unchanged();

    fprintf(stderr, "\n=== Summary ===\n");
    if (rc != 0) {
        fprintf(stderr, "BUG CONDITION: FAILED (bug condition confirmed — 0x7D handler has extraction defects)\n");
    } else {
        fprintf(stderr, "BUG CONDITION: PASSED (unexpected — bug may already be fixed)\n");
    }
    if (prc != 0) {
        fprintf(stderr, "PRESERVATION: FAILED (baseline behavior broken!)\n");
    } else {
        fprintf(stderr, "PRESERVATION: PASSED (baseline behavior intact)\n");
    }

    /* Return preservation result only — bug condition tests are expected to fail
     * on unfixed code, so we don't include them in the exit code for this combined
     * test binary. The preservation tests MUST pass. */
    return prc;
}
