// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 LCW Channel Identifier Update tests.
 *
 * SDRTrunk's LCChannelIdentifierUpdate and LCChannelIdentifierUpdateVU classes
 * expose complete FDMA frequency-band data: iden, spacing, base frequency, and
 * transmit offset. These tests verify dsd-neo stores those LCW updates as full
 * FDMA IDEN entries rather than base-only provisional entries.
 */

#include <assert.h>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/file_compat.h"
#include "p25_iden_test_vectors.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
tait_iso7_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_gps(dsd_opts* opts, dsd_state* state, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

// Alias decode helper stubs referenced by VPDU handler
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL %s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_long(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL %s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_u8(const char* tag, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL %s: got %u want %u\n", tag, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

static void
put_bits(uint8_t* bits, int start, int width, unsigned long long value) {
    for (int i = 0; i < width; i++) {
        int shift = width - 1 - i;
        bits[start + i] = (uint8_t)((value >> shift) & 1ULL);
    }
}

static void
build_lcw_standard(uint8_t* bits, int iden, int bandwidth, int sign, int tx_raw, int spacing, unsigned long base) {
    DSD_MEMSET(bits, 0, 96);
    put_bits(bits, 0, 8, 0x58);
    put_bits(bits, 8, 4, (unsigned)iden);
    put_bits(bits, 12, 9, (unsigned)bandwidth);
    bits[21] = (uint8_t)(sign & 1);
    put_bits(bits, 22, 8, (unsigned)tx_raw);
    put_bits(bits, 30, 10, (unsigned)spacing);
    put_bits(bits, 40, 32, base);
}

static void
build_lcw_vuhf(uint8_t* bits, int iden, int bw_vu, int sign, int tx_raw, int spacing, unsigned long base) {
    DSD_MEMSET(bits, 0, 96);
    put_bits(bits, 0, 8, 0x59);
    put_bits(bits, 8, 4, (unsigned)iden);
    put_bits(bits, 12, 4, (unsigned)bw_vu);
    bits[16] = (uint8_t)(sign & 1);
    put_bits(bits, 17, 13, (unsigned)tx_raw);
    put_bits(bits, 30, 10, (unsigned)spacing);
    put_bits(bits, 40, 32, base);
}

static int
test_lcw_standard_populates_fdma(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    uint8_t bits[96];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    int iden = 2;
    long base = 850000000L / 5L;
    build_lcw_standard(bits, iden, 0x55, 0, 7, 50, (unsigned long)base);

    p25_lcw(&opts, &st, bits, 0);

    rc |= expect_eq_u8("standard populated", st.p25_iden_fdma[iden].populated, 1);
    rc |= expect_eq_long("standard base", st.p25_iden_fdma[iden].base_freq, base);
    rc |= expect_eq_int("standard type", st.p25_iden_fdma[iden].chan_type, 1);
    rc |= expect_eq_int("standard spacing", st.p25_iden_fdma[iden].chan_spac, 50);
    rc |= expect_eq_int("standard offset", st.p25_iden_fdma[iden].trans_off, -7);
    rc |= expect_eq_u8("standard bw_vu", st.p25_iden_fdma[iden].bw_vu, 0);
    rc |= expect_eq_u8("standard bitmask", st.p25_chan_tdma_explicit[iden], 1);

    int chan = (iden << 12) | 5;
    long freq = process_channel_to_freq(&opts, &st, chan);
    long want = 850000000L + (5L * 50 * 125);
    rc |= expect_eq_long("standard frequency", freq, want);

    if (rc == 0) {
        DSD_FPRINTF(stderr, "PASS test_lcw_standard_populates_fdma\n");
    }
    return rc;
}

static int
test_lcw_vuhf_populates_fdma(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    uint8_t bits[96];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    int iden = 3;
    long base = 450000000L / 5L;
    build_lcw_vuhf(bits, iden, 5, 1, 123, 100, (unsigned long)base);

    p25_lcw(&opts, &st, bits, 0);

    rc |= expect_eq_u8("vuhf populated", st.p25_iden_fdma[iden].populated, 1);
    rc |= expect_eq_long("vuhf base", st.p25_iden_fdma[iden].base_freq, base);
    rc |= expect_eq_int("vuhf type", st.p25_iden_fdma[iden].chan_type, 1);
    rc |= expect_eq_int("vuhf spacing", st.p25_iden_fdma[iden].chan_spac, 100);
    rc |= expect_eq_int("vuhf offset", st.p25_iden_fdma[iden].trans_off, 123);
    rc |= expect_eq_u8("vuhf bw", st.p25_iden_fdma[iden].bw_vu, 5);
    rc |= expect_eq_u8("vuhf bitmask", st.p25_chan_tdma_explicit[iden], 1);

    int chan = (iden << 12) | 4;
    long freq = process_channel_to_freq(&opts, &st, chan);
    long want = 450000000L + (4L * 100 * 125);
    rc |= expect_eq_long("vuhf frequency", freq, want);

    if (rc == 0) {
        DSD_FPRINTF(stderr, "PASS test_lcw_vuhf_populates_fdma\n");
    }
    return rc;
}

static int
test_lcw_control_channel_provenance_promotes_trust(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    uint8_t bits[96];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    opts.trunk_is_tuned = 0;
    st.p25_cc_freq = 851000000;
    st.p2_wacn = 0xABCDE;
    st.p2_sysid = 0x123;
    st.p2_rfssid = 0x45;
    st.p2_siteid = 0x67;

    int iden = 5;
    long base = 851000000L / 5L;
    build_lcw_standard(bits, iden, 0x24, 1, 12, 100, (unsigned long)base);

    p25_lcw(&opts, &st, bits, 0);

    rc |= expect_eq_u8("cc trust promoted", st.p25_iden_fdma[iden].trust, 2);
    rc |= expect_eq_long("cc provenance wacn", (long)st.p25_iden_fdma[iden].wacn, 0xABCDE);
    rc |= expect_eq_long("cc provenance sysid", (long)st.p25_iden_fdma[iden].sysid, 0x123);
    rc |= expect_eq_long("cc provenance rfss", (long)st.p25_iden_fdma[iden].rfss, 0x45);
    rc |= expect_eq_long("cc provenance site", (long)st.p25_iden_fdma[iden].site, 0x67);
    rc |= expect_eq_int("cc positive offset", st.p25_iden_fdma[iden].trans_off, 12);

    if (rc == 0) {
        DSD_FPRINTF(stderr, "PASS test_lcw_control_channel_provenance_promotes_trust\n");
    }
    return rc;
}

static int
test_lcw_rejects_incomplete_fdma_updates(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    uint8_t bits[96];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    int iden = 6;
    st.p25_iden_fdma[iden].base_freq = 12345678L;
    st.p25_iden_fdma[iden].chan_type = 1;
    st.p25_iden_fdma[iden].chan_spac = 25;
    st.p25_iden_fdma[iden].trans_off = -3;
    st.p25_iden_fdma[iden].bw_vu = 2;
    st.p25_iden_fdma[iden].trust = 2;
    st.p25_iden_fdma[iden].populated = 1;
    st.p25_chan_tdma_explicit[iden] = 3;
    st.trunk_chan_map[(iden << 12) | 0x0010] = 987654321L;

    build_lcw_standard(bits, iden, 0x55, 0, 7, 0, 850000000UL / 5UL);
    p25_lcw(&opts, &st, bits, 0);

    rc |= expect_eq_long("zero spacing preserves base", st.p25_iden_fdma[iden].base_freq, 12345678L);
    rc |= expect_eq_int("zero spacing preserves spacing", st.p25_iden_fdma[iden].chan_spac, 25);
    rc |= expect_eq_int("zero spacing preserves offset", st.p25_iden_fdma[iden].trans_off, -3);
    rc |= expect_eq_u8("zero spacing preserves bw", st.p25_iden_fdma[iden].bw_vu, 2);
    rc |= expect_eq_u8("zero spacing preserves bitmask", st.p25_chan_tdma_explicit[iden], 3);
    rc |= expect_eq_long("zero spacing preserves cache", st.trunk_chan_map[(iden << 12) | 0x0010], 987654321L);

    build_lcw_vuhf(bits, iden, 4, 1, 88, 100, 0);
    p25_lcw(&opts, &st, bits, 0);

    rc |= expect_eq_long("zero base preserves base", st.p25_iden_fdma[iden].base_freq, 12345678L);
    rc |= expect_eq_int("zero base preserves spacing", st.p25_iden_fdma[iden].chan_spac, 25);
    rc |= expect_eq_int("zero base preserves offset", st.p25_iden_fdma[iden].trans_off, -3);
    rc |= expect_eq_u8("zero base preserves bw", st.p25_iden_fdma[iden].bw_vu, 2);
    rc |= expect_eq_u8("zero base preserves bitmask", st.p25_chan_tdma_explicit[iden], 3);
    rc |= expect_eq_long("zero base preserves cache", st.trunk_chan_map[(iden << 12) | 0x0010], 987654321L);

    if (rc == 0) {
        DSD_FPRINTF(stderr, "PASS test_lcw_rejects_incomplete_fdma_updates\n");
    }
    return rc;
}

static int
test_lcw_replaces_existing_fdma(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    uint8_t bits[96];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    int iden = 4;
    st.p25_iden_fdma[iden].base_freq = 11111111L;
    st.p25_iden_fdma[iden].chan_type = 1;
    st.p25_iden_fdma[iden].chan_spac = 50;
    st.p25_iden_fdma[iden].trans_off = -2200;
    st.p25_iden_fdma[iden].bw_vu = 4;
    st.p25_iden_fdma[iden].trust = 2;
    st.p25_iden_fdma[iden].populated = 1;
    st.p25_chan_tdma_explicit[iden] = 1;
    st.trunk_chan_map[(iden << 12) | 0x000A] = 123456789L;

    build_lcw_vuhf(bits, iden, 5, 1, 123, 100, 450000000UL / 5UL);
    p25_lcw(&opts, &st, bits, 0);

    rc |= expect_eq_u8("replace populated", st.p25_iden_fdma[iden].populated, 1);
    rc |= expect_eq_long("replace base", st.p25_iden_fdma[iden].base_freq, 450000000L / 5L);
    rc |= expect_eq_int("replace type", st.p25_iden_fdma[iden].chan_type, 1);
    rc |= expect_eq_int("replace spacing", st.p25_iden_fdma[iden].chan_spac, 100);
    rc |= expect_eq_int("replace offset", st.p25_iden_fdma[iden].trans_off, 123);
    rc |= expect_eq_u8("replace bw", st.p25_iden_fdma[iden].bw_vu, 5);
    rc |= expect_eq_u8("replace bitmask", st.p25_chan_tdma_explicit[iden], 1);
    rc |= expect_eq_long("replace invalidates channel cache", st.trunk_chan_map[(iden << 12) | 0x000A], 0);

    if (rc == 0) {
        DSD_FPRINTF(stderr, "PASS test_lcw_replaces_existing_fdma\n");
    }
    return rc;
}

static int
test_iden_cache_invalidation_covers_last_channel(void) {
    int rc = 0;
    static dsd_state st;
    DSD_MEMSET(&st, 0, sizeof st);

    st.trunk_chan_map[0xF000] = 12345L;
    st.trunk_chan_map[0xFFFE] = 67890L;

    p25_invalidate_chan_map_for_iden(&st, 15);

    rc |= expect_eq_long("invalidate iden 15 first channel", st.trunk_chan_map[0xF000], 0);
    rc |= expect_eq_long("invalidate iden 15 last channel", st.trunk_chan_map[0xFFFE], 0);

    if (rc == 0) {
        DSD_FPRINTF(stderr, "PASS test_iden_cache_invalidation_covers_last_channel\n");
    }
    return rc;
}

#if LONG_MAX > 2147483647L
_Static_assert(sizeof(p25_iden_entry_t) == 56, "IDEN flags must fit existing LP64 padding");
#endif

static void
iden_local_system(dsd_state* state) {
    state->p2_wacn = 0x1490E;
    state->p2_sysid = 0xCFF;
    state->synctype = 0;
    state->p25_cc_is_tdma = 0;
}

static int
iden_expect_tdma(const char* tag, const dsd_opts* opts, dsd_state* state) {
    p25_freq_trace_t trace;
    int rc = expect_eq_long(tag, process_channel_to_freq_trace(opts, state, 0x4119, &trace), 770756250L);
    rc |= expect_eq_int(tag, state->p25_chan_tdma_explicit[4], 2);
    rc |= expect_eq_int(tag, state->p25_iden_fdma[4].populated, 0);
    rc |= expect_eq_int(tag, trace.denom, 2);
    rc |= expect_eq_int(tag, strcmp(trace.source, "iden-tdma"), 0);
    char suffix[64];
    p25_format_chan_suffix(state, 0x4119, -1, suffix, sizeof suffix);
    rc |= expect_eq_int(tag, strstr(suffix, "FDMA 008C S2") != NULL, 1);
    return rc;
}

static int
test_iden_arrival_orders(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof *opts);
    dsd_state* state = (dsd_state*)calloc(1, sizeof *state);
    assert(opts && state);
    int rc = 0;
    const int opcodes[] = {0x73, 0xF3};
    for (int format = 0x58; format <= 0x59; format++) {
        for (int op = 0; op < 2; op++) {
            for (int lcw_first = 0; lcw_first < 2; lcw_first++) {
                DSD_MEMSET(state, 0, sizeof *state);
                iden_local_system(state);
                if (lcw_first) {
                    iden_test_lcw(opts, state, format, 769006250 / 5, 100);
                    p25_freq_trace_t trace;
                    rc |= expect_eq_long("T2 before", process_channel_to_freq_trace(opts, state, 0x4119, &trace),
                                         772518750L);
                    rc |= expect_eq_int("T2 before trace", strcmp(trace.source, "iden-fdma"), 0);
                }
                iden_test_mac(opts, state, opcodes[op], 4, 3, 769006250 / 5, 100, 0x1490E, 0xCFF, 1);
                if (!lcw_first) {
                    iden_test_lcw(opts, state, format, 769006250 / 5, 100);
                }
                rc |= iden_expect_tdma(lcw_first ? "T2 LCW first" : "T1 TDMA first", opts, state);
            }
        }
    }
    free(opts);
    free(state);
    return rc;
}

static int
test_typed_fdma_preserved(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof *opts);
    dsd_state* state = (dsd_state*)calloc(1, sizeof *state);
    assert(opts && state);
    int rc = 0;
    for (int type = 0; type < 3; type++) {
        DSD_MEMSET(state, 0, sizeof *state);
        iden_local_system(state);
        iden_test_mac(opts, state, type == 1 ? 0x7D : 0x73, 4, type, 769006250 / 5, 100, 0, 0, 1);
        iden_test_lcw(opts, state, 0x58, 769006250 / 5, 100);
        rc |= expect_eq_int("T4 typed FDMA type", state->p25_iden_fdma[4].chan_type, type);
        rc |= expect_eq_int("T4 typed FDMA flags", state->p25_iden_fdma[4].flags, 0);
        iden_test_mac(opts, state, 0x73, 4, 3, 769006250 / 5, 100, 0, 0, 1);
        rc |= expect_eq_int("T3 two typed entries", state->p25_chan_tdma_explicit[4], 3);
        p25_freq_trace_t trace;
        rc |= expect_eq_long("T3 Phase 1 context", process_channel_to_freq_trace(opts, state, 0x4119, &trace),
                             772518750L);
        state->p25_cc_is_tdma = 1;
        rc |= expect_eq_long("T3 Phase 2 context", process_channel_to_freq_trace(opts, state, 0x4119, &trace),
                             770756250L);
    }
    free(opts);
    free(state);
    return rc;
}

static int
test_lcw_keeps_tdma_numbers(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof *opts);
    dsd_state* state = (dsd_state*)calloc(1, sizeof *state);
    assert(opts && state);
    iden_local_system(state);
    iden_test_mac(opts, state, 0x73, 4, 3, 769006250 / 5, 100, 0, 0, 1);
    const p25_iden_entry_t before = state->p25_iden_tdma[4];
    state->trunk_chan_map[0x4001] = 12345;
    iden_test_lcw(opts, state, 0x58, 850000000 / 5, 50);
    int rc = iden_expect_tdma("T6 differing LCW", opts, state);
    rc |= expect_eq_long("T6 cache unchanged", state->trunk_chan_map[0x4001], 12345);
    rc |= expect_eq_int("T6 TDMA unchanged", memcmp(&before, &state->p25_iden_tdma[4], sizeof before), 0);
    for (int format = 0x58; format <= 0x59; format++) {
        DSD_MEMSET(state, 0, sizeof *state);
        iden_test_lcw(opts, state, format, 769006250 / 5, 100);
        rc |= expect_eq_long("T7 LCW only", process_channel_to_freq(opts, state, 0x4119), 772518750L);
        const int flags = P25_IDEN_F_UNTYPED | (format == 0x59 ? P25_IDEN_F_OFF_SPACING : 0);
        rc |= expect_eq_int("T7 flags", state->p25_iden_fdma[4].flags, flags);
    }
    free(opts);
    free(state);
    return rc;
}

