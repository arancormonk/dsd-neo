// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Lifecycle contracts for the async UI wrapper without starting a real curses thread.
 */

#include <curses.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/history.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/ui/menu_core.h>
#include <dsd-neo/ui/ncurses.h>
#include <dsd-neo/ui/ui_async.h>
#include <dsd-neo/ui/ui_prims.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/app_control/commands_internal.h"
#include "../../src/app_control/snapshot_internal.h"
#include "dsd-neo/core/frontend_types.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state.h"
#include "dsd-neo/core/state_fwd.h"
#include "rr_panel.h"
#include "ui_keys.h"
#include "ui_mouse.h"

#if UI_KEY_HAVE_WHEEL && !defined(DSD_USE_PDCURSES)
#include <dsd-neo/platform/env.h>
#endif

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
static int g_ncurses_open_calls;
static int g_ncurses_close_calls;
static int g_clearok_calls;
static int g_ncurses_input_calls;
static int g_last_menu_key = ERR;
static int g_last_ncurses_key = ERR;
static int g_printer_calls;
static const dsd_opts* g_printer_opts;
static const dsd_state* g_printer_state;
static int g_redraw_requested;
static uint64_t g_time_ns;

/* Commands the frame loop posted itself (the wheel). */
static int g_posted_cmds[16];
static int g_posted_count;

/* Mouse: what the frame loop asked of curses, and the event getmouse() reports. */
static int g_mousemask_calls;
static unsigned long g_last_mousemask;
static int g_mouseinterval_calls;
static int g_last_mouseinterval = -1;
static int g_getmouse_rc = OK;
static int g_mouse_queued;
static unsigned long g_mouse_bstate;
static int g_putp_calls;
static char g_last_putp[96];
static const char* g_env_term = "xterm-256color";
static const char* g_env_gpm_terms;
static char g_kmous[8] = "\033[<";
static char g_xm[48];
static int g_xm_num = -1;

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
dsd_app_frontend_history_set_mode(int mode) {
    g_history_mode = mode;
}

int
dsd_app_drain_cmds(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return 7;
}

void
dsd_app_install_telemetry_hooks(void) {}

static void
test_frontend_control_pump(dsd_opts* opts, dsd_state* state) {
    (void)dsd_app_drain_cmds(opts, state);
}

void
dsd_app_frontend_runtime_start(const dsd_opts* initial_opts, const dsd_state* initial_state) {
    g_latest_opts = initial_opts;
    g_latest_state = initial_state;
    dsd_runtime_set_control_pump(test_frontend_control_pump);
}

void
dsd_app_frontend_runtime_stop(void) {
    dsd_runtime_set_control_pump(NULL);
}

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
dsd_app_get_latest_opts_snapshot(void) {
    return g_latest_opts;
}

const dsd_state*
dsd_app_get_latest_snapshot(void) {
    return g_latest_state;
}

void
dsd_app_request_redraw(void) {
    g_redraw_requested = 1;
}

int
dsd_app_frontend_redraw_consume(void) {
    int requested = g_redraw_requested;
    g_redraw_requested = 0;
    return requested;
}

void
dsd_terminal_open(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_ncurses_open_calls++;
}

void
dsd_terminal_close(void) {
    g_ncurses_close_calls++;
}

void
rr_panel_shutdown(void) {}

void
dsd_terminal_render(dsd_opts* opts, dsd_state* state) {
    g_printer_calls++;
    g_printer_opts = opts;
    g_printer_state = state;
}

uint8_t
dsd_terminal_handle_input(dsd_opts* opts, dsd_state* state, int c) {
    (void)opts;
    (void)state;
    g_ncurses_input_calls++;
    g_last_ncurses_key = c;
    return 0U;
}

int
dsd_app_command_action(int cmd_id) {
    if (g_posted_count < (int)(sizeof g_posted_cmds / sizeof g_posted_cmds[0])) {
        g_posted_cmds[g_posted_count] = cmd_id;
    }
    g_posted_count++;
    return 0;
}

