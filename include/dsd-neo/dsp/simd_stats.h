// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_DSP_SIMD_STATS_H
#define DSD_NEO_DSP_SIMD_STATS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Average the two smallest and two largest samples, counting duplicate values
 * separately. Null input or fewer than two samples produces zero. Either output
 * may be null. Non-finite samples and signed zeros retain scalar comparison order.
 */
void dsd_simd_extrema_avg2(const float* samples, size_t count, float* out_min, float* out_max);

/**
 * Measure interleaved I/Q magnitudes. The mean is the sum of both component
 * magnitudes, accumulated in double precision, divided by the complex sample
 * count. The peak is the largest component magnitude. Null input or zero pairs
 * produces zero; either output may be null. Input contains exactly 2 * pairs
 * floats and may be unaligned. Non-finite input follows the scalar reduction.
 */
void dsd_simd_iq_abs_stats(const float* iq, size_t pairs, float* mean_abs, float* max_abs);

#ifdef __cplusplus
}
#endif

#endif
