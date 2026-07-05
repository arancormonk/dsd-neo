// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result,bugprone-suspicious-include)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 P1 HDU helper tests: verify the deterministic field packing and
 * soft-decision RS reliability ordering used before HDU decode side effects.
 */

#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/core/talkgroup_policy.h"
#include "dsd-neo/platform/timing.h"
#include "dsd-neo/protocol/p25/p25_lfsr.h"
#include "dsd-neo/protocol/p25/p25_status_symbol.h"
#include "dsd-neo/protocol/p25/p25_trunk_sm.h"
#include "dsd-neo/protocol/p25/p25p1_check_hdu.h"
#include "dsd-neo/runtime/p25_optional_hooks.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

uint8_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25p1_llr_reliability(const int16_t* llr, int bit_count) {
    if (llr == NULL || bit_count <= 0) {
        return 0;
    }

    int min_reliability = 255;
    for (int i = 0; i < bit_count; i++) {
        int reliab = llr[i] < 0 ? -(int)llr[i] : (int)llr[i];
        if (reliab > 255) {
            reliab = 255;
        }
        if (reliab < min_reliability) {
            min_reliability = reliab;
        }
    }
    return (uint8_t)min_reliability;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
ConvertBitIntoBytes(const uint8_t* BufferIn, uint32_t BitLength) {
    uint64_t out = 0;
    for (uint32_t i = 0; i < BitLength; i++) {
        out = (out << 1U) | (uint64_t)(BufferIn[i] & 1U);
    }
    return out;
}

#include "../../../src/protocol/p25/phase1/p25p1_hdu.c"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif

static int g_lfsr128_calls;
static int g_release_calls;
static int g_watchdog_calls;
static int g_write_event_calls;
static int g_push_event_calls;
static int g_init_event_calls;
static int g_policy_make_calls;
static int g_policy_upsert_calls;
static uint32_t g_last_policy_id;
static uint8_t g_last_policy_source;
static dsd_tg_policy_upsert_mode g_last_policy_upsert_mode;
static const char* g_lookup_label;
static Event_History_I g_event_history[2];
static int g_hard_golay_fixed;
static int g_hard_golay_result;
static int g_soft_golay_fixed;
static int g_soft_golay_result;
static char g_soft_golay_hex[6];
static char g_soft_golay_parity[12];
static int g_rs_hard_result;
static int g_rs_soft_result;
static int g_soft_input_count;
static int g_soft_input_index;
static int g_soft_dibits[400];
static dsd_dibit_soft_t g_soft_values[400];
static int g_status_accum_count;
static int g_status_values[16];

static void
reset_soft_inputs(void) {
    g_soft_input_count = 0;
    g_soft_input_index = 0;
    DSD_MEMSET(g_soft_dibits, 0, sizeof(g_soft_dibits));
    DSD_MEMSET(g_soft_values, 0, sizeof(g_soft_values));
    g_status_accum_count = 0;
    DSD_MEMSET(g_status_values, 0, sizeof(g_status_values));
}

static void
append_soft_input(int dibit, uint8_t reliability, int16_t llr0, int16_t llr1) {
    if (g_soft_input_count >= (int)(sizeof(g_soft_dibits) / sizeof(g_soft_dibits[0]))) {
        return;
    }
    g_soft_dibits[g_soft_input_count] = dibit & 0x03;
    g_soft_values[g_soft_input_count].reliability = reliability;
    g_soft_values[g_soft_input_count].llr[0] = llr0;
    g_soft_values[g_soft_input_count].llr[1] = llr1;
    g_soft_input_count++;
}

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    if (g_soft_input_index >= g_soft_input_count) {
        if (out_soft != NULL) {
            DSD_MEMSET(out_soft, 0, sizeof(*out_soft));
        }
        return 0;
    }
    if (out_soft != NULL) {
        *out_soft = g_soft_values[g_soft_input_index];
    }
    return g_soft_dibits[g_soft_input_index++];
}

void
p25_status_accum_add(dsd_state* state, int dibit_value) {
    (void)state;
    g_status_values[g_status_accum_count++] = dibit_value & 0x03;
}

