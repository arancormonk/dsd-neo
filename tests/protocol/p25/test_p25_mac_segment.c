// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Verify MAC VPDU length inference from MCO for unknown opcode and capacity capping.
 */

#include <dsd-neo/core/gps.h>
#include <dsd-neo/protocol/p25/p25p2_mac_parse.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "test_env.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

#define setenv dsd_test_setenv

// Forward declare config init to keep test dependencies narrow.
void dsd_neo_config_init(void);

// Test shim entrypoint implemented in tests/test_support/p25_test_shim.c.
void p25_test_process_mac_vpdu_ex(int type, const unsigned char* mac_bytes, int mac_len, int is_lcch, int currentslot);

// Minimal stubs to satisfy linked objects from the P25 proto library
typedef struct dsd_opts dsd_opts;
typedef struct dsd_state dsd_state;

static int g_apx_alias_header_calls;
static int g_l3h_alias_calls;
static int g_nmea_harris_calls;
static uint8_t g_nmea_harris_body[DSD_HARRIS_GPS_BODY_OCTETS];

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
    g_apx_alias_header_calls++;
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
    g_l3h_alias_calls++;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(const dsd_opts* opts, dsd_state* state, const uint8_t* body, const dsd_gps_report_ctx* ctx) {
    (void)opts;
    (void)state;
    (void)ctx;
    g_nmea_harris_calls++;
    if (body) {
        DSD_MEMCPY(g_nmea_harris_body, body, sizeof g_nmea_harris_body);
    }
    return 0;
}

static int
parse_len_fields(const char* s, int* lenB, int* lenC) {
    const char* p = strstr(s, "\"lenB\":");
    const char* q = strstr(s, "\"lenC\":");
    if (!p || !q) {
        return -1;
    }
    errno = 0;
    char* end_b = NULL;
    char* end_c = NULL;
    long vb = strtol(p + 7, &end_b, 10);
    long vc = strtol(q + 7, &end_c, 10);
    if (end_b == (p + 7) || end_c == (q + 7) || errno == ERANGE || vb < INT_MIN || vb > INT_MAX || vc < INT_MIN
        || vc > INT_MAX) {
        return -1;
    }
    *lenB = (int)vb;
    *lenC = (int)vc;
    return 0;
}

static int
run_case(int type, uint8_t opcode, int expectB, int expectC) {
    // Ensure JSON output is enabled
    setenv("DSD_NEO_PDU_JSON", "1", 1);
    dsd_neo_config_init();

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "p25_mac_segment") != 0) {
        DSD_FPRINTF(stderr, "Failed to capture stderr: %s\n", strerror(errno));
        return 100;
    }

    unsigned char mac[24];
    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[0] = 1;      // mark header present so MCO heuristic applies on FACCH
    mac[1] = opcode; // opcode with low 6 bits interpreted as MCO
    p25_test_process_mac_vpdu_ex(type, mac, 24, 0, 0);

    dsd_test_capture_stderr_end(&cap);

    // Read file back
    FILE* f = fopen(cap.path, "r");
    if (!f) {
        DSD_FPRINTF(stderr, "Failed to open %s\n", cap.path);
        return 101;
    }
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    int lenB = -1, lenC = -1;
    if (parse_len_fields(buf, &lenB, &lenC) != 0) {
        DSD_FPRINTF(stderr, "JSON parse failed: %s\n", buf);
        return 102;
    }
    if (lenB != expectB) {
        DSD_FPRINTF(stderr, "lenB mismatch type=%d op=%02X got B=%d want B=%d (C=%d)\n", type, opcode, lenB, expectB,
                    lenC);
        return 103;
    }
    if (lenC != expectC) {
        DSD_FPRINTF(stderr, "lenC mismatch type=%d op=%02X got C=%d want C=%d\n", type, opcode, lenC, expectC);
        return 104;
    }
    (void)remove(cap.path);
    return 0;
}

