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
#include <dsd-neo/core/key_presence.h>

#include <dsd-neo/core/ambe_interleave.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/secret_redaction.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/protocol/dpmr/dpmr.h>
#include <dsd-neo/protocol/dpmr/dpmr_data.h>
#include <dsd-neo/runtime/colors.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dpmr_confirm.h"
#include "dpmr_internal.h"

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
        uint32_t k = 0;
        for (uint32_t i = 0; i < DSD_AMBE_2450_DIBITS; i++) {
            const dsd_ambe_2450_dibit_map_entry* map = &dsd_ambe_2450_dibit_map[i];
            uint32_t dibit = dpmr_read_dibit(opts, state);
            ctx->ambe_fr[frame_base + j][map->high_row][map->high_col] = (char)(1U & (dibit >> 1)); // bit 1
            ctx->ambe_fr[frame_base + j][map->low_row][map->low_col] = (char)(1U & dibit);          // bit 0
            state->dPMRVoiceFS2Frame.RawVoiceBit[frame_base + j][k] = (uint8_t)(1U & (dibit >> 1));
            state->dPMRVoiceFS2Frame.RawVoiceBit[frame_base + j][k + 1] = (uint8_t)(1U & dibit);
            k += 2;
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

/* Whether the frame's two halves carry one identity together (issue #890, B9b): both passed their CRC-7 and are
 * numbered 0/1 (ID0, ID1: the called station) or 2/3 (ID2, ID3: the calling station), TS 102 658 Tables 5.5 to 5.8. A
 * half whose Hamming words merely correct is no check (the CRC covers the bits they produced), and a pair numbered
 * otherwise joins the halves of two identities. */
static int
dpmr_ids_are_strong(const dpmr_superframe_part* part) {
    return part->crc_ok[0] && part->crc_ok[1]
           && ((part->frame_number[0] == 0U && part->frame_number[1] == 1U)
               || (part->frame_number[0] == 2U && part->frame_number[1] == 3U));
}

/*
 * Report what this frame's CCH CRC-7 proved. Both halves passing is one chance in 16384
 * and confirms at once; one half is one in 128 and has to repeat. The Hamming flags are
 * deliberately not evidence at any strength: the CRC covers the bits they produced.
 */
static void
dpmr_note_cch_evidence(dsd_state* state, const dpmr_voice_ctx_t* ctx) {
    const int passing = (ctx->CrcOk[0] != 0U ? 1 : 0) + (ctx->CrcOk[1] != 0U ? 1 : 0);
    if (passing == 2) {
        dpmr_confirm_note_evidence(state, DPMR_EVIDENCE_STRONG);
    } else if (passing == 1) {
        dpmr_confirm_note_evidence(state, DPMR_EVIDENCE_WEAK);
    }
}

static void
dpmr_mark_unknown_superframe_part(dsd_opts* opts) {
    if (opts->dPMR_next_part_of_superframe == 1) {
        opts->dPMR_next_part_of_superframe = 2;
    } else if (opts->dPMR_next_part_of_superframe == 2) {
        opts->dPMR_next_part_of_superframe = 1;
    } else {
        opts->dPMR_next_part_of_superframe = 0;
    }
}

/* Which identity the frame carries (dPMRVoiceFS2Frame_t::BoundIdKind), for dpmr_publish_call() to publish with the
 * frame's checked service; and which superframe part comes next, by the halves' frame numbers. */
void
dpmr_update_superframe_part(dsd_opts* opts, dsd_state* state, const dpmr_superframe_part* part) {
    dPMRVoiceFS2Frame_t* f = &state->dPMRVoiceFS2Frame;
    f->BoundIdKind = DPMR_FRAME_ID_NONE;
    f->BoundIdValue = part->id_value;
    if (dpmr_ids_are_strong(part)) {
        f->BoundIdKind = part->frame_number[0] == 0U ? DPMR_FRAME_ID_CALLED : DPMR_FRAME_ID_CALLING;
    }
    if (((part->crc_ok[0] || part->hamming_ok[0][0]) && (part->frame_number[0] == 0))
        || ((part->crc_ok[1] || part->hamming_ok[1][0]) && (part->frame_number[1] == 1))) {
        opts->dPMR_next_part_of_superframe = 2;
        return;
    }
    if (((part->crc_ok[0] || part->hamming_ok[0][0]) && (part->frame_number[0] == 2))
        || ((part->crc_ok[1] || part->hamming_ok[1][0]) && (part->frame_number[1] == 3))) {
        opts->dPMR_next_part_of_superframe = 1;
        return;
    }
    dpmr_mark_unknown_superframe_part(opts);
}

/* The identities a frame may show: those of the open dPMR call a confirmed transmission published, read whole from
 * frames whose CCH passed its CRC-7. The canonical call alone is not enough: its text outlives the transmission that
 * decoded it, and another protocol's call leaves its own there, so a noise frame that syncs as dPMR would show an
 * earlier call's identities as if it had decoded them. */
static int
dpmr_shown_call(const dsd_state* state, dsd_call_snapshot* call) {
    return dpmr_confirm_is_confirmed(state) && dsd_call_state_get(state, 0U, call) > 0
           && call->phase == DSD_CALL_PHASE_ACTIVE && DSD_SYNC_IS_DPMR(call->protocol);
}

void
dpmr_print_ids(const dsd_state* state) {
    dsd_call_snapshot call;
    if (!dpmr_shown_call(state, &call)) {
        DSD_MEMSET(&call, 0, sizeof(call));
    }
    DSD_FPRINTF(stderr, "\n");
    if (call.target_text[0] != '\0') {
        DSD_FPRINTF(stderr, "%s", KGRN);
        DSD_FPRINTF(stderr, " TG=%s", call.target_text);
        DSD_FPRINTF(stderr, "%s", KNRM);
    } else {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " TG=(CRC ERR)");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    if (call.source_text[0] != '\0') {
        DSD_FPRINTF(stderr, "%s", KGRN);
        DSD_FPRINTF(stderr, " Src=%s", call.source_text);
        DSD_FPRINTF(stderr, "%s", KNRM);
        if (state->dPMRVoiceFS2Frame.ColorCode[0] != (unsigned int)(-1)) {
            DSD_FPRINTF(stderr, "%s", KGRN);
            DSD_FPRINTF(stderr, " Channel Code=%02d", (int)state->dPMRVoiceFS2Frame.ColorCode[0]);
            DSD_FPRINTF(stderr, "%s", KNRM);
        }
    } else {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " Src=(CRC ERR) Channel Code =(CRC ERR)");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

/* Which halves of the frame are scrambled (issue #655), in order: a half whose CCH CRC-7 passed by its own Version (3
 * is scrambled), and the transmission learns it; a half whose CRC failed by the transmission's last verified half, or,
 * when none has verified yet, by the other half of this frame if that one passed; with neither, the half is unverified
 * and plays nothing (a transmission boundary leaves confirmation standing, so a scrambled call after a clear one must
 * not play its first undecodable halves as clear). Unverified bits never decide it: a failed half reading Version 3
 * does not scramble a clear call, and one reading 0 does not unscramble a scrambled one. */
/* Which Communications Mode each half plays by (issue #890), by the same rule as its scrambling: a half whose CRC-7
 * passed by its own (and the transmission learns it), a failed half by the transmission's last checked half or header,
 * or, with none yet, by this frame's other checked half; with neither, by none, and it plays nothing. A failed half's
 * own mode bits never decide it: a damaged data half reading voice must not reach the vocoder. */
static void
dpmr_resolve_half_modes(dsd_state* state) {
    dPMRVoiceFS2Frame_t* f = &state->dPMRVoiceFS2Frame;
    for (int o = 0; o < NB_OF_DPMR_VOICE_FRAME_TO_DECODE; o++) {
        if (f->CCHDataCrcOk[o] != 0U) {
            state->dpmr_mode = (uint8_t)(f->CommunicationMode[o] + 1U);
        }
        unsigned int mode = state->dpmr_mode != 0U ? state->dpmr_mode - 1U : DPMR_HALF_MODE_UNVERIFIED;
        for (int other = 0; mode == DPMR_HALF_MODE_UNVERIFIED && other < NB_OF_DPMR_VOICE_FRAME_TO_DECODE; other++) {
            if (other != o && f->CCHDataCrcOk[other] != 0U) {
                mode = f->CommunicationMode[other];
            }
        }
        f->ServiceMode[o] = mode;
    }
}

void
dpmr_resolve_half_scrambling(dsd_state* state) {
    dPMRVoiceFS2Frame_t* f = &state->dPMRVoiceFS2Frame;
    for (int o = 0; o < NB_OF_DPMR_VOICE_FRAME_TO_DECODE; o++) {
        if (f->CCHDataCrcOk[o] != 0U) {
            state->dpmr_scrambled = (f->Version[o] == 3U) ? 2U : 1U;
        }
        uint8_t known = state->dpmr_scrambled;
        if (known == 0U) {
            for (int other = 0; other < NB_OF_DPMR_VOICE_FRAME_TO_DECODE; other++) {
                if (other != o && f->CCHDataCrcOk[other] != 0U) {
                    known = (f->Version[other] == 3U) ? 2U : 1U;
                    break;
                }
            }
        }
        f->Scrambled[o] = known == 2U ? DPMR_HALF_SCRAMBLED : known == 1U ? DPMR_HALF_CLEAR : DPMR_HALF_UNVERIFIED;
    }
    dpmr_resolve_half_modes(state);
}

static void DSD_ATTR_USED
dpmr_print_scrambler_state(const dsd_opts* opts, const dsd_state* state) {
    if (state->dPMRVoiceFS2Frame.Scrambled[0] != DPMR_HALF_SCRAMBLED) {
        return;
    }
    DSD_FPRINTF(stderr, "%s", KRED);
    DSD_FPRINTF(stderr, " Scrambler");
    DSD_FPRINTF(stderr, "%s", KNRM);
    if (dsd_key_scalar_present(state, 0)) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        char key_text[16];
        DSD_FPRINTF(stderr, " Key %s ",
                    dsd_secret_format_decimal(key_text, sizeof key_text, opts->show_keys, state->R, 5U));
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

/* What this frame's own verified halves say about scrambling (dsd_call_crypto_update::enc_observed): a half whose CCH
 * CRC passed, first one first. Scrambling the transmission learned from an earlier frame (Scrambled[]) is what plays
 * the audio, not this call's own indication. */
static uint8_t
dpmr_frame_enc_observed(const dPMRVoiceFS2Frame_t* f) {
    for (int o = 0; o < NB_OF_DPMR_VOICE_FRAME_TO_DECODE; o++) {
        if (f->CCHDataCrcOk[o] != 0U) {
            return f->Version[o] == 3U ? DSD_CALL_ENC_OBSERVED_ENCRYPTED : DSD_CALL_ENC_OBSERVED_CLEAR;
        }
    }
    return DSD_CALL_ENC_OBSERVED_NONE;
}

/* The frame's first half whose CCH CRC-7 passed, or -1: the only half whose service a frame may publish (issue #890,
 * B9b). */
static int
dpmr_checked_half(const dPMRVoiceFS2Frame_t* f) {
    for (int o = 0; o < NB_OF_DPMR_VOICE_FRAME_TO_DECODE; o++) {
        if (f->CCHDataCrcOk[o] != 0U) {
            return o;
        }
    }
    return -1;
}

/* What a confirmed frame publishes, once (issue #890, F13): the identity its halves carry together
 * (dpmr_update_superframe_part()) with the service and call kind of its checked half, so a checked data CCH is a data
 * call and never voice. A frame with no checked half names nothing: it only keeps an open dPMR call going, never opens
 * one. */
static int
dpmr_publish_frame_observation(dsd_state* state) {
    const dPMRVoiceFS2Frame_t* f = &state->dPMRVoiceFS2Frame;
    int protocol = DSD_SYNC_IS_DPMR(state->synctype) ? state->synctype : state->lastsynctype;
    if (!DSD_SYNC_IS_DPMR(protocol)) {
        protocol = DSD_SYNC_DPMR_FS2_POS;
    }
    dsd_call_observation observation = {
        .protocol = protocol,
        .slot = 0U,
        .kind = DSD_CALL_KIND_UNKNOWN,
    };
    const int c = dpmr_checked_half(f);
    if (c < 0) {
        dsd_call_snapshot call;
        if (dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
            && DSD_SYNC_IS_DPMR(call.protocol)) {
            observation.kind = call.kind; /* the call's own: nothing here says otherwise */
            (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE);
        }
        return 0;
    }
    /* No channel: dPMR names no trunk channel, and the colour code is the row's access code. */
    observation.kind = dpmr_call_kind_for_mode(f->CommunicationMode[c]);
    observation.service_options =
        (uint16_t)((f->Version[c] << 8U) | (f->CommunicationMode[c] << 4U) | f->CommsFormat[c]);
    observation.emergency = (uint8_t)(f->EmergencyPriority[c] != 0U);
    observation.has_service_metadata = 1U;
    if (f->BoundIdKind != DPMR_FRAME_ID_NONE) {
        char identity[8];
        dpmr_convert_air_interface_id(f->BoundIdValue, identity);
        char* text = f->BoundIdKind == DPMR_FRAME_ID_CALLED ? observation.target_text : observation.source_text;
        dsd_strncpy_s(text, DSD_CALL_IDENTITY_TEXT_SIZE, identity, DSD_CALL_IDENTITY_TEXT_SIZE - 1U);
    }
    return dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) > 0;
}

/* Whether a dPMR call is open on the slot. */
static int
dpmr_call_open(const dsd_state* state) {
    dsd_call_snapshot call;
    return dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
           && DSD_SYNC_IS_DPMR(call.protocol);
}

void
dpmr_publish_call(dsd_opts* opts, dsd_state* state) {
    if (!dpmr_confirm_is_confirmed(state)) {
        /* Service options, colour code and emergency all come out of a CCH that has not
         * proved itself; publishing them would put a call row on the air alone (#407). */
        return;
    }
    /* The colour code is published on the confirmed path whether or not a caller identity decoded with it. A frame
       whose channel code did not decode keeps an open call's code, but a call it opens has none of its own: the code
       an earlier call left is not this one's access code (issue #890). */
    const int known = state->dPMRVoiceFS2Frame.ColorCode[0] != (unsigned int)(-1);
    if (known) {
        dpmr_publish_color_code(state, (int)state->dPMRVoiceFS2Frame.ColorCode[0]);
    } else if (!dpmr_call_open(state) && dpmr_checked_half(&state->dPMRVoiceFS2Frame) >= 0) {
        state->dpmr_color_code = -1;
    }
    if (dpmr_publish_frame_observation(state) && !known) {
        state->dpmr_color_code = -1; /* the observation began another call (another identity) */
    }

    /* A frame no verified half speaks for since the transmission's boundary leaves the call's classification alone. */
    if (state->dPMRVoiceFS2Frame.Scrambled[0] != DPMR_HALF_UNVERIFIED) {
        dpmr_publish_crypto(state, state->dPMRVoiceFS2Frame.Scrambled[0],
                            dpmr_frame_enc_observed(&state->dPMRVoiceFS2Frame));
    }
    dsd_event_sync_slot(opts, state, 0U);
}

/* Whether half @p o's payload frame carries speech in a Voice and attached data call (M = 101, TS 102 658 §5.5.7 Table
   5.55): its TCH is AMBE only while the SLD of its CRC-checked CCH says the frame holds no data (DP = 00, §5.5.29.2
   Table 5.95; §11.1.1 Figure 11.3: after PTT release the remaining frames carry the attached data with DP = 11). A
   half whose CRC-7 failed, or with a reserved DP, is not known to be speech (issue #890). The SLD's 18 bits are read
   first bit first: 5 reserved bits, then DP. */
static int
dpmr_attached_data_half_is_voice(const dsd_state* state, short o) {
    return state->dPMRVoiceFS2Frame.CCHDataCrcOk[o] != 0U && ((state->dPMRVoiceFS2Frame.SlowData[o] >> 11U) & 3U) == 0U;
}

/* The frame number half @p o carries, by checked content only (issue #890): its own when its CRC-7 passed, else the
 * one next to the frame's other half when that one passed (one FS2 frame carries frames 0 and 1, or 2 and 3, Figure
 * 4.8); -1 when neither says. A failed half's own frame number bits never decide it. */
static int
dpmr_half_frame_number(const dPMRVoiceFS2Frame_t* f, int o) {
    if (f->CCHDataCrcOk[o] != 0U) {
        return (int)(f->FrameNumbering[o] & 3U);
    }
    const int other = 1 - o;
    if (f->CCHDataCrcOk[other] == 0U) {
        return -1;
    }
    const int fn = (int)(f->FrameNumbering[other] & 3U) + (o == 0 ? -1 : 1);
    return (fn >= 0 && fn <= 3) ? fn : -1;
}

/* Whether half @p o is played: its scrambling is verified (issue #655), it is not attached data (issue #890: attached
 * data is never AMBE, so it is neither played nor the scan's voice credit), and its checked mode
 * (dpmr_resolve_half_modes(), never its own unchecked bits) is a voice mode. */
static int
dpmr_half_plays(const dsd_state* state, short o) {
    const dPMRVoiceFS2Frame_t* f = &state->dPMRVoiceFS2Frame;
    if (f->Scrambled[o] == DPMR_HALF_UNVERIFIED) {
        return 0;
    }
    if (f->ServiceMode[o] == 5U && !dpmr_attached_data_half_is_voice(state, o)) {
        return 0;
    }
    return dpmr_call_kind_for_mode(f->ServiceMode[o]) == DSD_CALL_KIND_VOICE;
}

/* Where a confirmed scrambled half descrambles from (issue #890): the scrambler reloads from the key at frame 0 of
 * each superframe and runs on through its four payload frames of four AMBE frames each, 49 bits an AMBE frame
 * (dsd_mbe_nxdn_scramble_skip_voice_frame(), the vocoder's own step), so a half whose place in the superframe is known
 * descrambles from that place whatever came before it: a muted half (a keyed Voice and attached data call's frame 0
 * whose CCH failed before a checked frame 1 of speech, TS 102 658 5.5.7, 11.1.1 Figure 11.3; an unverified half; a
 * data half) or a frame played before the transmission was confirmed. A half whose place is not known runs on from the
 * register as the last half played left it. The register is NXDN's too, so only a confirmed transmission writes it. */
static void
dpmr_place_scrambler(dsd_state* state) {
    if (state->dpmr_scramble_place == 0U) {
        return;
    }
    state->nxdn_payload_mi = 0; /* the scrambler reloads from the key (shared with NXDN) */
    state->nxdn_iv_on_air = 0U;
    for (unsigned i = 1U; i < state->dpmr_scramble_place; i++) {
        dsd_mbe_nxdn_scramble_skip_voice_frame(state);
    }
}

static void
dpmr_play_half(dsd_opts* opts, dsd_state* state, char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24], short o) {
    const int realsynctype = state->synctype;
    /* Each half is scrambled or clear on its own (issue #655): a scrambled half decodes through the NXDN scrambler, and
     * the verdict the vocoder and mixers take from nxdn_cipher_type follows it half by half. */
    const int scrambled = state->dPMRVoiceFS2Frame.Scrambled[o] == DPMR_HALF_SCRAMBLED;
    state->nxdn_cipher_type = scrambled ? 0x01 : 0x00;
    if (scrambled) {
        state->synctype = DSD_SYNC_NXDN_POS;
        dpmr_place_scrambler(state);
    }
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n FN %d/4", state->dPMRVoiceFS2Frame.FrameNumbering[o] + 1);
    }

    /* A scrambled half plays through NXDN's decoder under its synctype; the receive policy judges dPMR's. */
    const dsd_receive_context receive = dsd_receive_context_begin(state, DSD_SYNC_DPMR_FS2_POS);
    const uint32_t start = o == 0 ? 0U : 4U;
    for (uint32_t i = start; i < start + 4U; i++) {
        processMbeFrame(opts, state, DSD_VOICE_PROTOCOL_DPMR, NULL, ambe_fr[i], NULL);
        playSynthesizedVoiceMS(opts, state);
    }
    dsd_receive_context_end(state, receive);
    state->synctype = realsynctype;
    state->nxdn_cipher_type = 0;
}

