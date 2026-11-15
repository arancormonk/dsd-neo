// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#ifndef DSD_NEO_SRC_PROTOCOL_P25_PHASE1_P25P1_CURRENT_CALL_H_
#define DSD_NEO_SRC_PROTOCOL_P25_PHASE1_P25P1_CURRENT_CALL_H_

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <limits.h>

/* The open P25 Phase 1 call on slot 0, into @p call; 0 when slot 0 holds none. A Phase 1 frame takes a call's
 * identities only from this one: slot 0 keeps an ended call's, and another protocol's, after that call is over, and
 * a header or an ESS that decodes before this transmission's link control names nothing of its own. */
static inline int
p25p1_current_call(const dsd_state* state, dsd_call_snapshot* call) {
    return dsd_call_state_get(state, 0U, call) > 0 && call->phase == DSD_CALL_PHASE_ACTIVE
           && DSD_SYNC_IS_P25P1(call->protocol);
}

/* The talkgroup of p25p1_current_call(), or 0. */
static inline int
p25p1_current_call_target(const dsd_state* state) {
    dsd_call_snapshot call;
    return p25p1_current_call(state, &call) && call.ota_target_id <= INT_MAX ? (int)call.ota_target_id : 0;
}

#endif
