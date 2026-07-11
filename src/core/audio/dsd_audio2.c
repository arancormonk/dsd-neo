// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 *
 * Float-path audio processing helpers and playback mixers
 * (DMR stereo variants and utilities)
 *
 * LWVMOBILE
 * 2023-10 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/p25_p2_audio_ring.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <math.h>
#include <mbelib.h>
#include <sndfile.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd_audio2_internal.h"

static void
write_s16_audio(dsd_opts* opts, const int16_t* buf, size_t frames) {
    if (opts->audio_out_stream) {
        dsd_audio_write(opts->audio_out_stream, buf, frames);
    }
}

static void
dsd_audio2_write_wav_short_block(SNDFILE* file, const short* samples, sf_count_t sample_count, const char* context) {
    if (file == NULL || samples == NULL || sample_count <= 0) {
        return;
    }
    sf_count_t written = sf_write_short(file, samples, sample_count);
    if (written != sample_count) {
        LOG_WARN("%s: wrote %lld/%lld samples to WAV output\n", context, (long long)written, (long long)sample_count);
    }
}

/* Convert float audio to int16 and write using the abstraction layer */
static void
write_float_audio(dsd_opts* opts, const float* buf, size_t frames) {
    if (!opts->audio_out_stream || !buf) {
        return;
    }
    /* Convert float [-1.0, 1.0] to int16 with clipping */
    int channels = opts->pulse_digi_out_channels;
    size_t total_samples = frames * (size_t)channels;
    int16_t tmp[320 * 2]; /* Max 320 stereo frames */
    if (total_samples > sizeof(tmp) / sizeof(tmp[0])) {
        total_samples = sizeof(tmp) / sizeof(tmp[0]);
    }
    for (size_t i = 0; i < total_samples; i++) {
        float v = buf[i] * 32767.0f;
        if (v > 32767.0f) {
            v = 32767.0f;
        } else if (v < -32768.0f) {
            v = -32768.0f;
        }
        tmp[i] = (int16_t)v;
    }
    dsd_audio_write(opts->audio_out_stream, tmp, frames);
}

// Return 1 if all elements are effectively zero (|x| < 1e-12f)
static inline int
dsd_is_all_zero_f(const float* buf, size_t n) {
    if (!buf) {
        return 1;
    }
    const float eps = 1e-12f;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] > eps || buf[i] < -eps) {
            return 0;
        }
    }
    return 1;
}

static inline void
write_audio_out(int fd, const void* buf, size_t bytes) {
    const ssize_t written = dsd_write(fd, buf, bytes);
    (void)written;
}

DSD_AUDIO2_INTERNAL int
dsd_is_all_zero_s16(const short* buf, size_t n) {
    if (!buf) {
        return 1;
    }
    for (size_t i = 0; i < n; i++) {
        if (buf[i] != 0) {
            return 0;
        }
    }
    return 1;
}

DSD_AUDIO2_INTERNAL void
dsd_audio_maybe_reset_output_ring_left(dsd_state* state) {
    if (state->audio_out_idx2 >= 800000) {
        state->audio_out_float_buf_p = state->audio_out_float_buf + 100;
        state->audio_out_buf_p = state->audio_out_buf + 100;
        DSD_MEMSET(state->audio_out_float_buf, 0, 100 * sizeof(float));
        DSD_MEMSET(state->audio_out_buf, 0, 100 * sizeof(short));
        state->audio_out_idx2 = 0;
    }
}

DSD_AUDIO2_INTERNAL void
dsd_audio_maybe_reset_output_ring_right(dsd_state* state) {
    if (state->audio_out_idx2R >= 800000) {
        state->audio_out_float_buf_pR = state->audio_out_float_bufR + 100;
        state->audio_out_buf_pR = state->audio_out_bufR + 100;
        DSD_MEMSET(state->audio_out_float_bufR, 0, 100 * sizeof(float));
        DSD_MEMSET(state->audio_out_bufR, 0, 100 * sizeof(short));
        state->audio_out_idx2R = 0;
    }
}

static void
dsd_audio_reset_float_mix_working_state(dsd_state* state) {
    DSD_MEMSET(state->audio_out_temp_buf, 0.0f, sizeof(state->audio_out_temp_buf));
    DSD_MEMSET(state->audio_out_temp_bufR, 0.0f, sizeof(state->audio_out_temp_bufR));
    DSD_MEMSET(state->f_l4, 0.0f, sizeof(state->f_l4));
    DSD_MEMSET(state->f_r4, 0.0f, sizeof(state->f_r4));
    state->audio_out_idx = 0;
    state->audio_out_idxR = 0;
    dsd_audio_maybe_reset_output_ring_left(state);
    dsd_audio_maybe_reset_output_ring_right(state);
}

static void
dsd_audio_reset_short_mono_left_working_state(dsd_state* state) {
    state->audio_out_idx = 0;
    DSD_MEMSET(state->s_l, 0, sizeof(state->s_l));
    DSD_MEMSET(state->audio_out_temp_buf, 0.0f, sizeof(state->audio_out_temp_buf));
    dsd_audio_maybe_reset_output_ring_left(state);
}

static void
dsd_audio_reset_short_stereo_working_state(dsd_state* state) {
    state->audio_out_idx = 0;
    state->audio_out_idxR = 0;
    DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
    DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
    dsd_audio_maybe_reset_output_ring_left(state);
    dsd_audio_maybe_reset_output_ring_right(state);
}

static void
dsd_audio_reset_short_lr_working_state(dsd_state* state) {
    state->audio_out_idx = 0;
    state->audio_out_idxR = 0;
    DSD_MEMSET(state->s_l, 0, sizeof(state->s_l));
    DSD_MEMSET(state->s_r, 0, sizeof(state->s_r));
    dsd_audio_maybe_reset_output_ring_left(state);
    dsd_audio_maybe_reset_output_ring_right(state);
}

