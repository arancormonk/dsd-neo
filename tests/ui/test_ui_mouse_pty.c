// SPDX-License-Identifier: GPL-3.0-or-later
// NOLINTBEGIN(misc-use-internal-linkage)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The terminal mouse wheel on the host's real ncurses, through a pseudo-terminal (issue #666).
 *
 * ncurses' getch() loops back into a blocking read when a queued mouse report does not match the parse mask, so a
 * capture that switched off with mousemask(0) froze the UI thread on the next click until another key arrived. The
 * frame loop (ui_async.c) with the real capture (ui_mouse.c) and key read (ui_read_key.c) runs here on an
 * xterm-256color screen whose input is a pty: the wheel steps while a -Y scan runs, and after the menu opens, the
 * setting goes off and the scan stops, a queued click (press and release) and a lone press are each read and dropped
 * in one frame with no key typed after them. A frame that blocked would hang; SIGALRM fails the test instead. A burst
 * of wheel reports typed at once (a flick) steps once, however many frames follow, and a key typed after it is still
 * handled; notches typed apart each step. After Ctrl+Z / fg (endwin(), SIGCONT and the resume's doupdate(), as
 * ncurses' SIGTSTP handler runs them), whose resume turns tracking on, the next frame turns it back off when the
 * capture is off, and idle frames after it write nothing. A closed and reopened terminal does not turn tracking on.
 *
 * The pty's other end is read on a thread of its own the whole time: endwin() sets the tty mode with TCSADRAIN, and
 * macOS (as the BSDs) holds that until the reader takes the exit strings endwin() just wrote, so a test that read only
 * between calls would hang there. Linux passes a pty's output straight to the reader and does not wait.
 *
 * Exit 77 (skipped) when the host has no pty, no xterm-256color terminfo, or an ncurses without button 5.
 */

#include <curses.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/history.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/frontend_types.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/env.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/ui/menu_core.h>
#include <dsd-neo/ui/ncurses.h>
#include <dsd-neo/ui/ui_async.h>
#include <dsd-neo/ui/ui_prims.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include "rr_panel.h"
#include "ui_keys.h"
#include "ui_mouse.h"

enum { kSkip = 77 };

static dsd_opts* g_osnap;
static dsd_state* g_ssnap;
static int g_menu_open;
static int g_posted[16];
static int g_posted_count;
static int g_handled_keys;

/* ---- The rest of the program, as the frame loop sees it ---- */

void
dsd_app_frontend_runtime_start(const dsd_opts* initial_opts, const dsd_state* initial_state) {
    (void)initial_opts;
    (void)initial_state;
}

void
dsd_app_frontend_runtime_stop(void) {}

void
dsd_app_frontend_history_set_mode(int mode) {
    (void)mode;
}

int
ui_menu_is_open(void) {
    return g_menu_open;
}

int
ui_menu_handle_key(int ch, dsd_opts* opts, dsd_state* state) {
    (void)ch;
    (void)opts;
    (void)state;
    g_handled_keys++;
    return 0;
}

void
ui_menu_tick(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
ui_commit_frame(void) {
    doupdate();
}

const dsd_opts*
dsd_app_get_latest_opts_snapshot(void) {
    return g_osnap;
}

const dsd_state*
dsd_app_get_latest_snapshot(void) {
    return g_ssnap;
}

void
dsd_app_request_redraw(void) {}

int
dsd_app_frontend_redraw_consume(void) {
    return 0;
}

void
dsd_terminal_open(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
dsd_terminal_close(void) {}

void
rr_panel_shutdown(void) {}

void
dsd_terminal_render(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

uint8_t
dsd_terminal_handle_input(dsd_opts* opts, dsd_state* state, int c) {
    (void)opts;
    (void)state;
    (void)c;
    g_handled_keys++;
    return 0U;
}

int
dsd_app_command_action(int cmd_id) {
    if (g_posted_count < (int)(sizeof g_posted / sizeof g_posted[0])) {
        g_posted[g_posted_count] = cmd_id;
    }
    g_posted_count++;
    return 0;
}

uint64_t
dsd_realtime_mono_ns(void) {
    return dsd_time_monotonic_ns();
}

#if UI_KEY_HAVE_WHEEL

/* ---- The pty ---- */

/* A frame that blocks waits for a key that never comes, so it hangs until alarm() fires. The bound only has to sit
   past any scheduler stall on a loaded runner, far above the UI's 66 ms frame (ui_async.c) and far below the hang. */
static const uint64_t kFrameBudgetNs = 1000ULL * 1000ULL * 1000ULL;

static int g_master = -1;
/* The step under way, named when the test times out. */
static const char* volatile g_step = "setup";
/* What the screen wrote, read by drain_screen_output() and the reader thread; g_out_lock guards it and the stop. */
static dsd_mutex_t g_out_lock;
static char g_screen_out[65536];
static size_t g_screen_out_len;
static int g_reader_stop;

static void
on_alarm(int sig) {
    (void)sig;
    static const char msg[] = "UI_MOUSE_PTY: timed out, blocked in: ";
    const char* step = g_step;
    size_t len = 0U;
    while (step[len] != '\0') {
        len++;
    }
    (void)!write(STDERR_FILENO, msg, sizeof msg - 1U);
    (void)!write(STDERR_FILENO, step, len);
    (void)!write(STDERR_FILENO, "\n", 1U);
    _exit(1);
}

/* Read what the screen wrote, so the pty never fills, and keep it for the tracking-string checks. g_out_lock held. */
static void
drain_screen_output_locked(void) {
    char buf[4096];
    for (;;) {
        ssize_t n = read(g_master, buf, sizeof buf);
        if (n <= 0) {
            return;
        }
        size_t room = sizeof g_screen_out - 1U - g_screen_out_len;
        size_t take = (size_t)n < room ? (size_t)n : room;
        DSD_MEMCPY(g_screen_out + g_screen_out_len, buf, take);
        g_screen_out_len += take;
        g_screen_out[g_screen_out_len] = '\0';
    }
}

/* Everything the screen has written so far is in g_screen_out once this returns, whichever thread read it. */
static void
drain_screen_output(void) {
    (void)dsd_mutex_lock(&g_out_lock);
    drain_screen_output_locked();
    (void)dsd_mutex_unlock(&g_out_lock);
}

static int
screen_wrote(const char* seq) {
    (void)dsd_mutex_lock(&g_out_lock);
    const int found = strstr(g_screen_out, seq) != NULL;
    (void)dsd_mutex_unlock(&g_out_lock);
    return found;
}

static size_t
screen_output_len(void) {
    (void)dsd_mutex_lock(&g_out_lock);
    const size_t len = g_screen_out_len;
    (void)dsd_mutex_unlock(&g_out_lock);
    return len;
}

static void
screen_output_clear(void) {
    (void)dsd_mutex_lock(&g_out_lock);
    g_screen_out_len = 0;
    g_screen_out[0] = '\0';
    (void)dsd_mutex_unlock(&g_out_lock);
}

/* The direction of the last tracking string the screen wrote since the clear: 'h' on, 'l' off, 0 none. */
static char
last_tracking(void) {
    static const char kXm[] = "\033[?1006;1000";
    char dir = 0;
    (void)dsd_mutex_lock(&g_out_lock);
    for (const char* p = strstr(g_screen_out, kXm); p != NULL; p = strstr(p + 1, kXm)) {
        dir = p[sizeof kXm - 1U];
    }
    (void)dsd_mutex_unlock(&g_out_lock);
    return dir;
}

/* 1 when @p fd has input within @p timeout_ms. select(), not poll(): macOS documents poll() as unsupported on
   devices, a pty among them (it can report POLLNVAL as an event). */
static int
fd_readable(int fd, int timeout_ms) {
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = {.tv_sec = (time_t)(timeout_ms / 1000), .tv_usec = (suseconds_t)(timeout_ms % 1000) * 1000};
        int n = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return n > 0 && FD_ISSET(fd, &rfds);
    }
}

/* Reads the pty's other end while the test runs, above all while endwin() waits for its output to be read. */
static DSD_THREAD_RETURN_TYPE
screen_reader(void* arg) {
    (void)arg;
    for (;;) {
        (void)dsd_mutex_lock(&g_out_lock);
        const int stop = g_reader_stop;
        if (!stop) {
            drain_screen_output_locked();
        }
        (void)dsd_mutex_unlock(&g_out_lock);
        if (stop) {
            break;
        }
        (void)fd_readable(g_master, 20);
    }
    DSD_THREAD_RETURN;
}

/* Type @p report into the terminal and wait until the screen's input has it. */
static int
queue_input(int slave, const char* report) {
    size_t len = strlen(report);
    if (write(g_master, report, len) != (ssize_t)len) {
        return 1;
    }
    return fd_readable(slave, 2000) ? 0 : 1;
}

static int
input_pending(int slave) {
    return fd_readable(slave, 0);
}

/* One UI frame: input (with the capture switch), then the draw's doupdate(). Fails when it took over the budget. */
static int
frame(const char* tag) {
    g_step = tag;
    const uint64_t t0 = dsd_time_monotonic_ns();
    dsd_neo_ui_async_test_process_input_frame(g_osnap);
    doupdate();
    const uint64_t took = dsd_time_monotonic_ns() - t0;
    drain_screen_output();
    if (took > kFrameBudgetNs) {
        DSD_FPRINTF(stderr, "%s: frame took %llu ms\n", tag, (unsigned long long)(took / 1000000ULL));
        return 1;
    }
    return 0;
}

/* Queue @p reports with no key after them: every frame returns in time, the input empties, nothing steps and no key
   reaches a handler. */
static int
queued_reports_never_block(int slave, const char* tag, const char* reports) {
    int rc = 0;
    const int posted = g_posted_count;
    if (queue_input(slave, reports) != 0) {
        DSD_FPRINTF(stderr, "%s: the reports never reached the screen\n", tag);
        return 1;
    }
    for (int i = 0; i < 3; i++) {
        rc |= frame(tag);
    }
    if (input_pending(slave)) {
        DSD_FPRINTF(stderr, "%s: reports left unread\n", tag);
        rc = 1;
    }
    if (g_posted_count != posted || g_handled_keys != 0) {
        DSD_FPRINTF(stderr, "%s: a queued report stepped or reached a key handler\n", tag);
        rc = 1;
    }
    return rc;
}

/* With the capture off: a click (button 1 press and release), a lone button 3 press and a wheel notch, each queued on
   its own. A capture switched off with mousemask(0) blocks the first frame after the click until another key. */
static int
queued_clicks_never_block(int slave, const char* tag) {
    int rc = queued_reports_never_block(slave, tag, "\033[<0;10;5M\033[<0;10;5m");
    rc |= queued_reports_never_block(slave, tag, "\033[<2;12;5M");
    rc |= queued_reports_never_block(slave, tag, "\033[<65;10;5M");
    return rc;
}

static int
expect(const char* tag, int ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "%s: failed\n", tag);
        return 1;
    }
    return 0;
}

/* Capture on (a running -Y scan, the setting on, no menu), then a wheel notch up and down steps. */
static int
capture_on_and_step(int slave, const char* tag) {
    g_menu_open = 0;
    g_osnap->frontend_terminal_display.mouse_wheel = 1;
    g_osnap->scanner_mode = 1;
    int rc = frame(tag);
    rc |= expect(tag, ui_mouse_capture_is_on() == 1);
    const int posted = g_posted_count;
    /* The throttle allows one step per 150 ms. */
    dsd_sleep_ms(160);
    if (queue_input(slave, "\033[<64;10;5M") != 0) {
        return 1;
    }
    rc |= frame(tag);
    rc |= expect(tag, g_posted_count == posted + 1 && g_posted[posted] == DSD_APP_CMD_CHANNEL_CYCLE_PREV);
    dsd_sleep_ms(160);
    if (queue_input(slave, "\033[<65;10;5M") != 0) {
        return 1;
    }
    rc |= frame(tag);
    rc |= expect(tag, g_posted_count == posted + 2 && g_posted[posted + 1] == DSD_APP_CMD_CHANNEL_CYCLE);
    return rc;
}

/* A flick of the wheel: @p count notch-down reports and then a plain key, typed at once. The frames that follow run
   for longer than the throttle's gap many times over. */
static int
wheel_burst_steps_once(int slave, const char* tag, int count) {
    static char burst[2048];
    static const char kNotch[] = "\033[<65;10;5M";
    size_t len = 0U;
    for (int i = 0; i < count && len + sizeof kNotch < sizeof burst - 1U; i++) {
        DSD_MEMCPY(burst + len, kNotch, sizeof kNotch - 1U);
        len += sizeof kNotch - 1U;
    }
    burst[len++] = 'x';
    burst[len] = '\0';
    const int posted = g_posted_count;
    const int handled = g_handled_keys;
    dsd_sleep_ms(160);
    if (queue_input(slave, burst) != 0) {
        return 1;
    }
    int rc = 0;
    for (int i = 0; i < 40; i++) {
        rc |= frame(tag);
        dsd_sleep_ms(20);
    }
    if (g_posted_count != posted + 1) {
        DSD_FPRINTF(stderr, "%s: %d steps (want 1)\n", tag, g_posted_count - posted);
        rc = 1;
    }
    rc |= expect(tag, g_posted_count > posted && g_posted[posted] == DSD_APP_CMD_CHANNEL_CYCLE);
    rc |= expect(tag, g_handled_keys == handled + 1);
    rc |= expect(tag, !input_pending(slave));
    return rc;
}

/* Notches made apart from one another, each past the throttle's gap, each step. */
static int
spread_notches_each_step(int slave, const char* tag) {
    const int posted = g_posted_count;
    int rc = 0;
    for (int i = 0; i < 3; i++) {
        dsd_sleep_ms(200);
        if (queue_input(slave, "\033[<65;10;5M") != 0) {
            return 1;
        }
        rc |= frame(tag);
    }
    rc |= expect(tag, g_posted_count == posted + 3);
    return rc;
}

/* SIGCONT reaches whichever thread the kernel picks, so it is raised on a thread other than the UI's. The flag is
   relaxed on purpose: it orders nothing, so the capture's own flag must be safe across threads by itself. */
static atomic_int g_sigcont_sent;

static DSD_THREAD_RETURN_TYPE
send_sigcont(void* arg) {
    (void)arg;
    (void)raise(SIGCONT);
    atomic_store_explicit(&g_sigcont_sent, 1, memory_order_relaxed);
    DSD_THREAD_RETURN;
}

static int
sigcont_from_another_thread(dsd_thread_t* sender) {
    atomic_store_explicit(&g_sigcont_sent, 0, memory_order_relaxed);
    if (dsd_thread_create(sender, send_sigcont, NULL) != 0) {
        DSD_FPRINTF(stderr, "could not start the SIGCONT thread\n");
        return 1;
    }
    while (!atomic_load_explicit(&g_sigcont_sent, memory_order_relaxed)) {
        dsd_sleep_ms(1);
    }
    return 0;
}

/*
 * Ctrl+Z / fg, as ncurses' SIGTSTP handler runs it: endwin(), the stop, SIGCONT, then doupdate(), which resumes the
 * mouse and so turns tracking back on (the parse mask stays set). With @p frame_between a frame runs between SIGCONT
 * and that doupdate(), as when the handler runs on another thread, and the frame's own refresh resumes. By the next
 * frame the tracking is the capture's again, and idle frames after it write nothing.
 */
static int
resume_restores_tracking(const char* tag, int frame_between) {
    const char want = ui_mouse_capture_is_on() ? 'h' : 'l';
    screen_output_clear();
    g_step = "endwin() before a resume";
    endwin();
    drain_screen_output();
    dsd_thread_t sender;
    int rc = sigcont_from_another_thread(&sender);
    if (rc != 0) {
        return rc;
    }
    if (frame_between) {
        rc |= frame(tag);
    } else {
        doupdate();
        drain_screen_output();
    }
    if (last_tracking() != 'h') {
        DSD_FPRINTF(stderr, "%s: the resume did not turn tracking on, so this case shows nothing\n", tag);
        rc = 1;
    }
    rc |= frame(tag);
    if (last_tracking() != want) {
        DSD_FPRINTF(stderr, "%s: tracking '%c' after the resume (want '%c')\n", tag, last_tracking(), want);
        rc = 1;
    }
    screen_output_clear();
    for (int i = 0; i < 3; i++) {
        rc |= frame(tag);
    }
    const size_t idle_len = screen_output_len();
    if (idle_len != 0U) {
        DSD_FPRINTF(stderr, "%s: idle frames wrote %zu bytes\n", tag, idle_len);
        rc = 1;
    }
    /* Joined only now: a join orders the threads, and it would hide a race on the capture's flag. */
    (void)dsd_thread_join(sender);
    return rc;
}

/* The terminal closes (the capture's close, then dsd_terminal_close()'s endwin()) and a reopened one refreshes
   (initscr() again is a no-op that keeps the screen, so its first doupdate() is a resume): nothing turns tracking on
   before a capture asks for it. */
static int
reopen_leaves_tracking_off(void) {
    g_step = "the terminal's close and reopen";
    dsd_neo_ui_async_test_close_curses_if_opened(1);
    drain_screen_output();
    int rc = expect("closed: tracking off", ui_mouse_capture_is_on() == 0 && last_tracking() == 'l');
    screen_output_clear();
    endwin();
    doupdate();
    drain_screen_output();
    rc |= expect("reopened: tracking stays off", !screen_wrote("\033[?1006;1000h"));
    return rc;
}

static int
run_transitions(int slave) {
    int rc = capture_on_and_step(slave, "first capture");
    rc |= wheel_burst_steps_once(slave, "wheel burst", 60);
    rc |= spread_notches_each_step(slave, "spread notches");
    /* The burst's plain key reached the handler; the cases below count from none. */
    g_handled_keys = 0;
    rc |= expect("tracking turned on", screen_wrote("\033[?1006;1000h"));

    screen_output_clear();
    g_menu_open = 1;
    rc |= frame("menu open");
    rc |= expect("menu open: capture off", ui_mouse_capture_is_on() == 0);
    rc |= expect("menu open: tracking off", screen_wrote("\033[?1006;1000l"));
    rc |= queued_clicks_never_block(slave, "menu open");

    rc |= capture_on_and_step(slave, "menu closed");
    g_osnap->frontend_terminal_display.mouse_wheel = 0;
    rc |= frame("setting off");
    rc |= expect("setting off: capture off", ui_mouse_capture_is_on() == 0);
    rc |= queued_clicks_never_block(slave, "setting off");

    rc |= capture_on_and_step(slave, "setting on");
    g_osnap->scanner_mode = 0;
    rc |= frame("scan stopped");
    rc |= expect("scan stopped: capture off", ui_mouse_capture_is_on() == 0);
    rc |= queued_clicks_never_block(slave, "scan stopped");

    rc |= resume_restores_tracking("resumed, capture off", 0);
    rc |= resume_restores_tracking("resumed in a frame, capture off", 1);
    rc |= queued_clicks_never_block(slave, "resumed");
    rc |= capture_on_and_step(slave, "before a resume");
    rc |= resume_restores_tracking("resumed, capture on", 0);
    rc |= spread_notches_each_step(slave, "resumed, capture on");
    screen_output_clear();
    rc |= reopen_leaves_tracking_off();
    return rc;
}

/* The terminal under test: a pty whose slave is the screen's input and, dup'ed onto stdout, its output. */
typedef struct {
    int slave;
    int saved_stdout;
    FILE* in;
    SCREEN* screen;
    dsd_thread_t reader;
    int reader_started;
} pty_screen;

static int
open_pty(pty_screen* p) {
    g_master = posix_openpt(O_RDWR | O_NOCTTY);
    if (g_master < 0 || grantpt(g_master) != 0 || unlockpt(g_master) != 0) {
        return -1;
    }
    const char* name = ptsname(g_master);
    if (name == NULL) {
        return -1;
    }
    p->slave = open(name, O_RDWR | O_NOCTTY);
    if (p->slave < 0) {
        return -1;
    }
    struct winsize ws = {.ws_row = 24, .ws_col = 80};
    (void)ioctl(g_master, TIOCSWINSZ, &ws);
    (void)fcntl(g_master, F_SETFL, fcntl(g_master, F_GETFL) | O_NONBLOCK);
    if (dsd_thread_create(&p->reader, screen_reader, NULL) != 0) {
        return -1;
    }
    p->reader_started = 1;
    return 0;
}

/* Undo whatever open_screen() got to; the test's own report goes to the restored stdout. */
static void
close_screen(pty_screen* p) {
    if (p->screen != NULL) {
        endwin();
        drain_screen_output();
        delscreen(p->screen);
    }
    if (p->reader_started) {
        (void)dsd_mutex_lock(&g_out_lock);
        g_reader_stop = 1;
        (void)dsd_mutex_unlock(&g_out_lock);
        (void)dsd_thread_join(p->reader);
    }
    (void)fflush(stdout);
    if (p->saved_stdout >= 0) {
        (void)dsd_dup2(p->saved_stdout, STDOUT_FILENO);
        (void)close(p->saved_stdout);
    }
    if (p->in != NULL) {
        (void)fclose(p->in);
    } else if (p->slave >= 0) {
        (void)close(p->slave);
    }
    if (g_master >= 0) {
        (void)close(g_master);
    }
}

/* The screen writes to stdout, as initscr() does: putp() (the tracking switch) always writes there. Returns -1 with
   the reason in @p why when the host cannot run the test. */
static int
open_screen(pty_screen* p, const char** why) {
    if (open_pty(p) != 0) {
        *why = "no pty";
        return -1;
    }
    p->in = fdopen(p->slave, "r");
    if (p->in == NULL) {
        *why = "fdopen failed";
        return -1;
    }
    (void)fflush(stdout);
    p->saved_stdout = dup(STDOUT_FILENO);
    if (p->saved_stdout < 0 || dsd_dup2(p->slave, STDOUT_FILENO) < 0) {
        *why = "dup2 failed";
        return -1;
    }
    /* The admission reads TERM and NCURSES_GPM_TERMS through the override store; the screen is xterm-256color. */
    (void)dsd_env_set(DSD_ENV_LAYER_SESSION, "TERM", "xterm-256color");
    (void)dsd_env_set(DSD_ENV_LAYER_SESSION, "NCURSES_GPM_TERMS", NULL);
    p->screen = newterm("xterm-256color", stdout, p->in);
    if (p->screen == NULL) {
        *why = "no xterm-256color terminfo";
        return -1;
    }
    (void)set_term(p->screen);
    cbreak();
    noecho();
    drain_screen_output();
    return 0;
}

static int
run_on_screen(int slave) {
    static dsd_opts live_opts;
    static dsd_state live_state;
    static dsd_opts osnap;
    static dsd_state ssnap;
    DSD_MEMSET(&osnap, 0, sizeof(osnap));
    DSD_MEMSET(&ssnap, 0, sizeof(ssnap));
    osnap.frontend_kind = DSD_FRONTEND_TERMINAL;
    g_osnap = &osnap;
    g_ssnap = &ssnap;
    dsd_neo_ui_async_test_set_context(&live_opts, &live_state);

    (void)signal(SIGALRM, on_alarm);
    (void)alarm(20);
    int rc = run_transitions(slave);
    (void)alarm(0);
    return rc;
}

#endif /* UI_KEY_HAVE_WHEEL */

int
main(void) {
#if !UI_KEY_HAVE_WHEEL
    printf("UI_MOUSE_PTY: skipped (this ncurses has no button 5)\n");
    return kSkip;
#else
    pty_screen p = {.slave = -1, .saved_stdout = -1, .in = NULL, .screen = NULL, .reader_started = 0};
    const char* why = NULL;
    if (dsd_mutex_init(&g_out_lock) != 0) {
        DSD_FPRINTF(stderr, "UI_MOUSE_PTY: mutex init failed\n");
        return 1;
    }
    if (open_screen(&p, &why) != 0) {
        close_screen(&p);
        printf("UI_MOUSE_PTY: skipped (%s)\n", why);
        return kSkip;
    }
    int rc = run_on_screen(p.slave);
    close_screen(&p);
    if (rc == 0) {
        printf("UI_MOUSE_PTY: OK\n");
    }
    return rc;
#endif
}

// NOLINTEND(misc-use-internal-linkage)
