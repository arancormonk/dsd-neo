// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Talkgroup/private policy evaluation and shared mutation helpers.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_TALKGROUP_POLICY_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_TALKGROUP_POLICY_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSD_TG_POLICY_MATCH_NONE = 0,
    DSD_TG_POLICY_MATCH_RANGE = 1,
    DSD_TG_POLICY_MATCH_EXACT = 2,
} dsd_tg_policy_match_type;

typedef enum {
    DSD_TG_POLICY_SOURCE_IMPORTED = 0,
    DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS = 1,
    DSD_TG_POLICY_SOURCE_USER_LOCKOUT = 2,
    // Legacy/reserved: encrypted-call lockout no longer writes policy rows
    // (it lives in the core/enc_lockout.h ledger), but the value may still
    // appear in serialized policy tables from older sessions. Do not reuse.
    DSD_TG_POLICY_SOURCE_ENC_LOCKOUT = 3,
} dsd_tg_policy_entry_source;

typedef enum {
    DSD_TG_POLICY_UPSERT_ADD_IF_MISSING = 0,
    DSD_TG_POLICY_UPSERT_REPLACE_FIRST = 1,
    DSD_TG_POLICY_UPSERT_REPLACE_LEARNED_ONLY = 2,
} dsd_tg_policy_upsert_mode;

typedef enum {
    DSD_TG_POLICY_BLOCK_NONE = 0u,
    DSD_TG_POLICY_BLOCK_GROUP_DISABLED = 1u << 0,
    DSD_TG_POLICY_BLOCK_PRIVATE_DISABLED = 1u << 1,
    DSD_TG_POLICY_BLOCK_DATA_DISABLED = 1u << 2,
    DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED = 1u << 3,
    DSD_TG_POLICY_BLOCK_ALLOWLIST = 1u << 4,
    DSD_TG_POLICY_BLOCK_MODE = 1u << 5,
    DSD_TG_POLICY_BLOCK_HOLD = 1u << 6,
    DSD_TG_POLICY_BLOCK_AUDIO = 1u << 7,
    DSD_TG_POLICY_BLOCK_RECORD = 1u << 8,
    DSD_TG_POLICY_BLOCK_STREAM = 1u << 9,
    DSD_TG_POLICY_BLOCK_ENC_LOCKOUT = 1u << 10,
    DSD_TG_POLICY_BLOCK_SESSION_AVOID = 1u << 11,
    DSD_TG_POLICY_BLOCK_CALL_SKIP = 1u << 12,
} dsd_tg_policy_block_reason;

/** @brief Return the highest-priority diagnostic label for a block-reason mask. */
const char* dsd_tg_policy_block_reason_label(uint32_t block_reasons);

typedef struct {
    uint32_t id_start;
    uint32_t id_end;
    char mode[8];
    char name[50];
    char tags[50]; /* Free text from the CSV tags/category column; frontends only, never consulted by policy. */
    int priority;
    uint8_t preempt;
    uint8_t audio;
    uint8_t record;
    uint8_t stream;
    uint8_t is_range;
    uint8_t source;
    unsigned int row;
} dsd_tg_policy_entry;

typedef struct {
    dsd_tg_policy_match_type match;
    dsd_tg_policy_entry entry;
} dsd_tg_policy_lookup;

typedef struct {
    uint32_t target_id;
    uint32_t source_id;
    int encrypted;
    int data_call;
    int private_call;
    int tune_allowed;
    int audio_allowed;
    int record_allowed;
    int stream_allowed;
    int priority;
    int preempt_requested;
    uint32_t block_reasons;
    int tg_hold_active;
    int tg_hold_match;
    char mode[8];
    char name[50];
    dsd_tg_policy_match_type match;
} dsd_tg_policy_decision;

typedef struct {
    uint32_t target_id;
    uint32_t source_id;
    long freq_hz;
    int channel;
    int slot;
    int requires_tuner_retune;
} dsd_tg_policy_call_route;

int dsd_tg_policy_make_exact_entry(uint32_t id, const char* mode, const char* name, dsd_tg_policy_entry_source source,
                                   dsd_tg_policy_entry* out);
int dsd_tg_policy_add_range_entry(dsd_state* state, const dsd_tg_policy_entry* entry);
int dsd_tg_policy_lookup_id(const dsd_state* state, uint32_t id, dsd_tg_policy_lookup* out);
int dsd_tg_policy_has_entries(const dsd_state* state);
int dsd_tg_policy_lookup_label(const dsd_state* state, uint32_t id, char* mode, size_t mode_sz, char* name,
                               size_t name_sz);
