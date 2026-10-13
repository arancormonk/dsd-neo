// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Voice/vocoder decode entrypoints and vocoder type abstraction.
 *
 * Declares the MBE decode functions implemented in `src/core/vocoder/`,
 * the `dsd_vocoder_type_t` enumeration, and the `dsd_vocoder_from_synctype()`
 * helper that maps a sync-type ID to the codec it requires.
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Identifies which speech codec is required for a given protocol.
 *
 * The enumeration is intentionally kept small and stable: new vocoders
 * that may be added in the future (e.g. LPCNet, Opus for PoC) should
 * extend this list rather than reuse existing values.
 */
typedef enum {
    DSD_VOCODER_NONE = 0,      /**< No voice / unknown / data-only frame */
    DSD_VOCODER_IMBE_7200 = 1, /**< IMBE 7200x4400 — P25 Phase 1         */
    DSD_VOCODER_IMBE_7100 = 2, /**< IMBE 7100x4400 — ProVoice / EDACS    */
    DSD_VOCODER_AMBE_3600 = 3, /**< AMBE 3600x2400 — D-STAR              */
    DSD_VOCODER_AMBE2_EHR = 4, /**< AMBE+2 3600x2450 EHR — DMR, P25p2,
                                 *   NXDN, YSF, X2-TDMA, dPMR            */
    DSD_VOCODER_CODEC2 = 5,    /**< Codec2 (1600 / 3200 bps) — M17       */
    DSD_VOCODER_ACELP = 6,     /**< ACELP via external subprocess — TETRA */
} dsd_vocoder_type_t;

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
 * @brief Map a synctype ID to the vocoder it requires.
 *
 * This function encapsulates the same dispatch logic used by
 * `processMbeFrame()` and `soft_mbe()` so callers can query the codec
 * identity without having to reproduce the if/else chain themselves.
 *
 * @param synctype  A `DSD_SYNC_*` constant from synctype_ids.h.
 * @return          The `dsd_vocoder_type_t` value for the given synctype,
 *                  or `DSD_VOCODER_NONE` for data, control, or unknown frames.
 */
dsd_vocoder_type_t dsd_vocoder_from_synctype(int synctype);

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