static int
test_iden_unusable_case(int opcode, uint32_t base, int spacing, int lcw_first) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof *opts);
    dsd_state* state = (dsd_state*)calloc(1, sizeof *state);
    assert(opts && state);
    if (lcw_first) {
        iden_test_lcw(opts, state, 0x58, 769006250 / 5, 100);
    }
    state->trunk_chan_map[0x4119] = 12345;
    if (opcode < 0x60) {
        iden_test_lcw(opts, state, opcode, base, spacing);
    } else {
        iden_test_mac(opts, state, opcode, 4, 3, base, spacing, 0, 0, 1);
    }
    int rc = expect_eq_long("T5 unusable preserves map", state->trunk_chan_map[0x4119], 12345);
    rc |= expect_eq_int("T5 unusable preserves hint", state->p25_chan_tdma_explicit[4], lcw_first);
    if (!lcw_first) {
        iden_test_lcw(opts, state, 0x58, 769006250 / 5, 100);
    }
    rc |= expect_eq_int("T5 no unusable TDMA", state->p25_iden_tdma[4].populated, 0);
    rc |= expect_eq_long("T5 usable LCW remains", state->p25_iden_fdma[4].base_freq, 769006250 / 5);
    free(opts);
    free(state);
    return rc;
}

static int
test_unusable_keeps_typed_entry(int opcode) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof *opts);
    dsd_state* state = (dsd_state*)calloc(1, sizeof *state);
    assert(opts && state);
    iden_test_mac(opts, state, opcode, 4, 3, 769006250 / 5, 100, 0, 0, 1);
    const p25_iden_entry_t f = state->p25_iden_fdma[4], t = state->p25_iden_tdma[4];
    const uint8_t hint = state->p25_chan_tdma_explicit[4];
    const uint32_t bases[] = {0, 769006250 / 5, DSD_TRUNK_FREQ_MAX_HZ / 5 + 1U, 0xFFFFFFFFU};
    int rc = 0;
    for (int value = 0; value < 4; value++) {
        state->trunk_chan_map[0x4119] = 12345;
        iden_test_mac(opts, state, opcode, 4, 3, bases[value], value == 1 ? 0 : 100, 0, 0, 1);
        rc |= expect_eq_int("T5 earlier typed FDMA kept", memcmp(&f, &state->p25_iden_fdma[4], sizeof f), 0);
        rc |= expect_eq_int("T5 earlier typed TDMA kept", memcmp(&t, &state->p25_iden_tdma[4], sizeof t), 0);
        rc |= expect_eq_int("T5 earlier typed hint kept", state->p25_chan_tdma_explicit[4], hint);
        rc |= expect_eq_long("T5 earlier typed cache kept", state->trunk_chan_map[0x4119], 12345);
    }
    free(state);
    free(opts);
    return rc;
}

