// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief UI primitives for the ncurses-based terminal frontend.
 *
 * Window helpers, transient status utilities, lightweight drawing helpers,
 * and a gamma LUT for density visualizations.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_UI_UI_PRIMS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_UI_UI_PRIMS_H_

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/platform.h>

#include <curses.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Create a boxed ncurses window with keypad + nonblocking input. */
WINDOW* ui_make_window(int h, int w, int y, int x);
/** @brief Commit all pending window updates to the physical screen (double buffering). */
void ui_commit_frame(void);

/** @brief Set a transient status/footer message (printf-style). */
void ui_statusf(const char* fmt, ...) DSD_ATTR_FORMAT(printf, 1, 2);
/** @brief Copy active status into buf when not expired at `now`; returns 1 if copied. */
int ui_status_peek(char* buf, size_t n, time_t now);
/** @brief Clear status when expired at `now` (no-op if still active). */
void ui_status_clear_if_expired(time_t now);

/** @brief One wrapped row of text, as a slice of the source string. */
typedef struct {
    size_t start;
    size_t len;
} UiTextSlice;

/**
 * @brief How much of text[pos..len) belongs on one @p width-wide row.
 *
 * Breaks at the last space inside the window when there is one, otherwise
 * hard-wraps at @p width.
 */
static inline size_t
ui_text_wrap_take(const char* text, size_t pos, size_t len, size_t width) {
    const size_t remaining = len - pos;
    if (remaining <= width) {
        return remaining;
    }
    size_t brk = width;
    /* `pos + brk < len` is checked before the read and len is strlen(text), so
       the index is inside the string. clang-analyzer's ArrayBound does not tie
       the strlen of a caller's formatted buffer to that buffer's extent and
       reports the read as past it; the guard is the proof it cannot see.
       NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
    while (brk > 0 && (pos + brk) < len && text[pos + brk] != ' ') {
        brk--;
    }
    return (brk > 0) ? brk : width;
}

/**
 * @brief Break @p text into rows at most @p width columns wide.
 *
 * Pure: no curses, so the same split is what a test measures and what a widget
 * draws.
 *
 * @param rows     Receives up to @p max_rows slices.
 * @param consumed Optional; receives the bytes covered, including the spaces
 *                 skipped between rows. Less than strlen(@p text) means the
 *                 text did not fit in @p max_rows.
 * @return Rows produced, 0..@p max_rows.
 */
static inline int
ui_text_wrap(const char* text, int width, int max_rows, UiTextSlice* rows, size_t* consumed) {
    if (consumed != NULL) {
        *consumed = 0;
    }
    if (text == NULL || rows == NULL || width < 1 || max_rows < 1) {
        return 0;
    }
    const size_t len = strlen(text);
    size_t pos = 0;
    int n = 0;
    while (pos < len && n < max_rows) {
        const size_t take = ui_text_wrap_take(text, pos, len, (size_t)width);
        rows[n].start = pos;
        rows[n].len = take;
        n++;
        pos += take;
        while (pos < len && text[pos] == ' ') {
            pos++;
        }
    }
    if (consumed != NULL) {
        *consumed = pos;
    }
    return n;
}

#define UI_STATUS_FLAG_PREFIX        1u /* lead with "Status: ", as the menu footer does */
#define UI_STATUS_FLAG_ANCHOR_BOTTOM 2u /* grow upward from the last of the rows offered */
#define UI_STATUS_FLAG_BOLD          4u /* for a notice inside a dialog, which has no prefix to set it apart */
#define UI_STATUS_MAX_ROWS           4

/**
 * @brief Draw the live status toast, wrapped, into up to @p max_rows rows.
 *
 * One implementation for every window that shows the toast (menu footer,
 * prompt, chooser, RadioReference panel), so a message too long for one row
 * wraps onto the next everywhere instead of being cut off somewhere. Each row
 * it uses is cleared across @p width first; rows it does not need are left
 * alone. Clears an expired toast when there is nothing to draw.
 *
 * @param y        First of the @p max_rows rows offered.
 * @param x        Column the text starts in.
 * @param width    Columns available to the text.
 * @param flags    UI_STATUS_FLAG_* bits.
 * @return Rows drawn, 0 when no toast is live.
 */
static inline int
ui_status_draw(WINDOW* win, int y, int x, int width, int max_rows, unsigned flags, time_t now) {
    char sline[256];
    if (!ui_status_peek(sline, sizeof sline, now)) {
        ui_status_clear_if_expired(now);
        return 0;
    }
    if (win == NULL || width < 4 || max_rows < 1) {
        return 0;
    }
    if (max_rows > UI_STATUS_MAX_ROWS) {
        max_rows = UI_STATUS_MAX_ROWS;
    }
    char text[288];
    (void)DSD_SNPRINTF(text, sizeof text, "%s%s", (flags & UI_STATUS_FLAG_PREFIX) ? "Status: " : "", sline);
    UiTextSlice rows[UI_STATUS_MAX_ROWS];
    const int n = ui_text_wrap(text, width, max_rows, rows, NULL);
    const int y0 = (flags & UI_STATUS_FLAG_ANCHOR_BOTTOM) ? (y + max_rows - n) : y;
    if (flags & UI_STATUS_FLAG_BOLD) {
        wattron(win, A_BOLD);
    }
    for (int i = 0; i < n; i++) {
        mvwhline(win, y0 + i, x, ' ', width);
        mvwaddnstr(win, y0 + i, x, text + rows[i].start, (int)rows[i].len);
    }
    if (flags & UI_STATUS_FLAG_BOLD) {
        wattroff(win, A_BOLD);
    }
    return n;
}

/** @brief Draw a horizontal rule to end of line on stdscr. */
void ui_print_hr(void);
/** @brief Draw a section header prefixing the given title. */
void ui_print_header(const char* title);
/** @brief Draw a left border using the primary color. */
void ui_print_lborder(void);
/** @brief Draw a left border using the active/green color. */
void ui_print_lborder_green(void);
/** @brief Map IDEN nibble to color pair (21..28) for trunking displays. */
short ui_iden_color_pair(int iden);

/** @brief Map [0,1] -> [0,1] brightness using sqrt gamma LUT. */
double ui_gamma_map01(double f);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_UI_UI_PRIMS_H_ */
