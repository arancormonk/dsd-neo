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
 * write stays reported and is retried by later writes, flush and close until that file's own write succeeds. */
int dsd_app_signal_search_store_open(const char* path);
void dsd_app_signal_search_store_close(void);
void dsd_app_signal_search_store_flush(void);
void dsd_app_signal_search_status_get(dsd_signal_search_status* status, char* error, size_t capacity);
/** Newest first; consistent copied records, retained even without any frontend screen. */
size_t dsd_app_signal_search_history_get(dsd_signal_search_hit* hits, size_t capacity);
/** Bumped by every history change (upsert, trim, reload); status refreshes leave it alone. */
uint64_t dsd_app_signal_search_history_revision(void);
/** dsd_app_signal_search_history_get() and the revision of that copy, taken together. */
size_t dsd_app_signal_search_history_snapshot(dsd_signal_search_hit* hits, size_t capacity, uint64_t* revision);
/** Permanent avoids. */
size_t dsd_app_signal_search_avoids_get(uint32_t* frequencies, size_t capacity);
int dsd_app_signal_search_avoid_remove(uint32_t frequency);
/** Bumped by every change to the permanent or session avoid lists. */
uint64_t dsd_app_signal_search_avoids_revision(void);
/** Frequencies avoided for the running search session only; cleared when it stops. */
size_t dsd_app_signal_search_session_avoids_get(uint32_t* frequencies, size_t capacity);
/** Decoder-thread mirror of the session's own avoid list, kept by the control actions. */
void dsd_app_signal_search_session_avoid_add(uint64_t session, uint32_t frequency);
void dsd_app_signal_search_session_avoid_remove(uint32_t frequency);
void dsd_app_signal_search_session_avoids_clear(void);
/** Decoder session teardown: no status may outlive it as a running or owned search. */
void dsd_app_signal_search_session_ended(void);
/** Decoder-thread publication; no disk access. */
void dsd_app_signal_search_publish(const dsd_signal_search_status*, const dsd_signal_search_hit*, void*);
void dsd_app_signal_search_permanent_avoid(uint32_t frequency);
#ifdef __cplusplus
}
#endif
#endif
