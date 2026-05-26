// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_lfsr.h>
#include <dsd-neo/protocol/p25/p25_lsd.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25p1_check_ldu.h>
#include <dsd-neo/protocol/p25/p25p1_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_ldu.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/p25_optional_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/secret_redaction.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/dsp/p25p1_heuristics.h"

static void
build_ldu2_rs_reliability(const AnalogSignal* analog_signal_array, uint8_t data_reliab[16], uint8_t parity_reliab[8]) {
    for (int i = 0; i < 16; i++) {
        int analog_index = (15 - i) * (3 + 2);
        data_reliab[i] = p25p1_hamming_rs_symbol_reliability(analog_signal_array + analog_index);
    }
    for (int i = 0; i < 8; i++) {
        int analog_index = (16 * (3 + 2)) + ((7 - i) * (3 + 2));
        parity_reliab[i] = p25p1_hamming_rs_symbol_reliability(analog_signal_array + analog_index);
    }
}

static uint8_t
p25p1_lsd_corrected_byte(const uint8_t bits16[16], char out_bits[9]) {
    uint8_t value = 0;

    for (int i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)(bits16[i] & 1U);
        value = (uint8_t)((value << 1) | bit);
        if (out_bits != NULL) {
            out_bits[i] = (char)(bit + '0');
        }
    }
    if (out_bits != NULL) {
        out_bits[8] = 0;
    }
    return value;
}

typedef struct {
    uint8_t mi[73];
    char algid[9];
    char kid[17];
    char lsd1[9];
    char lsd2[9];
    int algidhex;
    int kidhex;
    uint8_t lsd_hex1;
    uint8_t lsd_hex2;
    uint8_t lowspeeddata[32];
    int16_t lowspeed_llr[32];
    unsigned long long mihex1;
    unsigned long long mihex2;
    unsigned long long mihex3;
    int lsd1_okay;
    int lsd2_okay;
    int irrecoverable_errors;
    char hex_data[16][6];
    char hex_parity[8][6];
    AnalogSignal analog_signal_array[16 * (3 + 2) + 8 * (3 + 2)];
    int analog_signal_index;
    int status_count;
} Ldu2Frame;

static void
ldu2_refresh_hold_hysteresis(const dsd_opts* opts, dsd_state* state) {
    time_t now = time(NULL);
    double hold_hyst = opts->trunk_hangtime * 0.75;
    if (hold_hyst < 0.75) {
        hold_hyst = 0.75;
    }
    if (state->last_vc_sync_time != 0 && (double)(now - state->last_vc_sync_time) <= hold_hyst) {
        state->last_vc_sync_time = now;
        state->last_vc_sync_time_m = dsd_time_now_monotonic_s();
    }
}

static void
ldu2_process_imbe_frame(dsd_opts* opts, dsd_state* state, int* status_count, char debug_prefix, int emit_active) {
#ifdef TRACE_DSD
    state->debug_prefix_2 = debug_prefix;
#else
    UNUSED(debug_prefix);
#endif
    process_IMBE(opts, state, status_count);
    p25p1_play_imbe_audio(opts, state);
    if (emit_active != 0) {
        p25_sm_emit_active(opts, state, 0);
    }
}

static void
ldu2_read_hex_word_block(dsd_opts* opts, dsd_state* state, char words[][6], int start_index, int word_count,
                         Ldu2Frame* frame, int sequence_break_word) {
    for (int i = 0; i < word_count; i++) {
        read_and_correct_hex_word(opts, state, &(words[start_index - i][0]), &frame->status_count,
                                  frame->analog_signal_array, &frame->analog_signal_index);
    }
    if (sequence_break_word >= 0) {
        size_t idx = (size_t)sequence_break_word * ((size_t)3 + 2);
        frame->analog_signal_array[idx].sequence_broken = 1;
    }
}

