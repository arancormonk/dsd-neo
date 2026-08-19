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
#include <dsd-neo/ui/keymap.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stddef.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "menu_prompts.h"
#include "rr_panel.h"
#include "rr_wizard_core.h"

typedef struct {
    RrWizardCore* core; /* one per app session; survives rr_panel_close() */
    WINDOW* win;        /* Stage 10 creates it in rr_panel_render(); NULL until then */
    int active;
} RrPanel;

static RrPanel g_rr_panel;

int
rr_panel_active(void) {
    return g_rr_panel.active;
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
    /* Reachable only once rr_refresh_available() returns true, which Stage 11 flips on.
       Stage 11 replaces this body with rr_wizard_core_begin_refresh(). */
    (void)opts;
    (void)state;
}

// cppcheck-suppress-end constParameterPointer

void
rr_panel_close(void) {
    if (g_rr_panel.core != NULL) {
        rr_wizard_core_cancel(g_rr_panel.core); /* bumps the generation; late results are dropped */
    }
    if (g_rr_panel.win != NULL) {
        delwin(g_rr_panel.win);
        g_rr_panel.win = NULL;
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
    if (g_rr_panel.win != NULL) {
        delwin(g_rr_panel.win);
        g_rr_panel.win = NULL;
    }
    g_rr_panel.active = 0;
}

/* ---- Tick and render ----------------------------------------------------- */

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
    if (rr_wizard_core_step(g_rr_panel.core) == RR_STEP_IDLE) {
        g_rr_panel.active = 0;
    }
}

void
rr_panel_render(void) {
    /* Stage 10 draws the system / fetching / error views here, creating g_rr_panel.win once
       with ui_make_window() and finishing with wnoutrefresh(win). It must NOT call
       ui_commit_frame(): ui_handle_menu_input (ui_async.c) commits the whole overlay stack. */
}

/* ---- Keys ---------------------------------------------------------------- */

static void
rr_panel_handle_resize_event(void) {
#if DSD_CURSES_NEEDS_EXPLICIT_RESIZE
    // PDCurses doesn't auto-update dimensions on resize;
    // resize_term(0,0) queries actual console size.
    resize_term(0, 0);
#endif
    if (g_rr_panel.win != NULL) {
        delwin(g_rr_panel.win);
        g_rr_panel.win = NULL;
    }
    dsd_app_request_redraw();
}

static int
rr_panel_is_back_key(int ch) {
    return (ch == DSD_KEY_ESC || ch == 'q' || ch == 'Q' || ch == KEY_LEFT);
}

int
rr_panel_handle_key(int ch) {
    if (!g_rr_panel.active) {
        return 0;
    }
    if (ch == ERR) {
        return 1;
    }
    if (ch == KEY_RESIZE) {
        rr_panel_handle_resize_event();
        return 1;
    }
    if (rr_panel_is_back_key(ch)) {
        if (g_rr_panel.core != NULL) {
            rr_wizard_core_cancel(g_rr_panel.core);
            if (rr_wizard_core_step(g_rr_panel.core) == RR_STEP_IDLE) {
                rr_panel_close();
            }
        } else {
            rr_panel_close();
        }
        dsd_app_request_redraw();
        return 1;
    }
    /* Stage 10 adds Up/Down/PgUp/PgDn/Home/End, Space, p/s/e and Enter here. Until then the
       panel still swallows every remaining key, exactly like ui_chooser_handle_key's final
       `return 1;` - a key the panel returns 0 for is dropped, it does NOT fall through to
       the menu. */
    return 1;
}
