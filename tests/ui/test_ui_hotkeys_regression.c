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
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/curses_compat.h>
#include <dsd-neo/ui/keymap.h>
#include <dsd-neo/ui/ui_cmd.h>

/* Function under test (compiled from src/ui/terminal/dsd_ncurses_handler.c). */
uint8_t ncurses_input_handler(dsd_opts* opts, dsd_state* state, int c);

/* --- Stubs for external dependencies referenced by dsd_ncurses_handler.c --- */
typedef struct {
    int id;
    size_t n;
    uint8_t data[32];
    int calls;
} UiPostCapture;

static UiPostCapture g_cap;
static int g_redraw_calls = 0;
static int g_history_mode = 1;
static int g_history_cycle_calls = 0;

int
ui_post_cmd(int cmd_id, const void* payload, size_t payload_sz) {
    g_cap.id = cmd_id;
    g_cap.n = payload_sz;
    if (payload_sz > sizeof(g_cap.data)) {
        payload_sz = sizeof(g_cap.data);
    }
    if (payload && payload_sz > 0) {
        memcpy(g_cap.data, payload, payload_sz);
    } else {
        memset(g_cap.data, 0, sizeof(g_cap.data));
    }
    g_cap.calls++;
    return 0;
}

void
ui_request_redraw(void) {
    g_redraw_calls++;
}

int
ui_history_get_mode(void) {
    return g_history_mode;
}

void
ui_history_set_mode(int mode) {
    g_history_mode = mode % 3;
    if (g_history_mode < 0) {
        g_history_mode += 3;
    }
}

int
ui_history_cycle_mode(void) {
    g_history_cycle_calls++;
    ui_history_set_mode(g_history_mode + 1);
    return g_history_mode;
}

int
ui_menu_is_open(void) {
    return 0;
}

int
ui_menu_handle_key(int ch, dsd_opts* opts, dsd_state* state) {
    (void)ch;
    (void)opts;
    (void)state;
    return 0;
}

void
ui_menu_open_async(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

WINDOW* stdscr = NULL;

int
wgetch(WINDOW* win) {
    (void)win;
    return ERR;
}

int
rtl_stream_spectrum_get_size(void) {
    return 512;
}

static void
cap_reset(void) {
    memset(&g_cap, 0, sizeof(g_cap));
    g_redraw_calls = 0;
    g_history_cycle_calls = 0;
    g_history_mode = 1;
}

static uint32_t
cap_u32(void) {
    uint32_t v = 0;
    memcpy(&v, g_cap.data, sizeof(v));
    return v;
}

int
main(void) {
    dsd_opts opts;
    dsd_state state;
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));

    /* 'h' must cycle immediately in UI thread (no command queue dependency). */
    cap_reset();
    opts.ncurses_history = 1;
    assert(ncurses_input_handler(&opts, &state, DSD_KEY_HISTORY) == 1);
    assert(ui_history_get_mode() == 2);
    assert(g_cap.calls == 0);
    assert(g_history_cycle_calls == 1);
    assert(g_redraw_calls == 1);
    assert(ncurses_input_handler(&opts, &state, DSD_KEY_HISTORY) == 1);
    assert(ui_history_get_mode() == 0);
    assert(g_cap.calls == 0);
    assert(g_history_cycle_calls == 2);
    assert(g_redraw_calls == 2);

    /* 'k' should set hold from slot-1 TG when no hold is active. */
    cap_reset();
    state.tg_hold = 0;
    state.lasttg = 1001;
    opts.frame_nxdn48 = 0;
    opts.frame_nxdn96 = 0;
    opts.frame_provoice = 0;
    assert(ncurses_input_handler(&opts, &state, DSD_KEY_TG_HOLD1) == 1);
    assert(g_cap.calls == 1);
    assert(g_cap.id == UI_CMD_TG_HOLD_SET);
    assert(g_cap.n == sizeof(uint32_t));
    assert(cap_u32() == 1001U);

    /* 'k' should clear hold (post 0) when hold is already active. */
    cap_reset();
    state.tg_hold = 4242;
    state.lasttg = 9999;
    assert(ncurses_input_handler(&opts, &state, DSD_KEY_TG_HOLD1) == 1);
    assert(g_cap.id == UI_CMD_TG_HOLD_SET);
    assert(cap_u32() == 0U);

    /* 'l' should set hold from slot-2 TG when no hold is active. */
    cap_reset();
    state.tg_hold = 0;
    state.lasttgR = 2002;
    assert(ncurses_input_handler(&opts, &state, DSD_KEY_TG_HOLD2) == 1);
    assert(g_cap.id == UI_CMD_TG_HOLD_SET);
    assert(cap_u32() == 2002U);

    /* NXDN fallback path for slot-1 hold when DMR/P25 TG is absent. */
    cap_reset();
    state.tg_hold = 0;
    state.lasttg = 0;
    state.nxdn_last_tg = 3003;
    opts.frame_nxdn48 = 1;
    opts.frame_nxdn96 = 0;
    opts.frame_provoice = 0;
    assert(ncurses_input_handler(&opts, &state, DSD_KEY_TG_HOLD1) == 1);
    assert(g_cap.id == UI_CMD_TG_HOLD_SET);
    assert(cap_u32() == 3003U);

    /* ProVoice fallback path for slot-2 hold when TG is absent. */
    cap_reset();
    state.tg_hold = 0;
    state.lasttgR = 0;
    state.lastsrcR = 4004;
    state.ea_mode = 0;
    opts.frame_nxdn48 = 0;
    opts.frame_nxdn96 = 0;
    opts.frame_provoice = 1;
    assert(ncurses_input_handler(&opts, &state, DSD_KEY_TG_HOLD2) == 1);
    assert(g_cap.id == UI_CMD_TG_HOLD_SET);
    assert(cap_u32() == 4004U);

    printf("UI_HOTKEYS_REGRESSION: OK\n");
    return 0;
}
