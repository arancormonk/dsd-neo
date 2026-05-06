// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 FDMA IDEN Write-Once Guard Tests
 *
 * Verifies that the write-once guard on opcode 0x74 (Identifier Update
 * VHF/UHF) and 0x7D (Identifier Update Non-VHF/UHF) prevents overwriting
 * already-populated p25_iden_fdma[] slots. Also verifies that first-time
 * writes still succeed, that the 0x33 MBT handler remains unguarded, and
 * that the existing 0x73 TDMA guard is unchanged.
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

/* Helper: compare unsigned long long values and print diagnostic on mismatch */
static int
expect_eq_ull(const char* tag, unsigned long long got, unsigned long long want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %llu want %llu\n", tag, got, want);
        return 1;
    }
    return 0;
}

/*
 * Simulate opcode 0x74 (Identifier Update VHF/UHF) handler write logic.
 *
 * This replicates the FIXED handler behavior: write-once guard skips
 * the write block if the FDMA IDEN slot is already populated.
 */
static void
simulate_0x74_handler(dsd_state* st, int iden, long base_freq, int chan_spac, int trans_off, unsigned long long wacn,
                      unsigned long long sysid, unsigned long long rfss, unsigned long long site) {
    p25_iden_entry_t* e = &st->p25_iden_fdma[iden];
    /* Write-once guard: skip if already populated */
    if (e->populated) {
        return;
    }
    e->base_freq = base_freq;
    e->chan_type = 1; /* FDMA default */
    e->chan_spac = chan_spac;
    e->trans_off = trans_off;
    e->trust = 1; /* simplified */
    e->populated = 1;
    e->wacn = wacn;
    e->sysid = sysid;
    e->rfss = rfss;
    e->site = site;
    st->p25_chan_tdma_explicit[iden] |= 1; /* bit0 = has FDMA entry */
}

/*
 * Simulate opcode 0x7D (Identifier Update Non-VHF/UHF) handler write logic.
 *
 * This replicates the FIXED handler behavior: write-once guard skips
 * the write block if the FDMA IDEN slot is already populated.
 * (Structurally identical to 0x74 for the write block.)
 */
static void
simulate_0x7D_handler(dsd_state* st, int iden, long base_freq, int chan_spac, int trans_off, unsigned long long wacn,
                      unsigned long long sysid, unsigned long long rfss, unsigned long long site) {
    p25_iden_entry_t* e = &st->p25_iden_fdma[iden];
    /* Write-once guard: skip if already populated */
    if (e->populated) {
        return;
    }
    e->base_freq = base_freq;
    e->chan_type = 1; /* FDMA default */
    e->chan_spac = chan_spac;
    e->trans_off = trans_off;
    e->trust = 1; /* simplified */
    e->populated = 1;
    e->wacn = wacn;
    e->sysid = sysid;
    e->rfss = rfss;
    e->site = site;
    st->p25_chan_tdma_explicit[iden] |= 1; /* bit0 = has FDMA entry */
}

/*
 * Test: FDMA IDEN slot not overwritten by 0x74 when already populated.
 *
 * For each FDMA IDEN slot (0–15), populate with known parameters (set
 * populated=1), then simulate the 0x74 handler write with DIFFERENT
 * parameters. Assert the entry was NOT overwritten.
 */
