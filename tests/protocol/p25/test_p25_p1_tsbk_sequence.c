// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Regression: a P25 Phase 1 TSDU can carry multiple independent TSBKs.
 * processTSBK() must dispatch each CRC-valid TSBK block until the last-block
 * bit, not majority-vote the blocks as if they were repeated copies.
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_callsign.h>
#include <dsd-neo/protocol/p25/p25_crc.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25_mfid90_utils.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static const uint8_t k_p25_interleave[98] = {
    0,  1,  8,  9,  16, 17, 24, 25, 32, 33, 40, 41, 48, 49, 56, 57, 64, 65, 72, 73, 80, 81, 88, 89, 96,
    97, 2,  3,  10, 11, 18, 19, 26, 27, 34, 35, 42, 43, 50, 51, 58, 59, 66, 67, 74, 75, 82, 83, 90, 91,
    4,  5,  12, 13, 20, 21, 28, 29, 36, 37, 44, 45, 52, 53, 60, 61, 68, 69, 76, 77, 84, 85, 92, 93, 6,
    7,  14, 15, 22, 23, 30, 31, 38, 39, 46, 47, 54, 55, 62, 63, 70, 71, 78, 79, 86, 87, 94, 95};

static const uint8_t k_p25_dtm[16] = {2, 12, 1, 15, 14, 0, 13, 3, 9, 7, 10, 4, 5, 11, 6, 8};

static uint8_t g_stream[3 * 101];
static int g_stream_len = 0;
static int g_stream_pos = 0;
static int g_mac_count = 0;
static int g_mac_group[3] = {0};
static int g_mac_source[3] = {0};
static int g_status_count = 0;
static long g_channel_freq = 0;

static void
bytes_to_tdibits(const uint8_t bytes[12], uint8_t tdibits[49]) {
    for (int i = 0; i < 48; i++) {
        tdibits[i] = (uint8_t)((bytes[i / 4] >> (6 - (2 * (i % 4)))) & 3U);
    }
    tdibits[48] = 0;
}

static void
encode_12_to_dibits(const uint8_t bytes[12], uint8_t dibits[98]) {
    uint8_t tdibits[49];
    uint8_t deint[98];
    bytes_to_tdibits(bytes, tdibits);

    uint8_t prev = 0;
    for (int i = 0; i < 49; i++) {
        uint8_t next = tdibits[i] & 3U;
        uint8_t nibble = k_p25_dtm[(prev << 2) | next] & 0xFU;
        deint[(i * 2) + 0] = (uint8_t)((nibble >> 2) & 3U);
        deint[(i * 2) + 1] = (uint8_t)(nibble & 3U);
        prev = next;
    }

    for (int i = 0; i < 98; i++) {
        dibits[i] = deint[k_p25_interleave[i]];
    }
}

static void
bytes_to_bits80(const uint8_t bytes[12], uint8_t bits[80]) {
    for (int i = 0; i < 80; i++) {
        bits[i] = (uint8_t)((bytes[i / 8] >> (7 - (i % 8))) & 1U);
    }
}

static void
append_crc16(uint8_t bytes[12]) {
    uint8_t bits[80];
    bytes_to_bits80(bytes, bits);
    uint16_t crc = ComputeCrcCCITT16b(bits, 80);
    bytes[10] = (uint8_t)(crc >> 8);
    bytes[11] = (uint8_t)(crc & 0xFF);
}

static void
build_group_grant_tsbk(uint8_t out[12], uint8_t lb, uint16_t group, uint32_t source) {
    DSD_MEMSET(out, 0, 12);
    out[0] = lb ? 0x80U : 0x00U;
    out[1] = 0x00;
    out[2] = 0x00;
    out[3] = 0x10;
    out[4] = 0x0A;
    out[5] = (uint8_t)(group >> 8);
    out[6] = (uint8_t)(group & 0xFF);
    out[7] = (uint8_t)((source >> 16) & 0xFF);
    out[8] = (uint8_t)((source >> 8) & 0xFF);
    out[9] = (uint8_t)(source & 0xFF);
    append_crc16(out);
}

