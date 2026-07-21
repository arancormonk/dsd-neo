// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 * p25p1_mpdu.c
 * P25p1 Multi Block PDU Assembly
 *
 * LWVMOBILE
 * 2025-03 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/bit_packing.h>

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_12.h>
#include <dsd-neo/protocol/p25/p25_crc.h>
#include <dsd-neo/protocol/p25/p25_pdu.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25p1_mbf34.h>
#include <dsd-neo/protocol/p25/p25p1_pdu_trunking.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifdef USE_RADIO
#endif

static int16_t
saturating_llr_add(int acc, int value) {
    acc += value;
    if (acc > INT16_MAX) {
        return INT16_MAX;
    }
    if (acc < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)acc;
}

static uint32_t
crc32mbf(const uint8_t* buf, int len) {
    uint32_t g = 0x04c11db7;
    uint64_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc <<= 1;
        int b = (buf[i / 8] >> (7 - (i % 8))) & 1;
        if (((crc >> 32) ^ b) & 1) {
            crc ^= g;
        }
    }
    crc = (crc & 0xffffffff) ^ 0xffffffff;
    return crc;
}

enum {
    P25_MPDU_R12_BYTES = 12,
    P25_MPDU_R34_BYTES = 18,
    P25_MPDU_MAX_BLOCKS = 129,
    P25_MPDU_MAX_DATA_BLOCKS = 127,
    P25_MPDU_MAX_BYTES = P25_MPDU_R34_BYTES * P25_MPDU_MAX_BLOCKS,
    P25_MPDU_MAX_BITS = P25_MPDU_MAX_BYTES * 8,
    P25_MPDU_HEADER_REPS = 3,
    P25_MPDU_HEADER_BITS = 96,
    P25_MPDU_HEADER_LLR = 196,
    P25_MPDU_TSBK_DIBITS = 98,
};

typedef struct {
    uint8_t tsbk_dibit[P25_MPDU_TSBK_DIBITS];
    int16_t tsbk_llr[P25_MPDU_HEADER_LLR];
    uint8_t tsbk_byte[P25_MPDU_R12_BYTES];
    uint8_t r34byte_b[P25_MPDU_R34_BYTES];
    uint8_t r34bytes[P25_MPDU_MAX_BYTES];
    int tsbk_decoded_bits[P25_MPDU_HEADER_BITS];
    uint8_t hdr_rep_bits[P25_MPDU_HEADER_REPS][P25_MPDU_HEADER_BITS];
    int16_t hdr_rep_llr[P25_MPDU_HEADER_REPS][P25_MPDU_HEADER_LLR];
    uint8_t hdr_rep_bytes[P25_MPDU_HEADER_REPS][P25_MPDU_R12_BYTES];
    int hdr_rep_crc[P25_MPDU_HEADER_REPS];
    uint8_t mpdu_crc_bits[P25_MPDU_MAX_BITS];
    uint8_t mpdu_crc9_bits[P25_MPDU_MAX_BITS];
    uint8_t mpdu_byte[P25_MPDU_MAX_BYTES];
    int err[2];
    int r34;
    int end;
    int crc_bit_count;
    int crc9_bit_count;
    uint8_t io;
    uint8_t fmt;
    uint8_t sap;
    uint8_t blks;
} P25MpduContext;

static void
p25_mpdu_context_init(P25MpduContext* ctx) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->end = 3;
    for (int i = 0; i < P25_MPDU_HEADER_REPS; i++) {
        ctx->hdr_rep_crc[i] = -2;
    }
    ctx->err[0] = -2;
    ctx->err[1] = -2;
}

static void
p25_mpdu_prepare_state(dsd_opts* opts, dsd_state* state) {
    state->p25_p1_duid_mpdu++;
    state->voice_counter[0] = 0;
    state->voice_counter[1] = 0;
    DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
    DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
    opts->slot_preference = 2;
    state->currentslot = 0;

    p25_status_accum_ensure_started(state);
    DSD_SNPRINTF(state->call_string[0], sizeof(state->call_string[0]), "%s", "                     ");
    DSD_SNPRINTF(state->call_string[1], sizeof(state->call_string[1]), "%s", "                     ");

    (void)dsd_recent_activity_expire(state, 0U, DSD_RECENT_ACTIVITY_TTL_MS);
}

