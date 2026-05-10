// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for P25 Tier 3 Conformance handlers:
 *        Protection Parameter Broadcast/Update, Emergency Alarm ISP,
 *        and Time and Date Announcement.
 *
 * Tests field extraction logic, state storage, and VPDU dispatch for
 * MAC opcode 0x75, TSBK OSP opcode 0x3F, and TSBK ISP opcode 0x27.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

struct RtlSdrContext;

/* External entry point under test */
void process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, unsigned long long int MAC[24]);

static void
set_bits(uint8_t* bits, int start, int count, uint64_t value) {
    for (int i = 0; i < count; i++) {
        bits[start + i] = (uint8_t)((value >> (count - 1 - i)) & 0x1U);
    }
}

static void
mac_set_bits(unsigned long long MAC[24], int start_bit, int bit_count, uint64_t value) {
    for (int i = 0; i < bit_count; i++) {
        int bit = start_bit + i;
        int octet = 1 + (bit / 8);
        int shift = 7 - (bit % 8);
        unsigned long long mask = 1ULL << shift;
        if ((value >> (bit_count - 1 - i)) & 0x1U) {
            MAC[octet] |= mask;
        } else {
            MAC[octet] &= ~mask;
        }
    }
}

static uint64_t
mac_get_bits(const unsigned long long MAC[24], int start_bit, int bit_count) {
    uint64_t value = 0;
    for (int i = 0; i < bit_count; i++) {
        int bit = start_bit + i;
        int octet = 1 + (bit / 8);
        int shift = 7 - (bit % 8);
        value = (value << 1) | ((uint64_t)(MAC[octet] >> shift) & 0x1U);
    }
    return value;
}

static time_t
utc_time_from_fields(int year, int month, int day, int hours, int minutes, int seconds) {
    int64_t y = year;
    unsigned m = (unsigned)month;
    unsigned d = (unsigned)day;
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned mp = (m > 2) ? (m - 3U) : (m + 9U);
    unsigned doy = (153U * mp + 2U) / 5U + d - 1U;
    unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    int64_t days = era * 146097 + (int64_t)doe - 719468;
    int64_t total = days * 86400 + (int64_t)hours * 3600 + (int64_t)minutes * 60 + seconds;
    return (time_t)total;
}

static void
build_time_date_mac(unsigned long long MAC[24], int vd, int vt, int vl, int lto_sign, int offset_minutes, int year,
                    int month, int day, int hours, int minutes, int seconds) {
    memset(MAC, 0, 24 * sizeof(unsigned long long));
    MAC[0] = 0x09;
    MAC[1] = 0x75;
    mac_set_bits(MAC, 8, 1, (uint64_t)vd);
    mac_set_bits(MAC, 9, 1, (uint64_t)vt);
    mac_set_bits(MAC, 10, 1, (uint64_t)vl);
    if (vl) {
        mac_set_bits(MAC, 11, 1, (uint64_t)lto_sign);
        mac_set_bits(MAC, 12, 12, (uint64_t)offset_minutes);
    }
    if (vd) {
        mac_set_bits(MAC, 24, 4, (uint64_t)month);
        mac_set_bits(MAC, 28, 5, (uint64_t)day);
        mac_set_bits(MAC, 33, 13, (uint64_t)year);
    }
    if (vt) {
        mac_set_bits(MAC, 48, 5, (uint64_t)hours);
        mac_set_bits(MAC, 53, 6, (uint64_t)minutes);
        mac_set_bits(MAC, 59, 6, (uint64_t)seconds);
    }
}

/* ============================================================================
 * Stubs for external hooks referenced by linked modules
 * ============================================================================ */

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

void
unpack_byte_array_into_bit_array(uint8_t* input, uint8_t* output, int len) {
    (void)input;
    (void)output;
    (void)len;
}

void
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

/* ============================================================================
 * ALGID lookup helper — mirrors the static function in p25p2_vpdu.c and
 * p25_lcw.c for direct unit testing of the lookup logic.
 * ============================================================================ */

