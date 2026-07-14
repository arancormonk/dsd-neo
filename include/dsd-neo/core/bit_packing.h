// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Bit/byte packing helpers shared across modules.
 *
 * Declares conversion utilities implemented in core.
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

/** @brief Pack an array of bits into bytes (length multiple of 8). */
static inline void
pack_bit_array_into_byte_array(const uint8_t* input, uint8_t* output, int len) {
    if (!input || !output || len <= 0) {
        return;
    }
    for (int i = 0; i < len; i++) {
        const uint8_t* bits = &input[(size_t)i * 8U];
        output[i] =
            (uint8_t)(((bits[0] & 1U) << 7U) | ((bits[1] & 1U) << 6U) | ((bits[2] & 1U) << 5U) | ((bits[3] & 1U) << 4U)
                      | ((bits[4] & 1U) << 3U) | ((bits[5] & 1U) << 2U) | ((bits[6] & 1U) << 1U) | (bits[7] & 1U));
    }
}

/** @brief Unpack a byte array into individual bits (MSB-first per octet). */
static inline void
unpack_byte_array_into_bit_array(const uint8_t* input, uint8_t* output, int len) {
    if (!input || !output || len <= 0) {
        return;
    }
    for (int i = 0; i < len; i++) {
        for (unsigned int bit = 0U; bit < 8U; bit++) {
            output[(size_t)i * 8U + bit] = (uint8_t)((input[i] >> (7U - bit)) & 1U);
        }
    }
}

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

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_BIT_PACKING_H_H */
