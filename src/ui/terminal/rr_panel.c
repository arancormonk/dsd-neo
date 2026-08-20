// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Presenter for the modal RadioReference import panel.
 *
 * Owns exactly one RrWizardCore for the whole app session and supplies it with
 * curses widgets through RrWizardHooks. Everything here runs on the UI thread
 * and writes neither dsd_opts nor dsd_state: the account write path is a
 * command posted to the decoder thread.
 *
 * rr_panel_render() draws nothing yet - Stage 10 fills it. Until then the
 * credential prompts, the search-mode chooser, the browse choosers and the
 * results chooser all render through menu_prompts.c, which keeps priority over
 * the panel in both the key chain and the render chain.
 */

#include <curses.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/platform/curses_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <dsd-neo/ui/keymap.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/ui/menu_core.h"
#include "menu_internal.h"
#include "menu_prompts.h"
#include "rr_panel.h"
#include "rr_panel_format.h"
#include "rr_wizard_core.h"

#define RR_PANEL_CHROME_ROWS  12
#define RR_PANEL_WARNING_ROWS 3
#define RR_PANEL_MIN_H        13 /* chrome + one site row */
#define RR_PANEL_MIN_W        62 /* footer line 2 is 57 chars + 4 columns of frame + 1 */
#define RR_PANEL_MAX_W        100
#define RR_PANEL_BOX_MIN_W    36 /* fetching box */
#define RR_PANEL_ERROR_W      72
#define RR_PANEL_CP_ERROR     2 /* red under PRETTY_COLORS, white/black otherwise */
#define RR_PANEL_CP_WARN      1 /* yellow under PRETTY_COLORS, white/black otherwise */

typedef struct {
    RrWizardCore* core; /* one per app session; survives rr_panel_close() */
    WINDOW* win;
    int win_h;
    int win_w;
    int win_y;
    int win_x;
    int sel;
    int top;
    int page_rows;
    int active;
} RrPanel;

static RrPanel g_rr_panel;

int
rr_panel_active(void) {
    return g_rr_panel.active;
}

static void
rr_panel_release_window(void) {
    if (g_rr_panel.win != NULL) {
        delwin(g_rr_panel.win);
        g_rr_panel.win = NULL;
    }
}

/* ---- RrWizardHooks trampolines ------------------------------------------- */

static void
rr_panel_on_prompt_done(void* user, const char* text) {
    RrPanel* p = (RrPanel*)user;
    if (!p || !p->core) {
        return;
    }
    rr_wizard_core_on_prompt_done(p->core, text);
}

static void
rr_panel_on_chooser_done(void* user, int selected) {
    RrPanel* p = (RrPanel*)user;
    if (!p || !p->core) {
        return;
    }
    rr_wizard_core_on_chooser_done(p->core, selected);
}

static void
rr_hook_open_string(void* user, const char* title, const char* prefill, size_t cap) {
    ui_prompt_open_string_async(title, prefill, cap, rr_panel_on_prompt_done, user);
}

static void
rr_hook_open_secret(void* user, const char* title, size_t cap) {
    ui_prompt_open_secret_async(title, cap, rr_panel_on_prompt_done, user);
}

static void
rr_hook_open_chooser(void* user, const char* title, const char* const* items, int count) {
    ui_chooser_start(title, items, count, rr_panel_on_chooser_done, user);
}

static void
rr_hook_panel_changed(void* user) {
    (void)user;
    dsd_app_request_redraw();
}

static void
rr_hook_status(void* user, const char* text) {
    (void)user;
    if (text == NULL) {
        return;
    }
    ui_statusf("%s", text);
}

static int
rr_hook_apply(void* user, const dsd_app_rr_apply_payload* payload) {
    (void)user;
    return dsd_app_command_set_rr_apply(payload);
}

static int
rr_hook_post_import_path(void* user, int cmd_id, const char* path) {
    (void)user;
    return dsd_app_command_set_string(cmd_id, path);
}

static int
rr_hook_account_changed(void* user, const dsd_app_rr_account_payload* account) {
    (void)user;
    return dsd_app_command_set_rr_account(account);
}

/* ---- Lifecycle ----------------------------------------------------------- */

static RrWizardCore*
rr_panel_ensure_core(void) {
    static const RrWizardHooks hooks = {
        .open_string = rr_hook_open_string,
        .open_secret = rr_hook_open_secret,
        .open_chooser = rr_hook_open_chooser,
        .panel_changed = rr_hook_panel_changed,
        .status = rr_hook_status,
        .apply = rr_hook_apply,
        .post_import_path = rr_hook_post_import_path,
        .account_changed = rr_hook_account_changed,
    };
    if (g_rr_panel.core == NULL) {
        g_rr_panel.core = rr_wizard_core_create(&hooks, &g_rr_panel);
    }
    return g_rr_panel.core;
}

