// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Verify P25 Phase 1 MBT decode for Network Status Broadcast (0x3B)
 * updates CC frequency and system identifiers using pre-seeded IDEN tables.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm_api.h>
#include <dsd-neo/protocol/p25/p25p1_pdu_trunking.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

struct RtlSdrContext;

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Test shim wrapper: decode one MBT using seeded IDEN table and return fields
int p25_test_decode_mbt_with_iden(const unsigned char* mbt, int mbt_len, int iden, int type, int tdma, long base,
                                  int spac, long* out_cc, long* out_wacn, int* out_sysid);

static void
sm_noop_init(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static void
sm_noop_on_group_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src) {
    (void)opts;
    (void)state;
    (void)channel;
    (void)svc_bits;
    (void)tg;
    (void)src;
}

static void
sm_noop_on_indiv_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src) {
    (void)opts;
    (void)state;
    (void)channel;
    (void)svc_bits;
    (void)dst;
    (void)src;
}

static void
sm_noop_on_release(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static void
sm_noop_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    (void)opts;
    (void)state;
    (void)freqs;
    (void)count;
}

static void
sm_noop_tick(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static int
sm_noop_next_cc_candidate(dsd_state* state, long* out_freq) {
    (void)state;
    (void)out_freq;
    return 0;
}

static p25_sm_api
sm_noop_api(void) {
    p25_sm_api api = {0};
    api.init = sm_noop_init;
    api.on_group_grant = sm_noop_on_group_grant;
    api.on_indiv_grant = sm_noop_on_indiv_grant;
    api.on_release = sm_noop_on_release;
    api.on_neighbor_update = sm_noop_on_neighbor_update;
    api.next_cc_candidate = sm_noop_next_cc_candidate;
    api.tick = sm_noop_tick;
    return api;
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

// Alias decode helpers stubbed as they may be referenced by linked objects
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
unpack_byte_array_into_bit_array(const uint8_t* input, uint8_t* output, int len) {
    (void)input;
    (void)output;
    (void)len;
}

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
expect_eq_long(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
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
expect_contains_text(const char* tag, const char* text, const char* needle) {
    if (!text || !needle || strstr(text, needle) == NULL) {
        DSD_FPRINTF(stderr, "%s: missing '%s' in '%s'\n", tag, needle ? needle : "(null)", text ? text : "(null)");
        return 1;
    }
    return 0;
}

static int
read_capture_file(const char* path, char* out, size_t out_sz) {
    if (!path || !out || out_sz == 0) {
        return -1;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    size_t n = fread(out, 1, out_sz - 1, f);
    out[n] = '\0';
    fclose(f);
    return 0;
}

int
main(void) {
    int rc = 0;

    {
        p25_sm_api api = sm_noop_api();
        p25_sm_set_api(&api);
    }

    // Craft ALT MBT: NET_STS_BCST (0x3B), channelt=0x100A (iden=1, ch=10), WACN=0xABCDE, SYSID=0x123
    uint8_t mbt[48];
    DSD_MEMSET(mbt, 0, sizeof(mbt));
    mbt[0] = 0x17;  // ALT format
    mbt[2] = 0x00;  // MFID standard
    mbt[6] = 0x02;  // blks=2 (enough payload)
    mbt[7] = 0x3B;  // opcode
    mbt[3] = 0x01;  // LRA
    mbt[4] = 0x01;  // SYSID hi (low nibble used)
    mbt[5] = 0x23;  // SYSID lo -> 0x123
    mbt[12] = 0xAB; // WACN bits 19..12
    mbt[13] = 0xCD; // WACN bits 11..4
    mbt[14] = 0xE0; // WACN bits 3..0 (<<4)
    mbt[15] = 0x10; // CHAN-T hi
    mbt[16] = 0x0A; // CHAN-T lo
    // CHAN-R optional

    long cc = 0, wacn = 0;
    int sysid = 0;
    int sh = p25_test_decode_mbt_with_iden(mbt, (int)sizeof(mbt), /*iden*/ 1, /*type*/ 1, /*tdma*/ 0,
                                           /*base*/ 851000000 / 5, /*spac*/ 100, &cc, &wacn, &sysid);
    if (sh != 0) {
        DSD_FPRINTF(stderr, "shim invocation failed (%d)\n", sh);
        return 99;
    }

    long want_freq = 851000000 + 10 * 100 * 125; // 851.125 MHz
    rc |= expect_eq_long("p25_cc_freq", cc, want_freq);
    rc |= expect_eq_long("p2_wacn", wacn, 0xABCDE);
    rc |= expect_eq_int("p2_sysid", sysid, 0x123);

    // AMBTC Group Affiliation Response (0x28): accepted response tracks TA -> GA only.
    {
        dsd_opts opts;
        dsd_state state;
        uint8_t aff[48];
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        DSD_MEMSET(aff, 0, sizeof aff);

        state.p25_cc_freq = 851000000;
        state.trunk_cc_freq = 851000000;
        state.p2_wacn = 0x11111;
        state.p2_sysid = 0x222;

        aff[0] = 0x17; // ALT MBT only
        aff[2] = 0x00; // MFID
        aff[3] = 0x01;
        aff[4] = 0x23;
        aff[5] = 0x45; // TA
        aff[6] = 0x02;
        aff[7] = 0x28; // Group Affiliation Response
        aff[8] = 0xAB;
        aff[9] = 0xCD;
        aff[12] = 0xE1; // WACN low nibble + SYSID high nibble
        aff[13] = 0x23;
        aff[14] = 0x56;
        aff[15] = 0x78; // GID
        aff[16] = 0x12;
        aff[17] = 0x34; // AGA
        aff[18] = 0x45;
        aff[19] = 0x67; // GA
        aff[20] = 0x80; // LG=1, GAV=0 accepted

        dsd_test_capture_stderr cap;
        if (dsd_test_capture_stderr_begin(&cap, "p25_mbt_aff_rsp") != 0) {
            return 100;
        }
        p25_decode_pdu_trunking(&opts, &state, aff);
        dsd_test_capture_stderr_end(&cap);

        char out[2048];
        if (read_capture_file(cap.path, out, sizeof out) != 0) {
            return 101;
        }

        rc |= expect_eq_int("mbt 0x28 aff count", state.p25_aff_count, 1);
        rc |= expect_eq_int("mbt 0x28 ga count", state.p25_ga_count, 1);
        rc |= expect_eq_long("mbt 0x28 TA", state.p25_aff_rid[0], 0x012345);
        rc |= expect_eq_long("mbt 0x28 GA rid", state.p25_ga_rid[0], 0x012345);
        rc |= expect_eq_long("mbt 0x28 GA tg", state.p25_ga_tg[0], 0x4567);
        rc |= expect_eq_long("mbt 0x28 preserves p25 cc", state.p25_cc_freq, 851000000);
        rc |= expect_eq_long("mbt 0x28 preserves trunk cc", state.trunk_cc_freq, 851000000);
        rc |= expect_eq_long("mbt 0x28 preserves wacn", (long)state.p2_wacn, 0x11111);
        rc |= expect_eq_int("mbt 0x28 preserves sysid", state.p2_sysid, 0x222);
        rc |= expect_contains_text("mbt 0x28 WACN/SYSID", out, "WACN [ABCDE] SYSID [123]");
        rc |= expect_contains_text("mbt 0x28 GID", out, "GID [5678]");
        rc |= expect_contains_text("mbt 0x28 LG/GAV", out, "LG [1] GAV [0]");
        rc |= expect_contains_text("mbt 0x28 AGA", out, "AGA [4660]");
        rc |= expect_contains_text("mbt 0x28 GA", out, "GA [17767]");
        rc |= expect_contains_text("mbt 0x28 TA print", out, "TA [74565]");
    }

    // AMBTC Group Affiliation Response (0x28): rejected response does not track affiliation.
    {
        dsd_opts opts;
        dsd_state state;
        uint8_t aff[48];
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        DSD_MEMSET(aff, 0, sizeof aff);

        aff[0] = 0x17;
        aff[3] = 0x01;
        aff[4] = 0x23;
        aff[5] = 0x45;
        aff[6] = 0x02;
        aff[7] = 0x28;
        aff[18] = 0x45;
        aff[19] = 0x67;
        aff[20] = 0x02; // GAV=2 rejected

        p25_decode_pdu_trunking(&opts, &state, aff);
        rc |= expect_eq_int("mbt 0x28 rejected aff count", state.p25_aff_count, 0);
        rc |= expect_eq_int("mbt 0x28 rejected ga count", state.p25_ga_count, 0);
    }

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
