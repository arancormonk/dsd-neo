// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression tests for P25 Phase 2 Motorola talker-alias length handling.
 *
 * The alias header (opcode 0x91) and alias block (opcode 0x95) handlers unpack a
 * packet-derived length octet into a fixed 24-octet staging buffer. An unclamped
 * length overran both the source buffer and the 192-byte bit destination
 * (GHSA-vqjv-vjrv-g9gf). These tests feed a malformed maximal length and assert
 * the unpack stays inside the staging buffer, while a well-formed length is
 * still unpacked in full.
 */

#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/* Staging buffer in the handlers is uint8_t[24]; bytes + 1 leaves 23 readable octets. */
#define TA_STAGING_OCTETS    24
#define TA_MAX_UNPACK_OCTETS (TA_STAGING_OCTETS - 1)
#define TA_BIT_CAPACITY      (TA_STAGING_OCTETS * 8)

// Test shim
void p25_test_process_mac_vpdu_ex(int type, const unsigned char* mac_bytes, int mac_len, int is_lcch, int currentslot);

static int g_alias_calls;
static uint8_t g_alias_bits[TA_BIT_CAPACITY];

static void
capture_alias_bits(const uint8_t* lc_bits) {
    g_alias_calls++;
    for (int i = 0; i < TA_BIT_CAPACITY; i++) {
        g_alias_bits[i] = lc_bits[i];
    }
}

// Stubs referenced by the MAC VPDU path
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    capture_alias_bits(lc_bits);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    capture_alias_bits(lc_bits);
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
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

/* Build a talker-alias PDU: opcode/MFID 0x90, then a length octet, then filler. */
static void
build_alias_mac(unsigned char mac[TA_STAGING_OCTETS], unsigned char opcode, unsigned char len_octet) {
    DSD_MEMSET(mac, 0, TA_STAGING_OCTETS);
    mac[0] = 0x00;
    mac[1] = opcode;
    mac[2] = 0x90;
    mac[3] = len_octet;
    for (int i = 4; i < TA_STAGING_OCTETS; i++) {
        mac[i] = (unsigned char)(0xA0u | (unsigned)(i & 0x0F));
    }
}

/*
 * Verify the captured bits are the MSB-first unpacking of mac[1 .. 1 + expect_octets - 1]
 * and that every bit past that span is untouched.
 */
static int
check_unpacked(const char* tag, const unsigned char mac[TA_STAGING_OCTETS], int expect_octets) {
    int rc = 0;
    for (int i = 0; i < expect_octets; i++) {
        const unsigned char octet = mac[1 + i];
        for (int bit = 0; bit < 8; bit++) {
            const int want = (octet >> (7 - bit)) & 1;
            const int got = g_alias_bits[(i * 8) + bit];
            if (got != want) {
                DSD_FPRINTF(stderr, "%s: octet %d bit %d got %d want %d\n", tag, i, bit, got, want);
                rc = 1;
            }
        }
    }
    for (int i = expect_octets * 8; i < TA_BIT_CAPACITY; i++) {
        if (g_alias_bits[i] != 0) {
            DSD_FPRINTF(stderr, "%s: bit %d written past unpacked span\n", tag, i);
            rc = 1;
        }
    }
    return rc;
}

/* A maximal length octet must be clamped to the staging buffer instead of overrunning it. */
static int
test_malformed_length_is_clamped(unsigned char opcode, const char* tag) {
    unsigned char mac[TA_STAGING_OCTETS];
    build_alias_mac(mac, opcode, 0xFF);

    g_alias_calls = 0;
    DSD_MEMSET(g_alias_bits, 0, sizeof(g_alias_bits));
    p25_test_process_mac_vpdu_ex(0, mac, TA_STAGING_OCTETS, 0, 0);

    int rc = expect_eq_int(tag, g_alias_calls, 1);
    rc |= check_unpacked(tag, mac, TA_MAX_UNPACK_OCTETS);
    return rc;
}

/* A well-formed length must still be unpacked in full. */
static int
test_normal_length_unchanged(unsigned char opcode, const char* tag) {
    const unsigned char len_octet = 0x11; /* 17 octets, the value seen on air */
    unsigned char mac[TA_STAGING_OCTETS];
    build_alias_mac(mac, opcode, len_octet);

    g_alias_calls = 0;
    DSD_MEMSET(g_alias_bits, 0, sizeof(g_alias_bits));
    p25_test_process_mac_vpdu_ex(0, mac, TA_STAGING_OCTETS, 0, 0);

    int rc = expect_eq_int(tag, g_alias_calls, 1);
    rc |= check_unpacked(tag, mac, (int)len_octet);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_malformed_length_is_clamped(0x91, "alias header clamp");
    rc |= test_malformed_length_is_clamped(0x95, "alias blocks clamp");
    rc |= test_normal_length_unchanged(0x91, "alias header normal");
    rc |= test_normal_length_unchanged(0x95, "alias blocks normal");

    if (rc != 0) {
        DSD_FPRINTF(stderr, "P25_P2_TA_LEN_BOUNDS: FAIL\n");
        return 1;
    }
    DSD_FPRINTF(stderr, "P25_P2_TA_LEN_BOUNDS: PASS\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
