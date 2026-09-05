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

typedef struct {
    dsd_scan_option_values values;
    dsd_tg_policy_store* groups;
} dsd_scan_row_profile;

/** Parsed key values replace the entire key set; out remains unchanged on failure. */
int dsd_scan_options_keys(const dsd_scan_options* options, dsd_key_set* out);
/** Merge legacy key columns into parsed options without changing their legacy mode semantics.
 * Reject duplicate definitions/source-family conflicts. No files are opened. */
int dsd_scan_options_merge_keys(dsd_scan_options* options, const char* hex_file, const char* dec_file,
                                const char* single_hex, const char* single_dec, char* error, size_t error_size);
/** Resolve paths relative to the list. On failure the caller discards the import object. */
int dsd_scan_options_resolve(dsd_scan_options* options, const char* base, char* error, size_t error_size);
/** Build one profile and key set. Failure leaves outputs untouched. */
int dsd_scan_profile_load(const dsd_scan_options* options, int show_keys, dsd_scan_row_profile** profile,
                          dsd_key_set* keys);

static inline void
dsd_scan_profile_free(dsd_scan_row_profile* profile) {
    if (!profile) {
        return;
    }
    dsd_tg_policy_release(profile->groups);
    free(profile);
}

/** Conventional profile by positional scan slot; borrowed until list replacement. */
const dsd_scan_row_profile* dsd_channel_profile_get(const dsd_state* state, size_t row);
/** Transfer profile ownership on success. */
int dsd_channel_profile_set(dsd_state* state, size_t row, dsd_scan_row_profile* profile);
/** Reserve group scope before a tune. */
int dsd_scan_groups_begin(dsd_state* state);
/** Apply/restore the preloaded policy without allocating. Scope must have been reserved. */
void dsd_scan_groups_enter(dsd_state* state, const dsd_scan_row_profile* profile);
void dsd_scan_groups_leave(dsd_state* state);
/** Group imports update the global policy beneath a parked row. */
int dsd_scan_groups_suspend(dsd_state* state);
void dsd_scan_groups_resume(dsd_state* state);
#ifdef __cplusplus
}
#endif
#endif
