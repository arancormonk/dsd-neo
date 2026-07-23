// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
* dsd_ncurses_handler.c
* Terminal user input handler
*
* LWVMOBILE
* 2025-05 DSD-FME Florida Man Edition
*-----------------------------------------------------------------------------*/

#include <curses.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/history.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/telemetry.h>
#include <dsd-neo/ui/keymap.h>
#include <dsd-neo/ui/menu_core.h>
#include <dsd-neo/ui/ncurses.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"

typedef struct {
    int key;
    int cmd;
} ncurses_key_cmd_t;

static void
ncurses_drain_escape_sequence(void) {
    int ch2;
    while ((ch2 = getch()) != ERR) {
        (void)ch2;
    }
}

static uint32_t DSD_ATTR_USED
ncurses_resolve_tg_hold_target(const dsd_opts* opts, const dsd_state* state, int right_slot) {
    (void)opts;
    if (state->tg_hold != 0) {
        return 0;
    }

    dsd_call_snapshot call;
    if (dsd_call_state_get(state, (uint8_t)(right_slot != 0), &call) && call.phase != DSD_CALL_PHASE_ENDED) {
        uint64_t target = call.policy_target_id != 0 ? call.policy_target_id : call.ota_target_id;
        if (target == 0 && state->ea_mode == 0 && DSD_SYNC_IS_PROVOICE(call.protocol)) {
            target = call.ota_source_id;
        }
        if (target <= UINT32_MAX) {
            return (uint32_t)target;
        }
    }
    return 0;
}