#if UI_KEY_HAVE_WHEEL
mmask_t
mousemask(mmask_t newmask, mmask_t* oldmask) {
    (void)oldmask;
    g_mousemask_calls++;
    g_last_mousemask = (unsigned long)newmask;
    return newmask;
}

int
mouseinterval(int erval) {
    g_mouseinterval_calls++;
    g_last_mouseinterval = erval;
    return 166;
}

/* One event behind each KEY_MOUSE getch() returns; ERR once it was read (ncurses' queue is empty). */
static int
fake_getmouse(MEVENT* event) {
    DSD_MEMSET(event, 0, sizeof(*event));
    if (!g_mouse_queued) {
        return ERR;
    }
    g_mouse_queued = 0;
    event->bstate = (mmask_t)g_mouse_bstate;
    return g_getmouse_rc;
}

#if defined(DSD_USE_PDCURSES)
int
nc_getmouse(MEVENT* event) {
    return fake_getmouse(event);
}
#else
int
getmouse(MEVENT* event) {
    return fake_getmouse(event);
}

char*
tigetstr(const char* cap) {
    if (strcmp(cap, "kmous") == 0) {
        return g_kmous[0] != '\0' ? g_kmous : NULL;
    }
    if (strcmp(cap, "XM") == 0) {
        return g_xm[0] != '\0' ? g_xm : NULL;
    }
    return NULL;
}

int
tigetnum(const char* cap) {
    return strcmp(cap, "XM") == 0 ? g_xm_num : -1;
}

/* The template and its one parameter, so a test sees which tracking string and which direction were written. */
char*
tiparm(const char* str, ...) {
    static char out[96];
    va_list ap;
    va_start(ap, str);
    const int param = va_arg(ap, int);
    va_end(ap);
    DSD_SNPRINTF(out, sizeof out, "%s|%d", str, param);
    return out;
}

int
putp(const char* str) {
    g_putp_calls++;
    DSD_SNPRINTF(g_last_putp, sizeof g_last_putp, "%s", str);
    return 0;
}

static size_t
fake_env_copy_value(const char* value, char* buf, size_t cap) {
    if (value == NULL) {
        if (cap > 0) {
            buf[0] = '\0';
        }
        return 0;
    }
    DSD_SNPRINTF(buf, cap, "%s", value);
    return strlen(value) + 1U;
}

size_t
dsd_env_copy(const char* name, char* buf, size_t cap) {
    if (strcmp(name, "TERM") == 0) {
        return fake_env_copy_value(g_env_term, buf, cap);
    }
    if (strcmp(name, "NCURSES_GPM_TERMS") == 0) {
        return fake_env_copy_value(g_env_gpm_terms, buf, cap);
    }
    return fake_env_copy_value(NULL, buf, cap);
}
#endif
#endif

uint64_t
dsd_realtime_mono_ns(void) {
    return g_time_ns;
}

void
dsd_sleep_ms(unsigned int ms) {
    (void)ms;
}

/* PDCurses (Windows) has no set_escdelay(), and dsd_curses_set_escdelay() skips it there. */
#if defined(DSD_USE_PDCURSES)
enum { kEscdelayCallsPerConfigure = 0 };
#else
enum { kEscdelayCallsPerConfigure = 1 };

int
set_escdelay(int delay) {
    (void)delay;
    g_escdelay_calls++;
    return 0;
}
#endif

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

#if defined(DSD_USE_PDCURSES)
/* PDCurses (Windows) implements timeout() as a function; ncurses defines it as wtimeout() on stdscr. */
void
timeout(int delay) {
    wtimeout(stdscr, delay);
}
#endif

int
wgetch(WINDOW* win) {
    (void)win;
    g_getch_calls++;
    g_mouse_queued = g_getch_value == KEY_MOUSE;
    return g_getch_value;
}

