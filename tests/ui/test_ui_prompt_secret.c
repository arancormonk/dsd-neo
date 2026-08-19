// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/ui/keymap.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "menu_prompts.h"

static int g_string_done_called = 0;
static int g_string_was_null = 0;
static char g_string_text[128];
static int g_status_called = 0;
static char g_status_text[128];

WINDOW* ui_make_window(int h, int w, int y, int x);                  // NOLINT(misc-use-internal-linkage)
void ui_statusf(const char* fmt, ...) DSD_ATTR_FORMAT(printf, 1, 2); // NOLINT(misc-use-internal-linkage)

WINDOW*
ui_make_window(int h, int w, int y, int x) { // NOLINT(misc-use-internal-linkage)
    (void)h;
    (void)w;
    (void)y;
    (void)x;
    return NULL;
}

void
ui_statusf(const char* fmt, ...) { // NOLINT(misc-use-internal-linkage)
    va_list ap;
    va_start(ap, fmt);
    DSD_VSNPRINTF(g_status_text, sizeof g_status_text, fmt, ap);
    va_end(ap);
    g_status_called++;
}

static void
reset_string_capture(void) {
    g_string_done_called = 0;
    g_string_was_null = 0;
    g_string_text[0] = '\0';
}

static void
capture_string_done(void* user, const char* text) {
    (void)user;
    g_string_done_called++;
    g_string_was_null = (text == NULL);
    if (text) {
        DSD_SNPRINTF(g_string_text, sizeof g_string_text, "%s", text);
    } else {
        g_string_text[0] = '\0';
    }
}

static void
type_text(const char* text) {
    for (const char* p = text; p && *p; p++) {
        assert(ui_prompt_handle_key((unsigned char)*p) == 1);
    }
}

// The mask changes what is drawn, never what the callback receives.
static void
test_secret_prompt_masks_display_but_not_payload(void) {
    char shown[64];

    reset_string_capture();
    ui_prompt_open_secret_async("RadioReference password", 16, capture_string_done, NULL);
    assert(ui_prompt_active() == 1);
    assert(ui_prompt_mask_active_for_test() == 1);

    type_text("s3c");
    assert(ui_prompt_display_text_for_test(shown, sizeof shown) == 1);
    assert(strcmp(shown, "***") == 0);

    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_string_done_called == 1);
    assert(g_string_was_null == 0);
    assert(strcmp(g_string_text, "s3c") == 0);
    assert(ui_prompt_active() == 0);
    assert(ui_prompt_mask_active_for_test() == 0);
}

// A plain string prompt keeps rendering its real text.
static void
test_string_prompt_is_not_masked(void) {
    char shown[64];

    reset_string_capture();
    ui_prompt_open_string_async("Plain", NULL, 16, capture_string_done, NULL);
    assert(ui_prompt_mask_active_for_test() == 0);
    type_text("s3c");
    assert(ui_prompt_display_text_for_test(shown, sizeof shown) == 1);
    assert(strcmp(shown, "s3c") == 0);
    assert(ui_prompt_handle_key('\r') == 1);
    assert(strcmp(g_string_text, "s3c") == 0);
}

// Esc delivers NULL, exactly as it does for a plain string prompt.
static void
test_secret_prompt_cancel_delivers_null(void) {
    reset_string_capture();
    ui_prompt_open_secret_async("RadioReference password", 16, capture_string_done, NULL);
    type_text("s3cr3t");
    assert(ui_prompt_handle_key(DSD_KEY_ESC) == 1);
    assert(g_string_done_called == 1);
    assert(g_string_was_null == 1);
    assert(strcmp(g_string_text, "") == 0);
    assert(ui_prompt_active() == 0);
    assert(ui_prompt_mask_active_for_test() == 0);
}

// Force-close without an explicit completion also cancels, and clears the mask.
static void
test_secret_prompt_force_close_cancels(void) {
    reset_string_capture();
    ui_prompt_open_secret_async("Forced", 16, capture_string_done, NULL);
    type_text("pw");
    ui_prompt_close_all();
    assert(g_string_done_called == 1);
    assert(g_string_was_null == 1);
    assert(ui_prompt_mask_active_for_test() == 0);
}

// ui_prompt_insert_char inserts while len + 1 < cap, so it accepts bytes while
// len <= cap - 2 and the longest secret that survives is cap - 1 -- the ordinary
// calloc(cap, 1) capacity of characters plus the NUL. Typing past the ceiling is
// silently dropped, which is what the fifth byte here proves.
static void
test_secret_prompt_capacity_is_cap_minus_one(void) {
    reset_string_capture();
    ui_prompt_open_secret_async("Tiny", 5, capture_string_done, NULL);
    type_text("abcde");
    assert(ui_prompt_handle_key(KEY_ENTER) == 1);
    assert(g_string_done_called == 1);
    assert(strcmp(g_string_text, "abcd") == 0);
}

// The display hook is defensive about its own arguments and truncates safely.
static void
test_display_text_hook_arguments(void) {
    char shown[4];

    assert(ui_prompt_active() == 0);
    assert(ui_prompt_display_text_for_test(shown, sizeof shown) == 0);
    assert(shown[0] == '\0');
    assert(ui_prompt_display_text_for_test(NULL, sizeof shown) == 0);

    reset_string_capture();
    ui_prompt_open_secret_async("Truncating", 32, capture_string_done, NULL);
    type_text("abcdefgh");
    assert(ui_prompt_display_text_for_test(shown, sizeof shown) == 1);
    assert(strcmp(shown, "***") == 0);
    ui_prompt_close_all();
}

int
main(void) {
    test_secret_prompt_masks_display_but_not_payload();
    test_string_prompt_is_not_masked();
    test_secret_prompt_cancel_delivers_null();
    test_secret_prompt_force_close_cancels();
    test_secret_prompt_capacity_is_cap_minus_one();
    test_display_text_hook_arguments();

    // The prompt widget never echoes typed text to the status line.
    assert(g_status_called == 0);
    assert(g_status_text[0] == '\0');

    printf("UI_PROMPT_SECRET: OK\n");
    return 0;
}