/* ---- Refresh: picking a stored import ------------------------------------ */

/*
 * ui_chooser_start() copies NOTHING - it keeps the title, the item array and
 * every string in it by pointer until the chooser closes (menu_prompts.c,
 * identifier ui_chooser_start). Building these in locals would leave the chooser
 * pointing at dead stack, so the whole list is one heap allocation that the done
 * callback frees unconditionally.
 */
#define RR_REFRESH_LIST_MAX 128
#define RR_REFRESH_DIR_MAX  1024
#define RR_REFRESH_PATH_MAX 1024

typedef struct {
    char* label; /* heap: what the chooser row shows */
    char* path;  /* heap: absolute path of the stored CSV */
} RrRefreshEntry;

typedef struct {
    RrRefreshEntry entries[RR_REFRESH_LIST_MAX];
    const char* items[RR_REFRESH_LIST_MAX]; /* the borrowed view the chooser holds */
    char dir[RR_REFRESH_DIR_MAX];
    int count;
} RrRefreshList;

static void
rr_refresh_list_free(RrRefreshList* list) {
    if (list == NULL) {
        return;
    }
    for (int i = 0; i < list->count; i++) {
        free(list->entries[i].label);
        free(list->entries[i].path);
    }
    free(list);
}

/** @brief dsd_dir_list callback: keep every ".csv" that has a readable sidecar. */
static int
rr_refresh_collect(const char* name, void* user) {
    RrRefreshList* list = (RrRefreshList*)user;
    dsd_rr_provenance prov;
    char path[RR_REFRESH_PATH_MAX];
    char label[192];

    if (list->count >= RR_REFRESH_LIST_MAX) {
        return 1; /* non-zero stops the walk */
    }
    const size_t len = (name != NULL) ? strlen(name) : 0U;
    if (len < 5U || strcmp(name + (len - 4U), ".csv") != 0) {
        return 0;
    }
    const int n = DSD_SNPRINTF(path, sizeof path, "%s%c%s", list->dir, RR_PATH_SEP, name);
    if (n <= 0 || (size_t)n >= sizeof path) {
        /* Truncation is not "shorten it and carry on": the shortened path could
           name a different file, and the sidecar read below would be answering
           about that one. */
        return 0;
    }
    DSD_MEMSET(&prov, 0, sizeof prov);
    if (dsd_rr_provenance_read(path, &prov) != 0) {
        /* No sidecar means nothing records which system the file came from, so
           it is not offered rather than offered-and-refused. */
        return 0;
    }
    (void)DSD_SNPRINTF(label, sizeof label, "%s  (%s, sid %d)", name, prov.kind, prov.sid);
    list->entries[list->count].label = dsd_strdup(label);
    list->entries[list->count].path = dsd_strdup(path);
    if (list->entries[list->count].label == NULL || list->entries[list->count].path == NULL) {
        free(list->entries[list->count].label);
        free(list->entries[list->count].path);
        list->entries[list->count].label = NULL;
        list->entries[list->count].path = NULL;
        return 1;
    }
    list->count++;
    return 0;
}

/* dsd_dir_list() reports entries in whatever order the platform hands back, so
   the rows are sorted here. Label and path move together - sorting the label
   array alone would hand the chooser a row that opens a different file. */
static int
rr_refresh_entry_cmp(const void* lhs, const void* rhs) {
    const RrRefreshEntry* a = (const RrRefreshEntry*)lhs;
    const RrRefreshEntry* b = (const RrRefreshEntry*)rhs;
    return strcmp(a->label, b->label);
}

static void
chooser_done_rr_refresh(void* u, int sel) {
    RrRefreshList* list = (RrRefreshList*)u;
    if (list == NULL) {
        return;
    }
    if (sel >= 0 && sel < list->count) {
        RrWizardCore* core = rr_panel_ensure_core();
        if (core == NULL) {
            ui_statusf("RadioReference wizard could not be started.");
        } else {
            /* Marked active before the call: begin_refresh() can reach
               RR_STEP_ERROR synchronously, and rr_panel_render() draws nothing
               while the panel is inactive. Not marked earlier, in
               rr_panel_open_refresh(): a fresh core sits at RR_STEP_IDLE, and
               rr_panel_tick() deactivates the panel on that step, so an active
               panel would be switched back off while the chooser was still up. */
            g_rr_panel.active = 1;
            (void)rr_wizard_core_begin_refresh(core, list->entries[sel].path);
        }
    }
    rr_refresh_list_free(list);
}