int dsd_tg_policy_copy_snapshot(dsd_state* dst, const dsd_state* src);
int dsd_tg_policy_evaluate_group_call(const dsd_opts* opts, const dsd_state* state, uint32_t tg, uint32_t src,
                                      int encrypted, int data_call, dsd_tg_policy_decision* out);
int dsd_tg_policy_evaluate_private_call(const dsd_opts* opts, const dsd_state* state, uint32_t src, uint32_t dst,
                                        int encrypted, int data_call, dsd_tg_policy_decision* out);

/**
 * Blocks on a patched call's over-the-air supergroup (SG) that no member WG
 * overrides: a call skip, the encryption lockout (armed on the SG), a session
 * avoid or a mode B/DE row. Every member rides the SG's traffic, so an allowed
 * or held member must not readmit it. An allow-list or Hold miss on the SG is
 * not in the set: a listed or held member is how patch-aware following admits
 * the patch.
 */
#define DSD_TG_POLICY_BLOCK_OTA_FINAL                                                                                  \
    ((uint32_t)DSD_TG_POLICY_BLOCK_CALL_SKIP | (uint32_t)DSD_TG_POLICY_BLOCK_ENC_LOCKOUT                               \
     | (uint32_t)DSD_TG_POLICY_BLOCK_SESSION_AVOID | (uint32_t)DSD_TG_POLICY_BLOCK_MODE)

/**
 * @brief Merge a patched call's final supergroup blocks into its member decision.
 *
 * P25 judges a patched call on the member WG its grant matched (@p policy_target,
 * the decision in @p decision). When @p ota_target differs and its own group
 * evaluation carries any DSD_TG_POLICY_BLOCK_OTA_FINAL reason, those reasons are
 * merged, tuning is denied, and media is denied unless the only reason is the
 * encryption lockout, which never gated media. Nothing else from the supergroup
 * is merged.
 *
 * @return 1 when a block was merged; 0 when none applies, the targets match,
 *         @p ota_target is 0 or @p decision is NULL.
 */
int dsd_tg_policy_apply_ota_final_blocks(const dsd_opts* opts, const dsd_state* state, uint32_t ota_target,
                                         uint32_t policy_target, uint32_t src, dsd_tg_policy_decision* decision);
/**
 * @brief Evaluate a private grant while allowing entirely unlisted endpoints.
 *
 * Explicit source/destination policy entries and all non-allow-list gates still
 * apply. This is used by grant paths where group IDs and radio IDs are separate
 * namespaces, so an absent radio-ID entry is not itself a denial.
 */
int dsd_tg_policy_evaluate_private_grant(const dsd_opts* opts, const dsd_state* state, uint32_t src, uint32_t dst,
                                         int encrypted, int data_call, dsd_tg_policy_decision* out);
int dsd_tg_policy_append_exact(dsd_state* state, const dsd_tg_policy_entry* entry);
/** REPLACE_LEARNED_ONLY replaces a learned row only when its name or mode differs, so a repeat bumps no generation. */
int dsd_tg_policy_upsert_exact(dsd_state* state, const dsd_tg_policy_entry* entry, dsd_tg_policy_upsert_mode mode);

/** Outcome of dsd_tg_policy_learn_exact(). */
typedef enum {
    DSD_TG_POLICY_LEARN_FAILED = -1,   /**< invalid entry, or the row could not be stored */
    DSD_TG_POLICY_LEARN_UNCHANGED = 0, /**< the learned row already has this name and mode; nothing changed */
    DSD_TG_POLICY_LEARN_ADDED = 1,     /**< no exact row for the ID: the learned row was appended */
    DSD_TG_POLICY_LEARN_REPLACED = 2,  /**< the learned row's name or mode changed and was replaced */
    DSD_TG_POLICY_LEARN_KEPT = 3,      /**< an imported or user row holds the ID and wins; nothing changed */
} dsd_tg_policy_learn_result;

/**
 * @brief Learn an over-the-air alias (a DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS exact row) for an ID.
 *
 * Compares before it writes: the first exact row for the ID is kept when it was imported or entered by the user, and
 * a learned one is replaced only when its normalised name or mode (D vs DE) changed, so a repeated alias leaves the
 * table and its generation alone while a renamed radio's new alias replaces the old one.
 */
