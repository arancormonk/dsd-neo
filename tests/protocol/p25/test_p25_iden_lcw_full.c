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

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

struct RtlSdrContext;

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return false;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
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

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
ConvertBitIntoBytes(const uint8_t* BufferIn, uint32_t BitLength) {
    uint64_t out = 0;
    for (uint32_t i = 0; i < BitLength; i++) {
        out = (out << 1) | (uint64_t)(BufferIn[i] & 1);
    }
    return out;
}

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

    opts.p25_is_tuned = 0;
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

int
main(void) {
    int rc = 0;

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
