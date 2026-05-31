// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief D-STAR protocol decode entrypoints.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DSTAR_DSTAR_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DSTAR_DSTAR_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void processDSTAR(dsd_opts* opts, dsd_state* state);
void processDSTAR_HD(dsd_opts* opts, dsd_state* state);
void processDSTAR_SD(const dsd_opts* opts, dsd_state* state, uint8_t* sd);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DSTAR_DSTAR_H_ */
