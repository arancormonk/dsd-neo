// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Terminal mouse wheel capture (see ui_mouse.h).
 */

#include "ui_mouse.h"

#include <stdbool.h>
#include <string.h>

#include "ui_keys.h"

#if UI_KEY_HAVE_WHEEL
#include <curses.h>
#endif
#if UI_KEY_HAVE_WHEEL && !defined(DSD_USE_PDCURSES)
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/env.h>
#include <dsd-neo/platform/platform.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#if DSD_PLATFORM_POSIX
#include <signal.h>
#endif
#endif

int
ui_mouse_xterm_admitted(const char* term, const char* gpm_terms, const char* kmous) {
    if (gpm_terms != NULL) {
        return 0;
    }
    if (term != NULL && strstr(term, "linux") != NULL) {
        return 0;
    }
    return kmous != NULL && kmous[0] == '\033' && kmous[1] == '[';
}

#if UI_KEY_HAVE_WHEEL

/* Every button press and release a terminal reports in normal tracking: buttons 1-3 as well as the wheel, so a
   click parses (and reads as nothing) instead of failing the mask. Button 4 and 5 presses are the wheel. */
#define UI_MOUSE_PARSE_MASK                                                                                            \
    (BUTTON1_PRESSED | BUTTON1_RELEASED | BUTTON2_PRESSED | BUTTON2_RELEASED | BUTTON3_PRESSED | BUTTON3_RELEASED      \
     | BUTTON4_PRESSED | BUTTON5_PRESSED)

#if defined(DSD_USE_PDCURSES) && defined(MOUSE_WHEEL_SCROLL)
/* PDCurses drops wheel events unless the mask carries its MOUSE_WHEEL_SCROLL. */
#define UI_MOUSE_ON_MASK (UI_MOUSE_PARSE_MASK | MOUSE_WHEEL_SCROLL)
#else
#define UI_MOUSE_ON_MASK UI_MOUSE_PARSE_MASK
#endif

static int g_available = -1; /* -1 = not asked yet on this terminal */
static int g_primed;         /* the mask and mouseinterval(0) are set */
static int g_on;

#if !defined(DSD_USE_PDCURSES)

/* ncurses' own tracking strings when terminfo has no XM string (lib_mouse.c init_xterm_mouse()). */
static const char k_xm_sgr1006[] = "\033[?1006;1000%?%p1%{1}%=%th%el%;";
static const char k_xm_x10[] = "\033[?1000%?%p1%{1}%=%th%el%;";

static const char* g_xm;

/* A terminfo string, or NULL when the terminal has none (absent, or cancelled: (char*)-1). */
static const char*
ui_mouse_terminfo_string(const char* cap) {
    const char* value = tigetstr(cap);
    if (value == NULL || (intptr_t)value == (intptr_t)-1) {
        return NULL;
    }
    return value;
}

static const char*
ui_mouse_xm_template(void) {
    const char* xm = ui_mouse_terminfo_string("XM");
    if (xm != NULL) {
        return xm;
    }
    return tigetnum("XM") == 1006 ? k_xm_sgr1006 : k_xm_x10;
}

static int
ui_mouse_terminal_admitted(void) {
    char term[256];
    char gpm_terms[256];
    const size_t term_need = dsd_env_copy("TERM", term, sizeof term);
    if (term_need > sizeof term) {
        return 0; /* too long to check for "linux" */
    }
    /* Any value counts, so a cut one does too. */
    const size_t gpm_need = dsd_env_copy("NCURSES_GPM_TERMS", gpm_terms, sizeof gpm_terms);
    return ui_mouse_xterm_admitted(term_need != 0 ? term : NULL, gpm_need != 0 ? gpm_terms : NULL,
                                   ui_mouse_terminfo_string("kmous"));
}

/* Between frames, after the last doupdate() flushed ncurses' own output: putp() writes to stdout, the terminal
   initscr() drives, outside ncurses' buffer, so the string is flushed here. */
static void
ui_mouse_set_tracking(int on) {
    const char* seq = tiparm(g_xm, on ? 1 : 0);
    if (seq != NULL) {
        (void)putp(seq);
        (void)fflush(stdout);
    }
}

#if DSD_PLATFORM_POSIX

/*
 * Ctrl+Z / fg: ncurses' SIGTSTP handler calls endwin(), stops, and once continued calls doupdate(), whose resume
 * sends the mouse-on string because the mask is set, whatever the capture is. The frame after it sends the capture's
 * own state once more. SIGCONT marks the resume; it is caught only where nothing else handles it (as ncurses catches
 * its own signals), and with SA_RESTART.
 */
