// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The status row writer (issue #669) on a real ncurses screen: a row that fits is drawn whole, one that does not is
 * cut after its last whole field with a marker, nothing ever reaches the last column (which wraps), the cursor ends at
 * column 0 of the next line, the row's attributes are back when it ends, and widths follow wcwidth().
 */

#define NCURSES_WIDECHAR 1

#include <assert.h>
#include <curses.h>
#include <dsd-neo/core/safe_api.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "ui_row.h"

#if defined(NCURSES_VERSION)

static void
set_env(const char* name, const char* value) {
#if defined(_WIN32)
    (void)_putenv_s(name, value ? value : "");
#else
    if (value) {
        (void)setenv(name, value, 1);
    } else {
        (void)unsetenv(name);
    }
#endif
}

/* The text on screen row y, columns 0..cols-1, trailing blanks dropped; non-ASCII cells read as their code point in
   "<U+XXXX>" form so a test can name them. */
static void
screen_row_text(int y, int cols, char* out, size_t out_size) {
    size_t n = 0;
    out[0] = '\0';
    for (int x = 0; x < cols && n + 12U < out_size; x++) {
        cchar_t cc;
        if (mvin_wch(y, x, &cc) == ERR) {
            break;
        }
        wchar_t wch[CCHARW_MAX + 1];
        attr_t attrs = 0;
        short pair = 0;
        DSD_MEMSET(wch, 0, sizeof wch);
        (void)getcchar(&cc, wch, &attrs, &pair, NULL);
        if (wch[0] < 0x80) {
            out[n++] = (char)wch[0];
        } else {
            n += (size_t)DSD_SNPRINTF(out + n, out_size - n, "<U+%04X>", (unsigned int)wch[0]);
            if (wcwidth(wch[0]) == 2) {
                x++; /* the right half of a wide character reads as the same character */
            }
        }
        out[n] = '\0';
    }
    while (n > 0 && out[n - 1] == ' ') {
        out[--n] = '\0';
    }
}

static int
cell_attrs(int y, int x) {
    cchar_t cc;
    assert(mvin_wch(y, x, &cc) != ERR);
    wchar_t wch[CCHARW_MAX + 1];
    attr_t attrs = 0;
    short pair = 0;
    (void)getcchar(&cc, wch, &attrs, &pair, NULL);
    return (int)attrs;
}

static void
start_row_screen(int cols) {
    erase();
    assert(resizeterm(6, cols) == OK);
    move(0, 0);
}

/* Reading cells back moves the cursor, so the cursor is checked first. */
static void
expect_row(int cols, const char* want) {
    int y = 0;
    int x = 0;
    getyx(stdscr, y, x);
    if (y != 1 || x != 0) {
        DSD_FPRINTF(stderr, "cols %d: cursor at %d,%d after the row, want 1,0\n", cols, y, x);
        assert(0);
    }
    char got[256];
    screen_row_text(0, cols, got, sizeof got);
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "cols %d: row '%s', want '%s'\n", cols, got, want);
        assert(0);
    }
}

/* "| A: 1;" (7) " B: 22;" (7) " C: 333;" (8): 22 cells in three fields. */
static void
draw_three_fields(int cols, int start_col) {
    ui_row row;
    ui_row_begin(&row, cols, start_col);
    ui_row_field(&row, "| A: %d;", 1);
    ui_row_field(&row, " B: %d;", 22);
    ui_row_field(&row, " C: %d;", 333);
    ui_row_end(&row);
}

