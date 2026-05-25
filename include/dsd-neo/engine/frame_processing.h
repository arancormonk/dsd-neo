// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Engine-owned frame processing entrypoints.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_FRAME_PROCESSING_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_FRAME_PROCESSING_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

void processFrame(dsd_opts* opts, dsd_state* state);
void noCarrier(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_FRAME_PROCESSING_H_H */
