// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The imports directory's reads and writes a frontend asks for: list its generated CSVs with their provenance
 * sidecars, write and replace an import's files, read and write a sidecar, delete an import (issue #672).
 *
 * The terminal's CSV picker, Imported Systems browser and RadioReference import wizard select and call these; they do
 * no file I/O themselves (the "menu callbacks call app-control services" rule, docs/code_map.md). Each runs on the
 * caller's thread and touches no decoder struct. A generated CSV's sidecar is "<csv path>.rr"
 * (dsd_rr_provenance_write()).
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_IMPORT_FILES_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_IMPORT_FILES_H_

#include <dsd-neo/runtime/radioreference_import.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Longest path, terminator included, a listed import can carry; a longer one is skipped, never shortened. */
#define DSD_APP_IMPORT_PATH_MAX 1024

/**
 * @brief One listed import: @p path is "<dir><sep><name>", @p name the bare file name, @p prov its parsed sidecar. All
 * three are good only for the call.
 * @return 0 to go on, non-zero to stop the walk.
 */
typedef int (*dsd_app_import_visit_fn)(const char* path, const char* name, const dsd_rr_provenance* prov, void* user);

/**
 * @brief Walk the directory @p dir and hand @p visit each ".csv" that has a readable provenance sidecar, in the order
 * the platform lists them. A file without one is skipped (its kind is unknown, and it cannot be refreshed), as is a
 * name whose path would not fit DSD_APP_IMPORT_PATH_MAX.
 * @return 0 when the directory was walked (to its end or until @p visit stopped it); 1 when it does not exist, the
 * first-run state before any import; -1 when it exists but could not be listed (entries already visited stand), or for
 * a NULL or empty @p dir or a NULL @p visit.
 */
int dsd_app_imports_list(const char* dir, dsd_app_import_visit_fn visit, void* user);

/**
 * @brief Create the imports directory (dsd_user_imports_dir()) and its parents with mode 0700.
 * @return 0 when it exists afterwards, -1 otherwise.
 */
int dsd_app_imports_dir_create(void);

/**
 * @brief Delete one stored import: the CSV @p csv_path and its sidecar. A sidecar path that would not fit is left
 * alone rather than shortened, since a shortened path names a different file.
 * @return 1 when the CSV was removed, 0 when it was not (also for a NULL or empty path).
 */
int dsd_app_import_remove(const char* csv_path);

/**
 * @brief Write @p len bytes of @p text to @p final_path through a private temp sibling, synced, then replacing any file
 * there. Binary: the generator's '\n' stays '\n' on Windows.
 * @return 0 when written; -1 when not, with the temp file removed and any file at @p final_path untouched.
 */
int dsd_app_import_write(const char* final_path, const char* text, size_t len);

/**
 * @brief Replace the stored import @p path with @p len bytes of @p text, but only when the new text validates as a
 * channel map (@p is_chan) or a group list with at least one accepted row: it is staged in a private temp sibling,
 * checked there, then swapped in.
 * @return 0 when replaced; -1 when the staging file could not be written; -2 when it was written but did not validate.
 * The stored file is untouched in both failure cases.
 */
int dsd_app_import_replace_checked(const char* path, const char* text, size_t len, int is_chan);

/**
 * @brief Read the sidecar of the generated CSV @p csv_path (dsd_rr_provenance_read()).
 * @return 0 on success, -1 when it is absent, unreadable or malformed.
 */
int dsd_app_import_provenance_read(const char* csv_path, dsd_rr_provenance* out);

/**
 * @brief Write the sidecar of the generated CSV @p csv_path atomically (dsd_rr_provenance_write()).
 * @return 0 on success, -1 on failure.
 */
int dsd_app_import_provenance_write(const char* csv_path, const dsd_rr_provenance* prov);

/**
 * @brief Whether the CSV path @p path is spoken for by something other than this import: a file is there and its
 * sidecar is missing (a hand-made file) or names another system (@p sid) or other sites (@p site_ids, the import
 * plan's site list). One system is stored once per site, so the same system with other sites is another stored import.
 * @return 0 when @p path is free (an orphan sidecar with no CSV does not count) or holds this same import, which a
 * re-import overwrites in place; 1 otherwise.
 */
int dsd_app_import_path_conflicts(const char* path, int sid, const char* site_ids);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_IMPORT_FILES_H_ */