static void
p25_mpdu_bytes_to_int_bits(const uint8_t* bytes, int* bits, int byte_count) {
    int bit_index = 0;
    for (int byte_idx = 0; byte_idx < byte_count; byte_idx++) {
        for (int bit = 0; bit < 8; bit++) {
            bits[bit_index++] = ((bytes[byte_idx] << bit) & 0x80) >> 7;
        }
    }
}

static uint16_t
p25_mpdu_candidate_crc9(const uint8_t bytes[P25_MPDU_R34_BYTES]) {
    uint8_t crc9_bits[135];
    int bit_index = 0;
    for (int bit = 0; bit < 7; bit++) {
        crc9_bits[bit_index++] = ((bytes[0] << bit) & 0x80) >> 7;
    }
    for (int byte_idx = 2; byte_idx < P25_MPDU_R34_BYTES; byte_idx++) {
        for (int bit = 0; bit < 8; bit++) {
            crc9_bits[bit_index++] = ((bytes[byte_idx] << bit) & 0x80) >> 7;
        }
    }
    return ComputeCrc9Bit(crc9_bits, 135);
}

static int
p25_mpdu_select_mbf34_candidate(const p25_mbf34_candidate_t* candidates, int candidate_count) {
    for (int candidate_idx = 0; candidate_idx < candidate_count; candidate_idx++) {
        const uint8_t* bytes = candidates[candidate_idx].bytes;
        uint16_t crc9_ext = (uint16_t)(((bytes[0] & 1) << 8) | bytes[1]);
        if (p25_mpdu_candidate_crc9(bytes) == crc9_ext) {
            return candidate_idx;
        }
    }
    return 0;
}

static int
p25_mpdu_select_crc16_candidate(const p25_12_candidate_t* candidates, int candidate_count) {
    for (int candidate_idx = 0; candidate_idx < candidate_count; candidate_idx++) {
        int candidate_bits[P25_MPDU_HEADER_BITS];
        p25_mpdu_bytes_to_int_bits(candidates[candidate_idx].bytes, candidate_bits, P25_MPDU_R12_BYTES);
        if (crc16_lb_bridge(candidate_bits, 80) == 0) {
            return candidate_idx;
        }
    }
    return 0;
}

static void
p25_mpdu_read_repetition(dsd_opts* opts, dsd_state* state, P25MpduContext* ctx, int* skipdibit) {
    int dibit_count = 0;
    int out_idx = 0;
    for (int input_idx = 0; input_idx < 101; input_idx++) {
        dsd_dibit_soft_t soft;
        int dibit = getDibitSoft(opts, state, &soft);
        if ((*skipdibit / 36) == 0) {
            dibit_count++;
            ctx->tsbk_dibit[out_idx] = (uint8_t)dibit;
            ctx->tsbk_llr[(out_idx * 2) + 0] = soft.llr[0];
            ctx->tsbk_llr[(out_idx * 2) + 1] = soft.llr[1];
            out_idx++;
        } else {
            p25_status_accum_add(state, dibit);
            *skipdibit = 0;
        }

        (*skipdibit)++;
        if (dibit_count == P25_MPDU_TSBK_DIBITS) {
            break;
        }
    }
}

