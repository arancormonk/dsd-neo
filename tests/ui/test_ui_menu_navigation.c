// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression coverage for ncurses menu navigation:
 *  - leaf items should activate via Right as well as Enter
 *  - Down should skip disabled items
 *  - Home/End/PageUp/PageDown should move predictably across visible items
 *  - Left should back out of submenus and close the root overlay
 *  - the prompt-only overlay ('=', issue #671) lives exactly as long as its modal
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/app_control/config_files.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/ui/menu_core.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "menu_internal.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

#if defined(_MSC_VER) && defined(PDCURSES)
#define DSD_TEST_CURSES_EXPORT __declspec(dllexport)
#else
#define DSD_TEST_CURSES_EXPORT
#endif

volatile uint8_t exitflag = 0; // NOLINT(misc-use-internal-linkage)

static int g_action_calls = 0;
static const char* g_last_action = NULL;
static int g_fixture = 0;
static int g_prompt_active = 0;
static int g_prompt_key_calls = 0;
static int g_prompt_render_calls = 0;
static int g_help_active = 0;
static int g_help_key_calls = 0;
static int g_help_open_calls = 0;
static int g_help_render_calls = 0;
static char g_last_help[128];
static int g_chooser_active = 0;
static int g_chooser_key_calls = 0;
static int g_chooser_render_calls = 0;
static int g_rr_panel_active = 0;
static int g_rr_panel_key_calls = 0;
static int g_rr_panel_render_calls = 0;
static int g_rr_panel_close_calls = 0;
/* The Scan lists modal (issue #697): open, the keys and frames it took, and whether it ends in its next tick (its session
   ended) or on the key it handles (Esc). */
static int g_slm_active = 0;
static int g_slm_key_calls = 0;
static int g_slm_tick_calls = 0;
static int g_slm_render_calls = 0;
static int g_slm_close_calls = 0;
static int g_slm_ends_on_tick = 0;
static int g_slm_ends_on_key = 0;
/* 1: the prompt stub closes on the key it handles, as submit, cancel and Esc do. */
static int g_prompt_closes_on_key = 0;
/* 1: the prompt stub closes on the key it handles and its callback opens a follow-up prompt (a refusal reopening the
   prompt with the reason), all within that key. */
static int g_prompt_reopens_on_key = 0;
static int g_prompt_reopen_calls = 0;
/* 1: the RR panel stub ends in its next tick (an import that finished), as the panel can on its own. */
static int g_rr_panel_ends_on_tick = 0;
static int g_draw_menu_calls = 0; /* frames the tick set out to draw */
static int g_modal_opener_calls = 0;
static UiCtx* g_modal_opener_ctx = NULL;
/* Issue #672 (B9): a prompt answered by a later key runs its callback (g_prompt_answer, with the user its opener gave)
   from inside that key's handling, as the real prompt does; the callback records the pair it reads. */
static void (*g_prompt_answer)(void* user) = NULL;
static void* g_prompt_answer_user = NULL;
static const dsd_opts* g_answer_opts = NULL;
static const dsd_state* g_answer_state = NULL;
/* Issue #672 (B17): what the overlay shutdown closed, in order ("help", "prompt", "chooser"), and how many times the
   prompt's cancel opens a follow-up prompt, as a callback may. */
static char g_close_log[128];
static int g_prompt_cancel_reopens = 0;

static void
capture_action(const char* id) {
    g_action_calls++;
    g_last_action = id;
}

static void
act_root0(void* ctx) {
    (void)ctx;
    capture_action("root0");
}

static void
act_root2(void* ctx) {
    (void)ctx;
    capture_action("root2");
}

static void
act_root3(void* ctx) {
    (void)ctx;
    capture_action("root3");
}

static void
act_root4(void* ctx) {
    (void)ctx;
    capture_action("root4");
}

static void
act_root6(void* ctx) {
    (void)ctx;
    capture_action("root6");
}

static void
act_sub0(void* ctx) {
    (void)ctx;
    capture_action("sub0");
}

static void
act_sub1(void* ctx) {
    (void)ctx;
    capture_action("sub1");
}

static bool
item_disabled(const void* ctx) {
    (void)ctx;
    return false;
}

void ui_help_open(const char* help); // NOLINT(misc-use-internal-linkage) (stubbed below)

/* Openers for the prompt-only overlay: one opens a prompt, one opens help, one opens nothing (an allocation failure). */
static void
open_prompt_modal(void* ctx) {
    g_modal_opener_calls++;
    g_modal_opener_ctx = (UiCtx*)ctx;
    g_prompt_active = 1;
}

/* A deferred callback: it reads the menu's context when it runs, the frame's pair at that moment. */
static void
record_answer_pair(void* user) {
    const UiCtx* c = (const UiCtx*)user;
    g_answer_opts = c ? c->opts : NULL;
    g_answer_state = c ? c->state : NULL;
}

static void
open_answerable_prompt_modal(void* ctx) {
    g_modal_opener_calls++;
    g_modal_opener_ctx = (UiCtx*)ctx;
    g_prompt_active = 1;
    g_prompt_answer = record_answer_pair;
    g_prompt_answer_user = ctx;
}

static void
open_help_modal(void* ctx) {
    g_modal_opener_calls++;
    g_modal_opener_ctx = (UiCtx*)ctx;
    ui_help_open("one-line reason");
}

static void
open_rr_panel_modal(void* ctx) {
    g_modal_opener_calls++;
    g_modal_opener_ctx = (UiCtx*)ctx;
    g_rr_panel_active = 1;
}

static void
open_scan_list_modal(void* ctx) {
    g_modal_opener_calls++;
    g_modal_opener_ctx = (UiCtx*)ctx;
    g_slm_active = 1;
}

static void
open_nothing_modal(void* ctx) {
    g_modal_opener_calls++;
    g_modal_opener_ctx = (UiCtx*)ctx;
}

static const NcMenuItem SUB_ITEMS[] = {
    {.id = "sub0", .label = "Sub Item", .on_select = act_sub0},
    {.id = "sub1", .label = "Sub Item 2", .on_select = act_sub1},
};

static const NcMenuItem HELP_ONLY_ITEMS[] = {
    {.id = "help0", .label = "Help Only", .help = "leaf help text"},
};

static void
act_kind0(void* ctx) {
    (void)ctx;
    capture_action("kind0");
}

static void
act_kind1(void* ctx) {
    (void)ctx;
    capture_action("kind1");
}

static void
act_kind2(void* ctx) {
    (void)ctx;
    capture_action("kind2");
}

/* A status row first, a separator in the middle: neither may take the highlight. */
static const NcMenuItem KIND_ITEMS[] = {
    {.id = "status", .label = "Status row", .kind = NC_ITEM_STATUS},
    {.id = "kind0", .label = "Kind 0", .on_select = act_kind0},
    {.id = "sep", .kind = NC_ITEM_SEPARATOR},
    {.id = "kind1", .label = "Kind 1", .on_select = act_kind1},
    {.id = "kind2", .label = "Kind 2", .on_select = act_kind2},
};

/* The first and last rows opt out of the jump keys, the way the root menu's Quit
   row does. Home and End must not park the highlight one Enter away from them. */
static const NcMenuItem JUMP_GUARD_ITEMS[] = {
    {.id = "jump0", .label = "Jump 0", .on_select = act_kind0, .no_jump = true},
    {.id = "jump1", .label = "Jump 1", .on_select = act_kind1},
    {.id = "jump.sep", .kind = NC_ITEM_SEPARATOR},
    {.id = "jump2", .label = "Jump 2", .on_select = act_kind2, .no_jump = true},
};

/* A submenu whose rows all go away while it is open, as Clear avoids empties Trunking > Avoids (issue #672). */
static bool g_emptiable_rows_on = true;

static bool
emptiable_row_on(const void* ctx) {
    (void)ctx;
    return g_emptiable_rows_on;
}

static const NcMenuItem EMPTIABLE_ITEMS[] = {
    {.id = "gone0", .label = "Gone 0", .is_enabled = emptiable_row_on, .on_select = act_sub0},
    {.id = "gone.sep", .kind = NC_ITEM_SEPARATOR},
    {.id = "gone1", .label = "Gone 1", .is_enabled = emptiable_row_on, .on_select = act_sub1},
};

static const NcMenuItem EMPTIABLE_ROOT_ITEMS[] = {
    {.id = "emptiable",
     .label = "Emptiable",
     .submenu = EMPTIABLE_ITEMS,
     .submenu_len = sizeof EMPTIABLE_ITEMS / sizeof EMPTIABLE_ITEMS[0]},
    {.id = "root6", .label = "Root 6", .on_select = act_root6},
};

static const NcMenuItem ROOT_ITEMS[] = {
    {.id = "root0", .label = "Root 0", .on_select = act_root0},
    {.id = "root1", .label = "Root 1", .is_enabled = item_disabled, .on_select = act_root0},
    {.id = "root2", .label = "Root 2", .on_select = act_root2},
    {.id = "root3", .label = "Root 3", .on_select = act_root3},
    {.id = "root4", .label = "Root 4", .on_select = act_root4},
    {.id = "root5", .label = "Root 5", .submenu = SUB_ITEMS, .submenu_len = sizeof SUB_ITEMS / sizeof SUB_ITEMS[0]},
    {.id = "root6", .label = "Root 6", .on_select = act_root6},
};

static void
reset_capture(void) {
    g_action_calls = 0;
    g_last_action = NULL;
}

static void
reset_modal_capture(void) {
    g_prompt_active = 0;
    g_prompt_key_calls = 0;
    g_prompt_render_calls = 0;
    g_help_active = 0;
    g_help_key_calls = 0;
    g_help_open_calls = 0;
    g_help_render_calls = 0;
    g_last_help[0] = '\0';
    g_chooser_active = 0;
    g_chooser_key_calls = 0;
    g_chooser_render_calls = 0;
    g_rr_panel_active = 0;
    g_rr_panel_key_calls = 0;
    g_rr_panel_render_calls = 0;
    g_rr_panel_close_calls = 0;
    g_slm_active = 0;
    g_slm_key_calls = 0;
    g_slm_tick_calls = 0;
    g_slm_render_calls = 0;
    g_slm_close_calls = 0;
    g_slm_ends_on_tick = 0;
    g_slm_ends_on_key = 0;
}

static void
assert_last_action(const char* want) {
    assert(g_action_calls == 1);
    assert(g_last_action != NULL);
    assert(strcmp(g_last_action, want) == 0);
}

static char g_status[160];

void ui_statusf(const char* fmt, ...) DSD_ATTR_FORMAT(printf, 1, 2);

void
ui_statusf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)DSD_VSNPRINTF(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
}

