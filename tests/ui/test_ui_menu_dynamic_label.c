// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression coverage for dynamic menu labels:
 *  - label_fn results must remain valid after the internal label helper returns
 *  - submenu suffix rendering must not overwrite or truncate the dynamic text
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/ui/menu_core.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "menu_internal.h"

static const NcMenuItem SUB_ITEMS[] = {
    {.id = "child", .label = "Child"},
};

static const char*
dynamic_label(const void* ctx, char* buf, size_t buf_len) {
    const char* state = (const char*)ctx;
    DSD_SNPRINTF(buf, buf_len, "Toggle Payload Logging [%s]", state ? state : "Inactive");
    return buf;
}

WINDOW*
ui_make_window(int h, int w, int y, int x) { // NOLINT(misc-use-internal-linkage)
    (void)h;
    (void)w;
    (void)y;
    (void)x;
    return NULL;
}

int
ui_status_peek(char* buf, size_t n, time_t now) { // NOLINT(misc-use-internal-linkage)
    (void)buf;
    (void)n;
    (void)now;
    return 0;
}

void
ui_status_clear_if_expired(time_t now) { // NOLINT(misc-use-internal-linkage)
    (void)now;
}

int
main(void) {
    const NcMenuItem leaf = {.id = "leaf", .label = "Fallback", .label_fn = dynamic_label};
    char label[140];

    const char* got = ui_menu_item_label_for_test(&leaf, "Active", label, sizeof label);
    assert(got == label);
    assert(strcmp(got, "Toggle Payload Logging [Active]") == 0);

    DSD_MEMSET(label, 0x5A, sizeof label);
    const NcMenuItem submenu = {.id = "submenu",
                                .label = "Fallback",
                                .label_fn = dynamic_label,
                                .submenu = SUB_ITEMS,
                                .submenu_len = sizeof SUB_ITEMS / sizeof SUB_ITEMS[0]};

    got = ui_menu_item_label_for_test(&submenu, "Inactive", label, sizeof label);
    assert(got == label);
    assert(strcmp(got, "Toggle Payload Logging [Inactive] >") == 0);

    printf("UI_MENU_DYNAMIC_LABEL: OK\n");
    return 0;
}