static void
p25_mpdu_decode_r34_block(P25MpduContext* ctx, int block_idx) {
    if (!ctx || block_idx < 1 || block_idx > P25_MPDU_MAX_BLOCKS) {
        return;
    }
    p25_mbf34_candidate_t candidates[P25_MBF34_MAX_CANDIDATES];
    int candidate_count =
        p25_mbf34_decode_soft_list(ctx->tsbk_dibit, ctx->tsbk_llr, candidates, P25_MBF34_MAX_CANDIDATES);
    if (candidate_count > 0) {
        int selected = p25_mpdu_select_mbf34_candidate(candidates, candidate_count);
        DSD_MEMCPY(ctx->r34byte_b, candidates[selected].bytes, sizeof(ctx->r34byte_b));
    } else {
        (void)p25_mbf34_decode_soft(ctx->tsbk_dibit, ctx->tsbk_llr, ctx->r34byte_b);
    }

    DSD_MEMCPY(ctx->r34bytes + ((size_t)(block_idx - 1) * P25_MPDU_R34_BYTES), ctx->r34byte_b, sizeof(ctx->r34byte_b));

    for (int byte_idx = 2; byte_idx < P25_MPDU_R34_BYTES; byte_idx++) {
        for (int bit = 0; bit < 8; bit++) {
            ctx->mpdu_crc_bits[ctx->crc_bit_count++] = ((ctx->r34byte_b[byte_idx] << bit) & 0x80) >> 7;
        }
    }

    for (int bit = 0; bit < 7; bit++) {
        ctx->mpdu_crc9_bits[ctx->crc9_bit_count++] = ((ctx->r34byte_b[0] << bit) & 0x80) >> 7;
    }
    for (int byte_idx = 2; byte_idx < P25_MPDU_R34_BYTES; byte_idx++) {
        for (int bit = 0; bit < 8; bit++) {
            ctx->mpdu_crc9_bits[ctx->crc9_bit_count++] = ((ctx->r34byte_b[byte_idx] << bit) & 0x80) >> 7;
        }
    }
}

static void
p25_mpdu_decode_r12_block(P25MpduContext* ctx, int block_idx) {
    p25_12_candidate_t candidates[P25_12_MAX_CANDIDATES];
    int candidate_count = p25_12_soft_llr_list(ctx->tsbk_dibit, ctx->tsbk_llr, candidates, P25_12_MAX_CANDIDATES);
    if (candidate_count > 0) {
        int selected = (block_idx == 0) ? p25_mpdu_select_crc16_candidate(candidates, candidate_count) : 0;
        DSD_MEMCPY(ctx->tsbk_byte, candidates[selected].bytes, sizeof(ctx->tsbk_byte));
    } else {
        (void)p25_12_soft_llr(ctx->tsbk_dibit, ctx->tsbk_llr, ctx->tsbk_byte);
    }
}

static void
p25_mpdu_unpack_tsbk_bytes(P25MpduContext* ctx) {
    p25_mpdu_bytes_to_int_bits(ctx->tsbk_byte, ctx->tsbk_decoded_bits, P25_MPDU_R12_BYTES);
}

static void
p25_mpdu_store_header_rep(P25MpduContext* ctx, int block_idx) {
    if (block_idx >= P25_MPDU_HEADER_REPS) {
        return;
    }
    for (int bit = 0; bit < P25_MPDU_HEADER_BITS; bit++) {
        ctx->hdr_rep_bits[block_idx][bit] = (uint8_t)(ctx->tsbk_decoded_bits[bit] & 1);
    }
    DSD_MEMCPY(ctx->hdr_rep_llr[block_idx], ctx->tsbk_llr, sizeof(ctx->tsbk_llr));
    DSD_MEMCPY(ctx->hdr_rep_bytes[block_idx], ctx->tsbk_byte, P25_MPDU_R12_BYTES);
    ctx->hdr_rep_crc[block_idx] = crc16_lb_bridge(ctx->tsbk_decoded_bits, 80);
}

static void
p25_mpdu_store_r12_block(P25MpduContext* ctx, int block_idx) {
    for (int byte_idx = 0; byte_idx < P25_MPDU_R12_BYTES; byte_idx++) {
        int byte = 0;
        for (int bit = 0; bit < 8; bit++) {
            byte = (byte << 1) | ctx->tsbk_decoded_bits[(byte_idx * 8) + bit];
        }
        ctx->tsbk_byte[byte_idx] = (uint8_t)byte;
        size_t mpdu_off = (size_t)byte_idx + ((size_t)block_idx * P25_MPDU_R12_BYTES);
        if (mpdu_off < sizeof(ctx->mpdu_byte)) {
            ctx->mpdu_byte[mpdu_off] = (uint8_t)byte;
        }
    }
}

