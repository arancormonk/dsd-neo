// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression coverage for overflowed chooser paging:
 *  - PgDn should advance the visible range by a full page step
 *  - PgUp should restore the previous page
 *  - Home/End should anchor to the first/last page
 * The owned chooser (issue #672, B8): its context is disposed exactly once on every path - pick, cancel, close,
 * replacement, the empty list, the render path's cancel - after the callback, unless the callback took it; a callback
 * that opens another chooser leaves the new one untouched.
 * Rendering (issue #672, B7) on a real ncurses screen: items and the title are measured in cells and cut between code
 * points with the status row's marker, whatever bytes they hold.
 */

#define NCURSES_WIDECHAR 1

#include <assert.h>
#include <curses.h>
#include <dsd-neo/core/safe_api.h>
#include <locale.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <time.h>
#include "menu_prompts.h"
#include "test_env.h"

WINDOW* ui_make_window(int h, int w, int y, int x); // NOLINT(misc-use-internal-linkage)
void ui_statusf(const char* fmt, ...);              // NOLINT(misc-use-internal-linkage)

/* 1: ui_make_window() makes a real window on the test's screen (the rendering cases); 0: none, so a render cancels. */
static int g_real_windows = 0;
static WINDOW* g_last_win = NULL;

static int g_done_sel = -2;

static const char* const ITEMS[] = {
    "Item 01", "Item 02", "Item 03", "Item 04", "Item 05", "Item 06", "Item 07", "Item 08", "Item 09", "Item 10",
    "Item 11", "Item 12", "Item 13", "Item 14", "Item 15", "Item 16", "Item 17", "Item 18", "Item 19", "Item 20",
};

static void
capture_done(void* user, int sel) {
    (void)user;
    g_done_sel = sel;
}

int ui_status_peek(char* buf, size_t n, time_t now); // NOLINT(misc-use-internal-linkage)
void ui_status_clear_if_expired(time_t now);         // NOLINT(misc-use-internal-linkage)

/* The widgets now draw the live toast; these tests raise none. */
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

void
ui_statusf(const char* fmt, ...) { // NOLINT(misc-use-internal-linkage)
    (void)fmt;
}

WINDOW*
ui_make_window(int h, int w, int y, int x) { // NOLINT(misc-use-internal-linkage)
    if (!g_real_windows) {
        return NULL;
    }
    g_last_win = newwin(h, w, y, x);
    return g_last_win;
}

/* ---- Owned contexts (issue #672, B8) ---- */

typedef struct {
    int disposed;
    int callbacks;
    int last_sel;
    int disposed_before_callback_returned;
    int take_in_callback;     /* 1: the callback takes the context */
    int hand_on;              /* 1: the taken context starts the next owned chooser */
    int open_other;           /* 1: the callback starts another owned chooser with g_other */
    int fail_next_in_handoff; /* 1: the next start answers at once */
    void* taken;
    void* taken_again;
} OwnedCtx;

static OwnedCtx g_ctx_a;
static OwnedCtx g_other;
static int g_in_callback = 0;
static const char* const OWNED_ITEMS[] = {"One", "Two", "Three"};

static void
count_dispose(void* user) {
    OwnedCtx* c = (OwnedCtx*)user;
    c->disposed++;
    if (g_in_callback) {
        c->disposed_before_callback_returned = 1;
    }
}

static void owned_done(void* user, int sel);

static void
owned_done(void* user, int sel) {
    OwnedCtx* c = (OwnedCtx*)user;
    g_in_callback++;
    c->callbacks++;
    c->last_sel = sel;
    if (c->take_in_callback) {
        c->taken = ui_chooser_take_context();
        c->taken_again = ui_chooser_take_context();
    }
    if (c->hand_on && c->taken) {
        c->hand_on = 0;
        c->take_in_callback = 0;
        if (c->fail_next_in_handoff) {
            ui_chooser_test_fail_next_start();
        }
        ui_chooser_start_owned("Next", OWNED_ITEMS, 3, 0, owned_done, c->taken, count_dispose);
    }
    if (c->open_other) {
        c->open_other = 0;
        ui_chooser_start_owned("Other", OWNED_ITEMS, 3, 0, owned_done, &g_other, count_dispose);
    }
    g_in_callback--;
}

static void
reset_owned(void) {
    DSD_MEMSET(&g_ctx_a, 0, sizeof g_ctx_a);
    DSD_MEMSET(&g_other, 0, sizeof g_other);
    g_ctx_a.last_sel = -2;
    g_other.last_sel = -2;
}

static void
start_owned_a(void) {
    reset_owned();
    ui_chooser_start_owned("Owned", OWNED_ITEMS, 3, 1, owned_done, &g_ctx_a, count_dispose);
    assert(ui_chooser_active() == 1);
    assert(ui_chooser_test_snapshot().sel == 1);
    assert(g_ctx_a.disposed == 0);
}

/* What a borrowed chooser's callback could take. */
typedef struct {
    void* taken;
} TakeProbe;

static void
borrowed_done(void* user, int sel) {
    (void)sel;
    TakeProbe* probe = (TakeProbe*)user;
    probe->taken = ui_chooser_take_context();
}

/* Heap contexts (ASan sees a dispose under a widget that still reads them) for the implicit-ownership cases. */
typedef struct {
    int callbacks;
    int restart;  /* 1: the callback restarts the chooser with its own context, without taking it */
    int borrowed; /* 1: that restart is a plain (borrowed) start */
} HeapCtx;

static int g_heap_disposed = 0;

static void
heap_dispose(void* user) {
    g_heap_disposed++;
    free(user);
}

static void
heap_done(void* user, int sel) {
    HeapCtx* c = (HeapCtx*)user;
    (void)sel;
    c->callbacks++;
    if (c->restart) {
        c->restart = 0;
        if (c->borrowed) {
            ui_chooser_start_at("Again", OWNED_ITEMS, 3, 0, heap_done, c);
        } else {
            ui_chooser_start_owned("Again", OWNED_ITEMS, 3, 0, heap_done, c, heap_dispose);
        }
    }
}

static HeapCtx*
new_heap_ctx(int restart, int borrowed) {
    HeapCtx* c = (HeapCtx*)calloc(1, sizeof(*c));
    assert(c != NULL);
    c->restart = restart;
    c->borrowed = borrowed;
    return c;
}

/* A start handed a context someone already owns takes that ownership over: no dispose under the new chooser, one at
   its end (issue #672, B8). */
static void
test_owned_chooser_reuse(void) {
    for (int borrowed = 0; borrowed <= 1; borrowed++) {
        /* A callback that restarts with its own context without taking it. */
        g_heap_disposed = 0;
        HeapCtx* c = new_heap_ctx(1, borrowed);
        ui_chooser_start_owned("Owned", OWNED_ITEMS, 3, 0, heap_done, c, heap_dispose);
        assert(ui_chooser_handle_key('\n') == 1);
        assert(g_heap_disposed == 0);
        assert(ui_chooser_active() == 1);
        assert(c->callbacks == 1);
        assert(ui_chooser_handle_key('\n') == 1);
        assert(g_heap_disposed == 1);
        assert(ui_chooser_active() == 0);

        /* A start over the open owned chooser handed its context. */
        g_heap_disposed = 0;
        c = new_heap_ctx(0, 0);
        ui_chooser_start_owned("Owned", OWNED_ITEMS, 3, 0, heap_done, c, heap_dispose);
        if (borrowed) {
            ui_chooser_start_at("Plain", OWNED_ITEMS, 3, 0, heap_done, c);
        } else {
            ui_chooser_start_owned("Again", OWNED_ITEMS, 3, 0, heap_done, c, heap_dispose);
        }
        assert(g_heap_disposed == 0);
        assert(ui_chooser_handle_key(27) == 1);
        assert(g_heap_disposed == 1);
    }
}

static void
test_owned_chooser(void) {
    /* Pick: the callback, then the dispose, once. */
    start_owned_a();
    assert(ui_chooser_handle_key('\n') == 1);
    assert(g_ctx_a.callbacks == 1 && g_ctx_a.last_sel == 1);
    assert(g_ctx_a.disposed == 1 && !g_ctx_a.disposed_before_callback_returned);
    assert(ui_chooser_active() == 0);

    /* Esc. */
    start_owned_a();
    assert(ui_chooser_handle_key(27) == 1);
    assert(g_ctx_a.callbacks == 1 && g_ctx_a.last_sel == -1 && g_ctx_a.disposed == 1);

    /* Closed (the exit path): no callback, disposed once; a second close disposes nothing. */
    start_owned_a();
    ui_chooser_close();
    assert(g_ctx_a.callbacks == 0 && g_ctx_a.disposed == 1);
    ui_chooser_close();
    assert(g_ctx_a.disposed == 1);

    /* Replaced by a borrowed chooser: disposed once without a callback; the new one is untouched. */
    start_owned_a();
    ui_chooser_start_at("Plain", OWNED_ITEMS, 3, 0, owned_done, &g_other);
    assert(g_ctx_a.callbacks == 0 && g_ctx_a.disposed == 1);
    assert(ui_chooser_active() == 1);
    ui_chooser_close();
    assert(g_other.disposed == 0 && g_other.callbacks == 0);

    /* Replaced by a start handed the same context: no dispose (it moves to the new start), then one at its pick. */
    start_owned_a();
    ui_chooser_start_owned("Again", OWNED_ITEMS, 3, 0, owned_done, &g_ctx_a, count_dispose);
    assert(g_ctx_a.disposed == 0);
    assert(ui_chooser_handle_key('\n') == 1);
    assert(g_ctx_a.callbacks == 1 && g_ctx_a.disposed == 1);

    /* A callback that opens another owned chooser: the old context goes after the callback, the new one stays open. */
    start_owned_a();
    g_ctx_a.open_other = 1;
    assert(ui_chooser_handle_key('\n') == 1);
    assert(g_ctx_a.disposed == 1 && !g_ctx_a.disposed_before_callback_returned);
    assert(ui_chooser_active() == 1);
    assert(g_other.disposed == 0);
    ui_chooser_close();
    assert(g_other.disposed == 1 && g_other.callbacks == 0);

    /* A callback that takes its context and hands it to the next level: not disposed until that level is done. */
    start_owned_a();
    g_ctx_a.take_in_callback = 1;
    g_ctx_a.hand_on = 1;
    assert(ui_chooser_handle_key('\n') == 1);
    assert(g_ctx_a.taken == &g_ctx_a && g_ctx_a.taken_again == NULL);
    assert(g_ctx_a.disposed == 0);
    assert(ui_chooser_active() == 1);
    assert(ui_chooser_handle_key(KEY_DOWN) == 1);
    assert(ui_chooser_handle_key('\n') == 1);
    assert(g_ctx_a.callbacks == 2 && g_ctx_a.last_sel == 1);
    assert(g_ctx_a.disposed == 1);

    /* The same hand-off whose next start answers at once: the next level's callback runs inside this one, and the
       context is still disposed exactly once, after it. */
    start_owned_a();
    g_ctx_a.take_in_callback = 1;
    g_ctx_a.hand_on = 1;
    g_ctx_a.fail_next_in_handoff = 1;
    assert(ui_chooser_handle_key('\n') == 1);
    assert(g_ctx_a.callbacks == 2 && g_ctx_a.last_sel == -1);
    assert(g_ctx_a.disposed == 1);
    assert(ui_chooser_active() == 0);

    /* The empty list answers at once, then disposes. */
    reset_owned();
    ui_chooser_start_owned("Empty", OWNED_ITEMS, 0, 0, owned_done, &g_ctx_a, count_dispose);
    assert(g_ctx_a.callbacks == 1 && g_ctx_a.last_sel == -1 && g_ctx_a.disposed == 1);
    assert(ui_chooser_active() == 0);

    /* The render path's cancel (no window can be made): the callback, then the dispose. */
    start_owned_a();
    ui_chooser_render();
    assert(g_ctx_a.callbacks == 1 && g_ctx_a.last_sel == -1 && g_ctx_a.disposed == 1);
    assert(ui_chooser_active() == 0);

    /* Take outside any callback, and inside a borrowed chooser's: nothing to take. */
    assert(ui_chooser_take_context() == NULL);
    /* Static: the chooser keeps its context by pointer. */
    static TakeProbe probe;
    probe = (TakeProbe){&g_ctx_a};
    ui_chooser_start("Plain", OWNED_ITEMS, 3, borrowed_done, &probe);
    assert(ui_chooser_handle_key('\n') == 1);
    assert(probe.taken == NULL);
}

/* The chooser copies the title, the item array and its strings as it starts: the caller's storage, rewritten once the
   start returns, changes nothing it holds or answers. */
static void
test_chooser_copies_its_text(void) {
    char first[16];
    char second[16];
    (void)DSD_SNPRINTF(first, sizeof first, "%s", "First");
    (void)DSD_SNPRINTF(second, sizeof second, "%s", "Second");
    const char* items[] = {first, second};
    g_done_sel = -2;
    ui_chooser_start("Copied", items, 2, capture_done, NULL);
    (void)DSD_SNPRINTF(first, sizeof first, "%s", "Rewritten");
    DSD_MEMSET(second, 0, sizeof second);
    items[0] = NULL;
    items[1] = NULL;
    assert(ui_chooser_test_snapshot().count == 2);
    assert(ui_chooser_test_item(0) != NULL && strcmp(ui_chooser_test_item(0), "First") == 0);
    assert(ui_chooser_test_item(1) != NULL && strcmp(ui_chooser_test_item(1), "Second") == 0);
    assert(ui_chooser_handle_key(KEY_DOWN) == 1);
    assert(ui_chooser_handle_key('\r') == 1);
    assert(g_done_sel == 1);
    assert(ui_chooser_active() == 0);
}

/* ---- Rendering (issue #672, B7) ---- */

#if defined(NCURSES_VERSION)

/* The text of window row y from column x, trailing blanks dropped; non-ASCII cells read as "<U+XXXX>", a combining mark
   in the same cell as "+<U+XXXX>". */
static void
win_row_text(WINDOW* win, int y, int x, char* out, size_t out_size) {
    size_t n = 0;
    out[0] = '\0';
    const int cols = getmaxx(win);
    while (x < cols - 1 && n + 24U < out_size) {
        cchar_t cc;
        if (mvwin_wch(win, y, x, &cc) == ERR) {
            break;
        }
        wchar_t wch[CCHARW_MAX + 1];
        attr_t attrs = 0;
        short pair = 0;
        DSD_MEMSET(wch, 0, sizeof wch);
        (void)getcchar(&cc, wch, &attrs, &pair, NULL);
        int step = 1;
        if (wch[0] < 0x80) {
            out[n++] = (char)wch[0];
            out[n] = '\0';
        } else {
            n += (size_t)DSD_SNPRINTF(out + n, out_size - n, "<U+%04X>", (unsigned int)wch[0]);
            if (wcwidth(wch[0]) == 2) {
                step = 2;
            }
        }
        if (wch[1] != 0) {
            n += (size_t)DSD_SNPRINTF(out + n, out_size - n, "+<U+%04X>", (unsigned int)wch[1]);
        }
        x += step;
    }
    while (n > 0 && out[n - 1] == ' ') {
        out[--n] = '\0';
    }
}

static void
expect_row(int y, const char* want) {
    char got[512];
    win_row_text(g_last_win, y, 2, got, sizeof got);
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "row %d: '%s', want '%s'\n", y, got, want);
        assert(0);
    }
}

