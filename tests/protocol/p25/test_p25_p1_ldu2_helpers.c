// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result,bugprone-suspicious-include)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 P1 LDU2 helper tests: verify deterministic ESS/LSD field packing,
 * decrypt-key gating, and soft-decision RS reliability ordering.
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25p1_check_ldu.h>
#include <dsd-neo/protocol/p25/p25p1_ldu.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/protocol/p25/p25p1_soft.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

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

static int g_hard_rs_result;
static int g_soft_rs_result;
static int g_lsd_soft_result;
static int g_lfsr128_calls;
static int g_release_calls;
static int g_watchdog_calls;
static int g_write_event_calls;
static int g_push_event_calls;
static int g_init_event_calls;
static int g_policy_make_calls;
static int g_policy_upsert_calls;
static int g_soft_dibit_value;
static int g_get_dibit_soft_calls;
static int g_read_dibit_soft_calls;
static int g_read_dibit_soft_values[16];
static int g_status_add_calls;
static int g_status_classify_calls;
static int g_last_status_dibit;
static const dsd_opts* g_last_classify_opts;
static uint32_t g_last_policy_id;
static uint8_t g_last_policy_source;
static dsd_tg_policy_upsert_mode g_last_policy_upsert_mode;
static const char* g_lookup_label;
static Event_History_I g_event_history[2];

int
check_and_fix_reedsolomon_24_16_9(char* data, const char* parity) {
    (void)data;
    (void)parity;
    return g_hard_rs_result;
}

int
p25p1_rs_24_16_9_soft_reliability(char* data, const char* parity, const uint8_t* data_reliab,
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

uint64_t
dsd_time_monotonic_ns(void) {
    return 42000000000ULL;
}

uint64_t
ConvertBitIntoBytes(const uint8_t* BufferIn, uint32_t BitLength) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < BitLength; i++) {
        value = (value << 1U) | (uint64_t)(BufferIn[i] & 1U);
    }
    return value;
}

