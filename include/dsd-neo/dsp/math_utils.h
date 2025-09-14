// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Common lightweight math utilities used across DSP modules.
 */

#pragma once

#include <math.h>
#include <stdint.h>

/**
 * @brief Saturate a 32-bit integer to the 16-bit signed range.
 *
 * Clamps the provided 32-bit value to the inclusive range [-32768, 32767]
 * and returns it as a 16-bit signed integer.
 *
 * @param x 32-bit integer input value.
 * @return Clamped 16-bit signed integer.
 */
static inline int16_t
sat16(int32_t x) {
    if (x > 32767) {
        return 32767;
    }
    if (x < -32768) {
        return -32768;
    }
    return (int16_t)x;
}

/**
 * @brief Compute the greatest common divisor using the Euclidean algorithm.
 *
 * Handles negative inputs by using their absolute values. If both inputs are
 * zero, returns 1.
 *
 * @param a First integer.
 * @param b Second integer.
 * @return Greatest common divisor of |a| and |b|, or 1 if both are zero.
 */
static inline int
gcd_int(int a, int b) {
    if (a < 0) {
        a = -a;
    }
    if (b < 0) {
        b = -b;
    }
    while (b != 0) {
        int t = a % b;
        a = b;
        b = t;
    }
    return (a == 0) ? 1 : a;
}

/**
 * @brief Normalized sinc function.
 *
 * Computes sinc(x) = sin(pi*x)/(pi*x) with the special case sinc(0) = 1
 * implemented to avoid division by zero.
 *
 * @param x Input value.
 * @return Normalized sinc evaluated at x.
 */
static inline double
dsd_neo_sinc(double x) {
    if (x == 0.0) {
        return 1.0;
    }
    const double kPiLocal = 3.14159265358979323846;
    return sin(kPiLocal * x) / (kPiLocal * x);
}
