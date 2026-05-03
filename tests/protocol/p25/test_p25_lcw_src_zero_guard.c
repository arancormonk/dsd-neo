// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 LCW source-zero guard tests.
 *
 * Bug condition: p25_lcw() unconditionally writes state->lastsrc = source
 * for formats 0x00 (Group Voice), 0x03 (Unit-to-Unit), and MFID90 0x00
 * (Motorola Group Regroup).  When the decoded source field is zero, this
 * destroys a previously stored non-zero source ID.
 *
 * Each test case sets state->lastsrc to a known non-zero value, feeds an
 * LCW with source=0, and asserts that lastsrc is preserved.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

struct RtlSdrContext;

void p25_lcw(dsd_opts* opts, dsd_state* state, uint8_t LCW_bits[], uint8_t irrecoverable_errors);

/* ── Strong stubs (same set used by test_p25_lcw_call_term) ────────────── */

bool
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return true;
}

bool
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return true;
}

struct RtlSdrContext* g_rtl_ctx = 0;

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    if (opts) {
        opts->p25_is_tuned = 0;
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
    }
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.return_to_cc = return_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
}

/* Alias / GPS stubs — not exercised by the formats under test */

void
apx_embedded_alias_header_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
apx_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
l3h_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
tait_iso7_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
apx_embedded_gps(dsd_opts* opts, dsd_state* state, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)lc_bits;
}

void
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

/* ── Minimal ConvertBitIntoBytes (MSB-first) used by LCW ──────────────── */

uint64_t
ConvertBitIntoBytes(uint8_t* BufferIn, uint32_t BitLength) {
    uint64_t out = 0;
    for (uint32_t i = 0; i < BitLength; i++) {
        out = (out << 1) | (uint64_t)(BufferIn[i] & 1);
    }
    return out;
}

/* ── Bit-packing helper ───────────────────────────────────────────────── */

static void
set_bits_msb(uint8_t* b, int off, int n, uint32_t v) {
    for (int i = 0; i < n; i++) {
        int bit = (v >> (n - 1 - i)) & 1;
        b[off + i] = (uint8_t)bit;
    }
}

/* ── Test assertion helper ────────────────────────────────────────────── */

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    fprintf(stderr, "PASS: %s\n", tag);
    return 0;
}

