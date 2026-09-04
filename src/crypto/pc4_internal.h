// SPDX-License-Identifier: ISC
#ifndef DSD_NEO_SRC_CRYPTO_PC4_INTERNAL_H_
#define DSD_NEO_SRC_CRYPTO_PC4_INTERNAL_H_

#include <stddef.h>
#include <stdint.h>

void pc4_tyt_set_key(const unsigned char* key, size_t key_len);
void pc4_tyt_decrypt_frame49(short frame_bits[49]);
void pc4_tyt_set_static_keystream(const uint8_t bits[49]);
void pc4_tyt_apply_static_keystream(char frame_bits[49]);
void pc4_kirisun_generate_keystream(const uint8_t key[32], uint64_t initial_state, uint8_t output[126]);

#endif /* DSD_NEO_SRC_CRYPTO_PC4_INTERNAL_H_ */