/* The frame's halves in order: each half's place in the superframe's scrambler (dsd_state::dpmr_scramble_place) is
 * learned from its checked frame number, or follows the half before it; a confirmed transmission (@p confirmed)
 * plays the halves that play. A frame not confirmed plays nothing and writes nothing but dPMR's own place: it may be
 * noise where an NXDN call works, whose cipher, register and IV it must leave alone. */
static void
dpmr_route_voice_frames(dsd_opts* opts, dsd_state* state, char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24],
                        int confirmed) {
    for (short o = 0; o < 2; o++) {
        const int fn = dpmr_half_frame_number(&state->dPMRVoiceFS2Frame, o);
        if (fn >= 0) {
            state->dpmr_scramble_place = (uint8_t)((4 * fn) + 1); /* the half's first AMBE frame, plus one */
        }
        if (confirmed && dpmr_half_plays(state, o)) {
            dpmr_play_half(opts, state, ambe_fr, o);
        }
        if (state->dpmr_scramble_place != 0U) {
            state->dpmr_scramble_place = (uint8_t)(((state->dpmr_scramble_place - 1U + 4U) % 16U) + 1U);
        }
    }
}

void
dpmr_play_voice_frames(dsd_opts* opts, dsd_state* state, char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24]) {
    dpmr_route_voice_frames(opts, state, ambe_fr, 1);
}

