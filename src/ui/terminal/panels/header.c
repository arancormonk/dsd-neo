// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * header.c
 * Header panel renderer for the ncurses terminal UI
 */

#include <curses.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/runtime/git_ver.h>
#include <dsd-neo/ui/panels.h>
#include <dsd-neo/ui/ui_prims.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

void
ui_panel_header_render(const dsd_opts* opts, dsd_state* state) {
    (void)state;
    if (!opts) {
        return;
    }
    // header banner
    attron(COLOR_PAIR(6));
    ui_print_hr();
    if (opts->frontend_terminal_display.terminal_compact == 1) {
        printw("| Digital Speech Decoder: DSD-neo %s (%s)  | Enter=Menu  q=Quit  | Compact (c)\n", GIT_TAG, GIT_HASH);
    } else {
        printw("| Digital Speech Decoder: DSD-neo %s (%s)  | Enter=Menu  q=Quit\n", GIT_TAG, GIT_HASH);
    }
    ui_print_hr();
    attroff(COLOR_PAIR(6));
    attron(COLOR_PAIR(4));
}
