// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 MAC vendor opcode length checks (table overrides):
 * - Motorola: MFID 0x90 with op 0x91 and 0x95 -> lenB=17
 * - Harris:   MFID 0xA4 with fixed grant/location opcodes
 * - Tait:     MFID 0xD8 op 0xB5 observed with a five-octet structure
 * All cases evaluated on SACCH (capacity 19) to avoid capacity fallback.
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

typedef struct dsd_opts dsd_opts;
typedef struct dsd_state dsd_state;

typedef struct dsdneoRuntimeConfig dsdneoRuntimeConfig;
void dsd_neo_config_init(void);
const dsdneoRuntimeConfig* dsd_neo_get_config(void);

void p25_test_process_mac_vpdu_ex(int type, const unsigned char* mac_bytes, int mac_len, int is_lcch, int currentslot);

// Alias helpers referenced by linked objects.
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
extract_last_lenB(const char* buf, int len) {
    if (!buf || len <= 0) {
        return -1;
    }
    // Find last JSON object by scanning backwards for '{'
    const char* p = strrchr(buf, '{');
    if (!p) {
        return -1;
    }
    int b = -1;
    if (!parse_json_int_field(p, "\"lenB\":", &b)) {
        return -2;
    }
    return b;
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
run_one(uint8_t mfid, uint8_t opcode, uint8_t len_octet, int want_lenB) {
    // Enable JSON
    setenv("DSD_NEO_PDU_JSON", "1", 1);
    dsd_neo_config_init();

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "p25_mac_json_vendor") != 0) {
        DSD_FPRINTF(stderr, "Failed to capture stderr: %s\n", strerror(errno));
        return 101;
    }

    unsigned char mac[24];
    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[1] = opcode;
    mac[2] = mfid;
    mac[3] = len_octet;
    p25_test_process_mac_vpdu_ex(1 /* SACCH */, mac, 24, 0, 0);

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
        return 102;
    }
    fseek(rf, 0, SEEK_SET);
    size_t alloc = (size_t)sz + 1;
    char* buf = (char*)calloc(alloc, 1);
    if (!buf) {
        fclose(rf);
        return 103;
    }
    size_t nread = fread(buf, 1, alloc - 1, rf);
    if (nread >= alloc) {
        nread = alloc - 1;
    }
    fclose(rf);

    int lenB = extract_last_lenB(buf, (int)nread);
    free(buf);
    if (lenB < 0) {
        DSD_FPRINTF(stderr, "failed to parse lenB (er=%d)\n", lenB);
        return 104;
    }
    (void)remove(cap.path);
    return expect_eq_int("lenB", lenB, want_lenB);
}

int
main(void) {
    int rc = 0;
    rc |= run_one(0x90, 0x85, 0x09, 9);  // Motorola BSI, MCO 0x05
    rc |= run_one(0x90, 0x91, 0x00, 17); // Motorola
    rc |= run_one(0x90, 0x95, 0x00, 17); // Motorola
    rc |= run_one(0xA4, 0xA0, 0x2A, 9);  // Harris private data grant
    rc |= run_one(0xA4, 0xAA, 0x21, 17); // Harris GPS
    rc |= run_one(0xA4, 0xAC, 0x2C, 12); // Harris unit-to-unit data grant
    rc |= run_one(0xD8, 0xB5, 0x00, 5);  // Tait
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
