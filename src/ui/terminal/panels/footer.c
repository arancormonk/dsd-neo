// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * footer.c
 * Footer panel renderer for the ncurses terminal UI
 */

#include <curses.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/ui/panels.h>
#include <dsd-neo/ui/ui_prims.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

void
ui_panel_footer_status_render(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    // Transient toast message (e.g., mute toggled)
    time_t now = time(NULL);
    if (state->ui_msg[0] != '\0' && state->ui_msg_expire > now) {
#ifdef PRETTY_COLORS
        // Preserve current color pair to avoid forcing default/white after toast
        attr_t saved_attrs = 0;
        short saved_pair = 0;
        attr_get(&saved_attrs, &saved_pair, NULL);
#endif
        attron(COLOR_PAIR(2));
        printw("| %s\n", state->ui_msg);
        attroff(COLOR_PAIR(2));
        ui_print_hr();
#ifdef PRETTY_COLORS
        // Restore whichever color/attrs were active before the toast
        attr_set(saved_attrs, saved_pair, NULL);
#endif
    } else if (state->ui_msg_expire <= now && state->ui_msg[0] != '\0') {
        // Clear only the UI snapshot copy here. Posting clear commands from the
        // render loop can flood the queue if the demod thread is blocked.
        state->ui_msg[0] = '\0';
        state->ui_msg_expire = 0;
    }
}
