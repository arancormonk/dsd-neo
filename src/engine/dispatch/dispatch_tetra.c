// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA protocol dispatch integration
 */
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/tetra/tetra.h>

int dsd_dispatch_matches_tetra(int synctype) {
    return DSD_SYNC_IS_TETRA(synctype);
}

dsd_frame_verdict dsd_dispatch_handle_tetra(dsd_opts* opts, dsd_state* state) {
    if (state->lastsynctype == DSD_SYNC_TETRA_SB_POS ||
        state->lastsynctype == DSD_SYNC_TETRA_SB_NEG) {
        processTetraSBFrame(opts, state);
    } else {
        processTetraFrame(opts, state);
    }
    return DSD_FRAME_VERDICT_PRODUCTIVE;
}
