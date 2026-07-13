// SPDX-License-Identifier: ISC
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

#include <dsd-neo/core/bit_packing.h>

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/protocol/dpmr/dpmr_const.h>
#include <dsd-neo/protocol/dpmr/dpmr_data.h>
#include <dsd-neo/runtime/colors.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dpmr_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/secret_redaction.h"
#include "dsd-neo/core/state_fwd.h"

typedef struct {
    uint8_t CCH[NB_OF_DPMR_VOICE_FRAME_TO_DECODE][72];
    uint8_t CCHDescrambled[NB_OF_DPMR_VOICE_FRAME_TO_DECODE][72];
    uint8_t CCHDeInterleaved[NB_OF_DPMR_VOICE_FRAME_TO_DECODE][72];
    uint8_t CCHDataHammingCorrected[NB_OF_DPMR_VOICE_FRAME_TO_DECODE][48];
    uint8_t CCHDataCRC[NB_OF_DPMR_VOICE_FRAME_TO_DECODE];
    uint8_t CCHDataCRCComputed[NB_OF_DPMR_VOICE_FRAME_TO_DECODE];
    uint8_t CC[NB_OF_DPMR_VOICE_FRAME_TO_DECODE / 2][24];
    char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24];
    bool HammingCorrectable[NB_OF_DPMR_VOICE_FRAME_TO_DECODE][6];
    uint32_t CrcOk[NB_OF_DPMR_VOICE_FRAME_TO_DECODE];
    uint32_t HammingOk[NB_OF_DPMR_VOICE_FRAME_TO_DECODE];
    uint32_t CCH_FrameNumber[NB_OF_DPMR_VOICE_FRAME_TO_DECODE];
    uint32_t CCH_CommunicationMode[NB_OF_DPMR_VOICE_FRAME_TO_DECODE];
    uint32_t CCH_Version[NB_OF_DPMR_VOICE_FRAME_TO_DECODE];
    uint32_t CCH_CommsFormat[NB_OF_DPMR_VOICE_FRAME_TO_DECODE];
    uint32_t CCH_EmergencyPriority[NB_OF_DPMR_VOICE_FRAME_TO_DECODE];
    uint32_t CCH_Reserved[NB_OF_DPMR_VOICE_FRAME_TO_DECODE];
    uint32_t CCH_SlowData[NB_OF_DPMR_VOICE_FRAME_TO_DECODE];
} dpmr_voice_ctx_t;

static uint32_t
dpmr_read_dibit(dsd_opts* opts, dsd_state* state) {
    uint32_t dibit = get_dibit_and_analog_signal(opts, state, NULL);
    if (opts->inverted_dpmr == 1) {
        dibit ^= 2U;
    }
    return dibit;
}

/*
 * Mark processdPMRvoice helper roots used by the public decoder entrypoint.
 * CodeQL's manual C/C++ database can miss this local call chain.
 */
static void DSD_ATTR_USED
dpmr_read_first_cch(dsd_opts* opts, dsd_state* state, dpmr_voice_ctx_t* ctx) {
    for (uint32_t i = 0; i < 36; i++) {
        uint32_t dibit = dpmr_read_dibit(opts, state);
        ctx->CCH[0][((size_t)i * 2)] = (uint8_t)(1U & (dibit >> 1)); // bit 1
        ctx->CCH[0][((size_t)i * 2) + 1] = (uint8_t)(1U & dibit);    // bit 0
    }
}

static void DSD_ATTR_USED
dpmr_read_tch_group(dsd_opts* opts, dsd_state* state, dpmr_voice_ctx_t* ctx, uint32_t frame_base) {
    for (uint32_t j = 0; j < 4; j++) {
        const int* w = dpmr_ambe_interleave_w;
        const int* x = dpmr_ambe_interleave_x;
        const int* y = dpmr_ambe_interleave_y;
        const int* z = dpmr_ambe_interleave_z;
        uint32_t k = 0;
        for (uint32_t i = 0; i < 36; i++) {
            uint32_t dibit = dpmr_read_dibit(opts, state);
            ctx->ambe_fr[frame_base + j][*w][*x] = (char)(1U & (dibit >> 1)); // bit 1
            ctx->ambe_fr[frame_base + j][*y][*z] = (char)(1U & dibit);        // bit 0
            state->dPMRVoiceFS2Frame.RawVoiceBit[frame_base + j][k] = (uint8_t)(1U & (dibit >> 1));
            state->dPMRVoiceFS2Frame.RawVoiceBit[frame_base + j][k + 1] = (uint8_t)(1U & dibit);
            k += 2;
            w++;
            x++;
            y++;
            z++;
        }
    }
}

