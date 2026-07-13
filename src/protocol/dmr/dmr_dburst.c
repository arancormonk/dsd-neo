// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*-------------------------------------------------------------------------------
 * dmr_dburst.c
 * DMR Data Burst Handling and related BPTC/FEC/CRC Functions
 *
 * Portions of BPTC/FEC/CRC code from LouisErigHerve
 * Source: https://github.com/LouisErigHerve/dsd/blob/master/src/dmr_sync.c
 *
 * LWVMOBILE
 * 2023-12 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

//TODO: Test USBD LIP Decoder with Real World Samples (if/when available)
//TODO: Test UDT NMEA and LIP Decoders with Real World Samples (if/when available)

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/fec/bptc.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/dmr/r34_viterbi.h>
#include <dsd-neo/runtime/colors.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

enum {
    DMR_DBURST_F_BPTC = 1U << 0U,
    DMR_DBURST_F_TRELLIS = 1U << 1U,
    DMR_DBURST_F_EMB = 1U << 2U,
    DMR_DBURST_F_LC = 1U << 3U,
    DMR_DBURST_F_FULL = 1U << 4U,
    DMR_DBURST_F_UDT = 1U << 7U,
};

typedef struct {
    const char* subtype;
    uint32_t crcmask;
    uint8_t flags;
    uint8_t crclen;
    uint8_t pdu_len;
} dmr_dburst_profile;

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    uint8_t* info;
    const uint8_t* reliab98;
    uint8_t databurst;
    uint8_t slot;

    uint32_t crc_extracted;
    uint32_t crc_computed;
    uint32_t crc_correct;
    uint32_t irrecoverable_errors;

    uint8_t blockcounter;
    uint8_t confdatabits[250];

    uint8_t bptc_deinterleaved[196];
    uint8_t bptc_data_bits[96];
    uint8_t bptc_data_bytes[12];

    uint8_t bptc_matrix[8][16];
    uint8_t lc_data_bits[77];
    int burst;

    uint8_t dmr_pdu[25];
    uint8_t dmr_pdu_bits[196];

    uint8_t r[3];
    uint8_t bptc_reserved_bits;
    uint8_t is_ras;
    uint32_t crc_original_validity;

    uint32_t crcmask;
    uint8_t crclen;

    uint8_t is_bptc;
    uint8_t is_trellis;
    uint8_t is_emb;
    uint8_t is_lc;
    uint8_t is_full;
    uint8_t is_udt;
    uint8_t pdu_len;
    uint8_t pdu_start;

    uint8_t usbd_st;
    uint8_t dbsn_for_seq;
    int dbsn_valid;
} dmr_data_burst_ctx;

static void
dmr_dburst_ctx_init(dmr_data_burst_ctx* ctx, dsd_opts* opts, dsd_state* state, uint8_t info[196], uint8_t databurst,
                    const uint8_t* reliab98) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->opts = opts;
    ctx->state = state;
    ctx->info = info;
    ctx->databurst = databurst;
    ctx->reliab98 = reliab98;
    ctx->slot = state->currentslot;
    ctx->burst = -1;
}

static void
dmr_dburst_bits_from_bytes(uint8_t* out_bits, const uint8_t* bytes, uint32_t count_bytes) {
    uint32_t i;
    uint32_t j;
    for (i = 0, j = 0; i < count_bytes; i++, j += 8) {
        out_bits[j + 0] = (bytes[i] >> 7) & 0x01;
        out_bits[j + 1] = (bytes[i] >> 6) & 0x01;
        out_bits[j + 2] = (bytes[i] >> 5) & 0x01;
        out_bits[j + 3] = (bytes[i] >> 4) & 0x01;
        out_bits[j + 4] = (bytes[i] >> 3) & 0x01;
        out_bits[j + 5] = (bytes[i] >> 2) & 0x01;
        out_bits[j + 6] = (bytes[i] >> 1) & 0x01;
        out_bits[j + 7] = (bytes[i] >> 0) & 0x01;
    }
}

