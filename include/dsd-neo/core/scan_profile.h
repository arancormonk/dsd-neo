// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/** @file @brief Materialized row options; companion files are read only at import. */
#ifndef DSD_NEO_CORE_SCAN_PROFILE_H
#define DSD_NEO_CORE_SCAN_PROFILE_H
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif

/** One row's materialized options. `groups` is an owned policy reference (one retain), or NULL
 * when the row names no group file. */
typedef struct {
    dsd_scan_option_values values;
    dsd_tg_policy_store* groups;
} dsd_scan_row_profile;

/** Materialize the direct key values (`-b`, `-H`, `-1`, `-R`, or the merged legacy columns) into
 * @p out, releasing whatever @p out held. `present` is set only when a direct source exists.
 * Returns -1 on a bad argument with @p out untouched. */
int dsd_scan_options_keys(const dsd_scan_options* options, dsd_key_set* out);
/** Merge legacy key columns into parsed options. Columns alone do not claim a mute override;
 * when option text contains `-b`/`-H`, all merged BP/Hytera material decides DMR muting. Rejects a
 * column that duplicates an option or mixes direct keys with key files. No files are opened;
 * on failure @p options is untouched. */
int dsd_scan_options_merge_keys(dsd_scan_options* options, const char* hex_file, const char* dec_file,
                                const char* single_hex, const char* single_dec, char* error, size_t error_size);
/** Resolve the key and group paths relative to the list file @p base, each within its own field
 * capacity. On failure @p options is untouched and the caller discards the import object. */
int dsd_scan_options_resolve(dsd_scan_options* options, const char* base, char* error, size_t error_size);
/**
 * Build one profile and key set, reading the group and key files named in @p options.
 * On success `*profile` is overwritten with a new heap profile (pass NULL or a pointer whose
 * previous value the caller has already freed or still owns elsewhere; it is not released
 * here) and `*keys` is released and replaced. On failure both outputs are untouched.
 * Returns 0 on success, -1 otherwise.
 */
int dsd_scan_profile_load(const dsd_scan_options* options, int show_keys, dsd_scan_row_profile** profile,
                          dsd_key_set* keys);

/** Release a profile and its group reference. NULL, and a profile with NULL groups, are fine. */
static inline void
dsd_scan_profile_free(dsd_scan_row_profile* profile) {
    if (!profile) {
        return;
    }
    dsd_tg_policy_release(profile->groups);
    free(profile);
}

/** Conventional profile by positional scan slot; borrowed until list replacement. NULL when
 * the row carries no options. */
const dsd_scan_row_profile* dsd_channel_profile_get(const dsd_state* state, size_t row);
/**
 * Store a row profile, taking ownership of @p profile on success (the slot's previous
 * profile is freed; NULL clears the slot). Returns -1 on allocation failure, in which case
 * the caller still owns @p profile and the slot is unchanged. A stored profile with any
 * option present makes dsd_channel_modes_present() true, so the typed scanner runs it.
 */
int dsd_channel_profile_set(dsd_state* state, size_t row, dsd_scan_row_profile* profile);
/** Reserve group scope before a tune. Returns -1 on allocation failure, 0 otherwise. */
int dsd_scan_groups_begin(dsd_state* state);
/** Install the row's preloaded policy (the global policy is retained as the baseline on the first
 * entry), or restore the baseline for a profile without groups. No allocation; the scope must
 * have been reserved with dsd_scan_groups_begin(). */
void dsd_scan_groups_enter(dsd_state* state, const dsd_scan_row_profile* profile);
/** Restore the baseline policy and drop the scope. Safe when no scope is active. */
void dsd_scan_groups_leave(dsd_state* state);
/** Park the row policy so a group import edits the global baseline beneath it. Returns 1 when
 * parked (resume is then owed), 0 when no row policy is active. */
int dsd_scan_groups_suspend(dsd_state* state);
/** Adopt the edited global baseline and restore the parked policy with active calls intact. */
void dsd_scan_groups_resume(dsd_state* state);
#ifdef __cplusplus
}
#endif
#endif