static int
test_iden_bounds_and_crc(void) {
    const int opcodes[] = {0x73, 0x74, 0x7D, 0xF3, 0x58, 0x59};
    const uint32_t bases[] = {0, 769006250 / 5, DSD_TRUNK_FREQ_MAX_HZ / 5 + 1U, 0xFFFFFFFFU};
    int rc = 0;
    for (unsigned op = 0; op < sizeof opcodes / sizeof opcodes[0]; op++) {
        for (int value = 0; value < 4; value++) {
            for (int order = 0; order < 2; order++) {
                rc |= test_iden_unusable_case(opcodes[op], bases[value], value == 1 ? 0 : 100, order);
            }
        }
        dsd_opts* opts = (dsd_opts*)calloc(1, sizeof *opts);
        dsd_state* state = (dsd_state*)calloc(1, sizeof *state);
        assert(opts && state);
        if (opcodes[op] < 0x60) {
            iden_test_lcw(opts, state, opcodes[op], DSD_TRUNK_FREQ_MAX_HZ / 5, 100);
        } else {
            iden_test_mac(opts, state, opcodes[op], 4, 3, DSD_TRUNK_FREQ_MAX_HZ / 5, 100, 0, 0, 1);
        }
        p25_iden_entry_t fdma = state->p25_iden_fdma[4], tdma = state->p25_iden_tdma[4];
        rc |= expect_eq_int("T5 maximum usable base", fdma.populated + tdma.populated, 1);
        if (opcodes[op] >= 0x60) {
            rc |= test_unusable_keeps_typed_entry(opcodes[op]);
            state->trunk_chan_map[0x4119] = 12345;
            const uint8_t hint = state->p25_chan_tdma_explicit[4];
            iden_test_mac(opts, state, opcodes[op], 4, 1, 769006250 / 5, 50, 1, 1, 0);
            rc |= expect_eq_int("T10 CRC FDMA unchanged", memcmp(&fdma, &state->p25_iden_fdma[4], sizeof fdma), 0);
            rc |= expect_eq_int("T10 CRC TDMA unchanged", memcmp(&tdma, &state->p25_iden_tdma[4], sizeof tdma), 0);
            rc |= expect_eq_int("T10 CRC hint unchanged", state->p25_chan_tdma_explicit[4], hint);
            rc |= expect_eq_long("T10 CRC cache unchanged", state->trunk_chan_map[0x4119], 12345);
        }
        free(opts);
        free(state);
    }
    return rc;
}

