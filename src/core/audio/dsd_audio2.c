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
#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/key_presence.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25p2_playout.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/runtime/p25_p2_audio_ring.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <limits.h>
#include <math.h>
#include <mbelib-neo/mbelib.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd_audio2_internal.h"
#include "dsd_audio_internal.h"

static void
write_s16_audio(dsd_opts* opts, const int16_t* buf, size_t frames) {
    if (opts->audio_out_stream) {
        dsd_audio_write(opts->audio_out_stream, buf, frames);
    }
}

static unsigned long
dsd_audio_call_target(const dsd_state* state, uint8_t slot) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE) {
        return 0UL;
    }
    uint64_t target = DSD_SYNC_IS_P25(call.protocol)
                          ? call.ota_target_id
                          : (call.policy_target_id != 0U ? call.policy_target_id : call.ota_target_id);
    return target <= ULONG_MAX ? (unsigned long)target : 0UL;
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

void
dsd_audio_maybe_reset_output_ring_left(dsd_state* state) {
    if (state->audio_out_idx2 >= 800000) {
        state->audio_out_float_buf_p = state->audio_out_float_buf + 100;
        state->audio_out_buf_p = state->audio_out_buf + 100;
        DSD_MEMSET(state->audio_out_float_buf, 0, 100 * sizeof(float));
        DSD_MEMSET(state->audio_out_buf, 0, 100 * sizeof(short));
        state->audio_out_idx2 = 0;
    }
}

void
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

void
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

void
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

// Returns 1 when it loaded a decoded frame (160 samples, or 960 upsampled), 0 when @p len holds none.
static int
dsd_load_short_mono_samples(short* dst, size_t len, const short* current_frame, short** history_ptr) {
    if (len == 160) {
        for (size_t j = 0; j < len; j++) {
            dst[j] = current_frame[j];
        }
        return 1;
    }
    if (len == 960) {
        *history_ptr -= 960;
        for (size_t j = 0; j < len; j++) {
            dst[j] = **history_ptr;
            (*history_ptr)++;
        }
        return 1;
    }
    return 0;
}

// The static WAV (-w) is a recording: a slot whose talkgroup allows audio but
// not recording is heard but not written. Only the policy is asked; what is heard
// is already settled by the caller's mute flags.
static int
dsd_static_wav_slot_recordable(const dsd_opts* opts, const dsd_state* state, int slot) {
    int allow = 0;
    return dsd_audio_record_policy_gate_slot(opts, state, slot, &allow) == 0 && allow;
}

// X2-TDMA stages either timeslot in the mono buffer; every other mono source is slot 0.
static uint8_t
dsd_mono_source_slot(const dsd_state* state) {
    return (DSD_SYNC_IS_X2TDMA(state->synctype) && state->currentslot == 1) ? 1U : 0U;
}

// The slot (0 or 1) each channel of a stereo mix carries once its output policy has
// run: its own, or its companion when the policy copied that slot over it.
static void
dsd_stereo_channel_sources(int copy_right_to_left, int copy_left_to_right, int* source) {
    source[0] = copy_right_to_left ? 1 : 0;
    source[1] = (!copy_right_to_left && copy_left_to_right) ? 0 : 1;
}

// Whether a stereo mix emits fresh decoded media (the audible-audio stamp, issue
// #574): some channel carries a slot that is unmuted and has fresh media
// (@p fresh, per slot). A muted slot's channel is zeroed before the copy, and a
// mono sink mixes the same two buffers after it, so the routing decides for both.
static int
dsd_stereo_mix_carries_fresh(int encL, int encR, int copy_right_to_left, int copy_left_to_right, const int* fresh) {
    const int enc[2] = {encL, encR};
    int source[2];
    dsd_stereo_channel_sources(copy_right_to_left, copy_left_to_right, source);
    for (int ch = 0; ch < 2; ch++) {
        if (!enc[source[ch]] && fresh[source[ch]]) {
            return 1;
        }
    }
    return 0;
}