DSD_AUDIO2_INTERNAL void
dsd_output_float_block(dsd_opts* opts, dsd_state* state, const float* samples, size_t frames, int channels) {
    if (opts->audio_out != 1 || !samples || frames == 0) {
        return;
    }
    if (opts->audio_out_type == 0) {
        write_float_audio(opts, samples, frames);
    } else if (opts->audio_out_type == 8) {
        dsd_udp_audio_hook_blast(opts, state, frames * (size_t)channels * sizeof(float), (void*)samples);
    } else if (opts->audio_out_type == 1) {
        write_audio_out(opts->audio_out_fd, samples, frames * (size_t)channels * sizeof(float));
    }
}

DSD_AUDIO2_INTERNAL void
dsd_output_s16_block(dsd_opts* opts, dsd_state* state, const short* samples, size_t frames, int channels) {
    if (opts->audio_out != 1 || !samples || frames == 0) {
        return;
    }
    if (opts->audio_out_type == 0) {
        write_s16_audio(opts, (const int16_t*)samples, frames);
    } else if (opts->audio_out_type == 8) {
        dsd_udp_audio_hook_blast(opts, state, frames * (size_t)channels * sizeof(short), (void*)samples);
    } else if (opts->audio_out_type == 1) {
        write_audio_out(opts->audio_out_fd, samples, frames * (size_t)channels * sizeof(short));
    }
}

DSD_AUDIO2_INTERNAL void
dsd_output_float_blocks(dsd_opts* opts, dsd_state* state, const float* const* blocks, size_t block_count, size_t frames,
                        int channels, int skip_silent) {
    size_t samples_per_block = frames * (size_t)channels;
    for (size_t i = 0; i < block_count; i++) {
        if (skip_silent && dsd_is_all_zero_f(blocks[i], samples_per_block)) {
            continue;
        }
        dsd_output_float_block(opts, state, blocks[i], frames, channels);
    }
}

DSD_AUDIO2_INTERNAL void
dsd_output_s16_blocks(dsd_opts* opts, dsd_state* state, const short* const* blocks, size_t block_count, size_t frames,
                      int channels, int skip_silent) {
    size_t samples_per_block = frames * (size_t)channels;
    for (size_t i = 0; i < block_count; i++) {
        if (skip_silent && dsd_is_all_zero_s16(blocks[i], samples_per_block)) {
            continue;
        }
        dsd_output_s16_block(opts, state, blocks[i], frames, channels);
    }
}

static void
dsd_load_short_mono_samples(short* dst, size_t len, const short* current_frame, short** history_ptr) {
    if (len == 160) {
        for (size_t j = 0; j < len; j++) {
            dst[j] = current_frame[j];
        }
    } else if (len == 960) {
        *history_ptr -= 960;
        for (size_t j = 0; j < len; j++) {
            dst[j] = **history_ptr;
            (*history_ptr)++;
        }
    }
}

static void
dsd_write_static_wav_from_mono(dsd_opts* opts, const short* mono_samp, size_t len) {
    if (opts->wav_out_f == NULL || opts->static_wav_file != 1) {
        return;
    }
    short ss[320];
    DSD_MEMSET(ss, 0, sizeof(ss));
    if (len == 160) {
        for (int i = 0; i < 160; i++) {
            ss[(i * 2) + 0] = mono_samp[i];
            ss[(i * 2) + 1] = mono_samp[i];
        }
    } else if (len == 960) {
        for (int i = 0; i < 160; i++) {
            ss[(i * 2) + 0] = mono_samp[(size_t)i * 6];
            ss[(i * 2) + 1] = mono_samp[(size_t)i * 6];
        }
    }
    dsd_audio2_write_wav_short_block(opts->wav_out_f, ss, 320, "dsd_write_static_wav_from_mono");
}

DSD_AUDIO2_INTERNAL int
dsd_p25_algid_is_encrypted(const dsd_state* state) {
    return DSD_SYNC_IS_P25P1(state->synctype) && state->payload_algid != 0 && state->payload_algid != 0x80;
}

DSD_AUDIO2_INTERNAL int
dsd_p25_algid_can_decrypt(const dsd_state* state) {
    int algid = state->payload_algid;
    if (algid == 0xAA || algid == 0x81 || algid == 0x9F) {
        return state->R != 0;
    }
    if (algid == 0x84 || algid == 0x89) {
        return state->aes_key_loaded[0] == 1;
    }
    return 0;
}

DSD_AUDIO2_INTERNAL int
dsd_nxdn_can_decrypt(const dsd_state* state) {
    if (state->nxdn_cipher_type == 0x1 || state->nxdn_cipher_type == 0x2) {
        return state->R != 0;
    }
    if (state->nxdn_cipher_type == 0x3) {
        return state->aes_key_loaded[0] == 1;
    }
    return 0;
}

static int
dsd_p25p1_live_crypto_gate_applies(const dsd_state* state) {
    return DSD_SYNC_IS_P25P1(state->synctype) && state->mbe_file_type != 3;
}

static int
dsd_fdma_crypto_muted(const dsd_state* state, int include_nxdn) {
    if (DSD_SYNC_IS_P25P1(state->synctype)) {
        return dsd_p25p1_live_crypto_gate_applies(state) && !p25_crypto_audio_ready(state, 0);
    }

    int muted = dsd_p25_algid_is_encrypted(state) || (include_nxdn && state->nxdn_cipher_type != 0);
    if (!muted) {
        return 0;
    }

    const int can_p25 = dsd_p25_algid_can_decrypt(state) || (state->payload_algid == 0x83 && state->R != 0);
    return (can_p25 || (include_nxdn && dsd_nxdn_can_decrypt(state))) ? 0 : 1;
}

static int
dsd_fdma_apply_group_gate(const dsd_opts* opts, const dsd_state* state, unsigned long tg, int muted) {
    (void)dsd_audio_group_gate_mono(opts, state, tg, muted, &muted);
    if (dsd_p25p1_live_crypto_gate_applies(state) && !p25_crypto_audio_ready(state, 0)) {
        return 1;
    }
    return muted;
}

static int
p25p2_s16_frames_have_audio(short frames[18][160]) {
    for (int j = 0; j < 18; j++) {
        for (int i = 0; i < 160; i++) {
            if (frames[j][i] != 0) {
                return 1;
            }
        }
    }
    return 0;
}

