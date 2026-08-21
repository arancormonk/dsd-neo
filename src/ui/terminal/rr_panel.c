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
 * The panel draws the system view, the fetching placeholder and the error view
 * itself; the credential prompts, the search-mode chooser, the browse choosers
 * and the results chooser all render through menu_prompts.c, which keeps
 * priority over the panel in both the key chain and the render chain.
 */

#include <curses.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/platform/curses_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <dsd-neo/ui/keymap.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/ui/menu_core.h"
#include "menu_internal.h"
#include "menu_prompts.h"
#include "rr_library.h"
#include "rr_panel.h"
#include "rr_panel_format.h"
#include "rr_wizard_core.h"

/* One import is at most two files (a group list and a channel map); the browser
   refreshes both in turn from a queue the panel owns. */
#define RR_REFRESH_QUEUE_MAX  2
#define RR_REFRESH_PATH_MAX   1024

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
    /* Sequential refresh of a system's file halves. refresh_active is 1 while the
       queue is driving the wizard, which lets rr_panel_tick() tell a refresh that
       finished (advance to the next file) from a plain cancel (just deactivate). */
    char refresh_queue[RR_REFRESH_QUEUE_MAX][RR_REFRESH_PATH_MAX];
    int refresh_queue_n;
    int refresh_queue_i;
    int refresh_active;
    /* The core's latest status text, kept past the toast's expiry: it is what
       the Fetching box reads while a request is in flight ("Loading counties...",
       "Checking your RadioReference account..."), which can outlast a toast. */
    char stage[128];
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
    RrPanel* p = (RrPanel*)user;
    if (text == NULL) {
        return;
    }
    if (p != NULL) {
        /* "" is the core clearing a stage that has finished. */
        (void)DSD_SNPRINTF(p->stage, sizeof p->stage, "%s", text);
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
        /* Seed the stored account HERE rather than only in rr_panel_open_import():
           the Imported Systems browser reaches begin_refresh() through this
           function, so a core created by a Refresh would otherwise start with no
           username and no application key and rr_refresh_creds_ready() would
           refuse every refresh with "enter your username, password and
           application key first" - even for an account already in the config.
           The published snapshot, never the live struct: the decoder thread owns
           dsd_opts. A NULL snapshot (nothing published yet) simply seeds nothing,
           and rr_panel_open_import() reseeds from the opts it was handed. */
        const dsd_opts* snap = dsd_app_get_latest_opts_snapshot();
        if (g_rr_panel.core != NULL && snap != NULL) {
            rr_wizard_core_set_username(g_rr_panel.core, snap->rr_username);
            rr_wizard_core_set_stored_app_key(g_rr_panel.core, snap->rr_app_key);
        }
    }
    return g_rr_panel.core;
}

/* ---- Refresh queue ------------------------------------------------------- */

static void
rr_panel_refresh_queue_clear(void) {
    g_rr_panel.refresh_queue_n = 0;
    g_rr_panel.refresh_queue_i = 0;
    g_rr_panel.refresh_active = 0;
}

/*
 * Pop the next queued file and start its refresh. Returns 1 when a refresh was
 * begun (whether it entered the fetch path or failed synchronously onto the
 * error step, both of which the panel renders), 0 when the queue is empty or the
 * wizard could not be created.
 */
static int
rr_panel_refresh_queue_start_next(void) {
    while (g_rr_panel.refresh_queue_i < g_rr_panel.refresh_queue_n) {
        const char* path = g_rr_panel.refresh_queue[g_rr_panel.refresh_queue_i++];
        RrWizardCore* core = rr_panel_ensure_core();
        if (core == NULL) {
            ui_statusf("RadioReference wizard could not be started.");
            rr_panel_refresh_queue_clear();
            return 0;
        }
        /* Active before the call: begin_refresh() can reach the error step
           synchronously, and the panel renders nothing while inactive. */
        g_rr_panel.active = 1;
        g_rr_panel.refresh_active = 1;
        if (rr_wizard_core_begin_refresh(core, path) == 0) {
            return 1; /* fetch in flight */
        }
        /* Synchronous failure parked on the error step; it renders and the user
           dismisses it. Stop advancing so the second file is not attempted. */
        g_rr_panel.refresh_queue_i = g_rr_panel.refresh_queue_n;
        return 1;
    }
    return 0;
}