static void
ldu2_extract_ess_fields(const char hex_data[16][6], uint8_t mi[73], char algid[9], char kid[17]) {
    size_t mi_pos = 0;
    for (int row = 15; row >= 4; row--) {
        for (int bit = 0; bit < 6; bit++) {
            mi[mi_pos++] = (uint8_t)(hex_data[row][bit] + '0');
        }
    }
    mi[72] = 0;

    size_t alg_pos = 0;
    for (int bit = 0; bit < 6; bit++) {
        algid[alg_pos++] = (char)(hex_data[3][bit] + '0');
    }
    algid[alg_pos++] = (char)(hex_data[2][0] + '0');
    algid[alg_pos++] = (char)(hex_data[2][1] + '0');
    algid[8] = 0;

    size_t kid_pos = 0;
    for (int bit = 2; bit < 6; bit++) {
        kid[kid_pos++] = (char)(hex_data[2][bit] + '0');
    }
    for (int row = 1; row >= 0; row--) {
        for (int bit = 0; bit < 6; bit++) {
            kid[kid_pos++] = (char)(hex_data[row][bit] + '0');
        }
    }
    kid[16] = 0;
}

static void
ldu2_maybe_apply_early_unmute(dsd_opts* opts, const dsd_state* state, const char hex_data[16][6]) {
    if (state->payload_algid != 0) {
        return;
    }

    uint8_t mi_bits[73];
    char algid_bits[9];
    char kid_bits[17];
    ldu2_extract_ess_fields(hex_data, mi_bits, algid_bits, kid_bits);
    int algid_early = (int)strtol(algid_bits, NULL, 2);

    if (state->R != 0 && (algid_early == 0xAA || algid_early == 0x81 || algid_early == 0x9F)) {
        opts->unmute_encrypted_p25 = 1;
        return;
    }
    if (algid_early == 0x84 || algid_early == 0x89) {
        return;
    }
    if (algid_early != 0 && algid_early != 0x80) {
        opts->unmute_encrypted_p25 = 0;
    }
}

static void
ldu2_read_soft_octet(dsd_opts* opts, dsd_state* state, int* status_count, char bits[8], int16_t llr_bits[8]) {
    for (int i = 0; i <= 6; i += 2) {
        int16_t llr[2];
        read_dibit_soft(opts, state, bits + i, status_count, NULL, NULL, NULL, llr);
        llr_bits[i + 0] = llr[0];
        llr_bits[i + 1] = llr[1];
    }
}

static void
ldu2_capture_lsd(dsd_opts* opts, dsd_state* state, Ldu2Frame* frame) {
    char lsd[8];
    char cyclic_parity[8];

    ldu2_read_soft_octet(opts, state, &frame->status_count, lsd, frame->lowspeed_llr + 0);
    ldu2_read_soft_octet(opts, state, &frame->status_count, cyclic_parity, frame->lowspeed_llr + 8);
    frame->lsd_hex1 = 0;
    for (int i = 0; i < 8; i++) {
        frame->lsd_hex1 = (uint8_t)((frame->lsd_hex1 << 1) | (uint8_t)lsd[i]);
        frame->lsd1[i] = (char)(lsd[i] + '0');
        frame->lowspeeddata[i + 0] = (uint8_t)lsd[i];
        frame->lowspeeddata[i + 8] = (uint8_t)cyclic_parity[i];
    }

    ldu2_read_soft_octet(opts, state, &frame->status_count, lsd, frame->lowspeed_llr + 16);
    ldu2_read_soft_octet(opts, state, &frame->status_count, cyclic_parity, frame->lowspeed_llr + 24);
    frame->lsd_hex2 = 0;
    for (int i = 0; i < 8; i++) {
        frame->lsd_hex2 = (uint8_t)((frame->lsd_hex2 << 1) | (uint8_t)lsd[i]);
        frame->lsd2[i] = (char)(lsd[i] + '0');
        frame->lowspeeddata[i + 16] = (uint8_t)lsd[i];
        frame->lowspeeddata[i + 24] = (uint8_t)cyclic_parity[i];
    }
    frame->lsd1[8] = 0;
    frame->lsd2[8] = 0;

    state->dropL += 2;
    state->octet_counter += 2;
}

