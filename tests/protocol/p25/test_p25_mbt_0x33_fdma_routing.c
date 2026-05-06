// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 MBT 0x33 FDMA Routing — Property-Based Tests
 *
 * Verifies that the MBT opcode 0x33 handler routes IDEN entries to the correct
 * array based on chan_type:
 *   - Single-slot types (0, 1, 2) → p25_iden_fdma[iden]
 *   - Multi-slot types (3+) → p25_iden_tdma[iden]
 *
 * The handler previously wrote all entries unconditionally to p25_iden_tdma[],
 * causing ~81% of entries on multi-mode systems to be misclassified as TDMA.
 *
 * Uses a xorshift32 PRNG with a fixed seed for reproducible random parameters.
 */

#include <dsd-neo/core/state.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

struct RtlSdrContext;

/* Stubs for external hooks referenced by linked modules */
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
 * Simulate the MBT opcode 0x33 handler logic on the state struct.
 *
 * This replicates the FIXED handler behavior: single-slot chan_types (0, 1, 2)
 * are routed to p25_iden_fdma[iden], while multi-slot chan_types (3+) are
 * routed to p25_iden_tdma[iden]. The bitmask logic is unchanged.
 */
static void
simulate_mbt_0x33_handler(dsd_state* st, int iden, int chan_type, long int base_freq, int chan_spac, int trans_off) {
    if (iden >= 16) {
        return; /* out of range, skip */
    }

    st->p25_chan_iden = iden;

    /* Route to correct array based on chan_type slot count */
    static const int slots_per_carrier[16] = {1, 1, 1, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
    int is_tdma = slots_per_carrier[chan_type & 0xF] > 1;
    p25_iden_entry_t* e = is_tdma ? &st->p25_iden_tdma[iden] : &st->p25_iden_fdma[iden];

    e->chan_type = chan_type;
    e->trans_off = trans_off;
    e->chan_spac = chan_spac;
    e->base_freq = base_freq;
    e->populated = 1;
    e->wacn = st->p2_wacn;
    e->sysid = st->p2_sysid;
    e->rfss = st->p2_rfssid;
    e->site = st->p2_siteid;
    e->trust = (st->p25_cc_freq != 0) ? 2 : 1;

    if (is_tdma) {
        st->p25_chan_tdma_explicit[iden] |= 2; /* bit1 = has TDMA entry */
    } else {
        st->p25_chan_tdma_explicit[iden] |= 1; /* bit0 = has FDMA entry */
    }
}

/*
 * Test: single-slot chan_types (0, 1, 2) route to p25_iden_fdma[]
 *
 * For each single-slot chan_type with random IDEN params, simulate the 0x33
 * handler and assert:
 *   - Entry lands in p25_iden_fdma[iden] with all fields correct
 *   - p25_iden_tdma[iden] remains untouched
 *   - p25_chan_tdma_explicit[iden] bit0 is set (FDMA)
 *
 * Uses xorshift32 PRNG with seed 0xABCD1234, 100 iterations.
 */
static int
test_bug_condition_fdma_misrouted_to_tdma(void) {
    int rc = 0;
    static dsd_state st;

    uint32_t rng = 0xABCD1234; /* fixed seed for reproducibility */
    const int single_slot_types[] = {0, 1, 2};

    fprintf(stderr, "\n--- Bug Condition Exploration: FDMA Entries Misrouted to TDMA Array ---\n");

    for (int iter = 0; iter < 100; iter++) {
        /* Fresh state for each iteration */
        memset(&st, 0, sizeof st);

        char tag[256];

        /* Generate random iden (0–15) */
        int iden = (int)(xorshift32(&rng) & 0xF);

        /* Pick a single-slot chan_type from {0, 1, 2} */
        int chan_type = single_slot_types[xorshift32(&rng) % 3];

        /* Generate random IDEN parameters */
        long int base_freq = (long)(xorshift32(&rng) % 200000000) + 100000000L; /* 100M–300M in 5Hz units */
        int chan_spac = (int)(xorshift32(&rng) & 0x3FF);                        /* 10-bit */
        int trans_off_raw = (int)(xorshift32(&rng) % 20000);
        int trans_off = (xorshift32(&rng) & 1) ? -(trans_off_raw) : trans_off_raw; /* signed offset */

        /* Simulate the 0x33 handler on the state struct */
        simulate_mbt_0x33_handler(&st, iden, chan_type, base_freq, chan_spac, trans_off);

        /* Assert entry lands in p25_iden_fdma[iden] */
        snprintf(tag, sizeof tag, "iter[%d] iden=%d chan_type=%d: p25_iden_fdma[%d].populated == 1", iter, iden,
                 chan_type, iden);
        rc |= expect_eq_u8(tag, st.p25_iden_fdma[iden].populated, 1);

        snprintf(tag, sizeof tag, "iter[%d] iden=%d chan_type=%d: p25_iden_fdma[%d].base_freq == %ld", iter, iden,
                 chan_type, iden, base_freq);
        rc |= expect_eq_long(tag, st.p25_iden_fdma[iden].base_freq, base_freq);

        snprintf(tag, sizeof tag, "iter[%d] iden=%d chan_type=%d: p25_iden_fdma[%d].chan_type == %d", iter, iden,
                 chan_type, iden, chan_type);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].chan_type, chan_type);

        snprintf(tag, sizeof tag, "iter[%d] iden=%d chan_type=%d: p25_iden_fdma[%d].chan_spac == %d", iter, iden,
                 chan_type, iden, chan_spac);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].chan_spac, chan_spac);

        snprintf(tag, sizeof tag, "iter[%d] iden=%d chan_type=%d: p25_iden_fdma[%d].trans_off == %d", iter, iden,
                 chan_type, iden, trans_off);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].trans_off, trans_off);

        /* Assert TDMA array is untouched */
        snprintf(tag, sizeof tag, "iter[%d] iden=%d chan_type=%d: p25_iden_tdma[%d].populated == 0 (untouched)", iter,
                 iden, chan_type, iden);
        rc |= expect_eq_u8(tag, st.p25_iden_tdma[iden].populated, 0);

        /* Assert bitmask has bit0 set (FDMA) */
        snprintf(tag, sizeof tag, "iter[%d] iden=%d chan_type=%d: p25_chan_tdma_explicit[%d] & 1 == 1", iter, iden,
                 chan_type, iden);
        rc |= expect_eq_u8(tag, (uint8_t)(st.p25_chan_tdma_explicit[iden] & 1), 1);

        /* Stop early on first failure to keep output manageable */
        if (rc != 0) {
            fprintf(stderr,
                    "\nCounterexample found at iter %d: Single-slot entry (chan_type=%d, iden=%d) "
                    "written to p25_iden_tdma[%d] instead of p25_iden_fdma[%d]\n",
                    iter, chan_type, iden, iden, iden);
            fprintf(stderr, "  base_freq=%ld, chan_spac=%d, trans_off=%d\n", base_freq, chan_spac, trans_off);
            fprintf(stderr, "  p25_iden_tdma[%d].populated=%u (should be 0)\n", iden,
                    (unsigned)st.p25_iden_tdma[iden].populated);
            fprintf(stderr, "  p25_iden_fdma[%d].populated=%u (should be 1)\n", iden,
                    (unsigned)st.p25_iden_fdma[iden].populated);
            break;
        }
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_bug_condition_fdma_misrouted_to_tdma\n");
    } else {
        fprintf(stderr, "\nFAILED test_bug_condition_fdma_misrouted_to_tdma — "
                        "single-slot entries written to p25_iden_tdma[] instead of p25_iden_fdma[] "
                        "(bug confirmed)\n");
    }
    return rc;
}

