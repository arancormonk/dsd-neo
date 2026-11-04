// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <curses.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/history.h>
#include <dsd-neo/app_control/scan_state_view.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/curses_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/ui/menu_core.h>
#include <dsd-neo/ui/ncurses.h>
#include <dsd-neo/ui/ui_async.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"
#include "rr_panel.h"
#include "ui_keys.h"
#include "ui_mouse.h"

/* At most one wheel step per 150 ms: a touchpad flick sends dozens of notches, and the command queue drops its
   oldest entry when full. Notches inside the gap are dropped, not queued. */
#define UI_WHEEL_STEP_GAP_NS    (150ULL * 1000ULL * 1000ULL)
/* At most this many reads after a wheel report in one input pass (ui_read_past_wheel_burst()); the bound only keeps a
   stream that never runs dry from holding the pass. */
#define UI_WHEEL_BURST_READ_MAX 256

// Minimal thread state.
static dsd_thread_t g_ui_thread;
static atomic_int g_ui_running = 0;
static atomic_int g_ui_stop = 0;
/* The few start-time options the frame needs before the decoder's first publication (whether curses runs, whether
   keys are read, whether opening it warns about -Z), copied by ui_start() so the UI thread never reads the decoder's
   live options. The rest stays zero. Static: a dsd_opts is too large for a stack. */
static dsd_opts g_ui_start_opts;
static const dsd_opts* g_ui_opts = NULL;
static int g_ui_curses_cfg_done = 0;
static atomic_int g_ui_in_context = 0;
// UI thread only: when the last wheel notch stepped the channel.
static uint64_t g_ui_wheel_step_ns = 0;
static int g_ui_wheel_stepped = 0;

int
ui_is_thread_context(void) {
    return atomic_load(&g_ui_in_context);
}

/*
 * The frame's snapshot pair (issue #672, B9): acquired once at the start of each frame, here and nowhere else in the
 * terminal UI, and handed to the key path (the hotkeys, the menu, its labels, actions and the callbacks of prompts and
 * choosers answered in this frame) and to the draw. Both accessors refresh their single consumer's buffers in place, so
 * a second acquisition mid-frame would change what the rest of the frame reads; nothing else acquires. Before the first
 * publication (runtime start publishes before this thread exists, so practically never) the state is NULL, which the
 * key path honours as Quit only and the draw as nothing to draw, and the options fall back to the start-time copy
 * ui_start() made (g_ui_start_opts), read only to tell whether curses runs (and warns about -Z) and keys are read. The
 * decoder's live structs are never read on this thread.
 */
typedef struct {
    const dsd_opts* opts;
    const dsd_state* state;
} UiFrame;

/* Keep the start-time fields the frame reads before the first publication. Before the UI thread exists. */
static void
ui_keep_start_opts(const dsd_opts* opts) {
    DSD_MEMSET(&g_ui_start_opts, 0, sizeof g_ui_start_opts);
    if (opts == NULL) {
        g_ui_opts = NULL;
        return;
    }
    g_ui_start_opts.frontend_kind = opts->frontend_kind;
    g_ui_start_opts.audio_in_type = opts->audio_in_type;
    g_ui_start_opts.payload = opts->payload; /* dsd_terminal_open()'s -Z warning */
    g_ui_start_opts.frontend_terminal_display.mouse_wheel = opts->frontend_terminal_display.mouse_wheel;
    g_ui_opts = &g_ui_start_opts;
}

static UiFrame
ui_frame_acquire(void) {
    UiFrame frame;
    frame.opts = dsd_app_get_latest_opts_snapshot();
    frame.state = dsd_app_get_latest_snapshot();
    if (!frame.opts) {
        frame.opts = g_ui_opts;
    }
    return frame;
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
    return ui_read_key();
}

static void
ui_handle_menu_input(int ch, const dsd_opts* osnap, const dsd_state* ssnap) {
    if (ch != ERR) {
        ui_menu_handle_key(ch, osnap, ssnap);
    }
    ui_menu_tick(osnap, ssnap);
    ui_commit_frame();
}

static void
ui_handle_normal_input(int ch, const dsd_opts* osnap, const dsd_state* ssnap) {
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
        (void)dsd_terminal_handle_input(osnap, ssnap, ch);
    }
}

