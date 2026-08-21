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
#define RR_LIBRARY_MAX      128

/** @brief Longest file stem this grouping key holds; longer files are skipped. */
#define RR_LIBRARY_STEM_MAX 256

/** @brief One stored import, folded from its generated file halves. */
typedef struct {
    /**
     * The grouping key: the shared leaf of the pair one import wrote, i.e. the
     * filename minus " group.csv" / " chan.csv".
     *
     * NOT the sid. One system is stored once per site - a statewide network
     * once per county - so several stored imports legitimately share a sid, and
     * grouping on it would fold separate counties into one row. The stem is
     * exactly what one import owns: both of its halves and nothing else.
     */
    char stem[RR_LIBRARY_STEM_MAX];
    int sid;               /**< RadioReference system id; what a refresh re-fetches. */
    char name[128];        /**< System name from the sidecar, for display. */
    char site_label[96];   /**< What this import covers; "" for a file imported before labels. */
    int has_group;         /**< 1 when group_path names a stored talkgroup list. */
    int has_chan;          /**< 1 when chan_path names a stored channel map. */
    char group_path[1024]; /**< Absolute path of the "<stem> group.csv", when has_group. */
    char chan_path[1024];  /**< Absolute path of the "<stem> chan.csv", when has_chan. */
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
 * A file whose stem already has a row is merged into it (its half and, when the
 * existing row has no recipe yet, its recipe); a new stem starts a new row. The
 * provenance's `kind` decides which half the path fills, and must be exactly
 * "group" or "chan" - anything else is not a half this browser can act on.
 *
 * Two rows may share a sid, and that is the point: several sites of one system,
 * and the stale pair RadioReference leaves behind when it renames a system.
 * Each is listed on its own, because each is a real stored import that can be
 * used, refreshed and deleted.
 *
 * @param lib      Library, previously rr_library_init()'d.
 * @param csv_path Absolute path of the generated CSV.
 * @param prov     Its parsed provenance.
 * @return 0 when folded, -1 when the library is full (overflow is set), when
 *         @p prov has no usable sid or kind, when the stem does not fit
 *         RR_LIBRARY_STEM_MAX, or when an argument is NULL.
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
 * @brief Order rows by display name, then site, then sid, for a stable list.
 *
 * Sites of one system land together and in a fixed order, so a statewide
 * network reads as a block of counties rather than as scattered duplicates.
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
 * @brief Name one stored import for a prompt or a status line.
 *
 * "<system> - <site>", or the system alone when the import predates the site
 * label. Several rows can carry the same system name, so a message that names
 * only the system - "Delete Iowa Statewide from disk?" - does not say which of
 * them it means.
 *
 * @param s      System, or NULL for an empty result.
 * @param out    Destination; always NUL-terminated when out_sz > 0.
 * @param out_sz Destination size in bytes, passed explicitly. A name that does
 *               not fit is truncated, never dropped.
 */
void rr_library_display_name(const RrLibrarySystem* s, char* out, size_t out_sz);

/** @brief Column widths one render of the browser shares. */
typedef struct {
    int name; /**< System-name column. */
    int site; /**< Site column; 0 when no stored import names a site. */
} RrLibraryLayout;

/**
 * @brief Measure the library against the width the browser has.
 *
 * Fixed widths cannot serve both an 80-column terminal and a 200-column one:
 * eighty is where the site column has to fight for space, and a wide terminal
 * should not pad two short columns with nothing. So the two variable columns
 * are sized from the longest text actually stored, clamped to a floor that
 * keeps them readable and a ceiling that stops one long name from pushing the
 * frequency off the line, and shared evenly when both cannot have what they
 * want. Lengths are counted in BYTES, matching the row formatter's own cut -
 * width-aware fitting for double-width glyphs is a bigger job than this row.
 *
 * @param lib        Library to measure; NULL yields the floors.
 * @param avail_cols Columns the row may occupy, gutter included.
 * @param out        Receives the widths. Never NULL.
 */
void rr_library_layout(const RrLibrary* lib, int avail_cols, RrLibraryLayout* out);

/**
 * @brief Format one stored import as an aligned browser row.
 *
 * A two-column gutter carrying "* " when @p in_use, then: system name, site
 * (omitted entirely when layout->site is 0), protocol short name (or "-" for a
 * system carrying no recipe), and the detail cell that closes the line (start
 * frequency in MHz, " scan" appended for a conventional scan list, or "-").
 * Both variable columns are truncated with ".." on a UTF-8 boundary.
 *
 * The in-use flag leads rather than trails: it costs two columns instead of
 * nine, and a mark in a column is found at a glance where a suffix on lines of
 * differing length is not.
 *
 * @param s      System.
 * @param layout Widths from rr_library_layout(). Never NULL.
 * @param in_use Non-zero to mark the row as loaded in this session.
 * @param out    Destination; always NUL-terminated when out_sz > 0.
 * @param out_sz Destination size.
 * @return The length written, or 0 on a NULL/empty-buffer argument.
 */
int rr_library_row_format(const RrLibrarySystem* s, const RrLibraryLayout* layout, int in_use, char* out,
                          size_t out_sz);

#endif /* DSD_NEO_SRC_UI_TERMINAL_RR_LIBRARY_H_ */
