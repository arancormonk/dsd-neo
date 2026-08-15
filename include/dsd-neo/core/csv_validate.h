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
 * accepted may legitimately be 0), or -1 when the file could not be opened.
 */
int dsd_csv_validate_group_file(const char* path, dsd_csv_validation* out);
int dsd_csv_validate_chan_file(const char* path, dsd_csv_validation* out);
int dsd_csv_validate_key_file_dec(const char* path, dsd_csv_validation* out);
int dsd_csv_validate_key_file_hex(const char* path, dsd_csv_validation* out);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_CSV_VALIDATE_H_H */
