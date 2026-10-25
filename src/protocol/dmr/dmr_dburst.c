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
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/fec/bptc.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/dmr/r34_viterbi.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dmr_crc_provenance.h"
#include "dmr_dburst_profile.h"
#include "dmr_r34_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

/** @brief Element count of the caller-owned info array (see dmr_data_burst_handler). */
#define DMR_DBURST_INFO_BITS 196U

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
    uint8_t ras_signature;  // dmr_ras_signature() holds for the burst, with or without -F
    uint8_t is_ras;         // accepted as RAS: a signature under -F
    uint8_t ras_announce;   // this signature confirmed RAS on the carrier while relaxed CRC is off
    uint8_t ras_row_strict; // ...with -F configured on and the scan row on air setting strict CRC itself
    uint32_t crc_original_validity;
    uint8_t crc_flags; // DSD_CRC_* bits for this burst, set once its CRC and RAS verdicts are final

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

void
dmr_dburst_profile_resolve(uint8_t databurst, uint8_t confirmed_data, uint8_t header_format, dmr_dburst_profile* out) {
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

    if (out == NULL) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));

    if (databurst <= 0x0B) {
        *out = profiles[databurst];
    } else if (databurst == 0xEB) {
        out->flags = DMR_DBURST_F_EMB;
        out->crclen = 5;
        out->pdu_len = 9;
    } else {
        out->subtype = " _UNK ";
        out->flags = DMR_DBURST_F_FULL;
        out->pdu_len = 25;
    }

    if (databurst == 0x07) {
        if (confirmed_data == 1) {
            out->pdu_len = 10;
            out->pdu_start = 2;
            out->subtype = " R12C ";
        }
        if (header_format == 0) {
            out->flags |= DMR_DBURST_F_UDT;
            if (confirmed_data == 1) {
                out->subtype = " UDTC ";
            } else {
                out->subtype = " UDTU ";
            }
        }
    } else if (databurst == 0x08) {
        if (confirmed_data == 1) {
            out->pdu_len = 16;
            out->pdu_start = 2;
            out->subtype = " R34C ";
        }
    } else if (databurst == 0x0A) {
        if (confirmed_data == 1) {
            out->pdu_len = 22;
            out->pdu_start = 2;
            out->subtype = " R_1C ";
        }
    }
}

static void
dmr_dburst_apply_profile(dmr_data_burst_ctx* ctx) {
    dmr_dburst_profile profile;
    dmr_dburst_profile_resolve(ctx->databurst, ctx->state->data_conf_data[ctx->slot],
                               ctx->state->data_header_format[ctx->slot], &profile);
    ctx->is_bptc = (profile.flags & DMR_DBURST_F_BPTC) != 0;
    ctx->is_trellis = (profile.flags & DMR_DBURST_F_TRELLIS) != 0;
    ctx->is_emb = (profile.flags & DMR_DBURST_F_EMB) != 0;
    ctx->is_lc = (profile.flags & DMR_DBURST_F_LC) != 0;
    ctx->is_full = (profile.flags & DMR_DBURST_F_FULL) != 0;
    ctx->is_udt = (profile.flags & DMR_DBURST_F_UDT) != 0;
    ctx->crclen = profile.crclen;
    ctx->crcmask = profile.crcmask;
    ctx->pdu_len = profile.pdu_len;
    ctx->pdu_start = profile.pdu_start;
    if (profile.subtype != NULL) {
        DSD_SNPRINTF(ctx->state->fsubtype, sizeof(ctx->state->fsubtype), "%s", profile.subtype);
    }
}

// Per-block CRC9 verdict, indexed by the 1-based block counter. A counter past the 127 blocks a header can
// announce has no row; such a block is never aggregated, so dropping its verdict loses nothing.
static void
dmr_dburst_set_block_verdict(const dmr_data_burst_ctx* ctx, uint8_t verdict) {
    if (ctx->slot < 2U && ctx->blockcounter < sizeof(ctx->state->data_block_crc_valid[0])) {
        ctx->state->data_block_crc_valid[ctx->slot][ctx->blockcounter] = verdict;
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
            DSD_FPRINTF(pfile, "\n%d 98 ", ctx->slot + 1);
            for (i = 0; i < 6; i++) {
                int cach_byte = (ctx->state->dmr_stereo_payload[((size_t)i * 2)] << 2)
                                | ctx->state->dmr_stereo_payload[((size_t)i * 2) + 1];
                DSD_FPRINTF(pfile, "%X", cach_byte);
            }
            DSD_FPRINTF(pfile, "\n%d %02X ", ctx->slot + 1, ctx->databurst);
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
        dmr_dburst_set_block_verdict(ctx, 1);
    } else {
        dmr_dburst_set_block_verdict(ctx, 0);
    }
}

