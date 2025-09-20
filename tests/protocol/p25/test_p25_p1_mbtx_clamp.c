// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25p1 MBT negative clamp: ensure invalid CHAN-T does not retune
 * and diagnostic notice is emitted.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Minimal types
typedef struct dsd_opts dsd_opts;
typedef struct dsd_state dsd_state;

// Shim: decode one MBT using seeded IDEN parameters and return fields
int p25_test_decode_mbt_with_iden(const unsigned char* mbt, int mbt_len, int iden, int type, int tdma, long base,
                                  int spac, long* out_cc, long* out_wacn, int* out_sysid);

// Stubs for trunk SM hooks referenced by trunking decoder
void
p25_sm_init(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
p25_sm_on_group_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src) {
    (void)opts;
    (void)state;
    (void)channel;
    (void)svc_bits;
    (void)tg;
    (void)src;
}

void
p25_sm_on_indiv_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src) {
    (void)opts;
    (void)state;
    (void)channel;
    (void)svc_bits;
    (void)dst;
    (void)src;
}

void
p25_sm_on_release(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
p25_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    (void)opts;
    (void)state;
    (void)freqs;
    (void)count;
}

void
p25_sm_tick(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

int
p25_sm_next_cc_candidate(dsd_state* state, long* out_freq) {
    (void)state;
    (void)out_freq;
    return 0;
}

// Additional stubs referenced by linked objects (rigctl/rtl streaming)
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

// Alias decode helpers stubbed
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

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        fprintf(stderr, "%s: expected true\n", tag);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    // Build ALT MBT NET_STS_BCST with CHAN-T referencing iden=1 while we seed iden=0 only.
    uint8_t mbt[48];
    memset(mbt, 0, sizeof(mbt));
    mbt[0] = 0x17;  // ALT format
    mbt[2] = 0x00;  // MFID standard
    mbt[6] = 0x02;  // blks
    mbt[7] = 0x3B;  // NET_STS_BCST opcode
    mbt[3] = 0x01;  // LRA
    mbt[4] = 0x01;  // SYSID hi
    mbt[5] = 0x23;  // SYSID lo -> 0x123
    mbt[12] = 0xAB; // WACN 19..12
    mbt[13] = 0xCD; // WACN 11..4
    mbt[14] = 0xE0; // WACN 3..0
    mbt[15] = 0x10; // CHAN-T hi (iden=1)
    mbt[16] = 0x0A; // CHAN-T lo (ch=10)

    // Capture stderr to parse diagnostic
    char tmpl[] = "/tmp/p25_p1_mbt_clamp_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        fprintf(stderr, "mkstemp failed: %s\n", strerror(errno));
        return 100;
    }
    if (!freopen(tmpl, "w+", stderr)) {
        fprintf(stderr, "freopen stderr failed\n");
        return 101;
    }

    long cc = -1, wacn = -1;
    int sysid = -1;
    // Seed only iden=0 (different than CHAN-T’s iden=1) so mapping should be rejected.
    int sh = p25_test_decode_mbt_with_iden(mbt, (int)sizeof(mbt), /*iden*/ 0, /*type*/ 1, /*tdma*/ 0,
                                           /*base*/ 851000000 / 5, /*spac*/ 100, &cc, &wacn, &sysid);
    if (sh != 0) {
        fprintf(stderr, "shim failed: %d\n", sh);
        return 102;
    }

    // Read back stderr
    fflush(stderr);
    FILE* rf = fopen(tmpl, "rb");
    if (!rf) {
        fprintf(stderr, "fopen read failed\n");
        return 103;
    }
    fseek(rf, 0, SEEK_END);
    long sz = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(rf);
        return 104;
    }
    fread(buf, 1, (size_t)sz, rf);
    buf[sz] = '\0';
    fclose(rf);

    // Clamp expectations: cc should remain 0 (no retune), and diagnostic text present
    rc |= expect_true("cc not updated", cc == 0);
    rc |= expect_true("diag present", strstr(buf, "ignoring invalid channel->freq") != NULL);
    free(buf);
    return rc;
}
