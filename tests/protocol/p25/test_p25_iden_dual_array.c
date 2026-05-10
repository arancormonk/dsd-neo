// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 IDEN Dual-Array Isolation Tests
 *
 * Validates that TDMA and FDMA IDEN entries on the same slot do not interfere
 * with each other. The dual-array split ensures that multi-mode systems
 * broadcasting both TDMA and FDMA IDEN updates on the same 4-bit identifier
 * ID resolve correctly regardless of broadcast cycle phase.
 *
 * Also exercises cycling scenarios where the control channel alternates
 * TDMA and FDMA updates on the same slot across multiple cycles.
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

/*
 * Simulate an opcode 0x73 (TDMA Abbreviated) IDEN update.
 * Writes to the TDMA array and sets the explicit hint bit1.
 */
static void
write_tdma_iden(dsd_state* st, int iden, long base_freq, int chan_spac, int trans_off, int chan_type) {
    st->p25_iden_tdma[iden].base_freq = base_freq;
    st->p25_iden_tdma[iden].chan_spac = chan_spac;
    st->p25_iden_tdma[iden].trans_off = trans_off;
    st->p25_iden_tdma[iden].chan_type = chan_type;
    st->p25_iden_tdma[iden].populated = 1;
    st->p25_chan_tdma_explicit[iden] |= 2; /* bit1 = has TDMA entry */
}

/*
 * Simulate an opcode 0x74 (VHF/UHF FDMA) IDEN update.
 * Writes to the FDMA array and sets the explicit hint bit0.
 */
static void
write_fdma_iden(dsd_state* st, int iden, long base_freq, int chan_spac, int trans_off, int chan_type) {
    st->p25_iden_fdma[iden].base_freq = base_freq;
    st->p25_iden_fdma[iden].chan_spac = chan_spac;
    st->p25_iden_fdma[iden].trans_off = trans_off;
    st->p25_iden_fdma[iden].chan_type = chan_type;
    st->p25_iden_fdma[iden].populated = 1;
    st->p25_chan_tdma_explicit[iden] |= 1; /* bit0 = has FDMA entry */
}

/*
 * Test 1: TDMA/FDMA Isolation
 *
 * Populate both arrays on the same slot with different parameters.
 * Verify resolving with FDMA context uses FDMA entry and TDMA context
 * uses the TDMA entry.
 */
static int
test_tdma_fdma_isolation(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    int iden = 0;
    long base_5hz = 850000000L / 5; /* 170000000 */

    /* Populate FDMA entry */
    st.p25_iden_fdma[iden].base_freq = base_5hz;
    st.p25_iden_fdma[iden].chan_spac = 50;
    st.p25_iden_fdma[iden].trans_off = 2200;
    st.p25_iden_fdma[iden].chan_type = 1; /* FDMA: denom=1 */
    st.p25_iden_fdma[iden].populated = 1;

    /* Populate TDMA entry */
    st.p25_iden_tdma[iden].base_freq = base_5hz;
    st.p25_iden_tdma[iden].chan_spac = 50;
    st.p25_iden_tdma[iden].trans_off = 7200;
    st.p25_iden_tdma[iden].chan_type = 3; /* TDMA: slots_per_carrier[3]=2, denom=2 */
    st.p25_iden_tdma[iden].populated = 1;

    st.p25_chan_tdma_explicit[iden] = 3; /* bit0=FDMA, bit1=TDMA */

    /* --- Test FDMA resolution --- */
    st.p25_chan_tdma_explicit[iden] = 1; /* FDMA only context */

    /* Channel 0x000A: iden=0, chan_number=10
     * FDMA: denom=1, step=10
     * freq = (base_5hz * 5) + (10 * 50 * 125) = 850000000 + 62500 = 850062500 */
    int chan_fdma = (iden << 12) | 0x000A;
    long f_fdma = process_channel_to_freq(&opts, &st, chan_fdma);
    long want_fdma = 850000000L + (10L * 50 * 125);
    rc |= expect_eq_long("fdma_isolation: FDMA resolve", f_fdma, want_fdma);

    /* --- Test TDMA resolution --- */
    st.p25_chan_tdma_explicit[iden] = 2; /* TDMA only context */

    /* Channel 0x0014: iden=0, chan_number=20
     * TDMA: chan_type=3, denom=2, step=20/2=10
     * freq = (base_5hz * 5) + (10 * 50 * 125) = 850000000 + 62500 = 850062500 */
    int chan_tdma = (iden << 12) | 0x0014;
    long f_tdma = process_channel_to_freq(&opts, &st, chan_tdma);
    long want_tdma = 850000000L + (10L * 50 * 125);
    rc |= expect_eq_long("fdma_isolation: TDMA resolve", f_tdma, want_tdma);

    /* Test with channel where denom difference produces different freq.
     * Channel 0x0018: iden=0, chan_number=24 */
    st.p25_chan_tdma_explicit[iden] = 1; /* FDMA context */
    int chan2 = (iden << 12) | 0x0018;
    st.trunk_chan_map[chan2] = 0;
    long f_fdma2 = process_channel_to_freq(&opts, &st, chan2);
    /* FDMA: denom=1, step=24, freq = 850000000 + (24 * 50 * 125) = 850150000 */
    long want_fdma2 = 850000000L + (24L * 50 * 125);
    rc |= expect_eq_long("fdma_isolation: FDMA chan 0x0018", f_fdma2, want_fdma2);

    st.trunk_chan_map[chan2] = 0;
    st.p25_chan_tdma_explicit[iden] = 2; /* TDMA context */
    long f_tdma2 = process_channel_to_freq(&opts, &st, chan2);
    /* TDMA: chan_type=3, denom=2, step=24/2=12, freq = 850000000 + (12 * 50 * 125) = 850075000 */
    long want_tdma2 = 850000000L + (12L * 50 * 125);
    rc |= expect_eq_long("fdma_isolation: TDMA chan 0x0018", f_tdma2, want_tdma2);

    if (rc == 0) {
        fprintf(stderr, "PASS test_tdma_fdma_isolation\n");
    }
    return rc;
}

