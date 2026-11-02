// SPDX-License-Identifier: GPL-3.0-or-later
// NOLINTBEGIN(misc-use-internal-linkage)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The terminal mouse wheel's admission and capability (issue #666).
 *
 * UI_MOUSE: which terminals ncurses reads through its xterm mouse driver, the one the wheel capture can switch off
 * and on without clearing the parse mask (ui_mouse_xterm_admitted(), a pure function of TERM, NCURSES_GPM_TERMS and
 * the terminfo kmous).
 *
 * UI_MOUSE_NO_WHEEL: the same source built against the fake tests/ui/curses.h, which has no KEY_MOUSE and no button
 * 5, as an ncurses with mouse version 1 (the macOS SDK's 5.7) is: the wheel is built out, so the Display row
 * "Mouse wheel steps channels" is hidden and the capture never turns on.
 */

#include <assert.h>
#include <dsd-neo/ui/menu_core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "menu_actions.h" // IWYU pragma: keep (prototypes for the action stubs)
#include "menu_items.h"
#include "menu_labels.h" // IWYU pragma: keep (prototypes for the label and predicate stubs)
#include "test_ui_menu_tree_audit_stubs.h"
#include "ui_keys.h"
#include "ui_mouse.h"

#if UI_KEY_HAVE_WHEEL
#define TEST_NAME "UI_MOUSE"
#else
#define TEST_NAME "UI_MOUSE_NO_WHEEL"
#endif

/* menu_items.c is linked for its Display rows; every handler but the wheel predicate is a stub. */
#define DEFINE_ACTION_STUB(name)                                                                                       \
    void name(void* v) { (void)v; }
AUDIT_ACTIONS(DEFINE_ACTION_STUB)
#undef DEFINE_ACTION_STUB

#define DEFINE_LABEL_STUB(name)                                                                                        \
    const char* name(const void* v, char* b, size_t n) {                                                               \
        (void)v;                                                                                                       \
        if (n > 0) {                                                                                                   \
            b[0] = '\0';                                                                                               \
        }                                                                                                              \
        return b;                                                                                                      \
    }
AUDIT_LABELS(DEFINE_LABEL_STUB)
#undef DEFINE_LABEL_STUB

#define DEFINE_PREDICATE_STUB(name)                                                                                    \
    bool name(const void* ctx) {                                                                                       \
        (void)ctx;                                                                                                     \
        return true;                                                                                                   \
    }
AUDIT_PREDICATES(DEFINE_PREDICATE_STUB)
#undef DEFINE_PREDICATE_STUB

/* ncurses picks GPM when NCURSES_GPM_TERMS names TERM, or with it unset when TERM contains "linux"; the wheel takes
   only terminals where neither can happen and kmous is an ESC [ report. */
static void
test_xterm_driver_admission(void) {
    static const char sgr_kmous[] = "\033[<";
    static const char x10_kmous[] = "\033[M";

    /* Terminal emulators: SGR (xterm, VTE, Konsole, Windows Terminal over ssh) and X10 (tmux, screen). */
    assert(ui_mouse_xterm_admitted("xterm-256color", NULL, sgr_kmous) == 1);
    assert(ui_mouse_xterm_admitted("tmux-256color", NULL, x10_kmous) == 1);
    assert(ui_mouse_xterm_admitted(NULL, NULL, sgr_kmous) == 1);

    /* The Linux console, and anything else ncurses would hand to GPM. */
    assert(ui_mouse_xterm_admitted("linux", NULL, x10_kmous) == 0);
    assert(ui_mouse_xterm_admitted("linux-16color", NULL, x10_kmous) == 0);
    assert(ui_mouse_xterm_admitted("xterm-linux", NULL, sgr_kmous) == 0);

    /* Any NCURSES_GPM_TERMS, even one that does not name TERM, or an empty one. */
    assert(ui_mouse_xterm_admitted("xterm-256color", "linux", sgr_kmous) == 0);
    assert(ui_mouse_xterm_admitted("xterm-256color", "xterm-256color", sgr_kmous) == 0);
    assert(ui_mouse_xterm_admitted("xterm-256color", "", sgr_kmous) == 0);

    /* No kmous, or one that is not an ESC [ report. */
    assert(ui_mouse_xterm_admitted("xterm-256color", NULL, NULL) == 0);
    assert(ui_mouse_xterm_admitted("xterm-256color", NULL, "") == 0);
    assert(ui_mouse_xterm_admitted("xterm-256color", NULL, "\033") == 0);
    assert(ui_mouse_xterm_admitted("xterm-256color", NULL, "\033OM") == 0);
    assert(ui_mouse_xterm_admitted("xterm-256color", NULL, "[M") == 0);
}

static const NcMenuItem*
find_display_row(const char* id) {
    for (size_t i = 0; i < DISPLAY_MENU_ITEMS_LEN; i++) {
        if (DISPLAY_MENU_ITEMS[i].id && strcmp(DISPLAY_MENU_ITEMS[i].id, id) == 0) {
            return &DISPLAY_MENU_ITEMS[i];
        }
    }
    return NULL;
}

/* The row asks the capability; a build without the wheel answers no, so the menu omits the row (menu_render.c
   draws only rows whose is_enabled is unset or true) and the capture stays off. */
static void
test_wheel_row_follows_capability(void) {
    const NcMenuItem* row = find_display_row("disp.mouse_wheel");
    assert(row != NULL);
    assert(row->is_enabled == ui_mouse_wheel_available);
#if !UI_KEY_HAVE_WHEEL
    assert(row->is_enabled(NULL) == false);
    ui_mouse_capture_set(1);
    assert(ui_mouse_capture_is_on() == 0);
    ui_mouse_capture_close();
    assert(ui_mouse_capture_is_on() == 0);
#endif
}

int
main(void) {
    test_xterm_driver_admission();
    test_wheel_row_follows_capability();
    printf("%s: OK\n", TEST_NAME);
    return 0;
}

// NOLINTEND(misc-use-internal-linkage)