static void
dmr_dburst_handle_bptc_crc(dmr_data_burst_ctx* ctx) {
    if (ctx->is_lc && ctx->bptc_reserved_bits != 0) {
        // Reserved bits set: possibly a RAS LC, whose private parity ordinary RS correction would "correct" into a
        // different, valid LC. Check the syndrome only and never correct.
        ctx->crc_correct = CheckFullLinkControlCrc(ctx->bptc_data_bytes, ctx->crcmask);
        ctx->crc_computed = (((uint32_t)ctx->bptc_data_bytes[9] << 16) | ((uint32_t)ctx->bptc_data_bytes[10] << 8)
                             | (uint32_t)ctx->bptc_data_bytes[11])
                            ^ ctx->crcmask;
        return;
    }
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
    // Bound the copy by the source remainder and the unpacked bit buffer so the span stays
    // provable locally rather than relying on the burst profile table.
    const size_t src_cap = sizeof(ctx->bptc_data_bytes);
    if ((size_t)ctx->pdu_start >= src_cap) {
        return; // Nothing left to copy; keep the offset below the array so no one-past pointer forms.
    }
    const size_t start = (size_t)ctx->pdu_start;

    const size_t max_bytes =
        dsd_unpack_bytes_to_bits(ctx->bptc_data_bytes + start, src_cap - start, ctx->bptc_data_bits,
                                 sizeof(ctx->bptc_data_bits), (size_t)ctx->pdu_len);

    for (size_t i = 0U; i < max_bytes; i++) {
        ctx->dmr_pdu[i] = ctx->bptc_data_bytes[i + start];
    }
    for (size_t i = 0U; i < max_bytes * 8U; i++) {
        ctx->dmr_pdu_bits[i] = ctx->bptc_data_bits[i];
    }
}

/* Reads R, the payload octets and the CRC field from bptc_data_bits / r and runs the burst's standard check. */
static void
dmr_dburst_check_bptc_payload(dmr_data_burst_ctx* ctx) {
    uint32_t i;

    ctx->bptc_reserved_bits = (ctx->r[0] & 0x01) | ((ctx->r[1] << 1) & 0x02) | ((ctx->r[2] << 2) & 0x04);

    dmr_dburst_unpack_bptc_bytes(ctx);

    ctx->crc_extracted = 0;
    for (i = 0; i < ctx->crclen; i++) {
        ctx->crc_extracted = (ctx->crc_extracted << 1) | (uint32_t)(ctx->bptc_data_bits[i + 96 - ctx->crclen] & 1);
    }
    ctx->crc_extracted ^= ctx->crcmask;

    dmr_dburst_handle_bptc_crc(ctx);
}

static int
dmr_ras_eligible_type(uint8_t databurst, uint8_t confirmed_data) {
    switch (databurst) {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x06:
        case 0x0B: return 1;
        case 0x07: return confirmed_data == 1;
        default: return 0;
    }
}

int
dmr_ras_signature(uint8_t databurst, uint8_t confirmed_data, const uint8_t* payload_bytes, uint8_t r_bits,
                  uint32_t distance, int standard_check_ok) {
    if (payload_bytes == NULL || !dmr_ras_eligible_type(databurst, confirmed_data) || r_bits == 0
        || distance > DSD_BPTC_196x96_UNIQUE_RADIUS || standard_check_ok) {
        return 0;
    }

    // Byte 1 is a FID/MFID only in some layouts; 0x68 there is Hytera, whose checks fail for its own reasons.
    const uint8_t fid = payload_bytes[1];
    switch (databurst) {
        // PI: dmr_pi() reads the MFID from octet 1; only Motorola's (0x10) PI is RAS.
        case 0x00: return fid == 0x10;
        // Voice LC header and terminator: the FID is LC bits 8..15 (dmr_flco.c); CSBK and MBC header: cs_pdu[1]
        // (dmr_cspdu()).
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04: return fid != 0x68;
        // Data header: the DPF is bits 4..7 and, in a proprietary (DPF 15) header only, the MFID bits 8..15
        // (dmr_dheader_parse_fields()); elsewhere byte 1 is SAP and pad-octet count.
        case 0x06: return !((payload_bytes[0] & 0x0F) == 0x0F && fid == 0x68);
        // USBD and confirmed blocks carry payload in byte 1.
        default: return 1;
    }
}