// The stereo static WAV holds what is heard minus calls whose talkgroup allows
// audio but not recording. Each channel carries the slot the output policy routed
// to it: itself, or its companion when the mix mirrors one slot over the other, so
// a slot the mix never played is never recorded either. Returns the channels to
// write silent (bit 0 left, bit 1 right), 0 when all is recordable, or -1 when no
// recordable audio is heard and the frame is not written.
static int
dsd_stereo_wav_channel_mask(const dsd_opts* opts, const dsd_state* state, int encL, int encR, int copy_right_to_left,
                            int copy_left_to_right) {
    if (opts->wav_out_f == NULL || opts->static_wav_file != 1) {
        return 0;
    }
    const int enc[2] = {encL, encR};
    int source[2];
    dsd_stereo_channel_sources(copy_right_to_left, copy_left_to_right, source);
    int recordable[2] = {-1, -1};
    int mask = 0;
    int heard = 0;
    for (int ch = 0; ch < 2; ch++) {
        const int slot = source[ch];
        if (enc[slot]) {
            continue;
        }
        if (recordable[slot] < 0) {
            recordable[slot] = dsd_static_wav_slot_recordable(opts, state, slot);
        }
        if (recordable[slot]) {
            heard = 1;
        } else {
            mask |= 1 << ch;
        }
    }
    return (mask != 0 && !heard) ? -1 : mask;
}

void
dsd_write_masked_stereo_wav_block(const dsd_opts* opts, const short* block, int mask, const char* context) {
    if (mask == 0) {
        dsd_audio_write_wav_short_block(opts->wav_out_f, block, 320, context);
        return;
    }
    short masked[320];
    DSD_MEMCPY(masked, block, sizeof(masked));
    for (int i = 0; i < 160; i++) {
        if (mask & 1) {
            masked[(i * 2) + 0] = 0;
        }
        if (mask & 2) {
            masked[(i * 2) + 1] = 0;
        }
    }
    dsd_audio_write_wav_short_block(opts->wav_out_f, masked, 320, context);
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
    dsd_audio_write_wav_short_block(opts->wav_out_f, ss, 320, "dsd_write_static_wav_from_mono");
}

DSD_AUDIO2_INTERNAL int
dsd_p25_algid_is_encrypted(const dsd_state* state) {
    return DSD_SYNC_IS_P25P1(state->synctype) && state->payload_algid != 0 && state->payload_algid != 0x80;
}

DSD_AUDIO2_INTERNAL int
dsd_p25_algid_can_decrypt(const dsd_state* state) {
    int algid = state->payload_algid;
    if (algid == 0xAA || algid == 0x81 || algid == 0x9F) {
        return dsd_key_scalar_present(state, 0);
    }
    if (algid == 0x84 || algid == 0x89) {
        return state->aes_key_loaded[0] == 1;
    }
    return 0;
}

