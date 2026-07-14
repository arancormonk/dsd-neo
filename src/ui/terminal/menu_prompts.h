// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Async prompt, chooser, and help overlay helpers for menu subsystem.
 *
 * This header is internal to src/ui/terminal/ and should NOT be installed.
 */
#ifndef DSD_NEO_SRC_UI_TERMINAL_MENU_PROMPTS_H_
#define DSD_NEO_SRC_UI_TERMINAL_MENU_PROMPTS_H_

#include <stddef.h>

typedef void (*ui_prompt_string_done_fn)(void* user, const char* text);
typedef void (*ui_prompt_int_done_fn)(void* user, int ok, int value);
typedef void (*ui_prompt_double_done_fn)(void* user, int ok, double value);
typedef void (*ui_chooser_done_fn)(void* user, int selected);

// ---- String/Int/Double Prompt API ----

/**
 * @brief Open a string prompt asynchronously.
 *
 * The callback is invoked with entered text, "" on explicit empty submit,
 * or NULL on cancel.
 *
 * @param title    Title shown in prompt window.
 * @param prefill  Optional prefilled text (may be NULL).
 * @param cap      Max buffer capacity for input.
 * @param on_done  Callback invoked when user completes (text may be NULL on cancel).
 * @param user_ctx User context passed to callback.
 */
void ui_prompt_open_string_async(const char* title, const char* prefill, size_t cap, ui_prompt_string_done_fn on_done,
                                 void* user_ctx);

/**
 * @brief Open an integer prompt asynchronously.
 *
 * @param title    Title shown in prompt window.
 * @param initial  Initial integer value shown.
 * @param cb       Callback invoked with (user, ok, value).
 * @param user_ctx User context passed to callback.
 */
void ui_prompt_open_int_async(const char* title, int initial, ui_prompt_int_done_fn cb, void* user_ctx);

/**
 * @brief Open a double prompt asynchronously.
 *
 * @param title    Title shown in prompt window.
 * @param initial  Initial double value shown.
 * @param cb       Callback invoked with (user, ok, value).
 * @param user_ctx User context passed to callback.
 */
void ui_prompt_open_double_async(const char* title, double initial, ui_prompt_double_done_fn cb, void* user_ctx);

/**
 * @brief Close all active prompts (forcefully).
 */
void ui_prompt_close_all(void);

/**
 * @brief Check if a prompt is currently active.
 * @return 1 if active, 0 otherwise.
 */
int ui_prompt_active(void);

/**
 * @brief Handle a key event for the active prompt.
 * @param ch  Key code from ncurses.
 * @return 1 if handled, 0 otherwise.
 */
int ui_prompt_handle_key(int ch);

/**
 * @brief Render the active prompt overlay.
 */
void ui_prompt_render(void);

// ---- Help Overlay API ----

/**
 * @brief Open the help overlay.
 *
 * @param help  Help text to display.
 */
void ui_help_open(const char* help);

/**
 * @brief Close the help overlay.
 */
void ui_help_close(void);

/**
 * @brief Check if help overlay is currently active.
 * @return 1 if active, 0 otherwise.
 */
int ui_help_active(void);

/**
 * @brief Handle a key event for the help overlay.
 * @param ch  Key code from ncurses.
 * @return 1 if handled, 0 otherwise.
 */
int ui_help_handle_key(int ch);

/**
 * @brief Render the help overlay.
 */
void ui_help_render(void);

// ---- Chooser Overlay API ----

/**
 * @brief Start a chooser overlay with a list of items.
 *
 * @param title    Title shown in chooser window.
 * @param items    Array of string labels.
 * @param count    Number of items in the array.
 * @param on_done  Callback invoked with selected index (or -1 on cancel).
 * @param user_ctx User context passed to callback.
 */
void ui_chooser_start(const char* title, const char* const* items, int count, ui_chooser_done_fn on_done,
                      void* user_ctx);

/**
 * @brief Close the chooser overlay.
 */
void ui_chooser_close(void);

/**
 * @brief Check if chooser overlay is currently active.
 * @return 1 if active, 0 otherwise.
 */
int ui_chooser_active(void);

/**
 * @brief Handle a key event for the chooser overlay.
 * @param ch  Key code from ncurses.
 * @return 1 if handled, 0 otherwise.
 */
int ui_chooser_handle_key(int ch);

/**
 * @brief Render the chooser overlay.
 */
void ui_chooser_render(void);

#ifdef DSD_NEO_TEST_HOOKS
typedef struct {
    int active;
    int count;
    int sel;
    int top;
    int page_rows;
} UiChooserTestSnapshot;

typedef struct {
    int active;
    int scroll;
    int line_count;
    int page_rows;
} UiHelpTestSnapshot;

typedef struct {
    size_t start;
    size_t cursor;
    int show_left_ellipsis;
    int cursor_x;
} UiPromptViewTestSnapshot;

void ui_chooser_test_set_page_rows(int page_rows);
UiChooserTestSnapshot ui_chooser_test_snapshot(void);
void ui_help_test_set_metrics(int line_count, int page_rows, int scroll);
UiHelpTestSnapshot ui_help_test_snapshot(void);
int ui_help_wrap_line_for_test(const char* text, int width, int index, char* out, size_t out_size);
int ui_chooser_max_item_width_for_test(const char* const* items, int count);
int ui_chooser_layout_for_test(const char* title, const char* footer, int max_item, int count, int screen_h,
                               int screen_w, int* h, int* w, int* wy, int* wx);
int ui_prompt_center_axis_for_test(int screen_extent, int window_extent);
int ui_prompt_fit_width_for_test(int desired_width, int screen_width);
int ui_prompt_fit_height_for_test(int desired_height, int screen_height);
void ui_prompt_rows_for_test(int height, int* title_y, int* input_y, int* footer_y);
void ui_prompt_field_geometry_for_test(int width, int* field_col, int* field_right, int* field_width);
UiPromptViewTestSnapshot ui_prompt_view_for_test(const char* text, size_t cursor, int field_col, int field_right,
                                                 int field_width);
#endif

#endif /* DSD_NEO_SRC_UI_TERMINAL_MENU_PROMPTS_H_ */
