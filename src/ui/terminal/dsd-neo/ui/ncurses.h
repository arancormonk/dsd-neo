// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Terminal UI lifecycle, rendering, and input entry points.
 *
 * Declares the terminal UI entry points implemented in `src/ui/terminal/`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_UI_NCURSES_H_
#define DSD_NEO_INCLUDE_DSD_NEO_UI_NCURSES_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void dsd_terminal_open(const dsd_opts* opts, dsd_state* state);
void dsd_terminal_render(dsd_opts* opts, dsd_state* state);
void dsd_terminal_close(void);

/* A main-screen key, on the frame's published snapshot pair (issue #672, B9). Before the first publication (NULL
   @p state) only Quit is handled. */
uint8_t dsd_terminal_handle_input(const dsd_opts* opts, const dsd_state* state, int c);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_UI_NCURSES_H_ */