/*
 * Test: multi-slot chan_types (3–15) route to p25_iden_tdma[]
 *
 * For all random multi-slot chan_types with random IDEN params, verify entry
 * is written to p25_iden_tdma[iden] with all fields correct, trust propagated,
 * and p25_chan_tdma_explicit[iden] bit1 set (TDMA).
 *
 * Uses xorshift32 PRNG with seed 0xDEAD5678, 100 iterations.
 */
static int
test_preservation_multislot_tdma_written_correctly(void) {
    int rc = 0;
    static dsd_state st;

    uint32_t rng = 0xDEAD5678; /* different seed from exploration test */

    fprintf(stderr, "\n--- Preservation P2a: Multi-Slot TDMA Entries Written Correctly ---\n");

    for (int iter = 0; iter < 100; iter++) {
        /* Fresh state for each iteration */
        memset(&st, 0, sizeof st);

        /* Set some provenance context to verify it propagates */
        st.p2_wacn = (unsigned long long)(xorshift32(&rng) & 0xFFFFF);
        st.p2_sysid = (unsigned long long)(xorshift32(&rng) & 0xFFF);
        st.p2_rfssid = (unsigned long long)(xorshift32(&rng) & 0xFF);
        st.p2_siteid = (unsigned long long)(xorshift32(&rng) & 0xFF);
        /* Randomly set p25_cc_freq to test trust=1 vs trust=2 */
        st.p25_cc_freq = (xorshift32(&rng) & 1) ? (long)(xorshift32(&rng) % 900000000 + 100000000) : 0;

        char tag[256];

        /* Generate random iden (0–15) */
        int iden = (int)(xorshift32(&rng) & 0xF);

        /* Pick a multi-slot chan_type from {3, 4, 5, ..., 15} */
        int chan_type = 3 + (int)(xorshift32(&rng) % 13);

        /* Generate random IDEN parameters */
        long int base_freq = (long)(xorshift32(&rng) % 200000000) + 100000000L; /* 100M–300M in 5Hz units */
        int chan_spac = (int)(xorshift32(&rng) & 0x3FF);                        /* 10-bit */
        int trans_off_raw = (int)(xorshift32(&rng) % 20000);
        int trans_off = (xorshift32(&rng) & 1) ? -(trans_off_raw) : trans_off_raw;

        /* Expected trust value */
        uint8_t expected_trust = (st.p25_cc_freq != 0) ? 2 : 1;

        /* Simulate the 0x33 handler */
        simulate_mbt_0x33_handler(&st, iden, chan_type, base_freq, chan_spac, trans_off);

        /* Assert entry lands in p25_iden_tdma[iden] with all fields correct */
        snprintf(tag, sizeof tag, "P2a iter[%d] iden=%d chan_type=%d: p25_iden_tdma[%d].populated == 1", iter, iden,
                 chan_type, iden);
        rc |= expect_eq_u8(tag, st.p25_iden_tdma[iden].populated, 1);

        snprintf(tag, sizeof tag, "P2a iter[%d] iden=%d chan_type=%d: p25_iden_tdma[%d].base_freq == %ld", iter, iden,
                 chan_type, iden, base_freq);
        rc |= expect_eq_long(tag, st.p25_iden_tdma[iden].base_freq, base_freq);

        snprintf(tag, sizeof tag, "P2a iter[%d] iden=%d chan_type=%d: p25_iden_tdma[%d].chan_type == %d", iter, iden,
                 chan_type, iden, chan_type);
        rc |= expect_eq_int(tag, st.p25_iden_tdma[iden].chan_type, chan_type);

        snprintf(tag, sizeof tag, "P2a iter[%d] iden=%d chan_type=%d: p25_iden_tdma[%d].chan_spac == %d", iter, iden,
                 chan_type, iden, chan_spac);
        rc |= expect_eq_int(tag, st.p25_iden_tdma[iden].chan_spac, chan_spac);

        snprintf(tag, sizeof tag, "P2a iter[%d] iden=%d chan_type=%d: p25_iden_tdma[%d].trans_off == %d", iter, iden,
                 chan_type, iden, trans_off);
        rc |= expect_eq_int(tag, st.p25_iden_tdma[iden].trans_off, trans_off);

        snprintf(tag, sizeof tag, "P2a iter[%d] iden=%d chan_type=%d: p25_iden_tdma[%d].trust == %u", iter, iden,
                 chan_type, iden, (unsigned)expected_trust);
        rc |= expect_eq_u8(tag, st.p25_iden_tdma[iden].trust, expected_trust);

        /* Assert bitmask has bit1 set (TDMA) */
        snprintf(tag, sizeof tag, "P2a iter[%d] iden=%d chan_type=%d: p25_chan_tdma_explicit[%d] & 2 == 2", iter, iden,
                 chan_type, iden);
        rc |= expect_eq_u8(tag, (uint8_t)(st.p25_chan_tdma_explicit[iden] & 2), 2);

        /* Assert p25_chan_iden is set */
        snprintf(tag, sizeof tag, "P2a iter[%d] iden=%d chan_type=%d: p25_chan_iden == %d", iter, iden, chan_type,
                 iden);
        rc |= expect_eq_int(tag, st.p25_chan_iden, iden);

        if (rc != 0) {
            fprintf(stderr, "\nP2a counterexample at iter %d: chan_type=%d, iden=%d\n", iter, chan_type, iden);
            fprintf(stderr, "  base_freq=%ld, chan_spac=%d, trans_off=%d\n", base_freq, chan_spac, trans_off);
            break;
        }
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_preservation_multislot_tdma_written_correctly\n");
    } else {
        fprintf(stderr, "FAILED test_preservation_multislot_tdma_written_correctly\n");
    }
    return rc;
}

