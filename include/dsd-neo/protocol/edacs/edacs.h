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

/**
 * @brief Roll back an EDACS grant whose voice-channel tune failed after it was accepted in flight.
 *
 * A grant's tune accepted while still in flight marks the session tuned to the voice channel
 * (opts->trunk_is_tuned), and edacs() then acts on no control-channel message. When that tune fails the
 * radio reads the control channel again, so this returns the session there: the call the grant opened
 * ends, the control channel is tuned again (as eot_cc() does), the tuned state goes, and the failed
 * request, with nothing left staged for it, is let go so it withholds no frame. A return to the control
 * channel (eot_cc()'s or the rollback's) accepted in flight is watched too: one that fails is let go and
 * asked for again while the tuner takes the request, so no failure is left withholding every frame. All of
 * this runs only while EDACS still owns the recovery: a failure handed to another owner (trunking off,
 * another mode, a ledger reset or another owner's tune since) stays in the ledger for that owner or the
 * engine, and EDACS only forgets its reference; after a carrier boundary EDACS lets the record go and
 * touches nothing of the new carrier's; a retune also needs the tuner free for a protocol. The
 * engine runs it between frames, withheld ones included, and on each pass of its outer loop, under the
 * P25 SM tick guard. Two loads while nothing is in flight.
 */
void edacs_trunk_tick(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_EDACS_EDACS_H_ */