static void
p25_mpdu_update_header_from_first_block(P25MpduContext* ctx, const dsd_opts* opts) {
    if (ctx->hdr_rep_crc[0] != 0 && opts->aggressive_framesync != 0) {
        return;
    }

    uint8_t an = (ctx->mpdu_byte[0] >> 6) & 0x1;
    ctx->io = (ctx->mpdu_byte[0] >> 5) & 0x1;
    ctx->fmt = ctx->mpdu_byte[0] & 0x1F;
    ctx->sap = ctx->mpdu_byte[1] & 0x3F;
    ctx->blks = ctx->mpdu_byte[6] & 0x7F;
    ctx->r34 = (an == 1 && ctx->fmt == 0x16) ? 1 : ctx->r34;
    ctx->end = ctx->blks + 1;

    if ((ctx->sap == 61 || ctx->sap == 63) && ctx->blks > 10) {
        ctx->end = 4;
    }
}

static void
p25_mpdu_collect_blocks(dsd_opts* opts, dsd_state* state, P25MpduContext* ctx) {
    int skipdibit = 36 - 14;
    for (int block_idx = 0; block_idx < ctx->end; block_idx++) {
        p25_mpdu_read_repetition(opts, state, ctx, &skipdibit);
        if (ctx->r34 && block_idx != 0) {
            p25_mpdu_decode_r34_block(ctx, block_idx);
        } else {
            p25_mpdu_decode_r12_block(ctx, block_idx);
        }

        p25_mpdu_unpack_tsbk_bytes(ctx);
        p25_mpdu_store_header_rep(ctx, block_idx);
        p25_mpdu_store_r12_block(ctx, block_idx);
        if (block_idx == 0) {
            p25_mpdu_update_header_from_first_block(ctx, opts);
        }
    }
}

static void
p25_mpdu_note_header_fec(dsd_state* state, int ok, int soft_combined) {
    if (ok) {
        state->p25_p1_fec_ok++;
        if (soft_combined) {
            state->p25_p1_soft_combined_ok++;
        }
#ifdef USE_RADIO
        dsd_rtl_stream_metrics_hook_p25p1_ber_update(1, 0);
#endif
    } else {
        state->p25_p1_fec_err++;
#ifdef USE_RADIO
        dsd_rtl_stream_metrics_hook_p25p1_ber_update(0, 1);
#endif
    }
}

static int
p25_mpdu_try_combined_header(P25MpduContext* ctx, int hdr_reps) {
    int16_t combined_llr[P25_MPDU_HEADER_LLR];
    DSD_MEMSET(combined_llr, 0, sizeof(combined_llr));
    for (int bit = 0; bit < P25_MPDU_HEADER_LLR; bit++) {
        int acc = 0;
        for (int rep = 0; rep < hdr_reps; rep++) {
            acc = saturating_llr_add(acc, ctx->hdr_rep_llr[rep][bit]);
        }
        combined_llr[bit] = (int16_t)acc;
    }

    p25_12_candidate_t candidates[P25_12_MAX_CANDIDATES];
    int candidate_count = p25_12_soft_llr_list(NULL, combined_llr, candidates, P25_12_MAX_CANDIDATES);
    for (int candidate_idx = 0; candidate_idx < candidate_count; candidate_idx++) {
        int candidate_bits[P25_MPDU_HEADER_BITS];
        p25_mpdu_bytes_to_int_bits(candidates[candidate_idx].bytes, candidate_bits, P25_MPDU_R12_BYTES);
        if (crc16_lb_bridge(candidate_bits, 80) == 0) {
            ctx->err[0] = 0;
            DSD_MEMCPY(ctx->mpdu_byte, candidates[candidate_idx].bytes, P25_MPDU_R12_BYTES);
            return 1;
        }
    }
    return 0;
}