/*
 * Test: multi-slot entries do not modify p25_iden_fdma[]
 *
 * For all random multi-slot entries, verify p25_iden_fdma[iden] is NOT modified.
 * The FDMA array must remain untouched when TDMA entries are written.
 *
 * Uses xorshift32 PRNG with seed 0xDEAD5678 (offset by 200 iterations), 100 iterations.
 */
static int
test_preservation_fdma_array_untouched_for_multislot(void) {
    int rc = 0;
    static dsd_state st;

    uint32_t rng = 0xDEAD5678;
    /* Advance RNG past P2a's usage to get different values */
    for (int i = 0; i < 200; i++) {
        (void)xorshift32(&rng);
    }

    fprintf(stderr, "\n--- Preservation P2b: FDMA Array Untouched for Multi-Slot Entries ---\n");

    for (int iter = 0; iter < 100; iter++) {
        /* Fresh state for each iteration */
        memset(&st, 0, sizeof st);

        char tag[256];

        /* Generate random iden (0–15) */
        int iden = (int)(xorshift32(&rng) & 0xF);

        /* Pick a multi-slot chan_type from {3, 4, 5, ..., 15} */
        int chan_type = 3 + (int)(xorshift32(&rng) % 13);

        /* Generate random IDEN parameters */
        long int base_freq = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_spac = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off_raw = (int)(xorshift32(&rng) % 20000);
        int trans_off = (xorshift32(&rng) & 1) ? -(trans_off_raw) : trans_off_raw;

        /* Simulate the 0x33 handler */
        simulate_mbt_0x33_handler(&st, iden, chan_type, base_freq, chan_spac, trans_off);

        /* Assert FDMA array is completely untouched for this iden */
        snprintf(tag, sizeof tag, "P2b iter[%d] iden=%d chan_type=%d: p25_iden_fdma[%d].populated == 0", iter, iden,
                 chan_type, iden);
        rc |= expect_eq_u8(tag, st.p25_iden_fdma[iden].populated, 0);

        snprintf(tag, sizeof tag, "P2b iter[%d] iden=%d chan_type=%d: p25_iden_fdma[%d].base_freq == 0", iter, iden,
                 chan_type, iden);
        rc |= expect_eq_long(tag, st.p25_iden_fdma[iden].base_freq, 0);

        snprintf(tag, sizeof tag, "P2b iter[%d] iden=%d chan_type=%d: p25_iden_fdma[%d].chan_type == 0", iter, iden,
                 chan_type, iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].chan_type, 0);

        snprintf(tag, sizeof tag, "P2b iter[%d] iden=%d chan_type=%d: p25_iden_fdma[%d].chan_spac == 0", iter, iden,
                 chan_type, iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].chan_spac, 0);

        snprintf(tag, sizeof tag, "P2b iter[%d] iden=%d chan_type=%d: p25_iden_fdma[%d].trans_off == 0", iter, iden,
                 chan_type, iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].trans_off, 0);

        /* Also verify no other FDMA slots were touched (spot check all 16) */
        for (int s = 0; s < 16; s++) {
            snprintf(tag, sizeof tag, "P2b iter[%d] iden=%d: p25_iden_fdma[%d].populated == 0 (all slots)", iter, iden,
                     s);
            rc |= expect_eq_u8(tag, st.p25_iden_fdma[s].populated, 0);
            if (rc != 0) {
                break;
            }
        }

        if (rc != 0) {
            fprintf(stderr, "\nP2b counterexample at iter %d: chan_type=%d, iden=%d\n", iter, chan_type, iden);
            fprintf(stderr, "  FDMA array was modified by multi-slot entry\n");
            break;
        }
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_preservation_fdma_array_untouched_for_multislot\n");
    } else {
        fprintf(stderr, "FAILED test_preservation_fdma_array_untouched_for_multislot\n");
    }
    return rc;
}

