// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief Bounded MSB-first reads from TETRA bit arrays (one byte per bit, each 0 or 1).
 *
 * Every TETRA PDU parser reads its fields through a tetra_bit_reader. A read that would run past the end of the PDU
 * fails the reader: it returns 0, does not advance, and every later read fails too, so a parser can read a whole
 * element and check tetra_bits_ok() once before publishing anything.
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_BITS_H
#define DSD_NEO_PROTOCOL_TETRA_BITS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t* bits; /**< one bit per byte, MSB first */
    int nbits;           /**< bits available */
    int pos;             /**< next bit to read */
    int failed;          /**< set once a read ran past nbits; sticky */
} tetra_bit_reader;

/** A reader over @p nbits bits of @p bits, starting at bit @p pos. */
static inline tetra_bit_reader
tetra_bits_reader(const uint8_t* bits, int nbits, int pos) {
    tetra_bit_reader r;
    r.bits = bits;
    r.nbits = nbits > 0 ? nbits : 0;
    r.pos = pos;
    r.failed = (bits == NULL || pos < 0 || pos > r.nbits) ? 1 : 0;
    return r;
}

/** Bits left to read; 0 once the reader has failed. */
static inline int
tetra_bits_left(const tetra_bit_reader* r) {
    return r->failed ? 0 : r->nbits - r->pos;
}

/** 1 while every read so far fitted. */
static inline int
tetra_bits_ok(const tetra_bit_reader* r) {
    return r->failed == 0;
}

/** Read @p width (0..64) bits MSB first; 0 and a failed reader when they do not fit. */
static inline uint64_t
tetra_bits_read64(tetra_bit_reader* r, int width) {
    if (r->failed || width < 0 || width > 64 || width > r->nbits - r->pos) {
        r->failed = 1;
        return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < width; i++) {
        v = (v << 1) | (uint64_t)(r->bits[r->pos + i] & 1u);
    }
    r->pos += width;
    return v;
}

/** Read @p width (0..32) bits MSB first; 0 and a failed reader when they do not fit. */
static inline uint32_t
tetra_bits_read(tetra_bit_reader* r, int width) {
    if (width > 32) {
        r->failed = 1;
        return 0;
    }
    return (uint32_t)tetra_bits_read64(r, width);
}

/** Skip @p width bits; 1 when they were there. */
static inline int
tetra_bits_skip(tetra_bit_reader* r, int width) {
    if (r->failed || width < 0 || width > r->nbits - r->pos) {
        r->failed = 1;
        return 0;
    }
    r->pos += width;
    return 1;
}

/** Read a type-2 element's presence bit (P-bit, EN 300 392-2 clauses 14.7, 16.10 and 18.4): 1 when the element
 * follows. */
static inline int
tetra_bits_present(tetra_bit_reader* r) {
    return tetra_bits_read(r, 1) != 0 && tetra_bits_ok(r);
}

/** Skip a chain of type-3/4 elements: each starts with an M-bit of 1, a 4-bit identifier and an 11-bit length, and
 * the chain ends with an M-bit of 0. 1 when the chain ended inside the PDU. */
static inline int
tetra_bits_skip_type34(tetra_bit_reader* r) {
    while (tetra_bits_read(r, 1) != 0) {
        (void)tetra_bits_read(r, 4);
        const uint32_t length = tetra_bits_read(r, 11);
        if (!tetra_bits_ok(r) || length == 0 || !tetra_bits_skip(r, (int)length)) {
            r->failed = 1;
            return 0;
        }
    }
    return tetra_bits_ok(r);
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_BITS_H */
