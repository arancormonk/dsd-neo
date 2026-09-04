// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Property-Based Test: Preservation Checking (Single-Modulation Scenarios)
 *
 * **Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5**
 *
 * For all inputs where the bug condition does NOT hold (single-modulation
 * per slot), the fixed system produces the expected frequency result.
 *
 * Strategy: Generate random IDEN parameters within valid ranges, populate
 * only one modulation class per slot, and verify the frequency formula
 * produces the correct result for random channel numbers.
 *
 * Valid ranges:
 * - base_freq: 1 to 200000000 (in 5Hz units; covers 5Hz to 1GHz)
 * - chan_spac: 1 to 1023 (10-bit field)
 * - chan_type: 0 to 15 (4-bit field)
 * - iden: 0 to 15
 * - chan_number: 0 to 4095 (12-bit field)
 *
 * Properties verified:
 * P1: For FDMA-only slots (explicit=1), freq = (base_freq * 5) + (chan_number * chan_spac * 125)
 * P2: For TDMA-only slots (explicit=2), freq = (base_freq * 5) + ((chan_number / denom) * chan_spac * 125)
 *     where denom = slots_per_carrier[chan_type]
 * P3: trunk_chan_map cache always takes precedence (if map[chan16] != 0, return map[chan16])
 * P4: Empty slots (populated=0) always return 0
 *
 * Uses a simple xorshift32 PRNG for reproducibility with a fixed seed.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/* --------------------------------------------------------------------------
 * xorshift32 PRNG — simple, fast, deterministic for reproducibility.
 * Period: 2^32 - 1. Seed must be non-zero.
 * -------------------------------------------------------------------------- */
static uint32_t prng_state = 0;

static void
prng_seed(uint32_t seed) {
    prng_state = seed ? seed : 1u; /* Ensure non-zero */
}

static uint32_t
prng_next(void) {
    uint32_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    prng_state = x;
    return x;
}

/* Generate a random value in [lo, hi] inclusive */
static uint32_t
prng_range(uint32_t lo, uint32_t hi) {
    if (lo >= hi) {
        return lo;
    }
    uint32_t range = hi - lo + 1;
    return lo + (prng_next() % range);
}

/* Number of iterations per property */
#define PBT_ITERATIONS 10000

/* Fixed seed for reproducibility */
#define PBT_SEED       0xDEADBEEF

/* --------------------------------------------------------------------------
 * Property P1: FDMA-only frequency formula correctness
 *
 * For FDMA-only slots (explicit=1, denom=1):
 *   freq = (base_freq * 5) + (chan_number * chan_spac * 125)
 * -------------------------------------------------------------------------- */
static int
test_property_p1_fdma_formula(void) {
    int failures = 0;
    prng_seed(PBT_SEED);

    for (int iter = 0; iter < PBT_ITERATIONS; iter++) {
        static dsd_opts opts;
        static dsd_state st;
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);

        /* Generate random parameters within valid ranges */
        uint32_t iden = prng_range(0, 15);
        uint32_t base_freq = prng_range(1, 200000000);
        uint32_t chan_spac = prng_range(1, 1023);
        uint32_t chan_type = prng_range(0, 15); /* Ignored for FDMA denom, but stored */
        uint32_t chan_number = prng_range(0, 4095);

        /* Skip the sentinel value 0xFFFF (iden=15, chan_number=4095) which
         * returns 0 unconditionally as a protocol-level "no channel" marker */
        uint16_t chan16 = (uint16_t)((iden << 12) | (chan_number & 0xFFF));
        if (chan16 == 0xFFFF) {
            continue;
        }

        /* Populate FDMA-only entry */
        st.p25_iden_fdma[iden].base_freq = (long int)base_freq;
        st.p25_iden_fdma[iden].chan_spac = (int)chan_spac;
        st.p25_iden_fdma[iden].chan_type = (int)chan_type;
        st.p25_iden_fdma[iden].trans_off = 0;
        st.p25_iden_fdma[iden].populated = 1;
        st.p25_chan_tdma_explicit[iden] = 1; /* FDMA only (bit0 set) */

        /* Construct 16-bit channel: iden(4) | chan_number(12) */
        int channel = (int)chan16;

        /* Compute expected frequency:
         * FDMA: denom=1, step = chan_number / 1 = chan_number
         * freq = (base_freq * 5) + (step * chan_spac * 125) */
        long expected = ((long)base_freq * 5L) + ((long)chan_number * (long)chan_spac * 125L);

        /* Call the function under test */
        long actual = process_channel_to_freq(&opts, &st, channel);

        if (actual != expected) {
            if (failures < 5) { /* Limit diagnostic output */
                DSD_FPRINTF(stderr,
                            "P1 FAIL iter=%d: iden=%u base=%u spac=%u type=%u chan=%u "
                            "expected=%ld got=%ld\n",
                            iter, iden, base_freq, chan_spac, chan_type, chan_number, expected, actual);
            }
            failures++;
        }
    }

    if (failures == 0) {
        DSD_FPRINTF(stderr, "PASS P1: FDMA formula correctness (%d iterations)\n", PBT_ITERATIONS);
    } else {
        DSD_FPRINTF(stderr, "FAIL P1: %d/%d iterations failed\n", failures, PBT_ITERATIONS);
    }
    return failures;
}