static void
dmr_dburst_apply_base_profile(dmr_data_burst_ctx* ctx) {
    static const dmr_dburst_profile profiles[12] = {
        [0x00] = {.subtype = " PI  ", .crcmask = 0x6969, .flags = DMR_DBURST_F_BPTC, .crclen = 16, .pdu_len = 12},
        [0x01] = {.subtype = " VLC ",
                  .crcmask = 0x969696,
                  .flags = (DMR_DBURST_F_BPTC | DMR_DBURST_F_LC),
                  .crclen = 24,
                  .pdu_len = 12},
        [0x02] = {.subtype = " TLC ",
                  .crcmask = 0x999999,
                  .flags = (DMR_DBURST_F_BPTC | DMR_DBURST_F_LC),
                  .crclen = 24,
                  .pdu_len = 12},
        [0x03] = {.subtype = " CSBK ", .crcmask = 0xA5A5, .flags = DMR_DBURST_F_BPTC, .crclen = 16, .pdu_len = 12},
        [0x04] = {.subtype = " MBCH ", .crcmask = 0xAAAA, .flags = DMR_DBURST_F_BPTC, .crclen = 16, .pdu_len = 12},
        [0x05] = {.subtype = " MBCC ", .crcmask = 0x0, .flags = DMR_DBURST_F_BPTC, .crclen = 0, .pdu_len = 12},
        [0x06] = {.subtype = " DATA ", .crcmask = 0xCCCC, .flags = DMR_DBURST_F_BPTC, .crclen = 16, .pdu_len = 12},
        [0x07] = {.subtype = " R12U ", .crcmask = 0x0F0, .flags = DMR_DBURST_F_BPTC, .crclen = 9, .pdu_len = 12},
        [0x08] = {.subtype = " R34U ", .crcmask = 0x1FF, .flags = DMR_DBURST_F_TRELLIS, .crclen = 9, .pdu_len = 18},
        [0x09] = {.subtype = " IDLE ", .crcmask = 0x0, .flags = 0, .crclen = 0, .pdu_len = 0},
        [0x0A] = {.subtype = " R_1U ", .crcmask = 0x10F, .flags = DMR_DBURST_F_FULL, .crclen = 9, .pdu_len = 24},
        [0x0B] = {.subtype = " USBD ", .crcmask = 0x3333, .flags = DMR_DBURST_F_BPTC, .crclen = 16, .pdu_len = 12},
    };

    if (ctx->databurst <= 0x0B) {
        const dmr_dburst_profile* p = &profiles[ctx->databurst];
        ctx->is_bptc = (p->flags & DMR_DBURST_F_BPTC) != 0;
        ctx->is_trellis = (p->flags & DMR_DBURST_F_TRELLIS) != 0;
        ctx->is_emb = (p->flags & DMR_DBURST_F_EMB) != 0;
        ctx->is_lc = (p->flags & DMR_DBURST_F_LC) != 0;
        ctx->is_full = (p->flags & DMR_DBURST_F_FULL) != 0;
        ctx->crclen = p->crclen;
        ctx->crcmask = p->crcmask;
        ctx->pdu_len = p->pdu_len;
        if (p->subtype != NULL) {
            DSD_SNPRINTF(ctx->state->fsubtype, sizeof(ctx->state->fsubtype), "%s", p->subtype);
        }
        return;
    }

    if (ctx->databurst == 0xEB) {
        ctx->crclen = 5;
        ctx->is_emb = 1;
        ctx->pdu_len = 9;
        return;
    }

    ctx->is_full = 1;
    ctx->pdu_len = 25;
    DSD_SNPRINTF(ctx->state->fsubtype, sizeof(ctx->state->fsubtype), " _UNK ");
}

static void
dmr_dburst_apply_dynamic_profile(dmr_data_burst_ctx* ctx) {
    if (ctx->databurst == 0x07) {
        if (ctx->state->data_conf_data[ctx->slot] == 1) {
            ctx->pdu_len = 10;
            ctx->pdu_start = 2;
            DSD_SNPRINTF(ctx->state->fsubtype, sizeof(ctx->state->fsubtype), " R12C ");
        }
        if (ctx->state->data_header_format[ctx->slot] == 0) {
            ctx->is_udt = 1;
            if (ctx->state->data_conf_data[ctx->slot] == 1) {
                DSD_SNPRINTF(ctx->state->fsubtype, sizeof(ctx->state->fsubtype), " UDTC ");
            } else {
                DSD_SNPRINTF(ctx->state->fsubtype, sizeof(ctx->state->fsubtype), " UDTU ");
            }
        }
    } else if (ctx->databurst == 0x08) {
        if (ctx->state->data_conf_data[ctx->slot] == 1) {
            ctx->pdu_len = 16;
            ctx->pdu_start = 2;
            DSD_SNPRINTF(ctx->state->fsubtype, sizeof(ctx->state->fsubtype), " R34C ");
        }
    } else if (ctx->databurst == 0x0A) {
        if (ctx->state->data_conf_data[ctx->slot] == 1) {
            ctx->pdu_len = 22;
            ctx->pdu_start = 2;
            DSD_SNPRINTF(ctx->state->fsubtype, sizeof(ctx->state->fsubtype), " R_1C ");
        }
    }
}

