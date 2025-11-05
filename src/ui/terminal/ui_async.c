// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/ui/ui_async.h>

#include <ncurses.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/ui/menu_core.h>
#include <dsd-neo/ui/ui_opts_snapshot.h>
#include <dsd-neo/ui/ui_snapshot.h>

// Minimal thread state.
static pthread_t g_ui_thread;
static atomic_int g_ui_running = 0;
static atomic_int g_ui_stop = 0;
static atomic_int g_ui_dirty = 0; // notifier for redraw requests
static dsd_opts* g_ui_opts = NULL;
static dsd_state* g_ui_state = NULL;
static int g_ui_curses_cfg_done = 0;
static atomic_int g_ui_in_context = 0;

int
ui_is_thread_context(void) {
    return atomic_load(&g_ui_in_context);
}

void
ui_publish_both_and_redraw(const dsd_opts* opts, const dsd_state* state) {
    if (opts) {
        ui_publish_opts_snapshot(opts);
    }
    if (state) {
        ui_publish_snapshot(state);
    }
    ui_request_redraw();
}

static void*
ui_thread_main(void* arg) {
    (void)arg;

    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 15L * 1000L * 1000L; // ~15 ms sleep cadence

    // Initialize ncurses lifecycle in UI thread
    if (g_ui_opts && g_ui_opts->use_ncurses_terminal == 1) {
        ncursesOpen(g_ui_opts, g_ui_state);
    }

    struct timespec last_draw = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &last_draw);
    const long frame_ns = 66L * 1000L * 1000L; // ~15 FPS cap

    while (!atomic_load(&g_ui_stop)) {
        // Input + overlays handled in the UI thread when curses is ready.
        const dsd_opts* osnap = ui_get_latest_opts_snapshot();
        if (!osnap) {
            osnap = g_ui_opts;
        }
        if (osnap && osnap->use_ncurses_terminal == 1 && stdscr != NULL) {
            // One-time input config in UI thread (ESC delay, keypad, nonblocking getch)
            if (!g_ui_curses_cfg_done) {
                set_escdelay(25);
                keypad(stdscr, TRUE);
                timeout(0);
                g_ui_curses_cfg_done = 1;
            }

            int ch = ERR;
            if (osnap->audio_in_type != 1) { // Avoid getch when stdin is input
                ch = getch();
            }

            if (ui_menu_is_open()) {
                if (ch != ERR) {
                    ui_menu_handle_key(ch, g_ui_opts, g_ui_state);
                }
                // Keep overlays updated each frame
                ui_menu_tick(g_ui_opts, g_ui_state);
            } else {
                if (ch == KEY_RESIZE) {
                    clearok(stdscr, TRUE);
                    ui_request_redraw();
                } else if (ch != ERR) {
                    (void)ncurses_input_handler(g_ui_opts, g_ui_state, ch);
                }
            }
        }

        // Draw on dirty or FPS tick when curses is active
        if (osnap && osnap->use_ncurses_terminal == 1 && stdscr != NULL) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long dt_ns = (now.tv_sec - last_draw.tv_sec) * 1000000000L + (now.tv_nsec - last_draw.tv_nsec);
            if (atomic_exchange(&g_ui_dirty, 0) || dt_ns >= frame_ns) {
                atomic_store(&g_ui_in_context, 1);
                /* Draw using a state snapshot when available */
                const dsd_state* snap = ui_get_latest_snapshot();
                if (snap) {
                    ncursesPrinter((dsd_opts*)osnap, (dsd_state*)snap);
                } else {
                    ncursesPrinter((dsd_opts*)osnap, g_ui_state);
                }
                atomic_store(&g_ui_in_context, 0);
                last_draw = now;
            }
        }

        nanosleep(&ts, NULL);
    }

    if (g_ui_opts && g_ui_opts->use_ncurses_terminal == 1) {
        ncursesClose();
    }

    return NULL;
}

int
ui_start(dsd_opts* opts, dsd_state* state) {
    if (atomic_load(&g_ui_running)) {
        return 0; // already running
    }

    g_ui_opts = opts;
    g_ui_state = state;
    atomic_store(&g_ui_stop, 0);

    if (pthread_create(&g_ui_thread, NULL, ui_thread_main, NULL) != 0) {
        g_ui_opts = NULL;
        g_ui_state = NULL;
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
    atomic_store(&g_ui_stop, 1);
    pthread_join(g_ui_thread, NULL);
    atomic_store(&g_ui_running, 0);
    g_ui_opts = NULL;
    g_ui_state = NULL;
}

void
ui_request_redraw(void) {
    atomic_store(&g_ui_dirty, 1);
}
