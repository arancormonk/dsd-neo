// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime shutdown request API.
 *
 * Provides a non-exiting way for library code to request a graceful shutdown.
 * The caller is responsible for returning/breaking out of its control flow
 * after requesting shutdown.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_SHUTDOWN_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_SHUTDOWN_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

void dsd_request_shutdown(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_SHUTDOWN_H_ */
