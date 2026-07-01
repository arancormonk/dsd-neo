// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "frontend.h"

#include <dsd-neo/app_control/frontend_provider.h>
#include <dsd-neo/app_control/frontend_types.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/shutdown.h>
#include <stddef.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/engine/engine.h"

#if DSD_CLI_HAS_TERMINAL_UI
#include <dsd-neo/ui/terminal_provider.h>
#endif

#if DSD_CLI_HAS_NATIVE_UI
#include <dsd-neo/ui/native_provider.h>
#endif

#ifndef DSD_CLI_HAS_TERMINAL_UI
#define DSD_CLI_HAS_TERMINAL_UI 0
#endif

#ifndef DSD_CLI_HAS_NATIVE_UI
#define DSD_CLI_HAS_NATIVE_UI 0
#endif

static const char*
dsd_cli_frontend_kind_name(dsd_frontend_kind kind) {
    switch (kind) {
        case DSD_FRONTEND_NONE: return "none";
        case DSD_FRONTEND_TERMINAL: return "terminal";
        case DSD_FRONTEND_NATIVE: return "native";
        default: return "unknown";
    }
}

static const dsd_frontend_provider*
dsd_cli_find_frontend_provider(dsd_frontend_kind kind, const dsd_frontend_provider* const* providers,
                               size_t provider_count) {
    if (!providers) {
        return NULL;
    }
    for (size_t i = 0; i < provider_count; i++) {
        if (providers[i] && providers[i]->kind == kind) {
            return providers[i];
        }
    }
    return NULL;
}

static int
dsd_cli_frontend_select_from_registry_ex(dsd_opts* opts, dsd_state* state, dsd_engine_lifecycle_hooks* hooks_storage,
                                         const dsd_engine_lifecycle_hooks** out_hooks,
                                         const dsd_frontend_provider** out_provider,
                                         const dsd_frontend_provider* const* providers, size_t provider_count) {
    if (!opts || !hooks_storage || !out_hooks) {
        return -1;
    }
    *out_hooks = NULL;
    if (out_provider) {
        *out_provider = NULL;
    }
    if (!dsd_opts_frontend_active(opts)) {
        return 0;
    }

    const dsd_frontend_provider* provider =
        dsd_cli_find_frontend_provider(opts->frontend_kind, providers, provider_count);
    if (!provider || !provider->prepare) {
        DSD_FPRINTF(stderr, "%s frontend provider unavailable\n", dsd_cli_frontend_kind_name(opts->frontend_kind));
        return -1;
    }
    *hooks_storage = (dsd_engine_lifecycle_hooks){0};
    if (provider->prepare(opts, state, hooks_storage) != 0) {
        DSD_FPRINTF(stderr, "Failed to prepare %s frontend provider\n",
                    provider->name ? provider->name : dsd_cli_frontend_kind_name(opts->frontend_kind));
        return -1;
    }
    *out_hooks = hooks_storage;
    if (out_provider) {
        *out_provider = provider;
    }
    return 0;
}

int
dsd_cli_frontend_select_from_registry(dsd_opts* opts, dsd_state* state, dsd_engine_lifecycle_hooks* hooks_storage,
                                      const dsd_engine_lifecycle_hooks** out_hooks,
                                      const dsd_frontend_provider* const* providers, size_t provider_count) {
    return dsd_cli_frontend_select_from_registry_ex(opts, state, hooks_storage, out_hooks, NULL, providers,
                                                    provider_count);
}

static size_t
dsd_cli_collect_frontend_providers(const dsd_frontend_provider** providers, size_t max_providers) {
    size_t provider_count = 0;
    (void)providers;
    (void)max_providers;
#if DSD_CLI_HAS_TERMINAL_UI
    if (provider_count < max_providers) {
        providers[provider_count++] = dsd_terminal_frontend_provider();
    }
#endif
#if DSD_CLI_HAS_NATIVE_UI
    if (provider_count < max_providers) {
        providers[provider_count++] = dsd_native_frontend_provider();
    }
#endif
    return provider_count;
}

int
dsd_cli_frontend_select(dsd_opts* opts, dsd_state* state, dsd_engine_lifecycle_hooks* hooks_storage,
                        const dsd_engine_lifecycle_hooks** out_hooks) {
    const dsd_frontend_provider* providers[2] = {NULL, NULL};
    size_t provider_count = dsd_cli_collect_frontend_providers(providers, sizeof providers / sizeof providers[0]);
    return dsd_cli_frontend_select_from_registry(opts, state, hooks_storage, out_hooks, providers, provider_count);
}

