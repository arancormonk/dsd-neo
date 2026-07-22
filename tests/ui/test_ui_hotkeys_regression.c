// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression coverage for ncurses hotkeys:
 *  - 'h' (event history cycle) must work without queue drain latency.
 *  - 'k'/'l' (TG hold) must capture slot TG deterministically at keypress time.
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/ui/keymap.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/* Function under test (compiled from src/ui/terminal/dsd_ncurses_handler.c). */
uint8_t dsd_terminal_handle_input(dsd_opts* opts, dsd_state* state, int c);

/* --- Stubs for external dependencies referenced by dsd_ncurses_handler.c --- */
typedef struct {
    int id;
    size_t n;
    uint8_t data[32];
    int calls;
} UiPostCapture;

static void
seed_voice_call(dsd_state* state, uint8_t slot, int protocol, uint64_t target) {
    dsd_call_observation observation = {0};
    observation.protocol = protocol;
    observation.slot = slot;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = target;
    observation.policy_target_id = target;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
}

static UiPostCapture g_cap;
static int g_redraw_calls = 0;
static int g_history_mode = 1;
static int g_history_cycle_calls = 0;
static int g_menu_open = 0;
static int g_menu_handle_key_calls = 0;
static int g_menu_last_key = ERR;
static int g_menu_open_async_calls = 0;

static int
capture_command(int cmd_id, const void* payload, size_t payload_sz) { // NOLINT(misc-use-internal-linkage)
    g_cap.id = cmd_id;
    g_cap.n = payload_sz;
    if (payload_sz > sizeof(g_cap.data)) {
        payload_sz = sizeof(g_cap.data);
    }
    if (payload && payload_sz > 0) {
        DSD_MEMCPY(g_cap.data, payload, payload_sz);
    } else {
        DSD_MEMSET(g_cap.data, 0, sizeof(g_cap.data));
    }
    g_cap.calls++;
    return 0;
}

int
dsd_app_command_action(int cmd_id) { // NOLINT(misc-use-internal-linkage)
    return capture_command(cmd_id, NULL, 0U);
}

