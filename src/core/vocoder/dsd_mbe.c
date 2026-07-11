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

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/cleanup.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/mbe_api.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/des.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/crypto/rc4.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <mbelib.h>
#include <stdint.h>
#include <stdio.h>
#include "../mbe_result_context.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static void
p25p2_record_voice_err(dsd_state* state, int voice_err) {
    if (!state || !DSD_SYNC_IS_P25P2(state->synctype)) {
        return;
    }

    int len = state->p25_p2_voice_err_hist_len > 0 ? state->p25_p2_voice_err_hist_len : 50;
    if (len > (int)sizeof(state->p25_p2_voice_err_hist[0])) {
        len = (int)sizeof(state->p25_p2_voice_err_hist[0]);
    }
    state->p25_p2_voice_err_hist_len = len;

    int slot = (state->currentslot == 1) ? 1 : 0;
    int hpos = state->p25_p2_voice_err_hist_pos[slot] % len;
    uint8_t old = state->p25_p2_voice_err_hist[slot][hpos];
    uint8_t val = (uint8_t)(voice_err & 0xFF);
    state->p25_p2_voice_err_hist[slot][hpos] = val;
    state->p25_p2_voice_err_hist_sum[slot] += val;
    state->p25_p2_voice_err_hist_sum[slot] -= old;
    state->p25_p2_voice_err_hist_pos[slot] = (hpos + 1) % len;

    dsd_rtl_stream_metrics_hook_p25p2_err_update(slot, 0, 0, 0, 0, (int)val);
}

static void
copy_imbe7200_soft_frame(dsd_vocoder_soft_bit src[8][23], mbe_soft_bit dst[8][23]) {
    if (!src || !dst) {
        return;
    }
    for (int row = 0; row < 8; row++) {
        for (int bit = 0; bit < 23; bit++) {
            dst[row][bit].bit = src[row][bit].bit ? 1u : 0u;
            dst[row][bit].reliability = src[row][bit].reliability;
        }
    }
}

static void
copy_ambe_soft_frame(dsd_vocoder_soft_bit src[4][24], mbe_soft_bit dst[4][24]) {
    if (!src || !dst) {
        return;
    }
    for (int row = 0; row < 4; row++) {
        for (int bit = 0; bit < 24; bit++) {
            dst[row][bit].bit = src[row][bit].bit ? 1u : 0u;
            dst[row][bit].reliability = src[row][bit].reliability;
        }
    }
}

void
dsd_mbe_init_result_from_errors(mbe_process_result* result, int errs, int errs2, unsigned flags) {
    if (!result) {
        return;
    }

    int c0_errors = errs < 0 ? 0 : errs;
    int total_errors = errs2 < 0 ? 0 : errs2;
    if (total_errors < c0_errors) {
        total_errors = c0_errors;
    }

    mbe_initProcessResult(result);
    result->total_errors = total_errors;
    result->flags = flags;
    if ((flags & MBE_PROCESS_FLAG_C0_VALID) != 0u) {
        result->c0_errors = c0_errors;
        result->protected_errors = total_errors - c0_errors;
    } else {
        result->protected_errors = total_errors;
    }
}

void
dsd_mbe_store_result(int* errs, int* errs2, char* err_str, size_t err_str_size, const mbe_process_result* result) {
    if (!result) {
        return;
    }
    if (errs) {
        *errs = ((result->flags & MBE_PROCESS_FLAG_C0_VALID) != 0u) ? result->c0_errors : result->total_errors;
    }
    if (errs2) {
        *errs2 = result->total_errors;
    }
    if (err_str && err_str_size > 0) {
        mbe_formatProcessResult(err_str, err_str_size, result);
    }
}

static void
dsd_mbe_clear_status(int* errs, int* errs2, char* err_str, size_t err_str_size) {
    if (errs) {
        *errs = 0;
    }
    if (errs2) {
        *errs2 = 0;
    }
    if (err_str && err_str_size > 0) {
        err_str[0] = '\0';
    }
}

static int
dsd_mbe_store_decode_status(int ret, int* errs, int* errs2, const mbe_process_result* result) {
    if (ret < 0) {
        dsd_mbe_clear_status(errs, errs2, NULL, 0);
        return ret;
    }

    dsd_mbe_store_result(errs, errs2, NULL, 0, result);
    return ret;
}

static int
dsd_mbe_store_process_status(int ret, float* aout_buf, int* errs, int* errs2, char* err_str, size_t err_str_size,
                             const mbe_process_result* result) {
    if (ret < 0) {
        if (aout_buf) {
            mbe_synthesizeSilencef(aout_buf);
        }
        dsd_mbe_clear_status(errs, errs2, err_str, err_str_size);
        return ret;
    }

    dsd_mbe_store_result(errs, errs2, err_str, err_str_size, result);
    return ret;
}

int
dsd_mbe_decode_imbe7200_frame(int* errs, int* errs2, const char imbe_fr[8][23], char imbe_d[88],
                              mbe_process_result* result) {
    mbe_process_result local_result;
    mbe_process_result* decode_result = result ? result : &local_result;
    int ret = mbe_decodeImbe7200x4400Frame(imbe_fr, imbe_d, decode_result);
    return dsd_mbe_store_decode_status(ret, errs, errs2, decode_result);
}

int
dsd_mbe_decode_imbe7100_frame(int* errs, int* errs2, const char imbe_fr[7][24], char imbe_d[88],
                              mbe_process_result* result) {
    mbe_process_result local_result;
    mbe_process_result* decode_result = result ? result : &local_result;
    int ret = mbe_decodeImbe7100x4400Frame(imbe_fr, imbe_d, decode_result);
    return dsd_mbe_store_decode_status(ret, errs, errs2, decode_result);
}

int
dsd_mbe_decode_ambe2450_frame(int* errs, int* errs2, const char ambe_fr[4][24], char ambe_d[49],
                              mbe_process_result* result) {
    mbe_process_result local_result;
    mbe_process_result* decode_result = result ? result : &local_result;
    int ret = mbe_decodeAmbe3600x2450Frame(ambe_fr, ambe_d, decode_result);
    return dsd_mbe_store_decode_status(ret, errs, errs2, decode_result);
}

int
dsd_mbe_process_imbe4400_dataf(float* aout_buf, int* errs, int* errs2, char* err_str, size_t err_str_size,
                               const char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp,
                               mbe_parms* prev_mp_enhanced, mbe_process_result* result) {
    mbe_process_result local_result;
    mbe_process_result* process_result = result;
    if (!process_result) {
        dsd_mbe_init_result_from_errors(&local_result, errs ? *errs : 0, errs2 ? *errs2 : 0, 0u);
        process_result = &local_result;
    }

    int ret = mbe_processImbe4400Dataf(aout_buf, process_result, imbe_d, cur_mp, prev_mp, prev_mp_enhanced);
    return dsd_mbe_store_process_status(ret, aout_buf, errs, errs2, err_str, err_str_size, process_result);
}

int
dsd_mbe_process_ambe2450_dataf(float* aout_buf, int* errs, int* errs2, char* err_str, size_t err_str_size,
                               const char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp,
                               mbe_parms* prev_mp_enhanced, mbe_process_result* result) {
    mbe_process_result local_result;
    mbe_process_result* process_result = result;
    if (!process_result) {
        dsd_mbe_init_result_from_errors(&local_result, errs ? *errs : 0, errs2 ? *errs2 : 0, 0u);
        process_result = &local_result;
    }

    int ret = mbe_processAmbe2450Dataf(aout_buf, process_result, ambe_d, cur_mp, prev_mp, prev_mp_enhanced);
    return dsd_mbe_store_process_status(ret, aout_buf, errs, errs2, err_str, err_str_size, process_result);
}

int
dsd_mbe_process_ambe2400_dataf(float* aout_buf, int* errs, int* errs2, char* err_str, size_t err_str_size,
                               const char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp,
                               mbe_parms* prev_mp_enhanced, mbe_process_result* result) {
    mbe_process_result local_result;
    mbe_process_result* process_result = result;
    if (!process_result) {
        dsd_mbe_init_result_from_errors(&local_result, errs ? *errs : 0, errs2 ? *errs2 : 0, 0u);
        process_result = &local_result;
    }

    int ret = mbe_processAmbe2400Dataf(aout_buf, process_result, ambe_d, cur_mp, prev_mp, prev_mp_enhanced);
    return dsd_mbe_store_process_status(ret, aout_buf, errs, errs2, err_str, err_str_size, process_result);
}

int
dsd_mbe_process_ambe3600x2400_framef(float* aout_buf, int* errs, int* errs2, char* err_str, size_t err_str_size,
                                     char ambe_fr[4][24], char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp,
                                     mbe_parms* prev_mp_enhanced) {
    mbe_process_result result;
    int ret = mbe_processAmbe3600x2400Framef(aout_buf, &result, (const char (*)[24])ambe_fr, ambe_d, cur_mp, prev_mp,
                                             prev_mp_enhanced);
    return dsd_mbe_store_process_status(ret, aout_buf, errs, errs2, err_str, err_str_size, &result);
}