DSD_AUDIO2_INTERNAL int
dsd_nxdn_can_decrypt(const dsd_state* state) {
    if (state->nxdn_cipher_type == 0x1 || state->nxdn_cipher_type == 0x2) {
        return dsd_key_scalar_present(state, 0);
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
dsd_fdma_crypto_muted(const dsd_opts* opts, const dsd_state* state, int include_nxdn) {
    if (DSD_SYNC_IS_P25P1(state->synctype)) {
        return dsd_p25p1_live_crypto_gate_applies(state) && !p25_crypto_audio_output_permitted(opts, state, 0);
    }

    int muted = dsd_p25_algid_is_encrypted(state) || (include_nxdn && state->nxdn_cipher_type != 0);
    if (!muted) {
        return 0;
    }

    const int can_p25 =
        dsd_p25_algid_can_decrypt(state) || (state->payload_algid == 0x83 && dsd_key_scalar_present(state, 0));
    return (can_p25 || (include_nxdn && dsd_nxdn_can_decrypt(state))) ? 0 : 1;
}

static int
dsd_fdma_apply_group_gate(const dsd_opts* opts, const dsd_state* state, unsigned long tg, int muted) {
    (void)dsd_audio_group_gate_mono(opts, state, tg, muted, &muted);
    if (dsd_p25p1_live_crypto_gate_applies(state) && !p25_crypto_audio_output_permitted(opts, state, 0)) {
        return 1;
    }
    return muted;
}

int
dsd_audio_mono_output_muted(const dsd_opts* opts, const dsd_state* state) {
    return dsd_fdma_apply_group_gate(opts, state, dsd_audio_call_target(state, dsd_mono_source_slot(state)), 0);
}

// Whether an output receives a mix's blocks (issue #574), the stamp's output condition: every mix writes through
// dsd_output_*_block(), which feed the local stream while it is open, UDP, and the raw fd in either sample format.
int
dsd_mix_output_plays(const dsd_opts* opts) {
    return dsd_audio_activity_output_plays(opts, opts->audio_out_stream, 1);
}

// The audible-audio stamp for a DMR mix (issue #574), called while armed with each slot's final mute flags and the
// output policy's copy decisions: the mix takes both slots' staged media, and stamps when a channel it emits carries an
// unmuted slot that held media of @p kind, and an output receives the mix.
static void
dsd_dmr_mix_note_audible(const dsd_opts* opts, int encL, int encR, int copy_right_to_left, int copy_left_to_right,
                         unsigned int kind) {
    const unsigned int left = dsd_audio_dmr_mix_media_take(0);
    const unsigned int right = dsd_audio_dmr_mix_media_take(1);
    const int fresh[2] = {(left & kind) != 0U, (right & kind) != 0U};
    if (dsd_mix_output_plays(opts)
        && dsd_stereo_mix_carries_fresh(encL, encR, copy_right_to_left, copy_left_to_right, fresh)) {
        dsd_audio_activity_note();
    }
}

// Baofeng AP and CSI EE unmute a slot whatever its flags say, a mixer override older than the vocoder's forced clear
// for them (SS3 then read only the encryption bit of the service options, which forced clear leaves set). It never
// overrides reverse mute (-q): the vocoder applies forced clear first and -q then mutes the call, so the s16 path
// stages none of it and neither mixer may play it.
static inline int
dmr_forced_privacy_unmute_enabled(const dsd_opts* opts, const dsd_state* state) {
    return opts->reverse_mute != 1 && ((state->baofeng_ap == 1) || (state->csi_ee == 1));
}

// The stereo DMR mixes start each slot from the vocoder's verdict, dmr_encL/R as mbe_post_left/right_audio() leave
// it: set when the slot is encrypted and no loaded key decrypts it (a mapped Vertex keystream counts as decrypting),
// cleared under forced clear, and flipped by reverse mute (-q). The Baofeng AP / CSI EE override above comes first.
//
// The mixes differ over the encrypted-audio mute flag, dmr_mute_encL/R. It is 0 after the user's toggle of all
// mutes, after any key load (dsd_key_apply_mute_policy(), or a scan row's or trunk-scan target's direct keys through
// scan_option_apply_mute_dmr()) and after the vocoder's talkgroup key autoload or AES keystream; -q sets it with the
// verdict. While it is 0 the vocoder still runs a slot it flagged encrypted through the short path, and FS3 plays the
// slot (undecryptable garble, as it long has). SS3 does not: a slot flagged encrypted starts muted whatever the flag
// says, so a key load cannot unmute every call those keys do not decrypt. A talkgroup hold on that slot still unmutes
// it, and SS3 then plays the short samples the vocoder staged for it (mute flag 0, no key), as it always has.
static int
dmr_slot_flagged_encrypted(const dsd_opts* opts, const dsd_state* state, int slot) {
    if (dmr_forced_privacy_unmute_enabled(opts, state)) {
        return 0;
    }
    return ((slot == 0) ? state->dmr_encL : state->dmr_encR) != 0 ? 1 : 0;
}

// FS3's verdict: flagged encrypted while encrypted audio is muted.
static int
dmr_slot_mixer_muted(const dsd_opts* opts, const dsd_state* state, int slot) {
    const int mute = (slot == 0) ? opts->dmr_mute_encL : opts->dmr_mute_encR;
    return (dmr_slot_flagged_encrypted(opts, state, slot) && mute != 0) ? 1 : 0;
}

DSD_AUDIO2_INTERNAL void
dsd_dmr_apply_mono_slot_gate(const dsd_opts* opts, const dsd_state* state, int* encL, int* encR) {
    if (opts->dmr_mono != 1 || !DSD_SYNC_IS_DMR(state->synctype)) {
        return;
    }
    if (state->dmr_mono_slot == 1) {
        *encL = 1;
    } else {
        *encR = 1;
    }
}

DSD_AUDIO2_INTERNAL void
dsd_dmr_init_slot_mute_flags(const dsd_opts* opts, const dsd_state* state, int* encL, int* encR) {
    *encL = dmr_slot_mixer_muted(opts, state, 0);
    *encR = dmr_slot_mixer_muted(opts, state, 1);
    dsd_dmr_apply_mono_slot_gate(opts, state, encL, encR);
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
dsd_apply_slot_hard_mute_flags(const dsd_opts* opts, int* encL, int* encR) {
    if (opts->slot1_on == 0) {
        *encL = 1;
    }
    if (opts->slot2_on == 0) {
        *encR = 1;
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

DSD_AUDIO2_INTERNAL void
dsd_dmr_ss3_init_enc_flags(const dsd_opts* opts, const dsd_state* state, int* encL, int* encR) {
    // The vocoder's flag alone, whatever the encrypted-audio mute flag says (see dmr_slot_flagged_encrypted()).
    *encL = dmr_slot_flagged_encrypted(opts, state, 0);
    *encR = dmr_slot_flagged_encrypted(opts, state, 1);
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

// A muted companion slot must not hold its channel silent (the P25 Phase 2
// playout routes the same way): the muted side is zeroed before the copy, so duplication
// keeps a locked-out companion call transparent. The mute flag alone decides
// it -- an audible slot mid-superframe is not always sitting on burst hint 16,
// and gating duplication on the hint collapsed those spans into one ear.
static int
dsd_ss3_should_copy_right_to_left(const dsd_opts* opts, const dsd_state* state, int encL, int encR) {
    if (encR != 0) {
        return 0;
    }
    if (encL != 0) {
        return 1;
    }
    if (opts->dmr_mono == 1 && DSD_SYNC_IS_DMR(state->synctype) && state->dmr_mono_slot == 1) {
        return 1;
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
dsd_ss3_should_copy_left_to_right(const dsd_opts* opts, const dsd_state* state, int encL, int encR) {
    if (encL != 0) {
        return 0;
    }
    if (encR != 0) {
        return 1;
    }
    if (opts->dmr_mono == 1 && DSD_SYNC_IS_DMR(state->synctype) && state->dmr_mono_slot != 1) {
        return 1;
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
    if (dsd_ss3_should_copy_right_to_left(opts, state, encL, encR)) {
        DSD_MEMCPY(state->s_l4, state->s_r4, sizeof(state->s_l4));
    } else if (dsd_ss3_should_copy_left_to_right(opts, state, encL, encR)) {
        DSD_MEMCPY(state->s_r4, state->s_l4, sizeof(state->s_r4));
    }
}

// The release hook (issue #651): the receiver leaves the carrier, so everything the P25 Phase 2 playout holds plays now
// with the verdict each frame was decoded with, in either output format, and the queues empty.
void
dsd_p25p2_flush_partial_audio(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    dsd_p25p2_playout_drain(opts, state);
    dsd_p25p2_playout_discard(opts, state);
}

// A slot's transmission ended (END, IDLE, HANGTIME, MAC Release, SM media close): its stream closes and what it queued
// plays alongside the companion. Nothing is emitted early, and nothing is discarded.
void
dsd_p25p2_flush_partial_audio_slot(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    if (!state || slot < 0 || slot > 1) {
        return;
    }
    dsd_p25p2_playout_close(state, slot);
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

    unsigned long TGL = dsd_audio_call_target(state, 0U);
    unsigned long TGR = dsd_audio_call_target(state, 1U);

    // Apply whitelist/TG-hold gating shared with other mixers.
    (void)dsd_audio_group_gate_dual(opts, state, TGL, TGR, encL, encR, &encL, &encR);
    dsd_dmr_apply_mono_slot_gate(opts, state, &encL, &encR);
    if (dsd_audio_activity_armed()) {
        // FS3 copies no slot over another: its duplication below only fills a muted slot's channel with the other.
        dsd_dmr_mix_note_audible(opts, encL, encR, 0, 0, DSD_DMR_MIX_MEDIA_FLOAT);
    }

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
        // If only one slot is active, duplicate to both channels for stereo sinks.
        dsd_duplicate_active_float_slot_to_stereo(stereo_samp1, stereo_samp2, stereo_samp3, encL, encR, &encL, &encR);
        const float* stereo_blocks[] = {stereo_samp1, stereo_samp2, stereo_samp3};
        dsd_output_float_blocks(opts, state, stereo_blocks, 3, 160, 2, 0);
    }

FS3_END:
    dsd_audio_reset_float_mix_working_state(state);
}

//float stereo mix -- when using Float Stereo Output, we need to send P25p1, DMR MS/Simplex, DStar, and YSF here
void
playSynthesizedVoiceFS(dsd_opts* opts, dsd_state* state) {
    const int is_p25p1 = DSD_SYNC_IS_P25P1(state->synctype);
    int encL = is_p25p1 ? (p25_crypto_audio_output_permitted(opts, state, 0) ? 0 : 1)
                        : (dsd_p25_algid_is_encrypted(state) ? 1 : 0);
    float stereo_samp1[320]; //8k 2-channel stereo interleave mix

    DSD_MEMSET(stereo_samp1, 0.0f, sizeof(stereo_samp1));
    if (!is_p25p1 && encL
        && (dsd_p25_algid_can_decrypt(state) || (state->payload_algid == 0x83 && state->aes_key_loaded[0] == 1))) {
        encL = 0;
    }

    if (opts->slot1_on == 0) {
        encL = 1;
    }

    unsigned long TGL = dsd_audio_call_target(state, dsd_mono_source_slot(state));
    (void)dsd_audio_group_gate_mono(opts, state, TGL, encL, &encL);
    if (is_p25p1 && !p25_crypto_audio_output_permitted(opts, state, 0)) {
        encL = 1;
    }

    agf(opts, state, state->f_l, 0);
    if (!encL) {
        if (dsd_audio_activity_armed() && dsd_mix_output_plays(opts)) {
            dsd_audio_activity_note();
        }
        audio_mono_to_stereo_f32(state->f_l, stereo_samp1, 160);
        audio_apply_gain_f32(stereo_samp1, 320, 0.5f);
        dsd_output_float_block(opts, state, stereo_samp1, 160, 2);
    }
    dsd_audio_reset_float_mix_working_state(state);
}

void
playSynthesizedVoiceFM(dsd_opts* opts, dsd_state* state) {
    agf(opts, state, state->f_l, 0);
    int encL = dsd_fdma_crypto_muted(opts, state, 1);

    unsigned long TGL = dsd_audio_call_target(state, dsd_mono_source_slot(state));

    encL = dsd_fdma_apply_group_gate(opts, state, TGL, encL);

    if (!encL && opts->slot1_on != 0) {
        if (dsd_audio_activity_armed() && dsd_mix_output_plays(opts)) {
            dsd_audio_activity_note();
        }
        if (opts->audio_out == 1 && opts->pulse_digi_out_channels == 2) {
            float stereo[320];
            audio_mono_to_stereo_f32(state->f_l, stereo, 160);
            dsd_output_float_block(opts, state, stereo, 160, 2);
        } else {
            dsd_output_float_block(opts, state, state->f_l, 160, 1);
        }
    }
    dsd_audio_maybe_reset_output_ring_left(state);
    DSD_MEMSET(state->f_l, 0.0f, sizeof(state->f_l));
    DSD_MEMSET(state->audio_out_temp_buf, 0.0f, sizeof(state->audio_out_temp_buf));
}

// Mono source, formatted for the device opened at startup (which stays stereo in AUTO/mixed scans).
void
playSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state) {
    size_t len = state->audio_out_idx;
    if (len > 960) {
        len = 960; // clamp to buffer capacity
    }

    short mono_samp_buf[960];
    short* mono_samp = mono_samp_buf;
    DSD_MEMSET(mono_samp, 0, len * sizeof(short));

    // Crypto is settled before samples reach s_l (the MBE post-processing only
    // fills it for audible calls), but the talkgroup gate and reverse mute are
    // not, so apply them here as the float and stereo paths do.
    const int muted = dsd_audio_mono_output_muted(opts, state);

    if (opts->slot1_on != 0 && !muted) {
        const int loaded = dsd_load_short_mono_samples(mono_samp, len, state->s_l, &state->audio_out_buf_p);
        if (dsd_audio_activity_armed() && loaded && dsd_mix_output_plays(opts)) {
            dsd_audio_activity_note();
        }
        if (opts->use_hpf_d == 1) {
            hpf_dL(state, mono_samp, (int)len);
        }
        if (opts->audio_out == 1 && opts->pulse_digi_out_channels == 2) {
            short stereo[1920];
            audio_mono_to_stereo_s16(mono_samp, stereo, len);
            dsd_output_s16_block(opts, state, stereo, len, 2);
        } else {
            dsd_output_s16_block(opts, state, mono_samp, len, 1);
        }
        if (dsd_static_wav_slot_recordable(opts, state, dsd_mono_source_slot(state))) {
            dsd_write_static_wav_from_mono(opts, mono_samp, len);
        }
    }
    dsd_audio_reset_short_mono_left_working_state(state);
}

//Stereo Mix - Short (SB16LE) -- When Playing Short FDMA samples when setup for stereo output
void
playSynthesizedVoiceSS(dsd_opts* opts, dsd_state* state) {
    int encL = dsd_fdma_crypto_muted(opts, state, 0);
    short stereo_samp1[320]; //8k 2-channel stereo interleave mix
    DSD_MEMSET(stereo_samp1, 0, sizeof(stereo_samp1));

    if (opts->slot1_on == 0) {
        encL = 1;
    }

    const uint8_t source_slot = dsd_mono_source_slot(state);
    unsigned long TGL = dsd_audio_call_target(state, source_slot);

    encL = dsd_fdma_apply_group_gate(opts, state, TGL, encL);

    if (opts->use_hpf_d == 1) {
        hpf_dL(state, state->s_l, 160);
    }
    audio_mono_to_stereo_s16(state->s_l, stereo_samp1, 160);
    if (!encL) {
        if (dsd_audio_activity_armed() && dsd_mix_output_plays(opts)) {
            dsd_audio_activity_note();
        }
        dsd_output_s16_block(opts, state, stereo_samp1, 160, 2);
        if (opts->wav_out_f != NULL && opts->static_wav_file == 1
            && dsd_static_wav_slot_recordable(opts, state, source_slot)) {
            dsd_audio_write_wav_short_block(opts->wav_out_f, stereo_samp1, 320, "processAudioDMRslot");
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

    dsd_dmr_ss3_init_enc_flags(opts, state, &encL, &encR);

    unsigned long TGL = dsd_audio_call_target(state, 0U);
    unsigned long TGR = dsd_audio_call_target(state, 1U);

    // A held talkgroup unmutes its slot even when the vocoder muted it (encrypted with no key while encrypted audio is
    // muted, or clear under -q). FS3 then plays the float frames the vocoder always stages; SS3 plays the slot's short
    // samples, which the vocoder stages only for a slot it did not mute and otherwise leaves silent
    // (mbe_post_stage_slot_silence()), so here a held muted slot plays silence. Staging short samples for a muted slot
    // would run the short path's gain on audio the vocoder judged muted, for a rare combination. A slot flagged
    // encrypted that the vocoder still staged (encrypted-audio mute flag 0, no key) plays those samples under a hold.
    dsd_dmr_apply_tg_hold_and_slot_preference_ss3(opts, state, TGL, TGR, &encL, &encR);
    // Apply the final policy after Hold so a temporary avoid cannot be unmuted again.
    (void)dsd_audio_group_gate_dual(opts, state, TGL, TGR, encL, encR, &encL, &encR);
    dsd_apply_slot_hard_mute_flags(opts, &encL, &encR);
    dsd_dmr_apply_mono_slot_gate(opts, state, &encL, &encR);
    dsd_hpf_short_triplet_if_enabled(opts, state);
    // The copies dsd_dmr_apply_stereo_output_policy_ss3() makes below, decided from the same flags.
    const int copy_right_to_left = dsd_ss3_should_copy_right_to_left(opts, state, encL, encR);
    const int copy_left_to_right = !copy_right_to_left && dsd_ss3_should_copy_left_to_right(opts, state, encL, encR);
    if (dsd_audio_activity_armed()) {
        dsd_dmr_mix_note_audible(opts, encL, encR, copy_right_to_left, copy_left_to_right, DSD_DMR_MIX_MEDIA_SHORT);
    }
    const int wav_mask = dsd_stereo_wav_channel_mask(opts, state, encL, encR, copy_right_to_left, copy_left_to_right);
    dsd_dmr_apply_stereo_output_policy_ss3(opts, state, encL, encR);

    //at this point, if both channels are still flagged as enc, then we can skip all playback/writing functions
    if (encL && encR) {
        goto SS3_END;
    }

    audio_mix_interleave_stereo_s16(state->s_l4[0], state->s_r4[0], 160, 0, 0, stereo_samp1);
    audio_mix_interleave_stereo_s16(state->s_l4[1], state->s_r4[1], 160, 0, 0, stereo_samp2);
    audio_mix_interleave_stereo_s16(state->s_l4[2], state->s_r4[2], 160, 0, 0, stereo_samp3);

    if (opts->pulse_digi_out_channels == 1) {
        short mono1[160], mono2[160], mono3[160];
        int l_on = !encL;
        int r_on = !encR;
        audio_mix_mono_from_slots_s16(state->s_l4[0], state->s_r4[0], 160, l_on, r_on, mono1);
        audio_mix_mono_from_slots_s16(state->s_l4[1], state->s_r4[1], 160, l_on, r_on, mono2);
        audio_mix_mono_from_slots_s16(state->s_l4[2], state->s_r4[2], 160, l_on, r_on, mono3);
        const short* mono_blocks[] = {mono1, mono2, mono3};
        dsd_output_s16_blocks(opts, state, mono_blocks, 3, 160, 1, 0);
    } else {
        const short* stereo_blocks[] = {stereo_samp1, stereo_samp2, stereo_samp3};
        dsd_output_s16_blocks(opts, state, stereo_blocks, 3, 160, 2, 0);
    }

    if (opts->wav_out_f != NULL && opts->static_wav_file == 1 && wav_mask >= 0) {
        dsd_write_masked_stereo_wav_block(opts, stereo_samp1, wav_mask, "processAudioDMRstereo3v2 block1");
        dsd_write_masked_stereo_wav_block(opts, stereo_samp2, wav_mask, "processAudioDMRstereo3v2 block2");
        dsd_write_masked_stereo_wav_block(opts, stereo_samp3, wav_mask, "processAudioDMRstereo3v2 block3");
    }

SS3_END:
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
    // An alert for a P25 Phase 2 slot whose playout still holds audio sounds after that audio (issue #651).
    if (dsd_p25p2_playout_defer_alert(opts, state, lr, id, ad, len)) {
        return;
    }
    dsd_beeper_emit(opts, state, lr, id, ad, len);
}

void
dsd_beeper_emit(dsd_opts* opts, dsd_state* state, int lr, int id, int ad, int len) {
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
