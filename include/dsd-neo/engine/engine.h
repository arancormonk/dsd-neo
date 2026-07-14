// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_ENGINE_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_ENGINE_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*dsd_engine_lifecycle_start_fn)(dsd_opts* opts, dsd_state* state, void* context);
typedef void (*dsd_engine_lifecycle_stop_fn)(dsd_opts* opts, dsd_state* state, void* context);

typedef struct {
    dsd_engine_lifecycle_start_fn start;
    dsd_engine_lifecycle_stop_fn stop;
    void* context;
} dsd_engine_lifecycle_hooks;

/**
 * Run the engine with optional live-run lifecycle hooks.
 *
 * The start hook is called after engine and mode-specific setup succeeds, just
 * before live processing begins. If start succeeds, the stop hook is called
 * after live processing returns and before dsd_engine_cleanup() tears down
 * engine-owned state.
 */
int dsd_engine_run_with_lifecycle(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks);
void dsd_engine_cleanup(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_ENGINE_H_H */
