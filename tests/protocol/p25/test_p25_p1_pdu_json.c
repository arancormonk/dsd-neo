// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Verify P25 Phase 1 PDU JSON emission for data SAPs.
 */

#include <dsd-neo/core/bit_packing.h>

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

#define setenv dsd_test_setenv

typedef struct dsdneoRuntimeConfig dsdneoRuntimeConfig;
typedef struct dsd_opts dsd_opts;
typedef struct dsd_state dsd_state;
void dsd_neo_config_init(const dsd_opts* opts);
const dsdneoRuntimeConfig* dsd_neo_get_config(void);

// Use a local shim that sets up real opts/state in a separate TU.
void p25_test_p1_pdu_data_decode(const unsigned char* input, int len);

static int g_utf8_calls = 0;

// Stubs referenced by PDU data path
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
watchdog_event_datacall(dsd_opts* opts, dsd_state* state, uint32_t src, uint32_t dst, char* str, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)src;
    (void)dst;
    (void)str;
    (void)slot;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
watchdog_event_current(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
utf8_to_text(dsd_state* state, uint8_t wr, uint16_t len, uint8_t* input) {
    (void)state;
    (void)wr;
    (void)len;
    (void)input;
    g_utf8_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
unpack_byte_array_into_bit_array(const uint8_t* input, uint8_t* output, int len) {
    if (!input || !output || len <= 0) {
        return;
    }
    int k = 0;
    for (int i = 0; i < len; i++) {
        output[k++] = (input[i] >> 7) & 1;
        output[k++] = (input[i] >> 6) & 1;
        output[k++] = (input[i] >> 5) & 1;
        output[k++] = (input[i] >> 4) & 1;
        output[k++] = (input[i] >> 3) & 1;
        output[k++] = (input[i] >> 2) & 1;
        output[k++] = (input[i] >> 1) & 1;
        output[k++] = (input[i] >> 0) & 1;
    }
}

uint8_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_sentence_checker(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint8_t slot, int len_bytes) {
    (void)opts;
    (void)state;
    (void)slot;
    if (input == NULL || len_bytes < 6) {
        return 0;
    }

    char prefix[7];
    for (int i = 0; i < 6; i++) {
        prefix[i] = (char)convert_bits_into_output(input + ((size_t)i * 8U), 8);
    }
    prefix[6] = '\0';
    if (strcmp(prefix, "$GPRMC") != 0) {
        return 0;
    }

    DSD_FPRINTF(stderr, "$GPRMC,123519");
    return 1;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
decode_ip_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)len;
    (void)input;
}

// Additional stubs referenced by linked objects (rigctl/rtl streaming)
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

static int
parse_json_int_field(const char* line, const char* key, int* out) {
    if (!line || !key || !out) {
        return 0;
    }
    const char* p = strstr(line, key);
    if (!p) {
        return 0;
    }
    p += strlen(key);
    errno = 0;
    char* end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p || errno == ERANGE || v < INT_MIN || v > INT_MAX) {
        return 0;
    }
    *out = (int)v;
    return 1;
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_str_contains(const char* tag, const char* hay, const char* needle) {
    if (!strstr(hay, needle)) {
        DSD_FPRINTF(stderr, "%s: missing '%s' in '%s'\n", tag, needle, hay);
        return 1;
    }
    return 0;
}

static int
parse_last_json(const char* buf, int len, int* out_sap, int* out_mfid, int* out_io, int* out_len, char* out_summary,
                size_t sum_cap) {
    // find last JSON line
    if (!buf || len <= 0) {
        return -1;
    }
    const char* last_nl = strrchr(buf, '\n');
    const char* line = last_nl ? (last_nl + 1) : buf;

    int sap = -1, mfid = -1, io = -1, jlen = -1;
    if (!parse_json_int_field(line, "\"sap\":", &sap)) {
        return -1;
    }
    if (!parse_json_int_field(line, "\"mfid\":", &mfid)) {
        return -2;
    }
    if (!parse_json_int_field(line, "\"io\":", &io)) {
        return -3;
    }
    if (!parse_json_int_field(line, "\"len\":", &jlen)) {
        return -4;
    }

    if (out_sap) {
        *out_sap = sap;
    }
    if (out_mfid) {
        *out_mfid = mfid;
    }
    if (out_io) {
        *out_io = io;
    }
    if (out_len) {
        *out_len = jlen;
    }
    if (out_summary && sum_cap > 0) {
        const char* s = strstr(line, "\"summary\":\"");
        if (s) {
            s += 11;
            size_t i = 0;
            while (s[i] && s[i] != '"' && i + 1 < sum_cap) {
                out_summary[i] = s[i];
                i++;
            }
            out_summary[i] = '\0';
        } else {
            out_summary[0] = '\0';
        }
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    setenv("DSD_NEO_PDU_JSON", "1", 1);
    dsd_neo_config_init(NULL);

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "p25_p1_pdu_json") != 0) {
        DSD_FPRINTF(stderr, "Failed to capture stderr: %s\n", strerror(errno));
        return 101;
    }

    // Case 1: SAP 32 RegAuth
    {
        uint8_t pdu[64];
        DSD_MEMSET(pdu, 0, sizeof(pdu));
        // fmt/io (bit1), sap, mfid, llid
        pdu[0] = 0x10; // fmt=16, io=0
        pdu[1] = 32;   // SAP 32
        pdu[2] = 0xAA; // MFID
        pdu[3] = 0x00;
        pdu[4] = 0x01;
        pdu[5] = 0x02; // LLID
        pdu[6] = 0x03; // blks
        pdu[7] = 0x00; // pad
        pdu[9] = 0x00; // offset
        // payload at index 12
        pdu[12] = 0x42;
        pdu[13] = 0x11;
        pdu[14] = 0x22;
        pdu[15] = 0x33;
        pdu[16] = 0x44;             // 5 bytes
        int total_len = 12 + 5 + 4; // header + payload + CRC
        p25_test_p1_pdu_data_decode(pdu, total_len);
    }

    // Case 2: SAP 48 Location Service with a valid NMEA sentence.
    {
        uint8_t pdu[128];
        DSD_MEMSET(pdu, 0, sizeof(pdu));
        pdu[0] = 0x10; // fmt=16, io=0
        pdu[1] = 48;   // SAP 48
        pdu[2] = 0x01; // MFID
        pdu[6] = 0x02; // blks
        pdu[7] = 0x00; // pad
        pdu[9] = 0x00; // offset
        static const char nmea[] = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n";
        DSD_MEMCPY(pdu + 12, nmea, sizeof(nmea) - 1);
        int total_len = 12 + (int)(sizeof(nmea) - 1) + 4;
        p25_test_p1_pdu_data_decode(pdu, total_len);
    }

    // Case 3: SAP 34 SysCfg
    {
        uint8_t pdu[64];
        DSD_MEMSET(pdu, 0, sizeof(pdu));
        pdu[0] = 0x30; // fmt=16, io=1, low bit 1 clear to catch IO-bit regressions
        pdu[1] = 34;   // SAP 34
        pdu[2] = 0x55; // MFID
        pdu[3] = 0x00;
        pdu[4] = 0x00;
        pdu[5] = 0x10; // LLID
        pdu[6] = 0x02; // blks
        pdu[7] = 0x00; // pad
        pdu[9] = 0x00; // offset
        pdu[12] = 0x07;
        pdu[13] = 0x66;
        pdu[14] = 0x77;             // 3 bytes
        int total_len = 12 + 3 + 4; // header + payload + CRC
        p25_test_p1_pdu_data_decode(pdu, total_len);
    }

    // Case 4: SAP 6 inbound SNDCP Activate TDS Context Request.
    {
        uint8_t pdu[64];
        DSD_MEMSET(pdu, 0, sizeof(pdu));
        pdu[0] = 0x10; // fmt=16, io=0/inbound
        pdu[1] = 6;    // SAP 6
        pdu[2] = 0x21; // MFID
        pdu[6] = 0x02;
        pdu[12] = 0x07; // type=0, nsapi=7
        pdu[13] = 0x41; // version=4, IPv4 dynamic
        pdu[14] = 198;
        pdu[15] = 51;
        pdu[16] = 100;
        pdu[17] = 9;
        pdu[18] = 0x20; // DSUT=2
        pdu[19] = 0x00;
        pdu[20] = 0x00;
        pdu[21] = 0x01;
        int total_len = 12 + 10 + 4;
        p25_test_p1_pdu_data_decode(pdu, total_len);
    }

    // Case 5: SAP 6 outbound SNDCP Activate TDS Context Accept.
    {
        uint8_t pdu[64];
        DSD_MEMSET(pdu, 0, sizeof(pdu));
        pdu[0] = 0x30; // fmt=16, io=1/outbound
        pdu[1] = 6;    // SAP 6
        pdu[2] = 0x22; // MFID
        pdu[6] = 0x02;
        pdu[12] = 0x05; // type=0, nsapi=5
        pdu[13] = 0xAB; // priority=10, ready timer=60s
        pdu[14] = 0x31; // standby timer=60s, IPv4 dynamic
        pdu[15] = 192;
        pdu[16] = 0;
        pdu[17] = 2;
        pdu[18] = 7;
        pdu[19] = 0x00;
        pdu[20] = 0x00;
        pdu[21] = 0x20; // MTU 510, UDP compression none
        pdu[22] = 0x02; // MDP PPP
        int total_len = 12 + 11 + 4;
        p25_test_p1_pdu_data_decode(pdu, total_len);
    }

    // Case 6: SAP 6 outbound SNDCP Activate TDS Context Reject.
    {
        uint8_t pdu[64];
        DSD_MEMSET(pdu, 0, sizeof(pdu));
        pdu[0] = 0x30; // fmt=16, io=1/outbound
        pdu[1] = 6;
        pdu[2] = 0x23;
        pdu[6] = 0x02;
        pdu[12] = 0x34; // type=3, nsapi=4
        pdu[13] = 0x0A; // IPv4 not supported
        int total_len = 12 + 2 + 4;
        p25_test_p1_pdu_data_decode(pdu, total_len);
    }

    // Case 7: SAP 6 outbound SNDCP Deactivate TDS Context Request.
    {
        uint8_t pdu[64];
        DSD_MEMSET(pdu, 0, sizeof(pdu));
        pdu[0] = 0x30; // fmt=16, io=1/outbound
        pdu[1] = 6;
        pdu[2] = 0x24;
        pdu[6] = 0x02;
        pdu[12] = 0x21; // type=2, nsapi=1
        pdu[13] = 0x10; // deactivate this NSAPI
        int total_len = 12 + 2 + 4;
        p25_test_p1_pdu_data_decode(pdu, total_len);
    }

    // Case 8: SAP 4 packet data with the optional 2-octet SNDCP packet header.
    {
        uint8_t pdu[96];
        DSD_MEMSET(pdu, 0, sizeof(pdu));
        pdu[0] = 0x30; // fmt=16, io=1/outbound
        pdu[1] = 4;    // SAP 4
        pdu[2] = 0x33; // MFID
        pdu[6] = 0x03;
        pdu[9] = 0x02;  // data header offset = 2 octets
        pdu[12] = 0x45; // RF unconfirmed data, NSAPI 5
        pdu[13] = 0x00; // no IP/UDP header compression
        pdu[14] = 0x45; // start of IPv4 payload passed to decode_ip_pdu
        pdu[15] = 0x00;
        pdu[16] = 0x00;
        pdu[17] = 0x14;
        int total_len = 12 + 2 + 20 + 4;
        p25_test_p1_pdu_data_decode(pdu, total_len);
    }

    dsd_test_capture_stderr_end(&cap);

    FILE* rf = fopen(cap.path, "rb");
    if (!rf) {
        DSD_FPRINTF(stderr, "fopen read failed\n");
        return 102;
    }
    fseek(rf, 0, SEEK_END);
    long sz = ftell(rf);
    if (sz < 0) {
        fclose(rf);
        return 103;
    }
    fseek(rf, 0, SEEK_SET);
    size_t alloc = (size_t)sz + 1;
    char* buf = (char*)calloc(alloc, 1);
    size_t nread = fread(buf, 1, alloc - 1, rf);
    if (nread >= alloc) {
        nread = alloc - 1;
    }
    fclose(rf);

    // Parse last (SAP 4 packet header)
    int sap = -1, mfid = -1, io = -1, jlen = -1;
    char summary[128];
    int er = parse_last_json(buf, (int)nread, &sap, &mfid, &io, &jlen, summary, sizeof(summary));
    if (er != 0) {
        free(buf);
        DSD_FPRINTF(stderr, "parse_last_json er=%d\n", er);
        return 103;
    }
    rc |= expect_eq_int("SAP4 sap", sap, 4);
    rc |= expect_eq_int("SAP4 mfid", mfid, 0x33);
    rc |= expect_eq_int("SAP4 io", io, 1);
    rc |= expect_eq_int("SAP4 len", jlen, 22);
    rc |= expect_str_contains("SAP4 summary", summary, "SNDCP Packet Header");
    rc |= expect_str_contains("SysCfg summary", buf, "SysCfg");
    rc |= expect_str_contains("SAP6 request output", buf, "Activate TDS Context Request");
    rc |= expect_str_contains("SAP6 request IP", buf, "IP:198.51.100.9");
    rc |= expect_str_contains("SAP6 accept output", buf, "Activate TDS Context Accept");
    rc |= expect_str_contains("SAP6 SNDCP MTU", buf, "MTU:510");
    rc |= expect_str_contains("SAP6 reject output", buf, "IPv4 Not Supported");
    rc |= expect_str_contains("SAP6 deactivate output", buf, "Deactivate:This NSAPI");
    rc |= expect_str_contains("SAP48 NMEA output", buf, "$GPRMC,123519");
    rc |= expect_eq_int("SAP48 NMEA avoids UTF8 fallback", g_utf8_calls, 0);
    free(buf);

    (void)remove(cap.path);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