static inline int
dmr_forced_privacy_unmute_enabled(const dsd_state* state) {
    return state && ((state->baofeng_ap == 1) || (state->csi_ee == 1));
}

DSD_AUDIO2_INTERNAL void
dsd_dmr_init_slot_mute_flags(const dsd_opts* opts, const dsd_state* state, int* encL, int* encR) {
    const int forced_dmr_privacy = dmr_forced_privacy_unmute_enabled(state);
    int l_is_enc = state->dmr_encL != 0;
    int r_is_enc = state->dmr_encR != 0;
    *encL = (forced_dmr_privacy || !l_is_enc || opts->dmr_mute_encL == 0) ? 0 : 1;
    *encR = (forced_dmr_privacy || !r_is_enc || opts->dmr_mute_encR == 0) ? 0 : 1;
}

DSD_AUDIO2_INTERNAL void
dsd_duplicate_active_float_slot_to_stereo(float* a, float* b, float* c, int encL, int encR, int* outL, int* outR) {
    if (!encL && encR) {
        for (int i = 0; i < 320; i += 2) {
            a[i + 1] = a[i + 0];
            b[i + 1] = b[i + 0];
            c[i + 1] = c[i + 0];
        }
        *outR = 0;
    } else if (encL && !encR) {
        for (int i = 0; i < 320; i += 2) {
            a[i + 0] = a[i + 1];
            b[i + 0] = b[i + 1];
            c[i + 0] = c[i + 1];
        }
        *outL = 0;
    }
}

static void
dsd_set_p25p2_slot_mute_flags(const dsd_state* state, int* encL, int* encR) {
    *encL = state->p25_p2_audio_allowed[0] ? 0 : 1;
    *encR = state->p25_p2_audio_allowed[1] ? 0 : 1;
}

static void
dsd_apply_slot_hard_mute_flags(const dsd_opts* opts, int* encL, int* encR) {
    if (opts->slot1_on == 0) {
        *encL = 1;
    }
    if (opts->slot2_on == 0) {
        *encR = 1;
    }
}

static void
dsd_apply_dual_tg_audio_gate(const dsd_opts* opts, const dsd_state* state, int* encL, int* encR) {
    unsigned long TGL = (unsigned long)state->lasttg;
    unsigned long TGR = (unsigned long)state->lasttgR;
    (void)dsd_audio_group_gate_dual(opts, state, TGL, TGR, *encL, *encR, encL, encR);
    if (!p25_crypto_audio_ready(state, 0)) {
        *encL = 1;
    }
    if (!p25_crypto_audio_ready(state, 1)) {
        *encR = 1;
    }
}

DSD_AUDIO2_INTERNAL int
dsd_p25p2_encrypted_lockout_slot_muted(const dsd_opts* opts, const dsd_state* state, int slot, int muted) {
    if (!opts || !state || slot < 0 || slot > 1 || !muted) {
        return 0;
    }
    return p25_crypto_companion_suppressed(state, slot);
}

static void
dsd_duplicate_active_float_quad_to_stereo(const dsd_opts* opts, const dsd_state* state, float stereo[4][320], int* encL,
                                          int* encR) {
    if (!*encL && *encR) {
        if (dsd_p25p2_encrypted_lockout_slot_muted(opts, state, 1, *encR)) {
            return;
        }
        for (int j = 0; j < 4; j++) {
            for (int i = 0; i < 320; i += 2) {
                stereo[j][i + 1] = stereo[j][i + 0];
            }
        }
        *encR = 0;
        return;
    }
    if (*encL && !*encR) {
        if (dsd_p25p2_encrypted_lockout_slot_muted(opts, state, 0, *encL)) {
            return;
        }
        for (int j = 0; j < 4; j++) {
            for (int i = 0; i < 320; i += 2) {
                stereo[j][i + 0] = stereo[j][i + 1];
            }
        }
        *encL = 0;
    }
}

static void
dsd_output_float_first_two_then_nonzero(dsd_opts* opts, dsd_state* state, const float* block0, const float* block1,
                                        const float* block2, const float* block3, size_t frames, int channels) {
    const float* first_two[] = {block0, block1};
    const float* trailing[] = {block2, block3};
    dsd_output_float_blocks(opts, state, first_two, 2, frames, channels, 0);
    dsd_output_float_blocks(opts, state, trailing, 2, frames, channels, 1);
}

static void
dsd_fs4_pop_gain_frames(const dsd_opts* opts, dsd_state* state, float lf[4][160], float rf[4][160], int l_ok[4],
                        int r_ok[4]) {
    for (int j = 0; j < 4; j++) {
        l_ok[j] = p25_p2_audio_ring_pop(state, 0, lf[j]);
        r_ok[j] = p25_p2_audio_ring_pop(state, 1, rf[j]);
        if (l_ok[j]) {
            agf(opts, state, lf[j], 0);
        }
        if (r_ok[j]) {
            agf(opts, state, rf[j], 1);
        }
    }
}

static void
dsd_fs4_mix_interleaved_frames(float lf[4][160], float rf[4][160], int encL, int encR, int l_ok[4], int r_ok[4],
                               float stereo[4][320]) {
    for (int j = 0; j < 4; j++) {
        int l_muted = (encL || !l_ok[j]) ? 1 : 0;
        int r_muted = (encR || !r_ok[j]) ? 1 : 0;
        audio_mix_interleave_stereo_f32(lf[j], rf[j], 160, l_muted, r_muted, stereo[j]);
    }
}

DSD_AUDIO2_INTERNAL void
dsd_fs4_mix_mono_frames(float lf[4][160], float rf[4][160], int encL, int encR, int l_ok[4], int r_ok[4],
                        float mono[4][160]) {
    for (int j = 0; j < 4; j++) {
        int l_on = (!encL && l_ok[j]) ? 1 : 0;
        int r_on = (!encR && r_ok[j]) ? 1 : 0;
        audio_mix_mono_from_slots_f32(lf[j], rf[j], 160, l_on, r_on, mono[j]);
    }
}

