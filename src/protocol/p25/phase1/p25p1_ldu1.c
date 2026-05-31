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
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_lsd.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25p1_check_ldu.h>
#include <dsd-neo/protocol/p25/p25p1_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_ldu.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <dsd-neo/runtime/colors.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/dsp/p25p1_heuristics.h"

#ifdef SOFTID
#include <dsd-neo/core/talkgroup_policy.h>
#endif

static void
build_ldu1_rs_reliability(const AnalogSignal* analog_signal_array, uint8_t data_reliab[12], uint8_t parity_reliab[12]) {
    for (int i = 0; i < 12; i++) {
        int analog_index = (11 - i) * (3 + 2);
        data_reliab[i] = p25p1_hamming_rs_symbol_reliability(analog_signal_array + analog_index);
    }
    for (int i = 0; i < 12; i++) {
        int analog_index = (12 * (3 + 2)) + ((11 - i) * (3 + 2));
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
    char hex_data[12][6];
    char hex_parity[12][6];
    int status_count;
    uint8_t lowspeeddata[32];
    int16_t lowspeed_llr[32];
    char lsd1[9];
    char lsd2[9];
    uint8_t lsd_hex1;
    uint8_t lsd_hex2;
    int lsd1_okay;
    int lsd2_okay;
    AnalogSignal analog_signal_array[12 * (3 + 2) + 12 * (3 + 2)];
    int analog_signal_index;
} ldu1_decode_ctx_t;

static void
p25p1_ldu1_refresh_vc_hysteresis(const dsd_opts* opts, dsd_state* state) {
    // Hysteresis: if we have very recent activity within a fraction of the
    // hangtime window, refresh the timer early to avoid thrashing between the
    // VC and CC on marginal signals.
    time_t now = time(NULL);
    double hold_hyst = opts->trunk_hangtime * 0.75;
    if (hold_hyst < 0.75) {
        hold_hyst = 0.75; // minimum grace window
    }
    if (state->last_vc_sync_time != 0 && (double)(now - state->last_vc_sync_time) <= hold_hyst) {
        state->last_vc_sync_time = now;
        state->last_vc_sync_time_m = dsd_time_now_monotonic_s();
    }
}

static void
p25p1_ldu1_init_decode_ctx(dsd_state* state, ldu1_decode_ctx_t* ctx) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    // We skip status dibits every 36 symbols; first IMBE starts 14 symbols
    // before next status, so counter starts at 36-14-1 = 21.
    ctx->status_count = 21;

    // Rotating MIs are only in LDU2; keep this clear for LDU1.
    state->p25vc = 0;
}

static void
p25p1_ldu1_process_imbe_frame(dsd_opts* opts, dsd_state* state, int* status_count, char trace_digit, int emit_active) {
#ifdef TRACE_DSD
    state->debug_prefix_2 = trace_digit;
#else
    UNUSED(trace_digit);
#endif
    process_IMBE(opts, state, status_count);
    if (emit_active) {
        // SM event: ACTIVE (P1 uses slot 0).
        p25_sm_emit_active(opts, state, 0);
    }
    p25p1_play_imbe_audio(opts, state);
}

static void
p25p1_ldu1_read_hex_block(dsd_opts* opts, dsd_state* state, char words[12][6], int start_word, int* status_count,
                          AnalogSignal analog_signal_array[], int* analog_signal_index, int sequence_break_word) {
    for (int w = start_word; w > (start_word - 4); w--) {
        read_and_correct_hex_word(opts, state, &(words[w][0]), status_count, analog_signal_array, analog_signal_index);
    }
    analog_signal_array[(size_t)sequence_break_word * (3 + 2)].sequence_broken = 1;
}

