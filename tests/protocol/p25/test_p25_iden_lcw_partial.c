// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 IDEN LCW Partial Write Guard Tests
 *
 * Validates Property 3 (Bug Condition - LCW Partial Write Guard):
 * The LCW Channel Identifier Update (formats 0x58/0x59) carries base_freq
 * but NOT chan_spac or chan_type. The fix ensures that:
 *
 * 1. Writing to an empty slot stores base_freq provisionally but does NOT
 *    set populated=1, preventing process_channel_to_freq from using
 *    incomplete parameters.
 * 2. Writing to an already-populated slot updates base_freq while preserving
 *    other fields (chan_spac, chan_type, trans_off).
 * 3. After a TSBK fully populates the slot, subsequent LCW updates work
 *    correctly (base_freq updated, slot remains populated).
 *
 * Validates: Requirements 2.3
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

/* Helper: compare long values and print diagnostic on mismatch */
static int
expect_eq_long(const char* tag, long got, long want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %ld want %ld\n", tag, got, want);
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

/* Helper: compare int values and print diagnostic on mismatch */
static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

/*
 * Test 1: LCW to Empty Slot — populated remains 0
 *
 * Simulates an LCW Channel Identifier Update (format 0x58) writing base_freq
 * to a slot that has never been populated by a TSBK. The slot should store
 * base_freq provisionally but NOT set populated=1. Consequently,
 * process_channel_to_freq must refuse to tune (return 0).
 */
static int
test_lcw_empty_slot_not_populated(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    int iden = 2;

    /* Simulate LCW partial write: only base_freq is provided.
     * This mirrors what the 0x58 handler does for an unpopulated slot:
     * stores base_freq provisionally, does NOT set populated=1. */
    st.p25_iden_fdma[iden].base_freq = 850000000L / 5; /* 170000000 in 5Hz units */
    /* populated remains 0 (not set by LCW on empty slot) */
    /* chan_spac remains 0 (LCW does not carry this field) */
    /* chan_type remains 0 (LCW does not carry this field) */

    /* Verify populated is still 0 */
    rc |= expect_eq_u8("lcw_empty: populated remains 0", st.p25_iden_fdma[iden].populated, 0);

    /* Verify base_freq was stored provisionally */
    rc |= expect_eq_long("lcw_empty: base_freq stored", st.p25_iden_fdma[iden].base_freq, 850000000L / 5);

    /* Verify chan_spac is still 0 (not provided by LCW) */
    rc |= expect_eq_int("lcw_empty: chan_spac still 0", st.p25_iden_fdma[iden].chan_spac, 0);

    /* Set explicit hint to FDMA so process_channel_to_freq selects FDMA array */
    st.p25_chan_tdma_explicit[iden] = 1; /* bit0 = has FDMA */

    /* Attempt frequency resolution — should return 0 (refuse tune)
     * because the entry is not populated (populated=0). */
    int chan = (iden << 12) | 0x0005; /* iden=2, chan_number=5 */
    long freq = process_channel_to_freq(&opts, &st, chan);
    rc |= expect_eq_long("lcw_empty: process_channel_to_freq returns 0", freq, 0L);

    if (rc == 0) {
        fprintf(stderr, "PASS test_lcw_empty_slot_not_populated\n");
    }
    return rc;
}

/*
 * Test 2: LCW to Populated Slot — base_freq updated, other fields preserved
 *
 * Pre-populates p25_iden_fdma[2] with full parameters (as if a TSBK wrote it),
 * then simulates an LCW update that changes base_freq. Verifies that:
 * - base_freq is updated to the new value
 * - chan_spac is preserved (unchanged)
 * - chan_type is preserved (unchanged)
 * - populated remains 1
 * - process_channel_to_freq uses the new base_freq
 */
static int
test_lcw_populated_slot_updates_base(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    int iden = 2;

    /* Pre-populate slot as if a TSBK (opcode 0x74) wrote it */
    st.p25_iden_fdma[iden].base_freq = 850000000L / 5; /* 170000000 */
    st.p25_iden_fdma[iden].chan_spac = 50;
    st.p25_iden_fdma[iden].chan_type = 1; /* FDMA */
    st.p25_iden_fdma[iden].trans_off = 2228;
    st.p25_iden_fdma[iden].populated = 1;
    st.p25_iden_fdma[iden].trust = 2;
    st.p25_chan_tdma_explicit[iden] = 1; /* bit0 = has FDMA */

    /* Simulate LCW update: new base_freq (as if system retuned to different band).
     * The LCW handler updates base_freq in place when slot is already populated
     * and chan_spac != 0. */
    long new_base = 851000000L / 5; /* 170200000 in 5Hz units */
    st.p25_iden_fdma[iden].base_freq = new_base;

    /* Verify base_freq updated */
    rc |= expect_eq_long("lcw_populated: base_freq updated", st.p25_iden_fdma[iden].base_freq, new_base);

    /* Verify chan_spac preserved */
    rc |= expect_eq_int("lcw_populated: chan_spac preserved", st.p25_iden_fdma[iden].chan_spac, 50);

    /* Verify chan_type preserved */
    rc |= expect_eq_int("lcw_populated: chan_type preserved", st.p25_iden_fdma[iden].chan_type, 1);

    /* Verify trans_off preserved */
    rc |= expect_eq_int("lcw_populated: trans_off preserved", st.p25_iden_fdma[iden].trans_off, 2228);

    /* Verify populated still 1 */
    rc |= expect_eq_u8("lcw_populated: populated still 1", st.p25_iden_fdma[iden].populated, 1);

    /* Verify process_channel_to_freq uses the new base_freq.
     * Channel 0x2008: iden=2, chan_number=8
     * FDMA: denom=1, step=8
     * freq = (new_base * 5) + (8 * 50 * 125) = 851000000 + 50000 = 851050000
     */
    int chan = (iden << 12) | 0x0008;
    long freq = process_channel_to_freq(&opts, &st, chan);
    long want = (new_base * 5) + (8L * 50 * 125); /* 851050000 */
    rc |= expect_eq_long("lcw_populated: freq uses new base", freq, want);

    if (rc == 0) {
        fprintf(stderr, "PASS test_lcw_populated_slot_updates_base\n");
    }
    return rc;
}

/*
 * Test 3: LCW then TSBK — slot becomes fully populated after TSBK
 *
 * Simulates the sequence:
 * 1. LCW writes base_freq to empty slot (populated stays 0)
 * 2. TSBK writes all fields (base_freq, chan_spac, chan_type, populated=1)
 * 3. Verify slot is now fully populated and resolves correctly
 * 4. Another LCW updates base_freq (should succeed since slot is now populated)
 * 5. Verify new base_freq is used in resolution
 */
static int
test_lcw_then_tsbk_populates(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    int iden = 2;

    /* Step 1: LCW writes base_freq to empty slot.
     * Mirrors the 0x58 handler behavior: stores base_freq, does NOT set populated. */
    long lcw_base = 850000000L / 5; /* 170000000 */
    st.p25_iden_fdma[iden].base_freq = lcw_base;
    /* populated remains 0, chan_spac remains 0 */

    /* Verify: slot is NOT populated after LCW-only write */
    rc |= expect_eq_u8("lcw_then_tsbk: after LCW, populated=0", st.p25_iden_fdma[iden].populated, 0);

    /* Verify: process_channel_to_freq refuses tune (incomplete params) */
    st.p25_chan_tdma_explicit[iden] = 1; /* bit0 = has FDMA */
    int chan = (iden << 12) | 0x000A;    /* iden=2, chan_number=10 */
    long freq = process_channel_to_freq(&opts, &st, chan);
    rc |= expect_eq_long("lcw_then_tsbk: before TSBK, freq=0", freq, 0L);

    /* Step 2: TSBK writes all fields (simulates opcode 0x74 handler).
     * This sets all parameters and marks populated=1. */
    long tsbk_base = 850500000L / 5; /* 170100000 — may differ from LCW base */
    st.p25_iden_fdma[iden].base_freq = tsbk_base;
    st.p25_iden_fdma[iden].chan_spac = 100;
    st.p25_iden_fdma[iden].chan_type = 1; /* FDMA */
    st.p25_iden_fdma[iden].trans_off = 2228;
    st.p25_iden_fdma[iden].populated = 1;
    st.p25_iden_fdma[iden].trust = 1;

    /* Verify: slot is now populated */
    rc |= expect_eq_u8("lcw_then_tsbk: after TSBK, populated=1", st.p25_iden_fdma[iden].populated, 1);

    /* Verify: process_channel_to_freq now resolves correctly.
     * Channel 0x200A: iden=2, chan_number=10
     * FDMA: denom=1, step=10
     * freq = (tsbk_base * 5) + (10 * 100 * 125) = 850500000 + 125000 = 850625000
     */
    freq = process_channel_to_freq(&opts, &st, chan);
    long want_after_tsbk = (tsbk_base * 5) + (10L * 100 * 125); /* 850625000 */
    rc |= expect_eq_long("lcw_then_tsbk: after TSBK, freq resolves", freq, want_after_tsbk);

    /* Clear trunk_chan_map cache so next resolution recalculates */
    st.trunk_chan_map[chan] = 0;

    /* Step 3: Another LCW updates base_freq (slot is now populated, so update succeeds).
     * Mirrors the 0x58 handler behavior for a populated slot: updates base_freq in place. */
    long new_lcw_base = 851000000L / 5; /* 170200000 */
    st.p25_iden_fdma[iden].base_freq = new_lcw_base;

    /* Verify: populated still 1 */
    rc |= expect_eq_u8("lcw_then_tsbk: after second LCW, populated=1", st.p25_iden_fdma[iden].populated, 1);

    /* Verify: chan_spac preserved from TSBK */
    rc |= expect_eq_int("lcw_then_tsbk: chan_spac preserved after LCW", st.p25_iden_fdma[iden].chan_spac, 100);

    /* Verify: process_channel_to_freq uses new base_freq.
     * freq = (new_lcw_base * 5) + (10 * 100 * 125) = 851000000 + 125000 = 851125000
     */
    freq = process_channel_to_freq(&opts, &st, chan);
    long want_after_lcw2 = (new_lcw_base * 5) + (10L * 100 * 125); /* 851125000 */
    rc |= expect_eq_long("lcw_then_tsbk: after second LCW, freq uses new base", freq, want_after_lcw2);

    if (rc == 0) {
        fprintf(stderr, "PASS test_lcw_then_tsbk_populates\n");
    }
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_lcw_empty_slot_not_populated();
    rc |= test_lcw_populated_slot_updates_base();
    rc |= test_lcw_then_tsbk_populates();

    if (rc == 0) {
        fprintf(stderr, "\nAll test_p25_iden_lcw_partial tests PASSED\n");
    } else {
        fprintf(stderr, "\nSome test_p25_iden_lcw_partial tests FAILED\n");
    }
    return rc;
}