static void
p25_mpdu_rebuild_header_from_majority(P25MpduContext* ctx, int hdr_reps) {
    uint8_t hdr_maj_bits[P25_MPDU_HEADER_BITS];
    DSD_MEMSET(hdr_maj_bits, 0, sizeof(hdr_maj_bits));
    int thresh = (hdr_reps >= 2) ? ((hdr_reps + 1) / 2) : 1;
    for (int bit = 0; bit < P25_MPDU_HEADER_BITS; bit++) {
        int sum = 0;
        for (int rep = 0; rep < hdr_reps; rep++) {
            sum += (int)ctx->hdr_rep_bits[rep][bit];
        }
        hdr_maj_bits[bit] = (uint8_t)((sum >= thresh) ? 1 : 0);
    }

    int hdr_bits_int[P25_MPDU_HEADER_BITS];
    for (int bit = 0; bit < P25_MPDU_HEADER_BITS; bit++) {
        hdr_bits_int[bit] = (int)hdr_maj_bits[bit];
    }
    ctx->err[0] = crc16_lb_bridge(hdr_bits_int, 80);

    for (int byte_idx = 0; byte_idx < P25_MPDU_R12_BYTES; byte_idx++) {
        int byte = 0;
        for (int bit = 0; bit < 8; bit++) {
            byte = (byte << 1) | (hdr_maj_bits[(byte_idx * 8) + bit] & 1);
        }
        ctx->mpdu_byte[byte_idx] = (uint8_t)byte;
    }
}

static void
p25_mpdu_finalize_header(dsd_state* state, P25MpduContext* ctx) {
    int reps = (ctx->end < P25_MPDU_HEADER_REPS) ? ctx->end : P25_MPDU_HEADER_REPS;
    int hdr_reps = (ctx->blks >= 1) ? 1 : reps;
    int selected_rep = -1;
    for (int rep = 0; rep < hdr_reps; rep++) {
        if (ctx->hdr_rep_crc[rep] == 0) {
            selected_rep = rep;
            break;
        }
    }

    if (selected_rep >= 0) {
        ctx->err[0] = 0;
        DSD_MEMCPY(ctx->mpdu_byte, ctx->hdr_rep_bytes[selected_rep], P25_MPDU_R12_BYTES);
        p25_mpdu_note_header_fec(state, 1, 0);
    } else if (hdr_reps > 1 && p25_mpdu_try_combined_header(ctx, hdr_reps)) {
        p25_mpdu_note_header_fec(state, 1, 1);
    }

    if (selected_rep < 0 && ctx->err[0] != 0) {
        p25_mpdu_rebuild_header_from_majority(ctx, hdr_reps);
        p25_mpdu_note_header_fec(state, ctx->err[0] == 0, 0);
    }
}

static void
p25_mpdu_decode_header_if_usable(dsd_opts* opts, dsd_state* state, P25MpduContext* ctx) {
    if (ctx->err[0] != 0 && opts->aggressive_framesync != 0) {
        return;
    }

    ctx->io = (ctx->mpdu_byte[0] >> 5) & 0x1;
    ctx->fmt = ctx->mpdu_byte[0] & 0x1F;
    ctx->sap = ctx->mpdu_byte[1] & 0x3F;
    ctx->blks = ctx->mpdu_byte[6] & 0x7F;
    p25_decode_pdu_header(opts, state, ctx->mpdu_byte);
}

static void
p25_mpdu_log_header_crc_error(const P25MpduContext* ctx) {
    if (ctx->err[0] == 0) {
        return;
    }

    DSD_FPRINTF(stderr, "%s", KRED);
    DSD_FPRINTF(stderr, " P25 Data Header CRC Error");
    DSD_FPRINTF(stderr, "%s", KNRM);
    DSD_FPRINTF(stderr, " [HDR:");
    for (int byte_idx = 0; byte_idx < P25_MPDU_R12_BYTES; byte_idx++) {
        DSD_FPRINTF(stderr, "%02X", ctx->mpdu_byte[byte_idx]);
    }
    DSD_FPRINTF(stderr, " AN=%d IO=%d FMT=0x%02X SAP=0x%02X BLKS=%d]", (ctx->mpdu_byte[0] >> 6) & 0x1,
                (ctx->mpdu_byte[0] >> 5) & 0x1, ctx->mpdu_byte[0] & 0x1F, ctx->mpdu_byte[1] & 0x3F,
                ctx->mpdu_byte[6] & 0x7F);
}

