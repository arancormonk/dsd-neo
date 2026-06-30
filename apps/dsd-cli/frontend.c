// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "frontend.h"

#include <dsd-neo/app_control/frontend_provider.h>
#include <dsd-neo/core/opts.h>
#include <stddef.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/engine/engine.h"

#if DSD_CLI_HAS_TERMINAL_UI
#include <dsd-neo/ui/terminal_provider.h>
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

int
dsd_cli_frontend_select_from_registry(dsd_opts* opts, dsd_state* state, dsd_engine_lifecycle_hooks* hooks_storage,
                                      const dsd_engine_lifecycle_hooks** out_hooks,
                                      const dsd_frontend_provider* const* providers, size_t provider_count) {
    if (!opts || !hooks_storage || !out_hooks) {
        return -1;
    }
    *out_hooks = NULL;
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
    return 0;
}

int
dsd_cli_frontend_select(dsd_opts* opts, dsd_state* state, dsd_engine_lifecycle_hooks* hooks_storage,
                        const dsd_engine_lifecycle_hooks** out_hooks) {
    const dsd_frontend_provider* providers[1] = {NULL};
    size_t provider_count = 0;
#if DSD_CLI_HAS_TERMINAL_UI
    providers[provider_count++] = dsd_terminal_frontend_provider();
#endif
    return dsd_cli_frontend_select_from_registry(opts, state, hooks_storage, out_hooks, providers, provider_count);
}