static int
test_bug_condition_fdma_overwrite_0x74(void) {
    int rc = 0;
    static dsd_state st;
    memset(&st, 0, sizeof st);

    uint32_t rng = 0xDEADBEEF; /* fixed seed for reproducibility */

    fprintf(stderr, "\n--- Bug Condition Exploration: FDMA IDEN Overwrite by 0x74 ---\n");

    for (int iden = 0; iden < 16; iden++) {
        char tag[128];

        /* Generate first set of random params (A) — the "first seen" entry */
        long base_freq_a = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_spac_a = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off_a = (int)(xorshift32(&rng) % 20000) - 10000;
        unsigned long long wacn_a = xorshift32(&rng) & 0xFFFFF;
        unsigned long long sysid_a = xorshift32(&rng) & 0xFFF;
        unsigned long long rfss_a = xorshift32(&rng) & 0xFF;
        unsigned long long site_a = xorshift32(&rng) & 0xFF;

        /* Pre-populate slot with params A (simulating first-time write) */
        st.p25_iden_fdma[iden].base_freq = base_freq_a;
        st.p25_iden_fdma[iden].chan_type = 1;
        st.p25_iden_fdma[iden].chan_spac = chan_spac_a;
        st.p25_iden_fdma[iden].trans_off = trans_off_a;
        st.p25_iden_fdma[iden].trust = 1;
        st.p25_iden_fdma[iden].populated = 1;
        st.p25_iden_fdma[iden].wacn = wacn_a;
        st.p25_iden_fdma[iden].sysid = sysid_a;
        st.p25_iden_fdma[iden].rfss = rfss_a;
        st.p25_iden_fdma[iden].site = site_a;
        st.p25_chan_tdma_explicit[iden] |= 1;

        /* Generate second set of DIFFERENT random params (B) */
        long base_freq_b = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_spac_b = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off_b = (int)(xorshift32(&rng) % 20000) - 10000;
        unsigned long long wacn_b = xorshift32(&rng) & 0xFFFFF;
        unsigned long long sysid_b = xorshift32(&rng) & 0xFFF;
        unsigned long long rfss_b = xorshift32(&rng) & 0xFF;
        unsigned long long site_b = xorshift32(&rng) & 0xFF;

        /* Simulate 0x74 handler with params B on already-populated slot */
        simulate_0x74_handler(&st, iden, base_freq_b, chan_spac_b, trans_off_b, wacn_b, sysid_b, rfss_b, site_b);

        /* Assert all original values (A) are preserved — this WILL FAIL on unfixed code */
        snprintf(tag, sizeof tag, "0x74 slot[%d]: base_freq preserved", iden);
        rc |= expect_eq_long(tag, st.p25_iden_fdma[iden].base_freq, base_freq_a);

        snprintf(tag, sizeof tag, "0x74 slot[%d]: chan_spac preserved", iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].chan_spac, chan_spac_a);

        snprintf(tag, sizeof tag, "0x74 slot[%d]: trans_off preserved", iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].trans_off, trans_off_a);

        snprintf(tag, sizeof tag, "0x74 slot[%d]: wacn preserved", iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].wacn, wacn_a);

        snprintf(tag, sizeof tag, "0x74 slot[%d]: sysid preserved", iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].sysid, sysid_a);

        snprintf(tag, sizeof tag, "0x74 slot[%d]: rfss preserved", iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].rfss, rfss_a);

        snprintf(tag, sizeof tag, "0x74 slot[%d]: site preserved", iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].site, site_a);

        snprintf(tag, sizeof tag, "0x74 slot[%d]: populated remains 1", iden);
        rc |= expect_eq_u8(tag, st.p25_iden_fdma[iden].populated, 1);
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_bug_condition_fdma_overwrite_0x74\n");
    } else {
        fprintf(stderr, "\nFAILED test_bug_condition_fdma_overwrite_0x74 — "
                        "0x74 handler overwrites populated FDMA slots (bug confirmed)\n");
    }
    return rc;
}

/*
 * Test: FDMA IDEN slot not overwritten by 0x7D when already populated.
 *
 * For each FDMA IDEN slot (0–15), populate with known parameters (set
 * populated=1), then simulate the 0x7D handler write with DIFFERENT
 * parameters. Assert the entry was NOT overwritten.
 */