#if defined(DSD_USE_PDCURSES) && defined(DSD_HAS_PDCURSES_WIDE_API)
/* A wide PDCurses build reads keys through wget_wch(): the same scripted key, as a key code or a character. */
int
wget_wch(WINDOW* win, wint_t* wch) {
    int key = wgetch(win);
    if (key == ERR) {
        return ERR;
    }
    *wch = (wint_t)key;
    return key >= KEY_MIN ? KEY_CODE_YES : OK;
}
#endif

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
    opts.frontend_terminal_display.terminal_history = 2;

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
    opts.frontend_terminal_display.terminal_history = 1;

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

    dsd_app_request_redraw();
    ui_stop();
    rc |= expect_int("stop-join-count", g_join_calls, 1);
    rc |= expect_pump_null("stop-control-pump", g_control_pump);

    ui_stop();
    rc |= expect_int("second-stop-no-join", g_join_calls, 1);
    return rc;
}

static int
test_ui_curses_close_uses_opened_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.frontend_kind = DSD_FRONTEND_TERMINAL;

    g_ncurses_open_calls = 0;
    g_ncurses_close_calls = 0;
    dsd_neo_ui_async_test_set_context(&opts, &state);

    int curses_opened = dsd_neo_ui_async_test_open_curses_if_needed();
    opts.frontend_kind = DSD_FRONTEND_NONE;
    dsd_neo_ui_async_test_close_curses_if_opened(curses_opened);

    int rc = expect_int("curses-opened", curses_opened, 1);
    rc |= expect_int("curses-open-count", g_ncurses_open_calls, 1);
    rc |= expect_int("curses-close-after-frontend-change", g_ncurses_close_calls, 1);

    g_ncurses_open_calls = 0;
    g_ncurses_close_calls = 0;
    curses_opened = dsd_neo_ui_async_test_open_curses_if_needed();
    dsd_neo_ui_async_test_close_curses_if_opened(curses_opened);
    rc |= expect_int("curses-not-opened-after-disable", curses_opened, 0);
    rc |= expect_int("curses-no-open-after-disable", g_ncurses_open_calls, 0);
    rc |= expect_int("curses-no-close-after-disable", g_ncurses_close_calls, 0);
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
    g_redraw_requested = 0;
    g_time_ns = 0U;
    g_posted_count = 0;
    g_mousemask_calls = 0;
    g_last_mousemask = 0UL;
    g_mouseinterval_calls = 0;
    g_last_mouseinterval = -1;
    g_getmouse_rc = OK;
    g_mouse_bstate = 0UL;
    g_putp_calls = 0;
    g_last_putp[0] = '\0';
    g_env_term = "xterm-256color";
    g_env_gpm_terms = NULL;
    DSD_SNPRINTF(g_kmous, sizeof g_kmous, "%s", "\033[<");
    g_xm[0] = '\0';
    g_xm_num = -1;
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
    opts.frontend_kind = DSD_FRONTEND_TERMINAL;
    latest_opts.frontend_kind = DSD_FRONTEND_TERMINAL;

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
    latest_opts.frontend_kind = DSD_FRONTEND_NONE;
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
    rc |= expect_int("configure escdelay once", g_escdelay_calls, kEscdelayCallsPerConfigure);
    rc |= expect_int("configure keypad once", g_keypad_calls, 1);
    rc |= expect_int("configure timeout once", g_timeout_calls, 1);
    rc |= expect_int("normal input delivered", g_ncurses_input_calls, 1);
    rc |= expect_int("normal input key", g_last_ncurses_key, 'a');

    g_getch_value = KEY_RESIZE;
    dsd_neo_ui_async_test_process_input_frame(&opts);
    rc |= expect_int("configure not repeated", g_escdelay_calls, kEscdelayCallsPerConfigure);
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
    dsd_app_request_redraw();
    dsd_neo_ui_async_test_draw_if_needed(&opts, &last_draw_ns, 1000U);
    rc |= expect_int("dirty frame draws", g_printer_calls, 1);
    rc |= expect_ptr("draw uses latest state", g_printer_state, &latest_state);
    rc |= expect_ptr("draw opts", g_printer_opts, &opts);
    rc |= expect_int("draw commit", g_commit_calls, 1);
    return rc;
}

#if UI_KEY_HAVE_WHEEL

/* The mask the first capture sets: press and release of buttons 1-3 and the wheel presses, and on PDCurses its
   MOUSE_WHEEL_SCROLL. */