static int
test_foreign_and_identity(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof *opts);
    dsd_state* state = (dsd_state*)calloc(1, sizeof *state);
    assert(opts && state);
    int rc = 0;
    for (int same_wacn = 0; same_wacn < 2; same_wacn++) {
        DSD_MEMSET(state, 0, sizeof *state);
        iden_local_system(state);
        iden_test_mac(opts, state, 0x73, 4, 3, 769006250 / 5, 100, 0, 0, 1);
        state->trunk_chan_map[0x4119] = 12345;
        iden_test_mac(opts, state, 0xF3, 4, 3, 850000000 / 5, 50, same_wacn ? 0x1490E : 1, 1, 1);
        rc |= expect_eq_long("T8 foreign cache intact", state->trunk_chan_map[0x4119], 12345);
        rc |= expect_eq_long("T8 local TDMA kept", state->p25_iden_tdma[4].base_freq, 769006250 / 5);
    }
    DSD_MEMSET(state, 0, sizeof *state);
    iden_test_mac(opts, state, 0xF3, 4, 3, 850000000 / 5, 50, 1, 1, 1);
    iden_test_mac(opts, state, 0xF3, 5, 3, 769006250 / 5, 100, 0x1490E, 0xCFF, 1);
    rc |= expect_eq_int("T8 unknown stores extended", state->p25_iden_tdma[4].populated, 1);
    state->trunk_chan_map[0x4119] = 12345;
    (void)p25_update_system_identity(state, 0x1490E, 0xCFF);
    rc |= expect_eq_int("T8 first learn drops foreign", state->p25_iden_tdma[4].populated, 0);
    rc |= expect_eq_long("T8 first learn purges cache", state->trunk_chan_map[0x4119], 0);
    rc |= expect_eq_int("T8 matching entry kept", state->p25_iden_tdma[5].populated, 1);
    DSD_MEMSET(state, 0, sizeof *state);
    iden_test_lcw(opts, state, 0x58, 769006250 / 5, 100);
    iden_test_mac(opts, state, 0xF3, 4, 3, 769006250 / 5, 100, 0x1490E, 0xCFF, 1);
    rc |= expect_eq_int("T9 not applicable yet", state->p25_iden_fdma[4].populated, 1);
    (void)p25_update_system_identity(state, 0x1490E, 0xCFF);
    rc |= iden_expect_tdma("T9 first learn reconciles", opts, state);
    (void)p25_update_system_identity(state, 2, 2);
    rc |= expect_eq_int("T9 known change resets", state->p25_chan_tdma_explicit[4], 0);
    free(opts);
    free(state);
    return rc;
}

