// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend provider registration contract.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_FRONTEND_PROVIDER_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_FRONTEND_PROVIDER_H_

#include <dsd-neo/core/frontend_types.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/engine.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsd_frontend_host_callbacks {
    void* context;
    int (*engine_finished)(void* context);
    void (*request_engine_stop)(void* context);
} dsd_frontend_host_callbacks;

enum {
    DSD_FRONTEND_PROVIDER_MAIN_THREAD_UI = 1u << 0,
};

typedef struct dsd_frontend_provider {
    dsd_frontend_kind kind;
    const char* name;
    int (*prepare)(const dsd_opts* opts, const dsd_state* state, dsd_engine_lifecycle_hooks* out);
    unsigned int flags;
    int (*run_main_loop)(const dsd_frontend_host_callbacks* host, void* context);
} dsd_frontend_provider;

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_FRONTEND_PROVIDER_H_ */