/* The config service's look at the default file (issue #672): the menu asks it and does no I/O itself. */
static const char* g_default_missing_path = NULL;
static int g_default_missing_calls = 0;

int
dsd_app_config_default_missing(char* path, size_t path_size) { // NOLINT(misc-use-internal-linkage)
    g_default_missing_calls++;
    if (!g_default_missing_path || path_size == 0U) {
        return 0;
    }
    DSD_SNPRINTF(path, path_size, "%s", g_default_missing_path);
    return 1;
}

void
ui_menu_get_main_items(const NcMenuItem** out_items, size_t* out_n, UiCtx* ctx) { // NOLINT(misc-use-internal-linkage)
    (void)ctx;
    const NcMenuItem* items = ROOT_ITEMS;
    size_t n = sizeof ROOT_ITEMS / sizeof ROOT_ITEMS[0];
    if (g_fixture == 1) {
        items = NULL;
        n = 0;
    } else if (g_fixture == 2) {
        items = HELP_ONLY_ITEMS;
        n = sizeof HELP_ONLY_ITEMS / sizeof HELP_ONLY_ITEMS[0];
    } else if (g_fixture == 3) {
        items = KIND_ITEMS;
        n = sizeof KIND_ITEMS / sizeof KIND_ITEMS[0];
    } else if (g_fixture == 4) {
        items = JUMP_GUARD_ITEMS;
        n = sizeof JUMP_GUARD_ITEMS / sizeof JUMP_GUARD_ITEMS[0];
    } else if (g_fixture == 5) {
        items = EMPTIABLE_ROOT_ITEMS;
        n = sizeof EMPTIABLE_ROOT_ITEMS / sizeof EMPTIABLE_ROOT_ITEMS[0];
    }
    if (out_items) {
        *out_items = items;
    }
    if (out_n) {
        *out_n = n;
    }
}

