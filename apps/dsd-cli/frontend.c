// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "frontend.h"

#include <dsd-neo/core/opts.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/engine/engine.h"

#if DSD_CLI_HAS_TERMINAL_UI
#include <dsd-neo/ui/ui_async.h>
#endif

#ifndef DSD_CLI_HAS_TERMINAL_UI
#define DSD_CLI_HAS_TERMINAL_UI 0
#endif

#if DSD_CLI_HAS_TERMINAL_UI
static int
dsd_cli_terminal_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)context;
    if (ui_start(opts, state) != 0) {
        DSD_FPRINTF(stderr, "Failed to start terminal frontend\n");
        return -1;
    }
    return 0;
}

static void
dsd_cli_terminal_stop(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    (void)context;
    ui_stop();
}
#endif

int
dsd_cli_frontend_run(dsd_opts* opts, dsd_state* state) {
    if (!opts) {
        return -1;
    }

    const dsd_engine_lifecycle_hooks* run_hooks = NULL;
#if DSD_CLI_HAS_TERMINAL_UI
    dsd_engine_lifecycle_hooks lifecycle_hooks = {0};
#endif
    if (dsd_opts_frontend_active(opts)) {
        if (!dsd_opts_frontend_is_terminal(opts)) {
            DSD_FPRINTF(stderr, "Unsupported frontend kind: %d\n", (int)opts->frontend_kind);
            return 1;
        }
#if DSD_CLI_HAS_TERMINAL_UI
        lifecycle_hooks = (dsd_engine_lifecycle_hooks){
            .start = dsd_cli_terminal_start,
            .stop = dsd_cli_terminal_stop,
            .context = NULL,
        };
        run_hooks = &lifecycle_hooks;
#else
        (void)state;
        DSD_FPRINTF(stderr, "Terminal frontend requested, but this build was configured with "
                            "DSD_ENABLE_TERMINAL_UI=OFF\n");
        return 1;
#endif
    }

    return dsd_engine_run_with_lifecycle(opts, state, run_hooks);
}