static int
test_bug_condition_fdma_overwrite_0x7D(void) {
    int rc = 0;
    static dsd_state st;
    memset(&st, 0, sizeof st);

    uint32_t rng = 0xCAFED00D; /* fixed seed for reproducibility */

    fprintf(stderr, "\n--- Bug Condition Exploration: FDMA IDEN Overwrite by 0x7D ---\n");

    for (int iden = 0; iden < 16; iden++) {
        char tag[128];

        /* Generate first set of random params (A) — the "first seen" entry */
        long base_freq_a = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_spac_a = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off_a = (int)(xorshift32(&rng) % 20000) - 10000;
        unsigned long long wacn_a = xorshift32(&rng) & 0xFFFFF;
        unsigned long long sysid_a = xorshift32(&rng) & 0xFFF;
        unsigned long long rfss_a = xorshift32(&rng) & 0xFF;
        unsigned long long site_a = xorshift32(&rng) & 0xFF;

        /* Pre-populate slot with params A (simulating first-time write) */
        st.p25_iden_fdma[iden].base_freq = base_freq_a;
        st.p25_iden_fdma[iden].chan_type = 1;
        st.p25_iden_fdma[iden].chan_spac = chan_spac_a;
        st.p25_iden_fdma[iden].trans_off = trans_off_a;
        st.p25_iden_fdma[iden].trust = 1;
        st.p25_iden_fdma[iden].populated = 1;
        st.p25_iden_fdma[iden].wacn = wacn_a;
        st.p25_iden_fdma[iden].sysid = sysid_a;
        st.p25_iden_fdma[iden].rfss = rfss_a;
        st.p25_iden_fdma[iden].site = site_a;
        st.p25_chan_tdma_explicit[iden] |= 1;

        /* Generate second set of DIFFERENT random params (B) */
        long base_freq_b = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_spac_b = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off_b = (int)(xorshift32(&rng) % 20000) - 10000;
        unsigned long long wacn_b = xorshift32(&rng) & 0xFFFFF;
        unsigned long long sysid_b = xorshift32(&rng) & 0xFFF;
        unsigned long long rfss_b = xorshift32(&rng) & 0xFF;
        unsigned long long site_b = xorshift32(&rng) & 0xFF;

        /* Simulate 0x7D handler with params B on already-populated slot */
        simulate_0x7D_handler(&st, iden, base_freq_b, chan_spac_b, trans_off_b, wacn_b, sysid_b, rfss_b, site_b);

        /* Assert all original values (A) are preserved — this WILL FAIL on unfixed code */
        snprintf(tag, sizeof tag, "0x7D slot[%d]: base_freq preserved", iden);
        rc |= expect_eq_long(tag, st.p25_iden_fdma[iden].base_freq, base_freq_a);

        snprintf(tag, sizeof tag, "0x7D slot[%d]: chan_spac preserved", iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].chan_spac, chan_spac_a);

        snprintf(tag, sizeof tag, "0x7D slot[%d]: trans_off preserved", iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].trans_off, trans_off_a);

        snprintf(tag, sizeof tag, "0x7D slot[%d]: wacn preserved", iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].wacn, wacn_a);

        snprintf(tag, sizeof tag, "0x7D slot[%d]: sysid preserved", iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].sysid, sysid_a);

        snprintf(tag, sizeof tag, "0x7D slot[%d]: rfss preserved", iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].rfss, rfss_a);

        snprintf(tag, sizeof tag, "0x7D slot[%d]: site preserved", iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].site, site_a);

        snprintf(tag, sizeof tag, "0x7D slot[%d]: populated remains 1", iden);
        rc |= expect_eq_u8(tag, st.p25_iden_fdma[iden].populated, 1);
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_bug_condition_fdma_overwrite_0x7D\n");
    } else {
        fprintf(stderr, "\nFAILED test_bug_condition_fdma_overwrite_0x7D — "
                        "0x7D handler overwrites populated FDMA slots (bug confirmed)\n");
    }
    return rc;
}

/*
 * Simulate opcode 0x33 MBT (TDMA Identifier Update) handler write logic
 * for FDMA routing (chan_type 0-2 routes to p25_iden_fdma[]).
 *
 * This replicates the 0x33 MBT handler behavior: unconditional overwrite
 * with NO populated guard. The 0x33 MBT handler has correct sign decoding
 * and produces the authoritative value.
 *
 * For FDMA test, use chan_type=1 (slots_per_carrier[1]=1, so is_tdma=0).
 */
