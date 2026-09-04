// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(misc-use-internal-linkage)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/unicode.h>
#include <dsd-neo/ui/ncurses.h>
#include <stdio.h>
#include <string.h>

#include "curses.h"

struct WINDOW {
    int unused;
};

WINDOW g_stdscr_storage;
WINDOW* stdscr = &g_stdscr_storage;

static int g_initscr_calls;
static int g_endwin_calls;
static int g_escdelay_value;
static int g_has_colors = 1;
static int g_use_default_colors_calls;
static int g_assume_default_colors_calls;
static int g_init_pair_calls;

static int g_isatty_result;
static int g_dup_result = 10;
static int g_devnull_open_ok = 1;
static int g_fileno_stderr = 2;
static int g_fileno_devnull = 99;
static FILE* g_devnull_file;
static int g_fileno_calls;
static int g_isatty_calls;
static int g_dup_calls;
static int g_dup2_calls;
static int g_dup2_old[4];
static int g_dup2_new[4];
static int g_close_calls;
static int g_closed_fd[4];
static int g_fopen_private_calls;
static const char* g_fopen_private_path;
static const char* g_fopen_private_mode;
static int g_unicode_init_calls;

WINDOW*
initscr(void) {
    g_initscr_calls++;
    return stdscr;
}

int
curs_set(int visibility) {
    (void)visibility;
    return 0;
}

void
timeout(int delay) {
    (void)delay;
}

int
start_color(void) {
    return 0;
}

int
keypad(WINDOW* win, int bf) {
    (void)win;
    (void)bf;
    return 0;
}

int
has_colors(void) {
    return g_has_colors;
}

int
use_default_colors(void) {
    g_use_default_colors_calls++;
    return 0;
}

int
assume_default_colors(int fg, int bg) {
    (void)fg;
    (void)bg;
    g_assume_default_colors_calls++;
    return 0;
}

int
init_pair(short pair, short fg, short bg) {
    (void)pair;
    (void)fg;
    (void)bg;
    g_init_pair_calls++;
    return 0;
}

int
noecho(void) {
    return 0;
}

int
cbreak(void) {
    return 0;
}

int
endwin(void) {
    g_endwin_calls++;
    return 0;
}

int
set_escdelay(int size) {
    g_escdelay_value = size;
    return 0;
}

int
wgetch(WINDOW* win) {
    (void)win;
    return ERR;
}

void
dsd_unicode_init_locale(void) {
    g_unicode_init_calls++;
}

int
dsd_fileno(FILE* fp) {
    g_fileno_calls++;
    if (fp == stderr) {
        return g_fileno_stderr;
    }
    if (fp == g_devnull_file) {
        return g_fileno_devnull;
    }
    return -1;
}

int
dsd_isatty(int fd) {
    g_isatty_calls++;
    assert(fd == g_fileno_stderr);
    return g_isatty_result;
}

int
dsd_dup(int oldfd) {
    g_dup_calls++;
    assert(oldfd == g_fileno_stderr);
    return g_dup_result;
}

int
dsd_dup2(int oldfd, int newfd) {
    assert(g_dup2_calls < 4);
    g_dup2_old[g_dup2_calls] = oldfd;
    g_dup2_new[g_dup2_calls] = newfd;
    g_dup2_calls++;
    return newfd;
}

int
dsd_close(int fd) {
    assert(g_close_calls < 4);
    g_closed_fd[g_close_calls++] = fd;
    return 0;
}

FILE*
dsd_fopen_private(const char* path, const char* mode) {
    g_fopen_private_calls++;
    g_fopen_private_path = path;
    g_fopen_private_mode = mode;
    if (!g_devnull_open_ok) {
        return NULL;
    }
    g_devnull_file = tmpfile();
    assert(g_devnull_file != NULL);
    return g_devnull_file;
}

const char*
dsd_null_device(void) {
    return "test-null";
}

static void
reset_stubs(void) {
    g_initscr_calls = 0;
    g_endwin_calls = 0;
    g_escdelay_value = 0;
    g_has_colors = 1;
    g_use_default_colors_calls = 0;
    g_assume_default_colors_calls = 0;
    g_init_pair_calls = 0;
    g_isatty_result = 0;
    g_dup_result = 10;
    g_devnull_open_ok = 1;
    g_devnull_file = NULL;
    g_fileno_calls = 0;
    g_isatty_calls = 0;
    g_dup_calls = 0;
    g_dup2_calls = 0;
    DSD_MEMSET(g_dup2_old, 0, sizeof(g_dup2_old));
    DSD_MEMSET(g_dup2_new, 0, sizeof(g_dup2_new));
    g_close_calls = 0;
    DSD_MEMSET(g_closed_fd, 0, sizeof(g_closed_fd));
    g_fopen_private_calls = 0;
    g_fopen_private_path = NULL;
    g_fopen_private_mode = NULL;
    g_unicode_init_calls = 0;
}

static void
test_non_tty_stderr_is_not_suppressed(void) {
    reset_stubs();
    g_isatty_result = 0;

    dsd_terminal_open(NULL, NULL);
    dsd_terminal_close();

    assert(g_unicode_init_calls == 1);
    assert(g_initscr_calls == 1);
    assert(g_escdelay_value == 25);
    assert(g_use_default_colors_calls == 1);
    assert(g_assume_default_colors_calls == 1);
    assert(g_init_pair_calls > 0);
    assert(g_isatty_calls == 1);
    assert(g_dup_calls == 0);
    assert(g_fopen_private_calls == 0);
    assert(g_dup2_calls == 0);
    assert(g_close_calls == 0);
    assert(g_endwin_calls == 1);
}

static void
test_tty_stderr_suppressed_once_and_restored(void) {
    reset_stubs();
    g_isatty_result = 1;
    g_dup_result = 42;

    dsd_terminal_open(NULL, NULL);
    dsd_terminal_open(NULL, NULL);
    dsd_terminal_close();

    assert(g_dup_calls == 1);
    assert(g_fopen_private_calls == 1);
    assert(strcmp(g_fopen_private_path, "test-null") == 0);
    assert(strcmp(g_fopen_private_mode, "w") == 0);
    assert(g_dup2_calls == 2);
    assert(g_dup2_old[0] == g_fileno_devnull);
    assert(g_dup2_new[0] == g_fileno_stderr);
    assert(g_dup2_old[1] == 42);
    assert(g_dup2_new[1] == g_fileno_stderr);
    assert(g_close_calls == 1);
    assert(g_closed_fd[0] == 42);
    assert(g_endwin_calls == 1);

    dsd_terminal_close();
    assert(g_dup2_calls == 2);
    assert(g_close_calls == 1);
    assert(g_endwin_calls == 2);
}

static void
test_devnull_open_failure_closes_backup(void) {
    reset_stubs();
    g_isatty_result = 1;
    g_dup_result = 77;
    g_devnull_open_ok = 0;

    dsd_terminal_open(NULL, NULL);
    dsd_terminal_close();

    assert(g_dup_calls == 1);
    assert(g_fopen_private_calls == 1);
    assert(g_dup2_calls == 0);
    assert(g_close_calls == 1);
    assert(g_closed_fd[0] == 77);
    assert(g_endwin_calls == 1);
}

int
main(void) {
    test_non_tty_stderr_is_not_suppressed();
    test_tty_stderr_suppressed_once_and_restored();
    test_devnull_open_failure_closes_backup();
    return 0;
}

// NOLINTEND(misc-use-internal-linkage)
