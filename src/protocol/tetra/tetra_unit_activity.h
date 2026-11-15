// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief TETRA rows of the unit activity log (<dsd-neo/core/unit_activity.h>): location update accept and reject
 * (EN 300 392-2 clause 16.9.2), and D-DISABLE / D-ENABLE (EN 300 392-7 annex A).
 *
 * The MM dispatch calls these for a complete PDU, which the MAC hands on only when it was clear or a key decrypted it.
 * A location update names its MS by the MAC address it came with (tetra_mm_addr): only a clear SSI or USSI is a radio;
 * an SMI is a management identity of unknown kind (never named as a radio), an event label or a missing address names
 * nobody, and an encrypted short identity adds no row. The address's MCC / MNC are the cell's network, never the
 * radio's home.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_UNIT_ACTIVITY_H_
#define DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_UNIT_ACTIVITY_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include "tetra_mm_pdu.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A location registration accepted for the addressed MS: the accept type (Table 16.68) as the value and the SSI
 * element, the ASSI / (V)ASSI given to it, as the assigned identity. */
void tetra_ua_lu_accept(const dsd_opts* opts, dsd_state* state, const tetra_mm_addr* addr,
                        const tetra_mm_lu_accept_t* accept);

/** A location registration refused for the addressed MS, the reject cause (Table 16.81) as the reason; the location
 * update type it answers (Table 16.67) is keyed but not shown. */
void tetra_ua_lu_reject(const dsd_opts* opts, dsd_state* state, const tetra_mm_addr* addr,
                        const tetra_mm_lu_reject_t* reject);

/** A D-DISABLE (temporary: stun, permanent: kill) or D-ENABLE (revive) command, at its intent or authenticated confirm
 * stage (Table A.68). The subject is the subscription's SSI with its ITSI network as the home, else, for an
 * equipment-only command, the TEI as an equipment identity (TEI_PARTIAL, never named). */
void tetra_ua_enable_disable(const dsd_opts* opts, dsd_state* state, const tetra_mm_enable_disable_t* command);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_UNIT_ACTIVITY_H_ */
