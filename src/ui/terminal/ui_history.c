// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/ui/ui_history.h>

static atomic_int g_ui_history_mode = 1;

static int
ui_history_normalize_mode(int mode) {
    int m = mode % 3;
    if (m < 0) {
        m += 3;
    }
    return m;
}

int
ui_history_get_mode(void) {
    return ui_history_normalize_mode(atomic_load(&g_ui_history_mode));
}

void
ui_history_set_mode(int mode) {
    atomic_store(&g_ui_history_mode, ui_history_normalize_mode(mode));
}

int
ui_history_cycle_mode(void) {
    int current = atomic_load(&g_ui_history_mode);
    for (;;) {
        int next = ui_history_normalize_mode(current + 1);
        if (atomic_compare_exchange_strong(&g_ui_history_mode, &current, next)) {
            return next;
        }
    }
}