static void
build_network_status_tsbk(uint8_t out[12], uint32_t wacn, uint16_t sysid, uint16_t channel) {
    DSD_MEMSET(out, 0, 12);
    out[0] = 0x80U | 0x3BU;
    out[1] = 0x00;
    out[3] = (uint8_t)((wacn >> 12) & 0xFFU);
    out[4] = (uint8_t)((wacn >> 4) & 0xFFU);
    out[5] = (uint8_t)(((wacn & 0x0FU) << 4) | ((sysid >> 8) & 0x0FU));
    out[6] = (uint8_t)(sysid & 0xFFU);
    out[7] = (uint8_t)(channel >> 8);
    out[8] = (uint8_t)(channel & 0xFFU);
    append_crc16(out);
}

static void
append_tsbk_stream_block(const uint8_t dibits[98], int* skipdibit) {
    int valid_idx = 0;
    for (int i = 0; i < 101; i++) {
        if ((*skipdibit / 36) == 0) {
            g_stream[g_stream_len++] = dibits[valid_idx++];
        } else {
            g_stream[g_stream_len++] = 0;
            *skipdibit = 0;
        }
        (*skipdibit)++;
    }
}

static void
reset_decode_counters(void) {
    g_mac_count = 0;
    g_mac_group[0] = 0;
    g_mac_group[1] = 0;
    g_mac_group[2] = 0;
    g_mac_source[0] = 0;
    g_mac_source[1] = 0;
    g_mac_source[2] = 0;
    g_status_count = 0;
}

static void
build_two_block_stream(void) {
    uint8_t block[12];
    uint8_t dibits[98];
    int skipdibit = 36 - 14;

    DSD_MEMSET(g_stream, 0, sizeof(g_stream));
    g_stream_len = 0;
    g_stream_pos = 0;

    build_group_grant_tsbk(block, 0, 0x1111, 0x000101);
    encode_12_to_dibits(block, dibits);
    append_tsbk_stream_block(dibits, &skipdibit);

    build_group_grant_tsbk(block, 1, 0x2222, 0x000202);
    encode_12_to_dibits(block, dibits);
    append_tsbk_stream_block(dibits, &skipdibit);
}

static void
build_network_status_stream(void) {
    uint8_t block[12];
    uint8_t dibits[98];
    int skipdibit = 36 - 14;

    DSD_MEMSET(g_stream, 0, sizeof(g_stream));
    g_stream_len = 0;
    g_stream_pos = 0;

    build_network_status_tsbk(block, 0xABCDE, 0x123, 0x8123);
    encode_12_to_dibits(block, dibits);
    append_tsbk_stream_block(dibits, &skipdibit);
}

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    uint8_t dibit = 0;
    if (g_stream_pos < g_stream_len) {
        dibit = g_stream[g_stream_pos++];
    }
    if (out_soft) {
        out_soft->reliability = 255;
        out_soft->llr[0] = (dibit & 2U) ? 220 : -220;
        out_soft->llr[1] = (dibit & 1U) ? 220 : -220;
    }
    return dibit;
}

void
p25_status_accum_ensure_started(dsd_state* state) {
    (void)state;
}

void
p25_status_accum_add(dsd_state* state, int dibit_value) {
    (void)state;
    (void)dibit_value;
    g_status_count++;
}

void
p25_status_accum_classify(dsd_state* state, const dsd_opts* opts) {
    (void)state;
    (void)opts;
}

void
process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, unsigned long long int mac[24]) {
    (void)opts;
    (void)state;
    (void)type;
    if (g_mac_count < 3) {
        g_mac_group[g_mac_count] = (int)((mac[5] << 8) | mac[6]);
        g_mac_source[g_mac_count] = (int)((mac[7] << 16) | (mac[8] << 8) | mac[9]);
    }
    g_mac_count++;
}

long int
process_channel_to_freq(const dsd_opts* opts, dsd_state* state, int channel) {
    (void)opts;
    (void)state;
    (void)channel;
    return g_channel_freq;
}

void
p25_format_chan_suffix(const dsd_state* state, uint16_t chan, int slot_hint, char* out, size_t outsz) {
    (void)state;
    (void)chan;
    (void)slot_hint;
    if (out && outsz > 0) {
        out[0] = '\0';
    }
}

void
p25_sm_seed_cc_from_current_tuner_if_unknown(const dsd_opts* opts, dsd_state* state) {
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
p25_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    (void)opts;
    (void)state;
    (void)freqs;
    (void)count;
}

