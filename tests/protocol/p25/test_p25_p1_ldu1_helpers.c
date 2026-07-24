// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result,bugprone-suspicious-include)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 P1 LDU1 helper tests: verify deterministic LCW/LSD field packing and
 * soft-decision RS reliability ordering independent of live IMBE collection.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25p1_check_ldu.h>
#include <dsd-neo/protocol/p25/p25p1_ldu.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/protocol/p25/p25_status_symbol.h"
#include "dsd-neo/protocol/p25/p25p1_soft.h"

#define SOFTID 1

static void
seed_p25_call(dsd_state* state, uint64_t target, uint64_t source) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = source,
    };
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN);
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int g_hard_rs_result;
static int g_soft_rs_result;
static int g_lsd_soft_result = 1;
static int g_lcw_calls;
static int g_policy_make_calls;
static int g_policy_upsert_calls;
static int g_soft_dibit_value;
static int g_get_dibit_soft_calls;
static int g_read_dibit_soft_calls;
static int g_read_dibit_soft_values[16];
static int g_status_add_calls;
static int g_status_classify_calls;
static int g_audio_play_calls;
static int g_active_calls;
static int g_last_status_dibit;
static uint32_t g_last_policy_id;
static uint8_t g_last_policy_source;
static dsd_tg_policy_upsert_mode g_last_policy_upsert_mode;
static char g_last_policy_mode[8];
static char g_last_policy_name[32];

uint8_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25p1_hamming_rs_symbol_reliability(const P25P1SoftDibit* symbol) {
    if (symbol == NULL) {
        return 0;
    }

    int min_reliability = 255;
    for (int i = 0; i < 3; i++) {
        int r0 = symbol[i].llr[0] < 0 ? -(int)symbol[i].llr[0] : (int)symbol[i].llr[0];
        int r1 = symbol[i].llr[1] < 0 ? -(int)symbol[i].llr[1] : (int)symbol[i].llr[1];
        if (r0 < min_reliability) {
            min_reliability = r0;
        }
        if (r1 < min_reliability) {
            min_reliability = r1;
        }
    }
    return (uint8_t)min_reliability;
}

uint64_t
dsd_time_monotonic_ns(void) {
    return 43000000000ULL;
}

int
check_and_fix_reedsolomon_24_12_13(char* data, const char* parity) {
    (void)data;
    (void)parity;
    return g_hard_rs_result;
}

int
p25p1_rs_24_12_13_soft_reliability(char* data, const char* parity, const uint8_t* data_reliab,
                                   const uint8_t* parity_reliab) {
    (void)data;
    (void)parity;
    (void)data_reliab;
    (void)parity_reliab;
    return g_soft_rs_result;
}

int
p25_lsd_fec_16x8_soft(uint8_t* bits16, const int16_t llr16[16]) {
    (void)bits16;
    (void)llr16;
    return g_lsd_soft_result;
}

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    g_get_dibit_soft_calls++;
    if (out_soft != NULL) {
        out_soft->reliability = 77U;
        out_soft->llr[0] = 12;
        out_soft->llr[1] = -8;
    }
    return g_soft_dibit_value;
}

int
read_dibit_soft(dsd_opts* opts, dsd_state* state, char* output, int* status_count, P25P1SoftDibit* soft_dibit) {
    (void)opts;
    (void)state;
    const int call = g_read_dibit_soft_calls++;
    const int dibit = g_read_dibit_soft_values[call % 16] & 0x03;
    if (status_count != NULL) {
        (*status_count)++;
    }
    if (output != NULL) {
        output[0] = (char)((dibit >> 1) & 1);
        output[1] = (char)(dibit & 1);
    }
    if (soft_dibit != NULL) {
        soft_dibit->reliab = (uint8_t)(90 + call);
        soft_dibit->llr[0] = (int16_t)(100 + call);
        soft_dibit->llr[1] = (int16_t)(-50 - call);
    }
    return dibit;
}