/* The rr_panel_open_* pair is frozen non-const to match the nc_action_fn shape the menu glue
   and the Stage 10/11 bodies share; the panel only ever reads through these pointers, because
   the UI thread never mutates dsd_opts or dsd_state. */
// cppcheck-suppress-begin constParameterPointer
void
rr_panel_open_import(dsd_opts* opts, dsd_state* state) {
    (void)state;
    RrWizardCore* w = rr_panel_ensure_core();
    if (w == NULL) {
        ui_statusf("RadioReference wizard could not be started.");
        return;
    }
    if (opts != NULL) {
        rr_wizard_core_set_username(w, opts->rr_username);
        rr_wizard_core_set_stored_app_key(w, opts->rr_app_key);
    }
    /* Set before begin_import: the core may synchronously call open_string and
       panel_changed from inside it. */
    g_rr_panel.active = 1;
    rr_wizard_core_begin_import(w);
}

void
rr_panel_open_refresh(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    const char* dir = dsd_user_imports_dir();
    if (dir == NULL || dir[0] == '\0') {
        ui_statusf("No config directory, so there is nowhere to look for imports.");
        return;
    }
    RrRefreshList* list = (RrRefreshList*)calloc(1U, sizeof(*list));
    if (list == NULL) {
        ui_statusf("Out of memory");
        return;
    }
    const int n = DSD_SNPRINTF(list->dir, sizeof list->dir, "%s", dir);
    if (n <= 0 || (size_t)n >= sizeof list->dir) {
        rr_refresh_list_free(list);
        ui_statusf("No config directory, so there is nowhere to look for imports.");
        return;
    }
    (void)dsd_dir_list(list->dir, rr_refresh_collect, list);
    if (list->count == 0) {
        /* Reported once here, on activation, rather than by the menu predicate:
           predicates run on every menu render and listing a directory plus
           reading a sidecar per entry at 15 FPS is filesystem traffic for
           nothing. Status text is emitted before the free that owns the dir. */
        ui_statusf("No RadioReference imports found in %s", list->dir);
        rr_refresh_list_free(list);
        return;
    }
    qsort(list->entries, (size_t)list->count, sizeof list->entries[0], rr_refresh_entry_cmp);
    for (int i = 0; i < list->count; i++) {
        list->items[i] = list->entries[i].label;
    }
    /* ui_chooser_start() answers -1 synchronously when count <= 0, which the
       early return above already rules out; chooser_done_rr_refresh() is
       re-entrant-safe either way because it frees exactly once. */
    ui_chooser_start("Refresh RadioReference import", list->items, list->count, chooser_done_rr_refresh, list);
}

// cppcheck-suppress-end constParameterPointer

void
rr_panel_close(void) {
    rr_panel_release_window();
    if (g_rr_panel.core != NULL) {
        rr_wizard_core_cancel(g_rr_panel.core); /* bumps the generation; late results are dropped */
    }
    g_rr_panel.active = 0;
    /* The core, its client and the password deliberately survive: the password is asked
       once per app session. rr_panel_shutdown() is what destroys them. */
}

void
rr_panel_shutdown(void) {
    if (g_rr_panel.core != NULL) {
        rr_wizard_core_cancel(g_rr_panel.core); /* cancel FIRST: destroy joins the worker */
        rr_wizard_core_destroy(g_rr_panel.core);
        g_rr_panel.core = NULL;
    }
    rr_panel_release_window();
    g_rr_panel.active = 0;
}

/* ---- Tick ---------------------------------------------------------------- */

void
rr_panel_tick(dsd_opts* opts, dsd_state* state) {
    /* Called from ui_menu_tick on two cadences (~15 ms input path, ~66 ms base-render path);
       on the render path these are read-only snapshot pointers. Nothing here reads or writes
       them - they exist so the call site mirrors ui_menu_tick and a later stage can widen
       the body without a contract change. */
    (void)opts;
    (void)state;
    if (!g_rr_panel.active || g_rr_panel.core == NULL) {
        return;
    }
    if (rr_wizard_core_pump(g_rr_panel.core) != 0) {
        dsd_app_request_redraw();
    }
    const RrWizardStep step = rr_wizard_core_step(g_rr_panel.core);
    /* Both steps are terminal. RR_STEP_IMPORTING is where a written-and-applied
       import parks and nothing walks it back; RR_STEP_IDLE is where a cancel and
       a completed refresh land. Neither has a renderer, and rr_panel_handle_key()
       consumes every key while the panel is active, so staying active on either
       would leave a modal that draws nothing and cannot be dismissed. */
    if (step == RR_STEP_IDLE || step == RR_STEP_IMPORTING) {
        g_rr_panel.active = 0;
    }
}

/* ---- Window and layout --------------------------------------------------- */

