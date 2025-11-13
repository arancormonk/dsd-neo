// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Panel renderers for the ncurses terminal UI */

#pragma once

#include <dsd-neo/core/dsd.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_panel_header_render(dsd_opts* opts, dsd_state* state);
void ui_panel_footer_status_render(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
