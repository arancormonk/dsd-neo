// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Block-code (Hamming/Golay/QR) helpers used by protocol decoders.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_FEC_BLOCK_CODES_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_FEC_BLOCK_CODES_H_H

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Correct a Hamming(10,6,3) codeword represented as six data bits and four parity bits.
 *
 * @param data Six data bits, corrected in place when one data-bit error is detected.
 * @param parity Four parity bits.
 * @return 0 when valid, 1 when one bit was corrected or identified, and 2 when invalid or uncorrectable.
 */
int hamming_10_6_3_decode(char* data, const char* parity);

void Hamming_7_4_init(void);
bool Hamming_7_4_decode(unsigned char* rxBits);

void Hamming_12_8_init(void);
bool Hamming_12_8_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords);

void Hamming_13_9_init(void);
bool Hamming_13_9_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords);

void Hamming_15_11_init(void);
bool Hamming_15_11_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords);

void Hamming_16_11_4_init(void);
bool Hamming_16_11_4_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords);

void Golay_20_8_init(void);
bool Golay_20_8_decode(unsigned char* rxBits);

void Golay_24_12_init(void);
void Golay_24_12_encode(const unsigned char* origBits, unsigned char* encodedBits);
bool Golay_24_12_decode(unsigned char* rxBits);

void QR_16_7_6_init(void);
bool QR_16_7_6_decode(unsigned char* rxBits);

void InitAllFecFunction(void);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_FEC_BLOCK_CODES_H_H */