/*
 * The wheel steps channels only while the setting is on, a -Y or --trunk-scan rotation runs (a paused scan does not),
 * no menu, prompt or other overlay is open (the `=` prompt is one), keys are read at all (not with STDIN input), the
 * input is no I/Q replay (which refuses every retune, so a notch could only toast) and the terminal can report the
 * wheel. Decided from the frame's snapshots, never the decoder's live structs.
 */
static int
ui_wheel_capture_wanted(const dsd_opts* osnap, const dsd_state* ssnap) {
    if (osnap->frontend_terminal_display.mouse_wheel == 0 || osnap->audio_in_type == AUDIO_IN_STDIN
        || dsd_opts_input_is_iq_replay(osnap)) {
        return 0;
    }
    if (ui_menu_is_open()) {
        return 0;
    }
    dsd_app_scan_state_view scan;
    if (dsd_app_scan_state_view_get(osnap, ssnap, &scan) != 0 || !scan.running) {
        return 0;
    }
    return ui_mouse_wheel_available(NULL) ? 1 : 0;
}

static int
ui_key_is_wheel(int ch) {
    return ch == UI_KEY_WHEEL_UP || ch == UI_KEY_WHEEL_DOWN;
}

/* Wheel up (away from the user) is Previous and down is Next, as Up and Down move through a list. */
static void
ui_handle_wheel(int ch, int eligible) {
    if (!eligible) {
        return;
    }
    const uint64_t now_ns = dsd_realtime_mono_ns();
    if (g_ui_wheel_stepped && now_ns - g_ui_wheel_step_ns < UI_WHEEL_STEP_GAP_NS) {
        return;
    }
    g_ui_wheel_stepped = 1;
    g_ui_wheel_step_ns = now_ns;
    (void)dsd_app_command_action(ch == UI_KEY_WHEEL_UP ? DSD_APP_CMD_CHANNEL_CYCLE_PREV : DSD_APP_CMD_CHANNEL_CYCLE);
}

/*
 * The reports already queued behind a wheel report arrived with it: one gesture, which steps once at most. Read one a
 * frame, a flick's dozens would outlast the throttle and go on stepping after the wheel stopped, so they are read now,
 * without waiting for more. The first key that is not a wheel report ends the run and is returned, to be handled in
 * this pass in the order it was typed; ERR when the input ran dry. A click reads as ERR too and ends the run early.
 */
static int
ui_read_past_wheel_burst(const dsd_opts* osnap) {
    for (int i = 0; i < UI_WHEEL_BURST_READ_MAX; i++) {
        const int ch = ui_read_key_nonblocking(osnap);
        if (!ui_key_is_wheel(ch)) {
            return ch;
        }
    }
    return ERR;
}

static void
ui_wheel_forget(void) {
    g_ui_wheel_stepped = 0;
    g_ui_wheel_step_ns = 0;
}

static void
ui_process_input_frame(const dsd_opts* osnap, const dsd_state* ssnap) {
    ui_configure_curses_once();
    const int wheel_wanted = ui_wheel_capture_wanted(osnap, ssnap);
    ui_mouse_capture_set(wheel_wanted);
    int ch = ui_read_key_nonblocking(osnap);
    if (ui_key_is_wheel(ch)) {
        // Handled here: dsd_terminal_handle_input() and the menu never see a wheel code.
        ui_handle_wheel(ch, wheel_wanted && ui_mouse_capture_is_on());
        ch = ui_read_past_wheel_burst(osnap);
    }
    if (ui_menu_is_open()) {
        ui_handle_menu_input(ch, osnap, ssnap);
        return;
    }
    ui_handle_normal_input(ch, osnap, ssnap);
}

static int
ui_open_curses_if_needed(void) {
    const UiFrame frame = ui_frame_acquire();
    if (!dsd_opts_frontend_is_terminal(frame.opts)) {
        return 0;
    }
    dsd_terminal_open(frame.opts, NULL);
    return 1;
}

static void
ui_close_curses_if_opened(int curses_opened) {
    if (curses_opened) {
        ui_mouse_capture_close();
        ui_wheel_forget();
        dsd_terminal_close();
    }
}

/* The frame's pair, which the printer reads only; nothing is drawn before the first publication. */
static void
ui_draw_frame(const dsd_opts* osnap, const dsd_state* ssnap) {
    dsd_terminal_render((dsd_opts*)osnap, (dsd_state*)ssnap);
}

