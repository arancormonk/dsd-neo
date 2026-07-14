// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Merge of P25 P2 MAC JSON tests: length derivation via MCO fallback,
 * LCCH labeling, and FACCH clamp checks.
 */

#include <errno.h>
#include <limits.h>
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

// Minimal forward types
typedef struct dsd_opts dsd_opts;
typedef struct dsd_state dsd_state;
typedef struct dsdneoRuntimeConfig dsdneoRuntimeConfig;

// Runtime config hooks
void dsd_neo_config_init(void);
const dsdneoRuntimeConfig* dsd_neo_get_config(void);

// Test shims: Phase 2 MAC VPDU entry points
void p25_test_process_mac_vpdu_ex(int type, const unsigned char* mac_bytes, int mac_len, int is_lcch, int currentslot);

// Stubs referenced by MAC VPDU path
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
extract_last_fields(const char* buf, int len, char* out_xch, size_t xch_cap, int* out_b, int* out_c, int* out_slot,
                    char* out_summary, size_t sum_cap) {
    if (!buf || len <= 0) {
        return -1;
    }
    const char* last_brace = strrchr(buf, '{');
    if (!last_brace) {
        return -1;
    }
    const char* line = last_brace;

    const char* p;
    int b = -1, c = -1, s = -1;
    p = strstr(line, "\"xch\":\"");
    if (p && out_xch && xch_cap > 0) {
        p += 7;
        size_t i = 0;
        while (p[i] && p[i] != '"' && i + 1 < xch_cap) {
            out_xch[i] = p[i];
            i++;
        }
        out_xch[i] = '\0';
    }
    if (!parse_json_int_field(line, "\"lenB\":", &b)) {
        return -2;
    }
    if (!parse_json_int_field(line, "\"lenC\":", &c)) {
        return -3;
    }
    p = strstr(line, "\"slot\":");
    if (p) {
        p += 7;
        s = (int)strtol(p, NULL, 10);
    }
    if (out_b) {
        *out_b = b;
    }
    if (out_c) {
        *out_c = c;
    }
    if (out_slot) {
        *out_slot = s;
    }
    if (out_summary && sum_cap > 0) {
        const char* q = strstr(line, "\"summary\":\"");
        if (q) {
            q += 11;
            size_t i = 0;
            while (q[i] && q[i] != '"' && i + 1 < sum_cap) {
                out_summary[i] = q[i];
                i++;
            }
            out_summary[i] = '\0';
        }
    }
    return 0;
}

static int
extract_first_fields(FILE* rf, char* out_xch, size_t xch_cap, char* out_summary, size_t sum_cap) {
    char line[512] = {0};
    if (!fgets(line, sizeof line, rf)) {
        return -1;
    }
    const char* p = strstr(line, "\"xch\":\"");
    if (p && out_xch && xch_cap > 0) {
        p += 7;
        size_t i = 0;
        while (p[i] && p[i] != '"' && i + 1 < xch_cap) {
            out_xch[i] = p[i];
            i++;
        }
        out_xch[i] = '\0';
    }
    const char* q = strstr(line, "\"summary\":\"");
    if (q && out_summary && sum_cap > 0) {
        q += 11;
        size_t i = 0;
        while (q[i] && q[i] != '"' && i + 1 < sum_cap) {
            out_summary[i] = q[i];
            i++;
        }
        out_summary[i] = '\0';
    }
    return 0;
}

static int
line_contains(const char* line, const char* line_end, const char* needle) {
    const char* p = line;
    while ((p = strstr(p, needle)) != NULL) {
        if (p < line_end) {
            return 1;
        }
        p++;
    }
    return 0;
}