static int
iden_offset_csv_roundtrip(const dsd_state* state) {
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "p25wireoffset");
    assert(fd >= 0);
    (void)dsd_close(fd);
    p25_bandplan_row_t rows[16];
    const int count = dsd_p25_bandplan_append_tables(rows, 0, 16, state->p25_iden_fdma, NULL, NULL);
    int rc = expect_eq_int("T14 wire CSV export", csvP25BandplanExportRows(path, rows, count), 0);
    FILE* fp = dsd_fopen_private(path, "r");
    assert(fp);
    char text[512] = {0};
    const size_t text_len = fread(text, 1, sizeof text - 1, fp);
    fclose(fp);
    rc |= expect_eq_int("T14 wire CSV read", text_len > 0U, 1);
    rc |= expect_eq_int("T14 wire exported 1 MHz", strstr(text, "4,769006250,12500,1,1000000,0,,") != NULL, 1);
    dsd_state* back = (dsd_state*)calloc(1, sizeof *back);
    assert(back);
    rc |= expect_eq_int("T14 wire CSV import", csvP25BandplanImportPath(path, back), 0);
    const p25_iden_entry_t* e = &back->p25_iden_fdma[4];
    const long unit = (e->flags & P25_IDEN_F_OFF_SPACING) ? 12500 : 250000;
    rc |= expect_eq_long("T14 wire roundtrip Hz", e->trans_off * unit, 1000000);
    rc |= expect_eq_int("T14 wire roundtrip FDMA", e->populated, 1);
    (void)remove(path);
    free(back);
    return rc;
}