static void DSD_ATTR_USED
dpmr_read_first_cc(dsd_opts* opts, dsd_state* state, dpmr_voice_ctx_t* ctx) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < 12; i++) {
        uint32_t dibit = dpmr_read_dibit(opts, state);
        ctx->CC[0][k++] = (uint8_t)(1U & (dibit >> 1)); // bit 1
        ctx->CC[0][k++] = (uint8_t)(1U & dibit);        // bit 0
    }
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)GetdPmrColorCode(ctx->CC[0]);
}

static void DSD_ATTR_USED
dpmr_read_second_cch(dsd_opts* opts, dsd_state* state, dpmr_voice_ctx_t* ctx) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < 36; i++) {
        uint32_t dibit = dpmr_read_dibit(opts, state);
        ctx->CCH[1][k++] = (uint8_t)(1U & (dibit >> 1)); // bit 1
        ctx->CCH[1][k++] = (uint8_t)(1U & dibit);        // bit 0
    }
}

uint8_t
dpmr_extract_cch_crc(const uint8_t cch_bits[48]) {
    uint8_t crc = 0;
    crc |= (uint8_t)(cch_bits[41] << 6);
    crc |= (uint8_t)(cch_bits[42] << 5);
    crc |= (uint8_t)(cch_bits[43] << 4);
    crc |= (uint8_t)(cch_bits[44] << 3);
    crc |= (uint8_t)(cch_bits[45] << 2);
    crc |= (uint8_t)(cch_bits[46] << 1);
    crc |= (uint8_t)(cch_bits[47] << 0);
    return crc;
}

static void DSD_ATTR_USED
dpmr_decode_cch_frames(dsd_state* state, dpmr_voice_ctx_t* ctx) {
    for (uint32_t i = 0; i < NB_OF_DPMR_VOICE_FRAME_TO_DECODE; i++) {
        uint32_t scrambler_lfsr = 0x1FF;
        dpmr_scrambled_pmr_bits(&scrambler_lfsr, ctx->CCH[i], ctx->CCHDescrambled[i], 72);
        dpmr_deinterleave_6x12(ctx->CCHDescrambled[i], ctx->CCHDeInterleaved[i]);

        bool correctable = true;
        for (uint32_t j = 0; j < 6; j++) {
            ctx->HammingCorrectable[i][j] = Hamming_12_8_decode(&ctx->CCHDeInterleaved[i][(size_t)j * 12u],
                                                                &ctx->CCHDataHammingCorrected[i][(size_t)j * 8u], 1);
            if (ctx->HammingCorrectable[i][j] == false) {
                correctable = false;
            }
        }

        ctx->HammingOk[i] = correctable ? 1U : 0U;
        ctx->CCHDataCRC[i] = dpmr_extract_cch_crc(ctx->CCHDataHammingCorrected[i]);
        ctx->CCHDataCRCComputed[i] = dpmr_crc7(ctx->CCHDataHammingCorrected[i], 41);
        ctx->CrcOk[i] = (ctx->CCHDataCRC[i] == ctx->CCHDataCRCComputed[i]) ? 1U : 0U;

        ctx->CCH_FrameNumber[i] = convert_bits_into_output(&ctx->CCHDataHammingCorrected[i][0], 2);
        ctx->CCH_CommunicationMode[i] = convert_bits_into_output(&ctx->CCHDataHammingCorrected[i][14], 3);
        ctx->CCH_Version[i] = convert_bits_into_output(&ctx->CCHDataHammingCorrected[i][17], 2);
        ctx->CCH_CommsFormat[i] = convert_bits_into_output(&ctx->CCHDataHammingCorrected[i][19], 2);
        ctx->CCH_EmergencyPriority[i] = (uint32_t)ctx->CCHDataHammingCorrected[i][21];
        ctx->CCH_Reserved[i] = (uint32_t)ctx->CCHDataHammingCorrected[i][22];
        ctx->CCH_SlowData[i] = convert_bits_into_output(&ctx->CCHDataHammingCorrected[i][23], 18);

        DSD_MEMCPY(state->dPMRVoiceFS2Frame.CCHData[i], ctx->CCHDataHammingCorrected[i], 48);
        state->dPMRVoiceFS2Frame.CCHDataHammingOk[i] = ctx->HammingOk[i];
        state->dPMRVoiceFS2Frame.CCHDataCRC[i] = ctx->CCHDataCRC[i];
        state->dPMRVoiceFS2Frame.CCHDataCrcOk[i] = ctx->CrcOk[i];
        state->dPMRVoiceFS2Frame.FrameNumbering[i] = ctx->CCH_FrameNumber[i];
        state->dPMRVoiceFS2Frame.CommunicationMode[i] = ctx->CCH_CommunicationMode[i];
        state->dPMRVoiceFS2Frame.Version[i] = ctx->CCH_Version[i];
        state->dPMRVoiceFS2Frame.CommsFormat[i] = ctx->CCH_CommsFormat[i];
        state->dPMRVoiceFS2Frame.EmergencyPriority[i] = ctx->CCH_EmergencyPriority[i];
        state->dPMRVoiceFS2Frame.Reserved[i] = ctx->CCH_Reserved[i];
        state->dPMRVoiceFS2Frame.SlowData[i] = ctx->CCH_SlowData[i];
    }
}

