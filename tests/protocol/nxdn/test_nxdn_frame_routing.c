// SPDX-License-Identifier: GPL-3.0-or-later

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/protocol/nxdn/nxdn_voice.h>
#include <stdint.h>
#include <stdio.h>

static dsd_opts g_opts;
static dsd_state g_state;
static int g_lfsr_calls;
static int g_sacch_calls;
static int g_scch_calls;
static int g_cac_calls;
static int g_facch_calls;
static int g_facch_part_mask;
static int g_facch2_calls;
static int g_udch_calls;
static int g_facch3_calls;
static int g_udch2_calls;
static int g_sacch2_calls;
static int g_pich_tch_calls;
static int g_voice_calls;
static int g_last_voice;
static uint8_t g_last_sacch_bit;
static uint8_t g_last_sacch_reliab;
static uint8_t g_last_facch_bit;
static uint8_t g_last_facch_reliab;
static uint8_t g_last_cac_bit;
static uint8_t g_last_cac_reliab;

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u64(const char* label, unsigned long long got, unsigned long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %llu want %llu\n", label, got, want);
        return 1;
    }
    return 0;
}

static void
reset_state(void) {
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    g_lfsr_calls = 0;
    g_sacch_calls = 0;
    g_scch_calls = 0;
    g_cac_calls = 0;
    g_facch_calls = 0;
    g_facch_part_mask = 0;
    g_facch2_calls = 0;
    g_udch_calls = 0;
    g_facch3_calls = 0;
    g_udch2_calls = 0;
    g_sacch2_calls = 0;
    g_pich_tch_calls = 0;
    g_voice_calls = 0;
    g_last_voice = 0;
    g_last_sacch_bit = 0;
    g_last_sacch_reliab = 0;
    g_last_facch_bit = 0;
    g_last_facch_reliab = 0;
    g_last_cac_bit = 0;
    g_last_cac_reliab = 0;
}

static void
fill_soft_bits(uint8_t bits[364], uint8_t reliab[364]) {
    for (size_t i = 0U; i < 364U; i++) {
        bits[i] = (uint8_t)(((i * 5U) + 1U) & 1U);
        reliab[i] = (uint8_t)(250U - (i % 200U));
    }
}

void
LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state) {
    (void)buffer_in;
    (void)buffer_out;
    g_lfsr_calls++;
    state->payload_miN++;
}

void
nxdn_descramble_with_seed(uint8_t dibits[], int len, uint16_t seed) {
    (void)dibits;
    (void)len;
    (void)seed;
}

uint64_t
ConvertBitIntoBytes(const uint8_t* buffer_in, uint32_t bit_length) {
    uint64_t out = 0;
    for (uint32_t i = 0U; i < bit_length; i++) {
        out = (out << 1U) | (uint64_t)(buffer_in[i] & 1U);
    }
    return out;
}

int
getDibitWithReliability(dsd_opts* opts, dsd_state* state, uint8_t* out_reliability) {
    (void)opts;
    (void)state;
    if (out_reliability != NULL) {
        *out_reliability = 255U;
    }
    return 0;
}

uint64_t
dsd_time_monotonic_ns(void) {
    return 42000000000ULL;
}

void
printFrameSync(const dsd_opts* opts, const dsd_state* state, const char* frametype, int offset,
               const char* modulation) {
    (void)opts;
    (void)state;
    (void)frametype;
    (void)offset;
    (void)modulation;
}

void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    opts->mbe_out_f = stdout;
}

void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    opts->mbe_out_f = NULL;
}

void
nxdn_deperm_sacch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[60], const uint8_t reliab[60]) {
    (void)opts;
    (void)state;
    g_sacch_calls++;
    g_last_sacch_bit = bits[0];
    g_last_sacch_reliab = reliab[0];
}

void
nxdn_deperm_scch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[60], const uint8_t reliab[60], uint8_t direction) {
    (void)opts;
    (void)state;
    (void)bits;
    (void)reliab;
    g_scch_calls++;
    g_facch_part_mask |= (int)direction << 8;
}

void
nxdn_deperm_cac_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[300], const uint8_t reliab[300]) {
    (void)opts;
    (void)state;
    g_cac_calls++;
    g_last_cac_bit = bits[0];
    g_last_cac_reliab = reliab[0];
}

