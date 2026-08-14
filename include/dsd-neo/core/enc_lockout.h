// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Session-permanent encrypted-target lockout ledger.
 *
 * One protocol-agnostic store replaces the per-protocol encrypted-call
 * memories (the P25 transient TTL cache and the DMR/NXDN "ENC LO"
 * talkgroup-policy entries). An entry is armed by confirmed encrypted voice
 * on a target the user cannot decrypt and blocks voice-grant tuning for the
 * rest of the session; there is no retry backoff.
 *
 * Release paths, in decreasing strength:
 *  - explicit clear evidence (clear service options, regroup clear-key
 *    override, or a call that resolves decryptable) removes the entry. This
 *    store honours every such request; a protocol may still rate-limit how
 *    often it believes the weakest form of that evidence -- P25 spaces out
 *    re-admissions driven by a bare grant service bit, because sites whose
 *    grant bits do not track encryption repeat one per second for the life of
 *    the encrypted call;
 *  - new key material bumps the global key epoch instead of dropping
 *    entries: a stale-epoch entry stops blocking, the next grant runs one
 *    silent classification probe, and the outcome either re-locks the entry
 *    at the current epoch or releases it;
 *  - the user purges the ledger or re-enables encrypted-call tuning (the
 *    ledger is inert while opts->trunk_tune_enc_calls != 0).
 *
 * Threading: the decoder thread owns all mutation. UI readers
 * (dsd_enc_lockout_entry_active / _active_count for lock markers and menu
 * counts) read without synchronization, which is deliberately best effort --
 * the worst case is a lock marker that renders one frame late or early. No
 * decoding, tuning, or audio decision may be made off the decoder thread.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_ENC_LOCKOUT_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_ENC_LOCKOUT_H_H

#include <dsd-neo/platform/platform.h>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

enum DSD_ATTR_PACKED { DSD_ENC_LOCKOUT_MAX = 128 };

/** ALGID sentinel for lockouts armed without definitive crypto metadata. */
enum DSD_ATTR_PACKED { DSD_ENC_LOCKOUT_ALGID_UNKNOWN = -1 };

typedef struct {
    uint64_t key_epoch; /**< state->enc_lockout_key_epoch at last confirmation */
    uint64_t last_seq;  /**< monotonic confirmation ticket; recency key for eviction */
    time_t last_seen;   /**< wall clock of last confirmation (UI display only) */
    uint32_t target;    /**< group or private destination id */
    uint32_t hits;      /**< number of lockout confirmations */
    int16_t algid;      /**< last confirmed ALGID, DSD_ENC_LOCKOUT_ALGID_UNKNOWN if none */
    uint16_t keyid;     /**< last confirmed key id (0 when unknown) */
    uint8_t is_group;   /**< 1 = group/SG target, 0 = private destination */
    uint8_t in_use;
} dsd_enc_lockout_entry;

/**
 * @brief Record confirmed undecryptable encrypted voice on a target.
 *
 * Upserts the (target, is_group) entry at the current key epoch. A
 * non-negative @p algid refreshes the stored ALGID/KID evidence; pass
 * DSD_ENC_LOCKOUT_ALGID_UNKNOWN to keep previous evidence.
 *
 * @return 1 when the target newly locks (fresh entry, or an entry re-locked
 *         after a key-epoch change) — callers emit the lockout event exactly
 *         then; 0 for a same-epoch refresh or invalid input.
 */
int dsd_enc_lockout_note(dsd_state* state, uint32_t target, int is_group, int algid, int keyid);

/**
 * @brief Return non-zero while the target's lockout entry is current.
 *
 * A stale-epoch entry (key material changed since confirmation) reports 0 so
 * the next grant is admitted as a single classification probe. Ignores
 * opts; use dsd_enc_lockout_is_blocked() for tuning decisions.
 */
int dsd_enc_lockout_entry_active(const dsd_state* state, uint32_t target, int is_group);

/**
 * @brief Tuning-policy check: entry active and encrypted-call tuning disabled.
 *
 * Returns 0 whenever opts->trunk_tune_enc_calls != 0 so the ledger is inert
 * while the user follows encrypted calls.
 */
int dsd_enc_lockout_is_blocked(const dsd_opts* opts, const dsd_state* state, uint32_t target, int is_group);

/** Copy the target's entry to @p out (may be stale). @return 1 when found. */
int dsd_enc_lockout_lookup(const dsd_state* state, uint32_t target, int is_group, dsd_enc_lockout_entry* out);

/** Remove the target's entry on clear/decryptable evidence. @return 1 when removed. */
int dsd_enc_lockout_release(dsd_state* state, uint32_t target, int is_group);

/**
 * @brief Invalidate all entries after key material changed.
 *
 * Entries are retained with their old epoch: each affected target re-verifies
 * with one probe on its next grant instead of every target re-probing on a
 * timer.
 */
void dsd_enc_lockout_bump_key_epoch(dsd_state* state);

/** Drop every entry (user purge). The key epoch is preserved. */
void dsd_enc_lockout_clear_all(dsd_state* state);

/** Number of entries still locking at the current key epoch. */
int dsd_enc_lockout_active_count(const dsd_state* state);

/** Reset ledger storage and epoch to boot defaults (state init only). */
void dsd_enc_lockout_init(dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_ENC_LOCKOUT_H_H */
