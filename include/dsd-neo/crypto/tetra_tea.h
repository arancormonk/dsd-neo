// SPDX-License-Identifier: GPL-3.0-or-later
/* Air-interface TEA1/2/3 and the TA61 identity encryption. The keystream
 * generators are the pinned TETRA_crypto sources compiled into DSD-neo; the
 * frame-number packing and TA61 (ported from the same sources) are implemented
 * here. This wrapper does not unwrap OTAR keys. */
#ifndef DSD_NEO_CRYPTO_TETRA_TEA_H
#define DSD_NEO_CRYPTO_TETRA_TEA_H

#include <stdint.h>

/* alg is the KSG number: 0 TEA1, 1 TEA2, 2 TEA3. Other values do nothing. */
void tetra_tea_keystream(int alg, const uint8_t key[10], uint32_t iv, uint8_t* out, int nbytes);

/* TEA1 from its 32-bit key register instead of an 80-bit ECK. TEA1 compresses the ECK into that register before it
 * generates anything, so the register (the reduced ECK) determines the key stream alone; as it comes from the ECK, it
 * belongs to the one carrier, location area and colour code TB5 derived that ECK for. A NULL out or a length below 1
 * does nothing. */
void tetra_tea1_keystream_reduced(uint32_t key_register, uint32_t iv, uint8_t* out, int nbytes);

/* The 32-bit key register TEA1 compresses the 10-byte key (an ECK) into: the reduced key of
 * tetra_tea1_keystream_reduced() that gives the same key stream. The caller wipes it after use; NULL gives 0. */
uint32_t tetra_tea1_reduce_key(const uint8_t key[10]);

/* EN 300 392-7 clause 6.3.2.1. Out-of-range slot, frame, multiframe or direction returns 0. */
uint32_t tetra_tea_build_iv(uint8_t tn, uint8_t fn, uint8_t mn, uint16_t hn, uint8_t dir);

/* The encryption cipher key (ECK) the key stream generators take, from the 80-bit cipher key ck (SCK in a class 2
 * cell; DCK, CCK or MGCK in a class 3 cell) with algorithm TB5 (EN 300 392-7 clauses 4.2.7 and 6.3.2.2): the carrier
 * number cn (12 bits), the location area la (14 bits) and the colour code cc (6 bits) of the carrier being received.
 * Writes the 10-byte ECK to eck; the caller wipes it after use. NULL pointers do nothing. */
void tetra_tea_derive_eck(uint16_t cn, uint16_t la, uint8_t cc, const uint8_t* ck, uint8_t* eck);

/* TA61 (EN 300 392-7 clause 4.2.6): the encrypted short identity (ESI) of a 24-bit SSI under the 80-bit cipher key
 * key (the SCK in a class 2 cell, the CCK in a class 3 one), and its inverse. Only the low 24 bits of the input are
 * read. NULL pointers do nothing. */
void tetra_ta61_encrypt(const uint8_t key[10], uint32_t ssi, uint32_t* esi);
void tetra_ta61_decrypt(const uint8_t key[10], uint32_t esi, uint32_t* ssi);

#endif /* DSD_NEO_CRYPTO_TETRA_TEA_H */
