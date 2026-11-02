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

/**
 * @brief Frees an owned prompt or chooser context (issue #672, B8).
 *
 * The owned variants (ui_prompt_open_string_owned(), ui_chooser_start_owned())
 * take the context with this function and call it exactly once, after the
 * widget has stopped reading anything the context holds (a borrowed title, the
 * item strings and the array of them): after the done-callback returns, or
 * without a callback when the widget is closed, replaced or shut down. A
 * done-callback that hands the context on to another owned widget takes it
 * back first with ui_prompt_take_context() / ui_chooser_take_context(), and the
 * dispose is then the new widget's to call. A start or open handed a context
 * that is already owned - by the callback running now, or by the open widget of
 * the same kind it replaces - takes that ownership over the same way (no
 * dispose then); given no dispose of its own, it disposes with the owner's.
 *
 * A context belongs to one widget at a time. Never open a prompt with a
 * context an open chooser still owns, or the reverse: neither claims the
 * other's, so both would dispose it. A cancel callback (Esc, or the cancel a
 * close, a replacing open or shutdown delivers) must not reopen a widget with
 * the context it was handed: the replace or shutdown that is closing it goes
 * on to close and dispose that new widget too, freeing the context under it.
 */
typedef void (*ui_context_dispose_fn)(void* user);

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
 * @brief Open a string prompt that owns its context (issue #672, B8).
 *
 * As ui_prompt_open_string_async(), and @p dispose(user_ctx) runs exactly once:
 * after @p on_done returns (submit, Esc, or the cancel a close, a replacing
 * open or a failed buffer allocation delivers), unless the callback took the
 * context with ui_prompt_take_context(). The title may point into the context.
 */
void ui_prompt_open_string_owned(const char* title, const char* prefill, size_t cap, ui_prompt_string_done_fn on_done,
                                 void* user_ctx, ui_context_dispose_fn dispose);

/**
 * @brief Take the context of the owned prompt whose callback is running.
 *
 * Only valid inside that done-callback: returns the context and clears the
 * pending dispose, so the caller now owns it (to hand it to another owned
 * widget, or to free it). Returns NULL outside an owned prompt's callback, and
 * on a second call.
 */
void* ui_prompt_take_context(void);

/**
 * @brief Open a masked (secret) string prompt asynchronously.
 *
 * Identical to ui_prompt_open_string_async() except that the field renders one
 * '*' per typed byte, the input buffer is overwritten with zeroes before it is
 * freed, and the copy handed to @p on_done is overwritten after the callback
 * returns. There is no prefill parameter on purpose: a secret is never
 * redisplayed.
 *
 * The visible length of the secret is still disclosed by the cursor column;
 * that is accepted.
 *
 * @param title    Title shown in prompt window. NOT copied: the widget stores
 *                 the pointer, so it must outlive the prompt.
 * @param cap      Buffer capacity, as passed to calloc(). The longest secret
 *                 that survives is cap - 1 bytes (characters plus the NUL), so
 *                 pass max_secret_len + 1; further typing is silently dropped.
 *                 Values below 2 are clamped to 2.
 * @param on_done  Callback invoked when the user completes. The text is freed
 *                 immediately after the callback returns, so a caller that
 *                 keeps it must copy it (and scrub its own copy). NULL text
 *                 means cancel; "" means Enter on an empty field.
 * @param user_ctx User context passed to callback.
 */
void ui_prompt_open_secret_async(const char* title, size_t cap, ui_prompt_string_done_fn on_done, void* user_ctx);

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
 *
 * An open prompt's callback gets the cancel (NULL text) after the prompt is
 * gone, so the callback may open another; an owned prompt's context is then
 * disposed.
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
 * @brief Start a chooser overlay with the bar already on one item.
 *
 * For pickers that show a setting's current value: opening on index 0 means the
 * next Enter picks whatever happens to be first, which for a destructive first
 * entry turns a double-tap into a state change nobody asked for.
 *
 * @param initial_sel Item to open on; out-of-range values fall back to 0.
 */
void ui_chooser_start_at(const char* title, const char* const* items, int count, int initial_sel,
                         ui_chooser_done_fn on_done, void* user_ctx);

/**
 * @brief Start a chooser that owns its context (issue #672, B8).
 *
 * As ui_chooser_start_at(), and @p dispose(user_ctx) runs exactly once: after
 * @p on_done returns (pick, cancel, the empty list's or the render path's
 * cancel), unless the callback took the context with
 * ui_chooser_take_context(); or, with no callback, when the chooser is closed
 * (ui_chooser_close(), shutdown) or replaced by another start (a start handed
 * this same context takes it over instead: see ui_context_dispose_fn). The
 * title, the item array and its strings may all live in the context.
 */
void ui_chooser_start_owned(const char* title, const char* const* items, int count, int initial_sel,
                            ui_chooser_done_fn on_done, void* user_ctx, ui_context_dispose_fn dispose);

/**
 * @brief Take the context of the owned chooser whose callback is running.
 *
 * Only valid inside that done-callback: returns the context and clears the
 * pending dispose, so the caller now owns it - to start the next owned
 * chooser or prompt with it, which disposes it from then on. Returns NULL
 * outside an owned chooser's callback, and on a second call.
 */
void* ui_chooser_take_context(void);

/**
 * @brief Close the chooser overlay.
 *
 * No callback runs; an owned chooser's context is disposed.
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
/** @brief Test hook: the next chooser start cancels at once, as an empty list does (a failed hand-off). */
void ui_chooser_test_fail_next_start(void);
/** @brief Test hook: the next prompt open fails its buffer allocation (cancel delivered synchronously). */
void ui_prompt_test_fail_next_alloc(void);
UiChooserTestSnapshot ui_chooser_test_snapshot(void);
/** @brief Test hook: the open chooser's item @p index as it was started with, or NULL when none is open or it is out
 *         of range. */
const char* ui_chooser_test_item(int index);
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

/** @brief Test hook: 1 when the active prompt masks its input, 0 otherwise. */
int ui_prompt_mask_active_for_test(void);

/** @brief Test hook: the bytes the last prompt close zeroed in the input buffer (its capacity), then reset to 0. */
size_t ui_prompt_zeroed_on_close_for_test(void);

/**
 * @brief Test hook: the text ui_prompt_render() would draw for the whole buffer.
 *
 * Masking applied, horizontal scroll window NOT applied (that window depends on
 * the curses screen geometry). Truncated to @p out_size - 1 bytes.
 *
 * @param out      Destination buffer; set to "" on every failure path.
 * @param out_size Size of @p out in bytes.
 * @return 1 when a prompt is active and @p out was filled, 0 otherwise.
 */
int ui_prompt_display_text_for_test(char* out, size_t out_size);
#endif

#endif /* DSD_NEO_SRC_UI_TERMINAL_MENU_PROMPTS_H_ */
