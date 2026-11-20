// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A dPMR header and end frame encoder for tests, written from ETSI TS 102 658 V2.6.1 in the transmit direction, which
 * the decoder never implements, so its frames are ground truth for the decoder rather than a snapshot of it:
 *
 *  - MI (7.6, 11.5, Figure 11.8): 72 bits, CRC-8 X^8 + X^2 + X + 1 (7.1) by long division, ten Hamming(12,8) words
 *    from the generator matrix (Table 7.2), the 12 x 10 interleave written column by column and read out row by row
 *    (Table 7.4), the scrambler (7.3); copied after the channel code.
 *  - END (7.7, 11.6, Figure 11.9): 17 bits, CRC-7 X^7 + X^3 + 1, three Hamming(12,8) words, each 36-bit copy scrambled
 *    with the scrambler's bits from a given offset: 0 for a fresh scrambler (11.6), 36 for the second copy of one
 *    72-bit run (7.7, and the off-air recording behind tests/fixtures/iq/dpmr.iq).
 *
 * The scrambler's sequence is the committed 72-bit golden of DPMR_SCRAMBLER (from the X^9 + X^5 + 1 polynomial and its
 * all-ones preset, and the keystream the CCH reference vectors were cross-derived with), carried on by the
 * polynomial's own recurrence s[n + 9] = s[n + 4] ^ s[n].
 */

#ifndef DSD_NEO_TESTS_PROTOCOL_DPMR_DPMR_SPEC_ENCODER_H_
#define DSD_NEO_TESTS_PROTOCOL_DPMR_DPMR_SPEC_ENCODER_H_

#include <stdint.h>
#include <string.h>

enum {
    DPMR_SPEC_MI_BITS = 72,
    DPMR_SPEC_MI_WIRE = 120,
    DPMR_SPEC_END_BITS = 17,
    DPMR_SPEC_END_WIRE = 36,
    /* Preamble (6.1.6, 72 bits of 5F), FS1, MI0, CC, MI1, in dibits. */
    DPMR_SPEC_PREAMBLE_DIBITS = 36,
    DPMR_SPEC_HEADER_DIBITS = 24 + 60 + 12 + 60,
    DPMR_SPEC_END_FRAME_DIBITS = 12 + 36,
};

static const char k_dpmr_spec_keystream_golden[] = "111111111000001111011111000101110011"
                                                   "001000001001010011101101000111100111";

/* Table 7.2: X7..1 as rows, C3..C0 the parity each information bit sets. */
static const uint8_t k_dpmr_spec_generator[8][4] = {
    {1, 1, 1, 0}, {0, 1, 1, 1}, {1, 0, 1, 0}, {0, 1, 0, 1}, {1, 0, 1, 1}, {1, 1, 0, 0}, {0, 1, 1, 0}, {0, 0, 1, 1},
};

static inline void
dpmr_spec_keystream(uint8_t* out, int count) {
    for (int i = 0; i < count; i++) {
        out[i] = i < 72 ? (uint8_t)(k_dpmr_spec_keystream_golden[i] - '0') : (uint8_t)(out[i - 9] ^ out[i - 5]);
    }
}

static inline void
dpmr_spec_put(uint8_t* bits, int at, int count, uint32_t value) {
    for (int i = 0; i < count; i++) {
        bits[at + i] = (uint8_t)((value >> (count - 1 - i)) & 1U);
    }
}

/* The remainder of @p count message bits times X^width, divided by the generator @p gen (width + 1 bits, X^width
   first), by long division on a copy. */
static inline uint32_t
dpmr_spec_crc(const uint8_t* bits, int count, const uint8_t* gen, int width) {
    uint8_t work[128];
    memset(work, 0, sizeof(work));
    memcpy(work, bits, (size_t)count);
    for (int i = 0; i < count; i++) {
        if (work[i]) {
            for (int j = 0; j <= width; j++) {
                work[i + j] ^= gen[j];
            }
        }
    }
    uint32_t r = 0;
    for (int j = 0; j < width; j++) {
        r = (r << 1) | work[count + j];
    }
    return r;
}

static inline void
dpmr_spec_hamming(const uint8_t* data8, uint8_t* word12) {
    for (int i = 0; i < 8; i++) {
        word12[i] = data8[i];
    }
    for (int p = 0; p < 4; p++) {
        uint8_t acc = 0;
        for (int i = 0; i < 8; i++) {
            acc ^= (uint8_t)(data8[i] & k_dpmr_spec_generator[i][p]);
        }
        word12[8 + p] = acc;
    }
}

