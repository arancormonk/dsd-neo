// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/crypto/backup_seal.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/safe_api.h"

#define TEST_ONE_MIB          (1024U * 1024U)
#define TEST_GOLDEN_PAYLOAD   "DSD-neo backup envelope v1 golden"
#define TEST_GOLDEN_PAYLOAD_N (sizeof(TEST_GOLDEN_PAYLOAD) - 1U)
#define TEST_GOLDEN_SEALED_N  (DSD_BACKUP_HEADER_BYTES + TEST_GOLDEN_PAYLOAD_N + DSD_BACKUP_TAG_BYTES)

static const char k_pass[] = "correct horse battery staple";

/*
 * seal_with(k_pass, {log2_n 14, r 8, p 1}, TEST_GOLDEN_PAYLOAD, salt 00..0f, nonce f0..fb). Computed outside this
 * code base from the byte layout alone, with Python hashlib.scrypt + cryptography AESGCM and again with a
 * pure-Python scrypt/AES-256-GCM.
 */
static const uint8_t k_golden_sealed[TEST_GOLDEN_SEALED_N] = {
    0x44U, 0x53U, 0x44U, 0x4EU, 0x45U, 0x4FU, 0x42U, 0x4BU, 0x00U, 0x01U, 0x01U, 0x0EU, 0x00U, 0x00U, 0x00U,
    0x08U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x09U,
    0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0xF0U, 0xF1U, 0xF2U, 0xF3U, 0xF4U, 0xF5U, 0xF6U, 0xF7U, 0xF8U,
    0xF9U, 0xFAU, 0xFBU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x21U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0xA0U, 0x99U, 0xDAU, 0x2DU, 0x98U, 0x10U, 0x7BU, 0x8FU, 0x03U, 0x35U,
    0xB4U, 0xF2U, 0x6BU, 0xFFU, 0xE9U, 0xCDU, 0x8EU, 0xE5U, 0xBAU, 0x07U, 0xD0U, 0x3AU, 0x49U, 0x44U, 0xB1U,
    0x6CU, 0x78U, 0xBCU, 0xD1U, 0x97U, 0xC5U, 0xC0U, 0x65U, 0x2CU, 0x46U, 0xBDU, 0x4AU, 0x9CU, 0x41U, 0x8DU,
    0xADU, 0xA2U, 0xA7U, 0xDCU, 0x1BU, 0x5BU, 0x94U, 0xBAU,
};

static int
expect_int(const char* name, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d, want %d\n", name, got, want);
        return 1;
    }
    return 0;
}

static int
expect_size(const char* name, size_t got, size_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %zu, want %zu\n", name, got, want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* name, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: expectation failed\n", name);
        return 1;
    }
    return 0;
}

static int
all_zero(const uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != 0U) {
            return 0;
        }
    }
    return 1;
}

static void
fill_pattern(uint8_t* buf, size_t len, uint32_t seed) {
    uint32_t x = seed != 0U ? seed : 0x9E3779B9U;
    for (size_t i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (uint8_t)(x >> 24);
    }
}

static dsd_backup_kdf_params
fast_kdf(uint32_t p) {
    dsd_backup_kdf_params kdf;
    kdf.log2_n = 14U;
    kdf.r = 8U;
    kdf.p = p;
    return kdf;
}

static const uint8_t*
pass_bytes(void) {
    return (const uint8_t*)k_pass;
}

static size_t
pass_len(void) {
    return sizeof(k_pass) - 1U;
}

static void
put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int
test_defaults_and_sizes(void) {
    int rc = 0;
    dsd_backup_kdf_params kdf;
    DSD_MEMSET(&kdf, 0xA5, sizeof(kdf));
    dsd_backup_kdf_params_default(&kdf);
    rc |= expect_int("default log2_n", kdf.log2_n, 16);
    rc |= expect_int("default r", (int)kdf.r, 8);
    rc |= expect_int("default p", (int)kdf.p, 2);

    rc |= expect_size("sealed size 0", dsd_backup_sealed_size(0U), 80U);
    rc |= expect_size("sealed size 1", dsd_backup_sealed_size(1U), 81U);
    rc |= expect_size("sealed size max", dsd_backup_sealed_size(DSD_BACKUP_MAX_PAYLOAD_BYTES),
                      (size_t)DSD_BACKUP_MAX_PAYLOAD_BYTES + 80U);
    rc |= expect_size("sealed size max+1", dsd_backup_sealed_size((size_t)DSD_BACKUP_MAX_PAYLOAD_BYTES + 1U), 0U);
    rc |= expect_size("sealed size SIZE_MAX", dsd_backup_sealed_size(SIZE_MAX), 0U);
    return rc;
}

