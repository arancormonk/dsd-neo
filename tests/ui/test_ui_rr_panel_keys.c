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
 * And the Imported Systems browser's context through its three chooser levels
 * (issue #672, B8): owned by each level's chooser in turn, disposed exactly once
 * when a level is answered, replaced, cancelled from the render path or closed
 * at UI exit, and when a hand-off to the next level fails at once.
 *
 * And the Imported Systems browser's "Use this system" / "Load these files"
 * over a real imports directory (issue #803): each asks the session gate
 * before it posts anything - the full adoption for the recipe's own decode
 * mode, the files-only load for the channel map it loads - and posts nothing
 * when the gate says no.
 *
 * The wizard core and the app-control commands are stubbed at the seams
 * rr_panel.c calls; the panel, the library and the chooser and prompt widgets
 * are the real code.
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/config_files.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <dsd-neo/ui/menu_core.h>
#include <dsd-neo/ui/menu_defs.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "menu_internal.h"
#include "menu_prompts.h"
#include "rr_panel.h"
#include "rr_wizard_core.h"
#include "scan_list_modal.h"
#include "test_env.h"
#include "test_support.h"

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

static int g_session_blocked = 0;

int
rr_wizard_core_session_blocked(const RrWizardCore* w) {
    (void)w;
    return g_session_blocked;
}

int
rr_wizard_core_import_now(RrWizardCore* w) {
    (void)w;
    g_import_calls++;
    return 0;
}

/* The session gate, scripted: what the browser asked, and whether it says no. */
static const char k_gate_reason[] = "Gate says no.";
static int g_apply_gate_calls = 0;
static int g_apply_gate_blocks = 0;
static dsd_rr_protocol g_apply_gate_protocol = DSD_RR_PROTO_UNSUPPORTED;
static int g_apply_gate_has_group = -1;
static int g_apply_gate_has_chan = -1;
static int g_files_gate_calls = 0;
static int g_files_gate_blocks = 0;
static char g_files_gate_chan[1024];
/* The pair the gate was handed last. The browser must pass the frame's published pair it was opened with, never
   acquire one itself (issue #672, B9). */
static const dsd_opts* g_gate_opts = NULL;
static const dsd_state* g_gate_state = NULL;

int
rr_wizard_core_apply_block_reason(const dsd_opts* osnap, const dsd_state* snap, const dsd_rr_import_plan* plan,
                                  int has_group, int has_chan, char* out, size_t out_sz) {
    g_apply_gate_calls++;
    g_gate_opts = osnap;
    g_gate_state = snap;
    g_apply_gate_protocol = plan->protocol;
    g_apply_gate_has_group = has_group;
    g_apply_gate_has_chan = has_chan;
    if (g_apply_gate_blocks) {
        DSD_SNPRINTF(out, out_sz, "%s", k_gate_reason);
    }
    return g_apply_gate_blocks;
}

int
rr_wizard_core_files_block_reason(const dsd_opts* osnap, const dsd_state* snap, const char* chan_path, char* out,
                                  size_t out_sz) {
    g_files_gate_calls++;
    g_gate_opts = osnap;
    g_gate_state = snap;
    DSD_SNPRINTF(g_files_gate_chan, sizeof g_files_gate_chan, "%s", chan_path != NULL ? chan_path : "(none)");
    if (g_files_gate_blocks) {
        DSD_SNPRINTF(out, out_sz, "%s", k_gate_reason);
    }
    return g_files_gate_blocks;
}

/* ---- Window, status and app-control seams: inert (no window: the render path cancels) ---- */

WINDOW*
ui_make_window(int h, int w, int y, int x) {
    (void)h;
    (void)w;
    (void)y;
    (void)x;
    return NULL;
}

static char g_status[256];

void
ui_statusf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)DSD_VSNPRINTF(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
}

void
ui_status_expire_by(time_t until) {
    (void)until;
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

/* ---- The Scan lists modal (issue #697), which menu_core.c drives: never open here ---- */

int
scan_list_modal_active(void) {
    return 0;
}

int
scan_list_modal_handle_key(int ch) {
    (void)ch;
    return 0;
}

void
scan_list_modal_render(void) {}

void
scan_list_modal_tick(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
}

void
scan_list_modal_close(void) {}

/* What the browser posted: the IMPORT_* commands by id and in all, and the apply. */
static int g_set_string_calls = 0;
static int g_import_chan_posts = 0;
static int g_import_group_posts = 0;
static int g_rr_apply_posts = 0;
static dsd_app_rr_apply_payload g_rr_apply_payload;

int
dsd_app_command_set_string(int cmd_id, const char* value) {
    (void)value;
    g_set_string_calls++;
    if (cmd_id == DSD_APP_CMD_IMPORT_CHANNEL_MAP) {
        g_import_chan_posts++;
    } else if (cmd_id == DSD_APP_CMD_IMPORT_GROUP_LIST) {
        g_import_group_posts++;
    }
    return 1; /* DSD_APP_COMMAND_SUBMIT_QUEUED */
}

int
dsd_app_command_set_rr_apply(const dsd_app_rr_apply_payload* payload) {
    g_rr_apply_posts++;
    g_rr_apply_payload = *payload;
    return 1;
}

int
dsd_app_command_set_rr_account(const dsd_app_rr_account_payload* payload) {
    (void)payload;
    return 0;
}

/* The mode and the paths are all the browser cases look at. */
int
dsd_app_rr_fill_apply_payload(const dsd_rr_import_plan* plan, const char* chan_path, const char* group_path,
                              dsd_app_rr_apply_payload* out) {
    int mode = 0;
    if (plan == NULL || out == NULL || !plan->ok || dsd_rr_protocol_decode_mode(plan->protocol, &mode) != 0) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof *out);
    out->decode_mode = (int32_t)mode;
    out->trunking = plan->trunking ? 1U : 0U;
    out->has_chan = (chan_path != NULL && chan_path[0] != '\0') ? 1U : 0U;
    out->has_group = (group_path != NULL && group_path[0] != '\0') ? 1U : 0U;
    DSD_SNPRINTF(out->chan_path, sizeof out->chan_path, "%s", out->has_chan ? chan_path : "");
    DSD_SNPRINTF(out->group_path, sizeof out->group_path, "%s", out->has_group ? group_path : "");
    return 0;
}

/* ---- Tests ---- */

static void
open_system_view(void) {
    DSD_MEMSET(&g_plan, 0, sizeof g_plan);
    g_plan.ok = 1;
    g_session_blocked = 0;
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

/* ---- The Imported Systems browser over a real imports directory ---- */

static char g_scratch[DSD_TEST_PATH_MAX];
static char g_imports[DSD_TEST_PATH_MAX];

static void
write_text(const char* path, const char* body) {
    FILE* fp = dsd_fopen_private(path, "wb");
    assert(fp != NULL);
    assert(fputs(body, fp) >= 0);
    assert(fclose(fp) == 0);
}

/* One stored import: the CSV and its sidecar, with a recipe when @p protocol is not UNSUPPORTED. */
static void
store_import(const char* leaf, const char* kind, dsd_rr_protocol protocol, char* path_out, size_t path_sz) {
    assert(dsd_test_path_join(path_out, path_sz, g_imports, leaf) == 0);
    write_text(path_out, strcmp(kind, "chan") == 0 ? "1,390012500\n" : "1,D,Group one\n");
    dsd_rr_provenance prov;
    DSD_MEMSET(&prov, 0, sizeof prov);
    DSD_SNPRINTF(prov.kind, sizeof prov.kind, "%s", kind);
    prov.sid = 10880;
    DSD_SNPRINTF(prov.system_name, sizeof prov.system_name, "%s", "Auckland TETRA");
    DSD_SNPRINTF(prov.site_label, sizeof prov.site_label, "%s", "Waiatarua");
    DSD_SNPRINTF(prov.site_ids, sizeof prov.site_ids, "%s", "35614");
    prov.imported_at = 1755500000LL;
    if (protocol != DSD_RR_PROTO_UNSUPPORTED) {
        prov.recipe.present = 1;
        prov.recipe.protocol = protocol;
        prov.recipe.tune_hz = 858087500LL;
        prov.recipe.trunking = 1;
    }
    assert(dsd_rr_provenance_write(path_out, &prov) == 0);
}

static void
remove_import(const char* path) {
    char side[DSD_TEST_PATH_MAX + 8];
    DSD_SNPRINTF(side, sizeof side, "%s.rr", path);
    (void)remove(side);
    (void)remove(path);
}

/* The imports directory lives under a scratch config home, set before the first lookup latches it. */
static void
browser_setup(void) {
    assert(dsd_test_mkdtemp(g_scratch, sizeof g_scratch, "dsdneo_rr_browse") != NULL);
    assert(dsd_test_setenv("XDG_CONFIG_HOME", g_scratch, 1) == 0);
    assert(dsd_test_setenv("APPDATA", g_scratch, 1) == 0);
    const char* dir = dsd_user_imports_dir();
    assert(dir != NULL);
    DSD_SNPRINTF(g_imports, sizeof g_imports, "%s", dir);
    assert(strncmp(g_imports, g_scratch, strlen(g_scratch)) == 0);
    assert(dsd_user_imports_dir_create() == 0);
}

static void
browser_teardown(void) {
    char app_config[DSD_TEST_PATH_MAX];
    assert(dsd_test_rmdir(g_imports) == 0);
    assert(dsd_test_path_join(app_config, sizeof app_config, g_scratch, "dsd-neo") == 0);
    assert(dsd_test_rmdir(app_config) == 0);
    assert(dsd_test_rmdir(g_scratch) == 0);
}

static void
browser_reset(void) {
    g_apply_gate_calls = 0;
    g_apply_gate_blocks = 0;
    g_apply_gate_protocol = DSD_RR_PROTO_UNSUPPORTED;
    g_apply_gate_has_group = -1;
    g_apply_gate_has_chan = -1;
    g_files_gate_calls = 0;
    g_files_gate_blocks = 0;
    g_files_gate_chan[0] = '\0';
    g_gate_opts = NULL;
    g_gate_state = NULL;
    g_import_chan_posts = 0;
    g_import_group_posts = 0;
    g_rr_apply_posts = 0;
    DSD_MEMSET(&g_rr_apply_payload, 0, sizeof g_rr_apply_payload);
    g_status[0] = '\0';
}

/* The frame's published pair the browser is opened with. File-static: a dsd_opts or dsd_state on the stack is ~41 KB
   and more. Zeroed options mark no file in use. */
static dsd_opts g_frame_opts;
static dsd_state g_frame_state;

/* Open the browser on the frame's pair, pick its only system, then its first action, whose label must be
   @p want_action, through the real chooser's keys. */
static void
browse_first_action(const char* want_action) {
    rr_panel_open_library(&g_frame_opts, &g_frame_state);
    assert(ui_chooser_active() == 1 && ui_chooser_test_snapshot().count == 1);
    assert(ui_chooser_handle_key('\n') == 1); /* the system: its actions */
    assert(ui_chooser_active() == 1 && ui_chooser_test_snapshot().count == 3);
    assert(ui_chooser_test_snapshot().sel == 0);
    const char* first = ui_chooser_test_item(0);
    assert(first != NULL && strcmp(first, want_action) == 0);
    assert(ui_chooser_handle_key('\n') == 1); /* the action, which frees the browser's context */
    assert(ui_chooser_active() == 0);
    assert(rr_panel_browse_live_contexts_for_test() == 0);
}

/* "Use this system" asks the gate for the recipe's own decode mode, and a refusal posts nothing. */
static void
test_use_this_system_is_checked_for_its_kind(void) {
    char group[DSD_TEST_PATH_MAX];
    store_import("Auckland TETRA - Waiatarua group.csv", "group", DSD_RR_PROTO_TETRA, group, sizeof group);

    browser_reset();
    g_apply_gate_blocks = 1;
    browse_first_action("Use this system");
    assert(g_apply_gate_calls == 1);
    assert(g_gate_opts == &g_frame_opts && g_gate_state == &g_frame_state);
    assert(g_apply_gate_protocol == DSD_RR_PROTO_TETRA);
    assert(g_apply_gate_has_group == 1 && g_apply_gate_has_chan == 0);
    assert(g_files_gate_calls == 0);
    assert(g_rr_apply_posts == 0);
    assert(strcmp(g_status, k_gate_reason) == 0);

    /* The offline re-apply itself, once the session can take it. */
    browser_reset();
    browse_first_action("Use this system");
    assert(g_apply_gate_calls == 1);
    assert(g_rr_apply_posts == 1);
    assert(g_rr_apply_payload.decode_mode == (int32_t)DSDCFG_MODE_TETRA);
    assert(strcmp(g_rr_apply_payload.group_path, group) == 0);
    assert(strcmp(g_status, "Applying Auckland TETRA - Waiatarua to this session.") == 0);

    remove_import(group);
}

/* "Load these files" (no recipe) keeps a check of its own: the files it loads, never a decode mode. */
static void
test_load_these_files_is_still_checked(void) {
    char group[DSD_TEST_PATH_MAX];
    char chan[DSD_TEST_PATH_MAX];
    store_import("Auckland TETRA - Waiatarua group.csv", "group", DSD_RR_PROTO_UNSUPPORTED, group, sizeof group);
    store_import("Auckland TETRA - Waiatarua chan.csv", "chan", DSD_RR_PROTO_UNSUPPORTED, chan, sizeof chan);

    browser_reset();
    g_files_gate_blocks = 1;
    browse_first_action("Load these files");
    assert(g_files_gate_calls == 1);
    assert(g_gate_opts == &g_frame_opts && g_gate_state == &g_frame_state);
    assert(strcmp(g_files_gate_chan, chan) == 0);
    assert(g_apply_gate_calls == 0);
    assert(g_import_chan_posts == 0 && g_import_group_posts == 0);
    assert(strcmp(g_status, k_gate_reason) == 0);

    browser_reset();
    browse_first_action("Load these files");
    assert(g_files_gate_calls == 1);
    assert(g_import_chan_posts == 1 && g_import_group_posts == 1);
    assert(strcmp(g_status, "Loaded Auckland TETRA - Waiatarua files; decode mode unchanged.") == 0);

    remove_import(group);
    remove_import(chan);
}

/* ---- The Imported Systems browser (issue #672, B8) ---- */

static int g_other_done_calls;

static void
other_chooser_done(void* user, int sel) {
    (void)user;
    (void)sel;
    g_other_done_calls++;
}

static int
live(void) {
    return rr_panel_browse_live_contexts_for_test();
}

/* Level 1: the system list over the one imported system. */
static void
open_level(int level) {
    rr_panel_open_library(NULL, NULL);
    assert(ui_chooser_active() == 1);
    assert(ui_chooser_test_snapshot().count == 1);
    assert(live() == 1);
    if (level >= 2) {
        assert(ui_chooser_handle_key('\n') == 1); /* the system: its actions */
        assert(ui_chooser_active() == 1);
        assert(ui_chooser_test_snapshot().count == 3);
        assert(live() == 1);
    }
    if (level >= 3) {
        assert(ui_chooser_handle_key(KEY_END) == 1); /* "Delete imported files": the confirmation */
        assert(ui_chooser_handle_key('\n') == 1);
        assert(ui_chooser_active() == 1);
        assert(ui_chooser_test_snapshot().count == 2);
        assert(ui_chooser_test_snapshot().sel == 0); /* Cancel */
        assert(live() == 1);
    }
}

static void
expect_gone(void) {
    assert(live() == 0);
    assert(ui_chooser_active() == 0);
}

static void
test_browser_levels(void) {
    static const char* const other_items[] = {"Other"};
    for (int level = 1; level <= 3; level++) {
        /* Esc at the level. */
        open_level(level);
        assert(ui_chooser_handle_key(27) == 1);
        expect_gone();

        /* Replaced by another chooser: disposed with no callback of its own; the new chooser is untouched. */
        open_level(level);
        g_other_done_calls = 0;
        ui_chooser_start("Other", other_items, 1, other_chooser_done, NULL);
        assert(live() == 0);
        assert(ui_chooser_active() == 1);
        assert(ui_chooser_handle_key('\n') == 1);
        assert(g_other_done_calls == 1);
        expect_gone();

        /* Cancelled from the render path (no window can be made here). */
        open_level(level);
        ui_chooser_render();
        expect_gone();

        /* UI exit with no wizard core: rr_panel_shutdown() leaves the chooser, the overlay shutdown closes it. */
        open_level(level);
        rr_panel_shutdown();
        assert(live() == 1);
        ui_chooser_close();
        expect_gone();

        /* UI exit after an import made the wizard core: rr_panel_shutdown() closes the chooser itself. */
        rr_panel_open_import(NULL, NULL);
        rr_panel_close();
        open_level(level);
        rr_panel_shutdown();
        expect_gone();
    }

    /* A hand-off whose next level answers at once (a start that fails): the context is disposed exactly once. */
    open_level(1);
    ui_chooser_test_fail_next_start();
    assert(ui_chooser_handle_key('\n') == 1);
    expect_gone();
    open_level(2);
    assert(ui_chooser_handle_key(KEY_END) == 1);
    ui_chooser_test_fail_next_start();
    assert(ui_chooser_handle_key('\n') == 1);
    expect_gone();

    /* The confirmation's Cancel answers and disposes; the files stay. */
    open_level(3);
    assert(ui_chooser_handle_key('\n') == 1);
    expect_gone();

    /* Level 2's Use (no saved recipe: the files are loaded by path) answers and disposes. */
    open_level(2);
    g_set_string_calls = 0;
    assert(ui_chooser_handle_key('\n') == 1);
    assert(g_set_string_calls == 1); /* the group list, the system's one half */
    expect_gone();

    /* Level 2's Refresh hands the file to the wizard and disposes the browser's context. */
    open_level(2);
    assert(ui_chooser_handle_key(KEY_DOWN) == 1);
    assert(ui_chooser_handle_key('\n') == 1);
    expect_gone();
    assert(rr_panel_active() == 1); /* the refresh runs in the panel */
    rr_panel_shutdown();
}

/* ---- UI exit over real overlays (issue #672, B17): the menu core's real ui_overlays_shutdown() ---- */

static void
noop_action(void* ctx) {
    (void)ctx;
}

static const NcMenuItem SUB_ITEMS[] = {
    {.id = "sub", .label = "Sub", .on_select = noop_action},
};

static const NcMenuItem MAIN_ITEMS[] = {
    {.id = "main", .label = "Main", .submenu = SUB_ITEMS, .submenu_len = 1},
};

void
ui_menu_get_main_items(const NcMenuItem** out_items, size_t* out_n, UiCtx* ctx) {
    (void)ctx;
    *out_items = MAIN_ITEMS;
    *out_n = sizeof MAIN_ITEMS / sizeof MAIN_ITEMS[0];
}

int
dsd_app_config_default_missing(char* path, size_t path_size) {
    (void)path;
    (void)path_size;
    return 0;
}

int
ui_is_enabled(const NcMenuItem* it, const void* ctx) {
    (void)ctx;
    return it != NULL;
}

int
ui_is_selectable(const NcMenuItem* it, const void* ctx) {
    (void)ctx;
    return it != NULL && it->kind == NC_ITEM_ACTION;
}

int
ui_submenu_has_visible(const NcMenuItem* items, size_t n, const void* ctx) {
    for (size_t i = 0; items != NULL && i < n; i++) {
        if (ui_is_selectable(&items[i], ctx)) {
            return 1;
        }
    }
    return 0;
}

int
ui_next_selectable(const NcMenuItem* items, size_t n, const void* ctx, int from, int dir) {
    (void)items;
    (void)n;
    (void)ctx;
    (void)dir;
    return from;
}

int
ui_visible_index_for_item(const NcMenuItem* items, size_t n, const void* ctx, int idx) {
    (void)items;
    (void)n;
    (void)ctx;
    return idx;
}

int
ui_visible_count_and_maxlab(const NcMenuItem* items, size_t n, const void* ctx, int* out_maxlab) {
    (void)items;
    (void)ctx;
    if (out_maxlab) {
        *out_maxlab = 8;
    }
    return (int)n;
}

void
ui_overlay_layout(UiMenuFrame* f, const void* ctx) {
    (void)ctx;
    f->h = 10;
    f->w = 30;
    f->y = 0;
    f->x = 0;
}

void
ui_overlay_ensure_window(UiMenuFrame* f) {
    (void)f;
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

static int g_exit_prompt_cancels = 0;
static int g_exit_prompt_disposed = 0;

static void
exit_prompt_done(void* user, const char* text) {
    (void)user;
    if (!text) {
        g_exit_prompt_cancels++;
    }
}

static void
exit_prompt_dispose(void* user) {
    g_exit_prompt_disposed++;
    free(user);
}

/* A menu two frames deep, help, an owned prompt and the browser's owned chooser, then exit as the UI thread does it:
   everything closed, each owned context disposed once; the next start opens on a fresh overlay. */
static void
test_exit_over_real_overlays(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    for (int round = 0; round < 2; round++) {
        g_exit_prompt_cancels = 0;
        g_exit_prompt_disposed = 0;
        ui_menu_open_async(opts, state);
        assert(ui_menu_is_open() == 1);
        assert(ui_menu_handle_key('\n', opts, state) == 1); /* into the submenu */
        ui_help_open("help text");
        ui_prompt_open_string_owned("Name", NULL, 16, exit_prompt_done, calloc(1, 32), exit_prompt_dispose);
        rr_panel_open_library(NULL, NULL);
        assert(ui_help_active() == 1 && ui_prompt_active() == 1 && ui_chooser_active() == 1);
        assert(live() == 1);

        rr_panel_shutdown();
        ui_overlays_shutdown();
        assert(ui_help_active() == 0 && ui_prompt_active() == 0 && ui_chooser_active() == 0);
        assert(ui_menu_is_open() == 0);
        assert(live() == 0);
        assert(g_exit_prompt_cancels == 1 && g_exit_prompt_disposed == 1);
    }
    /* Nothing open: a second shutdown is harmless. */
    ui_overlays_shutdown();
    assert(ui_menu_is_open() == 0);
    free(state);
    free(opts);
}

/* One scratch directory for the process: the config path is resolved once. */
static void
test_browser_and_exit(void) {
    test_browser_levels();
    test_exit_over_real_overlays();
}

/* One imported system (a group list and its sidecar) in the scratch imports directory browser_setup() made: the
   config path latches on its first lookup, so every browser case shares that one directory. */
static void
with_one_import(void (*run)(void)) {
    char csv[DSD_TEST_PATH_MAX];
    assert(dsd_test_path_join(csv, sizeof csv, g_imports, "Sys group.csv") == 0);
    FILE* fp = dsd_fopen_private(csv, "w");
    assert(fp != NULL);
    assert(fputs("1,x\n", fp) >= 0);
    assert(fclose(fp) == 0);
    dsd_rr_provenance p;
    DSD_MEMSET(&p, 0, sizeof p);
    DSD_STRNCPY(p.kind, "group", sizeof p.kind - 1);
    p.sid = 100;
    DSD_STRNCPY(p.system_name, "Sys", sizeof p.system_name - 1);
    p.imported_at = 1755500000LL;
    assert(dsd_rr_provenance_write(csv, &p) == 0);

    run();

    char side[DSD_TEST_PATH_MAX + 8];
    (void)DSD_SNPRINTF(side, sizeof side, "%s.rr", csv);
    assert(remove(side) == 0); /* still there: no level deleted anything */
    assert(remove(csv) == 0);
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

    browser_setup();
    test_use_this_system_is_checked_for_its_kind();
    test_load_these_files_is_still_checked();

    /* A blocked plan: the planner's block is said here, a session refusal goes
       to the core, which asks the session again (issue #803). */
    open_system_view();
    g_plan.ok = 0;
    DSD_SNPRINTF(g_plan.blocked_reason, sizeof g_plan.blocked_reason, "%s", "No control channel.");
    g_status[0] = '\0';
    assert(rr_panel_handle_key('\n') == 1);
    assert(g_import_calls == 0);
    assert(strcmp(g_status, "No control channel.") == 0);
    g_session_blocked = 1;
    assert(rr_panel_handle_key('\n') == 1);
    assert(g_import_calls == 1);

    rr_panel_shutdown();
    assert(rr_panel_active() == 0);

    with_one_import(test_browser_and_exit);
    browser_teardown();

    printf("UI_RR_PANEL_KEYS: OK\n");
    return 0;
}