/* ── Test cases ───────────────────────────────────────────────────────── */

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    install_trunk_tuning_hooks();

    /*
     * Test case 1 — Format 0x00 (Group Voice Channel User)
     * Pre-condition: state->lastsrc = 1234567 (known-good source ID)
     * Input:         LCW with format=0x00, group=100, source=0
     * Expected:      state->lastsrc remains 1234567
     *
     * Without the guard, the unconditional `state->lastsrc = source`
     * would write zero.
     */
    {
        memset(&opts, 0, sizeof opts);
        memset(&st, 0, sizeof st);
        st.lastsrc = 1234567;

        uint8_t lcw[96];
        memset(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x00);  /* lc_format = 0x00 (Group Voice) */
        set_bits_msb(lcw, 8, 8, 0x00);  /* lc_mfid   = 0x00 (standard)   */
        set_bits_msb(lcw, 16, 8, 0x00); /* lc_svcopt = 0x00              */
        set_bits_msb(lcw, 32, 16, 100); /* group     = 100               */
        set_bits_msb(lcw, 48, 24, 0);   /* source    = 0  (bug trigger)  */

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("Fmt0x00_src0_preserves_lastsrc: lastsrc was 1234567, source=0 should NOT overwrite",
                          st.lastsrc == 1234567);
        if (st.lastsrc != 1234567) {
            fprintf(stderr,
                    "  counterexample: Format 0x00: lastsrc was 1234567, after LCW with source=0 it became %ld\n",
                    (long)st.lastsrc);
        }
    }

    /*
     * Test case 2 — Format 0x03 (Unit-to-Unit Voice Channel User)
     * Pre-condition: state->lastsrc = 102
     * Input:         LCW with format=0x03, target=200, source=0
     * Expected:      state->lastsrc remains 102
     */
    {
        memset(&opts, 0, sizeof opts);
        memset(&st, 0, sizeof st);
        st.lastsrc = 102;

        uint8_t lcw[96];
        memset(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x03);  /* lc_format = 0x03 (Unit-to-Unit) */
        set_bits_msb(lcw, 8, 8, 0x00);  /* lc_mfid   = 0x00               */
        set_bits_msb(lcw, 16, 8, 0x00); /* lc_svcopt = 0x00               */
        set_bits_msb(lcw, 24, 24, 200); /* target    = 200                */
        set_bits_msb(lcw, 48, 24, 0);   /* source    = 0  (bug trigger)   */

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("Fmt0x03_src0_preserves_lastsrc: lastsrc was 102, source=0 should NOT overwrite",
                          st.lastsrc == 102);
        if (st.lastsrc != 102) {
            fprintf(stderr, "  counterexample: Format 0x03: lastsrc was 102, after LCW with source=0 it became %ld\n",
                    (long)st.lastsrc);
        }
    }

    /*
     * Test case 3 — MFID90 format 0x00 (Motorola Group Regroup Channel User)
     * Pre-condition: state->lastsrc = 54321
     * Input:         LCW with format=0x00, MFID=0x90, sg=300, src=0
     * Expected:      state->lastsrc remains 54321
     *
     * Note: MFID90 0x00 means lc_format byte = 0x00 with SF=0, so
     * lc_mfid = 0x90 and lc_opcode = 0x00.  The is_standard_mfid check
     * fails (0x90 != 0/1 and SF=0), routing to the MFID90 branch.
     */
    {
        memset(&opts, 0, sizeof opts);
        memset(&st, 0, sizeof st);
        st.lastsrc = 54321;

        uint8_t lcw[96];
        memset(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x00);  /* lc_format = 0x00 (PB=0,SF=0,LCO=0x00) */
        set_bits_msb(lcw, 8, 8, 0x90);  /* lc_mfid   = 0x90 (Motorola)            */
        set_bits_msb(lcw, 16, 8, 0x00); /* lc_svcopt = 0x00                        */
        set_bits_msb(lcw, 32, 16, 300); /* sg        = 300                         */
        set_bits_msb(lcw, 48, 24, 0);   /* src       = 0  (bug trigger)            */

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("MFID90_Fmt0x00_src0_preserves_lastsrc: lastsrc was 54321, src=0 should NOT overwrite",
                          st.lastsrc == 54321);
        if (st.lastsrc != 54321) {
            fprintf(stderr, "  counterexample: MFID90 0x00: lastsrc was 54321, after LCW with src=0 it became %ld\n",
                    (long)st.lastsrc);
        }
    }

    /*
     * Edge case — source=0 when lastsrc is already 0
     * Pre-condition: state->lastsrc = 0
     * Input:         LCW with format=0x00, group=100, source=0
     * Expected:      state->lastsrc remains 0
     *
     * This is NOT a bug condition — no non-zero value is destroyed.
     * Should PASS — lastsrc is already 0, so the guard is a no-op.
     */
    {
        memset(&opts, 0, sizeof opts);
        memset(&st, 0, sizeof st);
        st.lastsrc = 0;

        uint8_t lcw[96];
        memset(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x00);  /* lc_format = 0x00 (Group Voice) */
        set_bits_msb(lcw, 8, 8, 0x00);  /* lc_mfid   = 0x00              */
        set_bits_msb(lcw, 16, 8, 0x00); /* lc_svcopt = 0x00              */
        set_bits_msb(lcw, 32, 16, 100); /* group     = 100               */
        set_bits_msb(lcw, 48, 24, 0);   /* source    = 0                 */

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("EdgeCase_src0_lastsrc0_stays_zero: no non-zero value destroyed", st.lastsrc == 0);
    }

    return rc;
}