void
p25_status_accum_add(dsd_state* state, int dibit_value) {
    (void)state;
    g_status_add_calls++;
    g_last_status_dibit = dibit_value;
}

void
p25_status_accum_classify(dsd_state* state) {
    g_status_classify_calls++;
    if (state != NULL) {
        state->p25_ss_classification = P25_SS_CLASS_INFRASTRUCTURE;
    }
}

void
p25_status_accum_ensure_started(dsd_state* state) {
    (void)state;
}

void
process_IMBE(dsd_opts* opts, dsd_state* state, int* status_count) {
    (void)opts;
    (void)state;
    (void)status_count;
}

void
dsd_play_synthesized_voice(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_audio_play_calls++;
}

int
p25_sm_emit_active(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    (void)state;
    if (slot == 0) {
        g_active_calls++;
    }
    return 1;
}

void
read_and_correct_hex_word(dsd_opts* opts, dsd_state* state, char* hex, int* status_count, P25P1SoftDibit* soft_dibits,
                          int* soft_dibit_index) {
    (void)opts;
    (void)state;
    (void)hex;
    (void)status_count;
    (void)soft_dibits;
    (void)soft_dibit_index;
}

void
p25_lcw(dsd_opts* opts, dsd_state* state, uint8_t LCW_bits[], uint8_t irrecoverable_errors) {
    (void)opts;
    (void)state;
    (void)LCW_bits;
    (void)irrecoverable_errors;
    g_lcw_calls++;
}

int
dsd_tg_policy_make_exact_entry(uint32_t id, const char* mode, const char* name, dsd_tg_policy_entry_source source,
                               dsd_tg_policy_entry* out) {
    g_policy_make_calls++;
    if (out == NULL) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->id_start = id;
    out->id_end = id;
    out->source = (uint8_t)source;
    (void)DSD_SNPRINTF(out->mode, sizeof(out->mode), "%s", mode != NULL ? mode : "");
    (void)DSD_SNPRINTF(out->name, sizeof(out->name), "%s", name != NULL ? name : "");
    return 0;
}

int
dsd_tg_policy_upsert_exact(dsd_state* state, const dsd_tg_policy_entry* entry, dsd_tg_policy_upsert_mode mode) {
    (void)state;
    g_policy_upsert_calls++;
    g_last_policy_upsert_mode = mode;
    if (entry != NULL) {
        g_last_policy_id = entry->id_start;
        g_last_policy_source = entry->source;
        (void)DSD_SNPRINTF(g_last_policy_mode, sizeof(g_last_policy_mode), "%s", entry->mode);
        (void)DSD_SNPRINTF(g_last_policy_name, sizeof(g_last_policy_name), "%s", entry->name);
    }
    return 0;
}

#include "../../../src/protocol/p25/phase1/p25p1_ldu1.c"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif

static void
put_bits6(char dst[6], const char* bits) {
    for (int i = 0; i < 6; i++) {
        dst[i] = (char)(bits[i] == '1');
    }
}

static void
put_u8_bits(uint8_t* dst, uint8_t value) {
    for (int i = 0; i < 8; i++) {
        dst[i] = (uint8_t)((value >> (7 - i)) & 1U);
    }
}

static void
reset_hook_counters(void) {
    g_hard_rs_result = 0;
    g_soft_rs_result = 0;
    g_lsd_soft_result = 1;
    g_lcw_calls = 0;
    g_policy_make_calls = 0;
    g_policy_upsert_calls = 0;
    g_soft_dibit_value = 0;
    g_get_dibit_soft_calls = 0;
    g_read_dibit_soft_calls = 0;
    DSD_MEMSET(g_read_dibit_soft_values, 0, sizeof(g_read_dibit_soft_values));
    g_status_add_calls = 0;
    g_status_classify_calls = 0;
    g_audio_play_calls = 0;
    g_active_calls = 0;
    g_last_status_dibit = -1;
    g_last_policy_id = 0U;
    g_last_policy_source = 0U;
    g_last_policy_upsert_mode = 0;
    g_last_policy_mode[0] = '\0';
    g_last_policy_name[0] = '\0';
}

