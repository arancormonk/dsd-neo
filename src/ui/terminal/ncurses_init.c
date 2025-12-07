// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * ncurses_init.c
 * ncurses initialization and cleanup
 */

#include <dsd-neo/core/dsd.h>

#include <locale.h>
#include <ncurses.h>
#include <string.h>

#ifdef __unix__
#include <unistd.h>
#endif

/* MBE library version string (populated by ncursesOpen) */
char mbeversionstr[25];

/* EDACS channel tree state (initialized in ncursesOpen) */
unsigned long long int edacs_channel_tree[33][6];

/* When ncurses UI is active, we temporarily suppress stderr to avoid stdio
 * mixed with the curses screen. Keep track so we can restore on close. */
#ifdef __unix__
static int s_stderr_suppressed = 0;
static int s_saved_stderr_fd = -1;
#endif

void
ncursesOpen(dsd_opts* opts, dsd_state* state) {

    UNUSED(opts);
    UNUSED(state);

    // menu overlays are nonblocking and do not gate demod processing
    mbe_printVersion(mbeversionstr);
    setlocale(LC_ALL, "");
    initscr(); // Initialize NCURSES screen window
    // Improve ESC-key responsiveness and UI ergonomics
    set_escdelay(25);
    curs_set(0); // hide cursor in main UI (menus will show it when needed)
    timeout(0);  // non-blocking input on stdscr; menus use nonblocking wtimeout
    start_color();
    // Ensure special keys (arrows, keypad Enter) are decoded as KEY_* constants
    keypad(stdscr, TRUE);

    if (has_colors()) {
        // Respect terminal themes: use default colors when supported
        use_default_colors();
        assume_default_colors(-1, -1);
#ifdef PRETTY_COLORS
        init_pair(1, COLOR_YELLOW, COLOR_BLACK);  // Yellow/Amber for frame sync/control channel, NV style
        init_pair(2, COLOR_RED, COLOR_BLACK);     // Red for Terminated Calls
        init_pair(3, COLOR_GREEN, COLOR_BLACK);   // Green for Active Calls
        init_pair(4, COLOR_CYAN, COLOR_BLACK);    // Cyan for Site Extra and Patches
        init_pair(5, COLOR_MAGENTA, COLOR_BLACK); // Magenta for no frame sync/signal
        init_pair(6, COLOR_WHITE, COLOR_BLACK);   // White Card Color Scheme
        init_pair(7, COLOR_BLUE, COLOR_BLACK);    // Blue on Black
        init_pair(8, COLOR_BLACK, COLOR_WHITE);   // Black on White
        init_pair(9, COLOR_RED, COLOR_WHITE);     // Red on White
        init_pair(10, COLOR_BLUE, COLOR_WHITE);   // Blue on White
        /* Quality bands for SNR sparkline */
        init_pair(11, COLOR_GREEN, COLOR_BLACK);  // good
        init_pair(12, COLOR_YELLOW, COLOR_BLACK); // moderate
        init_pair(13, COLOR_RED, COLOR_BLACK);    // poor
        init_pair(14, COLOR_YELLOW, COLOR_BLACK); // DSP status (explicit yellow)
        /* IDEN color palette (per-bandplan); 8 slots, wrap IDEN nibble modulo 8 */
        init_pair(21, COLOR_YELLOW, COLOR_BLACK);
        init_pair(22, COLOR_GREEN, COLOR_BLACK);
        init_pair(23, COLOR_CYAN, COLOR_BLACK);
        init_pair(24, COLOR_MAGENTA, COLOR_BLACK);
        init_pair(25, COLOR_BLUE, COLOR_BLACK);
        init_pair(26, COLOR_WHITE, COLOR_BLACK);
        init_pair(27, COLOR_RED, COLOR_BLACK);
        init_pair(28, COLOR_BLACK, COLOR_WHITE); /* high contrast alt */
#else
        init_pair(1, COLOR_WHITE, COLOR_BLACK);  // White Card Color Scheme
        init_pair(2, COLOR_WHITE, COLOR_BLACK);  // White Card Color Scheme
        init_pair(3, COLOR_WHITE, COLOR_BLACK);  // White Card Color Scheme
        init_pair(4, COLOR_WHITE, COLOR_BLACK);  // White Card Color Scheme
        init_pair(5, COLOR_WHITE, COLOR_BLACK);  // White Card Color Scheme
        init_pair(6, COLOR_WHITE, COLOR_BLACK);  // White Card Color Scheme
        init_pair(7, COLOR_WHITE, COLOR_BLACK);  // White Card Color Scheme
        init_pair(8, COLOR_BLACK, COLOR_WHITE);  // White Card Color Scheme
        init_pair(9, COLOR_BLACK, COLOR_WHITE);  // White Card Color Scheme
        init_pair(10, COLOR_BLACK, COLOR_WHITE); // White Card Color Scheme
        init_pair(11, COLOR_WHITE, COLOR_BLACK); // fallback
        init_pair(12, COLOR_WHITE, COLOR_BLACK);
        init_pair(13, COLOR_WHITE, COLOR_BLACK);
        init_pair(14, COLOR_YELLOW, COLOR_BLACK); // DSP status stays yellow even on white card scheme
        /* IDEN color palette fallback */
        for (int p = 21; p <= 28; p++) {
            init_pair(p, COLOR_WHITE, COLOR_BLACK);
        }
#endif
    }

    noecho();
    cbreak();

    // initialize EDACS channel tree
    memset(edacs_channel_tree, 0, sizeof(edacs_channel_tree));

    // When ncurses UI is active, suppress direct stderr logging to prevent
    // screen corruption from background fprintf calls in protocol paths.
    // This avoids mixed ncurses/stdio output overwriting the UI until resize.
    // However, if stderr has already been redirected (not a TTY), honor that
    // redirect so users can capture logs with e.g. 2>log.txt.
#ifdef __unix__
    if (!s_stderr_suppressed && isatty(fileno(stderr))) {
        // Backup current stderr FD, then redirect to /dev/null.
        int backup_fd = dup(fileno(stderr));
        if (backup_fd >= 0) {
            FILE* devnull = fopen("/dev/null", "w");
            if (devnull) {
                fflush(stderr);
                dup2(fileno(devnull), fileno(stderr));
                fclose(devnull);
                s_saved_stderr_fd = backup_fd;
                s_stderr_suppressed = 1;
            } else {
                // If we cannot open /dev/null, discard backup.
                close(backup_fd);
            }
        }
    }
#endif
}

void
ncursesClose(void) {
#ifdef __unix__
    // Restore stderr so exit-time logs (e.g., ring stats) are visible.
    if (s_stderr_suppressed) {
        fflush(stderr);
        if (s_saved_stderr_fd >= 0) {
            dup2(s_saved_stderr_fd, fileno(stderr));
            close(s_saved_stderr_fd);
            s_saved_stderr_fd = -1;
        }
        s_stderr_suppressed = 0;
    }
#endif
    endwin();
}
