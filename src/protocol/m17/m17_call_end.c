// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Ending the slot's M17 call on an M17 end (<dsd-neo/protocol/m17/m17.h>): the EOT marker, a stream frame's EOS bit,
 * an IP stream's EOT, EOTX or DISC, and a packet's end. */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/m17/m17.h>

void
m17_end_call(const dsd_opts* opts, dsd_state* state, dsd_call_end_reason reason, int data_only) {
    dsd_call_snapshot call;
    /* An M17 call is handed to the call store ACTIVE or ended: one a fade ended (sync loss, recoverable) is made final
       by its terminator, without moving its end time, so the station's next transmission is not merged into its row;
       the store leaves any other ended call as it is. */
    if (state == NULL || dsd_call_state_get(state, 0U, &call) <= 0 || !DSD_SYNC_IS_M17(call.protocol)
        || (data_only && call.kind != DSD_CALL_KIND_DATA)) {
        return;
    }
    if (dsd_call_state_end_ex(state, 0U, 0.0, reason) > 0) {
        dsd_event_sync_slot((dsd_opts*)opts, state, 0U);
    }
}
