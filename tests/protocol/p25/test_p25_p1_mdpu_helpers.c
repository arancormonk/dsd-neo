// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-suspicious-include)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 P1 MDPU helper tests: verify deterministic header, CRC candidate,
 * repetition, and rate 3/4 bit-assembly behavior without live dibit input.
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/p25/p25_12.h>
#include <dsd-neo/protocol/p25/p25_crc.h>
#include <dsd-neo/protocol/p25/p25_pdu.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25p1_mbf34.h>
#include <dsd-neo/protocol/p25/p25p1_pdu_trunking.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static p25_12_candidate_t g_soft_candidates[P25_12_MAX_CANDIDATES];
static int g_soft_candidate_count;
static uint8_t g_r12_fallback_bytes[12];
static int g_r12_fallback_calls;
static p25_mbf34_candidate_t g_mbf34_candidates[P25_MBF34_MAX_CANDIDATES];
static int g_mbf34_candidate_count;
static uint8_t g_mbf34_fallback_bytes[18];
static int g_mbf34_fallback_calls;
static int g_get_dibit_soft_calls;
static int g_status_add_calls;
static int g_last_status_dibit;
static int g_pdu_header_calls;
static int g_pdu_data_calls;
static int g_pdu_trunking_calls;
static int g_status_ensure_started_calls;
static int g_status_classify_calls;
static int g_last_pdu_data_len;