static void
p25_mpdu_print_trunking_payload(const dsd_opts* opts, const P25MpduContext* ctx, uint32_t crc_extracted,
                                uint32_t crc_computed) {
    if (opts->payload != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, "\n P25 MBT Payload \n  ");
    for (int byte_idx = 0; byte_idx < ((ctx->blks + 1) * P25_MPDU_R12_BYTES); byte_idx++) {
        if ((byte_idx != 0) && ((byte_idx % P25_MPDU_R12_BYTES) == 0)) {
            DSD_FPRINTF(stderr, "\n  ");
        }
        DSD_FPRINTF(stderr, "[%02X]", ctx->mpdu_byte[byte_idx]);
    }

    DSD_FPRINTF(stderr, "\n ");
    DSD_FPRINTF(stderr, " CRC EXT %08X CMP %08X", crc_extracted, crc_computed);
    DSD_FPRINTF(stderr, "%s ", KNRM);

    if (ctx->err[0] != 0) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " (HDR CRC16 ERR)");
        DSD_FPRINTF(stderr, "%s", KCYN);
    }
    if (ctx->err[1] != 0) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " (MBT CRC32 ERR)");
        DSD_FPRINTF(stderr, "%s", KCYN);
    }
}

static int
p25_mpdu_handle_trunking(dsd_opts* opts, dsd_state* state, P25MpduContext* ctx) {
    if ((ctx->sap != 0x3D) || ((ctx->fmt != 0x17) && (ctx->fmt != 0x15))) {
        return 0;
    }

    int len = P25_MPDU_R12_BYTES * (ctx->blks + 1);
    uint32_t crc_extracted = ((uint32_t)ctx->mpdu_byte[len - 4] << 24) | ((uint32_t)ctx->mpdu_byte[len - 3] << 16)
                             | ((uint32_t)ctx->mpdu_byte[len - 2] << 8) | ctx->mpdu_byte[len - 1];
    uint32_t crc_computed = crc32mbf(ctx->mpdu_byte + P25_MPDU_R12_BYTES, (P25_MPDU_HEADER_BITS * ctx->blks) - 32);
    if (crc_computed == crc_extracted) {
        ctx->err[1] = 0;
    }

    if (ctx->err[0] == 0 && ctx->err[1] == 0) {
        (void)p25_decode_pdu_trunking(opts, state, ctx->mpdu_byte, (size_t)len);
    }

    p25_mpdu_print_trunking_payload(opts, ctx, crc_extracted, crc_computed);
    DSD_FPRINTF(stderr, "%s ", KNRM);
    DSD_FPRINTF(stderr, "\n");
    return 1;
}

static void
p25_mpdu_compute_rate34_crc(P25MpduContext* ctx, uint32_t* crc_extracted, uint32_t* crc_computed) {
    uint8_t crc_bytes[P25_MPDU_MAX_DATA_BLOCKS * P25_MPDU_R34_BYTES];
    DSD_MEMSET(crc_bytes, 0, sizeof(crc_bytes));
    for (int byte_idx = 0; byte_idx < 16 * (ctx->blks + 1); byte_idx++) {
        crc_bytes[byte_idx] = (uint8_t)convert_bits_into_output(&ctx->mpdu_crc_bits[(size_t)byte_idx * 8], 8);
    }

    if (ctx->blks > 0) {
        *crc_extracted = (uint32_t)convert_bits_into_output(&ctx->mpdu_crc_bits[(((size_t)128) * ctx->blks) - 32], 32);
        *crc_computed = crc32mbf(crc_bytes, (((size_t)128) * ctx->blks) - 32);
    } else {
        *crc_extracted = 0;
        *crc_computed = 0;
    }
    if (*crc_computed == *crc_extracted) {
        ctx->err[1] = 0;
    }
}

