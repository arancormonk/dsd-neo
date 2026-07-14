// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief DES/3DES keystream helpers.
 *
 * Declares the DES and Triple-DES keystream generators implemented in
 * `src/crypto/crypt-des.c`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_DES_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_DES_H_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Generate DES-OFB keystream blocks from a packed message indicator and key. */
void des_ofb_keystream_output(unsigned long long int mi, unsigned long long int key_ulli, uint8_t* output, int nblocks);

/** @brief Generate a DES-XL keystream; late_entry selects the shorter LFSR fast-forward. */
void des_xl_keystream_output(unsigned long long int mi, unsigned long long int key_ulli, uint8_t* output,
                             int late_entry);

/** @brief Generate TDEA output-feedback keystream blocks from a packed message indicator. */
void tdea_tofb_keystream_output(unsigned long long int mi, const uint8_t* key, uint8_t* output, int nblocks);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_DES_H_H */
