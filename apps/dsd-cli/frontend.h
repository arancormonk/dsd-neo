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

int dsd_cli_frontend_select(dsd_opts* opts, dsd_state* state, dsd_engine_lifecycle_hooks* hooks_storage,
                            const dsd_engine_lifecycle_hooks** out_hooks);
int dsd_cli_frontend_select_from_registry(dsd_opts* opts, dsd_state* state, dsd_engine_lifecycle_hooks* hooks_storage,
                                          const dsd_engine_lifecycle_hooks** out_hooks,
                                          const dsd_frontend_provider* const* providers, size_t provider_count);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_APPS_DSD_CLI_FRONTEND_H_ */