int
ui_prompt_active(void) { // NOLINT(misc-use-internal-linkage)
    return g_prompt_active;
}

int
ui_prompt_handle_key(int ch) { // NOLINT(misc-use-internal-linkage)
    (void)ch;
    g_prompt_key_calls++;
    if (g_prompt_answer) {
        void (*answer)(void*) = g_prompt_answer;
        g_prompt_answer = NULL;
        g_prompt_active = 0;
        answer(g_prompt_answer_user);
        return 1;
    }
    if (g_prompt_closes_on_key || g_prompt_reopens_on_key) {
        g_prompt_active = 0;
    }
    if (g_prompt_reopens_on_key) {
        g_prompt_reopen_calls++;
        g_prompt_active = 1;
    }
    return 1;
}

void
ui_prompt_render(void) { // NOLINT(misc-use-internal-linkage)
    g_prompt_render_calls++;
}

int
ui_help_active(void) { // NOLINT(misc-use-internal-linkage)
    return g_help_active;
}

int
ui_help_handle_key(int ch) { // NOLINT(misc-use-internal-linkage)
    (void)ch;
    g_help_key_calls++;
    return 1;
}

void
ui_help_open(const char* help) { // NOLINT(misc-use-internal-linkage)
    g_help_active = 1;
    g_help_open_calls++;
    DSD_SNPRINTF(g_last_help, sizeof g_last_help, "%s", help ? help : "");
}

void
ui_help_render(void) { // NOLINT(misc-use-internal-linkage)
    g_help_render_calls++;
}

static void
log_close(const char* what) {
    const size_t used = strlen(g_close_log);
    (void)DSD_SNPRINTF(g_close_log + used, sizeof g_close_log - used, "%s%s", used ? "," : "", what);
}

void
ui_help_close(void) { // NOLINT(misc-use-internal-linkage)
    log_close("help");
    g_help_active = 0;
}

void
ui_prompt_close_all(void) { // NOLINT(misc-use-internal-linkage)
    log_close("prompt");
    if (g_prompt_active && g_prompt_cancel_reopens > 0) {
        g_prompt_cancel_reopens--;
        return; /* the cancel opened the next prompt */
    }
    g_prompt_active = 0;
}

void
ui_chooser_close(void) { // NOLINT(misc-use-internal-linkage)
    log_close("chooser");
    g_chooser_active = 0;
}

int
ui_chooser_active(void) { // NOLINT(misc-use-internal-linkage)
    return g_chooser_active;
}

int
ui_chooser_handle_key(int ch) { // NOLINT(misc-use-internal-linkage)
    (void)ch;
    g_chooser_key_calls++;
    return 1;
}

void
ui_chooser_render(void) { // NOLINT(misc-use-internal-linkage)
    g_chooser_render_calls++;
}

int
rr_panel_active(void) { // NOLINT(misc-use-internal-linkage)
    return g_rr_panel_active;
}

