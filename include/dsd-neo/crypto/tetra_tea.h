// SPDX-License-Identifier: GPL-3.0-or-later
/* Air-interface TEA1/2/3. The keystream generators are the pinned TETRA_crypto
 * sources compiled into DSD-neo; the frame-number packing is implemented here.
 * This wrapper does not unwrap OTAR keys. */
#ifndef DSD_NEO_CRYPTO_TETRA_TEA_H
#define DSD_NEO_CRYPTO_TETRA_TEA_H

#include <stdint.h>

/* alg is the KSG number: 0 TEA1, 1 TEA2, 2 TEA3. Other values do nothing. */
void tetra_tea_keystream(int alg, const uint8_t key[10], uint32_t iv, uint8_t* out, int nbytes);

/* EN 300 392-7 clause 6.3.2.1. Out-of-range slot, frame, multiframe or direction returns 0. */
uint32_t tetra_tea_build_iv(uint8_t tn, uint8_t fn, uint8_t mn, uint16_t hn, uint8_t dir);

/* The encryption cipher key (ECK) the key stream generators take, from the 80-bit cipher key ck (SCK in a class 2
 * cell; DCK, CCK or MGCK in a class 3 cell) with algorithm TB5 (EN 300 392-7 clauses 4.2.7 and 6.3.2.2): the carrier
 * number cn (12 bits), the location area la (14 bits) and the colour code cc (6 bits) of the carrier being received.
 * Writes the 10-byte ECK to eck; the caller wipes it after use. NULL pointers do nothing. */
void tetra_tea_derive_eck(uint16_t cn, uint16_t la, uint8_t cc, const uint8_t* ck, uint8_t* eck);

#endif /* DSD_NEO_CRYPTO_TETRA_TEA_H */
