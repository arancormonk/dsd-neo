// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Panel renderers for the ncurses terminal UI */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_panel_header_render(dsd_opts* opts, dsd_state* state);
void ui_panel_footer_status_render(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