int
rr_panel_handle_key(int ch) { // NOLINT(misc-use-internal-linkage)
    (void)ch;
    g_rr_panel_key_calls++;
    return 1;
}

void
rr_panel_render(void) { // NOLINT(misc-use-internal-linkage)
    g_rr_panel_render_calls++;
}

void
rr_panel_tick(const dsd_opts* opts, const dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    if (g_rr_panel_ends_on_tick) {
        g_rr_panel_active = 0;
    }
}

void
rr_panel_close(void) { // NOLINT(misc-use-internal-linkage)
    g_rr_panel_close_calls++;
}

int
scan_list_modal_active(void) { // NOLINT(misc-use-internal-linkage)
    return g_slm_active;
}

int
scan_list_modal_handle_key(int ch) { // NOLINT(misc-use-internal-linkage)
    (void)ch;
    g_slm_key_calls++;
    if (g_slm_ends_on_key) {
        g_slm_active = 0;
    }
    return 1;
}

void
scan_list_modal_render(void) { // NOLINT(misc-use-internal-linkage)
    g_slm_render_calls++;
}

void
scan_list_modal_tick(const dsd_opts* opts, const dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    g_slm_tick_calls++;
    if (g_slm_ends_on_tick) {
        g_slm_active = 0;
    }
}

void
scan_list_modal_close(void) { // NOLINT(misc-use-internal-linkage)
    g_slm_close_calls++;
    g_slm_active = 0;
}

int
ui_submenu_has_visible(const NcMenuItem* items, size_t n, const void* ctx) {
    if (!items || n == 0) {
        return 0;
    }
    /* Selectable, as menu_render.c counts: a separator alone keeps no submenu on screen. */
    for (size_t i = 0; i < n; i++) {
        if (ui_is_selectable(&items[i], ctx)) {
            return 1;
        }
    }
    return 0;
}

int
ui_is_enabled(const NcMenuItem* it, const void* ctx) {
    if (!it) {
        return 0;
    }
    if (it->is_enabled) {
        return it->is_enabled(ctx) ? 1 : 0;
    }
    if (it->submenu && it->submenu_len > 0) {
        return ui_submenu_has_visible(it->submenu, it->submenu_len, ctx);
    }
    return 1;
}

int
ui_is_selectable(const NcMenuItem* it, const void* ctx) {
    return (it && it->kind == NC_ITEM_ACTION && ui_is_enabled(it, ctx)) ? 1 : 0;
}

int
ui_next_selectable(const NcMenuItem* items, size_t n, const void* ctx, int from, int dir) {
    if (!items || n == 0) {
        return 0;
    }
    int idx = from;
    for (size_t i = 0; i < n; i++) {
        idx = (idx + ((dir > 0) ? 1 : -1) + (int)n) % (int)n;
        if (ui_is_selectable(&items[idx], ctx)) {
            return idx;
        }
    }
    return from;
}

int
ui_visible_index_for_item(const NcMenuItem* items, size_t n, const void* ctx, int idx) {
    if (!items || n == 0 || idx < 0 || idx >= (int)n) {
        return 0;
    }
    int vis_pos = 0;
    for (size_t i = 0; i < n; i++) {
        if (!ui_is_enabled(&items[i], ctx)) {
            continue;
        }
        if ((int)i == idx) {
            return vis_pos;
        }
        vis_pos++;
    }
    return 0;
}

int
ui_visible_count_and_maxlab(const NcMenuItem* items, size_t n, const void* ctx, int* out_maxlab) {
    int vis = 0;
    int maxlab = 0;
    for (size_t i = 0; i < n; i++) {
        if (!ui_is_enabled(&items[i], ctx)) {
            continue;
        }
        const char* label = items[i].label ? items[i].label : items[i].id;
        int len = (int)strlen(label);
        if (len > maxlab) {
            maxlab = len;
        }
        vis++;
    }
    if (out_maxlab) {
        *out_maxlab = maxlab;
    }
    return vis;
}

void
ui_overlay_layout(UiMenuFrame* f, const void* ctx) {
    (void)ctx;
    assert(f != NULL);
    f->h = 11; // 4 visible rows -> page step of 3
    f->w = 40;
    f->y = 0;
    f->x = 0;
}

/* Only a frame being drawn gets here (ui_menu_tick()); the stubbed window stays NULL, so ui_draw_menu() never runs. */
void
ui_overlay_ensure_window(UiMenuFrame* f) {
    (void)f;
    g_draw_menu_calls++;
}

void
ui_overlay_recreate_if_needed(UiMenuFrame* f) {
    (void)f;
}

void
ui_draw_menu(WINDOW* win, const NcMenuItem* items, size_t n, int hi, int* top_io, const char* title, const void* ctx) {
    (void)win;
    (void)items;
    (void)n;
    (void)hi;
    (void)top_io;
    (void)title;
    (void)ctx;
}

DSD_TEST_CURSES_EXPORT int
delwin(WINDOW* win) {
    (void)win;
    return OK;
}

DSD_TEST_CURSES_EXPORT int
resize_term(int lines, int columns) {
    (void)lines;
    (void)columns;
    return OK;
}