/* Seal `len` pattern bytes with random salt/nonce, check the header, open, compare. */
static int
round_trip(const char* name, size_t len, const dsd_backup_kdf_params* kdf) {
    int rc = 0;
    const size_t sealed_cap = dsd_backup_sealed_size(len);
    uint8_t* payload = (uint8_t*)malloc(len + 1U);
    uint8_t* sealed = (uint8_t*)malloc(sealed_cap);
    uint8_t* opened = (uint8_t*)malloc(len + 1U);
    if (payload == NULL || sealed == NULL || opened == NULL) {
        free(payload);
        free(sealed);
        free(opened);
        return expect_true(name, 0);
    }
    fill_pattern(payload, len, (uint32_t)len + 7U);

    size_t sealed_len = 0;
    rc |= expect_int(name,
                     dsd_backup_seal(pass_bytes(), pass_len(), kdf, len == 0U ? NULL : payload, len, sealed, sealed_cap,
                                     &sealed_len),
                     DSD_BACKUP_OK);
    rc |= expect_size(name, sealed_len, sealed_cap);
    if (rc != 0 || sealed_cap < DSD_BACKUP_HEADER_BYTES) {
        free(payload);
        free(sealed);
        free(opened);
        return 1;
    }

    dsd_backup_kdf_params parsed = {0U, 0U, 0U};
    size_t parsed_len = 999U;
    rc |= expect_int(name, dsd_backup_parse_header(sealed, sealed_len, &parsed, &parsed_len), DSD_BACKUP_OK);
    rc |= expect_int(name, parsed.log2_n, kdf->log2_n);
    rc |= expect_int(name, (int)parsed.r, (int)kdf->r);
    rc |= expect_int(name, (int)parsed.p, (int)kdf->p);
    rc |= expect_size(name, parsed_len, len);
    rc |= expect_true(name, memcmp(sealed, "DSDNEOBK", 8U) == 0);

    size_t opened_len = 999U;
    rc |= expect_int(
        name,
        dsd_backup_open(pass_bytes(), pass_len(), sealed, sealed_len, len == 0U ? NULL : opened, len, &opened_len),
        DSD_BACKUP_OK);
    rc |= expect_size(name, opened_len, len);
    rc |= expect_true(name, len == 0U || memcmp(opened, payload, len) == 0);

    free(payload);
    free(sealed);
    free(opened);
    return rc;
}

static int
test_round_trips(void) {
    int rc = 0;
    const dsd_backup_kdf_params fast = fast_kdf(1U);
    rc |= round_trip("round trip 0 B", 0U, &fast);
    rc |= round_trip("round trip 1 B", 1U, &fast);
    rc |= round_trip("round trip 1 MiB", TEST_ONE_MIB, &fast);

    dsd_backup_kdf_params defaults;
    dsd_backup_kdf_params_default(&defaults);
    rc |= round_trip("round trip defaults", 4096U, &defaults);
    return rc;
}

static int
test_random_salt_and_nonce(void) {
    int rc = 0;
    const dsd_backup_kdf_params fast = fast_kdf(1U);
    uint8_t a[DSD_BACKUP_HEADER_BYTES + DSD_BACKUP_TAG_BYTES];
    uint8_t b[DSD_BACKUP_HEADER_BYTES + DSD_BACKUP_TAG_BYTES];
    size_t a_len = 0;
    size_t b_len = 0;
    rc |= expect_int("seal a", dsd_backup_seal(pass_bytes(), pass_len(), &fast, NULL, 0U, a, sizeof(a), &a_len),
                     DSD_BACKUP_OK);
    rc |= expect_int("seal b", dsd_backup_seal(pass_bytes(), pass_len(), &fast, NULL, 0U, b, sizeof(b), &b_len),
                     DSD_BACKUP_OK);
    rc |= expect_true("salts differ", memcmp(a + 20, b + 20, DSD_BACKUP_SALT_BYTES) != 0);
    rc |= expect_true("nonces differ", memcmp(a + 36, b + 36, DSD_BACKUP_NONCE_BYTES) != 0);
    return rc;
}

