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
 * The table belongs to one system (`p25_patch_owner_*`, dsd_p25_patch_note_system()); carrier boundaries that leave the
 * system empty it, and trunk scan and Signal search keep a system's table aside in a dsd_p25_patch_snapshot.
 *
 * This header is included by state.h for the capacities, so it only needs the forward declaration.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_P25_PATCH_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_P25_PATCH_H_H

#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>
#include <time.h>

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

/**
 * @brief Note a P25 system identity (WACN, SYSID) decoded from content that passed its CRC/FEC check.
 *
 * The table belongs to one system. An unknown owner adopts the first identity and keeps the table; the owner's own
 * identity changes nothing; another system's empties the table (dsd_p25_patch_clear_all()) and becomes the owner.
 * A zero identity (WACN 0 and SYSID 0) names no system and is ignored. Called wherever WACN/SYSID are decoded from the
 * air, whatever the descrambler seed is (a hand-set one included), and by p25_update_system_identity().
 *
 * @return 1 when it emptied the table, 0 otherwise.
 */
int dsd_p25_patch_note_system(dsd_state* state, uint32_t wacn, uint16_t sysid);

/**
 * @brief The whole patch table of one system, kept while the receiver is elsewhere.
 *
 * Every entry with its members, KEY context, stamps and recency, the sequence counter, the listening clock's reading
 * and the owner. The previous clock reading is not kept: a restore breaks continuity.
 */
typedef struct dsd_p25_patch_snapshot {
    double listen_s;
    double seen_listen_s[DSD_P25_PATCH_MAX];
    double members_listen_s[DSD_P25_PATCH_MAX];
    time_t last_update[DSD_P25_PATCH_MAX];
    int count;
    uint32_t wuid[DSD_P25_PATCH_MAX][DSD_P25_PATCH_RADIO_MAX];
    uint32_t seq[DSD_P25_PATCH_MAX];
    uint32_t seq_counter;
    uint32_t owner_wacn;
    uint16_t sgid[DSD_P25_PATCH_MAX];
    uint16_t key[DSD_P25_PATCH_MAX];
    uint16_t wgid[DSD_P25_PATCH_MAX][DSD_P25_PATCH_GROUP_MAX];
    uint16_t owner_sysid;
    uint8_t is_patch[DSD_P25_PATCH_MAX];
    uint8_t active[DSD_P25_PATCH_MAX];
    uint8_t wgid_count[DSD_P25_PATCH_MAX];
    uint8_t wuid_count[DSD_P25_PATCH_MAX];
    uint8_t alg[DSD_P25_PATCH_MAX];
    uint8_t ssn[DSD_P25_PATCH_MAX];
    uint8_t key_valid[DSD_P25_PATCH_MAX];
    uint8_t owner_known;
} dsd_p25_patch_snapshot;

/**
 * @brief Copy @p state's patch table into @p snapshot. NULL arguments do nothing.
 */
void dsd_p25_patch_snapshot_save(const dsd_state* state, dsd_p25_patch_snapshot* snapshot);

/**
 * @brief Put @p snapshot's patch table back on @p state, replacing the table there, and break listening continuity.
 *
 * NULL arguments do nothing.
 */
void dsd_p25_patch_snapshot_restore(dsd_state* state, const dsd_p25_patch_snapshot* snapshot);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_P25_PATCH_H_H */
