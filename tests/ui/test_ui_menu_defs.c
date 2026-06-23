// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(misc-use-internal-linkage)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/ui/menu_defs.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/ui/menu_core.h"

const NcMenuItem IO_MENU_ITEMS[] = {{.id = "io.child", .label = "IO Child"}};
const size_t IO_MENU_ITEMS_LEN = sizeof IO_MENU_ITEMS / sizeof IO_MENU_ITEMS[0];
const NcMenuItem LOGGING_MENU_ITEMS[] = {{.id = "logging.child", .label = "Logging Child"}};
const size_t LOGGING_MENU_ITEMS_LEN = sizeof LOGGING_MENU_ITEMS / sizeof LOGGING_MENU_ITEMS[0];
const NcMenuItem TRUNK_MENU_ITEMS[] = {{.id = "trunk.child", .label = "Trunk Child"}};
const size_t TRUNK_MENU_ITEMS_LEN = sizeof TRUNK_MENU_ITEMS / sizeof TRUNK_MENU_ITEMS[0];
const NcMenuItem KEYS_MENU_ITEMS[] = {{.id = "keys.child", .label = "Keys Child"}};
const size_t KEYS_MENU_ITEMS_LEN = sizeof KEYS_MENU_ITEMS / sizeof KEYS_MENU_ITEMS[0];
const NcMenuItem UI_DISPLAY_MENU_ITEMS[] = {{.id = "ui.child", .label = "UI Child"}};
const size_t UI_DISPLAY_MENU_ITEMS_LEN = sizeof UI_DISPLAY_MENU_ITEMS / sizeof UI_DISPLAY_MENU_ITEMS[0];
const NcMenuItem LRRP_MENU_ITEMS[] = {{.id = "lrrp.child", .label = "LRRP Child"}};
const size_t LRRP_MENU_ITEMS_LEN = sizeof LRRP_MENU_ITEMS / sizeof LRRP_MENU_ITEMS[0];
const NcMenuItem CONFIG_MENU_ITEMS[] = {{.id = "config.child", .label = "Config Child"}};
const size_t CONFIG_MENU_ITEMS_LEN = sizeof CONFIG_MENU_ITEMS / sizeof CONFIG_MENU_ITEMS[0];
const NcMenuItem ADV_MENU_ITEMS[] = {{.id = "adv.child", .label = "Advanced Child"}};
const size_t ADV_MENU_ITEMS_LEN = sizeof ADV_MENU_ITEMS / sizeof ADV_MENU_ITEMS[0];

static int g_exit_calls = 0;

bool
io_rtl_active(const void* ctx) {
    return ctx != NULL;
}

void
act_exit(void* v) {
    (void)v;
    g_exit_calls++;
}

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static int
expect_size_eq(const char* label, size_t got, size_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s (got %zu want %zu)\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_str_eq(const char* label, const char* got, const char* want) {
    if (!got || strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s (got %s want %s)\n", label, got ? got : "(null)", want);
        return 1;
    }
    return 0;
}

static int
expect_menu_item(const NcMenuItem* item, const char* id, const char* label, const NcMenuItem* submenu,
                 size_t submenu_len) {
    int rc = 0;
    rc |= expect_true("menu item", item != NULL);
    if (!item) {
        return rc;
    }
    rc |= expect_str_eq("menu id", item->id, id);
    rc |= expect_str_eq("menu label", item->label, label);
    rc |= expect_true("menu has help", item->help != NULL && item->help[0] != '\0');
    rc |= expect_true("submenu pointer", item->submenu == submenu);
    rc |= expect_size_eq("submenu length", item->submenu_len, submenu_len);
    return rc;
}

static int
test_main_menu_contract(void) {
    const NcMenuItem* items = NULL;
    const NcMenuItem* second_items = NULL;
    size_t n = 0;
    size_t second_n = 0;
    int rc = 0;

    ui_menu_get_main_items(NULL, NULL, NULL);
    ui_menu_get_main_items(&items, &n, NULL);
    ui_menu_get_main_items(&second_items, &second_n, NULL);

    rc |= expect_true("main menu pointer set", items != NULL);
    rc |= expect_size_eq("main menu count", n, 10U);
    rc |= expect_true("main menu pointer stable", second_items == items);
    rc |= expect_size_eq("main menu count stable", second_n, n);
    if (items == NULL || n < 10U) {
        return rc != 0 ? rc : 1;
    }

    rc |= expect_menu_item(&items[0], "main.io", "Devices & IO", IO_MENU_ITEMS, IO_MENU_ITEMS_LEN);
    rc |= expect_menu_item(&items[1], "main.logging", "Logging & Capture", LOGGING_MENU_ITEMS, LOGGING_MENU_ITEMS_LEN);
    rc |= expect_menu_item(&items[2], "main.trunk", "Trunking & Control", TRUNK_MENU_ITEMS, TRUNK_MENU_ITEMS_LEN);
    rc |= expect_menu_item(&items[3], "main.keys", "Keys & Security", KEYS_MENU_ITEMS, KEYS_MENU_ITEMS_LEN);
    rc |= expect_menu_item(&items[5], "main.ui", "UI Display", UI_DISPLAY_MENU_ITEMS, UI_DISPLAY_MENU_ITEMS_LEN);
    rc |= expect_menu_item(&items[6], "lrrp", "LRRP", LRRP_MENU_ITEMS, LRRP_MENU_ITEMS_LEN);
    rc |= expect_menu_item(&items[7], "main.config", "Config", CONFIG_MENU_ITEMS, CONFIG_MENU_ITEMS_LEN);
    rc |= expect_menu_item(&items[8], "main.adv", "Advanced & Env", ADV_MENU_ITEMS, ADV_MENU_ITEMS_LEN);

    rc |= expect_str_eq("DSP menu id", items[4].id, "main.dsp");
    rc |= expect_true("DSP predicate wired", items[4].is_enabled == io_rtl_active);
    rc |= expect_true("DSP submenu disabled without USE_RADIO", items[4].submenu == NULL);
    rc |= expect_size_eq("DSP submenu length without USE_RADIO", items[4].submenu_len, 0U);

    rc |= expect_str_eq("exit id", items[9].id, "exit");
    rc |= expect_true("exit has no submenu", items[9].submenu == NULL && items[9].submenu_len == 0U);
    rc |= expect_true("exit action wired", items[9].on_select == act_exit);
    items[9].on_select(NULL);
    rc |= expect_size_eq("exit action invoked", (size_t)g_exit_calls, 1U);

    return rc;
}

int
main(void) {
    return test_main_menu_contract() ? 1 : 0;
}

// NOLINTEND(misc-use-internal-linkage)