static int
test_golden_vector(void) {
    int rc = 0;
    uint8_t salt[DSD_BACKUP_SALT_BYTES];
    uint8_t nonce[DSD_BACKUP_NONCE_BYTES];
    for (size_t i = 0; i < sizeof(salt); i++) {
        salt[i] = (uint8_t)i;
    }
    for (size_t i = 0; i < sizeof(nonce); i++) {
        nonce[i] = (uint8_t)(0xF0U + i);
    }
    const dsd_backup_kdf_params kdf = fast_kdf(1U);

    uint8_t sealed[TEST_GOLDEN_SEALED_N + 8U];
    size_t sealed_len = 0;
    rc |= expect_int("golden seal_with",
                     dsd_backup_seal_with(pass_bytes(), pass_len(), &kdf, (const uint8_t*)TEST_GOLDEN_PAYLOAD,
                                          TEST_GOLDEN_PAYLOAD_N, salt, nonce, sealed, sizeof(sealed), &sealed_len),
                     DSD_BACKUP_OK);
    rc |= expect_size("golden length", sealed_len, TEST_GOLDEN_SEALED_N);
    rc |= expect_true("golden bytes", memcmp(sealed, k_golden_sealed, TEST_GOLDEN_SEALED_N) == 0);

    uint8_t opened[TEST_GOLDEN_PAYLOAD_N];
    size_t opened_len = 0;
    rc |= expect_int("golden open",
                     dsd_backup_open(pass_bytes(), pass_len(), k_golden_sealed, sizeof(k_golden_sealed), opened,
                                     sizeof(opened), &opened_len),
                     DSD_BACKUP_OK);
    rc |= expect_size("golden open length", opened_len, TEST_GOLDEN_PAYLOAD_N);
    rc |= expect_true("golden payload", memcmp(opened, TEST_GOLDEN_PAYLOAD, TEST_GOLDEN_PAYLOAD_N) == 0);
    return rc;
}

/* Open `sealed` with the golden passphrase; on AUTH the output region must come back zeroed. */
static int
expect_open(const char* name, const uint8_t* sealed, size_t sealed_len, int want) {
    uint8_t opened[TEST_GOLDEN_PAYLOAD_N];
    DSD_MEMSET(opened, 0xA5, sizeof(opened));
    size_t opened_len = 999U;
    int rc = expect_int(
        name, dsd_backup_open(pass_bytes(), pass_len(), sealed, sealed_len, opened, sizeof(opened), &opened_len), want);
    if (want != DSD_BACKUP_OK) {
        rc |= expect_size(name, opened_len, 0U);
    }
    if (want == DSD_BACKUP_ERR_AUTH) {
        rc |= expect_true(name, all_zero(opened, sizeof(opened)));
    }
    return rc;
}

static int
test_wrong_passphrase(void) {
    int rc = 0;
    static const char wrong[] = "correct horse battery stapler";
    uint8_t opened[TEST_GOLDEN_PAYLOAD_N + 4U];
    DSD_MEMSET(opened, 0xA5, sizeof(opened));
    size_t opened_len = 999U;
    rc |= expect_int("wrong passphrase",
                     dsd_backup_open((const uint8_t*)wrong, sizeof(wrong) - 1U, k_golden_sealed,
                                     sizeof(k_golden_sealed), opened, sizeof(opened), &opened_len),
                     DSD_BACKUP_ERR_AUTH);
    rc |= expect_size("wrong passphrase length", opened_len, 0U);
    rc |= expect_true("wrong passphrase cleansed", all_zero(opened, TEST_GOLDEN_PAYLOAD_N));
    return rc;
}

