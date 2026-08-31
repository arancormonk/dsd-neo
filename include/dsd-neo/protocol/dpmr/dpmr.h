// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief dPMR interfaces the layers above the protocol need.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DPMR_DPMR_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DPMR_DPMR_H_

#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Forget the CRC evidence gathered for the current dPMR transmission.
 *
 * Defined in src/protocol/dpmr/dpmr_confirm.c and declared here so the engine can clear it
 * with the carrier; the rest of that module's interface is private to the protocol.
 */
void dpmr_confirm_reset(dsd_state* state);

/**
 * @brief Whether the current dPMR transmission has produced a CRC-verified CCH.
 *
 * The sticky per-transmission flag, not this frame's evidence. Dispatch reads it to decide
 * whether a sync alone may open a call row, which it may not until something has decoded.
 */
int dpmr_confirm_is_confirmed(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DPMR_DPMR_H_ */