static void
dsd_hpf_short_triplet_if_enabled(const dsd_opts* opts, dsd_state* state) {
    if (opts->use_hpf_d != 1) {
        return;
    }
    for (int j = 0; j < 3; j++) {
        hpf_dL(state, state->s_l4[j], 160);
        hpf_dR(state, state->s_r4[j], 160);
    }
}

static void
dsd_hpf_short_18_if_enabled(const dsd_opts* opts, dsd_state* state) {
    if (opts->use_hpf_d != 1) {
        return;
    }
    for (int j = 0; j < 18; j++) {
        hpf_dL(state, state->s_l4[j], 160);
        hpf_dR(state, state->s_r4[j], 160);
    }
}

static void
dsd_interleave_s16_18_blocks(const dsd_state* state, short stereo_sf[18][320]) {
    for (int j = 0; j < 18; j++) {
        audio_mix_interleave_stereo_s16(state->s_l4[j], state->s_r4[j], 160, 0, 0, stereo_sf[j]);
    }
}

static void
dsd_output_s16_18_blocks(dsd_opts* opts, dsd_state* state, short stereo_sf[18][320]) {
    if (opts->audio_out != 1) {
        return;
    }
    for (int j = 0; j < 18; j++) {
        dsd_output_s16_block(opts, state, stereo_sf[j], 160, 2);
    }
}

static void
dsd_write_s16_wav_18_blocks(dsd_opts* opts, short stereo_sf[18][320]) {
    if (opts->wav_out_f == NULL || opts->static_wav_file != 1) {
        return;
    }
    for (int j = 0; j < 18; j++) {
        dsd_audio2_write_wav_short_block(opts->wav_out_f, stereo_sf[j], 320, "dsd_write_s16_wav_18_blocks");
    }
}

DSD_AUDIO2_INTERNAL void
dsd_dmr_ss3_init_enc_flags(const dsd_state* state, int* encL, int* encR) {
    *encL = (state->dmr_so >> 6) & 0x1;
    *encR = (state->dmr_soR >> 6) & 0x1;
    const int forced_dmr_privacy = dmr_forced_privacy_unmute_enabled(state);

    if (*encL) {
        const int can_decrypt =
            forced_dmr_privacy
            || ((state->payload_algid == 0) ? dsd_dmr_missing_alg_key_can_decrypt(state, 0)
                                            : dsd_dmr_voice_slot_can_decrypt(state, 0, state->payload_algid, state->R));
        if (can_decrypt) {
            *encL = 0;
        }
    }
    if (*encR) {
        const int can_decrypt = forced_dmr_privacy
                                || ((state->payload_algidR == 0)
                                        ? dsd_dmr_missing_alg_key_can_decrypt(state, 1)
                                        : dsd_dmr_voice_slot_can_decrypt(state, 1, state->payload_algidR, state->RR));
        if (can_decrypt) {
            *encR = 0;
        }
    }
}

DSD_AUDIO2_INTERNAL void
dsd_dmr_apply_tg_hold_and_slot_preference_ss3(dsd_opts* opts, const dsd_state* state, unsigned long TGL,
                                              unsigned long TGR, int* encL, int* encR) {
    if (state->tg_hold != 0 && state->tg_hold != TGL) {
        *encL = 1;
    }
    if (state->tg_hold != 0 && state->tg_hold != TGR) {
        *encR = 1;
    }
    if (state->tg_hold != 0 && state->tg_hold == TGL) {
        *encL = 0;
        opts->slot1_on = 1;
        opts->slot_preference = 0;
    } else if (state->tg_hold != 0 && state->tg_hold == TGR) {
        *encR = 0;
        opts->slot2_on = 1;
        opts->slot_preference = 1;
    } else {
        opts->slot_preference = 2;
    }
}

static int
dsd_ss3_should_copy_right_to_left(const dsd_opts* opts, const dsd_state* state, int encR) {
    if (encR != 0) {
        return 0;
    }
    if (opts->slot1_on == 0 && opts->slot2_on == 1) {
        return 1;
    }
    if (opts->slot_preference == 1 && state->dmrburstR == 16) {
        return 1;
    }
    if (state->dmrburstR == 16 && state->dmrburstL != 16) {
        return 1;
    }
    return 0;
}

static int
dsd_ss3_should_copy_left_to_right(const dsd_opts* opts, const dsd_state* state, int encL) {
    if (encL != 0) {
        return 0;
    }
    if (opts->slot1_on == 1 && opts->slot2_on == 0) {
        return 1;
    }
    if (opts->slot_preference == 0 && state->dmrburstL == 16) {
        return 1;
    }
    if (state->dmrburstL == 16 && state->dmrburstR != 16) {
        return 1;
    }
    return 0;
}

DSD_AUDIO2_INTERNAL void
dsd_dmr_apply_stereo_output_policy_ss3(const dsd_opts* opts, dsd_state* state, int encL, int encR) {
    if (encL) {
        DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
    }
    if (encR) {
        DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
    }
    if (dsd_ss3_should_copy_right_to_left(opts, state, encR)) {
        DSD_MEMCPY(state->s_l4, state->s_r4, sizeof(state->s_l4));
    } else if (dsd_ss3_should_copy_left_to_right(opts, state, encL)) {
        DSD_MEMCPY(state->s_r4, state->s_l4, sizeof(state->s_r4));
    }
}

DSD_AUDIO2_INTERNAL void
dsd_p25p2_apply_slot_preference_ss18(dsd_opts* opts, const dsd_state* state, unsigned long TGL, unsigned long TGR) {
    if (state->tg_hold != 0 && state->tg_hold == TGL) {
        opts->slot1_on = 1;
        opts->slot_preference = 0;
    } else if (state->tg_hold != 0 && state->tg_hold == TGR) {
        opts->slot2_on = 1;
        opts->slot_preference = 1;
    } else {
        opts->slot_preference = 2;
    }
}