static int
decode_imbe7200_frame(dsd_state* state, char imbe_fr[8][23], dsd_vocoder_soft_bit imbe_soft_fr[8][23], char imbe_d[88],
                      mbe_process_result* result) {
    if (imbe_soft_fr) {
        mbe_soft_bit soft_fr[8][23];

        copy_imbe7200_soft_frame(imbe_soft_fr, soft_fr);
        int ret = mbe_decodeImbe7200x4400SoftFrame((const mbe_soft_bit(*)[23])soft_fr, imbe_d, result);
        if (ret < 0) {
            dsd_mbe_clear_status(&state->errs, &state->errs2, NULL, 0);
            return 0;
        }
        dsd_mbe_store_result(&state->errs, &state->errs2, NULL, 0, result);
        return 1;
    }

    return dsd_mbe_decode_imbe7200_frame(&state->errs, &state->errs2, (const char (*)[23])imbe_fr, imbe_d, result) >= 0;
}

static int
decode_ambe2450_frame(int* errs, int* errs2, char ambe_fr[4][24], dsd_vocoder_soft_bit ambe_soft_fr[4][24],
                      char ambe_d[49], mbe_process_result* result) {
    if (ambe_soft_fr) {
        mbe_soft_bit soft_fr[4][24];

        copy_ambe_soft_frame(ambe_soft_fr, soft_fr);
        int ret = mbe_decodeAmbe3600x2450SoftFrame((const mbe_soft_bit(*)[24])soft_fr, ambe_d, result);
        if (ret < 0) {
            dsd_mbe_clear_status(errs, errs2, NULL, 0);
            return 0;
        }
        dsd_mbe_store_result(errs, errs2, NULL, 0, result);
        return 1;
    }

    return dsd_mbe_decode_ambe2450_frame(errs, errs2, (const char (*)[24])ambe_fr, ambe_d, result) >= 0;
}

typedef struct {
    float* aout_buf;
    int* errs;
    int* errs2;
    char* err_str;
    size_t err_str_size;
    mbe_parms* cur_mp;
    mbe_parms* prev_mp;
    mbe_parms* prev_mp_enhanced;
} mbe_process_params;

static void
process_imbe4400_params(const mbe_process_params* params, const char imbe_d[88], mbe_process_result* result,
                        int have_result) {
    (void)dsd_mbe_process_imbe4400_dataf(params->aout_buf, params->errs, params->errs2, params->err_str,
                                         params->err_str_size, imbe_d, params->cur_mp, params->prev_mp,
                                         params->prev_mp_enhanced, have_result ? result : NULL);
}

static void
process_ambe2450_params(const mbe_process_params* params, const char ambe_d[49], mbe_process_result* result,
                        int have_result) {
    (void)dsd_mbe_process_ambe2450_dataf(params->aout_buf, params->errs, params->errs2, params->err_str,
                                         params->err_str_size, ambe_d, params->cur_mp, params->prev_mp,
                                         params->prev_mp_enhanced, have_result ? result : NULL);
}

static void
update_p25_p1_voice_err_hist(dsd_state* state) {
    int len = state->p25_p1_voice_err_hist_len > 0 ? state->p25_p1_voice_err_hist_len : 50;
    if (len > (int)sizeof(state->p25_p1_voice_err_hist)) {
        len = (int)sizeof(state->p25_p1_voice_err_hist);
    }
    state->p25_p1_voice_err_hist_len = len;
    uint8_t pos = (uint8_t)state->p25_p1_voice_err_hist_pos;
    uint8_t old = state->p25_p1_voice_err_hist[pos % len];
    uint8_t val = (uint8_t)(state->errs2 & 0xFF);
    state->p25_p1_voice_err_hist[pos % len] = val;
    state->p25_p1_voice_err_hist_sum += val;
    state->p25_p1_voice_err_hist_sum -= old;
    state->p25_p1_voice_err_hist_pos = (pos + 1) % len;
}

static void
play_mbe_output_frame(dsd_opts* opts, dsd_state* state, int want_static_wav) {
    if ((opts->audio_out == 1 || want_static_wav) && opts->floating_point == 0) {
        processAudio(opts, state);
    }
    if (opts->wav_out_f != NULL && opts->dmr_stereo_wav == 1) {
        writeSynthesizedVoice(opts, state);
    }
    if ((opts->audio_out == 1 || want_static_wav) && opts->floating_point == 0) {
        playSynthesizedVoiceMS(opts, state);
    }
    if (opts->floating_point == 1) {
        DSD_MEMCPY(state->f_l, state->audio_out_temp_buf, sizeof(state->f_l));
        playSynthesizedVoiceFM(opts, state);
    }
}

static int
play_imbe_file_frame(dsd_opts* opts, dsd_state* state, char imbe_d[88], char file_err_str[260], int want_static_wav) {
    if (readImbe4400Data(opts, state, imbe_d) != 0) {
        return -1;
    }
    file_err_str[0] = '\0';
    (void)dsd_mbe_process_imbe4400_dataf(state->audio_out_temp_buf, &state->errs, &state->errs2, file_err_str,
                                         sizeof(state->err_str), imbe_d, state->cur_mp, state->prev_mp,
                                         state->prev_mp_enhanced, NULL);
    DSD_STRNCPY(state->err_str, file_err_str, sizeof(state->err_str) - 1);
    state->err_str[sizeof(state->err_str) - 1] = '\0';
    if (DSD_SYNC_IS_P25P1(state->synctype)) {
        update_p25_p1_voice_err_hist(state);
    }
    play_mbe_output_frame(opts, state, want_static_wav);
    return 0;
}

static void
decrypt_ambe_with_pr_key(const dsd_state* state, char ambe_d[49]) {
    if (state == NULL) {
        return;
    }
    (void)dmr_basic_privacy_apply_frame49(state->K, ambe_d);
}

static void
decode_ambe_file_frame(dsd_state* state, char ambe_d[49], char file_err_str[260]) {
    if (state->mbe_file_type == 1) {
        file_err_str[0] = '\0';
        (void)dsd_mbe_process_ambe2450_dataf(state->audio_out_temp_buf, &state->errs, &state->errs2, file_err_str,
                                             sizeof(state->err_str), ambe_d, state->cur_mp, state->prev_mp,
                                             state->prev_mp_enhanced, NULL);
    } else if (state->mbe_file_type == 2) {
        file_err_str[0] = '\0';
        (void)dsd_mbe_process_ambe2400_dataf(state->audio_out_temp_buf, &state->errs, &state->errs2, file_err_str,
                                             sizeof(state->err_str), ambe_d, state->cur_mp, state->prev_mp,
                                             state->prev_mp_enhanced, NULL);
    }
    DSD_STRNCPY(state->err_str, file_err_str, sizeof(state->err_str) - 1);
    state->err_str[sizeof(state->err_str) - 1] = '\0';
}

static int
play_ambe_file_frame(dsd_opts* opts, dsd_state* state, char ambe_d[49], char file_err_str[260], int want_static_wav) {
    if (readAmbe2450Data(opts, state, ambe_d) != 0) {
        return -1;
    }
    decrypt_ambe_with_pr_key(state, ambe_d);
    decode_ambe_file_frame(state, ambe_d, file_err_str);
    play_mbe_output_frame(opts, state, want_static_wav);
    return 0;
}

typedef struct {
    char imbe_d[88];
    char ambe_d[49];
    int vertex_ks_applied_l;
    int vertex_ks_applied_r;
} mbe_frame_ctx_t;

static void
mbe_prepare_frame_state(dsd_opts* opts, dsd_state* state, mbe_frame_ctx_t* frame_ctx,
                        dsd_vocoder_soft_bit imbe7100_soft_fr[7][24]) {
    (void)imbe7100_soft_fr;
    frame_ctx->vertex_ks_applied_l = 0;
    frame_ctx->vertex_ks_applied_r = 0;

    (void)dsd_dmr_apply_forced_algid(state);

    //these conditions should ensure no clashing with the BP/HBP/Scrambler key loading machanisms already coded in
    if (state->currentslot == 0 && state->payload_algid != 0 && state->payload_algid != 0x80 && state->keyloader == 1) {
        keyring(opts, state);
    }

    if (state->currentslot == 1 && state->payload_algidR != 0 && state->payload_algidR != 0x80
        && state->keyloader == 1) {
        keyring(opts, state);
    }

    DSD_MEMSET(frame_ctx->imbe_d, 0, sizeof(frame_ctx->imbe_d));
    DSD_MEMSET(frame_ctx->ambe_d, 0, sizeof(frame_ctx->ambe_d));
}

static void
mbe_load_aes_key_slot0(const dsd_state* state, uint8_t aes_key[32]) {
    for (int i = 0; i < 8; i++) {
        aes_key[i + 0] = (state->A1[0] >> (56 - (i * 8))) & 0xFF;
        aes_key[i + 8] = (state->A2[0] >> (56 - (i * 8))) & 0xFF;
        aes_key[i + 16] = (state->A3[0] >> (56 - (i * 8))) & 0xFF;
        aes_key[i + 24] = (state->A4[0] >> (56 - (i * 8))) & 0xFF;
    }
}

