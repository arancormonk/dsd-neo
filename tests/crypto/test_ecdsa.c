// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/crypto/ecdsa.h>

#include <stdint.h>
#include <stdio.h>

#include "dsd-neo/core/safe_api.h"

#define M17_TEST_DIGEST_BYTES 16U

static int
expect_int(const char* name, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d, want %d\n", name, got, want);
        return 1;
    }
    return 0;
}

static void
load_m17_signature_vector(uint8_t digest[M17_TEST_DIGEST_BYTES], uint8_t public_key[DSD_ECDSA_P256_PUBLIC_KEY_BYTES],
                          uint8_t signature[DSD_ECDSA_P256_SIGNATURE_BYTES]) {
    static const uint8_t vector_digest[M17_TEST_DIGEST_BYTES] = {
        0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU,
    };
    static const uint8_t vector_public_key[DSD_ECDSA_P256_PUBLIC_KEY_BYTES] = {
        0x25U, 0x3DU, 0xD9U, 0xCEU, 0x17U, 0x70U, 0x42U, 0xA6U, 0x05U, 0x6FU, 0x06U, 0x9CU, 0x09U, 0x6AU, 0x68U, 0xF9U,
        0x93U, 0x7EU, 0x5EU, 0xC8U, 0x2FU, 0x76U, 0xF4U, 0x9BU, 0xDCU, 0xB7U, 0x8EU, 0xE1U, 0x0BU, 0x69U, 0x13U, 0x73U,
        0xA4U, 0x89U, 0x11U, 0xB5U, 0x9CU, 0x26U, 0x9EU, 0xAAU, 0x33U, 0xBCU, 0x42U, 0x8FU, 0xE5U, 0x98U, 0xCEU, 0x87U,
        0xADU, 0xD4U, 0xEDU, 0x6DU, 0x1BU, 0x4EU, 0x0EU, 0xFAU, 0xFBU, 0x25U, 0x58U, 0x45U, 0x6DU, 0xFCU, 0x35U, 0xDEU,
    };
    static const uint8_t vector_signature[DSD_ECDSA_P256_SIGNATURE_BYTES] = {
        0x78U, 0xC7U, 0x91U, 0x85U, 0xDEU, 0xBCU, 0x13U, 0x76U, 0x98U, 0x70U, 0xB1U, 0x13U, 0xFCU, 0xF3U, 0x7EU, 0x77U,
        0xBBU, 0x3FU, 0x83U, 0x20U, 0x3FU, 0x89U, 0x0CU, 0xBEU, 0xEBU, 0xFFU, 0x81U, 0x7AU, 0x7CU, 0xDCU, 0xE7U, 0x58U,
        0x74U, 0x4FU, 0x1FU, 0xF0U, 0x0CU, 0x12U, 0xBDU, 0x32U, 0x30U, 0x51U, 0x89U, 0x11U, 0x9CU, 0xA5U, 0x0DU, 0x17U,
        0x47U, 0xE7U, 0x4DU, 0x41U, 0x0DU, 0x18U, 0x00U, 0x31U, 0x53U, 0xCEU, 0xBDU, 0xC1U, 0xB8U, 0x10U, 0xF4U, 0xDBU,
    };

    DSD_MEMCPY(digest, vector_digest, sizeof(vector_digest));
    DSD_MEMCPY(public_key, vector_public_key, sizeof(vector_public_key));
    DSD_MEMCPY(signature, vector_signature, sizeof(vector_signature));
}

static int
test_p256_verify_accepts_valid_m17_digest_signature(void) {
    uint8_t digest[M17_TEST_DIGEST_BYTES];
    uint8_t public_key[DSD_ECDSA_P256_PUBLIC_KEY_BYTES];
    uint8_t signature[DSD_ECDSA_P256_SIGNATURE_BYTES];
    load_m17_signature_vector(digest, public_key, signature);

    return expect_int("valid p256 signature",
                      dsd_ecdsa_p256_verify_digest(digest, sizeof(digest), public_key, signature), 1);
}

static int
test_p256_verify_rejects_modified_digest_and_signature(void) {
    uint8_t digest[M17_TEST_DIGEST_BYTES];
    uint8_t public_key[DSD_ECDSA_P256_PUBLIC_KEY_BYTES];
    uint8_t signature[DSD_ECDSA_P256_SIGNATURE_BYTES];
    load_m17_signature_vector(digest, public_key, signature);

    int rc = 0;
    digest[0] ^= 0x01U;
    rc |= expect_int("modified digest", dsd_ecdsa_p256_verify_digest(digest, sizeof(digest), public_key, signature), 0);
    digest[0] ^= 0x01U;

    signature[63] ^= 0x01U;
    rc |= expect_int("modified signature", dsd_ecdsa_p256_verify_digest(digest, sizeof(digest), public_key, signature),
                     0);
    signature[63] ^= 0x01U;

    DSD_MEMSET(public_key, 0, sizeof(public_key));
    rc |= expect_int("invalid public key", dsd_ecdsa_p256_verify_digest(digest, sizeof(digest), public_key, signature),
                     -3);

    return rc;
}

static int
test_p256_verify_rejects_invalid_args(void) {
    uint8_t digest[M17_TEST_DIGEST_BYTES] = {0};
    uint8_t public_key[DSD_ECDSA_P256_PUBLIC_KEY_BYTES] = {0};
    uint8_t signature[DSD_ECDSA_P256_SIGNATURE_BYTES] = {0};
    int rc = 0;
    rc |= expect_int("null digest", dsd_ecdsa_p256_verify_digest(NULL, sizeof(digest), public_key, signature), -1);
    rc |= expect_int("zero digest length", dsd_ecdsa_p256_verify_digest(digest, 0, public_key, signature), -1);
    rc |= expect_int("null public key", dsd_ecdsa_p256_verify_digest(digest, sizeof(digest), NULL, signature), -1);
    rc |= expect_int("null signature", dsd_ecdsa_p256_verify_digest(digest, sizeof(digest), public_key, NULL), -1);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_p256_verify_accepts_valid_m17_digest_signature();
    rc |= test_p256_verify_rejects_modified_digest_and_signature();
    rc |= test_p256_verify_rejects_invalid_args();
    return rc;
}
