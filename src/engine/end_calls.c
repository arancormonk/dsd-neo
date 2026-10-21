// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/analog_call.h>
#include <stdint.h>

void
dsd_engine_end_calls(dsd_opts* opts, dsd_state* state, double ended_m, dsd_call_end_reason reason, int sync_all) {
    if (!opts || !state) {
        return;
    }
    /* The analog call first (issue #637): its per-call recording closes under the row it was heard as, its mode and
       service name included, before the commit below clears that row. */
    dsd_engine_analog_call_end(opts, state);
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        if (dsd_call_state_end_ex(state, (uint8_t)slot, ended_m, reason) > 0 || sync_all) {
            dsd_event_sync_slot(opts, state, (uint8_t)slot);
        }
    }
}
