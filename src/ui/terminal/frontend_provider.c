// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/ui/terminal_provider.h>

#include <dsd-neo/app_control/frontend_types.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/ui/ui_async.h>
#include <stdio.h>
#include "dsd-neo/app_control/frontend_provider.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/engine/engine.h"

static int
dsd_terminal_frontend_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)context;
    if (ui_start(opts, state) != 0) {
        DSD_FPRINTF(stderr, "Failed to start terminal frontend\n");
        return -1;
    }
    return 0;
}

static void
dsd_terminal_frontend_stop(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    (void)context;
    ui_stop();
}

static int
dsd_terminal_frontend_prepare(const dsd_opts* opts, const dsd_state* state, dsd_engine_lifecycle_hooks* out) {
    if (!opts || !out) {
        return -1;
    }
    (void)state;
    *out = (dsd_engine_lifecycle_hooks){
        .start = dsd_terminal_frontend_start,
        .stop = dsd_terminal_frontend_stop,
        .context = NULL,
    };
    return 0;
}

const dsd_frontend_provider*
dsd_terminal_frontend_provider(void) {
    static const dsd_frontend_provider provider = {
        .kind = DSD_FRONTEND_TERMINAL,
        .name = "terminal",
        .prepare = dsd_terminal_frontend_prepare,
    };
    return &provider;
}