static int
dmr_dburst_keeps_data_p_head(uint8_t databurst) {
    return databurst == 0x06 || databurst == 0x07 || databurst == 0x08 || databurst == 0x0A || databurst == 0x0B;
}

static void
dmr_dburst_print_header_and_dump(dmr_data_burst_ctx* ctx) {
    if (ctx->databurst == 0xEB) {
        return;
    }

    if (ctx->state->dmr_ms_mode == 0) {
        if (ctx->state->dmr_color_code != 16) {
            DSD_FPRINTF(stderr, "| Color Code=%02d ", ctx->state->dmr_color_code);
        } else {
            DSD_FPRINTF(stderr, "| Color Code=XX ");
        }
    }
    DSD_FPRINTF(stderr, "|%s", ctx->state->fsubtype);

    if (ctx->opts->use_dsp_output == 1) {
        FILE* pfile = dsd_fopen_private(ctx->opts->dsp_out_file, "a");
        if (pfile != NULL) {
            uint32_t i;
            DSD_FPRINTF(pfile, "\\n%d 98 ", ctx->slot + 1);
            for (i = 0; i < 6; i++) {
                int cach_byte = (ctx->state->dmr_stereo_payload[((size_t)i * 2)] << 2)
                                | ctx->state->dmr_stereo_payload[((size_t)i * 2) + 1];
                DSD_FPRINTF(pfile, "%X", cach_byte);
            }
            DSD_FPRINTF(pfile, "\\n%d %02X ", ctx->slot + 1, ctx->databurst);
            for (i = 6; i < 72; i++) {
                int dsp_byte = (ctx->state->dmr_stereo_payload[((size_t)i * 2)] << 2)
                               | ctx->state->dmr_stereo_payload[((size_t)i * 2) + 1];
                DSD_FPRINTF(pfile, "%X", dsp_byte);
            }
            fclose(pfile);
        }
    }
}

static void
dmr_dburst_unpack_bptc_bytes(dmr_data_burst_ctx* ctx) {
    uint32_t i;
    uint32_t j;
    uint32_t k = 0;

    for (i = 0; i < 12; i++) {
        ctx->bptc_data_bytes[i] = 0;
        for (j = 0; j < 8; j++) {
            ctx->bptc_data_bytes[i] = (uint8_t)(ctx->bptc_data_bytes[i] << 1);
            ctx->bptc_data_bytes[i] = (uint8_t)(ctx->bptc_data_bytes[i] | (ctx->bptc_data_bits[k] & 0x01));
            k++;
        }
    }
}

static void
dmr_dburst_bptc_crc_confirmed_1_2_rate(dmr_data_burst_ctx* ctx) {
    uint32_t i;

    ctx->blockcounter = ctx->state->data_block_counter[ctx->slot];
    ctx->dbsn_for_seq = (uint8_t)convert_bits_into_output(&ctx->bptc_data_bits[0], 7);
    ctx->dbsn_valid = 1;
    ctx->crc_extracted = (uint32_t)convert_bits_into_output(&ctx->bptc_data_bits[7], 9);
    ctx->crc_extracted ^= ctx->crcmask;

    for (i = 0; i < 80; i++) {
        ctx->confdatabits[i] = ctx->bptc_data_bits[i + 16];
    }
    for (i = 0; i < 7; i++) {
        ctx->confdatabits[i + 80] = ctx->bptc_data_bits[i];
    }

    ctx->crc_computed = ComputeCrc9Bit(ctx->confdatabits, 87);
    if (ctx->crc_extracted == ctx->crc_computed) {
        ctx->crc_correct = 1;
        ctx->state->data_block_crc_valid[ctx->slot][ctx->blockcounter] = 1;
    } else {
        ctx->state->data_block_crc_valid[ctx->slot][ctx->blockcounter] = 0;
    }
}

static void
dmr_dburst_handle_bptc_crc(dmr_data_burst_ctx* ctx) {
    if (ctx->is_lc) {
        ctx->crc_correct = ComputeAndCorrectFullLinkControlCrc(ctx->bptc_data_bytes, &ctx->crc_computed, ctx->crcmask);
        return;
    }

    if (ctx->state->data_conf_data[ctx->slot] == 0 && ctx->databurst == 0x07) {
        ctx->crc_computed = 0;
        ctx->crc_correct = 1;
        return;
    }

    if (ctx->state->data_conf_data[ctx->slot] == 1 && ctx->databurst == 0x07) {
        dmr_dburst_bptc_crc_confirmed_1_2_rate(ctx);
        return;
    }

    ctx->crc_computed = ComputeCrcCCITT(ctx->bptc_data_bits);
    ctx->crc_correct = (ctx->crc_computed == ctx->crc_extracted);
}

