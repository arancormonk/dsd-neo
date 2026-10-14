// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 *
 * Digital voice playback mixers (mono, stereo, DMR stereo), their output
 * primitives and the alert beeper
 *
 * LWVMOBILE
 * 2023-10 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/core/audio_sink.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25p2_playout.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd_audio2_internal.h"
#include "dsd_audio_internal.h"

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

/* Convert float audio to int16 for the local device stream. A block of any length goes out in chunks the conversion
   buffer holds, each written with its own frame count, so the backend never reads past what was converted. */
DSD_AUDIO2_INTERNAL void
dsd_write_float_audio_device(dsd_opts* opts, const float* buf, size_t frames, int channels) {
    if (!opts->audio_out_stream || !buf || channels <= 0) {
        return;
    }
    int16_t tmp[320 * 2];
    const size_t chunk_frames_max = (sizeof(tmp) / sizeof(tmp[0])) / (size_t)channels;
    if (chunk_frames_max == 0U) {
        return;
    }
    size_t done = 0;
    while (done < frames) {
        size_t chunk_frames = frames - done;
        if (chunk_frames > chunk_frames_max) {
            chunk_frames = chunk_frames_max;
        }
        const float* src = buf + (done * (size_t)channels);
        const size_t chunk_samples = chunk_frames * (size_t)channels;
        /* Convert float [-1.0, 1.0] to int16 with clipping */
        for (size_t i = 0; i < chunk_samples; i++) {
            float v = src[i] * 32767.0f;
            if (v > 32767.0f) {
                v = 32767.0f;
            } else if (v < -32768.0f) {
                v = -32768.0f;
            }
            tmp[i] = (int16_t)v;
        }
        if (dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, opts->audio_out_stream, tmp, chunk_frames) < 0) {
            /* The device is down: the rest of the block would fail too. */
            return;
        }
        done += chunk_frames;
    }
}

static inline void
write_audio_out(int fd, const void* buf, size_t bytes, size_t frame_bytes) {
    (void)dsd_audio_sink_write_fd(fd, buf, bytes, frame_bytes);
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
        dsd_write_float_audio_device(opts, samples, frames, channels);
    } else if (opts->audio_out_type == 8) {
        dsd_udp_audio_hook_blast(opts, state, frames * (size_t)channels * sizeof(float), (void*)samples);
    } else if (opts->audio_out_type == 1) {
        write_audio_out(opts->audio_out_fd, samples, frames * (size_t)channels * sizeof(float),
                        (size_t)channels * sizeof(float));
    }
}

// The most samples one f32 write carries: a 960-frame stereo block (48 kHz mono duplicated), the largest block a
// mixer writes, so each block goes out as one write and one UDP datagram.
enum { DSD_OUTPUT_F32_CHUNK_SAMPLES = 1920 };

// Float output (-y) is the 16-bit output written as 32-bit float samples, s16 / 32768 (exact), to the outputs that
// take bytes: the raw fd (-o -) and UDP. A block larger than a chunk goes out in whole frames, chunk by chunk.
static void
dsd_output_s16_as_f32(const dsd_opts* opts, dsd_state* state, const short* samples, size_t frames, size_t channels) {
    float f32[DSD_OUTPUT_F32_CHUNK_SAMPLES];
    const size_t chunk_frames = DSD_OUTPUT_F32_CHUNK_SAMPLES / channels;
    for (size_t done = 0; done < frames;) {
        const size_t n = (frames - done < chunk_frames) ? frames - done : chunk_frames;
        const short* src = samples + (done * channels);
        for (size_t i = 0; i < n * channels; i++) {
            f32[i] = (float)src[i] * (1.0f / 32768.0f);
        }
        if (opts->audio_out_type == 8) {
            dsd_udp_audio_hook_blast(opts, state, n * channels * sizeof(float), f32);
        } else {
            write_audio_out(opts->audio_out_fd, f32, n * channels * sizeof(float), channels * sizeof(float));
        }
        done += n;
    }
}

