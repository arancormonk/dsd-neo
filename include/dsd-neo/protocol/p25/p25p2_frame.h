// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 Phase 2 frame helpers.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25P2_FRAME_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25P2_FRAME_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

void p25_p2_frame_reset(void);
void process_ESS(dsd_opts* opts, dsd_state* state);
void process_2V(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25P2_FRAME_H_H */
