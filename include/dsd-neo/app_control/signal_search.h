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
 * The worker writes an append journal, compacts atomically, and keeps memory on failures. */
int dsd_app_signal_search_store_open(const char* path);
void dsd_app_signal_search_store_close(void);
void dsd_app_signal_search_store_flush(void);
void dsd_app_signal_search_status_get(dsd_signal_search_status* status, char* error, size_t capacity);
/** Newest first; consistent copied records, retained even without any frontend screen. */
size_t dsd_app_signal_search_history_get(dsd_signal_search_hit* hits, size_t capacity);
size_t dsd_app_signal_search_avoids_get(uint32_t* frequencies, size_t capacity);
int dsd_app_signal_search_avoid_remove(uint32_t frequency);
/** Decoder-thread publication; no disk access. */
void dsd_app_signal_search_publish(const dsd_signal_search_status*, const dsd_signal_search_hit*, void*);
void dsd_app_signal_search_permanent_avoid(uint32_t frequency);
#ifdef __cplusplus
}
#endif
#endif
