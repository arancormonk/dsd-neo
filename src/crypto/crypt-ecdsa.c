// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/crypto/ecdsa.h>

#include <openssl/bn.h>
#include <openssl/core.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/types.h>

#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/safe_api.h"

#define DSD_ECDSA_P256_UNCOMPRESSED_KEY_BYTES (1U + DSD_ECDSA_P256_PUBLIC_KEY_BYTES)
#define DSD_ECDSA_P256_DER_SIGNATURE_MAX      80U

static EVP_PKEY*
dsd_ecdsa_p256_public_key_from_xy(const uint8_t public_key_xy[DSD_ECDSA_P256_PUBLIC_KEY_BYTES]) {
    uint8_t public_key_octets[DSD_ECDSA_P256_UNCOMPRESSED_KEY_BYTES];
    public_key_octets[0] = 0x04U;
    DSD_MEMCPY(public_key_octets + 1, public_key_xy, DSD_ECDSA_P256_PUBLIC_KEY_BYTES);

    char group_name[] = "prime256v1";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group_name, 0),
        OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, public_key_octets, sizeof(public_key_octets)),
        OSSL_PARAM_construct_end(),
    };

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if (ctx == NULL) {
        return NULL;
    }

    EVP_PKEY* key = NULL;
    if (EVP_PKEY_fromdata_init(ctx) <= 0 || EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
        EVP_PKEY_free(key);
        key = NULL;
    }
    EVP_PKEY_CTX_free(ctx);
    return key;
}

static int
dsd_ecdsa_p256_signature_rs_to_der(const uint8_t signature_rs[DSD_ECDSA_P256_SIGNATURE_BYTES],
                                   uint8_t der[DSD_ECDSA_P256_DER_SIGNATURE_MAX], size_t* der_len) {
    if (signature_rs == NULL || der == NULL || der_len == NULL) {
        return -1;
    }

    ECDSA_SIG* signature = ECDSA_SIG_new();
    BIGNUM* r = BN_bin2bn(signature_rs, DSD_ECDSA_P256_SIGNATURE_BYTES / 2U, NULL);
    BIGNUM* s =
        BN_bin2bn(signature_rs + (DSD_ECDSA_P256_SIGNATURE_BYTES / 2U), DSD_ECDSA_P256_SIGNATURE_BYTES / 2U, NULL);
    if (signature == NULL || r == NULL || s == NULL) {
        ECDSA_SIG_free(signature);
        BN_free(r);
        BN_free(s);
        return -1;
    }

    if (ECDSA_SIG_set0(signature, r, s) != 1) {
        ECDSA_SIG_free(signature);
        BN_free(r);
        BN_free(s);
        return -1;
    }
    r = NULL;
    s = NULL;

    const int encoded_len = i2d_ECDSA_SIG(signature, NULL);
    if (encoded_len <= 0 || (size_t)encoded_len > DSD_ECDSA_P256_DER_SIGNATURE_MAX) {
        ECDSA_SIG_free(signature);
        return -1;
    }

    uint8_t* out = der;
    const int written = i2d_ECDSA_SIG(signature, &out);
    ECDSA_SIG_free(signature);
    if (written != encoded_len) {
        return -1;
    }

    *der_len = (size_t)written;
    return 0;
}

int
dsd_ecdsa_p256_verify_digest(const uint8_t* digest, size_t digest_len,
                             const uint8_t public_key_xy[DSD_ECDSA_P256_PUBLIC_KEY_BYTES],
                             const uint8_t signature_rs[DSD_ECDSA_P256_SIGNATURE_BYTES]) {
    if (digest == NULL || digest_len == 0U || public_key_xy == NULL || signature_rs == NULL) {
        return -1;
    }

    uint8_t signature_der[DSD_ECDSA_P256_DER_SIGNATURE_MAX];
    size_t signature_der_len = 0U;
    if (dsd_ecdsa_p256_signature_rs_to_der(signature_rs, signature_der, &signature_der_len) != 0) {
        return -2;
    }

    EVP_PKEY* public_key = dsd_ecdsa_p256_public_key_from_xy(public_key_xy);
    if (public_key == NULL) {
        return -3;
    }

    EVP_PKEY_CTX* verify_ctx = EVP_PKEY_CTX_new(public_key, NULL);
    if (verify_ctx == NULL) {
        EVP_PKEY_free(public_key);
        return -4;
    }

    const int verify_rc = (EVP_PKEY_verify_init(verify_ctx) > 0)
                              ? EVP_PKEY_verify(verify_ctx, signature_der, signature_der_len, digest, digest_len)
                              : -1;

    EVP_PKEY_CTX_free(verify_ctx);
    EVP_PKEY_free(public_key);

    if (verify_rc == 1) {
        return 1;
    }
    if (verify_rc == 0) {
        return 0;
    }
    return -5;
}
