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
int p25_decode_pdu_trunking_bounded(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte, size_t mpdu_len);

static int g_indiv_grant_count;
static int g_group_grant_count;
static int g_last_indiv_channel;
static int g_last_indiv_svc;
static int g_last_indiv_dst;
static int g_last_indiv_src;

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
    g_group_grant_count++;
}

static void
sm_noop_on_indiv_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src) {
    (void)opts;
    (void)state;
    g_indiv_grant_count++;
    g_last_indiv_channel = channel;
    g_last_indiv_svc = svc_bits;
    g_last_indiv_dst = dst;
    g_last_indiv_src = src;
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

static void
reset_indiv_grants(void) {
    g_indiv_grant_count = 0;
    g_group_grant_count = 0;
    g_last_indiv_channel = 0;
    g_last_indiv_svc = 0;
    g_last_indiv_dst = 0;
    g_last_indiv_src = 0;
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
expect_not_contains_text(const char* tag, const char* text, const char* needle) {
    if (text && needle && strstr(text, needle) != NULL) {
        DSD_FPRINTF(stderr, "%s: unexpected '%s' in '%s'\n", tag, needle, text);
        return 1;
    }
    return 0;
}

static void
seed_fdma_iden(dsd_state* state, int iden) {
    state->p25_chan_iden = iden & 0xF;
    state->p25_iden_fdma[iden].base_freq = 851000000L / 5L;
    state->p25_iden_fdma[iden].chan_type = 1;
    state->p25_iden_fdma[iden].chan_spac = 100;
    state->p25_iden_fdma[iden].trust = 2;
    state->p25_iden_fdma[iden].populated = 1;
    state->p25_chan_tdma_explicit[iden] = 1;
}

static void
seed_tdma_iden(dsd_state* state, int iden) {
    state->p25_chan_iden = iden & 0xF;
    state->p25_iden_tdma[iden].base_freq = 851000000L / 5L;
    state->p25_iden_tdma[iden].chan_type = 3;
    state->p25_iden_tdma[iden].chan_spac = 100;
    state->p25_iden_tdma[iden].trust = 2;
    state->p25_iden_tdma[iden].populated = 1;
    state->p25_chan_tdma_explicit[iden] = 2;
}

static void
init_private_trunking(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->p25_trunk = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_tune_data_calls = 1;
    opts->trunk_tune_enc_calls = 1;
    state->p25_cc_freq = 851000000L;
}

static void
build_ambtc_unit_to_unit(uint8_t* mbt, uint8_t opcode, uint8_t svc, uint16_t channelt, uint16_t channelr) {
    DSD_MEMSET(mbt, 0, 48);
    mbt[0] = 0x37;
    mbt[2] = 0x00;
    mbt[6] = 0x02;
    mbt[7] = opcode;
    mbt[3] = 0x01;
    mbt[4] = 0x23;
    mbt[5] = 0x45; // local source
    mbt[8] = svc;
    mbt[9] = 0xAB; // target WACN high 8 bits

    mbt[12] = 0x12;
    mbt[13] = 0x34;
    mbt[14] = 0x56;
    mbt[15] = 0x78; // source FQ: WACN 12345, SYS 678
    mbt[16] = 0x23;
    mbt[17] = 0x45;
    mbt[18] = 0x67; // source FQ ID
    mbt[19] = 0x0A;
    mbt[20] = 0xBC;
    mbt[21] = 0xDE; // local target
    mbt[22] = (uint8_t)(channelt >> 8);
    mbt[23] = (uint8_t)(channelt & 0xFFU);

    mbt[24] = (uint8_t)(channelr >> 8);
    mbt[25] = (uint8_t)(channelr & 0xFFU);
    mbt[26] = 0xCD;
    mbt[27] = 0xE2;
    mbt[28] = 0x34; // target FQ: WACN ABCDE, SYS 234
    mbt[29] = 0x45;
    mbt[30] = 0x67;
    mbt[31] = 0x89; // target FQ ID
}

static void
build_ambtc_base(uint8_t* mbt, uint8_t opcode, uint8_t blocks, uint32_t header_address) {
    DSD_MEMSET(mbt, 0, 48);
    mbt[0] = 0x37;
    mbt[2] = 0x00;
    mbt[3] = (uint8_t)((header_address >> 16) & 0xFFU);
    mbt[4] = (uint8_t)((header_address >> 8) & 0xFFU);
    mbt[5] = (uint8_t)(header_address & 0xFFU);
    mbt[6] = blocks;
    mbt[7] = opcode;
}

static void
build_ambtc_unit_answer(uint8_t* mbt) {
    build_ambtc_base(mbt, 0x05, 0x01, 0x0ABCDE);
    mbt[8] = 0x82;
    mbt[13] = 0x12;
    mbt[14] = 0x34;
    mbt[15] = 0x56;
    mbt[16] = 0x78;
    mbt[17] = 0x23;
    mbt[18] = 0x45;
    mbt[19] = 0x67;
}

static void
build_ambtc_extended_command(uint8_t* mbt, uint8_t opcode, uint8_t byte17, uint8_t byte18) {
    build_ambtc_base(mbt, opcode, 0x02, 0x0ABCDE);
    mbt[8] = 0x12;
    mbt[9] = 0x34;
    mbt[12] = 0x56;
    mbt[13] = 0x78;
    mbt[14] = 0x23;
    mbt[15] = 0x45;
    mbt[16] = 0x67;
    mbt[17] = byte17;
    mbt[18] = byte18;
    mbt[22] = 0x01;
    mbt[23] = 0x23;
    mbt[24] = 0x45;
    mbt[25] = 0xAB;
    mbt[26] = 0xCD;
    mbt[27] = 0xE2;
    mbt[28] = 0x34;
    mbt[29] = 0x45;
    mbt[30] = 0x67;
    mbt[31] = 0x89;
}

static void
build_ambtc_group_affiliation_query(uint8_t* mbt) {
    build_ambtc_base(mbt, 0x2A, 0x01, 0x0ABCDE);
    mbt[8] = 0x12;
    mbt[9] = 0x34;
    mbt[12] = 0x56;
    mbt[13] = 0x78;
    mbt[14] = 0x23;
    mbt[15] = 0x45;
    mbt[16] = 0x67;
}

static void
build_ambtc_roaming(uint8_t* mbt, uint8_t opcode) {
    build_ambtc_base(mbt, opcode, 0x01, 0x0ABCDE);
    mbt[8] = 0x85;
    mbt[9] = 0xAB;
    mbt[12] = 0xCD;
    mbt[13] = 0xE2;
    mbt[14] = 0x34;
}

static void
build_ambtc_individual_data_grant(uint8_t* mbt) {
    build_ambtc_base(mbt, 0x10, 0x02, 0x012345);
    mbt[8] = 0x04;
    mbt[12] = 0x12;
    mbt[13] = 0x34;
    mbt[14] = 0x56;
    mbt[15] = 0x78;
    mbt[16] = 0x23;
    mbt[17] = 0x45;
    mbt[18] = 0x67;
    mbt[19] = 0x0A;
    mbt[20] = 0xBC;
    mbt[21] = 0xDE;
    mbt[22] = 0x10;
    mbt[23] = 0x0A;
    mbt[24] = 0x10;
    mbt[25] = 0x10;
}

static void
build_ambtc_group_data_grant(uint8_t* mbt) {
    build_ambtc_base(mbt, 0x11, 0x01, 0x012345);
    mbt[8] = 0x04;
    mbt[14] = 0x10;
    mbt[15] = 0x0A;
    mbt[16] = 0x10;
    mbt[17] = 0x10;
    mbt[18] = 0x12;
    mbt[19] = 0x34;
}

static void
build_umbtc_grant_like(uint8_t* mbt, uint8_t opcode, uint8_t mfid) {
    DSD_MEMSET(mbt, 0, 48);
    mbt[0] = 0x35; // outbound UMBTC
    mbt[1] = 0x3D; // trunking SAP
    mbt[2] = mfid;
    mbt[3] = 0x01;
    mbt[4] = 0x23;
    mbt[5] = 0x45; // source if misdecoded as AMBTC
    mbt[6] = 0x02;
    mbt[8] = 0x04; // service options if misdecoded as AMBTC
    mbt[12] = opcode;
    mbt[14] = 0x10;
    mbt[15] = 0x0A;
    mbt[16] = 0x10;
    mbt[17] = 0x10;
    mbt[18] = 0x12;
    mbt[19] = 0x34;
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

static int
capture_mbt_output(const char* name, const uint8_t* mbt, size_t mbt_len, char* out, size_t out_sz) {
    static dsd_opts opts;
    static dsd_state state;
    init_private_trunking(&opts, &state);
    seed_fdma_iden(&state, 1);
    reset_indiv_grants();

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, name) != 0) {
        return -1;
    }
    (void)p25_decode_pdu_trunking_bounded(&opts, &state, mbt, mbt_len);
    dsd_test_capture_stderr_end(&cap);

    return read_capture_file(cap.path, out, out_sz);
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
    mbt[0] = 0x37;  // outbound ALT format
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

    // AMBTC Unit-to-Unit Voice Channel Grant (0x04): resolved FDMA channel dispatches one private grant.
    {
        static dsd_opts opts;
        static dsd_state state;
        uint8_t uu[48];
        init_private_trunking(&opts, &state);
        seed_fdma_iden(&state, 1);
        build_ambtc_unit_to_unit(uu, 0x04, 0x00, 0x100A, 0x100A);
        reset_indiv_grants();

        dsd_test_capture_stderr cap;
        if (dsd_test_capture_stderr_begin(&cap, "p25_mbt_uu_0x04") != 0) {
            return 104;
        }
        p25_decode_pdu_trunking(&opts, &state, uu);
        dsd_test_capture_stderr_end(&cap);

        char out[4096];
        if (read_capture_file(cap.path, out, sizeof out) != 0) {
            return 105;
        }

        rc |= expect_eq_int("mbt 0x04 indiv count", g_indiv_grant_count, 1);
        rc |= expect_eq_int("mbt 0x04 channel", g_last_indiv_channel, 0x100A);
        rc |= expect_eq_int("mbt 0x04 svc", g_last_indiv_svc, 0x00);
        rc |= expect_eq_int("mbt 0x04 dst", g_last_indiv_dst, 0x0ABCDE);
        rc |= expect_eq_int("mbt 0x04 src", g_last_indiv_src, 0x012345);
        rc |= expect_contains_text("mbt 0x04 active", state.active_channel[0], "Active UU Ch: 100A");
        rc |= expect_contains_text("mbt 0x04 active src", state.active_channel[0], "SRC: 74565");
        rc |= expect_contains_text("mbt 0x04 active tgt", state.active_channel[0], "TGT: 703710");
        rc |= expect_contains_text("mbt 0x04 label", out, "Unit to Unit Voice Channel Grant - Extended");
        rc |= expect_contains_text("mbt 0x04 src fq", out, "FULL SRC [12345.678.234567]");
        rc |= expect_contains_text("mbt 0x04 tgt fq", out, "FULL TGT [ABCDE.234.456789]");
        rc |= expect_contains_text("mbt 0x04 implicit uplink", out, "CHAN-R [100A]");
    }

    // AMBTC Unit-to-Unit Voice Channel Grant Update (0x06): existing interpretation remains routeable.
    {
        static dsd_opts opts;
        static dsd_state state;
        uint8_t uu[48];
        init_private_trunking(&opts, &state);
        seed_fdma_iden(&state, 1);
        build_ambtc_unit_to_unit(uu, 0x06, 0x02, 0x100A, 0x1010);
        reset_indiv_grants();

        dsd_test_capture_stderr cap;
        if (dsd_test_capture_stderr_begin(&cap, "p25_mbt_uu_0x06") != 0) {
            return 106;
        }
        p25_decode_pdu_trunking(&opts, &state, uu);
        dsd_test_capture_stderr_end(&cap);

        char out[4096];
        if (read_capture_file(cap.path, out, sizeof out) != 0) {
            return 107;
        }

        rc |= expect_eq_int("mbt 0x06 indiv count", g_indiv_grant_count, 1);
        rc |= expect_eq_int("mbt 0x06 channel", g_last_indiv_channel, 0x100A);
        rc |= expect_eq_int("mbt 0x06 svc", g_last_indiv_svc, 0x02);
        rc |= expect_contains_text("mbt 0x06 update label", out, "Unit to Unit Voice Channel Grant Update - Extended");
        rc |= expect_contains_text("mbt 0x06 explicit uplink", out, "CHAN-R [1010]");
    }

    // Resolved metadata is preserved even when the channel is not tunable because IDEN data is missing.
    {
        static dsd_opts opts;
        static dsd_state state;
        uint8_t uu[48];
        init_private_trunking(&opts, &state);
        build_ambtc_unit_to_unit(uu, 0x04, 0x00, 0x200A, 0x200A);
        reset_indiv_grants();
        p25_decode_pdu_trunking(&opts, &state, uu);
        rc |= expect_eq_int("mbt 0x04 unresolved no grant", g_indiv_grant_count, 0);
        rc |= expect_contains_text("mbt 0x04 unresolved active", state.active_channel[0], "Active UU Ch: 200A");
    }

    // TDMA IDEN channels report the derived FDMA channel and slot in the active-channel text.
    {
        static dsd_opts opts;
        static dsd_state state;
        uint8_t uu[48];
        init_private_trunking(&opts, &state);
        seed_tdma_iden(&state, 1);
        build_ambtc_unit_to_unit(uu, 0x04, 0x00, 0x100B, 0x100B);
        reset_indiv_grants();
        p25_decode_pdu_trunking(&opts, &state, uu);
        rc |= expect_eq_int("mbt 0x04 tdma grant", g_indiv_grant_count, 1);
        rc |= expect_contains_text("mbt 0x04 tdma suffix", state.active_channel[0], "(FDMA 0005 S2)");
    }

    // Existing private-call policy blocks encrypted or held-off individual grants before SM dispatch.
    {
        static dsd_opts opts;
        static dsd_state state;
        uint8_t uu[48];
        init_private_trunking(&opts, &state);
        seed_fdma_iden(&state, 1);
        opts.trunk_tune_enc_calls = 0;
        build_ambtc_unit_to_unit(uu, 0x04, 0x40, 0x100A, 0x100A);
        reset_indiv_grants();
        p25_decode_pdu_trunking(&opts, &state, uu);
        rc |= expect_eq_int("mbt 0x04 enc lockout no grant", g_indiv_grant_count, 0);
        rc |= expect_eq_int("mbt 0x04 encrypted service valid", state.p25_service_options_valid[0], 1);
        rc |= expect_eq_int("mbt 0x04 encrypted svc stored", state.dmr_so, 0x40);

        opts.trunk_tune_enc_calls = 1;
        state.tg_hold = 0x222222;
        build_ambtc_unit_to_unit(uu, 0x04, 0x00, 0x100A, 0x100A);
        reset_indiv_grants();
        p25_decode_pdu_trunking(&opts, &state, uu);
        rc |= expect_eq_int("mbt 0x04 hold mismatch no grant", g_indiv_grant_count, 0);
    }

    // Bounded MBT decode rejects malformed/short AMBTC 0x04 safely.
    {
        static dsd_opts opts;
        static dsd_state state;
        uint8_t uu[48];
        init_private_trunking(&opts, &state);
        seed_fdma_iden(&state, 1);
        build_ambtc_unit_to_unit(uu, 0x04, 0x00, 0x100A, 0x100A);
        reset_indiv_grants();

        dsd_test_capture_stderr cap;
        if (dsd_test_capture_stderr_begin(&cap, "p25_mbt_uu_short") != 0) {
            return 108;
        }
        (void)p25_decode_pdu_trunking_bounded(&opts, &state, uu, 12U);
        dsd_test_capture_stderr_end(&cap);

        char out[2048];
        if (read_capture_file(cap.path, out, sizeof out) != 0) {
            return 109;
        }
        rc |= expect_eq_int("mbt 0x04 short no grant", g_indiv_grant_count, 0);
        rc |= expect_contains_text("mbt 0x04 short log", out, "short payload");
    }

    // Legacy MBT entry point clamps to the declared BLKS count before decoding.
    {
        static dsd_opts opts;
        static dsd_state state;
        uint8_t uu[48];
        init_private_trunking(&opts, &state);
        seed_fdma_iden(&state, 1);
        build_ambtc_unit_to_unit(uu, 0x04, 0x00, 0x100A, 0x100A);
        uu[6] = 0x00;
        reset_indiv_grants();

        dsd_test_capture_stderr cap;
        if (dsd_test_capture_stderr_begin(&cap, "p25_mbt_uu_legacy_declared_short") != 0) {
            return 110;
        }
        p25_decode_pdu_trunking(&opts, &state, uu);
        dsd_test_capture_stderr_end(&cap);

        char out[2048];
        if (read_capture_file(cap.path, out, sizeof out) != 0) {
            return 111;
        }
        rc |= expect_eq_int("mbt 0x04 legacy declared short no grant", g_indiv_grant_count, 0);
        rc |= expect_eq_int("mbt 0x04 legacy declared short inactive", state.active_channel[0][0] == '\0', 1);
        rc |= expect_contains_text("mbt 0x04 legacy declared short log", out, "short payload");
    }

    // AMBTC metadata-only decoders log useful fields and do not dispatch voice grants.
    {
        uint8_t meta[48];
        char out[4096];

        build_ambtc_unit_answer(meta);
        if (capture_mbt_output("p25_mbt_meta_0x05", meta, sizeof meta, out, sizeof out) != 0) {
            return 112;
        }
        rc |= expect_eq_int("mbt 0x05 no indiv grant", g_indiv_grant_count, 0);
        rc |= expect_eq_int("mbt 0x05 no group grant", g_group_grant_count, 0);
        rc |= expect_contains_text("mbt 0x05 label", out, "Unit to Unit Answer Request MBT - Extended");
        rc |= expect_contains_text("mbt 0x05 svc", out, "SVC [82]");
        rc |= expect_contains_text("mbt 0x05 target", out, "TO [703710]");
        rc |= expect_contains_text("mbt 0x05 source", out, "FULL SRC [12345.678.234567]");

        build_ambtc_extended_command(meta, 0x18, 0x21, 0x43);
        if (capture_mbt_output("p25_mbt_meta_0x18", meta, sizeof meta, out, sizeof out) != 0) {
            return 111;
        }
        rc |= expect_contains_text("mbt 0x18 label", out, "Status Update MBT - Extended");
        rc |= expect_contains_text("mbt 0x18 statuses", out, "UNIT STATUS [21] USER STATUS [43]");
        rc |= expect_contains_text("mbt 0x18 source", out, "FM [74565] FULL [12345.678.234567]");
        rc |= expect_contains_text("mbt 0x18 target", out, "TO [703710] FULL [ABCDE.234.456789]");

        build_ambtc_extended_command(meta, 0x1A, 0x00, 0x00);
        if (capture_mbt_output("p25_mbt_meta_0x1A", meta, sizeof meta, out, sizeof out) != 0) {
            return 112;
        }
        rc |= expect_contains_text("mbt 0x1A label", out, "Status Query MBT - Extended");
        rc |= expect_contains_text("mbt 0x1A source", out, "FM [74565] FULL [12345.678.234567]");

        build_ambtc_extended_command(meta, 0x1C, 0xBE, 0xEF);
        if (capture_mbt_output("p25_mbt_meta_0x1C", meta, sizeof meta, out, sizeof out) != 0) {
            return 113;
        }
        rc |= expect_contains_text("mbt 0x1C label", out, "Message Update MBT - Extended");
        rc |= expect_contains_text("mbt 0x1C sdm", out, "SHORT DATA [BEEF]");

        build_ambtc_extended_command(meta, 0x1F, 0x00, 0x00);
        if (capture_mbt_output("p25_mbt_meta_0x1F", meta, sizeof meta, out, sizeof out) != 0) {
            return 114;
        }
        rc |= expect_contains_text("mbt 0x1F label", out, "Call Alert MBT - Extended");
        rc |= expect_contains_text("mbt 0x1F target", out, "TO [703710] FULL [ABCDE.234.456789]");

        build_ambtc_group_affiliation_query(meta);
        if (capture_mbt_output("p25_mbt_meta_0x2A", meta, sizeof meta, out, sizeof out) != 0) {
            return 115;
        }
        rc |= expect_contains_text("mbt 0x2A label", out, "Group Affiliation Query MBT - Extended");
        rc |= expect_contains_text("mbt 0x2A source", out, "FULL SRC [12345.678.234567]");

        build_ambtc_roaming(meta, 0x36);
        if (capture_mbt_output("p25_mbt_meta_0x36", meta, sizeof meta, out, sizeof out) != 0) {
            return 116;
        }
        rc |= expect_contains_text("mbt 0x36 label", out, "Roaming Address Command MBT - Extended");
        rc |= expect_contains_text("mbt 0x36 msn", out, "MSN [5] FINAL [1] ADDR-A [ABCDE.234]");

        build_ambtc_roaming(meta, 0x37);
        if (capture_mbt_output("p25_mbt_meta_0x37", meta, sizeof meta, out, sizeof out) != 0) {
            return 117;
        }
        rc |= expect_contains_text("mbt 0x37 label", out, "Roaming Address Update MBT - Extended");
        rc |= expect_contains_text("mbt 0x37 msn", out, "MSN [5] FINAL [1] ADDR-A [ABCDE.234]");
    }

    // Obsolete AMBTC data grants are metadata-only and do not trigger voice grant callbacks.
    {
        uint8_t meta[48];
        char out[4096];

        build_ambtc_individual_data_grant(meta);
        if (capture_mbt_output("p25_mbt_data_0x10", meta, sizeof meta, out, sizeof out) != 0) {
            return 118;
        }
        rc |= expect_eq_int("mbt 0x10 no indiv grant", g_indiv_grant_count, 0);
        rc |= expect_eq_int("mbt 0x10 no group grant", g_group_grant_count, 0);
        rc |= expect_contains_text("mbt 0x10 label", out, "Individual Data Channel Grant MBT - Obsolete");
        rc |= expect_contains_text("mbt 0x10 channel", out, "CHAN-T [100A] CHAN-R [1010]");
        rc |= expect_contains_text("mbt 0x10 target", out, "TO [703710]");

        build_ambtc_group_data_grant(meta);
        if (capture_mbt_output("p25_mbt_data_0x11", meta, sizeof meta, out, sizeof out) != 0) {
            return 119;
        }
        rc |= expect_eq_int("mbt 0x11 no indiv grant", g_indiv_grant_count, 0);
        rc |= expect_eq_int("mbt 0x11 no group grant", g_group_grant_count, 0);
        rc |= expect_contains_text("mbt 0x11 label", out, "Group Data Channel Grant MBT - Obsolete");
        rc |= expect_contains_text("mbt 0x11 channel", out, "CHAN-T [100A] CHAN-R [1010]");
        rc |= expect_contains_text("mbt 0x11 group", out, "Group [4660][1234]");
    }

    // Vendor MFIDs with standard opcode collisions stay on the vendor/raw path.
    {
        uint8_t meta[48];
        char out[4096];

        build_ambtc_individual_data_grant(meta);
        meta[2] = 0x90;
        if (capture_mbt_output("p25_mbt_mfid90_collision_0x10", meta, sizeof meta, out, sizeof out) != 0) {
            return 121;
        }
        rc |= expect_contains_text("mbt mfid90 collision raw", out, "MFID 90 (Moto); Opcode: 10");
        rc |= expect_not_contains_text("mbt mfid90 collision no standard", out,
                                       "Individual Data Channel Grant MBT - Obsolete");

        build_ambtc_base(meta, 0x28, 0x02, 0x012345);
        meta[2] = 0xA4;
        if (capture_mbt_output("p25_mbt_mfid_a4_collision_0x28", meta, sizeof meta, out, sizeof out) != 0) {
            return 122;
        }
        rc |= expect_contains_text("mbt mfid a4 collision raw", out, "MFID A4 (Harris); Opcode: 28");
        rc |= expect_not_contains_text("mbt mfid a4 collision no standard", out,
                                       "Group Affiliation Response MBT - Extended");
    }

    // Outbound UMBTC opcodes use block-0 offsets and must not enter AMBTC grant handlers.
    {
        uint8_t umbtc[48];
        char out[4096];

        build_umbtc_grant_like(umbtc, 0x00, 0x00);
        if (capture_mbt_output("p25_mbt_umbtc_standard_no_ambtc", umbtc, sizeof umbtc, out, sizeof out) != 0) {
            return 123;
        }
        rc |= expect_eq_int("umbtc standard no group grant", g_group_grant_count, 0);
        rc |= expect_eq_int("umbtc standard no indiv grant", g_indiv_grant_count, 0);
        rc |= expect_contains_text("umbtc standard guard log", out, "UMBTC standard opcode 00 not handled as AMBTC");
        rc |= expect_not_contains_text("umbtc standard no ambtc grant", out,
                                       "Group Voice Channel Grant Update - Extended");

        build_umbtc_grant_like(umbtc, 0x02, 0x90);
        if (capture_mbt_output("p25_mbt_umbtc_mfid90_no_ambtc", umbtc, sizeof umbtc, out, sizeof out) != 0) {
            return 124;
        }
        rc |= expect_eq_int("umbtc mfid90 no group grant", g_group_grant_count, 0);
        rc |= expect_eq_int("umbtc mfid90 no indiv grant", g_indiv_grant_count, 0);
        rc |= expect_contains_text("umbtc mfid90 raw", out, "MFID 90 (Moto); Opcode: 02");
        rc |= expect_not_contains_text("umbtc mfid90 no ambtc grant", out,
                                       "MFID90 Group Regroup Channel Grant - Explicit");
    }

    // Each new metadata/data opcode has an explicit short-payload guard.
    {
        static const uint8_t short_ops[] = {0x05, 0x10, 0x11, 0x18, 0x1A, 0x1C, 0x1F, 0x2A, 0x36, 0x37};
        char out[2048];
        for (size_t i = 0; i < sizeof short_ops / sizeof short_ops[0]; i++) {
            uint8_t meta[48];
            build_ambtc_base(meta, short_ops[i], 0x02, 0x0ABCDE);
            if (capture_mbt_output("p25_mbt_meta_short", meta, 12U, out, sizeof out) != 0) {
                return 120;
            }
            rc |= expect_eq_int("mbt metadata short no indiv grant", g_indiv_grant_count, 0);
            rc |= expect_eq_int("mbt metadata short no group grant", g_group_grant_count, 0);
            rc |= expect_contains_text("mbt metadata short log", out, "short payload");
        }
    }

    // AMBTC Group Affiliation Response (0x28): accepted response tracks TA -> GA only.
    {
        static dsd_opts opts;
        static dsd_state state;
        uint8_t aff[48];
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        DSD_MEMSET(aff, 0, sizeof aff);

        state.p25_cc_freq = 851000000;
        state.trunk_cc_freq = 851000000;
        state.p2_wacn = 0x11111;
        state.p2_sysid = 0x222;

        aff[0] = 0x37; // outbound ALT MBT only
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
        static dsd_opts opts;
        static dsd_state state;
        uint8_t aff[48];
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        DSD_MEMSET(aff, 0, sizeof aff);

        aff[0] = 0x37;
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

    // AMBTC Unit Registration Response (0x2C): accepted response tracks the registered local RID.
    {
        static dsd_opts opts;
        static dsd_state state;
        uint8_t reg[48];
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        DSD_MEMSET(reg, 0, sizeof reg);

        state.p25_cc_freq = 851000000;
        state.trunk_cc_freq = 851000000;
        state.p2_wacn = 0x11111;
        state.p2_sysid = 0x222;

        reg[0] = 0x37; // outbound ALT MBT only
        reg[2] = 0x00; // MFID
        reg[3] = 0x01;
        reg[4] = 0x23;
        reg[5] = 0x45; // local source/WUID
        reg[6] = 0x01;
        reg[7] = 0x2C; // Unit Registration Response
        reg[8] = 0xAB;
        reg[9] = 0xCD;
        reg[12] = 0xE1; // WACN low nibble + SYSID high nibble
        reg[13] = 0x23;
        reg[14] = 0x56;
        reg[15] = 0x78;
        reg[16] = 0x9A; // fully qualified source ID
        reg[17] = 0x00; // reserved=0, RV=0 accepted

        dsd_test_capture_stderr cap;
        if (dsd_test_capture_stderr_begin(&cap, "p25_mbt_unit_reg_rsp") != 0) {
            return 102;
        }
        p25_decode_pdu_trunking(&opts, &state, reg);
        dsd_test_capture_stderr_end(&cap);

        char out[2048];
        if (read_capture_file(cap.path, out, sizeof out) != 0) {
            return 103;
        }

        rc |= expect_eq_int("mbt 0x2C aff count", state.p25_aff_count, 1);
        rc |= expect_eq_long("mbt 0x2C local source", state.p25_aff_rid[0], 0x012345);
        rc |= expect_eq_long("mbt 0x2C preserves p25 cc", state.p25_cc_freq, 851000000);
        rc |= expect_eq_long("mbt 0x2C preserves trunk cc", state.trunk_cc_freq, 851000000);
        rc |= expect_eq_long("mbt 0x2C preserves wacn", (long)state.p2_wacn, 0x11111);
        rc |= expect_eq_int("mbt 0x2C preserves sysid", state.p2_sysid, 0x222);
        rc |= expect_contains_text("mbt 0x2C WACN/SYSID", out, "WACN [ABCDE] SYSID [123]");
        rc |= expect_contains_text("mbt 0x2C SRC_ID", out, "SRC_ID [56789A]");
        rc |= expect_contains_text("mbt 0x2C SRC", out, "SRC [74565]");
        rc |= expect_contains_text("mbt 0x2C response", out, "REG_ACCEPT");
    }

    // AMBTC Unit Registration Response (0x2C): rejected response does not track affiliation.
    {
        static dsd_opts opts;
        static dsd_state state;
        uint8_t reg[48];
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        DSD_MEMSET(reg, 0, sizeof reg);

        reg[0] = 0x37;
        reg[3] = 0x01;
        reg[4] = 0x23;
        reg[5] = 0x45;
        reg[6] = 0x01;
        reg[7] = 0x2C;
        reg[17] = 0x02; // RV=2 denied

        p25_decode_pdu_trunking(&opts, &state, reg);
        rc |= expect_eq_int("mbt 0x2C rejected aff count", state.p25_aff_count, 0);
        rc |= expect_eq_int("mbt 0x2C rejected ga count", state.p25_ga_count, 0);
    }

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
