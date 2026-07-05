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
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/state.h>
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
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/secret_redaction.h"
#include "dsd-neo/core/state_fwd.h"

static void
build_ldu2_rs_reliability(const P25P1SoftDibit* soft_dibits, uint8_t data_reliab[16], uint8_t parity_reliab[8]) {
    for (int i = 0; i < 16; i++) {
        int soft_index = (15 - i) * (3 + 2);
        data_reliab[i] = p25p1_hamming_rs_symbol_reliability(soft_dibits + soft_index);
    }
    for (int i = 0; i < 8; i++) {
        int soft_index = (16 * (3 + 2)) + ((7 - i) * (3 + 2));
        parity_reliab[i] = p25p1_hamming_rs_symbol_reliability(soft_dibits + soft_index);
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
    P25P1SoftDibit soft_dibits[16 * (3 + 2) + 8 * (3 + 2)];
    int soft_dibit_index;
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
                         Ldu2Frame* frame) {
    for (int i = 0; i < word_count; i++) {
        read_and_correct_hex_word(opts, state, &(words[start_index - i][0]), &frame->status_count, frame->soft_dibits,
                                  &frame->soft_dibit_index);
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
    uint32_t algid_early_bits = 0;
    int algid_early = (dsd_parse_binary_u32_n(algid_bits, 8, &algid_early_bits) == 0) ? (int)algid_early_bits : 0;

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
        P25P1SoftDibit soft_dibit;
        read_dibit_soft(opts, state, bits + i, status_count, &soft_dibit);
        llr_bits[i + 0] = soft_dibit.llr[0];
        llr_bits[i + 1] = soft_dibit.llr[1];
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
    frame->soft_dibit_index = 0;
    state->p25vc = 9;

    for (int imbe = 0; imbe < 9; imbe++) {
        ldu2_process_imbe_frame(opts, state, &frame->status_count, trace_prefix[imbe], (imbe == 0));
        if (imbe >= 1 && imbe <= 4) {
            int start = 15 - ((imbe - 1) * 4);
            ldu2_read_hex_word_block(opts, state, frame->hex_data, start, 4, frame);
            if (imbe == 4) {
                ldu2_maybe_apply_early_unmute(opts, state, (const char (*)[6])frame->hex_data);
            }
            continue;
        }
        if (imbe == 5) {
            ldu2_read_hex_word_block(opts, state, frame->hex_parity, 7, 4, frame);
            continue;
        }
        if (imbe == 6) {
            ldu2_read_hex_word_block(opts, state, frame->hex_parity, 3, 4, frame);
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
ldu2_run_fec(dsd_state* state, char hex_data[16][6], char hex_parity[8][6],
             P25P1SoftDibit soft_dibits[16 * (3 + 2) + 8 * (3 + 2)]) {
    int irrecoverable_errors = check_and_fix_reedsolomon_24_16_9((char*)hex_data, (char*)hex_parity);
    if (irrecoverable_errors == 1) {
        uint8_t data_reliab[16];
        uint8_t parity_reliab[8];
        build_ldu2_rs_reliability(soft_dibits, data_reliab, parity_reliab);
        if (p25p1_rs_24_16_9_soft_reliability((char*)hex_data, (char*)hex_parity, data_reliab, parity_reliab) == 0) {
            state->p25_p1_soft_rs_ok++;
            irrecoverable_errors = 0;
        }
    }
    if (irrecoverable_errors == 1) {
        state->p25_p1_voice_fec_err++;
        state->debug_header_critical_errors++;
        return 1;
    }

    state->p25_p1_voice_fec_ok++;
    state->last_vc_sync_time = time(NULL);
    state->last_vc_sync_time_m = dsd_time_now_monotonic_s();

    return 0;
}

static void
ldu2_decode_post_fec_fields(const dsd_state* state, Ldu2Frame* frame) {
    ldu2_extract_ess_fields((const char (*)[6])frame->hex_data, frame->mi, frame->algid, frame->kid);
    uint32_t algidhex = 0;
    uint32_t kidhex = 0;
    frame->algidhex = (dsd_parse_binary_u32_n(frame->algid, 8, &algidhex) == 0) ? (int)algidhex : 0;
    frame->kidhex = (dsd_parse_binary_u32_n(frame->kid, 16, &kidhex) == 0) ? (int)kidhex : 0;
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
        const unsigned int key_width = (state->payload_algid == 0xAA) ? 10U : 16U;
        char key_text[17];
        DSD_FPRINTF(stderr, " Key: %s",
                    dsd_secret_format_hex(key_text, sizeof key_text, opts->show_keys, state->R, key_width, 0));
        opts->unmute_encrypted_p25 = 1;
        return;
    }
    if ((state->payload_algid == 0x84 || state->payload_algid == 0x89) && state->aes_key_loaded[0] == 1) {
        DSD_FPRINTF(stderr, "\n ");
        DSD_FPRINTF(stderr, "%s", KYEL);
        const unsigned long long segments[4] = {state->A1[0], state->A2[0], state->A3[0], state->A4[0]};
        char key_text[68];
        DSD_FPRINTF(stderr, "Key: %s ",
                    dsd_secret_format_u64_segments(key_text, sizeof key_text, opts->show_keys, segments,
                                                   (state->payload_algid == 0x84) ? 4U : 2U));
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
        if (state->payload_algid != 0x80 && opts->trunk_tune_enc_calls == 0 && state->R == 0) {
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

    p25_sm_note_encrypted_call(opts, state, talkgroup);

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

static int
ldu2_payload_has_decrypt_key(const dsd_state* state) {
    int alg = state->payload_algid;
    if ((alg == 0xAA || alg == 0x81 || alg == 0x9F) && state->R != 0) {
        return 1;
    }
    if ((alg == 0x84 || alg == 0x89) && state->aes_key_loaded[0] == 1) {
        return 1;
    }
    return 0;
}

static void
ldu2_maybe_enc_lockout(dsd_opts* opts, dsd_state* state, int irrecoverable_errors) {
    if (irrecoverable_errors != 0 || state->payload_algid == 0x80 || state->payload_algid == 0) {
        return;
    }
    if (!(opts->p25_trunk == 1 && opts->p25_is_tuned == 1 && opts->trunk_tune_enc_calls == 0)) {
        return;
    }

    if (ldu2_payload_has_decrypt_key(state) || state->lasttg == 0) {
        return;
    }

    ldu2_record_enc_lockout(opts, state, state->lasttg);
    DSD_FPRINTF(stderr, " No Enc Following on P25p1 Trunking; Return to CC; \n");
    state->p25_sm_force_release = 1;
    p25_sm_on_release(opts, state);
}

void
processLDU2(dsd_opts* opts, dsd_state* state) {
    state->p25_p1_duid_ldu2++;

    ldu2_refresh_hold_hysteresis(opts, state);
    p25_status_accum_ensure_started(state);
    state->currentslot = 0;

    Ldu2Frame frame = {0};
    ldu2_collect_voice_symbols(opts, state, &frame);
    ldu2_consume_trailing_status(opts, state);
    frame.irrecoverable_errors = ldu2_run_fec(state, frame.hex_data, frame.hex_parity, frame.soft_dibits);

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