void
dpmr_track_voice_frames(dsd_state* state) {
    dpmr_route_voice_frames(NULL, state, NULL, 0);
}

int
processdPMRvoice(dsd_opts* opts, dsd_state* state) {
    dpmr_voice_ctx_t ctx;
    dpmr_superframe_part part;
    uint32_t PartOfSuperFrame = 0;
    UNUSED(PartOfSuperFrame);

    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    DSD_MEMSET(&part, 0, sizeof(part));
    DSD_MEMSET(ctx.CCHDataHammingCorrected, 1, sizeof(ctx.CCHDataHammingCorrected));
    DSD_MEMSET(ctx.CCHDataCRC, 1, sizeof(ctx.CCHDataCRC));

    const uint32_t carrier_seq = state->carrier_seq;
    /* Where the frame begins, right behind its FS2 word: a weak streak counts only frames that run on. */
    const uint32_t frame_symbol = state->symbolcnt;
    dpmr_read_first_cch(opts, state, &ctx);
    dpmr_read_tch_group(opts, state, &ctx, 0);

    dpmr_read_first_cc(opts, state, &ctx);
    dpmr_read_second_cch(opts, state, &ctx);

    dpmr_read_tch_group(opts, state, &ctx, 4);
    if (dsd_state_frame_read_lost(state, carrier_seq)) {
        /* The carrier boundary moved the carrier count while the two frames were read (issue #575). Nothing decodes
           before both are in, so the first frame's CCH and the channel code, read whole before the move, would publish
           the carrier left's call and colour code after the boundary ended that call and forgot the code: the pair is
           dropped, as on a sync loss, with no CRC to report. So is a pair an input interruption cut, whose rest is
           erasures the reader stood in. */
        DSD_FPRINTF(stderr, "\n");
        return 0;
    }

    /* Evidence is noted before anything is published, so the frame that completes a weak
     * streak still publishes itself rather than waiting for the next one. A signal search hears
     * the confirmation at dpmr_confirm_end_frame(), after the publication. */
    dpmr_confirm_note_frame_start(state, frame_symbol);
    dpmr_confirm_begin_frame(state);
    dpmr_decode_cch_frames(state, &ctx);
    dpmr_note_cch_evidence(state, &ctx);
    dpmr_resolve_half_scrambling(state);

    dpmr_extract_superframe_part(&ctx, &part);
    dpmr_update_superframe_part(opts, state, &part);
    /* Published before it is printed: the line shows the identity this frame bound, not the one before it. */
    dpmr_publish_call(opts, state);
    dpmr_print_ids(state);
    if (opts->payload == 1) {
        /* Each CCH half's CRC-7 verdict (-Z). */
        DSD_FPRINTF(stderr, " CCH %s/%s", ctx.CrcOk[0] != 0U ? "OK" : "ERR", ctx.CrcOk[1] != 0U ? "OK" : "ERR");
    }
    dpmr_print_scrambler_state(opts, state);
    if (dpmr_confirm_is_confirmed(state)) {
        dpmr_play_voice_frames(opts, state, ctx.ambe_fr);
    } else {
        /* Nothing plays before the transmission is confirmed, but the frame keeps its place in the scrambler. */
        dpmr_track_voice_frames(state);
    }
    DSD_FPRINTF(stderr, "\n");

    dpmr_confirm_end_frame(opts, state);

    /* How many of the two CCH halves passed their CRC-7, for the caller's verdict. */
    return (int)((ctx.CrcOk[0] != 0U ? 1U : 0U) + (ctx.CrcOk[1] != 0U ? 1U : 0U));
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

/* End of file */
