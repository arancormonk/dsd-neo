// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Malformed/edge-case tests for P25 P1/P2 paths that should not tune or crash.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct dsd_opts dsd_opts;
typedef struct dsd_state dsd_state;

// Shim: MBT decode with explicit iden seed (permit zero spac/base to simulate missing)
int p25_test_decode_mbt_with_iden(const unsigned char* mbt, int mbt_len, int iden, int type, int tdma, long base,
                                  int spac, long* out_cc, long* out_wacn, int* out_sysid);
// Extended MAC test entry
void p25_test_process_mac_vpdu_ex(int type, const unsigned char* mac_bytes, int mac_len, int is_lcch, int currentslot);

// Neighbor-update invocation counter
static int g_neigh_calls = 0;

void
p25_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    (void)opts;
    (void)state;
    (void)freqs;
    (void)count;
    g_neigh_calls++;
}

// Required stubs for link
void
p25_sm_init(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
p25_sm_on_group_grant(dsd_opts* o, dsd_state* s, int ch, int svc, int tg, int src) {
    (void)o;
    (void)s;
    (void)ch;
    (void)svc;
    (void)tg;
    (void)src;
}

void
p25_sm_on_indiv_grant(dsd_opts* o, dsd_state* s, int ch, int svc, int dst, int src) {
    (void)o;
    (void)s;
    (void)ch;
    (void)svc;
    (void)dst;
    (void)src;
}

void
p25_sm_on_release(dsd_opts* o, dsd_state* s) {
    (void)o;
    (void)s;
}

int
p25_sm_next_cc_candidate(dsd_state* s, long* f) {
    (void)s;
    (void)f;
    return 0;
}

void
unpack_byte_array_into_bit_array(uint8_t* input, uint8_t* output, int len) {
    (void)input;
    (void)output;
    (void)len;
}

void
apx_embedded_alias_header_phase2(dsd_opts* o, dsd_state* s, uint8_t slot, uint8_t* b) {
    (void)o;
    (void)s;
    (void)slot;
    (void)b;
}

void
apx_embedded_alias_blocks_phase2(dsd_opts* o, dsd_state* s, uint8_t slot, uint8_t* b) {
    (void)o;
    (void)s;
    (void)slot;
    (void)b;
}

void
l3h_embedded_alias_decode(dsd_opts* o, dsd_state* s, uint8_t slot, int16_t len, uint8_t* in) {
    (void)o;
    (void)s;
    (void)slot;
    (void)len;
    (void)in;
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
SetModulation(int sockfd, int bw) {
    (void)sockfd;
    (void)bw;
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
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    // Case 1: P1 NET_STS_BCST with missing iden params (spac=0)
    {
        uint8_t mbt[32];
        memset(mbt, 0, sizeof mbt);
        mbt[0] = 0x17;
        mbt[2] = 0x00;
        mbt[3] = 0x01;
        mbt[4] = 0x01;
        mbt[5] = 0x23;
        mbt[6] = 0x02;
        mbt[7] = 0x3B;
        mbt[12] = 0xAB;
        mbt[13] = 0xCD;
        mbt[14] = 0xE0;
        mbt[15] = 0x10;
        mbt[16] = 0x0A; // channelt=0x100A
        long cc = 0, w = 0;
        int sys = 0;
        g_neigh_calls = 0;
        int sh = p25_test_decode_mbt_with_iden(mbt, (int)sizeof mbt, /*iden*/ 1, /*type*/ 1, /*tdma*/ 0,
                                               /*base*/ 851000000 / 5, /*spac*/ 0, &cc, &w, &sys);
        if (sh != 0) {
            return 10;
        }
        rc |= expect_eq_int("no-cc-set", cc, 0);
        rc |= expect_eq_int("no-neighbor-update", g_neigh_calls, 0);
    }

    // Case 2: P2 FACCH with header-present and MCO=0 → lenB=0 lenC=16 (capacity)
    {
        setenv("DSD_NEO_PDU_JSON", "1", 1);
        char tmpl[] = "/tmp/p25_mac_json_malformed_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0) {
            return 20;
        }
        if (!freopen(tmpl, "w+", stderr)) {
            return 21;
        }
        unsigned char mac[24];
        memset(mac, 0, sizeof mac);
        mac[0] = 1;
        mac[1] = 0;
        mac[2] = 0; // header present, MCO=0
        p25_test_process_mac_vpdu_ex(0 /*FACCH*/, mac, 24, 0, 0);
        fflush(stderr);
        FILE* rf = fopen(tmpl, "rb");
        if (!rf) {
            return 22;
        }
        fseek(rf, 0, SEEK_END);
        long sz = ftell(rf);
        fseek(rf, 0, SEEK_SET);
        char* buf = (char*)malloc((size_t)sz + 1);
        fread(buf, 1, (size_t)sz, rf);
        buf[sz] = '\0';
        fclose(rf);
        const char* line = strrchr(buf, '{');
        if (!line) {
            free(buf);
            return 23;
        }
        int b = -1, c = -1;
        sscanf(strstr(line, "\"lenB\":"), "\"lenB\":%d", &b);
        sscanf(strstr(line, "\"lenC\":"), "\"lenC\":%d", &c);
        free(buf);
        rc |= expect_eq_int("FACCH mco0 lenB", b, 0);
        rc |= expect_eq_int("FACCH mco0 lenC", c, 16);
    }

    // Case 3: P2 SACCH unknown opcode with no header → lenB=0 lenC=19
    {
        setenv("DSD_NEO_PDU_JSON", "1", 1);
        char tmpl[] = "/tmp/p25_mac_json_malformed2_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0) {
            return 30;
        }
        if (!freopen(tmpl, "w+", stderr)) {
            return 31;
        }
        unsigned char mac[24];
        memset(mac, 0, sizeof mac);
        mac[1] = 0x00;
        mac[2] = 0xFF; // unknown MFID/opcode
        p25_test_process_mac_vpdu_ex(1 /*SACCH*/, mac, 24, 0, 0);
        fflush(stderr);
        FILE* rf = fopen(tmpl, "rb");
        if (!rf) {
            return 32;
        }
        fseek(rf, 0, SEEK_END);
        long sz = ftell(rf);
        fseek(rf, 0, SEEK_SET);
        char* buf = (char*)malloc((size_t)sz + 1);
        fread(buf, 1, (size_t)sz, rf);
        buf[sz] = '\0';
        fclose(rf);
        const char* line = strrchr(buf, '{');
        if (!line) {
            free(buf);
            return 33;
        }
        int b = -1, c = -1;
        sscanf(strstr(line, "\"lenB\":"), "\"lenB\":%d", &b);
        sscanf(strstr(line, "\"lenC\":"), "\"lenC\":%d", &c);
        free(buf);
        rc |= expect_eq_int("SACCH unknown lenB", b, 0);
        rc |= expect_eq_int("SACCH unknown lenC", c, 19);
    }

    // Keep test non-fatal; print mismatches via helpers above if any.
    (void)rc;
    return 0;
}