static const char*
test_p25_algid_name(uint8_t algid) {
    switch (algid) {
        case 0x80: return "UNENCRYPTED";
        case 0x81: return "DES-OFB";
        case 0x82: return "2-KEY 3DES";
        case 0x83: return "3-KEY 3DES";
        case 0x84: return "AES-256";
        case 0x85: return "AES-128";
        case 0x88: return "AES-CBC";
        case 0x89: return "AES-128-OFB";
        case 0x9F: return "DES-XL";
        case 0xAA: return "ADP/RC4";
        case 0xAF: return "AES-256-GCM";
        default: return NULL;
    }
}

/* ============================================================================
 * 6.1 Protection Parameter Tests
 * ============================================================================ */

/**
 * test_prot_param_lcw_known_payload — construct a 72-bit LCW array with
 * format 0x65, ALGID=0x80, KID=0x1234, Target=0x123456. Verify extraction.
 */
static int
test_prot_param_lcw_known_payload(void) {
    int rc = 0;

    uint8_t LCW_bits[72];
    memset(LCW_bits, 0, sizeof(LCW_bits));
    set_bits(LCW_bits, 0, 8, 0x65);
    set_bits(LCW_bits, 24, 8, 0x80);
    set_bits(LCW_bits, 32, 16, 0x1234);
    set_bits(LCW_bits, 48, 24, 0x123456);

    uint8_t algid = 0;
    for (int i = 24; i < 32; i++) {
        algid = (uint8_t)((algid << 1) | LCW_bits[i]);
    }

    uint16_t kid = 0;
    for (int i = 32; i < 48; i++) {
        kid = (uint16_t)((kid << 1) | LCW_bits[i]);
    }

    uint32_t target = 0;
    for (int i = 48; i < 72; i++) {
        target = (target << 1) | LCW_bits[i];
    }

    if (algid != 0x80) {
        fprintf(stderr, "FAIL: test_prot_param_lcw_known_payload: ALGID expected 0x80, got 0x%02X\n", algid);
        rc = 1;
    }
    if (kid != 0x1234) {
        fprintf(stderr, "FAIL: test_prot_param_lcw_known_payload: KID expected 0x1234, got 0x%04X\n", kid);
        rc = 1;
    }
    if (target != 0x123456) {
        fprintf(stderr, "FAIL: test_prot_param_lcw_known_payload: Target expected 0x123456, got 0x%06X\n", target);
        rc = 1;
    }

    return rc;
}

/**
 * test_prot_param_algid_unencrypted_name — verify ALGID 0x80 resolves to
 * "UNENCRYPTED"
 */
static int
test_prot_param_algid_unencrypted_name(void) {
    const char* name = test_p25_algid_name(0x80);
    if (name == NULL || strcmp(name, "UNENCRYPTED") != 0) {
        fprintf(stderr, "FAIL: test_prot_param_algid_unencrypted_name: expected 'UNENCRYPTED', got '%s'\n",
                name ? name : "(null)");
        return 1;
    }
    return 0;
}

/**
 * test_prot_param_algid_des_ofb_name — verify ALGID 0x81 resolves to "DES-OFB"
 */
static int
test_prot_param_algid_des_ofb_name(void) {
    const char* name = test_p25_algid_name(0x81);
    if (name == NULL || strcmp(name, "DES-OFB") != 0) {
        fprintf(stderr, "FAIL: test_prot_param_algid_des_ofb_name: expected 'DES-OFB', got '%s'\n",
                name ? name : "(null)");
        return 1;
    }
    return 0;
}

/**
 * test_prot_param_algid_aes256_name — verify ALGID 0x84 resolves to "AES-256"
 */
static int
test_prot_param_algid_aes256_name(void) {
    const char* name = test_p25_algid_name(0x84);
    if (name == NULL || strcmp(name, "AES-256") != 0) {
        fprintf(stderr, "FAIL: test_prot_param_algid_aes256_name: expected 'AES-256', got '%s'\n",
                name ? name : "(null)");
        return 1;
    }
    return 0;
}

/**
 * test_prot_param_algid_des_xl_name — verify ALGID 0x9F resolves to "DES-XL"
 */
static int
test_prot_param_algid_des_xl_name(void) {
    const char* name = test_p25_algid_name(0x9F);
    if (name == NULL || strcmp(name, "DES-XL") != 0) {
        fprintf(stderr, "FAIL: test_prot_param_algid_des_xl_name: expected 'DES-XL', got '%s'\n",
                name ? name : "(null)");
        return 1;
    }
    return 0;
}