static int
ncurses_try_post_simple_cmd(int c) {
    static const ncurses_key_cmd_t map[] = {
        {DSD_KEY_MUTE_LOWER, DSD_APP_CMD_TOGGLE_MUTE},
        {DSD_KEY_MUTE_UPPER, DSD_APP_CMD_TOGGLE_MUTE},
        {DSD_KEY_COMPACT, DSD_APP_CMD_TOGGLE_COMPACT},
        {DSD_KEY_SLOT1_TOGGLE, DSD_APP_CMD_SLOT1_TOGGLE},
        {DSD_KEY_SLOT2_TOGGLE, DSD_APP_CMD_SLOT2_TOGGLE},
        {DSD_KEY_SLOT_PREF, DSD_APP_CMD_SLOT_PREF_CYCLE},
        {DSD_KEY_TRUNK_TOGGLE, DSD_APP_CMD_TRUNK_TOGGLE},
        {DSD_KEY_SCANNER_TOGGLE, DSD_APP_CMD_SCANNER_TOGGLE},
        {DSD_KEY_PAYLOAD_TOGGLE, DSD_APP_CMD_PAYLOAD_TOGGLE},
        {DSD_KEY_TOGGLE_P25GA, DSD_APP_CMD_P25_GA_TOGGLE},
        {DSD_KEY_CONST_VIEW_LOWER, DSD_APP_CMD_CONST_TOGGLE},
        {DSD_KEY_CONST_VIEW_UPPER, DSD_APP_CMD_CONST_TOGGLE},
        {DSD_KEY_CONST_NORM, DSD_APP_CMD_CONST_NORM_TOGGLE},
        {DSD_KEY_EYE_VIEW, DSD_APP_CMD_EYE_TOGGLE},
        {DSD_KEY_EYE_UNICODE, DSD_APP_CMD_EYE_UNICODE_TOGGLE},
        {DSD_KEY_EYE_COLOR, DSD_APP_CMD_EYE_COLOR_TOGGLE},
        {DSD_KEY_FSK_HIST, DSD_APP_CMD_FSK_HIST_TOGGLE},
        {DSD_KEY_SPECTRUM, DSD_APP_CMD_SPECTRUM_TOGGLE},
        {DSD_KEY_EH_NEXT, DSD_APP_CMD_EH_NEXT},
        {DSD_KEY_EH_PREV, DSD_APP_CMD_EH_PREV},
        {DSD_KEY_RTL_VOL_CYCLE, DSD_APP_CMD_INPUT_VOL_CYCLE},
        {DSD_KEY_LPF_TOGGLE, DSD_APP_CMD_LPF_TOGGLE},
        {DSD_KEY_HPF_TOGGLE, DSD_APP_CMD_HPF_TOGGLE},
        {DSD_KEY_PBF_TOGGLE, DSD_APP_CMD_PBF_TOGGLE},
        {DSD_KEY_HPF_DIG_TOGGLE, DSD_APP_CMD_HPF_D_TOGGLE},
        {DSD_KEY_AGGR_SYNC, DSD_APP_CMD_AGGR_SYNC_TOGGLE},
        {DSD_KEY_CALL_ALERT, DSD_APP_CMD_CALL_ALERT_TOGGLE},
        {DSD_KEY_INVERT, DSD_APP_CMD_INVERT_TOGGLE},
        {DSD_KEY_MOD_TOGGLE, DSD_APP_CMD_MOD_TOGGLE},
        {DSD_KEY_MOD_P2, DSD_APP_CMD_MOD_P2_TOGGLE},
        {DSD_KEY_DMR_RESET, DSD_APP_CMD_DMR_RESET},
        {DSD_KEY_TRUNK_WLIST, DSD_APP_CMD_TRUNK_WLIST_TOGGLE},
        {DSD_KEY_TRUNK_PRIV, DSD_APP_CMD_TRUNK_PRIV_TOGGLE},
        {DSD_KEY_TRUNK_DATA, DSD_APP_CMD_TRUNK_DATA_TOGGLE},
        {DSD_KEY_TRUNK_ENC, DSD_APP_CMD_TRUNK_ENC_TOGGLE},
        {'g', DSD_APP_CMD_TRUNK_GROUP_TOGGLE},
        {'A', DSD_APP_CMD_PROVOICE_ESK_TOGGLE},
        {'S', DSD_APP_CMD_PROVOICE_MODE_TOGGLE},
        {DSD_KEY_TCP_AUDIO, DSD_APP_CMD_TCP_CONNECT_AUDIO},
        {DSD_KEY_RIGCTL_CONN, DSD_APP_CMD_RIGCTL_CONNECT},
        {DSD_KEY_RETURN_CC, DSD_APP_CMD_RETURN_CC},
        {DSD_KEY_CHANNEL_CYCLE, DSD_APP_CMD_CHANNEL_CYCLE},
        {DSD_KEY_SYMCAP_SAVE, DSD_APP_CMD_SYMCAP_SAVE},
        {DSD_KEY_SYMCAP_STOP, DSD_APP_CMD_SYMCAP_STOP},
        {DSD_KEY_REPLAY_LAST, DSD_APP_CMD_REPLAY_LAST},
        {DSD_KEY_WAV_START, DSD_APP_CMD_WAV_START},
        {DSD_KEY_WAV_STOP, DSD_APP_CMD_WAV_STOP},
        {DSD_KEY_STOP_PLAYBACK, DSD_APP_CMD_STOP_PLAYBACK},
        {DSD_KEY_QUIT, DSD_APP_CMD_QUIT},
        {DSD_KEY_FORCE_PRIV, DSD_APP_CMD_FORCE_PRIV_TOGGLE},
        {DSD_KEY_FORCE_RC4, DSD_APP_CMD_FORCE_RC4_TOGGLE},
        {DSD_KEY_SIM_NOCAR, DSD_APP_CMD_SIM_NOCAR},
    };

    for (size_t i = 0; i < (sizeof(map) / sizeof(map[0])); i++) {
        if (map[i].key != c) {
            continue;
        }
        (void)dsd_app_command_action(map[i].cmd);
        return 1;
    }
    return 0;
}

static int DSD_ATTR_USED
ncurses_handle_escape_or_history(dsd_opts* opts, dsd_state* state, int c) {
    (void)opts;
    (void)state;
    if (c == DSD_KEY_ESC) {
        ncurses_drain_escape_sequence();
        return 1;
    }
    if (c == DSD_KEY_HISTORY) {
        (void)dsd_app_frontend_history_cycle_mode();
        dsd_telemetry_request_redraw();
        return 1;
    }
    return 0;
}