static void
mbe_init_p25p1_multicrypt_keystream(dsd_state* state, const uint8_t aes_key[32]) {
    if (state->p25vc != 0) {
        return;
    }

    if (state->payload_algid == 0x81 || state->payload_algid == 0x83) { //DES1 and DES3
        state->octet_counter = 11 + 8; //start on 19 for DES-OFB (8 discard + 8 LC + 3 reserved)
    } else if (state->payload_algid == 0x9F) {
        state->octet_counter = 11; //11 with info from LFSR run values (no discard)
    } else {
        state->octet_counter = 11 + 16; //start on 27 for AES (16 discard + 8 LC + 3 reserved)
    }
    DSD_MEMSET(state->ks_octetL, 0, sizeof(state->ks_octetL));

    if (state->payload_algid == 0x81) { //DES-56
        des_multi_keystream_output(state->payload_miP, state->R, state->ks_octetL, 1, 28);
    }
    if (state->payload_algid == 0x83) { //3DES, or TDEA
        tdea_multi_keystream_output(state->payload_miP, aes_key, state->ks_octetL, 1, 28);
    }
    if (state->payload_algid == 0x9F) { //DES-XL
        des_multi_keystream_output(state->payload_miP, state->R, state->ks_octetL, 2,
                                   state->xl_is_hdu); //xl_is_hdu determines lfsr run values
    }
    if (state->payload_algid == 0x84) { //AES256
        aes_ofb_keystream_output(state->aes_iv, aes_key, state->ks_octetL, 2, 14);
    }
    if (state->payload_algid == 0x89) { //AES128
        aes_ofb_keystream_output(state->aes_iv, aes_key, state->ks_octetL, 0, 14);
    }
}

static void
mbe_pack_imbe_bits(char imbe_d[88], uint8_t packed[11]) {
    int z = 0;
    for (int i = 0; i < 11; i++) {
        packed[i] = 0;
        for (int j = 0; j < 8; j++) {
            packed[i] = (uint8_t)((packed[i] << 1) + imbe_d[z]);
            imbe_d[z] = 0;
            z++;
        }
    }
}

static void
mbe_unpack_imbe_bits(char imbe_d[88], uint8_t packed[11]) {
    int z = 0;
    for (int i = 0; i < 11; i++) {
        for (int j = 0; j < 8; j++) {
            imbe_d[z++] = (packed[i] & 0x80) >> 7;
            packed[i] = (uint8_t)(packed[i] << 1);
        }
    }
}

static void
mbe_apply_p25p1_multicrypt(dsd_state* state, char imbe_d[88]) {
    uint8_t cipher[11];
    uint8_t plain[11];
    DSD_MEMSET(cipher, 0, sizeof(cipher));
    DSD_MEMSET(plain, 0, sizeof(plain));

    uint8_t aes_key[32];
    DSD_MEMSET(aes_key, 0, sizeof(aes_key));

    mbe_load_aes_key_slot0(state, aes_key);
    mbe_init_p25p1_multicrypt_keystream(state, aes_key);
    mbe_pack_imbe_bits(imbe_d, cipher);

    for (int i = 0; i < 11; i++) {
        plain[i] = cipher[i] ^ state->ks_octetL[state->octet_counter++];
    }
    mbe_unpack_imbe_bits(imbe_d, plain);
}

static void
mbe_apply_p25p1_rc4(dsd_state* state, char imbe_d[88]) {
    uint8_t cipher[11] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t plain[11] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t rckey[13] = {0x00, 0x00, 0x00, 0x00, 0x00, // <- RC4 Key
                         0x00, 0x00, 0x00, 0x00, 0x00, // <- MI
                         0x00, 0x00, 0x00};            // <- MI cont.

    //easier to manually load up rather than make a loop
    rckey[0] = ((state->R & 0xFF00000000) >> 32);
    rckey[1] = ((state->R & 0xFF000000) >> 24);
    rckey[2] = ((state->R & 0xFF0000) >> 16);
    rckey[3] = ((state->R & 0xFF00) >> 8);
    rckey[4] = ((state->R & 0xFF) >> 0);

    // load valid MI from state->payload_miP
    rckey[5] = ((state->payload_miP & 0xFF00000000000000) >> 56);
    rckey[6] = ((state->payload_miP & 0xFF000000000000) >> 48);
    rckey[7] = ((state->payload_miP & 0xFF0000000000) >> 40);
    rckey[8] = ((state->payload_miP & 0xFF00000000) >> 32);
    rckey[9] = ((state->payload_miP & 0xFF000000) >> 24);
    rckey[10] = ((state->payload_miP & 0xFF0000) >> 16);
    rckey[11] = ((state->payload_miP & 0xFF00) >> 8);
    rckey[12] = ((state->payload_miP & 0xFF) >> 0);

    //load imbe_d into imbe_cipher octets
    int z = 0;
    for (int i = 0; i < 11; i++) {
        cipher[i] = 0;
        plain[i] = 0;
        for (short int j = 0; j < 8; j++) {
            cipher[i] = cipher[i] << 1;
            cipher[i] = cipher[i] + imbe_d[z];
            imbe_d[z] = 0;
            z++;
        }
    }

    rc4_voice_decrypt(state->dropL, 13, 11, rckey, cipher, plain);
    state->dropL += 11;

    z = 0;
    for (short p = 0; p < 11; p++) {
        for (short o = 0; o < 8; o++) {
            imbe_d[z] = (plain[p] & 0x80) >> 7;
            plain[p] = plain[p] << 1;
            z++;
        }
    }
}

static int
mbe_p25p1_multicrypt_enabled(const dsd_state* state) {
    switch (state->payload_algid) {
        case 0x81:
        case 0x9F: return state->R != 0;
        case 0x83:
        case 0x84:
        case 0x89: return state->aes_key_loaded[0] == 1;
        default: return 0;
    }
}

static void
mbe_process_p25p1(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], dsd_vocoder_soft_bit imbe_soft_fr[8][23],
                  mbe_frame_ctx_t* frame_ctx) {
    mbe_process_result imbe_result;
    int have_imbe_result = decode_imbe7200_frame(state, imbe_fr, imbe_soft_fr, frame_ctx->imbe_d, &imbe_result);
    if (!have_imbe_result) {
        dsd_mbe_store_process_status(MBE_STATUS_INVALID_BITS, state->audio_out_temp_buf, &state->errs, &state->errs2,
                                     state->err_str, sizeof(state->err_str), NULL);
        return;
    }

    char decoded_imbe_d[88];
    DSD_MEMCPY(decoded_imbe_d, frame_ctx->imbe_d, sizeof(decoded_imbe_d));

    //P25p1 Multi Crypt Handler (DES1, DES3, DES-XL and AES)
    if (mbe_p25p1_multicrypt_enabled(state)) {
        mbe_apply_p25p1_multicrypt(state, frame_ctx->imbe_d);
    }

    //P25p1 RC4 Handling
    if (state->payload_algid == 0xAA && state->R != 0) {
        mbe_apply_p25p1_rc4(state, frame_ctx->imbe_d);
    }

    (void)dsd_mbe_strip_imbe_context_if_changed(decoded_imbe_d, frame_ctx->imbe_d, &imbe_result);

    mbe_process_params process = {state->audio_out_temp_buf, &state->errs,  &state->errs2,  state->err_str,
                                  sizeof(state->err_str),    state->cur_mp, state->prev_mp, state->prev_mp_enhanced};
    process_imbe4400_params(&process, frame_ctx->imbe_d, &imbe_result, have_imbe_result);
    update_p25_p1_voice_err_hist(state);

    if (dsd_frame_detail_enabled(opts)) {
        PrintIMBEData(opts, state, frame_ctx->imbe_d);
    }

    //increment vc counter by one.
    state->p25vc++;

    // if (opts->mbe_out_f != NULL && state->dmr_encL == 0) //only save if this bit not set
    if (opts->mbe_out_f != NULL) // && state->dmr_encL == 0) //only save if this bit not set //TODO: Fix this checkdown
    {
        saveImbe4400Data(opts, state, frame_ctx->imbe_d);
    }
}

static void
mbe_process_provoice(dsd_opts* opts, dsd_state* state, char imbe7100_fr[7][24], mbe_frame_ctx_t* frame_ctx) {
    mbe_process_result imbe_result;
    if (dsd_mbe_decode_imbe7100_frame(&state->errs, &state->errs2, (const char (*)[24])imbe7100_fr, frame_ctx->imbe_d,
                                      &imbe_result)
        < 0) {
        dsd_mbe_store_process_status(MBE_STATUS_INVALID_BITS, state->audio_out_temp_buf, &state->errs, &state->errs2,
                                     state->err_str, sizeof(state->err_str), NULL);
        return;
    }

    if (dsd_frame_detail_enabled(opts)) {
        PrintIMBEData(opts, state, frame_ctx->imbe_d);
        if (opts->payload == 1) {
            DSD_FPRINTF(stderr, " 7100");
        }
    }

    mbe_process_params process = {state->audio_out_temp_buf, &state->errs,  &state->errs2,  state->err_str,
                                  sizeof(state->err_str),    state->cur_mp, state->prev_mp, state->prev_mp_enhanced};
    process_imbe4400_params(&process, frame_ctx->imbe_d, &imbe_result, 1);
    if (DSD_SYNC_IS_P25P1(state->synctype)) {
        update_p25_p1_voice_err_hist(state);
    }

    if (opts->mbe_out_f != NULL) {
        saveImbe4400Data(opts, state, frame_ctx->imbe_d);
    }
}

static void
mbe_process_dstar(dsd_opts* opts, dsd_state* state, char ambe_fr[4][24], mbe_frame_ctx_t* frame_ctx) {
    (void)dsd_mbe_process_ambe3600x2400_framef(state->audio_out_temp_buf, &state->errs, &state->errs2, state->err_str,
                                               sizeof(state->err_str), ambe_fr, frame_ctx->ambe_d, state->cur_mp,
                                               state->prev_mp, state->prev_mp_enhanced);
    if (dsd_frame_detail_enabled(opts)) {
        PrintAMBEData(opts, state, frame_ctx->ambe_d);
    }
    if (opts->mbe_out_f != NULL) {
        saveAmbe2450Data(opts, state, frame_ctx->ambe_d);
    }
}