static uint8_t
p25p1_ldu1_read_lsd_codeword(dsd_opts* opts, dsd_state* state, int* status_count, uint8_t lowspeeddata[32],
                             int16_t lowspeed_llr[32], int data_offset, int llr_offset, char out_bits[9]) {
    char lsd[8];
    char cyclic_parity[8];

    for (int i = 0; i <= 6; i += 2) {
        int16_t llr[2];
        read_dibit_soft(opts, state, lsd + i, status_count, NULL, NULL, NULL, llr);
        lowspeed_llr[llr_offset + i + 0] = llr[0];
        lowspeed_llr[llr_offset + i + 1] = llr[1];
    }
    for (int i = 0; i <= 6; i += 2) {
        int16_t llr[2];
        read_dibit_soft(opts, state, cyclic_parity + i, status_count, NULL, NULL, NULL, llr);
        lowspeed_llr[llr_offset + 8 + i + 0] = llr[0];
        lowspeed_llr[llr_offset + 8 + i + 1] = llr[1];
    }

    uint8_t lsd_hex = 0;
    for (int i = 0; i < 8; i++) {
        lsd_hex = (uint8_t)(lsd_hex << 1);
        out_bits[i] = (char)(lsd[i] + '0');
        lsd_hex |= (uint8_t)lsd[i];
        lowspeeddata[data_offset + i] = (uint8_t)lsd[i];
        lowspeeddata[data_offset + 8 + i] = (uint8_t)cyclic_parity[i];
    }
    out_bits[8] = 0;
    return lsd_hex;
}

static void
p25p1_ldu1_collect_lsd(dsd_opts* opts, dsd_state* state, ldu1_decode_ctx_t* ctx) {
    ctx->lsd_hex1 = p25p1_ldu1_read_lsd_codeword(opts, state, &ctx->status_count, ctx->lowspeeddata, ctx->lowspeed_llr,
                                                 0, 0, ctx->lsd1);
    ctx->lsd_hex2 = p25p1_ldu1_read_lsd_codeword(opts, state, &ctx->status_count, ctx->lowspeeddata, ctx->lowspeed_llr,
                                                 16, 16, ctx->lsd2);

    // Need to skip two octets for the LSD bytes.
    state->dropL += 2;
    state->octet_counter += 2;
}

static void
p25p1_ldu1_collect_voice_and_data(dsd_opts* opts, dsd_state* state, ldu1_decode_ctx_t* ctx) {
    p25p1_ldu1_process_imbe_frame(opts, state, &ctx->status_count, '0', 1);
    p25p1_ldu1_process_imbe_frame(opts, state, &ctx->status_count, '1', 0);
    p25p1_ldu1_read_hex_block(opts, state, ctx->hex_data, 11, &ctx->status_count, ctx->analog_signal_array,
                              &ctx->analog_signal_index, 0);

    p25p1_ldu1_process_imbe_frame(opts, state, &ctx->status_count, '2', 0);
    p25p1_ldu1_read_hex_block(opts, state, ctx->hex_data, 7, &ctx->status_count, ctx->analog_signal_array,
                              &ctx->analog_signal_index, 4);

    p25p1_ldu1_process_imbe_frame(opts, state, &ctx->status_count, '3', 0);
    p25p1_ldu1_read_hex_block(opts, state, ctx->hex_data, 3, &ctx->status_count, ctx->analog_signal_array,
                              &ctx->analog_signal_index, 8);

    p25p1_ldu1_process_imbe_frame(opts, state, &ctx->status_count, '4', 0);
    p25p1_ldu1_read_hex_block(opts, state, ctx->hex_parity, 11, &ctx->status_count, ctx->analog_signal_array,
                              &ctx->analog_signal_index, 12);

    p25p1_ldu1_process_imbe_frame(opts, state, &ctx->status_count, '5', 0);
    p25p1_ldu1_read_hex_block(opts, state, ctx->hex_parity, 7, &ctx->status_count, ctx->analog_signal_array,
                              &ctx->analog_signal_index, 16);

    p25p1_ldu1_process_imbe_frame(opts, state, &ctx->status_count, '6', 0);
    p25p1_ldu1_read_hex_block(opts, state, ctx->hex_parity, 3, &ctx->status_count, ctx->analog_signal_array,
                              &ctx->analog_signal_index, 20);

    p25p1_ldu1_process_imbe_frame(opts, state, &ctx->status_count, '7', 0);
    p25p1_ldu1_collect_lsd(opts, state, ctx);

    p25p1_ldu1_process_imbe_frame(opts, state, &ctx->status_count, '8', 0);
}

static void
p25p1_ldu1_finalize_status(dsd_opts* opts, dsd_state* state) {
    // Trailing status symbol: feed to accumulator for advisory source classification.
    dsd_dibit_soft_t status_soft;
    int ss = getDibitSoft(opts, state, &status_soft);
    p25_status_accum_add(state, ss);

    // Classify accumulated status symbols and set advisory AFC gate flag.
    p25_status_accum_classify(state, opts);
}

