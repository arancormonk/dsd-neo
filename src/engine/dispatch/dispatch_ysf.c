// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/ysf/ysf.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

int
dsd_dispatch_matches_ysf(int synctype) {
    return DSD_SYNC_IS_YSF(synctype);
}

dsd_frame_verdict
dsd_dispatch_handle_ysf(dsd_opts* opts, dsd_state* state) {
    /* processYSF() answers with dsd_state::ysf_fich_confirmed, which is sticky for the
     * transmission rather than per frame: zero only until one FICH has decoded -- Golay
     * corrected and the CRC-16 over the corrected bits held. Before that, a failure leaves the
     * ~460 dibits behind it laid out on dt/fi nothing read off the air supplied, so they
     * validated nothing. After it, the same failure falls back to dt/fi a confirmed frame
     * supplied and ysf_handle_vd_type2() still synthesizes and plays voice from them, so the
     * frame is productive whatever its own FICH did. */
    return processYSF(opts, state) != 0 ? DSD_FRAME_VERDICT_PRODUCTIVE : DSD_FRAME_VERDICT_UNPRODUCTIVE;
}
