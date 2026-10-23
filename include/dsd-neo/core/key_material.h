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
 * A cell is present when it was imported (rkey_array_loaded), whatever its value, or holds a non-zero value
 * (keyring_kid_scalar_present()). DSD_KEY_NEED_AES_4 and DSD_KEY_NEED_QUARTET are not redundant: AES_4 asks that
 * all four segment cells be present; QUARTET asks that they also be non-zero, which is what
 * keyring_kid_kirisun_complete() predicts about activation.
 */
typedef enum {
    DSD_KEY_NEED_NONE = 0, /**< ALG consumes no keyring material; a map row cannot help it. */
    DSD_KEY_NEED_SCALAR,   /**< The key ID's own cell present. */
    DSD_KEY_NEED_AES_2,    /**< First 2 segments present (DMR AES-128, P25 AES-128). */
    DSD_KEY_NEED_AES_3,    /**< First 3 segments present (P25 TDEA). */
    DSD_KEY_NEED_AES_4,    /**< All 4 segments present (DMR/P25 AES-256). */
    DSD_KEY_NEED_QUARTET,  /**< Kirisun 0x36/0x37: a complete, all-non-zero quartet. */
} dsd_key_material_need;

/** The AES key width, in 64-bit segments, an AES need requires (2, 3 or 4); 0 for any other need. */
static inline unsigned int
dsd_key_need_aes_width(dsd_key_material_need need) {
    switch (need) {
        case DSD_KEY_NEED_AES_2: return 2U;
        case DSD_KEY_NEED_AES_3: return 3U;
        case DSD_KEY_NEED_AES_4: return 4U;
        case DSD_KEY_NEED_NONE:
        case DSD_KEY_NEED_SCALAR:
        case DSD_KEY_NEED_QUARTET: break;
    }
    return 0U;
}

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_KEY_MATERIAL_H_H */