static int
p25p1_ldu1_apply_rs_and_heuristics(dsd_state* state, P25Heuristics* heur, char hex_data[12][6], char hex_parity[12][6],
                                   AnalogSignal analog_signal_array[]) {
    int irrecoverable_errors = check_and_fix_reedsolomon_24_12_13((char*)hex_data, (char*)hex_parity);
    if (irrecoverable_errors == 1) {
        uint8_t data_reliab[12];
        uint8_t parity_reliab[12];
        build_ldu1_rs_reliability(analog_signal_array, data_reliab, parity_reliab);
        if (p25p1_rs_24_12_13_soft_reliability((char*)hex_data, (char*)hex_parity, data_reliab, parity_reliab) == 0) {
            state->p25_p1_soft_rs_ok++;
            irrecoverable_errors = 0;
        }
    }

    if (irrecoverable_errors == 1) {
        state->p25_p1_voice_fec_err++;
        state->debug_header_critical_errors++;
        // We can correct (13-1)/2 = 6 errors. If we failed, there were more
        // than 6 word errors post-Hamming stage. Approximate as 7x2 bit errs.
        update_error_stats(heur, 12 * 6 + 12 * 6, 7 * 2);
        return irrecoverable_errors;
    }

    state->p25_p1_voice_fec_ok++;
    // Passed FEC checks: mark recent voice activity for trunk hangtime
    // tracking so we don't prematurely return to CC mid-call.
    state->last_vc_sync_time = time(NULL);
    state->last_vc_sync_time_m = dsd_time_now_monotonic_s();

    char fixed_parity[12 * 6];
    correct_hamming_dibits((char*)hex_data, 12, analog_signal_array);
    encode_reedsolomon_24_12_13((char*)hex_data, fixed_parity);
    ptrdiff_t hoff = (ptrdiff_t)12 * (3 + 2);
    correct_hamming_dibits(fixed_parity, 12, analog_signal_array + hoff);
    contribute_to_heuristics(state->rf_mod, heur, analog_signal_array, 12 * (3 + 2) + 12 * (3 + 2));
    return irrecoverable_errors;
}

static void
p25p1_ldu1_unpack_lc_fields(char hex_data[12][6], uint8_t lcformat[9], uint8_t mfid[9], uint8_t lcinfo[57]) {
    lcformat[8] = 0;
    mfid[8] = 0;
    lcinfo[56] = 0;

    for (int i = 0; i < 6; i++) {
        lcformat[i] = (uint8_t)(hex_data[11][i] + '0');
    }
    lcformat[6] = (uint8_t)(hex_data[10][0] + '0');
    lcformat[7] = (uint8_t)(hex_data[10][1] + '0');

    for (int i = 0; i < 4; i++) {
        mfid[i] = (uint8_t)(hex_data[10][i + 2] + '0');
        mfid[i + 4] = (uint8_t)(hex_data[9][i] + '0');
    }

    lcinfo[0] = (uint8_t)(hex_data[9][4] + '0');
    lcinfo[1] = (uint8_t)(hex_data[9][5] + '0');
    int idx = 2;
    for (int word = 8; word >= 0; word--) {
        for (int bit = 0; bit < 6; bit++) {
            lcinfo[idx++] = (uint8_t)(hex_data[word][bit] + '0');
        }
    }
}

static void
p25p1_ldu1_build_lcw_buffers(const uint8_t lcformat[9], const uint8_t mfid[9], const uint8_t lcinfo[57],
                             uint8_t LCW_bytes[9], uint8_t LCW_bits[72]) {
    DSD_MEMSET(LCW_bytes, 0, 9);
    DSD_MEMSET(LCW_bits, 0, 72);

    LCW_bytes[0] = (uint8_t)ConvertBitIntoBytes(&lcformat[0], 8);
    LCW_bytes[1] = (uint8_t)ConvertBitIntoBytes(&mfid[0], 8);
    for (int i = 0; i < 7; i++) {
        ptrdiff_t o = (ptrdiff_t)i * 8;
        LCW_bytes[i + 2] = (uint8_t)ConvertBitIntoBytes(&lcinfo[o], 8);
    }

    for (int i = 0, j = 0; i < 9; i++, j += 8) {
        LCW_bits[j + 0] = (LCW_bytes[i] >> 7) & 0x01;
        LCW_bits[j + 1] = (LCW_bytes[i] >> 6) & 0x01;
        LCW_bits[j + 2] = (LCW_bytes[i] >> 5) & 0x01;
        LCW_bits[j + 3] = (LCW_bytes[i] >> 4) & 0x01;
        LCW_bits[j + 4] = (LCW_bytes[i] >> 3) & 0x01;
        LCW_bits[j + 5] = (LCW_bytes[i] >> 2) & 0x01;
        LCW_bits[j + 6] = (LCW_bytes[i] >> 1) & 0x01;
        LCW_bits[j + 7] = (LCW_bytes[i] >> 0) & 0x01;
    }
}