static int
run_payload_len_case(const char* tag, int type, uint8_t opcode, uint8_t mfid, uint8_t payload_len, int expectB,
                     int expectC) {
    setenv("DSD_NEO_PDU_JSON", "1", 1);
    dsd_neo_config_init();

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, tag) != 0) {
        DSD_FPRINTF(stderr, "Failed to capture stderr: %s\n", strerror(errno));
        return 200;
    }

    unsigned char mac[24];
    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[0] = 1;
    mac[1] = opcode;
    mac[2] = mfid;
    mac[3] = payload_len;
    p25_test_process_mac_vpdu_ex(type, mac, 24, 0, 0);

    dsd_test_capture_stderr_end(&cap);

    FILE* f = fopen(cap.path, "r");
    if (!f) {
        DSD_FPRINTF(stderr, "Failed to open %s\n", cap.path);
        return 201;
    }
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    int lenB = -1, lenC = -1;
    if (parse_len_fields(buf, &lenB, &lenC) != 0) {
        DSD_FPRINTF(stderr, "%s JSON parse failed: %s\n", tag, buf);
        return 202;
    }
    if (lenB != expectB) {
        DSD_FPRINTF(stderr, "%s lenB mismatch got B=%d want B=%d (C=%d)\n", tag, lenB, expectB, lenC);
        return 203;
    }
    if (lenC != expectC) {
        DSD_FPRINTF(stderr, "%s lenC mismatch got C=%d want C=%d\n", tag, lenC, expectC);
        return 204;
    }
    (void)remove(cap.path);
    return 0;
}

static int
run_tdma_paging_len_case(const char* tag, uint8_t opcode, uint8_t count_bits, int expectB, int expectC) {
    setenv("DSD_NEO_PDU_JSON", "1", 1);
    dsd_neo_config_init();

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, tag) != 0) {
        DSD_FPRINTF(stderr, "Failed to capture stderr: %s\n", strerror(errno));
        return 300;
    }

    unsigned char mac[24];
    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = opcode;
    mac[2] = count_bits;
    if ((2 + expectB) < (int)sizeof(mac)) {
        mac[1 + expectB] = 0x30; // following fixed-length MAC structure proves alignment
        mac[2 + expectB] = 0x00;
    }
    p25_test_process_mac_vpdu_ex(1 /* SACCH */, mac, 24, 0, 0);

    dsd_test_capture_stderr_end(&cap);

    FILE* f = fopen(cap.path, "r");
    if (!f) {
        DSD_FPRINTF(stderr, "Failed to open %s\n", cap.path);
        return 301;
    }
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    int lenB = -1, lenC = -1;
    if (parse_len_fields(buf, &lenB, &lenC) != 0) {
        DSD_FPRINTF(stderr, "%s JSON parse failed: %s\n", tag, buf);
        return 302;
    }
    if (lenB != expectB) {
        DSD_FPRINTF(stderr, "%s lenB mismatch got B=%d want B=%d (C=%d)\n", tag, lenB, expectB, lenC);
        return 303;
    }
    if (lenC != expectC) {
        DSD_FPRINTF(stderr, "%s lenC mismatch got C=%d want C=%d\n", tag, lenC, expectC);
        return 304;
    }
    (void)remove(cap.path);
    return 0;
}

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

/* Each transport's MAC message span: FACCH 17, SACCH 20, LCCH 18 octets after the MAC header (TIA-102.BBAC bit
   counts: FACCH 8 + 136 + CRC-12; SACCH 8 + 160 + CRC-12; LCCH 8 + 144 + NAC 12 + CRC-16). */
