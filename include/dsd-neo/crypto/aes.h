// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief AES helper entrypoints.
 *
 * Declares the AES wrapper helpers implemented in `src/crypto/crypt-aes.c`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_AES_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_AES_H_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Generate AES OFB keystream blocks for the given IV/key. */
void aes_ofb_keystream_output(const uint8_t* iv, const uint8_t* key, uint8_t* output, int type, int nblocks);

/** @brief Decrypt whole AES ECB blocks. Supports in-place input/output buffers. */
void aes_ecb_decrypt_blocks(const uint8_t* input, const uint8_t* key, uint8_t* output, int type, int nblocks);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_AES_H_H */