static void
mbe_apply_nxdn_cipher1(dsd_state* state, char ambe_d[49]) {
    if (state->payload_miN == 0) {
        state->payload_miN = state->R;
    }

    char ambe_temp[49];
    for (short int i = 0; i < 49; i++) {
        ambe_temp[i] = ambe_d[i];
        ambe_d[i] = 0;
    }
    LFSRN(ambe_temp, ambe_d, state);
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
void dsd_mbe_init_nxdn_cipher23_keystream(dsd_state* state);

void
dsd_mbe_init_nxdn_cipher23_keystream(dsd_state* state) {
    if (state->nxdn_cipher_type == 0x02 && state->nxdn_new_iv == 1 && state->nxdn_part_of_frame == 0) {
        DSD_MEMSET(state->ks_octetL, 0, sizeof(state->ks_octetL));
        DSD_MEMSET(state->ks_bitstreamL, 0, sizeof(state->ks_bitstreamL));
        des_multi_keystream_output(state->payload_miN, state->R, state->ks_octetL, 1, 26);
        state->bit_counterL = 0;
        unpack_byte_array_into_bit_array(state->ks_octetL + 8, state->ks_bitstreamL, 26 * 8);
        state->nxdn_new_iv = 0;
    }

    if (state->nxdn_cipher_type == 0x03 && state->nxdn_new_iv == 1 && state->nxdn_part_of_frame == 0) {
        DSD_MEMSET(state->ks_octetL, 0, sizeof(state->ks_octetL));
        DSD_MEMSET(state->ks_bitstreamL, 0, sizeof(state->ks_bitstreamL));
        aes_ofb_keystream_output(state->aes_iv, state->aes_key, state->ks_octetL, 2, 15);
        state->bit_counterL = 0;
        unpack_byte_array_into_bit_array(state->ks_octetL + 16, state->ks_bitstreamL, 14 * 16);
        state->nxdn_new_iv = 0;
    }
}

static void
mbe_apply_nxdn_cipher23(dsd_state* state, char ambe_d[49]) {
    dsd_mbe_init_nxdn_cipher23_keystream(state);

    //sanity check, don't exceed bit application counter
    if (state->bit_counterL > (1568 - 49)) {
        state->bit_counterL = (1568 - 49);
    }

    for (int i = 0; i < 49; i++) {
        ambe_d[i] ^= state->ks_bitstreamL[state->bit_counterL++];
    }
}

static void
mbe_apply_vendor_retevis(dsd_state* state, char ambe_d[49]) {
    (void)retevis_rc2_apply_frame49(state, ambe_d);
}

static void
mbe_apply_vendor_tyt_ap(const dsd_state* state, char ambe_d[49]) {
    (void)tyt_ap_pc4_apply_frame49(state, ambe_d);
}

static void
mbe_apply_vendor_baofeng(const dsd_state* state, char ambe_d[49]) {
    (void)baofeng_pc5_apply_frame49(state, ambe_d);
}

static void
mbe_apply_vendor_tyt_ep(const dsd_state* state, char ambe_d[49]) {
    (void)tyt_ep_aes_apply_frame49(state, ambe_d);
}

static void
mbe_apply_vendor_static_scramblers(dsd_state* state, char ambe_d[49]) {
    (void)ken_dmr_scrambler_apply_frame49(state, state->currentslot, ambe_d);
    (void)anytone_bp_apply_frame49(state, state->currentslot, ambe_d);
}

static void
mbe_apply_vendor_overlays(dsd_state* state, char ambe_d[49]) {
    mbe_apply_vendor_retevis(state, ambe_d);
    mbe_apply_vendor_tyt_ap(state, ambe_d);
    mbe_apply_vendor_baofeng(state, ambe_d);
    mbe_apply_vendor_tyt_ep(state, ambe_d);
    mbe_apply_vendor_static_scramblers(state, ambe_d);
}

static uint32_t
mbe_hash_tg_for_key(uint32_t tg) {
    uint32_t hash = tg & 0xFFFFFF;
    if (hash <= 0xFFFF) {
        return hash;
    }

    uint8_t hash_bits[24];
    DSD_MEMSET(hash_bits, 0, sizeof(hash_bits));
    for (int i = 0; i < 24; i++) {
        hash_bits[i] = ((hash << i) & 0x800000) >> 23;
    }
    hash = ComputeCrcCCITT16d(hash_bits, 24);
    return hash & 0xFFFF;
}

static void
mbe_slot_apply_straight_ks_left(dsd_state* state, char ambe_d[49]) {
    if (state->straight_ks == 1 && state->straight_mod > 0) {
        state->dmr_so = 0;
        state->p25_service_options_valid[0] = 0;
        state->payload_algid = 0;
        straight_mod_xor_apply_frame49(state, state->currentslot, ambe_d);
    }
}

static void
mbe_slot_apply_straight_ks_right(dsd_state* state, char ambe_d[49]) {
    if (state->straight_ks == 1 && state->straight_mod > 0) {
        state->dmr_soR = 0;
        state->p25_service_options_valid[1] = 0;
        state->payload_algidR = 0;
        straight_mod_xor_apply_frame49(state, state->currentslot, ambe_d);
    }
}

static void
mbe_finalize_slot_left(dsd_opts* opts, dsd_state* state, char ambe_d[49], mbe_process_result* ambe_result,
                       int have_ambe_result) {
    mbe_process_params process = {state->audio_out_temp_buf, &state->errs,  &state->errs2,  state->err_str,
                                  sizeof(state->err_str),    state->cur_mp, state->prev_mp, state->prev_mp_enhanced};
    process_ambe2450_params(&process, ambe_d, ambe_result, have_ambe_result);
    p25p2_record_voice_err(state, state->errs2);
    if (dsd_frame_detail_enabled(opts)) {
        PrintAMBEData(opts, state, ambe_d);
    }
    if (opts->mbe_out_f != NULL && (state->dmr_encL == 0 || opts->dmr_mute_encL == 0)) {
        saveAmbe2450Data(opts, state, ambe_d);
    }
}

static void
mbe_finalize_slot_right(dsd_opts* opts, dsd_state* state, char ambe_d[49], mbe_process_result* ambe_result,
                        int have_ambe_result) {
    mbe_process_params process = {
        state->audio_out_temp_bufR, &state->errsR,  &state->errs2R,  state->err_strR,
        sizeof(state->err_strR),    state->cur_mp2, state->prev_mp2, state->prev_mp_enhanced2};
    process_ambe2450_params(&process, ambe_d, ambe_result, have_ambe_result);
    p25p2_record_voice_err(state, state->errs2R);
    if (dsd_frame_detail_enabled(opts)) {
        PrintAMBEData(opts, state, ambe_d);
    }
    if (opts->mbe_out_fR != NULL && (state->dmr_encR == 0 || opts->dmr_mute_encR == 0)) {
        saveAmbe2450DataR(opts, state, ambe_d);
    }
}

static void
mbe_process_nxdn(dsd_opts* opts, dsd_state* state, char ambe_fr[4][24], dsd_vocoder_soft_bit ambe_soft_fr[4][24],
                 mbe_frame_ctx_t* frame_ctx) {
    mbe_process_result ambe_result;
    int have_ambe_result =
        decode_ambe2450_frame(&state->errs, &state->errs2, ambe_fr, ambe_soft_fr, frame_ctx->ambe_d, &ambe_result);
    if (!have_ambe_result) {
        dsd_mbe_store_process_status(MBE_STATUS_INVALID_BITS, state->audio_out_temp_buf, &state->errs, &state->errs2,
                                     state->err_str, sizeof(state->err_str), NULL);
        return;
    }

    char decoded_ambe_d[49];
    DSD_MEMCPY(decoded_ambe_d, frame_ctx->ambe_d, sizeof(decoded_ambe_d));

    if ((state->nxdn_cipher_type == 0x01 && state->R != 0) || (state->M == 1 && state->R > 0)) {
        mbe_apply_nxdn_cipher1(state, frame_ctx->ambe_d);
    }

    //NXDN Generic Cipher 2 and Cipher 3 Keystream Application (to be tested)
    else if ((state->nxdn_cipher_type == 0x02 && state->R != 0)
             || (state->nxdn_cipher_type == 0x03 && state->aes_key_loaded[0] == 1)) {
        mbe_apply_nxdn_cipher23(state, frame_ctx->ambe_d);
    }

    (void)dsd_mbe_strip_ambe_context_if_changed(decoded_ambe_d, frame_ctx->ambe_d, &ambe_result);

    mbe_process_params process = {state->audio_out_temp_buf, &state->errs,  &state->errs2,  state->err_str,
                                  sizeof(state->err_str),    state->cur_mp, state->prev_mp, state->prev_mp_enhanced};
    process_ambe2450_params(&process, frame_ctx->ambe_d, &ambe_result, have_ambe_result);
    p25p2_record_voice_err(state, state->errs2);

    if (dsd_frame_detail_enabled(opts)) {
        PrintAMBEData(opts, state, frame_ctx->ambe_d);
    }

    if (opts->mbe_out_f != NULL && (state->dmr_encL == 0 || opts->dmr_mute_encL == 0)) {
        saveAmbe2450Data(opts, state, frame_ctx->ambe_d);
    }
}

static void
mbeslot_left_autoload_keys(dsd_opts* opts, dsd_state* state) {
    if (state->M == 0 && state->payload_algid == 0) {
        uint32_t hash = mbe_hash_tg_for_key((uint32_t)state->lasttg);
        if (state->rkey_array[hash] != 0) {
            state->K = state->rkey_array[hash] & 0xFF;                     //doesn't exceed 255
            state->K1 = state->H = state->rkey_array[hash] & 0xFFFFFFFFFF; //doesn't exceed 40-bit limit
            state->K2 = state->K3 = state->K4 = 0ULL;
            state->hytera_key_segments = 1U;
            opts->dmr_mute_encL = 0;
        }
    }
}

static void
mbeslot_right_autoload_keys(dsd_opts* opts, dsd_state* state) {
    if (state->M == 0 && state->payload_algidR == 0) {
        uint32_t hash = mbe_hash_tg_for_key((uint32_t)state->lasttgR);
        if (state->rkey_array[hash] != 0) {
            state->K = state->rkey_array[hash] & 0xFF;
            state->K1 = state->H = state->rkey_array[hash] & 0xFFFFFFFFFF;
            state->K2 = state->K3 = state->K4 = 0ULL;
            state->hytera_key_segments = 1U;
            opts->dmr_mute_encR = 0;
        }
    }
}

static void
mbeslot_left_apply_basic_privacy(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if ((state->K > 0 && state->dmr_so & 0x40 && state->payload_keyid == 0 && state->dmr_fid == 0x10)
        || (state->K > 0 && state->M == 1)) {
        (void)dmr_basic_privacy_apply_frame49(state->K, frame_ctx->ambe_d);
    }

    if ((state->K1 > 0 && state->dmr_so & 0x40 && state->payload_keyid == 0 && state->dmr_fid == 0x68)
        || (state->K1 > 0 && state->M == 1)) {
        (void)hytera_bp_apply_frame49(state->K1, state->K2, state->K3, state->K4, &state->DMRvcL, frame_ctx->ambe_d);
    }
}

static void
mbeslot_right_apply_basic_privacy(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if ((state->K > 0 && state->dmr_soR & 0x40 && state->payload_keyidR == 0 && state->dmr_fidR == 0x10)
        || (state->K > 0 && state->M == 1)) {
        (void)dmr_basic_privacy_apply_frame49(state->K, frame_ctx->ambe_d);
    }

    if ((state->K1 > 0 && state->dmr_soR & 0x40 && state->payload_keyidR == 0 && state->dmr_fidR == 0x68)
        || (state->K1 > 0 && state->M == 1)) {
        (void)hytera_bp_apply_frame49(state->K1, state->K2, state->K3, state->K4, &state->DMRvcR, frame_ctx->ambe_d);
    }
}

static void
mbeslot_left_apply_vertex_standard(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (state->payload_algid == 0x07 && state->straight_ks != 1) {
        frame_ctx->vertex_ks_applied_l = vertex_key_map_apply_frame49(state, 0, state->R, frame_ctx->ambe_d);
        if (frame_ctx->vertex_ks_applied_l == 1) {
            state->dmr_so &= ~0x40U;
        }
        if (frame_ctx->vertex_ks_applied_l == 0 && state->vertex_ks_warned[0] == 0) {
            DSD_FPRINTF(stderr, "\n DMR Vertex Std voice decrypt needs a mapped keystream (--dmr-vertex-ks-csv) or "
                                "manual -S bits:hex[:offset[:step]].");
            state->vertex_ks_warned[0] = 1;
        }
    }
}

static void
mbeslot_right_apply_vertex_standard(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (state->payload_algidR == 0x07 && state->straight_ks != 1) {
        frame_ctx->vertex_ks_applied_r = vertex_key_map_apply_frame49(state, 1, state->RR, frame_ctx->ambe_d);
        if (frame_ctx->vertex_ks_applied_r == 1) {
            state->dmr_soR &= ~0x40U;
        }
        if (frame_ctx->vertex_ks_applied_r == 0 && state->vertex_ks_warned[1] == 0) {
            DSD_FPRINTF(stderr, "\n DMR Vertex Std voice decrypt needs a mapped keystream (--dmr-vertex-ks-csv) or "
                                "manual -S bits:hex[:offset[:step]].");
            state->vertex_ks_warned[1] = 1;
        }
    }
}

static void
mbeslot_left_apply_des(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (!((state->payload_algid == 0x22 && state->R != 0) || (state->payload_algid == 0x81 && state->R != 0))) {
        return;
    }

    if (state->DMRvcL > 17) {
        state->DMRvcL = 17;
    }

    int z = 0;
    if (state->DMRvcL == 0) {
        int n = 8;
        DSD_MEMSET(state->ks_octetL, 0, sizeof(state->ks_octetL));
        DSD_MEMSET(state->ks_bitstreamL, 0, sizeof(state->ks_bitstreamL));
        state->bit_counterL = 0;
        des_multi_keystream_output(state->payload_miP, state->R, state->ks_octetL, 1, 19); //18 + 1

        for (int i = 0; i < 18 * 8; i++) //19 blocks minus 1 discard block at 8 bits each
        {
            for (int j = 0; j < 8; j++) {
                uint8_t b = (((state->ks_octetL[n] << j) & 0x80) >> 7);
                state->ks_bitstreamL[z++] = b;
            }
            n++;
        }
    }

    z = 0;
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 8; j++) {
            frame_ctx->ambe_d[z++] ^= state->ks_bitstreamL[state->bit_counterL++];
        }
    }
    frame_ctx->ambe_d[48] ^= state->ks_bitstreamL[state->bit_counterL++];
    state->bit_counterL += 7;
    state->DMRvcL++;
}