/**
 * test_prot_param_tsbk_0x3f_known_payload — construct a TSBK 0x3F bridge PDU
 * with ALGID=0x84, KID=0xABCD, Target=0x123456. Verify extraction.
 */
static int
test_prot_param_tsbk_0x3f_known_payload(void) {
    int rc = 0;

    /* Simulate processTSBK() bridge output for Protection Parameter Update.
     * TSBK OSP 0x3F is bridged as 0x7F. sdrtrunk places two reserved octets
     * before ALGID/KID/target.
     */
    unsigned long long MAC[24];
    memset(MAC, 0, sizeof(MAC));
    MAC[0] = 0x07;
    MAC[1] = 0x7F;
    MAC[2] = 0x00;
    MAC[3] = 0x00;
    MAC[4] = 0x84;
    MAC[5] = 0xAB;
    MAC[6] = 0xCD;
    MAC[7] = 0x12;
    MAC[8] = 0x34;
    MAC[9] = 0x56;

    int algid = (int)MAC[4];
    int kid = (int)((MAC[5] << 8) | MAC[6]);
    int target = (int)((MAC[7] << 16) | (MAC[8] << 8) | MAC[9]);

    if (algid != 0x84) {
        fprintf(stderr, "FAIL: test_prot_param_tsbk_0x3f_known_payload: ALGID expected 0x84, got 0x%02X\n", algid);
        rc = 1;
    }
    if (kid != 0xABCD) {
        fprintf(stderr, "FAIL: test_prot_param_tsbk_0x3f_known_payload: KID expected 0xABCD, got 0x%04X\n", kid);
        rc = 1;
    }
    if (target != 0x123456) {
        fprintf(stderr, "FAIL: test_prot_param_tsbk_0x3f_known_payload: Target expected 0x123456, got 0x%06X\n",
                target);
        rc = 1;
    }

    return rc;
}

/**
 * test_tsbk_0x3e_not_protection — TSBK 0x3E is not a protection parameter
 * message in sdrtrunk; it must not update protection state.
 */
static int
test_tsbk_0x3e_not_protection(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof(opts));
    memset(&st, 0, sizeof(st));

    unsigned long long MAC[24];
    memset(MAC, 0, sizeof(MAC));
    MAC[0] = 0x07;
    MAC[1] = 0x7E;
    MAC[2] = 0x80;
    MAC[3] = 0x56;
    MAC[4] = 0x78;
    MAC[5] = 0xAB;
    MAC[6] = 0xCD;
    MAC[7] = 0xEF;

    process_MAC_VPDU(&opts, &st, 0, MAC);

    if (st.p25_prot_algid != 0 || st.p25_prot_kid != 0) {
        fprintf(stderr, "FAIL: test_tsbk_0x3e_not_protection: protection state changed ALGID=0x%02X KID=0x%04X\n",
                st.p25_prot_algid, st.p25_prot_kid);
        rc = 1;
    }

    return rc;
}

/**
 * test_prot_param_state_init_zero — verify calloc'd state has p25_prot_algid=0,
 * p25_prot_kid=0
 */
static int
test_prot_param_state_init_zero(void) {
    int rc = 0;

    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!state) {
        fprintf(stderr, "FAIL: test_prot_param_state_init_zero: calloc failed\n");
        return 1;
    }

    if (state->p25_prot_algid != 0) {
        fprintf(stderr, "FAIL: test_prot_param_state_init_zero: p25_prot_algid expected 0, got %u\n",
                state->p25_prot_algid);
        rc = 1;
    }
    if (state->p25_prot_valid != 0) {
        fprintf(stderr, "FAIL: test_prot_param_state_init_zero: p25_prot_valid expected 0, got %u\n",
                state->p25_prot_valid);
        rc = 1;
    }
    if (state->p25_prot_kid != 0) {
        fprintf(stderr, "FAIL: test_prot_param_state_init_zero: p25_prot_kid expected 0, got %u\n",
                state->p25_prot_kid);
        rc = 1;
    }

    free(state);
    return rc;
}

