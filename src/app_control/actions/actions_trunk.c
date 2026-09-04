// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* UI command actions — trunking domain */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include "../command_dispatch.h"

#include "dsd-neo/app_control/commands.h"
#include "dsd-neo/core/key_set.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
ui_handle_trunk_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    if (opts->trunk_enable == 1) {
        opts->trunk_enable = 0;
    } else {
        opts->trunk_enable = 1;
    }
    return 1;
}

static int
ui_handle_trunk_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    int32_t want = 0;
    if (c->n < (int)sizeof(int32_t)) {
        /* A truncated payload is not a request to stop trunking. Falling through
         * with want still 0 would hand the tuner back mid-call on a command nobody
         * sent, and the session would quietly stop following the system.
         *
         * Handled-and-declined, not unhandled: 0 means "this table does not know
         * this id", so the drain would keep walking every remaining group with a
         * command they were never meant to see and finally report it as an unknown
         * id rather than a bad payload. ui_handle_mod_set() answers the same
         * condition the same way. */
        return 1;
    }
    DSD_MEMCPY(&want, c->data, sizeof(int32_t));
    const int on = (want != 0) ? 1 : 0;
    opts->trunk_enable = on;
    if (on) {
        // Scanner mode is the other automatic owner of the tuner, and
        // ui_handle_scanner_toggle clears trunking for the same reason: whichever
        // one was asked for last is the one driving, not both at once.
        // Leaving -Y hands the foreground keyring back to the globals.
        dsd_engine_channel_scan_leave(opts, state);
        dsd_scan_keys_leave(state);
        opts->scanner_mode = 0;
    }
    return 1;
}

static int
ui_handle_scanner_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    const int was_scanner = opts->scanner_mode ? 1 : 0;
    opts->scanner_mode = opts->scanner_mode ? 0 : 1;
    opts->trunk_enable = 0;
    if (was_scanner) {
        dsd_engine_channel_scan_leave(opts, state);
        dsd_scan_keys_leave(state);
    }
    return 1;
}

static int
ui_handle_trunk_group_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    if (opts->trunk_enable == 1) {
        opts->trunk_tune_group_calls = opts->trunk_tune_group_calls ? 0 : 1;
    }
    return 1;
}

static int
ui_handle_tg_hold_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    uint8_t slot = 0;
    if (c->n >= 1) {
        slot = c->data[0] & 1;
    }
    if (state->tg_hold != 0) {
        state->tg_hold = 0;
        return 1;
    }

    dsd_call_snapshot call;
    if (dsd_call_state_get(state, slot, &call) && call.phase != DSD_CALL_PHASE_ENDED) {
        uint64_t target = call.policy_target_id != 0 ? call.policy_target_id : call.ota_target_id;
        if (target == 0 && state->ea_mode == 0 && DSD_SYNC_IS_PROVOICE(call.protocol)) {
            target = call.ota_source_id;
        }
        if (target != 0 && target <= UINT32_MAX) {
            state->tg_hold = (uint32_t)target;
        }
    }
    (void)opts;
    return 1;
}

static int
ui_handle_trunk_wlist_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->trunk_use_allow_list = opts->trunk_use_allow_list ? 0 : 1;
    return 1;
}

static int
ui_handle_trunk_priv_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->trunk_tune_private_calls = opts->trunk_tune_private_calls ? 0 : 1;
    return 1;
}

static int
ui_handle_trunk_data_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->trunk_tune_data_calls = opts->trunk_tune_data_calls ? 0 : 1;
    return 1;
}

static int
ui_handle_trunk_enc_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->trunk_tune_enc_calls = opts->trunk_tune_enc_calls ? 0 : 1;
    return 1;
}

static int
ui_handle_enc_lockout_clear(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)opts;
    (void)c;
    dsd_enc_lockout_clear_all(state);
    // Trunk scan parks a ledger copy per target; without this the purge only
    // covers the target currently on air and switching restores the rest.
    dsd_trunk_scan_hook_enc_lockout_clear_snapshots(state);
    return 1;
}

const struct dsd_app_command_reg dsd_app_actions_trunk[] = {
    {DSD_APP_CMD_TRUNK_TOGGLE, ui_handle_trunk_toggle},
    {DSD_APP_CMD_TRUNK_SET, ui_handle_trunk_set},
    {DSD_APP_CMD_SCANNER_TOGGLE, ui_handle_scanner_toggle},
    {DSD_APP_CMD_TRUNK_GROUP_TOGGLE, ui_handle_trunk_group_toggle},
    {DSD_APP_CMD_TG_HOLD_TOGGLE, ui_handle_tg_hold_toggle},
    {DSD_APP_CMD_TRUNK_WLIST_TOGGLE, ui_handle_trunk_wlist_toggle},
    {DSD_APP_CMD_TRUNK_PRIV_TOGGLE, ui_handle_trunk_priv_toggle},
    {DSD_APP_CMD_TRUNK_DATA_TOGGLE, ui_handle_trunk_data_toggle},
    {DSD_APP_CMD_TRUNK_ENC_TOGGLE, ui_handle_trunk_enc_toggle},
    {DSD_APP_CMD_ENC_LOCKOUT_CLEAR, ui_handle_enc_lockout_clear},
    {0, NULL},
};
