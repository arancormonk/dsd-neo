// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Exercise P25p1 PDU Data path through Extended Address (SAP 31)
 * followed by Encryption Sync Header (SAP 1) that signals aux SAP 32.
 * This drives p25_decode_extended_address and p25_decode_es_header paths.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_pdu.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

#define setenv dsd_test_setenv

typedef struct dsdneoRuntimeConfig dsdneoRuntimeConfig;
void dsd_neo_config_init(void);
const dsdneoRuntimeConfig* dsd_neo_get_config(void);

// Shim to invoke real decoder
void p25_test_p1_pdu_data_decode(const unsigned char* input, int len);

static int g_datacall_count;
static int g_history_count;
static uint32_t g_datacall_src;
static uint32_t g_datacall_dst;
static char g_datacall_text[256];

// Stubs required by linked decoder units
int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    (void)opts;
    (void)state;
    (void)slot;
    g_datacall_count++;
    g_datacall_src = observation->ota_source_id;
    g_datacall_dst = observation->ota_target_id;
    DSD_SNPRINTF(g_datacall_text, sizeof(g_datacall_text), "%s", notice ? notice : "");
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
    g_history_count++;
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
dsd_event_sync_slot(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
    g_history_count++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
utf8_to_text(dsd_state* state, uint8_t wr, uint16_t len, uint8_t* input) {
    (void)state;
    (void)wr;
    (void)len;
    (void)input;
}

uint8_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_sentence_checker(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint8_t slot, int len_bytes) {
    (void)opts;
    (void)state;
    (void)input;
    (void)slot;
    (void)len_bytes;
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
decode_ip_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)len;
    (void)input;
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

static void
pack_bits(uint8_t* dst, size_t cap_bytes, int bit_offset, int width, uint32_t value) {
    // MSB-first placement into dst byte array
    for (int i = 0; i < width; i++) {
        int b = (value >> (width - 1 - i)) & 1;
        int pos = bit_offset + i;
        int byte = pos / 8;
        int bit = 7 - (pos % 8);
        if ((size_t)byte >= cap_bytes) {
            break; // guard against overflow when called with bad params
        }
        dst[byte] = (uint8_t)(dst[byte] & ~(1u << bit));
        dst[byte] = (uint8_t)(dst[byte] | (b << bit));
    }
}

static int
parse_last_json(const char* buf, int len, int* out_sap) {
    if (!buf || len <= 0) {
        return -1;
    }
    const char* last_nl = strrchr(buf, '\n');
    const char* line = last_nl ? (last_nl + 1) : buf;
    int sap = -1;
    if (!parse_json_int_field(line, "\"sap\":", &sap)) {
        return -1;
    }
    if (out_sap) {
        *out_sap = sap;
    }
    return 0;
}

static int
expect_bytes(const char* label, const uint8_t* got, const uint8_t* want, size_t len) {
    if (memcmp(got, want, len) != 0) {
        DSD_FPRINTF(stderr, "%s: byte mismatch\n", label);
        return 1;
    }
    return 0;
}

static int
expect_u8(const char* label, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%02X want 0x%02X\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u32(const char* label, uint32_t got, uint32_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%08X want 0x%08X\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_contains(const char* label, const char* got, const char* needle) {
    if (got == NULL || strstr(got, needle) == NULL) {
        DSD_FPRINTF(stderr, "%s: missing '%s' in '%s'\n", label, needle, got ? got : "(null)");
        return 1;
    }
    return 0;
}

static void
reset_watchdog_counters(void) {
    g_datacall_count = 0;
    g_history_count = 0;
    g_datacall_src = 0;
    g_datacall_dst = 0;
    g_datacall_text[0] = '\0';
}

static int
test_p25_pdu_des_decrypt_vector(void) {
    static const uint8_t expect[] = {0x67, 0xAE, 0x7A, 0x29, 0x61, 0xDF, 0xA3, 0x45};
    static dsd_opts opts;
    static dsd_state st;
    uint8_t input[sizeof expect];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    DSD_MEMSET(input, 0, sizeof input);
    st.R = 0x133457799BBCDFF1ULL;

    uint8_t encrypted = p25_decrypt_pdu(&opts, &st, input, 0x81, 0x4321, 0x0123456789ABCDEFULL, (int)sizeof input);

    int rc = 0;
    rc |= expect_u8("P25 PDU DES decrypt flag", encrypted, 0);
    rc |= expect_bytes("P25 PDU DES OFB discard vector", input, expect, sizeof expect);
    return rc;
}

static int
test_p25_pdu_rc4_decrypt_vector(void) {
    static const uint8_t expect[] = {0xFD, 0x03, 0xAB, 0x28, 0x7B, 0x5C, 0x1D, 0x19,
                                     0x5A, 0x3F, 0xE2, 0x45, 0xFE, 0x54, 0xDB, 0x10};
    static dsd_opts opts;
    static dsd_state st;
    uint8_t input[sizeof expect];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    DSD_MEMSET(input, 0, sizeof input);
    st.R = 0x0102030405ULL;

    uint8_t encrypted = p25_decrypt_pdu(&opts, &st, input, 0xAA, 0x4321, 0x0123456789ABCDEFULL, (int)sizeof input);

    int rc = 0;
    rc |= expect_u8("P25 PDU RC4 decrypt flag", encrypted, 0);
    rc |= expect_bytes("P25 PDU RC4 drop-256 vector", input, expect, sizeof expect);
    return rc;
}

static int
test_p25_pdu_aes128_decrypt_vector(void) {
    static const uint8_t expect[] = {0xEC, 0xAB, 0x6A, 0x30, 0x3A, 0x05, 0x65, 0x68,
                                     0xE0, 0x29, 0x0F, 0x56, 0x58, 0xA3, 0x07, 0xF3};
    static dsd_opts opts;
    static dsd_state st;
    uint8_t input[sizeof expect];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    DSD_MEMSET(input, 0, sizeof input);
    st.K1 = 0x0011223344556677ULL;
    st.K2 = 0x8899AABBCCDDEEFFULL;

    uint8_t encrypted = p25_decrypt_pdu(&opts, &st, input, 0x89, 0x4321, 0x0123456789ABCDEFULL, (int)sizeof input);

    int rc = 0;
    rc |= expect_u8("P25 PDU AES-128 decrypt flag", encrypted, 0);
    rc |= expect_bytes("P25 PDU AES-128 OFB discard vector", input, expect, sizeof expect);
    return rc;
}

static int
test_p25_pdu_missing_keys_stay_encrypted(void) {
    static const uint8_t expect[] = {0xC0, 0xDE, 0x12, 0x34};
    static dsd_opts opts;
    static dsd_state st;
    uint8_t des_input[sizeof expect];
    uint8_t rc4_input[sizeof expect];
    uint8_t aes_input[sizeof expect];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    DSD_MEMCPY(des_input, expect, sizeof expect);
    DSD_MEMCPY(rc4_input, expect, sizeof expect);
    DSD_MEMCPY(aes_input, expect, sizeof expect);

    uint8_t des_encrypted =
        p25_decrypt_pdu(&opts, &st, des_input, 0x81, 0x4321, 0x0123456789ABCDEFULL, (int)sizeof des_input);
    uint8_t rc4_encrypted =
        p25_decrypt_pdu(&opts, &st, rc4_input, 0xAA, 0x4321, 0x0123456789ABCDEFULL, (int)sizeof rc4_input);
    uint8_t aes_encrypted =
        p25_decrypt_pdu(&opts, &st, aes_input, 0x89, 0x4321, 0x0123456789ABCDEFULL, (int)sizeof aes_input);

    int rc = 0;
    rc |= expect_u8("P25 PDU DES missing-key encrypted flag", des_encrypted, 1);
    rc |= expect_u8("P25 PDU RC4 missing-key encrypted flag", rc4_encrypted, 1);
    rc |= expect_u8("P25 PDU AES missing-key encrypted flag", aes_encrypted, 1);
    rc |= expect_bytes("P25 PDU DES missing-key unchanged", des_input, expect, sizeof expect);
    rc |= expect_bytes("P25 PDU RC4 missing-key unchanged", rc4_input, expect, sizeof expect);
    rc |= expect_bytes("P25 PDU AES missing-key unchanged", aes_input, expect, sizeof expect);
    return rc;
}

static int
test_p25_pdu_label_helpers(void) {
    char label[48];
    int rc = 0;

    label[0] = 'x';
    p25_decode_sap(0, label, sizeof(label));
    rc |= expect_contains("SAP user-data label", label, "User Data");

    p25_decode_sap(0xFE, label, sizeof(label));
    rc |= expect_contains("SAP unknown label", label, "Unknown SAP");

    label[0] = 'x';
    p25_decode_rsp(0, 0, 0, label, sizeof(label));
    rc |= expect_contains("RSP ACK label", label, "ACK");

    p25_decode_rsp(1, 4, 0, label, sizeof(label));
    rc |= expect_contains("RSP NACK undeliverable label", label, "Undeliverable");

    p25_decode_rsp(2, 0, 0, label, sizeof(label));
    rc |= expect_contains("RSP SACK label", label, "SACK");

    p25_decode_rsp(3, 0, 0, label, sizeof(label));
    rc |= expect_contains("RSP unknown label", label, "Unknown RSP");

    p25_decode_sap(0, NULL, sizeof(label));
    p25_decode_rsp(0, 0, 0, NULL, sizeof(label));
    p25_decode_sap(0, label, 0);
    p25_decode_rsp(0, 0, 0, label, 0);
    return rc;
}

static int
seed_voice_call(dsd_state* state, uint64_t target, uint64_t source) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = source,
        .observed_m = 1.0,
    };
    return dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0;
}

static int
same_voice_identity(const dsd_call_snapshot* before, const dsd_call_snapshot* after) {
    return before->epoch == after->epoch && before->phase == after->phase && before->protocol == after->protocol
           && before->kind == after->kind && before->ota_target_id == after->ota_target_id
           && before->policy_target_id == after->policy_target_id && before->ota_source_id == after->ota_source_id;
}

static int
test_p25_pdu_header_state_updates(void) {
    static dsd_opts opts;
    static dsd_state st;
    uint8_t header[12];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    DSD_MEMSET(header, 0, sizeof(header));

    header[0] = 0x30; // io=1, fmt=16
    header[1] = 4;    // Packet Data
    header[3] = 0x12;
    header[4] = 0x34;
    header[5] = 0x56;
    header[6] = 0x82; // FMF plus two blocks
    header[7] = 0x05; // pad
    header[8] = 0x21; // NS/FSNF
    header[9] = 0x07; // offset

    reset_watchdog_counters();
    p25_decode_pdu_header(&opts, &st, header);
    int rc = 0;
    dsd_call_snapshot call;
    rc |= expect_u32("header does not start voice call", (uint32_t)(dsd_call_state_get(&st, 0U, &call) > 0), 0);
    rc |= expect_contains("header data-call summary", st.dmr_lrrp_gps[0], "Packet Data");
    rc |= expect_u32("header non-response no datacall", (uint32_t)g_datacall_count, 0);

    DSD_MEMSET(&st, 0, sizeof(st));
    DSD_MEMSET(header, 0, sizeof(header));
    header[0] = 0x03; // response format
    header[1] = (uint8_t)((1U << 6) | (4U << 3) | 2U);
    header[3] = 0x00;
    header[4] = 0x01;
    header[5] = 0x23;
    reset_watchdog_counters();
    p25_decode_pdu_header(&opts, &st, header);
    rc |= expect_u32("response does not start voice call", (uint32_t)(dsd_call_state_get(&st, 0U, &call) > 0), 0);
    rc |= expect_u32("response datacall count", (uint32_t)g_datacall_count, 1);
    rc |= expect_u32("response datacall src", g_datacall_src, 0x000123);
    rc |= expect_u32("response datacall dst", g_datacall_dst, 0);
    rc |= expect_contains("response datacall text", g_datacall_text, "Undeliverable");

    dsd_state_ext_free_all(&st);
    DSD_MEMSET(&st, 0, sizeof(st));
    st.lastsynctype = DSD_SYNC_P25P1_POS;
    rc |= expect_u32("trunking header seed voice", (uint32_t)seed_voice_call(&st, 0x222222, 0x111111), 1);
    dsd_call_snapshot before;
    dsd_call_snapshot after;
    rc |= expect_u32("trunking header get before", (uint32_t)(dsd_call_state_get(&st, 0U, &before) > 0), 1);
    DSD_MEMSET(header, 0, sizeof(header));
    header[0] = 0x10;
    header[1] = 61; // trunking-control SAP is intentionally skipped
    header[3] = 0xAA;
    header[4] = 0xBB;
    header[5] = 0xCC;
    reset_watchdog_counters();
    p25_decode_pdu_header(&opts, &st, header);
    rc |= expect_u32("trunking header get after", (uint32_t)(dsd_call_state_get(&st, 0U, &after) > 0), 1);
    rc |= expect_u32("trunking header preserves voice", (uint32_t)same_voice_identity(&before, &after), 1);
    rc |= expect_u32("trunking header no datacall", (uint32_t)g_datacall_count, 0);
    dsd_state_ext_free_all(&st);
    return rc;
}

static int
test_p25_pdu_es_header_decrypt_and_advance(void) {
    static const uint8_t des_expect[] = {0x67, 0xAE, 0x7A, 0x29, 0x61, 0xDF, 0xA3, 0x45};
    static dsd_opts opts;
    static dsd_state st;
    uint8_t es[13 + sizeof(des_expect)];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    DSD_MEMSET(es, 0, sizeof(es));
    st.R = 0x133457799BBCDFF1ULL;

    es[0] = 0x01;
    es[1] = 0x23;
    es[2] = 0x45;
    es[3] = 0x67;
    es[4] = 0x89;
    es[5] = 0xAB;
    es[6] = 0xCD;
    es[7] = 0xEF;
    es[9] = 0x81;  // DES
    es[12] = 0xE0; // aux_res=3, aux_sap=32

    uint8_t sap = 0;
    int ptr = 0;
    uint8_t encrypted = p25_decode_es_header(&opts, &st, es, &sap, &ptr, (int)sizeof(es));
    int rc = 0;
    rc |= expect_u8("ES header decrypt flag", encrypted, 0);
    rc |= expect_u8("ES header aux SAP", sap, 32);
    rc |= expect_u32("ES header ptr", (uint32_t)ptr, 13);
    rc |= expect_bytes("ES header DES payload", es + 13, des_expect, sizeof(des_expect));

    return rc;
}

int
main(void) {
    int rc = 0;

    // Enable JSON emission
    setenv("DSD_NEO_PDU_JSON", "1", 1);
    dsd_neo_config_init();

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "p25_p1_pdu_es_ext") != 0) {
        DSD_FPRINTF(stderr, "Failed to capture stderr: %s\n", strerror(errno));
        return 101;
    }

    // Build PDU with initial SAP=31 (extended address), which sets aux SAP=1,
    // then ES header sets aux_sap=32 (RegAuth). Follow with small payload.
    uint8_t pdu[96];
    DSD_MEMSET(pdu, 0, sizeof(pdu));

    pdu[0] = 0x10; // fmt=16, io=0
    pdu[1] = 31;   // SAP 31 triggers extended addressing
    pdu[2] = 0x22; // MFID (header level)
    pdu[6] = 0x02; // blks
    pdu[7] = 0x00; // pad
    pdu[9] = 0x00; // offset

    // Extended Address header (12 bytes) at offset 12
    // Layout: ea_sap @ bit 10 (6b) → 1; ea_mfid @16 (6b) → 0x15; ea_llid @24 (24b) → 0x000102
    uint8_t* ext = pdu + 12;
    DSD_MEMSET(ext, 0, 12);
    pack_bits(ext, 12, 10, 6, 1);         // ea_sap = 1 (encryption header follows)
    pack_bits(ext, 12, 16, 6, 0x15);      // ea_mfid
    pack_bits(ext, 12, 24, 24, 0x000102); // ea_llid

    // ES header (13 bytes) immediately after ext
    uint8_t* es = pdu + 12 + 12;
    DSD_MEMSET(es, 0, 13);
    // MI 64-bit: 0x0102030405060708
    es[0] = 0x01;
    es[1] = 0x02;
    es[2] = 0x03;
    es[3] = 0x04;
    es[4] = 0x05;
    es[5] = 0x06;
    es[6] = 0x07;
    es[7] = 0x08;
    // mi_res (8b) = 0 (already zero)
    // alg_id (8b) at bits[72..79] = 0x80 (clear)
    es[9] = 0x80; // aligns with unpack→bits mapping for subsequent fields
    // key_id (16b) bits[80..95] = 0x1234
    es[10] = 0x12;
    es[11] = 0x34;
    // aux_res (2b) at [96..97] = 3, aux_sap (6b) [98..103] = 32
    pack_bits(es, 13, 96, 2, 3);
    pack_bits(es, 13, 98, 6, 32);

    // Minimal payload for SAP 32 after headers (place at current index)
    int payload_off = 12 + 12 + 13;
    pdu[payload_off + 0] = 0x42;
    pdu[payload_off + 1] = 0x99;
    pdu[payload_off + 2] = 0x00;

    int total_len = payload_off + 3 + 4; // +CRC
    p25_test_p1_pdu_data_decode(pdu, total_len);

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

    int sap = -1;
    if (parse_last_json(buf, (int)nread, &sap) != 0) {
        free(buf);
        return 103;
    }
    free(buf);
    // Expect aux_sap=32 (RegAuth) after ES header
    if (sap != 32) {
        DSD_FPRINTF(stderr, "expected SAP 32 after ES header, got %d\n", sap);
        rc = 1;
    }
    (void)remove(cap.path);
    rc |= test_p25_pdu_des_decrypt_vector();
    rc |= test_p25_pdu_rc4_decrypt_vector();
    rc |= test_p25_pdu_aes128_decrypt_vector();
    rc |= test_p25_pdu_missing_keys_stay_encrypted();
    rc |= test_p25_pdu_label_helpers();
    rc |= test_p25_pdu_header_state_updates();
    rc |= test_p25_pdu_es_header_decrypt_and_advance();
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