/*
 * Test 2: Multi-Band Cycling — No Corruption
 *
 * Write TDMA params for one band to a slot, then write FDMA params for
 * the same slot. Verify neither array is corrupted by the other write.
 */
static int
test_multiband_cycling_no_corruption(void) {
    int rc = 0;
    static dsd_state st;
    memset(&st, 0, sizeof st);

    int iden = 2;

    /* Write TDMA params (band A) */
    st.p25_iden_tdma[iden].base_freq = 850500000L / 5;
    st.p25_iden_tdma[iden].chan_spac = 100;
    st.p25_iden_tdma[iden].trans_off = 3600;
    st.p25_iden_tdma[iden].chan_type = 3;
    st.p25_iden_tdma[iden].populated = 1;
    st.p25_chan_tdma_explicit[iden] |= 2;

    /* Write FDMA params (band B) */
    st.p25_iden_fdma[iden].base_freq = 850000000L / 5;
    st.p25_iden_fdma[iden].chan_spac = 50;
    st.p25_iden_fdma[iden].trans_off = 2200;
    st.p25_iden_fdma[iden].chan_type = 1;
    st.p25_iden_fdma[iden].populated = 1;
    st.p25_chan_tdma_explicit[iden] |= 1;

    /* Verify TDMA entry unchanged after FDMA write */
    rc |= expect_eq_long("multiband: TDMA base_freq", st.p25_iden_tdma[iden].base_freq, 850500000L / 5);
    rc |= expect_eq_long("multiband: TDMA chan_spac", (long)st.p25_iden_tdma[iden].chan_spac, 100L);
    rc |= expect_eq_long("multiband: TDMA trans_off", (long)st.p25_iden_tdma[iden].trans_off, 3600L);
    rc |= expect_eq_long("multiband: TDMA chan_type", (long)st.p25_iden_tdma[iden].chan_type, 3L);

    /* Verify FDMA entry correct */
    rc |= expect_eq_long("multiband: FDMA base_freq", st.p25_iden_fdma[iden].base_freq, 850000000L / 5);
    rc |= expect_eq_long("multiband: FDMA chan_spac", (long)st.p25_iden_fdma[iden].chan_spac, 50L);
    rc |= expect_eq_long("multiband: FDMA trans_off", (long)st.p25_iden_fdma[iden].trans_off, 2200L);
    rc |= expect_eq_long("multiband: FDMA chan_type", (long)st.p25_iden_fdma[iden].chan_type, 1L);

    if (rc == 0) {
        fprintf(stderr, "PASS test_multiband_cycling_no_corruption\n");
    }
    return rc;
}

/*
 * Test 3: Explicit Hint Bitmask
 *
 * Verify that writing FDMA sets bit0 and writing TDMA sets bit1, and that
 * the OR-semantics preserve previously set bits.
 */