/* ---- Imported Systems browser -------------------------------------------- */

/*
 * ui_chooser_start() copies NOTHING - it keeps the title, the item array and
 * every string in it by pointer until the chooser closes (menu_prompts.c). So
 * one heap context outlives every chooser level of this flow: the system list,
 * the per-system action list, and the delete confirmation all borrow strings
 * from it, and each done-callback either advances to the next level (keeping the
 * context) or frees it exactly once.
 */
#define RR_BROWSE_ROW_MAX        256
#define RR_BROWSE_TITLE_MAX      176

/* Widest a system name gets inside a status toast. The fixed wording of every
   toast below stays under 40 columns so that, with this, the whole message
   fits one row of an 80-column terminal's panel and two rows of the menu. */
#define RR_STATUS_NAME_MAX       28

/* Columns the chooser spends on itself: a border either side and two columns of
   padding either side (menu_prompts.c draws items at x = 2). */
#define RR_BROWSE_CHOOSER_CHROME 6

enum { RR_ACT_USE = 0, RR_ACT_REFRESH, RR_ACT_DELETE, RR_ACT_MAX };

typedef struct {
    RrLibrary lib;
    char rows[RR_LIBRARY_MAX][RR_BROWSE_ROW_MAX];
    const char* items[RR_LIBRARY_MAX];
    int sel; /* the system the action list is about */
    char title[RR_BROWSE_TITLE_MAX];
    char action_rows[RR_ACT_MAX][48];
    const char* action_items[RR_ACT_MAX];
    int action_kind[RR_ACT_MAX]; /* visible row -> RR_ACT_* */
    int action_n;
    const char* confirm_items[2];
} RrBrowseCtx;

static void
rr_browse_ctx_free(RrBrowseCtx* ctx) {
    free(ctx);
}

/* Post the stored system to the decoder, exactly as a fresh import would. */
static void
rr_browse_apply(const RrLibrarySystem* s) {
    const char* chan = s->has_chan ? s->chan_path : NULL;
    const char* group = s->has_group ? s->group_path : NULL;
    /* Named with its site: several stored imports carry the same system name,
       one per site, so the system alone would not say which one this is. */
    char named[RR_BROWSE_TITLE_MAX];
    rr_library_display_name(s, named, sizeof named);

    /* The same pre-check the wizard runs before its own apply, and ahead of BOTH
       branches: svc_import_channel_map() refuses outright during a trunk-scan
       session and dsd_app_drain_cmds() discards the handler's return value, so
       the files-only path would otherwise read "Loaded <system> files" for a
       command the decoder silently threw away. */
    char blocked[256];
    blocked[0] = '\0';
    if (rr_wizard_core_session_block_reason(blocked, sizeof blocked) != 0) {
        ui_statusf("%s", blocked);
        return;
    }

    if (s->recipe.present) {
        dsd_rr_import_plan plan;
        if (dsd_rr_recipe_to_plan(&s->recipe, s->partial_enc_as_de, &plan) == 0) {
            dsd_app_rr_apply_payload payload;
            DSD_MEMSET(&payload, 0, sizeof payload);
            if (dsd_app_rr_fill_apply_payload(&plan, chan, group, &payload) == 0) {
                const int rc = dsd_app_command_set_rr_apply(&payload);
                /* Names are clipped so the wording always fits one status row
                   (the menu offers ~39 columns after "Status: "); a failure
                   drops the name entirely - it was the title of the list the
                   user just left - and keeps the cause and the way out. */
                if (rc > 0) {
                    ui_statusf("Applying %.*s to this session.", RR_STATUS_NAME_MAX, named);
                } else {
                    ui_statusf("The decoder is not accepting commands; nothing was applied.");
                }
                return;
            }
        }
        ui_statusf("Saved settings unreadable; try Refresh from RadioReference.");
        return;
    }

    /* No recipe (an older sidecar, or a system a newer build wrote): load the
       files by path and leave the decode mode alone. */
    int loaded = 0;
    if (chan != NULL) {
        loaded |= (dsd_app_command_set_string(DSD_APP_CMD_IMPORT_CHANNEL_MAP, chan) > 0) ? 1 : 0;
    }
    if (group != NULL) {
        loaded |= (dsd_app_command_set_string(DSD_APP_CMD_IMPORT_GROUP_LIST, group) > 0) ? 1 : 0;
    }
    if (loaded) {
        ui_statusf("Loaded %.*s files; decode mode unchanged.", RR_STATUS_NAME_MAX, named);
    } else {
        ui_statusf("%.*s has no files to load.", RR_STATUS_NAME_MAX, named);
    }
}