// The one digital voice writer (issue #652): every mixer, the P25 Phase 2 playout and the legacy short output hand it
// 16-bit blocks of @p channels interleaved channels, in either output format. The local device stream is 16-bit in
// both formats; under -y the raw fd and UDP take the same samples as f32.
void
dsd_output_s16_block(const dsd_opts* opts, dsd_state* state, const short* samples, size_t frames, int channels) {
    if (opts->audio_out != 1 || !samples || frames == 0 || channels < 1 || channels > 2) {
        return;
    }
    if (opts->audio_out_type == 0) {
        (void)dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, opts->audio_out_stream, samples, frames);
    } else if (opts->floating_point == 1 && (opts->audio_out_type == 8 || opts->audio_out_type == 1)) {
        dsd_output_s16_as_f32(opts, state, samples, frames, (size_t)channels);
    } else if (opts->audio_out_type == 8) {
        dsd_udp_audio_hook_blast(opts, state, frames * (size_t)channels * sizeof(short), (void*)samples);
    } else if (opts->audio_out_type == 1) {
        write_audio_out(opts->audio_out_fd, samples, frames * (size_t)channels * sizeof(short),
                        (size_t)channels * sizeof(short));
    }
}

DSD_AUDIO2_INTERNAL void
dsd_output_s16_blocks(const dsd_opts* opts, dsd_state* state, const short* const* blocks, size_t block_count,
                      size_t frames, int channels, int skip_silent) {
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

// The slot switch (-V, slot1_on/slot2_on) of the timeslot the mono buffer holds (issue #654). The mono mixers apply it
// after every other gate, so a talkgroup hold never plays a switched-off timeslot.
static int
dsd_mono_source_slot_on(const dsd_opts* opts, const dsd_state* state) {
    return (dsd_mono_source_slot(state) == 1U ? opts->slot2_on : opts->slot1_on) != 0;
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
dsd_write_static_wav_from_mono(const dsd_opts* opts, const short* mono_samp, size_t len) {
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

// The FDMA mixers' encryption verdict on the frame they play: the one the vocoder judged it by, from the protocol that
// owns the call (dsd_voice_crypto_audible(), issue #655), so the float and short outputs, the per-call WAV and the MBE
// capture agree, the user's encrypted-audio mute and reverse mute (-q) included.
static int
dsd_fdma_crypto_muted(const dsd_opts* opts, const dsd_state* state) {
    return dsd_voice_crypto_audible(opts, state, dsd_mono_source_slot(state)) ? 0 : 1;
}

// The talkgroup gate on top: a hold decides the policy only, so it never clears the crypto mute (issue #655).
static int
dsd_fdma_apply_group_gate(const dsd_opts* opts, const dsd_state* state, unsigned long tg, int muted) {
    (void)dsd_audio_group_gate_mono(opts, state, tg, muted, &muted);
    return muted;
}

int
dsd_audio_mono_output_muted(const dsd_opts* opts, const dsd_state* state) {
    return dsd_fdma_apply_group_gate(opts, state, dsd_audio_call_target(state, dsd_mono_source_slot(state)),
                                     dsd_fdma_crypto_muted(opts, state));
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

// The shared DMR mixer reads the frame verdict already settled by the vocoder. Forced privacy and reverse mute
// participate there; a mixer override must not unmute an exhausted or otherwise rejected frame.
static int
dmr_slot_mixer_muted(const dsd_state* state, int slot) {
    return state->mbe_slot_muted[slot == 1 ? 1 : 0] != 0 ? 1 : 0;
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

static void
dsd_apply_slot_hard_mute_flags(const dsd_opts* opts, int* encL, int* encR) {
    if (opts->slot1_on == 0) {
        *encL = 1;
    }
    if (opts->slot2_on == 0) {
        *encR = 1;
    }
}

DSD_AUDIO2_INTERNAL void
dsd_dmr_ss3_init_enc_flags(const dsd_opts* opts, const dsd_state* state, int* encL, int* encR) {
    (void)opts;
    // Both output formats read the same vocoder verdict.
    *encL = dmr_slot_mixer_muted(state, 0);
    *encR = dmr_slot_mixer_muted(state, 1);
}

// The slot (0 or 1) a talkgroup hold names, or -1: slot 1 when both slots carry the held talkgroup.
static int
dsd_dmr_held_slot(const dsd_state* state, unsigned long TGL, unsigned long TGR) {
    if (state->tg_hold == 0) {
        return -1;
    }
    if (state->tg_hold == TGL) {
        return 0;
    }
    return (state->tg_hold == TGR) ? 1 : -1;
}

// The slot preference a mix applies (issue #657): the slot a hold names, else the user's preference, where 2 and
// anything else is Auto. Mixes read the preference and the slot switches and never write them, so what the user set is
// what the menu, the API and the next mix see.
DSD_AUDIO2_INTERNAL int
dsd_tdma_effective_slot_preference(const dsd_opts* opts, int held_slot) {
    if (held_slot == 0 || held_slot == 1) {
        return held_slot;
    }
    return (opts->slot_preference == 0 || opts->slot_preference == 1) ? opts->slot_preference : 2;
}

// A hold mutes the slots not carrying the held talkgroup and unmutes the one that does; returns the preference the
// mix applies. The slot switches are applied after the hold, so a held call on a switched-off slot stays silent.
DSD_AUDIO2_INTERNAL int
dsd_dmr_apply_tg_hold_ss3(const dsd_opts* opts, const dsd_state* state, unsigned long TGL, unsigned long TGR, int* encL,
                          int* encR) {
    const int held_slot = dsd_dmr_held_slot(state, TGL, TGR);
    if (state->tg_hold != 0 && state->tg_hold != TGL) {
        *encL = 1;
    }
    if (state->tg_hold != 0 && state->tg_hold != TGR) {
        *encR = 1;
    }
    // A hold selects a talkgroup without overriding the frame's crypto verdict.
    return dsd_tdma_effective_slot_preference(opts, held_slot);
}

// The preference's tie-break, used in both output formats: with both slots audible, the preferred slot fills both
// channels while it carries voice. Returns that slot, or -1.
static int
dsd_dmr_preferred_slot_fills_both(const dsd_state* state, int pref, int encL, int encR) {
    if (encL != 0 || encR != 0) {
        return -1;
    }
    if (pref == 1 && state->dmrburstR == 16) {
        return 1;
    }
    if (pref == 0 && state->dmrburstL == 16) {
        return 0;
    }
    return -1;
}

// A muted companion slot must not hold its channel silent (the P25 Phase 2
// playout routes the same way): the muted side is zeroed before the copy, so duplication
// keeps a locked-out companion call transparent. The mute flag alone decides
// it -- an audible slot mid-superframe is not always sitting on burst hint 16,
// and gating duplication on the hint collapsed those spans into one ear.
static int
dsd_ss3_should_copy_right_to_left(const dsd_opts* opts, const dsd_state* state, int pref, int encL, int encR) {
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
    if (dsd_dmr_preferred_slot_fills_both(state, pref, encL, encR) == 1) {
        return 1;
    }
    if (state->dmrburstR == 16 && state->dmrburstL != 16) {
        return 1;
    }
    return 0;
}

static int
dsd_ss3_should_copy_left_to_right(const dsd_opts* opts, const dsd_state* state, int pref, int encL, int encR) {
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
    if (dsd_dmr_preferred_slot_fills_both(state, pref, encL, encR) == 0) {
        return 1;
    }
    if (state->dmrburstL == 16 && state->dmrburstR != 16) {
        return 1;
    }
    return 0;
}

DSD_AUDIO2_INTERNAL void
dsd_dmr_apply_stereo_output_policy_ss3(const dsd_opts* opts, dsd_state* state, int pref, int encL, int encR) {
    if (encL) {
        DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
    }
    if (encR) {
        DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
    }
    if (dsd_ss3_should_copy_right_to_left(opts, state, pref, encL, encR)) {
        DSD_MEMCPY(state->s_l4, state->s_r4, sizeof(state->s_l4));
    } else if (dsd_ss3_should_copy_left_to_right(opts, state, pref, encL, encR)) {
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

// Mono source, formatted for the device opened at startup (which stays stereo in AUTO/mixed scans).
//
// It plays exactly the frame dsd_voice_stage_short_left() staged: audio_out_idx is that frame's count (160, or 960
// upsampled), and 0 when the frame staged no short samples (muted crypto). Any other count is no frame either.
// Without a frame nothing is played, recorded or stamped (issue #654). The static WAV takes a frame
// that passed every gate, whether or not an output is open (-o null -w records).
// How a mixer takes the frame staged for it: judged by the protocol's verdict and the talkgroup policy, or settled by a
// decoder that gated it itself (dsd_audio_play_decoded_pcm16()), which also says whether the recordings take it.
typedef enum {
    DSD_MIX_JUDGED = 0,
    DSD_MIX_SETTLED = 1,
    DSD_MIX_SETTLED_UNRECORDED = 2,
} dsd_mix_verdict;

// The static WAV takes a judged frame its slot's record policy allows, and a settled one the decoder lets record.
static int
dsd_mix_static_wav_takes(const dsd_opts* opts, const dsd_state* state, dsd_mix_verdict verdict, int slot) {
    if (verdict == DSD_MIX_JUDGED) {
        return dsd_static_wav_slot_recordable(opts, state, slot);
    }
    return verdict == DSD_MIX_SETTLED;
}

static void
dsd_play_voice_ms(const dsd_opts* opts, dsd_state* state, dsd_mix_verdict verdict) {
    const int settled = verdict != DSD_MIX_JUDGED;
    const int count = state->audio_out_idx;
    const size_t len = (count == 160 || count == 960) ? (size_t)count : 0U;
    short mono_samp[960];

    // The vocoder stages s_l only for a frame its verdict lets play; the same verdict and the talkgroup gate apply
    // here and in the stereo path, in both output formats.
    const int muted = settled ? 0 : dsd_audio_mono_output_muted(opts, state);
    const int slot_on = settled ? opts->slot1_on : dsd_mono_source_slot_on(opts, state);

    if (len != 0U && !muted && slot_on
        && dsd_load_short_mono_samples(mono_samp, len, state->s_l, &state->audio_out_buf_p)) {
        if (dsd_audio_activity_armed() && dsd_mix_output_plays(opts)) {
            dsd_audio_activity_note();
        }
        if (opts->audio_out == 1 && opts->pulse_digi_out_channels == 2) {
            short stereo[1920];
            audio_mono_to_stereo_s16(mono_samp, stereo, len);
            dsd_output_s16_block(opts, state, stereo, len, 2);
        } else {
            dsd_output_s16_block(opts, state, mono_samp, len, 1);
        }
        if (dsd_mix_static_wav_takes(opts, state, verdict, dsd_mono_source_slot(state))) {
            dsd_write_static_wav_from_mono(opts, mono_samp, len);
        }
    }
    dsd_audio_reset_short_mono_left_working_state(state);
}

//Stereo Mix - Short (SB16LE) -- When Playing Short FDMA samples when setup for stereo output
static void
dsd_play_voice_ss(const dsd_opts* opts, dsd_state* state, dsd_mix_verdict verdict) {
    const int settled = verdict != DSD_MIX_JUDGED;
    int encL = settled ? 0 : dsd_fdma_crypto_muted(opts, state);
    short stereo_samp1[320]; //8k 2-channel stereo interleave mix
    DSD_MEMSET(stereo_samp1, 0, sizeof(stereo_samp1));

    const uint8_t source_slot = settled ? 0U : dsd_mono_source_slot(state);
    if (!settled) {
        encL = dsd_fdma_apply_group_gate(opts, state, dsd_audio_call_target(state, source_slot), encL);
    }
    // The source slot switch mutes last: a hold never plays a switched-off slot.
    if (!(source_slot == 1U ? opts->slot2_on : opts->slot1_on)) {
        encL = 1;
    }

    audio_mono_to_stereo_s16(state->s_l, stereo_samp1, 160);
    if (!encL) {
        if (dsd_audio_activity_armed() && dsd_mix_output_plays(opts)) {
            dsd_audio_activity_note();
        }
        dsd_output_s16_block(opts, state, stereo_samp1, 160, 2);
        if (opts->wav_out_f != NULL && opts->static_wav_file == 1
            && dsd_mix_static_wav_takes(opts, state, verdict, source_slot)) {
            dsd_audio_write_wav_short_block(opts->wav_out_f, stereo_samp1, 320, "processAudioDMRslot");
        }
    }
    dsd_audio_reset_short_lr_working_state(state);
}

// Public mixers retain the protocol and talkgroup policy; independently gated PCM uses the same routing below.
void
// cppcheck-suppress constParameterPointer -- signature fixed by dsd-neo/core/audio.h
playSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state) {
    dsd_play_voice_ms(opts, state, DSD_MIX_JUDGED);
}

void
// cppcheck-suppress constParameterPointer -- signature fixed by dsd-neo/core/audio.h
playSynthesizedVoiceSS(dsd_opts* opts, dsd_state* state) {
    dsd_play_voice_ss(opts, state, DSD_MIX_JUDGED);
}

void
dsd_audio_play_decoded_pcm16(const dsd_opts* opts, dsd_state* state, const short* pcm, size_t count) {
    dsd_audio_play_decoded_pcm16_record(opts, state, pcm, count, 1);
}

/* One DSD_VOICE_FRAME_SAMPLES frame of decoded PCM through the voice chain to the outputs, and to the recordings when
   @p record. */
static void
dsd_audio_play_decoded_frame(const dsd_opts* opts, dsd_state* state, const short* frame, int record) {
    for (size_t i = 0; i < DSD_VOICE_FRAME_SAMPLES; i++) {
        state->audio_out_temp_buf[i] = (float)frame[i];
    }
    dsd_voice_chain_run(opts, state, 0, state->audio_out_temp_buf);
    dsd_voice_stage_short_left(opts, state);
    if (record && opts->wav_out_f != NULL && opts->dmr_stereo_wav == 1) {
        writeSynthesizedVoice(opts, state);
    }
    const dsd_mix_verdict verdict = record ? DSD_MIX_SETTLED : DSD_MIX_SETTLED_UNRECORDED;
    if (opts->pulse_digi_out_channels == 2) {
        dsd_play_voice_ss(opts, state, verdict);
    } else {
        dsd_play_voice_ms(opts, state, verdict);
    }
}

void
dsd_audio_play_decoded_pcm16_record(const dsd_opts* opts, dsd_state* state, const short* pcm, size_t count,
                                    int record) {
    if (!opts || !state || !pcm || !state->audio_out_buf_p || !state->aout_max_buf_p
        || opts->pulse_digi_out_channels < 1 || opts->pulse_digi_out_channels > 2) {
        return;
    }
    /* Voice decoded while an input interruption stands comes from a frame it cut (a Signal search left the frame, or a
       live read gave up on a stalled input): the rest of that frame is erasures getSymbol() stood in, not speech. */
    if (state->input_interrupted) {
        return;
    }
    for (size_t offset = 0; count - offset >= DSD_VOICE_FRAME_SAMPLES; offset += DSD_VOICE_FRAME_SAMPLES) {
        dsd_audio_play_decoded_frame(opts, state, pcm + offset, record);
    }
}

//short stereo mix 3v2 DMR
void
// cppcheck-suppress constParameterPointer -- signature fixed by dsd-neo/core/audio.h
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

    // The held slot keeps its crypto verdict and the user's hard slot switch.
    const int pref = dsd_dmr_apply_tg_hold_ss3(opts, state, TGL, TGR, &encL, &encR);
    // Apply the final policy after Hold so a temporary avoid cannot be unmuted again.
    (void)dsd_audio_group_gate_dual(opts, state, TGL, TGR, encL, encR, &encL, &encR);
    // The slot switches mute last, as in every mix: a hold never plays a switched-off slot (issue #657).
    dsd_apply_slot_hard_mute_flags(opts, &encL, &encR);
    dsd_dmr_apply_mono_slot_gate(opts, state, &encL, &encR);
    // The copies dsd_dmr_apply_stereo_output_policy_ss3() makes below, decided from the same flags.
    const int copy_right_to_left = dsd_ss3_should_copy_right_to_left(opts, state, pref, encL, encR);
    const int copy_left_to_right =
        !copy_right_to_left && dsd_ss3_should_copy_left_to_right(opts, state, pref, encL, encR);
    if (dsd_audio_activity_armed()) {
        dsd_dmr_mix_note_audible(opts, encL, encR, copy_right_to_left, copy_left_to_right, DSD_DMR_MIX_MEDIA_SHORT);
    }
    const int wav_mask = dsd_stereo_wav_channel_mask(opts, state, encL, encR, copy_right_to_left, copy_left_to_right);
    dsd_dmr_apply_stereo_output_policy_ss3(opts, state, pref, encL, encR);

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

// One 20 ms block of the alert tone, largely borrowed from Boatbod OP25 (simplified single tone ID version): a sine at
// 31.25 Hz x ID with a peak of AD x 75 / 8000 of full scale, from sample @p n of the tone on. It is built once on the
// int16 scale (issue #653): the short output plays these samples and the float output the same samples / 32768, so an
// alert sounds the same in both formats.
static void
dsd_beeper_tone_block(short* samp, int n, int id, int ad) {
    const double pi = 3.14159265358979323846;
    const double step = 2.0 * pi * (31.25 * (double)id) / 8000.0;
    const double amplitude = ((double)ad * 75.0) / 8000.0;
    for (int i = 0; i < 160; i++) {
        double v = amplitude * sin((double)(n + i) * step) * 32767.0;
        if (v > 32767.0) {
            v = 32767.0;
        } else if (v < -32767.0) {
            v = -32767.0;
        }
        samp[i] = (short)lrint(v);
    }
}

static void
dsd_beeper_output_samples(const dsd_opts* opts, dsd_state* state, const short* samp_s, const short* samp_ss) {
    if (opts->pulse_digi_out_channels == 2) {
        dsd_output_s16_block(opts, state, samp_ss, 160, 2);
    } else {
        dsd_output_s16_block(opts, state, samp_s, 160, 1);
    }
}

void
// cppcheck-suppress constParameterPointer -- signature fixed by dsd-neo/core/audio.h
beeper(dsd_opts* opts, dsd_state* state, int lr, int id, int ad, int len) {
    // An alert for a P25 Phase 2 slot whose playout still holds audio sounds after that audio (issue #651).
    if (dsd_p25p2_playout_defer_alert(opts, state, lr, id, ad, len)) {
        return;
    }
    dsd_beeper_emit(opts, state, lr, id, ad, len);
}

void
dsd_beeper_emit(const dsd_opts* opts, dsd_state* state, int lr, int id, int ad, int len) {
    //use lr as left or right channel designation in stereo config
    short samp_s[160];  //mono short sample
    short samp_ss[320]; //stereo short sample
    const int ch = (lr == 1) ? 1 : 0;

    //double len if not using Pulse Audio,
    //anything over UDP may
    //not clear the buffer at the shorter len
    if (opts->audio_out_type != 0) {
        len *= 2;
    }

    //each j increment is 20 ms at 160 samples / 8 kHz; the tone runs on across blocks
    for (int j = 0; j < len; j++) {
        DSD_MEMSET(samp_ss, 0, sizeof(samp_ss));
        dsd_beeper_tone_block(samp_s, j * 160, id, ad);
        for (int i = 0; i < 160; i++) {
            samp_ss[(i * 2) + ch] = samp_s[i];
        }
        dsd_beeper_output_samples(opts, state, samp_s, samp_ss);
    }
}