static WINDOW*
rr_panel_ensure_window(int h, int w, int wy, int wx) {
    if (g_rr_panel.win != NULL
        && (g_rr_panel.win_h != h || g_rr_panel.win_w != w || g_rr_panel.win_y != wy || g_rr_panel.win_x != wx)) {
        rr_panel_release_window();
    }
    if (g_rr_panel.win == NULL) {
        g_rr_panel.win = ui_make_window(h, w, wy, wx);
        if (g_rr_panel.win == NULL) {
            return NULL;
        }
        keypad(g_rr_panel.win, TRUE);
        wtimeout(g_rr_panel.win, 0);
        g_rr_panel.win_h = h;
        g_rr_panel.win_w = w;
        g_rr_panel.win_y = wy;
        g_rr_panel.win_x = wx;
    }
    return g_rr_panel.win;
}

static int
rr_panel_center_axis(int screen_extent, int window_extent) {
    int pos = (screen_extent - window_extent) / 2;
    if (pos < 0) {
        pos = 0;
    }
    return pos;
}

static int
rr_panel_compute_rect(int site_count, int screen_h, int screen_w, int* h, int* w, int* wy, int* wx) {
    if (h == NULL || w == NULL || wy == NULL || wx == NULL) {
        return 0;
    }
    if (screen_h < RR_PANEL_MIN_H + 2 || screen_w < RR_PANEL_MIN_W + 2) {
        return 0;
    }
    /* The guard above is what enforces both minimums: screen_h >= RR_PANEL_MIN_H + 2 and
       screen_w >= RR_PANEL_MIN_W + 2, so neither value can be clamped below its floor here
       (local_h starts at RR_PANEL_CHROME_ROWS + 1 == RR_PANEL_MIN_H). A second "raise back
       to the minimum" clamp would therefore be dead code, which CodeQL's
       cpp/constant-comparison flags. */
    int local_h = RR_PANEL_CHROME_ROWS + ((site_count > 0) ? site_count : 1);
    if (local_h > screen_h - 2) {
        local_h = screen_h - 2;
    }
    int local_w = RR_PANEL_MAX_W;
    if (local_w > screen_w - 2) {
        local_w = screen_w - 2;
    }
    *h = local_h;
    *w = local_w;
    *wy = rr_panel_center_axis(screen_h, local_h);
    *wx = rr_panel_center_axis(screen_w, local_w);
    return 1;
}

/* ---- Section draws ------------------------------------------------------- */

static int
rr_panel_core_site_selected(const void* user, size_t index) {
    return rr_wizard_core_site_selected((const RrWizardCore*)user, index);
}

static void
rr_panel_clamp_selection(int count) {
    if (count <= 0) {
        g_rr_panel.sel = 0;
        g_rr_panel.top = 0;
        return;
    }
    if (g_rr_panel.sel < 0) {
        g_rr_panel.sel = 0;
    }
    if (g_rr_panel.sel >= count) {
        g_rr_panel.sel = count - 1;
    }
    g_rr_panel.top = ui_scroll_follow_selection(count, g_rr_panel.page_rows, g_rr_panel.top, g_rr_panel.sel);
}

static void
rr_panel_draw_title(WINDOW* win, int w, const char* name) {
    char title[160];
    (void)DSD_SNPRINTF(title, sizeof title, " RadioReference: %.100s ",
                       (name != NULL && name[0] != '\0') ? name : "system");
    mvwaddnstr(win, 0, 2, title, (w > 6) ? (w - 6) : 1);
}

static void
rr_panel_draw_header(WINDOW* win, int body_w, const dsd_rr_system_info* info) {
    char ids[64];
    if (info->sysid_hex[0] == '\0') {
        (void)DSD_SNPRINTF(ids, sizeof ids, "no sysid");
    } else if (info->sysid_count > 1) {
        (void)DSD_SNPRINTF(ids, sizeof ids, "sysid %s wacn %s (+%d)", info->sysid_hex, info->wacn_hex,
                           info->sysid_count - 1);
    } else {
        (void)DSD_SNPRINTF(ids, sizeof ids, "sysid %s wacn %s", info->sysid_hex, info->wacn_hex);
    }
    char line[512];
    (void)DSD_SNPRINTF(line, sizeof line, "%s - %s | %s / %s | %s", info->name, info->city, info->type_descr,
                       info->flavor_descr, ids);
    mvwaddnstr(win, 1, 2, line, body_w);
}