static int
run_transport_span_cases(void) {
    unsigned long long mac[24] = {0};
    struct p25p2_mac_result res;
    int rc = 0;

    rc |= expect_int("span FACCH", p25p2_mac_span(0, 0), 17);
    rc |= expect_int("span SACCH", p25p2_mac_span(1, 0), 20);
    rc |= expect_int("span LCCH", p25p2_mac_span(1, 1), 18);
    rc |= expect_int("span FACCH ignores LCCH flag", p25p2_mac_span(0, 1), 17);

    const int spans[] = {P25P2_MAC_SPAN_FACCH, P25P2_MAC_SPAN_SACCH, P25P2_MAC_SPAN_LCCH};
    for (size_t i = 0; i < sizeof spans / sizeof spans[0]; i++) {
        DSD_MEMSET(mac, 0, sizeof(mac));
        if (p25p2_mac_parse_span(1, spans[i], mac, &res) != 0) {
            return 410;
        }
        rc |= expect_int("span null fill count", res.segment_count, 1);
        rc |= expect_int("span null fill len", res.segments[0].length, spans[i]);
        rc |= expect_int("span recorded", res.span, spans[i]);

        // A Motorola Group Regroup Add that ends exactly on the span is one segment; the next octet is outside.
        DSD_MEMSET(mac, 0, sizeof(mac));
        mac[1] = 0x30;
        mac[6] = 0x81;
        mac[7] = 0x90;
        mac[8] = (unsigned long long)(spans[i] - 5);
        if (p25p2_mac_parse_span(1, spans[i], mac, &res) != 0) {
            return 411;
        }
        rc |= expect_int("span exact later segment count", res.segment_count, 2);
        rc |= expect_int("span exact later segment len", res.segments[1].length, spans[i] - 5);
        mac[8] = (unsigned long long)(spans[i] - 4);
        if (p25p2_mac_parse_span(1, spans[i], mac, &res) != 0) {
            return 412;
        }
        rc |= expect_int("span overflowing later segment dropped", res.segment_count, 1);
    }

    rc |= expect_int("span zero rejected", p25p2_mac_parse_span(1, 0, mac, &res), -1);
    rc |= expect_int("span past buffer rejected", p25p2_mac_parse_span(1, 24, mac, &res), -1);
    return rc;
}

/* A native message's next one starts where its TIA-102.BBAD-A length ends: Unit to Unit Answer Request 0x45 / 0xC5
   is 8 / 12 octets and Telephone Interconnect Answer Request 0x4A is 9, each here followed by a 7-octet Call Alert. */
static int
run_answer_request_length_cases(void) {
    static const struct {
        const char* tag;
        uint8_t opcode;
        int len;
    } cases[] = {
        {"uu answer abbreviated", 0x45, 8},
        {"uu answer extended", 0xC5, 12},
        {"telephone answer", 0x4A, 9},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        unsigned long long mac[24] = {0};
        struct p25p2_mac_result res;
        char tag[96];
        mac[1] = cases[i].opcode;
        mac[1 + cases[i].len] = 0x5F;
        if (p25p2_mac_parse(1, mac, &res) != 0) {
            return 410;
        }
        DSD_SNPRINTF(tag, sizeof tag, "%s first len", cases[i].tag);
        rc |= expect_int(tag, res.segments[0].length, cases[i].len);
        DSD_SNPRINTF(tag, sizeof tag, "%s next offset", cases[i].tag);
        rc |= expect_int(tag, res.segment_count >= 2 ? res.segments[1].offset : -1, cases[i].len);
        DSD_SNPRINTF(tag, sizeof tag, "%s next len", cases[i].tag);
        rc |= expect_int(tag, res.segment_count >= 2 ? res.segments[1].length : -1, 7);
    }
    return rc;
}

/* Issue #800 (R3-2): the legacy shifted L3Harris GPS message `80 AA A4 11` is 18 octets, the 0x80 octet plus the 17
   its length octet counts from the MCO octet `AA`; its last octet (GPS quality) is no next segment's opcode. */
static int
run_shifted_harris_length_cases(void) {
    unsigned long long mac[24];
    struct p25p2_mac_result res;
    int rc = 0;

    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x80;
    mac[2] = 0xAA;
    mac[3] = 0xA4;
    mac[4] = 0x11;
    mac[18] = 0x1C;
    if (p25p2_mac_parse(1, mac, &res) != 0) {
        return 413;
    }
    rc |= expect_int("shifted harris len", res.segments[0].length, 18);
    rc |= expect_int("shifted harris next offset", res.segments[1].offset, 18);
    rc |= expect_int("shifted harris next len", res.segments[1].length, 2);

    // After a 3-octet segment, 18 octets end at MAC[21]: past the SACCH span, so the segment is not taken.
    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x08;
    mac[2] = 0x03;
    mac[4] = 0x80;
    mac[5] = 0xAA;
    mac[6] = 0xA4;
    mac[7] = 0x11;
    if (p25p2_mac_parse(1, mac, &res) != 0) {
        return 414;
    }
    rc |= expect_int("shifted harris past span count", res.segment_count, 1);

    return rc;
}

