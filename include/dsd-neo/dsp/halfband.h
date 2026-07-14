// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Half-band decimation coefficient definitions (float).
 *
 * Declares fixed FIR coefficient sets for the SIMD half-band decimators.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_HALFBAND_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_HALFBAND_H_

/**
 * Number of taps for the half-band FIR low-pass filter.
 */
#define HB_TAPS 15

/**
 * Symmetric half-band coefficients normalized to unity DC gain.
 *
 * Odd-indexed taps are zero; the center tap is 0.5. The remaining even
 * taps sum to 0.5 to yield unity DC gain.
 */
extern const float hb_q15_taps[HB_TAPS];

/* Higher-order half-band prototype used by the first decimation stage. */
extern const float hb31_q15_taps[31];

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_HALFBAND_H_ */
