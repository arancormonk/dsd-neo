// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/edacs/edacs.h>
#include <stddef.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

int
dsd_dispatch_matches_edacs(int synctype) {
    return synctype == DSD_SYNC_EDACS_POS || synctype == DSD_SYNC_EDACS_NEG;
}

dsd_frame_verdict
dsd_dispatch_handle_edacs(dsd_opts* opts, dsd_state* state) {
    if (opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
    /* Zero means the 12-bit BCH over the triple-voted frame failed, so the 240 dibits behind
     * it were not an EDACS control frame. edacs() runs that check even when a tuned trunk
     * makes it forgo acting on the message, so declining to decode does not read here as
     * failing to (#391). */
    return edacs(opts, state) != 0 ? DSD_FRAME_VERDICT_PRODUCTIVE : DSD_FRAME_VERDICT_UNPRODUCTIVE;
}
