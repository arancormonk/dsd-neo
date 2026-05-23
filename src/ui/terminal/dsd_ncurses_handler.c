// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
* dsd_ncurses_handler.c
* DSD-FME ncurses terminal user input handler
*
* LWVMOBILE
* 2025-05 DSD-FME Florida Man Edition
*-----------------------------------------------------------------------------*/

#include <curses.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/telemetry.h>
#include <dsd-neo/ui/keymap.h>
#include <dsd-neo/ui/menu_core.h>
#include <dsd-neo/ui/ncurses.h>
#include <dsd-neo/ui/ui_async.h>
#include <dsd-neo/ui/ui_cmd.h>
#include <dsd-neo/ui/ui_history.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#endif

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

static void
ncurses_post_delta_i32(int cmd, int32_t value) {
    int32_t delta = value;
    ui_post_cmd(cmd, &delta, sizeof delta);
}

static void
ncurses_post_delta_f32(int cmd, float value) {
    float delta = value;
    ui_post_cmd(cmd, &delta, sizeof delta);
}

static uint32_t
ncurses_resolve_tg_hold_target(const dsd_opts* opts, const dsd_state* state, int right_slot) {
    uint32_t tg = 0;
    if (state->tg_hold != 0) {
        return tg;
    }

    tg = (uint32_t)(right_slot ? state->lasttgR : state->lasttg);
    if (tg == 0 && (opts->frame_nxdn48 == 1 || opts->frame_nxdn96 == 1)) {
        return (uint32_t)state->nxdn_last_tg;
    }
    if (tg == 0 && opts->frame_provoice == 1 && state->ea_mode == 0) {
        return (uint32_t)(right_slot ? state->lastsrcR : state->lastsrc);
    }
    return tg;
}

static int
ncurses_try_post_simple_cmd(int c) {
    static const ncurses_key_cmd_t map[] = {
        {DSD_KEY_MUTE_LOWER, UI_CMD_TOGGLE_MUTE},
        {DSD_KEY_MUTE_UPPER, UI_CMD_TOGGLE_MUTE},
        {DSD_KEY_COMPACT, UI_CMD_TOGGLE_COMPACT},
        {DSD_KEY_SLOT1_TOGGLE, UI_CMD_SLOT1_TOGGLE},
        {DSD_KEY_SLOT2_TOGGLE, UI_CMD_SLOT2_TOGGLE},
        {DSD_KEY_SLOT_PREF, UI_CMD_SLOT_PREF_CYCLE},
        {DSD_KEY_TRUNK_TOGGLE, UI_CMD_TRUNK_TOGGLE},
        {DSD_KEY_SCANNER_TOGGLE, UI_CMD_SCANNER_TOGGLE},
        {DSD_KEY_PAYLOAD_TOGGLE, UI_CMD_PAYLOAD_TOGGLE},
        {DSD_KEY_TOGGLE_P25GA, UI_CMD_P25_GA_TOGGLE},
        {DSD_KEY_CONST_VIEW_LOWER, UI_CMD_CONST_TOGGLE},
        {DSD_KEY_CONST_VIEW_UPPER, UI_CMD_CONST_TOGGLE},
        {DSD_KEY_CONST_NORM, UI_CMD_CONST_NORM_TOGGLE},
        {DSD_KEY_EYE_VIEW, UI_CMD_EYE_TOGGLE},
        {DSD_KEY_EYE_UNICODE, UI_CMD_EYE_UNICODE_TOGGLE},
        {DSD_KEY_EYE_COLOR, UI_CMD_EYE_COLOR_TOGGLE},
        {DSD_KEY_FSK_HIST, UI_CMD_FSK_HIST_TOGGLE},
        {DSD_KEY_SPECTRUM, UI_CMD_SPECTRUM_TOGGLE},
        {DSD_KEY_EH_NEXT, UI_CMD_EH_NEXT},
        {DSD_KEY_EH_PREV, UI_CMD_EH_PREV},
        {DSD_KEY_RTL_VOL_CYCLE, UI_CMD_INPUT_VOL_CYCLE},
        {DSD_KEY_LPF_TOGGLE, UI_CMD_LPF_TOGGLE},
        {DSD_KEY_HPF_TOGGLE, UI_CMD_HPF_TOGGLE},
        {DSD_KEY_PBF_TOGGLE, UI_CMD_PBF_TOGGLE},
        {DSD_KEY_HPF_DIG_TOGGLE, UI_CMD_HPF_D_TOGGLE},
        {DSD_KEY_AGGR_SYNC, UI_CMD_AGGR_SYNC_TOGGLE},
        {DSD_KEY_CALL_ALERT, UI_CMD_CALL_ALERT_TOGGLE},
        {DSD_KEY_INVERT, UI_CMD_INVERT_TOGGLE},
        {DSD_KEY_MOD_TOGGLE, UI_CMD_MOD_TOGGLE},
        {DSD_KEY_MOD_P2, UI_CMD_MOD_P2_TOGGLE},
        {DSD_KEY_DMR_RESET, UI_CMD_DMR_RESET},
        {DSD_KEY_TRUNK_WLIST, UI_CMD_TRUNK_WLIST_TOGGLE},
        {DSD_KEY_TRUNK_PRIV, UI_CMD_TRUNK_PRIV_TOGGLE},
        {DSD_KEY_TRUNK_DATA, UI_CMD_TRUNK_DATA_TOGGLE},
        {DSD_KEY_TRUNK_ENC, UI_CMD_TRUNK_ENC_TOGGLE},
        {'g', UI_CMD_TRUNK_GROUP_TOGGLE},
        {'A', UI_CMD_PROVOICE_ESK_TOGGLE},
        {'S', UI_CMD_PROVOICE_MODE_TOGGLE},
        {DSD_KEY_TCP_AUDIO, UI_CMD_TCP_CONNECT_AUDIO},
        {DSD_KEY_RIGCTL_CONN, UI_CMD_RIGCTL_CONNECT},
        {DSD_KEY_RETURN_CC, UI_CMD_RETURN_CC},
        {DSD_KEY_CHANNEL_CYCLE, UI_CMD_CHANNEL_CYCLE},
        {DSD_KEY_SYMCAP_SAVE, UI_CMD_SYMCAP_SAVE},
        {DSD_KEY_SYMCAP_STOP, UI_CMD_SYMCAP_STOP},
        {DSD_KEY_REPLAY_LAST, UI_CMD_REPLAY_LAST},
        {DSD_KEY_WAV_START, UI_CMD_WAV_START},
        {DSD_KEY_WAV_STOP, UI_CMD_WAV_STOP},
        {DSD_KEY_STOP_PLAYBACK, UI_CMD_STOP_PLAYBACK},
        {DSD_KEY_QUIT, UI_CMD_QUIT},
        {DSD_KEY_FORCE_PRIV, UI_CMD_FORCE_PRIV_TOGGLE},
        {DSD_KEY_FORCE_RC4, UI_CMD_FORCE_RC4_TOGGLE},
        {DSD_KEY_SIM_NOCAR, UI_CMD_SIM_NOCAR},
    };

    for (size_t i = 0; i < (sizeof(map) / sizeof(map[0])); i++) {
        if (map[i].key != c) {
            continue;
        }
        ui_post_cmd(map[i].cmd, NULL, 0);
        return 1;
    }
    return 0;
}