static int
dsd_cli_default_engine_runner(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks,
                              void* context) {
    (void)context;
    return dsd_engine_run_with_lifecycle(opts, state, hooks);
}

typedef struct dsd_cli_engine_worker {
    dsd_opts* opts;
    dsd_state* state;
    const dsd_engine_lifecycle_hooks* hooks;
    dsd_cli_engine_runner runner;
    void* runner_context;
    atomic_int finished;
    int rc;
} dsd_cli_engine_worker;

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    dsd_cli_engine_worker_main(void* arg) {
    dsd_cli_engine_worker* worker = (dsd_cli_engine_worker*)arg;
    if (worker && worker->runner) {
        worker->rc = worker->runner(worker->opts, worker->state, worker->hooks, worker->runner_context);
        atomic_store(&worker->finished, 1);
    }
    DSD_THREAD_RETURN;
}

static int
dsd_cli_engine_worker_finished(void* context) {
    dsd_cli_engine_worker* worker = (dsd_cli_engine_worker*)context;
    return worker ? atomic_load(&worker->finished) : 1;
}

static void
dsd_cli_engine_worker_request_stop(void* context) {
    dsd_cli_engine_worker* worker = (dsd_cli_engine_worker*)context;
    if (worker) {
        dsd_request_shutdown(worker->opts, worker->state);
    }
}

static int
dsd_cli_run_main_thread_provider(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks,
                                 const dsd_frontend_provider* provider, dsd_cli_engine_runner runner,
                                 void* runner_context) {
    if (!provider || !provider->run_main_loop || !runner) {
        return -1;
    }

    dsd_cli_engine_worker worker = {
        .opts = opts,
        .state = state,
        .hooks = hooks,
        .runner = runner,
        .runner_context = runner_context,
        .rc = 1,
    };
    atomic_store(&worker.finished, 0);

    dsd_thread_t engine_thread;
    if (dsd_thread_create(&engine_thread, dsd_cli_engine_worker_main, &worker) != 0) {
        return -1;
    }

    const dsd_frontend_host_callbacks host = {
        .context = &worker,
        .engine_finished = dsd_cli_engine_worker_finished,
        .request_engine_stop = dsd_cli_engine_worker_request_stop,
    };
    int ui_rc = provider->run_main_loop(&host, hooks ? hooks->context : NULL);
    if (!dsd_cli_engine_worker_finished(&worker)) {
        dsd_cli_engine_worker_request_stop(&worker);
    }
    (void)dsd_thread_join(engine_thread);

    return (ui_rc != 0) ? ui_rc : worker.rc;
}

int
dsd_cli_frontend_run_from_registry(dsd_opts* opts, dsd_state* state, const dsd_frontend_provider* const* providers,
                                   size_t provider_count, dsd_cli_engine_runner runner, void* runner_context) {
    if (!opts) {
        return -1;
    }
    if (!runner) {
        runner = dsd_cli_default_engine_runner;
    }

    dsd_engine_lifecycle_hooks lifecycle_hooks = {0};
    const dsd_engine_lifecycle_hooks* run_hooks = NULL;
    const dsd_frontend_provider* provider = NULL;
    if (dsd_cli_frontend_select_from_registry_ex(opts, state, &lifecycle_hooks, &run_hooks, &provider, providers,
                                                 provider_count)
        != 0) {
        return 1;
    }

    if (provider && (provider->flags & DSD_FRONTEND_PROVIDER_MAIN_THREAD_UI) != 0u) {
        return dsd_cli_run_main_thread_provider(opts, state, run_hooks, provider, runner, runner_context);
    }

    return runner(opts, state, run_hooks, runner_context);
}

int
dsd_cli_frontend_run(dsd_opts* opts, dsd_state* state) {
    const dsd_frontend_provider* providers[2] = {NULL, NULL};
    size_t provider_count = dsd_cli_collect_frontend_providers(providers, sizeof providers / sizeof providers[0]);
    return dsd_cli_frontend_run_from_registry(opts, state, providers, provider_count, dsd_cli_default_engine_runner,
                                              NULL);
}