static void
mbeslot_right_apply_des(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (!((state->payload_algidR == 0x22 && state->RR != 0) || (state->payload_algidR == 0x81 && state->RR != 0))) {
        return;
    }

    if (state->DMRvcR > 17) {
        state->DMRvcR = 17;
    }

    int z = 0;
    if (state->DMRvcR == 0) {
        int n = 8;
        DSD_MEMSET(state->ks_octetR, 0, sizeof(state->ks_octetR));
        DSD_MEMSET(state->ks_bitstreamR, 0, sizeof(state->ks_bitstreamR));
        state->bit_counterR = 0;
        des_multi_keystream_output(state->payload_miN, state->RR, state->ks_octetR, 1, 19);
        for (int i = 0; i < 18 * 8; i++) {
            for (int j = 0; j < 8; j++) {
                uint8_t b = (((state->ks_octetR[n] << j) & 0x80) >> 7);
                state->ks_bitstreamR[z++] = b;
            }
            n++;
        }
    }

    z = 0;
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 8; j++) {
            frame_ctx->ambe_d[z++] ^= state->ks_bitstreamR[state->bit_counterR++];
        }
    }
    frame_ctx->ambe_d[48] ^= state->ks_bitstreamR[state->bit_counterR++];
    state->bit_counterR += 7;
    state->DMRvcR++;
}

static int
mbeslot_left_aes_enabled(const dsd_state* state) {
    switch (state->payload_algid) {
        case 0x02: return state->R != 0;
        case 0x24:
        case 0x25:
        case 0x36:
        case 0x37:
        case 0x84:
        case 0x89: return dsd_dmr_voice_slot_can_decrypt(state, 0, state->payload_algid, state->R);
        default: return 0;
    }
}

static int
mbeslot_right_aes_enabled(const dsd_state* state) {
    switch (state->payload_algidR) {
        case 0x02: return state->RR != 0;
        case 0x24:
        case 0x25:
        case 0x36:
        case 0x37:
        case 0x84:
        case 0x89: return dsd_dmr_voice_slot_can_decrypt(state, 1, state->payload_algidR, state->RR);
        default: return 0;
    }
}

static void
mbeslot_left_make_aes_key(const dsd_state* state, uint8_t aes_key[32]) {
    DSD_MEMSET(aes_key, 0, 32 * sizeof(uint8_t));
    for (int i = 0; i < 8; i++) {
        aes_key[i + 0] = (state->A1[0] >> (56 - (i * 8))) & 0xFF;
        aes_key[i + 8] = (state->A2[0] >> (56 - (i * 8))) & 0xFF;
        aes_key[i + 16] = (state->A3[0] >> (56 - (i * 8))) & 0xFF;
        aes_key[i + 24] = (state->A4[0] >> (56 - (i * 8))) & 0xFF;
    }
}

static void
mbeslot_right_make_aes_key(const dsd_state* state, uint8_t aes_key[32]) {
    DSD_MEMSET(aes_key, 0, 32 * sizeof(uint8_t));
    for (int i = 0; i < 8; i++) {
        aes_key[i + 0] = (state->A1[1] >> (56 - (i * 8))) & 0xFF;
        aes_key[i + 8] = (state->A2[1] >> (56 - (i * 8))) & 0xFF;
        aes_key[i + 16] = (state->A3[1] >> (56 - (i * 8))) & 0xFF;
        aes_key[i + 24] = (state->A4[1] >> (56 - (i * 8))) & 0xFF;
    }
}

