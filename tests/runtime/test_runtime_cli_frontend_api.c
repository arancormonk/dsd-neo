// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The CLI's lifecycle with the control API built in: the API server starts after the command session opens, and stops
 * after it closes -- closing the session cancels the commands still queued, and the server's stop is what reports
 * those results to its clients. The token goes to the server and is wiped from the options.
 */

#include "frontend.h"

#include <dsd-neo/api/api.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/frontend_types.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/engine/engine.h"

enum { MAX_CALLS = 8 };

static const char* g_calls[MAX_CALLS];
static int g_call_count;
static int g_token_received;

static void
record(const char* name) {
    if (g_call_count < MAX_CALLS) {
        g_calls[g_call_count] = name;
    }
    g_call_count++;
}

int
dsd_engine_run_with_lifecycle(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks) {
    if (hooks == NULL || hooks->start == NULL || hooks->stop == NULL) {
        return -1;
    }
    if (hooks->start(opts, state, hooks->context) != 0) {
        return -1;
    }
    hooks->stop(opts, state, hooks->context);
    return 0;
}

void
dsd_app_frontend_runtime_start(const dsd_opts* initial_opts, const dsd_state* initial_state) {
    (void)initial_opts;
    (void)initial_state;
    record("runtime_start");
}

void
dsd_app_frontend_runtime_stop(void) {
    record("runtime_stop");
}

int
dsd_api_start(const dsd_api_config* config) {
    record("api_start");
    g_token_received = config != NULL && strcmp(config->token, "tok3n") == 0;
    return 0;
}

void
dsd_api_stop(void) {
    record("api_stop");
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.frontend_kind = DSD_FRONTEND_NONE;
    opts.api_port = 4532;
    DSD_SNPRINTF(opts.api_token, sizeof opts.api_token, "%s", "tok3n");

    int rc = 0;
    if (dsd_cli_frontend_run(&opts, &state) != 0) {
        DSD_FPRINTF(stderr, "the run failed\n");
        rc = 1;
    }
    static const char* const k_want[] = {"runtime_start", "api_start", "runtime_stop", "api_stop"};
    const int want_count = (int)(sizeof k_want / sizeof k_want[0]);
    if (g_call_count != want_count) {
        DSD_FPRINTF(stderr, "got %d lifecycle calls, want %d\n", g_call_count, want_count);
        rc = 1;
    }
    for (int i = 0; i < want_count && i < g_call_count && i < MAX_CALLS; i++) {
        if (strcmp(g_calls[i], k_want[i]) != 0) {
            DSD_FPRINTF(stderr, "call %d: got %s want %s\n", i, g_calls[i], k_want[i]);
            rc = 1;
        }
    }
    if (!g_token_received) {
        DSD_FPRINTF(stderr, "the server did not get the token\n");
        rc = 1;
    }
    if (opts.api_token[0] != '\0') {
        DSD_FPRINTF(stderr, "the token was left in the options\n");
        rc = 1;
    }
    if (rc == 0) {
        puts("RUNTIME_CLI_FRONTEND_API: OK");
    }
    return rc;
}
