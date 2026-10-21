// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/scan_row_view.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <stdint.h>
#include "dsd-neo/ui/menu_core.h"
#include "menu_actions.h"
#include "menu_prompts.h"

/* One prompt chain at a time. Its destination is captured before any chooser opens. */
static struct {
    dsd_app_receive_payload value;
    dsd_app_scan_row_view row;
    dsd_receive_policy defaults;
    dsd_receive_policy effective;
    int row_scope;
    char title[96];
} receive_editor;

static void
receive_submit(int action) {
    char why[160];
    dsd_receive_policy check = {0};
    const dsd_app_receive_payload* p = &receive_editor.value;
    if (action == DSD_SCAN_ROW_EDIT_SET
        && dsd_receive_edit_parse(&check, p->kind, p->enabled, p->allow, p->block, p->names, why, sizeof why)) {
        ui_statusf("Refused: %s", why);
        return;
    }
    if (receive_editor.row_scope) {
        dsd_app_scan_row_edit_payload edit;
        if (dsd_app_scan_row_view_payload(&receive_editor.row, (uint32_t)DSD_SCAN_ROW_FIELD_SLOT << p->kind, action,
                                          &edit)
            == 0) {
            edit.receive = *p;
            (void)dsd_app_command_scan_row_edit(&edit);
        }
    } else if (action == DSD_SCAN_ROW_EDIT_SET) {
        (void)dsd_app_command_submit(DSD_APP_CMD_RECEIVE_SET, p, sizeof(*p));
    }
}

static void
receive_block_done(void* context, const char* text) {
    (void)context;
    if (text) {
        DSD_SNPRINTF(receive_editor.value.block, sizeof receive_editor.value.block, "%s", text);
        receive_submit(DSD_SCAN_ROW_EDIT_SET);
    }
}

static void
receive_allow_done(void* context, const char* text) {
    if (text) {
        DSD_SNPRINTF(receive_editor.value.allow, sizeof receive_editor.value.allow, "%s", text);
        ui_prompt_open_string_async("Block (/ between identifiers; block wins; empty = none)",
                                    receive_editor.value.block, sizeof receive_editor.value.block, receive_block_done,
                                    context);
    }
}

static void
receive_names_done(void* context, const char* text) {
    (void)context;
    if (text) {
        DSD_SNPRINTF(receive_editor.value.names, sizeof receive_editor.value.names, "%s", text);
        receive_submit(DSD_SCAN_ROW_EDIT_SET);
    }
}

static void
receive_action(void* context, int selected) {
    if (selected < 0) {
        return;
    }
    if (receive_editor.value.kind == DSD_RECEIVE_FILTER_COUNT) {
        if (selected == 0) {
            ui_prompt_open_string_async("Names: --dcc-name \"7=Hospital\" --tone-name \"141.3=Traveler\"",
                                        receive_editor.value.names, sizeof receive_editor.value.names,
                                        receive_names_done, context);
        } else if (selected == 1) {
            receive_editor.value.names[0] = '\0';
            receive_submit(DSD_SCAN_ROW_EDIT_SET);
        } else if (receive_editor.row_scope) {
            receive_submit(selected == 2 ? DSD_SCAN_ROW_EDIT_INHERIT : DSD_SCAN_ROW_EDIT_RESET);
        }
    } else if (selected == 0) {
        ui_prompt_open_string_async("Allow (/ between; NAC in hex; empty = all known)", receive_editor.value.allow,
                                    sizeof receive_editor.value.allow, receive_allow_done, context);
    } else if (selected == 1 || selected == 2) {
        receive_editor.value.enabled = selected == 1;
        receive_submit(DSD_SCAN_ROW_EDIT_SET);
    } else if (receive_editor.row_scope) {
        receive_submit(selected == 3 ? DSD_SCAN_ROW_EDIT_INHERIT : DSD_SCAN_ROW_EDIT_RESET);
    }
}

static void
receive_scope(void* context, int selected) {
    if (selected < 0) {
        return;
    }
    receive_editor.row_scope = selected == 1;
    const dsd_receive_policy* policy = selected == 1 ? &receive_editor.effective : &receive_editor.defaults;
    dsd_app_receive_payload* p = &receive_editor.value;
    static const char* const filters[] = {"Allow / block lists...", "Enable filter", "Disable filter", "Use default",
                                          "List value"};
    static const char* const names[] = {"Edit naming rules...", "Clear service names", "Use default", "List value"};
    if (p->kind == DSD_RECEIVE_FILTER_COUNT) {
        dsd_service_names applicable = {0};
        for (unsigned int i = 0; i < policy->names.count; ++i) {
            const dsd_service_name* rule = &policy->names.rules[i];
            const uint32_t field = rule->kind >= DSD_RECEIVE_CTCSS ? DSD_SCAN_ROW_FIELD_TONE
                                                                   : (uint32_t)DSD_SCAN_ROW_FIELD_SLOT << rule->kind;
            if (!receive_editor.row_scope || (receive_editor.row.editable & field)) {
                applicable.rules[applicable.count++] = *rule;
            }
        }
        (void)dsd_receive_names_format(&applicable, p->names, sizeof p->names);
        ui_chooser_start_at("Service names", names, selected == 1 ? 4 : 2, 0, receive_action, context);
    } else {
        const dsd_receive_filter* filter = &policy->filters[p->kind];
        p->enabled = filter->enabled;
        (void)dsd_receive_set_format(p->kind, &filter->allow, p->allow, sizeof p->allow);
        (void)dsd_receive_set_format(p->kind, &filter->block, p->block, sizeof p->block);
        /* As the menu rows spell them ("DCC filter..."), not the option keys dsd_receive_kind_name() gives. */
        static const char* const kinds[DSD_RECEIVE_FILTER_COUNT] = {"Slot", "DCC", "RAN", "NAC"};
        DSD_SNPRINTF(receive_editor.title, sizeof receive_editor.title, "%s filter [%s]", kinds[p->kind],
                     p->enabled ? "On" : "Off");
        ui_chooser_start_at(receive_editor.title, filters, selected == 1 ? 5 : 3, 0, receive_action, context);
    }
}

static void
receive_open(void* context, int kind) {
    const dsd_opts* opts = dsd_app_get_latest_opts_snapshot();
    const dsd_state* state = dsd_app_get_latest_snapshot();
    if (!opts) {
        ui_statusf("Start the decoder before editing receive settings");
        return;
    }
    DSD_MEMSET(&receive_editor, 0, sizeof receive_editor);
    receive_editor.value.kind = kind;
    receive_editor.effective = opts->receive;
    dsd_scan_mode_configured_receive(opts, state, &receive_editor.defaults);
    (void)dsd_app_scan_row_view_get(opts, state, &receive_editor.row);
    if (dsd_app_scan_row_view_offers(&receive_editor.row, (uint32_t)DSD_SCAN_ROW_FIELD_SLOT << kind)) {
        static const char* const scopes[] = {"All channels", "This channel (session only)"};
        ui_chooser_start_at("Receive setting scope", scopes, 2, 0, receive_scope, context);
    } else {
        receive_scope(context, 0);
    }
}

void
act_receive_slot(void* context) {
    receive_open(context, DSD_RECEIVE_SLOT);
}

void
act_receive_dcc(void* context) {
    receive_open(context, DSD_RECEIVE_DCC);
}

void
act_receive_ran(void* context) {
    receive_open(context, DSD_RECEIVE_RAN);
}

void
act_receive_nac(void* context) {
    receive_open(context, DSD_RECEIVE_NAC);
}

void
act_service_names(void* context) {
    receive_open(context, DSD_RECEIVE_FILTER_COUNT);
}