dsd_tg_policy_learn_result dsd_tg_policy_learn_exact(dsd_state* state, const dsd_tg_policy_entry* entry);
/**
 * @brief Append one exact row to opts->group_in_file in the file's own schema, creating the file when missing.
 *
 * A last line without a newline is ended first. @p metadata (free text, may be NULL) never lands in a column the
 * header models as tags: it goes one column further, past an empty tags field.
 *
 * @return 0 written or nothing to write (NULL opts/entry, empty path), 1 not an exact row, -1 on I/O failure
 *         (including a failed final close).
 */
int dsd_tg_policy_append_group_file_row(const dsd_opts* opts, const dsd_tg_policy_entry* entry, const char* metadata);

int dsd_tg_policy_should_preempt(const dsd_opts* opts, const dsd_state* state,
                                 const dsd_tg_policy_call_route* candidate_route,
                                 const dsd_tg_policy_decision* candidate, double now_mono_s);
int dsd_tg_policy_note_active_call(dsd_state* state, const dsd_tg_policy_call_route* route,
                                   const dsd_tg_policy_decision* decision, double now_mono_s);
int dsd_tg_policy_clear_active_call(dsd_state* state, int slot);
/**
 * @brief Record the call a protocol is holding, right before it asks dsd_tg_policy_should_preempt().
 *
 * @p start_mono_s is the time the protocol started following that call (a stable stamp, never "now" for a continuing
 * call), so dwell runs from the call rather than from now. A record that already matches @p route (target, channel,
 * slot, call class) keeps its cooldown and takes the fresh priority and start; a missing or different one is replaced.
 * Never stamps a preemption cooldown: describing what is held is not a takeover. Slot -1 describes both slots.
 *
 * @return 0 recorded, 1 invalid arguments or an incomplete route.
 */
int dsd_tg_policy_describe_followed_call(dsd_state* state, const dsd_tg_policy_call_route* route,
                                         const dsd_tg_policy_decision* decision, double start_mono_s,
                                         double now_mono_s);
/**
 * @brief Commit an accepted takeover's cooldown once the preempting call was acquired.
 *
 * A protocol's release clears the displaced records before the preempting call is noted, so the takeover is committed
 * explicitly: this stamps the global cooldown and the records now held.
 *
 * @return 0 committed, 1 invalid state.
 */
int dsd_tg_policy_commit_preemption(dsd_state* state, double now_mono_s);

int dsd_tg_policy_reload_group_file(const dsd_opts* opts, dsd_state* state);

/**
 * @brief Drop every loaded talkgroup entry, leaving an empty policy.
 *
 * The counterpart to dsd_tg_policy_reload_group_file() for a frontend that lets
 * a running session deselect its group file: re-importing cannot express "no
 * list", so without this the previous one keeps naming and blocking talkgroups.
 * Generations advance as they do on a reload, so readers holding one re-read.
 *
 * @return 0 when the policy is empty afterwards (including when none was
 *         loaded), -1 on a null state or allocation failure.
 */
int dsd_tg_policy_clear(dsd_state* state);

/** Temporary exact-ID blocks owned by the effective policy context, never CSV rows.
 * Survive retunes/scan visits; a list reload/replacement or context destruction resets them.
 * Match group targets and private endpoints just like exact mode-B user lockouts.
 * add: 0 applied/already present, 1 invalid (NULL state or zero ID), -1 allocation failure
 * with the previous policy intact. Mutations advance the published policy generation. */
int dsd_tg_policy_session_avoid_add(dsd_state* state, uint32_t id);
int dsd_tg_policy_session_avoid_contains(const dsd_state* state, uint32_t id);
/** Count distinct avoided IDs in inclusive bounds; 0..UINT32_MAX counts the whole scope. */
size_t dsd_tg_policy_session_avoid_count(const dsd_state* state, uint32_t start, uint32_t end);
/** Clear only temporary blocks in the current scope; NULL/empty is a no-op. */
void dsd_tg_policy_session_avoid_clear(dsd_state* state);

/** Maximum concurrent call skips and their monotonic lifetime limits, in seconds. */
#define DSD_TG_CALL_SKIP_MAX       8
#define DSD_TG_CALL_SKIP_QUIET_S   15.0
#define DSD_TG_CALL_SKIP_MAX_AGE_S 600.0

