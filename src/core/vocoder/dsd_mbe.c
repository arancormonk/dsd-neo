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
#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/key_presence.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/des.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/crypto/rc4.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/shutdown.h>
#include <mbelib-neo/mbelib.h>
#include <stdint.h>
#include <stdio.h>
#include "../mbe_result_context.h"

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
    if (state->p25_p2_voice_err_hist_count[slot] < len) {
        state->p25_p2_voice_err_hist_count[slot]++;
    }

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

/* Unlike the P25 and AMBE 2450 copies, the ProVoice and D-STAR copies pass bits through unchanged: a cell that is not
 * 0 or 1 then fails mbelib's validation exactly as it does on the hard decode, instead of being read as a 1. */
static void
copy_imbe7100_soft_frame(dsd_vocoder_soft_bit src[7][24], mbe_soft_bit dst[7][24]) {
    for (int row = 0; row < 7; row++) {
        for (int bit = 0; bit < 24; bit++) {
            dst[row][bit].bit = src[row][bit].bit;
            dst[row][bit].reliability = src[row][bit].reliability;
        }
    }
}

static void
copy_ambe2400_soft_frame(dsd_vocoder_soft_bit src[4][24], mbe_soft_bit dst[4][24]) {
    for (int row = 0; row < 4; row++) {
        for (int bit = 0; bit < 24; bit++) {
            dst[row][bit].bit = src[row][bit].bit;
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

static void
clear_mbe_status(int* errs, int* errs2, char* err_str, size_t err_str_size) {
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

static void
store_mbe_result(int* errs, int* errs2, char* err_str, size_t err_str_size, const mbe_process_result* result) {
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

static int
store_decode_result(int ret, int* errs, int* errs2, const mbe_process_result* result) {
    if (ret < 0) {
        clear_mbe_status(errs, errs2, NULL, 0);
        return ret;
    }

    store_mbe_result(errs, errs2, NULL, 0, result);
    return ret;
}

static int
store_process_result(int ret, float* aout_buf, int* errs, int* errs2, char* err_str, size_t err_str_size,
                     const mbe_process_result* result) {
    if (ret < 0) {
        if (aout_buf) {
            mbe_synthesizeSilencef(aout_buf);
        }
        clear_mbe_status(errs, errs2, err_str, err_str_size);
        return ret;
    }

    dsd_mbe_float_to_pcm16_scale(aout_buf);
    store_mbe_result(errs, errs2, err_str, err_str_size, result);
    return ret;
}

static int
decode_imbe7200_frame(dsd_state* state, char imbe_fr[8][23], dsd_vocoder_soft_bit imbe_soft_fr[8][23], char imbe_d[88],
                      mbe_process_result* result) {
    if (imbe_soft_fr) {
        mbe_soft_bit soft_fr[8][23];

        copy_imbe7200_soft_frame(imbe_soft_fr, soft_fr);
        int ret = mbe_decodeImbe7200x4400SoftFrame((const mbe_soft_bit(*)[23])soft_fr, imbe_d, result);
        if (ret < 0) {
            clear_mbe_status(&state->errs, &state->errs2, NULL, 0);
            return 0;
        }
        store_mbe_result(&state->errs, &state->errs2, NULL, 0, result);
        return 1;
    }

    int ret = mbe_decodeImbe7200x4400Frame((const char (*)[23])imbe_fr, imbe_d, result);
    return store_decode_result(ret, &state->errs, &state->errs2, result) >= 0;
}

/* Soft when the caller brought reliabilities (the live ProVoice reader), hard otherwise. */
static int
decode_imbe7100_frame(dsd_state* state, char imbe7100_fr[7][24], dsd_vocoder_soft_bit imbe7100_soft_fr[7][24],
                      char imbe_d[88], mbe_process_result* result) {
    int ret;
    if (imbe7100_soft_fr) {
        mbe_soft_bit soft_fr[7][24];

        copy_imbe7100_soft_frame(imbe7100_soft_fr, soft_fr);
        ret = mbe_decodeImbe7100x4400SoftFrame((const mbe_soft_bit(*)[24])soft_fr, imbe_d, result);
    } else {
        ret = mbe_decodeImbe7100x4400Frame((const char (*)[24])imbe7100_fr, imbe_d, result);
    }
    return store_decode_result(ret, &state->errs, &state->errs2, result) >= 0;
}

/* Soft when the caller brought reliabilities (the live D-STAR reader), hard otherwise. */
static int
decode_ambe2400_frame(dsd_state* state, char ambe_fr[4][24], dsd_vocoder_soft_bit ambe_soft_fr[4][24], char ambe_d[49],
                      mbe_process_result* result) {
    int ret;
    if (ambe_soft_fr) {
        mbe_soft_bit soft_fr[4][24];

        copy_ambe2400_soft_frame(ambe_soft_fr, soft_fr);
        ret = mbe_decodeAmbe3600x2400SoftFrame((const mbe_soft_bit(*)[24])soft_fr, ambe_d, result);
    } else {
        ret = mbe_decodeAmbe3600x2400Frame((const char (*)[24])ambe_fr, ambe_d, result);
    }
    return store_decode_result(ret, &state->errs, &state->errs2, result) >= 0;
}

static int
decode_ambe2450_frame(int* errs, int* errs2, char ambe_fr[4][24], dsd_vocoder_soft_bit ambe_soft_fr[4][24],
                      char ambe_d[49], mbe_process_result* result) {
    if (ambe_soft_fr) {
        mbe_soft_bit soft_fr[4][24];

        copy_ambe_soft_frame(ambe_soft_fr, soft_fr);
        int ret = mbe_decodeAmbe3600x2450SoftFrame((const mbe_soft_bit(*)[24])soft_fr, ambe_d, result);
        if (ret < 0) {
            clear_mbe_status(errs, errs2, NULL, 0);
            return 0;
        }
        store_mbe_result(errs, errs2, NULL, 0, result);
        return 1;
    }

    int ret = mbe_decodeAmbe3600x2450Frame((const char (*)[24])ambe_fr, ambe_d, result);
    return store_decode_result(ret, errs, errs2, result) >= 0;
}

void
dsd_mbe_log_imbe_soft_frame(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit imbe_fr[8][23]) {
    if (!state || !imbe_fr || !dsd_frame_detail_enabled(opts)) {
        return;
    }

    char imbe_d[88] = {0};
    mbe_process_result result;
    if (decode_imbe7200_frame(state, NULL, imbe_fr, imbe_d, &result)) {
        PrintIMBEData(opts, state, imbe_d);
    }
}

void
dsd_mbe_log_ambe_soft_frame(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit ambe_fr[4][24]) {
    if (!state || !ambe_fr || !dsd_frame_detail_enabled(opts)) {
        return;
    }

    int* errs = state->currentslot == 1 ? &state->errsR : &state->errs;
    int* errs2 = state->currentslot == 1 ? &state->errs2R : &state->errs2;
    char ambe_d[49] = {0};
    mbe_process_result result;
    if (decode_ambe2450_frame(errs, errs2, NULL, ambe_fr, ambe_d, &result)) {
        PrintAMBEData(opts, state, ambe_d);
    }
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
    if (state->p25_p1_voice_err_hist_count < len) {
        state->p25_p1_voice_err_hist_count++;
    }
}

// One frame of -r playback, through the voice chain and the shared mono mixer in either output format.
static void
play_mbe_output_frame(dsd_opts* opts, dsd_state* state, int want_static_wav) {
    dsd_voice_chain_run(opts, state, 0, state->audio_out_temp_buf);
    if (opts->audio_out == 1 || want_static_wav) {
        dsd_voice_stage_short_left(opts, state);
    }
    if (opts->wav_out_f != NULL && opts->dmr_stereo_wav == 1) {
        writeSynthesizedVoice(opts, state);
    }
    if (opts->audio_out == 1 || want_static_wav) {
        playSynthesizedVoiceMS(opts, state);
    }
}

static void
init_mbe_file_result(mbe_process_result* result, int stored_errors) {
    mbe_initProcessResult(result);
    result->total_errors = stored_errors < 0 ? 0 : stored_errors;
    result->protected_errors = result->total_errors;
}

static int
play_imbe_file_frame(dsd_opts* opts, dsd_state* state, char imbe_d[88], char file_err_str[260], int want_static_wav) {
    if (readImbe4400Data(opts, state, imbe_d) != 0) {
        return -1;
    }
    file_err_str[0] = '\0';
    mbe_process_result result;
    init_mbe_file_result(&result, state->errs2);
    int ret = mbe_processImbe4400Dataf(state->audio_out_temp_buf, &result, imbe_d, state->cur_mp, state->prev_mp,
                                       state->prev_mp_enhanced);
    (void)store_process_result(ret, state->audio_out_temp_buf, &state->errs, &state->errs2, file_err_str,
                               sizeof(state->err_str), &result);
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
    mbe_process_result result;
    init_mbe_file_result(&result, state->errs2);
    int ret = MBE_STATUS_INVALID_ARGUMENT;
    if (state->mbe_file_type == 1) {
        file_err_str[0] = '\0';
        ret = mbe_processAmbe2450Dataf(state->audio_out_temp_buf, &result, ambe_d, state->cur_mp, state->prev_mp,
                                       state->prev_mp_enhanced);
    } else if (state->mbe_file_type == 2) {
        file_err_str[0] = '\0';
        ret = mbe_processAmbe2400Dataf(state->audio_out_temp_buf, &result, ambe_d, state->cur_mp, state->prev_mp,
                                       state->prev_mp_enhanced);
    }
    (void)store_process_result(ret, state->audio_out_temp_buf, &state->errs, &state->errs2, file_err_str,
                               sizeof(state->err_str), &result);
    DSD_STRNCPY(state->err_str, file_err_str, sizeof(state->err_str) - 1);
    state->err_str[sizeof(state->err_str) - 1] = '\0';
}

static int
play_ambe_file_frame(dsd_opts* opts, dsd_state* state, char ambe_d[49], char file_err_str[260], int want_static_wav) {
    if (readAmbe2450Data(opts, state, ambe_d) != 0) {
        return -1;
    }
    // DMR basic privacy is an AMBE+2 transform: D-STAR's AMBE 2400 (.dmb) frames never carry it (issue #656).
    if (state->mbe_file_type == 1) {
        decrypt_ambe_with_pr_key(state, ambe_d);
    }
    decode_ambe_file_frame(state, ambe_d, file_err_str);
    play_mbe_output_frame(opts, state, want_static_wav);
    return 0;
}

/* Which save point the frame reached: the frame is captured by mbe_capture_frame() once the post stage has classified
   its slot verdict, so the capture follows this frame's verdict, not the previous frame's (issue #661). */
typedef enum {
    MBE_CAPTURE_NONE = 0,
    MBE_CAPTURE_P25P1,
    MBE_CAPTURE_PROVOICE,
    MBE_CAPTURE_DSTAR,
    MBE_CAPTURE_X2,
    MBE_CAPTURE_NXDN,
    MBE_CAPTURE_SLOT_LEFT,
    MBE_CAPTURE_SLOT_RIGHT,
} mbe_capture_point;

typedef struct {
    char imbe_d[88];
    char ambe_d[49];
    int vertex_ks_applied_l;
    int vertex_ks_applied_r;
    /* A keyed transform the slot path ran on this frame beyond what dsd_dmr_voice_slot_can_decrypt() covers: Basic or
       Hytera Privacy (forced with -4 too), or the generic -S keystream. */
    int slot_keyed[2];
    /* The generic -S keystream ran on the slot: it clears the slot's service options and ALG ID on every frame, so
       the next frame of the burst arrives with no encryption evidence left to judge. */
    int straight_ks[2];
    /* The slot's service options and ALG ID as the frame arrived, before a transform (-S, a Vertex keystream) clears
       them: the encryption evidence the verdict judges (issue #655). */
    int frame_so;
    int frame_algid;
    mbe_capture_point capture;
    /* The protocol that owns the frame; it, not the codec path, decides which crypto transforms run (issue #656). */
    dsd_voice_protocol protocol;
    /* The frame needed keystream past what was generated for it: it is muted, not decrypted with a wrong keystream. */
    int crypto_muted;
    /* The buffer the decode synthesized this frame into (NULL: none) and the slot whose voice chain runs on it. */
    float* synth;
    int synth_slot;
} mbe_frame_ctx_t;

/* Key selection by the signalled ALG ID / KEY ID belongs to the protocols that signal them to the vocoder. NXDN voice
   loads its keys from its own VCALL (nxdn_vcall_load_key()); YSF, dPMR, D-STAR, X2-TDMA and ProVoice signal none, so
   for them payload_algid/payload_keyid are another call's and must not overwrite R (the NXDN and dPMR scrambler key)
   or the AES key. */
static int
mbe_protocol_selects_keys(dsd_voice_protocol protocol) {
    return protocol == DSD_VOICE_PROTOCOL_DMR || protocol == DSD_VOICE_PROTOCOL_P25P1
           || protocol == DSD_VOICE_PROTOCOL_P25P2;
}

static void
mbe_activate_keyloader_slot(dsd_state* state, const dsd_call_snapshot* call) {
    const int slot = (state->currentslot == 1) ? 1 : 0;
    const int algid = (slot == 0) ? state->payload_algid : state->payload_algidR;

    // Unchanged gate: no ALG ID means the BP/HBP TG autoload owns the slot, and 0x80 stays with
    // the scrambler path.
    if (state->keyloader == 1 && algid != 0 && algid != 0x80) {
        const int signaled = (slot == 0) ? state->payload_keyid : state->payload_keyidR;
        // A --dmr-tg-key-csv row for the slot's active talkgroup selects its key id in place of
        // the signaled one. The resolver hands back `signaled` whenever the map does not apply --
        // including for every non-DMR protocol, whose snapshots are never mappable, and for a
        // 16-bit P25 KID, which it refuses by width -- so there is one activation here rather
        // than two paths to keep in sync.
        const int kid = keyring_dmr_slot_kid_for_call(state, slot, call, dsd_dmr_alg_key_need(algid), signaled);
        keyring_activate_slot_with_kid(state, slot, kid);
    }
}

static void
mbe_prepare_frame_state(dsd_state* state, mbe_frame_ctx_t* frame_ctx, const dsd_call_snapshot* call,
                        dsd_voice_protocol protocol) {
    frame_ctx->vertex_ks_applied_l = 0;
    frame_ctx->vertex_ks_applied_r = 0;
    frame_ctx->slot_keyed[0] = 0;
    frame_ctx->slot_keyed[1] = 0;
    frame_ctx->straight_ks[0] = 0;
    frame_ctx->straight_ks[1] = 0;
    frame_ctx->capture = MBE_CAPTURE_NONE;
    frame_ctx->protocol = protocol;
    frame_ctx->crypto_muted = 0;
    frame_ctx->synth = NULL;
    frame_ctx->synth_slot = 0;

    if (protocol == DSD_VOICE_PROTOCOL_DMR) {
        (void)dsd_dmr_apply_forced_algid(state);
    }
    const int slot = state->currentslot == 1 ? 1 : 0;
    frame_ctx->frame_so = slot == 0 ? state->dmr_so : state->dmr_soR;
    frame_ctx->frame_algid = slot == 0 ? state->payload_algid : state->payload_algidR;
    if (mbe_protocol_selects_keys(protocol)) {
        mbe_activate_keyloader_slot(state, call);
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

/* P25 Phase 1 multicrypt keystream: an LDU1/LDU2 superframe reads 18 IMBE frames of 11 octets and skips 2 octets for
   each LSD, after the start offset (19 for DES/TDEA, 11 for DES-XL, 27 for AES), so it reads up to octet 228 for
   AES. AES generates 15 blocks (240 octets) to cover it, as OP25 does for FDMA; 14 left the last 40 bits of every
   superframe's 18th frame encrypted (issue #656). */
#define MBE_P25P1_DES_BLOCKS   28
#define MBE_P25P1_DESXL_OCTETS 213 /* des_xl_keystream_output() emits 1704 bits */
#define MBE_P25P1_AES_BLOCKS   15

/* Octets of keystream mbe_init_p25p1_multicrypt_keystream() generates for an ALG ID, 0 for none. */
static int
mbe_p25p1_keystream_octets(int algid) {
    switch (algid) {
        case 0x81:
        case 0x83: return MBE_P25P1_DES_BLOCKS * 8;
        case 0x9F: return MBE_P25P1_DESXL_OCTETS;
        case 0x84:
        case 0x89: return MBE_P25P1_AES_BLOCKS * 16;
        default: return 0;
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
        des_ofb_keystream_output(state->payload_miP, state->R, state->ks_octetL, MBE_P25P1_DES_BLOCKS);
    }
    if (state->payload_algid == 0x83) { //3DES, or TDEA
        tdea_tofb_keystream_output(state->payload_miP, aes_key, state->ks_octetL, MBE_P25P1_DES_BLOCKS);
    }
    if (state->payload_algid == 0x9F) { //DES-XL
        des_xl_keystream_output(state->payload_miP, state->R, state->ks_octetL,
                                state->xl_is_hdu); //xl_is_hdu determines lfsr run values
    }
    if (state->payload_algid == 0x84) { //AES256
        aes_ofb_keystream_output(state->aes_iv, aes_key, state->ks_octetL, DSD_AES_KEY_256, MBE_P25P1_AES_BLOCKS);
    }
    if (state->payload_algid == 0x89) { //AES128
        aes_ofb_keystream_output(state->aes_iv, aes_key, state->ks_octetL, DSD_AES_KEY_128, MBE_P25P1_AES_BLOCKS);
    }
}

/* The current frame's 11 keystream octets, advancing the counter past them, or NULL with the counter unchanged when
   they would lie outside the generated keystream. */
static const uint8_t*
mbe_p25p1_take_keystream(dsd_state* state) {
    const int len = mbe_p25p1_keystream_octets(state->payload_algid);
    const int pos = state->octet_counter;
    if (len <= 0 || (size_t)len > sizeof(state->ks_octetL) || pos < 0 || pos > len - 11) {
        return NULL;
    }
    state->octet_counter = pos + 11;
    return &state->ks_octetL[pos];
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

/* Returns 1 when the frame was decrypted, -1 when its keystream lies outside the generated keystream (the frame is
   left as it is). */
static int
mbe_apply_p25p1_multicrypt(dsd_state* state, char imbe_d[88]) {
    uint8_t cipher[11];
    uint8_t plain[11];
    DSD_MEMSET(cipher, 0, sizeof(cipher));
    DSD_MEMSET(plain, 0, sizeof(plain));

    uint8_t aes_key[32];
    DSD_MEMSET(aes_key, 0, sizeof(aes_key));

    mbe_load_aes_key_slot0(state, aes_key);
    mbe_init_p25p1_multicrypt_keystream(state, aes_key);
    DSD_SECURE_ZERO(aes_key, sizeof(aes_key));

    const uint8_t* ks = mbe_p25p1_take_keystream(state);
    if (ks == NULL) {
        return -1;
    }
    mbe_pack_imbe_bits(imbe_d, cipher);
    for (int i = 0; i < 11; i++) {
        plain[i] = cipher[i] ^ ks[i];
    }
    mbe_unpack_imbe_bits(imbe_d, plain);
    return 1;
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
        case 0x9F: return dsd_key_scalar_present(state, 0);
        case 0x83:
        case 0x84:
        case 0x89: return state->aes_key_loaded[0] == 1;
        default: return 0;
    }
}

/* One IMBE frame position of the P25p1 superframe passes without its keystream being applied: consume what the frame
   would have (generating the keystream first when this frame would have generated it), so the frames after it
   decrypt with their own keystream (issue #656). */
static void
mbe_p25p1_advance_keystream(dsd_state* state) {
    if (mbe_p25p1_multicrypt_enabled(state)) {
        uint8_t aes_key[32];
        DSD_MEMSET(aes_key, 0, sizeof(aes_key));
        mbe_load_aes_key_slot0(state, aes_key);
        mbe_init_p25p1_multicrypt_keystream(state, aes_key);
        DSD_SECURE_ZERO(aes_key, sizeof(aes_key));
        (void)mbe_p25p1_take_keystream(state);
    }
    if (state->payload_algid == 0xAA && dsd_key_scalar_present(state, 0)) {
        state->dropL += 11;
    }
    state->p25vc++;
}

static void
mbe_mute_exhausted_frame(dsd_opts* opts, dsd_state* state, int slot, mbe_frame_ctx_t* frame_ctx) {
    float* audio = (slot == 1) ? state->audio_out_temp_bufR : state->audio_out_temp_buf;
    int* errs = (slot == 1) ? &state->errsR : &state->errs;
    int* errs2 = (slot == 1) ? &state->errs2R : &state->errs2;
    char* err_str = (slot == 1) ? state->err_strR : state->err_str;
    store_process_result(MBE_STATUS_INVALID_BITS, audio, errs, errs2, err_str, sizeof(state->err_str), NULL);
    frame_ctx->crypto_muted = 1;
    frame_ctx->capture = MBE_CAPTURE_NONE;
    dsd_frame_logf(opts, "FRAME EVENT slot=%d type=KEYSTREAM_EXHAUSTED action=mute", slot + 1);
}

static int
mbe_p25p1_is_tail_erasure(const dsd_state* state, const char imbe_d[88], int corrections) {
    if (state->p25_crypto_state[0] != DSD_P25_CRYPTO_CLEAR || corrections < 10) {
        return 0;
    }

    uint8_t prefix = 0;
    int set_bits = 0;
    for (int i = 0; i < 88; i++) {
        const uint8_t bit = (uint8_t)(imbe_d[i] & 1);
        if (i < 8) {
            prefix = (uint8_t)((prefix << 1) | bit);
        }
        set_bits += bit;
    }
    return prefix == 0xFCU && set_bits <= 24;
}

static void
mbe_p25p1_record_accepted_frame(dsd_state* state, int corrections, unsigned process_flags) {
    state->p25_p1_accepted_frames++;
    if (corrections > 0) {
        state->p25_p1_accepted_corrections += (uint64_t)corrections;
    }

    if (corrections == 0) {
        state->p25_p1_clean_frames++;
    } else if ((process_flags & (MBE_PROCESS_FLAG_REPEAT | MBE_PROCESS_FLAG_MUTE)) != 0U) {
        state->p25_p1_concealed_frames++;
    } else {
        state->p25_p1_corrected_frames++;
    }
}

// Recording (MBE capture and the policy-only WAVs) answers to the talkgroup
// policy of the call on the slot the protocol publishes. Each save point keeps its
// own crypto condition; this adds only the policy.
static int
mbe_record_policy_allows(const dsd_opts* opts, const dsd_state* state, int slot) {
    int allow = 0;
    return (dsd_audio_record_policy_gate_slot(opts, state, slot, &allow) == 0 && allow) ? 1 : 0;
}

static void
mbe_process_p25p1(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], dsd_vocoder_soft_bit imbe_soft_fr[8][23],
                  mbe_frame_ctx_t* frame_ctx) {
    // YSF full rate borrows this decoder: P25 keys, keystreams and the keystream position stay P25's.
    const int p25_owned = frame_ctx->protocol == DSD_VOICE_PROTOCOL_P25P1;
    mbe_process_result imbe_result;
    int have_imbe_result = decode_imbe7200_frame(state, imbe_fr, imbe_soft_fr, frame_ctx->imbe_d, &imbe_result);
    if (!have_imbe_result) {
        store_process_result(MBE_STATUS_INVALID_BITS, state->audio_out_temp_buf, &state->errs, &state->errs2,
                             state->err_str, sizeof(state->err_str), NULL);
        if (p25_owned) {
            mbe_p25p1_advance_keystream(state);
        }
        return;
    }

    const int decoded_corrections = imbe_result.total_errors;
    if (mbe_p25p1_is_tail_erasure(state, frame_ctx->imbe_d, decoded_corrections)) {
        if (dsd_frame_detail_enabled(opts)) {
            PrintIMBEData(opts, state, frame_ctx->imbe_d);
        }
        dsd_frame_logf(opts, "FRAME EVENT slot=1 type=P25P1_TAIL_ERASURE action=mute excluded_corrections=%d",
                       decoded_corrections);
        mbe_synthesizeSilencef(state->audio_out_temp_buf);
        state->p25_p1_suppressed_tail_frames++;
        state->p25_p1_excluded_tail_corrections += (uint64_t)decoded_corrections;
        clear_mbe_status(&state->errs, &state->errs2, state->err_str, sizeof(state->err_str));
        if (p25_owned) {
            mbe_p25p1_advance_keystream(state);
        }
        return;
    }

    char decoded_imbe_d[88];
    DSD_MEMCPY(decoded_imbe_d, frame_ctx->imbe_d, sizeof(decoded_imbe_d));

    //P25p1 Multi Crypt Handler (DES1, DES3, DES-XL and AES)
    if (p25_owned && mbe_p25p1_multicrypt_enabled(state)) {
        if (mbe_apply_p25p1_multicrypt(state, frame_ctx->imbe_d) < 0) {
            mbe_mute_exhausted_frame(opts, state, 0, frame_ctx);
            state->p25vc++;
            return;
        }
    }

    //P25p1 RC4 Handling
    if (p25_owned && state->payload_algid == 0xAA && dsd_key_scalar_present(state, 0)) {
        mbe_apply_p25p1_rc4(state, frame_ctx->imbe_d);
    }

    (void)dsd_mbe_strip_imbe_context_if_changed(decoded_imbe_d, frame_ctx->imbe_d, &imbe_result);

    int process_ret = mbe_processImbe4400Dataf(state->audio_out_temp_buf, &imbe_result, frame_ctx->imbe_d,
                                               state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
    (void)store_process_result(process_ret, state->audio_out_temp_buf, &state->errs, &state->errs2, state->err_str,
                               sizeof(state->err_str), &imbe_result);
    mbe_p25p1_record_accepted_frame(state, decoded_corrections, imbe_result.flags);
    update_p25_p1_voice_err_hist(state);

    if (dsd_frame_detail_enabled(opts)) {
        PrintIMBEData(opts, state, frame_ctx->imbe_d);
    }

    //increment vc counter by one.
    if (p25_owned) {
        state->p25vc++;
    }
    frame_ctx->capture = MBE_CAPTURE_P25P1;
}

static void
mbe_process_provoice(dsd_opts* opts, dsd_state* state, char imbe7100_fr[7][24],
                     dsd_vocoder_soft_bit imbe7100_soft_fr[7][24], mbe_frame_ctx_t* frame_ctx) {
    /* The decode's result goes to the 4400 data API unchanged: its C0/C4 context, the soft-input flag, and
     * MBE_PROCESS_FLAG_PROVOICE, without which a muted ProVoice frame would get the P25 noise level instead of
     * ProVoice comfort noise. */
    mbe_process_result imbe_result;
    if (!decode_imbe7100_frame(state, imbe7100_fr, imbe7100_soft_fr, frame_ctx->imbe_d, &imbe_result)) {
        store_process_result(MBE_STATUS_INVALID_BITS, state->audio_out_temp_buf, &state->errs, &state->errs2,
                             state->err_str, sizeof(state->err_str), NULL);
        return;
    }

    if (dsd_frame_detail_enabled(opts)) {
        PrintIMBEData(opts, state, frame_ctx->imbe_d);
        if (opts->payload == 1) {
            DSD_FPRINTF(stderr, " 7100");
        }
    }

    int process_ret = mbe_processImbe4400Dataf(state->audio_out_temp_buf, &imbe_result, frame_ctx->imbe_d,
                                               state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
    (void)store_process_result(process_ret, state->audio_out_temp_buf, &state->errs, &state->errs2, state->err_str,
                               sizeof(state->err_str), &imbe_result);
    if (DSD_SYNC_IS_P25P1(state->synctype)) {
        update_p25_p1_voice_err_hist(state);
    }
    frame_ctx->capture = MBE_CAPTURE_PROVOICE;
}

static void
mbe_process_dstar(dsd_opts* opts, dsd_state* state, char ambe_fr[4][24], dsd_vocoder_soft_bit ambe_soft_fr[4][24],
                  mbe_frame_ctx_t* frame_ctx) {
    /* The decode's result goes to the 2400 data API unchanged, soft-input flag included: its C0 error count is what
     * decides whether a tone frame is played or replaced with comfort noise. */
    mbe_process_result result;
    if (!decode_ambe2400_frame(state, ambe_fr, ambe_soft_fr, frame_ctx->ambe_d, &result)) {
        store_process_result(MBE_STATUS_INVALID_BITS, state->audio_out_temp_buf, &state->errs, &state->errs2,
                             state->err_str, sizeof(state->err_str), NULL);
        return;
    }

    int ret = mbe_processAmbe2400Dataf(state->audio_out_temp_buf, &result, frame_ctx->ambe_d, state->cur_mp,
                                       state->prev_mp, state->prev_mp_enhanced);
    (void)store_process_result(ret, state->audio_out_temp_buf, &state->errs, &state->errs2, state->err_str,
                               sizeof(state->err_str), &result);
    if (dsd_frame_detail_enabled(opts)) {
        PrintAMBEData(opts, state, frame_ctx->ambe_d);
    }
    frame_ctx->capture = MBE_CAPTURE_DSTAR;
}

static void
mbe_save_x2_frame(dsd_opts* opts, dsd_state* state, const char* ambe_d) {
    /* X2 records both timeslots in one interleaved .amb stream. */
    int saved_errs2 = state->errs2;

    if (state->currentslot == 1) {
        state->errs2 = state->errs2R;
    }
    dsd_mbe_out_write(opts, state, 0, DSD_MBE_OUT_FORMAT_AMBE, ambe_d);
    state->errs2 = saved_errs2;
}

static void
mbe_process_x2(dsd_opts* opts, dsd_state* state, char ambe_fr[4][24], dsd_vocoder_soft_bit ambe_soft_fr[4][24],
               mbe_frame_ctx_t* frame_ctx) {
    int* errs = &state->errs;
    int* errs2 = &state->errs2;
    char* err_str = state->err_str;
    mbe_parms* cur_mp = state->cur_mp;
    mbe_parms* prev_mp = state->prev_mp;
    mbe_parms* prev_mp_enhanced = state->prev_mp_enhanced;

    if (state->currentslot == 1) {
        errs = &state->errsR;
        errs2 = &state->errs2R;
        err_str = state->err_strR;
        cur_mp = state->cur_mp2;
        prev_mp = state->prev_mp2;
        prev_mp_enhanced = state->prev_mp_enhanced2;
    }

    mbe_process_result ambe_result;
    int have_ambe_result = decode_ambe2450_frame(errs, errs2, ambe_fr, ambe_soft_fr, frame_ctx->ambe_d, &ambe_result);
    if (!have_ambe_result) {
        store_process_result(MBE_STATUS_INVALID_BITS, state->audio_out_temp_buf, errs, errs2, err_str,
                             sizeof(state->err_str), NULL);
        return;
    }

    /* X2 is a mono output mode even though each timeslot keeps independent decoder history. */
    int process_ret = mbe_processAmbe2450Dataf(state->audio_out_temp_buf, &ambe_result, frame_ctx->ambe_d, cur_mp,
                                               prev_mp, prev_mp_enhanced);
    (void)store_process_result(process_ret, state->audio_out_temp_buf, errs, errs2, err_str, sizeof(state->err_str),
                               &ambe_result);

    if (dsd_frame_detail_enabled(opts)) {
        PrintAMBEData(opts, state, frame_ctx->ambe_d);
    }
    frame_ctx->capture = MBE_CAPTURE_X2;
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

static void
mbe_init_nxdn_cipher23_keystream(dsd_state* state) {
    if (state->nxdn_cipher_type == 0x02 && state->nxdn_new_iv == 1 && state->nxdn_part_of_frame == 0) {
        DSD_MEMSET(state->ks_octetL, 0, sizeof(state->ks_octetL));
        DSD_MEMSET(state->ks_bitstreamL, 0, sizeof(state->ks_bitstreamL));
        des_ofb_keystream_output(state->payload_miN, state->R, state->ks_octetL, 26);
        state->bit_counterL = 0;
        dsd_unpack_bytes_to_bits(state->ks_octetL + 8, sizeof(state->ks_octetL) - 8U, state->ks_bitstreamL,
                                 sizeof(state->ks_bitstreamL), (size_t)26U * 8U);
        state->nxdn_new_iv = 0;
    }

    if (state->nxdn_cipher_type == 0x03 && state->nxdn_new_iv == 1 && state->nxdn_part_of_frame == 0) {
        DSD_MEMSET(state->ks_octetL, 0, sizeof(state->ks_octetL));
        DSD_MEMSET(state->ks_bitstreamL, 0, sizeof(state->ks_bitstreamL));
        aes_ofb_keystream_output(state->aes_iv, state->aes_key, state->ks_octetL, DSD_AES_KEY_256, 15);
        state->bit_counterL = 0;
        dsd_unpack_bytes_to_bits(state->ks_octetL + 16, sizeof(state->ks_octetL) - 16U, state->ks_bitstreamL,
                                 sizeof(state->ks_bitstreamL), (size_t)14U * 16U);
        state->nxdn_new_iv = 0;
    }
}

/* NXDN TS 1-D section 5.4.4.2: one DES/AES encryption session contains 32 EHR voice words of 49 bits. A missing
   replacement IV cannot extend that session by repeating its final word. */
#define MBE_NXDN_VOICE_KS_BITS ((size_t)32U * 49U)

static int
mbe_apply_nxdn_cipher23(dsd_state* state, char ambe_d[49]) {
    mbe_init_nxdn_cipher23_keystream(state);

    if (state->bit_counterL < 0 || (size_t)state->bit_counterL > MBE_NXDN_VOICE_KS_BITS - 49U) {
        return -1;
    }

    for (int i = 0; i < 49; i++) {
        ambe_d[i] ^= (char)(state->ks_bitstreamL[state->bit_counterL++] & 1U);
    }
    return 1;
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
    hash = dsd_crc_ccitt16_bits(hash_bits, 24U);
    return hash & 0xFFFF;
}

static int
mbe_slot_straight_ks_armed(const dsd_state* state) {
    return state->straight_ks == 1 && state->straight_mod > 0;
}

static int
mbe_slot_apply_straight_ks_left(dsd_state* state, char ambe_d[49]) {
    if (mbe_slot_straight_ks_armed(state)) {
        state->dmr_so = 0;
        state->payload_algid = 0;
        straight_mod_xor_apply_frame49(state, state->currentslot, ambe_d);
        return 1;
    }
    return 0;
}

static int
mbe_slot_apply_straight_ks_right(dsd_state* state, char ambe_d[49]) {
    if (mbe_slot_straight_ks_armed(state)) {
        state->dmr_soR = 0;
        state->payload_algidR = 0;
        straight_mod_xor_apply_frame49(state, state->currentslot, ambe_d);
        return 1;
    }
    return 0;
}

static int
mbe_post_dmr_mono_active(const dsd_opts* opts, const dsd_state* state) {
    return opts->dmr_mono == 1 && DSD_SYNC_IS_DMR(state->synctype);
}

static int
mbe_dmr_output_slot_enabled(const dsd_opts* opts, const dsd_state* state, int slot) {
    if (!mbe_post_dmr_mono_active(opts, state)) {
        return 1;
    }
    return state->dmr_mono_slot == slot;
}

static void
mbe_finalize_slot_left(dsd_opts* opts, dsd_state* state, char ambe_d[49], mbe_process_result* ambe_result) {
    int ret = mbe_processAmbe2450Dataf(state->audio_out_temp_buf, ambe_result, ambe_d, state->cur_mp, state->prev_mp,
                                       state->prev_mp_enhanced);
    (void)store_process_result(ret, state->audio_out_temp_buf, &state->errs, &state->errs2, state->err_str,
                               sizeof(state->err_str), ambe_result);
    p25p2_record_voice_err(state, state->errs2);
    if (dsd_frame_detail_enabled(opts)) {
        PrintAMBEData(opts, state, ambe_d);
    }
}

static void
mbe_finalize_slot_right(dsd_opts* opts, dsd_state* state, char ambe_d[49], mbe_process_result* ambe_result) {
    int ret = mbe_processAmbe2450Dataf(state->audio_out_temp_bufR, ambe_result, ambe_d, state->cur_mp2, state->prev_mp2,
                                       state->prev_mp_enhanced2);
    (void)store_process_result(ret, state->audio_out_temp_bufR, &state->errsR, &state->errs2R, state->err_strR,
                               sizeof(state->err_strR), ambe_result);
    p25p2_record_voice_err(state, state->errs2R);
    if (dsd_frame_detail_enabled(opts)) {
        PrintAMBEData(opts, state, ambe_d);
    }
}

static void
mbe_process_nxdn(dsd_opts* opts, dsd_state* state, char ambe_fr[4][24], dsd_vocoder_soft_bit ambe_soft_fr[4][24],
                 mbe_frame_ctx_t* frame_ctx) {
    mbe_process_result ambe_result;
    int have_ambe_result =
        decode_ambe2450_frame(&state->errs, &state->errs2, ambe_fr, ambe_soft_fr, frame_ctx->ambe_d, &ambe_result);
    if (!have_ambe_result) {
        store_process_result(MBE_STATUS_INVALID_BITS, state->audio_out_temp_buf, &state->errs, &state->errs2,
                             state->err_str, sizeof(state->err_str), NULL);
        return;
    }

    char decoded_ambe_d[49];
    DSD_MEMCPY(decoded_ambe_d, frame_ctx->ambe_d, sizeof(decoded_ambe_d));

    // YSF V/D Type 1 borrows this decoder and has no cipher: the scrambler and cipher 2/3 are NXDN's, and scrambled
    // dPMR halves', only.
    const int nxdn_crypto =
        frame_ctx->protocol == DSD_VOICE_PROTOCOL_NXDN || frame_ctx->protocol == DSD_VOICE_PROTOCOL_DPMR;
    if (nxdn_crypto
        && ((state->nxdn_cipher_type == 0x01 && dsd_key_scalar_present(state, 0))
            || (state->M == 1 && dsd_key_scalar_present(state, 0)))) {
        mbe_apply_nxdn_cipher1(state, frame_ctx->ambe_d);
    }

    //NXDN Generic Cipher 2 and Cipher 3 Keystream Application (to be tested)
    else if (nxdn_crypto
             && ((state->nxdn_cipher_type == 0x02 && dsd_key_scalar_present(state, 0))
                 || (state->nxdn_cipher_type == 0x03 && state->aes_key_loaded[0] == 1))) {
        if (mbe_apply_nxdn_cipher23(state, frame_ctx->ambe_d) < 0) {
            mbe_mute_exhausted_frame(opts, state, 0, frame_ctx);
            return;
        }
    }

    (void)dsd_mbe_strip_ambe_context_if_changed(decoded_ambe_d, frame_ctx->ambe_d, &ambe_result);

    int process_ret = mbe_processAmbe2450Dataf(state->audio_out_temp_buf, &ambe_result, frame_ctx->ambe_d,
                                               state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
    (void)store_process_result(process_ret, state->audio_out_temp_buf, &state->errs, &state->errs2, state->err_str,
                               sizeof(state->err_str), &ambe_result);
    p25p2_record_voice_err(state, state->errs2);

    if (dsd_frame_detail_enabled(opts)) {
        PrintAMBEData(opts, state, frame_ctx->ambe_d);
    }
    frame_ctx->capture = MBE_CAPTURE_NXDN;
}

static void
mbeslot_left_autoload_keys(dsd_state* state) {
    if (state->M == 0 && state->payload_algid == 0) {
        dsd_call_snapshot call;
        uint32_t target = dsd_call_state_get(state, 0U, &call) > 0 && call.ota_target_id <= UINT32_MAX
                              ? (uint32_t)call.ota_target_id
                              : 0U;
        uint32_t hash = mbe_hash_tg_for_key(target);
        if (state->rkey_array[hash] != 0) {
            state->K = state->rkey_array[hash] & 0xFF;                     //doesn't exceed 255
            state->K1 = state->H = state->rkey_array[hash] & 0xFFFFFFFFFF; //doesn't exceed 40-bit limit
            state->K2 = state->K3 = state->K4 = 0ULL;
            state->hytera_key_segments = 1U;
            (void)dsd_call_state_note_key_selection(state, 0, call.epoch, DSD_CALL_KEY_DESTINATION, call.kid,
                                                    (int)target, 1, 0);
        }
    }
}

static void
mbeslot_right_autoload_keys(dsd_state* state) {
    if (state->M == 0 && state->payload_algidR == 0) {
        dsd_call_snapshot call;
        uint32_t target = dsd_call_state_get(state, 1U, &call) > 0 && call.ota_target_id <= UINT32_MAX
                              ? (uint32_t)call.ota_target_id
                              : 0U;
        uint32_t hash = mbe_hash_tg_for_key(target);
        if (state->rkey_array[hash] != 0) {
            state->K = state->rkey_array[hash] & 0xFF;
            state->K1 = state->H = state->rkey_array[hash] & 0xFFFFFFFFFF;
            state->K2 = state->K3 = state->K4 = 0ULL;
            state->hytera_key_segments = 1U;
            (void)dsd_call_state_note_key_selection(state, 1, call.epoch, DSD_CALL_KEY_DESTINATION, call.kid,
                                                    (int)target, 1, 0);
        }
    }
}

static void
mbeslot_left_apply_basic_privacy(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if ((dsd_key_basic_present(state) && state->dmr_so & 0x40 && state->payload_keyid == 0 && state->dmr_fid == 0x10)
        || (dsd_key_basic_present(state) && state->M == 1)) {
        (void)dmr_basic_privacy_apply_frame49(state->K, frame_ctx->ambe_d);
        frame_ctx->slot_keyed[0] = 1;
    }

    if ((dsd_key_hytera_present(state) && state->dmr_so & 0x40 && state->payload_keyid == 0 && state->dmr_fid == 0x68)
        || (dsd_key_hytera_present(state) && state->M == 1)) {
        (void)hytera_bp_apply_frame49(state->K1, state->K2, state->K3, state->K4, &state->DMRvcL, frame_ctx->ambe_d);
        frame_ctx->slot_keyed[0] = 1;
    }
}

static void
mbeslot_right_apply_basic_privacy(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if ((dsd_key_basic_present(state) && state->dmr_soR & 0x40 && state->payload_keyidR == 0 && state->dmr_fidR == 0x10)
        || (dsd_key_basic_present(state) && state->M == 1)) {
        (void)dmr_basic_privacy_apply_frame49(state->K, frame_ctx->ambe_d);
        frame_ctx->slot_keyed[1] = 1;
    }

    if ((dsd_key_hytera_present(state) && state->dmr_soR & 0x40 && state->payload_keyidR == 0
         && state->dmr_fidR == 0x68)
        || (dsd_key_hytera_present(state) && state->M == 1)) {
        (void)hytera_bp_apply_frame49(state->K1, state->K2, state->K3, state->K4, &state->DMRvcR, frame_ctx->ambe_d);
        frame_ctx->slot_keyed[1] = 1;
    }
}

static void
mbeslot_left_apply_vertex_standard(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (frame_ctx->protocol == DSD_VOICE_PROTOCOL_DMR && state->payload_algid == 0x07 && state->straight_ks != 1) {
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
    if (frame_ctx->protocol == DSD_VOICE_PROTOCOL_DMR && state->payload_algidR == 0x07 && state->straight_ks != 1) {
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

/* The slot path's DES-OFB keystream: 19 blocks minus the discard block, expanded to bits. */
#define MBE_SLOT_DES_KS_BITS ((size_t)18U * 8U * 8U)

/* XOR one frame with the next 49 keystream bits and step past the frame's 56; -1 (frame and counter unchanged) when
   the bits lie outside the ks_bit_count generated ones (issue #656). */
static int
mbeslot_xor_keystream_frame(const uint8_t* ks_bits, size_t ks_bit_count, long int* bit_counter, char ambe_d[49]) {
    if (*bit_counter < 0) {
        *bit_counter = 0;
    }
    const size_t pos = (size_t)*bit_counter;
    if (pos > ks_bit_count || ks_bit_count - pos < 49U) {
        return -1;
    }
    for (size_t i = 0; i < 49U; i++) {
        ambe_d[i] ^= (char)(ks_bits[pos + i] & 1U);
    }
    *bit_counter = (long int)((ks_bit_count - pos < 56U) ? ks_bit_count : pos + 56U);
    return 1;
}

/* The ALG IDs the slot path may decrypt for the frame's owner: on DMR every one it decrypts, as before; on P25 Phase 2
   only P25's own (DES, AES, RC4); on clear dPMR none. */
static int
mbe_slot_algid_allowed(dsd_voice_protocol protocol, int algid) {
    if (protocol == DSD_VOICE_PROTOCOL_DMR) {
        return 1;
    }
    if (protocol == DSD_VOICE_PROTOCOL_P25P2) {
        return algid == 0x81 || algid == 0x84 || algid == 0x89 || algid == 0xAA;
    }
    return 0;
}

/* Returns 1 when the frame was decrypted, 0 when DES does not apply, -1 when its keystream is exhausted. */
static int
mbeslot_left_apply_des(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (!mbe_slot_algid_allowed(frame_ctx->protocol, state->payload_algid)
        || !((state->payload_algid == 0x22 && dsd_key_scalar_present(state, 0))
             || (state->payload_algid == 0x81 && dsd_key_scalar_present(state, 0)))) {
        return 0;
    }

    if (state->DMRvcL > 17) {
        state->DMRvcL = 17;
    }

    if (state->DMRvcL == 0) {
        int z = 0;
        int n = 8;
        DSD_MEMSET(state->ks_octetL, 0, sizeof(state->ks_octetL));
        DSD_MEMSET(state->ks_bitstreamL, 0, sizeof(state->ks_bitstreamL));
        state->bit_counterL = 0;
        des_ofb_keystream_output(state->payload_miP, state->R, state->ks_octetL, 19); //18 + 1

        for (int i = 0; i < 18 * 8; i++) //19 blocks minus 1 discard block at 8 bits each
        {
            for (int j = 0; j < 8; j++) {
                uint8_t b = (((state->ks_octetL[n] << j) & 0x80) >> 7);
                state->ks_bitstreamL[z++] = b;
            }
            n++;
        }
    }

    const int applied = mbeslot_xor_keystream_frame(state->ks_bitstreamL, MBE_SLOT_DES_KS_BITS, &state->bit_counterL,
                                                    frame_ctx->ambe_d);
    state->DMRvcL++;
    return applied;
}

static int
mbeslot_right_apply_des(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (!mbe_slot_algid_allowed(frame_ctx->protocol, state->payload_algidR)
        || !((state->payload_algidR == 0x22 && dsd_key_scalar_present(state, 1))
             || (state->payload_algidR == 0x81 && dsd_key_scalar_present(state, 1)))) {
        return 0;
    }

    if (state->DMRvcR > 17) {
        state->DMRvcR = 17;
    }

    if (state->DMRvcR == 0) {
        int z = 0;
        int n = 8;
        DSD_MEMSET(state->ks_octetR, 0, sizeof(state->ks_octetR));
        DSD_MEMSET(state->ks_bitstreamR, 0, sizeof(state->ks_bitstreamR));
        state->bit_counterR = 0;
        des_ofb_keystream_output(state->payload_miN, state->RR, state->ks_octetR, 19);
        for (int i = 0; i < 18 * 8; i++) {
            for (int j = 0; j < 8; j++) {
                uint8_t b = (((state->ks_octetR[n] << j) & 0x80) >> 7);
                state->ks_bitstreamR[z++] = b;
            }
            n++;
        }
    }

    const int applied = mbeslot_xor_keystream_frame(state->ks_bitstreamR, MBE_SLOT_DES_KS_BITS, &state->bit_counterR,
                                                    frame_ctx->ambe_d);
    state->DMRvcR++;
    return applied;
}

static int
mbeslot_left_aes_enabled(const dsd_state* state) {
    switch (state->payload_algid) {
        case 0x02: return dsd_key_scalar_present(state, 0);
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
        case 0x02: return dsd_key_scalar_present(state, 1);
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
        aes_ofb_keystream_output(state->aes_iv, aes_key, state->ks_octetL, DSD_AES_KEY_128, 10);
    }
    if (state->payload_algid == 0x25 || state->payload_algid == 0x84) { //AES256
        aes_ofb_keystream_output(state->aes_iv, aes_key, state->ks_octetL, DSD_AES_KEY_256, 10);
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
        aes_ofb_keystream_output(state->aes_ivR, aes_key, state->ks_octetR, DSD_AES_KEY_128, 10);
    }
    if (state->payload_algidR == 0x25 || state->payload_algidR == 0x84) {
        aes_ofb_keystream_output(state->aes_ivR, aes_key, state->ks_octetR, DSD_AES_KEY_256, 10);
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

/* Bits of generated keystream mbeslot_*_init_keystream() leaves in ks_bitstream for an ALG ID: AES-OFB's 9 blocks
   after the discard block, Hytera Enhanced's 135 RC4 octets (hytera_enhanced_rc4_setup()) and Kirisun's 126 octets
   (kirisun_*_keystream_creation()). The rest of the expanded 144 octets is zero fill, not keystream. */
static size_t
mbeslot_stream_keystream_bits(int algid) {
    switch (algid) {
        case 0x02: return (size_t)135U * 8U;
        case 0x36:
        case 0x37: return (size_t)126U * 8U;
        case 0x24:
        case 0x25:
        case 0x84:
        case 0x89: return (size_t)9U * 16U * 8U;
        default: return 0U;
    }
}

static int
mbeslot_left_apply_keystream_bits(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    // P25 ciphertext has no DMR clear-silence exception: every admitted word consumes and applies its 49 bits.
    if (frame_ctx->protocol == DSD_VOICE_PROTOCOL_P25P2) {
        return mbeslot_xor_keystream_frame(state->ks_bitstreamL, mbeslot_stream_keystream_bits(state->payload_algid),
                                           &state->bit_counterL, frame_ctx->ambe_d);
    }
    return dmr_voice_stream_apply_frame49(state->ks_bitstreamL, mbeslot_stream_keystream_bits(state->payload_algid),
                                          &state->bit_counterL, state->payload_algid, frame_ctx->ambe_d);
}

static int
mbeslot_right_apply_keystream_bits(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (frame_ctx->protocol == DSD_VOICE_PROTOCOL_P25P2) {
        return mbeslot_xor_keystream_frame(state->ks_bitstreamR, mbeslot_stream_keystream_bits(state->payload_algidR),
                                           &state->bit_counterR, frame_ctx->ambe_d);
    }
    return dmr_voice_stream_apply_frame49(state->ks_bitstreamR, mbeslot_stream_keystream_bits(state->payload_algidR),
                                          &state->bit_counterR, state->payload_algidR, frame_ctx->ambe_d);
}

/* Returns 1 when the frame was decrypted (or skipped as silence), 0 when no stream cipher applies, -1 when its
   keystream is exhausted. */
static int
mbeslot_left_apply_aes_and_streams(dsd_opts* opts, dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (!mbe_slot_algid_allowed(frame_ctx->protocol, state->payload_algid) || !mbeslot_left_aes_enabled(state)) {
        return 0;
    }

    uint8_t aes_key[32];
    mbeslot_left_make_aes_key(state, aes_key);

    if (state->DMRvcL > 17) {
        state->DMRvcL = 17;
    }
    if (state->DMRvcL == 0) {
        mbeslot_left_init_keystream(opts, state, aes_key);
    }

    const int applied = mbeslot_left_apply_keystream_bits(state, frame_ctx);
    DSD_SECURE_ZERO(aes_key, sizeof(aes_key));
    state->DMRvcL++;
    if (applied < 0) {
        return -1;
    }
    return 1;
}

static int
mbeslot_right_apply_aes_and_streams(dsd_opts* opts, dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (!mbe_slot_algid_allowed(frame_ctx->protocol, state->payload_algidR) || !mbeslot_right_aes_enabled(state)) {
        return 0;
    }

    uint8_t aes_key[32];
    mbeslot_right_make_aes_key(state, aes_key);

    if (state->DMRvcR > 17) {
        state->DMRvcR = 17;
    }
    if (state->DMRvcR == 0) {
        mbeslot_right_init_keystream(opts, state, aes_key);
    }

    const int applied = mbeslot_right_apply_keystream_bits(state, frame_ctx);
    DSD_SECURE_ZERO(aes_key, sizeof(aes_key));
    state->DMRvcR++;
    if (applied < 0) {
        return -1;
    }
    return 1;
}

static void
mbeslot_left_apply_rc4(dsd_state* state, mbe_frame_ctx_t* frame_ctx) {
    if (!(mbe_slot_algid_allowed(frame_ctx->protocol, state->payload_algid) && state->payload_algid == 0x21
          && dsd_key_scalar_present(state, 0))) {
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

    if (dmr_ambe49_should_skip_crypto(frame_ctx->ambe_d) == 1) {
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
    if (!(mbe_slot_algid_allowed(frame_ctx->protocol, state->payload_algidR) && state->payload_algidR == 0x21
          && dsd_key_scalar_present(state, 1))) {
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

    if (dmr_ambe49_should_skip_crypto(frame_ctx->ambe_d) == 1) {
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
    if (!(state->payload_algid == 0xAA && dsd_key_scalar_present(state, 0)
          && frame_ctx->protocol == DSD_VOICE_PROTOCOL_P25P2)) {
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
    if (!(state->payload_algidR == 0xAA && dsd_key_scalar_present(state, 1)
          && frame_ctx->protocol == DSD_VOICE_PROTOCOL_P25P2)) {
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
        store_process_result(MBE_STATUS_INVALID_BITS, state->audio_out_temp_buf, &state->errs, &state->errs2,
                             state->err_str, sizeof(state->err_str), NULL);
        return;
    }

    char decoded_ambe_d[49];
    DSD_MEMCPY(decoded_ambe_d, frame_ctx->ambe_d, sizeof(decoded_ambe_d));

    // DMR's talkgroup key autoload, privacy, Hytera, Vertex and vendor scramblers are DMR's alone: clear dPMR and P25
    // Phase 2 reach this path too (issue #656).
    const int dmr_owned = frame_ctx->protocol == DSD_VOICE_PROTOCOL_DMR;
    if (dmr_owned) {
        mbeslot_left_autoload_keys(state);
        mbeslot_left_apply_basic_privacy(state, frame_ctx);
    }
    mbeslot_left_apply_vertex_standard(state, frame_ctx);
    // Past its keystream the frame is muted, unless the manual -S keystream is armed: it replaces the signalled
    // cipher (and clears the ALG ID) below, as it does on a frame the cipher decrypted.
    if ((mbeslot_left_apply_des(state, frame_ctx) < 0 || mbeslot_left_apply_aes_and_streams(opts, state, frame_ctx) < 0)
        && !mbe_slot_straight_ks_armed(state)) {
        mbe_mute_exhausted_frame(opts, state, 0, frame_ctx);
        return;
    }
    mbeslot_left_apply_rc4(state, frame_ctx);
    mbeslot_left_apply_p25p2_rc4(state, frame_ctx);
    if (dmr_owned) {
        mbe_apply_vendor_overlays(state, frame_ctx->ambe_d);
    }
    if (mbe_slot_apply_straight_ks_left(state, frame_ctx->ambe_d)) {
        frame_ctx->slot_keyed[0] = 1;
        frame_ctx->straight_ks[0] = 1;
    }
    (void)dsd_mbe_strip_ambe_context_if_changed(decoded_ambe_d, frame_ctx->ambe_d, &ambe_result);
    mbe_finalize_slot_left(opts, state, frame_ctx->ambe_d, &ambe_result);
    frame_ctx->capture = MBE_CAPTURE_SLOT_LEFT;
}

static void
mbeslot_process_right(dsd_opts* opts, dsd_state* state, char ambe_fr[4][24], dsd_vocoder_soft_bit ambe_soft_fr[4][24],
                      mbe_frame_ctx_t* frame_ctx) {
    mbe_process_result ambe_result;
    int have_ambe_result =
        decode_ambe2450_frame(&state->errsR, &state->errs2R, ambe_fr, ambe_soft_fr, frame_ctx->ambe_d, &ambe_result);
    if (!have_ambe_result) {
        store_process_result(MBE_STATUS_INVALID_BITS, state->audio_out_temp_bufR, &state->errsR, &state->errs2R,
                             state->err_strR, sizeof(state->err_strR), NULL);
        return;
    }

    char decoded_ambe_d[49];
    DSD_MEMCPY(decoded_ambe_d, frame_ctx->ambe_d, sizeof(decoded_ambe_d));

    const int dmr_owned = frame_ctx->protocol == DSD_VOICE_PROTOCOL_DMR;
    if (dmr_owned) {
        mbeslot_right_autoload_keys(state);
        mbeslot_right_apply_basic_privacy(state, frame_ctx);
    }
    mbeslot_right_apply_vertex_standard(state, frame_ctx);
    if ((mbeslot_right_apply_des(state, frame_ctx) < 0
         || mbeslot_right_apply_aes_and_streams(opts, state, frame_ctx) < 0)
        && !mbe_slot_straight_ks_armed(state)) {
        mbe_mute_exhausted_frame(opts, state, 1, frame_ctx);
        return;
    }
    mbeslot_right_apply_rc4(state, frame_ctx);
    mbeslot_right_apply_p25p2_rc4(state, frame_ctx);
    if (dmr_owned) {
        mbe_apply_vendor_overlays(state, frame_ctx->ambe_d);
    }
    if (mbe_slot_apply_straight_ks_right(state, frame_ctx->ambe_d)) {
        frame_ctx->slot_keyed[1] = 1;
        frame_ctx->straight_ks[1] = 1;
    }
    (void)dsd_mbe_strip_ambe_context_if_changed(decoded_ambe_d, frame_ctx->ambe_d, &ambe_result);
    mbe_finalize_slot_right(opts, state, frame_ctx->ambe_d, &ambe_result);
    frame_ctx->capture = MBE_CAPTURE_SLOT_RIGHT;
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

// A muted slot stages silence: the stereo mixes copy each slot's short frame into their superframe buffers every
// frame, and a frame left unrefreshed would replay the last one staged on that slot. The silence replaces the short
// media staged on the slot, so it retires the slot's SHORT mark for the stamp (dsd_audio_dmr_mix_media_silenced()).
static void
mbe_post_stage_slot_silence(dsd_state* state, int slot) {
    if (slot == 0) {
        DSD_MEMSET(state->s_l, 0, sizeof(state->s_l));
    } else {
        DSD_MEMSET(state->s_r, 0, sizeof(state->s_r));
    }
    state->mbe_short_silenced[slot ? 1 : 0] = 1U;
    if (dsd_audio_activity_armed()) {
        dsd_audio_dmr_mix_media_silenced(slot, DSD_DMR_MIX_MEDIA_SHORT);
    }
}

// Decoded media a DMR slot staged for the stereo mix's audible-audio stamp (dsd_audio_dmr_mix_media_staged(), issue
// #574); called only while the stamp is armed. P25 Phase 2 stages through here as well, but its playout reads the
// provenance from dsd_state::mbe_short_silenced instead (issue #651).
static void
mbe_post_note_dmr_mix_media(const dsd_state* state, int slot, unsigned int kind) {
    if (DSD_SYNC_IS_DMR(state->synctype)) {
        dsd_audio_dmr_mix_media_staged(slot, kind);
    }
}

// The DMR classification of the current slot's frame (issue #655), from the encryption evidence it arrived with: the
// privacy bit of the service options or an ALG ID other than clear. A mapped Vertex keystream clears the bit, so both
// are read from before the frame's transforms. The -S keystream clears both on every frame, leaving the burst's later
// frames nothing to judge: a frame it ran on counts as an encrypted call it decrypted, so -q treats the whole burst
// alike. Decryptable when a loaded key decrypts the ALG ID (dsd_dmr_voice_slot_can_decrypt(); with no ALG ID, the
// loaded scalar, Basic or Hytera material the LC and PI classification also count, dsd_dmr_missing_alg_key_can_decrypt()),
// when the frame got a keyed transform (Basic or Hytera Privacy, forced with -4 too, a mapped Vertex keystream), or
// under forced clear (Baofeng AP, CSI EE, Kenwood scrambler, static keys forced onto every frame).
static dsd_voice_crypto_class
mbe_dmr_slot_crypto_class(const dsd_state* state, int slot, const mbe_frame_ctx_t* frame_ctx) {
    if (frame_ctx->straight_ks[slot]) {
        return DSD_VOICE_CRYPTO_DECRYPTABLE;
    }
    const int algid = frame_ctx->frame_algid;
    if ((frame_ctx->frame_so & 0x40) == 0 && (algid == 0 || algid == 0x80)) {
        return DSD_VOICE_CRYPTO_CLEAR;
    }
    const int vertex = (slot == 0) ? frame_ctx->vertex_ks_applied_l : frame_ctx->vertex_ks_applied_r;
    int decryptable = frame_ctx->slot_keyed[slot] || vertex == 1 || dsd_key_dmr_forced_clear(state);
    if (!decryptable) {
        decryptable = (algid == 0)
                          ? dsd_dmr_missing_alg_key_can_decrypt(state, slot)
                          : dsd_dmr_voice_slot_can_decrypt(state, slot, algid, (slot == 0) ? state->R : state->RR);
    }
    return decryptable ? DSD_VOICE_CRYPTO_DECRYPTABLE : DSD_VOICE_CRYPTO_UNDECRYPTABLE;
}

// The explicit frame owner decides the verdict as well as the transforms, even when YSF or dPMR borrows another
// protocol's decoder outside processFrame(). An exhausted keystream cannot be overridden by a key, -q or a user mute.
static int
mbe_frame_slot_muted(const dsd_opts* opts, const dsd_state* state, int slot, const mbe_frame_ctx_t* frame_ctx) {
    if (frame_ctx->crypto_muted) {
        return 1;
    }
    switch (frame_ctx->protocol) {
        case DSD_VOICE_PROTOCOL_DMR:
            return !dsd_voice_crypto_class_audible(opts, slot, mbe_dmr_slot_crypto_class(state, slot, frame_ctx));
        case DSD_VOICE_PROTOCOL_P25P1: return !p25_crypto_audio_output_permitted(opts, state, 0);
        case DSD_VOICE_PROTOCOL_P25P2:
            return state->p25_p2_audio_allowed[slot] == 0 || !p25_crypto_audio_output_permitted(opts, state, slot);
        case DSD_VOICE_PROTOCOL_NXDN:
        case DSD_VOICE_PROTOCOL_DPMR:
            return !dsd_voice_crypto_class_audible(opts, 0, dsd_nxdn_voice_crypto_class(state));
        default: return 0;
    }
}

static int
mbe_post_stereo_active(const dsd_opts* opts, const dsd_state* state) {
    return opts->dmr_stereo == 1 || (DSD_SYNC_IS_DMR(state->synctype) && state->dmr_stereo == 1);
}

/* What the post stage does with a slot for this frame: nothing, silence (a DMR mono slot not playing), or the frame
   under the verdict mbe_frame_slot_muted() set. */
typedef enum {
    MBE_POST_SLOT_SKIP = 0,
    MBE_POST_SLOT_MONO_OFF,
    MBE_POST_SLOT_ACTIVE,
    MBE_POST_SLOT_CRYPTO_MUTED, /* the frame ran past its keystream (issue #656): it stages silence */
} mbe_post_slot_class;

static mbe_post_slot_class
mbe_post_slot_classify(const dsd_opts* opts, dsd_state* state, const mbe_frame_ctx_t* frame_ctx, int slot) {
    const int dmr_mono_active = mbe_post_dmr_mono_active(opts, state);
    if (dmr_mono_active && !mbe_dmr_output_slot_enabled(opts, state, slot)) {
        state->mbe_slot_muted[slot] = 1U;
        return MBE_POST_SLOT_MONO_OFF;
    }
    if ((!dmr_mono_active && !mbe_post_stereo_active(opts, state)) || state->currentslot != slot) {
        return MBE_POST_SLOT_SKIP;
    }
    return frame_ctx->crypto_muted ? MBE_POST_SLOT_CRYPTO_MUTED : MBE_POST_SLOT_ACTIVE;
}

static void
mbe_post_left_stage(const dsd_opts* opts, dsd_state* state, mbe_post_slot_class slot_class) {
    if (slot_class == MBE_POST_SLOT_MONO_OFF) {
        mbe_post_stage_slot_silence(state, 0);
        return;
    }
    if (slot_class == MBE_POST_SLOT_CRYPTO_MUTED) {
        mbe_post_stage_slot_silence(state, 0);
        return;
    }
    if (slot_class != MBE_POST_SLOT_ACTIVE) {
        return;
    }

    state->debug_audio_errors += state->errs2;
    if (state->mbe_slot_muted[0] == 0) {
        dsd_voice_stage_short_left(opts, state);
        state->mbe_short_silenced[0] = 0U;
        if (dsd_audio_activity_armed()) {
            mbe_post_note_dmr_mix_media(state, 0, DSD_DMR_MIX_MEDIA_SHORT);
        }
    } else {
        mbe_post_stage_slot_silence(state, 0);
    }
}

static void
mbe_post_right_stage(const dsd_opts* opts, dsd_state* state, mbe_post_slot_class slot_class) {
    if (slot_class == MBE_POST_SLOT_MONO_OFF) {
        mbe_post_stage_slot_silence(state, 1);
        return;
    }
    if (slot_class == MBE_POST_SLOT_CRYPTO_MUTED) {
        mbe_post_stage_slot_silence(state, 1);
        return;
    }
    if (slot_class != MBE_POST_SLOT_ACTIVE) {
        return;
    }

    state->debug_audio_errorsR += state->errs2R;
    if (state->mbe_slot_muted[1] == 0) {
        dsd_voice_stage_short_right(opts, state);
        state->mbe_short_silenced[1] = 0U;
        if (dsd_audio_activity_armed()) {
            mbe_post_note_dmr_mix_media(state, 1, DSD_DMR_MIX_MEDIA_SHORT);
        }
    } else {
        mbe_post_stage_slot_silence(state, 1);
    }
}

static void
mbe_post_other_process_audio(const dsd_opts* opts, dsd_state* state, int is_p25p2) {
    const int slot = (is_p25p2 && state->currentslot == 1) ? 1 : 0;
    state->debug_audio_errors += state->errs2;
    if (opts->audio_out != 1 && !(opts->wav_out_f != NULL && opts->static_wav_file == 1)) {
        // The slot's short frame is left as it was: it holds no sample of this frame.
        state->mbe_short_silenced[slot] = 1U;
        return;
    }
    if (slot == 1) {
        dsd_voice_stage_short_right(opts, state);
    } else {
        dsd_voice_stage_short_left(opts, state);
    }
    state->mbe_short_silenced[slot] = 0U;
}

// The mono post path plays the frame when the verdict on its slot lets it (issue #655): YSF full rate, decoded through
// the P25 Phase 1 decoder, is judged as YSF, not by a P25 classification its session never made.
static int
mbe_post_other_is_allowed(const dsd_state* state, int is_p25p2) {
    if (!is_p25p2) {
        return state->mbe_slot_muted[state->currentslot == 1 ? 1 : 0] == 0;
    }

    if (state->currentslot != 0 && state->currentslot != 1) {
        return 0;
    }
    return state->mbe_slot_muted[state->currentslot] == 0;
}

static void
mbe_post_other_audio(const dsd_opts* opts, dsd_state* state, const mbe_frame_ctx_t* frame_ctx) {
    if (mbe_post_dmr_mono_active(opts, state) || mbe_post_stereo_active(opts, state)) {
        return;
    }

    int is_p25p2 = DSD_SYNC_IS_P25P2(state->synctype);
    if (frame_ctx->crypto_muted) {
        mbe_post_stage_slot_silence(state, (is_p25p2 && state->currentslot == 1) ? 1 : 0);
        return;
    }
    if (mbe_post_other_is_allowed(state, is_p25p2)) {
        mbe_post_other_process_audio(opts, state, is_p25p2);
    } else {
        mbe_post_stage_slot_silence(state, (is_p25p2 && state->currentslot == 1) ? 1 : 0);
    }
}

static int
mbe_post_uses_mono_left_staging(const dsd_state* state) {
    return DSD_SYNC_IS_X2TDMA(state->synctype) || DSD_SYNC_IS_DSTAR(state->synctype);
}

static void
mbe_post_mono_left_audio(const dsd_opts* opts, dsd_state* state) {
    int frame_errors = state->errs2;
    if (DSD_SYNC_IS_X2TDMA(state->synctype) && state->currentslot == 1) {
        frame_errors = state->errs2R;
    }
    state->debug_audio_errors += frame_errors;
    dsd_voice_stage_short_left(opts, state);
}

// The slot whose per-call WAV a frame on the mono post path belongs to: DMR mono's playing slot, and a P25 Phase 2
// frame's own timeslot whatever opts->dmr_stereo says (issue #651); every other mono source is slot 0.
static int
mbe_post_mono_wav_slot(const dsd_opts* opts, const dsd_state* state) {
    if (state->currentslot != 1) {
        return 0;
    }
    return (mbe_post_dmr_mono_active(opts, state) || DSD_SYNC_IS_P25P2(state->synctype)) ? 1 : 0;
}

static int
mbe_post_allow_mono_wav(const dsd_opts* opts, const dsd_state* state) {
    const int dmr_mono_active = mbe_post_dmr_mono_active(opts, state);
    const int slot = mbe_post_mono_wav_slot(opts, state);
    if (opts->static_wav_file != 0) {
        return 0;
    }
    if (dmr_mono_active) {
        if (!mbe_dmr_output_slot_enabled(opts, state, slot)) {
            return 0;
        }
    } else if (mbe_post_stereo_active(opts, state)) {
        return 0;
    }
    if ((slot == 0 ? opts->wav_out_f : opts->wav_out_fR) == NULL) {
        return 0;
    }
    int allow_wav = 0;
    return (dsd_audio_record_gate_mono(opts, state, &allow_wav) == 0 && allow_wav) ? 1 : 0;
}

static int
mbe_post_allow_stereo_slot_wav(const dsd_opts* opts, const dsd_state* state, int slot) {
    if (opts->dmr_stereo_wav != 1 || !mbe_post_stereo_active(opts, state) || mbe_post_dmr_mono_active(opts, state)
        || state->currentslot != slot) {
        return 0;
    }
    int allow_wav = 0;
    return (dsd_audio_record_gate_mono(opts, state, &allow_wav) == 0 && allow_wav) ? 1 : 0;
}

// X2-TDMA and D-STAR classify no encryption, so their WAV (mbe_post_wav_outputs()) answers to the talkgroup policy
// alone.
// The mono post path's per-call WAV: slot 2's for DMR mono playing slot 2, and for a P25 Phase 2 slot-2 frame whatever
// opts->dmr_stereo says (issue #651).
static void
mbe_post_write_mono_wav(dsd_opts* opts, dsd_state* state) {
    if ((mbe_post_dmr_mono_active(opts, state) && state->dmr_mono_slot == 1)
        || (DSD_SYNC_IS_P25P2(state->synctype) && state->currentslot == 1)) {
        writeSynthesizedVoiceR(opts, state);
    } else {
        writeSynthesizedVoice(opts, state);
    }
}

static void
mbe_post_wav_outputs(dsd_opts* opts, dsd_state* state) {
    if (DSD_SYNC_IS_X2TDMA(state->synctype)) {
        // X2-TDMA stages either timeslot in the mono buffer. Only the per-call WAV (-P) is written here, into the
        // file of the timeslot the frame belongs to; the static WAV (-w) is the mixer's, in its stereo format
        // (issue #654).
        const int slot = state->currentslot == 1 ? 1 : 0;
        if (opts->dmr_stereo_wav == 1 && (slot == 1 ? opts->wav_out_fR : opts->wav_out_f) != NULL
            && mbe_record_policy_allows(opts, state, slot)) {
            dsd_write_mono_voice_to_slot_wav(opts, state, slot);
        }
        return;
    }
    if (DSD_SYNC_IS_DSTAR(state->synctype)) {
        // D-STAR publishes on slot 0 whatever slot a TDMA decode left current.
        if (opts->wav_out_f != NULL && opts->dmr_stereo_wav == 1 && mbe_record_policy_allows(opts, state, 0)) {
            writeSynthesizedVoice(opts, state);
        }
        return;
    }

    if (mbe_post_allow_mono_wav(opts, state)) {
        mbe_post_write_mono_wav(opts, state);
    }
    if (mbe_post_allow_stereo_slot_wav(opts, state, 0)) {
        writeSynthesizedVoice(opts, state);
    }
    if (mbe_post_allow_stereo_slot_wav(opts, state, 1)) {
        writeSynthesizedVoiceR(opts, state);
    }
}

// A frame records as its per-call WAV does (dsd_audio_record_slot_allows_audio()): by the verdict
// mbe_frame_slot_muted() just set on its slot, the protocol's own crypto rule, the user's encrypted-audio mute and
// reverse mute included (issue #655).
static int
mbe_capture_slot_verdict_allows(const dsd_state* state, int slot) {
    return state->mbe_slot_muted[slot == 1 ? 1 : 0] == 0;
}

// The FDMA save points: the IMBE frames of the P25 Phase 1 decoder (live P25, and YSF full rate, judged as YSF) by the
// frame's verdict, ProVoice and D-STAR by the record policy alone. D-STAR publishes on slot 0 whatever slot a TDMA
// decode left current.
static void
mbe_capture_fdma_frame(dsd_opts* opts, dsd_state* state, const mbe_frame_ctx_t* frame_ctx) {
    if (!dsd_mbe_out_enabled(opts, 0) || !mbe_record_policy_allows(opts, state, 0)) {
        return;
    }
    if (frame_ctx->capture == MBE_CAPTURE_P25P1) {
        if (mbe_capture_slot_verdict_allows(state, state->currentslot)) {
            dsd_mbe_out_write(opts, state, 0, DSD_MBE_OUT_FORMAT_IMBE, frame_ctx->imbe_d);
        }
    } else if (frame_ctx->capture == MBE_CAPTURE_PROVOICE) {
        dsd_mbe_out_write(opts, state, 0, DSD_MBE_OUT_FORMAT_IMBE, frame_ctx->imbe_d);
    } else {
        dsd_mbe_out_write(opts, state, 0, DSD_MBE_OUT_FORMAT_DSTAR, frame_ctx->ambe_d);
    }
}

// The AMBE 2450 save points that read the slot verdict the post stage just classified: NXDN (and the frames YSF and
// scrambled dPMR decode through it) on slot 1, and the DMR / P25 Phase 2 / clear dPMR slot path on its slot.
static void
mbe_capture_verdict_frame(dsd_opts* opts, dsd_state* state, const mbe_frame_ctx_t* frame_ctx) {
    const int slot = frame_ctx->capture == MBE_CAPTURE_SLOT_RIGHT ? 1 : 0;
    if (!dsd_mbe_out_enabled(opts, slot)) {
        return;
    }
    if (frame_ctx->capture == MBE_CAPTURE_NXDN) {
        if (mbe_capture_slot_verdict_allows(state, state->currentslot) && mbe_record_policy_allows(opts, state, 0)) {
            dsd_mbe_out_write(opts, state, 0, DSD_MBE_OUT_FORMAT_AMBE, frame_ctx->ambe_d);
        }
        return;
    }
    // YSF is FDMA: it publishes on slot 0 even when a TDMA decode left currentslot at 1.
    const int policy_slot = (slot == 1 && !DSD_SYNC_IS_YSF(state->synctype)) ? 1 : 0;
    if (mbe_dmr_output_slot_enabled(opts, state, slot) && mbe_capture_slot_verdict_allows(state, slot)
        && mbe_record_policy_allows(opts, state, policy_slot)) {
        dsd_mbe_out_write(opts, state, slot, DSD_MBE_OUT_FORMAT_AMBE, frame_ctx->ambe_d);
    }
}

// MBE capture (-d) of the frame, after the post stage classified its slot verdict and before staging: each save point
// keeps its own crypto condition and adds the talkgroup record policy (issue #661).
static void
mbe_capture_frame(dsd_opts* opts, dsd_state* state, const mbe_frame_ctx_t* frame_ctx) {
    switch (frame_ctx->capture) {
        case MBE_CAPTURE_P25P1:
        case MBE_CAPTURE_PROVOICE:
        case MBE_CAPTURE_DSTAR: mbe_capture_fdma_frame(opts, state, frame_ctx); break;
        case MBE_CAPTURE_X2:
            if (dsd_mbe_out_enabled(opts, 0)
                && mbe_record_policy_allows(opts, state, state->currentslot == 1 ? 1 : 0)) {
                mbe_save_x2_frame(opts, state, frame_ctx->ambe_d);
            }
            break;
        case MBE_CAPTURE_NXDN:
        case MBE_CAPTURE_SLOT_LEFT:
        case MBE_CAPTURE_SLOT_RIGHT: mbe_capture_verdict_frame(opts, state, frame_ctx); break;
        case MBE_CAPTURE_NONE:
        default: break;
    }
}

static void
mbe_post_audio_and_recording(dsd_opts* opts, dsd_state* state, const mbe_frame_ctx_t* frame_ctx) {
    const int slot = state->currentslot == 1 ? 1 : 0;
    // The frame goes through its slot's voice chain once, before anything reads it (issue #653): the shared short
    // staging and the per-call WAV take the same filtered, gained samples, whatever the output format, the output
    // type or the mute. An exhausted keystream produced no decoded media: keep that silence out of the HPF so its
    // history cannot revive it. The gates below still decide who receives it.
    if (frame_ctx->synth && !frame_ctx->crypto_muted) {
        dsd_voice_chain_run(opts, state, frame_ctx->synth_slot, frame_ctx->synth);
    }
    // Until a branch below stages this frame's short samples, the current slot's short frame is not this frame's.
    state->mbe_short_silenced[slot] = 1U;
    // The frame's one verdict (issue #655), after its keystream and before anything reads it: capture, short staging,
    // the per-call WAV, and the mixers that play it.
    state->mbe_slot_muted[slot] = (uint8_t)mbe_frame_slot_muted(opts, state, slot, frame_ctx);
    // A frame that stages no samples must not leave a previous frame for the mono mixer (issue #654).
    state->audio_out_idx = 0;
    if (mbe_post_uses_mono_left_staging(state)) {
        mbe_capture_frame(opts, state, frame_ctx);
        mbe_post_mono_left_audio(opts, state);
    } else {
        // Both slots are classified before the capture saves the frame (a DMR mono slot not playing is muted), then
        // the slots stage: staging reads no verdict of the other slot and classification reads nothing staging writes.
        const mbe_post_slot_class left = mbe_post_slot_classify(opts, state, frame_ctx, 0);
        const mbe_post_slot_class right = mbe_post_slot_classify(opts, state, frame_ctx, 1);
        mbe_capture_frame(opts, state, frame_ctx);
        mbe_post_left_stage(opts, state, left);
        mbe_post_right_stage(opts, state, right);
        mbe_post_other_audio(opts, state, frame_ctx);
    }
    mbe_post_wav_outputs(opts, state);

    if (opts->audio_out_type == 9) {
        opts->audio_out = 0;
    }
}

// Each file replays its own traffic. An SDRTrunk JSON export leaves its last
// call open and its protocol set at EOF, and a raw .imb/.amb/.dmb that follows
// publishes neither, so the output gates would judge it on the previous file's
// talkgroup, or as an unclassified live P25 call. Start each file as a fresh
// -r run does: no call and no protocol.
static void
mbe_file_end_replayed_call(dsd_opts* opts, dsd_state* state) {
    if (dsd_call_state_end(state, 0U, 0.0) > 0) {
        dsd_event_sync_slot(opts, state, 0U);
    }
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    // A file is a reception of its own: its first frame starts from clear high-pass state (issue #653).
    dsd_voice_hpf_reset(state, -1);
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
        mbe_file_end_replayed_call(opts, state);
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
            if (dsd_exitflag_load() == 1) {
                dsd_request_shutdown(opts, state);
                break;
            }
        }
        if (opts->mbe_in_f != NULL) {
            fclose(opts->mbe_in_f); //close file after playing it
            opts->mbe_in_f = NULL;
        }
        if (dsd_exitflag_load() == 1) {
            return;
        }
    }
}

static int
mark_vocoder_call_media_sync_supported(int protocol) {
    return DSD_SYNC_IS_P25P1(protocol) || protocol == DSD_SYNC_X2TDMA_VOICE_POS || protocol == DSD_SYNC_X2TDMA_VOICE_NEG
           || DSD_SYNC_IS_DSTAR(protocol) || protocol == DSD_SYNC_DMR_BS_VOICE_POS
           || protocol == DSD_SYNC_DMR_BS_VOICE_NEG || protocol == DSD_SYNC_DMR_MS_VOICE
           || DSD_SYNC_IS_PROVOICE(protocol) || DSD_SYNC_IS_NXDN(protocol) || DSD_SYNC_IS_YSF(protocol)
           || DSD_SYNC_IS_DPMR(protocol);
}

typedef enum {
    MBE_MEDIA_FAMILY_NONE,
    MBE_MEDIA_FAMILY_P25P1,
    MBE_MEDIA_FAMILY_X2TDMA,
    MBE_MEDIA_FAMILY_DSTAR,
    MBE_MEDIA_FAMILY_DMR,
    MBE_MEDIA_FAMILY_PROVOICE,
    MBE_MEDIA_FAMILY_NXDN,
    MBE_MEDIA_FAMILY_YSF,
    MBE_MEDIA_FAMILY_DPMR,
} mbe_media_protocol_family;

static int
mark_vocoder_call_media_protocol_family(int protocol) {
    if (DSD_SYNC_IS_P25P1(protocol)) {
        return MBE_MEDIA_FAMILY_P25P1;
    }
    if (DSD_SYNC_IS_X2TDMA(protocol)) {
        return MBE_MEDIA_FAMILY_X2TDMA;
    }
    if (DSD_SYNC_IS_DSTAR(protocol)) {
        return MBE_MEDIA_FAMILY_DSTAR;
    }
    if (DSD_SYNC_IS_DMR(protocol)) {
        return MBE_MEDIA_FAMILY_DMR;
    }
    if (DSD_SYNC_IS_PROVOICE(protocol)) {
        return MBE_MEDIA_FAMILY_PROVOICE;
    }
    if (DSD_SYNC_IS_NXDN(protocol)) {
        return MBE_MEDIA_FAMILY_NXDN;
    }
    if (DSD_SYNC_IS_YSF(protocol)) {
        return MBE_MEDIA_FAMILY_YSF;
    }
    return DSD_SYNC_IS_DPMR(protocol) ? MBE_MEDIA_FAMILY_DPMR : MBE_MEDIA_FAMILY_NONE;
}

static int
mark_vocoder_call_media_protocol_compatible(int decoder_protocol, int call_protocol) {
    const int decoder_family = mark_vocoder_call_media_protocol_family(decoder_protocol);
    const int call_family = mark_vocoder_call_media_protocol_family(call_protocol);
    if (decoder_family != MBE_MEDIA_FAMILY_NONE && decoder_family == call_family) {
        return 1;
    }

    /* YSF and scrambled dPMR temporarily select another synctype to reuse an MBE decoder. */
    if (call_family == MBE_MEDIA_FAMILY_YSF
        && (decoder_family == MBE_MEDIA_FAMILY_NXDN || decoder_family == MBE_MEDIA_FAMILY_P25P1)) {
        return 1;
    }
    return call_family == MBE_MEDIA_FAMILY_DPMR && decoder_family == MBE_MEDIA_FAMILY_NXDN;
}

static uint8_t
mark_vocoder_call_media_slot(int protocol, int current_slot) {
    if ((DSD_SYNC_IS_X2TDMA(protocol) || DSD_SYNC_IS_DMR(protocol)) && current_slot == 1) {
        return 1U;
    }
    return 0U;
}

// Returns 1 and fills *out_call when the slot carries a matching active call, so the caller can
// reuse this snapshot instead of taking the lock a second time. dsd_call_state_update_media()
// below touches only media_active, which no reader here reads.
//
// A BEGIN taken here can still hand back an immediately-mappable snapshot: dsd_call_state_observe()
// specializes a provisional (identity-less) epoch rather than forking, and reacquires a
// recoverably-ended one, seeding it with the ending epoch's kind/target/etc. -- e.g. a brief
// sync loss mid-call, or late entry that lands between the previous epoch's end and this
// transmission's own header. Either way the just-observed epoch may already carry real identity,
// so the snapshot taken before the observe() is stale and this re-fetches after it rather than
// handing back NULL for that frame. The extra lock+copy only happens on this rare BEGIN frame,
// not the ~50 Hz steady state, which is what the reuse path above still avoids.
static int
mark_vocoder_call_media(dsd_opts* opts, dsd_state* state, dsd_call_snapshot* out_call) {
    int protocol = state->synctype;
    const int voice_sync = mark_vocoder_call_media_sync_supported(protocol);
    if (!voice_sync) {
        return 0;
    }
    const uint8_t slot = mark_vocoder_call_media_slot(protocol, state->currentslot);
    dsd_call_snapshot call;
    int has_active = dsd_call_state_get(state, slot, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
                     && mark_vocoder_call_media_protocol_compatible(protocol, call.protocol);
    if (!has_active) {
        const dsd_call_observation observation = {
            .protocol = protocol,
            .slot = slot,
            .kind = DSD_CALL_KIND_VOICE,
        };
        if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0) {
            dsd_event_sync_slot(opts, state, slot);
            has_active = dsd_call_state_get(state, slot, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
                         && mark_vocoder_call_media_protocol_compatible(protocol, call.protocol);
        }
    }
    (void)dsd_call_state_update_media(state, slot, 1, 0.0);

    if (has_active && out_call != NULL) {
        *out_call = call;
        return 1;
    }
    return 0;
}

static void
processMbeFrameInternal(dsd_opts* opts, dsd_state* state, dsd_voice_protocol protocol, char imbe_fr[8][23],
                        char ambe_fr[4][24], char imbe7100_fr[7][24], dsd_vocoder_soft_bit imbe_soft_fr[8][23],
                        dsd_vocoder_soft_bit ambe_soft_fr[4][24], dsd_vocoder_soft_bit imbe7100_soft_fr[7][24]) {
    mbe_frame_ctx_t frame_ctx;
    dsd_call_snapshot call;

    const int have_call = mark_vocoder_call_media(opts, state, &call);
    mbe_prepare_frame_state(state, &frame_ctx, have_call ? &call : NULL, protocol);

    // Every decode below writes its frame, speech or silence, into the buffer named here, on every path.
    frame_ctx.synth = state->audio_out_temp_buf;
    if (DSD_SYNC_IS_P25P1(state->synctype)) {
        mbe_process_p25p1(opts, state, imbe_fr, imbe_soft_fr, &frame_ctx);
    } else if (DSD_SYNC_IS_PROVOICE(state->synctype)) {
        mbe_process_provoice(opts, state, imbe7100_fr, imbe7100_soft_fr, &frame_ctx);
    } else if (DSD_SYNC_IS_DSTAR(state->synctype)) {
        /* Header synctypes too: processDSTAR_HD() decodes the superframe behind the header under its own synctype. */
        mbe_process_dstar(opts, state, ambe_fr, ambe_soft_fr, &frame_ctx);
    } else if (DSD_SYNC_IS_X2TDMA(state->synctype)) {
        // Both timeslots synthesize into the left buffer, each on its own slot's chain.
        frame_ctx.synth_slot = (state->currentslot == 1) ? 1 : 0;
        mbe_process_x2(opts, state, ambe_fr, ambe_soft_fr, &frame_ctx);
    } else if (DSD_SYNC_IS_NXDN(state->synctype)) {
        mbe_process_nxdn(opts, state, ambe_fr, ambe_soft_fr, &frame_ctx);
    } else {
        // The slot path synthesizes slot 1 into the left buffer and slot 2 into the right one, and nothing for any
        // other slot value.
        if (state->currentslot == 1) {
            frame_ctx.synth = state->audio_out_temp_bufR;
            frame_ctx.synth_slot = 1;
        } else if (state->currentslot != 0) {
            frame_ctx.synth = NULL;
        }
        mbe_process_slot_traffic(opts, state, ambe_fr, ambe_soft_fr, &frame_ctx);
    }

    mbe_post_audio_and_recording(opts, state, &frame_ctx);
}

void
processMbeFrame(dsd_opts* opts, dsd_state* state, dsd_voice_protocol protocol, char imbe_fr[8][23], char ambe_fr[4][24],
                char imbe7100_fr[7][24]) {
    processMbeFrameInternal(opts, state, protocol, imbe_fr, ambe_fr, imbe7100_fr, NULL, NULL, NULL);
}

void
processMbeFrameSoft(dsd_opts* opts, dsd_state* state, dsd_voice_protocol protocol, dsd_vocoder_soft_bit imbe_fr[8][23],
                    dsd_vocoder_soft_bit ambe_fr[4][24], dsd_vocoder_soft_bit imbe7100_fr[7][24]) {
    processMbeFrameInternal(opts, state, protocol, NULL, NULL, NULL, imbe_fr, ambe_fr, imbe7100_fr);
}

void
dsd_mbe_p25p1_skip_voice_frame(dsd_state* state) {
    if (state == NULL) {
        return;
    }
    // The key a decoded frame would have selected, so a keystream this frame generates is the call's.
    mbe_activate_keyloader_slot(state, NULL);
    mbe_p25p1_advance_keystream(state);
}
