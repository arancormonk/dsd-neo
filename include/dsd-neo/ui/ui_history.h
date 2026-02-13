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

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Get current event-history display mode (0=Off, 1=Short, 2=Long). */
int ui_history_get_mode(void);

/** @brief Set event-history display mode (normalized to [0,2]). */
void ui_history_set_mode(int mode);

/** @brief Cycle mode Short->Long->Off (mod 3), returning the new mode. */
int ui_history_cycle_mode(void);

#ifdef __cplusplus
}
#endif
