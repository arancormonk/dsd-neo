// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #672 (B9): the receive editor (Decoder > Receive filters) opens on the frame's published snapshot pair, the
 * menu's context, and copies what it edits as it opens. It never acquires a snapshot of its own: the accessors below
 * stand for a newer publication that landed during the activation, and nothing may read them. The chooser it opens
 * (what the same frame then renders) and the edit answered in a later frame both come from the frame's pair.
 */

#include <assert.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/scan_row_view.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/ui/menu_core.h"
#include "menu_actions.h"
#include "menu_internal.h"
#include "menu_prompts.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/* A publication newer than the frame's: what a second acquisition mid-frame would read. */
static dsd_opts g_newer_opts;
static dsd_state g_newer_state;
static int g_acquisitions;

static const dsd_opts* g_configured_opts;
static const dsd_state* g_configured_state;
static const dsd_opts* g_row_view_opts;
static const dsd_state* g_row_view_state;

static char g_chooser_title[128];
static ui_chooser_done_fn g_chooser_done;
static void* g_chooser_user;
static int g_chooser_calls;
static char g_status[160];
static int g_submit_id;
static dsd_app_receive_payload g_submitted;

const dsd_opts*
dsd_app_get_latest_opts_snapshot(void) {
    g_acquisitions++;
    return &g_newer_opts;
}

const dsd_state*
dsd_app_get_latest_snapshot(void) {
    g_acquisitions++;
    return &g_newer_state;
}

/* The configured policy is the one in the options it is handed; the activation's publication lands here, in the newer
   copy only. */
void
dsd_scan_mode_configured_receive(const dsd_opts* opts, const dsd_state* state, dsd_receive_policy* out) {
    g_configured_opts = opts;
    g_configured_state = state;
    g_newer_opts.receive.filters[DSD_RECEIVE_DCC].enabled = 0;
    *out = opts->receive;
}

int
dsd_app_scan_row_view_get(const dsd_opts* opts, const dsd_state* state, dsd_app_scan_row_view* out) {
    g_row_view_opts = opts;
    g_row_view_state = state;
    DSD_MEMSET(out, 0, sizeof *out);
    return 0;
}

int
dsd_app_scan_row_view_offers(const dsd_app_scan_row_view* view, uint32_t field) {
    (void)view;
    (void)field;
    return 0;
}

int
dsd_app_scan_row_view_payload(const dsd_app_scan_row_view* view, uint32_t field, int action,
                              dsd_app_scan_row_edit_payload* out) {
    (void)view;
    (void)field;
    (void)action;
    (void)out;
    return -1;
}

int
dsd_receive_set_format(int kind, const dsd_receive_set* set, char* out, size_t capacity) {
    (void)kind;
    (void)set;
    if (capacity > 0U) {
        out[0] = '\0';
    }
    return 0;
}

int
dsd_receive_names_format(const dsd_service_names* names, char* out, size_t capacity) {
    (void)names;
    if (capacity > 0U) {
        out[0] = '\0';
    }
    return 0;
}

int
dsd_receive_edit_parse(dsd_receive_policy* policy, int kind, int enabled, const char* allow, const char* block,
                       const char* names, char* why, size_t why_size) {
    (void)policy;
    (void)kind;
    (void)enabled;
    (void)allow;
    (void)block;
    (void)names;
    (void)why;
    (void)why_size;
    return 0;
}

void
ui_chooser_start_at(const char* title, const char* const* items, int count, int initial_sel, ui_chooser_done_fn on_done,
                    void* user_ctx) {
    (void)items;
    (void)count;
    (void)initial_sel;
    DSD_SNPRINTF(g_chooser_title, sizeof g_chooser_title, "%s", title ? title : "");
    g_chooser_done = on_done;
    g_chooser_user = user_ctx;
    g_chooser_calls++;
}

void
ui_prompt_open_string_async(const char* title, const char* prefill, size_t cap, ui_prompt_string_done_fn on_done,
                            void* user_ctx) {
    (void)title;
    (void)prefill;
    (void)cap;
    (void)on_done;
    (void)user_ctx;
}

void ui_statusf(const char* fmt, ...) DSD_ATTR_FORMAT(printf, 1, 2);

void
ui_statusf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)DSD_VSNPRINTF(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
}

int
dsd_app_command_submit(int cmd_id, const void* payload, size_t payload_sz) {
    g_submit_id = cmd_id;
    if (payload && payload_sz >= sizeof g_submitted) {
        DSD_MEMCPY(&g_submitted, payload, sizeof g_submitted);
    }
    return DSD_APP_COMMAND_SUBMIT_QUEUED;
}

int
dsd_app_command_scan_row_edit(const dsd_app_scan_row_edit_payload* payload) {
    (void)payload;
    return DSD_APP_COMMAND_SUBMIT_QUEUED;
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(&g_newer_opts, 0, sizeof g_newer_opts);
    DSD_MEMSET(&g_newer_state, 0, sizeof g_newer_state);
    opts.receive.filters[DSD_RECEIVE_DCC].enabled = 1;
    g_newer_opts.receive.filters[DSD_RECEIVE_DCC].enabled = 1;
    UiCtx ctx = {.opts = &opts, .state = &state};

    /* The frame's pair seeds the editor; the publication that landed during the activation is not read. */
    act_receive_dcc(&ctx);
    assert(g_acquisitions == 0);
    assert(g_configured_opts == &opts && g_configured_state == &state);
    assert(g_row_view_opts == &opts && g_row_view_state == &state);
    assert(g_chooser_calls == 1);
    assert(strcmp(g_chooser_title, "DCC filter [On]") == 0);

    /* Answered in a later frame, the edit is the one the editor copied, for the filter it opened on. */
    assert(g_chooser_done != NULL);
    g_chooser_done(g_chooser_user, 2); /* Disable filter */
    assert(g_submit_id == DSD_APP_CMD_RECEIVE_SET);
    assert(g_submitted.kind == DSD_RECEIVE_DCC);
    assert(g_submitted.enabled == 0);
    assert(g_acquisitions == 0);

    /* No published options in the frame: nothing to edit, and the editor says so. */
    UiCtx empty = {.opts = NULL, .state = NULL};
    g_chooser_calls = 0;
    act_receive_dcc(&empty);
    assert(g_chooser_calls == 0);
    assert(strcmp(g_status, "Start the decoder before editing receive settings") == 0);
    assert(g_acquisitions == 0);

    printf("UI_MENU_RECEIVE: OK\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
