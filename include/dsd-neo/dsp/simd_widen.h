// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief SIMD widening and optional 90° IQ rotation API with runtime dispatch.
 *
 * Exposes wrappers that convert RTL-SDR unsigned 8-bit I/Q samples to signed
 * 16-bit integers, with optional 90° rotation. Runtime feature detection
 * selects scalar or SIMD specializations (AVX2, SSSE3/SSE2, NEON).
 */
#ifndef DSD_NEO_SIMD_WIDEN_H
#define DSD_NEO_SIMD_WIDEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Function pointer types for runtime dispatch */
/**
 * @brief Function pointer for widening u8 to s16 centered at 127.
 *
 * @param src Source buffer of unsigned bytes (I/Q interleaved).
 * @param dst Destination int16 buffer.
 * @param len Number of bytes in src to process.
 */
typedef void (*dsd_neo_widen_fn)(const unsigned char*, int16_t*, uint32_t);
/**
 * @brief Function pointer for 90° IQ rotation + widen u8→s16 centered at 127.
 *
 * @param src Source buffer of unsigned bytes (I/Q interleaved).
 * @param dst Destination int16 buffer.
 * @param len Number of bytes in src to process.
 */
typedef void (*dsd_neo_widen_rot_fn)(const unsigned char*, int16_t*, uint32_t);

/**
 * @brief Widen u8 to s16 centered at 127 via runtime-dispatched implementation.
 *
 * Public wrapper that lazy-initializes runtime dispatch and widens u8 to s16
 * centered at 127.
 *
 * @param src Source buffer of unsigned bytes (I/Q interleaved).
 * @param dst Destination int16 buffer.
 * @param len Number of bytes in src to process.
 */
void widen_u8_to_s16_bias127(const unsigned char* src, int16_t* dst, uint32_t len);

/**
 * @brief Rotate 90° (IQ) and widen u8→s16 centered at 127 via runtime dispatch.
 *
 * Public wrapper that lazy-initializes runtime dispatch and performs 90° IQ
 * rotation combined with widen u8→s16 centered at 127.
 *
 * @param src Source buffer of unsigned bytes (I/Q interleaved).
 * @param dst Destination int16 buffer.
 * @param len Number of bytes in src to process.
 */
void widen_rotate90_u8_to_s16_bias127(const unsigned char* src, int16_t* dst, uint32_t len);

/**
 * @brief Widen u8 to s16 centered at 128 (for legacy pre-rotation negation).
 *
 * Scalar widening that subtracts 128 instead of 127. Intended to pair with
 * legacy byte-wise rotate_90(u8) which performs 255-x negation so that overall
 * effect equals correct centered negation (127-x).
 *
 * @param src Source buffer of unsigned bytes.
 * @param dst Destination int16 buffer.
 * @param len Number of bytes to process.
 */
void widen_u8_to_s16_bias128_scalar(const unsigned char* src, int16_t* dst, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SIMD_WIDEN_H */