static int
test_explicit_hint_bitmask(void) {
    int rc = 0;
    static dsd_state st;
    memset(&st, 0, sizeof st);

    int iden = 0;

    rc |= expect_eq_u8("bitmask: initial zero", st.p25_chan_tdma_explicit[iden], 0);

    /* FDMA write sets bit0 */
    st.p25_iden_fdma[iden].base_freq = 850000000L / 5;
    st.p25_iden_fdma[iden].chan_spac = 50;
    st.p25_iden_fdma[iden].chan_type = 1;
    st.p25_iden_fdma[iden].populated = 1;
    st.p25_chan_tdma_explicit[iden] |= 1;

    rc |= expect_eq_u8("bitmask: after FDMA write", st.p25_chan_tdma_explicit[iden], 1);

    /* TDMA write sets bit1, preserves bit0 */
    st.p25_iden_tdma[iden].base_freq = 850000000L / 5;
    st.p25_iden_tdma[iden].chan_spac = 50;
    st.p25_iden_tdma[iden].chan_type = 3;
    st.p25_iden_tdma[iden].populated = 1;
    st.p25_chan_tdma_explicit[iden] |= 2;

    rc |= expect_eq_u8("bitmask: after TDMA write (both bits)", st.p25_chan_tdma_explicit[iden], 3);

    /* Repeated writes don't clear other bits */
    st.p25_chan_tdma_explicit[iden] |= 1;
    rc |= expect_eq_u8("bitmask: second FDMA preserves TDMA", st.p25_chan_tdma_explicit[iden], 3);
    st.p25_chan_tdma_explicit[iden] |= 2;
    rc |= expect_eq_u8("bitmask: second TDMA preserves FDMA", st.p25_chan_tdma_explicit[iden], 3);

    if (rc == 0) {
        fprintf(stderr, "PASS test_explicit_hint_bitmask\n");
    }
    return rc;
}

/*
 * Test 4: Fallback to Other Array
 *
 * When the preferred array (based on explicit hint) is unpopulated, the
 * resolution logic should fall back to the other array.
 */
static int
test_fallback_to_other_array(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    int iden = 1;
    long base_5hz = 850000000L / 5;

    /* Populate ONLY the TDMA array */
    st.p25_iden_tdma[iden].base_freq = base_5hz;
    st.p25_iden_tdma[iden].chan_spac = 50;
    st.p25_iden_tdma[iden].trans_off = 7200;
    st.p25_iden_tdma[iden].chan_type = 3;
    st.p25_iden_tdma[iden].populated = 1;

    /* Set FDMA context — should fall back to TDMA entry */
    st.p25_chan_tdma_explicit[iden] = 1;
    int chan = (iden << 12) | 0x0008;
    long f = process_channel_to_freq(&opts, &st, chan);
    /* FDMA context: denom=1, step=8, freq = 850000000 + (8 * 50 * 125) = 850050000 */
    long want = 850000000L + (8L * 50 * 125);
    rc |= expect_eq_long("fallback: FDMA context uses TDMA entry", f, want);

    /* Reverse: only FDMA populated, TDMA context */
    memset(&st, 0, sizeof st);
    st.p25_iden_fdma[iden].base_freq = base_5hz;
    st.p25_iden_fdma[iden].chan_spac = 50;
    st.p25_iden_fdma[iden].trans_off = 2200;
    st.p25_iden_fdma[iden].chan_type = 1;
    st.p25_iden_fdma[iden].populated = 1;

    st.p25_chan_tdma_explicit[iden] = 2; /* TDMA context, but only FDMA populated */
    chan = (iden << 12) | 0x0008;
    f = process_channel_to_freq(&opts, &st, chan);
    /* Falls back to FDMA entry; chan_type=1, slots_per_carrier[1]=1, denom=1, step=8 */
    rc |= expect_eq_long("fallback: TDMA context uses FDMA entry", f, want);

    if (rc == 0) {
        fprintf(stderr, "PASS test_fallback_to_other_array\n");
    }
    return rc;
}

/*
 * Test 5: Alternating TDMA/FDMA Cycling on Same Slot
 *
 * Simulates a control channel alternating TDMA then FDMA IDEN updates on
 * the same slot across multiple cycles. Verifies neither array is corrupted
 * and grants resolve correctly after each cycle.
 */
