// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(misc-use-internal-linkage)
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
#include <unistd.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

struct RtlSdrContext;

uint64_t ConvertBitIntoBytes(const uint8_t* BufferIn, uint32_t BitLength);
void p25_lcw(dsd_opts* opts, dsd_state* state, uint8_t LCW_bits[], uint8_t irrecoverable_errors);

static int g_nmea_harris_calls;
static uint32_t g_nmea_harris_src;
static uint16_t g_nmea_harris_prefix;
static int g_apx_gps_calls;
static int g_apx_alias_header_calls;
static int g_apx_alias_block_calls;
static int g_l3h_alias_block_calls;
static int g_tait_alias_calls;
static uint8_t g_last_alias_slot;
static int16_t g_tait_alias_len;
static uint16_t g_last_alias_prefix;

/* ── Strong stubs (same set used by test_p25_lcw_call_term) ────────────── */

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return true;
}

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return true;
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
struct RtlSdrContext* g_rtl_ctx = 0;

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
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
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    g_apx_alias_header_calls++;
    g_last_alias_slot = slot;
    g_last_alias_prefix = (uint16_t)ConvertBitIntoBytes(lc_bits, 16);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    g_apx_alias_block_calls++;
    g_last_alias_slot = slot;
    g_last_alias_prefix = (uint16_t)ConvertBitIntoBytes(lc_bits, 16);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    g_l3h_alias_block_calls++;
    g_last_alias_slot = slot;
    g_last_alias_prefix = (uint16_t)ConvertBitIntoBytes(lc_bits, 16);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
tait_iso7_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    g_tait_alias_calls++;
    g_last_alias_slot = slot;
    g_tait_alias_len = len;
    g_last_alias_prefix = (uint16_t)ConvertBitIntoBytes(input, 16);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_gps(dsd_opts* opts, dsd_state* state, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    g_apx_gps_calls++;
    g_last_alias_prefix = (uint16_t)ConvertBitIntoBytes(lc_bits, 16);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    g_nmea_harris_calls++;
    g_nmea_harris_src = src;
    g_nmea_harris_prefix = (uint16_t)ConvertBitIntoBytes(input, 16);
    (void)slot;
}

/* ── Minimal ConvertBitIntoBytes (MSB-first) used by LCW ──────────────── */

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
ConvertBitIntoBytes(const uint8_t* BufferIn, uint32_t BitLength) {
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
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    DSD_FPRINTF(stderr, "PASS: %s\n", tag);
    return 0;
}

static int
expect_contains(const char* tag, const char* text, const char* needle) {
    if (!text || !needle || strstr(text, needle) == NULL) {
        DSD_FPRINTF(stderr, "FAIL: %s missing '%s' in '%s'\n", tag, needle ? needle : "(null)", text ? text : "(null)");
        return 1;
    }
    DSD_FPRINTF(stderr, "PASS: %s\n", tag);
    return 0;
}

