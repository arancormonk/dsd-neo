// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief One process-wide lock for dsd-neo's own writers of the CSV files a session uses.
 *
 * The decoder rewrites, exports and appends to the group list file, and the frontends replace an imported file in
 * place (Qt "Update from file" and RadioReference refresh, the terminal RadioReference refresh). Each of those is a
 * short sequence of file operations that must not interleave with another: the decoder checks that the group file
 * still holds the rows it loaded and then replaces it, and a frontend backs a file up, installs the new one and rolls
 * back on failure. Every such sequence runs under this lock. Hold it around the file operations only: stage temporary
 * files before taking it, and never wait for another thread while holding it.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_MANAGED_FILE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_MANAGED_FILE_H_

#ifdef __cplusplus
extern "C" {
#endif

/** Take the managed-file lock (not recursive). */
void dsd_managed_file_lock(void);
/** Release the managed-file lock. */
void dsd_managed_file_unlock(void);

/** A point inside a held lock where another writer's file operation would do harm. */
typedef enum {
    /** A group list rewrite found the file still holding its rows and is about to replace it. */
    DSD_MANAGED_FILE_STEP_CHECKED = 1,
    /** A frontend installed a replacement and has not yet committed it or rolled it back. */
    DSD_MANAGED_FILE_STEP_INSTALLED = 2,
} dsd_managed_file_step;

/** Mark @p step, with the lock held. Does nothing unless a test drives the interleavings at these points. */
void dsd_managed_file_step_reached(dsd_managed_file_step step);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_MANAGED_FILE_H_ */
