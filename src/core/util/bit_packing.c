// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/bit_packing.h>

#ifdef DSD_NEO_TEST_HOOKS
static unsigned long g_unexpected_truncations = 0UL;

unsigned long
dsd_bit_packing_unexpected_truncations(void) {
    return g_unexpected_truncations;
}

void
dsd_bit_packing_reset_truncation_stats(void) {
    g_unexpected_truncations = 0UL;
}

static void
note_truncation(size_t requested, size_t converted) {
    if (converted < requested) {
        g_unexpected_truncations++;
    }
}
#else
#define note_truncation(requested, converted) ((void)0)
#endif

/**
 * @brief Smallest of the request and the two buffer spans.
 *
 * Stating the bound here, rather than at each call site, is what keeps the
 * conversion loops provably in range for the vectorizer.
 */
static size_t
clamp_to_capacity(size_t requested, size_t src_limit, size_t dst_limit) {
    size_t count = requested;
    if (count > src_limit) {
        count = src_limit;
    }
    if (count > dst_limit) {
        count = dst_limit;
    }
    return count;
}

static size_t
unpack_bounded(const uint8_t* input, size_t input_capacity_bytes, uint8_t* output, size_t output_capacity_bits,
               size_t requested_bytes) {
    if (input == NULL || output == NULL) {
        return 0U;
    }

    const size_t count = clamp_to_capacity(requested_bytes, input_capacity_bytes, output_capacity_bits / 8U);
    for (size_t i = 0U; i < count; i++) {
        const uint8_t octet = input[i];
        uint8_t* bits = &output[i * 8U];
        bits[0] = (uint8_t)((octet >> 7U) & 1U);
        bits[1] = (uint8_t)((octet >> 6U) & 1U);
        bits[2] = (uint8_t)((octet >> 5U) & 1U);
        bits[3] = (uint8_t)((octet >> 4U) & 1U);
        bits[4] = (uint8_t)((octet >> 3U) & 1U);
        bits[5] = (uint8_t)((octet >> 2U) & 1U);
        bits[6] = (uint8_t)((octet >> 1U) & 1U);
        bits[7] = (uint8_t)(octet & 1U);
    }
    return count;
}

static size_t
pack_bounded(const uint8_t* input, size_t input_capacity_bits, uint8_t* output, size_t output_capacity_bytes,
             size_t requested_bytes) {
    if (input == NULL || output == NULL) {
        return 0U;
    }

    const size_t count = clamp_to_capacity(requested_bytes, input_capacity_bits / 8U, output_capacity_bytes);
    for (size_t i = 0U; i < count; i++) {
        const uint8_t* bits = &input[i * 8U];
        output[i] =
            (uint8_t)(((bits[0] & 1U) << 7U) | ((bits[1] & 1U) << 6U) | ((bits[2] & 1U) << 5U) | ((bits[3] & 1U) << 4U)
                      | ((bits[4] & 1U) << 3U) | ((bits[5] & 1U) << 2U) | ((bits[6] & 1U) << 1U) | (bits[7] & 1U));
    }
    return count;
}

size_t
dsd_unpack_bytes_to_bits(const uint8_t* input, size_t input_capacity_bytes, uint8_t* output,
                         size_t output_capacity_bits, size_t requested_bytes) {
    const size_t count = unpack_bounded(input, input_capacity_bytes, output, output_capacity_bits, requested_bytes);
    note_truncation(requested_bytes, count);
    return count;
}

size_t
dsd_unpack_bytes_to_bits_truncating(const uint8_t* input, size_t input_capacity_bytes, uint8_t* output,
                                    size_t output_capacity_bits, size_t requested_bytes) {
    return unpack_bounded(input, input_capacity_bytes, output, output_capacity_bits, requested_bytes);
}

size_t
dsd_pack_bits_to_bytes(const uint8_t* input, size_t input_capacity_bits, uint8_t* output, size_t output_capacity_bytes,
                       size_t requested_bytes) {
    const size_t count = pack_bounded(input, input_capacity_bits, output, output_capacity_bytes, requested_bytes);
    note_truncation(requested_bytes, count);
    return count;
}

size_t
dsd_pack_bits_to_bytes_truncating(const uint8_t* input, size_t input_capacity_bits, uint8_t* output,
                                  size_t output_capacity_bytes, size_t requested_bytes) {
    return pack_bounded(input, input_capacity_bits, output, output_capacity_bytes, requested_bytes);
}

static inline uint64_t
pack_8_bits_msb(const uint8_t* bits) {
    return ((uint64_t)(bits[0] & 1U) << 7) | ((uint64_t)(bits[1] & 1U) << 6) | ((uint64_t)(bits[2] & 1U) << 5)
           | ((uint64_t)(bits[3] & 1U) << 4) | ((uint64_t)(bits[4] & 1U) << 3) | ((uint64_t)(bits[5] & 1U) << 2)
           | ((uint64_t)(bits[6] & 1U) << 1) | (uint64_t)(bits[7] & 1U);
}

uint64_t
convert_bits_into_output(const uint8_t* input, uint32_t len) {
    uint64_t output = 0;
    const uint8_t* bits = input;

    while (len >= 8U) {
        output = (output << 8) | pack_8_bits_msb(bits);
        bits += 8;
        len -= 8U;
    }
    while (len-- > 0U) {
        output = (output << 1) | (uint64_t)(*bits++ & 1U);
    }
    return output;
}

uint16_t
dsd_crc_ccitt16_bits(const uint8_t* input, size_t bit_count) {
    if (input == NULL) {
        return 0U;
    }

    uint16_t crc = 0U;
    for (size_t i = 0U; i < bit_count; i++) {
        const uint16_t feedback = (uint16_t)(((crc >> 15U) & 1U) ^ (input[i] & 1U));
        crc = (uint16_t)(crc << 1U);
        if (feedback != 0U) {
            crc ^= 0x1021U;
        }
    }
    return (uint16_t)(crc ^ 0xFFFFU);
}
