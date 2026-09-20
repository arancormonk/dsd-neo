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
 * @brief Evaluate a private grant while allowing entirely unlisted endpoints.
 *
 * Explicit source/destination policy entries and all non-allow-list gates still
 * apply. This is used by grant paths where group IDs and radio IDs are separate
 * namespaces, so an absent radio-ID entry is not itself a denial.
 */
int dsd_tg_policy_evaluate_private_grant(const dsd_opts* opts, const dsd_state* state, uint32_t src, uint32_t dst,
                                         int encrypted, int data_call, dsd_tg_policy_decision* out);
int dsd_tg_policy_append_exact(dsd_state* state, const dsd_tg_policy_entry* entry);
int dsd_tg_policy_upsert_exact(dsd_state* state, const dsd_tg_policy_entry* entry, dsd_tg_policy_upsert_mode mode);
int dsd_tg_policy_append_group_file_row(const dsd_opts* opts, const dsd_tg_policy_entry* entry, const char* metadata);

int dsd_tg_policy_should_preempt(const dsd_opts* opts, const dsd_state* state,
                                 const dsd_tg_policy_call_route* candidate_route,
                                 const dsd_tg_policy_decision* candidate, double now_mono_s);
int dsd_tg_policy_note_active_call(dsd_state* state, const dsd_tg_policy_call_route* route,
                                   const dsd_tg_policy_decision* decision, double now_mono_s);
int dsd_tg_policy_clear_active_call(dsd_state* state, int slot);

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

/** Rows in table order (file order, then runtime appends). 0 without a policy context.
 * Works on live decoder state and frontend snapshot copies. */
size_t dsd_tg_policy_entry_count(const dsd_state* state);
/** Copy row @p index into @p out. Returns 1 when copied, 0 for NULL out or out of range (out zeroed). */
int dsd_tg_policy_entry_at(const dsd_state* state, size_t index, dsd_tg_policy_entry* out);
/** Live source context identity and table generation; both 0 without a context.
 * Every row mutation advances the generation. Equal pairs mean equal rows; snapshots report their source id. */
void dsd_tg_policy_table_version(const dsd_state* state, uint64_t* out_context_id, unsigned int* out_generation);
/** Set the mode of the first row with exactly these bounds, keeping its name, tags, priority,
 * preempt and source, and re-deriving audio/record/stream from mode. A missing exact id is
 * appended with an empty name and source USER_LOCKOUT; a missing range is refused.
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

/** Apply an exact captured selection atomically. Context/generation mismatches
 * or invalid rows return 1; allocation failure returns -1, leaving policy intact.
 * Existing aliases, ranges, priority and preemption are retained. */
int dsd_tg_policy_set_listening_selection(dsd_state* state, uint64_t context, unsigned int generation,
                                          const dsd_tg_policy_selection* selection, size_t count, int listening);

enum {
    DSD_TG_POLICY_FIELD_LISTEN = 1U << 0,
    DSD_TG_POLICY_FIELD_PRIORITY = 1U << 1,
    DSD_TG_POLICY_FIELD_PREEMPT = 1U << 2,
    DSD_TG_POLICY_FIELD_NAME = 1U << 3,
    DSD_TG_POLICY_FIELD_TAGS = 1U << 4
};

/** Edit the first row with these bounds, or append an exact/range row (default A).
 * Selected values come from entry.mode (A/B), priority (0..100), preempt (0/1), name and tags.
 * Unselected metadata is preserved; changing mode derives media flags.
 * Alias/source RUNTIME_ALIAS and mode-D rows are immutable.
 * Returns 0 applied, 1 invalid fields/bounds/alias, -1 allocation failure. */
int dsd_tg_policy_set_fields(dsd_state* state, uint32_t id_start, uint32_t id_end, const dsd_tg_policy_entry* values,
                             uint32_t mask);
/** Remove the first row with these bounds. Returns 0 removed, 1 missing/invalid/alias.
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