static void
ldu2_collect_voice_symbols(dsd_opts* opts, dsd_state* state, Ldu2Frame* frame) {
    static const char trace_prefix[9] = {'0', '1', '2', '3', '4', '5', '6', '7', '8'};

    frame->status_count = 21;
    frame->analog_signal_index = 0;
    state->p25vc = 9;

    for (int imbe = 0; imbe < 9; imbe++) {
        ldu2_process_imbe_frame(opts, state, &frame->status_count, trace_prefix[imbe], (imbe == 0));
        if (imbe >= 1 && imbe <= 4) {
            int start = 15 - ((imbe - 1) * 4);
            int break_word = (imbe - 1) * 4;
            ldu2_read_hex_word_block(opts, state, frame->hex_data, start, 4, frame, break_word);
            if (imbe == 4) {
                ldu2_maybe_apply_early_unmute(opts, state, (const char (*)[6])frame->hex_data);
            }
            continue;
        }
        if (imbe == 5) {
            ldu2_read_hex_word_block(opts, state, frame->hex_parity, 7, 4, frame, 16);
            continue;
        }
        if (imbe == 6) {
            ldu2_read_hex_word_block(opts, state, frame->hex_parity, 3, 4, frame, 20);
            continue;
        }
        if (imbe == 7) {
            ldu2_capture_lsd(opts, state, frame);
        }
    }

    state->p25vc = 0;
    state->dropL = 267;
    state->octet_counter = 0;

    if (opts->errorbars == 1) {
        DSD_FPRINTF(stderr, "\n");
    }
    if (opts->p25status == 1) {
        DSD_FPRINTF(stderr, "lsd1: %s lsd2: %s\n", frame->lsd1, frame->lsd2);
    }
}

static void
ldu2_consume_trailing_status(dsd_opts* opts, dsd_state* state) {
    dsd_dibit_soft_t status_soft;
    int ss = getDibitSoft(opts, state, &status_soft);
    p25_status_accum_add(state, ss);
    p25_status_accum_classify(state, opts);
}

static int
ldu2_run_fec_and_heuristics(dsd_state* state, P25Heuristics* heur, char hex_data[16][6], char hex_parity[8][6],
                            AnalogSignal analog_signal_array[16 * (3 + 2) + 8 * (3 + 2)]) {
    int irrecoverable_errors = check_and_fix_reedsolomon_24_16_9((char*)hex_data, (char*)hex_parity);
    if (irrecoverable_errors == 1) {
        uint8_t data_reliab[16];
        uint8_t parity_reliab[8];
        build_ldu2_rs_reliability(analog_signal_array, data_reliab, parity_reliab);
        if (p25p1_rs_24_16_9_soft_reliability((char*)hex_data, (char*)hex_parity, data_reliab, parity_reliab) == 0) {
            state->p25_p1_soft_rs_ok++;
            irrecoverable_errors = 0;
        }
    }
    if (irrecoverable_errors == 1) {
        state->p25_p1_voice_fec_err++;
        state->debug_header_critical_errors++;
        update_error_stats(heur, 12 * 6 + 12 * 6, 5 * 2);
        return 1;
    }

    state->p25_p1_voice_fec_ok++;
    state->last_vc_sync_time = time(NULL);
    state->last_vc_sync_time_m = dsd_time_now_monotonic_s();

    char fixed_parity[8 * 6];
    correct_hamming_dibits((char*)hex_data, 16, analog_signal_array);
    encode_reedsolomon_24_16_9((char*)hex_data, fixed_parity);
    correct_hamming_dibits(fixed_parity, 8, analog_signal_array + ((size_t)16) * (3 + 2));
    contribute_to_heuristics(state->rf_mod, heur, analog_signal_array, 16 * (3 + 2) + 8 * (3 + 2));
    return 0;
}

static void
ldu2_decode_post_fec_fields(const dsd_state* state, Ldu2Frame* frame) {
    ldu2_extract_ess_fields((const char (*)[6])frame->hex_data, frame->mi, frame->algid, frame->kid);
    frame->algidhex = (int)strtol(frame->algid, NULL, 2);
    frame->kidhex = (int)strtol(frame->kid, NULL, 2);
    frame->mihex1 = (unsigned long long)ConvertBitIntoBytes(&frame->mi[0], 32);
    frame->mihex2 = (unsigned long long)ConvertBitIntoBytes(&frame->mi[32], 32);
    frame->mihex3 = (unsigned long long)ConvertBitIntoBytes(&frame->mi[64], 8);

    frame->lsd1_okay = p25_lsd_fec_16x8_soft(frame->lowspeeddata + 0, frame->lowspeed_llr + 0);
    frame->lsd2_okay = p25_lsd_fec_16x8_soft(frame->lowspeeddata + 16, frame->lowspeed_llr + 16);
    frame->lsd_hex1 = p25p1_lsd_corrected_byte(frame->lowspeeddata + 0, frame->lsd1);
    frame->lsd_hex2 = p25p1_lsd_corrected_byte(frame->lowspeeddata + 16, frame->lsd2);

    if (state->payload_algid != 0x80) {
        frame->lsd_hex1 = 0;
        frame->lsd_hex2 = 0;
    }
}