/* Issue #672 (B17): exit closes every overlay - help, the prompt (whose cancel may open a chooser), the chooser and every
   menu frame - and resets the overlay, so the next start opens on a fresh root menu. */
static void
test_overlays_shutdown_and_restart(const dsd_opts* opts, const dsd_state* state) {
    g_fixture = 0;
    reset_capture();
    reset_modal_capture();
    ui_menu_open_async(opts, state);
    for (int i = 0; i < 4; i++) { /* root0 -> root2 (root1 is disabled) -> ... -> root5 */
        assert(ui_menu_handle_key(KEY_DOWN, opts, state) == 1);
    }
    assert(ui_menu_handle_key('\n', opts, state) == 1); /* into root5's submenu: two frames */
    assert(ui_menu_handle_key('\n', opts, state) == 1);
    assert_last_action("sub0");
    g_help_active = 1;
    g_prompt_active = 1;
    g_chooser_active = 1;
    g_prompt_cancel_reopens = 1;
    g_close_log[0] = '\0';
    ui_overlays_shutdown();
    /* The prompt's cancel opened a follow-up prompt; a second pass closes it. */
    assert(strcmp(g_close_log, "help,prompt,chooser,prompt,chooser") == 0);
    assert(!g_help_active && !g_prompt_active && !g_chooser_active);
    assert(ui_menu_is_open() == 0);
    assert(g_rr_panel_close_calls == 1); /* the frames' own close, after rr_panel_shutdown() has run */

    /* Nothing open: help is closed (idempotent) and nothing else runs. */
    g_close_log[0] = '\0';
    ui_overlays_shutdown();
    assert(strcmp(g_close_log, "help") == 0);
    assert(ui_menu_is_open() == 0);

    /* Restart: the root menu opens fresh at depth one, so Left closes it rather than popping a stale submenu. */
    reset_capture();
    ui_menu_open_async(opts, state);
    assert(ui_menu_is_open() == 1);
    assert(ui_menu_handle_key('\n', opts, state) == 1);
    assert_last_action("root0");
    assert(ui_menu_handle_key(KEY_LEFT, opts, state) == 1);
    assert(ui_menu_is_open() == 0);

    /* A prompt-only overlay shut down with its modal open does not survive as a modal-only overlay either. */
    reset_modal_capture();
    ui_menu_open_modal_async(opts, state, open_prompt_modal);
    assert(ui_menu_is_open() == 1);
    ui_overlays_shutdown();
    assert(ui_menu_is_open() == 0);
    ui_menu_open_async(opts, state);
    ui_menu_tick(opts, state);
    assert(ui_menu_is_open() == 1); /* a full menu now, not closed as an empty modal-only frame */
    assert(ui_menu_handle_key(27, opts, state) == 1);
    assert(ui_menu_is_open() == 0);
}