#if defined(DSD_USE_PDCURSES) && defined(MOUSE_WHEEL_SCROLL)
#define TEST_WHEEL_ON_MASK (TEST_WHEEL_PARSE_MASK | MOUSE_WHEEL_SCROLL)
#else
#define TEST_WHEEL_ON_MASK TEST_WHEEL_PARSE_MASK
#endif
#define TEST_WHEEL_PARSE_MASK                                                                                          \
    (BUTTON1_PRESSED | BUTTON1_RELEASED | BUTTON2_PRESSED | BUTTON2_RELEASED | BUTTON3_PRESSED | BUTTON3_RELEASED      \
     | BUTTON4_PRESSED | BUTTON5_PRESSED)

/* A running -Y scan with the wheel setting on, in the published snapshots. */
static void
wheel_snapshots_ready(dsd_opts* osnap, dsd_state* ssnap) {
    DSD_MEMSET(osnap, 0, sizeof(*osnap));
    DSD_MEMSET(ssnap, 0, sizeof(*ssnap));
    osnap->frontend_kind = DSD_FRONTEND_TERMINAL;
    osnap->frontend_terminal_display.mouse_wheel = 1;
    osnap->scanner_mode = 1;
    g_latest_opts = osnap;
    g_latest_state = ssnap;
}

/* One frame of the UI thread with the published snapshots, as ui_thread_main() runs it. */
static void
wheel_frame(const dsd_opts* osnap) {
    dsd_neo_ui_async_test_process_input_frame(osnap);
}

#if defined(DSD_USE_PDCURSES)
/* PDCurses switches with the mask: the full one on, 0 off. */
static int
expect_tracking_write(const char* tag, int on, int switches) {
    int rc = expect_int(tag, g_mousemask_calls, 1 + switches);
    rc |= expect_int(tag, (int)(g_last_mousemask == (on ? (unsigned long)TEST_WHEEL_ON_MASK : 0UL)), 1);
    return rc;
}
#else
/* ncurses keeps the mask and switches the terminal's tracking with the XM string. */
static int
expect_tracking_write(const char* tag, int on, int switches) {
    char want[96];
    DSD_SNPRINTF(want, sizeof want, "%s|%d", "\033[?1006;1000%?%p1%{1}%=%th%el%;", on);
    int rc = expect_int(tag, g_mousemask_calls, 1);
    rc |= expect_int(tag, g_putp_calls, switches);
    if (strcmp(g_last_putp, want) != 0) {
        DSD_FPRINTF(stderr, "%s: wrote \"%s\" want \"%s\"\n", tag, g_last_putp, want);
        rc = 1;
    }
    return rc;
}
#endif

/* The capture follows the setting, the running scan, the overlay and the input kind in the published snapshots,
   whatever the decoder's live structs say; the first capture sets the mask and mouseinterval(0) once, and every later
   switch is the terminal's tracking string (ncurses) or the mask (PDCurses). */
