// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RC4 keystream helpers.
 *
 * Declares minimal RC4 utilities implemented in `src/crypto/crypt-rc4.c`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_RC4_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_RC4_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rc4_block_output(int drop, int keylen, int meslen, const uint8_t* key, uint8_t* output_blocks);
void rc4_voice_decrypt(int drop, uint8_t keylength, uint8_t messagelength, const uint8_t key[], const uint8_t cipher[],
                       uint8_t plain[]);
void hytera_enhanced_rc4_setup(dsd_opts* opts, dsd_state* state, unsigned long long int key_value,
                               unsigned long long int mi_value);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_RC4_H_H */
