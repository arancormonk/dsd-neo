// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(misc-use-internal-linkage)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The root of the overlay menu is the receiver's signal chain, then
 * housekeeping, a separator, and Quit. This pins that contract.
 */

#include <dsd-neo/ui/menu_defs.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/ui/menu_core.h"

const NcMenuItem INPUT_MENU_ITEMS[] = {{.id = "input.child", .label = "Input Child"}};
const size_t INPUT_MENU_ITEMS_LEN = sizeof INPUT_MENU_ITEMS / sizeof INPUT_MENU_ITEMS[0];
const NcMenuItem DECODER_MENU_ITEMS[] = {{.id = "decoder.child", .label = "Decoder Child"}};
const size_t DECODER_MENU_ITEMS_LEN = sizeof DECODER_MENU_ITEMS / sizeof DECODER_MENU_ITEMS[0];
const NcMenuItem TRUNK_MENU_ITEMS[] = {{.id = "trunk.child", .label = "Trunk Child"}};
const size_t TRUNK_MENU_ITEMS_LEN = sizeof TRUNK_MENU_ITEMS / sizeof TRUNK_MENU_ITEMS[0];
const NcMenuItem ENC_MENU_ITEMS[] = {{.id = "enc.child", .label = "Encryption Child"}};
const size_t ENC_MENU_ITEMS_LEN = sizeof ENC_MENU_ITEMS / sizeof ENC_MENU_ITEMS[0];
const NcMenuItem AUDIO_MENU_ITEMS[] = {{.id = "audio.child", .label = "Audio Child"}};
const size_t AUDIO_MENU_ITEMS_LEN = sizeof AUDIO_MENU_ITEMS / sizeof AUDIO_MENU_ITEMS[0];
const NcMenuItem REC_MENU_ITEMS[] = {{.id = "rec.child", .label = "Recording Child"}};
const size_t REC_MENU_ITEMS_LEN = sizeof REC_MENU_ITEMS / sizeof REC_MENU_ITEMS[0];
const NcMenuItem DISPLAY_MENU_ITEMS[] = {{.id = "display.child", .label = "Display Child"}};
const size_t DISPLAY_MENU_ITEMS_LEN = sizeof DISPLAY_MENU_ITEMS / sizeof DISPLAY_MENU_ITEMS[0];
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
    rc |= expect_true("menu is an action row", item->kind == NC_ITEM_ACTION);
    rc |= expect_true("menu has no predicate", item->is_enabled == NULL);
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
    rc |= expect_size_eq("main menu count", n, 11U);
    rc |= expect_true("main menu pointer stable", second_items == items);
    rc |= expect_size_eq("main menu count stable", second_n, n);
    if (items == NULL || n < 11U) {
        return rc != 0 ? rc : 1;
    }

    /* Signal chain first... */
    rc |= expect_menu_item(&items[0], "main.input", "Input", INPUT_MENU_ITEMS, INPUT_MENU_ITEMS_LEN);
    rc |= expect_menu_item(&items[1], "main.decoder", "Decoder", DECODER_MENU_ITEMS, DECODER_MENU_ITEMS_LEN);
    rc |= expect_menu_item(&items[2], "main.trunking", "Trunking", TRUNK_MENU_ITEMS, TRUNK_MENU_ITEMS_LEN);
    rc |= expect_menu_item(&items[3], "main.encryption", "Encryption", ENC_MENU_ITEMS, ENC_MENU_ITEMS_LEN);
    rc |= expect_menu_item(&items[4], "main.audio", "Audio", AUDIO_MENU_ITEMS, AUDIO_MENU_ITEMS_LEN);
    rc |= expect_menu_item(&items[5], "main.recording", "Recording & logs", REC_MENU_ITEMS, REC_MENU_ITEMS_LEN);
    /* ...then what you look at, then housekeeping. */
    rc |= expect_menu_item(&items[6], "main.display", "Display", DISPLAY_MENU_ITEMS, DISPLAY_MENU_ITEMS_LEN);
    rc |= expect_menu_item(&items[7], "main.config", "Config", CONFIG_MENU_ITEMS, CONFIG_MENU_ITEMS_LEN);
    rc |= expect_menu_item(&items[8], "main.advanced", "Advanced", ADV_MENU_ITEMS, ADV_MENU_ITEMS_LEN);

    /* Quit sits alone below a rule so End + Enter is never an accident. */
    rc |= expect_true("separator before quit", items[9].kind == NC_ITEM_SEPARATOR);
    rc |= expect_true("separator has no action", items[9].on_select == NULL && items[9].submenu == NULL);
    rc |= expect_str_eq("exit id", items[10].id, "exit");
    rc |= expect_str_eq("exit label", items[10].label, "Quit DSD-neo");
    rc |= expect_str_eq("exit hotkey", items[10].hotkey, "q");
    rc |= expect_true("exit has no submenu", items[10].submenu == NULL && items[10].submenu_len == 0U);
    rc |= expect_true("exit action wired", items[10].on_select == act_exit);
    items[10].on_select(NULL);
    rc |= expect_size_eq("exit action invoked", (size_t)g_exit_calls, 1U);

    /* The root is the same in every build; nothing on it is conditional. */
    for (size_t i = 0; i < n; i++) {
        if (items[i].kind == NC_ITEM_SEPARATOR) {
            continue;
        }
        rc |= expect_true("root rows carry no predicate", items[i].is_enabled == NULL);
    }

    return rc;
}

int
main(void) {
    return test_main_menu_contract() ? 1 : 0;
}

// NOLINTEND(misc-use-internal-linkage)
