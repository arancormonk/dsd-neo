// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Thread-safe state for event-history display mode.
 *
 * The event-history cycle is a frontend concern and should not mutate shared
 * decoder options from renderer snapshots.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_HISTORY_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_HISTORY_H_

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Get current event-history display mode (0=Off, 1=Short, 2=Long). */
int dsd_app_frontend_history_get_mode(void);

/** @brief Set event-history display mode (normalized to [0,2]). */
void dsd_app_frontend_history_set_mode(int mode);

/** @brief Cycle mode Short->Long->Off (mod 3), returning the new mode. */
int dsd_app_frontend_history_cycle_mode(void);

/**
 * @brief Build display text for event-history lines.
 *
 * In Short mode, this compacts canonical timestamps from
 * `YYYY-MM-DD HH:MM:SS ...` to `HH:MM:SS ...` to reclaim horizontal space.
 * Other modes preserve the original string.
 *
 * @param out Destination buffer.
 * @param out_size Destination buffer size in bytes.
 * @param event_text Source event string (may be NULL).
 * @param mode Event-history mode (normalized internally).
 * @return Number of characters written to @p out (excluding NUL).
 */
size_t dsd_app_frontend_history_compact_event_text(char* out, size_t out_size, const char* event_text, int mode);

/**
 * @brief Return the timestamp used for event-history ordering.
 *
 * Canonical event strings start with `YYYY-MM-DD HH:MM:SS `. When present,
 * that displayed timestamp is authoritative so ordering matches what the UI
 * prints. Noncanonical strings use @p fallback_time.
 */
time_t dsd_app_frontend_history_event_sort_time(const char* event_text, time_t fallback_time);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_HISTORY_H_ */