static inline uint32_t
dpmr_spec_crc8(const uint8_t* info72) {
    static const uint8_t crc8[9] = {1, 0, 0, 0, 0, 0, 1, 1, 1}; /* X^8 + X^2 + X + 1 */
    return dpmr_spec_crc(info72, 72, crc8, 8);
}

static inline uint32_t
dpmr_spec_crc7(const uint8_t* info17) {
    static const uint8_t crc7[8] = {1, 0, 0, 0, 1, 0, 0, 1}; /* X^7 + X^3 + 1 */
    return dpmr_spec_crc(info17, 17, crc7, 7);
}

/* One MI copy on the wire from its 72 bits and the CRC-8 @p crc it is sent with. */
static inline void
dpmr_spec_encode_mi_crc(const uint8_t* info72, uint32_t crc, uint8_t* wire120) {
    uint8_t data[80];
    uint8_t coded[120];
    uint8_t key[120];
    memcpy(data, info72, 72);
    dpmr_spec_put(data, 72, 8, crc);
    for (int w = 0; w < 10; w++) {
        dpmr_spec_hamming(data + (w * 8), coded + (w * 12));
    }
    dpmr_spec_keystream(key, 120);
    /* Written down the 12 rows column by column, sent row by row. */
    for (int r = 0; r < 12; r++) {
        for (int c = 0; c < 10; c++) {
            wire120[(r * 10) + c] = (uint8_t)(coded[(c * 12) + r] ^ key[(r * 10) + c]);
        }
    }
}

/* One MI copy on the wire from its 72 bits. */
static inline void
dpmr_spec_encode_mi(const uint8_t* info72, uint8_t* wire120) {
    dpmr_spec_encode_mi_crc(info72, dpmr_spec_crc8(info72), wire120);
}

/* One END copy on the wire from its 17 bits and the CRC-7 @p crc, scrambled with the bits from @p key_offset on. */
static inline void
dpmr_spec_encode_end_crc(const uint8_t* info17, uint32_t crc, int key_offset, uint8_t* wire36) {
    uint8_t data[24];
    uint8_t key[72];
    memcpy(data, info17, 17);
    dpmr_spec_put(data, 17, 7, crc);
    dpmr_spec_keystream(key, 72);
    for (int w = 0; w < 3; w++) {
        dpmr_spec_hamming(data + (w * 8), wire36 + (w * 12));
    }
    for (int i = 0; i < 36; i++) {
        wire36[i] ^= key[key_offset + i];
    }
}

/* One END copy on the wire from its 17 bits. */
static inline void
dpmr_spec_encode_end(const uint8_t* info17, int key_offset, uint8_t* wire36) {
    dpmr_spec_encode_end_crc(info17, dpmr_spec_crc7(info17), key_offset, wire36);
}

/* Header MT and PARMS (Table 5.4). */
typedef struct {
    uint32_t mt, called, calling, mode, version, format, emergency, mi_type, mi_detail;
} dpmr_spec_header;

static inline void
dpmr_spec_header_bits(const dpmr_spec_header* h, uint8_t* info72) {
    memset(info72, 0, 72);
    dpmr_spec_put(info72, 0, 4, h->mt);
    dpmr_spec_put(info72, 4, 24, h->called);
    dpmr_spec_put(info72, 28, 24, h->calling);
    dpmr_spec_put(info72, 52, 3, h->mode);
    dpmr_spec_put(info72, 55, 2, h->version);
    dpmr_spec_put(info72, 57, 2, h->format);
    dpmr_spec_put(info72, 59, 1, h->emergency);
    dpmr_spec_put(info72, 61, 3, h->mi_type);
    dpmr_spec_put(info72, 64, 8, h->mi_detail);
}

/* Bits to dibits, the first bit high. */
static inline void
dpmr_spec_dibits(const uint8_t* bits, int nbits, uint8_t* dibits) {
    for (int i = 0; i < nbits / 2; i++) {
        dibits[i] = (uint8_t)((bits[2 * i] << 1) | bits[(2 * i) + 1]);
    }
}

/* A sync word as dibits ('1' +3 is dibit 1, '3' -3 is dibit 3). */
static inline int
dpmr_spec_word(const char* word, uint8_t* dibits) {
    const int n = (int)strlen(word);
    for (int i = 0; i < n; i++) {
        dibits[i] = word[i] == '1' ? 1U : 3U;
    }
    return n;
}

#endif /* DSD_NEO_TESTS_PROTOCOL_DPMR_DPMR_SPEC_ENCODER_H_ */