/* What parsing must say about the golden header with bit `bit` of byte `off` flipped (field table of the format). */
static int
expected_flip_status(size_t off, unsigned bit) {
    if (off < 8U) {
        return DSD_BACKUP_ERR_FORMAT; /* magic */
    }
    if (off < 10U) {
        return DSD_BACKUP_ERR_VERSION;
    }
    if (off == 10U) {
        return DSD_BACKUP_ERR_PARAMS; /* kdf id */
    }
    if (off == 11U) {
        const unsigned log2_n = 14U ^ (1U << bit);
        return (log2_n >= 14U && log2_n <= 17U) ? DSD_BACKUP_OK : DSD_BACKUP_ERR_PARAMS;
    }
    if (off < 16U) {
        return DSD_BACKUP_ERR_PARAMS; /* r must stay 8 */
    }
    if (off < 20U) {
        const uint32_t p = 1U ^ (1U << ((8U * (19U - (unsigned)off)) + bit));
        return (p >= 1U && p <= 4U) ? DSD_BACKUP_OK : DSD_BACKUP_ERR_PARAMS;
    }
    if (off < 48U) {
        return DSD_BACKUP_OK; /* salt and nonce only change the key/IV */
    }
    if (off < 56U) {
        const uint64_t len = (uint64_t)TEST_GOLDEN_PAYLOAD_N ^ ((uint64_t)1U << ((8U * (55U - (unsigned)off)) + bit));
        return len > DSD_BACKUP_MAX_PAYLOAD_BYTES ? DSD_BACKUP_ERR_TOO_LARGE : DSD_BACKUP_ERR_FORMAT;
    }
    return DSD_BACKUP_ERR_FORMAT; /* reserved */
}

static int
test_header_bit_flips_parse(void) {
    int rc = 0;
    uint8_t sealed[TEST_GOLDEN_SEALED_N];
    char name[64];
    for (size_t off = 0; off < DSD_BACKUP_HEADER_BYTES; off++) {
        for (unsigned bit = 0; bit < 8U; bit++) {
            DSD_MEMCPY(sealed, k_golden_sealed, sizeof(sealed));
            sealed[off] ^= (uint8_t)(1U << bit);
            DSD_SNPRINTF(name, sizeof(name), "parse flip byte %zu bit %u", off, bit);
            rc |= expect_int(name, dsd_backup_parse_header(sealed, sizeof(sealed), NULL, NULL),
                             expected_flip_status(off, bit));
        }
    }
    return rc;
}

static int
flip_and_open(const char* name, size_t off, unsigned bit, int want) {
    uint8_t sealed[TEST_GOLDEN_SEALED_N];
    DSD_MEMCPY(sealed, k_golden_sealed, sizeof(sealed));
    sealed[off] ^= (uint8_t)(1U << bit);
    return expect_open(name, sealed, sizeof(sealed), want);
}

static int
test_tamper_open(void) {
    int rc = 0;
    rc |= flip_and_open("open flip magic", 3U, 0U, DSD_BACKUP_ERR_FORMAT);
    rc |= flip_and_open("open flip version", 9U, 1U, DSD_BACKUP_ERR_VERSION);
    rc |= flip_and_open("open flip kdf", 10U, 0U, DSD_BACKUP_ERR_PARAMS);
    rc |= flip_and_open("open flip log2_n to 15", 11U, 0U, DSD_BACKUP_ERR_AUTH);
    rc |= flip_and_open("open flip log2_n out of range", 11U, 7U, DSD_BACKUP_ERR_PARAMS);
    rc |= flip_and_open("open flip r", 15U, 0U, DSD_BACKUP_ERR_PARAMS);
    rc |= flip_and_open("open flip p to 3", 19U, 1U, DSD_BACKUP_ERR_AUTH);
    rc |= flip_and_open("open flip p out of range", 16U, 7U, DSD_BACKUP_ERR_PARAMS);
    rc |= flip_and_open("open flip salt", 27U, 4U, DSD_BACKUP_ERR_AUTH);
    rc |= flip_and_open("open flip nonce", 40U, 2U, DSD_BACKUP_ERR_AUTH);
    rc |= flip_and_open("open flip length low", 55U, 0U, DSD_BACKUP_ERR_FORMAT);
    rc |= flip_and_open("open flip length high", 48U, 7U, DSD_BACKUP_ERR_TOO_LARGE);
    rc |= flip_and_open("open flip reserved", 63U, 0U, DSD_BACKUP_ERR_FORMAT);
    rc |= flip_and_open("open flip ciphertext first", DSD_BACKUP_HEADER_BYTES, 0U, DSD_BACKUP_ERR_AUTH);
    rc |= flip_and_open("open flip ciphertext last", DSD_BACKUP_HEADER_BYTES + TEST_GOLDEN_PAYLOAD_N - 1U, 6U,
                        DSD_BACKUP_ERR_AUTH);
    rc |=
        flip_and_open("open flip tag first", DSD_BACKUP_HEADER_BYTES + TEST_GOLDEN_PAYLOAD_N, 0U, DSD_BACKUP_ERR_AUTH);
    rc |= flip_and_open("open flip tag last", TEST_GOLDEN_SEALED_N - 1U, 7U, DSD_BACKUP_ERR_AUTH);
    return rc;
}

