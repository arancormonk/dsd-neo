// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief u8 IQ widening and optional 90° IQ rotation API.
 *
 * Exposes wrappers that convert RTL-SDR unsigned 8-bit I/Q samples to
 * normalized float baseband in [-1.0, 1.0] with optional 90° rotation.
 * The current implementation is scalar.
 */
#ifndef DSD_NEO_SIMD_WIDEN_H
#define DSD_NEO_SIMD_WIDEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Function pointer typedefs retained for API stability and future specialization. */
/**
 * @brief Function pointer for widening u8 to float centered at 127.5.
 *
 * @param src Source buffer of unsigned bytes (I/Q interleaved).
 * @param dst Destination float buffer.
 * @param len Number of bytes in src to process.
 */
typedef void (*dsd_neo_widen_fn)(const unsigned char*, float*, uint32_t);
/**
 * @brief Function pointer for 90° IQ rotation + widen u8→float centered at 127.5.
 *
 * @param src Source buffer of unsigned bytes (I/Q interleaved).
 * @param dst Destination float buffer.
 * @param len Number of bytes in src to process.
 */
typedef void (*dsd_neo_widen_rot_fn)(const unsigned char*, float*, uint32_t);

/**
 * @brief Widen u8 to float centered at 127.5.
 *
 * Widens u8 to normalized float centered at 127.5.
 *
 * @param src Source buffer of unsigned bytes (I/Q interleaved).
 * @param dst Destination float buffer.
 * @param len Number of bytes in src to process.
 */
void widen_u8_to_f32_bias127(const unsigned char* src, float* dst, uint32_t len);

/**
 * @brief Rotate 90° (IQ) and widen u8→float centered at 127.5.
 *
 * Performs 90° IQ rotation combined with widen u8→float centered at 127.5.
 *
 * @param src Source buffer of unsigned bytes (I/Q interleaved).
 * @param dst Destination float buffer.
 * @param len Number of bytes in src to process.
 */
void widen_rotate90_u8_to_f32_bias127(const unsigned char* src, float* dst, uint32_t len);

/**
 * @brief Rotate 90° (IQ) and widen u8→float centered at 127.5 with explicit phase.
 *
 * Applies the `j^n` sequence starting at `phase & 3`, where phase 0 leaves the
 * first I/Q pair unchanged. Processes `floor(len/2)` pairs and returns the phase
 * to use for the next pair-aligned chunk.
 *
 * Callers must preserve I/Q byte alignment themselves. If a transport split can
 * leave one dangling byte, buffer that byte externally and resume with a
 * pair-aligned span on the next call.
 *
 * @param src Source buffer of unsigned bytes (I/Q interleaved).
 * @param dst Destination float buffer.
 * @param len Number of bytes in src to process.
 * @param phase Starting rotation phase in [0, 3]; other bits are ignored.
 * @return Next rotation phase after processing the available I/Q pairs.
 */
uint32_t widen_rotate90_u8_to_f32_bias127_phase(const unsigned char* src, float* dst, uint32_t len, uint32_t phase);

/**
 * @brief Legacy in-place byte-domain 90° IQ rotation with explicit phase.
 *
 * Rotates interleaved u8 I/Q samples using byte-domain negation (`255 - x`).
 * Intended to pair with `widen_u8_to_f32_bias128_scalar()` for the legacy
 * two-pass RTL path. Processes `floor(len/2)` pairs and returns the phase to
 * use for the next pair-aligned chunk.
 *
 * Callers must preserve I/Q byte alignment themselves. If a transport split can
 * leave one dangling byte, buffer that byte externally and resume with a
 * pair-aligned span on the next call.
 *
 * @param buf Buffer of interleaved I/Q bytes to rotate in place.
 * @param len Number of bytes in buf to process.
 * @param phase Starting rotation phase in [0, 3]; other bits are ignored.
 * @return Next rotation phase after processing the available I/Q pairs.
 */
uint32_t rotate90_u8_inplace_phase(unsigned char* buf, uint32_t len, uint32_t phase);

/**
 * @brief Widen u8 to float centered at 128 (for legacy pre-rotation negation).
 *
 * Scalar widening that subtracts 128 instead of 127. Intended to pair with
 * legacy byte-wise `rotate90_u8_inplace_phase()` which performs 255-x negation.
 *
 * @param src Source buffer of unsigned bytes.
 * @param dst Destination float buffer.
 * @param len Number of bytes to process.
 */
void widen_u8_to_f32_bias128_scalar(const unsigned char* src, float* dst, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SIMD_WIDEN_H */
