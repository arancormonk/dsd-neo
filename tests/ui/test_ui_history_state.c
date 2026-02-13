// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <stdio.h>

#include <dsd-neo/ui/ui_history.h>

int
main(void) {
    ui_history_set_mode(1);
    assert(ui_history_get_mode() == 1);

    assert(ui_history_cycle_mode() == 2);
    assert(ui_history_get_mode() == 2);

    assert(ui_history_cycle_mode() == 0);
    assert(ui_history_get_mode() == 0);

    assert(ui_history_cycle_mode() == 1);
    assert(ui_history_get_mode() == 1);

    ui_history_set_mode(-1);
    assert(ui_history_get_mode() == 2);

    ui_history_set_mode(8);
    assert(ui_history_get_mode() == 2);

    printf("UI_HISTORY_STATE: OK\n");
    return 0;
}