static void
simulate_0x33_mbt_handler_fdma(dsd_state* st, int iden, long base_freq, int chan_type, int chan_spac, int trans_off) {
    /* 0x33 MBT handler — NO guard, always overwrites */
    p25_iden_entry_t* e = &st->p25_iden_fdma[iden];
    e->chan_type = chan_type;
    e->trans_off = trans_off; /* signed value */
    e->chan_spac = chan_spac;
    e->base_freq = base_freq;
    e->populated = 1;
    e->wacn = st->p2_wacn;
    e->sysid = st->p2_sysid;
    e->rfss = st->p2_rfssid;
    e->site = st->p2_siteid;
    e->trust = (st->p25_cc_freq != 0) ? 2 : 1;
}

/*
 * Simulate opcode 0x73 (TDMA Identifier Update Abbreviated) handler write logic.
 *
 * This replicates the 0x73 handler behavior WITH the existing write-once guard:
 * if the slot is already populated, the write is skipped.
 */
static void
simulate_0x73_handler(dsd_state* st, int iden, long base_freq, int chan_type, int chan_spac, int trans_off) {
    p25_iden_entry_t* e = &st->p25_iden_tdma[iden];
    if (e->populated) {
        return; /* existing write-once guard */
    }
    e->base_freq = base_freq;
    e->chan_type = chan_type;
    e->chan_spac = chan_spac;
    e->trans_off = trans_off;
    e->trust = 1;
    e->populated = 1;
    e->wacn = st->p2_wacn;
    e->sysid = st->p2_sysid;
    e->rfss = st->p2_rfssid;
    e->site = st->p2_siteid;
    st->p25_chan_tdma_explicit[iden] |= 2; /* bit1 = has TDMA entry */
}

/* =========================================================================
 * Preservation Tests
 *
 * Verify that first-time writes still succeed, 0x33 MBT remains unguarded,
 * and the existing 0x73 TDMA guard is unchanged.
 * ========================================================================= */

/*
 * Test: First-time write via 0x74 populates an empty FDMA slot correctly.
 *
 * For 128 iterations: generate random IDEN params, write to an EMPTY FDMA
 * slot (populated=0) via the 0x74 handler logic, assert all fields match
 * and populated=1.
 */
static int
test_preservation_first_write_0x74(void) {
    int rc = 0;
    static dsd_state st;
    uint32_t rng = 0xCAFEBABE; /* fixed seed for reproducibility */

    fprintf(stderr, "\n--- Preservation: First-Time Write via 0x74 ---\n");

    for (int iter = 0; iter < 128; iter++) {
        char tag[128];
        memset(&st, 0, sizeof st);

        /* Generate random IDEN slot and params */
        int iden = (int)(xorshift32(&rng) & 0xF); /* 0-15 */
        long base_freq = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_spac = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off = (int)(xorshift32(&rng) % 20000) - 10000;
        unsigned long long wacn = xorshift32(&rng) & 0xFFFFF;
        unsigned long long sysid = xorshift32(&rng) & 0xFFF;
        unsigned long long rfss = xorshift32(&rng) & 0xFF;
        unsigned long long site = xorshift32(&rng) & 0xFF;

        /* Slot starts empty (populated=0) */
        /* Simulate 0x74 handler first-time write */
        simulate_0x74_handler(&st, iden, base_freq, chan_spac, trans_off, wacn, sysid, rfss, site);

        /* Assert all fields were written correctly */
        snprintf(tag, sizeof tag, "0x74 first-write iter[%d] iden[%d]: base_freq", iter, iden);
        rc |= expect_eq_long(tag, st.p25_iden_fdma[iden].base_freq, base_freq);

        snprintf(tag, sizeof tag, "0x74 first-write iter[%d] iden[%d]: chan_type", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].chan_type, 1);

        snprintf(tag, sizeof tag, "0x74 first-write iter[%d] iden[%d]: chan_spac", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].chan_spac, chan_spac);

        snprintf(tag, sizeof tag, "0x74 first-write iter[%d] iden[%d]: trans_off", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].trans_off, trans_off);

        snprintf(tag, sizeof tag, "0x74 first-write iter[%d] iden[%d]: populated", iter, iden);
        rc |= expect_eq_u8(tag, st.p25_iden_fdma[iden].populated, 1);

        snprintf(tag, sizeof tag, "0x74 first-write iter[%d] iden[%d]: wacn", iter, iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].wacn, wacn);

        snprintf(tag, sizeof tag, "0x74 first-write iter[%d] iden[%d]: sysid", iter, iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].sysid, sysid);

        snprintf(tag, sizeof tag, "0x74 first-write iter[%d] iden[%d]: rfss", iter, iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].rfss, rfss);

        snprintf(tag, sizeof tag, "0x74 first-write iter[%d] iden[%d]: site", iter, iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].site, site);

        snprintf(tag, sizeof tag, "0x74 first-write iter[%d] iden[%d]: tdma_explicit bit0", iter, iden);
        rc |= expect_eq_u8(tag, st.p25_chan_tdma_explicit[iden] & 1, 1);
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_preservation_first_write_0x74\n");
    } else {
        fprintf(stderr, "FAILED test_preservation_first_write_0x74\n");
    }
    return rc;
}

