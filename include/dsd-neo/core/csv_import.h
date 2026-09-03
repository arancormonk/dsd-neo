// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief CSV import helpers for runtime/UI one-shot actions.
 *
 * Declares CSV import entrypoints implemented in core.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_CSV_IMPORT_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_CSV_IMPORT_H_H

#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

struct p25_bandplan_row;

#ifdef __cplusplus
extern "C" {
#endif

int csvGroupImport(const dsd_opts* opts, dsd_state* state);
int csvGroupImportPath(const char* group_file_path, dsd_state* state);
int csvChanImport(const dsd_opts* opts, dsd_state* state);
int csvKeyImportDec(const dsd_opts* opts, dsd_state* state);
int csvKeyImportHex(const dsd_opts* opts, dsd_state* state);
int csvKeyImportDecPath(const char* path, int show_keys, dsd_state* state, dsd_csv_validation* stats);
int csvKeyImportHexPath(const char* path, int show_keys, dsd_state* state, dsd_csv_validation* stats);
int csvVertexKsImport(dsd_state* state, const char* path);
int csvDmrTgKeyImport(dsd_state* state, const char* path);

/**
 * Load a P25 band plan CSV (docs/csv-formats.md, "P25 Band Plan CSV") into
 * state->p25_bandplan_rows, replacing any stored plan, then seed the live IDEN
 * tables (dsd_state_p25_bandplan_seed). Bad rows are skipped with a warning; a
 * file with no usable row fails and leaves the stored plan untouched.
 * Returns 0 on success, -1 otherwise.
 */
int csvP25BandplanImportPath(const char* path, dsd_state* state);
/** csvP25BandplanImportPath() on opts->p25_bandplan_in_file. */
int csvP25BandplanImport(const dsd_opts* opts, dsd_state* state);
/**
 * Write band-plan rows (see dsd_p25_bandplan_append_tables) to `path` in the
 * import format, through a private temp file and atomic replace. Refuses an
 * empty list. Returns 0 on success, -1 otherwise.
 */
int csvP25BandplanExportRows(const char* path, const struct p25_bandplan_row* rows, int count);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_CSV_IMPORT_H_H */
