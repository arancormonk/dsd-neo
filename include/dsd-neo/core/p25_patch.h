// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 regroup/patch table: capacities, the listening clock and entry freshness.
 *
 * The patch table lives in `dsd_state` (`p25_patch_*`). Its freshness is judged on a per-system listening
 * clock rather than on wall or decode time: `p25_patch_listen_s` advances only while the receiver keeps
 * hearing the system's control channel (`dsd_p25_patch_listen_note()`), and stops at every departure
 * (`dsd_p25_patch_listen_break()`). Each entry carries two stamps on that clock:
 *  - `p25_patch_seen_listen_s[i]`: the last message that named the supergroup (grants, voice users, adds,
 *    regroup commands). It keeps the row.
 *  - `p25_patch_members_listen_s[i]`: the last membership message (regroup Add, regroup commands that carry
 *    a member or key context). It keeps the member lists and the KEY/ALG/SSN context usable.
 *
 * An entry or its members are current while the clock has moved at most DSD_P25_PATCH_TTL_SECONDS past the
 * stamp. Readers take a const state and never mutate it; writers expire non-current members first.
 * `p25_patch_seq[i]` (from `p25_patch_seq_counter`) orders entries by recency for display and eviction;
 * `p25_patch_last_update[i]` stays the decode-time display stamp.
 *
 * This header is included by state.h for the capacities, so it only needs the forward declaration.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_P25_PATCH_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_P25_PATCH_H_H

#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Concurrent supergroup entries in the table. */
#define DSD_P25_PATCH_MAX           16
/** Work groups stored per supergroup. */
#define DSD_P25_PATCH_GROUP_MAX     32
/** Radios (WUIDs) stored per supergroup. */
#define DSD_P25_PATCH_RADIO_MAX     16
/** Listening seconds an entry or its members stay current after their stamp. */
#define DSD_P25_PATCH_TTL_SECONDS   20
/** Largest gap between two control-channel PDUs that still counts as continuous listening. */
#define DSD_P25_PATCH_LISTEN_GAP_S  0.25
/** A stamp that is never current (expired members, an entry created without members). */
#define DSD_P25_PATCH_STAMP_NEVER_S (-1.0e9)

/**
 * @brief Advance the listening clock for one CRC-good control-channel PDU.
 *
 * Adds `now_m - last` to `p25_patch_listen_s` only when the previous reading exists and
 * `0 < now_m - last <= DSD_P25_PATCH_LISTEN_GAP_S`, then stores `now_m` as the previous reading.
 *
 * @param state Decoder state.
 * @param now_m Decode monotonic clock reading, in seconds.
 */
void dsd_p25_patch_listen_note(dsd_state* state, double now_m);

/**
 * @brief Break listening continuity: the next note only stores its reading.
 *
 * @param state Decoder state.
 */
void dsd_p25_patch_listen_break(dsd_state* state);

/**
 * @brief Whether entry @p idx was named within the TTL on the listening clock.
 *
 * Judges the seen stamp only; callers check `p25_patch_active` and the SG themselves.
 *
 * @return 1 when current, 0 otherwise or for a NULL state or an out-of-range index.
 */
int dsd_p25_patch_entry_seen_current(const dsd_state* state, int idx);

/**
 * @brief Whether entry @p idx's member lists and KEY/ALG/SSN context are current.
 *
 * @return 1 when current, 0 otherwise or for a NULL state or an out-of-range index.
 */
int dsd_p25_patch_members_current(const dsd_state* state, int idx);

/**
 * @brief Whether entry @p idx lists work group @p wgid among members that are current.
 */
int dsd_p25_patch_has_current_wgid(const dsd_state* state, int idx, uint16_t wgid);

/**
 * @brief How many refreshes happened since entry @p idx was last refreshed (wrap-safe recency).
 *
 * Smaller is more recent. Returns UINT32_MAX for a NULL state or an out-of-range index.
 */
uint32_t dsd_p25_patch_seq_age(const dsd_state* state, int idx);

/**
 * @brief Refresh entry @p idx's seen stamp and recency (a message that names the SG only).
 */
void dsd_p25_patch_touch_seen(dsd_state* state, int idx);

/**
 * @brief Refresh entry @p idx's seen and members stamps and its recency (a membership message).
 */
void dsd_p25_patch_touch_members(dsd_state* state, int idx);

/**
 * @brief Clear entry @p idx's work groups, radios and KEY/ALG/SSN context and mark its members expired.
 *
 * The entry itself (SG, type, active flag, seen stamp) is kept.
 */
void dsd_p25_patch_expire_members(dsd_state* state, int idx);

/**
 * @brief Expire entry @p idx's members when they are not current.
 *
 * Writers call this before changing an existing entry.
 *
 * @return 1 when it expired them, 0 when they were current (or nothing to do).
 */
int dsd_p25_patch_expire_stale_members(dsd_state* state, int idx);

/**
 * @brief Empty the whole table, its owner, sequence counter and listening clock.
 */
void dsd_p25_patch_clear_all(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_P25_PATCH_H_H */