void
LFSR128(dsd_state* state) {
    (void)state;
    g_lfsr128_calls++;
}

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    g_get_dibit_soft_calls++;
    if (out_soft != NULL) {
        out_soft->reliability = 88U;
        out_soft->llr[0] = -9;
        out_soft->llr[1] = 14;
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
        soft_dibit->reliab = (uint8_t)(80 + call);
        soft_dibit->llr[0] = (int16_t)(100 + call);
        soft_dibit->llr[1] = (int16_t)(-60 - call);
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
p25_status_accum_classify(dsd_state* state, const dsd_opts* opts) {
    g_status_classify_calls++;
    g_last_classify_opts = opts;
    if (state != NULL) {
        state->p25_ss_classification = P25_SS_CLASS_SUBSCRIBER;
    }
}

void
p25_status_accum_ensure_started(dsd_state* state) {
    (void)state;
}

void
LFSRP(dsd_state* state) {
    (void)state;
}

void
process_IMBE(dsd_opts* opts, dsd_state* state, int* status_count) {
    (void)opts;
    (void)state;
    (void)status_count;
}

void
p25p1_play_imbe_audio(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
p25_sm_emit_active(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    (void)state;
    (void)slot;
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

#include "../../../src/protocol/p25/phase1/p25p1_ldu2.c"

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
reset_hook_counters(void) {
    g_hard_rs_result = 0;
    g_soft_rs_result = 0;
    g_lsd_soft_result = 1;
    g_lfsr128_calls = 0;
    g_release_calls = 0;
    g_watchdog_calls = 0;
    g_write_event_calls = 0;
    g_push_event_calls = 0;
    g_init_event_calls = 0;
    g_policy_make_calls = 0;
    g_policy_upsert_calls = 0;
    g_soft_dibit_value = 0;
    g_get_dibit_soft_calls = 0;
    g_read_dibit_soft_calls = 0;
    DSD_MEMSET(g_read_dibit_soft_values, 0, sizeof g_read_dibit_soft_values);
    g_status_add_calls = 0;
    g_status_classify_calls = 0;
    g_last_status_dibit = -1;
    g_last_classify_opts = NULL;
    g_last_policy_id = 0U;
    g_last_policy_source = 0U;
    g_last_policy_upsert_mode = 0;
    g_lookup_label = NULL;
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
expect_u64(const char* tag, unsigned long long got, unsigned long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %llu want %llu\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
test_ldu2_extracts_ess_fields(void) {
    char hex_data[16][6] = {{0}};
    uint8_t mi[73];
    char algid[9];
    char kid[17];

    put_bits6(hex_data[15], "100001");
    put_bits6(hex_data[14], "010010");
    put_bits6(hex_data[13], "110011");
    put_bits6(hex_data[12], "001100");
    put_bits6(hex_data[11], "101101");
    put_bits6(hex_data[10], "011110");
    put_bits6(hex_data[9], "111000");
    put_bits6(hex_data[8], "000111");
    put_bits6(hex_data[7], "101010");
    put_bits6(hex_data[6], "010101");
    put_bits6(hex_data[5], "111111");
    put_bits6(hex_data[4], "000000");
    put_bits6(hex_data[3], "101100");
    put_bits6(hex_data[2], "110011");
    put_bits6(hex_data[1], "001101");
    put_bits6(hex_data[0], "010110");

    ldu2_extract_ess_fields((const char (*)[6])hex_data, mi, algid, kid);

    int rc = 0;
    rc |= expect_string("MI", (const char*)mi,
                        "100001010010110011001100101101011110111000000111101010010101111111000000");
    rc |= expect_string("ALGID", algid, "10110011");
    rc |= expect_string("KID", kid, "0011001101010110");
    return rc;
}

static int
test_lsd_corrected_byte(void) {
    const uint8_t bits16[16] = {0, 1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 0, 1, 1};
    char out_bits[9];

    uint8_t value = p25p1_lsd_corrected_byte(bits16, out_bits);

    int rc = 0;
    rc |= expect_u8("LSD byte", value, 0x5A);
    rc |= expect_string("LSD out bits", out_bits, "01011010");
    rc |= expect_u8("LSD null output byte", p25p1_lsd_corrected_byte(bits16, NULL), 0x5A);
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
test_ldu2_rs_reliability_uses_wire_order(void) {
    P25P1SoftDibit soft_dibits[16 * (3 + 2) + 8 * (3 + 2)] = {0};
    uint8_t data_reliab[16] = {0};
    uint8_t parity_reliab[8] = {0};

    for (int wire_symbol = 0; wire_symbol < 16; wire_symbol++) {
        set_symbol_reliability(&soft_dibits[wire_symbol * (3 + 2)], 40 + wire_symbol);
    }
    for (int wire_symbol = 0; wire_symbol < 8; wire_symbol++) {
        set_symbol_reliability(&soft_dibits[(16 * (3 + 2)) + (wire_symbol * (3 + 2))], 90 + wire_symbol);
    }

    build_ldu2_rs_reliability(soft_dibits, data_reliab, parity_reliab);

    int rc = 0;
    rc |= expect_u8("data[0] reliability", data_reliab[0], 55);
    rc |= expect_u8("data[15] reliability", data_reliab[15], 40);
    rc |= expect_u8("parity[0] reliability", parity_reliab[0], 97);
    rc |= expect_u8("parity[7] reliability", parity_reliab[7], 90);
    return rc;
}

static int
test_ldu2_decrypt_key_gate(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;

    state.payload_algid = 0xAA;
    rc |= expect_int("ADP without key", ldu2_payload_has_decrypt_key(&state), 0);
    state.R = 0x1234U;
    rc |= expect_int("ADP with key", ldu2_payload_has_decrypt_key(&state), 1);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.payload_algid = 0x84;
    rc |= expect_int("AES without key", ldu2_payload_has_decrypt_key(&state), 0);
    state.aes_key_loaded[0] = 1;
    rc |= expect_int("AES with key", ldu2_payload_has_decrypt_key(&state), 1);

    state.payload_algid = 0x80;
    rc |= expect_int("clear voice is not decrypt-keyed", ldu2_payload_has_decrypt_key(&state), 0);
    return rc;
}

static void
fill_ess_fields(char hex_data[16][6], unsigned long long mi_hi64, uint8_t mi_tail8, uint8_t algid, uint16_t kid) {
    char mi_bits[72];
    for (int i = 0; i < 64; i++) {
        mi_bits[i] = ((mi_hi64 >> (63 - i)) & 1ULL) != 0ULL ? '1' : '0';
    }
    for (int i = 0; i < 8; i++) {
        mi_bits[64 + i] = ((mi_tail8 >> (7 - i)) & 1U) != 0U ? '1' : '0';
    }

    int pos = 0;
    for (int row = 15; row >= 4; row--) {
        for (int bit = 0; bit < 6; bit++) {
            hex_data[row][bit] = (char)(mi_bits[pos++] == '1');
        }
    }

    for (int bit = 0; bit < 6; bit++) {
        hex_data[3][bit] = (char)(((algid >> (7 - bit)) & 1U) != 0U);
    }
    hex_data[2][0] = (char)(((algid >> 1) & 1U) != 0U);
    hex_data[2][1] = (char)((algid & 1U) != 0U);
    for (int bit = 0; bit < 4; bit++) {
        hex_data[2][bit + 2] = (char)(((kid >> (15 - bit)) & 1U) != 0U);
    }
    for (int bit = 0; bit < 6; bit++) {
        hex_data[1][bit] = (char)(((kid >> (11 - bit)) & 1U) != 0U);
        hex_data[0][bit] = (char)(((kid >> (5 - bit)) & 1U) != 0U);
    }
}

static void
fill_lsd_byte(uint8_t bits16[16], uint8_t value) {
    for (int i = 0; i < 8; i++) {
        bits16[i] = (uint8_t)(((value >> (7 - i)) & 1U) != 0U);
    }
}

static int
test_ldu2_early_unmute_policy(void) {
    static dsd_opts opts;
    static dsd_state state;
    char hex_data[16][6] = {{0}};
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    fill_ess_fields(hex_data, 0x0102030405060708ULL, 0x09U, 0xAAU, 0x1234U);
    state.R = 0x55AAU;
    ldu2_maybe_apply_early_unmute(&opts, &state, (const char (*)[6])hex_data);
    rc |= expect_int("RC4 early unmute with key", opts.unmute_encrypted_p25, 1);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.unmute_encrypted_p25 = 1;
    ldu2_maybe_apply_early_unmute(&opts, &state, (const char (*)[6])hex_data);
    rc |= expect_int("RC4 early mute without key", opts.unmute_encrypted_p25, 0);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.unmute_encrypted_p25 = 7;
    fill_ess_fields(hex_data, 0x0102030405060708ULL, 0x09U, 0x84U, 0x1234U);
    ldu2_maybe_apply_early_unmute(&opts, &state, (const char (*)[6])hex_data);
    rc |= expect_int("AES early policy leaves mute state for key check", opts.unmute_encrypted_p25, 7);
    return rc;
}

static int
test_ldu2_fec_outcomes(void) {
    static dsd_state state;
    char hex_data[16][6] = {{0}};
    char hex_parity[8][6] = {{0}};
    P25P1SoftDibit soft_dibits[16 * (3 + 2) + 8 * (3 + 2)] = {0};
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_hook_counters();
    g_hard_rs_result = 0;
    rc |= expect_int("hard RS success result", ldu2_run_fec(&state, hex_data, hex_parity, soft_dibits), 0);
    rc |= expect_int("hard RS success ok count", state.p25_p1_voice_fec_ok, 1);
    rc |= expect_int("hard RS success err count", state.p25_p1_voice_fec_err, 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_hook_counters();
    g_hard_rs_result = 1;
    g_soft_rs_result = 0;
    rc |= expect_int("soft RS recovery result", ldu2_run_fec(&state, hex_data, hex_parity, soft_dibits), 0);
    rc |= expect_int("soft RS recovery count", state.p25_p1_soft_rs_ok, 1);
    rc |= expect_int("soft RS recovery ok count", state.p25_p1_voice_fec_ok, 1);

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_hook_counters();
    g_hard_rs_result = 1;
    g_soft_rs_result = 1;
    rc |= expect_int("RS failure result", ldu2_run_fec(&state, hex_data, hex_parity, soft_dibits), 1);
    rc |= expect_int("RS failure err count", state.p25_p1_voice_fec_err, 1);
    rc |= expect_int("RS failure critical count", state.debug_header_critical_errors, 1);
    return rc;
}

static int
test_ldu2_hold_hysteresis_refreshes_only_recent_activity(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.trunk_hangtime = 0.0;
    state.last_vc_sync_time = time(NULL);
    state.last_vc_sync_time_m = 17.0;

    ldu2_refresh_hold_hysteresis(&opts, &state);
    rc |= expect_int("recent VC monotonic refresh", (int)state.last_vc_sync_time_m, 42);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.trunk_hangtime = 1.0;
    state.last_vc_sync_time = time(NULL) - 10;
    state.last_vc_sync_time_m = 19.0;

    ldu2_refresh_hold_hysteresis(&opts, &state);
    rc |= expect_int("stale VC monotonic preserved", (int)state.last_vc_sync_time_m, 19);
    return rc;
}

static int
test_ldu2_consume_trailing_status_feeds_classifier(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_hook_counters();
    g_soft_dibit_value = 1;

    ldu2_consume_trailing_status(&opts, &state);

    rc |= expect_int("status getDibitSoft calls", g_get_dibit_soft_calls, 1);
    rc |= expect_int("status add calls", g_status_add_calls, 1);
    rc |= expect_int("status trailing dibit", g_last_status_dibit, 1);
    rc |= expect_int("status classify calls", g_status_classify_calls, 1);
    if (g_last_classify_opts != &opts) {
        DSD_FPRINTF(stderr, "status classify opts pointer mismatch\n");
        rc = 1;
    }
    rc |= expect_int("status classification side effect", state.p25_ss_classification, P25_SS_CLASS_SUBSCRIBER);
    return rc;
}

static int
test_ldu2_capture_lsd_reads_soft_octets_and_updates_counters(void) {
    static dsd_opts opts;
    static dsd_state state;
    Ldu2Frame frame;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&frame, 0, sizeof(frame));
    reset_hook_counters();

    const int dibits[16] = {1, 2, 3, 0, 2, 1, 0, 3, 3, 3, 0, 0, 1, 1, 2, 2};
    for (int i = 0; i < 16; i++) {
        g_read_dibit_soft_values[i] = dibits[i];
    }
    frame.status_count = 21;
    state.dropL = 10;
    state.octet_counter = 4;

    ldu2_capture_lsd(&opts, &state, &frame);

    rc |= expect_int("LDU2 LSD soft reads", g_read_dibit_soft_calls, 16);
    rc |= expect_int("LDU2 LSD status count", frame.status_count, 37);
    rc |= expect_string("LDU2 LSD1 bits", frame.lsd1, "01101100");
    rc |= expect_string("LDU2 LSD2 bits", frame.lsd2, "11110000");
    rc |= expect_u8("LDU2 LSD1 byte", frame.lsd_hex1, 0x6CU);
    rc |= expect_u8("LDU2 LSD2 byte", frame.lsd_hex2, 0xF0U);
    rc |= expect_int("LDU2 LSD data bit 0", frame.lowspeeddata[0], 0);
    rc |= expect_int("LDU2 LSD data bit 7", frame.lowspeeddata[7], 0);
    rc |= expect_int("LDU2 LSD parity bit 8", frame.lowspeeddata[8], 1);
    rc |= expect_int("LDU2 LSD parity bit 15", frame.lowspeeddata[15], 1);
    rc |= expect_int("LDU2 LSD second data bit 16", frame.lowspeeddata[16], 1);
    rc |= expect_int("LDU2 LSD second parity bit 31", frame.lowspeeddata[31], 0);
    rc |= expect_int("LDU2 LSD first llr", frame.lowspeed_llr[0], 100);
    rc |= expect_int("LDU2 LSD first lsb llr", frame.lowspeed_llr[1], -60);
    rc |= expect_int("LDU2 LSD last llr", frame.lowspeed_llr[31], -75);
    rc |= expect_int("LDU2 LSD drop counter", state.dropL, 12);
    rc |= expect_int("LDU2 LSD octet counter", state.octet_counter, 6);
    return rc;
}

static int
test_ldu2_decode_unmute_and_lsd_alias_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    Ldu2Frame frame;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&frame, 0, sizeof(frame));
    reset_hook_counters();

    fill_ess_fields(frame.hex_data, 0x123456789ABCDEF0ULL, 0x12U, 0x84U, 0x1234U);
    fill_lsd_byte(frame.lowspeeddata + 0, 0x41U);
    fill_lsd_byte(frame.lowspeeddata + 16, 0x42U);
    state.payload_algid = 0x80;
    state.aes_key_loaded[0] = 1;
    state.dmr_alias_format[0] = 0x02;
    state.dmr_alias_block_len[0] = 2;
    state.lastsrc = 77;

    ldu2_decode_post_fec_fields(&state, &frame);
    ldu2_print_decode_result(&opts, &state, &frame);
    ldu2_handle_lsd_alias(&opts, &state, &frame);

    rc |= expect_int("decoded ALGID", state.payload_algid, 0x84);
    rc |= expect_int("decoded KID", state.payload_keyid, 0x1234);
    rc |= expect_u64("decoded MI", state.payload_miP, 0x123456789ABCDEF0ULL);
    rc |= expect_u8("decoded LSD1", frame.lsd_hex1, 0x41U);
    rc |= expect_u8("decoded LSD2", frame.lsd_hex2, 0x42U);
    rc |= expect_int("AES LDU2 unmute", opts.unmute_encrypted_p25, 1);
    rc |= expect_int("alias finalized resets format", state.dmr_alias_format[0], 0);
    rc |= expect_int("alias policy make count", g_policy_make_calls, 1);
    rc |= expect_int("alias policy upsert count", g_policy_upsert_calls, 2);
    rc |= expect_int("alias policy id", (int)g_last_policy_id, 77);
    rc |= expect_int("alias policy source", (int)g_last_policy_source, (int)DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    rc |=
        expect_int("alias last upsert mode", (int)g_last_policy_upsert_mode, (int)DSD_TG_POLICY_UPSERT_ADD_IF_MISSING);
    return rc;
}

static int
test_ldu2_decode_post_fec_rejects_malformed_ess_bits(void) {
    static dsd_state state;
    Ldu2Frame frame;
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&frame, 0, sizeof(frame));
    reset_hook_counters();

    fill_ess_fields(frame.hex_data, 0x0102030405060708ULL, 0x09U, 0x80U, 0x1234U);
    frame.hex_data[3][0] = 2;
    frame.hex_data[2][2] = 2;
    fill_lsd_byte(frame.lowspeeddata + 0, 0x41U);
    fill_lsd_byte(frame.lowspeeddata + 16, 0x42U);
    state.payload_algid = 0x84;

    ldu2_decode_post_fec_fields(&state, &frame);

    rc |= expect_int("malformed ALGID falls back to zero", frame.algidhex, 0);
    rc |= expect_int("malformed KID falls back to zero", frame.kidhex, 0);
    rc |= expect_u64("malformed ESS keeps MI high", frame.mihex1, 0x01020304ULL);
    rc |= expect_u64("malformed ESS keeps MI low", frame.mihex2, 0x05060708ULL);
    rc |= expect_u64("malformed ESS keeps MI tail", frame.mihex3, 0x09ULL);
    rc |= expect_string("malformed ESS LSD1 bits", frame.lsd1, "01000001");
    rc |= expect_string("malformed ESS LSD2 bits", frame.lsd2, "01000010");
    rc |= expect_u8("encrypted malformed ESS suppresses LSD1", frame.lsd_hex1, 0U);
    rc |= expect_u8("encrypted malformed ESS suppresses LSD2", frame.lsd_hex2, 0U);
    return rc;
}

static int
test_ldu2_unmute_policy_variants(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.payload_algid = 0x81;
    state.R = 0x123456789ABCDEF0ULL;
    ldu2_apply_unmute_policy(&opts, &state);
    rc |= expect_int("RC4 0x81 key unmutes", opts.unmute_encrypted_p25, 1);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.payload_algid = 0x9F;
    state.R = 0x123456789ABCDEF0ULL;
    ldu2_apply_unmute_policy(&opts, &state);
    rc |= expect_int("RC4 0x9F key unmutes", opts.unmute_encrypted_p25, 1);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.payload_algid = 0x89;
    state.aes_key_loaded[0] = 1;
    state.A1[0] = 0x0011223344556677ULL;
    state.A2[0] = 0x8899AABBCCDDEEFFULL;
    ldu2_apply_unmute_policy(&opts, &state);
    rc |= expect_int("AES-128 key unmutes", opts.unmute_encrypted_p25, 1);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.unmute_encrypted_p25 = 1;
    state.payload_algid = 0x83;
    ldu2_apply_unmute_policy(&opts, &state);
    rc |= expect_int("unknown encrypted ALGID mutes", opts.unmute_encrypted_p25, 0);
    return rc;
}

static int
test_ldu2_lsd_alias_begin_clamps_length(void) {
    static dsd_state state;
    Ldu2Frame frame;

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&frame, 0, sizeof(frame));
    frame.lsd_hex1 = 0x02;
    frame.lsd_hex2 = 0x0B;
    frame.lsd1_okay = 1;
    frame.lsd2_okay = 1;

    ldu2_maybe_begin_lsd_alias(&state, &frame);

    int rc = 0;
    rc |= expect_int("alias begin format", state.dmr_alias_format[0], 0x02);
    rc |= expect_int("alias begin clamped length", state.dmr_alias_block_len[0], 8);
    rc |= expect_int("alias begin counter reset", state.data_block_counter[0], 0);
    return rc;
}

static int
test_ldu2_encrypted_trunk_lockout_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(g_event_history, 0, sizeof(g_event_history));
    reset_hook_counters();

    opts.p25_trunk = 1;
    opts.p25_is_tuned = 1;
    opts.trunk_tune_enc_calls = 0;
    (void)DSD_SNPRINTF(opts.event_out_file, sizeof(opts.event_out_file), "%s", "events.log");
    state.event_history_s = g_event_history;
    state.payload_algid = 0xAA;
    state.payload_keyid = 0x2A2A;
    state.lasttg = 2468;

    ldu2_maybe_enc_lockout(&opts, &state, 0);

    rc |= expect_int("lockout release", g_release_calls, 1);
    rc |= expect_int("lockout force release", state.p25_sm_force_release, 1);
    rc |= expect_int("lockout does not make runtime policy", g_policy_make_calls, 0);
    rc |= expect_int("lockout does not upsert runtime policy", g_policy_upsert_calls, 0);
    rc |= expect_int("lockout watchdog", g_watchdog_calls, 1);
    rc |= expect_int("lockout write event", g_write_event_calls, 1);
    rc |= expect_int("lockout push event", g_push_event_calls, 1);
    rc |= expect_int("lockout init event", g_init_event_calls, 1);

    reset_hook_counters();
    state.p25_sm_force_release = 0;
    state.R = 0x1234U;
    ldu2_maybe_enc_lockout(&opts, &state, 0);
    rc |= expect_int("lockout skipped with key", g_release_calls, 0);
    rc |= expect_int("lockout skipped force release", state.p25_sm_force_release, 0);
    rc |= expect_int("lockout skipped policy", g_policy_make_calls, 0);
    return rc;
}

static int
test_ldu2_lockout_suppression_variants(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(g_event_history, 0, sizeof(g_event_history));
    reset_hook_counters();

    state.event_history_s = g_event_history;
    ldu2_record_enc_lockout(&opts, &state, 0);
    rc |= expect_int("zero talkgroup skips policy", g_policy_make_calls, 0);
    rc |= expect_int("zero talkgroup skips release", g_release_calls, 0);

    reset_hook_counters();
    g_lookup_label = "Known ENC";
    ldu2_record_enc_lockout(&opts, &state, 2468);
    rc |= expect_int("labeled lockout does not make runtime policy", g_policy_make_calls, 0);
    rc |= expect_int("labeled lockout does not upsert runtime policy", g_policy_upsert_calls, 0);
    rc |= expect_int("labeled lockout still logs event", g_watchdog_calls, 1);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(g_event_history, 0, sizeof(g_event_history));
    reset_hook_counters();
    opts.p25_trunk = 1;
    opts.p25_is_tuned = 1;
    state.event_history_s = g_event_history;
    state.payload_algid = 0xAA;
    state.lasttg = 2468;
    (void)DSD_SNPRINTF(state.event_history_s[0].Event_History_Items[1].internal_str,
                       sizeof state.event_history_s[0].Event_History_Items[1].internal_str,
                       "Target: %d; has been locked out; Encryption Lock Out Enabled.", state.lasttg);

    ldu2_maybe_enc_lockout(&opts, &state, 0);
    rc |= expect_int("duplicate lockout still releases", g_release_calls, 1);
    rc |= expect_int("duplicate lockout watchdog", g_watchdog_calls, 1);
    rc |= expect_int("duplicate lockout no history push", g_push_event_calls, 0);
    rc |= expect_int("duplicate lockout no history init", g_init_event_calls, 0);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_ldu2_extracts_ess_fields();
    rc |= test_lsd_corrected_byte();
    rc |= test_ldu2_rs_reliability_uses_wire_order();
    rc |= test_ldu2_decrypt_key_gate();
    rc |= test_ldu2_early_unmute_policy();
    rc |= test_ldu2_fec_outcomes();
    rc |= test_ldu2_hold_hysteresis_refreshes_only_recent_activity();
    rc |= test_ldu2_consume_trailing_status_feeds_classifier();
    rc |= test_ldu2_capture_lsd_reads_soft_octets_and_updates_counters();
    rc |= test_ldu2_decode_unmute_and_lsd_alias_state();
    rc |= test_ldu2_decode_post_fec_rejects_malformed_ess_bits();
    rc |= test_ldu2_unmute_policy_variants();
    rc |= test_ldu2_lsd_alias_begin_clamps_length();
    rc |= test_ldu2_encrypted_trunk_lockout_state();
    rc |= test_ldu2_lockout_suppression_variants();
    return rc;
}

// NOLINTEND(bugprone-implicit-widening-of-multiplication-result,bugprone-suspicious-include)