static int
test_truncation_and_trailing(void) {
    int rc = 0;
    char name[64];
    for (size_t len = 0; len < TEST_GOLDEN_SEALED_N; len++) {
        DSD_SNPRINTF(name, sizeof(name), "parse truncated to %zu", len);
        rc |= expect_int(name, dsd_backup_parse_header(k_golden_sealed, len, NULL, NULL), DSD_BACKUP_ERR_FORMAT);
        DSD_SNPRINTF(name, sizeof(name), "open truncated to %zu", len);
        rc |= expect_open(name, k_golden_sealed, len, DSD_BACKUP_ERR_FORMAT);
    }
    rc |= expect_int("parse NULL empty", dsd_backup_parse_header(NULL, 0U, NULL, NULL), DSD_BACKUP_ERR_FORMAT);

    uint8_t longer[TEST_GOLDEN_SEALED_N + 1U];
    DSD_MEMCPY(longer, k_golden_sealed, TEST_GOLDEN_SEALED_N);
    longer[TEST_GOLDEN_SEALED_N] = 0U;
    rc |= expect_int("parse trailing byte", dsd_backup_parse_header(longer, sizeof(longer), NULL, NULL),
                     DSD_BACKUP_ERR_FORMAT);
    rc |= expect_open("open trailing byte", longer, sizeof(longer), DSD_BACKUP_ERR_FORMAT);
    return rc;
}

static int
seal_and_open_params(const char* name, const dsd_backup_kdf_params* kdf) {
    uint8_t payload[3] = {0x01U, 0x02U, 0x03U};
    uint8_t sealed[DSD_BACKUP_HEADER_BYTES + sizeof(payload) + DSD_BACKUP_TAG_BYTES];
    uint8_t opened[sizeof(payload)];
    size_t sealed_len = 0;
    size_t opened_len = 0;
    int rc = expect_int(
        name,
        dsd_backup_seal(pass_bytes(), pass_len(), kdf, payload, sizeof(payload), sealed, sizeof(sealed), &sealed_len),
        DSD_BACKUP_OK);
    rc |= expect_int(name,
                     dsd_backup_open(pass_bytes(), pass_len(), sealed, sealed_len, opened, sizeof(opened), &opened_len),
                     DSD_BACKUP_OK);
    rc |= expect_true(name, opened_len == sizeof(payload) && memcmp(opened, payload, sizeof(payload)) == 0);
    return rc;
}

/* The seal side and the open side both refuse params outside log2_n [14,17], r 8, p [1,4]. */
static int
reject_params(const char* name, uint8_t log2_n, uint32_t r, uint32_t p) {
    int rc = 0;
    dsd_backup_kdf_params kdf;
    kdf.log2_n = log2_n;
    kdf.r = r;
    kdf.p = p;
    uint8_t sealed[TEST_GOLDEN_SEALED_N];
    size_t sealed_len = 999U;
    rc |= expect_int(name,
                     dsd_backup_seal(pass_bytes(), pass_len(), &kdf, (const uint8_t*)TEST_GOLDEN_PAYLOAD,
                                     TEST_GOLDEN_PAYLOAD_N, sealed, sizeof(sealed), &sealed_len),
                     DSD_BACKUP_ERR_PARAMS);
    rc |= expect_size(name, sealed_len, 0U);

    DSD_MEMCPY(sealed, k_golden_sealed, sizeof(sealed));
    sealed[11] = log2_n;
    put_be32(sealed + 12, r);
    put_be32(sealed + 16, p);
    rc |= expect_int(name, dsd_backup_parse_header(sealed, sizeof(sealed), NULL, NULL), DSD_BACKUP_ERR_PARAMS);
    rc |= expect_open(name, sealed, sizeof(sealed), DSD_BACKUP_ERR_PARAMS);
    return rc;
}

static int
test_param_boundaries(void) {
    int rc = 0;
    dsd_backup_kdf_params kdf = fast_kdf(1U);
    rc |= seal_and_open_params("accept log2_n 14 p 1", &kdf);
    kdf.p = 4U;
    rc |= seal_and_open_params("accept p 4", &kdf);
    kdf.p = 1U;
    kdf.log2_n = 17U;
    rc |= seal_and_open_params("accept log2_n 17", &kdf);

    rc |= reject_params("reject log2_n 13", 13U, 8U, 1U);
    rc |= reject_params("reject log2_n 18", 18U, 8U, 1U);
    rc |= reject_params("reject r 7", 14U, 7U, 1U);
    rc |= reject_params("reject r 9", 14U, 9U, 1U);
    rc |= reject_params("reject p 0", 14U, 8U, 0U);
    rc |= reject_params("reject p 5", 14U, 8U, 5U);
    return rc;
}

