// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Curses-free model of the imports directory as a list of systems.
 *
 * The terminal "Imported Systems" browser shows one row per RadioReference
 * system, not one per file: the group list and the channel map an import wrote
 * share a system id, and are folded back together here. Everything in this
 * header is pure or filesystem-only - no curses, no app_control - so it links
 * against the runtime alone and is tested directly on a scratch directory.
 *
 * This header is internal to src/ui/terminal/ and should NOT be installed.
 */
#ifndef DSD_NEO_SRC_UI_TERMINAL_RR_LIBRARY_H_
#define DSD_NEO_SRC_UI_TERMINAL_RR_LIBRARY_H_

#include <dsd-neo/runtime/radioreference_import.h>
#include <stddef.h>

/** @brief Ceiling on systems held; a scan that meets it sets RrLibrary::overflow. */
#define RR_LIBRARY_MAX 128

/** @brief One RadioReference system, folded from its generated file halves. */
typedef struct {
    int sid;               /**< RadioReference system id; the grouping key. */
    char name[128];        /**< System name from the sidecar, for display. */
    int has_group;         /**< 1 when group_path names a stored talkgroup list. */
    int has_chan;          /**< 1 when chan_path names a stored channel map. */
    char group_path[1024]; /**< Absolute path of the "<system> group.csv", when has_group. */
    char chan_path[1024];  /**< Absolute path of the "<system> chan.csv", when has_chan. */
    long long group_at;    /**< imported_at of the stored group half; the newest-wins key. */
    long long chan_at;     /**< imported_at of the stored chan half; the newest-wins key. */
    int partial_enc_as_de; /**< The partial-encryption answer both halves were built with. */
    dsd_rr_recipe recipe;  /**< How to re-apply; recipe.present == 0 for a files-only system. */
} RrLibrarySystem;

/** @brief The imports directory as a list of systems. */
typedef struct {
    RrLibrarySystem systems[RR_LIBRARY_MAX];
    int count;
    int overflow; /**< 1 when more than RR_LIBRARY_MAX systems were present. */
} RrLibrary;

/** @brief Reset a library to empty. */
void rr_library_init(RrLibrary* lib);

/**
 * @brief Fold one generated CSV, with its provenance, into the library.
 *
 * A file whose sid already has a system is merged into it (its half and, when
 * the existing system has no recipe yet, its recipe); a new sid starts a new
 * system. The provenance's `kind` decides which half the path fills, and must be
 * exactly "group" or "chan" - anything else is not a half this browser can act
 * on. When two files on disk claim the same sid and half - which happens when
 * RadioReference renames a system, because the file stem comes from the name and
 * the old pair stays behind - the newer `imported_at` wins, so the row does not
 * depend on directory order.
 *
 * @param lib      Library, previously rr_library_init()'d.
 * @param csv_path Absolute path of the generated CSV.
 * @param prov     Its parsed provenance.
 * @return 0 when folded or deliberately skipped as older, -1 when the library is
 *         full (overflow is set), when @p prov has no usable sid or kind, or
 *         when an argument is NULL.
 */
int rr_library_add(RrLibrary* lib, const char* csv_path, const dsd_rr_provenance* prov);

/**
 * @brief Scan an imports directory into a library.
 *
 * Walks @p dir, reads each ".csv" that has a readable sidecar, and folds it in.
 * Files without a sidecar are skipped, as they cannot be refreshed or re-applied.
 * Systems are left in first-seen order; the caller sorts if it wants a stable one.
 *
 * A directory that does not exist yet is the first-run state, not an error: it
 * reports 0 systems, the same as an empty one.
 *
 * @param lib Library to fill; init'd internally.
 * @param dir Directory to walk.
 * @return The number of systems found, or -1 when @p dir exists but cannot be
 *         listed.
 */
int rr_library_scan(RrLibrary* lib, const char* dir);

/**
 * @brief Order systems by display name, then sid, for a stable list.
 *
 * @param lib Library to sort in place.
 */
void rr_library_sort(RrLibrary* lib);

/**
 * @brief Whether a running session is decoding one of this system's files.
 *
 * @param s            System.
 * @param chan_in_use  opts->chan_in_file from the session, or NULL.
 * @param group_in_use opts->group_in_file from the session, or NULL.
 * @return 1 when a stored path equals the matching in-use path, 0 otherwise.
 */
int rr_library_system_in_use(const RrLibrarySystem* s, const char* chan_in_use, const char* group_in_use);

/**
 * @brief Format one system as an aligned browser row.
 *
 * Columns: name (fixed width, truncated with ".." when longer), protocol short
 * name (or "-" when the system carries no recipe), a detail cell (start
 * frequency in MHz, with " scan" appended for a conventional scan list, or "-"),
 * and an " * in use" marker when @p in_use.
 *
 * @param s      System.
 * @param in_use Non-zero to append the in-use marker.
 * @param out    Destination; always NUL-terminated when out_sz > 0.
 * @param out_sz Destination size.
 * @return The length written, or 0 on a NULL/empty-buffer argument.
 */
int rr_library_row_format(const RrLibrarySystem* s, int in_use, char* out, size_t out_sz);

#endif /* DSD_NEO_SRC_UI_TERMINAL_RR_LIBRARY_H_ */
