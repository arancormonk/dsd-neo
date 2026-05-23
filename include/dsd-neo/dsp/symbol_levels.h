// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_SYMBOL_LEVELS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_SYMBOL_LEVELS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Map a dibit value (0..3) to a nominal 4-level symbol amplitude.
 *
 * The project uses a consistent dibit ordering across protocols:
 * - 0 -> +1
 * - 1 -> +3
 * - 2 -> -1
 * - 3 -> -3
 *
 * This helper is used for symbol-capture playback where the input stream is
 * already digitized into dibits (e.g. `.bin` symbol capture files).
 */
float dsd_symbol_level_from_dibit(uint8_t dibit);

typedef struct dsd_fsk_soft_symbol_metrics {
    int levels;
    float symbol;
    float ideal;
    float error;
    uint8_t reliability;
    int clipped;
} dsd_fsk_soft_symbol_metrics;

/**
 * @brief Compute a hard-decision-relative soft metric for normalized FSK symbols.
 *
 * This helper only measures distance from the nearest nominal symbol level. It
 * does not alter slicer thresholds or decoded dibit decisions.
 *
 * @param symbol Normalized symbol value (2-level: +/-1, 4-level: +/-1,+/-3).
 * @param levels Number of FSK levels; values other than 2 are treated as 4.
 * @return Per-symbol soft metric snapshot.
 */
dsd_fsk_soft_symbol_metrics dsd_fsk_soft_symbol_metrics_from_symbol(float symbol, int levels);

/**
 * @brief Return the 0..255 reliability component of
 * dsd_fsk_soft_symbol_metrics_from_symbol().
 */
uint8_t dsd_fsk_symbol_reliability(float symbol, int levels);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_SYMBOL_LEVELS_H_ */
