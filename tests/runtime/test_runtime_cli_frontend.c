// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "frontend.h"

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdio.h>
#include "dsd-neo/core/frontend_types.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/engine/engine.h"

static int g_engine_calls;
static int g_engine_hooks_present;

int
dsd_engine_run_with_lifecycle(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks) {
    (void)opts;
    (void)state;
    g_engine_calls++;
    g_engine_hooks_present = hooks ? 1 : 0;
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
    opts.frontend_kind = DSD_FRONTEND_NONE;
    g_engine_calls = 0;
    g_engine_hooks_present = 0;
    rc |= expect_int("headless return", dsd_cli_frontend_run(&opts, &state), 23);
    rc |= expect_int("headless engine calls", g_engine_calls, 1);
    rc |= expect_int("headless lifecycle hooks", g_engine_hooks_present, 0);

    opts.frontend_kind = DSD_FRONTEND_TERMINAL;
    g_engine_calls = 0;
    rc |= expect_int("disabled terminal return", dsd_cli_frontend_run(&opts, &state), 1);
    rc |= expect_int("disabled terminal engine calls", g_engine_calls, 0);
    rc |= expect_int("null options", dsd_cli_frontend_run(NULL, &state), -1);

    if (rc == 0) {
        puts("RUNTIME_CLI_FRONTEND: OK");
    }
    return rc;
}