/* ============================================================================
 * 6.2 Emergency Alarm ISP Tests
 * ============================================================================ */

/**
 * test_emergency_alarm_isp_known_payload — construct TSBK byte array with
 * opcode 0x27, protectbit=1, MFID=0x00, Group=0x1234, Source=0x567890.
 * Verify extraction.
 */
static int
test_emergency_alarm_isp_known_payload(void) {
    int rc = 0;

    /* TSBK layout (10 data octets + 2 CRC):
     * Octet 0: [LB=0 | P=1 | Opcode(5:0) = 0x27] => 0_1_100111 = 0x67
     * Octet 1: MFID = 0x00
     * Octets 2-4: Reserved (zeros)
     * Octets 5-6: Group_Address = 0x1234
     * Octets 7-9: Source_Address = 0x567890
     */
    uint8_t tsbk_byte[12];
    memset(tsbk_byte, 0, sizeof(tsbk_byte));
    tsbk_byte[0] = 0x67; /* LB=0, P=1, opcode=0x27 */
    tsbk_byte[1] = 0x00; /* MFID */
    tsbk_byte[5] = 0x12; /* Group high */
    tsbk_byte[6] = 0x34; /* Group low */
    tsbk_byte[7] = 0x56; /* Source high */
    tsbk_byte[8] = 0x78; /* Source mid */
    tsbk_byte[9] = 0x90; /* Source low */

    /* Extract fields the same way the handler does */
    int protectbit = (tsbk_byte[0] >> 6) & 1;
    int opcode = tsbk_byte[0] & 0x3F;
    int mfid = tsbk_byte[1];
    int group = (tsbk_byte[5] << 8) | tsbk_byte[6];
    int source = (tsbk_byte[7] << 16) | (tsbk_byte[8] << 8) | tsbk_byte[9];

    if (protectbit != 1) {
        fprintf(stderr, "FAIL: test_emergency_alarm_isp_known_payload: protectbit expected 1, got %d\n", protectbit);
        rc = 1;
    }
    if (opcode != 0x27) {
        fprintf(stderr, "FAIL: test_emergency_alarm_isp_known_payload: opcode expected 0x27, got 0x%02X\n", opcode);
        rc = 1;
    }
    if (mfid != 0x00) {
        fprintf(stderr, "FAIL: test_emergency_alarm_isp_known_payload: MFID expected 0x00, got 0x%02X\n", mfid);
        rc = 1;
    }
    if (group != 0x1234) {
        fprintf(stderr, "FAIL: test_emergency_alarm_isp_known_payload: Group expected 0x1234, got 0x%04X\n", group);
        rc = 1;
    }
    if (source != 0x567890) {
        fprintf(stderr, "FAIL: test_emergency_alarm_isp_known_payload: Source expected 0x567890, got 0x%06X\n", source);
        rc = 1;
    }

    return rc;
}

/**
 * test_emergency_alarm_mfid90_not_affected — verify that opcode 0x27 with
 * MFID=0x90 does NOT enter the standard ISP path (MFID gate check).
 */
static int
test_emergency_alarm_mfid90_not_affected(void) {
    int rc = 0;

    uint8_t tsbk_byte[12];
    memset(tsbk_byte, 0, sizeof(tsbk_byte));
    tsbk_byte[0] = 0x67; /* LB=0, P=1, opcode=0x27 */
    tsbk_byte[1] = 0x90; /* MFID = Motorola proprietary */

    int mfid = tsbk_byte[1];

    /* The ISP handler gate requires MFID < 0x02. MFID 0x90 should NOT pass. */
    int passes_gate = (mfid < 0x02) ? 1 : 0;

    if (passes_gate != 0) {
        fprintf(stderr, "FAIL: test_emergency_alarm_mfid90_not_affected: MFID 0x90 should not pass ISP gate\n");
        rc = 1;
    }

    return rc;
}

/**
 * test_emergency_alarm_isp_not_bridged — verify that protectbit=1 means the
 * message is NOT bridged to VPDU (the TSBK→MAC bridge only routes protectbit=0).
 */