void
nxdn_deperm_facch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[144], const uint8_t reliab[144], uint8_t part) {
    (void)opts;
    (void)state;
    g_facch_calls++;
    g_facch_part_mask |= 1 << part;
    g_last_facch_bit = bits[0];
    g_last_facch_reliab = reliab[0];
}

void
nxdn_deperm_facch2_udch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[348], const uint8_t reliab[348],
                             uint8_t is_facch2) {
    (void)opts;
    (void)state;
    (void)bits;
    (void)reliab;
    if (is_facch2 != 0U) {
        g_facch2_calls++;
    } else {
        g_udch_calls++;
    }
}

void
nxdn_deperm_facch3_udch2_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[288], const uint8_t reliab[288],
                              uint8_t is_facch3) {
    (void)opts;
    (void)state;
    (void)bits;
    (void)reliab;
    if (is_facch3 != 0U) {
        g_facch3_calls++;
    } else {
        g_udch2_calls++;
    }
}

void
nxdn_deperm_sacch2_soft(const dsd_opts* opts, dsd_state* state, uint8_t bits[60], const uint8_t reliab[60]) {
    (void)opts;
    (void)state;
    (void)bits;
    (void)reliab;
    g_sacch2_calls++;
}

void
nxdn_deperm_pich_tch_soft(const dsd_opts* opts, dsd_state* state, uint8_t bits[144], const uint8_t reliab[144],
                          uint8_t lich) {
    (void)opts;
    (void)state;
    (void)bits;
    (void)reliab;
    (void)lich;
    g_pich_tch_calls++;
}

void
nxdn_voice(dsd_opts* opts, dsd_state* state, int voice, uint8_t dbuf[182], const uint8_t* dbuf_reliab) {
    (void)opts;
    (void)state;
    (void)dbuf;
    (void)dbuf_reliab;
    g_voice_calls++;
    g_last_voice = voice;
}

static int
route(uint8_t lich, const uint8_t bits[364], const uint8_t reliab[364]) {
    return dsd_neo_nxdn_test_route_decoded_lich(&g_opts, &g_state, lich, bits, reliab);
}

static int
test_control_channel_routes_and_reliability(void) {
    int rc = 0;
    uint8_t bits[364];
    uint8_t reliab[364];
    fill_soft_bits(bits, reliab);

    reset_state();
    rc |= expect_int("cac accepted", route(0x01U, bits, reliab), 1);
    rc |= expect_int("cac route", g_cac_calls, 1);
    rc |= expect_int("cac first bit", g_last_cac_bit, bits[16]);
    rc |= expect_int("cac first reliability", g_last_cac_reliab, reliab[16]);

    reset_state();
    rc |= expect_int("sacch/facch accepted", route(0x32U, bits, reliab), 1);
    rc |= expect_int("sacch route", g_sacch_calls, 1);
    rc |= expect_int("facch route", g_facch_calls, 1);
    rc |= expect_int("voice route", g_voice_calls, 1);
    rc |= expect_int("voice mode", g_last_voice, 2);
    rc |= expect_int("sacch first bit", g_last_sacch_bit, bits[16]);
    rc |= expect_int("sacch first reliability", g_last_sacch_reliab, reliab[16]);
    rc |= expect_int("facch first bit", g_last_facch_bit, bits[76]);
    rc |= expect_int("facch first reliability", g_last_facch_reliab, reliab[76]);

    reset_state();
    rc |= expect_int("facch-both accepted", route(0x20U, bits, reliab), 1);
    rc |= expect_int("facch both routes", g_facch_calls, 2);
    rc |= expect_int("non-superframe sacch mode", g_state.nxdn_sacch_non_superframe, 1);

    reset_state();
    rc |= expect_int("scch accepted", route(0x76U, bits, reliab), 1);
    rc |= expect_int("scch route", g_scch_calls, 1);

    reset_state();
    rc |= expect_int("facch2 accepted", route(0x28U, bits, reliab), 1);
    rc |= expect_int("facch2 route", g_facch2_calls, 1);
    rc |= expect_int("udch accepted", route(0x2EU, bits, reliab), 1);
    rc |= expect_int("udch route", g_udch_calls, 1);

    reset_state();
    rc |= expect_int("facch3 accepted", route(0x68U, bits, reliab), 1);
    rc |= expect_int("facch3 route", g_facch3_calls, 1);
    reset_state();
    rc |= expect_int("udch2 accepted", route(0x6EU, bits, reliab), 1);
    rc |= expect_int("udch2 route", g_udch2_calls, 1);

    reset_state();
    rc |= expect_int("sacch2-pich accepted", route(0x08U, bits, reliab), 1);
    rc |= expect_int("sacch2 route", g_sacch2_calls, 1);
    rc |= expect_int("pich single route", g_pich_tch_calls, 1);
    reset_state();
    rc |= expect_int("sacch2-pich both accepted", route(0x48U, bits, reliab), 1);
    rc |= expect_int("pich both routes", g_pich_tch_calls, 2);

    return rc;
}