static int
test_rfc7914_vector(void) {
    static const uint8_t want[32] = {
        0xFDU, 0xBAU, 0xBEU, 0x1CU, 0x9DU, 0x34U, 0x72U, 0x00U, 0x78U, 0x56U, 0xE7U, 0x19U, 0x0DU, 0x01U, 0xE9U, 0xFEU,
        0x7CU, 0x6AU, 0xD7U, 0xCBU, 0xC8U, 0x23U, 0x78U, 0x30U, 0xE7U, 0x73U, 0x76U, 0x63U, 0x4BU, 0x37U, 0x31U, 0x62U,
    };
    static const char pass[] = "password";
    static const char salt[] = "NaCl";
    const uint8_t* p8 = (const uint8_t*)pass;
    const uint8_t* s8 = (const uint8_t*)salt;
    const size_t pl = sizeof(pass) - 1U;
    const size_t sl = sizeof(salt) - 1U;
    uint8_t key[32];
    int rc = 0;

    rc |= expect_int("rfc7914 vector 2", dsd_backup_derive_key(p8, pl, s8, sl, 1024U, 8U, 16U, key, sizeof(key)),
                     DSD_BACKUP_OK);
    rc |= expect_true("rfc7914 vector 2 bytes", memcmp(key, want, sizeof(want)) == 0);

    rc |= expect_int("kat n 0", dsd_backup_derive_key(p8, pl, s8, sl, 0U, 8U, 1U, key, sizeof(key)),
                     DSD_BACKUP_ERR_PARAMS);
    rc |= expect_int("kat n 1", dsd_backup_derive_key(p8, pl, s8, sl, 1U, 8U, 1U, key, sizeof(key)),
                     DSD_BACKUP_ERR_PARAMS);
    rc |= expect_int("kat n 1000", dsd_backup_derive_key(p8, pl, s8, sl, 1000U, 8U, 1U, key, sizeof(key)),
                     DSD_BACKUP_ERR_PARAMS);
    rc |= expect_int("kat n 2^21", dsd_backup_derive_key(p8, pl, s8, sl, (uint64_t)1U << 21, 8U, 1U, key, sizeof(key)),
                     DSD_BACKUP_ERR_PARAMS);
    rc |= expect_int("kat n 2^64-1", dsd_backup_derive_key(p8, pl, s8, sl, UINT64_MAX, 8U, 1U, key, sizeof(key)),
                     DSD_BACKUP_ERR_PARAMS);
    rc |= expect_int("kat r 0", dsd_backup_derive_key(p8, pl, s8, sl, 1024U, 0U, 1U, key, sizeof(key)),
                     DSD_BACKUP_ERR_PARAMS);
    rc |= expect_int("kat p 0", dsd_backup_derive_key(p8, pl, s8, sl, 1024U, 8U, 0U, key, sizeof(key)),
                     DSD_BACKUP_ERR_PARAMS);
    rc |= expect_int("kat r*p overflow",
                     dsd_backup_derive_key(p8, pl, s8, sl, 1024U, UINT32_MAX, UINT32_MAX, key, sizeof(key)),
                     DSD_BACKUP_ERR_PARAMS);
    rc |= expect_int("kat r*p at 2^30",
                     dsd_backup_derive_key(p8, pl, s8, sl, 1024U, 1U << 15, 1U << 15, key, sizeof(key)),
                     DSD_BACKUP_ERR_PARAMS);
    /* RFC 7914 section 2: N < 2^(128 * r / 8). */
    rc |= expect_int("kat n too big for r 1", dsd_backup_derive_key(p8, pl, s8, sl, 65536U, 1U, 1U, key, sizeof(key)),
                     DSD_BACKUP_ERR_PARAMS);

    rc |= expect_int("kat NULL pass", dsd_backup_derive_key(NULL, pl, s8, sl, 1024U, 8U, 1U, key, sizeof(key)),
                     DSD_BACKUP_ERR_ARGS);
    rc |= expect_int("kat empty pass", dsd_backup_derive_key(p8, 0U, s8, sl, 1024U, 8U, 1U, key, sizeof(key)),
                     DSD_BACKUP_ERR_ARGS);
    rc |= expect_int("kat NULL salt", dsd_backup_derive_key(p8, pl, NULL, sl, 1024U, 8U, 1U, key, sizeof(key)),
                     DSD_BACKUP_ERR_ARGS);
    rc |= expect_int("kat NULL key", dsd_backup_derive_key(p8, pl, s8, sl, 1024U, 8U, 1U, NULL, sizeof(key)),
                     DSD_BACKUP_ERR_ARGS);
    rc |=
        expect_int("kat empty key", dsd_backup_derive_key(p8, pl, s8, sl, 1024U, 8U, 1U, key, 0U), DSD_BACKUP_ERR_ARGS);
    return rc;
}