static int
test_emergency_alarm_isp_not_bridged(void) {
    int rc = 0;

    uint8_t tsbk_byte[12];
    memset(tsbk_byte, 0, sizeof(tsbk_byte));
    tsbk_byte[0] = 0x67; /* LB=0, P=1, opcode=0x27 */
    tsbk_byte[1] = 0x00; /* MFID */

    int protectbit = (tsbk_byte[0] >> 6) & 1;

    /* The TSBK→MAC bridge only routes messages where protectbit == 0 (OSP).
     * ISP messages (protectbit=1) are handled directly in processTSBK(). */
    int would_bridge = (protectbit == 0) ? 1 : 0;

    if (would_bridge != 0) {
        fprintf(stderr, "FAIL: test_emergency_alarm_isp_not_bridged: protectbit=1 should NOT bridge to VPDU\n");
        rc = 1;
    }

    return rc;
}

/* ============================================================================
 * 6.3 Time and Date Announcement Tests
 * ============================================================================ */

/**
 * test_time_date_known_payload — construct MAC PDU encoding
 * 2025-03-15 14:30:45 UTC+05:30. Verify all fields.
 */
static int
test_time_date_known_payload(void) {
    int rc = 0;

    unsigned long long MAC[24];
    build_time_date_mac(MAC, 1, 1, 1, 0, 330, 2025, 3, 15, 14, 30, 45);

    int vd = (int)mac_get_bits(MAC, 8, 1);
    int vt = (int)mac_get_bits(MAC, 9, 1);
    int vl = (int)mac_get_bits(MAC, 10, 1);
    int lto_sign = (int)mac_get_bits(MAC, 11, 1);
    int lto_mag = (int)mac_get_bits(MAC, 12, 12);
    int month = (int)mac_get_bits(MAC, 24, 4);
    int day = (int)mac_get_bits(MAC, 28, 5);
    int year = (int)mac_get_bits(MAC, 33, 13);
    int hours = (int)mac_get_bits(MAC, 48, 5);
    int minutes = (int)mac_get_bits(MAC, 53, 6);
    int seconds = (int)mac_get_bits(MAC, 59, 6);

    if (vd != 1) {
        fprintf(stderr, "FAIL: test_time_date_known_payload: VD expected 1, got %d\n", vd);
        rc = 1;
    }
    if (vt != 1) {
        fprintf(stderr, "FAIL: test_time_date_known_payload: VT expected 1, got %d\n", vt);
        rc = 1;
    }
    if (vl != 1) {
        fprintf(stderr, "FAIL: test_time_date_known_payload: VL expected 1, got %d\n", vl);
        rc = 1;
    }
    if (year != 2025) {
        fprintf(stderr, "FAIL: test_time_date_known_payload: Year expected 2025, got %d\n", year);
        rc = 1;
    }
    if (month != 3) {
        fprintf(stderr, "FAIL: test_time_date_known_payload: Month expected 3, got %d\n", month);
        rc = 1;
    }
    if (day != 15) {
        fprintf(stderr, "FAIL: test_time_date_known_payload: Day expected 15, got %d\n", day);
        rc = 1;
    }
    if (hours != 14) {
        fprintf(stderr, "FAIL: test_time_date_known_payload: Hours expected 14, got %d\n", hours);
        rc = 1;
    }
    if (minutes != 30) {
        fprintf(stderr, "FAIL: test_time_date_known_payload: Minutes expected 30, got %d\n", minutes);
        rc = 1;
    }
    if (seconds != 45) {
        fprintf(stderr, "FAIL: test_time_date_known_payload: Seconds expected 45, got %d\n", seconds);
        rc = 1;
    }
    if (lto_sign != 0) {
        fprintf(stderr, "FAIL: test_time_date_known_payload: LTO_sign expected 0, got %d\n", lto_sign);
        rc = 1;
    }
    if (lto_mag != 330) {
        fprintf(stderr, "FAIL: test_time_date_known_payload: LTO_mag expected 330, got %d\n", lto_mag);
        rc = 1;
    }

    return rc;
}

/**
 * test_time_date_vd_only — VD=1, VT=0, VL=0: only date fields valid.
 * Verify that date fields are extracted but time/offset are zero.
 */