void
p25_status_accum_ensure_started(dsd_state* state) {
    (void)state;
}

void
p25_status_accum_classify(dsd_state* state, const dsd_opts* opts) {
    (void)state;
    (void)opts;
}

uint64_t
dsd_time_monotonic_ns(void) {
    return 123456789000ULL;
}

int
check_and_fix_golay_24_6(char* hex, const char* parity, int* fixed_errors) {
    (void)hex;
    (void)parity;
    if (fixed_errors != NULL) {
        *fixed_errors = g_hard_golay_fixed;
    }
    return g_hard_golay_result;
}

int
check_and_fix_golay_24_6_soft(char* data, const char* parity, const int* reliab, int* fixed) {
    (void)reliab;
    if (fixed != NULL) {
        *fixed = g_soft_golay_fixed;
    }
    if (g_soft_golay_result == 0) {
        DSD_MEMCPY(data, g_soft_golay_hex, sizeof(g_soft_golay_hex));
        DSD_MEMCPY((char*)parity, g_soft_golay_parity, sizeof(g_soft_golay_parity));
    }
    return g_soft_golay_result;
}

int
check_and_fix_redsolomon_36_20_17(char* data, const char* parity) {
    (void)data;
    (void)parity;
    return g_rs_hard_result;
}

int
p25p1_rs_36_20_17_soft_reliability(char* data, const char* parity, const uint8_t* data_reliab,
                                   const uint8_t* parity_reliab) {
    (void)data;
    (void)parity;
    (void)data_reliab;
    (void)parity_reliab;
    return g_rs_soft_result;
}

void
LFSR128(dsd_state* state) {
    (void)state;
    g_lfsr128_calls++;
}

void
p25_sm_on_release(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_release_calls++;
}

void
p25_sm_note_encrypted_call(dsd_opts* opts, dsd_state* state, int tg) {
    (void)opts;
    (void)state;
    (void)tg;
}

int
dsd_tg_policy_lookup_label(const dsd_state* state, uint32_t id, char* mode, size_t mode_sz, char* name,
                           size_t name_sz) {
    (void)state;
    (void)id;
    if (g_lookup_label == NULL) {
        return 0;
    }
    if (mode != NULL && mode_sz > 0U) {
        (void)DSD_SNPRINTF(mode, mode_sz, "%s", "DE");
    }
    if (name != NULL && name_sz > 0U) {
        (void)DSD_SNPRINTF(name, name_sz, "%s", g_lookup_label);
    }
    return 1;
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
    }
    return 0;
}

void
dsd_p25_optional_hook_watchdog_event_current(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
    g_watchdog_calls++;
}

void
dsd_p25_optional_hook_write_event_to_log_file(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t swrite,
                                              char* event_string) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)swrite;
    (void)event_string;
    g_write_event_calls++;
}

void
dsd_p25_optional_hook_push_event_history(Event_History_I* event_struct) {
    (void)event_struct;
    g_push_event_calls++;
}

void
dsd_p25_optional_hook_init_event_history(Event_History_I* event_struct, uint8_t start, uint8_t stop) {
    (void)event_struct;
    (void)start;
    (void)stop;
    g_init_event_calls++;
}

static void
reset_hook_counters(void) {
    g_lfsr128_calls = 0;
    g_release_calls = 0;
    g_watchdog_calls = 0;
    g_write_event_calls = 0;
    g_push_event_calls = 0;
    g_init_event_calls = 0;
    g_policy_make_calls = 0;
    g_policy_upsert_calls = 0;
    g_last_policy_id = 0U;
    g_last_policy_source = 0U;
    g_last_policy_upsert_mode = 0;
    g_lookup_label = NULL;
}

static void
reset_fec_stubs(void) {
    g_hard_golay_fixed = 0;
    g_hard_golay_result = 0;
    g_soft_golay_fixed = 0;
    g_soft_golay_result = 1;
    DSD_MEMSET(g_soft_golay_hex, 0, sizeof(g_soft_golay_hex));
    DSD_MEMSET(g_soft_golay_parity, 0, sizeof(g_soft_golay_parity));
    g_rs_hard_result = 0;
    g_rs_soft_result = 1;
}