static int
ncurses_handle_escape_or_history(dsd_opts* opts, dsd_state* state, int c) {
    (void)opts;
    (void)state;
    if (c == DSD_KEY_ESC) {
        ncurses_drain_escape_sequence();
        return 1;
    }
    if (c == DSD_KEY_HISTORY) {
        (void)ui_history_cycle_mode();
        ui_request_redraw();
        return 1;
    }
    return 0;
}

static int
ncurses_handle_delta_keys(int c) {
    switch (c) {
        case DSD_KEY_GAIN_PLUS: ncurses_post_delta_i32(UI_CMD_GAIN_DELTA, +1); return 1;
        case DSD_KEY_GAIN_MINUS: ncurses_post_delta_i32(UI_CMD_GAIN_DELTA, -1); return 1;
        case DSD_KEY_AGAIN_PLUS: ncurses_post_delta_i32(UI_CMD_AGAIN_DELTA, +1); return 1;
        case DSD_KEY_AGAIN_MINUS: ncurses_post_delta_i32(UI_CMD_AGAIN_DELTA, -1); return 1;
        case DSD_KEY_CONST_GATE_DEC: ncurses_post_delta_f32(UI_CMD_CONST_GATE_DELTA, -0.02f); return 1;
        case DSD_KEY_CONST_GATE_INC: ncurses_post_delta_f32(UI_CMD_CONST_GATE_DELTA, +0.02f); return 1;
        case DSD_KEY_PPM_UP: ncurses_post_delta_i32(UI_CMD_PPM_DELTA, +1); return 1;
        case DSD_KEY_PPM_DOWN: ncurses_post_delta_i32(UI_CMD_PPM_DELTA, -1); return 1;
#ifdef USE_RTLSDR
        case DSD_KEY_SPEC_DEC:
            ncurses_post_delta_i32(UI_CMD_SPEC_SIZE_DELTA, -(rtl_stream_spectrum_get_size() / 2));
            return 1;
        case DSD_KEY_SPEC_INC:
            ncurses_post_delta_i32(UI_CMD_SPEC_SIZE_DELTA, +(rtl_stream_spectrum_get_size()));
            return 1;
#endif
        default: return 0;
    }
}

static int
ncurses_handle_tg_hold_keys(const dsd_opts* opts, const dsd_state* state, int c) {
    if (c != DSD_KEY_TG_HOLD1 && c != DSD_KEY_TG_HOLD2) {
        return 0;
    }
    uint32_t tg = ncurses_resolve_tg_hold_target(opts, state, c == DSD_KEY_TG_HOLD2);
    ui_post_cmd(UI_CMD_TG_HOLD_SET, &tg, sizeof tg);
    return 1;
}

static int
ncurses_handle_encoder_and_lockout_keys(dsd_opts* opts, dsd_state* state, int c) {
    if (c == DSD_KEY_EH_TOGGLE) {
        ui_post_cmd(opts->m17encoder == 1 ? UI_CMD_M17_TX_TOGGLE : UI_CMD_EH_TOGGLE_SLOT, NULL, 0);
        return 1;
    }
    if (c == '!' || c == '@') {
        uint8_t slot = (uint8_t)((c == '@') ? 1 : 0);
        ui_post_cmd(UI_CMD_LOCKOUT_SLOT, &slot, sizeof slot);
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
ncurses_input_handler(dsd_opts* opts, dsd_state* state, int c) {

    if (!opts || !state) {
        return 1;
    }

    // If the nonblocking menu overlay is open, route keys to it first.
    if (ui_menu_is_open()) {
        if (c != -1) {
            ui_menu_handle_key(c, opts, state);
        }
        return 1; // consume all keys while menu overlay is active
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