static void
rr_panel_draw_heading(WINDOW* win, int body_w, const dsd_rr_system_info* info, const dsd_rr_site_list* sites) {
    const int count = (int)sites->count;
    int last = g_rr_panel.top + g_rr_panel.page_rows;
    if (last > count) {
        last = count;
    }
    char line[128];
    (void)DSD_SNPRINTF(line, sizeof line, "%-30.30s(%d-%d/%d)",
                       info->trunked ? "Site (trunked: one only)" : "Repeaters (Space toggles)",
                       (count > 0) ? (g_rr_panel.top + 1) : 0, last, count);
    mvwaddnstr(win, 2, 2, line, body_w);
    if (info->trunked) {
        return;
    }
    RrPanelCounter counter;
    rr_panel_counter_state(sites->items, sites->count, rr_panel_core_site_selected, g_rr_panel.core, &counter);
    const int cx = 2 + body_w - (int)strlen(counter.text);
    if (cx <= (int)strlen(line) + 2) {
        return;
    }
    if (counter.over_cap) {
        wattron(win, COLOR_PAIR(RR_PANEL_CP_WARN) | A_BOLD);
    }
    mvwaddnstr(win, 2, cx, counter.text, (int)strlen(counter.text));
    if (counter.over_cap) {
        wattroff(win, COLOR_PAIR(RR_PANEL_CP_WARN) | A_BOLD);
    }
}

static void
rr_panel_draw_sites(WINDOW* win, int w, int body_w, const dsd_rr_system_info* info, const dsd_rr_site_list* sites) {
    int y = 3;
    int drawn = 0;
    for (int i = g_rr_panel.top; i < (int)sites->count && drawn < g_rr_panel.page_rows; i++, drawn++) {
        char row[160];
        const int selected = rr_wizard_core_site_selected(g_rr_panel.core, (size_t)i);
        if (rr_panel_site_row_format(&sites->items[i], info->trunked, selected, i == g_rr_panel.sel, row, sizeof row)
            != 0) {
            row[0] = '\0';
        }
        mvwhline(win, y, 1, ' ', w - 2);
        if (i == g_rr_panel.sel) {
            wattron(win, A_REVERSE);
        }
        mvwaddnstr(win, y++, 2, row, body_w);
        if (i == g_rr_panel.sel) {
            wattroff(win, A_REVERSE);
        }
    }
    while (drawn < g_rr_panel.page_rows) {
        mvwhline(win, y++, 1, ' ', w - 2);
        drawn++;
    }
}

static const char*
rr_panel_toggle_text(int value, char* buf, size_t buf_sz) {
    (void)DSD_SNPRINTF(buf, buf_sz, "%s", (value != 0) ? "ON" : "OFF");
    return buf;
}

static const char*
rr_panel_tri_text(int option, int resolved, char* buf, size_t buf_sz) {
    if (option < 0) {
        (void)DSD_SNPRINTF(buf, buf_sz, "auto(%s)", (resolved != 0) ? "ON" : "OFF");
        return buf;
    }
    (void)DSD_SNPRINTF(buf, buf_sz, "%s", (option != 0) ? "ON" : "OFF");
    return buf;
}

static void
rr_panel_draw_options(WINDOW* win, int h, int body_w, const dsd_rr_import_options* options,
                      const dsd_rr_import_plan* plan) {
    if (options == NULL) {
        return;
    }
    char p[16];
    char s[16];
    char e[16];
    char line[160];
    (void)DSD_SNPRINTF(line, sizeof line, "Options:  p partial-enc=%s   s simulcast=%s   e ESK=%s",
                       rr_panel_toggle_text(options->partial_enc_as_de, p, sizeof p),
                       rr_panel_tri_text(options->simulcast, (plan != NULL) ? plan->simulcast : 0, s, sizeof s),
                       rr_panel_tri_text(options->esk, (plan != NULL) ? plan->esk : 0, e, sizeof e));
    mvwaddnstr(win, h - 9, 2, line, body_w);
}

/**
 * @brief How much of text[pos..len) belongs on one @p width-wide row.
 *
 * Breaks at the last space inside the window when there is one, otherwise hard-
 * wraps at @p width. `pos + brk < len` holds on every iteration because the
 * hard-wrap branch is only taken when `len - pos > width`; it is nevertheless
 * spelled out in the loop condition, because clang-analyzer drops that relation
 * across the `take = width` assignment and reports a false ArrayBound without it.
 */
static size_t
rr_panel_wrap_take(const char* text, size_t pos, size_t len, size_t width) {
    const size_t remaining = len - pos;
    if (remaining <= width) {
        return remaining;
    }
    size_t brk = width;
    while (brk > 0 && (pos + brk) < len && text[pos + brk] != ' ') {
        brk--;
    }
    return (brk > 0) ? brk : width;
}