int
main(void) {
    /* The frame's pair: real objects (only their addresses are compared), on the heap as dsd_opts/dsd_state go. */
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* next_opts = (dsd_opts*)calloc(1, sizeof(*next_opts));
    dsd_state* next_state = (dsd_state*)calloc(1, sizeof(*next_state));
    assert(opts && state && next_opts && next_state);

    g_fixture = 1;
    ui_menu_open_async(opts, state);
    assert(ui_menu_is_open() == 0);

    g_fixture = 0;
    reset_capture();
    ui_menu_open_async(opts, state);
    assert(ui_menu_is_open() == 1);

    assert(ui_menu_handle_key(KEY_RIGHT, opts, state) == 1);
    assert_last_action("root0");

    reset_capture();
    assert(ui_menu_handle_key(KEY_DOWN, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root2");

#ifdef PADENTER
    /* Keypad Enter on a Windows terminal activates like Enter (B13). */
    reset_capture();
    assert(ui_menu_handle_key(PADENTER, opts, state) == 1);
    assert_last_action("root2");
    /* Shift+keypad Enter, the main screen's alias of '=', is Enter inside the menu (issue #671). */
    reset_capture();
    assert(ui_menu_handle_key(SHF_PADENTER, opts, state) == 1);
    assert_last_action("root2");
#endif

    assert(ui_menu_handle_key(KEY_LEFT, opts, state) == 1);
    assert(ui_menu_is_open() == 0);
    ui_menu_open_async(opts, state);
    assert(ui_menu_is_open() == 1);

    reset_capture();
    assert(ui_menu_handle_key(KEY_PPAGE, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root0");

    reset_capture();
    assert(ui_menu_handle_key(KEY_NPAGE, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root4");

    reset_capture();
    assert(ui_menu_handle_key(KEY_HOME, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root0");

    reset_capture();
    assert(ui_menu_handle_key(KEY_END, opts, state) == 1);
    assert(ui_menu_handle_key(KEY_NPAGE, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root6");

    reset_capture();
    assert(ui_menu_handle_key(KEY_END, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root6");

    reset_capture();
    assert(ui_menu_handle_key(KEY_PPAGE, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root3");

    reset_capture();
    assert(ui_menu_handle_key(KEY_DOWN, opts, state) == 1);
    assert(ui_menu_handle_key(KEY_DOWN, opts, state) == 1);
    assert(ui_menu_handle_key(KEY_RIGHT, opts, state) == 1);
    assert(g_action_calls == 0);
    assert(ui_menu_is_open() == 1);

    reset_capture();
    assert(ui_menu_handle_key(KEY_PPAGE, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("sub0");

    reset_capture();
    assert(ui_menu_handle_key(KEY_END, opts, state) == 1);
    assert(ui_menu_handle_key(KEY_NPAGE, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("sub1");

    reset_capture();
    assert(ui_menu_handle_key(KEY_HOME, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("sub0");

    reset_capture();
    assert(ui_menu_handle_key(KEY_LEFT, opts, state) == 1);
    assert(ui_menu_is_open() == 1);
    assert(ui_menu_handle_key(KEY_END, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root6");

    assert(ui_menu_handle_key(KEY_LEFT, opts, state) == 1);
    assert(ui_menu_is_open() == 0);

    g_fixture = 2;
    reset_capture();
    reset_modal_capture();
    ui_menu_open_async(opts, state);
    assert(ui_menu_is_open() == 1);
    assert(ui_menu_handle_key(KEY_RIGHT, opts, state) == 1);
    assert(g_action_calls == 0);
    assert(g_help_open_calls == 1);
    assert(strcmp(g_last_help, "leaf help text") == 0);
    assert(g_help_active == 1);
    assert(ui_menu_handle_key(KEY_DOWN, opts, state) == 1);
    assert(g_help_key_calls == 1);
    ui_menu_tick(opts, state);
    assert(g_help_render_calls == 1);
    g_help_active = 0;

    g_prompt_active = 1;
    assert(ui_menu_handle_key(KEY_DOWN, opts, state) == 1);
    assert(g_prompt_key_calls == 1);
    ui_menu_tick(opts, state);
    assert(g_prompt_render_calls == 1);
    g_prompt_active = 0;

    g_chooser_active = 1;
    assert(ui_menu_handle_key(KEY_UP, opts, state) == 1);
    assert(g_chooser_key_calls == 1);
    ui_menu_tick(opts, state);
    assert(g_chooser_render_calls == 1);
    g_chooser_active = 0;

    g_rr_panel_active = 1;
    assert(ui_menu_handle_key(KEY_UP, opts, state) == 1);
    assert(g_rr_panel_key_calls == 1);
    ui_menu_tick(opts, state);
    assert(g_rr_panel_render_calls == 1);
    g_rr_panel_active = 0;

    assert(g_rr_panel_close_calls == 0);
    assert(ui_menu_handle_key(KEY_LEFT, opts, state) == 1);
    assert(ui_menu_is_open() == 0);
    /* Closing the menu closes the RadioReference panel with it. */
    assert(g_rr_panel_close_calls == 1);

    /* Status rows and separators are drawn but never highlighted or activated. */
    g_fixture = 3;
    reset_capture();
    reset_modal_capture();
    ui_menu_open_async(opts, state);
    assert(ui_menu_is_open() == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("kind0");

    reset_capture();
    assert(ui_menu_handle_key(KEY_DOWN, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("kind1");

    reset_capture();
    assert(ui_menu_handle_key(KEY_UP, opts, state) == 1);
    assert(ui_menu_handle_key(KEY_UP, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("kind2");

    reset_capture();
    assert(ui_menu_handle_key(KEY_HOME, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("kind0");

    reset_capture();
    assert(ui_menu_handle_key(KEY_END, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("kind2");

    reset_capture();
    assert(ui_menu_handle_key(KEY_PPAGE, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("kind0");

    /* 'q' quits the program from the main screen, so inside the menu it must be inert. */
    assert(ui_menu_handle_key('q', opts, state) == 0);
    assert(ui_menu_is_open() == 1);
    assert(ui_menu_handle_key('Q', opts, state) == 0);
    assert(ui_menu_is_open() == 1);
    assert(ui_menu_handle_key(27, opts, state) == 1);
    assert(ui_menu_is_open() == 0);

    /* A row that opted out of the jump keys is reachable only by stepping onto it. */
    g_fixture = 4;
    reset_capture();
    reset_modal_capture();
    ui_menu_open_async(opts, state);
    assert(ui_menu_is_open() == 1);

    /* The frame does not even open on the guarded first row. */
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("kind1");

    reset_capture();
    assert(ui_menu_handle_key(KEY_END, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("kind1");

    reset_capture();
    assert(ui_menu_handle_key(KEY_HOME, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("kind1");

    /* The guard is against a single keystroke, not against the row: arrows still
       land on it, across the separator and around the wrap. */
    reset_capture();
    assert(ui_menu_handle_key(KEY_DOWN, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("kind2");

    reset_capture();
    assert(ui_menu_handle_key(KEY_DOWN, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("kind0");

    assert(ui_menu_handle_key(27, opts, state) == 1);
    assert(ui_menu_is_open() == 0);

    /* The prompt-only overlay ('=', issue #671): the opener runs at once with the stable context, keys go to its
       prompt, no frame is drawn under it, and it closes in the same call that closes the prompt. */
    g_fixture = 0;
    reset_capture();
    reset_modal_capture();
    g_draw_menu_calls = 0;
    g_modal_opener_calls = 0;
    g_modal_opener_ctx = NULL;
    ui_menu_open_modal_async(opts, state, open_prompt_modal);
    assert(g_modal_opener_calls == 1);
    assert(g_modal_opener_ctx != NULL);
    assert(g_modal_opener_ctx->opts == opts && g_modal_opener_ctx->state == state);
    assert(ui_menu_is_open() == 1);
    ui_menu_tick(opts, state);
    assert(g_prompt_render_calls == 1);
    assert(g_draw_menu_calls == 0);
    /* A key the prompt keeps (typing) leaves it open; the menu's own keys never reach a frame. */
    assert(ui_menu_handle_key(KEY_DOWN, opts, state) == 1);
    assert(g_prompt_key_calls == 1);
    assert(ui_menu_is_open() == 1);
    assert(g_action_calls == 0);
    /* Submit, cancel or Esc closes the prompt, and the overlay with it, in that same call. */
    g_prompt_closes_on_key = 1;
    assert(ui_menu_handle_key('\n', opts, state) == 1);
    assert(g_prompt_active == 0);
    assert(ui_menu_is_open() == 0);
    assert(g_action_calls == 0);
    ui_menu_tick(opts, state);
    assert(g_draw_menu_calls == 0);
    g_prompt_closes_on_key = 0;

    /* A callback that opens a follow-up modal (a refusal reopening the prompt) keeps the overlay: the key closes the
       prompt and its callback opens another before the key returns, and the next tick draws that prompt alone. */
    reset_modal_capture();
    ui_menu_open_modal_async(opts, state, open_prompt_modal);
    g_prompt_reopens_on_key = 1;
    g_prompt_reopen_calls = 0;
    assert(ui_menu_handle_key('\n', opts, state) == 1);
    g_prompt_reopens_on_key = 0;
    assert(g_prompt_reopen_calls == 1 && g_prompt_active == 1);
    assert(ui_menu_is_open() == 1);
    g_prompt_render_calls = 0;
    ui_menu_tick(opts, state);
    assert(g_prompt_render_calls == 1);
    assert(g_draw_menu_calls == 0);
    /* A prompt closed from elsewhere is noticed at the next tick, before anything is drawn. */
    g_prompt_active = 0;
    ui_menu_tick(opts, state);
    assert(ui_menu_is_open() == 0);
    assert(g_draw_menu_calls == 0);

    /* Help (the reason '=' cannot tune here) lives the same way. */
    reset_modal_capture();
    ui_menu_open_modal_async(opts, state, open_help_modal);
    assert(g_help_open_calls == 1);
    assert(strcmp(g_last_help, "one-line reason") == 0);
    assert(ui_menu_is_open() == 1);
    ui_menu_tick(opts, state);
    assert(g_help_render_calls == 1);
    assert(g_draw_menu_calls == 0);
    g_help_active = 0; /* the help stub's close key */
    assert(ui_menu_handle_key('\n', opts, state) == 1);
    assert(ui_menu_is_open() == 0);

    /* Final review, Minor 7: a modal-only overlay whose RR panel ends in its own tick closes in that tick, before the
       frame it held nothing else for would be drawn. */
    reset_modal_capture();
    ui_menu_open_modal_async(opts, state, open_rr_panel_modal);
    assert(ui_menu_is_open() == 1);
    ui_menu_tick(opts, state);
    assert(g_rr_panel_render_calls == 1 && g_draw_menu_calls == 0);
    g_rr_panel_ends_on_tick = 1;
    ui_menu_tick(opts, state);
    g_rr_panel_ends_on_tick = 0;
    assert(ui_menu_is_open() == 0);
    assert(g_draw_menu_calls == 0);
    assert(g_rr_panel_render_calls == 1);

    /* Issue #697: the Scan lists modal over the main screen (`#`) takes the keys and the frames, and the overlay goes
       with it, whether Esc closes it or its tick does (the session it opened on ended). */
    reset_modal_capture();
    g_draw_menu_calls = 0;
    ui_menu_open_modal_async(opts, state, open_scan_list_modal);
    assert(ui_menu_is_open() == 1);
    assert(ui_menu_handle_key(KEY_DOWN, opts, state) == 1);
    assert(g_slm_key_calls == 1 && g_action_calls == 0);
    ui_menu_tick(opts, state);
    assert(g_slm_tick_calls == 1 && g_slm_render_calls == 1 && g_draw_menu_calls == 0);
    g_slm_ends_on_key = 1;
    assert(ui_menu_handle_key(27, opts, state) == 1);
    assert(ui_menu_is_open() == 0);
    reset_modal_capture();
    ui_menu_open_modal_async(opts, state, open_scan_list_modal);
    g_slm_ends_on_tick = 1;
    ui_menu_tick(opts, state);
    assert(ui_menu_is_open() == 0);
    assert(g_slm_render_calls == 0 && g_draw_menu_calls == 0);
    /* Over the menu, the modal draws in place of the menu, and closing the menu closes the modal with it. */
    reset_capture();
    reset_modal_capture();
    ui_menu_open_async(opts, state);
    g_draw_menu_calls = 0;
    g_slm_active = 1;
    ui_menu_tick(opts, state);
    assert(g_slm_render_calls == 1 && g_draw_menu_calls == 0);
    g_slm_active = 0;
    ui_menu_tick(opts, state);
    assert(g_draw_menu_calls == 1);
    g_slm_active = 1;
    g_slm_close_calls = 0;
    ui_overlays_shutdown();
    assert(ui_menu_is_open() == 0 && g_slm_close_calls == 1 && g_slm_active == 0);
    g_draw_menu_calls = 0;

    /* An opener that opens nothing leaves no overlay behind, and no empty frame is ever drawn. */
    reset_modal_capture();
    g_modal_opener_calls = 0;
    ui_menu_open_modal_async(opts, state, open_nothing_modal);
    assert(g_modal_opener_calls == 1);
    assert(ui_menu_is_open() == 0);
    ui_menu_tick(opts, state);
    assert(g_draw_menu_calls == 0);

    /* The full menu opened after a prompt-only one is an ordinary menu: closing its own prompt keeps it open. */
    reset_capture();
    reset_modal_capture();
    ui_menu_open_modal_async(opts, state, open_prompt_modal);
    g_prompt_closes_on_key = 1;
    assert(ui_menu_handle_key(27, opts, state) == 1);
    assert(ui_menu_is_open() == 0);
    ui_menu_open_async(opts, state);
    assert(ui_menu_is_open() == 1);
    g_prompt_active = 1;
    assert(ui_menu_handle_key('\n', opts, state) == 1);
    assert(g_prompt_active == 0);
    assert(ui_menu_is_open() == 1);
    ui_menu_tick(opts, state);
    assert(g_draw_menu_calls == 1);
    g_prompt_closes_on_key = 0;
    assert(ui_menu_handle_key(27, opts, state) == 1);
    assert(ui_menu_is_open() == 0);

    /* Issue #672: opening the menu asks the config service whether the default config is still to be written and
       says so; with one on disk (or no default path) it says nothing. */
    ui_menu_handle_key(27, opts, state);
    g_fixture = 0;
    g_status[0] = '\0';
    g_default_missing_calls = 0;
    g_default_missing_path = "/home/u/.config/dsd-neo/config.ini";
    ui_menu_open_async(opts, state);
    assert(ui_menu_is_open() == 1);
    assert(g_default_missing_calls == 1);
    assert(strcmp(g_status, "No default config; use Config menu to save to /home/u/.config/dsd-neo/config.ini") == 0);
    assert(ui_menu_handle_key(27, opts, state) == 1);
    g_status[0] = '\0';
    g_default_missing_path = NULL;
    ui_menu_open_async(opts, state);
    assert(g_default_missing_calls == 2);
    assert(g_status[0] == '\0');
    assert(ui_menu_handle_key(27, opts, state) == 1);
    assert(ui_menu_is_open() == 0);

    /* Issue #672 (B9): the menu reads only the frame's published pair. Before the first publication (no state snapshot)
       neither the menu nor a prompt-only overlay opens. */
    reset_capture();
    reset_modal_capture();
    g_modal_opener_calls = 0;
    ui_menu_open_async(opts, NULL);
    assert(ui_menu_is_open() == 0);
    ui_menu_open_async(NULL, state);
    assert(ui_menu_is_open() == 0);
    ui_menu_open_modal_async(opts, NULL, open_prompt_modal);
    assert(g_modal_opener_calls == 0);
    assert(ui_menu_is_open() == 0);

    /* A prompt answered frames after it opened, across a publication and a decoder restart, runs its callback with the
       pair the frame it is answered in handed the menu, never the opener frame's. */
    reset_modal_capture();
    g_answer_opts = NULL;
    g_answer_state = NULL;
    ui_menu_open_modal_async(opts, state, open_answerable_prompt_modal);
    assert(g_modal_opener_ctx != NULL && g_modal_opener_ctx->opts == opts && g_modal_opener_ctx->state == state);
    ui_menu_tick(next_opts, next_state);
    assert(ui_menu_handle_key('\n', next_opts, next_state) == 1);
    assert(g_answer_opts == next_opts && g_answer_state == next_state);
    assert(ui_menu_is_open() == 0);

    test_overlays_shutdown_and_restart(opts, state);

    /* A submenu emptied while open (issue #672: Clear avoids outside a running scan hides all three Avoids rows) goes
       back to its parent at the next tick, rather than drawing a frame with only its separator and nothing to rest
       on; the parent's highlight moves off the row that now hides. */
    g_fixture = 5;
    g_emptiable_rows_on = true;
    reset_capture();
    reset_modal_capture();
    ui_menu_open_async(opts, state);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    ui_menu_tick(opts, state);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("sub0");
    g_emptiable_rows_on = false;
    ui_menu_tick(opts, state);
    assert(ui_menu_is_open() == 1);
    reset_capture();
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root6");
    assert(ui_menu_handle_key(KEY_LEFT, opts, state) == 1);
    assert(ui_menu_is_open() == 0);
    g_emptiable_rows_on = true;
    g_fixture = 0;

    free(next_state);
    free(next_opts);
    free(state);
    free(opts);
    printf("UI_MENU_NAVIGATION: OK\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
