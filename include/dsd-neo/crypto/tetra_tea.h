// SPDX-License-Identifier: Apache-2.0
/* Air-interface TEA1/2/3. The ciphers and frame-number packing come from
 * the tetracrypto vcpkg port. This wrapper does not unwrap OTAR keys. */
#ifndef DSD_NEO_CRYPTO_TETRA_TEA_H
#define DSD_NEO_CRYPTO_TETRA_TEA_H

#include <stdint.h>

/* alg is the KSG number: 0 TEA1, 1 TEA2, 2 TEA3. Other values do nothing. */
void tetra_tea_keystream(int alg, const uint8_t key[10], uint32_t iv, uint8_t* out, int nbytes);

/* EN 300 392-7 clause 6.3.2.1. Out-of-range slot, frame, multiframe or direction returns 0. */
uint32_t tetra_tea_build_iv(uint8_t tn, uint8_t fn, uint8_t mn, uint16_t hn, uint8_t dir);

#endif /* DSD_NEO_CRYPTO_TETRA_TEA_H */
