// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * The NXDN held call (dsd_state::nxdn_call) at a call end NXDN's own message handling never sees: the engine's
 * no-carrier pass. Kept apart from nxdn_element.c, which owns the rest of the held call, so the engine links only this.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

void
nxdn_held_call_note_call_end(dsd_state* state) {
    if (!state) {
        return;
    }
    dsd_nxdn_call_identity* const id = &state->nxdn_call;
    if (!id->valid || id->gap || id->call_epoch == 0U) {
        return;
    }
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    if (dsd_call_state_get(state, 0U, &call) > 0 && call.phase != DSD_CALL_PHASE_ACTIVE && call.ended_m > 0.0
        && dsd_call_snapshot_transmission(&call) == id->call_epoch) {
        id->call_seen_m = call.ended_m;
    }
}
