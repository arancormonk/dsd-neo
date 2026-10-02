// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief u8 IQ widening and optional 90° IQ rotation API.
 *
 * Exposes wrappers that convert RTL-SDR unsigned 8-bit I/Q samples to
 * normalized float baseband in [-1.0, 1.0] with optional 90° rotation,
 * and the bounded copy, with the same optional rotation, that external
 * CF32 samples enter the decoder through.
 */
#ifndef DSD_NEO_SIMD_WIDEN_H
#define DSD_NEO_SIMD_WIDEN_H

#include <dsd-neo/core/input_level.h>

#include <stddef.h>
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
 * @brief Widen CU8 bytes and accumulate their exact raw integer moments.
 *
 * The conversion is identical to widen_u8_to_f32_bias127(). Moments are
 * collected from the unmodified source bytes and merged into @p moments once
 * per call.
 */
void widen_u8_to_f32_bias127_moments(const unsigned char* src, float* dst, uint32_t len,
                                     dsd_input_level_cu8_moments* moments);

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
 * @brief Rotate/widen CU8 pairs and accumulate pre-rotation integer moments.
 *
 * As with the conversion-only API, only complete I/Q pairs are processed.
 * Moments therefore cover `floor(len / 2) * 2` source bytes.
 */
uint32_t widen_rotate90_u8_to_f32_bias127_phase_moments(const unsigned char* src, float* dst, uint32_t len,
                                                        uint32_t phase, dsd_input_level_cu8_moments* moments);

/**
 * @brief Copy external CF32 I/Q into the decoder, every component bounded.
 *
 * For samples from a source nothing has checked (a SoapySDR driver's CF32 buffer, a cf32 I/Q replay): each component
 * that is Inf, NaN or at least 2^60 in magnitude enters as +0.0f, every other one bit for bit. The test runs on the
 * component's bits, so it holds under fast-math, and a replaced component never reaches a floating-point operation.
 * @param src `pairs` complex samples, I then Q, at any alignment.
 * @param dst Destination, `2 * pairs` floats; must not overlap @p src.
 * @param pairs Number of complex samples.
 */
void bound_cf32_to_f32(const void* src, float* dst, size_t pairs);

/**
 * @brief bound_cf32_to_f32() with the `j^n` (fs/4) rotation from an explicit phase.
 *
 * Sample n is rotated at phase `(phase + n) & 3` as widen_rotate90_u8_to_f32_bias127_phase() rotates it (phase 0
 * leaves it, 1 gives (-Q, I), 2 (-I, -Q), 3 (Q, -I)), after it is bounded; the rotation works on the bits, so the
 * output is bit-identical to rotating the bounded floats.
 * @param src `pairs` complex samples, I then Q, at any alignment.
 * @param dst Destination, `2 * pairs` floats; must not overlap @p src.
 * @param pairs Number of complex samples.
 * @param phase Rotation phase of the first sample in [0, 3]; other bits are ignored.
 * @return Rotation phase for the sample after the last.
 */
uint32_t bound_rotate90_cf32_to_f32_phase(const void* src, float* dst, size_t pairs, uint32_t phase);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SIMD_WIDEN_H */
