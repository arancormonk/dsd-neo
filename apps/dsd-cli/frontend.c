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
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/ui/ui_async.h>
#endif

#if DSD_CLI_HAS_TERMINAL_UI
static int
dsd_cli_terminal_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)context;
    if (!dsd_opts_frontend_is_terminal(opts)) {
        return 0;
    }
    if (ui_start(opts, state) != 0) {
        DSD_FPRINTF(stderr, "Failed to start terminal UI\n");
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
dsd_cli_frontend_select(dsd_opts* opts, dsd_state* state, dsd_engine_lifecycle_hooks* hooks_storage,
                        const dsd_engine_lifecycle_hooks** out_hooks) {
    if (!opts || !hooks_storage || !out_hooks) {
        return -1;
    }
    *out_hooks = NULL;
    if (!dsd_opts_frontend_active(opts)) {
        return 0;
    }
    if (!dsd_opts_frontend_is_terminal(opts)) {
        DSD_FPRINTF(stderr, "Unsupported frontend kind: %d\n", (int)opts->frontend_kind);
        return -1;
    }

#if DSD_CLI_HAS_TERMINAL_UI
    if (state) {
        dsd_app_telemetry_publish_opts_snapshot(opts);
        dsd_app_telemetry_publish_snapshot(state);
    }
    *hooks_storage = (dsd_engine_lifecycle_hooks){
        .start = dsd_cli_terminal_start,
        .stop = dsd_cli_terminal_stop,
        .context = NULL,
    };
    *out_hooks = hooks_storage;
    return 0;
#else
    (void)state;
    DSD_FPRINTF(stderr, "Terminal frontend requested, but this build was configured with DSD_ENABLE_TERMINAL_UI=OFF\n");
    return -1;
#endif
}