static int
test_small_buffers_and_args(void) {
    int rc = 0;
    const dsd_backup_kdf_params kdf = fast_kdf(1U);
    const uint8_t* payload = (const uint8_t*)TEST_GOLDEN_PAYLOAD;
    uint8_t sealed[TEST_GOLDEN_SEALED_N];
    size_t sealed_len = 999U;

    rc |= expect_int("seal small out",
                     dsd_backup_seal(pass_bytes(), pass_len(), &kdf, payload, TEST_GOLDEN_PAYLOAD_N, sealed,
                                     sizeof(sealed) - 1U, &sealed_len),
                     DSD_BACKUP_ERR_ARGS);
    rc |= expect_size("seal small out length", sealed_len, 0U);
    rc |= expect_int(
        "seal NULL out",
        dsd_backup_seal(pass_bytes(), pass_len(), &kdf, payload, TEST_GOLDEN_PAYLOAD_N, NULL, 0U, &sealed_len),
        DSD_BACKUP_ERR_ARGS);
    rc |= expect_int("seal NULL out with cap",
                     dsd_backup_seal(pass_bytes(), pass_len(), &kdf, payload, TEST_GOLDEN_PAYLOAD_N, NULL,
                                     sizeof(sealed), &sealed_len),
                     DSD_BACKUP_ERR_ARGS);
    rc |= expect_int(
        "seal NULL pass",
        dsd_backup_seal(NULL, pass_len(), &kdf, payload, TEST_GOLDEN_PAYLOAD_N, sealed, sizeof(sealed), &sealed_len),
        DSD_BACKUP_ERR_ARGS);
    rc |= expect_int(
        "seal empty pass",
        dsd_backup_seal(pass_bytes(), 0U, &kdf, payload, TEST_GOLDEN_PAYLOAD_N, sealed, sizeof(sealed), &sealed_len),
        DSD_BACKUP_ERR_ARGS);
    rc |= expect_int("seal NULL kdf",
                     dsd_backup_seal(pass_bytes(), pass_len(), NULL, payload, TEST_GOLDEN_PAYLOAD_N, sealed,
                                     sizeof(sealed), &sealed_len),
                     DSD_BACKUP_ERR_ARGS);
    rc |= expect_int("seal NULL payload",
                     dsd_backup_seal(pass_bytes(), pass_len(), &kdf, NULL, TEST_GOLDEN_PAYLOAD_N, sealed,
                                     sizeof(sealed), &sealed_len),
                     DSD_BACKUP_ERR_ARGS);
    rc |= expect_int(
        "seal NULL out_len",
        dsd_backup_seal(pass_bytes(), pass_len(), &kdf, payload, TEST_GOLDEN_PAYLOAD_N, sealed, sizeof(sealed), NULL),
        DSD_BACKUP_ERR_ARGS);
    rc |= expect_int("seal_with NULL salt",
                     dsd_backup_seal_with(pass_bytes(), pass_len(), &kdf, payload, TEST_GOLDEN_PAYLOAD_N, NULL, sealed,
                                          sealed, sizeof(sealed), &sealed_len),
                     DSD_BACKUP_ERR_ARGS);
    rc |= expect_int("seal_with NULL nonce",
                     dsd_backup_seal_with(pass_bytes(), pass_len(), &kdf, payload, TEST_GOLDEN_PAYLOAD_N, sealed, NULL,
                                          sealed, sizeof(sealed), &sealed_len),
                     DSD_BACKUP_ERR_ARGS);

    uint8_t opened[TEST_GOLDEN_PAYLOAD_N];
    size_t opened_len = 999U;
    rc |= expect_int("open small out",
                     dsd_backup_open(pass_bytes(), pass_len(), k_golden_sealed, sizeof(k_golden_sealed), opened,
                                     sizeof(opened) - 1U, &opened_len),
                     DSD_BACKUP_ERR_ARGS);
    rc |= expect_size("open small out length", opened_len, 0U);
    rc |= expect_int("open NULL out with cap",
                     dsd_backup_open(pass_bytes(), pass_len(), k_golden_sealed, sizeof(k_golden_sealed), NULL,
                                     sizeof(opened), &opened_len),
                     DSD_BACKUP_ERR_ARGS);
    rc |= expect_int(
        "open NULL sealed",
        dsd_backup_open(pass_bytes(), pass_len(), NULL, sizeof(k_golden_sealed), opened, sizeof(opened), &opened_len),
        DSD_BACKUP_ERR_ARGS);
    rc |= expect_int("open empty pass",
                     dsd_backup_open(pass_bytes(), 0U, k_golden_sealed, sizeof(k_golden_sealed), opened, sizeof(opened),
                                     &opened_len),
                     DSD_BACKUP_ERR_ARGS);
    rc |= expect_int("open NULL out_len",
                     dsd_backup_open(pass_bytes(), pass_len(), k_golden_sealed, sizeof(k_golden_sealed), opened,
                                     sizeof(opened), NULL),
                     DSD_BACKUP_ERR_ARGS);
    rc |= expect_int("parse NULL sealed", dsd_backup_parse_header(NULL, sizeof(k_golden_sealed), NULL, NULL),
                     DSD_BACKUP_ERR_ARGS);
    return rc;
}