static void DSD_ATTR_USED
dpmr_extract_previous_ids(const dsd_state* state, char called_id[8], char calling_id[8]) {
    dsd_strncpy_s(called_id, 8, (const char*)state->dPMRVoiceFS2Frame.CalledID, 7);
    called_id[7] = '\0';
    dsd_strncpy_s(calling_id, 8, (const char*)state->dPMRVoiceFS2Frame.CallingID, 7);
    calling_id[7] = '\0';
}

static void DSD_ATTR_USED
dpmr_extract_superframe_part(const dpmr_voice_ctx_t* ctx, dpmr_superframe_part* part) {
    uint32_t first_half = convert_bits_into_output(&ctx->CCHDataHammingCorrected[0][2], 12);
    uint32_t second_half = convert_bits_into_output(&ctx->CCHDataHammingCorrected[1][2], 12);

    part->frame_number[0] = ctx->CCH_FrameNumber[0];
    part->frame_number[1] = ctx->CCH_FrameNumber[1];
    part->id_value = ((first_half << 12U) & 0x00FFF000U) | (second_half & 0x00000FFFU);
    part->crc_ok[0] = ctx->CrcOk[0] != 0U;
    part->crc_ok[1] = ctx->CrcOk[1] != 0U;
    part->hamming_ok[0][0] = ctx->HammingCorrectable[0][0];
    part->hamming_ok[0][1] = ctx->HammingCorrectable[0][1];
    part->hamming_ok[1][0] = ctx->HammingCorrectable[1][0];
    part->hamming_ok[1][1] = ctx->HammingCorrectable[1][1];
}

static int
dpmr_ids_are_strong(const dpmr_superframe_part* part) {
    return (part->crc_ok[0] || (part->hamming_ok[0][0] && part->hamming_ok[0][1]))
           && (part->crc_ok[1] || (part->hamming_ok[1][0] && part->hamming_ok[1][1]));
}

static void
dpmr_update_called_id(dsd_opts* opts, dsd_state* state, uint32_t id_value, int id_is_strong) {
    char called_id[8];
    dpmr_convert_air_interface_id(id_value, called_id);
    state->dPMRVoiceFS2Frame.CalledIDOk = id_is_strong ? 1U : 0U;
    dsd_strncpy_s((char*)state->dPMRVoiceFS2Frame.CalledID, sizeof(state->dPMRVoiceFS2Frame.CalledID), called_id,
                  sizeof(state->dPMRVoiceFS2Frame.CalledID) - 1);
    state->dPMRVoiceFS2Frame.CalledID[sizeof(state->dPMRVoiceFS2Frame.CalledID) - 1] = '\0';
    opts->dPMR_next_part_of_superframe = 2;
}