static int DSD_ATTR_USED
ncurses_handle_delta_keys(int c) {
    switch (c) {
        case DSD_KEY_GAIN_PLUS: (void)dsd_app_command_set_i32(DSD_APP_CMD_GAIN_DELTA, +1); return 1;
        case DSD_KEY_GAIN_MINUS: (void)dsd_app_command_set_i32(DSD_APP_CMD_GAIN_DELTA, -1); return 1;
        case DSD_KEY_AGAIN_PLUS: (void)dsd_app_command_set_i32(DSD_APP_CMD_AGAIN_DELTA, +1); return 1;
        case DSD_KEY_AGAIN_MINUS: (void)dsd_app_command_set_i32(DSD_APP_CMD_AGAIN_DELTA, -1); return 1;
        case DSD_KEY_CONST_GATE_DEC: (void)dsd_app_command_set_float(DSD_APP_CMD_CONST_GATE_DELTA, -0.02f); return 1;
        case DSD_KEY_CONST_GATE_INC: (void)dsd_app_command_set_float(DSD_APP_CMD_CONST_GATE_DELTA, +0.02f); return 1;
        case DSD_KEY_PPM_UP: (void)dsd_app_command_set_i32(DSD_APP_CMD_PPM_DELTA, +1); return 1;
        case DSD_KEY_PPM_DOWN: (void)dsd_app_command_set_i32(DSD_APP_CMD_PPM_DELTA, -1); return 1;
#ifdef USE_RTLSDR
        case DSD_KEY_SPEC_DEC: {
            dsd_frontend_metrics metrics;
            (void)dsd_app_frontend_get_metrics(&metrics);
            (void)dsd_app_command_set_i32(DSD_APP_CMD_SPEC_SIZE_DELTA, -(metrics.spectrum_size / 2));
            return 1;
        }
        case DSD_KEY_SPEC_INC: {
            dsd_frontend_metrics metrics;
            (void)dsd_app_frontend_get_metrics(&metrics);
            (void)dsd_app_command_set_i32(DSD_APP_CMD_SPEC_SIZE_DELTA, +metrics.spectrum_size);
            return 1;
        }
#endif
        default: return 0;
    }
}

static int DSD_ATTR_USED
ncurses_handle_tg_hold_keys(const dsd_opts* opts, const dsd_state* state, int c) {
    if (c != DSD_KEY_TG_HOLD1 && c != DSD_KEY_TG_HOLD2) {
        return 0;
    }
    uint32_t tg = ncurses_resolve_tg_hold_target(opts, state, c == DSD_KEY_TG_HOLD2);
    (void)dsd_app_command_set_u32(DSD_APP_CMD_TG_HOLD_SET, tg);
    return 1;
}

static int DSD_ATTR_USED
ncurses_handle_encoder_and_lockout_keys(dsd_opts* opts, dsd_state* state, int c) {
    if (c == DSD_KEY_EH_TOGGLE) {
        (void)dsd_app_command_action(opts->m17encoder == 1 ? DSD_APP_CMD_M17_TX_TOGGLE : DSD_APP_CMD_EH_TOGGLE_SLOT);
        return 1;
    }
    if (c == '!' || c == '@') {
        uint8_t slot = (uint8_t)((c == '@') ? 1 : 0);
        (void)dsd_app_command_set_u8(DSD_APP_CMD_LOCKOUT_SLOT, slot);
        return 1;
    }
    if (c == DSD_KEY_ENTER || c == '\r' || c == KEY_ENTER) {
        if (opts->m17encoder == 0) {
            ui_menu_open_async(opts, state);
        }
        return 1;
    }
    return 0;
}

uint8_t
dsd_terminal_handle_input(dsd_opts* opts, dsd_state* state, int c) {

    if (!opts || !state) {
        return 1;
    }

    // If the nonblocking menu overlay is open, route keys to it first.
    if (ui_menu_is_open()) {
        if (c == -1) {
            return 1;
        }
        return (uint8_t)(ui_menu_handle_key(c, opts, state) || ui_menu_is_open());
    }

    if (ncurses_handle_escape_or_history(opts, state, c)) {
        return 1;
    }
    if (ncurses_handle_tg_hold_keys(opts, state, c)) {
        return 1;
    }
    if (ncurses_handle_delta_keys(c)) {
        return 1;
    }
    if (ncurses_handle_encoder_and_lockout_keys(opts, state, c)) {
        return 1;
    }
    (void)ncurses_try_post_simple_cmd(c);
    return 1;
}