static void
dmr_dburst_copy_bptc_outputs(dmr_data_burst_ctx* ctx) {
    uint32_t i;
    uint8_t max_bytes = ctx->pdu_len;
    uint8_t avail = (uint8_t)(sizeof(ctx->bptc_data_bytes) - ctx->pdu_start);
    if (max_bytes > avail) {
        max_bytes = avail;
    }

    dmr_dburst_bits_from_bytes(ctx->bptc_data_bits, ctx->bptc_data_bytes + ctx->pdu_start, max_bytes);

    for (i = 0; i < max_bytes; i++) {
        ctx->dmr_pdu[i] = ctx->bptc_data_bytes[i + ctx->pdu_start];
    }
    for (i = 0; i < ((uint32_t)max_bytes * 8U); i++) {
        ctx->dmr_pdu_bits[i] = ctx->bptc_data_bits[i];
    }
}

static void
dmr_dburst_handle_bptc(dmr_data_burst_ctx* ctx) {
    uint32_t i;

    ctx->crc_computed = 0;
    ctx->irrecoverable_errors = 0;

    BPTCDeInterleaveDMRData(ctx->info, ctx->bptc_deinterleaved);
    ctx->irrecoverable_errors = BPTC_196x96_Extract_Data(ctx->bptc_deinterleaved, ctx->bptc_data_bits, ctx->r);
    ctx->bptc_reserved_bits = (ctx->r[0] & 0x01) | ((ctx->r[1] << 1) & 0x02) | ((ctx->r[2] << 2) & 0x04);

    dmr_dburst_unpack_bptc_bytes(ctx);

    ctx->crc_extracted = 0;
    for (i = 0; i < ctx->crclen; i++) {
        ctx->crc_extracted = (ctx->crc_extracted << 1) | (uint32_t)(ctx->bptc_data_bits[i + 96 - ctx->crclen] & 1);
    }
    ctx->crc_extracted ^= ctx->crcmask;

    dmr_dburst_handle_bptc_crc(ctx);

    if (ctx->opts->aggressive_framesync == 0 && ctx->crc_correct == 0 && ctx->irrecoverable_errors == 0
        && ctx->bptc_reserved_bits == 4) {
        ctx->is_ras = 1;
    }
    if (ctx->bptc_data_bytes[1] == 0x68) {
        ctx->is_ras = 0;
    }
    if (ctx->is_ras == 1) {
        ctx->crc_original_validity = ctx->crc_correct;
        ctx->crc_correct = 1;
    }

    if (ctx->databurst == 0x04 || ctx->databurst == 0x06) {
        ctx->state->data_block_crc_valid[ctx->slot][0] = (ctx->crc_correct != 0);
    }

    dmr_dburst_copy_bptc_outputs(ctx);
}

static void
dmr_dburst_handle_emb(dmr_data_burst_ctx* ctx) {
    uint32_t i;
    uint32_t j;
    uint32_t k = 0;

    ctx->crc_computed = 0;
    ctx->irrecoverable_errors = 0;

    ctx->burst = 1;
    for (i = 0; i < 16; i++) {
        for (j = 0; j < 8; j++) {
            ctx->bptc_matrix[j][i] = ctx->state->dmr_embedded_signalling[ctx->slot][ctx->burst][k + 8];
            k++;
            if (k >= 32) {
                k = 0;
                ctx->burst++;
            }
        }
    }

    ctx->irrecoverable_errors = BPTC_128x77_Extract_Data(ctx->bptc_matrix, ctx->lc_data_bits);
    ctx->crc_extracted = (uint32_t)convert_bits_into_output(&ctx->lc_data_bits[72], 5);
    ctx->crc_computed = ComputeCrc5Bit(ctx->lc_data_bits);
    ctx->crc_correct = (ctx->crc_extracted == ctx->crc_computed);

    for (i = 0; i < 72; i++) {
        ctx->dmr_pdu_bits[i] = ctx->lc_data_bits[i];
    }
    for (i = 0; i < 9; i++) {
        ctx->dmr_pdu[i] = (uint8_t)convert_bits_into_output(&ctx->lc_data_bits[((size_t)i * 8)], 8);
    }
}

