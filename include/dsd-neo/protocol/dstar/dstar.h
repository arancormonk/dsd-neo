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

/**
 * @brief Decode one D-STAR voice superframe.
 *
 * @return 1 once this transmission has proved itself -- a CRC-16/X.25 on the RF header or
 *         on the header rebroadcast in slow data, or a second superframe arriving behind
 *         its own exact sync word -- and 0 until then. The 1992 symbols a superframe
 *         consumes are the largest block any handler takes, and on the 4800/2 hunt profile
 *         D-STAR is the only candidate, so an unproved one must not buy dwell (#391, #421).
 *         The evidence lives in dsd_state::dstar_confirmed and clears with the carrier.
 */
int processDSTAR(dsd_opts* opts, dsd_state* state);

/**
 * @brief Decode a D-STAR header frame and the voice frame that follows it.
 *
 * @return 1 when the header's CRC-16/X.25 matched, 0 otherwise. A header opens a
 *         transmission, so this stays the header's own verdict rather than riding evidence
 *         an earlier frame supplied; the voice superframe behind it reports separately
 *         through processDSTAR() (#391, #421).
 */
int processDSTAR_HD(dsd_opts* opts, dsd_state* state);
void processDSTAR_SD(const dsd_opts* opts, dsd_state* state, uint8_t* sd);

/**
 * @brief Forget the evidence gathered for the current D-STAR transmission.
 *
 * Defined in src/protocol/dstar/dstar_confirm.c and declared here so the engine can clear
 * it with the carrier; the rest of that module's interface is private to the protocol.
 */
void dstar_confirm_reset(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DSTAR_DSTAR_H_ */
