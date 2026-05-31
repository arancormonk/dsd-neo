// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief ECDSA helper entrypoints.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_ECDSA_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_ECDSA_H_

#include <stddef.h>
#include <stdint.h>

#define DSD_ECDSA_P256_PUBLIC_KEY_BYTES 64U
#define DSD_ECDSA_P256_SIGNATURE_BYTES  64U

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Verify a raw ECDSA P-256 signature over an already-computed digest.
 *
 * @param digest         Message digest bytes. For M17 this is the 16-byte stream digest.
 * @param digest_len     Number of digest bytes.
 * @param public_key_xy  64-byte public key as big-endian X || Y.
 * @param signature_rs   64-byte signature as big-endian R || S.
 *
 * @return 1 for a valid signature, 0 for an invalid signature, negative on argument/internal errors.
 */
int dsd_ecdsa_p256_verify_digest(const uint8_t* digest, size_t digest_len,
                                 const uint8_t public_key_xy[DSD_ECDSA_P256_PUBLIC_KEY_BYTES],
                                 const uint8_t signature_rs[DSD_ECDSA_P256_SIGNATURE_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_ECDSA_H_ */