static void
dmr_dburst_trellis_candidate_metrics(dmr_data_burst_ctx* ctx, const uint8_t bytes18[18], uint8_t* cand_dbsn,
                                     int* cand_crc_ok) {
    uint32_t i;
    uint32_t cand_ext;
    uint32_t cand_comp;

    DSD_MEMSET(ctx->dmr_pdu_bits, 0, sizeof(ctx->dmr_pdu_bits));
    dmr_dburst_bits_from_bytes(ctx->dmr_pdu_bits, bytes18, 18);

    *cand_dbsn = (uint8_t)convert_bits_into_output(&ctx->dmr_pdu_bits[0], 7);
    cand_ext = (uint32_t)convert_bits_into_output(&ctx->dmr_pdu_bits[7], 9) ^ ctx->crcmask;

    for (i = 0; i < 128; i++) {
        ctx->confdatabits[i] = ctx->dmr_pdu_bits[i + 16];
    }
    for (i = 0; i < 7; i++) {
        ctx->confdatabits[i + 128] = ctx->dmr_pdu_bits[i];
    }

    cand_comp = ComputeCrc9Bit(ctx->confdatabits, 135);
    *cand_crc_ok = (cand_ext == cand_comp);
}

static int
dmr_dburst_trellis_choose_candidate_index(dmr_data_burst_ctx* ctx, const dmr_r34_candidate* list, int list_n) {
    const int have_expected_dbsn = ctx->state->data_dbsn_have[ctx->slot] != 0;
    const uint8_t expected_dbsn = ctx->state->data_dbsn_expected[ctx->slot];
    int best_crc_dbsn = -1;
    int best_dbsn = -1;
    int best_crc = -1;
    int ci;

    for (ci = 0; ci < list_n; ci++) {
        uint8_t cand_dbsn = 0;
        int cand_crc_ok = 0;
        dmr_dburst_trellis_candidate_metrics(ctx, list[ci].bytes18, &cand_dbsn, &cand_crc_ok);

        if (have_expected_dbsn && cand_dbsn == expected_dbsn) {
            if (best_dbsn < 0) {
                best_dbsn = ci;
            }
            if (cand_crc_ok) {
                best_crc_dbsn = ci;
                break;
            }
        }

        if (cand_crc_ok && best_crc < 0) {
            best_crc = ci;
        }
    }

    if (best_crc_dbsn >= 0) {
        return best_crc_dbsn;
    }
    if (best_dbsn >= 0) {
        return best_dbsn;
    }
    if (best_crc >= 0) {
        return best_crc;
    }
    return 0;
}

static const uint8_t*
dmr_dburst_trellis_fallback(int have_soft, int have_hard, const uint8_t soft[18], const uint8_t hard[18],
                            const uint8_t symbol_fallback[18]) {
    if (have_soft) {
        return soft;
    }
    if (have_hard) {
        return hard;
    }
    return symbol_fallback;
}

static void
dmr_dburst_pick_trellis_payload(dmr_data_burst_ctx* ctx, uint8_t tdibits[98], uint8_t trellis_return[18]) {
    uint8_t trellis_soft[18];
    uint8_t trellis_hard[18];
    uint8_t trellis_symbol_fallback[18];
    int have_soft = 0;
    int have_hard = 0;

    DSD_MEMSET(trellis_soft, 0, sizeof(trellis_soft));
    DSD_MEMSET(trellis_hard, 0, sizeof(trellis_hard));
    DSD_MEMSET(trellis_symbol_fallback, 0, sizeof(trellis_symbol_fallback));

    if (ctx->reliab98 != NULL && dmr_r34_viterbi_decode_soft(tdibits, ctx->reliab98, trellis_soft) == 0) {
        have_soft = 1;
    }
    if (dmr_r34_viterbi_decode(tdibits, trellis_hard) == 0) {
        have_hard = 1;
    }
    (void)dmr_34(tdibits, trellis_symbol_fallback);

    if (ctx->opts->audio_in_type == AUDIO_IN_SYMBOL_BIN) {
        DSD_MEMCPY(trellis_return, trellis_symbol_fallback, 18);
        return;
    }

    if (ctx->state->data_conf_data[ctx->slot] == 1) {
        dmr_r34_candidate list[256];
        int list_n = 0;
        if (dmr_r34_viterbi_decode_list(tdibits, ctx->reliab98, list, 256, &list_n) == 0 && list_n > 0) {
            int chosen_i = dmr_dburst_trellis_choose_candidate_index(ctx, list, list_n);
            DSD_MEMCPY(trellis_return, list[chosen_i].bytes18, 18);
            return;
        }
    }

    DSD_MEMCPY(trellis_return,
               dmr_dburst_trellis_fallback(have_soft, have_hard, trellis_soft, trellis_hard, trellis_symbol_fallback),
               18);
}