static int
test_wheel_capture_follows_the_snapshots(void) {
    static dsd_opts live_opts;
    static dsd_state live_state;
    static dsd_opts osnap;
    static dsd_state ssnap;
    DSD_MEMSET(&live_opts, 0, sizeof(live_opts));
    DSD_MEMSET(&live_state, 0, sizeof(live_state));
    reset_frame_counters();
    /* The live structs say the opposite of the snapshots throughout. */
    live_opts.frontend_kind = DSD_FRONTEND_TERMINAL;
    live_opts.frontend_terminal_display.mouse_wheel = 0;
    dsd_neo_ui_async_test_set_context(&live_opts, &live_state);
    stdscr = (WINDOW*)0x1;
    DSD_SNPRINTF(g_xm, sizeof g_xm, "%s", "\033[?1006;1000%?%p1%{1}%=%th%el%;");
    wheel_snapshots_ready(&osnap, &ssnap);

    int rc = 0;
    wheel_frame(&osnap);
    rc |= expect_int("wheel on: capture", ui_mouse_capture_is_on(), 1);
    rc |= expect_int("wheel on: mask once", g_mousemask_calls, 1);
    rc |= expect_int("wheel on: full mask", (int)(g_last_mousemask == (unsigned long)TEST_WHEEL_ON_MASK), 1);
    rc |= expect_int("wheel on: mouseinterval once", g_mouseinterval_calls, 1);
    rc |= expect_int("wheel on: no click wait", g_last_mouseinterval, 0);
    rc |= expect_int("wheel on: no tracking string yet", g_putp_calls, 0);
    wheel_frame(&osnap);
    rc |= expect_int("wheel on again: no curses call", g_mousemask_calls + g_putp_calls, 1);

    g_menu_open = 1;
    wheel_frame(&osnap);
    rc |= expect_int("menu open: capture off", ui_mouse_capture_is_on(), 0);
    rc |= expect_tracking_write("menu open: tracking off", 0, 1);
    g_menu_open = 0;
    live_opts.frontend_terminal_display.mouse_wheel = 1;
    wheel_frame(&osnap);
    rc |= expect_int("menu closed: capture on", ui_mouse_capture_is_on(), 1);
    rc |= expect_tracking_write("menu closed: tracking on", 1, 2);

    osnap.frontend_terminal_display.mouse_wheel = 0;
    wheel_frame(&osnap);
    rc |= expect_int("setting off: capture off", ui_mouse_capture_is_on(), 0);
    rc |= expect_tracking_write("setting off: tracking off", 0, 3);
    osnap.frontend_terminal_display.mouse_wheel = 1;
    wheel_frame(&osnap);
    rc |= expect_tracking_write("setting on: tracking on", 1, 4);

    /* -Y stopped in the snapshot while the live opts still run it. */
    live_opts.scanner_mode = 1;
    osnap.scanner_mode = 0;
    wheel_frame(&osnap);
    rc |= expect_int("scan stopped: capture off", ui_mouse_capture_is_on(), 0);
    rc |= expect_tracking_write("scan stopped: tracking off", 0, 5);

    /* A running --trunk-scan captures; a paused one does not, though trunk_scan_enabled stays 1. */
    osnap.trunk_scan_enabled = 1;
    wheel_frame(&osnap);
    rc |= expect_int("trunk scan running: capture on", ui_mouse_capture_is_on(), 1);
    ssnap.trunk_scan_paused = 1;
    wheel_frame(&osnap);
    rc |= expect_int("trunk scan paused: capture off", ui_mouse_capture_is_on(), 0);
    ssnap.trunk_scan_paused = 0;
    wheel_frame(&osnap);
    rc |= expect_int("trunk scan resumed: capture on", ui_mouse_capture_is_on(), 1);

    /* No state snapshot yet: no scan can be shown running. */
    g_latest_state = NULL;
    wheel_frame(&osnap);
    rc |= expect_int("no state snapshot: capture off", ui_mouse_capture_is_on(), 0);
    g_latest_state = &ssnap;

    /* STDIN input reads no keys, so it captures nothing. */
    osnap.audio_in_type = AUDIO_IN_STDIN;
    wheel_frame(&osnap);
    rc |= expect_int("stdin input: capture off", ui_mouse_capture_is_on(), 0);
    osnap.audio_in_type = 0;
    wheel_frame(&osnap);
    rc |= expect_int("back on: capture on", ui_mouse_capture_is_on(), 1);
    rc |= expect_int("mouseinterval still once", g_mouseinterval_calls, 1);
#if !defined(DSD_USE_PDCURSES)
    rc |= expect_int("ncurses mask still set once", g_mousemask_calls, 1);
#endif
    rc |= expect_int("no command posted", g_posted_count, 0);
    rc |= expect_int("no key handed on", g_ncurses_input_calls + g_menu_handle_calls, 0);
    return rc;
}

#if !defined(DSD_USE_PDCURSES)
/* With no XM string in terminfo, the tracking string is ncurses' own: the SGR form when XM is the number 1006, else
   plain mode 1000. */