/* --------------------------------------------------------------------------
 * Property P2: TDMA-only frequency formula correctness
 *
 * For TDMA-only slots (explicit=2):
 *   denom = slots_per_carrier[chan_type]
 *   step = chan_number / denom  (integer division)
 *   freq = (base_freq * 5) + (step * chan_spac * 125)
 * -------------------------------------------------------------------------- */
static int
test_property_p2_tdma_formula(void) {
    int failures = 0;
    prng_seed(PBT_SEED + 1); /* Different seed for variety */

    for (int iter = 0; iter < PBT_ITERATIONS; iter++) {
        static dsd_opts opts;
        static dsd_state st;
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);

        /* Generate random parameters within valid ranges */
        uint32_t iden = prng_range(0, 15);
        uint32_t base_freq = prng_range(1, 200000000);
        uint32_t chan_spac = prng_range(1, 1023);
        uint32_t chan_type = prng_range(0, 15);
        uint32_t chan_number = prng_range(0, 4095);

        /* Skip the sentinel value 0xFFFF (iden=15, chan_number=4095) which
         * returns 0 unconditionally as a protocol-level "no channel" marker */
        uint16_t chan16 = (uint16_t)((iden << 12) | (chan_number & 0xFFF));
        if (chan16 == 0xFFFF) {
            continue;
        }

        /* Populate TDMA-only entry */
        st.p25_iden_tdma[iden].base_freq = (long int)base_freq;
        st.p25_iden_tdma[iden].chan_spac = (int)chan_spac;
        st.p25_iden_tdma[iden].chan_type = (int)chan_type;
        st.p25_iden_tdma[iden].trans_off = 0;
        st.p25_iden_tdma[iden].populated = 1;
        st.p25_chan_tdma_explicit[iden] = 2; /* TDMA only (bit1 set) */

        /* Construct 16-bit channel: iden(4) | chan_number(12) */
        int channel = (int)chan16;

        /* Compute expected frequency:
         * TDMA: denom = slots_per_carrier[chan_type]
         * step = chan_number / denom (integer division)
         * freq = (base_freq * 5) + (step * chan_spac * 125) */
        int denom = p25_channel_type_slots_per_carrier((int)chan_type);
        int step = (int)chan_number / denom;
        long expected = ((long)base_freq * 5L) + ((long)step * (long)chan_spac * 125L);

        /* Call the function under test */
        long actual = process_channel_to_freq(&opts, &st, channel);

        if (actual != expected) {
            if (failures < 5) { /* Limit diagnostic output */
                DSD_FPRINTF(stderr,
                            "P2 FAIL iter=%d: iden=%u base=%u spac=%u type=%u chan=%u "
                            "denom=%d step=%d expected=%ld got=%ld\n",
                            iter, iden, base_freq, chan_spac, chan_type, chan_number, denom, step, expected, actual);
            }
            failures++;
        }
    }

    if (failures == 0) {
        DSD_FPRINTF(stderr, "PASS P2: TDMA formula correctness (%d iterations)\n", PBT_ITERATIONS);
    } else {
        DSD_FPRINTF(stderr, "FAIL P2: %d/%d iterations failed\n", failures, PBT_ITERATIONS);
    }
    return failures;
}

/* --------------------------------------------------------------------------
 * Property P3: trunk_chan_map cache always takes precedence
 *
 * If trunk_chan_map[chan16] != 0, process_channel_to_freq must return
 * that cached value regardless of what the IDEN table contains.
 * -------------------------------------------------------------------------- */