static void
dmr_dburst_trellis_update_confirmed_crc(dmr_data_burst_ctx* ctx) {
    uint32_t i;

    if (ctx->state->data_conf_data[ctx->slot] == 0) {
        ctx->crc_correct = 1;
        return;
    }

    ctx->blockcounter = ctx->state->data_block_counter[ctx->slot];
    (void)convert_bits_into_output(&ctx->dmr_pdu_bits[0], 7);
    ctx->crc_extracted = (uint32_t)convert_bits_into_output(&ctx->dmr_pdu_bits[7], 9);
    ctx->crc_extracted ^= ctx->crcmask;

    for (i = 0; i < 128; i++) {
        ctx->confdatabits[i] = ctx->dmr_pdu_bits[i + 16];
    }
    for (i = 0; i < 7; i++) {
        ctx->confdatabits[i + 128] = ctx->dmr_pdu_bits[i];
    }

    ctx->crc_computed = ComputeCrc9Bit(ctx->confdatabits, 135);
    if (ctx->crc_extracted == ctx->crc_computed) {
        ctx->crc_correct = 1;
        ctx->state->data_block_crc_valid[ctx->slot][ctx->blockcounter] = 1;
    } else {
        ctx->state->data_block_crc_valid[ctx->slot][ctx->blockcounter] = 0;
    }
}

static void
dmr_dburst_handle_trellis(dmr_data_burst_ctx* ctx) {
    uint32_t i;
    uint8_t tdibits[98];
    uint8_t trellis_return[18];

    ctx->crc_computed = 0;
    ctx->irrecoverable_errors = 1;

    DSD_MEMSET(tdibits, 0, sizeof(tdibits));
    DSD_MEMSET(trellis_return, 0, sizeof(trellis_return));

    for (i = 0; i < 98; i++) {
        tdibits[i] = (ctx->info[((size_t)i * 2)] << 1) | ctx->info[((size_t)i * 2) + 1];
    }

    dmr_dburst_pick_trellis_payload(ctx, tdibits, trellis_return);
    ctx->irrecoverable_errors = 0;

    for (i = 0; i < ctx->pdu_len; i++) {
        ctx->dmr_pdu[i] = trellis_return[i + ctx->pdu_start];
    }

    dmr_dburst_bits_from_bytes(ctx->dmr_pdu_bits, trellis_return, 18);
    if (ctx->state->data_conf_data[ctx->slot] == 1) {
        ctx->dbsn_for_seq = (uint8_t)convert_bits_into_output(&ctx->dmr_pdu_bits[0], 7);
        ctx->dbsn_valid = 1;
    }

    dmr_dburst_trellis_update_confirmed_crc(ctx);

    DSD_MEMSET(ctx->dmr_pdu_bits, 0, sizeof(ctx->dmr_pdu_bits));
    dmr_dburst_bits_from_bytes(ctx->dmr_pdu_bits, trellis_return + ctx->pdu_start, ctx->pdu_len);
}

static void
dmr_dburst_handle_full(dmr_data_burst_ctx* ctx) {
    ctx->crc_computed = 0;
    ctx->irrecoverable_errors = 0;

    pack_bit_array_into_byte_array(ctx->info + ((size_t)ctx->pdu_start * 8u), ctx->dmr_pdu, 12 - ctx->pdu_start);
    pack_bit_array_into_byte_array(ctx->info + 100, ctx->dmr_pdu + (12 - ctx->pdu_start), 12);

    if (ctx->state->data_conf_data[ctx->slot] == 0) {
        ctx->crc_correct = 1;
    } else {
        int k = 0;
        ctx->blockcounter = ctx->state->data_block_counter[ctx->slot];
        ctx->dbsn_for_seq = (uint8_t)convert_bits_into_output(&ctx->info[0], 7);
        ctx->dbsn_valid = 1;
        ctx->crc_extracted = (uint32_t)convert_bits_into_output(&ctx->info[7], 9);
        ctx->crc_extracted ^= ctx->crcmask;

        for (uint32_t i = 16; i < 96; i++) {
            ctx->confdatabits[k++] = ctx->info[i];
        }
        for (uint32_t i = 100; i < 196; i++) {
            ctx->confdatabits[k++] = ctx->info[i];
        }
        for (uint32_t i = 0; i < 7; i++) {
            ctx->confdatabits[k++] = ctx->info[i];
        }

        ctx->crc_computed = ComputeCrc9Bit(ctx->confdatabits, (uint32_t)k);
        if (ctx->crc_extracted == ctx->crc_computed) {
            ctx->crc_correct = 1;
            ctx->state->data_block_crc_valid[ctx->slot][ctx->blockcounter] = 1;
        } else {
            ctx->state->data_block_crc_valid[ctx->slot][ctx->blockcounter] = 0;
        }
    }

    DSD_MEMCPY(ctx->dmr_pdu_bits, ctx->info, sizeof(ctx->dmr_pdu_bits));
}

