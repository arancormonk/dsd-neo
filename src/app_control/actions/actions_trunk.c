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
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include "../command_dispatch.h"
#include "../services.h"

#include "dsd-neo/app_control/commands.h"
#include "dsd-neo/core/key_set.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

/* An action handler's results, as the command drain reads them (UI_CMD_APPLY_COMPLETED, UI_CMD_APPLY_FAILED). */
enum { TRUNK_ACTION_DONE = 1, TRUNK_ACTION_REFUSED = 2 };

static void
trunk_action_toast(dsd_state* state, int seconds, const char* text) {
    if (state) {
        DSD_SNPRINTF(state->ui_msg, sizeof(state->ui_msg), "%s", text);
        state->ui_msg_expire = dsd_realtime_time() + seconds;
    }
}

/* A trunk scan sets trunking per target, and its coordinator keeps the tuner whether running or paused: trunking
   turned off under it would leave a target following a call with no recovery, and turned on would follow a paused
   scan's manual frequency as a trunked target (issue #671, B5). */
static int
trunk_refused_under_trunk_scan(const dsd_opts* opts, dsd_state* state) {
    if (opts->trunk_scan_enabled != 1) {
        return 0;
    }
    trunk_action_toast(state, 3, "Trunk scan: each target sets its own trunking");
    return 1;
}

static void
trunk_enable_on(dsd_opts* opts, dsd_state* state) {
    /* Leave the conventional row and transfer tuner ownership as one guarded
     * step: watchdog admission also observes scanner_mode (#554). A paused -Y
     * scan ends here as a running one does (issue #671). */
    p25_sm_tick_guard_enter();
    opts->trunk_enable = 1;
    (void)svc_leave_channel_scan(opts, state);
    dsd_scan_keys_leave(state);
    opts->scanner_mode = 0;
    if (state) {
        state->operator_tuned = 0U; /* trunking owns the tuner again */
    }
    p25_sm_tick_guard_leave();
}

static int
ui_handle_trunk_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (trunk_refused_under_trunk_scan(opts, state)) {
        return TRUNK_ACTION_REFUSED;
    }
    if (opts->trunk_enable == 1) {
        opts->trunk_enable = 0;
    } else {
        trunk_enable_on(opts, state);
    }
    return TRUNK_ACTION_DONE;
}

static int
ui_handle_trunk_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
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
        return TRUNK_ACTION_DONE;
    }
    if (trunk_refused_under_trunk_scan(opts, state)) {
        return TRUNK_ACTION_REFUSED;
    }
    DSD_MEMCPY(&want, c->data, sizeof(int32_t));
    const int on = (want != 0) ? 1 : 0;
    if (on) {
        trunk_enable_on(opts, state);
    } else {
        opts->trunk_enable = 0;
    }
    return TRUNK_ACTION_DONE;
}

/* Resume the paused -Y scan (issue #671). A resume already waiting on its row's tune changes nothing: a second request
   neither tunes again nor ends a call. */
static int
scan_resume_channel(dsd_opts* opts, dsd_state* state) {
    if (state->lcn_scan_resume_pending) {
        trunk_action_toast(state, 3, "Resuming scan: waiting for the tune");
        return TRUNK_ACTION_DONE;
    }
    /* NO_ROW and REFUSED leave the scan paused, the reason on the status line. */
    switch (svc_resume_channel_scan(opts, state)) {
        case DSD_SCAN_RESUME_LANDED: trunk_action_toast(state, 3, "Scan resumed"); return TRUNK_ACTION_DONE;
        case DSD_SCAN_RESUME_PENDING:
            trunk_action_toast(state, 3, "Resuming scan: waiting for the tune");
            return TRUNK_ACTION_DONE;
        default: return TRUNK_ACTION_REFUSED;
    }
}

/* Resume the paused trunk scan on the target it left (issue #671). A park still in flight is running already: the
   target's tune lands as any target's does. A refused retune or failed preparation leaves it paused, with the reason
   on the status line. */
