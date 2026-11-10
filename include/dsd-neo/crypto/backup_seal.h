// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Passphrase-sealed backup envelope (scrypt + AES-256-GCM).
 *
 * A sealed backup is a 64-byte header, the ciphertext (as long as the payload) and a 16-byte GCM tag. Header
 * integers are big-endian:
 *
 * | off | size | field                                   |
 * |-----|------|-----------------------------------------|
 * |   0 |    8 | ASCII "DSDNEOBK"                        |
 * |   8 |    2 | version (1)                             |
 * |  10 |    1 | kdf (1 = scrypt)                        |
 * |  11 |    1 | log2(N)                                 |
 * |  12 |    4 | r                                       |
 * |  16 |    4 | p                                       |
 * |  20 |   16 | salt                                    |
 * |  36 |   12 | AES-GCM nonce                           |
 * |  48 |    8 | ciphertext length                       |
 * |  56 |    8 | reserved, all zero                      |
 *
 * The key is scrypt(passphrase, salt, 2^log2_n, r, p) cut to 32 bytes, and the whole header is the GCM AAD, so
 * changing any header byte fails authentication. Both sides accept only log2_n 14..17, r 8 and p 1..4; sealed input
 * is untrusted, so those bounds also cap the memory and time an opened file can demand.
 *
 * The int-returning functions return a ::dsd_backup_status value (0 on success). Seal and open wipe the derived key
 * and free the cipher state before they return.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_BACKUP_SEAL_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_BACKUP_SEAL_H_

#include <stddef.h>
#include <stdint.h>

#define DSD_BACKUP_HEADER_BYTES      64U
#define DSD_BACKUP_TAG_BYTES         16U
#define DSD_BACKUP_SALT_BYTES        16U
#define DSD_BACKUP_NONCE_BYTES       12U
#define DSD_BACKUP_KEY_BYTES         32U
#define DSD_BACKUP_MAX_PAYLOAD_BYTES 67108864U /* 64 MiB, written out so no narrow product widens */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSD_BACKUP_OK = 0,
    DSD_BACKUP_ERR_ARGS = -1,      /**< NULL pointer with a length, empty passphrase or output buffer too small. */
    DSD_BACKUP_ERR_FORMAT = -2,    /**< Damaged: magic, then short, version 0, reserved bytes set or wrong length.
                                        dsd_backup_parse_header() and dsd_backup_open() also return it for a file with
                                        no magic; only dsd_backup_check_header() returns ERR_NOT_BACKUP. */
    DSD_BACKUP_ERR_VERSION = -3,   /**< Sealed backup of a newer format version (above 1). */
    DSD_BACKUP_ERR_PARAMS = -4,    /**< Unknown KDF or KDF parameters outside the accepted range. */
    DSD_BACKUP_ERR_AUTH = -5,      /**< Wrong passphrase or tampered data; nothing was released. */
    DSD_BACKUP_ERR_TOO_LARGE = -6, /**< Payload longer than ::DSD_BACKUP_MAX_PAYLOAD_BYTES. */
    DSD_BACKUP_ERR_INTERNAL = -7,  /**< OpenSSL failure (allocation, RNG, cipher). */
    DSD_BACKUP_ERR_NOT_BACKUP = -8 /**< No backup magic: not a sealed backup at all (dsd_backup_check_header()). */
} dsd_backup_status;

/** scrypt cost parameters stored in the header (N = 2^log2_n). */
typedef struct {
    uint8_t log2_n;
    uint32_t r;
    uint32_t p;
} dsd_backup_kdf_params;

/** Fill `out` with the parameters new backups use: log2_n 16, r 8, p 2. A NULL `out` is ignored. */
void dsd_backup_kdf_params_default(dsd_backup_kdf_params* out);

/** Sealed size for a payload of `payload_len` bytes (header + payload + tag), or 0 when it exceeds the maximum. */
size_t dsd_backup_sealed_size(size_t payload_len);

/** 1 when `bytes` holds at least the 8 magic bytes and they match, else 0 (also for NULL). */
int dsd_backup_has_magic(const uint8_t* bytes, size_t len);