static int
test_wheel_tracking_string_fallback(void) {
    static dsd_opts live_opts;
    static dsd_state live_state;
    static dsd_opts osnap;
    static dsd_state ssnap;
    int rc = 0;
    for (int num = 0; num < 2; num++) {
        DSD_MEMSET(&live_opts, 0, sizeof(live_opts));
        reset_frame_counters();
        dsd_neo_ui_async_test_set_context(&live_opts, &live_state);
        stdscr = (WINDOW*)0x1;
        g_xm_num = num ? 1006 : -1;
        wheel_snapshots_ready(&osnap, &ssnap);
        wheel_frame(&osnap);
        g_menu_open = 1;
        wheel_frame(&osnap);
        char want[96];
        DSD_SNPRINTF(want, sizeof want, "%s|0",
                     num ? "\033[?1006;1000%?%p1%{1}%=%th%el%;" : "\033[?1000%?%p1%{1}%=%th%el%;");
        if (strcmp(g_last_putp, want) != 0) {
            DSD_FPRINTF(stderr, "XM fallback %d: wrote \"%s\" want \"%s\"\n", num, g_last_putp, want);
            rc = 1;
        }
    }
    return rc;
}

/* A terminal ncurses would read through GPM, or one without an ESC [ kmous, never turns the capture on. */
static int
test_wheel_refused_without_the_xterm_driver(void) {
    static dsd_opts live_opts;
    static dsd_state live_state;
    static dsd_opts osnap;
    static dsd_state ssnap;
    int rc = 0;
    for (int which = 0; which < 3; which++) {
        DSD_MEMSET(&live_opts, 0, sizeof(live_opts));
        reset_frame_counters();
        dsd_neo_ui_async_test_set_context(&live_opts, &live_state);
        stdscr = (WINDOW*)0x1;
        if (which == 0) {
            g_env_term = "linux";
        } else if (which == 1) {
            g_env_gpm_terms = "";
        } else {
            g_kmous[0] = '\0';
        }
        wheel_snapshots_ready(&osnap, &ssnap);
        wheel_frame(&osnap);
        rc |= expect_int("refused: row hidden", ui_mouse_wheel_available(NULL) ? 1 : 0, 0);
        rc |= expect_int("refused: capture off", ui_mouse_capture_is_on(), 0);
        rc |= expect_int("refused: no curses call", g_mousemask_calls + g_mouseinterval_calls + g_putp_calls, 0);
    }
    return rc;
}
#endif

/* Wheel up posts Previous and down posts Next, at most once per 150 ms; neither reaches the key handler. */
static int
test_wheel_steps_throttled(void) {
    static dsd_opts live_opts;
    static dsd_state live_state;
    static dsd_opts osnap;
    static dsd_state ssnap;
    DSD_MEMSET(&live_opts, 0, sizeof(live_opts));
    reset_frame_counters();
    dsd_neo_ui_async_test_set_context(&live_opts, &live_state);
    stdscr = (WINDOW*)0x1;
    wheel_snapshots_ready(&osnap, &ssnap);
    wheel_frame(&osnap);

    const uint64_t ms = 1000ULL * 1000ULL;
    int rc = 0;
    g_getch_value = KEY_MOUSE;
    g_mouse_bstate = (unsigned long)BUTTON4_PRESSED;
    g_time_ns = 1000U * ms;
    wheel_frame(&osnap);
    rc |= expect_int("first notch steps", g_posted_count, 1);
    rc |= expect_int("wheel up is Previous", g_posted_cmds[0], DSD_APP_CMD_CHANNEL_CYCLE_PREV);
    /* A burst inside 150 ms is one step. */
    for (uint64_t t = 1010U; t < 1150U; t += 20U) {
        g_time_ns = t * ms;
        wheel_frame(&osnap);
    }
    g_time_ns = 1149U * ms;
    wheel_frame(&osnap);
    rc |= expect_int("burst is one step", g_posted_count, 1);
    g_mouse_bstate = (unsigned long)BUTTON5_PRESSED;
    g_time_ns = 1150U * ms;
    wheel_frame(&osnap);
    rc |= expect_int("next notch after the gap steps", g_posted_count, 2);
    rc |= expect_int("wheel down is Next", g_posted_cmds[1], DSD_APP_CMD_CHANNEL_CYCLE);

    /* A click or an unreadable event is nothing. */
    g_mouse_bstate = (unsigned long)BUTTON1_PRESSED;
    g_time_ns = 2000U * ms;
    wheel_frame(&osnap);
    g_mouse_bstate = (unsigned long)BUTTON4_PRESSED;
    g_getmouse_rc = ERR;
    wheel_frame(&osnap);
    rc |= expect_int("click and unreadable event post nothing", g_posted_count, 2);
    g_getmouse_rc = OK;

    /* A notch queued before the menu opened arrives while the capture is off: dropped, and the menu sees no key. */
    g_menu_open = 1;
    g_time_ns = 3000U * ms;
    wheel_frame(&osnap);
    rc |= expect_int("menu open: notch dropped", g_posted_count, 2);
    rc |= expect_int("menu open: menu gets no key", g_menu_handle_calls, 0);
    rc |= expect_int("menu open: menu still ticks", g_menu_tick_calls, 1);
    g_menu_open = 0;
    osnap.frontend_terminal_display.mouse_wheel = 0;
    wheel_frame(&osnap);
    rc |= expect_int("setting off: notch dropped", g_posted_count, 2);
    osnap.frontend_terminal_display.mouse_wheel = 1;
    osnap.scanner_mode = 0;
    wheel_frame(&osnap);
    rc |= expect_int("no scan: notch dropped", g_posted_count, 2);
    rc |= expect_int("wheel never reaches the key handler", g_ncurses_input_calls, 0);
    return rc;
}