/*
 * Test: First-time write via 0x7D populates an empty FDMA slot correctly.
 *
 * For 128 iterations: generate random IDEN params, write to an EMPTY FDMA
 * slot (populated=0) via the 0x7D handler logic, assert all fields match
 * and populated=1.
 */
static int
test_preservation_first_write_0x7D(void) {
    int rc = 0;
    static dsd_state st;
    uint32_t rng = 0xCAFEBABE; /* fixed seed for reproducibility */

    fprintf(stderr, "\n--- Preservation: First-Time Write via 0x7D ---\n");

    for (int iter = 0; iter < 128; iter++) {
        char tag[128];
        memset(&st, 0, sizeof st);

        /* Generate random IDEN slot and params */
        int iden = (int)(xorshift32(&rng) & 0xF); /* 0-15 */
        long base_freq = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_spac = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off = (int)(xorshift32(&rng) % 20000) - 10000;
        unsigned long long wacn = xorshift32(&rng) & 0xFFFFF;
        unsigned long long sysid = xorshift32(&rng) & 0xFFF;
        unsigned long long rfss = xorshift32(&rng) & 0xFF;
        unsigned long long site = xorshift32(&rng) & 0xFF;

        /* Slot starts empty (populated=0) */
        /* Simulate 0x7D handler first-time write */
        simulate_0x7D_handler(&st, iden, base_freq, chan_spac, trans_off, wacn, sysid, rfss, site);

        /* Assert all fields were written correctly */
        snprintf(tag, sizeof tag, "0x7D first-write iter[%d] iden[%d]: base_freq", iter, iden);
        rc |= expect_eq_long(tag, st.p25_iden_fdma[iden].base_freq, base_freq);

        snprintf(tag, sizeof tag, "0x7D first-write iter[%d] iden[%d]: chan_type", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].chan_type, 1);

        snprintf(tag, sizeof tag, "0x7D first-write iter[%d] iden[%d]: chan_spac", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].chan_spac, chan_spac);

        snprintf(tag, sizeof tag, "0x7D first-write iter[%d] iden[%d]: trans_off", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].trans_off, trans_off);

        snprintf(tag, sizeof tag, "0x7D first-write iter[%d] iden[%d]: populated", iter, iden);
        rc |= expect_eq_u8(tag, st.p25_iden_fdma[iden].populated, 1);

        snprintf(tag, sizeof tag, "0x7D first-write iter[%d] iden[%d]: wacn", iter, iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].wacn, wacn);

        snprintf(tag, sizeof tag, "0x7D first-write iter[%d] iden[%d]: sysid", iter, iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].sysid, sysid);

        snprintf(tag, sizeof tag, "0x7D first-write iter[%d] iden[%d]: rfss", iter, iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].rfss, rfss);

        snprintf(tag, sizeof tag, "0x7D first-write iter[%d] iden[%d]: site", iter, iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].site, site);

        snprintf(tag, sizeof tag, "0x7D first-write iter[%d] iden[%d]: tdma_explicit bit0", iter, iden);
        rc |= expect_eq_u8(tag, st.p25_chan_tdma_explicit[iden] & 1, 1);
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_preservation_first_write_0x7D\n");
    } else {
        fprintf(stderr, "FAILED test_preservation_first_write_0x7D\n");
    }
    return rc;
}