static int
dsd_ss18_should_copy_right_to_left(const dsd_opts* opts, const dsd_state* state, int encL, int encR) {
    if (encR != 0) {
        return 0;
    }
    if (dsd_p25p2_encrypted_lockout_slot_muted(opts, state, 0, encL)) {
        return 0;
    }
    if (opts->slot1_on == 0 && opts->slot2_on == 1) {
        return 1;
    }
    if (opts->slot_preference == 1 && state->dmrburstR == 21) {
        return 1;
    }
    if (state->dmrburstR == 21 && state->dmrburstL != 21) {
        return 1;
    }
    return 0;
}

static int
dsd_ss18_should_copy_left_to_right(const dsd_opts* opts, const dsd_state* state, int encL, int encR) {
    if (encL != 0) {
        return 0;
    }
    if (dsd_p25p2_encrypted_lockout_slot_muted(opts, state, 1, encR)) {
        return 0;
    }
    if (opts->slot1_on == 1 && opts->slot2_on == 0) {
        return 1;
    }
    if (opts->slot_preference == 0 && state->dmrburstL == 21) {
        return 1;
    }
    if (state->dmrburstL == 21 && state->dmrburstR != 21) {
        return 1;
    }
    return 0;
}

DSD_AUDIO2_INTERNAL void
dsd_p25p2_apply_stereo_output_policy_ss18(const dsd_opts* opts, dsd_state* state, int encL, int encR) {
    if (encL) {
        DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
    }
    if (encR) {
        DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
    }
    if (dsd_ss18_should_copy_right_to_left(opts, state, encL, encR)) {
        DSD_MEMCPY(state->s_l4, state->s_r4, sizeof(state->s_l4));
    } else if (dsd_ss18_should_copy_left_to_right(opts, state, encL, encR)) {
        DSD_MEMCPY(state->s_r4, state->s_l4, sizeof(state->s_r4));
    }
}

void
dsd_p25p2_flush_partial_audio(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    // This helper is specifically for the short/int16 P25p2 SS18 path.
    if (opts->floating_point != 0 || opts->pulse_digi_rate_out != 8000) {
        return;
    }

    int has_l = p25p2_s16_frames_have_audio(state->s_l4);
    int has_r = p25p2_s16_frames_have_audio(state->s_r4);
    if (!(has_l || has_r)) {
        return;
    }

    // The SS18 mixer uses p25_p2_audio_allowed as its per-slot gate.
    // At release, MAC_END/IDLE may already have cleared the gate; the
    // s_l4/s_r4 buffers only contain decoded audio when a slot was allowed at
    // decode time, so gate playback based on actual buffered audio presence.
    state->p25_p2_audio_allowed[0] = has_l ? 1 : 0;
    state->p25_p2_audio_allowed[1] = has_r ? 1 : 0;

    playSynthesizedVoiceSS18(opts, state);
    state->voice_counter[0] = 0;
    state->voice_counter[1] = 0;
}

void
dsd_p25p2_flush_partial_audio_slot(dsd_opts* opts, dsd_state* state, int slot) {
    if (!opts || !state || slot < 0 || slot > 1) {
        return;
    }
    // This helper is specifically for the short/int16 P25p2 SS18 path.
    if (opts->floating_point != 0 || opts->pulse_digi_rate_out != 8000) {
        return;
    }

    const int other = slot ^ 1;
    int has_slot = (slot == 0) ? p25p2_s16_frames_have_audio(state->s_l4) : p25p2_s16_frames_have_audio(state->s_r4);
    if (!has_slot) {
        return;
    }

    short saved_other[18][160];
    int saved_other_counter = state->voice_counter[other];
    int saved_other_allowed = state->p25_p2_audio_allowed[other];

    if (other == 0) {
        DSD_MEMCPY(saved_other, state->s_l4, sizeof(saved_other));
        DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
    } else {
        DSD_MEMCPY(saved_other, state->s_r4, sizeof(saved_other));
        DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
    }

    state->p25_p2_audio_allowed[slot] = 1;
    state->p25_p2_audio_allowed[other] = 0;

    playSynthesizedVoiceSS18(opts, state);

    state->voice_counter[slot] = 0;
    state->voice_counter[other] = saved_other_counter;
    state->p25_p2_audio_allowed[other] = saved_other_allowed;
    if (other == 0) {
        DSD_MEMCPY(state->s_l4, saved_other, sizeof(saved_other));
    } else {
        DSD_MEMCPY(state->s_r4, saved_other, sizeof(saved_other));
    }
}

//NOTE: Tones produce ringing sound when put through the hpf_d, may want to look into tweaking it,
//or looking for a way to store is_tone by glancing at ambe_d values and not running hpf_d on them

//TODO: WAV File saving (works fine on shorts, but on float, writing short to wav is not auto-gained,
//so super quiet, either convert to float wav files, or run processAudio AFTER memcpy of the temp_buf)

//     //user gain factor
//     samp[i] *= gain;