static int
mbeslot_left_keystream_start_index(dsd_opts* opts, dsd_state* state, const uint8_t aes_key[32]) {
    int n = 16; //n=16 for AES-OFB discard round

    if (state->payload_algid == 0x24 || state->payload_algid == 0x89) { //AES128
        aes_ofb_keystream_output(state->aes_iv, aes_key, state->ks_octetL, 0, 10);
    }
    if (state->payload_algid == 0x25 || state->payload_algid == 0x84) { //AES256
        aes_ofb_keystream_output(state->aes_iv, aes_key, state->ks_octetL, 2, 10);
    }
    if (state->payload_algid == 0x02) {
        n = 0;
        hytera_enhanced_rc4_setup(opts, state, state->R, state->payload_mi);
    }
    if (state->payload_algid == 0x36) {
        n = 0;
        kirisun_adv_keystream_creation(state);
    }
    if (state->payload_algid == 0x37) {
        n = 0;
        kirisun_uni_keystream_creation(state);
    }

    return n;
}

static int
mbeslot_right_keystream_start_index(dsd_opts* opts, dsd_state* state, const uint8_t aes_key[32]) {
    int n = 16;

    if (state->payload_algidR == 0x24 || state->payload_algidR == 0x89) {
        aes_ofb_keystream_output(state->aes_ivR, aes_key, state->ks_octetR, 0, 10);
    }
    if (state->payload_algidR == 0x25 || state->payload_algidR == 0x84) {
        aes_ofb_keystream_output(state->aes_ivR, aes_key, state->ks_octetR, 2, 10);
    }
    if (state->payload_algidR == 0x02) {
        n = 0;
        hytera_enhanced_rc4_setup(opts, state, state->RR, state->payload_miR);
    }
    if (state->payload_algidR == 0x36) {
        n = 0;
        kirisun_adv_keystream_creation(state);
    }
    if (state->payload_algidR == 0x37) {
        n = 0;
        kirisun_uni_keystream_creation(state);
    }

    return n;
}

static void
mbeslot_left_expand_keystream_bits(dsd_state* state, int start_index) {
    int z = 0;
    for (int i = 0; i < 9 * 16; i++) //9 rounds at 16 octets
    {
        for (int j = 0; j < 8; j++) {
            uint8_t b = (((state->ks_octetL[start_index] << j) & 0x80) >> 7);
            state->ks_bitstreamL[z++] = b;
        }
        start_index++;
    }
}

static void
mbeslot_right_expand_keystream_bits(dsd_state* state, int start_index) {
    int z = 0;
    for (int i = 0; i < 9 * 16; i++) {
        for (int j = 0; j < 8; j++) {
            uint8_t b = (((state->ks_octetR[start_index] << j) & 0x80) >> 7);
            state->ks_bitstreamR[z++] = b;
        }
        start_index++;
    }
}

static void
mbeslot_left_init_keystream(dsd_opts* opts, dsd_state* state, const uint8_t aes_key[32]) {
    DSD_MEMSET(state->ks_octetL, 0, sizeof(state->ks_octetL));
    DSD_MEMSET(state->ks_bitstreamL, 0, sizeof(state->ks_bitstreamL));
    state->bit_counterL = 0;
    int start_index = mbeslot_left_keystream_start_index(opts, state, aes_key);
    mbeslot_left_expand_keystream_bits(state, start_index);
}

static void
mbeslot_right_init_keystream(dsd_opts* opts, dsd_state* state, const uint8_t aes_key[32]) {
    DSD_MEMSET(state->ks_octetR, 0, sizeof(state->ks_octetR));
    DSD_MEMSET(state->ks_bitstreamR, 0, sizeof(state->ks_bitstreamR));
    state->bit_counterR = 0;
    int start_index = mbeslot_right_keystream_start_index(opts, state, aes_key);
    mbeslot_right_expand_keystream_bits(state, start_index);
}

static void
mbeslot_left_apply_keystream_bits(dsd_state* state, char ambe_d[49]) {
    (void)dmr_voice_stream_apply_frame49(state->ks_bitstreamL, &state->bit_counterL, state->payload_algid, ambe_d);
}

static void
mbeslot_right_apply_keystream_bits(dsd_state* state, char ambe_d[49]) {
    (void)dmr_voice_stream_apply_frame49(state->ks_bitstreamR, &state->bit_counterR, state->payload_algidR, ambe_d);
}

static void
mbeslot_left_apply_aes_and_streams(dsd_opts* opts, dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (!mbeslot_left_aes_enabled(state)) {
        return;
    }

    uint8_t aes_key[32];
    mbeslot_left_make_aes_key(state, aes_key);

    if (state->DMRvcL > 17) {
        state->DMRvcL = 17;
    }
    if (state->DMRvcL == 0) {
        mbeslot_left_init_keystream(opts, state, aes_key);
    }

    mbeslot_left_apply_keystream_bits(state, frame_ctx->ambe_d);
    state->DMRvcL++;
    opts->dmr_mute_encL = 0; //shim to unmute
}

static void
mbeslot_right_apply_aes_and_streams(dsd_opts* opts, dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (!mbeslot_right_aes_enabled(state)) {
        return;
    }

    uint8_t aes_key[32];
    mbeslot_right_make_aes_key(state, aes_key);

    if (state->DMRvcR > 17) {
        state->DMRvcR = 17;
    }
    if (state->DMRvcR == 0) {
        mbeslot_right_init_keystream(opts, state, aes_key);
    }

    mbeslot_right_apply_keystream_bits(state, frame_ctx->ambe_d);
    state->DMRvcR++;
    opts->dmr_mute_encR = 0; //shim to unmute
}

