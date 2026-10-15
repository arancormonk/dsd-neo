// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/crypto/backup_seal.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/types.h>

/* <openssl/evp.h> declares EVP_PBE_scrypt() and, through its configuration header, says when a build left it out. */
#ifdef OPENSSL_NO_SCRYPT
#error "dsd-neo backups need an OpenSSL build with scrypt (OPENSSL_NO_SCRYPT is set)"
#endif

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dsd-neo/core/safe_api.h"

#define DSD_BACKUP_MAGIC             "DSDNEOBK"
#define DSD_BACKUP_MAGIC_BYTES       8U
#define DSD_BACKUP_VERSION           1U
#define DSD_BACKUP_KDF_SCRYPT        1U
#define DSD_BACKUP_RESERVED_BYTES    8U

#define DSD_BACKUP_OFF_VERSION       8U
#define DSD_BACKUP_OFF_KDF           10U
#define DSD_BACKUP_OFF_LOG2_N        11U
#define DSD_BACKUP_OFF_R             12U
#define DSD_BACKUP_OFF_P             16U
#define DSD_BACKUP_OFF_SALT          20U
#define DSD_BACKUP_OFF_NONCE         36U
#define DSD_BACKUP_OFF_LENGTH        48U
#define DSD_BACKUP_OFF_RESERVED      56U

/* Header policy, enforced when sealing and when opening. */
#define DSD_BACKUP_LOG2_N_MIN        14U
#define DSD_BACKUP_LOG2_N_MAX        17U
#define DSD_BACKUP_R                 8U
#define DSD_BACKUP_P_MIN             1U
#define DSD_BACKUP_P_MAX             4U

/* Limits of the raw scrypt entry point: N up to 2^20 and RFC 7914's r * p < 2^30. */
#define DSD_BACKUP_SCRYPT_LOG2_N_MAX 20U
#define DSD_BACKUP_SCRYPT_PR_MAX     ((1U << 30) - 1U)
#define DSD_BACKUP_SCRYPT_MEM_SLACK  ((uint64_t)1024U * 1024U)

/* The GCM calls take int lengths. */
#if DSD_BACKUP_MAX_PAYLOAD_BYTES > INT_MAX
#error "DSD_BACKUP_MAX_PAYLOAD_BYTES must fit in an int"
#endif

static void
dsd_backup_put_be(uint8_t* p, uint64_t v, size_t bytes) {
    for (size_t i = 0; i < bytes; i++) {
        p[i] = (uint8_t)(v >> (8U * (bytes - 1U - i)));
    }
}

static uint64_t
dsd_backup_get_be(const uint8_t* p, size_t bytes) {
    uint64_t v = 0U;
    for (size_t i = 0; i < bytes; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

static int
dsd_backup_kdf_params_accepted(const dsd_backup_kdf_params* kdf) {
    return kdf->log2_n >= DSD_BACKUP_LOG2_N_MIN && kdf->log2_n <= DSD_BACKUP_LOG2_N_MAX && kdf->r == DSD_BACKUP_R
           && kdf->p >= DSD_BACKUP_P_MIN && kdf->p <= DSD_BACKUP_P_MAX;
}

/*
 * scrypt with maxmem at OpenSSL's own requirement, 128 * r * (N + p + 2) bytes, plus slack, so any parameters the
 * callers let through can run. Callers bound N, r and p, which keeps the product far below 2^64.
 */
static int
dsd_backup_scrypt(const uint8_t* pass, size_t pass_len, const uint8_t* salt, size_t salt_len, uint64_t n, uint32_t r,
                  uint32_t p, uint8_t* key, size_t key_len) {
    const uint64_t maxmem = (128U * (uint64_t)r * (n + (uint64_t)p + 2U)) + DSD_BACKUP_SCRYPT_MEM_SLACK;
    if (EVP_PBE_scrypt((const char*)pass, pass_len, salt, salt_len, n, r, p, maxmem, key, key_len) != 1) {
        OPENSSL_cleanse(key, key_len);
        return DSD_BACKUP_ERR_INTERNAL;
    }
    return DSD_BACKUP_OK;
}

static int
dsd_backup_header_key(const uint8_t* pass, size_t pass_len, const uint8_t* header, const dsd_backup_kdf_params* kdf,
                      uint8_t* key) {
    return dsd_backup_scrypt(pass, pass_len, header + DSD_BACKUP_OFF_SALT, DSD_BACKUP_SALT_BYTES,
                             (uint64_t)1U << kdf->log2_n, kdf->r, kdf->p, key, DSD_BACKUP_KEY_BYTES);
}

/* AES-256-GCM over `len` bytes with the header as AAD; writes `len` ciphertext bytes to `out` and the tag. */
static int
dsd_backup_gcm_encrypt(const uint8_t* key, const uint8_t* header, const uint8_t* in, size_t len, uint8_t* out,
                       uint8_t* tag) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        return DSD_BACKUP_ERR_INTERNAL;
    }

    uint8_t final_block[DSD_BACKUP_TAG_BYTES];
    int outl = 0;
    int ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1
             && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)DSD_BACKUP_NONCE_BYTES, NULL) == 1
             && EVP_EncryptInit_ex(ctx, NULL, NULL, key, header + DSD_BACKUP_OFF_NONCE) == 1
             && EVP_EncryptUpdate(ctx, NULL, &outl, header, (int)DSD_BACKUP_HEADER_BYTES) == 1;
    if (ok && len > 0U) {
        ok = EVP_EncryptUpdate(ctx, out, &outl, in, (int)len) == 1 && outl == (int)len;
    }
    ok = ok && EVP_EncryptFinal_ex(ctx, final_block, &outl) == 1 && outl == 0
         && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, (int)DSD_BACKUP_TAG_BYTES, tag) == 1;

    /* Freeing the context also clears its key schedule. */
    EVP_CIPHER_CTX_free(ctx);
    return ok ? DSD_BACKUP_OK : DSD_BACKUP_ERR_INTERNAL;
}

