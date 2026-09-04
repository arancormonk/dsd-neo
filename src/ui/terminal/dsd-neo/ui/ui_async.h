// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Async ncurses UI thread lifecycle and command helpers.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_UI_UI_ASYNC_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_UI_UI_ASYNC_H_H

#ifdef DSD_NEO_TEST_HOOKS
#include <stdint.h>
#endif

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the async UI subsystem (spawns UI thread).
 * @return 0 on success or when already running; negative on failure.
 */
int ui_start(dsd_opts* opts, dsd_state* state);

/** @brief Stop the async UI subsystem and join the UI thread. */
void ui_stop(void);

/**
 * @brief Returns non-zero when executing in the UI thread context.
 *
 * Use to keep ncurses calls on the UI thread while decoder threads only
 * publish snapshots.
 */
int ui_is_thread_context(void);

#ifdef DSD_NEO_TEST_HOOKS
void dsd_neo_ui_async_test_set_context(dsd_opts* opts, dsd_state* state);
const dsd_opts* dsd_neo_ui_async_test_opts_snapshot_or_default(void);
int dsd_neo_ui_async_test_curses_is_active(const dsd_opts* opts);
int dsd_neo_ui_async_test_read_key_nonblocking(const dsd_opts* opts);
void dsd_neo_ui_async_test_process_input_frame(const dsd_opts* opts);
int dsd_neo_ui_async_test_open_curses_if_needed(void);
void dsd_neo_ui_async_test_close_curses_if_opened(int curses_opened);
void dsd_neo_ui_async_test_draw_if_needed(const dsd_opts* opts, uint64_t* last_draw_ns, uint64_t frame_ns);
#endif

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_UI_UI_ASYNC_H_H */
