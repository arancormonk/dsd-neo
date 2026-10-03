// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "frontend.h"

#include <dsd-neo/core/opts.h>
#include <dsd-neo/runtime/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#endif

/* The shared frontend runtime is needed whenever a live frontend (terminal or
 * API server) has to open the command session and telemetry hooks. */
#if DSD_CLI_HAS_API || DSD_CLI_HAS_TERMINAL_UI
#include <dsd-neo/app_control/frontend_runtime.h>
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
static void
api_read_token_file(const char* path, char* out, size_t cap) {
    if (path == NULL || path[0] == '\0' || out == NULL || cap == 0U) {
        return;
    }
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    const size_t n = fread(out, 1U, cap - 1U, f);
    out[n] = '\0';
    (void)fclose(f);
    size_t len = strlen(out);
    while (len > 0U) {
        const char c = out[len - 1U];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            out[--len] = '\0';
        } else {
            break;
        }
    }
}

static void
api_resolve_config(const dsd_opts* opts, dsd_api_config* cfg) {
    DSD_MEMSET(cfg, 0, sizeof(*cfg));
    cfg->port = opts->api_port;
    cfg->max_clients = 0;
    DSD_SNPRINTF(cfg->bind_addr, sizeof cfg->bind_addr, "%s",
                 (opts->api_bindaddr[0] != '\0') ? opts->api_bindaddr : "127.0.0.1");
    DSD_SNPRINTF(cfg->token, sizeof cfg->token, "%s", opts->api_token);

    if (cfg->port <= 0) {
        const char* env = dsd_neo_env_get("DSD_NEO_API_PORT");
        if (env != NULL && env[0] != '\0') {
            char* end = NULL;
            const long p = strtol(env, &end, 10);
            if (end != NULL && *end == '\0' && p > 0 && p <= 65535) {
                cfg->port = (int)p;
            }
        }
    }
    if (opts->api_bindaddr[0] == '\0') {
        const char* env = dsd_neo_env_get("DSD_NEO_API_BIND");
        if (env != NULL && env[0] != '\0') {
            DSD_SNPRINTF(cfg->bind_addr, sizeof cfg->bind_addr, "%s", env);
        }
    }
    if (cfg->token[0] == '\0') {
        const char* env = dsd_neo_env_get("DSD_NEO_API_TOKEN");
        if (env != NULL && env[0] != '\0') {
            DSD_SNPRINTF(cfg->token, sizeof cfg->token, "%s", env);
        }
    }
    if (cfg->token[0] == '\0') {
        const char* token_file = (opts->api_token_file[0] != '\0') ? opts->api_token_file : NULL;
        if (token_file == NULL) {
            const char* env = dsd_neo_env_get("DSD_NEO_API_TOKEN_FILE");
            if (env != NULL && env[0] != '\0') {
                token_file = env;
            }
        }
        if (token_file != NULL) {
            api_read_token_file(token_file, cfg->token, sizeof cfg->token);
        }
    }
}
#endif

/* One start/stop pair drives the terminal frontend and/or the API server, both
 * of which need the app-control frontend runtime (command session + telemetry
 * hooks) open for the run. */
static int
dsd_cli_run_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)context;
#if DSD_CLI_HAS_TERMINAL_UI
    if (dsd_opts_frontend_is_terminal(opts)) {
        if (dsd_cli_terminal_start(opts, state, NULL) != 0) {
            return -1;
        }
    } else
#endif
    {
#if DSD_CLI_HAS_TERMINAL_UI || DSD_CLI_HAS_API
        dsd_app_frontend_runtime_start(opts, state);
#if DSD_CLI_HAS_API
        g_own_runtime = 1;
#else
        (void)state;
#endif
#else
        (void)opts;
        (void)state;
#endif
    }
#if DSD_CLI_HAS_API
    if (g_api_enabled) {
        if (dsd_api_start(&g_api_cfg) != 0) {
            DSD_FPRINTF(stderr, "Failed to start API server\n");
            /* The decoder still runs; the API is an optional surface. */
        }
    }
#endif
    return 0;
}

static void
dsd_cli_run_stop(dsd_opts* opts, dsd_state* state, void* context) {
    (void)context;
    (void)opts;
    (void)state;
#if DSD_CLI_HAS_API
    if (g_api_enabled) {
        dsd_api_stop();
    }
#endif
#if DSD_CLI_HAS_TERMINAL_UI
    if (dsd_opts_frontend_is_terminal(opts)) {
        dsd_cli_terminal_stop(opts, state, NULL);
        return;
    }
#endif
#if DSD_CLI_HAS_API
    if (g_own_runtime) {
        dsd_app_frontend_runtime_stop();
        g_own_runtime = 0;
    }
#endif
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
    api_resolve_config(opts, &g_api_cfg);
    /* The server keeps its own copy; drop the CLI copy from opts so it is not
     * retained for the process lifetime. */
    if (opts->api_token[0] != '\0') {
        DSD_SECURE_ZERO(opts->api_token, sizeof opts->api_token);
    }
    g_api_enabled = (g_api_cfg.port > 0);
    if (g_api_enabled) {
        have_hooks = 1;
    }
#else
    /* --api is inert when the API was not built in. */
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
