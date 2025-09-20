// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 MAC JSON: LCCH label and MCO clamp tests.
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

void p25_test_process_mac_vpdu_ex(int type, const unsigned char* mac_bytes, int mac_len, int is_lcch, int currentslot);

// Stubs for linked symbols referenced by P25 P2 MAC path
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
extract_json_fields(const char* buf, int len, char* out_xch, size_t xch_cap, int* out_b, int* out_c, int* out_slot,
                    char* out_summary, size_t sum_cap) {
    const char* end = buf + len;
    const char* pcur = end;
    if (pcur > buf) {
        pcur--;
    }
    if (*pcur == '\n' && pcur > buf) {
        pcur--;
    }
    const char* line = pcur;
    while (line > buf && *(line - 1) != '\n') {
        line--;
    }

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
    p = strstr(line, "\"lenB\":");
    if (!p || sscanf(p, "\"lenB\":%d", &b) != 1) {
        return -1;
    }
    p = strstr(line, "\"lenC\":");
    if (!p || sscanf(p, "\"lenC\":%d", &c) != 1) {
        return -2;
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
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    setenv("DSD_NEO_PDU_JSON", "1", 1);
    dsd_neo_config_init(NULL);

    char tmpl[] = "/tmp/p25_mac_json_more_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        fprintf(stderr, "mkstemp: %s\n", strerror(errno));
        return 100;
    }
    if (!freopen(tmpl, "w+", stderr)) {
        fprintf(stderr, "freopen stderr failed\n");
        return 101;
    }

    // Case 1: LCCH labeling with is_lcch flag
    {
        unsigned char mac[24];
        memset(mac, 0, sizeof mac);
        mac[1] = 0x03; // IDLE
        mac[2] = 0x00; // standard MFID
        p25_test_process_mac_vpdu_ex(0 /*FACCH*/, mac, 24, /*is_lcch*/ 1, /*slot*/ 0);
    }

    // Case 2: FACCH MCO clamp beyond capacity
    {
        unsigned char mac[24];
        memset(mac, 0, sizeof mac);
        mac[0] = 1;  // header-present hint
        mac[1] = 63; // opcode with MCO=63 → guess=62 → clamp to 16
        mac[2] = 0x00;
        p25_test_process_mac_vpdu_ex(0 /*FACCH*/, mac, 24, /*is_lcch*/ 0, /*slot*/ 1);
    }

    fflush(stderr);
    FILE* rf = fopen(tmpl, "rb");
    if (!rf) {
        return 102;
    }
    fseek(rf, 0, SEEK_END);
    long sz = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, rf);
    buf[sz] = '\0';
    fclose(rf);

    // Parse last (FACCH clamp)
    char xch[8], summary[16];
    int lenB = -1, lenC = -1, slot = -1;
    int er = extract_json_fields(buf, (int)sz, xch, sizeof xch, &lenB, &lenC, &slot, summary, sizeof summary);
    free(buf);
    if (er != 0) {
        fprintf(stderr, "parse JSON er=%d\n", er);
        return 103;
    }
    rc |= expect_eq_int("FACCH lenB clamp", lenB, 16);
    rc |= expect_eq_int("FACCH lenC", lenC, 0);
    rc |= expect_eq_int("FACCH slot", slot, 1);

    // Now re-open file to read first line for LCCH case (simple scan)
    rf = fopen(tmpl, "rb");
    if (!rf) {
        return 104;
    }
    char line[256] = {0};
    if (!fgets(line, sizeof line, rf)) {
        fclose(rf);
        return 105;
    }
    fclose(rf);
    char xch0[8];
    summary[0] = '\0';
    lenB = lenC = slot = -1;
    er = extract_json_fields(line, (int)strlen(line), xch0, sizeof xch0, &lenB, &lenC, &slot, summary, sizeof summary);
    if (er != 0) {
        return 106;
    }
    rc |= expect_eq_str("LCCH label", xch0, "LCCH");
    rc |= expect_eq_str("summary tag", summary, "IDLE");

    return rc;
}