/* A RAS candidate (reserved bits set, standard check failed): decode the burst to a full codeword. Within
   DSD_BPTC_196x96_UNIQUE_RADIUS that codeword is the unique nearest one, so its payload, R and check replace the
   extractor's best effort and its signature is recorded; further away, or none, the burst stays as extracted. */
static void
dmr_dburst_decode_ras_candidate(dmr_data_burst_ctx* ctx) {
    uint8_t bits[96];
    uint8_t r[3];
    uint32_t distance = 0;
    if (BPTC_196x96_Decode_Codeword(ctx->bptc_deinterleaved, bits, r, &distance) == 0
        || distance > DSD_BPTC_196x96_UNIQUE_RADIUS) {
        return;
    }
    // At distance 0 the received word is the codeword, so the extractor already gave its payload, R and check.
    if (distance != 0U) {
        DSD_MEMCPY(ctx->bptc_data_bits, bits, sizeof(ctx->bptc_data_bits));
        DSD_MEMCPY(ctx->r, r, sizeof(ctx->r));
        ctx->irrecoverable_errors = 0;
        dmr_dburst_check_bptc_payload(ctx);
    }
    ctx->ras_signature =
        (uint8_t)dmr_ras_signature(ctx->databurst, ctx->state->data_conf_data[ctx->slot], ctx->bptc_data_bytes,
                                   ctx->bptc_reserved_bits, distance, ctx->crc_correct != 0);
}

