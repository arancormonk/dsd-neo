// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Half-band FIR coefficient definitions.
 *
 * Defines the fixed coefficient sets consumed by the SIMD half-band decimators.
 */

#include <dsd-neo/dsp/halfband.h>

/*
 * Half-band decimator coefficients.
 *
 * Normalization
 * -------------
 * A true half-band FIR has two simultaneous constraints:
 *   (a) h[center] = 1/2 exactly, and
 *   (b) sum(h[k]) = 1 (unity DC gain).
 *
 * With Q15-rounded side taps these two constraints are only compatible when
 * the side taps happen to sum to 2^14 exactly. The coefficients below satisfy
 * (b) to bit precision (all sums equal 32768/32768 = 1.0), and (a) is met to
 * within one Q15 ulp (|h[center] - 0.5| <= 2/32768 ≈ 6.1e-5). This trade-off
 * keeps the DC gain numerically exact — the property that matters most for
 * cascading stages — at the cost of a sub-LSB asymmetry around the half-band
 * frequency, which is far below the passband/stopband ripple of the design.
 *
 * Symmetry: odd-indexed taps are zero except for the center tap, enabling the
 * standard N/4+1-multiply halfband convolution in simd_hb_decim2_real().
 */
const float hb_q15_taps[HB_TAPS] = {-108.0f / 32768.0f, 0.0f, 1800.0f / 32768.0f, 0.0f, -500.0f / 32768.0f, 0.0f,
                                    7000.0f / 32768.0f, 0.5f, 7000.0f / 32768.0f, 0.0f, -500.0f / 32768.0f, 0.0f,
                                    1800.0f / 32768.0f, 0.0f, -108.0f / 32768.0f};

/*
 * Higher-order half-band coefficients generated via windowed-sinc (Blackman).
 * Same normalization trade-off as hb_q15_taps: sum(h) = 1 is exact; the center
 * tap is within one Q15 ulp of 0.5 (16386/32768).
 */
const float hb31_q15_taps[31] = {0.0f,
                                 0.0f,
                                 13.0f / 32768.0f,
                                 0.0f,
                                 -73.0f / 32768.0f,
                                 0.0f,
                                 233.0f / 32768.0f,
                                 0.0f,
                                 -587.0f / 32768.0f,
                                 0.0f,
                                 1314.0f / 32768.0f,
                                 0.0f,
                                 -2953.0f / 32768.0f,
                                 0.0f,
                                 10244.0f / 32768.0f,
                                 16386.0f / 32768.0f,
                                 10244.0f / 32768.0f,
                                 0.0f,
                                 -2953.0f / 32768.0f,
                                 0.0f,
                                 1314.0f / 32768.0f,
                                 0.0f,
                                 -587.0f / 32768.0f,
                                 0.0f,
                                 233.0f / 32768.0f,
                                 0.0f,
                                 -73.0f / 32768.0f,
                                 0.0f,
                                 13.0f / 32768.0f,
                                 0.0f,
                                 0.0f};
