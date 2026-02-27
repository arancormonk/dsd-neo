// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Thread-safe state for ncurses event-history display mode.
 *
 * The event-history cycle (h) is a UI concern and should not mutate shared
 * decoder options from renderer snapshots.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Get current event-history display mode (0=Off, 1=Short, 2=Long). */
int ui_history_get_mode(void);

/** @brief Set event-history display mode (normalized to [0,2]). */
void ui_history_set_mode(int mode);

/** @brief Cycle mode Short->Long->Off (mod 3), returning the new mode. */
int ui_history_cycle_mode(void);

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
size_t ui_history_compact_event_text(char* out, size_t out_size, const char* event_text, int mode);

#ifdef __cplusplus
}
#endif