static int
p25_mpdu_reconstruct_rate34_payload(P25MpduContext* ctx, uint8_t* dbsn, uint16_t* crc9_ext, uint16_t* crc9_cmp) {
    DSD_MEMSET(ctx->mpdu_byte + P25_MPDU_R12_BYTES, 0, sizeof(ctx->mpdu_byte) - P25_MPDU_R12_BYTES);
    int mpdu_idx = P25_MPDU_R12_BYTES;
    int next = 0;
    int block_ptr = 0;

    for (int byte_idx = 2, advance = 1; byte_idx <= P25_MPDU_R34_BYTES * ctx->blks; byte_idx += advance) {
        advance = 1;
        int read_idx = byte_idx;
        if ((byte_idx % P25_MPDU_R34_BYTES) == 0) {
            dbsn[block_ptr] = ctx->r34bytes[byte_idx - P25_MPDU_R34_BYTES] >> 1;
            crc9_ext[block_ptr] = (uint16_t)(((ctx->r34bytes[byte_idx - P25_MPDU_R34_BYTES] & 1) << 8)
                                             | ctx->r34bytes[byte_idx - (P25_MPDU_R34_BYTES - 1)]);
            crc9_cmp[block_ptr] = ComputeCrc9Bit(ctx->mpdu_crc9_bits + next, 135);
            next += 135;
            block_ptr++;
            if (byte_idx != P25_MPDU_R34_BYTES * ctx->blks) {
                read_idx = byte_idx + 2;
                advance = 3;
            }
        }
        if ((size_t)mpdu_idx < sizeof(ctx->mpdu_byte)) {
            ctx->mpdu_byte[mpdu_idx++] = ctx->r34bytes[read_idx];
        }
    }
    return mpdu_idx;
}

static void
p25_mpdu_print_rate34_payload(const dsd_opts* opts, const P25MpduContext* ctx, int mpdu_idx, const uint8_t* dbsn,
                              const uint16_t* crc9_ext, const uint16_t* crc9_cmp, uint32_t crc_extracted,
                              uint32_t crc_computed) {
    if (opts->payload != 1) {
        return;
    }

    int block_ptr = 0;
    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, "\n P25 MPDU Rate 34 Payload \n ");
    for (int byte_idx = 0; byte_idx < P25_MPDU_R12_BYTES; byte_idx++) {
        DSD_FPRINTF(stderr, "%02X", ctx->mpdu_byte[byte_idx]);
    }
    DSD_FPRINTF(stderr, "         Header \n ");

    for (int byte_idx = P25_MPDU_R12_BYTES; byte_idx < mpdu_idx; byte_idx++) {
        if (((byte_idx - P25_MPDU_R12_BYTES) != 0) && (((byte_idx - P25_MPDU_R12_BYTES) % 16) == 0)) {
            if (crc9_ext[block_ptr] == crc9_cmp[block_ptr]) {
                DSD_FPRINTF(stderr, " DBSN: %d;", dbsn[block_ptr] + 1);
            } else {
                DSD_FPRINTF(stderr, "%s", KRED);
                DSD_FPRINTF(stderr, " CRC ERR;");
                DSD_FPRINTF(stderr, "%s", KCYN);
            }
            if (byte_idx != (mpdu_idx - 1)) {
                DSD_FPRINTF(stderr, "\n ");
            }
            block_ptr++;
        }
        if (byte_idx != (mpdu_idx - 1)) {
            DSD_FPRINTF(stderr, "%02X", ctx->mpdu_byte[byte_idx]);
        }
    }

    if (ctx->err[1] != 0) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "\n (MPDU CRC32 ERR)");
        DSD_FPRINTF(stderr, "%s", KCYN);
        DSD_FPRINTF(stderr, " CRC EXT %08X CMP %08X", crc_extracted, crc_computed);
    }
}

static void
p25_mpdu_clear_last_call(dsd_state* state) {
    state->lasttg = 0;
    state->lastsrc = 0;
    state->p25_policy_tg[0] = 0;
}