static int
run_direct_segment_parse_cases(void) {
    unsigned long long mac[24] = {0};
    struct p25p2_mac_result res;
    int rc = 0;

    mac[1] = 0x30;
    mac[6] = 0x30;
    mac[11] = 0x30;
    if (p25p2_mac_parse(1, mac, &res) != 0) {
        return 400;
    }
    rc |= expect_int("three segment count", res.segment_count, 3);
    rc |= expect_int("three segment offset 0", res.segments[0].offset, 0);
    rc |= expect_int("three segment len 0", res.segments[0].length, 5);
    rc |= expect_int("three segment offset 1", res.segments[1].offset, 5);
    rc |= expect_int("three segment len 1", res.segments[1].length, 5);
    rc |= expect_int("three segment offset 2", res.segments[2].offset, 10);
    rc |= expect_int("three segment len 2", res.segments[2].length, 5);

    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x82;
    mac[2] = 0xA4;
    mac[3] = 0x05;
    mac[6] = 0x30;
    mac[11] = 0x30;
    if (p25p2_mac_parse(1, mac, &res) != 0) {
        return 401;
    }
    rc |= expect_int("generic harris length count", res.segment_count, 3);
    rc |= expect_int("generic harris offset 0", res.segments[0].offset, 0);
    rc |= expect_int("generic harris len 0", res.segments[0].length, 5);
    rc |= expect_int("generic harris offset 1", res.segments[1].offset, 5);
    rc |= expect_int("generic harris len 1", res.segments[1].length, 5);
    rc |= expect_int("generic harris offset 2", res.segments[2].offset, 10);
    rc |= expect_int("generic harris len 2", res.segments[2].length, 5);

    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x88;
    mac[2] = 0xA4;
    mac[3] = 0x08;
    mac[9] = 0x30;
    mac[14] = 0x30;
    if (p25p2_mac_parse(1, mac, &res) != 0) {
        return 409;
    }
    rc |= expect_int("harris collision count", res.segment_count, 3);
    rc |= expect_int("harris collision len 0", res.segments[0].length, 8);
    rc |= expect_int("harris collision offset 1", res.segments[1].offset, 8);
    rc |= expect_int("harris collision len 1", res.segments[1].length, 5);
    rc |= expect_int("harris collision offset 2", res.segments[2].offset, 13);
    rc |= expect_int("harris collision len 2", res.segments[2].length, 5);

    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x00;
    if (p25p2_mac_parse(1, mac, &res) != 0) {
        return 406;
    }
    rc |= expect_int("null fills remaining count", res.segment_count, 1);
    rc |= expect_int("null fills remaining len", res.segments[0].length, P25P2_MAC_SPAN_SACCH);

    const uint8_t bridged_ops[] = {0x69U, 0x7FU, 0xEFU};
    for (size_t i = 0; i < sizeof(bridged_ops) / sizeof(bridged_ops[0]); i++) {
        DSD_MEMSET(mac, 0, sizeof(mac));
        mac[1] = 0x30;
        mac[6] = bridged_ops[i];
        if (p25p2_mac_parse(1, mac, &res) != 0) {
            return 408;
        }
        rc |= expect_int("bridged later segment count", res.segment_count, 2);
        rc |= expect_int("bridged later segment offset", res.segments[1].offset, 5);
        rc |= expect_int("bridged later segment len", res.segments[1].length, P25P2_MAC_SPAN_SACCH - 5);
    }

    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x23;
    if (p25p2_mac_parse(0, mac, &res) != 0) {
        return 407;
    }
    rc |= expect_int("unsupported first count", res.segment_count, 0);

    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0xA0;
    mac[2] = 0xA4;
    mac[3] = 0x2A;
    mac[10] = 0x31;
    if (p25p2_mac_parse(0, mac, &res) != 0) {
        return 402;
    }
    rc |= expect_int("fixed harris count", res.segment_count, 3);
    rc |= expect_int("fixed harris len", res.segments[0].length, 9);
    rc |= expect_int("fixed harris next offset", res.segments[1].offset, 9);
    rc |= expect_int("fixed harris next len", res.segments[1].length, 7);
    // FACCH's 17th MAC octet (MAC[17]) is message area: null fill, not part of the CRC.
    rc |= expect_int("fixed harris fill offset", res.segments[2].offset, 16);
    rc |= expect_int("fixed harris fill len", res.segments[2].length, 1);

    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x30;
    mac[6] = 0xA8;
    mac[7] = 0xA4;
    mac[8] = 0x06;
    mac[12] = 0x30;
    if (p25p2_mac_parse(1, mac, &res) != 0) {
        return 403;
    }
    rc |= expect_int("vendor segment count", res.segment_count, 3);
    rc |= expect_int("vendor second offset", res.segments[1].offset, 5);
    rc |= expect_int("vendor second len", res.segments[1].length, 6);
    rc |= expect_int("vendor third offset", res.segments[2].offset, 11);
    rc |= expect_int("vendor third len", res.segments[2].length, 5);

    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x30;
    mac[6] = 0x80;
    mac[7] = 0xAA;
    mac[8] = 0xA4;
    mac[9] = 0x11;
    mac[23] = 0x30;
    if (p25p2_mac_parse(1, mac, &res) != 0) {
        return 404;
    }
    rc |= expect_int("truncated shifted harris count", res.segment_count, 1);

    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x30;
    mac[6] = 0x30;
    mac[11] = 0xE4;
    if (p25p2_mac_parse(1, mac, &res) != 0) {
        return 405;
    }
    rc |= expect_int("truncated third segment count", res.segment_count, 2);

    return rc | run_transport_span_cases() | run_answer_request_length_cases() | run_shifted_harris_length_cases();
}

