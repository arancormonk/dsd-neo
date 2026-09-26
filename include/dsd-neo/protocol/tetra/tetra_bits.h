// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA shared bit-extraction utility.  Phase 81.
 *
 * Provides a single inline function for reading N bits (MSB-first,
 * one-byte-per-bit representation) as a uint32_t.  Previously each
 * translation unit carried its own static copy; this header unifies
 * them.
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_BITS_H
#define DSD_NEO_PROTOCOL_TETRA_BITS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * tetra_bits_to_uint()
 *
 * Read @n bits starting at @bits[@off] and return them as an unsigned
 * 32-bit integer with MSB-first ordering.
 *
 * Each element of @bits is 0 or 1 (one byte per bit), as produced by
 * the Viterbi decoder in tetra.c.
 *
 * @bits  – decoded bit array
 * @off   – starting bit offset (0-based)
 * @n     – number of bits to read (1..32)
 */
static inline uint32_t tetra_bits_to_uint(const uint8_t *bits, int off, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | (bits[off + i] & 1u);
    return v;
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_BITS_H */
