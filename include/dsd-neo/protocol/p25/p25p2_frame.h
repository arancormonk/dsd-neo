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
void process_2V(dsd_opts* opts, dsd_state* state);

/**
 * @brief Drop what the Phase 2 slots gathered on a carrier the receiver left (issue #575).
 *
 * The ESS_B fragments with their reliabilities, the partial voice superframe and any staged rekey go, as the no-carrier
 * pass drops them on a sync loss, and the slots' ESS_B is marked as the carrier left's: a 2V burst decodes no ESS until
 * the slot's next 4V burst collects a fragment on the new carrier. The carrier boundary and the decoder's own carrier
 * check run it.
 */
void p25p2_frame_forget_carrier(dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25P2_FRAME_H_H */
