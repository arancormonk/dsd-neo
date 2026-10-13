// SPDX-License-Identifier: GPL-3.0-or-later

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

int
dsd_dispatch_matches_tetra(int synctype) {
    return DSD_SYNC_IS_TETRA(synctype);
}

/*
 * One TETRA burst and its verdict. A training sequence is weak enough for receiver noise to match, so a burst proves
 * its profile only when it decoded a block whose CRC passed (a signalling block, or a speech frame that also re-encodes
 * like speech); a burst that decoded none consumed symbols and validated nothing.
 */
dsd_frame_verdict
dsd_dispatch_handle_tetra(dsd_opts* opts, dsd_state* state) {
    int proven = 0;
    if (state->lastsynctype == DSD_SYNC_TETRA_SB_POS || state->lastsynctype == DSD_SYNC_TETRA_SB_NEG) {
        proven = processTetraSBFrame(opts, state);
    } else {
        proven = processTetraFrame(opts, state);
    }
    return proven ? DSD_FRAME_VERDICT_PROFILE_PROVEN : DSD_FRAME_VERDICT_UNPRODUCTIVE;
}