static int
dmr_dburst_update_dbsn_sequence(dmr_data_burst_ctx* ctx) {
    if ((ctx->databurst != 0x07 && ctx->databurst != 0x08 && ctx->databurst != 0x0A)
        || ctx->state->data_conf_data[ctx->slot] != 1 || !ctx->dbsn_valid) {
        return 1;
    }

    if (!ctx->state->data_dbsn_have[ctx->slot]) {
        if (ctx->crc_correct == 1 || ctx->opts->aggressive_framesync == 0) {
            ctx->state->data_dbsn_expected[ctx->slot] = (uint8_t)((ctx->dbsn_for_seq + 1) & 0x7F);
            ctx->state->data_dbsn_have[ctx->slot] = 1;
        }
        return 1;
    }

    if (ctx->crc_correct == 1 && ctx->dbsn_for_seq != ctx->state->data_dbsn_expected[ctx->slot]) {
        if (ctx->opts->aggressive_framesync == 1) {
            DSD_FPRINTF(stderr, "%s DBSN Seq Err: got %u expected %u %s", KRED, ctx->dbsn_for_seq,
                        ctx->state->data_dbsn_expected[ctx->slot], KNRM);
            dmr_reset_blocks(ctx->opts, ctx->state);
            return 0;
        }
    }

    if (ctx->crc_correct == 1) {
        ctx->state->data_dbsn_expected[ctx->slot] = (uint8_t)((ctx->dbsn_for_seq + 1) & 0x7F);
    }

    return 1;
}

static const char*
dmr_dburst_usbd_service_name(uint8_t service) {
    if (service <= 8) {
        static const char* names[9] = {
            "Location Information Protocol",
            "Standard Service 1",
            "Standard Service 2",
            "Standard Service 3",
            "Standard Service 4",
            "Standard Service 5",
            "Standard Service 6",
            "Standard Service 7",
            "Standard Service 8",
        };
        return names[service];
    }
    return (service <= 15) ? "Reserved (standard)" : "Manufacturer Specific";
}

static void
dmr_dburst_handle_usbd(dmr_data_burst_ctx* ctx) {
    int b;
    int k2;
    int i2;
    uint8_t tail4 = 0;
    uint8_t pl_bytes[11];

    ctx->usbd_st = (uint8_t)convert_bits_into_output(&ctx->dmr_pdu_bits[0], 4);
    DSD_FPRINTF(stderr, "%s\\n", KYEL);
    DSD_FPRINTF(stderr, " USBD - Service: %s (%u)", dmr_dburst_usbd_service_name(ctx->usbd_st), ctx->usbd_st);

    DSD_MEMSET(pl_bytes, 0, sizeof(pl_bytes));
    for (b = 0; b < 11; b++) {
        uint8_t v = 0;
        for (k2 = 0; k2 < 8; k2++) {
            v = (uint8_t)((v << 1) | (ctx->dmr_pdu_bits[4 + b * 8 + k2] & 1));
        }
        pl_bytes[b] = v;
    }
    for (k2 = 0; k2 < 4; k2++) {
        tail4 = (uint8_t)((tail4 << 1) | (ctx->dmr_pdu_bits[4 + 88 + k2] & 1));
    }

    DSD_FPRINTF(stderr, " - Payload: ");
    for (i2 = 0; i2 < 11; i2++) {
        DSD_FPRINTF(stderr, "[%02X]", pl_bytes[i2]);
    }
    DSD_FPRINTF(stderr, "[%1X]", tail4 & 0xF);

    if (ctx->usbd_st == 0) {
        lip_protocol_decoder(ctx->opts, ctx->state, ctx->dmr_pdu_bits);
    }
}