/**
 * Classify the start of a document as a sealed backup, without deriving a key and without the whole file.
 *
 * Reads only the first min(`header_len`, ::DSD_BACKUP_HEADER_BYTES) bytes of `header`; `total_len` is the whole
 * document's length. The first failing check decides:
 *
 * 1. no magic (fewer than 8 bytes, or other bytes) → ERR_NOT_BACKUP;
 * 2. `header_len` below ::DSD_BACKUP_HEADER_BYTES → ERR_FORMAT;
 * 3. version 0 → ERR_FORMAT; version above 1 → ERR_VERSION;
 * 4. reserved bytes set → ERR_FORMAT;
 * 5. unknown KDF or parameters outside the accepted range → ERR_PARAMS;
 * 6. payload length above ::DSD_BACKUP_MAX_PAYLOAD_BYTES → ERR_TOO_LARGE;
 * 7. `total_len` other than header + payload + tag → ERR_FORMAT.
 *
 * A NULL `header` with a non-zero `header_len` is ERR_ARGS. Steps 1-6 need no length, so a caller that does not know
 * it can pass UINT64_MAX: a passing header then ends in ERR_FORMAT at step 7.
 *
 * @param kdf_out          Optional; receives the header's KDF parameters on success.
 * @param payload_len_out  Optional; receives the payload length on success.
 *
 * @return DSD_BACKUP_OK, or ERR_ARGS / ERR_NOT_BACKUP / ERR_FORMAT / ERR_VERSION / ERR_PARAMS / ERR_TOO_LARGE.
 */
int dsd_backup_check_header(const uint8_t* header, size_t header_len, uint64_t total_len,
                            dsd_backup_kdf_params* kdf_out, size_t* payload_len_out);

/**
 * Validate a sealed backup's header and overall length without deriving a key.
 *
 * dsd_backup_check_header() over the whole `sealed` buffer, except that a missing magic is ERR_FORMAT.
 *
 * @param kdf_out          Optional; receives the header's KDF parameters on success.
 * @param payload_len_out  Optional; receives the payload length on success.
 *
 * @return DSD_BACKUP_OK, or ERR_ARGS / ERR_FORMAT / ERR_VERSION / ERR_PARAMS / ERR_TOO_LARGE.
 */
int dsd_backup_parse_header(const uint8_t* sealed, size_t sealed_len, dsd_backup_kdf_params* kdf_out,
                            size_t* payload_len_out);

/**
 * Seal `payload` under `pass` with a fresh random salt and nonce.
 *
 * `out` needs dsd_backup_sealed_size(payload_len) bytes and must not overlap `payload`. `*out_len` is the sealed
 * size on success and 0 on failure, and a seal that fails after writing to `out` wipes what it wrote.
 */
int dsd_backup_seal(const uint8_t* pass, size_t pass_len, const dsd_backup_kdf_params* kdf, const uint8_t* payload,
                    size_t payload_len, uint8_t* out, size_t out_cap, size_t* out_len);

/**
 * dsd_backup_seal() with a caller-chosen salt (::DSD_BACKUP_SALT_BYTES) and nonce (::DSD_BACKUP_NONCE_BYTES).
 *
 * For known-answer tests only: reusing a nonce with the same passphrase and salt breaks AES-GCM.
 */
int dsd_backup_seal_with(const uint8_t* pass, size_t pass_len, const dsd_backup_kdf_params* kdf, const uint8_t* payload,
                         size_t payload_len, const uint8_t* salt, const uint8_t* nonce, uint8_t* out, size_t out_cap,
                         size_t* out_len);

/**
 * Authenticate and decrypt a sealed backup.
 *
 * `out` needs the payload length reported by dsd_backup_parse_header() and must not overlap `sealed`. `*out_len` is
 * the payload length on success and 0 on failure. On ERR_AUTH (wrong passphrase or tampering) every byte written to
 * `out` has been wiped, so no unauthenticated plaintext is released. The header fails as in dsd_backup_parse_header()
 * (a missing magic is ERR_FORMAT).
 */
int dsd_backup_open(const uint8_t* pass, size_t pass_len, const uint8_t* sealed, size_t sealed_len, uint8_t* out,
                    size_t out_cap, size_t* out_len);

/**
 * Raw scrypt (RFC 7914) for known-answer tests, without the header's parameter policy.
 *
 * Test/KAT helper only: never pass it untrusted or user-supplied parameters. It caps N but not memory (r = 2^16 with
 * N = 2^20 already asks scrypt for 8 TiB). Production code uses dsd_backup_seal() and dsd_backup_open(), which bound
 * the parameters.
 *
 * Still refuses N that is not a power of two >= 2, N > 2^20, N >= 2^(16 r), r or p of 0, and r * p >= 2^30
 * (ERR_PARAMS). `key` is wiped when OpenSSL fails (ERR_INTERNAL).
 */
int dsd_backup_derive_key(const uint8_t* pass, size_t pass_len, const uint8_t* salt, size_t salt_len, uint64_t n,
                          uint32_t r, uint32_t p, uint8_t* key, size_t key_len);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_BACKUP_SEAL_H_ */
