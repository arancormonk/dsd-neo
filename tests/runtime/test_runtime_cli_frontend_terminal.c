// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The CLI with the terminal frontend: the session thread runs the engine with SIGTSTP blocked, so every thread the run
 * starts from it (or a library starts from one of those) leaves Ctrl+Z to the terminal UI thread, the one thread that
 * unblocks it. Curses' SIGTSTP handler run on another thread, beside the UI thread's drawing, could leave the signal
 * ignored for the rest of the session. A run without the terminal frontend keeps taking Ctrl+Z on this thread.
 */

#include "frontend.h"

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/ui/ui_async.h>
#include <signal.h>
#include <stdio.h>
#include "dsd-neo/core/frontend_types.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/engine/engine.h"

static int g_engine_calls;
static int g_engine_saw_stop_blocked = -1;
static int g_ui_start_calls;
static int g_ui_stop_calls;

static int
stop_blocked_here(void) {
    sigset_t mask;
    if (pthread_sigmask(SIG_BLOCK, NULL, &mask) != 0) {
        return -1;
    }
    return sigismember(&mask, SIGTSTP) == 1 ? 1 : 0;
}

int
ui_start(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    g_ui_start_calls++;
    return 0;
}

void
ui_stop(void) {
    g_ui_stop_calls++;
}

int
dsd_engine_run_with_lifecycle(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks) {
    g_engine_calls++;
    g_engine_saw_stop_blocked = stop_blocked_here();
    if (hooks && hooks->start && hooks->start(opts, state, hooks->context) == 0 && hooks->stop) {
        hooks->stop(opts, state, hooks->context);
    }
    return 23;
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", label, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    rc |= expect_int("unblocked at start", dsd_thread_block_stop_signal(0), 0);

    opts.frontend_kind = DSD_FRONTEND_NONE;
    rc |= expect_int("headless return", dsd_cli_frontend_run(&opts, &state), 23);
    rc |= expect_int("headless engine calls", g_engine_calls, 1);
    rc |= expect_int("headless run takes Ctrl+Z", g_engine_saw_stop_blocked, 0);
    rc |= expect_int("headless no terminal", g_ui_start_calls, 0);

    opts.frontend_kind = DSD_FRONTEND_TERMINAL;
    g_engine_calls = 0;
    g_engine_saw_stop_blocked = -1;
    rc |= expect_int("terminal return", dsd_cli_frontend_run(&opts, &state), 23);
    rc |= expect_int("terminal engine calls", g_engine_calls, 1);
    rc |= expect_int("terminal run blocks Ctrl+Z", g_engine_saw_stop_blocked, 1);
    rc |= expect_int("terminal started", g_ui_start_calls, 1);
    rc |= expect_int("terminal stopped", g_ui_stop_calls, 1);
    /* The run's tail and exit take no stop: the block outlives the UI. */
    rc |= expect_int("blocked after the run", stop_blocked_here(), 1);

    if (rc == 0) {
        puts("RUNTIME_CLI_FRONTEND_TERMINAL: OK");
    }
    return rc;
}
