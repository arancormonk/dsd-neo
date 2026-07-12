// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-suspicious-include)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 P1 common LDU helper tests: verify shared IMBE/status and Hamming
 * correction helpers without requiring a live LDU1/LDU2 dibit stream.
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25p1_check_ldu.h>
#include <dsd-neo/protocol/p25/p25p1_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/protocol/p25/p25p1_const.h"

static int g_status_dibit;
static int g_status_add_count;
static int g_get_dibit_soft_count;
static int g_dibit_sequence_len;
static int g_dibit_sequence[80];
static int g_open_mbe_calls;
static int g_vocoder_calls;
static dsd_vocoder_soft_bit g_last_imbe_soft[8][23];
static int g_hard_hamming_result;
static int g_soft_hamming_result;
static char g_read_word_bits[6];
static char g_read_parity_bits[4];
static char g_soft_hamming_output[10];

void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    g_open_mbe_calls++;
    opts->mbe_out_f = stdout;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    int call_index = g_get_dibit_soft_count++;
    int dibit = g_status_dibit;
    const int sequence_capacity = (int)(sizeof(g_dibit_sequence) / sizeof(g_dibit_sequence[0]));
    if (call_index >= 0 && call_index < g_dibit_sequence_len && call_index < sequence_capacity) {
        dibit = g_dibit_sequence[call_index];
    }
    if (out_soft != NULL) {
        out_soft->llr[0] = (int16_t)(11 + call_index);
        out_soft->llr[1] = (int16_t)(-(12 + call_index));
    }
    return dibit;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_status_accum_add(dsd_state* state, int dibit_value) {
    (void)state;
    g_status_add_count++;
    g_status_dibit = dibit_value;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
processMbeFrameSoft(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit imbe_fr[8][23],
                    dsd_vocoder_soft_bit ambe_fr[4][24], dsd_vocoder_soft_bit imbe7100_fr[7][24]) {
    (void)opts;
    (void)state;
    (void)imbe_fr;
    (void)ambe_fr;
    (void)imbe7100_fr;
    g_vocoder_calls++;
    DSD_MEMCPY(g_last_imbe_soft, imbe_fr, sizeof(g_last_imbe_soft));
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
read_word(dsd_opts* opts, dsd_state* state, char* word, unsigned int length, int* status_count,
          P25P1SoftDibit* soft_dibits, int* soft_dibit_index) {
    (void)opts;
    (void)state;
    (void)status_count;
    for (unsigned int idx = 0; idx < length && idx < 6; idx++) {
        word[idx] = g_read_word_bits[idx];
    }
    if (soft_dibits != NULL && soft_dibit_index != NULL) {
        for (int idx = 0; idx < 3; idx++) {
            soft_dibits[*soft_dibit_index + idx].llr[0] = (int16_t)(30 + idx);
            soft_dibits[*soft_dibit_index + idx].llr[1] = (int16_t)(-(40 + idx));
        }
        *soft_dibit_index += 3;
    }
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
read_hamm_parity(dsd_opts* opts, dsd_state* state, char* parity, int* status_count, P25P1SoftDibit* soft_dibits,
                 int* soft_dibit_index) {
    (void)opts;
    (void)state;
    (void)status_count;
    for (int idx = 0; idx < 4; idx++) {
        parity[idx] = g_read_parity_bits[idx];
    }
    if (soft_dibits != NULL && soft_dibit_index != NULL) {
        for (int idx = 0; idx < 2; idx++) {
            soft_dibits[*soft_dibit_index + idx].llr[0] = (int16_t)(50 + idx);
            soft_dibits[*soft_dibit_index + idx].llr[1] = (int16_t)(-(60 + idx));
        }
        *soft_dibit_index += 2;
    }
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
check_and_fix_hamming_10_6_3(char* hex, char* parity) {
    (void)hex;
    (void)parity;
    return g_hard_hamming_result;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
hamming_10_6_3_soft(const char* bits, const int* reliab, char* out_bits) {
    (void)bits;
    (void)reliab;
    DSD_MEMCPY(out_bits, g_soft_hamming_output, 10);
    return g_soft_hamming_result;
}

uint8_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25p1_llr_reliability(const int16_t* llr, int bit_count) {
    int min_reliability = 255;
    for (int bit = 0; bit < bit_count; bit++) {
        int reliab = llr[bit] < 0 ? -(int)llr[bit] : (int)llr[bit];
        if (reliab < min_reliability) {
            min_reliability = reliab;
        }
    }
    return (uint8_t)min_reliability;
}

#include "../../../src/protocol/p25/phase1/p25p1_ldu.c"

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_mem(const char* tag, const char* got, const char* want, size_t len) {
    if (memcmp(got, want, len) != 0) {
        DSD_FPRINTF(stderr, "%s: buffer mismatch\n", tag);
        return 1;
    }
    return 0;
}

static int
test_ldu_input_guards_and_status_symbol(void) {
    static dsd_opts opts;
    static dsd_state state;
    char hex[6] = {0};
    int status_count = 34;
    int soft_index = 0;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    int rc = 0;
    rc |= expect_int("valid inputs", has_valid_ldu_read_inputs(&opts, &state, hex, &status_count, &soft_index), 1);
    rc |= expect_int("null opts invalid", has_valid_ldu_read_inputs(NULL, &state, hex, &status_count, &soft_index), 0);
    rc |= expect_int("null index invalid", has_valid_ldu_read_inputs(&opts, &state, hex, &status_count, NULL), 0);

    maybe_read_midframe_status_symbol(&opts, &state, &status_count);
    rc |= expect_int("status count increments before symbol", status_count, 35);
    rc |= expect_int("no status add before symbol", g_status_add_count, 0);

    g_status_dibit = 3;
    maybe_read_midframe_status_symbol(&opts, &state, &status_count);
    rc |= expect_int("status count reset after symbol", status_count, 1);
    rc |= expect_int("status add count", g_status_add_count, 1);
    rc |= expect_int("status dibit forwarded", g_status_dibit, 3);
    return rc;
}

static int
test_ldu_soft_hamming_inputs_and_reliability(void) {
    P25P1SoftDibit soft[5] = {
        {.llr = {10, -11}}, {.llr = {-12, 13}}, {.llr = {14, -15}}, {.llr = {-16, 17}}, {.llr = {18, -19}},
    };
    char bits[10] = {0};
    int reliab[10] = {0};
    const char raw_hex[6] = {1, 0, 1, 1, 0, 0};
    const char raw_parity[4] = {0, 1, 1, 0};

    build_soft_hamming_inputs(bits, reliab, raw_hex, raw_parity, soft, 0);

    int rc = 0;
    rc |= expect_mem("soft hamming bits", bits, "\001\000\001\001\000\000\000\001\001\000", 10);
    static const int want_reliab[10] = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    for (int idx = 0; idx < 10; idx++) {
        rc |= expect_int("soft hamming reliability", reliab[idx], want_reliab[idx]);
    }
    rc |= expect_int("symbol reliability first three dibits", p25p1_hamming_rs_symbol_reliability(soft), 10);
    rc |= expect_int("null symbol reliability", p25p1_hamming_rs_symbol_reliability(NULL), 0);
    return rc;
}

static int
test_ldu_soft_hamming_fix_paths(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    char hex[6] = {1, 0, 1, 0, 1, 0};
    char parity[4] = {1, 1, 0, 0};
    const char hard_bits[10] = {1, 0, 1, 0, 1, 0, 1, 1, 0, 0};
    P25P1SoftDibit soft[5] = {0};

    g_soft_hamming_result = 2;
    DSD_MEMSET(g_soft_hamming_output, 0, sizeof(g_soft_hamming_output));
    int rc = 0;
    rc |= expect_int("soft irrecoverable returns hard result",
                     apply_soft_hamming_fix(&state, hex, parity, hard_bits, hex, parity, 2, soft, 0), 2);
    rc |= expect_int("soft ok count unchanged", (int)state.p25_p1_soft_hamming_ok, 0);

    static const char corrected[10] = {0, 1, 0, 1, 0, 1, 1, 0, 1, 0};
    DSD_MEMCPY(g_soft_hamming_output, corrected, sizeof(corrected));
    g_soft_hamming_result = 1;
    rc |= expect_int("soft corrected result",
                     apply_soft_hamming_fix(&state, hex, parity, hard_bits, hex, parity, 1, soft, 0), 1);
    rc |= expect_int("soft ok count increment", (int)state.p25_p1_soft_hamming_ok, 1);
    rc |= expect_mem("corrected hex", hex, corrected, 6);
    rc |= expect_mem("corrected parity", parity, corrected + 6, 4);
    return rc;
}

static int
test_ldu_read_and_correct_hex_word(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMCPY(g_read_word_bits, "\001\000\001\000\001\000", 6);
    DSD_MEMCPY(g_read_parity_bits, "\001\001\000\000", 4);
    static const char corrected[10] = {1, 1, 0, 0, 1, 1, 0, 1, 0, 1};
    DSD_MEMCPY(g_soft_hamming_output, corrected, sizeof(corrected));
    g_hard_hamming_result = 1;
    g_soft_hamming_result = 0;

    char hex[6] = {0};
    int status_count = 4;
    int soft_index = 0;
    P25P1SoftDibit soft[5] = {0};
    read_and_correct_hex_word(&opts, &state, hex, &status_count, soft, &soft_index);

    int rc = 0;
    rc |= expect_int("soft dibit index", soft_index, 5);
    rc |= expect_mem("read corrected hex", hex, corrected, 6);
    rc |= expect_int("soft hamming count through read", (int)state.p25_p1_soft_hamming_ok, 1);
    rc |= expect_int("no hard error after soft fix", (int)state.debug_header_errors, 0);

    state.debug_header_errors = 0;
    g_hard_hamming_result = 1;
    read_and_correct_hex_word(&opts, &state, hex, &status_count, NULL, &soft_index);
    rc |= expect_int("hard corrected header error", (int)state.debug_header_errors, 1);
    return rc;
}

static int
test_ldu_imbe_skip_and_encryption_flag(void) {
    static dsd_opts opts;
    static dsd_state state;
    char imbe_fr[8][23] = {{0}};
    dsd_vocoder_soft_bit imbe_soft_fr[8][23] = {{{0}}};
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.trunk_tune_enc_calls = 1;
    opts.dmr_mute_encL = 1;

    static const int non_standard_ones[] = {15, 16, 17};
    for (unsigned int idx = 0; idx < sizeof(non_standard_ones) / sizeof(non_standard_ones[0]); idx++) {
        imbe_fr[0][non_standard_ones[idx]] = 1;
    }

    g_vocoder_calls = 0;
    g_open_mbe_calls = 0;
    process_imbe_or_skip_non_standard(&opts, &state, imbe_fr, imbe_soft_fr);
    int rc = 0;
    rc |= expect_int("non-standard c0 skipped", g_vocoder_calls, 0);
    imbe_fr[0][0] = 1;
    DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "captures");
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;
    process_imbe_or_skip_non_standard(&opts, &state, imbe_fr, imbe_soft_fr);
    rc |= expect_int("pending crypto does not open recording", g_open_mbe_calls, 0);
    rc |= expect_int("pending crypto does not dispatch vocoder", g_vocoder_calls, 0);

    opts.unmute_encrypted_p25 = 1;
    process_imbe_or_skip_non_standard(&opts, &state, imbe_fr, imbe_soft_fr);
    rc |= expect_int("explicit P25 unmute dispatches pending crypto", g_vocoder_calls, 1);

    opts.unmute_encrypted_p25 = 0;
    opts.reverse_mute = 1;
    process_imbe_or_skip_non_standard(&opts, &state, imbe_fr, imbe_soft_fr);
    rc |= expect_int("reverse mute dispatches pending crypto", g_vocoder_calls, 2);

    opts.reverse_mute = 0;
    opts.unmute_encrypted_p25 = 1;
    opts.trunk_tune_enc_calls = 0;
    process_imbe_or_skip_non_standard(&opts, &state, imbe_fr, imbe_soft_fr);
    rc |= expect_int("lockout probe remains silent", g_vocoder_calls, 2);

    opts.unmute_encrypted_p25 = 0;
    opts.dmr_mute_encL = 1;
    opts.trunk_tune_enc_calls = 1;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    process_imbe_or_skip_non_standard(&opts, &state, imbe_fr, imbe_soft_fr);
    rc |= expect_int("standard c0 dispatched", g_vocoder_calls, 3);
    rc |= expect_int("classified voice opens recording", g_open_mbe_calls, 1);

    state.payload_algid = 0x80;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    update_ldu_encryption_flag(&state);
    rc |= expect_int("clear algid not encrypted", state.dmr_encL, 0);
    state.payload_algid = 0x00;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;
    update_ldu_encryption_flag(&state);
    rc |= expect_int("unknown algid encrypted until confirmed", state.dmr_encL, 1);
    return rc;
}

static int
test_ldu_process_imbe_status_cadence_and_soft_dispatch(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(g_last_imbe_soft, 0, sizeof(g_last_imbe_soft));

    g_get_dibit_soft_count = 0;
    g_status_add_count = 0;
    g_vocoder_calls = 0;
    g_dibit_sequence_len = 74;
    for (int idx = 0; idx < g_dibit_sequence_len; idx++) {
        g_dibit_sequence[idx] = idx & 0x3;
    }
    g_dibit_sequence[34] = 2;
    g_dibit_sequence[70] = 3;

    state.payload_algid = 0x80;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    int status_count = 1;
    process_IMBE(&opts, &state, &status_count);

    int rc = 0;
    rc |= expect_int("soft dibits consumed including status", g_get_dibit_soft_count, 74);
    rc |= expect_int("midframe status symbols forwarded", g_status_add_count, 2);
    rc |= expect_int("last status dibit forwarded", g_status_dibit, 3);
    rc |= expect_int("status cadence carries forward", status_count, 3);
    rc |= expect_int("clear payload keeps encryption flag clear", state.dmr_encL, 0);
    rc |= expect_int("IMBE dispatched once", g_vocoder_calls, 1);

    const int first_voice_call = 0;
    int first_dibit = g_dibit_sequence[first_voice_call];
    rc |= expect_int("first interleaved MSB",
                     g_last_imbe_soft[p25p1_imbe_interleave_w[0]][p25p1_imbe_interleave_x[0]].bit,
                     (first_dibit >> 1) & 1);
    rc |= expect_int("first interleaved LSB",
                     g_last_imbe_soft[p25p1_imbe_interleave_y[0]][p25p1_imbe_interleave_z[0]].bit, first_dibit & 1);
    rc |= expect_int("first MSB reliability",
                     g_last_imbe_soft[p25p1_imbe_interleave_w[0]][p25p1_imbe_interleave_x[0]].reliability, 11);
    rc |= expect_int("first LSB reliability",
                     g_last_imbe_soft[p25p1_imbe_interleave_y[0]][p25p1_imbe_interleave_z[0]].reliability, 12);

    const int voice_index_after_first_status = 34;
    const int call_after_first_status = voice_index_after_first_status + 1;
    int dibit_after_first_status = g_dibit_sequence[call_after_first_status];
    rc |= expect_int("voice after status MSB",
                     g_last_imbe_soft[p25p1_imbe_interleave_w[voice_index_after_first_status]]
                                     [p25p1_imbe_interleave_x[voice_index_after_first_status]]
                                         .bit,
                     (dibit_after_first_status >> 1) & 1);
    rc |= expect_int("voice after status LSB",
                     g_last_imbe_soft[p25p1_imbe_interleave_y[voice_index_after_first_status]]
                                     [p25p1_imbe_interleave_z[voice_index_after_first_status]]
                                         .bit,
                     dibit_after_first_status & 1);
    rc |= expect_int("voice after status MSB reliability",
                     g_last_imbe_soft[p25p1_imbe_interleave_w[voice_index_after_first_status]]
                                     [p25p1_imbe_interleave_x[voice_index_after_first_status]]
                                         .reliability,
                     11 + call_after_first_status);
    rc |= expect_int("voice after status LSB reliability",
                     g_last_imbe_soft[p25p1_imbe_interleave_y[voice_index_after_first_status]]
                                     [p25p1_imbe_interleave_z[voice_index_after_first_status]]
                                         .reliability,
                     12 + call_after_first_status);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_ldu_input_guards_and_status_symbol();
    rc |= test_ldu_soft_hamming_inputs_and_reliability();
    rc |= test_ldu_soft_hamming_fix_paths();
    rc |= test_ldu_read_and_correct_hex_word();
    rc |= test_ldu_imbe_skip_and_encryption_flag();
    rc |= test_ldu_process_imbe_status_cadence_and_soft_dispatch();
    return rc;
}

// NOLINTEND(bugprone-suspicious-include)
