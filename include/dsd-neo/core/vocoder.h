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
    DSD_VOCODER_NONE      = 0, /**< No voice / unknown / data-only frame */
    DSD_VOCODER_IMBE_7200 = 1, /**< IMBE 7200x4400 — P25 Phase 1         */
    DSD_VOCODER_IMBE_7100 = 2, /**< IMBE 7100x4400 — ProVoice / EDACS    */
    DSD_VOCODER_AMBE_3600 = 3, /**< AMBE 3600x2400 — D-STAR              */
    DSD_VOCODER_AMBE2_EHR = 4, /**< AMBE+2 3600x2450 EHR — DMR, P25p2,
                                 *   NXDN, YSF, X2-TDMA, dPMR            */
    DSD_VOCODER_CODEC2    = 5, /**< Codec2 (1600 / 3200 bps) — M17       */
    DSD_VOCODER_ACELP     = 6, /**< ACELP via external subprocess — TETRA */
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

void processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24],
                     char imbe7100_fr[7][24]);
void processMbeFrameSoft(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit imbe_fr[8][23],
                         dsd_vocoder_soft_bit ambe_fr[4][24], dsd_vocoder_soft_bit imbe7100_fr[7][24]);

/** Decode and log one soft IMBE frame without synthesizing or emitting media. */
void dsd_mbe_log_imbe_soft_frame(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit imbe_fr[8][23]);

/** Decode and log one soft AMBE frame without synthesizing or emitting media. */
void dsd_mbe_log_ambe_soft_frame(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit ambe_fr[4][24]);

void playMbeFiles(dsd_opts* opts, dsd_state* state, int argc, char** argv);

/** Purge queued/working audio and vocoder history for one logical voice slot. */
void dsd_mbe_purge_slot_audio(dsd_state* state, int slot);

#ifdef __cplusplus
}
#endif