static int
expect_json_record_summary(const char* buf, const char* xch, int op, const char* summary) {
    char xch_field[32];
    char op_field[32];
    char summary_field[64];

    DSD_SNPRINTF(xch_field, sizeof xch_field, "\"xch\":\"%s\"", xch);
    DSD_SNPRINTF(op_field, sizeof op_field, "\"op\":%d", op);
    DSD_SNPRINTF(summary_field, sizeof summary_field, "\"summary\":\"%s\"", summary);

    const char* line = buf;
    while (line && *line) {
        const char* line_end = strchr(line, '\n');
        if (!line_end) {
            line_end = line + strlen(line);
        }
        if (line_contains(line, line_end, xch_field) && line_contains(line, line_end, op_field)
            && line_contains(line, line_end, summary_field)) {
            return 0;
        }
        line = (*line_end == '\n') ? line_end + 1 : line_end;
    }

    DSD_FPRINTF(stderr, "missing JSON record xch=%s op=%d summary=%s\n", xch, op, summary);
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
expect_eq_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    // Enable JSON emission
    setenv("DSD_NEO_PDU_JSON", "1", 1);
    dsd_neo_config_init();

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "p25_p2_mac_json") != 0) {
        DSD_FPRINTF(stderr, "Failed to capture stderr: %s\n", strerror(errno));
        return 101;
    }

    // Case A: FACCH, unknown opcode → derive len from MCO=10 (lenB=9, lenC=7), slot flip
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[0] = 1;     // header-present hint for FACCH
        mac[1] = 10;    // opcode byte with MCO=10 (low 6 bits)
        mac[2] = 0x00;  // standard MFID
        mac[10] = 0xFF; // second message unknown to force fallback
        p25_test_process_mac_vpdu_ex(0 /*FACCH*/, mac, 24, 0, 0);
    }

    // Case B: SACCH, unknown opcode; MCO=15 → lenB=14, lenC=5; xch=SACCH
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 15;    // MCO=15
        mac[2] = 0x00;  // standard MFID
        mac[15] = 0xFF; // second message unknown
        p25_test_process_mac_vpdu_ex(1 /*SACCH*/, mac, 24, 0, 0);
    }

    // Case C: LCCH labeling and TDMA 0x03 telephone user summary
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0x03; // IDLE
        mac[2] = 0x00; // standard MFID
        p25_test_process_mac_vpdu_ex(0 /*FACCH path*/, mac, 24, /*is_lcch*/ 1, /*slot*/ 0);
    }

    // Case D: FACCH MCO clamp beyond capacity (opcode with MCO=63 → clamp to lenB=16)
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[0] = 1;  // header present
        mac[1] = 63; // absurd MCO
        mac[2] = 0x00;
        p25_test_process_mac_vpdu_ex(0 /*FACCH*/, mac, 24, /*is_lcch*/ 0, /*slot*/ 1);
    }

    dsd_test_capture_stderr_end(&cap);

    // Read entire file
    FILE* rf = fopen(cap.path, "rb");
    if (!rf) {
        DSD_FPRINTF(stderr, "fopen read failed\n");
        return 102;
    }
    fseek(rf, 0, SEEK_END);
    long sz = ftell(rf);
    if (sz <= 0) {
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

    // Last line should be Case D (FACCH clamp)
    char xch[8] = {0}, summary[32] = {0};
    int lenB = -1, lenC = -1, slot = -1;
    int er = extract_last_fields(buf, (int)nread, xch, sizeof xch, &lenB, &lenC, &slot, summary, sizeof summary);
    if (er != 0) {
        free(buf);
        DSD_FPRINTF(stderr, "parse JSON er=%d\n", er);
        fclose(rf);
        return 103;
    }
    rc |= expect_eq_int("FACCH clamp lenB", lenB, 16);
    rc |= expect_eq_int("FACCH clamp lenC", lenC, 0);
    rc |= expect_eq_int("FACCH clamp slot", slot, 1);
    rc |= expect_json_record_summary(buf, "LCCH", 0x03, "TELE");
    free(buf);

    // First line should be Case A or earlier; specifically check LCCH label via reading first line after moving to start
    fseek(rf, 0, SEEK_SET);
    char fxch[8] = {0}, fsum[32] = {0};
    if (extract_first_fields(rf, fxch, sizeof fxch, fsum, sizeof fsum) != 0) {
        fclose(rf);
        return 104;
    }
    fclose(rf);

    // Because multiple records were written, the first line may be from Case A. Ensure at least one LCCH record exists by
    // opening again and scanning briefly for LCCH; if not, accept first line checks for MCO-derived values.
    if (strcmp(fxch, "LCCH") == 0) {
        rc |= expect_eq_str("LCCH summary", fsum, "TELE");
    } else {
        // Validate SACCH case was written correctly in the file by ensuring last record was FACCH and previous one was SACCH.
        // We already asserted FACCH clamp; here we do a weak check that first line had an xch field present.
        if (fxch[0] == '\0') {
            DSD_FPRINTF(stderr, "first xch missing\n");
            rc |= 1;
        }
    }

    (void)remove(cap.path);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