int
dsd_app_command_set_i32(int cmd_id, int32_t value) { // NOLINT(misc-use-internal-linkage)
    return capture_command(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_u8(int cmd_id, uint8_t value) { // NOLINT(misc-use-internal-linkage)
    return capture_command(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_u32(int cmd_id, uint32_t value) { // NOLINT(misc-use-internal-linkage)
    return capture_command(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_float(int cmd_id, float value) { // NOLINT(misc-use-internal-linkage)
    return capture_command(cmd_id, &value, sizeof value);
}

void
dsd_telemetry_request_redraw(void) { // NOLINT(misc-use-internal-linkage)
    g_redraw_calls++;
}

int
dsd_app_frontend_history_get_mode(void) { // NOLINT(misc-use-internal-linkage)
    return g_history_mode;
}

void
dsd_app_frontend_history_set_mode(int mode) { // NOLINT(misc-use-internal-linkage)
    g_history_mode = mode % 3;
    if (g_history_mode < 0) {
        g_history_mode += 3;
    }
}

int
dsd_app_frontend_history_cycle_mode(void) { // NOLINT(misc-use-internal-linkage)
    g_history_cycle_calls++;
    dsd_app_frontend_history_set_mode(g_history_mode + 1);
    return g_history_mode;
}

int
ui_menu_is_open(void) { // NOLINT(misc-use-internal-linkage)
    return g_menu_open;
}

int
ui_menu_handle_key(int ch, dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    g_menu_handle_key_calls++;
    g_menu_last_key = ch;
    return 0;
}

void
ui_menu_open_async(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    g_menu_open_async_calls++;
}

WINDOW* stdscr = NULL;

int
wgetch(WINDOW* win) {
    (void)win;
    return ERR;
}

static void
cap_reset(void) {
    DSD_MEMSET(&g_cap, 0, sizeof(g_cap));
    g_redraw_calls = 0;
    g_history_cycle_calls = 0;
    g_history_mode = 1;
    g_menu_open = 0;
    g_menu_handle_key_calls = 0;
    g_menu_last_key = ERR;
    g_menu_open_async_calls = 0;
}

static uint32_t
cap_u32(void) {
    uint32_t v = 0;
    DSD_MEMCPY(&v, g_cap.data, sizeof(v));
    return v;
}

static int32_t
cap_i32(void) {
    int32_t v = 0;
    DSD_MEMCPY(&v, g_cap.data, sizeof(v));
    return v;
}

static float
cap_f32(void) {
    float v = 0.0f;
    DSD_MEMCPY(&v, g_cap.data, sizeof(v));
    return v;
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        free(state);
        free(opts);
        return 1;
    }

    /* Null inputs are treated as consumed and do not enqueue commands. */
    cap_reset();
    assert(dsd_terminal_handle_input(NULL, state, DSD_KEY_MUTE_LOWER) == 1);
    assert(g_cap.calls == 0);
    assert(dsd_terminal_handle_input(opts, NULL, DSD_KEY_MUTE_LOWER) == 1);
    assert(g_cap.calls == 0);

    /* Open menu overlay should receive keys before hotkeys and keep input consumed. */
    cap_reset();
    g_menu_open = 1;
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_HISTORY) == 1);
    assert(g_menu_handle_key_calls == 1);
    assert(g_menu_last_key == DSD_KEY_HISTORY);
    assert(g_history_cycle_calls == 0);
    assert(g_cap.calls == 0);

    /* Open menu overlay should ignore no-key polls without dispatching. */
    cap_reset();
    g_menu_open = 1;
    assert(dsd_terminal_handle_input(opts, state, -1) == 1);
    assert(g_menu_handle_key_calls == 0);
    assert(g_cap.calls == 0);

    /* Escape drains pending bytes and consumes the key without queueing. */
    cap_reset();
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_ESC) == 1);
    assert(g_cap.calls == 0);
    assert(g_history_cycle_calls == 0);

    /* 'h' must cycle immediately in UI thread (no command queue dependency). */
    cap_reset();
    opts->frontend_terminal_display.terminal_history = 1;
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_HISTORY) == 1);
    assert(dsd_app_frontend_history_get_mode() == 2);
    assert(g_cap.calls == 0);
    assert(g_history_cycle_calls == 1);
    assert(g_redraw_calls == 1);
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_HISTORY) == 1);
    assert(dsd_app_frontend_history_get_mode() == 0);
    assert(g_cap.calls == 0);
    assert(g_history_cycle_calls == 2);
    assert(g_redraw_calls == 2);

    /* Delta hotkeys should post signed/floating payloads immediately. */
    cap_reset();
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_GAIN_PLUS) == 1);
    assert(g_cap.id == DSD_APP_CMD_GAIN_DELTA);
    assert(g_cap.n == sizeof(int32_t));
    assert(cap_i32() == 1);

    cap_reset();
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_AGAIN_MINUS) == 1);
    assert(g_cap.id == DSD_APP_CMD_AGAIN_DELTA);
    assert(cap_i32() == -1);

    cap_reset();
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_CONST_GATE_DEC) == 1);
    assert(g_cap.id == DSD_APP_CMD_CONST_GATE_DELTA);
    assert(g_cap.n == sizeof(float));
    assert(cap_f32() < -0.019f && cap_f32() > -0.021f);

    cap_reset();
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_PPM_DOWN) == 1);
    assert(g_cap.id == DSD_APP_CMD_PPM_DELTA);
    assert(cap_i32() == -1);

    /* 'k' should set hold from slot-1 TG when no hold is active. */
    cap_reset();
    state->tg_hold = 0;
    seed_voice_call(state, 0, DSD_SYNC_DMR_BS_VOICE_POS, 1001);
    opts->frame_nxdn48 = 0;
    opts->frame_nxdn96 = 0;
    opts->frame_provoice = 0;
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_TG_HOLD1) == 1);
    assert(g_cap.calls == 1);
    assert(g_cap.id == DSD_APP_CMD_TG_HOLD_SET);
    assert(g_cap.n == sizeof(uint32_t));
    assert(cap_u32() == 1001U);

    /* 'k' should clear hold (post 0) when hold is already active. */
    cap_reset();
    state->tg_hold = 4242;
    seed_voice_call(state, 0, DSD_SYNC_DMR_BS_VOICE_POS, 9999);
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_TG_HOLD1) == 1);
    assert(g_cap.id == DSD_APP_CMD_TG_HOLD_SET);
    assert(cap_u32() == 0U);

    /* 'l' should set hold from slot-2 TG when no hold is active. */
    cap_reset();
    state->tg_hold = 0;
    seed_voice_call(state, 1, DSD_SYNC_DMR_BS_VOICE_POS, 2002);
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_TG_HOLD2) == 1);
    assert(g_cap.id == DSD_APP_CMD_TG_HOLD_SET);
    assert(cap_u32() == 2002U);

    /* NXDN fallback path for slot-1 hold when DMR/P25 TG is absent. */
    cap_reset();
    state->tg_hold = 0;
    seed_voice_call(state, 0, DSD_SYNC_NXDN_POS, 3003);
    opts->frame_nxdn48 = 1;
    opts->frame_nxdn96 = 0;
    opts->frame_provoice = 0;
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_TG_HOLD1) == 1);
    assert(g_cap.id == DSD_APP_CMD_TG_HOLD_SET);
    assert(cap_u32() == 3003U);

    /* ProVoice fallback path for slot-2 hold when TG is absent. */
    cap_reset();
    state->tg_hold = 0;
    seed_voice_call(state, 1, DSD_SYNC_PROVOICE_POS, 4004);
    state->ea_mode = 0;
    opts->frame_nxdn48 = 0;
    opts->frame_nxdn96 = 0;
    opts->frame_provoice = 1;
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_TG_HOLD2) == 1);
    assert(g_cap.id == DSD_APP_CMD_TG_HOLD_SET);
    assert(cap_u32() == 4004U);

    /* Crypto-affecting hotkeys should post the expected command intents. */
    cap_reset();
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_FORCE_PRIV) == 1);
    assert(g_cap.calls == 1);
    assert(g_cap.id == DSD_APP_CMD_FORCE_PRIV_TOGGLE);
    assert(g_cap.n == 0);

    cap_reset();
    assert(dsd_terminal_handle_input(opts, state, '6') == 1);
    assert(g_cap.calls == 1);
    assert(g_cap.id == DSD_APP_CMD_FORCE_RC4_TOGGLE);
    assert(g_cap.n == 0);

    cap_reset();
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_TRUNK_ENC) == 1);
    assert(g_cap.calls == 1);
    assert(g_cap.id == DSD_APP_CMD_TRUNK_ENC_TOGGLE);
    assert(g_cap.n == 0);

    /* Enter opens the menu only when the M17 encoder is not active. */
    cap_reset();
    opts->m17encoder = 0;
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_ENTER) == 1);
    assert(g_menu_open_async_calls == 1);
    assert(g_cap.calls == 0);

    cap_reset();
    opts->m17encoder = 1;
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_ENTER) == 1);
    assert(g_menu_open_async_calls == 0);
    assert(g_cap.calls == 0);

    /* Event-history toggle key is repurposed for M17 TX while encoder mode is active. */
    cap_reset();
    opts->m17encoder = 0;
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_EH_TOGGLE) == 1);
    assert(g_cap.id == DSD_APP_CMD_EH_TOGGLE_SLOT);
    assert(g_cap.n == 0);

    cap_reset();
    opts->m17encoder = 1;
    assert(dsd_terminal_handle_input(opts, state, DSD_KEY_EH_TOGGLE) == 1);
    assert(g_cap.id == DSD_APP_CMD_M17_TX_TOGGLE);
    assert(g_cap.n == 0);

    /* Slot lockout hotkeys should carry the exact slot index. */
    cap_reset();
    assert(dsd_terminal_handle_input(opts, state, '!') == 1);
    assert(g_cap.id == DSD_APP_CMD_LOCKOUT_SLOT);
    assert(g_cap.n == sizeof(uint8_t));
    assert(g_cap.data[0] == 0U);

    cap_reset();
    assert(dsd_terminal_handle_input(opts, state, '@') == 1);
    assert(g_cap.id == DSD_APP_CMD_LOCKOUT_SLOT);
    assert(g_cap.n == sizeof(uint8_t));
    assert(g_cap.data[0] == 1U);

    /* Unknown keys are still consumed but do not enqueue command work. */
    cap_reset();
    assert(dsd_terminal_handle_input(opts, state, '~') == 1);
    assert(g_cap.calls == 0);

    printf("UI_HOTKEYS_REGRESSION: OK\n");
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