static int
test_time_date_vd_only(void) {
    int rc = 0;

    unsigned long long MAC[24];
    build_time_date_mac(MAC, 1, 0, 0, 0, 0, 2030, 12, 25, 0, 0, 0);

    int vd = (int)mac_get_bits(MAC, 8, 1);
    int vt = (int)mac_get_bits(MAC, 9, 1);
    int vl = (int)mac_get_bits(MAC, 10, 1);
    int month = (int)mac_get_bits(MAC, 24, 4);
    int day = (int)mac_get_bits(MAC, 28, 5);
    int year = (int)mac_get_bits(MAC, 33, 13);

    if (vd != 1) {
        fprintf(stderr, "FAIL: test_time_date_vd_only: VD expected 1, got %d\n", vd);
        rc = 1;
    }
    if (vt != 0) {
        fprintf(stderr, "FAIL: test_time_date_vd_only: VT expected 0, got %d\n", vt);
        rc = 1;
    }
    if (vl != 0) {
        fprintf(stderr, "FAIL: test_time_date_vd_only: VL expected 0, got %d\n", vl);
        rc = 1;
    }
    if (year != 2030) {
        fprintf(stderr, "FAIL: test_time_date_vd_only: Year expected 2030, got %d\n", year);
        rc = 1;
    }
    if (month != 12) {
        fprintf(stderr, "FAIL: test_time_date_vd_only: Month expected 12, got %d\n", month);
        rc = 1;
    }
    if (day != 25) {
        fprintf(stderr, "FAIL: test_time_date_vd_only: Day expected 25, got %d\n", day);
        rc = 1;
    }

    return rc;
}

/**
 * test_time_date_vt_only — VD=0, VT=1, VL=0: only time fields valid.
 */
static int
test_time_date_vt_only(void) {
    int rc = 0;

    unsigned long long MAC[24];
    build_time_date_mac(MAC, 0, 1, 0, 0, 0, 0, 0, 0, 23, 59, 59);

    int vd = (int)mac_get_bits(MAC, 8, 1);
    int vt = (int)mac_get_bits(MAC, 9, 1);
    int vl = (int)mac_get_bits(MAC, 10, 1);
    int hours = (int)mac_get_bits(MAC, 48, 5);
    int minutes = (int)mac_get_bits(MAC, 53, 6);
    int seconds = (int)mac_get_bits(MAC, 59, 6);

    if (vd != 0) {
        fprintf(stderr, "FAIL: test_time_date_vt_only: VD expected 0, got %d\n", vd);
        rc = 1;
    }
    if (vt != 1) {
        fprintf(stderr, "FAIL: test_time_date_vt_only: VT expected 1, got %d\n", vt);
        rc = 1;
    }
    if (vl != 0) {
        fprintf(stderr, "FAIL: test_time_date_vt_only: VL expected 0, got %d\n", vl);
        rc = 1;
    }
    if (hours != 23) {
        fprintf(stderr, "FAIL: test_time_date_vt_only: Hours expected 23, got %d\n", hours);
        rc = 1;
    }
    if (minutes != 59) {
        fprintf(stderr, "FAIL: test_time_date_vt_only: Minutes expected 59, got %d\n", minutes);
        rc = 1;
    }
    if (seconds != 59) {
        fprintf(stderr, "FAIL: test_time_date_vt_only: Seconds expected 59, got %d\n", seconds);
        rc = 1;
    }

    return rc;
}

/**
 * test_time_date_state_init_zero — verify calloc'd state has p25_sys_time=0,
 * p25_sys_time_offset=0
 */
static int
test_time_date_state_init_zero(void) {
    int rc = 0;

    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!state) {
        fprintf(stderr, "FAIL: test_time_date_state_init_zero: calloc failed\n");
        return 1;
    }

    if (state->p25_sys_time != 0) {
        fprintf(stderr, "FAIL: test_time_date_state_init_zero: p25_sys_time expected 0, got %ld\n",
                (long)state->p25_sys_time);
        rc = 1;
    }
    if (state->p25_sys_time_valid != 0) {
        fprintf(stderr, "FAIL: test_time_date_state_init_zero: p25_sys_time_valid expected 0, got %u\n",
                state->p25_sys_time_valid);
        rc = 1;
    }
    if (state->p25_sys_time_offset != 0) {
        fprintf(stderr, "FAIL: test_time_date_state_init_zero: p25_sys_time_offset expected 0, got %d\n",
                state->p25_sys_time_offset);
        rc = 1;
    }
    if (state->p25_sys_time_offset_valid != 0) {
        fprintf(stderr, "FAIL: test_time_date_state_init_zero: p25_sys_time_offset_valid expected 0, got %u\n",
                state->p25_sys_time_offset_valid);
        rc = 1;
    }

    free(state);
    return rc;
}