/** @brief Remove one stored CSV and its ".rr" sidecar. @return 1 when the CSV went. */
static int
rr_browse_delete_one(const char* csv_path) {
    /* Sized off the field it is built from, not a magic literal, and truncation
       is rejected rather than shortened: a shortened path names a DIFFERENT file
       and this hands it straight to remove(). */
    char side[sizeof(((RrLibrarySystem*)0)->group_path) + 4];
    const int gone = (remove(csv_path) == 0) ? 1 : 0;
    const int n = DSD_SNPRINTF(side, sizeof side, "%s.rr", csv_path);
    if (n > 0 && (size_t)n < sizeof side) {
        (void)remove(side);
    }
    return gone;
}

/* Remove both halves of a system and their sidecars. */
static void
rr_browse_delete(const RrLibrarySystem* s) {
    char named[RR_BROWSE_TITLE_MAX];
    rr_library_display_name(s, named, sizeof named);
    int removed = 0;
    if (s->has_group) {
        removed += rr_browse_delete_one(s->group_path);
    }
    if (s->has_chan) {
        removed += rr_browse_delete_one(s->chan_path);
    }
    if (removed > 0) {
        ui_statusf("Deleted %.*s (%d file%s).", RR_STATUS_NAME_MAX, named, removed, (removed == 1) ? "" : "s");
    } else {
        ui_statusf("Could not delete %.*s.", RR_STATUS_NAME_MAX, named);
    }
}

/* Queue this system's files for refresh and start the first, entering the
   wizard's fetch/render path. Both halves are refreshed in turn. */
static void
rr_browse_refresh(const RrLibrarySystem* s) {
    rr_panel_refresh_queue_clear();
    if (s->has_group) {
        (void)DSD_SNPRINTF(g_rr_panel.refresh_queue[g_rr_panel.refresh_queue_n++], RR_REFRESH_PATH_MAX, "%s",
                           s->group_path);
    }
    if (s->has_chan) {
        (void)DSD_SNPRINTF(g_rr_panel.refresh_queue[g_rr_panel.refresh_queue_n++], RR_REFRESH_PATH_MAX, "%s",
                           s->chan_path);
    }
    if (g_rr_panel.refresh_queue_n == 0) {
        char named[RR_BROWSE_TITLE_MAX];
        rr_library_display_name(s, named, sizeof named);
        ui_statusf("%.*s has no files to refresh.", RR_STATUS_NAME_MAX, named);
        return;
    }
    (void)rr_panel_refresh_queue_start_next();
}

