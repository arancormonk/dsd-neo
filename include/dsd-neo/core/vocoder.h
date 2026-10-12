// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Voice/vocoder decode entrypoints.
 *
 * Declares the MBE decode functions implemented in `src/core/vocoder/`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_VOCODER_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_VOCODER_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t bit;
    uint8_t reliability;
} dsd_vocoder_soft_bit;

static inline dsd_vocoder_soft_bit
dsd_vocoder_soft_bit_from_hard_llr(int bit, int16_t llr) {
    int reliability = llr < 0 ? -(int)llr : (int)llr;
    if (reliability > 255) {
        reliability = 255;
    }
    dsd_vocoder_soft_bit out = {(uint8_t)(bit ? 1 : 0), (uint8_t)reliability};
    return out;
}

/**
 * The protocol that owns a voice frame, as distinct from the codec path that decodes it.
 *
 * The codec path still follows `state->synctype`, which YSF and scrambled dPMR set to another protocol's value to
 * borrow its decoder (YSF V/D Type 1 and scrambled dPMR halves decode as NXDN, YSF full rate as P25 Phase 1). The
 * owner decides which crypto transforms and key selection may touch the frame (issue #656):
 * - DMR: privacy, Hytera, vendor scramblers, DMR ALG IDs and the P25 ALG IDs DMR carries.
 * - P25P1: P25 multicrypt (DES, TDEA, DES-XL, AES) and RC4.
 * - P25P2: P25 DES, AES and RC4.
 * - NXDN, DPMR: the NXDN scrambler and NXDN cipher 2/3 (dPMR reaches them only on its scrambled halves).
 * - YSF, D-STAR, X2-TDMA, ProVoice, NONE: no crypto transform.
 * The manual `-S` keystream applies on the shared DMR / P25 Phase 2 / clear dPMR slot path whatever the owner.
 */
typedef enum {
    DSD_VOICE_PROTOCOL_NONE = 0,
    DSD_VOICE_PROTOCOL_DMR,
    DSD_VOICE_PROTOCOL_P25P1,
    DSD_VOICE_PROTOCOL_P25P2,
    DSD_VOICE_PROTOCOL_NXDN,
    DSD_VOICE_PROTOCOL_DPMR,
    DSD_VOICE_PROTOCOL_YSF,
    DSD_VOICE_PROTOCOL_DSTAR,
    DSD_VOICE_PROTOCOL_X2TDMA,
    DSD_VOICE_PROTOCOL_PROVOICE,
} dsd_voice_protocol;

void processMbeFrame(dsd_opts* opts, dsd_state* state, dsd_voice_protocol protocol, char imbe_fr[8][23],
                     char ambe_fr[4][24], char imbe7100_fr[7][24]);
void processMbeFrameSoft(dsd_opts* opts, dsd_state* state, dsd_voice_protocol protocol,
                         dsd_vocoder_soft_bit imbe_fr[8][23], dsd_vocoder_soft_bit ambe_fr[4][24],
                         dsd_vocoder_soft_bit imbe7100_fr[7][24]);

/**
 * Account for a P25 Phase 1 IMBE frame the decoder drops before the vocoder (non-standard C0 or the crypto gate):
 * the frame consumes its keystream position (11 octets of multicrypt keystream, generated first if this frame would have
 * generated it, or 11 RC4 drop octets) exactly as a decoded frame does, so the frames after it in the superframe
 * decrypt with their own keystream. Nothing is synthesized or output.
 */
void dsd_mbe_p25p1_skip_voice_frame(dsd_state* state);

/** Decode and log one soft IMBE frame without synthesizing or emitting media. */
void dsd_mbe_log_imbe_soft_frame(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit imbe_fr[8][23]);

/** Decode and log one soft AMBE frame without synthesizing or emitting media. */
void dsd_mbe_log_ambe_soft_frame(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit ambe_fr[4][24]);

void playMbeFiles(dsd_opts* opts, dsd_state* state, int argc, char** argv);

/**
 * mbelib-neo's float PCM is int16 / 7: mbe_floattoshort() multiplies by 7. The synthesized-voice buffers hold
 * int16-scale samples, which the output gain, auto-gain and WAV stages expect, so each frame mbelib-neo
 * synthesizes is scaled by this once, as it arrives.
 */
#define DSD_MBE_PCM16_PER_FLOAT 7.0f

/** Scale one 160-sample frame of mbelib-neo float PCM to the int16 scale of the voice buffers. */
static inline void
dsd_mbe_float_to_pcm16_scale(float* samples) {
    if (!samples) {
        return;
    }
    for (int i = 0; i < 160; i++) {
        samples[i] *= DSD_MBE_PCM16_PER_FLOAT;
    }
}

/** Purge queued/working audio and vocoder history for one logical voice slot. */
void dsd_mbe_purge_slot_audio(dsd_state* state, int slot);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_VOCODER_H_H */