/* Decrypt and verify; returns ERR_AUTH on a tag mismatch. The caller wipes `out` on any failure. */
static int
dsd_backup_gcm_decrypt(const uint8_t* key, const uint8_t* header, const uint8_t* in, size_t len, const uint8_t* tag,
                       uint8_t* out) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        return DSD_BACKUP_ERR_INTERNAL;
    }

    uint8_t expected_tag[DSD_BACKUP_TAG_BYTES];
    uint8_t final_block[DSD_BACKUP_TAG_BYTES];
    DSD_MEMCPY(expected_tag, tag, sizeof(expected_tag));
    int outl = 0;
    int ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1
             && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)DSD_BACKUP_NONCE_BYTES, NULL) == 1
             && EVP_DecryptInit_ex(ctx, NULL, NULL, key, header + DSD_BACKUP_OFF_NONCE) == 1
             && EVP_DecryptUpdate(ctx, NULL, &outl, header, (int)DSD_BACKUP_HEADER_BYTES) == 1;
    if (ok && len > 0U) {
        ok = EVP_DecryptUpdate(ctx, out, &outl, in, (int)len) == 1 && outl == (int)len;
    }
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (int)DSD_BACKUP_TAG_BYTES, expected_tag) == 1;

    int status = ok ? DSD_BACKUP_OK : DSD_BACKUP_ERR_INTERNAL;
    if (ok && (EVP_DecryptFinal_ex(ctx, final_block, &outl) != 1 || outl != 0)) {
        status = DSD_BACKUP_ERR_AUTH;
    }
    EVP_CIPHER_CTX_free(ctx);
    return status;
}

/* Full header and length validation; no key work. Fills both outputs on success. */
static int
dsd_backup_parse(const uint8_t* sealed, size_t sealed_len, dsd_backup_kdf_params* kdf, size_t* payload_len) {
    if (sealed == NULL && sealed_len != 0U) {
        return DSD_BACKUP_ERR_ARGS;
    }
    if (sealed_len < DSD_BACKUP_HEADER_BYTES || memcmp(sealed, DSD_BACKUP_MAGIC, DSD_BACKUP_MAGIC_BYTES) != 0) {
        return DSD_BACKUP_ERR_FORMAT;
    }
    if (dsd_backup_get_be(sealed + DSD_BACKUP_OFF_VERSION, 2U) != DSD_BACKUP_VERSION) {
        return DSD_BACKUP_ERR_VERSION;
    }
    if (dsd_backup_get_be(sealed + DSD_BACKUP_OFF_RESERVED, DSD_BACKUP_RESERVED_BYTES) != 0U) {
        return DSD_BACKUP_ERR_FORMAT;
    }

    dsd_backup_kdf_params parsed;
    parsed.log2_n = sealed[DSD_BACKUP_OFF_LOG2_N];
    parsed.r = (uint32_t)dsd_backup_get_be(sealed + DSD_BACKUP_OFF_R, 4U);
    parsed.p = (uint32_t)dsd_backup_get_be(sealed + DSD_BACKUP_OFF_P, 4U);
    if (sealed[DSD_BACKUP_OFF_KDF] != DSD_BACKUP_KDF_SCRYPT || !dsd_backup_kdf_params_accepted(&parsed)) {
        return DSD_BACKUP_ERR_PARAMS;
    }

    /* Bounded first, so the sum below cannot wrap. */
    const uint64_t len = dsd_backup_get_be(sealed + DSD_BACKUP_OFF_LENGTH, 8U);
    if (len > DSD_BACKUP_MAX_PAYLOAD_BYTES) {
        return DSD_BACKUP_ERR_TOO_LARGE;
    }
    if ((uint64_t)sealed_len != DSD_BACKUP_HEADER_BYTES + len + DSD_BACKUP_TAG_BYTES) {
        return DSD_BACKUP_ERR_FORMAT;
    }

    *kdf = parsed;
    *payload_len = (size_t)len;
    return DSD_BACKUP_OK;
}