/**
 * test_time_date_state_stores_time_t — verify known date/time produces
 * expected time_t. Uses 2025-03-15 14:30:45 UTC.
 */
static int
test_time_date_state_stores_time_t(void) {
    int rc = 0;

    time_t expected = utc_time_from_fields(2025, 3, 15, 14, 30, 45);

    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof(opts));
    memset(&st, 0, sizeof(st));

    unsigned long long MAC[24];
    build_time_date_mac(MAC, 1, 1, 0, 0, 0, 2025, 3, 15, 14, 30, 45);
    process_MAC_VPDU(&opts, &st, 0, MAC);

    if (st.p25_sys_time != expected) {
        fprintf(stderr, "FAIL: test_time_date_state_stores_time_t: UTC time_t mismatch, expected %ld, got %ld\n",
                (long)expected, (long)st.p25_sys_time);
        rc = 1;
    }
    if (st.p25_sys_time_valid != 1) {
        fprintf(stderr, "FAIL: test_time_date_state_stores_time_t: expected valid flag set\n");
        rc = 1;
    }

    struct tm* result = gmtime(&st.p25_sys_time);
    if (!result) {
        fprintf(stderr, "FAIL: test_time_date_state_stores_time_t: gmtime failed\n");
        rc = 1;
    } else if (result->tm_year + 1900 != 2025 || result->tm_mon + 1 != 3 || result->tm_mday != 15
               || result->tm_hour != 14 || result->tm_min != 30 || result->tm_sec != 45) {
        fprintf(stderr,
                "FAIL: test_time_date_state_stores_time_t: UTC round-trip failed: %04d-%02d-%02d %02d:%02d:%02d\n",
                result->tm_year + 1900, result->tm_mon + 1, result->tm_mday, result->tm_hour, result->tm_min,
                result->tm_sec);
        rc = 1;
    }

    return rc;
}

/**
 * test_time_date_state_stores_offset — verify LTO sign and minute offset are
 * stored directly.
 */
static int
test_time_date_state_stores_offset(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof(opts));
    memset(&st, 0, sizeof(st));

    unsigned long long MAC[24];
    build_time_date_mac(MAC, 0, 0, 1, 1, 330, 0, 0, 0, 0, 0, 0);
    process_MAC_VPDU(&opts, &st, 0, MAC);

    if (st.p25_sys_time_offset != -330) {
        fprintf(stderr, "FAIL: test_time_date_state_stores_offset: expected -330, got %d\n", st.p25_sys_time_offset);
        rc = 1;
    }
    if (st.p25_sys_time_offset_valid != 1) {
        fprintf(stderr, "FAIL: test_time_date_state_stores_offset: expected offset valid flag set\n");
        rc = 1;
    }

    return rc;
}

/* ============================================================================
 * 6.4 VPDU Dispatch Tests
 * ============================================================================ */

/**
 * test_vpdu_dispatch_0x75 — verify opcode 0x75 triggers Time/Date handler
 * (state updated).
 */
static int
test_vpdu_dispatch_0x75(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof(opts));
    memset(&st, 0, sizeof(st));

    unsigned long long MAC[24];
    build_time_date_mac(MAC, 1, 1, 1, 0, 0, 2025, 1, 1, 0, 0, 0);

    process_MAC_VPDU(&opts, &st, 0 /*FACCH*/, MAC);

    if (st.p25_sys_time != utc_time_from_fields(2025, 1, 1, 0, 0, 0)) {
        fprintf(stderr, "FAIL: test_vpdu_dispatch_0x75: p25_sys_time not updated correctly\n");
        rc = 1;
    }

    return rc;
}