static int
rr_panel_draw_wrapped(WINDOW* win, int y, int max_rows, int width, const char* text) {
    if (win == NULL || text == NULL || width < 4 || max_rows < 1) {
        return 0;
    }
    const size_t len = strlen(text);
    size_t pos = 0;
    int rows = 0;
    while (pos < len && rows < max_rows) {
        const size_t take = rr_panel_wrap_take(text, pos, len, (size_t)width);
        mvwaddnstr(win, y + rows, 2, text + pos, (int)take);
        pos += take;
        while (pos < len && text[pos] == ' ') {
            pos++;
        }
        rows++;
    }
    return rows;
}

static void
rr_panel_draw_warnings(WINDOW* win, int h, int body_w, const dsd_rr_import_plan* plan) {
    if (plan == NULL) {
        return;
    }
    const int y_end = (h - 8) + RR_PANEL_WARNING_ROWS;
    int y = h - 8;
    size_t i = 0;
    for (i = 0; i < plan->warnings.count && y < y_end; i++) {
        char line[DSD_RR_WARNING_TEXT_MAX + 8];
        (void)DSD_SNPRINTF(line, sizeof line, "! %s", plan->warnings.items[i].text);
        y += rr_panel_draw_wrapped(win, y, y_end - y, body_w, line);
    }
    if (i < plan->warnings.count) {
        char more[40];
        (void)DSD_SNPRINTF(more, sizeof more, "! (+%zu more)", plan->warnings.count - i);
        /* The marker replaces the last content row, so erase it first: mvwaddnstr() only
           overwrites the columns it fills, and a wrapped warning already occupies this row. */
        mvwhline(win, y_end - 1, 2, ' ', body_w);
        mvwaddnstr(win, y_end - 1, 2, more, body_w);
    }
}

static void
rr_panel_draw_plan(WINDOW* win, int h, int body_w, const dsd_rr_import_plan* plan) {
    char line[320];
    const int blocked = rr_panel_plan_line(plan, line, sizeof line);
    if (blocked < 0) {
        return;
    }
    if (blocked == 1) {
        wattron(win, COLOR_PAIR(RR_PANEL_CP_ERROR) | A_BOLD);
    }
    mvwaddnstr(win, h - 5, 2, line, body_w);
    if (blocked == 1) {
        wattroff(win, COLOR_PAIR(RR_PANEL_CP_ERROR) | A_BOLD);
    }
}

static void
rr_panel_draw_status(WINDOW* win, int h, int body_w) {
    char sline[256];
    const time_t now = time(NULL);
    if (ui_status_peek(sline, sizeof sline, now)) {
        char status_line[288];
        (void)DSD_SNPRINTF(status_line, sizeof status_line, "Status: %s", sline);
        mvwaddnstr(win, h - 4, 2, status_line, body_w);
        return;
    }
    ui_status_clear_if_expired(now);
}

static void
rr_panel_draw_footer(WINDOW* win, int h, int body_w) {
    mvwaddnstr(win, h - 3, 2, "Space=Select  p=partial-enc  s=simulcast  e=ESK", body_w);
    mvwaddnstr(win, h - 2, 2, "Up/Down/PgUp/PgDn/Home/End  Enter=Import  Esc/q/Left=Back", body_w);
}

/* ---- Views --------------------------------------------------------------- */

static void
rr_panel_render_system(void) {
    const dsd_rr_system_info* info = rr_wizard_core_system(g_rr_panel.core);
    const dsd_rr_site_list* sites = rr_wizard_core_sites(g_rr_panel.core);
    if (info == NULL || sites == NULL) {
        return;
    }
    int scr_h = 0;
    int scr_w = 0;
    getmaxyx(stdscr, scr_h, scr_w);
    int h = 0;
    int w = 0;
    int wy = 0;
    int wx = 0;
    if (!rr_panel_compute_rect((int)sites->count, scr_h, scr_w, &h, &w, &wy, &wx)) {
        return;
    }
    WINDOW* win = rr_panel_ensure_window(h, w, wy, wx);
    if (win == NULL) {
        return;
    }
    /* Re-fetch the plan on every render: any mutator frees the previous one. */
    const dsd_rr_import_plan* plan = rr_wizard_core_plan(g_rr_panel.core);
    const int body_w = (w > 4) ? (w - 4) : 1;
    g_rr_panel.page_rows = h - RR_PANEL_CHROME_ROWS;
    rr_panel_clamp_selection((int)sites->count);
    werase(win);
    box(win, 0, 0);
    rr_panel_draw_title(win, w, info->name);
    rr_panel_draw_header(win, body_w, info);
    rr_panel_draw_heading(win, body_w, info, sites);
    rr_panel_draw_sites(win, w, body_w, info, sites);
    rr_panel_draw_options(win, h, body_w, rr_wizard_core_options(g_rr_panel.core), plan);
    rr_panel_draw_warnings(win, h, body_w, plan);
    rr_panel_draw_plan(win, h, body_w, plan);
    rr_panel_draw_status(win, h, body_w);
    rr_panel_draw_footer(win, h, body_w);
    wnoutrefresh(win);
}

