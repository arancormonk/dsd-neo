// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Signal search avoid lists shared by the search and the screens (issue #828).
 *
 * Two lists of at most DSD_SEARCH_MAX_AVOIDS frequencies each, read as they stand by every side, so nothing has to be
 * sent, ordered or reconciled between them. The permanent list is written only by the app-control store (load, add,
 * remove, a restore applied); the running search takes it at its next tick or action. The session list is written
 * only by the engine coordinator: the running session's own avoids, never one that is permanent, cleared when the
 * session stops or fails. Every call is safe from any thread; nothing calls out while the lists' lock is held. Both
 * lists start empty, so the CLI, headless runs and tests are unaffected.
 */
#ifndef DSD_NEO_RUNTIME_SIGNAL_SEARCH_AVOIDS_H
#define DSD_NEO_RUNTIME_SIGNAL_SEARCH_AVOIDS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Replace the permanent list with @p list: 0 and repeated frequencies are dropped, the first
 * DSD_SEARCH_MAX_AVOIDS that remain are kept in order. The permanent revision moves only when the list changes. */
void dsd_signal_search_avoids_permanent_set(const uint32_t* list, size_t n);
/** Copy up to @p cap permanent frequencies to @p out and the revision they belong to to @p revision, under one lock;
 * returns the number copied. */
size_t dsd_signal_search_avoids_permanent_snapshot(uint32_t* out, size_t cap, uint64_t* revision);
/** The permanent list's revision: one atomic load, 0 until the list first changes. */
uint64_t dsd_signal_search_avoids_permanent_revision(void);

/** Replace the session list with @p session_id's @p list, normalised as the permanent list is. Session 0 clears it
 * whatever @p list holds. The session revision moves only when the session or its list changes. */
void dsd_signal_search_avoids_session_set(uint64_t session_id, const uint32_t* list, size_t n);
/** Copy up to @p cap session frequencies to @p out and their session to @p session_id (0: no session), under one
 * lock; returns the number copied. */
size_t dsd_signal_search_avoids_session_get(uint64_t* session_id, uint32_t* out, size_t cap);
/** The session list's revision: one atomic load, 0 until the list first changes. */
uint64_t dsd_signal_search_avoids_session_revision(void);

#ifdef __cplusplus
}
#endif
#endif
