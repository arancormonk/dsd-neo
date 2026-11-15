// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 Link Control Word (LCW) decoder interfaces.
 *
 * Declares the LCW decoder entrypoint implemented in the P25 protocol library
 * as a standalone API.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_LCW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_LCW_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Decode an LCW carried by an active-call data unit such as LDU1. */
void p25_lcw(dsd_opts* opts, dsd_state* state, uint8_t lcw_bits[], uint8_t irrecoverable_errors);

/**
 * Decode an LCW carried by a TDULC terminator.
 *
 * Voice-user metadata is retained, but cannot emit a new voice-start event
 * after the terminator has ended the current transmission. The link control
 * speaks for the open P25 Phase 1 call, or for the ended one whose epoch is
 * @p ended_epoch: the call this TDULC ended, or the one the TDULC it repeats
 * ended (0: none). An older ended call the slot still holds is not its call.
 */
void p25_lcw_from_tdulc(dsd_opts* opts, dsd_state* state, uint8_t lcw_bits[], uint8_t irrecoverable_errors,
                        uint64_t ended_epoch);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_LCW_H_ */
