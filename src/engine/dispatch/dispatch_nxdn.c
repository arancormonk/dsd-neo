// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

int
dsd_dispatch_matches_nxdn(int synctype) {
    return DSD_SYNC_IS_NXDN(synctype);
}

dsd_frame_verdict
dsd_dispatch_handle_nxdn(dsd_opts* opts, dsd_state* state) {
    /* NXDN's sync word and LICH are weak enough that receiver noise clears both (#398), so
     * a frame reaching the body proves nothing on its own. nxdn_frame() answers with the
     * transmission's CRC confirmation, which noise never reaches. */
    return nxdn_frame(opts, state) != 0 ? DSD_FRAME_VERDICT_PRODUCTIVE : DSD_FRAME_VERDICT_UNPRODUCTIVE;
}