static int
capture_lcw_output(dsd_opts* opts, dsd_state* st, uint8_t lcw[96], char* out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return -1;
    }
    out[0] = '\0';

    FILE* capture = tmpfile();
    if (!capture) {
        return -1;
    }

    (void)fflush(stderr);
    int saved = dup(fileno(stderr));
    if (saved < 0 || dup2(fileno(capture), fileno(stderr)) < 0) {
        if (saved >= 0) {
            (void)close(saved);
        }
        (void)fclose(capture);
        return -1;
    }

    p25_lcw(opts, st, lcw, /*irrecoverable_errors*/ 0);
    (void)fflush(stderr);
    int restored = dup2(saved, fileno(stderr));
    (void)close(saved);
    if (restored < 0) {
        (void)fclose(capture);
        return -1;
    }

    if (fseek(capture, 0, SEEK_SET) != 0) {
        (void)fclose(capture);
        return -1;
    }
    size_t n = fread(out, 1, out_sz - 1, capture);
    if (n < out_sz - 1 && ferror(capture) != 0) {
        (void)fclose(capture);
        return -1;
    }
    out[n] = '\0';
    (void)fclose(capture);
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
     * Public guard returns must leave state untouched and tolerate NULL inputs.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);
        st.lastsrc = 0x13579B;

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);

        p25_lcw(NULL, &st, lcw, /*irrecoverable_errors*/ 0);
        p25_lcw(&opts, NULL, lcw, /*irrecoverable_errors*/ 0);
        p25_lcw(&opts, &st, NULL, /*irrecoverable_errors*/ 0);

        rc |= expect_true("NullGuards_preserve_state", st.lastsrc == 0x13579B);
    }

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
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);
        st.lastsrc = 1234567;

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x00);  /* lc_format = 0x00 (Group Voice) */
        set_bits_msb(lcw, 8, 8, 0x00);  /* lc_mfid   = 0x00 (standard)   */
        set_bits_msb(lcw, 16, 8, 0x00); /* lc_svcopt = 0x00              */
        set_bits_msb(lcw, 32, 16, 100); /* group     = 100               */
        set_bits_msb(lcw, 48, 24, 0);   /* source    = 0  (bug trigger)  */

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("Fmt0x00_src0_preserves_lastsrc: lastsrc was 1234567, source=0 should NOT overwrite",
                          st.lastsrc == 1234567);
        if (st.lastsrc != 1234567) {
            DSD_FPRINTF(stderr,
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
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);
        st.lastsrc = 102;

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x03);  /* lc_format = 0x03 (Unit-to-Unit) */
        set_bits_msb(lcw, 8, 8, 0x00);  /* lc_mfid   = 0x00               */
        set_bits_msb(lcw, 16, 8, 0x00); /* lc_svcopt = 0x00               */
        set_bits_msb(lcw, 24, 24, 200); /* target    = 200                */
        set_bits_msb(lcw, 48, 24, 0);   /* source    = 0  (bug trigger)   */

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("Fmt0x03_src0_preserves_lastsrc: lastsrc was 102, source=0 should NOT overwrite",
                          st.lastsrc == 102);
        if (st.lastsrc != 102) {
            DSD_FPRINTF(stderr,
                        "  counterexample: Format 0x03: lastsrc was 102, after LCW with source=0 it became %ld\n",
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
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);
        st.lastsrc = 54321;

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x00);  /* lc_format = 0x00 (PB=0,SF=0,LCO=0x00) */
        set_bits_msb(lcw, 8, 8, 0x90);  /* lc_mfid   = 0x90 (Motorola)            */
        set_bits_msb(lcw, 16, 8, 0x00); /* lc_svcopt = 0x00                        */
        set_bits_msb(lcw, 32, 16, 300); /* sg        = 300                         */
        set_bits_msb(lcw, 48, 24, 0);   /* src       = 0  (bug trigger)            */

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("MFID90_Fmt0x00_src0_preserves_lastsrc: lastsrc was 54321, src=0 should NOT overwrite",
                          st.lastsrc == 54321);
        if (st.lastsrc != 54321) {
            DSD_FPRINTF(stderr,
                        "  counterexample: MFID90 0x00: lastsrc was 54321, after LCW with src=0 it became %ld\n",
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
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);
        st.lastsrc = 0;

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x00);  /* lc_format = 0x00 (Group Voice) */
        set_bits_msb(lcw, 8, 8, 0x00);  /* lc_mfid   = 0x00              */
        set_bits_msb(lcw, 16, 8, 0x00); /* lc_svcopt = 0x00              */
        set_bits_msb(lcw, 32, 16, 100); /* group     = 100               */
        set_bits_msb(lcw, 48, 24, 0);   /* source    = 0                 */

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("EdgeCase_src0_lastsrc0_stays_zero: no non-zero value destroyed", st.lastsrc == 0);
    }

    /*
     * Source ID Extension (0x49)
     * sdrtrunk parses this as WACN[16..35], SYSTEM[36..47], RADIO[48..71].
     * The radio ID must not be read starting at bit 40.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);
        st.lastsrc = 111;

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x49);
        set_bits_msb(lcw, 8, 8, 0x00);
        set_bits_msb(lcw, 16, 20, 0xABCDE);
        set_bits_msb(lcw, 36, 12, 0x123);
        set_bits_msb(lcw, 48, 24, 0x456789);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("SrcIdExtension_WACN_20bit", st.p25_src_nid == 0xABCDE);
        rc |= expect_true("SrcIdExtension_radio_24bit_at_bit48", st.lastsrc == 0x456789);
    }

    /*
     * Tait MFID 0xD8 format 0x01 uses the same fully-qualified source layout:
     * WACN[16..35], SYSTEM[36..47], RADIO[48..71].
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);
        st.lastsrc = 222;

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x01);
        set_bits_msb(lcw, 8, 8, 0xD8);
        set_bits_msb(lcw, 16, 20, 0x54321);
        set_bits_msb(lcw, 36, 12, 0x987);
        set_bits_msb(lcw, 48, 24, 0x2468AC);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("TaitD8Fmt01_WACN_20bit", st.p25_src_nid == 0x54321);
        rc |= expect_true("TaitD8Fmt01_radio_24bit_at_bit48", st.lastsrc == 0x2468AC);
    }

    /*
     * Format 0x00 service options drive the user-visible call string prefix.
     * Emergency takes priority over the encrypted bit when both are present.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);
        opts.payload = 1;

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x00);
        set_bits_msb(lcw, 8, 8, 0x00);
        set_bits_msb(lcw, 16, 8, 0xFF);
        set_bits_msb(lcw, 32, 16, 0x1234);
        set_bits_msb(lcw, 48, 24, 0x456789);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("Fmt0x00_emergency_prefix", strstr(st.call_string[0], "Emergency") != NULL);
        rc |= expect_true("Fmt0x00_emergency_group_state", st.gi[0] == 0 && st.lasttg == 0x1234);
    }

    /*
     * Format 0x03 should mark an individual/private call and preserve the encrypted
     * service option in the call string.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x03);
        set_bits_msb(lcw, 8, 8, 0x00);
        set_bits_msb(lcw, 16, 8, 0x40);
        set_bits_msb(lcw, 24, 24, 0x010203);
        set_bits_msb(lcw, 48, 24, 0x040506);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("Fmt0x03_encrypted_private_prefix", strstr(st.call_string[0], "Encrypted") != NULL);
        rc |= expect_true("Fmt0x03_private_state", st.gi[0] == 1 && st.lasttg == 0x010203 && st.lastsrc == 0x040506);
    }

    /*
     * Format 0x42 carries two implicit group voice channel updates. When the two
     * groups differ, both active-channel slots should be refreshed.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x42);
        set_bits_msb(lcw, 8, 16, 0x1111);
        set_bits_msb(lcw, 24, 16, 0x2222);
        set_bits_msb(lcw, 40, 16, 0x3333);
        set_bits_msb(lcw, 56, 16, 0x4444);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("Fmt0x42_first_active_channel", strstr(st.active_channel[0], "1111") != NULL);
        rc |= expect_true("Fmt0x42_second_active_channel", strstr(st.active_channel[1], "3333") != NULL);
    }

    /*
     * Format 0x4A is an extended unit-to-unit voice channel user. It does not set
     * source/target state today, but it must mark the call as individual.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x4A);
        set_bits_msb(lcw, 8, 8, 0x00);
        set_bits_msb(lcw, 16, 24, 0x101112);
        set_bits_msb(lcw, 40, 24, 0x202122);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("Fmt0x4A_sets_individual_flag", st.gi[0] == 1);
    }

    /*
     * Format 0x50 updates affiliation/query context when both group and source
     * are present.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x50);
        set_bits_msb(lcw, 8, 8, 0x00);
        set_bits_msb(lcw, 32, 16, 0x3456);
        set_bits_msb(lcw, 48, 24, 0x654321);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("Fmt0x50_group_affiliation_target", st.lasttg == 0x3456);
        rc |= expect_true("Fmt0x50_group_affiliation_source", st.lastsrc == 0x654321);
    }

    /*
     * Exercise the known protection ALGID map, including values that previously
     * only appeared in display text. The persistent state must still record the
     * exact ALGID/KID payload.
     */
    {
        const uint8_t algids[] = {0x81, 0x82, 0x83, 0x84, 0x85, 0x88, 0x89, 0x9F, 0xAA, 0xAF};
        for (size_t i = 0; i < sizeof(algids) / sizeof(algids[0]); i++) {
            DSD_MEMSET(&opts, 0, sizeof opts);
            DSD_MEMSET(&st, 0, sizeof st);

            uint8_t lcw[96];
            DSD_MEMSET(lcw, 0, sizeof lcw);
            set_bits_msb(lcw, 0, 8, 0x65);
            set_bits_msb(lcw, 8, 8, 0x00);
            set_bits_msb(lcw, 24, 8, algids[i]);
            set_bits_msb(lcw, 32, 16, (uint32_t)(0x1200U + i));
            set_bits_msb(lcw, 48, 24, 0x010203);

            p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

            rc |= expect_true("Fmt0x65_known_algid_valid", st.p25_prot_valid == 1);
            rc |= expect_true("Fmt0x65_known_algid_value", st.p25_prot_algid == algids[i]);
            rc |= expect_true("Fmt0x65_known_algid_kid", st.p25_prot_kid == (int)(0x1200U + i));
        }
    }

    /*
     * Unknown ALGIDs do not have a printable name, but the raw protection
     * parameters are still meaningful decoder state.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x65);
        set_bits_msb(lcw, 8, 8, 0x00);
        set_bits_msb(lcw, 24, 8, 0x01);
        set_bits_msb(lcw, 32, 16, 0xBEEF);
        set_bits_msb(lcw, 48, 24, 0x010203);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("Fmt0x65_unknown_algid_valid", st.p25_prot_valid == 1);
        rc |= expect_true("Fmt0x65_unknown_algid_value", st.p25_prot_algid == 0x01);
        rc |= expect_true("Fmt0x65_unknown_algid_kid", st.p25_prot_kid == 0xBEEF);
    }

    /*
     * MFID90 regroup channel users update both current call state and the
     * patch table. Set the rendering flag bits too so the parser keeps their
     * positions distinct from SG/SRC fields.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x00);
        set_bits_msb(lcw, 8, 8, 0x90);
        lcw[16] = 1;
        lcw[17] = 1;
        lcw[31] = 1;
        set_bits_msb(lcw, 32, 16, 0x345);
        set_bits_msb(lcw, 48, 24, 0x6789AB);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("MFID90_regroup_lasttg", st.lasttg == 0x345);
        rc |= expect_true("MFID90_regroup_lastsrc", st.lastsrc == 0x6789AB);
        rc |= expect_true("MFID90_regroup_group_call", st.gi[0] == 0);
        rc |= expect_true("MFID90_regroup_patch_count", st.p25_patch_count == 1);
        rc |= expect_true("MFID90_regroup_patch_sgid", st.p25_patch_sgid[0] == 0x345);
        rc |= expect_true("MFID90_regroup_patch_active", st.p25_patch_is_patch[0] == 1 && st.p25_patch_active[0] == 1);
    }

    /*
     * MFID90 regroup channel updates do not currently retain SG/channel state,
     * but they must continue to mark the call as a group call.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);
        st.gi[0] = 1;

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x01);
        set_bits_msb(lcw, 8, 8, 0x90);
        lcw[16] = 1;
        lcw[17] = 1;
        set_bits_msb(lcw, 24, 16, 0x456);
        set_bits_msb(lcw, 56, 16, 0x1234);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("MFID90_regroup_update_group_call", st.gi[0] == 0);
    }

    /*
     * Additional Motorola vendor LCWs are metadata/display-only; they should be
     * recognized without changing call or patch state.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);
        st.lasttg = 0xAAAA;
        st.lastsrc = 0xBBBBCC;
        char out[2048];

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x02);
        set_bits_msb(lcw, 8, 8, 0x90);
        set_bits_msb(lcw, 16, 8, 0xAB);
        if (capture_lcw_output(&opts, &st, lcw, out, sizeof(out)) != 0) {
            return 1;
        }
        rc |= expect_contains("MFID90_failsoft_label", out, "MFID90 (Moto) Failsoft");
        rc |= expect_true("MFID90_failsoft_no_state", st.lasttg == 0xAAAA && st.lastsrc == 0xBBBBCC);

        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x0A);
        set_bits_msb(lcw, 8, 8, 0x90);
        set_bits_msb(lcw, 32, 16, 0x1234);
        set_bits_msb(lcw, 48, 24, 0x456789);
        if (capture_lcw_output(&opts, &st, lcw, out, sizeof(out)) != 0) {
            return 1;
        }
        rc |= expect_contains("MFID90_emergency_label", out, "Emergency Alarm Activation");
        rc |= expect_contains("MFID90_emergency_group", out, "Group: 4660");
        rc |= expect_contains("MFID90_emergency_source", out, "Source: 4548489");
        rc |= expect_true("MFID90_emergency_no_patch", st.p25_patch_count == 0);
    }

    /*
     * Harris GPS is assembled from a block-1 prefix plus block-2 continuation.
     * A valid prefix dispatches to the GPS decoder stub and then clears the
     * scratch buffer; a missing block 1 only clears scratch state.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);
        g_nmea_harris_calls = 0;
        g_nmea_harris_src = 0;
        g_nmea_harris_prefix = 0;
        st.lastsrc = 0x123456;

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x2A);
        set_bits_msb(lcw, 8, 8, 0xA4);
        set_bits_msb(lcw, 16, 8, 0x5A);
        set_bits_msb(lcw, 40, 8, 0xC3);
        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);
        rc |= expect_true("HarrisGPS_block1_prefix", (uint16_t)ConvertBitIntoBytes(st.dmr_pdu_sf[0], 16) == 0x2AA4);
        rc |= expect_true("HarrisGPS_block1_payload_copy", st.dmr_pdu_sf[0][40] == 0 && st.dmr_pdu_sf[0][41] == 1);

        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x2B);
        set_bits_msb(lcw, 8, 8, 0xA4);
        set_bits_msb(lcw, 16, 8, 0x66);
        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);
        rc |= expect_true("HarrisGPS_valid_dispatch", g_nmea_harris_calls == 1);
        rc |= expect_true("HarrisGPS_dispatch_src", g_nmea_harris_src == 0x123456);
        rc |= expect_true("HarrisGPS_dispatch_prefix", g_nmea_harris_prefix == 0x2AA4);
        rc |= expect_true("HarrisGPS_clears_scratch", st.dmr_pdu_sf[0][0] == 0 && st.dmr_pdu_sf[0][40] == 0);

        DSD_MEMSET(st.dmr_pdu_sf[0], 1, sizeof(st.dmr_pdu_sf[0]));
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x2B);
        set_bits_msb(lcw, 8, 8, 0xA4);
        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);
        rc |= expect_true("HarrisGPS_missing_block1_no_dispatch", g_nmea_harris_calls == 1);
        rc |= expect_true("HarrisGPS_missing_block1_clears_scratch", st.dmr_pdu_sf[0][0] == 0);
    }

    /*
     * Vendor LCW alias/GPS opcodes should delegate to their protocol-specific
     * decoders with the raw LCW bits and voice slot 0, rather than only printing
     * static labels.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);
        g_apx_gps_calls = 0;
        g_apx_alias_header_calls = 0;
        g_apx_alias_block_calls = 0;
        g_l3h_alias_block_calls = 0;
        g_tait_alias_calls = 0;
        g_last_alias_slot = 0xFF;
        g_tait_alias_len = 0;
        g_last_alias_prefix = 0;

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x06);
        set_bits_msb(lcw, 8, 8, 0x90);
        set_bits_msb(lcw, 16, 8, 0xA1);
        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);
        rc |= expect_true("MFID90_GPS_dispatch", g_apx_gps_calls == 1);
        rc |= expect_true("MFID90_GPS_prefix", g_last_alias_prefix == 0x0690);

        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x15);
        set_bits_msb(lcw, 8, 8, 0x90);
        set_bits_msb(lcw, 16, 8, 0xB2);
        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);
        rc |= expect_true("MFID90_alias_header_dispatch", g_apx_alias_header_calls == 1);
        rc |= expect_true("MFID90_alias_header_slot", g_last_alias_slot == 0);
        rc |= expect_true("MFID90_alias_header_prefix", g_last_alias_prefix == 0x1590);

        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x17);
        set_bits_msb(lcw, 8, 8, 0x90);
        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);
        rc |= expect_true("MFID90_alias_block_dispatch", g_apx_alias_block_calls == 1);
        rc |= expect_true("MFID90_alias_block_slot", g_last_alias_slot == 0);

        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x32);
        set_bits_msb(lcw, 8, 8, 0xA4);
        set_bits_msb(lcw, 16, 8, 0xC3);
        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);
        rc |= expect_true("MFIDA4_alias_block_dispatch", g_l3h_alias_block_calls == 1);
        rc |= expect_true("MFIDA4_alias_block_slot", g_last_alias_slot == 0);
        rc |= expect_true("MFIDA4_alias_block_prefix", g_last_alias_prefix == 0x32A4);

        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x00);
        set_bits_msb(lcw, 8, 8, 0xD8);
        set_bits_msb(lcw, 16, 8, 0xD4);
        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);
        rc |= expect_true("MFIDD8_tait_alias_dispatch", g_tait_alias_calls == 1);
        rc |= expect_true("MFIDD8_tait_alias_slot", g_last_alias_slot == 0);
        rc |= expect_true("MFIDD8_tait_alias_len", g_tait_alias_len == 8);
        rc |= expect_true("MFIDD8_tait_alias_prefix", g_last_alias_prefix == 0x00D8);
    }

    /*
     * Unknown and protected LCWs are tolerated as no-op control messages: they
     * must not rewrite remembered call state or invoke vendor payload decoders.
     */
    {
        static const struct {
            const char* name;
            uint8_t format;
            uint8_t mfid;
        } unknown_cases[] = {
            {"UnknownStandard_no_state_mutation", 0x30, 0x00}, {"UnknownMoto_no_state_mutation", 0x30, 0x90},
            {"UnknownHarris_no_state_mutation", 0x30, 0xA4},   {"UnknownTait_no_state_mutation", 0x30, 0xD8},
            {"UnknownVendor_no_state_mutation", 0x30, 0x7F},
        };

        for (size_t i = 0; i < sizeof(unknown_cases) / sizeof(unknown_cases[0]); i++) {
            DSD_MEMSET(&opts, 0, sizeof opts);
            DSD_MEMSET(&st, 0, sizeof st);
            st.lastsrc = 0x123456;
            st.lasttg = 0x3456;
            st.gi[0] = 1;
            st.p25_prot_valid = 1;
            st.p25_prot_algid = 0x84;
            st.p25_prot_kid = 0x4321;
            g_apx_gps_calls = 0;
            g_apx_alias_header_calls = 0;
            g_apx_alias_block_calls = 0;
            g_l3h_alias_block_calls = 0;
            g_tait_alias_calls = 0;

            uint8_t lcw[96];
            DSD_MEMSET(lcw, 0, sizeof lcw);
            set_bits_msb(lcw, 0, 8, unknown_cases[i].format);
            set_bits_msb(lcw, 8, 8, unknown_cases[i].mfid);
            set_bits_msb(lcw, 16, 8, 0xFF);
            set_bits_msb(lcw, 48, 24, 0x654321);

            p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

            rc |= expect_true(unknown_cases[i].name,
                              st.lastsrc == 0x123456 && st.lasttg == 0x3456 && st.gi[0] == 1 && st.p25_prot_valid == 1
                                  && st.p25_prot_algid == 0x84 && st.p25_prot_kid == 0x4321 && g_apx_gps_calls == 0
                                  && g_apx_alias_header_calls == 0 && g_apx_alias_block_calls == 0
                                  && g_l3h_alias_block_calls == 0 && g_tait_alias_calls == 0);
        }
    }

    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);
        st.lastsrc = 0x234567;
        st.lasttg = 0x4567;
        st.gi[0] = 1;
        st.p25_prot_valid = 0;

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x80);
        set_bits_msb(lcw, 8, 8, 0x00);
        set_bits_msb(lcw, 16, 8, 0xFF);
        set_bits_msb(lcw, 32, 16, 0x1234);
        set_bits_msb(lcw, 48, 24, 0x654321);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("ProtectedLCW_no_state_mutation",
                          st.lastsrc == 0x234567 && st.lasttg == 0x4567 && st.gi[0] == 1 && st.p25_prot_valid == 0);
    }

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
// NOLINTEND(misc-use-internal-linkage)
