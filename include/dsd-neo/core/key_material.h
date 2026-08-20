// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief What kind of key material an encryption algorithm needs.
 *
 * Shared vocabulary, not keyring-specific: the ALG tables live with the modules that own their
 * numbering (voice IDs in core/audio, DMR data IDs in protocol/dmr), while the keyring answers
 * whether a key ID satisfies a need. Its own header so that including it costs nothing --
 * <dsd-neo/core/audio.h> otherwise pulls only forward-declaration headers.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_KEY_MATERIAL_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_KEY_MATERIAL_H_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Key material an ALG requires before a key ID can serve it.
 *
 * DSD_KEY_NEED_AES_4 and DSD_KEY_NEED_QUARTET are not redundant. AES_4 asks only that all four
 * segment cells be non-zero; QUARTET additionally requires keyring_aes_segment_count() to report
 * four, which is what keyring_kid_kirisun_complete() predicts about activation. Four non-zero
 * cells of which only two are flagged rkey_array_loaded satisfies the first and not the second.
 */
typedef enum {
    DSD_KEY_NEED_NONE = 0, /**< ALG consumes no keyring material; a map row cannot help it. */
    DSD_KEY_NEED_SCALAR,   /**< rkey_array[kid] non-zero. */
    DSD_KEY_NEED_AES_2,    /**< First 2 segments non-zero (DMR AES-128, P25 AES-128). */
    DSD_KEY_NEED_AES_3,    /**< First 3 segments non-zero (P25 TDEA). */
    DSD_KEY_NEED_AES_4,    /**< All 4 segments non-zero (DMR/P25 AES-256). */
    DSD_KEY_NEED_QUARTET,  /**< Kirisun 0x36/0x37: a complete, all-non-zero quartet. */
} dsd_key_material_need;

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_KEY_MATERIAL_H_H */
