// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/** @file Signal-search command submission and native background history. */
#ifndef DSD_NEO_APP_CONTROL_SIGNAL_SEARCH_H
#define DSD_NEO_APP_CONTROL_SIGNAL_SEARCH_H
#include <dsd-neo/runtime/signal_search.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int dsd_app_signal_search_start(const dsd_signal_search_config* config);
int dsd_app_signal_search_control(const dsd_signal_search_control* control);
/** Open once per app host, before starting decoder sessions. Parent directory must exist.
 * The worker writes an append journal, compacts atomically, and keeps memory on failures. A failed history or avoid
 * write stays reported and is retried by later writes, flush and close until that file's own write succeeds.
 * @p path names the version 1 journal, which is only ever read: the store keeps version 2 records (site, slot and
 * encryption included) in its own journal beside it (a trailing ".v1" becomes ".v2", else ".v2" is appended), and
 * the avoid list in "<path>.avoids.json". While no version 2 journal exists, the version 1 rows load with those
 * details empty and the first write creates the version 2 journal from them; once it exists it alone is read.
 * A version 2 journal or avoid list that did not load whole (it could not be opened or read, or is damaged, torn or
 * invalid) is never replaced: before its first replacement it is renamed, without replacing anything, to
 * "<name>.unreadable-N" (N = 1..99). A missing version 2 journal beside a kept one keeps the version 1 rows out, and a
 * version 1 journal that cannot be read is reported and left in place. Opening also removes this store's own
 * temporary files a crash left behind. dsd_app_signal_search_store_file_state() reports each file. */
int dsd_app_signal_search_store_open(const char* path);
/** Stop the worker after one last attempt at any write owed. The store then holds no permanent avoids, and neither does
 * the runtime's permanent list, until the next open loads the file again. */
void dsd_app_signal_search_store_close(void);
void dsd_app_signal_search_store_flush(void);
/** @p error receives the store's write failure ("…could not be saved; retained in memory") while a history or avoid
 * write is owed, else "". How each file loaded is dsd_app_signal_search_store_file_state()'s, for the frontend to
 * word. */
void dsd_app_signal_search_status_get(dsd_signal_search_status* status, char* error, size_t capacity);

/** The files dsd_app_signal_search_store_file_state() reports on. */
typedef enum {
    DSD_APP_SEARCH_STORE_HISTORY = 0, /**< the version 2 journal */
    DSD_APP_SEARCH_STORE_AVOIDS = 1,  /**< the permanent avoid list */
    DSD_APP_SEARCH_STORE_V1 = 2       /**< the version 1 journal, only ever read */
} dsd_app_search_store_file;

/** How the open found a store file, and whether one it could not read has been kept aside since. */
typedef enum {
    /** Read whole. */
    DSD_APP_SEARCH_FILE_OK,
    /** Not there (or, for the version 1 journal, not read because a version 2 journal or a kept one exists); also a
     * file that could not be read and was gone by its first replacement. */
    DSD_APP_SEARCH_FILE_MISSING,
    /** Could not be opened or read, or damaged, torn or invalid: what loaded before the failure is shown, and the file
     * stays as it is until it can be kept aside (the version 1 journal never is). Also a missing version 2 journal
     * whose kept names could not all be checked. */
    DSD_APP_SEARCH_FILE_UNREADABLE,
    /** Kept aside under the reported name, either by this run's first replacement or, for a missing version 2
     * journal, by an earlier run (the newest kept name is reported). */
    DSD_APP_SEARCH_FILE_KEPT
} dsd_app_search_file_load;

/** One store file's state (@p which a dsd_app_search_store_file) under the store's lock: its load state, whether its
 * write is owed after a failure (always 0 for the version 1 journal) and the base name (no folder) it was kept aside as,
 * "" unless DSD_APP_SEARCH_FILE_KEPT. Any output may be NULL. @return 0, or -1 for an unknown @p which. */
int dsd_app_signal_search_store_file_state(int which, dsd_app_search_file_load* state, int* write_failed,
                                           char* kept_name, size_t cap);
/** Newest first; consistent copied records, retained even without any frontend screen. */
size_t dsd_app_signal_search_history_get(dsd_signal_search_hit* hits, size_t capacity);
/** Bumped by every history change (upsert, trim, reload); status refreshes, and a republished detection whose
 * persisted content is unchanged, leave it alone. */
uint64_t dsd_app_signal_search_history_revision(void);
/** dsd_app_signal_search_history_get() and the revision of that copy, taken together. */
size_t dsd_app_signal_search_history_snapshot(dsd_signal_search_hit* hits, size_t capacity, uint64_t* revision);
/** Permanent avoids, as loaded (a repeated frequency once) and changed since. The store is the only writer of the
 * runtime's permanent list (<dsd-neo/runtime/signal_search_avoids.h>) and sets it at the load and at every add and
 * remove, so a running search takes each change at its next tick (issue #828). */
size_t dsd_app_signal_search_avoids_get(uint32_t* frequencies, size_t capacity);
/** Remove a permanent avoid; -1 when it is not one. Thread-safe; the store saves it in the background. */
int dsd_app_signal_search_avoid_remove(uint32_t frequency);
/** Moves whenever what dsd_app_signal_search_avoids_get() or dsd_app_signal_search_session_avoids_get() return can
 * change: the permanent list, the runtime's session list, or whether the store's search shows one. */
uint64_t dsd_app_signal_search_avoids_revision(void);
/** The running search's own avoids: the engine's session list in the runtime, returned only while it belongs to the
 * session the store's status names and that search has neither stopped nor failed, so a frontend never shows another
 * session's list or one that no longer applies. Never holds a permanent avoid. */
size_t dsd_app_signal_search_session_avoids_get(uint32_t* frequencies, size_t capacity);
/** Decoder session teardown: no status may outlive it as a running or owned search. */
void dsd_app_signal_search_session_ended(void);
/** Decoder-thread publication; no disk access. */
void dsd_app_signal_search_publish(const dsd_signal_search_status*, const dsd_signal_search_hit*, void*);

/** dsd_app_signal_search_permanent_avoid() results. */
typedef enum {
    DSD_APP_SEARCH_AVOID_ADDED = 0,
    /** Already avoided: the list is as asked, so this is no refusal. */
    DSD_APP_SEARCH_AVOID_DUPLICATE = 1,
    /** DSD_SEARCH_MAX_AVOIDS reached; nothing changed. */
    DSD_APP_SEARCH_AVOID_FULL = -1,
    /** Frequency 0 names no channel; nothing changed. */
    DSD_APP_SEARCH_AVOID_INVALID = -2
} dsd_app_search_avoid_result;

/** Add a permanent avoid. Thread-safe; the store saves it in the background. A frontend calls it directly for a
 * history row; DSD_SEARCH_ACTION_AVOID_PERMANENT runs it on the decoder thread for the hit being visited, after
 * validating the hit and before the coordinator leaves it. */
int dsd_app_signal_search_permanent_avoid(uint32_t frequency);
#ifdef __cplusplus
}
#endif
#endif
