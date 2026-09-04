// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Shared frontend runtime wiring owned by app-control.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_FRONTEND_RUNTIME_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_FRONTEND_RUNTIME_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

void dsd_app_frontend_runtime_start(const dsd_opts* initial_opts, const dsd_state* initial_state);
void dsd_app_frontend_runtime_stop(void);

/**
 * @brief Raise the pending-redraw flag consumed by @ref dsd_app_frontend_redraw_consume.
 *
 * Safe from any thread. Backends raise it through the telemetry hooks; a frontend
 * calls it directly after changing something it rendered itself.
 */
void dsd_app_request_redraw(void);

/**
 * @brief Take the pending-redraw flag.
 * @return Nonzero when a redraw was requested since the last call; clears the flag.
 */
int dsd_app_frontend_redraw_consume(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_FRONTEND_RUNTIME_H_ */