static int
test_foreign_fdma_lcw_and_metadata(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof *opts);
    dsd_state* state = (dsd_state*)calloc(1, sizeof *state);
    assert(opts && state);
    int rc = 0;
    for (int cleared = 0; cleared < 2; cleared++) {
        DSD_MEMSET(state, 0, sizeof *state);
        if (cleared) {
            (void)p25_update_system_identity(state, 1, 1);
        }
        iden_test_mac(opts, state, 0xF3, 4, 1, 850000000 / 5, 100, 1, 1, 1);
        state->p2_wacn = state->p2_sysid = 0;
        iden_test_lcw(opts, state, 0x58, 769006250 / 5, 100);
        (void)p25_update_system_identity(state, 0x1490E, 0xCFF);
        iden_test_mac(opts, state, 0x73, 4, 3, 769006250 / 5, 100, 0, 0, 1);
        rc |= iden_expect_tdma("T4 foreign typedness discarded", opts, state);
    }
    for (int opcode = 0x73; opcode <= 0xF3; opcode += 0x80) {
        DSD_MEMSET(state, 0, sizeof *state);
        iden_test_mac(opts, state, 0x74, 4, 5, 450000000 / 5, 100, 0, 0, 1);
        iden_test_mac(opts, state, opcode, 4, 1, 769006250 / 5, 100, 0, 0, 1);
        rc |= expect_eq_int("T14 VU bandwidth cleared", state->p25_iden_fdma[4].bw_vu, 0);
        rc |= expect_eq_int("T14 spacing offset flag", state->p25_iden_fdma[4].flags, P25_IDEN_F_OFF_SPACING);
        rc |= expect_eq_long("T14 offset Hz", state->p25_iden_fdma[4].trans_off * 12500L, 1000000);
        rc |= iden_offset_csv_roundtrip(state);
    }
    free(opts);
    free(state);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_iden_arrival_orders();
    rc |= test_typed_fdma_preserved();
    rc |= test_lcw_keeps_tdma_numbers();
    rc |= test_iden_bounds_and_crc();
    rc |= test_foreign_and_identity();
    rc |= test_foreign_fdma_lcw_and_metadata();
    rc |= test_lcw_standard_populates_fdma();
    rc |= test_lcw_vuhf_populates_fdma();
    rc |= test_lcw_control_channel_provenance_promotes_trust();
    rc |= test_lcw_rejects_incomplete_fdma_updates();
    rc |= test_lcw_replaces_existing_fdma();
    rc |= test_iden_cache_invalidation_covers_last_channel();

    if (rc == 0) {
        DSD_FPRINTF(stderr, "\nAll test_p25_iden_lcw_full tests PASSED\n");
    } else {
        DSD_FPRINTF(stderr, "\nSome test_p25_iden_lcw_full tests FAILED\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