//float stereo mix 3v2 DMR
void
playSynthesizedVoiceFS3(dsd_opts* opts, dsd_state* state) {

    //NOTE: This runs once for every two timeslots, if we are in the BS voice loop
    //it doesn't matter if both slots have voice, or if one does, the slot without voice
    //will play silence while this runs if no voice present

    int encL, encR;
    float stereo_samp1[320]; //8k 2-channel stereo interleave mix
    float stereo_samp2[320]; //8k 2-channel stereo interleave mix
    float stereo_samp3[320]; //8k 2-channel stereo interleave mix

    DSD_MEMSET(stereo_samp1, 0.0f, sizeof(stereo_samp1));
    DSD_MEMSET(stereo_samp2, 0.0f, sizeof(stereo_samp2));
    DSD_MEMSET(stereo_samp3, 0.0f, sizeof(stereo_samp3));

    //TODO: add option to bypass enc with a toggle as well

    // DMR per-slot ENC gating: derive from decoder-side flags and user policy.
    dsd_dmr_init_slot_mute_flags(opts, state, &encL, &encR);

    //CHEAT: Using the slot on/off, use that to set encL or encR back on
    //as a simple way to turn off voice synthesis in a particular slot
    //its not really 'disabled', we just aren't playing it
    if (opts->slot1_on == 0) {
        encL = 1;
    }
    if (opts->slot2_on == 0) {
        encR = 1;
    }

    unsigned long TGL = (unsigned long)state->lasttg;
    unsigned long TGR = (unsigned long)state->lasttgR;

    // Apply whitelist/TG-hold gating shared with other mixers.
    (void)dsd_audio_group_gate_dual(opts, state, TGL, TGR, encL, encR, &encL, &encR);

    //run autogain on the f_ buffers
    agf(opts, state, state->f_l4[0], 0);
    agf(opts, state, state->f_r4[0], 1);
    agf(opts, state, state->f_l4[1], 0);
    agf(opts, state, state->f_r4[1], 1);
    agf(opts, state, state->f_l4[2], 0);
    agf(opts, state, state->f_r4[2], 1);

    //interleave left and right channels from the temp (float) buffer
    audio_mix_interleave_stereo_f32(state->f_l4[0], state->f_r4[0], 160, encL, encR, stereo_samp1);
    audio_mix_interleave_stereo_f32(state->f_l4[1], state->f_r4[1], 160, encL, encR, stereo_samp2);
    audio_mix_interleave_stereo_f32(state->f_l4[2], state->f_r4[2], 160, encL, encR, stereo_samp3);

    //at this point, if both channels are still flagged as enc, then we can skip all playback/writing functions
    if (encL && encR) {
        goto FS3_END;
    }

    // If only one slot is active, duplicate to both channels for stereo sinks.
    dsd_duplicate_active_float_slot_to_stereo(stereo_samp1, stereo_samp2, stereo_samp3, encL, encR, &encL, &encR);

    if (opts->pulse_digi_out_channels == 1) {
        float mono1[160], mono2[160], mono3[160];
        DSD_MEMSET(mono1, 0, sizeof(mono1));
        DSD_MEMSET(mono2, 0, sizeof(mono2));
        DSD_MEMSET(mono3, 0, sizeof(mono3));
        int l_on = !encL;
        int r_on = !encR;
        audio_mix_mono_from_slots_f32(state->f_l4[0], state->f_r4[0], 160, l_on, r_on, mono1);
        audio_mix_mono_from_slots_f32(state->f_l4[1], state->f_r4[1], 160, l_on, r_on, mono2);
        audio_mix_mono_from_slots_f32(state->f_l4[2], state->f_r4[2], 160, l_on, r_on, mono3);
        const float* mono_blocks[] = {mono1, mono2, mono3};
        dsd_output_float_blocks(opts, state, mono_blocks, 3, 160, 1, 0);
    } else {
        const float* stereo_blocks[] = {stereo_samp1, stereo_samp2, stereo_samp3};
        dsd_output_float_blocks(opts, state, stereo_blocks, 3, 160, 2, 0);
    }

FS3_END:
    dsd_audio_reset_float_mix_working_state(state);
}

//NOTE: On FS4 and SS4 voice, the longer the transmission, the more the function will start to lag
//the entire DSD-neo loop due to the skipping of playback on SACCH frames (causes noticeable skip when it does play them),
//this isn't a major problem, since the buffer can handle it, but it does delay return to CC until the end
//of the call on busy systems where both VCH slots are constantly busy with voice
//the longer the call, the more delayed until returning to the control channel

//NOTE: Disabling voice synthesis clears up the delay issue (obviosly since we aren't having to wait on it to play)
//disabling voice in only one slot will also fix most random stutter from the 4v in one slot, and 2v in the other slot

//NOTE: The same skip may be occurring on the main and v2.1b branches of DSD-neo as well, so that may be due to the 4v/2v and
//playing back immediately instead of buffering x number of samples or 4v/2v to get a smoother playback

//NOTE: When using capture bins for playback, this issue is not as observable compared to real time reception due to how fast
//we can blow through pure data on bin files compared to waiting for the real time reception

//its usually a lot more noticeable on dual voices than single (probably due to various arrangements of dual 4v/2v in each superframe)

//float stereo mix 4v2 P25p2
void
playSynthesizedVoiceFS4(dsd_opts* opts, dsd_state* state) {

    //NOTE: This will run for every TS % 2, except on SACCH inverted slots (10 and 11)
    //WIP: Get the real TS number out of the P25p2 frame, and not our ts_counter values

    int encL, encR;
    float stereo[4][320];
    DSD_MEMSET(stereo, 0.0f, sizeof(stereo));

    float lf[4][160];
    float rf[4][160];
    int l_ok[4] = {0, 0, 0, 0};
    int r_ok[4] = {0, 0, 0, 0};

    dsd_set_p25p2_slot_mute_flags(state, &encL, &encR);
    dsd_apply_slot_hard_mute_flags(opts, &encL, &encR);
    dsd_apply_dual_tg_audio_gate(opts, state, &encL, &encR);

    dsd_fs4_pop_gain_frames(opts, state, lf, rf, l_ok, r_ok);
    dsd_fs4_mix_interleaved_frames(lf, rf, encL, encR, l_ok, r_ok, stereo);
    dsd_duplicate_active_float_quad_to_stereo(opts, state, stereo, &encL, &encR);

    if (encL && encR) {
        goto END_FS4;
    }

    // If output is mono, mix active channels into one buffer per frame span
    if (opts->pulse_digi_out_channels == 1) {
        float mono[4][160];
        DSD_MEMSET(mono, 0.0f, sizeof(mono));
        dsd_fs4_mix_mono_frames(lf, rf, encL, encR, l_ok, r_ok, mono);
        dsd_output_float_first_two_then_nonzero(opts, state, mono[0], mono[1], mono[2], mono[3], 160, 1);
        goto END_FS4;
    }

    // Stereo output (2ch)
    dsd_output_float_first_two_then_nonzero(opts, state, stereo[0], stereo[1], stereo[2], stereo[3], 160, 2);

END_FS4:
    dsd_audio_reset_float_mix_working_state(state);
}