static int
test_oversize(void) {
    int rc = 0;
    const dsd_backup_kdf_params kdf = fast_kdf(1U);
    size_t sealed_len = 999U;
    /*
     * Rejected on the length alone. The buffers really are that large (and stay untouched pages when the check
     * comes first), so if TOO_LARGE stopped being checked first, sealing would run in bounds and fail on the
     * status instead of reading or writing past the end.
     */
    const size_t oversize = (size_t)DSD_BACKUP_MAX_PAYLOAD_BYTES + 1U;
    const size_t oversize_cap = oversize + DSD_BACKUP_HEADER_BYTES + DSD_BACKUP_TAG_BYTES;
    uint8_t* big_payload = (uint8_t*)calloc(oversize, 1U);
    uint8_t* big_out = (uint8_t*)malloc(oversize_cap);
    if (big_payload == NULL || big_out == NULL) {
        rc |= expect_true("seal oversize buffers", 0);
    } else {
        rc |= expect_int(
            "seal oversize",
            dsd_backup_seal(pass_bytes(), pass_len(), &kdf, big_payload, oversize, big_out, oversize_cap, &sealed_len),
            DSD_BACKUP_ERR_TOO_LARGE);
        rc |= expect_size("seal oversize length", sealed_len, 0U);
    }
    free(big_payload);
    free(big_out);

    /* A header that announces MAX + 1 bytes is TOO_LARGE however long the input is. */
    uint8_t header[TEST_GOLDEN_SEALED_N];
    DSD_MEMCPY(header, k_golden_sealed, sizeof(header));
    const uint64_t len = (uint64_t)DSD_BACKUP_MAX_PAYLOAD_BYTES + 1U;
    for (unsigned i = 0; i < 8U; i++) {
        header[48U + i] = (uint8_t)(len >> (8U * (7U - i)));
    }
    rc |= expect_int("parse oversize", dsd_backup_parse_header(header, sizeof(header), NULL, NULL),
                     DSD_BACKUP_ERR_TOO_LARGE);
    rc |= expect_open("open oversize", header, sizeof(header), DSD_BACKUP_ERR_TOO_LARGE);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_defaults_and_sizes();
    rc |= test_golden_vector();
    rc |= test_rfc7914_vector();
    rc |= test_round_trips();
    rc |= test_random_salt_and_nonce();
    rc |= test_wrong_passphrase();
    rc |= test_header_bit_flips_parse();
    rc |= test_tamper_open();
    rc |= test_truncation_and_trailing();
    rc |= test_param_boundaries();
    rc |= test_small_buffers_and_args();
    rc |= test_oversize();
    return rc;
}