static int
expect_string(const char* tag, const uint8_t* got, const char* want) {
    if (strcmp((const char*)got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got \"%s\" want \"%s\"\n", tag, (const char*)got, want);
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

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_cstr(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got \"%s\" want \"%s\"\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
test_ldu1_unpack_lc_fields(void) {
    char hex_data[12][6] = {{0}};
    uint8_t lcformat[9];
    uint8_t mfid[9];
    uint8_t lcinfo[57];

    put_bits6(hex_data[11], "101010");
    put_bits6(hex_data[10], "101100");
    put_bits6(hex_data[9], "110011");
    put_bits6(hex_data[8], "000001");
    put_bits6(hex_data[7], "000010");
    put_bits6(hex_data[6], "000100");
    put_bits6(hex_data[5], "001000");
    put_bits6(hex_data[4], "010000");
    put_bits6(hex_data[3], "100000");
    put_bits6(hex_data[2], "111100");
    put_bits6(hex_data[1], "001111");
    put_bits6(hex_data[0], "010101");

    p25p1_ldu1_unpack_lc_fields(hex_data, lcformat, mfid, lcinfo);

    int rc = 0;
    rc |= expect_string("lcformat", lcformat, "10101010");
    rc |= expect_string("mfid", mfid, "11001100");
    rc |= expect_string("lcinfo", lcinfo, "11000001000010000100001000010000100000111100001111010101");
    return rc;
}

static int
test_ldu1_build_lcw_buffers(void) {
    uint8_t lcformat[9] = {'1', '0', '1', '0', '1', '0', '1', '0', 0};
    uint8_t mfid[9] = {'1', '1', '0', '0', '1', '1', '0', '0', 0};
    uint8_t lcinfo[57] = {0};
    const uint8_t want_payload[7] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE};
    uint8_t LCW_bytes[9];
    uint8_t LCW_bits[72];

    for (int byte = 0; byte < 7; byte++) {
        put_u8_bits(&lcinfo[byte * 8], want_payload[byte]);
    }
    lcinfo[56] = 0;

    p25p1_ldu1_build_lcw_buffers(lcformat, mfid, lcinfo, LCW_bytes, LCW_bits);

    int rc = 0;
    rc |= expect_u8("LCW byte 0", LCW_bytes[0], 0xAA);
    rc |= expect_u8("LCW byte 1", LCW_bytes[1], 0xCC);
    for (int byte = 0; byte < 7; byte++) {
        char tag[32];
        DSD_SNPRINTF(tag, sizeof(tag), "LCW payload byte %d", byte);
        rc |= expect_u8(tag, LCW_bytes[byte + 2], want_payload[byte]);
    }
    for (int bit = 0; bit < 72; bit++) {
        uint8_t want = (uint8_t)((LCW_bytes[bit / 8] >> (7 - (bit % 8))) & 1U);
        if (LCW_bits[bit] != want) {
            DSD_FPRINTF(stderr, "LCW bit %d: got %u want %u\n", bit, (unsigned int)LCW_bits[bit], (unsigned int)want);
            rc = 1;
        }
    }
    return rc;
}

static int
test_lsd_corrected_byte(void) {
    const uint8_t bits16[16] = {1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0};
    char out_bits[9];

    uint8_t value = p25p1_lsd_corrected_byte(bits16, out_bits);

    int rc = 0;
    rc |= expect_u8("LSD byte", value, 0xA5);
    if (strcmp(out_bits, "10100101") != 0) {
        DSD_FPRINTF(stderr, "LSD out bits: got \"%s\" want \"10100101\"\n", out_bits);
        rc = 1;
    }
    if (p25p1_lsd_corrected_byte(bits16, NULL) != 0xA5) {
        DSD_FPRINTF(stderr, "LSD null-output byte mismatch\n");
        rc = 1;
    }
    return rc;
}

static void
set_symbol_reliability(P25P1SoftDibit* symbol, int minimum_reliability) {
    symbol[0].llr[0] = (int16_t)(minimum_reliability + 5);
    symbol[0].llr[1] = (int16_t)-(minimum_reliability + 4);
    symbol[1].llr[0] = (int16_t)(minimum_reliability + 3);
    symbol[1].llr[1] = (int16_t)-(minimum_reliability + 2);
    symbol[2].llr[0] = (int16_t)(minimum_reliability + 1);
    symbol[2].llr[1] = (int16_t)minimum_reliability;
}

static int
test_ldu1_rs_reliability_uses_wire_order(void) {
    P25P1SoftDibit soft_dibits[12 * (3 + 2) + 12 * (3 + 2)] = {0};
    uint8_t data_reliab[12] = {0};
    uint8_t parity_reliab[12] = {0};

    for (int wire_symbol = 0; wire_symbol < 12; wire_symbol++) {
        set_symbol_reliability(&soft_dibits[wire_symbol * (3 + 2)], 30 + wire_symbol);
    }
    for (int wire_symbol = 0; wire_symbol < 12; wire_symbol++) {
        set_symbol_reliability(&soft_dibits[(12 * (3 + 2)) + (wire_symbol * (3 + 2))], 70 + wire_symbol);
    }

    build_ldu1_rs_reliability(soft_dibits, data_reliab, parity_reliab);

    int rc = 0;
    rc |= expect_u8("data[0] reliability", data_reliab[0], 41);
    rc |= expect_u8("data[11] reliability", data_reliab[11], 30);
    rc |= expect_u8("parity[0] reliability", parity_reliab[0], 81);
    rc |= expect_u8("parity[11] reliability", parity_reliab[11], 70);
    return rc;
}

static void
fill_lsd_byte(uint8_t bits16[16], uint8_t value) {
    for (int i = 0; i < 8; i++) {
        bits16[i] = (uint8_t)(((value >> (7 - i)) & 1U) != 0U);
    }
}

static int
test_ldu1_init_and_fec_outcomes(void) {
    static dsd_state state;
    ldu1_decode_ctx_t ctx;
    char hex_data[12][6] = {{0}};
    char hex_parity[12][6] = {{0}};
    P25P1SoftDibit soft_dibits[12 * (3 + 2) + 12 * (3 + 2)] = {0};
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    state.p25vc = 7;
    p25p1_ldu1_init_decode_ctx(&state, &ctx);
    rc |= expect_int("init status count", ctx.status_count, 21);
    rc |= expect_int("init clears p25vc", state.p25vc, 0);

    reset_hook_counters();
    rc |= expect_int("hard RS success result", p25p1_ldu1_apply_rs_fec(&state, hex_data, hex_parity, soft_dibits), 0);
    rc |= expect_int("hard RS success ok count", state.p25_p1_voice_fec_ok, 1);
    rc |= expect_int("hard RS success err count", state.p25_p1_voice_fec_err, 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_hook_counters();
    g_hard_rs_result = 1;
    g_soft_rs_result = 0;
    rc |= expect_int("soft RS recovery result", p25p1_ldu1_apply_rs_fec(&state, hex_data, hex_parity, soft_dibits), 0);
    rc |= expect_int("soft RS recovery count", state.p25_p1_soft_rs_ok, 1);
    rc |= expect_int("soft RS recovery ok count", state.p25_p1_voice_fec_ok, 1);

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_hook_counters();
    g_hard_rs_result = 1;
    g_soft_rs_result = 1;
    rc |= expect_int("RS failure result", p25p1_ldu1_apply_rs_fec(&state, hex_data, hex_parity, soft_dibits), 1);
    rc |= expect_int("RS failure err count", state.p25_p1_voice_fec_err, 1);
    rc |= expect_int("RS failure critical count", state.debug_header_critical_errors, 1);
    return rc;
}

static int
test_ldu1_activity_is_independent_of_media_gate(void) {
    static dsd_opts opts;
    static dsd_state state;
    int status_count = 0;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_hook_counters();
    opts.trunk_tune_enc_calls = 1;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;

    p25p1_ldu1_process_imbe_frame(&opts, &state, &status_count, 1);
    rc |= expect_int("blocked follow call emits activity", g_active_calls, 1);
    rc |= expect_int("blocked follow call keeps media muted", g_audio_play_calls, 0);

    p25p1_ldu1_process_imbe_frame(&opts, &state, &status_count, 0);
    rc |= expect_int("non-activity frame does not emit", g_active_calls, 1);
    rc |= expect_int("blocked non-activity frame stays muted", g_audio_play_calls, 0);

    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    p25p1_ldu1_process_imbe_frame(&opts, &state, &status_count, 1);
    rc |= expect_int("clear frame emits activity", g_active_calls, 2);
    rc |= expect_int("clear frame plays audio", g_audio_play_calls, 1);
    return rc;
}

static int
test_ldu1_hold_hysteresis_refreshes_only_recent_activity(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.trunk_hangtime = 0.0;
    state.last_vc_sync_time = time(NULL);
    state.last_vc_sync_time_m = 17.0;

    p25p1_ldu1_refresh_vc_hysteresis(&opts, &state);
    rc |= expect_int("recent VC monotonic refresh", (int)state.last_vc_sync_time_m, 43);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.trunk_hangtime = 1.0;
    state.last_vc_sync_time = time(NULL) - 10;
    state.last_vc_sync_time_m = 19.0;

    p25p1_ldu1_refresh_vc_hysteresis(&opts, &state);
    rc |= expect_int("stale VC monotonic preserved", (int)state.last_vc_sync_time_m, 19);
    return rc;
}

static int
test_ldu1_lcw_output_dispatch(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t lcw_bits[72] = {0};
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_hook_counters();

    p25p1_ldu1_handle_lcw_output(&opts, &state, lcw_bits, 0);
    rc |= expect_int("LCW dispatch on good FEC", g_lcw_calls, 1);

    p25p1_ldu1_handle_lcw_output(&opts, &state, lcw_bits, 1);
    rc |= expect_int("LCW dispatch suppressed on FEC error", g_lcw_calls, 1);
    return rc;
}

static int
test_ldu1_finalize_status_feeds_trailing_symbol(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_hook_counters();
    g_soft_dibit_value = 3;

    p25p1_ldu1_finalize_status(&opts, &state);

    rc |= expect_int("status getDibitSoft calls", g_get_dibit_soft_calls, 1);
    rc |= expect_int("status add calls", g_status_add_calls, 1);
    rc |= expect_int("status trailing dibit", g_last_status_dibit, 3);
    rc |= expect_int("status classify calls", g_status_classify_calls, 1);
    rc |= expect_int("status classification side effect", state.p25_ss_classification, P25_SS_CLASS_INFRASTRUCTURE);
    return rc;
}

static int
test_ldu1_collect_lsd_stores_soft_bits_and_counters(void) {
    static dsd_opts opts;
    static dsd_state state;
    ldu1_decode_ctx_t ctx;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    reset_hook_counters();
    ctx.status_count = 4;
    state.dropL = 10;
    state.octet_counter = 20;

    static const int dibits[16] = {
        2, 2, 1, 1, // LSD1 0xA5
        3, 0, 3, 0, // LSD1 parity bits
        0, 3, 3, 0, // LSD2 0x3C
        1, 2, 1, 2, // LSD2 parity bits
    };
    for (size_t i = 0U; i < 16U; i++) {
        g_read_dibit_soft_values[i] = dibits[i];
    }

    p25p1_ldu1_collect_lsd(&opts, &state, &ctx);

    rc |= expect_int("LSD read calls", g_read_dibit_soft_calls, 16);
    rc |= expect_int("LSD status count", ctx.status_count, 20);
    rc |= expect_int("LSD drop counter", state.dropL, 12);
    rc |= expect_int("LSD octet counter", state.octet_counter, 22);
    rc |= expect_u8("LSD1 collected byte", ctx.lsd_hex1, 0xA5U);
    rc |= expect_u8("LSD2 collected byte", ctx.lsd_hex2, 0x3CU);
    rc |= expect_cstr("LSD1 bit string", ctx.lsd1, "10100101");
    rc |= expect_cstr("LSD2 bit string", ctx.lsd2, "00111100");
    rc |= expect_u8("LSD1 data bit 0", ctx.lowspeeddata[0], 1U);
    rc |= expect_u8("LSD1 data bit 7", ctx.lowspeeddata[7], 1U);
    rc |= expect_u8("LSD1 parity bit 8", ctx.lowspeeddata[8], 1U);
    rc |= expect_u8("LSD2 data bit 16", ctx.lowspeeddata[16], 0U);
    rc |= expect_u8("LSD2 data bit 23", ctx.lowspeeddata[23], 0U);
    rc |= expect_u8("LSD2 parity bit 24", ctx.lowspeeddata[24], 0U);
    rc |= expect_int("LSD1 first LLR0", ctx.lowspeed_llr[0], 100);
    rc |= expect_int("LSD1 first LLR1", ctx.lowspeed_llr[1], -50);
    rc |= expect_int("LSD1 parity LLR0", ctx.lowspeed_llr[8], 104);
    rc |= expect_int("LSD2 first LLR0", ctx.lowspeed_llr[16], 108);
    rc |= expect_int("LSD2 parity LLR1", ctx.lowspeed_llr[25], -62);
    return rc;
}

static int
test_ldu1_lsd_correction_respects_encryption(void) {
    static dsd_state state;
    ldu1_decode_ctx_t ctx;
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    reset_hook_counters();
    fill_lsd_byte(ctx.lowspeeddata + 0, 0x41U);
    fill_lsd_byte(ctx.lowspeeddata + 16, 0x42U);
    state.payload_algid = 0x80;
    p25p1_ldu1_correct_lsd(&state, &ctx);
    rc |= expect_int("clear LSD1 ok", ctx.lsd1_okay, 1);
    rc |= expect_int("clear LSD2 ok", ctx.lsd2_okay, 1);
    rc |= expect_u8("clear LSD1", ctx.lsd_hex1, 0x41U);
    rc |= expect_u8("clear LSD2", ctx.lsd_hex2, 0x42U);

    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    fill_lsd_byte(ctx.lowspeeddata + 0, 0x41U);
    fill_lsd_byte(ctx.lowspeeddata + 16, 0x42U);
    state.payload_algid = 0x84;
    p25p1_ldu1_correct_lsd(&state, &ctx);
    rc |= expect_u8("encrypted LSD1 suppressed", ctx.lsd_hex1, 0U);
    rc |= expect_u8("encrypted LSD2 suppressed", ctx.lsd_hex2, 0U);
    return rc;
}

static int
test_ldu1_softid_alias_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    ldu1_decode_ctx_t ctx;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    reset_hook_counters();

    state.dmr_alias_format[0] = 0x02;
    state.dmr_alias_block_len[0] = 2;
    seed_p25_call(&state, 0U, 1357U);
    state.payload_algid = 0x80;
    ctx.lsd_hex1 = 0x41;
    ctx.lsd_hex2 = 0x42;
    ctx.lsd1_okay = 1;
    ctx.lsd2_okay = 1;

    p25p1_ldu1_handle_softid(&opts, &state, &ctx);
    rc |= expect_int("softid counter after segment", state.data_block_counter[0], 2);
    rc |= expect_int("softid policy make", g_policy_make_calls, 1);
    rc |= expect_int("softid policy upserts", g_policy_upsert_calls, 2);
    rc |= expect_int("softid policy id", (int)g_last_policy_id, 1357);
    rc |= expect_int("softid policy source", (int)g_last_policy_source, (int)DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    rc |=
        expect_int("softid last upsert mode", (int)g_last_policy_upsert_mode, (int)DSD_TG_POLICY_UPSERT_ADD_IF_MISSING);
    rc |= expect_cstr("softid clear mode", g_last_policy_mode, "D");
    rc |= expect_cstr("softid alias name", g_last_policy_name, "AB");

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    ctx.lsd_hex1 = 0x02;
    ctx.lsd_hex2 = 0x0B;
    ctx.lsd1_okay = 1;
    ctx.lsd2_okay = 1;
    p25p1_ldu1_handle_softid(&opts, &state, &ctx);
    rc |= expect_int("softid begin format", state.dmr_alias_format[0], 0x02);
    rc |= expect_int("softid begin clamped length", state.dmr_alias_block_len[0], 8);
    rc |= expect_int("softid begin counter", state.data_block_counter[0], 0);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    reset_hook_counters();
    state.dmr_alias_format[0] = 0x02;
    state.dmr_alias_block_len[0] = 2;
    seed_p25_call(&state, 0U, 2468U);
    state.payload_algid = 0xAA;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    opts.trunk_tune_enc_calls = 0;
    ctx.lsd_hex1 = 0x19;
    ctx.lsd_hex2 = 0x7F;
    ctx.lsd1_okay = 1;
    ctx.lsd2_okay = 1;
    p25p1_ldu1_handle_softid(&opts, &state, &ctx);
    rc |= expect_int("softid invalid chars advance counter", state.data_block_counter[0], 2);
    rc |= expect_int("softid invalid chars kept zero", state.dmr_alias_block_segment[0][0][0][0], 0);
    rc |= expect_int("softid encrypted policy make", g_policy_make_calls, 1);
    rc |= expect_int("softid encrypted policy upserts", g_policy_upsert_calls, 2);
    rc |= expect_cstr("softid encrypted no-key mode", g_last_policy_mode, "DE");
    rc |= expect_cstr("softid invalid alias name empty", g_last_policy_name, "");

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    reset_hook_counters();
    state.dmr_alias_format[0] = 0x02;
    state.dmr_alias_block_len[0] = 2;
    seed_p25_call(&state, 0U, 3069U);
    state.payload_algid = 0xA0;
    state.payload_keyid = 0x0064;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;
    state.p25_p1_crypto_conflict.active = 1U;
    state.p25_p1_crypto_conflict.algid = 0xA0U;
    state.p25_p1_crypto_conflict.keyid = 0x0064U;
    opts.trunk_tune_enc_calls = 0;
    ctx.lsd_hex1 = 0x41;
    ctx.lsd_hex2 = 0x42;
    ctx.lsd1_okay = 1;
    ctx.lsd2_okay = 1;
    p25p1_ldu1_handle_softid(&opts, &state, &ctx);
    rc |= expect_cstr("softid pending conflict remains clear mode", g_last_policy_mode, "D");

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    ctx.lsd_hex1 = 0x02;
    ctx.lsd_hex2 = 0x04;
    ctx.lsd1_okay = 1;
    ctx.lsd2_okay = 0;
    p25p1_ldu1_handle_softid(&opts, &state, &ctx);
    rc |= expect_int("softid begin rejects bad second LSD", state.dmr_alias_format[0], 0);
    rc |= expect_int("softid begin reject preserves counter", state.data_block_counter[0], 0);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_ldu1_unpack_lc_fields();
    rc |= test_ldu1_build_lcw_buffers();
    rc |= test_lsd_corrected_byte();
    rc |= test_ldu1_rs_reliability_uses_wire_order();
    rc |= test_ldu1_init_and_fec_outcomes();
    rc |= test_ldu1_activity_is_independent_of_media_gate();
    rc |= test_ldu1_hold_hysteresis_refreshes_only_recent_activity();
    rc |= test_ldu1_lcw_output_dispatch();
    rc |= test_ldu1_finalize_status_feeds_trailing_symbol();
    rc |= test_ldu1_collect_lsd_stores_soft_bits_and_counters();
    rc |= test_ldu1_lsd_correction_respects_encryption();
    rc |= test_ldu1_softid_alias_state();
    return rc;
}

// NOLINTEND(bugprone-implicit-widening-of-multiplication-result,bugprone-suspicious-include)