static void
put_bits(char dst[6], const char* bits) {
    for (int i = 0; i < 6; i++) {
        dst[i] = (char)(bits[i] == '1');
    }
}

static int
expect_string(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got \"%s\" want \"%s\"\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u8(const char* tag, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %u want %u\n", tag, (unsigned int)got, (unsigned int)want);
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
expect_u64(const char* tag, unsigned long long got, unsigned long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %llu want %llu\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
test_hdu_extracts_payload_fields(void) {
    char hex_data[20][6] = {{0}};
    uint8_t mi[73];
    char algid[9];
    char kid[17];

    put_bits(hex_data[19], "100001");
    put_bits(hex_data[18], "010010");
    put_bits(hex_data[17], "110011");
    put_bits(hex_data[16], "001100");
    put_bits(hex_data[15], "101101");
    put_bits(hex_data[14], "011110");
    put_bits(hex_data[13], "111000");
    put_bits(hex_data[12], "000111");
    put_bits(hex_data[11], "101010");
    put_bits(hex_data[10], "010101");
    put_bits(hex_data[9], "111111");
    put_bits(hex_data[8], "000000");
    put_bits(hex_data[7], "001011");
    put_bits(hex_data[6], "101100");
    put_bits(hex_data[5], "011001");
    put_bits(hex_data[4], "110010");
    put_bits(hex_data[3], "001101");
    put_bits(hex_data[2], "100000");

    hdu_extract_mi_algid_kid((const char (*)[6])hex_data, mi, algid, kid);

    int rc = 0;
    rc |= expect_string("MI", (const char*)mi,
                        "100001010010110011001100101101011110111000000111101010010101111111000000");
    rc |= expect_string("ALGID", algid, "11000110");
    rc |= expect_string("KID", kid, "0111001000110110");
    return rc;
}

static void
set_symbol_reliability(P25P1SoftDibit* symbol, int minimum_reliability) {
    symbol[0].llr[0] = (int16_t)(minimum_reliability + 6);
    symbol[0].llr[1] = (int16_t)-(minimum_reliability + 5);
    symbol[1].llr[0] = (int16_t)(minimum_reliability + 4);
    symbol[1].llr[1] = (int16_t)-(minimum_reliability + 3);
    symbol[2].llr[0] = (int16_t)(minimum_reliability + 2);
    symbol[2].llr[1] = (int16_t)minimum_reliability;
}

static int
test_hdu_rs_reliability_uses_wire_order(void) {
    P25P1SoftDibit soft_dibits[20 * (3 + 6) + 16 * (3 + 6)] = {0};
    uint8_t data_reliab[20] = {0};
    uint8_t parity_reliab[16] = {0};

    for (int wire_symbol = 0; wire_symbol < 20; wire_symbol++) {
        set_symbol_reliability(&soft_dibits[wire_symbol * (3 + 6)], 20 + wire_symbol);
    }
    for (int wire_symbol = 0; wire_symbol < 16; wire_symbol++) {
        set_symbol_reliability(&soft_dibits[(20 * (3 + 6)) + (wire_symbol * (3 + 6))], 80 + wire_symbol);
    }

    build_hdu_rs_reliability(soft_dibits, data_reliab, parity_reliab);

    int rc = 0;
    rc |= expect_u8("data[0] reliability", data_reliab[0], 39);
    rc |= expect_u8("data[19] reliability", data_reliab[19], 20);
    rc |= expect_u8("parity[0] reliability", parity_reliab[0], 95);
    rc |= expect_u8("parity[15] reliability", parity_reliab[15], 80);
    return rc;
}

static int
test_soft_abs_i16_handles_negative_and_saturated_inputs(void) {
    int rc = 0;
    if (soft_abs_i16((int16_t)-123) != 123) {
        DSD_FPRINTF(stderr, "negative soft abs mismatch\n");
        rc = 1;
    }
    if (soft_abs_i16((int16_t)0) != 0) {
        DSD_FPRINTF(stderr, "zero soft abs mismatch\n");
        rc = 1;
    }
    if (soft_abs_i16((int16_t)-32768) != 32768) {
        DSD_FPRINTF(stderr, "int16 minimum soft abs mismatch\n");
        rc = 1;
    }
    return rc;
}

static int
test_hdu_soft_dibit_reader_contracts(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    char out[4] = {0};
    P25P1SoftDibit soft_dibit;
    P25P1SoftDibit soft_dibits[4];
    int status_count;
    int soft_dibit_index;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&soft_dibit, 0, sizeof(soft_dibit));
    reset_soft_inputs();
    append_soft_input(3, 70U, 7, -8);
    append_soft_input(2, 44U, -5, 6);
    status_count = 35;

    int dibit = read_dibit_soft(&opts, &state, out, &status_count, &soft_dibit);
    rc |= expect_int("status-boundary-data-dibit", dibit, 2);
    rc |= expect_int("status-boundary-output-msb", out[0], 1);
    rc |= expect_int("status-boundary-output-lsb", out[1], 0);
    rc |= expect_int("status-boundary-counter-reset", status_count, 1);
    rc |= expect_int("status-boundary-symbol-recorded", g_status_accum_count, 1);
    rc |= expect_int("status-boundary-symbol-value", g_status_values[0], 3);
    rc |= expect_int("status-boundary-soft-index", g_soft_input_index, 2);
    rc |= expect_u8("status-boundary-reliability", soft_dibit.reliab, 44U);
    rc |= expect_int("status-boundary-llr0", soft_dibit.llr[0], -5);
    rc |= expect_int("status-boundary-llr1", soft_dibit.llr[1], 6);

    reset_soft_inputs();
    append_soft_input(2, 30U, 11, -12);
    append_soft_input(1, 31U, 13, -14);
    DSD_MEMSET(out, 0, sizeof(out));
    DSD_MEMSET(soft_dibits, 0, sizeof(soft_dibits));
    status_count = 33;
    soft_dibit_index = 0;

    read_dibit_update_soft_data(&opts, &state, out, 4U, &status_count, soft_dibits, &soft_dibit_index);
    rc |= expect_int("update-output-0", out[0], 1);
    rc |= expect_int("update-output-1", out[1], 0);
    rc |= expect_int("update-output-2", out[2], 0);
    rc |= expect_int("update-output-3", out[3], 1);
    rc |= expect_int("update-no-status-before-boundary", g_status_accum_count, 0);
    rc |= expect_int("update-status-counter", status_count, 35);
    rc |= expect_int("update-soft-count", soft_dibit_index, 2);
    rc |= expect_u8("update-soft0-reliability", soft_dibits[0].reliab, 30U);
    rc |= expect_u8("update-soft1-reliability", soft_dibits[1].reliab, 31U);

    reset_soft_inputs();
    for (int i = 0; i < 5; i++) {
        append_soft_input(i & 0x03, (uint8_t)(50 + i), (int16_t)i, (int16_t)-i);
    }
    append_soft_input(2, 90U, 21, -22);

    hdu_consume_trailing_dibits_and_status(&opts, &state);
    rc |= expect_int("trailing-consume-count", g_soft_input_index, 6);
    rc |= expect_int("trailing-status-count", g_status_accum_count, 1);
    rc |= expect_int("trailing-status-value", g_status_values[0], 2);

    return rc;
}