static void
rr_panel_render_fetching(void) {
    static const char* const k_line1 = "Fetching from RadioReference...";
    static const char* const k_line2 = "Esc/q/Left cancels";
    int scr_h = 0;
    int scr_w = 0;
    getmaxyx(stdscr, scr_h, scr_w);
    const int h = 6;
    int w = 4 + (int)strlen(k_line1);
    if (w < RR_PANEL_BOX_MIN_W) {
        w = RR_PANEL_BOX_MIN_W;
    }
    if (w > scr_w - 2) {
        w = scr_w - 2;
    }
    if (scr_h < h + 2 || w < 12) {
        return;
    }
    WINDOW* win = rr_panel_ensure_window(h, w, rr_panel_center_axis(scr_h, h), rr_panel_center_axis(scr_w, w));
    if (win == NULL) {
        return;
    }
    werase(win);
    box(win, 0, 0);
    mvwaddnstr(win, 2, 2, k_line1, w - 4);
    mvwaddnstr(win, 3, 2, k_line2, w - 4);
    wnoutrefresh(win);
}

static void
rr_panel_render_error(void) {
    const char* text = rr_wizard_core_error_text(g_rr_panel.core);
    int scr_h = 0;
    int scr_w = 0;
    getmaxyx(stdscr, scr_h, scr_w);
    const int h = 10;
    int w = RR_PANEL_ERROR_W;
    if (w > scr_w - 2) {
        w = scr_w - 2;
    }
    if (scr_h < h + 2 || w < 24) {
        return;
    }
    WINDOW* win = rr_panel_ensure_window(h, w, rr_panel_center_axis(scr_h, h), rr_panel_center_axis(scr_w, w));
    if (win == NULL) {
        return;
    }
    const int body_w = w - 4;
    werase(win);
    box(win, 0, 0);
    /* The w < 24 guard above already rules out the narrow case rr_panel_draw_title() has to
       clamp for, so no ternary here - cppcheck --strict rejects the always-true condition. */
    mvwaddnstr(win, 0, 2, " RadioReference: could not continue ", w - 6);
    wattron(win, COLOR_PAIR(RR_PANEL_CP_ERROR) | A_BOLD);
    (void)rr_panel_draw_wrapped(win, 2, 4, body_w, (text != NULL && text[0] != '\0') ? text : "Unknown error.");
    wattroff(win, COLOR_PAIR(RR_PANEL_CP_ERROR) | A_BOLD);
    rr_panel_draw_status(win, h, body_w);
    mvwaddnstr(win, h - 2, 2, "Esc/q/Left=Back", body_w);
    wnoutrefresh(win);
}

void
rr_panel_render(void) {
    if (!rr_panel_active() || g_rr_panel.core == NULL) {
        return;
    }
    if (rr_wizard_core_step(g_rr_panel.core) == RR_STEP_ERROR) {
        rr_panel_render_error();
        return;
    }
    if (rr_wizard_core_fetch_in_flight(g_rr_panel.core)) {
        rr_panel_render_fetching();
        return;
    }
    if (rr_wizard_core_step(g_rr_panel.core) == RR_STEP_REFRESHING) {
        /* Keeps the box up in the gap between the last reply landing and the
           assembly finishing, which the fetch_in_flight branch above no longer
           covers. */
        rr_panel_render_fetching();
        return;
    }
    if (rr_wizard_core_step(g_rr_panel.core) == RR_STEP_SYSTEM) {
        rr_panel_render_system();
        return;
    }
    /* A prompt or chooser owns the frame at every other step; ui_menu_tick renders it
     * ahead of us and never reaches this function. Drop our window so the next system
     * view rebuilds it at the right geometry. */
    rr_panel_release_window();
}

/* ---- Keys ---------------------------------------------------------------- */

static void
rr_panel_handle_resize_event(void) {
#if DSD_CURSES_NEEDS_EXPLICIT_RESIZE
    // PDCurses doesn't auto-update dimensions on resize;
    // resize_term(0,0) queries actual console size.
    resize_term(0, 0);
#endif
    rr_panel_release_window();
    dsd_app_request_redraw();
}

static int
rr_panel_is_cancel_key(int ch) {
    return (ch == 'q' || ch == 'Q' || ch == DSD_KEY_ESC || ch == KEY_LEFT);
}

static int
rr_panel_is_accept_key(int ch) {
    return (ch == 10 || ch == KEY_ENTER || ch == '\r');
}