static int
test_bad_frame_and_filter_gates(void) {
    int rc = 0;
    uint8_t bits[364];
    uint8_t reliab[364];
    fill_soft_bits(bits, reliab);

    reset_state();
    g_state.carrier = 1;
    g_state.synctype = 99;
    g_state.lastsynctype = 99;
    rc |= expect_int("unsupported lich rejected", route(0x7FU, bits, reliab), 0);
    rc |= expect_int("unsupported lich marks sync none", g_state.lastsynctype, DSD_SYNC_NONE);
    rc |= expect_int("unsupported lich clears carrier after sync reject", g_state.carrier, 0);
    rc |= expect_int("unsupported lich sacch reset", g_state.nxdn_sacch_frame_segment[0][0], 1);
    rc |= expect_int("unsupported lich sacch crc reset", g_state.nxdn_sacch_frame_segcrc[0], 1);

    reset_state();
    g_opts.p25_trunk = 1;
    rc |= expect_int("inbound trunk lich rejected", route(0x38U, bits, reliab), 0);
    rc |= expect_int("inbound trunk marks sync none", g_state.lastsynctype, DSD_SYNC_NONE);

    return rc;
}

static int
test_lfsr_and_scanner_state(void) {
    int rc = 0;
    uint8_t bits[364];
    uint8_t reliab[364];
    fill_soft_bits(bits, reliab);

    reset_state();
    g_state.nxdn_cipher_type = 0x1;
    g_state.R = 0x1234U;
    rc |= expect_int("data frame accepted", route(0x01U, bits, reliab), 1);
    rc |= expect_u64("data frame seeds mi", g_state.payload_miN, 0x1238ULL);
    rc |= expect_int("data frame lfsr calls", g_lfsr_calls, 4);

    reset_state();
    g_state.nxdn_cipher_type = 0x2;
    rc |= expect_int("data aes accepted", route(0x01U, bits, reliab), 1);
    rc |= expect_u64("data aes bit counter", (unsigned long long)g_state.bit_counterL, 196ULL);

    reset_state();
    g_state.M = 1;
    g_state.R = 0x2000U;
    rc |= expect_int("voice facch accepted", route(0x32U, bits, reliab), 1);
    rc |= expect_int("voice sets cipher type", g_state.nxdn_cipher_type, 1);
    rc |= expect_int("pre voice lfsr calls", g_lfsr_calls, 2);
    rc |= expect_u64("pre voice seeds mi", g_state.payload_miN, 0x2002ULL);

    reset_state();
    g_state.nxdn_cipher_type = 0x2;
    rc |= expect_int("post voice facch2 accepted", route(0x34U, bits, reliab), 1);
    rc |= expect_u64("post voice facch2 bit counter", (unsigned long long)g_state.bit_counterL, 98ULL);

    reset_state();
    g_opts.scanner_mode = 1;
    rc |= expect_int("scanner accepted", route(0x01U, bits, reliab), 1);
    rc |= expect_int("scanner extends cc sync", g_state.last_cc_sync_time > 0, 1);
    rc |= expect_int("carrier active", g_state.carrier, 1);

    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_control_channel_routes_and_reliability();
    rc |= test_bad_frame_and_filter_gates();
    rc |= test_lfsr_and_scanner_state();

    if (rc == 0) {
        DSD_FPRINTF(stdout, "NXDN_FRAME_ROUTING: OK\n");
    }
    return rc;
}