static int
run_voice_identity_parse_cases(void) {
    unsigned long long mac[24] = {0};
    struct p25p2_mac_voice_identity identity;
    int rc = 0;

    mac[1] = 0x01;
    mac[2] = 0x81;
    mac[3] = 0x45;
    mac[4] = 0x67;
    mac[5] = 0x12;
    mac[6] = 0x34;
    mac[7] = 0x56;
    rc |= expect_int("group voice identity present", p25p2_mac_decode_voice_identity(1, mac, &identity), 1);
    rc |= expect_int("group voice tg", identity.tg, 0x4567);
    rc |= expect_int("group voice dst", identity.dst, 0);
    rc |= expect_int("group voice src", identity.src, 0x123456);
    rc |= expect_int("group voice type", identity.is_group, 1);
    rc |= expect_int("group voice service", identity.svc_bits, 0x81);
    rc |= expect_int("group voice source required", identity.source_optional, 0);

    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x30;
    mac[6] = 0x02;
    mac[7] = 0x42;
    mac[8] = 0xAB;
    mac[9] = 0xCD;
    mac[10] = 0xEF;
    mac[11] = 0x65;
    mac[12] = 0x43;
    mac[13] = 0x21;
    rc |= expect_int("private voice identity present", p25p2_mac_decode_voice_identity(1, mac, &identity), 1);
    rc |= expect_int("private voice tg", identity.tg, 0);
    rc |= expect_int("private voice dst", identity.dst, 0xABCDEF);
    rc |= expect_int("private voice src", identity.src, 0x654321);
    rc |= expect_int("private voice type", identity.is_group, 0);
    rc |= expect_int("private voice service", identity.svc_bits, 0x42);
    rc |= expect_int("private voice source required", identity.source_optional, 0);

    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x03;
    mac[2] = 0x44;
    mac[3] = 0x01;
    mac[4] = 0xF4;
    mac[5] = 0xAB;
    mac[6] = 0xCD;
    mac[7] = 0xEF;
    rc |= expect_int("telephone voice identity present", p25p2_mac_decode_voice_identity(1, mac, &identity), 1);
    rc |= expect_int("telephone voice tg", identity.tg, 0);
    rc |= expect_int("telephone voice dst", identity.dst, 0xABCDEF);
    rc |= expect_int("telephone voice src", identity.src, 0);
    rc |= expect_int("telephone voice type", identity.is_group, 0);
    rc |= expect_int("telephone voice service", identity.svc_bits, 0x44);
    rc |= expect_int("telephone voice source optional", identity.source_optional, 1);

    for (int mfid = 0; mfid <= 1; mfid++) {
        DSD_MEMSET(mac, 0, sizeof(mac));
        mac[1] = 0x90;
        mac[2] = (unsigned long long)mfid;
        mac[3] = 0x45;
        mac[4] = 0x67;
        mac[5] = 0x12;
        mac[6] = 0x34;
        mac[7] = 0x56;
        rc |= expect_int("regroup voice identity present", p25p2_mac_decode_voice_identity(1, mac, &identity), 1);
        rc |= expect_int("regroup voice tg", identity.tg, 0x4567);
        rc |= expect_int("regroup voice dst", identity.dst, 0);
        rc |= expect_int("regroup voice src", identity.src, 0x123456);
        rc |= expect_int("regroup voice type", identity.is_group, 1);
        rc |= expect_int("regroup voice service unknown", identity.svc_bits, -1);
    }

    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x90;
    mac[2] = 0x90;
    rc |= expect_int("vendor regroup collision absent", p25p2_mac_decode_voice_identity(1, mac, &identity), 0);

    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x80;
    mac[2] = 0x90;
    mac[3] = 0x41;
    mac[4] = 0x56;
    mac[5] = 0x78;
    mac[6] = 0x23;
    mac[7] = 0x45;
    mac[8] = 0x67;
    rc |= expect_int("Motorola abbreviated regroup identity present",
                     p25p2_mac_decode_voice_identity(1, mac, &identity), 1);
    rc |= expect_int("Motorola abbreviated regroup tg", identity.tg, 0x5678);
    rc |= expect_int("Motorola abbreviated regroup src", identity.src, 0x234567);
    rc |= expect_int("Motorola abbreviated regroup type", identity.is_group, 1);
    rc |= expect_int("Motorola abbreviated regroup service", identity.svc_bits, 0x41);

    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0xA0;
    mac[2] = 0x90;
    mac[4] = 0x80;
    mac[5] = 0x67;
    mac[6] = 0x89;
    mac[7] = 0x34;
    mac[8] = 0x56;
    mac[9] = 0x78;
    rc |=
        expect_int("Motorola extended regroup identity present", p25p2_mac_decode_voice_identity(0, mac, &identity), 1);
    rc |= expect_int("Motorola extended regroup tg", identity.tg, 0x6789);
    rc |= expect_int("Motorola extended regroup src", identity.src, 0x345678);
    rc |= expect_int("Motorola extended regroup type", identity.is_group, 1);
    rc |= expect_int("Motorola extended regroup service", identity.svc_bits, 0x80);

    /* Extended forms: the Source Address stays the source; the Source SUID is the talker's home identity. */
    static const uint8_t k_ext_opcodes[] = {0x21u, 0x22u};
    for (size_t e = 0; e < sizeof(k_ext_opcodes); e++) {
        const int private_form = k_ext_opcodes[e] == 0x22u;
        DSD_MEMSET(mac, 0, sizeof(mac));
        int pos = 1;
        mac[pos++] = k_ext_opcodes[e];
        mac[pos++] = 0x00;
        pos += private_form ? 3 : 2; /* target */
        mac[pos++] = 0x00;
        mac[pos++] = 0x0B;
        mac[pos++] = 0xBA; /* Source Address 0x000BBA */
        mac[pos++] = 0xAB;
        mac[pos++] = 0xCD;
        mac[pos++] = 0xE2;
        mac[pos++] = 0xA7; /* WACN 0xABCDE, System ID 0x2A7 */
        mac[pos++] = 0x12;
        mac[pos++] = 0xD6;
        mac[pos++] = 0x87; /* unit 1234567 */
        rc |= expect_int("extended voice identity present", p25p2_mac_decode_voice_identity(0, mac, &identity), 1);
        rc |= expect_int("extended voice source is the working address", identity.src, 0x000BBA);
        rc |= expect_int("extended voice home", identity.home_valid, 1);
        rc |= expect_int("extended voice home wacn", (int)identity.home_wacn, 0xABCDE);
        rc |= expect_int("extended voice home sysid", (int)identity.home_sysid, 0x2A7);
        rc |= expect_int("extended voice home unit", (int)identity.home_unit, 1234567);
        mac[pos - 3] = 0xFF;
        mac[pos - 2] = 0xFF;
        mac[pos - 1] = 0xFD; /* a special address names no home */
        rc |= expect_int("extended voice special unit present", p25p2_mac_decode_voice_identity(0, mac, &identity), 1);
        rc |= expect_int("extended voice special unit: no home", identity.home_valid, 0);
    }

    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x30;
    rc |= expect_int("non-voice identity absent", p25p2_mac_decode_voice_identity(0, mac, &identity), 0);
    return rc;
}

