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
 * And the Imported Systems browser's "Use this system" / "Load these files"
 * over a real imports directory (issue #803): each asks the session gate
 * before it posts anything - the full adoption for the recipe's own decode
 * mode, the files-only load for the channel map it loads - and posts nothing
 * when the gate says no.
 *
 * The wizard core, the prompt widgets and the app-control commands are stubbed
 * at the seams rr_panel.c calls; the panel and the library are the real code.
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "menu_prompts.h"
#include "rr_panel.h"
#include "rr_wizard_core.h"
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

/* The last chooser opened: the browser walks its levels through these callbacks. */
static const char* const* g_chooser_items = NULL;
static int g_chooser_count = 0;
static ui_chooser_done_fn g_chooser_done = NULL;
static void* g_chooser_user = NULL;

void
ui_chooser_start(const char* title, const char* const* items, int count, ui_chooser_done_fn on_done, void* user_ctx) {
    (void)title;
    g_chooser_items = items;
    g_chooser_count = count;
    g_chooser_done = on_done;
    g_chooser_user = user_ctx;
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

static char g_status[256];

void
ui_statusf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)DSD_VSNPRINTF(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
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

/* What the browser posted: the IMPORT_* commands by id, and the apply. */
static int g_import_chan_posts = 0;
static int g_import_group_posts = 0;
static int g_rr_apply_posts = 0;
static dsd_app_rr_apply_payload g_rr_apply_payload;

int
dsd_app_command_set_string(int cmd_id, const char* value) {
    (void)value;
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
   @p want_action. */
static void
browse_first_action(const char* want_action) {
    g_chooser_done = NULL;
    rr_panel_open_library(&g_frame_opts, &g_frame_state);
    assert(g_chooser_done != NULL && g_chooser_count == 1);
    ui_chooser_done_fn pick_system = g_chooser_done;
    void* ctx = g_chooser_user;
    g_chooser_done = NULL;
    pick_system(ctx, 0);
    assert(g_chooser_done != NULL && g_chooser_count == 3);
    assert(strcmp(g_chooser_items[0], want_action) == 0);
    ui_chooser_done_fn pick_action = g_chooser_done;
    g_chooser_done = NULL;
    pick_action(g_chooser_user, 0); /* frees the browser's context */
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
    browser_teardown();

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

    printf("UI_RR_PANEL_KEYS: OK\n");
    return 0;
}
