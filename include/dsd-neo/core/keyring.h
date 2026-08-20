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
#include <dsd-neo/core/key_material.h>
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

/** Drop every DMR talkgroup -> key ID mapping and the per-slot applied- and skipped-notice latches. */
void keyring_dmr_tg_map_reset(dsd_state* state);

/**
 * keyring_dmr_kid_for_call() plus the once-per-call-epoch operator notice.
 *
 * Use this on the voice path, where a call epoch exists to latch the notice against. Other
 * consumers want keyring_dmr_kid_for_call(), which is silent.
 */
uint8_t keyring_dmr_slot_kid_for_call(dsd_state* state, int slot, const dsd_call_snapshot* call,
                                      dsd_key_material_need need, uint8_t signaled_kid);

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
 * Report whether a key ID holds a complete Kirisun (ALG 0x36/0x37) quartet.
 *
 * The Kirisun half of decryptability cannot be expressed as rkey/aes_loaded: it needs all four
 * AES segments present *and* strictly non-zero. keyring_kid_material() answers the scalar half of
 * "could this key id decrypt?"; this answers the Kirisun half, so classification and lockout can
 * evaluate a --dmr-tg-key-csv key id before any voice frame has activated it.
 *
 * Predicts exactly what dsd_dmr_kirisun_slot_key_complete() reports for a slot once
 * keyring_activate_slot_with_kid() has installed @p key_id into it.
 *
 * @return 1 when the key ID would activate as a complete Kirisun key.
 */
int keyring_kid_kirisun_complete(const dsd_state* state, int key_id);

/**
 * @brief Whether an imported key ID holds the material @p need calls for.
 *
 * The gate `keyring_dmr_effective_kid()` applies to a --dmr-tg-key-csv row. "Has any bytes at
 * all" is the wrong question twice over: a scalar cannot serve an AES ALG, and the flat
 * rkey_array aliases segment N of key K onto the scalar of key K + offset, so one unrelated
 * import can make an empty key ID look populated. Requiring the ALG's actual material answers
 * both -- an accidental match then needs as many colliding keys as the ALG needs segments.
 *
 * Tests non-zero segment cells rather than rkey_array_loaded, because a loaded-but-zero cell
 * activates as A_i == 0. Classification must predict activation.
 *
 * @return 1 when @p key_id satisfies @p need; 0 otherwise, including for DSD_KEY_NEED_NONE.
 */
int keyring_kid_satisfies_need(const dsd_state* state, int key_id, dsd_key_material_need need);

/**
 * Key ID a DMR slot should decrypt with (--dmr-tg-key-csv).
 *
 * A map row for `target` replaces the OTA-signaled key ID, but only when the mapped ID holds the
 * material @p need calls for: a row naming a key that cannot serve this ALG would otherwise zero
 * the slot key and shadow a signaled ID that would have worked, arming a session-permanent
 * lockout on a call that was decryptable. `need` comes from the caller because the ALG numbering
 * is the caller's -- voice IDs in core/audio, DMR data IDs in protocol/dmr.
 *
 * `target_is_group` is load-bearing, not defensive -- DMR radio IDs share the talkgroup's
 * 24-bit space, so an unchecked match keys a unit call off a colliding row.
 *
 * Returns `signaled_kid` unchanged whenever the map does not apply, so callers activate one
 * key ID rather than maintaining a map path and a signaled path in parallel.
 *
 * @param out_mapped set to 1 when a map row was applied, 0 otherwise (may be NULL)
 */
uint8_t keyring_dmr_effective_kid(const dsd_state* state, uint32_t target, int target_is_group,
                                  dsd_key_material_need need, uint8_t signaled_kid, int* out_mapped);

/** Activate imported key material for a slot using an explicit key ID. */
void keyring_activate_slot_with_kid(dsd_state* state, int slot, int key_id);

/**
 * keyring_dmr_effective_kid() for a call snapshot the caller already holds.
 *
 * Applies only to an active DMR group-voice call with a usable talkgroup; returns
 * `signaled_kid` for anything else. Announces nothing -- see keyring_dmr_slot_kid_for_call().
 */
uint8_t keyring_dmr_kid_for_call(const dsd_state* state, const dsd_call_snapshot* call, dsd_key_material_need need,
                                 uint8_t signaled_kid, int* out_mapped);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_KEYRING_H_H */