void
p25_confirm_idens_for_current_site(dsd_state* state) {
    (void)state;
}

void
p25_wacn_sysid_to_callsign(uint32_t wacn, uint16_t sysid, char out[7]) {
    (void)wacn;
    (void)sysid;
    if (out) {
        out[0] = '\0';
    }
}

int
p25_mfid90_base_station_id_decode(const uint8_t tsbk_byte[12], char* cwid, size_t cwid_size, uint16_t* channel) {
    (void)tsbk_byte;
    if (cwid && cwid_size > 0) {
        cwid[0] = '\0';
    }
    if (channel) {
        *channel = 0;
    }
    return 0;
}

void
p25_patch_add_wgid(dsd_state* state, int sg, int wgid) {
    (void)state;
    (void)sg;
    (void)wgid;
}

void
p25_patch_add_wuid(dsd_state* state, int sg, uint32_t wuid) {
    (void)state;
    (void)sg;
    (void)wuid;
}

void
p25_patch_remove_wgid(dsd_state* state, int sg, int wgid) {
    (void)state;
    (void)sg;
    (void)wgid;
}

void
p25_patch_set_kas(dsd_state* state, int sg, int kas, int algid, int ssn) {
    (void)state;
    (void)sg;
    (void)kas;
    (void)algid;
    (void)ssn;
}

void
p25_patch_update(dsd_state* state, int sg, int is_patch, int active) {
    (void)state;
    (void)sg;
    (void)is_patch;
    (void)active;
}

void
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
dsd_rtl_stream_metrics_hook_p25p1_ber_update(int ok_delta, int err_delta) {
    (void)ok_delta;
    (void)err_delta;
}

uint64_t
dsd_time_monotonic_ns(void) {
    return 1000000000ULL;
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    build_two_block_stream();
    reset_decode_counters();

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    processTSBK(&opts, &state);

    int rc = 0;
    rc |= expect_eq_int("mac dispatch count", g_mac_count, 2);
    rc |= expect_eq_int("first group", g_mac_group[0], 0x1111);
    rc |= expect_eq_int("second group", g_mac_group[1], 0x2222);
    rc |= expect_eq_int("first source", g_mac_source[0], 0x000101);
    rc |= expect_eq_int("second source", g_mac_source[1], 0x000202);
    rc |= expect_eq_int("stream consumed through last block", g_stream_pos, 202);
    rc |= expect_eq_int("status symbols collected", g_status_count, 6);
    rc |= expect_eq_int("fec ok count", (int)state.p25_p1_fec_ok, 2);
    rc |= expect_eq_int("fec err count", (int)state.p25_p1_fec_err, 0);

    build_network_status_stream();
    reset_decode_counters();
    g_channel_freq = 0;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.p25_cc_is_tdma = 2;

    processTSBK(&opts, &state);

    rc |= expect_eq_int("net-sts missing-iden stores wacn", (int)state.p2_wacn, 0xABCDE);
    rc |= expect_eq_int("net-sts missing-iden stores sysid", (int)state.p2_sysid, 0x123);
    rc |= expect_eq_int("net-sts missing-iden marks p1 cc fdma", state.p25_cc_is_tdma, 0);
    rc |= expect_eq_int("net-sts missing-iden leaves p25 cc empty", (int)state.p25_cc_freq, 0);
    rc |= expect_eq_int("net-sts missing-iden leaves trunk cc empty", (int)state.trunk_cc_freq, 0);

    build_network_status_stream();
    reset_decode_counters();
    g_channel_freq = 863812500;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.p25_is_tuned = 1;
    state.p25_cc_freq = 851000000;
    state.trunk_cc_freq = 851000000;
    state.p25_cc_is_tdma = 1;

    processTSBK(&opts, &state);

    rc |= expect_eq_int("net-sts rejected voice preserves p25 cc", (int)state.p25_cc_freq, 851000000);
    rc |= expect_eq_int("net-sts rejected voice preserves trunk cc", (int)state.trunk_cc_freq, 851000000);
    rc |= expect_eq_int("net-sts rejected voice preserves tdma cc hint", state.p25_cc_is_tdma, 1);
    rc |= expect_eq_int("net-sts rejected voice skips wacn", (int)state.p2_wacn, 0);
    rc |= expect_eq_int("net-sts rejected voice skips sysid", (int)state.p2_sysid, 0);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
