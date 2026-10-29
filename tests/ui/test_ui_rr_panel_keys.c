// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Key handling of the RadioReference import panel (rr_panel.c), driven through
 * rr_panel_handle_key() on the system view:
 *  - Enter, as '\n', '\r' or KEY_ENTER, starts the import.
 *  - Keypad Enter on a Windows terminal (PDCurses PADENTER) starts it too (B13).
 *  - Any other key does not.
 *
 * The wizard core, the prompt widgets and the app-control commands are stubbed
 * at the seams rr_panel.c calls; the panel itself is the real code.
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

#include "menu_prompts.h"
#include "rr_panel.h"
#include "rr_wizard_core.h"

/* ---- Wizard core seam: parked on the system view with an importable plan ---- */

struct RrWizardCore {
    int unused;
};

static RrWizardCore g_core;
static RrWizardStep g_step = RR_STEP_IDLE;
static dsd_rr_import_plan g_plan;
static dsd_rr_import_options g_options;
static int g_import_calls = 0;

RrWizardCore*
rr_wizard_core_create(const RrWizardHooks* hooks, void* hook_user) {
    (void)hooks;
    (void)hook_user;
    return &g_core;
}

void
rr_wizard_core_destroy(RrWizardCore* w) {
    (void)w;
}

void
rr_wizard_core_set_username(RrWizardCore* w, const char* username) {
    (void)w;
    (void)username;
}

void
rr_wizard_core_set_stored_app_key(RrWizardCore* w, const char* app_key) {
    (void)w;
    (void)app_key;
}

void
rr_wizard_core_begin_import(RrWizardCore* w) {
    (void)w;
    g_step = RR_STEP_SYSTEM;
}

int
rr_wizard_core_begin_refresh(RrWizardCore* w, const char* csv_path) {
    (void)w;
    (void)csv_path;
    return -1;
}

void
rr_wizard_core_on_prompt_done(RrWizardCore* w, const char* text) {
    (void)w;
    (void)text;
}

void
rr_wizard_core_on_chooser_done(RrWizardCore* w, int index) {
    (void)w;
    (void)index;
}

int
rr_wizard_core_pump(RrWizardCore* w) {
    (void)w;
    return 0;
}

void
rr_wizard_core_cancel(RrWizardCore* w) {
    (void)w;
    g_step = RR_STEP_IDLE;
}

RrWizardStep
rr_wizard_core_step(const RrWizardCore* w) {
    (void)w;
    return g_step;
}

int
rr_wizard_core_fetch_in_flight(const RrWizardCore* w) {
    (void)w;
    return 0;
}

const char*
rr_wizard_core_error_text(const RrWizardCore* w) {
    (void)w;
    return "";
}

const dsd_rr_system_info*
rr_wizard_core_system(const RrWizardCore* w) {
    (void)w;
    return NULL;
}

const dsd_rr_site_list*
rr_wizard_core_sites(const RrWizardCore* w) {
    (void)w;
    return NULL;
}

int
rr_wizard_core_site_selected(const RrWizardCore* w, size_t index) {
    (void)w;
    (void)index;
    return 0;
}

void
rr_wizard_core_toggle_site(RrWizardCore* w, size_t index) {
    (void)w;
    (void)index;
}

void
rr_wizard_core_cycle_option(RrWizardCore* w, int which) {
    (void)w;
    (void)which;
}

const dsd_rr_import_options*
rr_wizard_core_options(const RrWizardCore* w) {
    (void)w;
    return &g_options;
}

const dsd_rr_import_plan*
rr_wizard_core_plan(const RrWizardCore* w) {
    (void)w;
    return &g_plan;
}

int
rr_wizard_core_import_now(RrWizardCore* w) {
    (void)w;
    g_import_calls++;
    return 0;
}

int
rr_wizard_core_session_block_reason(char* out, size_t out_sz) {
    if (out != NULL && out_sz > 0) {
        out[0] = '\0';
    }
    return 0;
}

/* ---- Widget, status and app-control seams: inert ---- */

void
ui_prompt_open_string_async(const char* title, const char* prefill, size_t cap, ui_prompt_string_done_fn on_done,
                            void* user_ctx) {
    (void)title;
    (void)prefill;
    (void)cap;
    (void)on_done;
    (void)user_ctx;
}

void
ui_prompt_open_secret_async(const char* title, size_t cap, ui_prompt_string_done_fn on_done, void* user_ctx) {
    (void)title;
    (void)cap;
    (void)on_done;
    (void)user_ctx;
}

void
ui_chooser_start(const char* title, const char* const* items, int count, ui_chooser_done_fn on_done, void* user_ctx) {
    (void)title;
    (void)items;
    (void)count;
    (void)on_done;
    (void)user_ctx;
}

void
ui_chooser_close(void) {}

WINDOW*
ui_make_window(int h, int w, int y, int x) {
    (void)h;
    (void)w;
    (void)y;
    (void)x;
    return NULL;
}

void
ui_statusf(const char* fmt, ...) {
    (void)fmt;
}

int
ui_status_peek(char* buf, size_t n, time_t now) {
    (void)buf;
    (void)n;
    (void)now;
    return 0;
}

void
ui_status_clear_if_expired(time_t now) {
    (void)now;
}

void
dsd_app_request_redraw(void) {}

const dsd_opts*
dsd_app_get_latest_opts_snapshot(void) {
    return NULL;
}

int
dsd_app_command_set_string(int cmd_id, const char* value) {
    (void)cmd_id;
    (void)value;
    return 0;
}

int
dsd_app_command_set_rr_apply(const dsd_app_rr_apply_payload* payload) {
    (void)payload;
    return 0;
}

int
dsd_app_command_set_rr_account(const dsd_app_rr_account_payload* payload) {
    (void)payload;
    return 0;
}

int
dsd_app_rr_fill_apply_payload(const dsd_rr_import_plan* plan, const char* chan_path, const char* group_path,
                              dsd_app_rr_apply_payload* out) {
    (void)plan;
    (void)chan_path;
    (void)group_path;
    (void)out;
    return -1;
}

/* ---- Tests ---- */

static void
open_system_view(void) {
    DSD_MEMSET(&g_plan, 0, sizeof g_plan);
    g_plan.ok = 1;
    g_import_calls = 0;
    rr_panel_open_import(NULL, NULL);
    assert(rr_panel_active() == 1);
    assert(g_step == RR_STEP_SYSTEM);
}

static void
assert_key_imports(int ch) {
    open_system_view();
    assert(rr_panel_handle_key(ch) == 1);
    assert(g_import_calls == 1);
}

int
main(void) {
    assert_key_imports('\n');
    assert_key_imports('\r');
    assert_key_imports(KEY_ENTER);
#ifdef PADENTER
    assert_key_imports(PADENTER);
#endif

    /* A key that is not Enter leaves the plan where it is. */
    open_system_view();
    assert(rr_panel_handle_key('x') == 1);
    assert(g_import_calls == 0);

    rr_panel_shutdown();
    assert(rr_panel_active() == 0);

    printf("UI_RR_PANEL_KEYS: OK\n");
    return 0;
}