static int
dsd_backup_seal_check(const uint8_t* pass, size_t pass_len, const dsd_backup_kdf_params* kdf, const uint8_t* payload,
                      size_t payload_len, const uint8_t* out, size_t out_cap, const size_t* out_len) {
    if (out_len == NULL || pass == NULL || pass_len == 0U || kdf == NULL || (payload == NULL && payload_len != 0U)
        || (out == NULL && out_cap != 0U)) {
        return DSD_BACKUP_ERR_ARGS;
    }
    if (payload_len > DSD_BACKUP_MAX_PAYLOAD_BYTES) {
        return DSD_BACKUP_ERR_TOO_LARGE;
    }
    if (!dsd_backup_kdf_params_accepted(kdf)) {
        return DSD_BACKUP_ERR_PARAMS;
    }
    if (out_cap < dsd_backup_sealed_size(payload_len)) {
        return DSD_BACKUP_ERR_ARGS;
    }
    return DSD_BACKUP_OK;
}

/* Arguments already checked. Salt and nonce go into a local header before `out` is written, so they may alias it. */
static int
dsd_backup_seal_core(const uint8_t* pass, size_t pass_len, const dsd_backup_kdf_params* kdf, const uint8_t* payload,
                     size_t payload_len, const uint8_t* salt, const uint8_t* nonce, uint8_t* out, size_t* out_len) {
    uint8_t header[DSD_BACKUP_HEADER_BYTES];
    DSD_MEMCPY(header, DSD_BACKUP_MAGIC, DSD_BACKUP_MAGIC_BYTES);
    dsd_backup_put_be(header + DSD_BACKUP_OFF_VERSION, DSD_BACKUP_VERSION, 2U);
    header[DSD_BACKUP_OFF_KDF] = (uint8_t)DSD_BACKUP_KDF_SCRYPT;
    header[DSD_BACKUP_OFF_LOG2_N] = kdf->log2_n;
    dsd_backup_put_be(header + DSD_BACKUP_OFF_R, kdf->r, 4U);
    dsd_backup_put_be(header + DSD_BACKUP_OFF_P, kdf->p, 4U);
    DSD_MEMCPY(header + DSD_BACKUP_OFF_SALT, salt, DSD_BACKUP_SALT_BYTES);
    DSD_MEMCPY(header + DSD_BACKUP_OFF_NONCE, nonce, DSD_BACKUP_NONCE_BYTES);
    dsd_backup_put_be(header + DSD_BACKUP_OFF_LENGTH, (uint64_t)payload_len, 8U);
    DSD_MEMSET(header + DSD_BACKUP_OFF_RESERVED, 0, DSD_BACKUP_RESERVED_BYTES);

    uint8_t key[DSD_BACKUP_KEY_BYTES];
    int status = dsd_backup_header_key(pass, pass_len, header, kdf, key);
    if (status == DSD_BACKUP_OK) {
        uint8_t* ciphertext = out + DSD_BACKUP_HEADER_BYTES;
        status = dsd_backup_gcm_encrypt(key, header, payload, payload_len, ciphertext, ciphertext + payload_len);
    }
    OPENSSL_cleanse(key, sizeof(key));

    const size_t sealed_len = dsd_backup_sealed_size(payload_len);
    if (status != DSD_BACKUP_OK) {
        OPENSSL_cleanse(out, sealed_len);
        return status;
    }
    DSD_MEMCPY(out, header, sizeof(header));
    *out_len = sealed_len;
    return DSD_BACKUP_OK;
}

void
dsd_backup_kdf_params_default(dsd_backup_kdf_params* out) {
    if (out == NULL) {
        return;
    }
    out->log2_n = 16U;
    out->r = 8U;
    out->p = 2U;
}

size_t
dsd_backup_sealed_size(size_t payload_len) {
    if (payload_len > DSD_BACKUP_MAX_PAYLOAD_BYTES) {
        return 0U;
    }
    return DSD_BACKUP_HEADER_BYTES + payload_len + DSD_BACKUP_TAG_BYTES;
}

int
dsd_backup_parse_header(const uint8_t* sealed, size_t sealed_len, dsd_backup_kdf_params* kdf_out,
                        size_t* payload_len_out) {
    dsd_backup_kdf_params kdf;
    size_t payload_len = 0U;
    const int status = dsd_backup_parse(sealed, sealed_len, &kdf, &payload_len);
    if (status != DSD_BACKUP_OK) {
        return status;
    }
    if (kdf_out != NULL) {
        *kdf_out = kdf;
    }
    if (payload_len_out != NULL) {
        *payload_len_out = payload_len;
    }
    return DSD_BACKUP_OK;
}

