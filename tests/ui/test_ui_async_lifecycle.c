// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Lifecycle contracts for the async UI wrapper without starting a real curses thread.
 */

#include <curses.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/ui/menu_core.h>
#include <dsd-neo/ui/ncurses.h>
#include <dsd-neo/ui/ui_async.h>
#include <dsd-neo/ui/ui_history.h>
#include <dsd-neo/ui/ui_opts_snapshot.h>
#include <dsd-neo/ui/ui_prims.h>
#include <dsd-neo/ui/ui_snapshot.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state.h"
#include "dsd-neo/core/state_fwd.h"
#include "telemetry_hooks_impl.h"

WINDOW* stdscr;

static int g_create_calls;
static int g_join_calls;
static int g_history_mode = -1;
static int g_fail_create;
static dsd_control_pump_fn g_control_pump;
static const dsd_opts* g_latest_opts;
static const dsd_state* g_latest_state;
static int g_menu_open;
static int g_menu_handle_calls;
static int g_menu_tick_calls;
static int g_commit_calls;
static int g_getch_calls;
static int g_getch_value = ERR;
static int g_keypad_calls;
static int g_timeout_calls;
static int g_escdelay_calls;
static int g_clearok_calls;
static int g_ncurses_input_calls;
static int g_last_menu_key = ERR;
static int g_last_ncurses_key = ERR;
static int g_printer_calls;
static const dsd_opts* g_printer_opts;
static const dsd_state* g_printer_state;
static uint64_t g_time_ns;

int
dsd_thread_create_impl(dsd_thread_t* thread, void* arg, dsd_thread_fn func) {
    (void)arg;
    g_create_calls++;
    if (g_fail_create) {
        return EAGAIN;
    }
    if (!thread || !func) {
        return EINVAL;
    }
    *thread = (dsd_thread_t)0x1234U;
    return 0;
}

int
dsd_thread_join(dsd_thread_t thread) {
    (void)thread;
    g_join_calls++;
    return 0;
}

void
dsd_runtime_set_control_pump(dsd_control_pump_fn fn) {
    g_control_pump = fn;
}

void
ui_history_set_mode(int mode) {
    g_history_mode = mode;
}

int
ui_drain_cmds(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return 7;
}

void
ui_terminal_install_telemetry_hooks(void) {}

int
ui_menu_is_open(void) {
    return g_menu_open;
}

int
ui_menu_handle_key(int ch, dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_menu_handle_calls++;
    g_last_menu_key = ch;
    return 0;
}

void
ui_menu_tick(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_menu_tick_calls++;
}

void
ui_commit_frame(void) {
    g_commit_calls++;
}

const dsd_opts*
ui_get_latest_opts_snapshot(void) {
    return g_latest_opts;
}

const dsd_state*
ui_get_latest_snapshot(void) {
    return g_latest_state;
}