static int
test_hdu_fec_helpers_account_for_hard_soft_and_failed_corrections(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    char hex[6] = {1, 0, 1, 0, 1, 0};
    char parity[12] = {0};
    P25P1SoftDibit soft_dibits[9];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(soft_dibits, 0, sizeof(soft_dibits));
    for (int i = 0; i < 9; i++) {
        soft_dibits[i].llr[0] = (int16_t)(i + 10);
        soft_dibits[i].llr[1] = (int16_t)-(i + 20);
    }

    reset_fec_stubs();
    g_hard_golay_fixed = 2;
    correct_hex_word(&opts, &state, hex, parity, NULL);
    rc |= expect_int("hard golay fixed errors counted", state.debug_header_errors, 2);
    rc |= expect_int("hard golay no critical error", state.debug_header_critical_errors, 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_fec_stubs();
    g_hard_golay_result = 1;
    g_soft_golay_result = 0;
    g_soft_golay_fixed = 3;
    put_bits(g_soft_golay_hex, "010101");
    for (int i = 0; i < 12; i++) {
        g_soft_golay_parity[i] = (char)(i & 1);
    }
    correct_hex_word(&opts, &state, hex, parity, soft_dibits);
    rc |= expect_int("soft golay success count", (int)state.p25_p1_soft_golay_ok, 1);
    rc |= expect_int("soft golay fixed errors counted", state.debug_header_errors, 3);
    rc |= expect_int("soft golay avoids critical", state.debug_header_critical_errors, 0);
    rc |= expect_int("soft golay replaces hex bit", hex[0], 0);
    rc |= expect_int("soft golay replaces parity bit", parity[1], 1);

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_fec_stubs();
    g_hard_golay_result = 1;
    g_soft_golay_result = 1;
    correct_hex_word(&opts, &state, hex, parity, soft_dibits);
    rc |= expect_int("golay failure critical", state.debug_header_critical_errors, 1);

    return rc;
}

static int
test_hdu_read_and_fec_updates_rs_success_soft_success_and_failure_state(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    char hex_data[20][6];
    char hex_parity[16][6];
    P25P1SoftDibit soft_dibits[20 * (3 + 6) + 16 * (3 + 6)];
    int status_count;
    int soft_dibit_index;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(hex_data, 0, sizeof(hex_data));
    DSD_MEMSET(hex_parity, 0, sizeof(hex_parity));
    DSD_MEMSET(soft_dibits, 0, sizeof(soft_dibits));
    reset_soft_inputs();
    reset_fec_stubs();
    status_count = 21;
    soft_dibit_index = 0;

    rc |= expect_int(
        "hdu rs hard success",
        hdu_read_and_fec(&opts, &state, hex_data, hex_parity, soft_dibits, &status_count, &soft_dibit_index), 0);
    rc |= expect_int("hdu rs hard fec ok", (int)state.p25_p1_voice_fec_ok, 1);
    rc |= expect_int("hdu rs hard fec err", (int)state.p25_p1_voice_fec_err, 0);
    rc |= expect_int("hdu rs hard soft count", (int)state.p25_p1_soft_rs_ok, 0);
    rc |= expect_int("hdu rs hard collected soft dibits", soft_dibit_index, 324);
    rc |= expect_int("hdu rs hard sync time set", state.last_vc_sync_time != 0, 1);

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_soft_inputs();
    reset_fec_stubs();
    g_rs_hard_result = 1;
    g_rs_soft_result = 0;
    status_count = 21;
    soft_dibit_index = 0;
    rc |= expect_int(
        "hdu rs soft success",
        hdu_read_and_fec(&opts, &state, hex_data, hex_parity, soft_dibits, &status_count, &soft_dibit_index), 0);
    rc |= expect_int("hdu rs soft fec ok", (int)state.p25_p1_voice_fec_ok, 1);
    rc |= expect_int("hdu rs soft count", (int)state.p25_p1_soft_rs_ok, 1);
    rc |= expect_int("hdu rs soft no critical", state.debug_header_critical_errors, 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_soft_inputs();
    reset_fec_stubs();
    g_rs_hard_result = 1;
    g_rs_soft_result = 1;
    status_count = 21;
    soft_dibit_index = 0;
    rc |= expect_int(
        "hdu rs failure result",
        hdu_read_and_fec(&opts, &state, hex_data, hex_parity, soft_dibits, &status_count, &soft_dibit_index), 1);
    rc |= expect_int("hdu rs failure fec err", (int)state.p25_p1_voice_fec_err, 1);
    rc |= expect_int("hdu rs failure critical", state.debug_header_critical_errors, 1);
    rc |= expect_int("hdu rs failure no fec ok", (int)state.p25_p1_voice_fec_ok, 0);

    return rc;
}

static int
test_hdu_unmute_policy_and_good_decode_state(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.payload_algid = 0xAA;
    state.R = 0x12345U;
    hdu_apply_unmute_policy(&opts, &state);
    rc |= expect_int("rc4 key unmutes encrypted p25", opts.unmute_encrypted_p25, 1);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.unmute_encrypted_p25 = 1;
    state.payload_algid = 0xAA;
    hdu_apply_unmute_policy(&opts, &state);
    rc |= expect_int("missing rc4 key mutes encrypted p25", opts.unmute_encrypted_p25, 0);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.payload_algid = 0x84;
    state.aes_key_loaded[0] = 1;
    state.A1[0] = 0x1111111111111111ULL;
    state.A2[0] = 0x2222222222222222ULL;
    state.A3[0] = 0x3333333333333333ULL;
    state.A4[0] = 0x4444444444444444ULL;
    hdu_apply_unmute_policy(&opts, &state);
    rc |= expect_int("aes key unmutes encrypted p25", opts.unmute_encrypted_p25, 1);

    reset_hook_counters();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.aes_key_loaded[0] = 1;
    hdu_handle_good_decode(&opts, &state, 0x84, 0x1234, 0x01020304ULL, 0x05060708ULL, 0x09ULL);
    rc |= expect_int("good hdu algid", state.payload_algid, 0x84);
    rc |= expect_int("good hdu kid", state.payload_keyid, 0x1234);
    rc |= expect_u64("good hdu mi", state.payload_miP, 0x0102030405060708ULL);
    rc |= expect_int("good hdu xl flag", state.xl_is_hdu, 1);
    rc |= expect_int("good hdu aes lfsr", g_lfsr128_calls, 1);
    rc |= expect_int("good hdu no lockout release", g_release_calls, 0);

    return rc;
}

static int
test_hdu_encrypted_trunk_lockout_state(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

    reset_hook_counters();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(g_event_history, 0, sizeof(g_event_history));
    state.event_history_s = g_event_history;
    opts.p25_trunk = 1;
    opts.p25_is_tuned = 1;
    DSD_SNPRINTF(opts.event_out_file, sizeof(opts.event_out_file), "%s", "events.log");
    state.lasttg = 1234;
    g_lookup_label = "Secure TG";

    hdu_handle_good_decode(&opts, &state, 0xAA, 0x2A2A, 0xAABBCCDDULL, 0x10203040ULL, 0ULL);

    rc |= expect_int("lockout clears algid", state.payload_algid, 0);
    rc |= expect_int("lockout clears kid", state.payload_keyid, 0);
    rc |= expect_u64("lockout clears mi", state.payload_miP, 0ULL);
    rc |= expect_int("lockout force release", state.p25_sm_force_release, 1);
    rc |= expect_int("lockout release hook", g_release_calls, 1);
    rc |= expect_int("lockout does not make runtime policy", g_policy_make_calls, 0);
    rc |= expect_int("lockout does not upsert runtime policy", g_policy_upsert_calls, 0);
    rc |= expect_int("lockout watchdog", g_watchdog_calls, 1);
    rc |= expect_int("lockout event write", g_write_event_calls, 1);
    rc |= expect_int("lockout event push", g_push_event_calls, 1);
    rc |= expect_int("lockout event init", g_init_event_calls, 1);
    rc |= expect_int("lockout call string cleared", state.call_string[0][0] == ' ', 1);

    return rc;
}

int
main(void) {
    int rc = 0;
    reset_fec_stubs();
    rc |= test_hdu_extracts_payload_fields();
    rc |= test_hdu_rs_reliability_uses_wire_order();
    rc |= test_soft_abs_i16_handles_negative_and_saturated_inputs();
    rc |= test_hdu_soft_dibit_reader_contracts();
    rc |= test_hdu_fec_helpers_account_for_hard_soft_and_failed_corrections();
    rc |= test_hdu_read_and_fec_updates_rs_success_soft_success_and_failure_state();
    rc |= test_hdu_unmute_policy_and_good_decode_state();
    rc |= test_hdu_encrypted_trunk_lockout_state();
    return rc;
}

// NOLINTEND(bugprone-implicit-widening-of-multiplication-result,bugprone-suspicious-include)