static int
rr_panel_site_count(void) {
    const dsd_rr_site_list* sites = rr_wizard_core_sites(g_rr_panel.core);
    return (sites != NULL) ? (int)sites->count : 0;
}

static void
rr_panel_page_move(int direction) {
    const int count = rr_panel_site_count();
    const int step = ui_scroll_page_step_from_rows(g_rr_panel.page_rows);
    if (direction < 0) {
        g_rr_panel.sel -= step;
        if (g_rr_panel.sel < 0) {
            g_rr_panel.sel = 0;
        }
        g_rr_panel.top -= step;
    } else {
        g_rr_panel.sel += step;
        if (g_rr_panel.sel >= count) {
            g_rr_panel.sel = count - 1;
        }
        g_rr_panel.top += step;
    }
    g_rr_panel.top = ui_scroll_follow_selection(count, g_rr_panel.page_rows, g_rr_panel.top, g_rr_panel.sel);
}

static int
rr_panel_handle_nav_key(int ch) {
    const int count = rr_panel_site_count();
    if (count <= 0) {
        return 0;
    }
    if (ch == KEY_UP) {
        g_rr_panel.sel = (g_rr_panel.sel - 1 + count) % count;
    } else if (ch == KEY_DOWN) {
        g_rr_panel.sel = (g_rr_panel.sel + 1) % count;
    } else if (ch == KEY_HOME) {
        g_rr_panel.sel = 0;
        g_rr_panel.top = 0;
        return 1;
    } else if (ch == KEY_END) {
        g_rr_panel.sel = count - 1;
        g_rr_panel.top = ui_scroll_last_page_top(count, g_rr_panel.page_rows);
        return 1;
    } else if (ch == KEY_PPAGE) {
        rr_panel_page_move(-1);
        return 1;
    } else if (ch == KEY_NPAGE) {
        rr_panel_page_move(1);
        return 1;
    } else {
        return 0;
    }
    g_rr_panel.top = ui_scroll_follow_selection(count, g_rr_panel.page_rows, g_rr_panel.top, g_rr_panel.sel);
    return 1;
}

static int
rr_panel_handle_option_key(int ch) {
    int which = -1;
    if (ch == 'p' || ch == 'P') {
        which = 0;
    } else if (ch == 's' || ch == 'S') {
        which = 1;
    } else if (ch == 'e' || ch == 'E') {
        which = 2;
    }
    if (which < 0) {
        return 0;
    }
    rr_wizard_core_cycle_option(g_rr_panel.core, which);
    return 1;
}

static void
rr_panel_try_import(void) {
    const dsd_rr_import_plan* plan = rr_wizard_core_plan(g_rr_panel.core);
    if (plan == NULL || plan->ok == 0 || plan->blocked_reason[0] != '\0') {
        ui_statusf("%s",
                   (plan != NULL && plan->blocked_reason[0] != '\0') ? plan->blocked_reason : "Nothing to import yet.");
        return;
    }
    (void)rr_wizard_core_import_now(g_rr_panel.core);
}

static int
rr_panel_handle_system_key(int ch) {
    if (rr_panel_handle_nav_key(ch)) {
        return 1;
    }
    if (ch == ' ') {
        rr_wizard_core_toggle_site(g_rr_panel.core, (size_t)g_rr_panel.sel);
        return 1;
    }
    if (rr_panel_handle_option_key(ch)) {
        return 1;
    }
    if (rr_panel_is_accept_key(ch)) {
        rr_panel_try_import();
        return 1;
    }
    if (rr_panel_is_cancel_key(ch)) {
        rr_wizard_core_cancel(g_rr_panel.core);
    }
    return 1;
}

static int
rr_panel_handle_error_key(int ch) {
    if (rr_panel_is_cancel_key(ch)) {
        rr_wizard_core_cancel(g_rr_panel.core);
    }
    return 1;
}

int
rr_panel_handle_key(int ch) {
    if (!rr_panel_active()) {
        return 0;
    }
    if (ch == ERR) {
        return 1;
    }
    if (ch == KEY_RESIZE) {
        rr_panel_handle_resize_event();
        return 1;
    }
    if (g_rr_panel.core == NULL) {
        return 1;
    }
    if (rr_wizard_core_step(g_rr_panel.core) == RR_STEP_ERROR) {
        return rr_panel_handle_error_key(ch);
    }
    if (rr_wizard_core_fetch_in_flight(g_rr_panel.core)) {
        if (rr_panel_is_cancel_key(ch)) {
            rr_wizard_core_cancel(g_rr_panel.core);
        }
        return 1;
    }
    if (rr_wizard_core_step(g_rr_panel.core) == RR_STEP_SYSTEM) {
        return rr_panel_handle_system_key(ch);
    }
    return 1;
}
