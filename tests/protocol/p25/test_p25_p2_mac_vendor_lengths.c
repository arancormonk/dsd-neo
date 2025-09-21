// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 MAC vendor opcode length checks (table overrides):
 * - Motorola: MFID 0x90 with op 0x91 and 0x95 → lenB=17
 * - Harris:   MFID 0xB0 generic op → lenB=17
 * - Tait:     MFID 0xB5 generic op → lenB=5
 * - Harris extra: MFID 0x81 → lenB=7
 * All cases evaluated on SACCH (capacity 19) to avoid fallback clamp.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct dsd_opts dsd_opts;
typedef struct dsd_state dsd_state;

typedef struct dsdneoRuntimeConfig dsdneoRuntimeConfig;
void dsd_neo_config_init(const dsd_opts* opts);
const dsdneoRuntimeConfig* dsd_neo_get_config(void);

void p25_test_process_mac_vpdu(int type, const unsigned char* mac_bytes, int mac_len);

// Stubs for alias helpers and rigctl/rtl hooks referenced in linked objects
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

static int
extract_last_lenB(const char* buf, int len) {
    // Find last JSON object by scanning backwards for '{'
    const char* p = buf + len;
    while (p > buf && *p != '{') {
        p--;
    }
    if (p == buf && *p != '{') {
        return -1;
    }
    int b = -1;
    const char* q = strstr(p, "\"lenB\":");
    if (!q || sscanf(q, "\"lenB\":%d", &b) != 1) {
        return -2;
    }
    return b;
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
run_one(uint8_t mfid, uint8_t opcode, int want_lenB) {
    // Enable JSON
    setenv("DSD_NEO_PDU_JSON", "1", 1);
    dsd_neo_config_init(NULL);

    // New temp file
    char tmpl[] = "/tmp/p25_mac_json_vendor_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        fprintf(stderr, "mkstemp failed: %s\n", strerror(errno));
        return 100;
    }
    if (!freopen(tmpl, "w+", stderr)) {
        fprintf(stderr, "freopen stderr failed\n");
        return 101;
    }

    unsigned char mac[24];
    memset(mac, 0, sizeof(mac));
    mac[1] = opcode;
    mac[2] = mfid;
    p25_test_process_mac_vpdu(1 /* SACCH */, mac, 24);

    fflush(stderr);
    FILE* rf = fopen(tmpl, "rb");
    if (!rf) {
        fprintf(stderr, "fopen read failed\n");
        return 102;
    }
    fseek(rf, 0, SEEK_END);
    long sz = ftell(rf);
    if (sz < 0) {
        fclose(rf);
        return 102;
    }
    fseek(rf, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(rf);
        return 103;
    }
    fread(buf, 1, (size_t)sz, rf);
    buf[sz] = '\0';
    fclose(rf);

    int lenB = extract_last_lenB(buf, (int)sz);
    free(buf);
    if (lenB < 0) {
        fprintf(stderr, "failed to parse lenB (er=%d)\n", lenB);
        return 104;
    }
    return expect_eq_int("lenB", lenB, want_lenB);
}

int
main(void) {
    int rc = 0;
    rc |= run_one(0x90, 0x91, 17); // Motorola
    rc |= run_one(0x90, 0x95, 17); // Motorola
    rc |= run_one(0xB0, 0x12, 17); // Harris generic
    rc |= run_one(0xB5, 0x34, 5);  // Tait generic
    rc |= run_one(0x81, 0x20, 7);  // Harris extra
    return rc;
}
