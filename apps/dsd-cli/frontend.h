// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_APPS_DSD_CLI_FRONTEND_H_
#define DSD_NEO_APPS_DSD_CLI_FRONTEND_H_

#include <dsd-neo/app_control/frontend_provider.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/engine.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*dsd_cli_engine_runner)(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks,
                                     void* context);

int dsd_cli_frontend_run(dsd_opts* opts, dsd_state* state);
int dsd_cli_frontend_run_from_registry(dsd_opts* opts, dsd_state* state, const dsd_frontend_provider* const* providers,
                                       size_t provider_count, dsd_cli_engine_runner runner, void* runner_context);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_APPS_DSD_CLI_FRONTEND_H_ */
