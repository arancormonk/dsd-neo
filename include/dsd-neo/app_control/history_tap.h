// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief A second reader of the published event history: the app's background History tap (issue #708).
 *
 * The snapshot accessors in <dsd-neo/app_control/snapshot.h> are single-consumer and belong to the frontend's polling
 * thread. A thread that must keep reading committed History rows while that thread is paused (the Qt loop stops on
 * Android in the background) copies them here instead: into its own buffer, under the snapshot lock, without touching
 * the consumer's buffers or its change tracking, so the frontend's next read sees every change as before.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_HISTORY_TAP_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_HISTORY_TAP_H_

/* A frontend header names the history ring only through state_fwd.h (the arch rules keep state.h out); IWYU attributes
   the typedef to state.h in a unit that included that first. */
#include <dsd-neo/core/state_fwd.h> // IWYU pragma: keep
// IWYU pragma: no_include "dsd-neo/core/state.h"
// IWYU pragma: no_include <dsd-neo/core/state.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Where a tap last read one slot's ring. Zero-initialize it: a cursor that has read nothing copies on its first
 * read whatever the ring holds.
 */
typedef struct {
    uint64_t instance;   /**< Event_History_I::instance of the ring last copied */
    uint64_t commit_rev; /**< its Event_History_I::commit_rev then */
    int read;            /**< nonzero once a copy was made */
} dsd_app_history_tap_cursor;

/**
 * @brief What History attribution reads from the latest options snapshot, as the frontend's refresh() does: whether
 * options were published at all, since unknown options cannot establish a saved system's identity. Which rows carry it
 * is each row's own stamp (Event_History::operator_tuned, scan_member; issue #671), never the scan flags at the read.
 */
typedef struct {
    int opts_published; /**< 0 until the first options snapshot */
} dsd_app_history_tap_meta;

/**
 * @brief Copy one slot's published history ring into @p out when its committed rows changed since @p cursor.
 *
 * The ring is copied when its (instance, commit_rev) differs from the cursor's, or the cursor has read nothing yet;
 * a staged-row render alone (revision without commit_rev) copies nothing, as for the frontend. A copy moves the cursor
 * to the ring it copied. @p out is written only by a copy. The copy is one memcpy under the snapshot lock, with no
 * allocation and no I/O; the options are read under the options snapshot's lock, taken first and on its own. Neither
 * touches the single-consumer accessors' state. Any thread, any number of tap threads, each with its own @p out and
 * @p cursor.
 *
 * After the decoder stops, the last published rings stay, so a read after a stop still finds the final commits.
 *
 * @param slot History slot, 0 or 1.
 * @param out Receives the ring on a copy.
 * @param cursor In: where this tap last read; out: the ring copied (unchanged when nothing was copied).
 * @param meta Receives the published options' attribution fields on every 0 or 1 return; may be NULL.
 * @return 1 when the ring was copied; 0 when nothing new was published (no ring yet, or the cursor is current); -1 for
 *         a NULL @p out or @p cursor, or a slot other than 0 or 1.
 */
int dsd_app_history_tap_copy(int slot, Event_History_I* out, dsd_app_history_tap_cursor* cursor,
                             dsd_app_history_tap_meta* meta);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_HISTORY_TAP_H_ */
