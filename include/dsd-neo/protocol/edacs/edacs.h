// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief EDACS protocol decode entrypoints.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_EDACS_EDACS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_EDACS_EDACS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decode one EDACS control-channel frame.
 *
 * @return 1 when the triple-voted 40-bit frames both re-derived their 12-bit BCH, 0 when
 *         the BCH failed. The check runs on the collected bits before the tuned-trunk
 *         early-out, so a frame the call declines to act on still reports the verdict it
 *         earned rather than a blanket failure -- the SPS hunt reads this to decide
 *         whether the 240 dibits bought any dwell (#391).
 */
int edacs(dsd_opts* opts, dsd_state* state);
void eot_cc(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_EDACS_EDACS_H_ */