/* Closing the terminal turns the capture off, and a reopened terminal starts over with the mask. */
static int
test_wheel_close_leaves_capture_off(void) {
    static dsd_opts live_opts;
    static dsd_state live_state;
    static dsd_opts osnap;
    static dsd_state ssnap;
    DSD_MEMSET(&live_opts, 0, sizeof(live_opts));
    reset_frame_counters();
    live_opts.frontend_kind = DSD_FRONTEND_TERMINAL;
    dsd_neo_ui_async_test_set_context(&live_opts, &live_state);
    stdscr = (WINDOW*)0x1;
    DSD_SNPRINTF(g_xm, sizeof g_xm, "%s", "\033[?1006;1000%?%p1%{1}%=%th%el%;");
    wheel_snapshots_ready(&osnap, &ssnap);
    wheel_frame(&osnap);

    int rc = expect_int("before close: capture on", ui_mouse_capture_is_on(), 1);
    g_ncurses_close_calls = 0;
    dsd_neo_ui_async_test_close_curses_if_opened(1);
    rc |= expect_int("close: terminal closed", g_ncurses_close_calls, 1);
    rc |= expect_int("close: capture off", ui_mouse_capture_is_on(), 0);
    rc |= expect_tracking_write("close: tracking off", 0, 1);

    wheel_frame(&osnap);
    rc |= expect_int("reopened: capture on", ui_mouse_capture_is_on(), 1);
#if defined(DSD_USE_PDCURSES)
    rc |= expect_int("reopened: mask set again", g_mousemask_calls, 3);
#else
    rc |= expect_int("reopened: mask set again", g_mousemask_calls, 2);
#endif
    rc |= expect_int("reopened: mouseinterval again", g_mouseinterval_calls, 2);
    return rc;
}

#endif /* UI_KEY_HAVE_WHEEL */

int
main(void) {
    int rc = 0;

    rc |= test_ui_start_failure_resets_state();
    rc |= test_ui_start_stop_idempotency_and_control_pump();
    rc |= test_ui_curses_close_uses_opened_state();
    rc |= test_ui_single_frame_snapshot_input_and_draw_helpers();
#if UI_KEY_HAVE_WHEEL
    rc |= test_wheel_capture_follows_the_snapshots();
#if !defined(DSD_USE_PDCURSES)
    rc |= test_wheel_tracking_string_fallback();
    rc |= test_wheel_refused_without_the_xterm_driver();
#endif
    rc |= test_wheel_steps_throttled();
    rc |= test_wheel_close_leaves_capture_off();
#endif

    if (rc == 0) {
        printf("UI_ASYNC_LIFECYCLE: OK\n");
    }
    return rc;
}