static void
ldu2_apply_unmute_policy(dsd_opts* opts, const dsd_state* state) {
    if (state->R != 0
        && (state->payload_algid == 0xAA || state->payload_algid == 0x81 || state->payload_algid == 0x9F)) {
        DSD_FPRINTF(stderr, " Key: %s", DSD_SECRET_REDACTED);
        opts->unmute_encrypted_p25 = 1;
        return;
    }
    if ((state->payload_algid == 0x84 || state->payload_algid == 0x89) && state->aes_key_loaded[0] == 1) {
        DSD_FPRINTF(stderr, "\n ");
        DSD_FPRINTF(stderr, "%s", KYEL);
        DSD_FPRINTF(stderr, "Key: %s ", DSD_SECRET_REDACTED);
        DSD_FPRINTF(stderr, "%s ", KNRM);
        opts->unmute_encrypted_p25 = 1;
        return;
    }
    if (state->payload_algid != 0 && state->payload_algid != 0x80) {
        opts->unmute_encrypted_p25 = 0;
    }
}

static void
ldu2_print_decode_result(dsd_opts* opts, dsd_state* state, const Ldu2Frame* frame) {
    if (frame->irrecoverable_errors != 0) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " LDU2 FEC ERR ");
        DSD_FPRINTF(stderr, "%s", KNRM);
        return;
    }

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " LDU2 ALG ID: 0x%02X KEY ID: 0x%04X MI: 0x%08llX%08llX", frame->algidhex, frame->kidhex,
                frame->mihex1, frame->mihex2);
    state->payload_algid = frame->algidhex;
    state->payload_keyid = frame->kidhex;
    if (frame->mihex3 != 0ULL) {
        DSD_FPRINTF(stderr, "-%02llX", frame->mihex3);
    }

    ldu2_apply_unmute_policy(opts, state);
    DSD_FPRINTF(stderr, "%s", KNRM);

    state->payload_miP = (frame->mihex1 << 32) | frame->mihex2;
    if (state->payload_algid != 0x80 && state->payload_algid != 0) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " ENC");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