static void
mbeslot_left_apply_rc4(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (!(state->payload_algid == 0x21 && state->R != 0)) {
        return;
    }

    uint8_t cipher[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t plain[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t rckey[9] = {0x00, 0x00, 0x00, 0x00, 0x00, // <- RC4 Key
                        0x00, 0x00, 0x00, 0x00};      // <- MI

    rckey[0] = ((state->R & 0xFF00000000) >> 32);
    rckey[1] = ((state->R & 0xFF000000) >> 24);
    rckey[2] = ((state->R & 0xFF0000) >> 16);
    rckey[3] = ((state->R & 0xFF00) >> 8);
    rckey[4] = ((state->R & 0xFF) >> 0);
    rckey[5] = ((state->payload_mi & 0xFF000000) >> 24);
    rckey[6] = ((state->payload_mi & 0xFF0000) >> 16);
    rckey[7] = ((state->payload_mi & 0xFF00) >> 8);
    rckey[8] = ((state->payload_mi & 0xFF) >> 0);

    if (dmr_ambe49_should_skip_voice_stream(frame_ctx->ambe_d) == 1) {
        state->dropL += 7;
        return;
    }

    pack_ambe(frame_ctx->ambe_d, cipher, 49);
    if (state->errs < 3) {
        rc4_voice_decrypt(state->dropL, 9, 7, rckey, cipher, plain);
    } else {
        DSD_MEMCPY(plain, cipher, sizeof(plain));
    }
    state->dropL += 7;
    DSD_MEMSET(frame_ctx->ambe_d, 0, 49 * sizeof(char));
    unpack_ambe(plain, frame_ctx->ambe_d);
}

static void
mbeslot_right_apply_rc4(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (!(state->payload_algidR == 0x21 && state->RR != 0)) {
        return;
    }

    uint8_t cipher[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t plain[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t rckey[9] = {0x00, 0x00, 0x00, 0x00, 0x00, // <- RC4 Key
                        0x00, 0x00, 0x00, 0x00};      // <- MI

    rckey[0] = ((state->RR & 0xFF00000000) >> 32);
    rckey[1] = ((state->RR & 0xFF000000) >> 24);
    rckey[2] = ((state->RR & 0xFF0000) >> 16);
    rckey[3] = ((state->RR & 0xFF00) >> 8);
    rckey[4] = ((state->RR & 0xFF) >> 0);
    rckey[5] = ((state->payload_miR & 0xFF000000) >> 24);
    rckey[6] = ((state->payload_miR & 0xFF0000) >> 16);
    rckey[7] = ((state->payload_miR & 0xFF00) >> 8);
    rckey[8] = ((state->payload_miR & 0xFF) >> 0);

    if (dmr_ambe49_should_skip_voice_stream(frame_ctx->ambe_d) == 1) {
        state->dropR += 7;
        return;
    }

    pack_ambe(frame_ctx->ambe_d, cipher, 49);
    if (state->errsR < 3) {
        rc4_voice_decrypt(state->dropR, 9, 7, rckey, cipher, plain);
    } else {
        DSD_MEMCPY(plain, cipher, sizeof(plain));
    }
    state->dropR += 7;
    DSD_MEMSET(frame_ctx->ambe_d, 0, 49 * sizeof(char));
    unpack_ambe(plain, frame_ctx->ambe_d);
}

static void
mbeslot_left_apply_p25p2_rc4(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (!(state->payload_algid == 0xAA && state->R != 0 && DSD_SYNC_IS_P25P2(state->synctype))) {
        return;
    }

    uint8_t cipher[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t plain[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t rckey[13] = {0x00, 0x00, 0x00, 0x00, 0x00, // <- RC4 Key
                         0x00, 0x00, 0x00, 0x00, 0x00, // <- MI
                         0x00, 0x00, 0x00};            // <- MI cont.

    rckey[0] = ((state->R & 0xFF00000000) >> 32);
    rckey[1] = ((state->R & 0xFF000000) >> 24);
    rckey[2] = ((state->R & 0xFF0000) >> 16);
    rckey[3] = ((state->R & 0xFF00) >> 8);
    rckey[4] = ((state->R & 0xFF) >> 0);
    rckey[5] = ((state->payload_miP & 0xFF00000000000000) >> 56);
    rckey[6] = ((state->payload_miP & 0xFF000000000000) >> 48);
    rckey[7] = ((state->payload_miP & 0xFF0000000000) >> 40);
    rckey[8] = ((state->payload_miP & 0xFF00000000) >> 32);
    rckey[9] = ((state->payload_miP & 0xFF000000) >> 24);
    rckey[10] = ((state->payload_miP & 0xFF0000) >> 16);
    rckey[11] = ((state->payload_miP & 0xFF00) >> 8);
    rckey[12] = ((state->payload_miP & 0xFF) >> 0);

    pack_ambe(frame_ctx->ambe_d, cipher, 49);
    rc4_voice_decrypt(state->dropL, 13, 7, rckey, cipher, plain);
    state->dropL += 7;
    DSD_MEMSET(frame_ctx->ambe_d, 0, 49 * sizeof(char));
    unpack_ambe(plain, frame_ctx->ambe_d);
}

static void
mbeslot_right_apply_p25p2_rc4(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (!(state->payload_algidR == 0xAA && state->RR != 0 && DSD_SYNC_IS_P25P2(state->synctype))) {
        return;
    }

    uint8_t cipher[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t plain[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t rckey[13] = {0x00, 0x00, 0x00, 0x00, 0x00, // <- RC4 Key
                         0x00, 0x00, 0x00, 0x00, 0x00, // <- MI
                         0x00, 0x00, 0x00};            // <- MI cont.

    rckey[0] = ((state->RR & 0xFF00000000) >> 32);
    rckey[1] = ((state->RR & 0xFF000000) >> 24);
    rckey[2] = ((state->RR & 0xFF0000) >> 16);
    rckey[3] = ((state->RR & 0xFF00) >> 8);
    rckey[4] = ((state->RR & 0xFF) >> 0);
    rckey[5] = ((state->payload_miN & 0xFF00000000000000) >> 56);
    rckey[6] = ((state->payload_miN & 0xFF000000000000) >> 48);
    rckey[7] = ((state->payload_miN & 0xFF0000000000) >> 40);
    rckey[8] = ((state->payload_miN & 0xFF00000000) >> 32);
    rckey[9] = ((state->payload_miN & 0xFF000000) >> 24);
    rckey[10] = ((state->payload_miN & 0xFF0000) >> 16);
    rckey[11] = ((state->payload_miN & 0xFF00) >> 8);
    rckey[12] = ((state->payload_miN & 0xFF) >> 0);

    pack_ambe(frame_ctx->ambe_d, cipher, 49);
    rc4_voice_decrypt(state->dropR, 13, 7, rckey, cipher, plain);
    state->dropR += 7;
    DSD_MEMSET(frame_ctx->ambe_d, 0, 49 * sizeof(char));
    unpack_ambe(plain, frame_ctx->ambe_d);
}

static void
mbeslot_process_left(dsd_opts* opts, dsd_state* state, char ambe_fr[4][24], dsd_vocoder_soft_bit ambe_soft_fr[4][24],
                     mbe_frame_ctx_t* frame_ctx) {
    mbe_process_result ambe_result;
    int have_ambe_result =
        decode_ambe2450_frame(&state->errs, &state->errs2, ambe_fr, ambe_soft_fr, frame_ctx->ambe_d, &ambe_result);
    if (!have_ambe_result) {
        dsd_mbe_store_process_status(MBE_STATUS_INVALID_BITS, state->audio_out_temp_buf, &state->errs, &state->errs2,
                                     state->err_str, sizeof(state->err_str), NULL);
        return;
    }

    char decoded_ambe_d[49];
    DSD_MEMCPY(decoded_ambe_d, frame_ctx->ambe_d, sizeof(decoded_ambe_d));

    mbeslot_left_autoload_keys(opts, state);
    mbeslot_left_apply_basic_privacy(state, frame_ctx);
    mbeslot_left_apply_vertex_standard(state, frame_ctx);
    mbeslot_left_apply_des(state, frame_ctx);
    mbeslot_left_apply_aes_and_streams(opts, state, frame_ctx);
    mbeslot_left_apply_rc4(state, frame_ctx);
    mbeslot_left_apply_p25p2_rc4(state, frame_ctx);
    mbe_apply_vendor_overlays(state, frame_ctx->ambe_d);
    mbe_slot_apply_straight_ks_left(state, frame_ctx->ambe_d);
    (void)dsd_mbe_strip_ambe_context_if_changed(decoded_ambe_d, frame_ctx->ambe_d, &ambe_result);
    mbe_finalize_slot_left(opts, state, frame_ctx->ambe_d, &ambe_result, have_ambe_result);
}

static void
mbeslot_process_right(dsd_opts* opts, dsd_state* state, char ambe_fr[4][24], dsd_vocoder_soft_bit ambe_soft_fr[4][24],
                      mbe_frame_ctx_t* frame_ctx) {
    mbe_process_result ambe_result;
    int have_ambe_result =
        decode_ambe2450_frame(&state->errsR, &state->errs2R, ambe_fr, ambe_soft_fr, frame_ctx->ambe_d, &ambe_result);
    if (!have_ambe_result) {
        dsd_mbe_store_process_status(MBE_STATUS_INVALID_BITS, state->audio_out_temp_bufR, &state->errsR, &state->errs2R,
                                     state->err_strR, sizeof(state->err_strR), NULL);
        return;
    }

    char decoded_ambe_d[49];
    DSD_MEMCPY(decoded_ambe_d, frame_ctx->ambe_d, sizeof(decoded_ambe_d));

    mbeslot_right_autoload_keys(opts, state);
    mbeslot_right_apply_basic_privacy(state, frame_ctx);
    mbeslot_right_apply_vertex_standard(state, frame_ctx);
    mbeslot_right_apply_des(state, frame_ctx);
    mbeslot_right_apply_aes_and_streams(opts, state, frame_ctx);
    mbeslot_right_apply_rc4(state, frame_ctx);
    mbeslot_right_apply_p25p2_rc4(state, frame_ctx);
    mbe_apply_vendor_overlays(state, frame_ctx->ambe_d);
    mbe_slot_apply_straight_ks_right(state, frame_ctx->ambe_d);
    (void)dsd_mbe_strip_ambe_context_if_changed(decoded_ambe_d, frame_ctx->ambe_d, &ambe_result);
    mbe_finalize_slot_right(opts, state, frame_ctx->ambe_d, &ambe_result, have_ambe_result);
}

static void
mbe_process_slot_traffic(dsd_opts* opts, dsd_state* state, char ambe_fr[4][24],
                         dsd_vocoder_soft_bit ambe_soft_fr[4][24], mbe_frame_ctx_t* frame_ctx) {
    if (state->currentslot == 0) //&& opts->dmr_stereo == 1
    {
        mbeslot_process_left(opts, state, ambe_fr, ambe_soft_fr, frame_ctx);
    }

    if (state->currentslot == 1) //&& opts->dmr_stereo == 1
    {
        mbeslot_process_right(opts, state, ambe_fr, ambe_soft_fr, frame_ctx);
    }
}

static void
mbe_post_apply_reverse_mute(dsd_opts* opts, int16_t* enc, int16_t* mute_flag) {
    if (opts->reverse_mute != 1) {
        return;
    }
    if (*enc == 0) {
        *enc = 1;
        opts->unmute_encrypted_p25 = 0;
        *mute_flag = 1;
    } else {
        *enc = 0;
        opts->unmute_encrypted_p25 = 1;
        *mute_flag = 0;
    }
}

static void
mbe_post_apply_p25p2_metadata_gate(const dsd_state* state, int16_t* enc) {
    if (DSD_SYNC_IS_P25P2(state->synctype) && (state->p2_wacn == 0 || state->p2_sysid == 0 || state->p2_cc == 0)) {
        *enc = 1;
    }
}

static void
mbe_post_apply_forced_clear_gate(const dsd_state* state, int16_t* enc) {
    if (state->baofeng_ap == 1 || state->csi_ee == 1 || state->ken_sc == 1) {
        *enc = 0;
    }
}

static void
mbe_post_left_apply_decryptability(dsd_state* state, const mbe_frame_ctx_t* frame_ctx) {
    if (state->payload_algid == 0) {
        if (dsd_dmr_missing_alg_key_can_decrypt(state, 0)) {
            state->dmr_encL = 0;
        }
        return;
    }

    if (dsd_dmr_voice_slot_can_decrypt(state, 0, state->payload_algid, state->R)
        || (state->payload_algid == 0x07 && frame_ctx->vertex_ks_applied_l == 1)) {
        state->dmr_encL = 0;
    }
}

static void
mbe_post_left_audio(dsd_opts* opts, dsd_state* state, const mbe_frame_ctx_t* frame_ctx) {
    if ((opts->dmr_mono != 1 && opts->dmr_stereo != 1) || state->currentslot != 0) {
        return;
    }

    int enc_bit = (state->dmr_so >> 6) & 0x1;
    state->dmr_encL = (enc_bit == 1 || (state->payload_algid != 0 && state->payload_algid != 0x80)) ? 1 : 0;

    mbe_post_left_apply_decryptability(state, frame_ctx);
    mbe_post_apply_p25p2_metadata_gate(state, &state->dmr_encL);
    mbe_post_apply_forced_clear_gate(state, &state->dmr_encL);
    mbe_post_apply_reverse_mute(opts, &state->dmr_encL, &opts->dmr_mute_encL);

    state->debug_audio_errors += state->errs2;
    if ((state->dmr_encL == 0 || opts->dmr_mute_encL == 0) && opts->floating_point == 0) {
        processAudio(opts, state);
    }
    DSD_MEMCPY(state->f_l, state->audio_out_temp_buf, sizeof(state->f_l));
}

static void
mbe_post_right_apply_decryptability(dsd_state* state, const mbe_frame_ctx_t* frame_ctx) {
    if (state->payload_algidR == 0) {
        if (dsd_dmr_missing_alg_key_can_decrypt(state, 1)) {
            state->dmr_encR = 0;
        }
        return;
    }

    if (dsd_dmr_voice_slot_can_decrypt(state, 1, state->payload_algidR, state->RR)
        || (state->payload_algidR == 0x07 && frame_ctx->vertex_ks_applied_r == 1)) {
        state->dmr_encR = 0;
    }
}

static void
mbe_post_right_audio(dsd_opts* opts, dsd_state* state, const mbe_frame_ctx_t* frame_ctx) {
    if (opts->dmr_stereo != 1 || state->currentslot != 1) {
        return;
    }

    int enc_bit = (state->dmr_soR >> 6) & 0x1;
    state->dmr_encR = (enc_bit == 1 || (state->payload_algidR != 0 && state->payload_algidR != 0x80)) ? 1 : 0;

    mbe_post_right_apply_decryptability(state, frame_ctx);
    mbe_post_apply_p25p2_metadata_gate(state, &state->dmr_encR);
    mbe_post_apply_forced_clear_gate(state, &state->dmr_encR);
    mbe_post_apply_reverse_mute(opts, &state->dmr_encR, &opts->dmr_mute_encR);

    state->debug_audio_errorsR += state->errs2R;
    if ((state->dmr_encR == 0 || opts->dmr_mute_encR == 0) && opts->floating_point == 0) {
        processAudioR(opts, state);
    }
    DSD_MEMCPY(state->f_r, state->audio_out_temp_bufR, sizeof(state->f_r));
}

static void
mbe_post_other_process_audio(const dsd_opts* opts, dsd_state* state, int is_p25p2) {
    state->debug_audio_errors += state->errs2;
    if ((opts->audio_out != 1 && !(opts->wav_out_f != NULL && opts->static_wav_file == 1))
        || opts->floating_point != 0) {
        return;
    }
    if (is_p25p2 && state->currentslot == 1) {
        processAudioR(opts, state);
    } else {
        processAudio(opts, state);
    }
}

static int
mbe_post_other_is_allowed(const dsd_opts* opts, const dsd_state* state, int is_p25p2) {
    if (!is_p25p2) {
        if (DSD_SYNC_IS_P25P1(state->synctype)) {
            return p25_crypto_audio_permitted(opts, state, 0);
        }
        return (opts->unmute_encrypted_p25 == 1 || state->dmr_encL == 0);
    }

    if (state->currentslot != 0 && state->currentslot != 1) {
        return 0;
    }
    return state->p25_p2_audio_allowed[state->currentslot] != 0;
}

static void
mbe_post_other_copy_float_buffer(dsd_state* state, int is_p25p2) {
    if (is_p25p2 && state->currentslot == 1) {
        DSD_MEMCPY(state->f_l, state->audio_out_temp_bufR, sizeof(state->audio_out_temp_bufR));
    } else {
        DSD_MEMCPY(state->f_l, state->audio_out_temp_buf, sizeof(state->audio_out_temp_buf)); //P25p1 FDMA 8k/1
    }
}

static void
mbe_post_other_audio(const dsd_opts* opts, dsd_state* state) {
    if (opts->dmr_mono != 0 || opts->dmr_stereo != 0) {
        return;
    }

    int is_p25p2 = DSD_SYNC_IS_P25P2(state->synctype);
    if (mbe_post_other_is_allowed(opts, state, is_p25p2)) {
        mbe_post_other_process_audio(opts, state, is_p25p2);
    }

    mbe_post_other_copy_float_buffer(state, is_p25p2);
}

static int
mbe_post_allow_mono_wav(const dsd_opts* opts, const dsd_state* state) {
    if (opts->static_wav_file != 0 || opts->wav_out_f == NULL || opts->dmr_stereo != 0) {
        return 0;
    }
    int allow_wav = 0;
    return (dsd_audio_record_gate_mono(opts, state, &allow_wav) == 0 && allow_wav) ? 1 : 0;
}

static int
mbe_post_allow_stereo_slot_wav(const dsd_opts* opts, const dsd_state* state, int slot) {
    if (opts->dmr_stereo_wav != 1 || opts->dmr_stereo != 1 || state->currentslot != slot) {
        return 0;
    }
    int allow_wav = 0;
    return (dsd_audio_record_gate_mono(opts, state, &allow_wav) == 0 && allow_wav) ? 1 : 0;
}

static void
mbe_post_wav_outputs(dsd_opts* opts, dsd_state* state) {
    if (mbe_post_allow_mono_wav(opts, state)) {
        writeSynthesizedVoice(opts, state);
    }
    if (mbe_post_allow_stereo_slot_wav(opts, state, 0)) {
        writeSynthesizedVoice(opts, state);
    }
    if (mbe_post_allow_stereo_slot_wav(opts, state, 1)) {
        writeSynthesizedVoiceR(opts, state);
    }
}

static void
mbe_post_audio_and_recording(dsd_opts* opts, dsd_state* state, const mbe_frame_ctx_t* frame_ctx) {
    mbe_post_left_audio(opts, state, frame_ctx);
    mbe_post_right_audio(opts, state, frame_ctx);
    mbe_post_other_audio(opts, state);
    mbe_post_wav_outputs(opts, state);

    if (opts->audio_out_type == 9) {
        opts->audio_out = 0;
    }
}

void
playMbeFiles(dsd_opts* opts, dsd_state* state, int argc, char** argv) {

    char imbe_d[88];
    char ambe_d[49];
    char file_err_str[260];

    // Playback mode: keep static WAV writing functional even when audio output is disabled (-o null).
    const int want_static_wav = (opts->wav_out_f != NULL && opts->static_wav_file == 1) ? 1 : 0;

    for (int i = state->optind; i < argc; i++) {
        DSD_SNPRINTF(opts->mbe_in_file, sizeof(opts->mbe_in_file), "%s", argv[i]);
        openMbeInFile(opts, state);
        if (opts->mbe_in_f == NULL) {
            continue;
        }
        mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
        DSD_FPRINTF(stderr, "\n playing %s\n", opts->mbe_in_file);
        while (opts->mbe_in_f != NULL && feof(opts->mbe_in_f) == 0) {
            if (state->mbe_file_type == 0) {
                if (play_imbe_file_frame(opts, state, imbe_d, file_err_str, want_static_wav) != 0) {
                    break;
                }
            } else if (state->mbe_file_type == 3) {
                read_sdrtrunk_json_format(opts, state);
            } else if (state->mbe_file_type > 0) {
                if (play_ambe_file_frame(opts, state, ambe_d, file_err_str, want_static_wav) != 0) {
                    break;
                }
            }
            if (exitflag == 1) {
                cleanupAndExit(opts, state);
                break;
            }
        }
        if (opts->mbe_in_f != NULL) {
            fclose(opts->mbe_in_f); //close file after playing it
            opts->mbe_in_f = NULL;
        }
        if (exitflag == 1) {
            return;
        }
    }
}

static void
processMbeFrameInternal(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24],
                        char imbe7100_fr[7][24], dsd_vocoder_soft_bit imbe_soft_fr[8][23],
                        dsd_vocoder_soft_bit ambe_soft_fr[4][24], dsd_vocoder_soft_bit imbe7100_soft_fr[7][24]) {
    mbe_frame_ctx_t frame_ctx;

    mbe_prepare_frame_state(opts, state, &frame_ctx, imbe7100_soft_fr);

    if (DSD_SYNC_IS_P25P1(state->synctype)) {
        mbe_process_p25p1(opts, state, imbe_fr, imbe_soft_fr, &frame_ctx);
    } else if (DSD_SYNC_IS_PROVOICE(state->synctype)) {
        mbe_process_provoice(opts, state, imbe7100_fr, &frame_ctx);
    } else if ((state->synctype == DSD_SYNC_DSTAR_VOICE_POS) || (state->synctype == DSD_SYNC_DSTAR_VOICE_NEG)) {
        mbe_process_dstar(opts, state, ambe_fr, &frame_ctx);
    } else if (DSD_SYNC_IS_NXDN(state->synctype)) {
        mbe_process_nxdn(opts, state, ambe_fr, ambe_soft_fr, &frame_ctx);
    } else {
        mbe_process_slot_traffic(opts, state, ambe_fr, ambe_soft_fr, &frame_ctx);
    }

    mbe_post_audio_and_recording(opts, state, &frame_ctx);
}

void
processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]) {
    processMbeFrameInternal(opts, state, imbe_fr, ambe_fr, imbe7100_fr, NULL, NULL, NULL);
}

void
processMbeFrameSoft(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit imbe_fr[8][23],
                    dsd_vocoder_soft_bit ambe_fr[4][24], dsd_vocoder_soft_bit imbe7100_fr[7][24]) {
    processMbeFrameInternal(opts, state, NULL, NULL, NULL, imbe_fr, ambe_fr, imbe7100_fr);
}