static int
test_property_p3_cache_precedence(void) {
    int failures = 0;
    prng_seed(PBT_SEED + 2);

    for (int iter = 0; iter < PBT_ITERATIONS; iter++) {
        static dsd_opts opts;
        static dsd_state st;
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);

        /* Generate random parameters */
        uint32_t iden = prng_range(0, 15);
        uint32_t base_freq = prng_range(1, 200000000);
        uint32_t chan_spac = prng_range(1, 1023);
        uint32_t chan_type = prng_range(0, 15);
        uint32_t chan_number = prng_range(0, 4095);

        /* Generate a random cached frequency (non-zero) */
        long cached_freq = (long)prng_range(100000000, 999999999);

        /* Populate FDMA entry (so IDEN table has valid data) */
        st.p25_iden_fdma[iden].base_freq = (long int)base_freq;
        st.p25_iden_fdma[iden].chan_spac = (int)chan_spac;
        st.p25_iden_fdma[iden].chan_type = (int)chan_type;
        st.p25_iden_fdma[iden].populated = 1;
        st.p25_chan_tdma_explicit[iden] = 1;

        /* Construct 16-bit channel */
        uint16_t chan16 = (uint16_t)((iden << 12) | (chan_number & 0xFFF));
        if (chan16 == 0xFFFF) {
            continue;
        }
        int channel = (int)chan16;

        /* Pre-populate the cache — this should take precedence */
        st.trunk_chan_map[chan16] = cached_freq;

        /* Call the function under test */
        long actual = process_channel_to_freq(&opts, &st, channel);

        if (actual != cached_freq) {
            if (failures < 5) {
                DSD_FPRINTF(stderr, "P3 FAIL iter=%d: chan16=0x%04X cached=%ld got=%ld\n", iter, chan16, cached_freq,
                            actual);
            }
            failures++;
        }
    }

    if (failures == 0) {
        DSD_FPRINTF(stderr, "PASS P3: Cache precedence (%d iterations)\n", PBT_ITERATIONS);
    } else {
        DSD_FPRINTF(stderr, "FAIL P3: %d/%d iterations failed\n", failures, PBT_ITERATIONS);
    }
    return failures;
}

/* --------------------------------------------------------------------------
 * Property P4: Empty slots (populated=0) always return 0
 *
 * When neither FDMA nor TDMA entry is populated for a given IDEN slot,
 * process_channel_to_freq must return 0 (refuse tune).
 * -------------------------------------------------------------------------- */
static int
test_property_p4_empty_slot_returns_zero(void) {
    int failures = 0;
    prng_seed(PBT_SEED + 3);

    for (int iter = 0; iter < PBT_ITERATIONS; iter++) {
        static dsd_opts opts;
        static dsd_state st;
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);

        /* Generate random channel parameters */
        uint32_t iden = prng_range(0, 15);
        uint32_t chan_number = prng_range(0, 4095);

        /* Leave both arrays unpopulated (populated=0, base_freq=0, chan_spac=0) */
        /* Set explicit hint to FDMA-only or TDMA-only randomly */
        uint32_t hint = prng_range(1, 2);
        st.p25_chan_tdma_explicit[iden] = (uint8_t)hint;

        /* Construct 16-bit channel */
        uint16_t chan16 = (uint16_t)((iden << 12) | (chan_number & 0xFFF));
        int channel = (int)chan16;

        /* Ensure cache is empty for this channel */
        st.trunk_chan_map[chan16] = 0;

        /* Call the function under test — should return 0 */
        long actual = process_channel_to_freq(&opts, &st, channel);

        if (actual != 0) {
            if (failures < 5) {
                DSD_FPRINTF(stderr, "P4 FAIL iter=%d: iden=%u chan=%u hint=%u got=%ld (expected 0)\n", iter, iden,
                            chan_number, hint, actual);
            }
            failures++;
        }
    }

    if (failures == 0) {
        DSD_FPRINTF(stderr, "PASS P4: Empty slot returns 0 (%d iterations)\n", PBT_ITERATIONS);
    } else {
        DSD_FPRINTF(stderr, "FAIL P4: %d/%d iterations failed\n", failures, PBT_ITERATIONS);
    }
    return failures;
}

int
main(void) {
    int rc = 0;

    DSD_FPRINTF(stderr, "=== P25 IDEN Preservation Property-Based Tests ===\n");
    DSD_FPRINTF(stderr, "Seed: 0x%08X, Iterations per property: %d\n\n", PBT_SEED, PBT_ITERATIONS);

    rc |= test_property_p1_fdma_formula();
    rc |= test_property_p2_tdma_formula();
    rc |= test_property_p3_cache_precedence();
    rc |= test_property_p4_empty_slot_returns_zero();

    DSD_FPRINTF(stderr, "\n");
    if (rc == 0) {
        DSD_FPRINTF(stderr, "All preservation property tests PASSED (%d total iterations)\n", PBT_ITERATIONS * 4);
    } else {
        DSD_FPRINTF(stderr, "Some preservation property tests FAILED\n");
    }
    return rc != 0 ? 1 : 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