/* Level 3: the delete confirmation. Index 0 is Cancel so a stray Enter is safe. */
static void
rr_browse_confirm_done(void* u, int sel) {
    RrBrowseCtx* ctx = (RrBrowseCtx*)u;
    if (ctx == NULL) {
        return;
    }
    if (sel == 1 && ctx->sel >= 0 && ctx->sel < ctx->lib.count) {
        rr_browse_delete(&ctx->lib.systems[ctx->sel]);
    }
    rr_browse_ctx_free(ctx);
}

/* Level 2: the chosen action. */
static void
rr_browse_action_done(void* u, int sel) {
    RrBrowseCtx* ctx = (RrBrowseCtx*)u;
    if (ctx == NULL) {
        return;
    }
    if (sel < 0 || sel >= ctx->action_n || ctx->sel < 0 || ctx->sel >= ctx->lib.count) {
        rr_browse_ctx_free(ctx);
        return;
    }
    const RrLibrarySystem* s = &ctx->lib.systems[ctx->sel];
    switch (ctx->action_kind[sel]) {
        case RR_ACT_USE:
            rr_browse_apply(s);
            rr_browse_ctx_free(ctx);
            return;
        case RR_ACT_REFRESH:
            rr_browse_refresh(s);
            rr_browse_ctx_free(ctx);
            return;
        case RR_ACT_DELETE: {
            char named[RR_BROWSE_TITLE_MAX];
            rr_library_display_name(s, named, sizeof named);
            /* Bounded so the WORDING always survives: named[] is as wide as
               ctx->title, so a long "<system> - <site>" would otherwise push
               "from disk?" off the end and leave a destructive confirmation
               reading as a bare name above "Cancel" / "Delete permanently". */
            (void)DSD_SNPRINTF(ctx->title, sizeof ctx->title, "Delete %.*s from disk?",
                               (int)(sizeof ctx->title - sizeof "Delete  from disk?"), named);
            ctx->confirm_items[0] = "Cancel";
            ctx->confirm_items[1] = "Delete permanently";
            ui_chooser_start(ctx->title, ctx->confirm_items, 2, rr_browse_confirm_done, ctx);
            return;
        }
        default:
            /* Never the destructive branch: an action kind this switch does not
               know must do nothing, not fall through to deleting files. */
            rr_browse_ctx_free(ctx);
            return;
    }
}