static void
ldu2_print_payload_lsd(const dsd_opts* opts, const Ldu2Frame* frame) {
    if (opts->payload != 1) {
        return;
    }
    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, "    LSD: %02X %02X ", frame->lsd_hex1, frame->lsd_hex2);
    if ((frame->lsd_hex1 > 0x19) && (frame->lsd_hex1 < 0x7F) && (frame->lsd1_okay == 1)) {
        DSD_FPRINTF(stderr, "(%c", frame->lsd_hex1);
    } else {
        DSD_FPRINTF(stderr, "( ");
    }
    if ((frame->lsd_hex2 > 0x19) && (frame->lsd_hex2 < 0x7F) && (frame->lsd2_okay == 1)) {
        DSD_FPRINTF(stderr, "%c)", frame->lsd_hex2);
    } else {
        DSD_FPRINTF(stderr, " )");
    }
    if (frame->lsd1_okay == 0) {
        DSD_FPRINTF(stderr, " L1 ERR");
    }
    if (frame->lsd2_okay == 0) {
        DSD_FPRINTF(stderr, " L2 ERR");
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
ldu2_store_lsd_alias_chars(dsd_state* state, const Ldu2Frame* frame) {
    if (state->dmr_alias_format[0] != 0x02) {
        return;
    }

    int k = state->data_block_counter[0];
    if ((frame->lsd_hex1 > 0x19) && (frame->lsd_hex1 < 0x7F) && (frame->lsd1_okay == 1)) {
        state->dmr_alias_block_segment[0][0][k / 4][k % 4] = (char)frame->lsd_hex1;
    }
    k++;
    if ((frame->lsd_hex2 > 0x19) && (frame->lsd_hex2 < 0x7F) && (frame->lsd2_okay == 1)) {
        state->dmr_alias_block_segment[0][0][k / 4][k % 4] = (char)frame->lsd_hex2;
    }
    k++;
    state->data_block_counter[0] = k;
}

static void
ldu2_maybe_begin_lsd_alias(dsd_state* state, const Ldu2Frame* frame) {
    if (frame->lsd_hex1 != 0x02 || frame->lsd1_okay != 1 || frame->lsd2_okay != 1) {
        return;
    }
    uint8_t block_len = frame->lsd_hex2;
    if (block_len > 8U) {
        block_len = 8U;
    }
    state->dmr_alias_format[0] = 0x02;
    state->dmr_alias_block_len[0] = block_len;
    state->data_block_counter[0] = 0;
}

static void
ldu2_maybe_finalize_lsd_alias(const dsd_opts* opts, dsd_state* state) {
    int k = state->data_block_counter[0];
    if (state->dmr_alias_format[0] != 0x02 || k < state->dmr_alias_block_len[0]) {
        return;
    }

    char str[16];
    int tsrc = state->lastsrc;
    int str_pos = 0;
    DSD_MEMSET(str, 0, sizeof(str));

    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, " LSD Soft ID: ");
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            char ch = state->dmr_alias_block_segment[0][0][i][j];
            DSD_FPRINTF(stderr, "%c", ch);
            if (ch != 0 && str_pos < (int)sizeof(str) - 1) {
                str[str_pos++] = ch;
            }
        }
    }

    if (tsrc != 0) {
        const char* mode = "D";
        dsd_tg_policy_entry alias_entry;
        if (state->payload_algid != 0x80 && state->payload_algid != 0 && opts->trunk_tune_enc_calls == 0
            && state->R == 0) {
            mode = "DE";
        }
        if (dsd_tg_policy_make_exact_entry((uint32_t)tsrc, mode, str, DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS, &alias_entry)
            == 0) {
            (void)dsd_tg_policy_upsert_exact(state, &alias_entry, DSD_TG_POLICY_UPSERT_REPLACE_LEARNED_ONLY);
            (void)dsd_tg_policy_upsert_exact(state, &alias_entry, DSD_TG_POLICY_UPSERT_ADD_IF_MISSING);
        }
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
    DSD_FPRINTF(stderr, "\n");
    state->dmr_alias_format[0] = 0;
    state->data_block_counter[0] = 0;
    state->dmr_alias_block_len[0] = 0;
}

static void
ldu2_handle_lsd_alias(const dsd_opts* opts, dsd_state* state, const Ldu2Frame* frame) {
    ldu2_store_lsd_alias_chars(state, frame);
    ldu2_maybe_begin_lsd_alias(state, frame);
    ldu2_maybe_finalize_lsd_alias(opts, state);
}

static void
ldu2_record_enc_lockout(dsd_opts* opts, dsd_state* state, int talkgroup) {
    if (talkgroup == 0) {
        return;
    }

    int enc_existing = 0;
    char lockout_name_buf[50];
    const char* lockout_name = "ENC LO";
    dsd_tg_policy_entry lockout_entry;
    if (dsd_tg_policy_lookup_label(state, (uint32_t)talkgroup, NULL, 0, lockout_name_buf, sizeof(lockout_name_buf))) {
        enc_existing = 1;
        lockout_name = lockout_name_buf;
    }

    if (dsd_tg_policy_make_exact_entry((uint32_t)talkgroup, "DE", lockout_name, DSD_TG_POLICY_SOURCE_ENC_LOCKOUT,
                                       &lockout_entry)
            != 0
        || dsd_tg_policy_upsert_exact(state, &lockout_entry, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) != 0
        || enc_existing != 0) {
        return;
    }

    DSD_SNPRINTF(state->event_history_s[0].Event_History_Items[0].internal_str,
                 sizeof state->event_history_s[0].Event_History_Items[0].internal_str,
                 "Target: %d; has been locked out; Encryption Lock Out Enabled.", talkgroup);
    dsd_p25_optional_hook_watchdog_event_current(opts, state, 0);
    Event_History_I* eh = &state->event_history_s[0];
    if (strncmp(eh->Event_History_Items[1].internal_str, eh->Event_History_Items[0].internal_str,
                sizeof eh->Event_History_Items[0].internal_str)
        != 0) {
        if (opts->event_out_file[0] != '\0') {
            dsd_p25_optional_hook_write_event_to_log_file(opts, state, 0, /*swrite*/ 0,
                                                          eh->Event_History_Items[0].event_string);
        }
        dsd_p25_optional_hook_push_event_history(eh);
        dsd_p25_optional_hook_init_event_history(eh, 0, 1);
    }
}

