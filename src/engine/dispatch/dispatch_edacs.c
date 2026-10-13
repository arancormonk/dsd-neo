// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/edacs/edacs.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

int
dsd_dispatch_matches_edacs(int synctype) {
    return synctype == DSD_SYNC_EDACS_POS || synctype == DSD_SYNC_EDACS_NEG;
}

dsd_frame_verdict
dsd_dispatch_handle_edacs(dsd_opts* opts, dsd_state* state) {
    const uint32_t carrier_seq = state->carrier_seq;
    /* Zero means the 12-bit BCH over the triple-voted frame failed, so the 240 dibits behind
     * it were not an EDACS control frame. edacs() runs that check even when a tuned trunk
     * makes it forgo acting on the message, so declining to decode does not read here as
     * failing to (#391). */
    const int decoded = edacs(opts, state);
    /* A control frame that checked out on this carrier ends the EDACS/ProVoice capture: a false EDACS sync does not cut
       a ProVoice capture short, and another protocol's capture is not EDACS's to close (issue #661). */
    if (decoded != 0 && !dsd_state_frame_read_lost(state, carrier_seq)) {
        dsd_mbe_out_close_protocol(opts, state, DSD_MBE_OUT_SLOT1, DSD_SYNC_EDACS_POS);
    }
    return decoded != 0 ? DSD_FRAME_VERDICT_PRODUCTIVE : DSD_FRAME_VERDICT_UNPRODUCTIVE;
}
