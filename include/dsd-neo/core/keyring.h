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

#include <dsd-neo/core/call_state.h>
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

/** Drop every DMR talkgroup -> key ID mapping and the per-slot applied-notice latch. */
void keyring_dmr_tg_map_reset(dsd_state* state);

/**
 * Activate imported key material for a DMR slot whose active call talkgroup is mapped,
 * using the mapped key id in place of the OTA-signaled one (--dmr-tg-key-csv).
 *
 * Self-gated: applies only under DMR sync with the CSV keyring armed and a usable
 * slot ALG ID, and never rewrites the OTA payload_keyid. Returns 1 when applied.
 */
int keyring_dmr_tg_map_activate_slot(dsd_opts* opts, dsd_state* state, int slot);

/**
 * Report the imported key material behind a key ID without activating it.
 *
 * Classification and lockout must ask "could this key id decrypt?" before any voice frame
 * has run, which the activation path cannot answer without mutating R/A1..A4.
 *
 * Segment 0 shares the scalar key's index, so a scalar key also reports aes_loaded -- this
 * deliberately mirrors keyring_activate_slot_with_kid() so classification and activation
 * cannot disagree.
 *
 * @param out_rkey       scalar key, 0 when none (may be NULL)
 * @param out_aes_loaded 1 when any AES segment is present (may be NULL)
 * @return 1 when the key ID has any material at all.
 */
int keyring_kid_material(const dsd_state* state, int key_id, unsigned long long* out_rkey, int* out_aes_loaded);

/**
 * Key ID a DMR slot should decrypt with (--dmr-tg-key-csv).
 *
 * A map row for `target` replaces the OTA-signaled key ID, but only when the mapped ID has
 * imported material: a row naming an unimported key would otherwise zero the slot key and
 * shadow a signaled ID that would have worked, leaving the operator worse off than not
 * mapping the talkgroup at all.
 *
 * `target_is_group` is load-bearing, not defensive -- DMR radio IDs share the talkgroup's
 * 24-bit space, so an unchecked match keys a unit call off a colliding row.
 *
 * Returns `signaled_kid` unchanged whenever the map does not apply, so callers activate one
 * key ID rather than maintaining a map path and a signaled path in parallel.
 *
 * @param out_mapped set to 1 when a map row was applied, 0 otherwise (may be NULL)
 */
uint8_t keyring_dmr_effective_kid(const dsd_state* state, uint32_t target, int target_is_group, uint8_t signaled_kid,
                                  int* out_mapped);

/** Activate imported key material for a slot using an explicit key ID. */
void keyring_activate_slot_with_kid(dsd_state* state, int slot, int key_id);

/**
 * keyring_dmr_effective_kid() for a call snapshot the caller already holds.
 *
 * Applies only to an active DMR group-voice call with a usable talkgroup; returns
 * `signaled_kid` for anything else. Announces nothing -- see keyring_dmr_slot_kid_for_call().
 */
uint8_t keyring_dmr_kid_for_call(const dsd_state* state, const dsd_call_snapshot* call, uint8_t signaled_kid,
                                 int* out_mapped);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_KEYRING_H_H */
