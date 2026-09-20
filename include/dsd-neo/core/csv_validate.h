// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Dry-run CSV validation with row counts for UI feedback.
 *
 * Parses a CSV through the same loops as the real importers but into
 * throwaway state, so a frontend can report "N rows loaded, M skipped"
 * before wiring the file into a live session. Engine state is never touched.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_CSV_VALIDATE_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_CSV_VALIDATE_H_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsd_csv_validation {
    unsigned int accepted; /* data rows that would load */
    unsigned int skipped;  /* malformed or out-of-range data rows */
    unsigned int total;    /* data rows seen (header line excluded) */
} dsd_csv_validation;

/*
 * Each returns 0 when the file opened and was parsed (counts are valid, and
 * accepted may legitimately be 0), or -1 on open, read or allocation failure. A non-NULL out is zeroed on failure.
 */
/* Returns -1 on open, read or allocation failure; header and blank lines are excluded. */
int dsd_csv_validate_src_file(const char* path, dsd_csv_validation* out);
int dsd_csv_validate_group_file(const char* path, dsd_csv_validation* out);
int dsd_csv_validate_chan_file(const char* path, dsd_csv_validation* out);

/** Nonsecret configured row scope. Inherited fields use -1; key_source is
 * 0 inherit, 1 direct, 2 collection, 3 explicitly empty. No key bytes or paths. */
typedef struct {
    size_t index;
    uint64_t frequency_hz;
    char name[128];
    char mode[16];
    char profile_ref[64];
    int key_source;
    int force;
    int dmr_mapping_count;
} dsd_csv_channel_profile;

typedef void (*dsd_csv_channel_profile_cb)(const dsd_csv_channel_profile* row, void* context);
/** Validate and inspect configured channel rows using temporary owned state.
 * Callbacks run synchronously only after parsing succeeds; row expires on return. */
int dsd_csv_inspect_channel_profiles(const char* path, void* context, dsd_csv_channel_profile_cb callback);
int dsd_csv_validate_key_file_dec(const char* path, dsd_csv_validation* out);
int dsd_csv_validate_key_file_hex(const char* path, dsd_csv_validation* out);
/** Strict, atomic Vertex key to keystream validation. */
int dsd_csv_validate_vertex_file(const char* path, dsd_csv_validation* out);
int dsd_csv_validate_p25_bandplan_file(const char* path, dsd_csv_validation* out);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_CSV_VALIDATE_H_H */