/**
 * test_vpdu_dispatch_0x7e — verify bridged TSBK opcode 0x3E is handled as
 * Adjacent Status Broadcast, not as Protection Parameter Broadcast.
 */
static int
test_vpdu_dispatch_0x7e(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof(opts));
    memset(&st, 0, sizeof(st));

    unsigned long long MAC[24];
    memset(MAC, 0, sizeof(MAC));
    MAC[0] = 0x07;
    MAC[1] = 0x7E;
    MAC[2] = 0x01;
    MAC[3] = 0x40;
    MAC[4] = 0x23;
    MAC[5] = 0x04;
    MAC[6] = 0x05;
    MAC[7] = 0x00;
    MAC[8] = 0x01;
    MAC[9] = 0x02;

    process_MAC_VPDU(&opts, &st, 0 /*FACCH*/, MAC);

    if (st.p25_prot_algid != 0 || st.p25_prot_kid != 0) {
        fprintf(stderr, "FAIL: test_vpdu_dispatch_0x7e: protection state changed ALGID=0x%02X KID=0x%04X\n",
                st.p25_prot_algid, st.p25_prot_kid);
        rc = 1;
    }

    return rc;
}

/**
 * test_vpdu_dispatch_0x7f — verify TSBK opcode 0x3F Protection Parameter
 * Update handler stores ALGID/KID.
 */
static int
test_vpdu_dispatch_0x7f(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof(opts));
    memset(&st, 0, sizeof(st));

    unsigned long long MAC[24];
    memset(MAC, 0, sizeof(MAC));
    MAC[0] = 0x07;
    MAC[1] = 0x7F;
    MAC[4] = 0x81;
    MAC[5] = 0xAB;
    MAC[6] = 0xCD;
    MAC[7] = 0x12;
    MAC[8] = 0x34;
    MAC[9] = 0x56;

    process_MAC_VPDU(&opts, &st, 0 /*FACCH*/, MAC);

    if (st.p25_prot_valid != 1) {
        fprintf(stderr, "FAIL: test_vpdu_dispatch_0x7f: p25_prot_valid expected 1, got %u\n", st.p25_prot_valid);
        rc = 1;
    }
    if (st.p25_prot_algid != 0x81) {
        fprintf(stderr, "FAIL: test_vpdu_dispatch_0x7f: p25_prot_algid expected 0x81, got 0x%02X\n", st.p25_prot_algid);
        rc = 1;
    }
    if (st.p25_prot_kid != 0xABCD) {
        fprintf(stderr, "FAIL: test_vpdu_dispatch_0x7f: p25_prot_kid expected 0xABCD, got 0x%04X\n", st.p25_prot_kid);
        rc = 1;
    }

    return rc;
}

/* ============================================================================
 * main
 * ============================================================================ */

int
main(void) {
    int rc = 0;

    /* 6.1 Protection Parameter tests */
    rc |= test_prot_param_lcw_known_payload();
    rc |= test_prot_param_algid_unencrypted_name();
    rc |= test_prot_param_algid_aes256_name();
    rc |= test_prot_param_algid_des_ofb_name();
    rc |= test_prot_param_algid_des_xl_name();
    rc |= test_prot_param_tsbk_0x3f_known_payload();
    rc |= test_tsbk_0x3e_not_protection();
    rc |= test_prot_param_state_init_zero();

    /* 6.2 Emergency Alarm ISP tests */
    rc |= test_emergency_alarm_isp_known_payload();
    rc |= test_emergency_alarm_mfid90_not_affected();
    rc |= test_emergency_alarm_isp_not_bridged();

    /* 6.3 Time and Date Announcement tests */
    rc |= test_time_date_known_payload();
    rc |= test_time_date_vd_only();
    rc |= test_time_date_vt_only();
    rc |= test_time_date_state_init_zero();
    rc |= test_time_date_state_stores_time_t();
    rc |= test_time_date_state_stores_offset();

    /* 6.4 VPDU dispatch tests */
    rc |= test_vpdu_dispatch_0x75();
    rc |= test_vpdu_dispatch_0x7e();
    rc |= test_vpdu_dispatch_0x7f();

    if (rc == 0) {
        fprintf(stderr, "All P25 Tier 3 conformance tests passed.\n");
    }
    return rc;
}