static int
test_slot_tdma_fdma_cycling(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    int iden = 0;
    long base_5hz = 850000000L / 5;

    for (int cycle = 0; cycle < 5; cycle++) {
        write_tdma_iden(&st, iden, base_5hz, 50, 7200, 1);
        write_fdma_iden(&st, iden, base_5hz, 50, 2200, 1);

        /* Verify TDMA array not corrupted by FDMA write */
        rc |= expect_eq_long("cycling: TDMA trans_off", (long)st.p25_iden_tdma[iden].trans_off, 7200L);
        rc |= expect_eq_long("cycling: FDMA trans_off", (long)st.p25_iden_fdma[iden].trans_off, 2200L);
        rc |= expect_eq_u8("cycling: explicit both bits", st.p25_chan_tdma_explicit[iden], 3);

        /* Resolve FDMA grant */
        st.p25_chan_tdma_explicit[iden] = 1;
        int chan_fdma = (iden << 12) | 0x000A;
        st.trunk_chan_map[chan_fdma] = 0;
        long f_fdma = process_channel_to_freq(&opts, &st, chan_fdma);
        long want_fdma = 850000000L + (10L * 50 * 125);
        rc |= expect_eq_long("cycling: FDMA grant", f_fdma, want_fdma);

        /* Resolve TDMA grant */
        st.p25_chan_tdma_explicit[iden] = 2;
        int chan_tdma = (iden << 12) | 0x0014;
        st.trunk_chan_map[chan_tdma] = 0;
        long f_tdma = process_channel_to_freq(&opts, &st, chan_tdma);
        long want_tdma = 850000000L + (20L * 50 * 125);
        rc |= expect_eq_long("cycling: TDMA grant", f_tdma, want_tdma);

        st.p25_chan_tdma_explicit[iden] = 3;
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_slot_tdma_fdma_cycling\n");
    }
    return rc;
}

/*
 * Test 6: Intra-Class TDMA Cycling — Two Bands on Same Slot
 *
 * Two different TDMA bands share the same slot. The second write overwrites
 * the first in the TDMA array (expected — same modulation class). The FDMA
 * array remains independent. Ambiguous FDMA+TDMA slots are not cached because
 * one channel key can resolve through either modulation table.
 */
static int
test_multiband_tdma_cycling(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    int iden = 2;
    long base_a_5hz = 850500000L / 5; /* Band A */
    long base_b_5hz = 500000000L / 5; /* Band B */

    /* Populate FDMA (independent of TDMA cycling) */
    write_fdma_iden(&st, iden, 850000000L / 5, 50, 2200, 1);

    /* Write Band A TDMA */
    write_tdma_iden(&st, iden, base_a_5hz, 100, 3600, 3);

    /* Resolve a TDMA grant while Band A is active.
     * chan_type=3, denom=2, chan_number=20, step=10
     * freq = 850500000 + (10 * 100 * 125) = 850625000 */
    st.p25_chan_tdma_explicit[iden] = 2;
    int chan_band_a = (iden << 12) | 0x0014;
    long f_band_a = process_channel_to_freq(&opts, &st, chan_band_a);
    long want_band_a = 850500000L + (10L * 100 * 125);
    rc |= expect_eq_long("multiband_tdma: Band A resolves", f_band_a, want_band_a);

    /* Write Band B TDMA — overwrites Band A */
    write_tdma_iden(&st, iden, base_b_5hz, 50, -480, 1);

    /* Verify Band B is now in TDMA array */
    rc |= expect_eq_long("multiband_tdma: TDMA now Band B", st.p25_iden_tdma[iden].base_freq, base_b_5hz);

    /* Verify FDMA array unaffected */
    rc |= expect_eq_long("multiband_tdma: FDMA unchanged", st.p25_iden_fdma[iden].base_freq, 850000000L / 5);

    /* Previously-resolved Band A channel is intentionally not cached while an
     * FDMA entry also exists for this IDEN; the current TDMA entry is used. */
    long f_after_update = process_channel_to_freq(&opts, &st, chan_band_a);
    long want_after_update = 500000000L + (20L * 50 * 125);
    rc |= expect_eq_long("multiband_tdma: Band A not cached after TDMA update", f_after_update, want_after_update);

    /* New TDMA grant uses Band B.
     * chan_type=1, denom=1, chan_number=10, step=10
     * freq = 500000000 + (10 * 50 * 125) = 500062500 */
    int chan_band_b = (iden << 12) | 0x000A;
    long f_band_b = process_channel_to_freq(&opts, &st, chan_band_b);
    long want_band_b = 500000000L + (10L * 50 * 125);
    rc |= expect_eq_long("multiband_tdma: Band B resolves", f_band_b, want_band_b);

    if (rc == 0) {
        fprintf(stderr, "PASS test_multiband_tdma_cycling\n");
    }
    return rc;
}