/**
 * @brief Arm or re-arm an exact-ID call skip without changing policy rows or generation.
 *
 * Fresh skips block group targets and private endpoints. Re-arming resets both clocks;
 * a full ledger evicts the entry with the oldest last-seen time. Fallback skips expire
 * from the press time and ignore touches. Retained policy contexts keep their skips.
 * Live mutations require the caller's decoder/watchdog synchronization guard.
 *
 * @return 0 applied, 1 invalid (NULL state or zero ID), -1 allocation failure.
 */
int dsd_tg_policy_call_skip_arm(dsd_state* state, uint32_t id, uint32_t src, int fallback, double now_mono_s);
/** Refresh a fresh non-fallback skip. Returns 1 refreshed, 0 otherwise; expired entries
 * are dropped and missing entries are never armed. Requires the same guard as arm. */
int dsd_tg_policy_call_skip_touch(dsd_state* state, uint32_t id, double now_mono_s);
/** Return whether an ID has a fresh skip, without mutating the store. */
int dsd_tg_policy_call_skip_active(const dsd_state* state, uint32_t id, double now_mono_s);
/** Return whether the ledger has no entries, without reading the clock or checking expiry. */
int dsd_tg_policy_call_skip_empty(const dsd_state* state);
/** Count fresh skips; safe on a frontend snapshot store, without changing its clocks. */
size_t dsd_tg_policy_call_skip_count(const dsd_state* state, double now_mono_s);
/** Clear call skips without changing rows or generation. NULL/empty is a no-op.
 * Live callers require the same guard as arm. */
void dsd_tg_policy_call_skip_clear(dsd_state* state);

/** Rows in table order (file order, then runtime appends). 0 without a policy context.
 * Works on live decoder state and frontend snapshot copies. */
size_t dsd_tg_policy_entry_count(const dsd_state* state);
/** Copy row @p index into @p out. Returns 1 when copied, 0 for NULL out or out of range (out zeroed). */
int dsd_tg_policy_entry_at(const dsd_state* state, size_t index, dsd_tg_policy_entry* out);
/** Live source context identity and table generation; both 0 without a context.
 * Every row mutation advances the generation. Equal pairs mean equal rows; snapshots report their source id. */
void dsd_tg_policy_table_version(const dsd_state* state, uint64_t* out_context_id, unsigned int* out_generation);
/**
 * @brief Whether an edit captured at (@p context, @p generation) still addresses the same rows.
 *
 * True when the context is the live one and nothing but learned over-the-air aliases and session avoids changed since
 * the capture (they move the generation for snapshots but never a row a frontend edits), or when nothing was loaded
 * at capture time (0, 0) and the live context was only created in place by such decoder writes. A reload, clear,
 * another store, or any edit of a talkgroup row makes it stale.
 */
int dsd_tg_policy_edit_version_valid(const dsd_state* state, uint64_t context, unsigned int generation);
/** Set the mode of the first talkgroup (non-alias) row with exactly these bounds, keeping its name, tags,
 * priority, preempt and source, and re-deriving audio/record/stream from mode. An exact id held only by a learned
 * alias takes that row over (empty name, source USER_LOCKOUT); a missing exact id is appended the same way; a
 * missing range is refused.
 * Creates the context when absent.
 * Returns 0 applied, 1 invalid (NULL state/mode, empty or over-long mode, reversed bounds,
 * missing range), -1 allocation failure. */
int dsd_tg_policy_set_mode(dsd_state* state, uint32_t id_start, uint32_t id_end, const char* mode);
/** Same edit on the row at @p index (decoder thread only; indices remain stable while it holds state).
 * Returns 0 applied, 1 bad index or mode. */
int dsd_tg_policy_set_mode_at(dsd_state* state, size_t index, const char* mode);

typedef struct {
    int32_t policy_index; /**< -1 for a heard-only exact ID; otherwise the captured table row. */
    uint32_t id_start;
    uint32_t id_end;
} dsd_tg_policy_selection;

/** Apply an exact captured selection atomically. A stale capture (dsd_tg_policy_edit_version_valid())
 * or invalid rows return 1; allocation failure returns -1, leaving policy intact. A heard-only ID held
 * only by a learned alias takes that row over. Ranges, priority and preemption are retained. */
int dsd_tg_policy_set_listening_selection(dsd_state* state, uint64_t context, unsigned int generation,
                                          const dsd_tg_policy_selection* selection, size_t count, int listening);

