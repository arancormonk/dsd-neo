// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Panel renderers for the ncurses terminal UI.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_UI_PANELS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_UI_PANELS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Render the header/banner panel (tagline, version, hotkey hints). */
void ui_panel_header_render(const dsd_opts* opts, dsd_state* state);
/** @brief Render the footer/status panel (toast messages). */
void ui_panel_footer_status_render(const dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_UI_PANELS_H_ */
