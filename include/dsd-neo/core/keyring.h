// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Keyring helper API.
 *
 * Declares the keyring loader implemented in core.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_KEYRING_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_KEYRING_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Activate imported key material for an explicit decoder slot. */
void keyring_activate_slot(dsd_opts* opts, dsd_state* state, int slot);
/** Return whether the first required AES segments exist for an imported key ID. */
int keyring_aes_segments_complete(const dsd_state* state, int key_id, unsigned int required_segments);

/** Look up the DMR talkgroup -> key ID override map. Returns 1 on a hit with *out_kid set. */
int keyring_dmr_tg_map_kid(const dsd_state* state, uint32_t tg, uint8_t* out_kid);

/**
 * Activate imported key material for a DMR slot whose active call talkgroup is mapped,
 * using the mapped key id in place of the OTA-signaled one (--dmr-tg-key-csv).
 *
 * Self-gated: applies only under DMR sync with the CSV keyring armed and a usable
 * slot ALG ID, and never rewrites the OTA payload_keyid. Returns 1 when applied.
 */
int keyring_dmr_tg_map_activate_slot(dsd_opts* opts, dsd_state* state, int slot);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_KEYRING_H_H */