enum {
    DSD_TG_POLICY_FIELD_LISTEN = 1U << 0,
    DSD_TG_POLICY_FIELD_PRIORITY = 1U << 1,
    DSD_TG_POLICY_FIELD_PREEMPT = 1U << 2,
    DSD_TG_POLICY_FIELD_NAME = 1U << 3,
    DSD_TG_POLICY_FIELD_TAGS = 1U << 4,
    DSD_TG_POLICY_FIELD_RECORD = 1U << 5
};

/** Edit the first talkgroup (non-alias) row with these bounds, or append an exact/range row (default A).
 * Selected values come from entry.mode (A/B), priority (0..100), preempt (0/1), name, tags and record (0/1).
 * Unselected metadata is preserved; changing mode derives media flags. Record is applied after a mode change in the
 * same edit, and a row that blocks or plays no audio keeps record off whatever was asked (issue #708).
 * Learned alias rows (source RUNTIME_ALIAS or mode D) are never edited as such: an exact ID held only by one
 * takes it over as a fresh talkgroup row, as dsd_tg_policy_set_mode() does.
 * Returns 0 applied, 1 invalid fields/bounds, -1 allocation failure. */
int dsd_tg_policy_set_fields(dsd_state* state, uint32_t id_start, uint32_t id_end, const dsd_tg_policy_entry* values,
                             uint32_t mask);
/** Remove the first talkgroup (non-alias) row with these bounds. Returns 0 removed, 1 missing/invalid (an ID held
 * only by a learned alias has nothing to remove).
 * Remaining rows keep their order; a successful edit advances the table generation. */
int dsd_tg_policy_remove_bounds(dsd_state* state, uint32_t id_start, uint32_t id_end);
/** Atomically rewrite opts->group_in_file from the effective table using a sibling temporary file.
 * Keeps an extended policy header, or promotes for priority, preempt or media overrides. Otherwise uses
 * id,mode,name,tags when any row has tags, or id,mode,name. Ranges are written as start-end. Unmodelled free-text columns (e.g. alias metadata)
 * are not preserved. Returns 0 written or nothing to write (NULL opts, empty path), -1 on I/O
 * failure with the old file intact. */
int dsd_tg_policy_write_group_file(const dsd_opts* opts, const dsd_state* state);

/**
 * Decoder-thread owned, reference-counted policy context. References preserve row-local
 * aliases and lockouts across scan visits. Frontend snapshots still deep-copy their
 * effective context. Ownership protocol: every dsd_tg_policy_retain()/dsd_tg_policy_load()
 * result is one reference the caller must eventually dsd_tg_policy_release(); the state's
 * own extension slot holds its own reference, so installing a store never transfers the
 * caller's.
 * Stores observable by a running P25 watchdog may only be replaced or edited (every
 * replacement or edit: reload_group_file, clear, install, restore, the table editors,
 * the session-avoid and call-skip ledgers) with the P25 SM tick guard held by the
 * caller: release-time audio flushes read
 * policy under that guard. These functions are non-acquiring. Detached construction
 * (dsd_tg_policy_load, snapshot copies) and pre-start/post-stop lifecycle work are exceptions.
 */
typedef struct dsd_tg_policy_store dsd_tg_policy_store;
/** Take one reference on the state's effective context. NULL (no context) represents an
 * empty policy and needs no release. */
dsd_tg_policy_store* dsd_tg_policy_retain(const dsd_state* state);
/** Drop one reference; the context is freed with the last one. NULL is ignored. */
void dsd_tg_policy_release(dsd_tg_policy_store* store);
/**
 * Make @p store the state's effective context without allocating: the state takes its own
 * reference (the caller keeps and still owns theirs), the previously installed context is
 * released, and the installed store's active-call bookkeeping restarts. NULL installs an
 * empty policy. Reinstalling the current context only restarts the bookkeeping.
 */
void dsd_tg_policy_install(dsd_state* state, dsd_tg_policy_store* store);
/**
 * Restore a retained context after a temporary suspension, preserving active calls and
 * preemption cooldowns. Reference ownership is the same as dsd_tg_policy_install().
 * Use install instead for a target transition that must restart call bookkeeping.
 */
void dsd_tg_policy_restore(dsd_state* state, dsd_tg_policy_store* store);
/**
 * Load a standalone policy from a group file, with csvGroupImportPath() semantics (rows the
 * importer cannot store are skipped with a warning). On success `*out` is overwritten with a
 * new reference, so release any reference it held first. On failure `*out` is untouched.
 * Returns 0 on success, -1 otherwise.
 */
int dsd_tg_policy_load(const char* path, dsd_tg_policy_store** out);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_TALKGROUP_POLICY_H_H */