/* Level 1: a system was chosen; offer its actions. */
static void
rr_browse_system_done(void* u, int sel) {
    RrBrowseCtx* ctx = (RrBrowseCtx*)u;
    if (ctx == NULL) {
        return;
    }
    if (sel < 0 || sel >= ctx->lib.count) {
        rr_browse_ctx_free(ctx);
        return;
    }
    ctx->sel = sel;
    const RrLibrarySystem* s = &ctx->lib.systems[sel];

    ctx->action_n = 0;
    ctx->action_kind[ctx->action_n] = RR_ACT_USE;
    (void)DSD_SNPRINTF(ctx->action_rows[ctx->action_n], sizeof ctx->action_rows[0], "%s",
                       s->recipe.present ? "Use this system" : "Load these files");
    ctx->action_items[ctx->action_n] = ctx->action_rows[ctx->action_n];
    ctx->action_n++;

    ctx->action_kind[ctx->action_n] = RR_ACT_REFRESH;
    (void)DSD_SNPRINTF(ctx->action_rows[ctx->action_n], sizeof ctx->action_rows[0], "%s",
                       "Refresh from RadioReference");
    ctx->action_items[ctx->action_n] = ctx->action_rows[ctx->action_n];
    ctx->action_n++;

    ctx->action_kind[ctx->action_n] = RR_ACT_DELETE;
    (void)DSD_SNPRINTF(ctx->action_rows[ctx->action_n], sizeof ctx->action_rows[0], "%s", "Delete imported files");
    ctx->action_items[ctx->action_n] = ctx->action_rows[ctx->action_n];
    ctx->action_n++;

    rr_library_display_name(s, ctx->title, sizeof ctx->title);
    ui_chooser_start(ctx->title, ctx->action_items, ctx->action_n, rr_browse_action_done, ctx);
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
rr_panel_open_library(dsd_opts* opts, dsd_state* state) {
    (void)state;
    /* Copied before anything else runs, the same way rr_import_resolve_dir()
       does: dsd_user_imports_dir() has no latch and rewrites its internal static
       buffer on every call, so the returned pointer is only good until the next
       one - and it is held here across a directory walk and three status
       messages. */
    const char* resolved = dsd_user_imports_dir();
    char dir[RR_REFRESH_PATH_MAX];
    const int dir_n = (resolved != NULL) ? DSD_SNPRINTF(dir, sizeof dir, "%s", resolved) : -1;
    if (dir_n <= 0 || (size_t)dir_n >= sizeof dir) {
        ui_statusf("No config directory, so there is nowhere to look for imports.");
        return;
    }

    RrBrowseCtx* ctx = (RrBrowseCtx*)calloc(1U, sizeof(*ctx));
    if (ctx == NULL) {
        ui_statusf("Out of memory");
        return;
    }
    ctx->sel = -1;
    if (rr_library_scan(&ctx->lib, dir) < 0) {
        ui_statusf("Could not read the imports directory %s", dir);
        rr_browse_ctx_free(ctx);
        return;
    }
    if (ctx->lib.count == 0) {
        /* Reported once here, on activation, rather than by the menu predicate:
           listing a directory and reading a sidecar per entry at ~15 FPS would be
           filesystem traffic for nothing. Says what to do next, not where it
           looked: the directory is in docs/radioreference-import.md, and a path
           here was what pushed the advice off the row. */
        ui_statusf("No imported systems yet; use Import from RadioReference... first.");
        rr_browse_ctx_free(ctx);
        return;
    }
    rr_library_sort(&ctx->lib);

    /* In-use marking reads the published snapshot, never the live opts: the
       decoder thread rewrites chan_in_file / group_in_file under no lock. A NULL
       snapshot (no session, or one publish behind) simply marks nothing. */
    const dsd_opts* snap = dsd_app_get_latest_opts_snapshot();
    const char* chan_in_use = (snap != NULL) ? snap->chan_in_file : NULL;
    const char* group_in_use = (snap != NULL) ? snap->group_in_file : NULL;

    /* Measured once for the whole list, against the width the chooser has: it
       sizes its window to the longest row and clips at the screen edge, so a
       row built for a wider terminal loses its last column rather than wrapping.
       The chooser's own frame and padding are what RR_BROWSE_CHOOSER_CHROME
       accounts for. */
    RrLibraryLayout layout;
    rr_library_layout(&ctx->lib, COLS - RR_BROWSE_CHOOSER_CHROME, &layout);

    for (int idx = 0; idx < ctx->lib.count; idx++) {
        const int in_use = rr_library_system_in_use(&ctx->lib.systems[idx], chan_in_use, group_in_use);
        (void)rr_library_row_format(&ctx->lib.systems[idx], &layout, in_use, ctx->rows[idx], sizeof ctx->rows[0]);
        ctx->items[idx] = ctx->rows[idx];
    }
    /* An overflow is said in the title, which stays up as long as the list
       does; a toast raised here would expire behind the chooser. The title is
       borrowed by pointer for the life of the chooser, so it lives in ctx. */
    if (ctx->lib.overflow) {
        (void)DSD_SNPRINTF(ctx->title, sizeof ctx->title, "Imported Systems (first %d only)", ctx->lib.count);
    } else {
        (void)DSD_SNPRINTF(ctx->title, sizeof ctx->title, "Imported Systems");
    }
    /* Refresh runs on the shared session core, which rr_panel_ensure_core() seeds
       from the snapshot only on creation - so an account set from the menu after
       the core existed would not reach it. Reseed here, the same way
       rr_panel_open_import() does, before any row can be picked. */
    if (opts != NULL) {
        RrWizardCore* w = rr_panel_ensure_core();
        if (w != NULL) {
            rr_wizard_core_set_username(w, opts->rr_username);
            rr_wizard_core_set_stored_app_key(w, opts->rr_app_key);
        }
    }
    ui_chooser_start(ctx->title, ctx->items, ctx->lib.count, rr_browse_system_done, ctx);
}

// cppcheck-suppress-end constParameterPointer

void
rr_panel_close(void) {
    rr_panel_release_window();
    if (g_rr_panel.core != NULL) {
        rr_wizard_core_cancel(g_rr_panel.core); /* bumps the generation; late results are dropped */
    }
    rr_panel_refresh_queue_clear(); /* a half-done system refresh must not resume after a close */
    g_rr_panel.stage[0] = '\0';
    g_rr_panel.active = 0;
    /* The core, its client and the password deliberately survive: the password is asked
       once per app session. rr_panel_shutdown() is what destroys them. */
}

void
rr_panel_shutdown(void) {
    if (g_rr_panel.core != NULL) {
        rr_wizard_core_cancel(g_rr_panel.core); /* cancel FIRST: destroy joins the worker */
        /* Cancelling from RR_STEP_SYSTEM/LOADING_SYSTEM backs out onto the list it
           came from, which opens a chooser through the hooks - and ui_chooser_start()
           keeps the title and the item array BY POINTER, both of them owned by the
           core rr_wizard_core_destroy() is about to free. Close it while those
           pointers are still valid. */
        ui_chooser_close();
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
    /* RR_STEP_IDLE is terminal: it is where a cancel and a completed refresh
       land. It has no renderer, and rr_panel_handle_key() consumes every key
       while the panel is active, so staying active on it would leave a modal
       that draws nothing and cannot be dismissed. A finished import is NOT
       terminal - it stays on the site list so the next site of the same system
       costs one keypress instead of a whole re-fetch. */
    if (step == RR_STEP_ERROR) {
        /* A queued refresh that failed stops the queue: the error renders and the
           user dismisses it; the remaining file is not attempted. */
        rr_panel_refresh_queue_clear();
        return;
    }
    if (step == RR_STEP_IDLE) {
        /* A refresh that just succeeded returns the core to idle; if the system
           had a second file, start it before deactivating. A plain cancel also
           lands here, but with refresh_active clear, so it just deactivates. */
        if (g_rr_panel.refresh_active && rr_panel_refresh_queue_start_next()) {
            return;
        }
        rr_panel_refresh_queue_clear();
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
    /* Says how many may be chosen; the footer says which key does it. */
    (void)DSD_SNPRINTF(line, sizeof line, "%-30.30s(%d-%d/%d)",
                       info->trunked ? "Sites (choose one)" : "Repeaters (choose any)",
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

/* Widest block this panel wraps: the error view offers four rows. */
#define RR_PANEL_WRAP_ROWS_MAX 4

/**
 * @brief Draw @p text wrapped into at most @p max_rows rows.
 *
 * The split is ui_text_wrap()'s, shared with the status toast, so a warning and
 * a toast break the same way.
 *
 * @param out_clipped Optional; set to 1 when text was left undrawn, so a caller
 *                    listing several blocks can report the one that did not fit
 *                    as unread rather than counting it as shown.
 * @return Rows drawn.
 */
static int
rr_panel_draw_wrapped(WINDOW* win, int y, int max_rows, int width, const char* text, int* out_clipped) {
    if (out_clipped != NULL) {
        *out_clipped = 0;
    }
    if (win == NULL || text == NULL || width < 4 || max_rows < 1) {
        if (out_clipped != NULL && text != NULL && text[0] != '\0') {
            *out_clipped = 1;
        }
        return 0;
    }
    if (max_rows > RR_PANEL_WRAP_ROWS_MAX) {
        max_rows = RR_PANEL_WRAP_ROWS_MAX;
    }
    UiTextSlice rows[RR_PANEL_WRAP_ROWS_MAX];
    size_t consumed = 0;
    const int n = ui_text_wrap(text, width, max_rows, rows, &consumed);
    for (int i = 0; i < n; i++) {
        mvwaddnstr(win, y + i, 2, text + rows[i].start, (int)rows[i].len);
    }
    if (out_clipped != NULL && consumed < strlen(text)) {
        *out_clipped = 1;
    }
    return n;
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
        int clipped = 0;
        y += rr_panel_draw_wrapped(win, y, y_end - y, body_w, line, &clipped);
        if (clipped) {
            /* Break with i still ON this warning: only part of it was drawn, and
               the "(+N more)" marker below overwrites the row it landed in, so
               counting it as shown would understate the remainder by one. */
            break;
        }
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

/*
 * The status row, with the row under it as overflow. In the system view that
 * row is the first footer line, so a long toast borrows the key hints for a few
 * seconds rather than losing its second clause; draw it AFTER the footer.
 */
static void
rr_panel_draw_status(WINDOW* win, int h, int body_w) {
    (void)ui_status_draw(win, h - 4, 2, body_w, 2, UI_STATUS_FLAG_PREFIX, time(NULL));
}

static void
rr_panel_draw_footer(WINDOW* win, int h, int body_w) {
    mvwaddnstr(win, h - 3, 2, "Space=Select  p=partial-enc  s=simulcast  e=ESK", body_w);
    mvwaddnstr(win, h - 2, 2, "Up/Down/PgUp/PgDn/Home/End  Enter=Import  Esc/Left=Back", body_w);
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
    rr_panel_draw_footer(win, h, body_w);
    rr_panel_draw_status(win, h, body_w); /* after the footer: may borrow its first row */
    wnoutrefresh(win);
}

static void
rr_panel_render_fetching(void) {
    static const char* const k_line2 = "Esc/Left cancels";
    /* The core names each stage ("Checking your RadioReference account...",
       "Loading counties..."); the generic line is only for a fetch that did not. */
    const char* line1 = (g_rr_panel.stage[0] != '\0') ? g_rr_panel.stage : "Fetching from RadioReference...";
    int scr_h = 0;
    int scr_w = 0;
    getmaxyx(stdscr, scr_h, scr_w);
    const int h = 6;
    int w = 4 + (int)strlen(line1);
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
    mvwaddnstr(win, 2, 2, line1, w - 4);
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
    (void)rr_panel_draw_wrapped(win, 2, 4, body_w, (text != NULL && text[0] != '\0') ? text : "Unknown error.", NULL);
    wattroff(win, COLOR_PAIR(RR_PANEL_CP_ERROR) | A_BOLD);
    rr_panel_draw_status(win, h, body_w);
    mvwaddnstr(win, h - 2, 2, "Esc/Left=Back", body_w);
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

/* Esc and Left only: 'q' quits the program from the main screen, so it is not
   a back key anywhere inside the menu or its panels. */
static int
rr_panel_is_cancel_key(int ch) {
    return (ch == DSD_KEY_ESC || ch == KEY_LEFT);
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
            /* Drop the queue FIRST: rr_wizard_core_cancel() parks the core on
               RR_STEP_IDLE, which is the same step a completed refresh lands on,
               so a still-armed queue would make rr_panel_tick() read the cancel
               as "that half finished" and start fetching the other one. A cancel
               ends the whole system refresh, not just the file in flight. */
            rr_panel_refresh_queue_clear();
            rr_wizard_core_cancel(g_rr_panel.core);
        }
        return 1;
    }
    if (rr_wizard_core_step(g_rr_panel.core) == RR_STEP_SYSTEM) {
        return rr_panel_handle_system_key(ch);
    }
    return 1;
}