static void
p25_mpdu_handle_rate34(dsd_opts* opts, dsd_state* state, P25MpduContext* ctx) {
    uint32_t crc_extracted = 0;
    uint32_t crc_computed = 0;
    p25_mpdu_compute_rate34_crc(ctx, &crc_extracted, &crc_computed);

    uint8_t dbsn[P25_MPDU_MAX_DATA_BLOCKS];
    uint16_t crc9_ext[P25_MPDU_MAX_DATA_BLOCKS];
    uint16_t crc9_cmp[P25_MPDU_MAX_DATA_BLOCKS];
    DSD_MEMSET(dbsn, 0, sizeof(dbsn));
    DSD_MEMSET(crc9_ext, 0, sizeof(crc9_ext));
    DSD_MEMSET(crc9_cmp, 0, sizeof(crc9_cmp));

    int mpdu_idx = p25_mpdu_reconstruct_rate34_payload(ctx, dbsn, crc9_ext, crc9_cmp);
    if ((ctx->err[1] == 0 || opts->aggressive_framesync == 0) && ctx->blks != 0) {
        p25_decode_pdu_data(opts, state, ctx->mpdu_byte, mpdu_idx - 1);
    }

    p25_mpdu_print_rate34_payload(opts, ctx, mpdu_idx, dbsn, crc9_ext, crc9_cmp, crc_extracted, crc_computed);
    DSD_FPRINTF(stderr, "%s ", KNRM);
    DSD_FPRINTF(stderr, "\n");
    p25_mpdu_clear_last_call(state);
}

static void
p25_mpdu_print_rate12_payload(const dsd_opts* opts, const P25MpduContext* ctx, int len) {
    if (opts->payload != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, "\n P25 MPDU Rate 12 Payload: \n  ");
    for (int byte_idx = 0; byte_idx < len; byte_idx++) {
        if (byte_idx == P25_MPDU_R12_BYTES) {
            DSD_FPRINTF(stderr, " Header");
        }
        if ((byte_idx != 0) && ((byte_idx % P25_MPDU_R12_BYTES) == 0)) {
            DSD_FPRINTF(stderr, "\n  ");
        }
        DSD_FPRINTF(stderr, "%02X", ctx->mpdu_byte[byte_idx]);
    }
}

static void
p25_mpdu_handle_rate12(dsd_opts* opts, dsd_state* state, P25MpduContext* ctx) {
    int len = P25_MPDU_R12_BYTES * (ctx->blks + 1);
    uint32_t crc_extracted = 0;
    uint32_t crc_computed = 0;
    if (ctx->blks != 0) {
        crc_extracted = ((uint32_t)ctx->mpdu_byte[len - 4] << 24) | ((uint32_t)ctx->mpdu_byte[len - 3] << 16)
                        | ((uint32_t)ctx->mpdu_byte[len - 2] << 8) | ctx->mpdu_byte[len - 1];
        crc_computed = crc32mbf(ctx->mpdu_byte + P25_MPDU_R12_BYTES, (P25_MPDU_HEADER_BITS * ctx->blks) - 32);
        if (crc_computed == crc_extracted) {
            ctx->err[1] = 0;
        }
    } else {
        ctx->err[1] = 0;
    }

    if ((ctx->err[1] == 0 || opts->aggressive_framesync == 0) && ctx->blks != 0) {
        p25_decode_pdu_data(opts, state, ctx->mpdu_byte, len);
    }

    p25_mpdu_print_rate12_payload(opts, ctx, len);
    if (ctx->err[1] != 0) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "\n (MPDU CRC32 ERR)");
        DSD_FPRINTF(stderr, "%s", KCYN);
        DSD_FPRINTF(stderr, " CRC EXT %08X CMP %08X", crc_extracted, crc_computed);
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
    DSD_FPRINTF(stderr, "\n");
    p25_mpdu_clear_last_call(state);
}

static void
p25_mpdu_dispatch_payload(dsd_opts* opts, dsd_state* state, P25MpduContext* ctx) {
    if (p25_mpdu_handle_trunking(opts, state, ctx)) {
        return;
    }
    if (ctx->r34) {
        p25_mpdu_handle_rate34(opts, state, ctx);
    } else {
        p25_mpdu_handle_rate12(opts, state, ctx);
    }
}

void
processMPDU(dsd_opts* opts, dsd_state* state) {
    P25MpduContext ctx;
    p25_mpdu_prepare_state(opts, state);
    p25_mpdu_context_init(&ctx);
    p25_mpdu_collect_blocks(opts, state, &ctx);
    p25_mpdu_finalize_header(state, &ctx);
    p25_mpdu_decode_header_if_usable(opts, state, &ctx);
    p25_mpdu_log_header_crc_error(&ctx);
    p25_mpdu_dispatch_payload(opts, state, &ctx);
    p25_status_accum_classify(state);
}