static void
test_ascii_fit_and_cut(void) {
    /* Exact fit: 22 cells in a 23-column panel (the last column stays empty). */
    start_row_screen(23);
    draw_three_fields(23, 0);
    expect_row(23, "| A: 1; B: 22; C: 333;");
    /* One short: cut after the last whole field that leaves room for the marker. */
    start_row_screen(22);
    draw_three_fields(22, 0);
    expect_row(22, "| A: 1; B: 22;...");
    start_row_screen(20);
    draw_three_fields(20, 0);
    expect_row(20, "| A: 1; B: 22;...");
    start_row_screen(17);
    draw_three_fields(17, 0);
    expect_row(17, "| A: 1;...");
    /* No whole field fits beside the marker: cut inside the first one. */
    start_row_screen(8);
    draw_three_fields(8, 0);
    expect_row(8, "| A:...");
    /* Budgets 0..3: no room for a marker beside any text, so the text is cut bare; at 0 only the newline. */
    start_row_screen(1);
    draw_three_fields(1, 0);
    expect_row(1, "");
    start_row_screen(2);
    draw_three_fields(2, 0);
    expect_row(2, "|");
    start_row_screen(4);
    draw_three_fields(4, 0);
    expect_row(4, "| A");
    start_row_screen(5);
    draw_three_fields(5, 0);
    expect_row(5, "|...");
}

static void
test_start_column(void) {
    /* A caller that drew a one-cell border first. */
    start_row_screen(21);
    addch('#');
    draw_three_fields(21, 1);
    expect_row(21, "#| A: 1; B: 22;...");
    start_row_screen(24);
    addch('#');
    draw_three_fields(24, 1);
    expect_row(24, "#| A: 1; B: 22; C: 333;");
}

static void
test_attributes_restored(void) {
    start_row_screen(20);
    attrset(A_UNDERLINE);
    ui_row row;
    ui_row_begin(&row, 20, 0);
    ui_row_field(&row, "| A: %d;", 1);
    ui_row_attr_on(&row, A_BOLD);
    ui_row_field(&row, " B: %d;", 22);
    ui_row_field(&row, " C: %d;", 333);
    ui_row_attr_off(&row, A_BOLD);
    ui_row_field(&row, " D: %d;", 4444);
    ui_row_end(&row);
    printw("x");
    move(1, 0);
    expect_row(20, "| A: 1; B: 22;...");
    /* The highlighted field is bold; the cut dropped its attroff, and the marker and the next row are back to the
       row's own attributes. */
    assert((cell_attrs(0, 0) & A_UNDERLINE) && !(cell_attrs(0, 0) & A_BOLD));
    assert(cell_attrs(0, 9) & A_BOLD);
    assert(!(cell_attrs(0, 14) & A_BOLD) && (cell_attrs(0, 14) & A_UNDERLINE));
    assert(!(cell_attrs(1, 0) & A_BOLD) && (cell_attrs(1, 0) & A_UNDERLINE));
    attrset(A_NORMAL);
}

static void
test_ascii_sanitising(void) {
    /* Controls would move the cursor or draw as two-cell ^X; without UTF-8 every non-ASCII code point is '?'. */
    start_row_screen(40);
    ui_row row;
    ui_row_begin(&row, 40, 0);
    ui_row_field(&row, "| a\tb\x01"
                       "c\x7f"
                       "d \xC3\xA9 \xFF;");
    ui_row_end(&row);
    expect_row(40, "| a?b?c?d ? ?;");
    assert(ui_row_cells(&row, "\xC3\xA9", 2U) == 1);
}

static int
use_utf8_locale(void) {
    static const char* const names[] = {"C.UTF-8", "C.utf8", "en_US.UTF-8", "en_US.utf8"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (setlocale(LC_CTYPE, names[i]) != NULL) {
            return 1;
        }
    }
    return 0;
}

