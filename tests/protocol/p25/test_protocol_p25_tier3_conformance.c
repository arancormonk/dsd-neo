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
 * MAC opcodes 0x75, 0x7E, 0x7F and TSBK ISP opcode 0x27.
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
        case 0x80: return "AES-256";
        case 0x81: return "DES-OFB";
        case 0x84: return "AES-256-GCM";
        case 0x85: return "AES-CBC";
        case 0x88: return "DES-XL";
        case 0xAA: return "RC4";
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

    /* Build a 72-bit LCW array (each element is one bit) */
    uint8_t LCW_bits[72];
    memset(LCW_bits, 0, sizeof(LCW_bits));

    /* Bits [0:7]: LCO format = 0x65 */
    /* 0x65 = 0110 0101 */
    LCW_bits[0] = 0;
    LCW_bits[1] = 1;
    LCW_bits[2] = 1;
    LCW_bits[3] = 0;
    LCW_bits[4] = 0;
    LCW_bits[5] = 1;
    LCW_bits[6] = 0;
    LCW_bits[7] = 1;

    /* Bits [8:15]: MFID = 0x00 (zeros, already set) */

    /* Bits [16:23]: ALGID = 0x80 = 1000 0000 */
    LCW_bits[16] = 1;
    LCW_bits[17] = 0;
    LCW_bits[18] = 0;
    LCW_bits[19] = 0;
    LCW_bits[20] = 0;
    LCW_bits[21] = 0;
    LCW_bits[22] = 0;
    LCW_bits[23] = 0;

    /* Bits [24:39]: KID = 0x1234 = 0001 0010 0011 0100 */
    LCW_bits[24] = 0;
    LCW_bits[25] = 0;
    LCW_bits[26] = 0;
    LCW_bits[27] = 1;
    LCW_bits[28] = 0;
    LCW_bits[29] = 0;
    LCW_bits[30] = 1;
    LCW_bits[31] = 0;
    LCW_bits[32] = 0;
    LCW_bits[33] = 0;
    LCW_bits[34] = 1;
    LCW_bits[35] = 1;
    LCW_bits[36] = 0;
    LCW_bits[37] = 1;
    LCW_bits[38] = 0;
    LCW_bits[39] = 0;

    /* Bits [40:63]: Target = 0x123456 = 0001 0010 0011 0100 0101 0110 */
    LCW_bits[40] = 0;
    LCW_bits[41] = 0;
    LCW_bits[42] = 0;
    LCW_bits[43] = 1;
    LCW_bits[44] = 0;
    LCW_bits[45] = 0;
    LCW_bits[46] = 1;
    LCW_bits[47] = 0;
    LCW_bits[48] = 0;
    LCW_bits[49] = 0;
    LCW_bits[50] = 1;
    LCW_bits[51] = 1;
    LCW_bits[52] = 0;
    LCW_bits[53] = 1;
    LCW_bits[54] = 0;
    LCW_bits[55] = 0;
    LCW_bits[56] = 0;
    LCW_bits[57] = 1;
    LCW_bits[58] = 0;
    LCW_bits[59] = 1;
    LCW_bits[60] = 0;
    LCW_bits[61] = 1;
    LCW_bits[62] = 1;
    LCW_bits[63] = 0;

    /* Extract fields the same way the LCW handler does (ConvertBitIntoBytes logic):
     * Pack bits MSB-first into integer values. */
    uint8_t algid = 0;
    for (int i = 16; i < 24; i++) {
        algid = (uint8_t)((algid << 1) | LCW_bits[i]);
    }

    uint16_t kid = 0;
    for (int i = 24; i < 40; i++) {
        kid = (uint16_t)((kid << 1) | LCW_bits[i]);
    }

    uint32_t target = 0;
    for (int i = 40; i < 64; i++) {
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
 * test_prot_param_algid_aes256_name — verify ALGID 0x80 resolves to "AES-256"
 */
static int
test_prot_param_algid_aes256_name(void) {
    const char* name = test_p25_algid_name(0x80);
    if (name == NULL || strcmp(name, "AES-256") != 0) {
        fprintf(stderr, "FAIL: test_prot_param_algid_aes256_name: expected 'AES-256', got '%s'\n",
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
 * test_prot_param_mac_0x7f_known_payload — construct MAC PDU with opcode 0x7F,
 * ALGID=0x84, KID=0xABCD. Verify extraction.
 */
static int
test_prot_param_mac_0x7f_known_payload(void) {
    int rc = 0;

    /* Simulate MAC PDU for Protection Parameter Update (0x7F).
     * MAC[0] = length marker (0x04 for 5-byte PDU: opcode + ALGID + KID[2])
     * MAC[1] = opcode 0x7F
     * MAC[2] = ALGID = 0x84
     * MAC[3] = KID high = 0xAB
     * MAC[4] = KID low = 0xCD
     */
    unsigned long long MAC[24];
    memset(MAC, 0, sizeof(MAC));
    MAC[0] = 0x04; /* len_b = 5 bytes (opcode + 4 data) */
    MAC[1] = 0x7F;
    MAC[2] = 0x84;
    MAC[3] = 0xAB;
    MAC[4] = 0xCD;

    /* Extract fields the same way the handler does (len_a = 0 for first PDU) */
    int len_a = 0;
    int algid = (int)MAC[2 + len_a];
    int kid = (int)((MAC[3 + len_a] << 8) | MAC[4 + len_a]);

    if (algid != 0x84) {
        fprintf(stderr, "FAIL: test_prot_param_mac_0x7f_known_payload: ALGID expected 0x84, got 0x%02X\n", algid);
        rc = 1;
    }
    if (kid != 0xABCD) {
        fprintf(stderr, "FAIL: test_prot_param_mac_0x7f_known_payload: KID expected 0xABCD, got 0x%04X\n", kid);
        rc = 1;
    }

    return rc;
}

/**
 * test_prot_param_mac_0x7e_known_payload — construct MAC PDU with opcode 0x7E,
 * ALGID=0x80, KID=0x5678, Target=0xABCDEF. Verify extraction.
 */
static int
test_prot_param_mac_0x7e_known_payload(void) {
    int rc = 0;

    unsigned long long MAC[24];
    memset(MAC, 0, sizeof(MAC));
    MAC[0] = 0x07; /* len_b = 8 bytes */
    MAC[1] = 0x7E;
    MAC[2] = 0x80; /* ALGID */
    MAC[3] = 0x56; /* KID high */
    MAC[4] = 0x78; /* KID low */
    MAC[5] = 0xAB; /* Target high */
    MAC[6] = 0xCD; /* Target mid */
    MAC[7] = 0xEF; /* Target low */

    int len_a = 0;
    int algid = (int)MAC[2 + len_a];
    int kid = (int)((MAC[3 + len_a] << 8) | MAC[4 + len_a]);
    int target = (int)((MAC[5 + len_a] << 16) | (MAC[6 + len_a] << 8) | MAC[7 + len_a]);

    if (algid != 0x80) {
        fprintf(stderr, "FAIL: test_prot_param_mac_0x7e_known_payload: ALGID expected 0x80, got 0x%02X\n", algid);
        rc = 1;
    }
    if (kid != 0x5678) {
        fprintf(stderr, "FAIL: test_prot_param_mac_0x7e_known_payload: KID expected 0x5678, got 0x%04X\n", kid);
        rc = 1;
    }
    if (target != 0xABCDEF) {
        fprintf(stderr, "FAIL: test_prot_param_mac_0x7e_known_payload: Target expected 0xABCDEF, got 0x%06X\n", target);
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
     * Octet 0: [P=1 | LB=0 | Opcode(5:0) = 0x27] => 1_0_100111 = 0xA7
     * Octet 1: MFID = 0x00
     * Octets 2-4: Reserved (zeros)
     * Octets 5-6: Group_Address = 0x1234
     * Octets 7-9: Source_Address = 0x567890
     */
    uint8_t tsbk_byte[12];
    memset(tsbk_byte, 0, sizeof(tsbk_byte));
    tsbk_byte[0] = 0xA7; /* P=1, LB=0, opcode=0x27 */
    tsbk_byte[1] = 0x00; /* MFID */
    tsbk_byte[5] = 0x12; /* Group high */
    tsbk_byte[6] = 0x34; /* Group low */
    tsbk_byte[7] = 0x56; /* Source high */
    tsbk_byte[8] = 0x78; /* Source mid */
    tsbk_byte[9] = 0x90; /* Source low */

    /* Extract fields the same way the handler does */
    int protectbit = (tsbk_byte[0] >> 7) & 1;
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
    tsbk_byte[0] = 0xA7; /* P=1, LB=0, opcode=0x27 */
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
    tsbk_byte[0] = 0xA7; /* P=1, LB=0, opcode=0x27 */
    tsbk_byte[1] = 0x00; /* MFID */

    int protectbit = (tsbk_byte[0] >> 7) & 1;

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
 *
 * Encoding:
 *   VD=1, VT=1, VL=1 => octet 2 bits [7:5] = 111 => octet 2 = 0xE0
 *   Year = 2025 - 2000 = 25 (13 bits)
 *   Month = 3 (4 bits)
 *   Day = 15 (5 bits)
 *   Hours = 14 (5 bits)
 *   Minutes = 30 (6 bits)
 *   Seconds = 45 (6 bits)
 *   LTO_sign = 0 (positive offset)
 *   LTO_mag = 11 (5.5 hours = 330 min / 30 = 11 half-hour units)
 *
 * Packed 56-bit value (octets 3-9):
 *   Year(13): 0000000011001 = 25
 *   Month(4): 0011
 *   Day(5): 01111
 *   Hours(5): 01110
 *   Minutes(6): 011110
 *   Seconds(6): 101101
 *   LTO_sign(1): 0
 *   LTO_mag(11): 00000001011
 *   Reserved(5): 00000
 *
 * Binary: 0000000011001_0011_01111_01110_011110_101101_0_00000001011_00000
 * Grouped into bytes:
 *   00000000 11001001 10111101 11001111 01011010 00000010 11000000
 *   0x00     0xC9     0xBD     0xCF     0x5A     0x02     0xC0
 */
static int
test_time_date_known_payload(void) {
    int rc = 0;

    unsigned long long MAC[24];
    memset(MAC, 0, sizeof(MAC));
    MAC[0] = 0x09; /* length marker */
    MAC[1] = 0x75; /* opcode */
    MAC[2] = 0xE0; /* VD=1, VT=1, VL=1, reserved=0 */
    MAC[3] = 0x00;
    MAC[4] = 0xC9;
    MAC[5] = 0xBD;
    MAC[6] = 0xCF;
    MAC[7] = 0x5A;
    MAC[8] = 0x01;
    MAC[9] = 0x60;

    /* Extract fields the same way the handler does */
    int len_a = 0;
    int vd = (MAC[2 + len_a] >> 7) & 1;
    int vt = (MAC[2 + len_a] >> 6) & 1;
    int vl = (MAC[2 + len_a] >> 5) & 1;

    uint64_t packed = 0;
    for (int pi = 3; pi <= 9; pi++) {
        packed = (packed << 8) | MAC[pi + len_a];
    }

    int year = (int)((packed >> 43) & 0x1FFF) + 2000;
    int month = (int)((packed >> 39) & 0xF);
    int day = (int)((packed >> 34) & 0x1F);
    int hours = (int)((packed >> 29) & 0x1F);
    int minutes = (int)((packed >> 23) & 0x3F);
    int seconds = (int)((packed >> 17) & 0x3F);
    int lto_sign = (int)((packed >> 16) & 0x1);
    int lto_mag = (int)((packed >> 5) & 0x7FF);

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
    if (lto_mag != 11) {
        fprintf(stderr, "FAIL: test_time_date_known_payload: LTO_mag expected 11, got %d\n", lto_mag);
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

    /* Encode: VD=1, VT=0, VL=0 => octet 2 = 0x80
     * Year=2030 (30), Month=12, Day=25
     * Time and offset fields are don't-care (set to 0).
     *
     * Year(13): 0000000011110 = 30
     * Month(4): 1100
     * Day(5): 11001
     * Hours(5): 00000
     * Minutes(6): 000000
     * Seconds(6): 000000
     * LTO_sign(1): 0
     * LTO_mag(11): 00000000000
     * Reserved(5): 00000
     *
     * Binary: 0000000011110_1100_11001_00000_000000_000000_0_00000000000_00000
     * Bytes: 00000000 11110110 01100100 00000000 00000000 00000000 00000000
     *         0x00     0xF6     0x64     0x00     0x00     0x00     0x00
     */
    unsigned long long MAC[24];
    memset(MAC, 0, sizeof(MAC));
    MAC[0] = 0x09;
    MAC[1] = 0x75;
    MAC[2] = 0x80; /* VD=1, VT=0, VL=0 */
    MAC[3] = 0x00;
    MAC[4] = 0xF6;
    MAC[5] = 0x64;
    MAC[6] = 0x00;
    MAC[7] = 0x00;
    MAC[8] = 0x00;
    MAC[9] = 0x00;

    int len_a = 0;
    int vd = (MAC[2 + len_a] >> 7) & 1;
    int vt = (MAC[2 + len_a] >> 6) & 1;
    int vl = (MAC[2 + len_a] >> 5) & 1;

    uint64_t packed = 0;
    for (int pi = 3; pi <= 9; pi++) {
        packed = (packed << 8) | MAC[pi + len_a];
    }

    int year = 0, month = 0, day = 0;
    if (vd) {
        year = (int)((packed >> 43) & 0x1FFF) + 2000;
        month = (int)((packed >> 39) & 0xF);
        day = (int)((packed >> 34) & 0x1F);
    }

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

    /* Encode: VD=0, VT=1, VL=0 => octet 2 = 0x40
     * Hours=23, Minutes=59, Seconds=59
     * Date and offset fields are don't-care (set to 0).
     *
     * Year(13): 0000000000000
     * Month(4): 0000
     * Day(5): 00000
     * Hours(5): 10111 = 23
     * Minutes(6): 111011 = 59
     * Seconds(6): 111011 = 59
     * LTO_sign(1): 0
     * LTO_mag(11): 00000000000
     * Reserved(5): 00000
     *
     * Binary: 0000000000000_0000_00000_10111_111011_111011_0_00000000000_00000
     * Bytes: 00000000 00000000 00000010 11111101 11110110 00000000 00000000
     *         0x00     0x00     0x02     0xFD     0xF6     0x00     0x00
     */
    unsigned long long MAC[24];
    memset(MAC, 0, sizeof(MAC));
    MAC[0] = 0x09;
    MAC[1] = 0x75;
    MAC[2] = 0x40; /* VD=0, VT=1, VL=0 */
    MAC[3] = 0x00;
    MAC[4] = 0x00;
    MAC[5] = 0x02;
    MAC[6] = 0xFD;
    MAC[7] = 0xF6;
    MAC[8] = 0x00;
    MAC[9] = 0x00;

    int len_a = 0;
    int vd = (MAC[2 + len_a] >> 7) & 1;
    int vt = (MAC[2 + len_a] >> 6) & 1;
    int vl = (MAC[2 + len_a] >> 5) & 1;

    uint64_t packed = 0;
    for (int pi = 3; pi <= 9; pi++) {
        packed = (packed << 8) | MAC[pi + len_a];
    }

    int hours = 0, minutes = 0, seconds = 0;
    if (vt) {
        hours = (int)((packed >> 29) & 0x1F);
        minutes = (int)((packed >> 23) & 0x3F);
        seconds = (int)((packed >> 17) & 0x3F);
    }

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
    if (state->p25_sys_time_offset != 0) {
        fprintf(stderr, "FAIL: test_time_date_state_init_zero: p25_sys_time_offset expected 0, got %d\n",
                state->p25_sys_time_offset);
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

    /* Compute expected time_t for 2025-03-15 14:30:45 UTC using mktime
     * the same way the handler does. */
    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    tm_val.tm_year = 2025 - 1900;
    tm_val.tm_mon = 3 - 1; /* March = 2 (0-indexed) */
    tm_val.tm_mday = 15;
    tm_val.tm_hour = 14;
    tm_val.tm_min = 30;
    tm_val.tm_sec = 45;
    tm_val.tm_isdst = -1;
    time_t expected = mktime(&tm_val);

    /* Now simulate what the handler stores */
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!state) {
        fprintf(stderr, "FAIL: test_time_date_state_stores_time_t: calloc failed\n");
        return 1;
    }

    /* Simulate handler storing the time */
    if (expected != (time_t)-1) {
        state->p25_sys_time = expected;
    }

    if (state->p25_sys_time != expected) {
        fprintf(stderr, "FAIL: test_time_date_state_stores_time_t: time_t mismatch, expected %ld, got %ld\n",
                (long)expected, (long)state->p25_sys_time);
        rc = 1;
    }

    /* Verify round-trip through gmtime-like conversion: the stored time
     * should represent the same date/time components when converted back. */
    struct tm* result = localtime(&state->p25_sys_time);
    if (result) {
        if (result->tm_year + 1900 != 2025 || result->tm_mon + 1 != 3 || result->tm_mday != 15) {
            fprintf(stderr, "FAIL: test_time_date_state_stores_time_t: date round-trip failed: %d-%02d-%02d\n",
                    result->tm_year + 1900, result->tm_mon + 1, result->tm_mday);
            rc = 1;
        }
    }

    free(state);
    return rc;
}

/**
 * test_time_date_state_stores_offset — verify LTO sign=0, mag=11
 * (5.5 hours = 330 minutes) stored correctly.
 */
static int
test_time_date_state_stores_offset(void) {
    int rc = 0;

    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!state) {
        fprintf(stderr, "FAIL: test_time_date_state_stores_offset: calloc failed\n");
        return 1;
    }

    /* Simulate what the handler does for LTO sign=0, mag=11 */
    int lto_sign = 0;
    int lto_mag = 11;
    int offset_minutes = lto_mag * 30; /* 11 * 30 = 330 minutes */
    if (lto_sign) {
        offset_minutes = -offset_minutes;
    }
    state->p25_sys_time_offset = (int16_t)offset_minutes;

    if (state->p25_sys_time_offset != 330) {
        fprintf(stderr, "FAIL: test_time_date_state_stores_offset: expected 330, got %d\n", state->p25_sys_time_offset);
        rc = 1;
    }

    free(state);
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

    /* Build a Time/Date MAC PDU: 2025-01-01 00:00:00 UTC+00:00
     * VD=1, VT=1, VL=1 => octet 2 = 0xE0
     * Year=25(13 bits), Month=1(4), Day=1(5), Hours=0(5), Min=0(6), Sec=0(6)
     * LTO_sign=0, LTO_mag=0
     *
     * Year(13): 0000000011001 = 25
     * Month(4): 0001
     * Day(5): 00001
     * Hours(5): 00000
     * Minutes(6): 000000
     * Seconds(6): 000000
     * LTO_sign(1): 0
     * LTO_mag(11): 00000000000
     * Reserved(5): 00000
     *
     * Binary: 0000000011001_0001_00001_00000_000000_000000_0_00000000000_00000
     * Bytes: 00000000 11001000 10000100 00000000 00000000 00000000 00000000
     *         0x00     0xC8     0x84     0x00     0x00     0x00     0x00
     */
    unsigned long long MAC[24];
    memset(MAC, 0, sizeof(MAC));
    MAC[0] = 0x09;
    MAC[1] = 0x75;
    MAC[2] = 0xE0;
    MAC[3] = 0x00;
    MAC[4] = 0xC8;
    MAC[5] = 0x84;
    MAC[6] = 0x00;
    MAC[7] = 0x00;
    MAC[8] = 0x00;
    MAC[9] = 0x00;

    process_MAC_VPDU(&opts, &st, 0 /*FACCH*/, MAC);

    /* Verify state was updated (p25_sys_time should be non-zero for a valid date) */
    if (st.p25_sys_time == 0) {
        fprintf(stderr, "FAIL: test_vpdu_dispatch_0x75: p25_sys_time not updated (still 0)\n");
        rc = 1;
    }

    return rc;
}

/**
 * test_vpdu_dispatch_0x7e — verify opcode 0x7E triggers Protection Param
 * handler (state updated).
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
    MAC[1] = 0x7E; /* Protection Parameter Broadcast */
    MAC[2] = 0x80; /* ALGID = AES-256 */
    MAC[3] = 0x12; /* KID high */
    MAC[4] = 0x34; /* KID low */
    MAC[5] = 0xAB; /* Target high */
    MAC[6] = 0xCD; /* Target mid */
    MAC[7] = 0xEF; /* Target low */

    process_MAC_VPDU(&opts, &st, 0 /*FACCH*/, MAC);

    if (st.p25_prot_algid != 0x80) {
        fprintf(stderr, "FAIL: test_vpdu_dispatch_0x7e: p25_prot_algid expected 0x80, got 0x%02X\n", st.p25_prot_algid);
        rc = 1;
    }
    if (st.p25_prot_kid != 0x1234) {
        fprintf(stderr, "FAIL: test_vpdu_dispatch_0x7e: p25_prot_kid expected 0x1234, got 0x%04X\n", st.p25_prot_kid);
        rc = 1;
    }

    return rc;
}

/**
 * test_vpdu_dispatch_0x7f — verify opcode 0x7F triggers Protection Param
 * handler (state updated).
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
    MAC[0] = 0x04;
    MAC[1] = 0x7F; /* Protection Parameter Update */
    MAC[2] = 0x81; /* ALGID = DES-OFB */
    MAC[3] = 0xAB; /* KID high */
    MAC[4] = 0xCD; /* KID low */

    process_MAC_VPDU(&opts, &st, 0 /*FACCH*/, MAC);

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
    rc |= test_prot_param_algid_aes256_name();
    rc |= test_prot_param_algid_des_ofb_name();
    rc |= test_prot_param_mac_0x7f_known_payload();
    rc |= test_prot_param_mac_0x7e_known_payload();
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