int
dsd_backup_seal(const uint8_t* pass, size_t pass_len, const dsd_backup_kdf_params* kdf, const uint8_t* payload,
                size_t payload_len, uint8_t* out, size_t out_cap, size_t* out_len) {
    if (out_len != NULL) {
        *out_len = 0U;
    }
    const int status = dsd_backup_seal_check(pass, pass_len, kdf, payload, payload_len, out, out_cap, out_len);
    if (status != DSD_BACKUP_OK) {
        return status;
    }

    uint8_t salt[DSD_BACKUP_SALT_BYTES];
    uint8_t nonce[DSD_BACKUP_NONCE_BYTES];
    if (RAND_bytes(salt, (int)sizeof(salt)) != 1 || RAND_bytes(nonce, (int)sizeof(nonce)) != 1) {
        return DSD_BACKUP_ERR_INTERNAL;
    }
    return dsd_backup_seal_core(pass, pass_len, kdf, payload, payload_len, salt, nonce, out, out_len);
}

int
dsd_backup_seal_with(const uint8_t* pass, size_t pass_len, const dsd_backup_kdf_params* kdf, const uint8_t* payload,
                     size_t payload_len, const uint8_t* salt, const uint8_t* nonce, uint8_t* out, size_t out_cap,
                     size_t* out_len) {
    if (out_len != NULL) {
        *out_len = 0U;
    }
    if (salt == NULL || nonce == NULL) {
        return DSD_BACKUP_ERR_ARGS;
    }
    const int status = dsd_backup_seal_check(pass, pass_len, kdf, payload, payload_len, out, out_cap, out_len);
    if (status != DSD_BACKUP_OK) {
        return status;
    }
    return dsd_backup_seal_core(pass, pass_len, kdf, payload, payload_len, salt, nonce, out, out_len);
}

int
dsd_backup_open(const uint8_t* pass, size_t pass_len, const uint8_t* sealed, size_t sealed_len, uint8_t* out,
                size_t out_cap, size_t* out_len) {
    if (out_len == NULL) {
        return DSD_BACKUP_ERR_ARGS;
    }
    *out_len = 0U;
    if (pass == NULL || pass_len == 0U || (out == NULL && out_cap != 0U)) {
        return DSD_BACKUP_ERR_ARGS;
    }

    dsd_backup_kdf_params kdf;
    size_t payload_len = 0U;
    int status = dsd_backup_parse(sealed, sealed_len, &kdf, &payload_len);
    if (status != DSD_BACKUP_OK) {
        return status;
    }
    if (out_cap < payload_len) {
        return DSD_BACKUP_ERR_ARGS;
    }

    uint8_t key[DSD_BACKUP_KEY_BYTES];
    status = dsd_backup_header_key(pass, pass_len, sealed, &kdf, key);
    if (status == DSD_BACKUP_OK) {
        const uint8_t* ciphertext = sealed + DSD_BACKUP_HEADER_BYTES;
        status = dsd_backup_gcm_decrypt(key, sealed, ciphertext, payload_len, ciphertext + payload_len, out);
    }
    OPENSSL_cleanse(key, sizeof(key));

    if (status != DSD_BACKUP_OK) {
        /* Never release plaintext that failed authentication. */
        if (payload_len > 0U) {
            OPENSSL_cleanse(out, payload_len);
        }
        return status;
    }
    *out_len = payload_len;
    return DSD_BACKUP_OK;
}

int
dsd_backup_derive_key(const uint8_t* pass, size_t pass_len, const uint8_t* salt, size_t salt_len, uint64_t n,
                      uint32_t r, uint32_t p, uint8_t* key, size_t key_len) {
    if (pass == NULL || pass_len == 0U || (salt == NULL && salt_len != 0U) || key == NULL || key_len == 0U) {
        return DSD_BACKUP_ERR_ARGS;
    }
    if (n < 2U || (n & (n - 1U)) != 0U || n > ((uint64_t)1U << DSD_BACKUP_SCRYPT_LOG2_N_MAX) || r == 0U || p == 0U
        || (uint64_t)r * p > DSD_BACKUP_SCRYPT_PR_MAX) {
        return DSD_BACKUP_ERR_PARAMS;
    }
    /*
     * RFC 7914 section 2: N < 2^(128 * r / 8). With N <= 2^20 only r == 1 can break it; r < 4 keeps the shift
     * below 64.
     */
    if (r < 4U && n >= ((uint64_t)1U << (16U * r))) {
        return DSD_BACKUP_ERR_PARAMS;
    }
    return dsd_backup_scrypt(pass, pass_len, salt, salt_len, n, r, p, key, key_len);
}
