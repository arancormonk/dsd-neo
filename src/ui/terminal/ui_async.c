// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <curses.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/history.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/curses_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/ui/menu_core.h>
#include <dsd-neo/ui/ncurses.h>
#include <dsd-neo/ui/ui_async.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"

// Minimal thread state.
static dsd_thread_t g_ui_thread;
static atomic_int g_ui_running = 0;
static atomic_int g_ui_stop = 0;
static dsd_opts* g_ui_opts = NULL;
static dsd_state* g_ui_state = NULL;
static int g_ui_curses_cfg_done = 0;
static atomic_int g_ui_in_context = 0;

int
ui_is_thread_context(void) {
    return atomic_load(&g_ui_in_context);
}

static void
ui_control_pump(dsd_opts* opts, dsd_state* state) {
    (void)dsd_app_drain_cmds(opts, state);
}

static const dsd_opts*
ui_get_opts_snapshot_or_default(void) {
    const dsd_opts* osnap = dsd_app_get_latest_opts_snapshot();
    if (!osnap) {
        osnap = g_ui_opts;
    }
    return osnap;
}

static int
ui_curses_is_active(const dsd_opts* osnap) {
    return (dsd_opts_frontend_is_terminal(osnap) && stdscr != NULL);
}

static void
ui_configure_curses_once(void) {
    if (g_ui_curses_cfg_done) {
        return;
    }
    dsd_curses_set_escdelay(25);
    keypad(stdscr, TRUE);
    timeout(0);
    g_ui_curses_cfg_done = 1;
}

static int
ui_read_key_nonblocking(const dsd_opts* osnap) {
    if (osnap->audio_in_type == AUDIO_IN_STDIN) {
        return ERR;
    }
    return getch();
}

static void
ui_handle_menu_input(int ch) {
    if (ch != ERR) {
        ui_menu_handle_key(ch, g_ui_opts, g_ui_state);
    }
    ui_menu_tick(g_ui_opts, g_ui_state);
    ui_commit_frame();
}

static void
ui_handle_normal_input(int ch) {
    if (ch == KEY_RESIZE) {
#if DSD_CURSES_NEEDS_EXPLICIT_RESIZE
        // PDCurses doesn't auto-update dimensions on resize;
        // resize_term(0,0) queries actual console size.
        resize_term(0, 0);
#endif
        clearok(stdscr, TRUE);
        dsd_app_request_redraw();
        return;
    }
    if (ch != ERR) {
        (void)ncurses_input_handler(g_ui_opts, g_ui_state, ch);
    }
}

static void
ui_process_input_frame(const dsd_opts* osnap) {
    ui_configure_curses_once();
    int ch = ui_read_key_nonblocking(osnap);
    if (ui_menu_is_open()) {
        ui_handle_menu_input(ch);
        return;
    }
    ui_handle_normal_input(ch);
}

static int
ui_open_curses_if_needed(void) {
    if (!dsd_opts_frontend_is_terminal(g_ui_opts)) {
        return 0;
    }
    ncursesOpen(g_ui_opts, g_ui_state);
    return 1;
}

static void
ui_close_curses_if_opened(int curses_opened) {
    if (curses_opened) {
        ncursesClose();
    }
}

static void
ui_draw_frame(const dsd_opts* osnap) {
    /* Draw using a state snapshot when available */
    const dsd_state* snap = dsd_app_get_latest_snapshot();
    if (snap) {
        ncursesPrinter((dsd_opts*)osnap, (dsd_state*)snap);
    } else {
        ncursesPrinter((dsd_opts*)osnap, g_ui_state);
    }
}

static void
ui_draw_if_needed(const dsd_opts* osnap, uint64_t* last_draw_ns, uint64_t frame_ns) {
    uint64_t now_ns = dsd_time_monotonic_ns();
    uint64_t dt_ns = now_ns - *last_draw_ns;
    if (!(dsd_app_consume_redraw_requested() || dt_ns >= frame_ns)) {
        return;
    }
    atomic_store(&g_ui_in_context, 1);
    ui_draw_frame(osnap);
    atomic_store(&g_ui_in_context, 0);
    ui_commit_frame();
    *last_draw_ns = now_ns;
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_neo_ui_async_test_set_context(dsd_opts* opts, dsd_state* state) {
    g_ui_opts = opts;
    g_ui_state = state;
    g_ui_curses_cfg_done = 0;
    (void)dsd_app_consume_redraw_requested();
    atomic_store(&g_ui_in_context, 0);
}

const dsd_opts*
dsd_neo_ui_async_test_opts_snapshot_or_default(void) {
    return ui_get_opts_snapshot_or_default();
}

int
dsd_neo_ui_async_test_curses_is_active(const dsd_opts* opts) {
    return ui_curses_is_active(opts);
}

int
dsd_neo_ui_async_test_read_key_nonblocking(const dsd_opts* opts) {
    return ui_read_key_nonblocking(opts);
}

void
dsd_neo_ui_async_test_process_input_frame(const dsd_opts* opts) {
    ui_process_input_frame(opts);
}

int
dsd_neo_ui_async_test_open_curses_if_needed(void) {
    return ui_open_curses_if_needed();
}

void
dsd_neo_ui_async_test_close_curses_if_opened(int curses_opened) {
    ui_close_curses_if_opened(curses_opened);
}

void
dsd_neo_ui_async_test_draw_if_needed(const dsd_opts* opts, uint64_t* last_draw_ns, uint64_t frame_ns) {
    if (last_draw_ns == NULL) {
        return;
    }
    ui_draw_if_needed(opts, last_draw_ns, frame_ns);
}
#endif

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    ui_thread_main(void* arg) {
    (void)arg;

    const unsigned int sleep_ms = 15; // ~15 ms sleep cadence

    // Initialize ncurses lifecycle in UI thread
    int curses_opened = ui_open_curses_if_needed();

    uint64_t last_draw_ns = dsd_time_monotonic_ns();
    const uint64_t frame_ns = 66ULL * 1000ULL * 1000ULL; // ~15 FPS cap

    while (!atomic_load(&g_ui_stop)) {
        // Input + overlays are single-owner in the UI thread.
        const dsd_opts* osnap = ui_get_opts_snapshot_or_default();
        if (ui_curses_is_active(osnap)) {
            ui_process_input_frame(osnap);
            ui_draw_if_needed(osnap, &last_draw_ns, frame_ns);
        }

        dsd_sleep_ms(sleep_ms);
    }

    ui_close_curses_if_opened(curses_opened);

    DSD_THREAD_RETURN;
}

int
ui_start(dsd_opts* opts, dsd_state* state) {
    if (atomic_load(&g_ui_running)) {
        return 0; // already running
    }

    dsd_app_install_telemetry_hooks();
    g_ui_opts = opts;
    g_ui_state = state;
    ui_history_set_mode(opts ? opts->frontend_display.terminal_history : 1);
    atomic_store(&g_ui_stop, 0);

    if (dsd_thread_create(&g_ui_thread, ui_thread_main, NULL) != 0) {
        g_ui_opts = NULL;
        g_ui_state = NULL;
        return -1;
    }

    dsd_runtime_set_control_pump(ui_control_pump);
    atomic_store(&g_ui_running, 1);
    return 0;
}

void
ui_stop(void) {
    if (!atomic_load(&g_ui_running)) {
        return;
    }
    dsd_runtime_set_control_pump(NULL);
    atomic_store(&g_ui_stop, 1);
    dsd_thread_join(g_ui_thread);
    atomic_store(&g_ui_running, 0);
    g_ui_opts = NULL;
    g_ui_state = NULL;
}