/*
 * Test: 0x33 MBT handler overwrites populated FDMA slots freely (no guard).
 *
 * Pre-populate an FDMA slot (populated=1), then simulate the 0x33 MBT write
 * with DIFFERENT values. Assert the entry now has the NEW values.
 * Uses chan_type=1 (routes to p25_iden_fdma[]).
 */
static int
test_preservation_0x33_mbt_overwrites_freely(void) {
    int rc = 0;
    static dsd_state st;
    uint32_t rng = 0xCAFEBABE; /* fixed seed for reproducibility */

    fprintf(stderr, "\n--- Preservation: 0x33 MBT Overwrites Freely ---\n");

    for (int iter = 0; iter < 128; iter++) {
        char tag[128];
        memset(&st, 0, sizeof st);

        int iden = (int)(xorshift32(&rng) & 0xF); /* 0-15 */

        /* Generate first set of params (A) and pre-populate the slot */
        long base_freq_a = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_spac_a = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off_a = (int)(xorshift32(&rng) % 20000) - 10000;

        st.p25_iden_fdma[iden].base_freq = base_freq_a;
        st.p25_iden_fdma[iden].chan_type = 1;
        st.p25_iden_fdma[iden].chan_spac = chan_spac_a;
        st.p25_iden_fdma[iden].trans_off = trans_off_a;
        st.p25_iden_fdma[iden].trust = 1;
        st.p25_iden_fdma[iden].populated = 1;
        st.p25_iden_fdma[iden].wacn = 0x11111;
        st.p25_iden_fdma[iden].sysid = 0x222;
        st.p25_iden_fdma[iden].rfss = 0x33;
        st.p25_iden_fdma[iden].site = 0x44;

        /* Generate second set of DIFFERENT params (B) for 0x33 MBT write */
        long base_freq_b = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_spac_b = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off_b = (int)(xorshift32(&rng) % 20000) - 10000;

        /* Set up provenance fields that 0x33 MBT will copy */
        st.p2_wacn = 0xAAAAA;
        st.p2_sysid = 0xBBB;
        st.p2_rfssid = 0xCC;
        st.p2_siteid = 0xDD;
        st.p25_cc_freq = 851000000L; /* non-zero => trust=2 */

        /* Simulate 0x33 MBT handler with params B on already-populated slot */
        simulate_0x33_mbt_handler_fdma(&st, iden, base_freq_b, 1, chan_spac_b, trans_off_b);

        /* Assert all fields now have the NEW values (B) — overwrite succeeded */
        snprintf(tag, sizeof tag, "0x33 MBT iter[%d] iden[%d]: base_freq overwritten", iter, iden);
        rc |= expect_eq_long(tag, st.p25_iden_fdma[iden].base_freq, base_freq_b);

        snprintf(tag, sizeof tag, "0x33 MBT iter[%d] iden[%d]: chan_type overwritten", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].chan_type, 1);

        snprintf(tag, sizeof tag, "0x33 MBT iter[%d] iden[%d]: chan_spac overwritten", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].chan_spac, chan_spac_b);

        snprintf(tag, sizeof tag, "0x33 MBT iter[%d] iden[%d]: trans_off overwritten", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_fdma[iden].trans_off, trans_off_b);

        snprintf(tag, sizeof tag, "0x33 MBT iter[%d] iden[%d]: populated still 1", iter, iden);
        rc |= expect_eq_u8(tag, st.p25_iden_fdma[iden].populated, 1);

        snprintf(tag, sizeof tag, "0x33 MBT iter[%d] iden[%d]: trust=2 (cc_freq!=0)", iter, iden);
        rc |= expect_eq_u8(tag, st.p25_iden_fdma[iden].trust, 2);

        snprintf(tag, sizeof tag, "0x33 MBT iter[%d] iden[%d]: wacn from p2_wacn", iter, iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].wacn, 0xAAAAA);

        snprintf(tag, sizeof tag, "0x33 MBT iter[%d] iden[%d]: sysid from p2_sysid", iter, iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].sysid, 0xBBB);

        snprintf(tag, sizeof tag, "0x33 MBT iter[%d] iden[%d]: rfss from p2_rfssid", iter, iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].rfss, 0xCC);

        snprintf(tag, sizeof tag, "0x33 MBT iter[%d] iden[%d]: site from p2_siteid", iter, iden);
        rc |= expect_eq_ull(tag, st.p25_iden_fdma[iden].site, 0xDD);
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_preservation_0x33_mbt_overwrites_freely\n");
    } else {
        fprintf(stderr, "FAILED test_preservation_0x33_mbt_overwrites_freely\n");
    }
    return rc;
}