static volatile sig_atomic_t g_resumed;
static int g_resume_watched;

static void
ui_mouse_on_sigcont(int sig) {
    (void)sig;
    g_resumed = 1;
}

static void
ui_mouse_watch_resume(void) {
    if (g_resume_watched) {
        return;
    }
    g_resume_watched = 1;
    struct sigaction old;
    if (sigaction(SIGCONT, NULL, &old) != 0 || (old.sa_flags & SA_SIGINFO) != 0 || old.sa_handler != SIG_DFL) {
        return;
    }
    struct sigaction act;
    DSD_MEMSET(&act, 0, sizeof act);
    act.sa_handler = ui_mouse_on_sigcont;
    (void)sigemptyset(&act.sa_mask);
    act.sa_flags = SA_RESTART;
    (void)sigaction(SIGCONT, &act, NULL);
}

/* 1 once per resume, after ncurses' resume ran: while isendwin() holds, the doupdate() that resumes (the handler's,
   on whichever thread took the signal, or this thread's next refresh) is still to come. */
static int
ui_mouse_take_resume(void) {
    if (!g_resumed || isendwin()) {
        return 0;
    }
    g_resumed = 0;
    return 1;
}

#else

static void
ui_mouse_watch_resume(void) {}

static int
ui_mouse_take_resume(void) {
    return 0;
}

#endif

#else

/* PDCurses has no suspend. */
static int
ui_mouse_take_resume(void) {
    return 0;
}

static void
ui_mouse_set_tracking(int on) {
    (void)mousemask(on ? (mmask_t)UI_MOUSE_ON_MASK : (mmask_t)0, NULL);
}

#endif

/* The first capture: the mask (which turns tracking on) and no click wait. ncurses keeps this mask until the close. */
static int
ui_mouse_prime(void) {
    (void)mouseinterval(0);
    if (mousemask((mmask_t)UI_MOUSE_ON_MASK, NULL) == 0) {
        g_available = 0; /* the terminal cannot report these events after all: hide the row */
        return 0;
    }
#if !defined(DSD_USE_PDCURSES)
    g_xm = ui_mouse_xm_template();
    ui_mouse_watch_resume();
#endif
    g_primed = 1;
    return 1;
}

static void
ui_mouse_forget(void) {
    g_available = -1;
    g_primed = 0;
    g_on = 0;
#if !defined(DSD_USE_PDCURSES)
    g_xm = NULL;
#endif
}

bool
ui_mouse_wheel_available(const void* ctx) {
    (void)ctx;
    if (g_available < 0) {
#if defined(DSD_USE_PDCURSES)
        g_available = 1;
#else
        if (stdscr == NULL) {
            return false; /* no terminfo to ask before curses opens */
        }
        g_available = ui_mouse_terminal_admitted();
#endif
    }
    return g_available == 1;
}

void
ui_mouse_capture_set(int on) {
    on = on ? 1 : 0;
    const int resumed = ui_mouse_take_resume();
    if (on == g_on) {
        if (resumed && g_primed) {
            ui_mouse_set_tracking(on);
        }
        return;
    }
    if (on && !ui_mouse_wheel_available(NULL)) {
        return;
    }
    if (on && !g_primed) {
        g_on = ui_mouse_prime();
        return;
    }
    ui_mouse_set_tracking(on);
    g_on = on;
}

int
ui_mouse_capture_is_on(void) {
    return g_on;
}

void
ui_mouse_capture_close(void) {
#if defined(DSD_USE_PDCURSES)
    if (g_on) {
        ui_mouse_set_tracking(0);
    }
#else
    /* mousemask(0) turns tracking off and releases the mask, which ncurses keeps past endwin(): a reopened terminal's
       first doupdate() is a resume, which would turn tracking on again before any capture. No key is read before the
       next capture sets the mask again. */
    if (g_primed) {
        (void)mousemask((mmask_t)0, NULL);
    }
#endif
    ui_mouse_forget();
}

#ifdef DSD_NEO_TEST_HOOKS
void
ui_mouse_reset_for_test(void) {
    ui_mouse_forget();
}
#endif

#else /* !UI_KEY_HAVE_WHEEL */

bool
ui_mouse_wheel_available(const void* ctx) {
    (void)ctx;
    return false;
}

void
ui_mouse_capture_set(int on) {
    (void)on;
}

int
ui_mouse_capture_is_on(void) {
    return 0;
}

void
ui_mouse_capture_close(void) {}

#ifdef DSD_NEO_TEST_HOOKS
void
ui_mouse_reset_for_test(void) {}
#endif

#endif