static void
dmr_dburst_dispatch_by_type(dmr_data_burst_ctx* ctx) {
    switch (ctx->databurst) {
        case 0x00: dmr_pi(ctx->opts, ctx->state, ctx->dmr_pdu, ctx->crc_correct, ctx->irrecoverable_errors); break;
        case 0x01:
            dmr_flco(ctx->opts, ctx->state, ctx->dmr_pdu_bits, ctx->crc_correct, &ctx->irrecoverable_errors, 1);
            break;
        case 0x02:
            dmr_flco(ctx->opts, ctx->state, ctx->dmr_pdu_bits, ctx->crc_correct, &ctx->irrecoverable_errors, 2);
            break;
        case 0x03:
            dmr_cspdu(ctx->opts, ctx->state, ctx->dmr_pdu_bits, ctx->dmr_pdu, ctx->crc_correct,
                      ctx->irrecoverable_errors);
            break;
        case 0x04:
            ctx->state->data_block_counter[ctx->slot] = 0;
            ctx->state->data_header_valid[ctx->slot] = 1;
            dmr_block_assembler(ctx->opts, ctx->state, ctx->dmr_pdu, ctx->pdu_len, ctx->databurst, 2);
            break;
        case 0x05: dmr_block_assembler(ctx->opts, ctx->state, ctx->dmr_pdu, ctx->pdu_len, ctx->databurst, 2); break;
        case 0x06:
            dmr_dheader(ctx->opts, ctx->state, ctx->dmr_pdu, ctx->dmr_pdu_bits, ctx->crc_correct,
                        ctx->irrecoverable_errors);
            break;
        case 0x07:
            dmr_block_assembler(ctx->opts, ctx->state, ctx->dmr_pdu, ctx->pdu_len, ctx->databurst, ctx->is_udt ? 3 : 1);
            break;
        case 0x08:
        case 0x0A: dmr_block_assembler(ctx->opts, ctx->state, ctx->dmr_pdu, ctx->pdu_len, ctx->databurst, 1); break;
        case 0x0B: dmr_dburst_handle_usbd(ctx); break;
        case 0xEB:
            dmr_flco(ctx->opts, ctx->state, ctx->dmr_pdu_bits, ctx->crc_correct, &ctx->irrecoverable_errors, 3);
            break;
        default: break;
    }
}

static void
dmr_dburst_finalize_status(dmr_data_burst_ctx* ctx) {
    if (ctx->is_ras == 1) {
        ctx->crc_correct = ctx->crc_original_validity;
    }

    if (ctx->irrecoverable_errors != 0 && ctx->databurst != 0x08 && ctx->databurst != 0x09) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " (FEC ERR)");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    if (ctx->is_ras == 1) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " -RAS ");
        if (ctx->opts->payload == 1) {
            DSD_FPRINTF(stderr, "%X ", ctx->bptc_reserved_bits);
        }
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    if (ctx->irrecoverable_errors == 0 && ctx->crc_correct == 0 && ctx->is_ras == 0 && ctx->databurst != 0x09
        && ctx->databurst != 0x05) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " (CRC ERR) ");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    if (ctx->opts->payload == 1 && ctx->databurst != 0x09) {
        DSD_FPRINTF(stderr, "\\n");
        DSD_FPRINTF(stderr, "%s", KCYN);
        DSD_FPRINTF(stderr, " DMR PDU Payload ");
        for (uint32_t i = 0; i < ctx->pdu_len; i++) {
            DSD_FPRINTF(stderr, "[%02X]", ctx->dmr_pdu[i]);
        }
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

void
dmr_data_burst_handler(dsd_opts* opts, dsd_state* state, uint8_t info[196], uint8_t databurst,
                       const uint8_t* reliab98) {
    dmr_data_burst_ctx ctx;
    dmr_dburst_ctx_init(&ctx, opts, state, info, databurst, reliab98);

    dmr_dburst_apply_base_profile(&ctx);
    dmr_dburst_apply_dynamic_profile(&ctx);

    if (!dmr_dburst_keeps_data_p_head(ctx.databurst)) {
        state->data_p_head[ctx.slot] = 0;
    }

    dmr_dburst_print_header_and_dump(&ctx);

    if (ctx.is_bptc) {
        dmr_dburst_handle_bptc(&ctx);
    }
    if (ctx.is_emb) {
        dmr_dburst_handle_emb(&ctx);
    }
    if (ctx.is_trellis) {
        dmr_dburst_handle_trellis(&ctx);
    }
    if (ctx.is_full) {
        dmr_dburst_handle_full(&ctx);
    }

    if (!dmr_dburst_update_dbsn_sequence(&ctx)) {
        return;
    }

    dmr_dburst_dispatch_by_type(&ctx);
    dmr_dburst_finalize_status(&ctx);
}