static void
dpmr_update_calling_id(dsd_opts* opts, dsd_state* state, uint32_t id_value, int id_is_strong) {
    char calling_id[8];
    dpmr_convert_air_interface_id(id_value, calling_id);
    state->dPMRVoiceFS2Frame.CallingIDOk = id_is_strong ? 1U : 0U;
    dsd_strncpy_s((char*)state->dPMRVoiceFS2Frame.CallingID, sizeof(state->dPMRVoiceFS2Frame.CallingID), calling_id,
                  sizeof(state->dPMRVoiceFS2Frame.CallingID) - 1);
    state->dPMRVoiceFS2Frame.CallingID[sizeof(state->dPMRVoiceFS2Frame.CallingID) - 1] = '\0';
    opts->dPMR_next_part_of_superframe = 1;
}

static void
dpmr_mark_unknown_superframe_part(dsd_opts* opts, dsd_state* state) {
    state->dPMRVoiceFS2Frame.CalledIDOk = 0;
    state->dPMRVoiceFS2Frame.CallingIDOk = 0;
    if (opts->dPMR_next_part_of_superframe == 1) {
        opts->dPMR_next_part_of_superframe = 2;
    } else if (opts->dPMR_next_part_of_superframe == 2) {
        opts->dPMR_next_part_of_superframe = 1;
    } else {
        opts->dPMR_next_part_of_superframe = 0;
    }
}

void
dpmr_update_superframe_part(dsd_opts* opts, dsd_state* state, const dpmr_superframe_part* part) {
    int id_is_strong = dpmr_ids_are_strong(part);
    if (((part->crc_ok[0] || part->hamming_ok[0][0]) && (part->frame_number[0] == 0))
        || ((part->crc_ok[1] || part->hamming_ok[1][0]) && (part->frame_number[1] == 1))) {
        dpmr_update_called_id(opts, state, part->id_value, id_is_strong);
        return;
    }
    if (((part->crc_ok[0] || part->hamming_ok[0][0]) && (part->frame_number[0] == 2))
        || ((part->crc_ok[1] || part->hamming_ok[1][0]) && (part->frame_number[1] == 3))) {
        dpmr_update_calling_id(opts, state, part->id_value, id_is_strong);
        return;
    }
    dpmr_mark_unknown_superframe_part(opts, state);
}