static void
test_utf8(void) {
    /* The ellipsis takes one cell, so one more field fits. */
    start_row_screen(20);
    draw_three_fields(20, 0);
    expect_row(20, "| A: 1; B: 22;<U+2026>");
    start_row_screen(16);
    draw_three_fields(16, 0);
    expect_row(16, "| A: 1; B: 22;<U+2026>");
    start_row_screen(15);
    draw_three_fields(15, 0);
    expect_row(15, "| A: 1;<U+2026>");

    /* CJK takes two cells, a combining mark none; C1 controls and invalid bytes are '?'. */
    ui_row row;
    ui_row_begin(&row, 40, 0);
    assert(ui_row_cells(&row, "\xE6\xBC\xA2\xE5\xAD\x97", 6U) == 4);
    assert(ui_row_cells(&row, "e\xCC\x81", 3U) == 1);
    start_row_screen(40);
    ui_row_begin(&row, 40, 0);
    ui_row_field(&row, "| \xE6\xBC\xA2\xE5\xAD\x97;");
    ui_row_field(&row, " e\xCC\x81 \xC2\x85 \xFF \xED\xA0\x80;");
    ui_row_end(&row);
    int y = 0;
    int x = 0;
    char got[256];
    screen_row_text(0, 40, got, sizeof got);
    if (strncmp(got, "| <U+6F22><U+5B57>; e", 21) != 0) {
        DSD_FPRINTF(stderr, "wide row '%s'\n", got);
        assert(0);
    }
    assert(strstr(got, " ? ? ???;") != NULL); /* C1, a stray byte, and each byte of an encoded surrogate */
    /* A cut never lands inside a wide character: "| 漢字;" is 7 cells and does not fit 6 beside the marker. */
    start_row_screen(8);
    ui_row_begin(&row, 8, 0);
    ui_row_field(&row, "| \xE6\xBC\xA2\xE5\xAD\x97;");
    ui_row_field(&row, " x;");
    ui_row_end(&row);
    getyx(stdscr, y, x);
    assert(y == 1 && x == 0);
    screen_row_text(0, 8, got, sizeof got);
    assert(strcmp(got, "| <U+6F22><U+5B57><U+2026>") == 0);

    /* DSD_FORCE_ASCII is the user's word even in a UTF-8 locale: periods, and '?' for non-ASCII. */
    set_env("DSD_FORCE_ASCII", "1");
    start_row_screen(20);
    draw_three_fields(20, 0);
    expect_row(20, "| A: 1; B: 22;...");
    start_row_screen(20);
    ui_row_begin(&row, 20, 0);
    ui_row_field(&row, "| \xC3\xA9;");
    ui_row_end(&row);
    expect_row(20, "| ?;");
    /* Both forced: ASCII wins, as dsd_unicode_supported() decides. */
    set_env("DSD_FORCE_UTF8", "1");
    start_row_screen(20);
    draw_three_fields(20, 0);
    expect_row(20, "| A: 1; B: 22;...");
    set_env("DSD_FORCE_ASCII", NULL);
    set_env("DSD_FORCE_UTF8", NULL);
}

static void
test_forced_utf8_in_c_locale(void) {
    /* DSD_FORCE_UTF8 under a C locale: the C library cannot encode the ellipsis, so the row stays ASCII. */
    (void)setlocale(LC_CTYPE, "C");
    set_env("DSD_FORCE_UTF8", "1");
    start_row_screen(20);
    draw_three_fields(20, 0);
    expect_row(20, "| A: 1; B: 22;...");
    set_env("DSD_FORCE_UTF8", NULL);
}

int
main(void) {
    set_env("DSD_FORCE_ASCII", NULL);
    set_env("DSD_FORCE_UTF8", NULL);
    const int have_utf8 = use_utf8_locale();
    FILE* input = tmpfile();
    FILE* output = tmpfile();
    assert(input && output);
    SCREEN* screen = newterm("xterm", output, input);
    assert(screen);
    (void)set_term(screen);

    if (have_utf8) {
        test_utf8();
    } else {
        DSD_FPRINTF(stderr, "no UTF-8 locale installed; skipping the UTF-8 cases\n");
    }
    (void)setlocale(LC_CTYPE, "C");
    test_ascii_fit_and_cut();
    test_start_column();
    test_attributes_restored();
    test_ascii_sanitising();
    test_forced_utf8_in_c_locale();

    endwin();
    delscreen(screen);
    assert(fclose(output) == 0);
    assert(fclose(input) == 0);
    return 0;
}

#else

int
main(void) {
    /* PDCurses has no off-screen terminal to draw on; the printer tests cover the writer through their stubs. */
    return 0;
}

#endif