static void
ui_draw_if_needed(const dsd_opts* osnap, const dsd_state* ssnap, uint64_t* last_draw_ns, uint64_t frame_ns) {
    uint64_t now_ns = dsd_realtime_mono_ns();
    uint64_t dt_ns = now_ns - *last_draw_ns;
    if (!(dsd_app_frontend_redraw_consume() || dt_ns >= frame_ns)) {
        return;
    }
    atomic_store(&g_ui_in_context, 1);
    ui_draw_frame(osnap, ssnap);
    atomic_store(&g_ui_in_context, 0);
    ui_commit_frame();
    *last_draw_ns = now_ns;
}

/* One frame of the UI thread: one acquisition, shared by the key path and the draw. */
static void
ui_run_frame(uint64_t* last_draw_ns, uint64_t frame_ns) {
    const UiFrame frame = ui_frame_acquire();
    if (ui_curses_is_active(frame.opts)) {
        ui_process_input_frame(frame.opts, frame.state);
        ui_draw_if_needed(frame.opts, frame.state, last_draw_ns, frame_ns);
    }
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_neo_ui_async_test_set_context(const dsd_opts* opts, const dsd_state* state) {
    (void)state; /* the frame reads only the published state */
    ui_keep_start_opts(opts);
    g_ui_curses_cfg_done = 0;
    (void)dsd_app_frontend_redraw_consume();
    atomic_store(&g_ui_in_context, 0);
    ui_mouse_reset_for_test();
    ui_wheel_forget();
}

const dsd_opts*
dsd_neo_ui_async_test_opts_snapshot_or_default(void) {
    return ui_frame_acquire().opts;
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
    ui_process_input_frame(opts, ui_frame_acquire().state);
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
    ui_draw_if_needed(opts, ui_frame_acquire().state, last_draw_ns, frame_ns);
}

void
dsd_neo_ui_async_test_run_frame(uint64_t* last_draw_ns, uint64_t frame_ns) {
    if (last_draw_ns == NULL) {
        return;
    }
    ui_run_frame(last_draw_ns, frame_ns);
}
#endif

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    ui_thread_main(void* arg) {
    (void)arg;

    const unsigned int sleep_ms = 15; // ~15 ms sleep cadence

    /* Ctrl+Z is this thread's: curses' SIGTSTP handler ends the screen, stops, and redraws it at the resume, while this
       thread draws it every frame. Run on another thread, the handler's redraw overlapped this thread's, and the two
       curses updates, each of which ignores SIGTSTP for its length and then restores the action it saved, could leave
       the signal ignored for good. Every other thread starts with it blocked (dsd_thread_create(), and the session
       thread the CLI blocks before the run), so the kernel hands a Ctrl+Z only to this one. */
    (void)dsd_thread_block_stop_signal(0);

    // Initialize ncurses lifecycle in UI thread
    int curses_opened = ui_open_curses_if_needed();

    uint64_t last_draw_ns = dsd_realtime_mono_ns();
    const uint64_t frame_ns = 66ULL * 1000ULL * 1000ULL; // ~15 FPS cap

    while (!atomic_load(&g_ui_stop)) {
        // Input + overlays are single-owner in the UI thread.
        ui_run_frame(&last_draw_ns, frame_ns);

        dsd_sleep_ms(sleep_ms);
    }

    /* RadioReference first (it closes a chooser borrowing the wizard's strings before freeing them), then every other
       overlay, owned contexts disposed, while curses still runs. */
    rr_panel_shutdown();
    ui_overlays_shutdown();
    ui_close_curses_if_opened(curses_opened);

    DSD_THREAD_RETURN;
}

int
ui_start(const dsd_opts* opts, const dsd_state* state) {
    if (atomic_load(&g_ui_running)) {
        return 0; // already running
    }

    dsd_app_frontend_runtime_start(opts, state);
    ui_keep_start_opts(opts);
    dsd_app_frontend_history_set_mode(opts ? opts->frontend_terminal_display.terminal_history : 1);
    atomic_store(&g_ui_stop, 0);

    if (dsd_thread_create(&g_ui_thread, ui_thread_main, NULL) != 0) {
        dsd_app_frontend_runtime_stop();
        g_ui_opts = NULL;
        return -1;
    }

    atomic_store(&g_ui_running, 1);
    return 0;
}

void
ui_stop(void) {
    if (!atomic_load(&g_ui_running)) {
        return;
    }
    dsd_app_frontend_runtime_stop();
    atomic_store(&g_ui_stop, 1);
    dsd_thread_join(g_ui_thread);
    atomic_store(&g_ui_running, 0);
    g_ui_opts = NULL;
}