static int
run_offset_relative_vpdu_cases(void) {
    unsigned char mac[24];
    int rc = 0;

    g_apx_alias_header_calls = 0;
    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x30;
    mac[6] = 0x91;
    mac[7] = 0x90;
    mac[8] = 0x06;
    p25_test_process_mac_vpdu_ex(1, mac, 24, 0, 0);
    rc |= expect_int("offset motorola alias header", g_apx_alias_header_calls, 1);

    g_l3h_alias_calls = 0;
    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x30;
    mac[6] = 0xA8;
    mac[7] = 0xA4;
    mac[8] = 0x06;
    p25_test_process_mac_vpdu_ex(1, mac, 24, 0, 0);
    rc |= expect_int("offset harris alias", g_l3h_alias_calls, 1);

    g_apx_alias_header_calls = 0;
    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x82;
    mac[2] = 0xA4;
    mac[3] = 0x05;
    mac[6] = 0x91;
    mac[7] = 0x90;
    mac[8] = 0x06;
    p25_test_process_mac_vpdu_ex(1, mac, 24, 0, 0);
    rc |= expect_int("handler scratch keeps next segment", g_apx_alias_header_calls, 1);

    g_nmea_harris_calls = 0;
    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x30;
    mac[6] = 0x80;
    mac[7] = 0xAA;
    mac[8] = 0xA4;
    mac[9] = 0x11;
    p25_test_process_mac_vpdu_ex(1, mac, 24, 0, 0);
    rc |= expect_int("truncated shifted harris gps", g_nmea_harris_calls, 0);

    // The whole shifted message on a SACCH: the body is MAC[5..18], ending with Part B octet 8.
    g_nmea_harris_calls = 0;
    DSD_MEMSET(g_nmea_harris_body, 0, sizeof g_nmea_harris_body);
    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x80;
    mac[2] = 0xAA;
    mac[3] = 0xA4;
    mac[4] = 0x11;
    for (int i = 0; i < DSD_HARRIS_GPS_BODY_OCTETS; i++) {
        mac[5 + i] = (unsigned char)(0x21 + i);
    }
    p25_test_process_mac_vpdu_ex(1, mac, 24, 0, 0);
    rc |= expect_int("shifted harris gps", g_nmea_harris_calls, 1);
    rc |= expect_int("shifted harris gps first body octet", g_nmea_harris_body[0], 0x21);
    rc |= expect_int("shifted harris gps last body octet", g_nmea_harris_body[DSD_HARRIS_GPS_BODY_OCTETS - 1], 0x2E);

    // A FACCH carries 17 MAC octets: the 18-octet message never fits.
    g_nmea_harris_calls = 0;
    p25_test_process_mac_vpdu_ex(0, mac, 24, 0, 0);
    rc |= expect_int("shifted harris gps on facch", g_nmea_harris_calls, 0);

    // A length octet other than 17 is not the GPS message.
    g_nmea_harris_calls = 0;
    mac[4] = 0x05;
    p25_test_process_mac_vpdu_ex(1, mac, 24, 0, 0);
    rc |= expect_int("shifted harris gps short length", g_nmea_harris_calls, 0);

    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = 0x30;
    mac[6] = 0x30;
    mac[11] = 0x8B;
    mac[12] = 0x90;
    mac[13] = 0x05;
    p25_test_process_mac_vpdu_ex(1, mac, 24, 0, 0);

    return rc;
}