//float stereo mix -- when using Float Stereo Output, we need to send P25p1, DMR MS/Simplex, DStar, and YSF here
void
playSynthesizedVoiceFS(dsd_opts* opts, dsd_state* state) {
    const int is_p25p1 = DSD_SYNC_IS_P25P1(state->synctype);
    int encL = is_p25p1 ? (p25_crypto_audio_ready(state, 0) ? 0 : 1) : (dsd_p25_algid_is_encrypted(state) ? 1 : 0);
    float stereo_samp1[320]; //8k 2-channel stereo interleave mix

    DSD_MEMSET(stereo_samp1, 0.0f, sizeof(stereo_samp1));
    if (!is_p25p1 && encL
        && (dsd_p25_algid_can_decrypt(state) || (state->payload_algid == 0x83 && state->aes_key_loaded[0] == 1))) {
        encL = 0;
    }

    if (opts->slot1_on == 0) {
        encL = 1;
    }

    unsigned long TGL = (unsigned long)state->lasttg;
    (void)dsd_audio_group_gate_mono(opts, state, TGL, encL, &encL);
    if (is_p25p1 && !p25_crypto_audio_ready(state, 0)) {
        encL = 1;
    }

    agf(opts, state, state->f_l, 0);
    if (!encL) {
        audio_mono_to_stereo_f32(state->f_l, stereo_samp1, 160);
        audio_apply_gain_f32(stereo_samp1, 320, 0.5f);
        dsd_output_float_block(opts, state, stereo_samp1, 160, 2);
    }
    dsd_audio_reset_float_mix_working_state(state);
}

void
playSynthesizedVoiceFM(dsd_opts* opts, dsd_state* state) {
    agf(opts, state, state->f_l, 0);
    int encL = dsd_fdma_crypto_muted(state, 1);

    unsigned long TGL = (unsigned long)state->lasttg;
    if (opts->frame_nxdn48 == 1 || opts->frame_nxdn96 == 1) {
        TGL = (unsigned long)state->nxdn_last_tg;
    }

    encL = dsd_fdma_apply_group_gate(opts, state, TGL, encL);

    if (!encL && opts->slot1_on != 0) {
        dsd_output_float_block(opts, state, state->f_l, 160, 1);
    }
    dsd_audio_maybe_reset_output_ring_left(state);
    DSD_MEMSET(state->f_l, 0.0f, sizeof(state->f_l));
    DSD_MEMSET(state->audio_out_temp_buf, 0.0f, sizeof(state->audio_out_temp_buf));
}

//Mono - Short (SB16LE) - Drop-in replacement for playSyntesizedVoice, but easier to manipulate
void
playSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state) {
    size_t len = state->audio_out_idx;
    if (len > 960) {
        len = 960; // clamp to buffer capacity
    }

    short mono_samp_buf[960];
    short* mono_samp = mono_samp_buf;
    DSD_MEMSET(mono_samp, 0, len * sizeof(short));

    if (opts->slot1_on != 0) {
        dsd_load_short_mono_samples(mono_samp, len, state->s_l, &state->audio_out_buf_p);
        if (opts->use_hpf_d == 1) {
            hpf_dL(state, mono_samp, (int)len);
        }
        dsd_output_s16_block(opts, state, mono_samp, len, 1);
        dsd_write_static_wav_from_mono(opts, mono_samp, len);
    }
    dsd_audio_reset_short_mono_left_working_state(state);
}

//Stereo Mix - Short (SB16LE) -- When Playing Short FDMA samples when setup for stereo output
void
playSynthesizedVoiceSS(dsd_opts* opts, dsd_state* state) {
    int encL = dsd_fdma_crypto_muted(state, 0);
    short stereo_samp1[320]; //8k 2-channel stereo interleave mix
    DSD_MEMSET(stereo_samp1, 0, sizeof(stereo_samp1));

    if (opts->slot1_on == 0) {
        encL = 1;
    }

    unsigned long TGL = (unsigned long)state->lasttg;

    encL = dsd_fdma_apply_group_gate(opts, state, TGL, encL);

    if (opts->use_hpf_d == 1) {
        hpf_dL(state, state->s_l, 160);
    }
    audio_mono_to_stereo_s16(state->s_l, stereo_samp1, 160);
    if (!encL) {
        dsd_output_s16_block(opts, state, stereo_samp1, 160, 2);
        if (opts->wav_out_f != NULL && opts->static_wav_file == 1) {
            dsd_audio2_write_wav_short_block(opts->wav_out_f, stereo_samp1, 320, "processAudioDMRslot");
        }
    }
    dsd_audio_reset_short_lr_working_state(state);
}

//short stereo mix 3v2 DMR
void
playSynthesizedVoiceSS3(dsd_opts* opts, dsd_state* state) {

    //NOTE: This runs once for every two timeslots, if we are in the BS voice loop
    //it doesn't matter if both slots have voice, or if one does, the slot without voice
    //will play silence while this runs if no voice present

    int encL, encR;
    short stereo_samp1[320]; //8k 2-channel stereo interleave mix
    short stereo_samp2[320]; //8k 2-channel stereo interleave mix
    short stereo_samp3[320]; //8k 2-channel stereo interleave mix

    DSD_MEMSET(stereo_samp1, 0, sizeof(stereo_samp1));
    DSD_MEMSET(stereo_samp2, 0, sizeof(stereo_samp2));
    DSD_MEMSET(stereo_samp3, 0, sizeof(stereo_samp3));

    dsd_dmr_ss3_init_enc_flags(state, &encL, &encR);

    unsigned long TGL = (unsigned long)state->lasttg;
    unsigned long TGR = (unsigned long)state->lasttgR;

    (void)dsd_audio_group_gate_dual(opts, state, TGL, TGR, encL, encR, &encL, &encR);

    dsd_dmr_apply_tg_hold_and_slot_preference_ss3(opts, state, TGL, TGR, &encL, &encR);
    dsd_hpf_short_triplet_if_enabled(opts, state);
    dsd_dmr_apply_stereo_output_policy_ss3(opts, state, encL, encR);

    //check this last
    if (opts->slot1_on == 0 && opts->slot2_on == 0) //both slots are hard off, disable playback
    {
        encL = 1;
        encR = 1;
    }

    //at this point, if both channels are still flagged as enc, then we can skip all playback/writing functions
    if (encL && encR) {
        goto SS3_END;
    }

    audio_mix_interleave_stereo_s16(state->s_l4[0], state->s_r4[0], 160, 0, 0, stereo_samp1);
    audio_mix_interleave_stereo_s16(state->s_l4[1], state->s_r4[1], 160, 0, 0, stereo_samp2);
    audio_mix_interleave_stereo_s16(state->s_l4[2], state->s_r4[2], 160, 0, 0, stereo_samp3);

    const short* stereo_blocks[] = {stereo_samp1, stereo_samp2, stereo_samp3};
    dsd_output_s16_blocks(opts, state, stereo_blocks, 3, 160, 2, 0);

    if (opts->wav_out_f != NULL && opts->static_wav_file == 1) {
        dsd_audio2_write_wav_short_block(opts->wav_out_f, stereo_samp1, 320, "processAudioDMRstereo3v2 block1");
        dsd_audio2_write_wav_short_block(opts->wav_out_f, stereo_samp2, 320, "processAudioDMRstereo3v2 block2");
        dsd_audio2_write_wav_short_block(opts->wav_out_f, stereo_samp3, 320, "processAudioDMRstereo3v2 block3");
    }

SS3_END:
    dsd_audio_reset_short_stereo_working_state(state);
}