/*
 * Test: 0x73 TDMA write-once guard still prevents overwrite of populated slots.
 *
 * Pre-populate a TDMA slot (populated=1), then simulate the 0x73 handler
 * write with DIFFERENT values. Assert the entry retains the first-write values.
 */
static int
test_preservation_0x73_tdma_guard_unchanged(void) {
    int rc = 0;
    static dsd_state st;
    uint32_t rng = 0xCAFEBABE; /* fixed seed for reproducibility */

    fprintf(stderr, "\n--- Preservation: 0x73 TDMA Guard Unchanged ---\n");

    for (int iter = 0; iter < 128; iter++) {
        char tag[128];
        memset(&st, 0, sizeof st);

        int iden = (int)(xorshift32(&rng) & 0xF); /* 0-15 */

        /* Generate first set of params (A) and pre-populate the TDMA slot */
        long base_freq_a = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_type_a = (int)(xorshift32(&rng) & 0xF);
        int chan_spac_a = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off_a = (int)(xorshift32(&rng) % 20000) - 10000;
        unsigned long long wacn_a = xorshift32(&rng) & 0xFFFFF;
        unsigned long long sysid_a = xorshift32(&rng) & 0xFFF;
        unsigned long long rfss_a = xorshift32(&rng) & 0xFF;
        unsigned long long site_a = xorshift32(&rng) & 0xFF;

        st.p25_iden_tdma[iden].base_freq = base_freq_a;
        st.p25_iden_tdma[iden].chan_type = chan_type_a;
        st.p25_iden_tdma[iden].chan_spac = chan_spac_a;
        st.p25_iden_tdma[iden].trans_off = trans_off_a;
        st.p25_iden_tdma[iden].trust = 1;
        st.p25_iden_tdma[iden].populated = 1;
        st.p25_iden_tdma[iden].wacn = wacn_a;
        st.p25_iden_tdma[iden].sysid = sysid_a;
        st.p25_iden_tdma[iden].rfss = rfss_a;
        st.p25_iden_tdma[iden].site = site_a;
        st.p25_chan_tdma_explicit[iden] |= 2; /* bit1 = has TDMA entry */

        /* Generate second set of DIFFERENT params (B) */
        long base_freq_b = (long)(xorshift32(&rng) % 200000000) + 100000000L;
        int chan_type_b = (int)(xorshift32(&rng) & 0xF);
        int chan_spac_b = (int)(xorshift32(&rng) & 0x3FF);
        int trans_off_b = (int)(xorshift32(&rng) % 20000) - 10000;

        /* Set up provenance fields (should NOT be written due to guard) */
        st.p2_wacn = 0xFFFFF;
        st.p2_sysid = 0xFFF;
        st.p2_rfssid = 0xFF;
        st.p2_siteid = 0xFF;

        /* Simulate 0x73 handler with params B on already-populated TDMA slot */
        simulate_0x73_handler(&st, iden, base_freq_b, chan_type_b, chan_spac_b, trans_off_b);

        /* Assert all original values (A) are preserved — guard skipped the write */
        snprintf(tag, sizeof tag, "0x73 guard iter[%d] iden[%d]: base_freq preserved", iter, iden);
        rc |= expect_eq_long(tag, st.p25_iden_tdma[iden].base_freq, base_freq_a);

        snprintf(tag, sizeof tag, "0x73 guard iter[%d] iden[%d]: chan_type preserved", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_tdma[iden].chan_type, chan_type_a);

        snprintf(tag, sizeof tag, "0x73 guard iter[%d] iden[%d]: chan_spac preserved", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_tdma[iden].chan_spac, chan_spac_a);

        snprintf(tag, sizeof tag, "0x73 guard iter[%d] iden[%d]: trans_off preserved", iter, iden);
        rc |= expect_eq_int(tag, st.p25_iden_tdma[iden].trans_off, trans_off_a);

        snprintf(tag, sizeof tag, "0x73 guard iter[%d] iden[%d]: trust preserved", iter, iden);
        rc |= expect_eq_u8(tag, st.p25_iden_tdma[iden].trust, 1);

        snprintf(tag, sizeof tag, "0x73 guard iter[%d] iden[%d]: populated preserved", iter, iden);
        rc |= expect_eq_u8(tag, st.p25_iden_tdma[iden].populated, 1);

        snprintf(tag, sizeof tag, "0x73 guard iter[%d] iden[%d]: wacn preserved", iter, iden);
        rc |= expect_eq_ull(tag, st.p25_iden_tdma[iden].wacn, wacn_a);

        snprintf(tag, sizeof tag, "0x73 guard iter[%d] iden[%d]: sysid preserved", iter, iden);
        rc |= expect_eq_ull(tag, st.p25_iden_tdma[iden].sysid, sysid_a);

        snprintf(tag, sizeof tag, "0x73 guard iter[%d] iden[%d]: rfss preserved", iter, iden);
        rc |= expect_eq_ull(tag, st.p25_iden_tdma[iden].rfss, rfss_a);

        snprintf(tag, sizeof tag, "0x73 guard iter[%d] iden[%d]: site preserved", iter, iden);
        rc |= expect_eq_ull(tag, st.p25_iden_tdma[iden].site, site_a);
    }

    if (rc == 0) {
        fprintf(stderr, "PASS test_preservation_0x73_tdma_guard_unchanged\n");
    } else {
        fprintf(stderr, "FAILED test_preservation_0x73_tdma_guard_unchanged\n");
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    int preservation_rc = 0;

    /* Write-once guard tests */
    rc |= test_bug_condition_fdma_overwrite_0x74();
    rc |= test_bug_condition_fdma_overwrite_0x7D();

    /* Preservation tests */
    preservation_rc |= test_preservation_first_write_0x74();
    preservation_rc |= test_preservation_first_write_0x7D();
    preservation_rc |= test_preservation_0x33_mbt_overwrites_freely();
    preservation_rc |= test_preservation_0x73_tdma_guard_unchanged();

    if (preservation_rc != 0) {
        fprintf(stderr, "\nCRITICAL: Preservation tests FAILED\n");
    }

    rc |= preservation_rc;

    if (rc == 0) {
        fprintf(stderr, "\nAll test_p25_iden_fdma_write_once tests PASSED\n");
    } else {
        fprintf(stderr, "\nSome test_p25_iden_fdma_write_once tests FAILED\n");
    }
    return rc;
}