int
main(void) {
    int rc = run_direct_segment_parse_cases();
    if (rc != 0) {
        return rc;
    }
    rc = run_offset_relative_vpdu_cases();
    if (rc != 0) {
        return rc;
    }
    rc = run_voice_identity_parse_cases();
    if (rc != 0) {
        return rc;
    }

    // FACCH carries 17 MAC octets. Choose opcode 0x23 (base table 0), MCO=35 → infer 34 → cap 17.
    rc = run_case(/*FACCH*/ 0, 0x23, /*B*/ 17, /*C*/ 0);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_moto_81_len", /*FACCH*/ 0, 0x81, 0x90, 0x06, /*B*/ 6, /*C*/ 11);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_moto_82_len", /*FACCH*/ 0, 0x82, 0x90, 0x11, /*B*/ 17, /*C*/ 0);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_moto_8b_len", /*FACCH*/ 0, 0x8B, 0x90, 0x0F, /*B*/ 15, /*C*/ 2);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_moto_8f_len", /*FACCH*/ 0, 0x8F, 0x90, 0x0B, /*B*/ 11, /*C*/ 6);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_moto_bf_len", /*FACCH*/ 0, 0xBF, 0x90, 0x00, /*B*/ 3, /*C*/ 14);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_multifrag_cont_len", /*FACCH*/ 0, 0x10, 0x0A, 0x00, /*B*/ 10, /*C*/ 7);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_null_avoid_zero_bias_len", /*FACCH*/ 0, 0x08, 0x0A, 0x00, /*B*/ 10, /*C*/ 7);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_harris_a8_len", /*FACCH*/ 0, 0xA8, 0xA4, 0x0A, /*B*/ 10, /*C*/ 7);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_harris_b0_len", /*SACCH*/ 1, 0xB0, 0xA4, 0x11, /*B*/ 17, /*C*/ 3);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_tait_b5_fixed_len", /*FACCH*/ 0, 0xB5, 0xD8, 0x08, /*B*/ 5, /*C*/ 12);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_tait_variable_len", /*FACCH*/ 0, 0xB4, 0xD8, 0x08, /*B*/ 8, /*C*/ 9);
    if (rc != 0) {
        return rc;
    }
    rc = run_tdma_paging_len_case("p25_tdma_11_count1", 0x11, 0x00, /*B*/ 4, /*C*/ 5);
    if (rc != 0) {
        return rc;
    }
    rc = run_tdma_paging_len_case("p25_tdma_11_count2", 0x11, 0x01, /*B*/ 6, /*C*/ 5);
    if (rc != 0) {
        return rc;
    }
    rc = run_tdma_paging_len_case("p25_tdma_11_count3", 0x11, 0x02, /*B*/ 8, /*C*/ 5);
    if (rc != 0) {
        return rc;
    }
    rc = run_tdma_paging_len_case("p25_tdma_11_count4", 0x11, 0x03, /*B*/ 10, /*C*/ 5);
    if (rc != 0) {
        return rc;
    }
    rc = run_tdma_paging_len_case("p25_tdma_12_count1", 0x12, 0x00, /*B*/ 5, /*C*/ 5);
    if (rc != 0) {
        return rc;
    }
    rc = run_tdma_paging_len_case("p25_tdma_12_count2", 0x12, 0x01, /*B*/ 8, /*C*/ 5);
    if (rc != 0) {
        return rc;
    }
    rc = run_tdma_paging_len_case("p25_tdma_12_count3", 0x12, 0x02, /*B*/ 11, /*C*/ 5);
    if (rc != 0) {
        return rc;
    }
    rc = run_tdma_paging_len_case("p25_tdma_12_count4", 0x12, 0x03, /*B*/ 14, /*C*/ 5);
    if (rc != 0) {
        return rc;
    }
    DSD_FPRINTF(stderr, "P25p2 MAC MCO->length inference (FACCH) passed\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