//short stereo mix 18v superframe
void
playSynthesizedVoiceSS18(dsd_opts* opts, dsd_state* state) {

    //NOTE: This will run once every superframe during a sacch field
    //exact implementation to be determined

    int encL, encR;

    short stereo_sf[18][320]; //8k 2-channel stereo interleave mix for full superframe
    DSD_MEMSET(stereo_sf, 0, sizeof(stereo_sf));

    // Per-slot audio gating (P25p2): start from per-slot allowed flags,
    // then apply whitelist/TG-hold rules shared with other mixers.
    dsd_set_p25p2_slot_mute_flags(state, &encL, &encR);

    unsigned long TGL = (unsigned long)state->lasttg;
    unsigned long TGR = (unsigned long)state->lasttgR;

    (void)dsd_audio_group_gate_dual(opts, state, TGL, TGR, encL, encR, &encL, &encR);
    if (!p25_crypto_audio_ready(state, 0)) {
        encL = 1;
    }
    if (!p25_crypto_audio_ready(state, 1)) {
        encR = 1;
    }

    dsd_p25p2_apply_slot_preference_ss18(opts, state, TGL, TGR);
    dsd_hpf_short_18_if_enabled(opts, state);
    dsd_p25p2_apply_stereo_output_policy_ss18(opts, state, encL, encR);

    //check this last
    if (opts->slot1_on == 0 && opts->slot2_on == 0) //both slots are hard off, disable playback
    {
        encL = 1;
        encR = 1;
    }

    //at this point, if both channels are still flagged as enc, then we can skip all playback/writing functions
    if (encL && encR) {
        goto SS18_END;
    }

    dsd_interleave_s16_18_blocks(state, stereo_sf);
    dsd_output_s16_18_blocks(opts, state, stereo_sf);
    dsd_write_s16_wav_18_blocks(opts, stereo_sf);

SS18_END:
    dsd_audio_reset_short_stereo_working_state(state);
}

//largely borrowed from Boatbod OP25 (simplified single tone ID version)
static void
soft_tonef(float samp[160], int n, int ID, int AD) {
    int i;
    double step1, step2, amplitude, freq1, freq2;

    float gain = 1.0f;

    // Synthesize tones
    freq1 = 31.25 * (double)ID;
    freq2 = freq1;
    step1 = 2.0 * M_PI * freq1 / 8000.0;
    step2 = 2.0 * M_PI * freq2 / 8000.0;
    amplitude = (double)AD * 75.0;

    for (i = 0; i < 160; i++) {
        samp[i] = (float)(amplitude * (sin((double)n * step1) * 0.5 + sin((double)n * step2) * 0.5));
        samp[i] /= 8000.0f;
        samp[i] *= gain;
        n++;
    }
}

static void
dsd_beeper_output_samples(dsd_opts* opts, dsd_state* state, const float* samp_f, const float* samp_fs,
                          const short* samp_s, const short* samp_ss) {
    if (opts->floating_point == 1) {
        if (opts->pulse_digi_out_channels == 2) {
            dsd_output_float_block(opts, state, samp_fs, 160, 2);
        } else {
            dsd_output_float_block(opts, state, samp_f, 160, 1);
        }
        return;
    }
    if (opts->pulse_digi_out_channels == 2) {
        dsd_output_s16_block(opts, state, samp_ss, 160, 2);
    } else {
        dsd_output_s16_block(opts, state, samp_s, 160, 1);
    }
}

void
beeper(dsd_opts* opts, dsd_state* state, int lr, int id, int ad, int len) {
    UNUSED(state);
    int i, j, n;
    //use lr as left or right channel designation in stereo config
    float samp_f[160];  //mono float sample
    float samp_fs[320]; //stereo float sample
    short samp_s[160];  //mono short sample
    short samp_ss[320]; //stereo short sample

    n = 0; //rolling sine wave 'degree'

    //double len if not using Pulse Audio,
    //anything over UDP may
    //not clear the buffer at the shorter len
    if (opts->audio_out_type != 0) {
        len *= 2;
    }

    //each j increment is 20 ms at 160 samples / 8 kHz
    for (j = 0; j < len; j++) {
        //'zero' out stereo mix samples
        DSD_MEMSET(samp_fs, 0, sizeof(samp_fs));
        DSD_MEMSET(samp_ss, 0, sizeof(samp_ss));

        //generate a tone with supplied tone ID and AD value
        soft_tonef(samp_f, n, id, ad);

        //convert float to short if required
        if (opts->floating_point == 0) {
            mbe_floattoshort(samp_f, samp_s);
            for (i = 0; i < 160; i++) {
                samp_s[i] *= 4000; //apply gain
                samp_ss[(i * 2) + lr] = samp_s[i];
            }
        }

        for (i = 0; i < 160; i++) {
            samp_fs[(i * 2) + lr] = samp_f[i];
        }

        dsd_beeper_output_samples(opts, state, samp_f, samp_fs, samp_s, samp_ss);
    }
}
