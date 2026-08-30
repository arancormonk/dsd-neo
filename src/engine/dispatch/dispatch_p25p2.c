// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/p25/p25.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

int
dsd_dispatch_matches_p25p2(int synctype) {
    return DSD_SYNC_IS_P25P2(synctype);
}

/*
 * Always productive. processP2() returns nothing, and its per-burst FEC results are spread
 * across the MAC/ISCH paths with no single answer to "did this burst validate" (#391).
 */
dsd_frame_verdict
dsd_dispatch_handle_p25p2(dsd_opts* opts, dsd_state* state) {
    processP2(opts, state);
    return DSD_FRAME_VERDICT_PRODUCTIVE;
}