static void
ldu2_maybe_enc_lockout(dsd_opts* opts, dsd_state* state, int irrecoverable_errors) {
    if (irrecoverable_errors != 0 || state->payload_algid == 0x80 || state->payload_algid == 0) {
        return;
    }
    if (!(opts->p25_trunk == 1 && opts->p25_is_tuned == 1 && opts->trunk_tune_enc_calls == 0)) {
        return;
    }

    int enc_lo = 1;
    if (state->payload_algid == 0xAA && state->R != 0) {
        enc_lo = 0;
    }
    if (enc_lo == 0 || state->lasttg == 0) {
        return;
    }

    ldu2_record_enc_lockout(opts, state, state->lasttg);
    DSD_FPRINTF(stderr, " No Enc Following on P25p1 Trunking; Return to CC; \n");
    p25_sm_on_release(opts, state);
}

void
processLDU2(dsd_opts* opts, dsd_state* state) {
    state->p25_p1_duid_ldu2++;
    P25Heuristics* heur = (state->synctype == DSD_SYNC_P25P1_NEG) ? &state->inv_p25_heuristics : &state->p25_heuristics;

    ldu2_refresh_hold_hysteresis(opts, state);
    p25_status_accum_ensure_started(state);
    state->currentslot = 0;

    Ldu2Frame frame = {0};
    ldu2_collect_voice_symbols(opts, state, &frame);
    ldu2_consume_trailing_status(opts, state);
    frame.irrecoverable_errors =
        ldu2_run_fec_and_heuristics(state, heur, frame.hex_data, frame.hex_parity, frame.analog_signal_array);

#ifdef HEURISTICS_DEBUG
    DSD_FPRINTF(stderr, "(audio errors, header errors, critical header errors) (%i,%i,%i)\n", state->debug_audio_errors,
                state->debug_header_errors, state->debug_header_critical_errors);
#endif

    ldu2_decode_post_fec_fields(state, &frame);
    ldu2_print_decode_result(opts, state, &frame);
    ldu2_print_payload_lsd(opts, &frame);
    DSD_FPRINTF(stderr, "\n");

    ldu2_handle_lsd_alias(opts, state, &frame);

    if (frame.irrecoverable_errors != 0 && state->payload_algid != 0x80 && state->payload_keyid != 0
        && state->payload_miP != 0) {
        LFSRP(state);
        DSD_FPRINTF(stderr, "\n");
    }

    if (state->payload_algid == 0x84 || state->payload_algid == 0x89) {
        LFSR128(state);
        DSD_FPRINTF(stderr, "\n");
    }

    state->xl_is_hdu = 0;
    ldu2_maybe_enc_lockout(opts, state, frame.irrecoverable_errors);
}