static void
p25p1_ldu1_handle_lcw_output(dsd_opts* opts, dsd_state* state, uint8_t LCW_bits[72], int irrecoverable_errors) {
    if (irrecoverable_errors == 0) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        p25_lcw(opts, state, LCW_bits, 0);
        DSD_FPRINTF(stderr, "%s", KNRM);
        return;
    }
    DSD_FPRINTF(stderr, "%s", KRED);
    DSD_FPRINTF(stderr, " LCW FEC ERR ");
    DSD_FPRINTF(stderr, "%s\n", KNRM);
}

static void
p25p1_ldu1_correct_lsd(const dsd_state* state, ldu1_decode_ctx_t* ctx) {
    // LSD FEC (16,8) — correct single-bit errors in full codeword.
    ctx->lsd1_okay = p25_lsd_fec_16x8_soft(ctx->lowspeeddata + 0, ctx->lowspeed_llr + 0);
    ctx->lsd2_okay = p25_lsd_fec_16x8_soft(ctx->lowspeeddata + 16, ctx->lowspeed_llr + 16);
    ctx->lsd_hex1 = p25p1_lsd_corrected_byte(ctx->lowspeeddata + 0, ctx->lsd1);
    ctx->lsd_hex2 = p25p1_lsd_corrected_byte(ctx->lowspeeddata + 16, ctx->lsd2);

    // LSD is also encrypted when voice is encrypted.
    if (state->payload_algid != 0x80) {
        ctx->lsd_hex1 = 0;
        ctx->lsd_hex2 = 0;
    }
}

static void
p25p1_ldu1_print_payload(const dsd_opts* opts, const uint8_t LCW_bytes[9], const ldu1_decode_ctx_t* ctx) {
    if (opts->payload != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, " P25 LCW Payload ");
    for (int i = 0; i < 9; i++) {
        DSD_FPRINTF(stderr, "[%02X]", LCW_bytes[i]);
    }

    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, "    LSD: %02X %02X ", ctx->lsd_hex1, ctx->lsd_hex2);
    if ((ctx->lsd_hex1 > 0x19) && (ctx->lsd_hex1 < 0x7F) && (ctx->lsd1_okay == 1)) {
        DSD_FPRINTF(stderr, "(%c", ctx->lsd_hex1);
    } else {
        DSD_FPRINTF(stderr, "( ");
    }
    if ((ctx->lsd_hex2 > 0x19) && (ctx->lsd_hex2 < 0x7F) && (ctx->lsd2_okay == 1)) {
        DSD_FPRINTF(stderr, "%c)", ctx->lsd_hex2);
    } else {
        DSD_FPRINTF(stderr, " )");
    }
    if (ctx->lsd1_okay == 0) {
        DSD_FPRINTF(stderr, " L1 ERR");
    }
    if (ctx->lsd2_okay == 0) {
        DSD_FPRINTF(stderr, " L2 ERR");
    }
    DSD_FPRINTF(stderr, "%s\n", KNRM);
}

#ifdef SOFTID
static int
p25p1_ldu1_softid_append_segment(dsd_state* state, const ldu1_decode_ctx_t* ctx) {
    int k = 0;
    if (state->dmr_alias_format[0] != 0x02) {
        return k;
    }

    k = state->data_block_counter[0];
    if ((ctx->lsd_hex1 > 0x19) && (ctx->lsd_hex1 < 0x7F) && (ctx->lsd1_okay == 1)) {
        state->dmr_alias_block_segment[0][0][k / 4][k % 4] = (char)ctx->lsd_hex1;
    }
    k++;
    if ((ctx->lsd_hex2 > 0x19) && (ctx->lsd_hex2 < 0x7F) && (ctx->lsd2_okay == 1)) {
        state->dmr_alias_block_segment[0][0][k / 4][k % 4] = (char)ctx->lsd_hex2;
    }
    k++;
    state->data_block_counter[0] = k;
    return k;
}

