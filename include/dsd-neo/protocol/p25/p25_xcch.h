// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 Phase 2 xCCH (SACCH/FACCH/LCCH) handler interfaces.
 *
 * Declares the xCCH MAC-PDU handlers implemented in
 * `src/protocol/p25/phase2/p25p2_xcch.c`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_XCCH_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_XCCH_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

void process_SACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int payload[180]);
void process_FACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int payload[156]);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_XCCH_H_H */
