// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Polar discriminator API declarations.
 *
 * Declares accurate, fast integer, and LUT-based FM phase discriminators.
 */

#pragma once

#include <stdint.h>

/**
 * @brief Initialize the atan lookup table used by the LUT-based discriminator.
 *
 * Thread-safe and idempotent. If memory cannot be allocated, the function
 * leaves the LUT disabled and returns -1.
 *
 * @return 0 on success, -1 on allocation failure.
 */
int atan_lut_init(void);
/**
 * @brief Free memory associated with the atan LUT.
 *
 * Safe to call multiple times.
 */
void atan_lut_free(void);

/**
 * @brief Accurate polar discriminator using double-precision atan2.
 *
 * Computes b * conj(a) and returns the phase delta in Q14 where
 * pi == 1<<14.
 *
 * @param ar Real part of previous complex sample a.
 * @param aj Imag part of previous complex sample a.
 * @param br Real part of current complex sample b.
 * @param bj Imag part of current complex sample b.
 * @return Phase difference in Q14 units.
 */
int polar_discriminant(int ar, int aj, int br, int bj);
/**
 * @brief Fast polar discriminator using an integer atan2 approximation.
 *
 * Uses a low-cost integer approximation to atan2 with 64-bit safety.
 * Returns the phase delta in Q14 where pi == 1<<14.
 *
 * @param ar Real part of previous complex sample a.
 * @param aj Imag part of previous complex sample a.
 * @param br Real part of current complex sample b.
 * @param bj Imag part of current complex sample b.
 * @return Phase difference in Q14 units.
 */
int polar_disc_fast(int ar, int aj, int br, int bj);
/**
 * @brief LUT-based polar discriminator.
 *
 * Uses a precomputed atan LUT to approximate atan2. Falls back to the fast
 * integer approximation if the LUT is not available.
 *
 * @param ar Real part of previous complex sample a.
 * @param aj Imag part of previous complex sample a.
 * @param br Real part of current complex sample b.
 * @param bj Imag part of current complex sample b.
 * @return Phase difference in Q14 units.
 */
int polar_disc_lut(int ar, int aj, int br, int bj);
