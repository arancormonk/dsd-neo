// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * The NXDN held call (dsd_state::nxdn_call) at a call end NXDN's own message handling never sees: the engine's
 * no-carrier pass. Kept apart from nxdn_element.c, which owns the rest of the held call, so the engine links only this.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

int
nxdn_held_call_on_followed_route(const dsd_opts* opts, const dsd_state* state) {
    if (!state) {
        return 0;
    }
    const long int freq = state->nxdn_follow.freq_hz;
    const long int heard = state->nxdn_call.rx_freq_hz;
    return freq > 0 && dsd_opts_trunk_vc_followed(opts) && state->trunk_vc_freq[0] == freq
           && (heard <= 0 || heard == freq);
}

/* The bound call's end, a sync loss included, stamps when it was last seen. A held call still awaiting its call (a
 * follow no message has named since: voice with no VCALL or SCCH reads nothing) binds to the call that ended when that
 * call is the first to begin after it was named (nxdn_call_identity_refresh()'s rule), and only on its followed route,
 * as that rule (only a follow leaves it awaiting, so a call heard after the route was abandoned is never the one it
 * awaited), before another transmission replaces the ended one: else the next call would take the identity, its
 * priority and its dwell. */
void
nxdn_held_call_note_call_end(const dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return;
    }
    dsd_nxdn_call_identity* const id = &state->nxdn_call;
    if (!id->valid || id->gap) {
        return;
    }
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    if (dsd_call_state_get(state, 0U, &call) <= 0 || call.phase == DSD_CALL_PHASE_ACTIVE || call.ended_m <= 0.0) {
        return;
    }
    const uint64_t ended = dsd_call_snapshot_transmission(&call);
    if (id->call_epoch == 0U && ended > id->call_floor && nxdn_held_call_on_followed_route(opts, state)) {
        id->call_epoch = ended; // the call it awaited
    }
    if (id->call_epoch != 0U && ended == id->call_epoch) {
        id->call_seen_m = call.ended_m;
    }
}