static void
ignore_done(void* user, int sel) {
    (void)user;
    (void)sel;
}

/* A 30-column screen: the window is 28 wide, so text gets 24 cells. */
static void
render_on(int rows, const char* title, const char* const* items, int count) {
    assert(resizeterm(rows, 30) == OK);
    ui_chooser_start(title, items, count, ignore_done, NULL);
    ui_chooser_render();
    assert(ui_chooser_active() == 1 && g_last_win != NULL);
    assert(getmaxx(g_last_win) == 28);
}

static void
test_render_ascii(void) {
    /* Without UTF-8 output: three periods, and every non-ASCII code point (and a stray byte) one '?'. */
    static const char* const items[] = {"short", "a very long item that will not fit at all",
                                        "\xC3\xA9t\xC3\xA9 \xFF!"};
    render_on(12, "Pick", items, 3);
    expect_row(1, "Pick");
    expect_row(3, "short");
    expect_row(4, "a very long item that...");
    expect_row(5, "?t? ?!");
    ui_chooser_close();
    assert(ui_chooser_max_item_width_for_test(items, 3) == 41);

    /* The title it draws is its own copy: the caller's buffer, rewritten after the start, is not what it shows. */
    char title[8];
    (void)DSD_SNPRINTF(title, sizeof title, "%s", "Mine");
    render_on(12, title, items, 3);
    (void)DSD_SNPRINTF(title, sizeof title, "%s", "Theirs");
    ui_chooser_render();
    expect_row(1, "Mine");
    ui_chooser_close();
    static const char* const cjk[] = {"\xE6\xBC\xA2\xE5\xAD\x97"};
    assert(ui_chooser_max_item_width_for_test(cjk, 1) == 2); /* two '?' */
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

/* @p pairs times "漢字" (two characters, four cells). */
static void
kanji(char* out, size_t out_size, int pairs) {
    out[0] = '\0';
    for (int i = 0; i < pairs; i++) {
        const size_t used = strlen(out);
        (void)DSD_SNPRINTF(out + used, out_size - used, "%s", "\xE6\xBC\xA2\xE5\xAD\x97");
    }
}

static void
test_render_utf8(void) {
    /* Widths in cells: CJK two, a combining mark none (it stays with its letter across the cut), an emoji outside the
       Basic Multilingual Plane two; controls and invalid UTF-8 are '?'. */
    char wide[64];
    kanji(wide, sizeof wide, 7); /* 14 CJK, 28 cells */
    const char* const items[] = {
        wide,
        "xxxxxxxxxxxxxxxxxxxxxxe\xCC\x81yy", /* 22 + 1 + 2 cells */
        "\xF0\x9F\x98\x80 ok",
        "a\tb \xFF \xED\xA0\x80",
    };
    assert(ui_chooser_max_item_width_for_test(items, 4) == 28);

    /* Ten rows: a seven-row window shows three items, so the title carries the range, which stays whole. */
    render_on(10, "TTTTTTTTTTTTTTTTTTTTTTTTTTTT", items, 4);
    expect_row(1, "TTTTTTTTTTTTTTT<U+2026> (1-3/4)");
    expect_row(3, "<U+6F22><U+5B57><U+6F22><U+5B57><U+6F22><U+5B57><U+6F22><U+5B57><U+6F22><U+5B57><U+6F22><U+2026>");
    expect_row(4, "xxxxxxxxxxxxxxxxxxxxxxe+<U+0301><U+2026>");
    expect_row(5, "<U+1F600> ok");
    assert(ui_chooser_handle_key(KEY_END) == 1);
    ui_chooser_render();
    expect_row(1, "TTTTTTTTTTTTTTT<U+2026> (2-4/4)");
    expect_row(5, "a?b ? ???");
    ui_chooser_close();

    /* A title that fits is drawn whole, measured in cells: twelve CJK characters take 24, the whole body. */
    static const char* const one[] = {"x"};
    char title[64];
    kanji(title, sizeof title, 6);
    render_on(12, title, one, 1);
    expect_row(1, "<U+6F22><U+5B57><U+6F22><U+5B57><U+6F22><U+5B57><U+6F22><U+5B57><U+6F22><U+5B57><U+6F22><U+5B57>");
    ui_chooser_close();

    /* DSD_FORCE_ASCII is the user's word even in a UTF-8 locale. */
    (void)dsd_test_setenv("DSD_FORCE_ASCII", "1", 1);
    render_on(12, "Pick", items, 4);
    expect_row(3, "??????????????");
    ui_chooser_close();
    (void)dsd_test_unsetenv("DSD_FORCE_ASCII");
}

static void
test_render(void) {
    (void)dsd_test_unsetenv("DSD_FORCE_ASCII");
    (void)dsd_test_unsetenv("DSD_FORCE_UTF8");
    const int have_utf8 = use_utf8_locale();
    FILE* input = tmpfile();
    FILE* output = tmpfile();
    assert(input && output);
    SCREEN* screen = newterm("xterm", output, input);
    assert(screen);
    (void)set_term(screen);
    g_real_windows = 1;

    if (have_utf8) {
        test_render_utf8();
    } else {
        DSD_FPRINTF(stderr, "no UTF-8 locale installed; skipping the UTF-8 cases\n");
    }
    (void)setlocale(LC_CTYPE, "C");
    test_render_ascii();

    g_real_windows = 0;
    endwin();
    delscreen(screen);
    assert(fclose(output) == 0);
    assert(fclose(input) == 0);
}

#else

static void
test_render(void) {
    /* PDCurses has no off-screen terminal to draw on; UI_TEXT covers the shared model the chooser draws with. */
}

#endif

int
main(void) {
    UiChooserTestSnapshot snapshot;

    ui_chooser_start("Devices", ITEMS, (int)(sizeof ITEMS / sizeof ITEMS[0]), capture_done, NULL);
    ui_chooser_test_set_page_rows(10);
    snapshot = ui_chooser_test_snapshot();
    assert(snapshot.top == 0);
    assert(snapshot.sel == 0);
    assert(snapshot.count == 20);

    assert(ui_chooser_handle_key(KEY_NPAGE) == 1);
    snapshot = ui_chooser_test_snapshot();
    assert(snapshot.top == 9);
    assert(snapshot.sel == 9);

    assert(ui_chooser_handle_key(KEY_PPAGE) == 1);
    snapshot = ui_chooser_test_snapshot();
    assert(snapshot.top == 0);
    assert(snapshot.sel == 0);

    assert(ui_chooser_handle_key(KEY_END) == 1);
    snapshot = ui_chooser_test_snapshot();
    assert(snapshot.top == 10);
    assert(snapshot.sel == 19);

    assert(ui_chooser_handle_key(KEY_HOME) == 1);
    snapshot = ui_chooser_test_snapshot();
    assert(snapshot.top == 0);
    assert(snapshot.sel == 0);

    assert(ui_chooser_handle_key('\r') == 1);
    assert(g_done_sel == 0);

    /* 'q' quits the program from the main screen, so inside a picker it is inert; Esc cancels. */
    ui_chooser_start("Devices", ITEMS, (int)(sizeof ITEMS / sizeof ITEMS[0]), capture_done, NULL);
    assert(ui_chooser_handle_key('q') == 1);
    assert(g_done_sel == 0);
    assert(ui_chooser_active() == 1);
    assert(ui_chooser_handle_key(27) == 1);
    assert(g_done_sel == -1);
    assert(ui_chooser_active() == 0);

#ifdef PADENTER
    /* Keypad Enter on a Windows terminal accepts like Enter (B13). */
    ui_chooser_start("Devices", ITEMS, (int)(sizeof ITEMS / sizeof ITEMS[0]), capture_done, NULL);
    assert(ui_chooser_handle_key(KEY_DOWN) == 1);
    assert(ui_chooser_handle_key(PADENTER) == 1);
    assert(g_done_sel == 1);
    assert(ui_chooser_active() == 0);
#endif

    test_owned_chooser();
    test_owned_chooser_reuse();
    test_chooser_copies_its_text();
    test_render();

    printf("UI_CHOOSER_NAVIGATION: OK\n");
    return 0;
}
