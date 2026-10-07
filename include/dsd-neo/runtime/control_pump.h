// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime hook for pumping UI/control commands.
 *
 * Protocol/DSP code may call dsd_runtime_pump_controls() during long-running
 * loops to keep user controls responsive without depending on UI headers.
 * Never call it from code that can run under the P25 SM tick guard (processFrame()
 * does): the installed pump drains commands whose apply takes that non-recursive
 * guard (#554).
 *
 * The default behavior is a safe no-op until a control pump is registered.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_CONTROL_PUMP_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_CONTROL_PUMP_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*dsd_control_pump_fn)(dsd_opts* opts, dsd_state* state);

/**
 * @brief Register (or unregister) the global control pump.
 *
 * Passing NULL unregisters.
 */
void dsd_runtime_set_control_pump(dsd_control_pump_fn fn);

/**
 * @brief Pump pending UI/control commands if a pump is registered.
 *
 * Safe to call even when no pump is registered.
 */
void dsd_runtime_pump_controls(dsd_opts* opts, dsd_state* state);

typedef int (*dsd_controls_pending_fn)(void);

/**
 * @brief Register (or, with NULL, unregister) the query dsd_runtime_controls_pending() asks.
 */
void dsd_runtime_set_controls_pending(dsd_controls_pending_fn fn);

/**
 * @brief Whether a command waits to be pumped: 1 when the registered query says so, 0 without one.
 *
 * Asked only while an input waits on a silent source, so the decoder can leave the wait and apply it (issue #634).
 */
int dsd_runtime_controls_pending(void);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_CONTROL_PUMP_H_H */