/*
 * Test 7: Full Multi-Mode Pattern — All Slots Cycling
 *
 * Simulates a complete broadcast pattern across slots 0-3 with multiple
 * cycles. After each cycle, resolves grants for both FDMA and TDMA on
 * each slot and verifies correctness.
 */
static int
test_full_multimode_pattern(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    /* Generic test parameters per slot (base_freq in 5Hz units) */
    struct {
        long tdma_base;
        long fdma_base;
        int tdma_spac;
        int tdma_off;
        int tdma_type;
        int fdma_spac;
        int fdma_off;
        int fdma_type;
    } slot_params[4] = {
        {850000000L / 5, 850000000L / 5, 50, 7200, 1, 50, 2200, 1},
        {850000000L / 5, 850000000L / 5, 50, 7200, 3, 50, 2200, 1},
        {850500000L / 5, 850000000L / 5, 100, 3600, 3, 50, 2200, 1},
        {850500000L / 5, 850500000L / 5, 100, 3600, 3, 100, 2200, 1},
    };

    static const int slots_per_carrier[16] = {1, 1, 1, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

    for (int cycle = 0; cycle < 3; cycle++) {
        for (int slot = 0; slot < 4; slot++) {
            write_tdma_iden(&st, slot, slot_params[slot].tdma_base, slot_params[slot].tdma_spac,
                            slot_params[slot].tdma_off, slot_params[slot].tdma_type);
            write_fdma_iden(&st, slot, slot_params[slot].fdma_base, slot_params[slot].fdma_spac,
                            slot_params[slot].fdma_off, slot_params[slot].fdma_type);
        }

        for (int slot = 0; slot < 4; slot++) {
            char tag[128];

            /* Verify TDMA array integrity */
            snprintf(tag, sizeof tag, "full[c%d][s%d]: TDMA base", cycle, slot);
            rc |= expect_eq_long(tag, st.p25_iden_tdma[slot].base_freq, slot_params[slot].tdma_base);
            snprintf(tag, sizeof tag, "full[c%d][s%d]: FDMA base", cycle, slot);
            rc |= expect_eq_long(tag, st.p25_iden_fdma[slot].base_freq, slot_params[slot].fdma_base);

            /* Resolve FDMA grant */
            st.p25_chan_tdma_explicit[slot] = 1;
            int chan_fdma = (slot << 12) | 0x0008;
            st.trunk_chan_map[chan_fdma] = 0;
            long f_fdma = process_channel_to_freq(&opts, &st, chan_fdma);
            long want_fdma = (slot_params[slot].fdma_base * 5) + (8L * slot_params[slot].fdma_spac * 125);
            snprintf(tag, sizeof tag, "full[c%d][s%d]: FDMA freq", cycle, slot);
            rc |= expect_eq_long(tag, f_fdma, want_fdma);

            /* Resolve TDMA grant */
            st.p25_chan_tdma_explicit[slot] = 2;
            int tdma_type = slot_params[slot].tdma_type;
            int denom = slots_per_carrier[tdma_type & 0xF];
            int chan_num = 0x0010;
            int step = chan_num / denom;
            int chan_tdma = (slot << 12) | chan_num;
            st.trunk_chan_map[chan_tdma] = 0;
            long f_tdma = process_channel_to_freq(&opts, &st, chan_tdma);
            long want_tdma = (slot_params[slot].tdma_base * 5) + ((long)step * slot_params[slot].tdma_spac * 125);
            snprintf(tag, sizeof tag, "full[c%d][s%d]: TDMA freq", cycle, slot);
            rc |= expect_eq_long(tag, f_tdma, want_tdma);

            st.p25_chan_tdma_explicit[slot] = 3;
        }
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_full_multimode_pattern\n");
    }
    return rc;
}

/*
 * Test 8: trunk_chan_map Does Not Collapse Ambiguous Dual-Mode Slots
 *
 * When both FDMA and TDMA entries exist for one IDEN, the same 16-bit channel
 * key can have two valid formulas. In that case, process_channel_to_freq()
 * must calculate from the selected IDEN entry instead of caching one result
 * into trunk_chan_map and reusing it for the other modulation class.
 */
static int
test_no_cache_collision_for_dual_mode_slot(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    int iden = 0;
    long base_5hz = 850000000L / 5;

    write_fdma_iden(&st, iden, base_5hz, 50, 2200, 1);
    write_tdma_iden(&st, iden, base_5hz, 50, 7200, 3);

    int chan = (iden << 12) | 0x000C;
    st.p25_chan_tdma_explicit[iden] = 3;

    st.p25_cc_is_tdma = 0;
    st.synctype = 0;
    long f_fdma_auto = process_channel_to_freq(&opts, &st, chan);
    long want_fdma = 850000000L + (12L * 50 * 125);
    rc |= expect_eq_long("dual cache: fdma auto", f_fdma_auto, want_fdma);
    rc |= expect_eq_long("dual cache: ambiguous map not populated", st.trunk_chan_map[chan], 0);

    st.p25_cc_is_tdma = 1;
    long f_tdma_auto = process_channel_to_freq(&opts, &st, chan);
    long want_tdma = 850000000L + (6L * 50 * 125);
    rc |= expect_eq_long("dual cache: tdma auto", f_tdma_auto, want_tdma);
    rc |= expect_eq_long("dual cache: ambiguous map still empty", st.trunk_chan_map[chan], 0);

    long f_fdma_explicit = process_channel_to_freq_with_mode(&opts, &st, chan, 0);
    long f_tdma_explicit = process_channel_to_freq_with_mode(&opts, &st, chan, 1);
    rc |= expect_eq_long("dual cache: fdma explicit", f_fdma_explicit, want_fdma);
    rc |= expect_eq_long("dual cache: tdma explicit", f_tdma_explicit, want_tdma);

    if (rc == 0) {
        fprintf(stderr, "PASS test_no_cache_collision_for_dual_mode_slot\n");
    }
    return rc;
}

/*
 * Test 9: Reverse Cycle Order — FDMA Before TDMA
 *
 * Verifies the dual-array works regardless of which modulation class writes first.
 */
static int
test_reverse_cycle_order(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    int iden = 1;
    long base_5hz = 850000000L / 5;

    for (int cycle = 0; cycle < 5; cycle++) {
        /* Reverse order: FDMA first, then TDMA */
        write_fdma_iden(&st, iden, base_5hz, 50, 2200, 1);
        write_tdma_iden(&st, iden, base_5hz, 50, 7200, 3);

        rc |= expect_eq_long("reverse: FDMA trans_off", (long)st.p25_iden_fdma[iden].trans_off, 2200L);
        rc |= expect_eq_long("reverse: TDMA trans_off", (long)st.p25_iden_tdma[iden].trans_off, 7200L);

        /* Resolve FDMA grant */
        st.p25_chan_tdma_explicit[iden] = 1;
        int chan_fdma = (iden << 12) | 0x0006;
        st.trunk_chan_map[chan_fdma] = 0;
        long f_fdma = process_channel_to_freq(&opts, &st, chan_fdma);
        long want_fdma = 850000000L + (6L * 50 * 125);
        rc |= expect_eq_long("reverse: FDMA grant", f_fdma, want_fdma);

        /* Resolve TDMA grant: chan_type=3, denom=2, chan_number=12, step=6 */
        st.p25_chan_tdma_explicit[iden] = 2;
        int chan_tdma = (iden << 12) | 0x000C;
        st.trunk_chan_map[chan_tdma] = 0;
        long f_tdma = process_channel_to_freq(&opts, &st, chan_tdma);
        long want_tdma = 850000000L + (6L * 50 * 125);
        rc |= expect_eq_long("reverse: TDMA grant", f_tdma, want_tdma);

        st.p25_chan_tdma_explicit[iden] = 3;
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_reverse_cycle_order\n");
    }
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_tdma_fdma_isolation();
    rc |= test_multiband_cycling_no_corruption();
    rc |= test_explicit_hint_bitmask();
    rc |= test_fallback_to_other_array();
    rc |= test_slot_tdma_fdma_cycling();
    rc |= test_multiband_tdma_cycling();
    rc |= test_full_multimode_pattern();
    rc |= test_no_cache_collision_for_dual_mode_slot();
    rc |= test_reverse_cycle_order();

    if (rc == 0) {
        fprintf(stderr, "\nAll test_p25_iden_dual_array tests PASSED\n");
    } else {
        fprintf(stderr, "\nSome test_p25_iden_dual_array tests FAILED\n");
    }
    return rc;
}