static void
p25p1_ldu1_softid_reset_format(dsd_state* state, const ldu1_decode_ctx_t* ctx) {
    if (ctx->lsd_hex1 != 0x02 || ctx->lsd1_okay != 1 || ctx->lsd2_okay != 1) {
        return;
    }
    state->dmr_alias_format[0] = 0x02;
    uint8_t len = ctx->lsd_hex2;
    if (len > 8) {
        len = 8;
    }
    state->dmr_alias_block_len[0] = len;
    state->data_block_counter[0] = 0;
}

static void
p25p1_ldu1_softid_finalize_alias(const dsd_opts* opts, dsd_state* state, int k) {
    if (k < state->dmr_alias_block_len[0] || state->dmr_alias_format[0] != 0x02) {
        return;
    }

    char str[16];
    int tsrc = state->lastsrc;
    k = 0;
    for (int i = 0; i < 16; i++) {
        str[i] = 0;
    }

    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, " LSD Soft ID: ");
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            DSD_FPRINTF(stderr, "%c", state->dmr_alias_block_segment[0][0][i][j]);
            if (state->dmr_alias_block_segment[0][0][i][j] != 0) {
                str[k++] = state->dmr_alias_block_segment[0][0][i][j];
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
}

static void
p25p1_ldu1_handle_softid(const dsd_opts* opts, dsd_state* state, const ldu1_decode_ctx_t* ctx) {
    int k = p25p1_ldu1_softid_append_segment(state, ctx);
    p25p1_ldu1_softid_reset_format(state, ctx);
    p25p1_ldu1_softid_finalize_alias(opts, state, k);
}
#endif

void
processLDU1(dsd_opts* opts, dsd_state* state) {
    state->p25_p1_duid_ldu1++;
    P25Heuristics* heur = (state->synctype == DSD_SYNC_P25P1_NEG) ? &state->inv_p25_heuristics : &state->p25_heuristics;

    p25p1_ldu1_refresh_vc_hysteresis(opts, state);
    // Do not refresh last_vc_sync_time again until FEC passes below.

    // Start status-symbol collection unless dispatcher already did so.
    p25_status_accum_ensure_started(state);

    // Keep slot index sane when swapping protocol contexts.
    state->currentslot = 0;

    ldu1_decode_ctx_t ctx;
    p25p1_ldu1_init_decode_ctx(state, &ctx);

    p25p1_ldu1_collect_voice_and_data(opts, state, &ctx);
    if (opts->errorbars == 1) {
        DSD_FPRINTF(stderr, "\n");
    }
    if (opts->p25status == 1) {
        DSD_FPRINTF(stderr, "lsd1: %s lsd2: %s\n", ctx.lsd1, ctx.lsd2);
    }

    p25p1_ldu1_finalize_status(opts, state);
    int irrecoverable_errors =
        p25p1_ldu1_apply_rs_and_heuristics(state, heur, ctx.hex_data, ctx.hex_parity, ctx.analog_signal_array);

#ifdef HEURISTICS_DEBUG
    DSD_FPRINTF(stderr, "(audio errors, header errors, critical header errors) (%i,%i,%i)\n", state->debug_audio_errors,
                state->debug_header_errors, state->debug_header_critical_errors);
#endif

    uint8_t lcformat[9];
    uint8_t mfid[9];
    uint8_t lcinfo[57];
    uint8_t LCW_bytes[9];
    uint8_t LCW_bits[72];
    p25p1_ldu1_unpack_lc_fields(ctx.hex_data, lcformat, mfid, lcinfo);
    p25p1_ldu1_build_lcw_buffers(lcformat, mfid, lcinfo, LCW_bytes, LCW_bits);
    p25p1_ldu1_handle_lcw_output(opts, state, LCW_bits, irrecoverable_errors);

    p25p1_ldu1_correct_lsd(state, &ctx);
    p25p1_ldu1_print_payload(opts, LCW_bytes, &ctx);

#ifdef SOFTID
    p25p1_ldu1_handle_softid(opts, state, &ctx);
#else
    UNUSED2(ctx.lsd1_okay, ctx.lsd2_okay);
#endif // SOFTID
}