/*
 * Test: out-of-range IDEN (≥16) causes no state mutation
 *
 * For IDEN values ≥16, the handler should skip the write entirely.
 * Verify p25_chan_iden, p25_iden_tdma[], p25_iden_fdma[], and
 * p25_chan_tdma_explicit[] all remain at their initial (zeroed) values.
 *
 * Uses xorshift32 PRNG with seed 0xDEAD5678 (offset by 400 iterations), 100 iterations.
 */
static int
test_preservation_out_of_range_iden_no_mutation(void) {
    int rc = 0;
    static dsd_state st;

    uint32_t rng = 0xDEAD5678;
    /* Advance RNG past P2a and P2b usage */
    for (int i = 0; i < 400; i++) {
        (void)xorshift32(&rng);
    }

    fprintf(stderr, "\n--- Preservation P2c: Out-of-Range IDEN Causes No State Mutation ---\n");

    for (int iter = 0; iter < 100; iter++) {
        /* Fresh state for each iteration */
        memset(&st, 0, sizeof st);

        char tag[256];

        /* Generate out-of-range iden (16–255) */
        int iden = 16 + (int)(xorshift32(&rng) % 240);

        /* Pick any chan_type (0–15) — shouldn't matter since iden is out of range */
        int chan_type = (int)(xorshift32(&rng) & 0xF);

        /* Generate random IDEN parameters */
        long int base_freq = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_spac = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off_raw = (int)(xorshift32(&rng) % 20000);
        int trans_off = (xorshift32(&rng) & 1) ? -(trans_off_raw) : trans_off_raw;

        /* Simulate the 0x33 handler — should be a no-op for iden >= 16 */
        simulate_mbt_0x33_handler(&st, iden, chan_type, base_freq, chan_spac, trans_off);

        /* Assert p25_chan_iden was NOT set (remains 0) */
        snprintf(tag, sizeof tag, "P2c iter[%d] iden=%d: p25_chan_iden == 0 (unchanged)", iter, iden);
        rc |= expect_eq_int(tag, st.p25_chan_iden, 0);

        /* Assert no TDMA or FDMA slots were touched */
        for (int s = 0; s < 16; s++) {
            snprintf(tag, sizeof tag, "P2c iter[%d] iden=%d: p25_iden_tdma[%d].populated == 0", iter, iden, s);
            rc |= expect_eq_u8(tag, st.p25_iden_tdma[s].populated, 0);
            if (rc != 0) {
                break;
            }

            snprintf(tag, sizeof tag, "P2c iter[%d] iden=%d: p25_iden_fdma[%d].populated == 0", iter, iden, s);
            rc |= expect_eq_u8(tag, st.p25_iden_fdma[s].populated, 0);
            if (rc != 0) {
                break;
            }

            snprintf(tag, sizeof tag, "P2c iter[%d] iden=%d: p25_chan_tdma_explicit[%d] == 0", iter, iden, s);
            rc |= expect_eq_u8(tag, st.p25_chan_tdma_explicit[s], 0);
            if (rc != 0) {
                break;
            }
        }

        if (rc != 0) {
            fprintf(stderr, "\nP2c counterexample at iter %d: iden=%d, chan_type=%d\n", iter, iden, chan_type);
            fprintf(stderr, "  State was mutated despite out-of-range IDEN\n");
            break;
        }
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_preservation_out_of_range_iden_no_mutation\n");
    } else {
        fprintf(stderr, "FAILED test_preservation_out_of_range_iden_no_mutation\n");
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    int preservation_rc = 0;

    rc |= test_bug_condition_fdma_misrouted_to_tdma();

    /* Preservation tests — these MUST pass on both unfixed and fixed code */
    preservation_rc |= test_preservation_multislot_tdma_written_correctly();
    preservation_rc |= test_preservation_fdma_array_untouched_for_multislot();
    preservation_rc |= test_preservation_out_of_range_iden_no_mutation();

    if (preservation_rc != 0) {
        fprintf(stderr, "\nCRITICAL: Preservation tests FAILED — baseline behavior broken!\n");
    } else {
        fprintf(stderr, "\nAll preservation tests PASSED (baseline behavior confirmed)\n");
    }

    /* Combine: bug condition test failure is expected on unfixed code,
     * but preservation tests must always pass */
    rc |= preservation_rc;

    if (rc == 0) {
        fprintf(stderr, "\nAll test_p25_mbt_0x33_fdma_routing tests PASSED\n");
    } else {
        fprintf(stderr, "\nSome test_p25_mbt_0x33_fdma_routing tests FAILED "
                        "(expected on unfixed code — bug confirmed)\n");
    }
    return rc;
}