//LFSR code courtesy of https://github.com/mattames/LFSR/
void
LFSRP(dsd_state* state) {
    //rework for P2 TDMA support
    unsigned long long int lfsr = 0;
    if (state->currentslot == 0) {
        lfsr = state->payload_miP;
    }

    if (state->currentslot == 1) {
        lfsr = state->payload_miN;
    }

    int cnt = 0;
    for (cnt = 0; cnt < 64; cnt++) {
        // Polynomial is C(x) = x^64 + x^62 + x^46 + x^38 + x^27 + x^15 + 1
        unsigned long long int bit =
            ((lfsr >> 63) ^ (lfsr >> 61) ^ (lfsr >> 45) ^ (lfsr >> 37) ^ (lfsr >> 26) ^ (lfsr >> 14)) & 0x1;
        lfsr = (lfsr << 1) | (bit);
    }

    if (state->currentslot == 0) {
        state->payload_miP = lfsr;
    }

    if (state->currentslot == 1) {
        state->payload_miN = lfsr;
    }

    //print current ENC identifiers already known and new calculated MI
    DSD_FPRINTF(stderr, "%s", KYEL);
    if (state->currentslot == 0) {
        DSD_FPRINTF(stderr, "\n LDU2/ESS_B FEC ERR - ALG: 0x%02X KEY ID: 0x%04X LFSR MI: 0x%016llX",
                    state->payload_algid, state->payload_keyid, state->payload_miP);
    }
    if (state->currentslot == 1) {
        DSD_FPRINTF(stderr, "\n LDU2/ESS_B FEC ERR - ALG: 0x%02X KEY ID: 0x%04X LFSR MI: 0x%016llX",
                    state->payload_algidR, state->payload_keyidR, state->payload_miN);
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

void
LFSR128(dsd_state* state) {
    //generate a 128-bit IV from a 64-bit IV for AES blocks
    unsigned long long int lfsr = 0;

    int slot = state->currentslot;

    if (state->currentslot == 0) {
        lfsr = state->payload_miP;
    } else {
        lfsr = state->payload_miN;
    }

    //start packing aes_iv
    if (slot == 0) {
        state->aes_iv[0] = (lfsr >> 56) & 0xFF;
        state->aes_iv[1] = (lfsr >> 48) & 0xFF;
        state->aes_iv[2] = (lfsr >> 40) & 0xFF;
        state->aes_iv[3] = (lfsr >> 32) & 0xFF;
        state->aes_iv[4] = (lfsr >> 24) & 0xFF;
        state->aes_iv[5] = (lfsr >> 16) & 0xFF;
        state->aes_iv[6] = (lfsr >> 8) & 0xFF;
        state->aes_iv[7] = (lfsr >> 0) & 0xFF;
    }
    if (slot == 1) {
        state->aes_ivR[0] = (lfsr >> 56) & 0xFF;
        state->aes_ivR[1] = (lfsr >> 48) & 0xFF;
        state->aes_ivR[2] = (lfsr >> 40) & 0xFF;
        state->aes_ivR[3] = (lfsr >> 32) & 0xFF;
        state->aes_ivR[4] = (lfsr >> 24) & 0xFF;
        state->aes_ivR[5] = (lfsr >> 16) & 0xFF;
        state->aes_ivR[6] = (lfsr >> 8) & 0xFF;
        state->aes_ivR[7] = (lfsr >> 0) & 0xFF;
    }

    int cnt = 0;
    int x = 64;
    //polynomial P(x) = 1 + X15 + X27 + X38 + X46 + X62 + X64
    for (cnt = 0; cnt < 64; cnt++) {
        //63,61,45,37,27,14
        // Polynomial is C(x) = x^64 + x^62 + x^46 + x^38 + x^27 + x^15 + 1
        unsigned long long int bit =
            ((lfsr >> 63) ^ (lfsr >> 61) ^ (lfsr >> 45) ^ (lfsr >> 37) ^ (lfsr >> 26) ^ (lfsr >> 14)) & 0x1;
        lfsr = (lfsr << 1) | bit;

        // Continue packing aes_iv
        if (slot == 0) {
            state->aes_iv[x / 8] = (state->aes_iv[x / 8] << 1) + bit;
        }
        if (slot == 1) {
            state->aes_ivR[x / 8] = (state->aes_ivR[x / 8] << 1) + bit;
        }
        x++;
    }

    if (state->currentslot == 0) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        if (state->dmrburstL != 27) { // If not LDU2
            DSD_FPRINTF(stderr, "\n");
        }
        DSD_FPRINTF(stderr, "     ");
        DSD_FPRINTF(stderr, " ALG ID: 0x%02X KEY ID: 0x%04X MI(128): 0x", state->payload_algid, state->payload_keyid);
        for (x = 0; x < 16; x++) {
            DSD_FPRINTF(stderr, "%02X", state->aes_iv[x]);
        }
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    if (state->currentslot == 1) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        if (state->dmrburstL != 27) { // If not LDU2, shouldn't matter on P25p2 for the second slot
            DSD_FPRINTF(stderr, "\n");
        }
        DSD_FPRINTF(stderr, "     ");
        DSD_FPRINTF(stderr, " ALG ID: 0x%02X KEY ID: 0x%04X MI(128): 0x", state->payload_algidR, state->payload_keyidR);
        for (x = 0; x < 16; x++) {
            DSD_FPRINTF(stderr, "%02X", state->aes_ivR[x]);
        }
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}
