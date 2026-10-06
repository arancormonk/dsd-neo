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

#ifndef DSD_CLI_HAS_TERMINAL_UI
#define DSD_CLI_HAS_TERMINAL_UI 0
#endif
#ifndef DSD_CLI_HAS_API
#define DSD_CLI_HAS_API 0
#endif

#if DSD_CLI_HAS_API
#include <dsd-neo/api/api.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/runtime/telemetry.h>
#endif

#if DSD_CLI_HAS_TERMINAL_UI
#include <dsd-neo/ui/ui_async.h>
#endif

#if DSD_CLI_HAS_API
static dsd_api_config g_api_cfg;
static int g_api_enabled = 0;
static int g_own_runtime = 0;
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

#if DSD_CLI_HAS_API
/* Hand the parsed options to the server's config and wipe the token from opts: the server keeps its own copy. */
static void
api_take_config(dsd_opts* opts, dsd_api_config* cfg) {
    DSD_MEMSET(cfg, 0, sizeof(*cfg));
    cfg->port = opts->api_port;
    DSD_SNPRINTF(cfg->bind_addr, sizeof cfg->bind_addr, "%s", opts->api_bindaddr);
    DSD_SNPRINTF(cfg->token, sizeof cfg->token, "%s", opts->api_token);
    DSD_SECURE_ZERO(opts->api_token, sizeof opts->api_token);
}
#endif

/* One start/stop pair drives the terminal frontend and/or the API server. Both need the app-control frontend
 * runtime (command session and telemetry hooks) open for the run; the terminal opens it itself, and the API opens it
 * here when it runs without the terminal. */
static int
dsd_cli_run_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)context;
#if DSD_CLI_HAS_TERMINAL_UI
    if (dsd_opts_frontend_is_terminal(opts) && dsd_cli_terminal_start(opts, state, NULL) != 0) {
        return -1;
    }
#endif
#if DSD_CLI_HAS_API
    if (g_api_enabled) {
        if (!dsd_opts_frontend_is_terminal(opts)) {
            dsd_app_frontend_runtime_start(opts, state);
            g_own_runtime = 1;
        }
        const int started = dsd_api_start(&g_api_cfg) == 0;
        DSD_SECURE_ZERO(g_api_cfg.token, sizeof g_api_cfg.token);
        if (started) {
            /* The feed's first look, before the decoder runs: every event row from here on reaches subscribers. */
            dsd_telemetry_publish_both_and_redraw(opts, state);
        } else {
            /* The decoder still runs, as it does when the RTL UDP retune control cannot bind; the reason is logged. */
            DSD_FPRINTF(stderr, "Failed to start the control/telemetry API; decoding continues without it\n");
            g_api_enabled = 0;
            if (g_own_runtime) {
                dsd_app_frontend_runtime_stop();
                g_own_runtime = 0;
            }
        }
    }
#endif
    (void)opts;
    (void)state;
    return 0;
}

static void
dsd_cli_run_stop(dsd_opts* opts, dsd_state* state, void* context) {
    (void)context;
    /* The command session closes first, cancelling the commands still queued; the server goes last, so its stop
       reports those results to the clients before it disconnects them. Requests in between are refused. */
#if DSD_CLI_HAS_API
    if (g_own_runtime) {
        dsd_app_frontend_runtime_stop();
        g_own_runtime = 0;
    }
#endif
#if DSD_CLI_HAS_TERMINAL_UI
    if (dsd_opts_frontend_is_terminal(opts)) {
        dsd_cli_terminal_stop(opts, state, NULL);
    }
#endif
#if DSD_CLI_HAS_API
    if (g_api_enabled) {
        dsd_api_stop();
        g_api_enabled = 0;
    }
#endif
    (void)opts;
    (void)state;
}

int
dsd_cli_frontend_run(dsd_opts* opts, dsd_state* state) {
    if (!opts) {
        return -1;
    }

    const dsd_engine_lifecycle_hooks* run_hooks = NULL;
    dsd_engine_lifecycle_hooks lifecycle_hooks = {0};
    int have_hooks = 0;

    if (dsd_opts_frontend_active(opts)) {
        if (!dsd_opts_frontend_is_terminal(opts)) {
            DSD_FPRINTF(stderr, "Unsupported frontend kind: %d\n", (int)opts->frontend_kind);
            return 1;
        }
#if !DSD_CLI_HAS_TERMINAL_UI
        (void)state;
        DSD_FPRINTF(stderr, "Terminal frontend requested, but this build was configured with "
                            "DSD_ENABLE_TERMINAL_UI=OFF\n");
        return 1;
#else
        have_hooks = 1;
#endif
    }

#if DSD_CLI_HAS_API
    api_take_config(opts, &g_api_cfg);
    g_api_enabled = (g_api_cfg.port > 0);
    if (g_api_enabled) {
        have_hooks = 1;
    }
#else
    if (opts->api_port > 0) {
        DSD_SECURE_ZERO(opts->api_token, sizeof opts->api_token);
        DSD_FPRINTF(stderr, "Control/telemetry API requested, but this build was configured with DSD_ENABLE_API=OFF\n");
        return 1;
    }
#endif

    if (have_hooks) {
        lifecycle_hooks = (dsd_engine_lifecycle_hooks){
            .start = dsd_cli_run_start,
            .stop = dsd_cli_run_stop,
            .context = NULL,
        };
        run_hooks = &lifecycle_hooks;
    }

    return dsd_engine_run_with_lifecycle(opts, state, run_hooks);
}
