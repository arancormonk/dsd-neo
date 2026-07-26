// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 MAC VPDU handler interfaces.
 *
 * Declares VPDU processing entrypoints implemented in the P25 protocol code.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_VPDU_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_VPDU_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** P25 Phase 2 MAC PDU type carried by an XCCH block. */
typedef enum {
    P25_MAC_PDU_SIGNAL = 0,
    P25_MAC_PDU_PTT = 1,
    P25_MAC_PDU_END_PTT = 2,
    P25_MAC_PDU_IDLE = 3,
    P25_MAC_PDU_ACTIVE = 4,
    P25_MAC_PDU_HANGTIME = 6,
} p25_mac_pdu_type;

/**
 * @brief Process a P25 MAC VPDU block.
 *
 * @param type SACCH (1) or FACCH/bridged Phase 1 (0) transport.
 * @param pdu_type Outer Phase 2 MAC PDU type. Bridged Phase 1 callers use
 *                 P25_MAC_PDU_SIGNAL because they have no Phase 2 wrapper.
 */
void process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, p25_mac_pdu_type pdu_type,
                      unsigned long long int mac[24]);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_VPDU_H_ */