static void
dmr_dburst_handle_bptc(dmr_data_burst_ctx* ctx) {
    ctx->crc_computed = 0;
    ctx->irrecoverable_errors = 0;

    BPTCDeInterleaveDMRData(ctx->info, ctx->bptc_deinterleaved);
    ctx->irrecoverable_errors = BPTC_196x96_Extract_Data(ctx->bptc_deinterleaved, ctx->bptc_data_bits, ctx->r);
    dmr_dburst_check_bptc_payload(ctx);

    // Only bursts with R != 0 whose check failed pay for the full decode; every other burst keeps the extractor's.
    if (ctx->bptc_reserved_bits != 0 && ctx->crc_correct == 0
        && dmr_ras_eligible_type(ctx->databurst, ctx->state->data_conf_data[ctx->slot])) {
        dmr_dburst_decode_ras_candidate(ctx);
    }
    ctx->crc_original_validity = ctx->crc_correct;

    if (ctx->ras_signature && ctx->opts->aggressive_framesync == 0) {
        ctx->is_ras = 1;
    }
    if (ctx->is_ras == 1) {
        // Retain the standard CRC verdict separately from the RAS acceptance.
        ctx->crc_correct = 1;
        if (ctx->databurst == 0x07 && ctx->state->data_conf_data[ctx->slot] == 1) {
            // The CRC9 verdict stays "not verified" for the location gates, but the event marker reads it as RAS.
            dmr_dburst_set_block_verdict(ctx, DMR_DATA_BLOCK_CRC_RAS);
        }
    }

    if (ctx->databurst == 0x04 || ctx->databurst == 0x06) {
        ctx->state->data_block_crc_valid[ctx->slot][0] = (ctx->crc_original_validity != 0);
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
    dsd_unpack_bytes_to_bits(bytes18, 18U, ctx->dmr_pdu_bits, sizeof(ctx->dmr_pdu_bits), 18U);

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
dmr_dburst_trellis_lower_metric(const dmr_r34_candidate* candidates, int current, int candidate) {
    if (current < 0 || candidates[candidate].metric < candidates[current].metric) {
        return candidate;
    }
    return current;
}

static int
dmr_dburst_trellis_choose_candidate_index(dmr_data_burst_ctx* ctx, const dmr_r34_candidate* candidates,
                                          int candidate_count) {
    const int have_expected_dbsn = ctx->state->data_dbsn_have[ctx->slot] != 0;
    const uint8_t expected_dbsn = ctx->state->data_dbsn_expected[ctx->slot];
    int best_crc_dbsn = -1;
    int best_crc = -1;
    int best_dbsn = -1;
    int best_any = -1;
    int ci;

    for (ci = 0; ci < candidate_count; ci++) {
        uint8_t cand_dbsn = 0;
        int cand_crc_ok = 0;
        dmr_dburst_trellis_candidate_metrics(ctx, candidates[ci].bytes18, &cand_dbsn, &cand_crc_ok);
        best_any = dmr_dburst_trellis_lower_metric(candidates, best_any, ci);

        if (have_expected_dbsn && cand_dbsn == expected_dbsn) {
            if (cand_crc_ok) {
                best_crc_dbsn = dmr_dburst_trellis_lower_metric(candidates, best_crc_dbsn, ci);
            } else {
                best_dbsn = dmr_dburst_trellis_lower_metric(candidates, best_dbsn, ci);
            }
        }

        if (cand_crc_ok) {
            best_crc = dmr_dburst_trellis_lower_metric(candidates, best_crc, ci);
        }
    }

    if (best_crc_dbsn >= 0) {
        return best_crc_dbsn;
    }
    if (best_crc >= 0) {
        return best_crc;
    }
    if (best_dbsn >= 0) {
        return best_dbsn;
    }
    return best_any;
}

static void
dmr_dburst_trellis_add_candidate(const dmr_data_burst_ctx* ctx, const uint8_t tdibits[98],
                                 dmr_r34_candidate candidates[], int capacity, int* candidate_count,
                                 const uint8_t bytes18[18]) {
    if (*candidate_count >= capacity) {
        return;
    }
    for (int i = 0; i < *candidate_count; i++) {
        if (memcmp(candidates[i].bytes18, bytes18, 18) == 0) {
            return;
        }
    }

    int metric = 0;
    if (dmr_r34_candidate_metric(tdibits, ctx->reliab98, bytes18, &metric) != 0) {
        return;
    }
    candidates[*candidate_count].metric = metric;
    DSD_MEMCPY(candidates[*candidate_count].bytes18, bytes18, 18);
    (*candidate_count)++;
}

static int
dmr_dburst_trellis_lowest_metric_index(const dmr_r34_candidate candidates[], int candidate_count) {
    int best = -1;
    for (int i = 0; i < candidate_count; i++) {
        best = dmr_dburst_trellis_lower_metric(candidates, best, i);
    }
    return best;
}

static void
dmr_dburst_pick_trellis_payload(dmr_data_burst_ctx* ctx, const uint8_t tdibits[98], uint8_t trellis_return[18]) {
    enum { R34_LIST_CAPACITY = 32, R34_POOL_CAPACITY = R34_LIST_CAPACITY + 2 };

    dmr_r34_candidate candidates[R34_POOL_CAPACITY];
    dmr_r34_candidate list[R34_LIST_CAPACITY];
    uint8_t decoded[18];
    int candidate_count = 0;
    int list_count = 0;

    DSD_MEMSET(candidates, 0, sizeof(candidates));
    DSD_MEMSET(list, 0, sizeof(list));
    DSD_MEMSET(decoded, 0, sizeof(decoded));

    if (dmr_r34_viterbi_decode(tdibits, decoded) == 0) {
        dmr_dburst_trellis_add_candidate(ctx, tdibits, candidates, R34_POOL_CAPACITY, &candidate_count, decoded);
    }
    if (ctx->reliab98 != NULL && dmr_r34_viterbi_decode_soft(tdibits, ctx->reliab98, decoded) == 0) {
        dmr_dburst_trellis_add_candidate(ctx, tdibits, candidates, R34_POOL_CAPACITY, &candidate_count, decoded);
    }
    if ((ctx->state->data_conf_data[ctx->slot] == 1 || ctx->reliab98 == NULL)
        && dmr_r34_viterbi_decode_list(tdibits, ctx->reliab98, list, R34_LIST_CAPACITY, &list_count) == 0) {
        for (int i = 0; i < list_count; i++) {
            dmr_dburst_trellis_add_candidate(ctx, tdibits, candidates, R34_POOL_CAPACITY, &candidate_count,
                                             list[i].bytes18);
        }
    }

    int chosen = (ctx->state->data_conf_data[ctx->slot] == 1)
                     ? dmr_dburst_trellis_choose_candidate_index(ctx, candidates, candidate_count)
                     : dmr_dburst_trellis_lowest_metric_index(candidates, candidate_count);
    if (chosen >= 0) {
        DSD_MEMCPY(trellis_return, candidates[chosen].bytes18, 18);
    }
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
        dmr_dburst_set_block_verdict(ctx, 1);
    } else {
        dmr_dburst_set_block_verdict(ctx, 0);
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

    DSD_UNPACK_ARRAY_TO_BITS(trellis_return, ctx->dmr_pdu_bits, 18);
    if (ctx->state->data_conf_data[ctx->slot] == 1) {
        ctx->dbsn_for_seq = (uint8_t)convert_bits_into_output(&ctx->dmr_pdu_bits[0], 7);
        ctx->dbsn_valid = 1;
    }

    dmr_dburst_trellis_update_confirmed_crc(ctx);

    DSD_MEMSET(ctx->dmr_pdu_bits, 0, sizeof(ctx->dmr_pdu_bits));
    // Guard the offset so the remaining source span cannot underflow; the helper takes it from there.
    const size_t tr_cap = sizeof(trellis_return);
    const size_t start = (size_t)ctx->pdu_start < tr_cap ? (size_t)ctx->pdu_start : tr_cap;
    dsd_unpack_bytes_to_bits(trellis_return + start, tr_cap - start, ctx->dmr_pdu_bits, sizeof(ctx->dmr_pdu_bits),
                             (size_t)ctx->pdu_len);
}

static void
dmr_dburst_handle_full(dmr_data_burst_ctx* ctx) {
    ctx->crc_computed = 0;
    ctx->irrecoverable_errors = 0;

    // ctx->info spans DMR_DBURST_INFO_BITS elements by contract; state the remaining span at each copy.
    const size_t head_bytes = (size_t)ctx->pdu_start < 12U ? 12U - (size_t)ctx->pdu_start : 0U;
    const size_t head_bits = (size_t)ctx->pdu_start * 8U;
    const size_t head_avail = head_bits < DMR_DBURST_INFO_BITS ? DMR_DBURST_INFO_BITS - head_bits : 0U;
    dsd_pack_bits_to_bytes(ctx->info + head_bits, head_avail, ctx->dmr_pdu, sizeof(ctx->dmr_pdu), head_bytes);
    dsd_pack_bits_to_bytes(ctx->info + 100, DMR_DBURST_INFO_BITS - 100U, ctx->dmr_pdu + head_bytes,
                           sizeof(ctx->dmr_pdu) - head_bytes, 12U);

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
            dmr_dburst_set_block_verdict(ctx, 1);
        } else {
            dmr_dburst_set_block_verdict(ctx, 0);
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
    DSD_FPRINTF(stderr, "%s\n", KYEL);
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

    // Strict CRC publishes no failed DMR data, the same gate dmr_block.c applies; -F decodes it marked.
    const int crc_rejected =
        ctx->crc_correct == 0 && ctx->opts->aggressive_framesync != 0 && !ctx->opts->dmr_crc_relaxed_default;
    if (ctx->usbd_st == 0 && !crc_rejected) {
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
            // Arm only a checked header or a RAS acceptance under -F. The raw CRC
            // remains in data_block_crc_valid; acceptance is not an integrity verdict.
            ctx->state->data_header_valid[ctx->slot] =
                (uint8_t)(ctx->crc_correct != 0 && ctx->irrecoverable_errors == 0);
            ctx->state->data_header_crc_invalid[ctx->slot] = ctx->crc_flags;
            dmr_block_assembler(ctx->opts, ctx->state, ctx->dmr_pdu, ctx->pdu_len, ctx->databurst, 2);
            break;
        case 0x05: dmr_block_assembler(ctx->opts, ctx->state, ctx->dmr_pdu, ctx->pdu_len, ctx->databurst, 2); break;
        case 0x06:
            dmr_dheader_classified(ctx->opts, ctx->state, ctx->dmr_pdu, ctx->dmr_pdu_bits, ctx->crc_flags,
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
        DSD_FPRINTF(stderr, "\n");
        DSD_FPRINTF(stderr, "%s", KCYN);
        DSD_FPRINTF(stderr, " DMR PDU Payload ");
        for (uint32_t i = 0; i < ctx->pdu_len; i++) {
            DSD_FPRINTF(stderr, "[%02X]", ctx->dmr_pdu[i]);
        }
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

/* A burst that proves the carrier is DMR, so two-slot TDMA (dsd_state::tdma_carrier): one whose own 16- or 24-bit
   check passed (PI, voice and terminator LC, CSBK, MBC and data headers, USBD), not a RAS acceptance. This is how a
   direct-mode or mobile carrier proves it, which the base station colour-code lock (dmr_confidence.c) never sees. Data
   blocks, MBC continuations, idle bursts and the embedded LC's 5-bit CRC are left out: their checks are absent or too
   short to tell noise from a DMR burst. */
static int
dmr_dburst_proves_tdma_carrier(const dmr_data_burst_ctx* ctx) {
    switch (ctx->databurst) {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x06:
        case 0x0B: break;
        default: return 0;
    }
    return ctx->crc_original_validity != 0U && ctx->is_ras == 0U && ctx->irrecoverable_errors == 0U
           && ctx->state->input_interrupted == 0;
}

/* The carrier's RAS evidence (dmr_ras.c), noted before the burst dispatches: a retune its handler makes resets it with
   the carrier left. The second signature confirms RAS; without relaxed CRC the operator is told once that -F decodes
   what this carrier sends, or, with -F configured under a scan row's own --strict-crc, that the channel keeps strict
   CRC. */
static void
dmr_dburst_note_ras_signature(dmr_data_burst_ctx* ctx) {
    if (!ctx->ras_signature) {
        return;
    }
    const int was_confirmed = dmr_ras_confirmed(ctx->state);
    dmr_ras_note_signature(ctx->state);
    ctx->ras_announce = (uint8_t)(!was_confirmed && dmr_ras_confirmed(ctx->state) && !dsd_opts_relaxed_crc(ctx->opts));
    ctx->ras_row_strict = (uint8_t)(ctx->ras_announce && dsd_scan_mode_configured_relaxed_crc(ctx->opts, ctx->state));
}

static void
dmr_dburst_announce_ras(const dmr_data_burst_ctx* ctx) {
    if (ctx->ras_announce) {
        DSD_FPRINTF(stderr,
                    "\n%sDMR RAS (Restricted Access to System) detected: control and data fail the standard CRC; "
                    "-F decodes them%s%s",
                    KYEL, ctx->ras_row_strict ? ", but this channel sets strict CRC itself" : "", KNRM);
    }
}

void
dmr_data_burst_handler(dsd_opts* opts, dsd_state* state, uint8_t info[196], uint8_t databurst,
                       const uint8_t* reliab98) {
    dmr_data_burst_ctx ctx;
    dmr_dburst_ctx_init(&ctx, opts, state, info, databurst, reliab98);

    dmr_dburst_apply_profile(&ctx);

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

    dmr_dburst_note_ras_signature(&ctx);

    if (!dmr_dburst_update_dbsn_sequence(&ctx)) {
        dmr_dburst_announce_ras(&ctx);
        return;
    }

    // MBC continuation bursts have no independent CRC; their aggregate is checked by the assembler.
    if (ctx.databurst != 0x05) {
        if (ctx.is_ras) {
            ctx.crc_flags = DSD_CRC_RAS_UNVERIFIED;
        } else if (ctx.crc_correct == 0) {
            ctx.crc_flags = DSD_CRC_FAILED;
        }
    }
    if (dmr_dburst_proves_tdma_carrier(&ctx)) {
        state->tdma_carrier = 1U;
    }
    const uint8_t saved_crc_invalid = state->event_crc_invalid[ctx.slot];
    state->event_crc_invalid[ctx.slot] = (uint8_t)(saved_crc_invalid | ctx.crc_flags);
    dmr_dburst_dispatch_by_type(&ctx);
    state->event_crc_invalid[ctx.slot] = saved_crc_invalid;
    dmr_dburst_finalize_status(&ctx);
    dmr_dburst_announce_ras(&ctx);
}
