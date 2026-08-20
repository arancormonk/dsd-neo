// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Modal RadioReference import panel: presenter over the curses-free wizard core.
 *
 * This header is internal to src/ui/terminal/ and should NOT be installed. It names no
 * curses type so menu_core.c (and the two targets that compile menu_core.c standalone)
 * can include it without pulling curses in.
 */
#ifndef DSD_NEO_SRC_UI_TERMINAL_RR_PANEL_H_
#define DSD_NEO_SRC_UI_TERMINAL_RR_PANEL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

/** @brief Create-or-reuse the session wizard and start an import. Call only from an on_select. */
void rr_panel_open_import(dsd_opts* opts, dsd_state* state);
/** @brief Open the Imported Systems browser (use / refresh / delete). Call only from an on_select. */
void rr_panel_open_library(dsd_opts* opts, dsd_state* state);
/** @brief Nonzero while the panel owns the overlay. */
int rr_panel_active(void);
/** @brief Handle one key; returns 1 when consumed. Handles ERR and KEY_RESIZE itself. */
int rr_panel_handle_key(int ch);
/** @brief Draw the panel. Must not call ui_commit_frame(). */
void rr_panel_render(void);
/** @brief Pump the wizard. Idempotent; called ~15 ms and ~66 ms, only while the overlay is open. */
void rr_panel_tick(dsd_opts* opts, dsd_state* state);
/** @brief Cancel in-flight work and clear the active flag. Does NOT destroy the wizard. */
void rr_panel_close(void);
/** @brief Destroy the wizard. UI thread only, before curses closes; can block ~1 s. */
void rr_panel_shutdown(void);

#endif /* DSD_NEO_SRC_UI_TERMINAL_RR_PANEL_H_ */
