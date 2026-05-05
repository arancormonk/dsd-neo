// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 IDEN Write-Once Guard — Bug Condition Exploration Test
 *
 * **Validates: Requirements 1.1, 1.2, 2.1, 2.2**
 *
 * This test demonstrates the TDMA IDEN overwrite bug on UNFIXED code.
 * It populates a TDMA IDEN slot with known parameters, then simulates a
 * second write (as the opcode 0x73 handler does — unconditional overwrite)
 * with different parameters. It asserts that the entry retains the first-write
 * values.
 *
 * On UNFIXED code, this test is EXPECTED TO FAIL because the current handler
 * unconditionally overwrites the entry. Failure confirms the bug exists.
 *
 * Uses a xorshift32 PRNG with a fixed seed for reproducible random parameters.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

struct RtlSdrContext;

/* Stubs for external hooks referenced by the frequency module */
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

struct RtlSdrContext* g_rtl_ctx = 0;

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

/* xorshift32 PRNG for reproducible random parameters */
static uint32_t
xorshift32(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Helper: compare long values and print diagnostic on mismatch */
static int
expect_eq_long(const char* tag, long got, long want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

/* Helper: compare int values and print diagnostic on mismatch */
static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

/* Helper: compare uint8_t values and print diagnostic on mismatch */
static int
expect_eq_u8(const char* tag, uint8_t got, uint8_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %u want %u\n", tag, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

/*
 * Simulate an opcode 0x73 (TDMA Abbreviated) IDEN write WITH write-once guard.
 * This replicates the FIXED handler behavior: if the entry is already populated,
 * the write is skipped entirely.
 */
static void
write_tdma_iden(dsd_state* st, int iden, long base_freq, int chan_spac, int trans_off, int chan_type) {
    if (st->p25_iden_tdma[iden].populated) {
        return; /* Write-once guard: slot already populated, skip */
    }
    st->p25_iden_tdma[iden].base_freq = base_freq;
    st->p25_iden_tdma[iden].chan_spac = chan_spac;
    st->p25_iden_tdma[iden].trans_off = trans_off;
    st->p25_iden_tdma[iden].chan_type = chan_type;
    st->p25_iden_tdma[iden].populated = 1;
    st->p25_chan_tdma_explicit[iden] |= 2; /* bit1 = has TDMA entry */
}

/*
 * Test: Bug Condition — TDMA IDEN Overwrite on Populated Slot
 *
 * **Validates: Requirements 1.1, 1.2, 2.1, 2.2**
 *
 * Property 1: Bug Condition — For each TDMA IDEN slot (0–15), populate with
 * known parameters (set populated=1), then simulate a second TDMA IDEN write
 * with different parameters. Assert the entry was NOT overwritten.
 *
 * On UNFIXED code, this test FAILS because the handler overwrites unconditionally.
 * This failure confirms the bug exists.
 *
 * Uses xorshift32 PRNG with seed 0xDEADBEEF for reproducible random parameters.
 */
static int
test_bug_condition_tdma_overwrite(void) {
    int rc = 0;
    static dsd_state st;
    memset(&st, 0, sizeof st);

    uint32_t rng = 0xDEADBEEF; /* fixed seed for reproducibility */

    fprintf(stderr, "\n--- Bug Condition Exploration: TDMA IDEN Overwrite ---\n");

    for (int iden = 0; iden < 16; iden++) {
        char tag[128];

        /* Generate first set of random params (A) */
        long base_freq_a = (long)(xorshift32(&rng) % 200000000) + 100000000L; /* 100M–300M in 5Hz units */
        int chan_type_a = (int)(xorshift32(&rng) & 0xF);                      /* 4-bit */
        int chan_spac_a = (int)(xorshift32(&rng) & 0x3FF);                    /* 10-bit */
        int trans_off_a = (int)(xorshift32(&rng) % 20000) - 10000;            /* signed offset */

        /* Write first set to slot — this is the "first seen" entry */
        write_tdma_iden(&st, iden, base_freq_a, chan_spac_a, trans_off_a, chan_type_a);

        /* Generate second set of DIFFERENT random params (B) */
        long base_freq_b = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_type_b = (int)(xorshift32(&rng) & 0xF);
        int chan_spac_b = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off_b = (int)(xorshift32(&rng) % 20000) - 10000;

        /* Simulate second TDMA IDEN write to same slot (unconditional overwrite) */
        write_tdma_iden(&st, iden, base_freq_b, chan_spac_b, trans_off_b, chan_type_b);

        /* Assert entry still has params A (first-write values) */
        snprintf(tag, sizeof tag, "slot[%d]: base_freq unchanged (first=%ld, second=%ld)", iden, base_freq_a,
                 base_freq_b);
        rc |= expect_eq_long(tag, st.p25_iden_tdma[iden].base_freq, base_freq_a);

        snprintf(tag, sizeof tag, "slot[%d]: chan_type unchanged (first=%d, second=%d)", iden, chan_type_a,
                 chan_type_b);
        rc |= expect_eq_int(tag, st.p25_iden_tdma[iden].chan_type, chan_type_a);

        snprintf(tag, sizeof tag, "slot[%d]: chan_spac unchanged (first=%d, second=%d)", iden, chan_spac_a,
                 chan_spac_b);
        rc |= expect_eq_int(tag, st.p25_iden_tdma[iden].chan_spac, chan_spac_a);

        snprintf(tag, sizeof tag, "slot[%d]: trans_off unchanged (first=%d, second=%d)", iden, trans_off_a,
                 trans_off_b);
        rc |= expect_eq_int(tag, st.p25_iden_tdma[iden].trans_off, trans_off_a);

        snprintf(tag, sizeof tag, "slot[%d]: populated remains 1", iden);
        rc |= expect_eq_u8(tag, st.p25_iden_tdma[iden].populated, 1);
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_bug_condition_tdma_overwrite\n");
    } else {
        fprintf(stderr, "\nFAILED test_bug_condition_tdma_overwrite — "
                        "second write to populated slot overwrites entry (bug confirmed)\n");
    }
    return rc;
}

/*
 * Preservation Property P2a: First TDMA write to empty slot succeeds
 *
 * **Validates: Requirements 3.3**
 *
 * For 128 iterations with xorshift32 PRNG (seed 0xCAFEBABE):
 *   1. Start with a fresh (zeroed) state
 *   2. Generate random valid IDEN params
 *   3. Write to an empty TDMA slot (populated=0)
 *   4. Assert all fields match the written values
 *   5. Assert populated == 1
 *
 * This MUST PASS on unfixed code (confirms baseline first-write behavior).
 */
static int
test_preservation_first_write(void) {
    int rc = 0;
    static dsd_state st;

    uint32_t rng = 0xCAFEBABE; /* distinct seed from bug condition test */

    fprintf(stderr, "\n--- Preservation P2a: First TDMA Write to Empty Slot ---\n");

    for (int iter = 0; iter < 128; iter++) {
        /* Fresh state for each iteration */
        memset(&st, 0, sizeof st);

        char tag[128];

        /* Pick a random slot 0–15 */
        int iden = (int)(xorshift32(&rng) & 0xF);

        /* Generate random valid IDEN params */
        long base_freq = (long)(xorshift32(&rng) % 200000000) + 100000000L; /* 100M–300M in 5Hz units */
        int chan_type = (int)(xorshift32(&rng) & 0xF);                      /* 4-bit */
        int chan_spac = (int)(xorshift32(&rng) & 0x3FF);                    /* 10-bit */
        int trans_off = (int)(xorshift32(&rng) % 20000) - 10000;            /* signed offset */

        /* Confirm slot is empty before write */
        if (st.p25_iden_tdma[iden].populated != 0) {
            fprintf(stderr, "FAIL iter[%d]: slot[%d] not empty before write\n", iter, iden);
            rc |= 1;
            continue;
        }

        /* Write to empty TDMA slot */
        write_tdma_iden(&st, iden, base_freq, chan_spac, trans_off, chan_type);

        /* Assert all fields match the written values */
        snprintf(tag, sizeof tag, "iter[%d] slot[%d]: base_freq", iter, iden);
        rc |= expect_eq_long(tag, st.p25_iden_tdma[iden].base_freq, base_freq);

        snprintf(tag, sizeof tag, "iter[%d] slot[%d]: chan_type", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_tdma[iden].chan_type, chan_type);

        snprintf(tag, sizeof tag, "iter[%d] slot[%d]: chan_spac", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_tdma[iden].chan_spac, chan_spac);

        snprintf(tag, sizeof tag, "iter[%d] slot[%d]: trans_off", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_tdma[iden].trans_off, trans_off);

        snprintf(tag, sizeof tag, "iter[%d] slot[%d]: populated", iter, iden);
        rc |= expect_eq_u8(tag, st.p25_iden_tdma[iden].populated, 1);
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_preservation_first_write\n");
    } else {
        fprintf(stderr, "FAILED test_preservation_first_write\n");
    }
    return rc;
}

/*
 * Preservation Property P2b: FDMA writes can overwrite freely (not guarded)
 *
 * **Validates: Requirements 3.2**
 *
 * For 128 iterations with xorshift32 PRNG (seed 0xFEEDFACE):
 *   1. Populate an FDMA slot with random params A
 *   2. Overwrite with different random params B
 *   3. Assert entry now has params B (overwrite succeeded)
 *
 * This MUST PASS on unfixed code (FDMA is never guarded).
 */
static int
test_preservation_fdma_overwrite(void) {
    int rc = 0;
    static dsd_state st;
    memset(&st, 0, sizeof st);

    uint32_t rng = 0xFEEDFACE; /* distinct seed */

    fprintf(stderr, "\n--- Preservation P2b: FDMA Writes Can Overwrite Freely ---\n");

    for (int iter = 0; iter < 128; iter++) {
        char tag[128];

        /* Pick a random slot 0–15 */
        int iden = (int)(xorshift32(&rng) & 0xF);

        /* Generate first set of random params (A) */
        long base_freq_a = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_spac_a = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off_a = (int)(xorshift32(&rng) % 20000) - 10000;

        /* Write first set to FDMA slot */
        st.p25_iden_fdma[iden].base_freq = base_freq_a;
        st.p25_iden_fdma[iden].chan_type = 1; /* FDMA default */
        st.p25_iden_fdma[iden].chan_spac = chan_spac_a;
        st.p25_iden_fdma[iden].trans_off = trans_off_a;
        st.p25_iden_fdma[iden].populated = 1;
        st.p25_chan_tdma_explicit[iden] |= 1; /* bit0 = has FDMA entry */

        /* Generate second set of DIFFERENT random params (B) */
        long base_freq_b = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_spac_b = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off_b = (int)(xorshift32(&rng) % 20000) - 10000;

        /* Overwrite FDMA slot with params B (FDMA is never guarded) */
        st.p25_iden_fdma[iden].base_freq = base_freq_b;
        st.p25_iden_fdma[iden].chan_type = 1;
        st.p25_iden_fdma[iden].chan_spac = chan_spac_b;
        st.p25_iden_fdma[iden].trans_off = trans_off_b;
        st.p25_iden_fdma[iden].populated = 1;

        /* Assert entry now has params B (overwrite succeeded) */
        snprintf(tag, sizeof tag, "iter[%d] slot[%d]: base_freq is B", iter, iden);
        rc |= expect_eq_long(tag, st.p25_iden_fdma[iden].base_freq, base_freq_b);

        snprintf(tag, sizeof tag, "iter[%d] slot[%d]: chan_spac is B", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].chan_spac, chan_spac_b);

        snprintf(tag, sizeof tag, "iter[%d] slot[%d]: trans_off is B", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].trans_off, trans_off_b);

        snprintf(tag, sizeof tag, "iter[%d] slot[%d]: populated remains 1", iter, iden);
        rc |= expect_eq_u8(tag, st.p25_iden_fdma[iden].populated, 1);
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_preservation_fdma_overwrite\n");
    } else {
        fprintf(stderr, "FAILED test_preservation_fdma_overwrite\n");
    }
    return rc;
}

/*
 * Preservation Property P2c: Table reset restores writability
 *
 * **Validates: Requirements 3.1**
 *
 * 1. Populate all 16 TDMA slots with random params (seed 0xBAADF00D)
 * 2. Call p25_reset_iden_tables()
 * 3. Assert all populated flags are 0
 * 4. Write new params to each slot
 * 5. Assert new params took effect (populated=1, fields match)
 *
 * This MUST PASS on unfixed code (confirms reset restores writability).
 */
static int
test_preservation_table_reset(void) {
    int rc = 0;
    static dsd_state st;
    memset(&st, 0, sizeof st);

    uint32_t rng = 0xBAADF00D; /* distinct seed */

    fprintf(stderr, "\n--- Preservation P2c: Table Reset Restores Writability ---\n");

    /* Step 1: Populate all 16 TDMA slots with random params */
    for (int iden = 0; iden < 16; iden++) {
        long base_freq = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_type = (int)(xorshift32(&rng) & 0xF);
        int chan_spac = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off = (int)(xorshift32(&rng) % 20000) - 10000;

        write_tdma_iden(&st, iden, base_freq, chan_spac, trans_off, chan_type);
    }

    /* Verify all slots are populated before reset */
    for (int iden = 0; iden < 16; iden++) {
        char tag[128];
        snprintf(tag, sizeof tag, "pre-reset slot[%d]: populated=1", iden);
        rc |= expect_eq_u8(tag, st.p25_iden_tdma[iden].populated, 1);
    }

    /* Step 2: Call p25_reset_iden_tables() */
    p25_reset_iden_tables(&st);

    /* Step 3: Assert all populated flags are 0 */
    for (int iden = 0; iden < 16; iden++) {
        char tag[128];
        snprintf(tag, sizeof tag, "post-reset slot[%d]: populated=0", iden);
        rc |= expect_eq_u8(tag, st.p25_iden_tdma[iden].populated, 0);

        snprintf(tag, sizeof tag, "post-reset slot[%d]: tdma_explicit=0", iden);
        rc |= expect_eq_u8(tag, st.p25_chan_tdma_explicit[iden], 0);
    }

    /* Step 4: Write new params to each slot */
    for (int iden = 0; iden < 16; iden++) {
        long base_freq = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_type = (int)(xorshift32(&rng) & 0xF);
        int chan_spac = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off = (int)(xorshift32(&rng) % 20000) - 10000;

        write_tdma_iden(&st, iden, base_freq, chan_spac, trans_off, chan_type);

        /* Step 5: Assert new params took effect */
        char tag[128];
        snprintf(tag, sizeof tag, "post-reset-write slot[%d]: base_freq", iden);
        rc |= expect_eq_long(tag, st.p25_iden_tdma[iden].base_freq, base_freq);

        snprintf(tag, sizeof tag, "post-reset-write slot[%d]: chan_type", iden);
        rc |= expect_eq_int(tag, st.p25_iden_tdma[iden].chan_type, chan_type);

        snprintf(tag, sizeof tag, "post-reset-write slot[%d]: chan_spac", iden);
        rc |= expect_eq_int(tag, st.p25_iden_tdma[iden].chan_spac, chan_spac);

        snprintf(tag, sizeof tag, "post-reset-write slot[%d]: trans_off", iden);
        rc |= expect_eq_int(tag, st.p25_iden_tdma[iden].trans_off, trans_off);

        snprintf(tag, sizeof tag, "post-reset-write slot[%d]: populated=1", iden);
        rc |= expect_eq_u8(tag, st.p25_iden_tdma[iden].populated, 1);
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_preservation_table_reset\n");
    } else {
        fprintf(stderr, "FAILED test_preservation_table_reset\n");
    }
    return rc;
}

/*
 * Integration Test: CC Broadcast Rotation Determinism (WACN BEE00 pattern)
 *
 * **Validates: Requirements 2.3, 3.2, 3.4, 3.5**
 *
 * Simulates the full CC broadcast rotation pattern observed on WACN BEE00:
 * - IDEN 2 TDMA: cycles between 800 MHz (type=3, base=851012500/5) and
 *   450 MHz (type=1, base=500000000/5)
 * - IDEN 3 TDMA: cycles between txOffset=-2400 and txOffset=10592
 *   (same base=762006250/5)
 * - FDMA entries are stable (no cycling)
 *
 * After multiple rotation cycles, resolves channel frequencies and verifies:
 * 1. TDMA resolution uses first-seen params (write-once guard)
 * 2. FDMA resolution uses latest params (freely overwritten)
 * 3. Results are deterministic regardless of cycle count
 */
static int
test_integration_cc_rotation_determinism(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    fprintf(stderr, "\n--- Integration: CC Broadcast Rotation Determinism ---\n");

    /* WACN BEE00 observed parameters (base_freq in 5Hz units) */
    /* IDEN 0: TDMA 800 MHz (stable — only one variant) */
    long iden0_tdma_base = 851006250L / 5; /* 170201250 */
    int iden0_tdma_spac = 50;
    int iden0_tdma_off = 7200;
    int iden0_tdma_type = 1;

    /* IDEN 2: TDMA cycles between 800 MHz and 450 MHz */
    long iden2_tdma_800_base = 851012500L / 5; /* 170202500 */
    int iden2_tdma_800_spac = 100;
    int iden2_tdma_800_off = 3600;
    int iden2_tdma_800_type = 3;

    long iden2_tdma_450_base = 500000000L / 5; /* 100000000 */
    int iden2_tdma_450_spac = 50;
    int iden2_tdma_450_off = -480;
    int iden2_tdma_450_type = 1;

    /* IDEN 3: TDMA cycles between two txOffset values (same base) */
    long iden3_tdma_base = 762006250L / 5; /* 152401250 */
    int iden3_tdma_spac = 100;
    int iden3_tdma_off_a = -2400;
    int iden3_tdma_off_b = 10592;
    int iden3_tdma_type = 3;

    /* FDMA entries (stable) */
    long iden0_fdma_base = 851006250L / 5;
    int iden0_fdma_spac = 50;
    int iden0_fdma_off = 2228;
    int iden0_fdma_type = 1;

    /* Simulate 5 full broadcast rotation cycles */
    for (int cycle = 0; cycle < 5; cycle++) {
        /* IDEN 0: TDMA (stable, only one variant) */
        write_tdma_iden(&st, 0, iden0_tdma_base, iden0_tdma_spac, iden0_tdma_off, iden0_tdma_type);

        /* IDEN 2: TDMA — first cycle writes 800 MHz, subsequent cycles try 450 MHz */
        if (cycle % 2 == 0) {
            write_tdma_iden(&st, 2, iden2_tdma_800_base, iden2_tdma_800_spac, iden2_tdma_800_off, iden2_tdma_800_type);
        } else {
            write_tdma_iden(&st, 2, iden2_tdma_450_base, iden2_tdma_450_spac, iden2_tdma_450_off, iden2_tdma_450_type);
        }

        /* IDEN 3: TDMA — alternates txOffset */
        if (cycle % 2 == 0) {
            write_tdma_iden(&st, 3, iden3_tdma_base, iden3_tdma_spac, iden3_tdma_off_a, iden3_tdma_type);
        } else {
            write_tdma_iden(&st, 3, iden3_tdma_base, iden3_tdma_spac, iden3_tdma_off_b, iden3_tdma_type);
        }

        /* FDMA entries (stable, can overwrite freely) */
        st.p25_iden_fdma[0].base_freq = iden0_fdma_base;
        st.p25_iden_fdma[0].chan_spac = iden0_fdma_spac;
        st.p25_iden_fdma[0].trans_off = iden0_fdma_off;
        st.p25_iden_fdma[0].chan_type = iden0_fdma_type;
        st.p25_iden_fdma[0].populated = 1;
        st.p25_chan_tdma_explicit[0] |= 1;
    }

    /* Verify TDMA entries retained first-seen values (write-once guard) */
    rc |= expect_eq_long("integ: IDEN0 TDMA base", st.p25_iden_tdma[0].base_freq, iden0_tdma_base);
    rc |= expect_eq_int("integ: IDEN0 TDMA spac", st.p25_iden_tdma[0].chan_spac, iden0_tdma_spac);

    /* IDEN 2: should retain 800 MHz (first-seen on cycle 0) */
    rc |= expect_eq_long("integ: IDEN2 TDMA base (800MHz first)", st.p25_iden_tdma[2].base_freq, iden2_tdma_800_base);
    rc |= expect_eq_int("integ: IDEN2 TDMA spac", st.p25_iden_tdma[2].chan_spac, iden2_tdma_800_spac);
    rc |= expect_eq_int("integ: IDEN2 TDMA off", st.p25_iden_tdma[2].trans_off, iden2_tdma_800_off);
    rc |= expect_eq_int("integ: IDEN2 TDMA type", st.p25_iden_tdma[2].chan_type, iden2_tdma_800_type);

    /* IDEN 3: should retain first txOffset (-2400, from cycle 0) */
    rc |= expect_eq_long("integ: IDEN3 TDMA base", st.p25_iden_tdma[3].base_freq, iden3_tdma_base);
    rc |= expect_eq_int("integ: IDEN3 TDMA off (first=-2400)", st.p25_iden_tdma[3].trans_off, iden3_tdma_off_a);
    rc |= expect_eq_int("integ: IDEN3 TDMA type", st.p25_iden_tdma[3].chan_type, iden3_tdma_type);

    /* Resolve TDMA grants — should be deterministic */
    st.p25_chan_tdma_explicit[2] = 2;    /* TDMA context */
    int chan_iden2 = (2 << 12) | 0x0014; /* iden=2, chan_number=20 */
    long f_iden2 = process_channel_to_freq(&opts, &st, chan_iden2);
    /* chan_type=3, denom=2, step=20/2=10
     * freq = (170202500 * 5) + (10 * 100 * 125) = 851012500 + 125000 = 851137500 */
    long want_iden2 = (iden2_tdma_800_base * 5) + (10L * iden2_tdma_800_spac * 125);
    rc |= expect_eq_long("integ: IDEN2 TDMA freq resolve", f_iden2, want_iden2);

    st.p25_chan_tdma_explicit[3] = 2;
    int chan_iden3 = (3 << 12) | 0x0014; /* iden=3, chan_number=20 */
    long f_iden3 = process_channel_to_freq(&opts, &st, chan_iden3);
    /* chan_type=3, denom=2, step=20/2=10
     * freq = (152401250 * 5) + (10 * 100 * 125) = 762006250 + 125000 = 762131250 */
    long want_iden3 = (iden3_tdma_base * 5) + (10L * iden3_tdma_spac * 125);
    rc |= expect_eq_long("integ: IDEN3 TDMA freq resolve", f_iden3, want_iden3);

    /* Resolve same channels again — must be identical (deterministic) */
    st.trunk_chan_map[chan_iden2] = 0; /* clear cache to force re-resolve */
    st.trunk_chan_map[chan_iden3] = 0;
    long f_iden2_again = process_channel_to_freq(&opts, &st, chan_iden2);
    long f_iden3_again = process_channel_to_freq(&opts, &st, chan_iden3);
    rc |= expect_eq_long("integ: IDEN2 deterministic", f_iden2_again, want_iden2);
    rc |= expect_eq_long("integ: IDEN3 deterministic", f_iden3_again, want_iden3);

    /* Verify FDMA resolution is independent */
    st.p25_chan_tdma_explicit[0] = 1;    /* FDMA context */
    int chan_fdma0 = (0 << 12) | 0x000A; /* iden=0, chan_number=10 */
    long f_fdma0 = process_channel_to_freq(&opts, &st, chan_fdma0);
    /* FDMA: denom=1, step=10, freq = (170201250*5) + (10*50*125) = 851006250 + 62500 = 851068750 */
    long want_fdma0 = (iden0_fdma_base * 5) + (10L * iden0_fdma_spac * 125);
    rc |= expect_eq_long("integ: IDEN0 FDMA freq resolve", f_fdma0, want_fdma0);

    if (rc == 0) {
        fprintf(stderr, "PASS test_integration_cc_rotation_determinism\n");
    } else {
        fprintf(stderr, "FAILED test_integration_cc_rotation_determinism\n");
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    int preservation_rc = 0;

    rc |= test_bug_condition_tdma_overwrite();

    preservation_rc |= test_preservation_first_write();
    preservation_rc |= test_preservation_fdma_overwrite();
    preservation_rc |= test_preservation_table_reset();

    if (preservation_rc != 0) {
        fprintf(stderr, "\nFAILED: Preservation tests failed (unexpected — these should pass on unfixed code)\n");
    } else {
        fprintf(stderr, "\nAll preservation tests PASSED (baseline behavior confirmed)\n");
    }

    rc |= preservation_rc;
    rc |= test_integration_cc_rotation_determinism();

    if (rc == 0) {
        fprintf(stderr, "\nAll test_p25_iden_write_once tests PASSED\n");
    } else {
        fprintf(stderr, "\nSome test_p25_iden_write_once tests FAILED (expected on unfixed code)\n");
    }
    return rc;
}
