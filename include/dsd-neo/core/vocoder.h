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

void processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24],
                     char imbe7100_fr[7][24]);
void processMbeFrameSoft(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit imbe_fr[8][23],
                         dsd_vocoder_soft_bit ambe_fr[4][24], dsd_vocoder_soft_bit imbe7100_fr[7][24]);
void soft_mbe(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]);
void playMbeFiles(dsd_opts* opts, dsd_state* state, int argc, char** argv);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_VOCODER_H_H */