void
dpmr_print_ids(dsd_state* state, const char called_id[8], const char calling_id[8]) {
    DSD_FPRINTF(stderr, "\n");
    if (state->dPMRVoiceFS2Frame.CalledIDOk) {
        DSD_FPRINTF(stderr, "%s", KGRN);
        DSD_FPRINTF(stderr, " TG=%s", called_id);
        DSD_FPRINTF(stderr, "%s", KNRM);
        DSD_SNPRINTF(state->dpmr_target_id, sizeof state->dpmr_target_id, "%s", called_id);
    } else {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " TG=(CRC ERR)");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    if (state->dPMRVoiceFS2Frame.CallingIDOk) {
        DSD_FPRINTF(stderr, "%s", KGRN);
        DSD_FPRINTF(stderr, " Src=%s", calling_id);
        DSD_FPRINTF(stderr, "%s", KNRM);
        if (state->dPMRVoiceFS2Frame.CalledIDOk) {
            DSD_SNPRINTF(state->dpmr_caller_id, sizeof state->dpmr_caller_id, "%s", calling_id);
        }
        if (state->dPMRVoiceFS2Frame.ColorCode[0] != (unsigned int)(-1)) {
            DSD_FPRINTF(stderr, "%s", KGRN);
            DSD_FPRINTF(stderr, " Channel Code=%02d", (int)state->dPMRVoiceFS2Frame.ColorCode[0]);
            DSD_FPRINTF(stderr, "%s", KNRM);
            if (state->dPMRVoiceFS2Frame.CalledIDOk) {
                state->dpmr_color_code = (int)state->dPMRVoiceFS2Frame.ColorCode[0];
            }
        }
    } else {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " Src=(CRC ERR) Channel Code =(CRC ERR)");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

static void DSD_ATTR_USED
dpmr_print_scrambler_state(const dsd_opts* opts, const dsd_state* state) {
    if (state->dPMRVoiceFS2Frame.Version[0] != 3) {
        return;
    }
    DSD_FPRINTF(stderr, "%s", KRED);
    DSD_FPRINTF(stderr, " Scrambler");
    DSD_FPRINTF(stderr, "%s", KNRM);
    if (state->R != 0) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        char key_text[16];
        DSD_FPRINTF(stderr, " Key %s ",
                    dsd_secret_format_decimal(key_text, sizeof key_text, opts->show_keys, state->R, 5U));
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

void
dpmr_play_voice_frames(dsd_opts* opts, dsd_state* state, char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24]) {
    uint32_t start = 0;
    uint32_t end = 4;
    for (short o = 0; o < 2; o++) {
        if (state->dPMRVoiceFS2Frame.FrameNumbering[o] == 0) {
            state->payload_miN = 0;
        }
        if (o == 1) {
            start = 4;
            end = 8;
        }

        int realsynctype = state->synctype;
        if ((state->dPMRVoiceFS2Frame.CommunicationMode[o] == 0) || (state->dPMRVoiceFS2Frame.CommunicationMode[o] == 1)
            || (state->dPMRVoiceFS2Frame.CommunicationMode[o] == 5)) {
            if (state->dPMRVoiceFS2Frame.Version[o] == 3) {
                state->synctype = DSD_SYNC_NXDN_POS;
                state->nxdn_cipher_type = 0x01;
                state->dmr_encL = 1;
            }
            if (state->R != 0) {
                state->dmr_encL = 0;
            }
            if (opts->payload == 1) {
                DSD_FPRINTF(stderr, "\n FN %d/4", state->dPMRVoiceFS2Frame.FrameNumbering[o] + 1);
            }

            for (uint32_t i = start; i < end; i++) {
                processMbeFrame(opts, state, NULL, ambe_fr[i], NULL);
                if (opts->floating_point == 0) {
                    playSynthesizedVoiceMS(opts, state);
                }
                if (opts->floating_point == 1) {
                    playSynthesizedVoiceFM(opts, state);
                }
            }
            state->synctype = realsynctype;
            state->nxdn_cipher_type = 0;
        }
    }
}

#ifdef dPMR_PRINT_DEBUG_INFO
static void
dpmr_print_debug_cch(const dpmr_voice_ctx_t* ctx, const dsd_state* state) {
    for (uint32_t i = 0; i < NB_OF_DPMR_VOICE_FRAME_TO_DECODE; i++) {
        DSD_FPRINTF(stderr, "i = %u - ", i);
        DSD_FPRINTF(stderr, "Comm Mode = %01u - ", state->dPMRVoiceFS2Frame.CommunicationMode[i]);
        DSD_FPRINTF(stderr, "Version = %01u - ", state->dPMRVoiceFS2Frame.Version[i]);
        DSD_FPRINTF(stderr, "Comms Format = %01u - ", state->dPMRVoiceFS2Frame.CommsFormat[i]);
        DSD_FPRINTF(stderr, "Emergency = %01u - ", state->dPMRVoiceFS2Frame.EmergencyPriority[i]);
        DSD_FPRINTF(stderr, "Reserved = %01u - ", state->dPMRVoiceFS2Frame.Reserved[i]);
        DSD_FPRINTF(stderr, "Slow Data = 0x%05X - ", state->dPMRVoiceFS2Frame.SlowData[i]);
        if (ctx->HammingOk[i] && ctx->CrcOk[i]) {
            DSD_FPRINTF(stderr, "Valid");
        } else {
            DSD_FPRINTF(stderr, "CRC ERROR");
        }
        DSD_FPRINTF(stderr, "\n");
    }
}
#endif

void
processdPMRvoice(dsd_opts* opts, dsd_state* state) {
    dpmr_voice_ctx_t ctx;
    dpmr_superframe_part part;
    uint32_t PartOfSuperFrame = 0;
    char CalledID[8] = {0};
    char CallingID[8] = {0};
    UNUSED(PartOfSuperFrame);

    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    DSD_MEMSET(&part, 0, sizeof(part));
    DSD_MEMSET(ctx.CCHDataHammingCorrected, 1, sizeof(ctx.CCHDataHammingCorrected));
    DSD_MEMSET(ctx.CCHDataCRC, 1, sizeof(ctx.CCHDataCRC));

    dpmr_read_first_cch(opts, state, &ctx);
    dpmr_read_tch_group(opts, state, &ctx, 0);

    dpmr_read_first_cc(opts, state, &ctx);
    dpmr_read_second_cch(opts, state, &ctx);

    dpmr_read_tch_group(opts, state, &ctx, 4);
    dpmr_decode_cch_frames(state, &ctx);
    dpmr_extract_superframe_part(&ctx, &part);
    dpmr_update_superframe_part(opts, state, &part);
    dpmr_extract_previous_ids(state, CalledID, CallingID);
    dpmr_print_ids(state, CalledID, CallingID);
    dpmr_print_scrambler_state(opts, state);
    dpmr_play_voice_frames(opts, state, ctx.ambe_fr);
    DSD_FPRINTF(stderr, "\n");

#ifdef dPMR_PRINT_DEBUG_INFO
    dpmr_print_debug_cch(&ctx, state);
#endif
} //End processdPMRvoice()

/* Scrambler used for dPMR scrambling / descrambling,
 * see ETSI TS 102 658 chapter 7.4 for the
 * polynomial description.
 * It is a X^9 + X^5 + 1 polynomial. */
void
dpmr_deinterleave_6x12(const uint8_t* input, uint8_t* output) {
    uint8_t matrix[12][6] = {0};
    uint32_t i, j, k;

    /* Step 1 : Filling the 12 x 6 bit matrix */
    k = 0;
    for (i = 0; i < 12; i++) {
        for (j = 0; j < 6; j++) {
            matrix[i][j] = input[k++];
        }
    }

    /* Step 2 : Filling the output buffer with deinterleaved data */
    k = 0;
    for (j = 0; j < 6; j++) {
        for (i = 0; i < 12; i++) {
            output[k++] = matrix[i][j];
        }
    }
}

/* CRC 7 bit computation with the following
 * polynomial : X^7 + X^3 + 1 */
uint8_t
dpmr_crc7(const uint8_t* input, uint32_t bit_length) {
    uint8_t shift_register = 0x00; /* All bit to '0' (7 LSBit only used) */
    uint8_t polynomial = 0x09;     /* X^7 + X^3 + 1 */
    uint32_t i;

    for (i = 0; i < bit_length; i++) {
        if (((shift_register >> 6) & 1) ^ input[i]) {
            shift_register = ((shift_register << 1) ^ polynomial) & 0x7F;
        } else {
            shift_register = (shift_register << 1) & 0x7F;
        }
    }

    return shift_register;
}

/* Convert an air interface identifier (AI ID) into
 * a 7 ASCII digit string.
 *
 * See dPMR standard chapter A.1.2.1.1.6
 * "Mapping of dialled strings to the AI address space" */
void
dpmr_convert_air_interface_id(uint32_t ai_id, char id[8]) {
    uint32_t remaining = ai_id;
    uint32_t digit;

    /* 1st digit */
    digit = remaining / 1464100;
    remaining = remaining % 1464100;
    if (digit == 10) {
        id[0] = '*';
    } else {
        id[0] = digit + '0';
    }

    /* 2nd digit */
    digit = remaining / 146410;
    remaining = remaining % 146410;
    if (digit == 10) {
        id[1] = '*';
    } else {
        id[1] = digit + '0';
    }

    /* 3rd digit */
    digit = remaining / 14641;
    remaining = remaining % 14641;
    if (digit == 10) {
        id[2] = '*';
    } else {
        id[2] = digit + '0';
    }

    /* 4th digit */
    digit = remaining / 1331;
    remaining = remaining % 1331;
    if (digit == 10) {
        id[3] = '*';
    } else {
        id[3] = digit + '0';
    }

    /* 5th digit */
    digit = remaining / 121;
    remaining = remaining % 121;
    if (digit == 10) {
        id[4] = '*';
    } else {
        id[4] = digit + '0';
    }

    /* 6th digit */
    digit = remaining / 11;
    remaining = remaining % 11;
    if (digit == 10) {
        id[5] = '*';
    } else {
        id[5] = digit + '0';
    }

    /* 7th digit */
    digit = remaining;
    if (digit == 10) {
        id[6] = '*';
    } else {
        id[6] = digit + '0';
    }

    /* Add the "end of string" */
    id[7] = '\0';
}

/* End of file */
