// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Bit/byte packing helpers shared across modules.
 *
 * Declares conversion utilities implemented in core.
 *
 * The bit/byte conversion helpers are capacity-aware: every call states the
 * span of both buffers, and the conversion is clamped to whichever of the
 * source, the destination, or the request is tightest. Callers that must
 * reject malformed input compare the returned count against the request.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_BIT_PACKING_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_BIT_PACKING_H_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse a user-supplied hex string into an octet buffer.
 *
 * Converts ASCII hex into bytes with bounds checking; excess characters are
 * ignored once @p out_cap is reached.
 *
 * @param input  Null-terminated hex string.
 * @param output Destination buffer for parsed bytes.
 * @param out_cap Capacity of @p output in bytes.
 * @return Number of bytes written (<= out_cap).
 */
uint16_t parse_raw_user_string(const char* input, uint8_t* output, size_t out_cap);

/** @brief Convert a packed bit array into an integer output value (MSB-first). */
uint64_t convert_bits_into_output(const uint8_t* input, uint32_t len);

/**
 * @brief Pack unpacked bits into octets, MSB-first, bounded by both buffers.
 *
 * Converts at most `min(requested_bytes, input_capacity_bits / 8,
 * output_capacity_bytes)` complete octets. Partial octets are never emitted.
 * A NULL pointer or a zero capacity converts nothing and writes nothing.
 *
 * @param input                Unpacked bits; one bit per element, LSB significant.
 * @param input_capacity_bits  Number of readable elements in @p input.
 * @param output               Destination octets.
 * @param output_capacity_bytes Number of writable octets in @p output.
 * @param requested_bytes      Octets the caller would like converted.
 * @return Number of octets actually converted.
 */
size_t dsd_pack_bits_to_bytes(const uint8_t* input, size_t input_capacity_bits, uint8_t* output,
                              size_t output_capacity_bytes, size_t requested_bytes);

/**
 * @brief As dsd_pack_bits_to_bytes(), for sites where truncation is intended.
 *
 * Semantically identical. Use this where a short conversion is a valid outcome
 * for malformed input, so the test-build truncation counter stays a signal
 * about capacity mistakes rather than about hostile traffic.
 */
size_t dsd_pack_bits_to_bytes_truncating(const uint8_t* input, size_t input_capacity_bits, uint8_t* output,
                                         size_t output_capacity_bytes, size_t requested_bytes);

/**
 * @brief Unpack octets into individual bits, MSB-first, bounded by both buffers.
 *
 * Converts at most `min(requested_bytes, input_capacity_bytes,
 * output_capacity_bits / 8)` complete octets, writing eight elements per
 * octet. A NULL pointer or a zero capacity converts nothing and writes
 * nothing.
 *
 * @param input                 Source octets.
 * @param input_capacity_bytes  Number of readable octets in @p input.
 * @param output                Destination bits; one bit per element.
 * @param output_capacity_bits  Number of writable elements in @p output.
 * @param requested_bytes       Octets the caller would like converted.
 * @return Number of octets actually converted.
 */
size_t dsd_unpack_bytes_to_bits(const uint8_t* input, size_t input_capacity_bytes, uint8_t* output,
                                size_t output_capacity_bits, size_t requested_bytes);

/**
 * @brief As dsd_unpack_bytes_to_bits(), for sites where truncation is intended.
 *
 * @see dsd_pack_bits_to_bytes_truncating()
 */
size_t dsd_unpack_bytes_to_bits_truncating(const uint8_t* input, size_t input_capacity_bytes, uint8_t* output,
                                           size_t output_capacity_bits, size_t requested_bytes);

/**
 * @brief Evaluate to zero, or fail to compile when @p a is not an array.
 *
 * Guards the array convenience macros below against pointer decay, where
 * sizeof() would silently yield the pointer width instead of the buffer span.
 * Compilers without __typeof__ (MSVC) fall back to runtime bounds only.
 */
#if !defined(__cplusplus) && (defined(__GNUC__) || defined(__clang__))
#define DSD_MUST_BE_ARRAY(a)                                                                                           \
    (sizeof(struct {                                                                                                   \
         int dsd_argument_is_not_an_array                                                                              \
             : (__builtin_types_compatible_p(__typeof__(a), __typeof__(&(a)[0])) ? -1 : 1);                            \
     })                                                                                                                \
     * 0U)
#else
#define DSD_MUST_BE_ARRAY(a) 0U
#endif

/** @brief dsd_unpack_bytes_to_bits() with both capacities taken from array operands. */
#define DSD_UNPACK_ARRAY_TO_BITS(in, out, n)                                                                           \
    dsd_unpack_bytes_to_bits((in), sizeof(in) + DSD_MUST_BE_ARRAY(in), (out), sizeof(out) + DSD_MUST_BE_ARRAY(out), (n))

/** @brief dsd_unpack_bytes_to_bits_truncating() with both capacities taken from array operands. */
#define DSD_UNPACK_ARRAY_TO_BITS_TRUNCATING(in, out, n)                                                                \
    dsd_unpack_bytes_to_bits_truncating((in), sizeof(in) + DSD_MUST_BE_ARRAY(in), (out),                               \
                                        sizeof(out) + DSD_MUST_BE_ARRAY(out), (n))

/** @brief dsd_pack_bits_to_bytes() with both capacities taken from array operands. */
#define DSD_PACK_ARRAY_TO_BYTES(in, out, n)                                                                            \
    dsd_pack_bits_to_bytes((in), sizeof(in) + DSD_MUST_BE_ARRAY(in), (out), sizeof(out) + DSD_MUST_BE_ARRAY(out), (n))

/**
 * @brief Compute CRC-CCITT over an array of unpacked bits.
 *
 * Uses polynomial 0x1021, an initial value of zero, and a final XOR of 0xFFFF.
 * Each input element contributes its least-significant bit.
 *
 * @return The computed CRC, or zero when @p input is NULL.
 */
uint16_t dsd_crc_ccitt16_bits(const uint8_t* input, size_t bit_count);

/** @brief Pack AMBE bits into a contiguous byte buffer. */
void pack_ambe(const char* input, uint8_t* output, int len);
/** @brief Unpack AMBE bytes back into bit form. */
void unpack_ambe(const uint8_t* input, char* ambe);

#ifdef DSD_NEO_TEST_HOOKS
/**
 * @brief Count of conversions that returned fewer octets than requested.
 *
 * Only the non-truncating entry points contribute. A nonzero count in a test
 * run means a call site declared a capacity smaller than the data it meant to
 * convert, which would otherwise show up as a silent decode regression.
 */
unsigned long dsd_bit_packing_unexpected_truncations(void);

/** @brief Reset the counter read by dsd_bit_packing_unexpected_truncations(). */
void dsd_bit_packing_reset_truncation_stats(void);
#endif

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_BIT_PACKING_H_H */