void
ncursesOpen(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
ncursesClose(void) {}

void
ncursesPrinter(dsd_opts* opts, dsd_state* state) {
    g_printer_calls++;
    g_printer_opts = opts;
    g_printer_state = state;
}

uint8_t
ncurses_input_handler(dsd_opts* opts, dsd_state* state, int c) {
    (void)opts;
    (void)state;
    g_ncurses_input_calls++;
    g_last_ncurses_key = c;
    return 0U;
}

uint64_t
dsd_time_monotonic_ns(void) {
    return g_time_ns;
}

void
dsd_sleep_ms(unsigned int ms) {
    (void)ms;
}

int
set_escdelay(int delay) {
    (void)delay;
    g_escdelay_calls++;
    return 0;
}

int
keypad(WINDOW* win, bool bf) {
    (void)win;
    (void)bf;
    g_keypad_calls++;
    return 0;
}

void
wtimeout(WINDOW* win, int delay) {
    (void)win;
    (void)delay;
    g_timeout_calls++;
}

int
wgetch(WINDOW* win) {
    (void)win;
    g_getch_calls++;
    return g_getch_value;
}

int
clearok(WINDOW* win, bool bf) {
    (void)win;
    (void)bf;
    g_clearok_calls++;
    return 0;
}

int
resize_term(int lines, int columns) {
    (void)lines;
    (void)columns;
    return 0;
}

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_ptr(const char* tag, const void* got, const void* want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %p want %p\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_pump_set(const char* tag, dsd_control_pump_fn fn) {
    if (!fn) {
        DSD_FPRINTF(stderr, "%s: expected non-NULL pointer\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_pump_null(const char* tag, dsd_control_pump_fn fn) {
    if (fn) {
        DSD_FPRINTF(stderr, "%s: expected NULL pointer\n", tag);
        return 1;
    }
    return 0;
}

static int
test_ui_start_failure_resets_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.ncurses_history = 2;

    g_create_calls = 0;
    g_join_calls = 0;
    g_history_mode = -1;
    g_control_pump = NULL;
    g_fail_create = 1;

    int rc = expect_int("start-failure-return", ui_start(&opts, &state), -1);
    rc |= expect_int("start-failure-create-count", g_create_calls, 1);
    rc |= expect_int("start-failure-history-mode", g_history_mode, 2);
    rc |= expect_pump_null("start-failure-control-pump", g_control_pump);
    rc |= expect_int("start-failure-thread-context", ui_is_thread_context(), 0);

    ui_stop();
    rc |= expect_int("start-failure-no-join", g_join_calls, 0);
    return rc;
}

static int
test_ui_start_stop_idempotency_and_control_pump(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.ncurses_history = 1;

    g_create_calls = 0;
    g_join_calls = 0;
    g_history_mode = -1;
    g_control_pump = NULL;
    g_fail_create = 0;

    int rc = expect_int("start-success-return", ui_start(&opts, &state), 0);
    rc |= expect_int("start-success-create-count", g_create_calls, 1);
    rc |= expect_int("start-success-history-mode", g_history_mode, 1);
    rc |= expect_pump_set("start-success-control-pump", g_control_pump);

    rc |= expect_int("duplicate-start-return", ui_start(&opts, &state), 0);
    rc |= expect_int("duplicate-start-no-create", g_create_calls, 1);

    g_control_pump(&opts, &state);

    ui_terminal_telemetry_request_redraw();
    ui_stop();
    rc |= expect_int("stop-join-count", g_join_calls, 1);
    rc |= expect_pump_null("stop-control-pump", g_control_pump);

    ui_stop();
    rc |= expect_int("second-stop-no-join", g_join_calls, 1);
    return rc;
}

static void
reset_frame_counters(void) {
    g_menu_open = 0;
    g_menu_handle_calls = 0;
    g_menu_tick_calls = 0;
    g_commit_calls = 0;
    g_getch_calls = 0;
    g_getch_value = ERR;
    g_keypad_calls = 0;
    g_timeout_calls = 0;
    g_escdelay_calls = 0;
    g_clearok_calls = 0;
    g_ncurses_input_calls = 0;
    g_last_menu_key = ERR;
    g_last_ncurses_key = ERR;
    g_printer_calls = 0;
    g_printer_opts = NULL;
    g_printer_state = NULL;
    g_latest_opts = NULL;
    g_latest_state = NULL;
    g_time_ns = 0U;
}

static int
test_ui_single_frame_snapshot_input_and_draw_helpers(void) {
    static dsd_opts opts;
    static dsd_opts latest_opts;
    static dsd_state state;
    static dsd_state latest_state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&latest_opts, 0, sizeof(latest_opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&latest_state, 0, sizeof(latest_state));
    opts.use_ncurses_terminal = 1;
    latest_opts.use_ncurses_terminal = 1;

    reset_frame_counters();
    dsd_neo_ui_async_test_set_context(&opts, &state);
    stdscr = NULL;

    int rc = 0;
    rc |= expect_ptr("opts snapshot fallback", dsd_neo_ui_async_test_opts_snapshot_or_default(), &opts);
    g_latest_opts = &latest_opts;
    rc |= expect_ptr("opts snapshot preferred", dsd_neo_ui_async_test_opts_snapshot_or_default(), &latest_opts);
    rc |= expect_int("curses inactive without stdscr", dsd_neo_ui_async_test_curses_is_active(&opts), 0);
    stdscr = (WINDOW*)0x1;
    rc |= expect_int("curses active with stdscr", dsd_neo_ui_async_test_curses_is_active(&opts), 1);
    latest_opts.use_ncurses_terminal = 0;
    rc |= expect_int("curses inactive when disabled", dsd_neo_ui_async_test_curses_is_active(&latest_opts), 0);

    opts.audio_in_type = AUDIO_IN_STDIN;
    g_getch_value = 'x';
    rc |= expect_int("stdin input suppresses getch", dsd_neo_ui_async_test_read_key_nonblocking(&opts), ERR);
    rc |= expect_int("stdin input getch count", g_getch_calls, 0);

    opts.audio_in_type = 0;
    g_getch_value = 'n';
    rc |= expect_int("non-stdin input reads getch", dsd_neo_ui_async_test_read_key_nonblocking(&opts), 'n');
    rc |= expect_int("non-stdin getch count", g_getch_calls, 1);

    reset_frame_counters();
    dsd_neo_ui_async_test_set_context(&opts, &state);
    stdscr = (WINDOW*)0x1;
    g_getch_value = 'a';
    dsd_neo_ui_async_test_process_input_frame(&opts);
    rc |= expect_int("configure escdelay once", g_escdelay_calls, 1);
    rc |= expect_int("configure keypad once", g_keypad_calls, 1);
    rc |= expect_int("configure timeout once", g_timeout_calls, 1);
    rc |= expect_int("normal input delivered", g_ncurses_input_calls, 1);
    rc |= expect_int("normal input key", g_last_ncurses_key, 'a');

    g_getch_value = KEY_RESIZE;
    dsd_neo_ui_async_test_process_input_frame(&opts);
    rc |= expect_int("configure not repeated", g_escdelay_calls, 1);
    rc |= expect_int("resize clearok", g_clearok_calls, 1);

    uint64_t last_draw_ns = 100U;
    g_time_ns = 100U;
    dsd_neo_ui_async_test_draw_if_needed(&opts, &last_draw_ns, 1000U);
    rc |= expect_int("resize dirty draws", g_printer_calls, 1);
    rc |= expect_ptr("draw fallback state", g_printer_state, &state);
    rc |= expect_int("draw context restored", ui_is_thread_context(), 0);

    reset_frame_counters();
    dsd_neo_ui_async_test_set_context(&opts, &state);
    stdscr = (WINDOW*)0x1;
    g_menu_open = 1;
    g_getch_value = 'm';
    dsd_neo_ui_async_test_process_input_frame(&opts);
    rc |= expect_int("menu key handled", g_menu_handle_calls, 1);
    rc |= expect_int("menu key value", g_last_menu_key, 'm');
    rc |= expect_int("menu tick", g_menu_tick_calls, 1);
    rc |= expect_int("menu commit", g_commit_calls, 1);

    reset_frame_counters();
    dsd_neo_ui_async_test_set_context(&opts, &state);
    g_latest_state = &latest_state;
    g_time_ns = 500U;
    last_draw_ns = 500U;
    dsd_neo_ui_async_test_draw_if_needed(&opts, &last_draw_ns, 1000U);
    rc |= expect_int("clean frame skipped", g_printer_calls, 0);
    ui_terminal_telemetry_request_redraw();
    dsd_neo_ui_async_test_draw_if_needed(&opts, &last_draw_ns, 1000U);
    rc |= expect_int("dirty frame draws", g_printer_calls, 1);
    rc |= expect_ptr("draw uses latest state", g_printer_state, &latest_state);
    rc |= expect_ptr("draw opts", g_printer_opts, &opts);
    rc |= expect_int("draw commit", g_commit_calls, 1);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_ui_start_failure_resets_state();
    rc |= test_ui_start_stop_idempotency_and_control_pump();
    rc |= test_ui_single_frame_snapshot_input_and_draw_helpers();

    if (rc == 0) {
        printf("UI_ASYNC_LIFECYCLE: OK\n");
    }
    return rc;
}