static uint16_t
test_crc16_bits(const int* payload, int len) {
    uint16_t crc = 0;
    const uint16_t poly = 0x1021;
    for (int bit = 0; bit < len; bit++) {
        if (((crc >> 15) & 1) ^ (payload[bit] & 1)) {
            crc = (uint16_t)((crc << 1) ^ poly);
        } else {
            crc = (uint16_t)(crc << 1);
        }
    }
    return (uint16_t)(crc ^ 0xFFFF);
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
crc16_lb_bridge(const int* payload, int len) {
    uint16_t extracted = 0;
    for (int bit = 0; bit < 16; bit++) {
        extracted = (uint16_t)((extracted << 1) | (payload[len + bit] & 1));
    }
    return (test_crc16_bits(payload, len) == extracted) ? 0 : 1;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
ConvertBitIntoBytes(const uint8_t* buffer_in, uint32_t bit_length) {
    uint64_t value = 0;
    for (uint32_t bit = 0; bit < bit_length; bit++) {
        value = (value << 1) | (uint64_t)(buffer_in[bit] & 1);
    }
    return value;
}

uint16_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
ComputeCrc9Bit(const uint8_t* data, uint32_t bit_count) {
    uint16_t crc = 0;
    for (uint32_t bit = 0; bit < bit_count; bit++) {
        crc = (uint16_t)(((crc << 1) ^ (data[bit] & 1U) ^ ((crc >> 8) & 1U)) & 0x1FFU);
    }
    return crc;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_12_soft_llr_list(const uint8_t* input, const int16_t* bit_llr196, p25_12_candidate_t* candidates,
                     int max_candidates) {
    (void)input;
    (void)bit_llr196;
    int count = (g_soft_candidate_count < max_candidates) ? g_soft_candidate_count : max_candidates;
    for (int idx = 0; idx < count; idx++) {
        candidates[idx] = g_soft_candidates[idx];
    }
    return count;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_12_soft_llr(const uint8_t* input, const int16_t* bit_llr196, uint8_t treturn[12]) {
    (void)input;
    (void)bit_llr196;
    g_r12_fallback_calls++;
    DSD_MEMCPY(treturn, g_r12_fallback_bytes, sizeof(g_r12_fallback_bytes));
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_mbf34_decode_soft_list(const uint8_t dibits[98], const int16_t bit_llr[196], p25_mbf34_candidate_t* candidates,
                           int max_candidates) {
    (void)dibits;
    (void)bit_llr;
    int count = (g_mbf34_candidate_count < max_candidates) ? g_mbf34_candidate_count : max_candidates;
    for (int idx = 0; idx < count; idx++) {
        candidates[idx] = g_mbf34_candidates[idx];
    }
    return count;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_mbf34_decode_soft(const uint8_t dibits[98], const int16_t bit_llr[196], uint8_t out[18]) {
    (void)dibits;
    (void)bit_llr;
    g_mbf34_fallback_calls++;
    DSD_MEMCPY(out, g_mbf34_fallback_bytes, sizeof(g_mbf34_fallback_bytes));
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    int call = g_get_dibit_soft_calls++;
    if (out_soft != NULL) {
        out_soft->llr[0] = (int16_t)(1000 + call);
        out_soft->llr[1] = (int16_t)(-(2000 + call));
    }
    return call & 0x3;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_status_accum_add(dsd_state* state, int dibit_value) {
    (void)state;
    g_status_add_calls++;
    g_last_status_dibit = dibit_value;
}

void
p25_decode_pdu_header(dsd_opts* opts, dsd_state* state, const uint8_t* input) {
    (void)opts;
    (void)state;
    (void)input;
    g_pdu_header_calls++;
}

void
p25_decode_pdu_data(dsd_opts* opts, dsd_state* state, uint8_t* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    g_pdu_data_calls++;
    g_last_pdu_data_len = len;
}

void
p25_decode_pdu_trunking(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte) {
    (void)opts;
    (void)state;
    (void)mpdu_byte;
    g_pdu_trunking_calls++;
}

void
p25_status_accum_ensure_started(dsd_state* state) {
    (void)state;
    g_status_ensure_started_calls++;
}

void
p25_status_accum_classify(dsd_state* state, const dsd_opts* opts) {
    (void)state;
    (void)opts;
    g_status_classify_calls++;
}

#include "../../../src/protocol/p25/phase1/p25p1_mdpu.c"

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u8(const char* tag, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%02X want 0x%02X\n", tag, (unsigned int)got, (unsigned int)want);
        return 1;
    }
    return 0;
}

static void
reset_dispatch_counters(void) {
    g_pdu_header_calls = 0;
    g_pdu_data_calls = 0;
    g_pdu_trunking_calls = 0;
    g_status_ensure_started_calls = 0;
    g_status_classify_calls = 0;
    g_last_pdu_data_len = 0;
}

static void
bytes_to_int_bits(const uint8_t* bytes, int byte_count, int* bits) {
    for (int byte_idx = 0; byte_idx < byte_count; byte_idx++) {
        for (int bit = 0; bit < 8; bit++) {
            bits[(byte_idx * 8) + bit] = (bytes[byte_idx] >> (7 - bit)) & 1;
        }
    }
}

static void
append_crc16(uint8_t bytes[P25_MPDU_R12_BYTES]) {
    int bits[P25_MPDU_HEADER_BITS] = {0};
    bytes_to_int_bits(bytes, P25_MPDU_R12_BYTES, bits);
    uint16_t crc = test_crc16_bits(bits, 80);
    bytes[10] = (uint8_t)(crc >> 8);
    bytes[11] = (uint8_t)(crc & 0xFF);
}

static void
bytes_to_u8_bits(const uint8_t* bytes, int byte_count, uint8_t* bits) {
    for (int byte_idx = 0; byte_idx < byte_count; byte_idx++) {
        for (int bit = 0; bit < 8; bit++) {
            bits[(byte_idx * 8) + bit] = (uint8_t)((bytes[byte_idx] >> (7 - bit)) & 1);
        }
    }
}

static void
set_mbf34_crc9_extension(uint8_t bytes[P25_MPDU_R34_BYTES]) {
    bytes[0] &= 0xFEU;
    bytes[1] = 0;
    uint16_t crc9 = p25_mpdu_candidate_crc9(bytes);
    bytes[0] = (uint8_t)((bytes[0] & 0xFEU) | ((crc9 >> 8) & 1U));
    bytes[1] = (uint8_t)(crc9 & 0xFFU);
}

static int
test_context_init_and_saturating_add(void) {
    P25MpduContext ctx;
    p25_mpdu_context_init(&ctx);

    int rc = 0;
    rc |= expect_int("default end", ctx.end, 3);
    rc |= expect_int("header crc init", ctx.hdr_rep_crc[0], -2);
    rc |= expect_int("payload crc init", ctx.err[1], -2);
    rc |= expect_int("positive saturation", saturating_llr_add(INT16_MAX - 2, 10), INT16_MAX);
    rc |= expect_int("negative saturation", saturating_llr_add(INT16_MIN + 2, -10), INT16_MIN);
    rc |= expect_int("ordinary add", saturating_llr_add(100, -25), 75);
    return rc;
}

static int
test_prepare_state_resets_mpdu_frame_context(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.slot_preference = 1;
    state.p25_p1_duid_mpdu = 41;
    state.voice_counter[0] = 7;
    state.voice_counter[1] = 8;
    state.s_l4[0][0] = 11;
    state.s_r4[0][0] = 12;
    state.currentslot = 1;
    state.last_active_time = time(NULL) - 10;
    DSD_SNPRINTF(state.call_string[0], sizeof(state.call_string[0]), "%s", "left");
    DSD_SNPRINTF(state.call_string[1], sizeof(state.call_string[1]), "%s", "right");
    DSD_SNPRINTF(state.active_channel[0], sizeof(state.active_channel[0]), "%s", "active");
    reset_dispatch_counters();

    p25_mpdu_prepare_state(&opts, &state);

    int rc = 0;
    rc |= expect_int("mpdu counter increment", (int)state.p25_p1_duid_mpdu, 42);
    rc |= expect_int("left voice counter reset", state.voice_counter[0], 0);
    rc |= expect_int("right voice counter reset", state.voice_counter[1], 0);
    rc |= expect_int("left 4v samples reset", state.s_l4[0][0], 0);
    rc |= expect_int("right 4v samples reset", state.s_r4[0][0], 0);
    rc |= expect_int("slot preference set", opts.slot_preference, 2);
    rc |= expect_int("current slot reset", state.currentslot, 0);
    rc |= expect_int("status accumulator started", g_status_ensure_started_calls, 1);
    rc |= expect_int("left call string padded", strcmp(state.call_string[0], "                     "), 0);
    rc |= expect_int("right call string padded", strcmp(state.call_string[1], "                     "), 0);
    rc |= expect_int("stale active channel cleared", state.active_channel[0][0], 0);
    return rc;
}

static int
test_header_store_and_update_fields(void) {
    P25MpduContext ctx;
    static dsd_opts opts;
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    DSD_MEMSET(&opts, 0, sizeof(opts));

    const uint8_t header[P25_MPDU_R12_BYTES] = {0x56, 0x3D, 0x12, 0x34, 0x56, 0x78, 0x8C, 0x9A, 0xBC, 0xDE, 0x00, 0x00};
    bytes_to_int_bits(header, P25_MPDU_R12_BYTES, ctx.tsbk_decoded_bits);
    p25_mpdu_store_r12_block(&ctx, 0);

    int rc = 0;
    rc |= expect_u8("stored byte 0", ctx.mpdu_byte[0], 0x56);
    rc |= expect_u8("stored byte 7", ctx.mpdu_byte[7], 0x9A);

    opts.aggressive_framesync = 1;
    ctx.hdr_rep_crc[0] = 1;
    p25_mpdu_update_header_from_first_block(&ctx, &opts);
    rc |= expect_int("aggressive bad header leaves fmt unchanged", ctx.fmt, 0);

    ctx.hdr_rep_crc[0] = 0;
    p25_mpdu_update_header_from_first_block(&ctx, &opts);
    rc |= expect_int("io", ctx.io, 0);
    rc |= expect_int("fmt", ctx.fmt, 0x16);
    rc |= expect_int("sap", ctx.sap, 0x3D);
    rc |= expect_int("blks", ctx.blks, 12);
    rc |= expect_int("rate34", ctx.r34, 1);
    rc |= expect_int("large trunking end clamp", ctx.end, 4);
    return rc;
}

static int
test_candidate_selection_uses_crc_fields(void) {
    p25_mbf34_candidate_t mbf[P25_MBF34_MAX_CANDIDATES];
    DSD_MEMSET(mbf, 0, sizeof(mbf));
    for (int byte_idx = 0; byte_idx < P25_MPDU_R34_BYTES; byte_idx++) {
        mbf[0].bytes[byte_idx] = (uint8_t)(0x10 + byte_idx);
        mbf[1].bytes[byte_idx] = (uint8_t)(0x80 ^ (byte_idx * 7));
    }
    set_mbf34_crc9_extension(mbf[1].bytes);

    p25_12_candidate_t r12[P25_12_MAX_CANDIDATES];
    DSD_MEMSET(r12, 0, sizeof(r12));
    for (int byte_idx = 0; byte_idx < P25_MPDU_R12_BYTES; byte_idx++) {
        r12[0].bytes[byte_idx] = (uint8_t)(0xA0 + byte_idx);
        r12[1].bytes[byte_idx] = (uint8_t)(0x30 + (byte_idx * 3));
    }
    append_crc16(r12[1].bytes);

    int rc = 0;
    rc |= expect_int("mbf34 crc9 candidate", p25_mpdu_select_mbf34_candidate(mbf, 2), 1);
    mbf[1].bytes[1] ^= 0x01;
    rc |= expect_int("mbf34 fallback candidate", p25_mpdu_select_mbf34_candidate(mbf, 2), 0);
    rc |= expect_int("r12 crc16 candidate", p25_mpdu_select_crc16_candidate(r12, 2), 1);
    r12[1].bytes[11] ^= 0x01;
    rc |= expect_int("r12 fallback candidate", p25_mpdu_select_crc16_candidate(r12, 2), 0);
    return rc;
}

static int
test_read_repetition_captures_soft_symbols_and_status_dibits(void) {
    static dsd_opts opts;
    static dsd_state state;
    P25MpduContext ctx;
    int expected_dibit[P25_MPDU_TSBK_DIBITS] = {0};
    int expected_call[P25_MPDU_TSBK_DIBITS] = {0};
    int expected_count = 0;
    int expected_status_count = 0;
    int expected_last_status = 0;
    int expected_skip = 36 - 14;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    p25_mpdu_context_init(&ctx);
    g_get_dibit_soft_calls = 0;
    g_status_add_calls = 0;
    g_last_status_dibit = -1;

    for (int call = 0; call < 101 && expected_count < P25_MPDU_TSBK_DIBITS; call++) {
        int dibit = call & 0x3;
        if ((expected_skip / 36) == 0) {
            expected_dibit[expected_count] = dibit;
            expected_call[expected_count] = call;
            expected_count++;
        } else {
            expected_status_count++;
            expected_last_status = dibit;
            expected_skip = 0;
        }
        expected_skip++;
    }

    int skipdibit = 36 - 14;
    p25_mpdu_read_repetition(&opts, &state, &ctx, &skipdibit);

    int rc = 0;
    rc |= expect_int("captured dibits", expected_count, P25_MPDU_TSBK_DIBITS);
    rc |= expect_int("soft reader calls", g_get_dibit_soft_calls, 101);
    rc |= expect_int("status dibits forwarded", g_status_add_calls, expected_status_count);
    rc |= expect_int("last status dibit", g_last_status_dibit, expected_last_status);
    rc |= expect_int("skip state carried", skipdibit, expected_skip);
    for (int idx = 0; idx < P25_MPDU_TSBK_DIBITS; idx++) {
        rc |= expect_int("captured dibit", ctx.tsbk_dibit[idx], expected_dibit[idx]);
        rc |= expect_int("captured llr msb", ctx.tsbk_llr[(idx * 2) + 0], 1000 + expected_call[idx]);
        rc |= expect_int("captured llr lsb", ctx.tsbk_llr[(idx * 2) + 1], -(2000 + expected_call[idx]));
    }
    return rc;
}

static int
test_decode_blocks_use_candidates_or_fallback_and_pack_crc_bits(void) {
    P25MpduContext ctx;
    int rc = 0;

    p25_mpdu_context_init(&ctx);
    DSD_MEMSET(g_soft_candidates, 0, sizeof(g_soft_candidates));
    g_soft_candidate_count = 2;
    for (int byte_idx = 0; byte_idx < P25_MPDU_R12_BYTES; byte_idx++) {
        g_soft_candidates[0].bytes[byte_idx] = (uint8_t)(0x40 + byte_idx);
        g_soft_candidates[1].bytes[byte_idx] = (uint8_t)(0x90 + byte_idx);
    }
    append_crc16(g_soft_candidates[1].bytes);
    p25_mpdu_decode_r12_block(&ctx, 0);
    rc |= expect_u8("r12 header selected valid candidate", ctx.tsbk_byte[0], g_soft_candidates[1].bytes[0]);

    p25_mpdu_decode_r12_block(&ctx, 1);
    rc |= expect_u8("r12 data block uses first candidate", ctx.tsbk_byte[0], g_soft_candidates[0].bytes[0]);

    DSD_MEMSET(g_r12_fallback_bytes, 0, sizeof(g_r12_fallback_bytes));
    for (int byte_idx = 0; byte_idx < P25_MPDU_R12_BYTES; byte_idx++) {
        g_r12_fallback_bytes[byte_idx] = (uint8_t)(0xC0 + byte_idx);
    }
    g_soft_candidate_count = 0;
    g_r12_fallback_calls = 0;
    p25_mpdu_decode_r12_block(&ctx, 0);
    rc |= expect_int("r12 fallback called", g_r12_fallback_calls, 1);
    rc |= expect_u8("r12 fallback bytes copied", ctx.tsbk_byte[5], g_r12_fallback_bytes[5]);

    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    DSD_MEMSET(g_mbf34_candidates, 0, sizeof(g_mbf34_candidates));
    g_mbf34_candidate_count = 2;
    for (int byte_idx = 0; byte_idx < P25_MPDU_R34_BYTES; byte_idx++) {
        g_mbf34_candidates[0].bytes[byte_idx] = (uint8_t)(0x10 + byte_idx);
        g_mbf34_candidates[1].bytes[byte_idx] = (uint8_t)(0x80 ^ (byte_idx * 9));
    }
    set_mbf34_crc9_extension(g_mbf34_candidates[1].bytes);
    p25_mpdu_decode_r34_block(&ctx, 1);
    rc |= expect_u8("r34 selected crc9 candidate", ctx.r34byte_b[0], g_mbf34_candidates[1].bytes[0]);
    rc |= expect_u8("r34 stored block bytes", ctx.r34bytes[17], g_mbf34_candidates[1].bytes[17]);
    rc |= expect_int("r34 crc32 bit count", ctx.crc_bit_count, 128);
    rc |= expect_int("r34 crc9 bit count", ctx.crc9_bit_count, 135);
    rc |= expect_int("r34 first crc bit", ctx.mpdu_crc_bits[0], (g_mbf34_candidates[1].bytes[2] >> 7) & 1);
    rc |= expect_int("r34 first crc9 bit", ctx.mpdu_crc9_bits[0], (g_mbf34_candidates[1].bytes[0] >> 7) & 1);

    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    DSD_MEMSET(g_mbf34_fallback_bytes, 0, sizeof(g_mbf34_fallback_bytes));
    for (int byte_idx = 0; byte_idx < P25_MPDU_R34_BYTES; byte_idx++) {
        g_mbf34_fallback_bytes[byte_idx] = (uint8_t)(0xE0 + byte_idx);
    }
    g_mbf34_candidate_count = 0;
    g_mbf34_fallback_calls = 0;
    p25_mpdu_decode_r34_block(&ctx, 1);
    rc |= expect_int("r34 fallback called", g_mbf34_fallback_calls, 1);
    rc |= expect_u8("r34 fallback bytes copied", ctx.r34byte_b[4], g_mbf34_fallback_bytes[4]);
    return rc;
}

static int
test_finalize_header_selects_best_rep_and_majority(void) {
    static dsd_state state;
    P25MpduContext ctx;
    DSD_MEMSET(&state, 0, sizeof(state));
    p25_mpdu_context_init(&ctx);

    ctx.end = 3;
    ctx.blks = 0;
    ctx.hdr_rep_crc[0] = 7;
    ctx.hdr_rep_crc[1] = 0;
    ctx.hdr_rep_crc[2] = 9;
    for (int byte_idx = 0; byte_idx < P25_MPDU_R12_BYTES; byte_idx++) {
        ctx.hdr_rep_bytes[1][byte_idx] = (uint8_t)(0xE0 + byte_idx);
    }
    p25_mpdu_finalize_header(&state, &ctx);

    int rc = 0;
    rc |= expect_int("best rep crc", ctx.err[0], 0);
    rc |= expect_u8("best rep byte", ctx.mpdu_byte[3], 0xE3);
    rc |= expect_int("fec ok count", (int)state.p25_p1_fec_ok, 1);
    rc |= expect_int("soft combined count", (int)state.p25_p1_soft_combined_ok, 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    p25_mpdu_context_init(&ctx);
    uint8_t valid_header[P25_MPDU_R12_BYTES] = {0x37, 0x3D, 0x00, 0x01, 0x10, 0x0A, 0x01, 0x33, 0x01, 0x02, 0x00, 0x00};
    append_crc16(valid_header);
    ctx.end = 3;
    ctx.blks = 0;
    ctx.hdr_rep_crc[0] = 5;
    ctx.hdr_rep_crc[1] = 6;
    ctx.hdr_rep_crc[2] = 7;
    for (int rep = 0; rep < P25_MPDU_HEADER_REPS; rep++) {
        bytes_to_u8_bits(valid_header, P25_MPDU_R12_BYTES, ctx.hdr_rep_bits[rep]);
    }
    g_soft_candidate_count = 0;
    p25_mpdu_finalize_header(&state, &ctx);

    rc |= expect_int("majority rebuilt crc", ctx.err[0], 0);
    rc |= expect_u8("majority rebuilt byte", ctx.mpdu_byte[7], 0x33);
    rc |= expect_int("majority fec ok count", (int)state.p25_p1_fec_ok, 1);

    DSD_MEMSET(&state, 0, sizeof(state));
    p25_mpdu_context_init(&ctx);
    uint8_t combined_header[P25_MPDU_R12_BYTES] = {0x17, 0x3D, 0x55, 0x44, 0x33, 0x22,
                                                   0x01, 0x77, 0x88, 0x99, 0x00, 0x00};
    append_crc16(combined_header);
    ctx.end = 3;
    ctx.blks = 0;
    ctx.hdr_rep_crc[0] = 5;
    ctx.hdr_rep_crc[1] = 6;
    ctx.hdr_rep_crc[2] = 7;
    DSD_MEMSET(g_soft_candidates, 0, sizeof(g_soft_candidates));
    g_soft_candidate_count = 2;
    for (int byte_idx = 0; byte_idx < P25_MPDU_R12_BYTES; byte_idx++) {
        g_soft_candidates[0].bytes[byte_idx] = (uint8_t)(0xA0 + byte_idx);
        g_soft_candidates[1].bytes[byte_idx] = combined_header[byte_idx];
    }
    p25_mpdu_finalize_header(&state, &ctx);
    g_soft_candidate_count = 0;

    rc |= expect_int("combined header crc", ctx.err[0], 0);
    rc |= expect_u8("combined header byte", ctx.mpdu_byte[8], combined_header[8]);
    rc |= expect_int("combined fec ok count", (int)state.p25_p1_fec_ok, 1);
    rc |= expect_int("combined soft count", (int)state.p25_p1_soft_combined_ok, 1);
    return rc;
}

static int
test_rate34_crc_bit_packing(void) {
    P25MpduContext ctx;
    p25_mpdu_context_init(&ctx);
    ctx.blks = 1;

    uint8_t payload[16] = {0xC0, 0x01, 0x22, 0x43, 0x64, 0x85, 0xA6, 0xC7,
                           0xE8, 0x09, 0x2A, 0x4B, 0x00, 0x00, 0x00, 0x00};
    uint32_t crc = crc32mbf(payload, 96);
    payload[12] = (uint8_t)(crc >> 24);
    payload[13] = (uint8_t)(crc >> 16);
    payload[14] = (uint8_t)(crc >> 8);
    payload[15] = (uint8_t)crc;
    bytes_to_u8_bits(payload, 16, ctx.mpdu_crc_bits);

    uint32_t extracted = 0;
    uint32_t computed = 1;
    p25_mpdu_compute_rate34_crc(&ctx, &extracted, &computed);

    int rc = 0;
    rc |= expect_int("rate34 crc extracted", (int)extracted, (int)crc);
    rc |= expect_int("rate34 crc computed", (int)computed, (int)crc);
    rc |= expect_int("rate34 crc ok", ctx.err[1], 0);
    return rc;
}

static int
test_rate34_zero_block_crc_and_multiblock_reconstruction(void) {
    P25MpduContext ctx;
    uint8_t dbsn[P25_MPDU_MAX_DATA_BLOCKS] = {0};
    uint16_t crc9_ext[P25_MPDU_MAX_DATA_BLOCKS] = {0};
    uint16_t crc9_cmp[P25_MPDU_MAX_DATA_BLOCKS] = {0};
    uint32_t extracted = 123;
    uint32_t computed = 456;
    int rc = 0;

    p25_mpdu_context_init(&ctx);
    ctx.blks = 0;
    p25_mpdu_compute_rate34_crc(&ctx, &extracted, &computed);
    rc |= expect_int("rate34 zero-block extracted", (int)extracted, 0);
    rc |= expect_int("rate34 zero-block computed", (int)computed, 0);
    rc |= expect_int("rate34 zero-block crc ok", ctx.err[1], 0);

    p25_mpdu_context_init(&ctx);
    ctx.blks = 2;
    for (int byte_idx = 0; byte_idx < P25_MPDU_R34_BYTES * 2; byte_idx++) {
        ctx.r34bytes[byte_idx] = (uint8_t)(0x30 + byte_idx);
    }
    int mpdu_idx = p25_mpdu_reconstruct_rate34_payload(&ctx, dbsn, crc9_ext, crc9_cmp);

    rc |= expect_int("rate34 multiblock reconstruction index", mpdu_idx, 45);
    rc |= expect_u8("rate34 block0 dbsn", dbsn[0], ctx.r34bytes[0] >> 1);
    rc |= expect_u8("rate34 block1 dbsn", dbsn[1], ctx.r34bytes[18] >> 1);
    rc |= expect_u8("rate34 block0 first data byte", ctx.mpdu_byte[P25_MPDU_R12_BYTES], ctx.r34bytes[2]);
    rc |= expect_u8("rate34 block0 last data byte", ctx.mpdu_byte[P25_MPDU_R12_BYTES + 15], ctx.r34bytes[17]);
    rc |= expect_u8("rate34 block1 first data byte skips crc fields", ctx.mpdu_byte[P25_MPDU_R12_BYTES + 16],
                    ctx.r34bytes[20]);
    rc |= expect_u8("rate34 block1 last data byte", ctx.mpdu_byte[P25_MPDU_R12_BYTES + 31], ctx.r34bytes[35]);
    rc |= expect_u8("rate34 trailing dispatch-excluded byte", ctx.mpdu_byte[P25_MPDU_R12_BYTES + 32], 0);
    return rc;
}

static void
fill_rate12_payload_with_crc(P25MpduContext* ctx, const uint8_t payload_prefix[8]) {
    for (int idx = 0; idx < 8; idx++) {
        ctx->mpdu_byte[P25_MPDU_R12_BYTES + idx] = payload_prefix[idx];
    }
    uint32_t crc = crc32mbf(ctx->mpdu_byte + P25_MPDU_R12_BYTES, 64);
    ctx->mpdu_byte[20] = (uint8_t)(crc >> 24);
    ctx->mpdu_byte[21] = (uint8_t)(crc >> 16);
    ctx->mpdu_byte[22] = (uint8_t)(crc >> 8);
    ctx->mpdu_byte[23] = (uint8_t)crc;
}

static int
test_decode_header_if_usable_dispatch_gate(void) {
    static dsd_opts opts;
    static dsd_state state;
    P25MpduContext ctx;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    p25_mpdu_context_init(&ctx);
    ctx.err[0] = 1;
    ctx.mpdu_byte[0] = 0x37;
    ctx.mpdu_byte[1] = 0x3D;
    ctx.mpdu_byte[6] = 0x02;
    opts.aggressive_framesync = 1;
    reset_dispatch_counters();

    p25_mpdu_decode_header_if_usable(&opts, &state, &ctx);
    rc |= expect_int("aggressive bad header skip", g_pdu_header_calls, 0);
    rc |= expect_int("aggressive bad header fmt unchanged", ctx.fmt, 0);

    opts.aggressive_framesync = 0;
    p25_mpdu_decode_header_if_usable(&opts, &state, &ctx);
    rc |= expect_int("relaxed bad header dispatch", g_pdu_header_calls, 1);
    rc |= expect_int("relaxed header io", ctx.io, 1);
    rc |= expect_int("relaxed header fmt", ctx.fmt, 0x17);
    rc |= expect_int("relaxed header sap", ctx.sap, 0x3D);
    rc |= expect_int("relaxed header blks", ctx.blks, 2);
    return rc;
}

static int
test_rate12_payload_crc_dispatch_and_cleanup(void) {
    static dsd_opts opts;
    static dsd_state state;
    P25MpduContext ctx;
    const uint8_t payload_prefix[8] = {0x81, 0x02, 0x23, 0x44, 0x65, 0x86, 0xA7, 0xC8};
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    p25_mpdu_context_init(&ctx);
    ctx.blks = 1;
    ctx.err[1] = -2;
    state.lasttg = 100;
    state.lastsrc = 200;
    fill_rate12_payload_with_crc(&ctx, payload_prefix);
    reset_dispatch_counters();

    p25_mpdu_handle_rate12(&opts, &state, &ctx);
    rc |= expect_int("rate12 crc ok", ctx.err[1], 0);
    rc |= expect_int("rate12 data dispatch", g_pdu_data_calls, 1);
    rc |= expect_int("rate12 data len", g_last_pdu_data_len, 24);
    rc |= expect_int("rate12 clears tg", state.lasttg, 0);
    rc |= expect_int("rate12 clears src", state.lastsrc, 0);

    p25_mpdu_context_init(&ctx);
    ctx.blks = 0;
    ctx.err[1] = -2;
    state.lasttg = 300;
    state.lastsrc = 400;
    reset_dispatch_counters();
    p25_mpdu_handle_rate12(&opts, &state, &ctx);
    rc |= expect_int("rate12 zero-block crc ok", ctx.err[1], 0);
    rc |= expect_int("rate12 zero-block no data dispatch", g_pdu_data_calls, 0);
    rc |= expect_int("rate12 zero-block clears tg", state.lasttg, 0);
    rc |= expect_int("rate12 zero-block clears src", state.lastsrc, 0);

    p25_mpdu_context_init(&ctx);
    DSD_MEMSET(&opts, 0, sizeof(opts));
    ctx.blks = 1;
    ctx.err[1] = -2;
    ctx.mpdu_byte[P25_MPDU_R12_BYTES] = 0x55;
    opts.aggressive_framesync = 1;
    state.lasttg = 500;
    state.lastsrc = 600;
    reset_dispatch_counters();
    p25_mpdu_handle_rate12(&opts, &state, &ctx);
    rc |= expect_int("rate12 bad crc aggressive suppresses dispatch", g_pdu_data_calls, 0);
    rc |= expect_int("rate12 bad crc preserved", ctx.err[1] != 0, 1);
    rc |= expect_int("rate12 bad crc aggressive clears tg", state.lasttg, 0);

    p25_mpdu_context_init(&ctx);
    ctx.blks = 1;
    ctx.err[1] = -2;
    ctx.mpdu_byte[P25_MPDU_R12_BYTES] = 0x56;
    opts.aggressive_framesync = 0;
    state.lasttg = 700;
    state.lastsrc = 800;
    reset_dispatch_counters();
    p25_mpdu_handle_rate12(&opts, &state, &ctx);
    rc |= expect_int("rate12 bad crc relaxed dispatches", g_pdu_data_calls, 1);
    rc |= expect_int("rate12 bad crc relaxed len", g_last_pdu_data_len, 24);
    rc |= expect_int("rate12 bad crc relaxed clears src", state.lastsrc, 0);
    return rc;
}

static int
test_rate34_dispatch_reconstructs_payload_and_clears_last_call(void) {
    static dsd_opts opts;
    static dsd_state state;
    P25MpduContext ctx;
    uint8_t payload[16] = {0xC0, 0x01, 0x22, 0x43, 0x64, 0x85, 0xA6, 0xC7,
                           0xE8, 0x09, 0x2A, 0x4B, 0x00, 0x00, 0x00, 0x00};
    uint32_t crc = crc32mbf(payload, 96);
    payload[12] = (uint8_t)(crc >> 24);
    payload[13] = (uint8_t)(crc >> 16);
    payload[14] = (uint8_t)(crc >> 8);
    payload[15] = (uint8_t)crc;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    p25_mpdu_context_init(&ctx);
    ctx.r34 = 1;
    ctx.sap = 0x20;
    ctx.fmt = 0x16;
    ctx.blks = 1;
    ctx.err[1] = -2;
    bytes_to_u8_bits(payload, 16, ctx.mpdu_crc_bits);
    for (int byte_idx = 0; byte_idx < P25_MPDU_R34_BYTES; byte_idx++) {
        ctx.r34bytes[byte_idx] = (uint8_t)(0x20 + byte_idx);
    }
    state.lasttg = 900;
    state.lastsrc = 901;
    reset_dispatch_counters();

    p25_mpdu_dispatch_payload(&opts, &state, &ctx);

    int rc = 0;
    rc |= expect_int("rate34 crc ok", ctx.err[1], 0);
    rc |= expect_int("rate34 data dispatch", g_pdu_data_calls, 1);
    rc |= expect_int("rate34 data len", g_last_pdu_data_len, 28);
    rc |= expect_u8("rate34 first reconstructed payload", ctx.mpdu_byte[P25_MPDU_R12_BYTES], ctx.r34bytes[2]);
    rc |= expect_u8("rate34 last block byte", ctx.mpdu_byte[P25_MPDU_R12_BYTES + 15], ctx.r34bytes[17]);
    rc |= expect_int("rate34 clears tg", state.lasttg, 0);
    rc |= expect_int("rate34 clears src", state.lastsrc, 0);
    return rc;
}

static int
test_trunking_payload_crc_dispatch(void) {
    static dsd_opts opts;
    static dsd_state state;
    P25MpduContext ctx;
    const uint8_t payload_prefix[8] = {0x91, 0x12, 0x33, 0x54, 0x75, 0x96, 0xB7, 0xD8};
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    p25_mpdu_context_init(&ctx);
    ctx.sap = 0x3D;
    ctx.fmt = 0x17;
    ctx.io = 1;
    ctx.blks = 1;
    ctx.err[0] = 0;
    ctx.err[1] = -2;
    fill_rate12_payload_with_crc(&ctx, payload_prefix);
    reset_dispatch_counters();

    rc |= expect_int("trunking handler accepted", p25_mpdu_handle_trunking(&opts, &state, &ctx), 1);
    rc |= expect_int("trunking crc ok", ctx.err[1], 0);
    rc |= expect_int("trunking dispatch", g_pdu_trunking_calls, 1);

    ctx.io = 0;
    ctx.fmt = 0x17;
    ctx.err[0] = 0;
    ctx.err[1] = -2;
    fill_rate12_payload_with_crc(&ctx, payload_prefix);
    reset_dispatch_counters();
    rc |= expect_int("inbound ambtc handler accepted", p25_mpdu_handle_trunking(&opts, &state, &ctx), 1);
    rc |= expect_int("inbound ambtc dispatch", g_pdu_trunking_calls, 1);

    ctx.io = 1;
    ctx.fmt = 0x15;
    ctx.err[0] = 0;
    ctx.err[1] = -2;
    fill_rate12_payload_with_crc(&ctx, payload_prefix);
    reset_dispatch_counters();
    rc |= expect_int("umbtc handler accepted", p25_mpdu_handle_trunking(&opts, &state, &ctx), 1);
    rc |= expect_int("umbtc dispatch", g_pdu_trunking_calls, 1);

    ctx.err[0] = 1;
    ctx.err[1] = -2;
    reset_dispatch_counters();
    rc |= expect_int("trunking handler accepted bad header", p25_mpdu_handle_trunking(&opts, &state, &ctx), 1);
    rc |= expect_int("trunking bad header suppresses dispatch", g_pdu_trunking_calls, 0);

    ctx.sap = 0x20;
    reset_dispatch_counters();
    rc |= expect_int("non-trunking handler rejected", p25_mpdu_handle_trunking(&opts, &state, &ctx), 0);
    return rc;
}

static int
test_process_mpdu_zero_block_header_orchestration(void) {
    static dsd_opts opts;
    static dsd_state state;
    P25MpduContext probe;
    uint8_t header[P25_MPDU_R12_BYTES] = {0x17, 0x20, 0x10, 0x11, 0x12, 0x13, 0x00, 0x15, 0x16, 0x17, 0x00, 0x00};
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(g_soft_candidates, 0, sizeof(g_soft_candidates));
    append_crc16(header);
    DSD_MEMCPY(g_soft_candidates[0].bytes, header, sizeof(header));
    g_soft_candidate_count = 1;
    g_r12_fallback_calls = 0;
    g_mbf34_candidate_count = 0;
    g_mbf34_fallback_calls = 0;
    g_get_dibit_soft_calls = 0;
    g_status_add_calls = 0;
    state.lasttg = 77;
    state.lastsrc = 88;
    state.last_active_time = time(NULL);
    reset_dispatch_counters();

    processMPDU(&opts, &state);

    rc |= expect_int("process mpdu counter", (int)state.p25_p1_duid_mpdu, 1);
    rc |= expect_int("process used soft dibit reader", g_get_dibit_soft_calls, 101);
    rc |= expect_int("process avoided r12 fallback", g_r12_fallback_calls, 0);
    rc |= expect_int("process avoided r34 fallback", g_mbf34_fallback_calls, 0);
    rc |= expect_int("process header dispatch", g_pdu_header_calls, 1);
    rc |= expect_int("process no data dispatch for zero block", g_pdu_data_calls, 0);
    rc |= expect_int("process fec ok", (int)state.p25_p1_fec_ok, 1);
    rc |= expect_int("process clears tg", state.lasttg, 0);
    rc |= expect_int("process clears src", state.lastsrc, 0);
    rc |= expect_int("process classifies status", g_status_classify_calls, 1);

    p25_mpdu_context_init(&probe);
    DSD_MEMCPY(probe.tsbk_byte, header, sizeof(header));
    p25_mpdu_unpack_tsbk_bytes(&probe);
    p25_mpdu_store_header_rep(&probe, 0);
    rc |= expect_int("stored header rep crc", probe.hdr_rep_crc[0], 0);
    rc |= expect_u8("stored header rep byte", probe.hdr_rep_bytes[0][3], header[3]);
    p25_mpdu_store_header_rep(&probe, P25_MPDU_HEADER_REPS);
    rc |= expect_int("out-of-range header rep ignored", probe.hdr_rep_crc[0], 0);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_context_init_and_saturating_add();
    rc |= test_prepare_state_resets_mpdu_frame_context();
    rc |= test_header_store_and_update_fields();
    rc |= test_candidate_selection_uses_crc_fields();
    rc |= test_read_repetition_captures_soft_symbols_and_status_dibits();
    rc |= test_decode_blocks_use_candidates_or_fallback_and_pack_crc_bits();
    rc |= test_finalize_header_selects_best_rep_and_majority();
    rc |= test_rate34_crc_bit_packing();
    rc |= test_rate34_zero_block_crc_and_multiblock_reconstruction();
    rc |= test_decode_header_if_usable_dispatch_gate();
    rc |= test_rate12_payload_crc_dispatch_and_cleanup();
    rc |= test_rate34_dispatch_reconstructs_payload_and_clears_last_call();
    rc |= test_trunking_payload_crc_dispatch();
    rc |= test_process_mpdu_zero_block_header_orchestration();
    return rc;
}

// NOLINTEND(bugprone-suspicious-include)