static int
scan_resume_trunk(dsd_opts* opts, dsd_state* state) {
    const int rc = svc_resume_trunk_scan(opts, state);
    if (rc == 0 || rc == 1) {
        trunk_action_toast(state, 3, "Trunk scan resumed");
        return TRUNK_ACTION_DONE;
    }
    if (rc == -1) {
        trunk_action_toast(state, 3, "Trunk scan is not running");
    }
    return TRUNK_ACTION_REFUSED;
}

static int
trunk_scan_is_paused(const dsd_opts* opts, const dsd_state* state) {
    return opts->trunk_scan_enabled == 1 && state->trunk_scan_paused;
}

/* `y`: a paused scan resumes; a running trunk scan pauses where it is (it has no stop at runtime); a running -Y scan
   stops, and a stopped one starts (issue #671). A pending trunk-scan resume is running: `y` pauses it again. */
static int
ui_handle_scanner_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (!state) {
        return TRUNK_ACTION_DONE;
    }
    if (opts->trunk_scan_enabled == 1) {
        if (trunk_scan_is_paused(opts, state)) {
            return scan_resume_trunk(opts, state);
        }
        if (svc_pause_trunk_scan_in_place(opts, state) != 0) {
            trunk_action_toast(state, 3, "Trunk scan active: conventional scanner unavailable");
            return TRUNK_ACTION_REFUSED;
        }
        trunk_action_toast(state, 3, "Trunk scan paused");
        return TRUNK_ACTION_DONE;
    }
    if (state->lcn_scan_paused || state->lcn_scan_resume_pending) {
        return scan_resume_channel(opts, state);
    }
    const int was_scanner = opts->scanner_mode ? 1 : 0;
    opts->scanner_mode = opts->scanner_mode ? 0 : 1;
    opts->trunk_enable = 0;
    if (was_scanner) {
        (void)svc_leave_channel_scan(opts, state);
        dsd_scan_keys_leave(state);
    } else {
        state->operator_tuned = 0U; /* the scan owns the tuner again */
    }
    return TRUNK_ACTION_DONE;
}

/* DSD_APP_CMD_SCAN_RESUME: resume whichever scan is paused (issue #671), and only that: a running scan is left as it
   is, and with no scan paused nothing starts. */
static int
ui_handle_scan_resume(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (!state) {
        return TRUNK_ACTION_DONE;
    }
    if (trunk_scan_is_paused(opts, state)) {
        return scan_resume_trunk(opts, state);
    }
    if (state->lcn_scan_paused || state->lcn_scan_resume_pending) {
        return scan_resume_channel(opts, state);
    }
    if (opts->trunk_scan_enabled == 1 || opts->scanner_mode == 1) {
        trunk_action_toast(state, 3, "Not paused: the scan is running");
        return TRUNK_ACTION_REFUSED;
    }
    trunk_action_toast(state, 3, "Not scanning: no paused scan to resume");
    return TRUNK_ACTION_REFUSED;
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
    {DSD_APP_CMD_SCAN_RESUME, ui_handle_scan_resume},
    {DSD_APP_CMD_TRUNK_GROUP_TOGGLE, ui_handle_trunk_group_toggle},
    {DSD_APP_CMD_TG_HOLD_TOGGLE, ui_handle_tg_hold_toggle},
    {DSD_APP_CMD_TRUNK_WLIST_TOGGLE, ui_handle_trunk_wlist_toggle},
    {DSD_APP_CMD_TRUNK_PRIV_TOGGLE, ui_handle_trunk_priv_toggle},
    {DSD_APP_CMD_TRUNK_DATA_TOGGLE, ui_handle_trunk_data_toggle},
    {DSD_APP_CMD_TRUNK_ENC_TOGGLE, ui_handle_trunk_enc_toggle},
    {DSD_APP_CMD_ENC_LOCKOUT_CLEAR, ui_handle_enc_lockout_clear},
    {0, NULL},
};
