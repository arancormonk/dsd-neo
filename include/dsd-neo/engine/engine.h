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

int dsd_engine_run(dsd_opts* opts, dsd_state* state);
void dsd_engine_cleanup(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_ENGINE_H_H */
