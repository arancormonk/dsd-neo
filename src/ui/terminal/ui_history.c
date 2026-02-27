// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <ctype.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/ui/ui_history.h>
#include <string.h>

static atomic_int g_ui_history_mode = 1;

static int
ui_history_normalize_mode(int mode) {
    int m = mode % 3;
    if (m < 0) {
        m += 3;
    }
    return m;
}

static int
ui_history_has_full_datetime_prefix(const char* s) {
    if (s == NULL) {
        return 0;
    }

    const size_t min_len = 20; // "YYYY-MM-DD HH:MM:SS "
    if (strlen(s) < min_len) {
        return 0;
    }

    // Canonical prefix check produced by watchdog_event_current/datacall.
    return isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2])
           && isdigit((unsigned char)s[3]) && s[4] == '-' && isdigit((unsigned char)s[5])
           && isdigit((unsigned char)s[6]) && s[7] == '-' && isdigit((unsigned char)s[8])
           && isdigit((unsigned char)s[9]) && s[10] == ' ' && isdigit((unsigned char)s[11])
           && isdigit((unsigned char)s[12]) && s[13] == ':' && isdigit((unsigned char)s[14])
           && isdigit((unsigned char)s[15]) && s[16] == ':' && isdigit((unsigned char)s[17])
           && isdigit((unsigned char)s[18]) && s[19] == ' ';
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

size_t
ui_history_compact_event_text(char* out, size_t out_size, const char* event_text, int mode) {
    if (out == NULL || out_size == 0) {
        return 0;
    }

    const char* src = event_text ? event_text : "";
    if (ui_history_normalize_mode(mode) == 1 && ui_history_has_full_datetime_prefix(src)) {
        src += 11; // drop "YYYY-MM-DD " while keeping time and payload
    }

    size_t n = strlen(src);
    if (n >= out_size) {
        n = out_size - 1;
    }
    memcpy(out, src, n);
    out[n] = '\0';
    return n;
}
