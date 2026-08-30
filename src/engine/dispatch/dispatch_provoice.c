// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/provoice/provoice.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

int
dsd_dispatch_matches_provoice(int synctype) {
    return synctype == DSD_SYNC_PROVOICE_POS || synctype == DSD_SYNC_PROVOICE_NEG;
}

/*
 * Always productive, and this one is a known hole. processProVoice() spends 736 dibits per
 * call on the same 9600/2 profile EDACS uses, with no CRC, no FEC verdict and no error
 * return -- its only failure path fires when the dibit callback fails, never on bad data.
 * A false ProVoice match therefore still buys the profile 736 symbols of dwell. Closing it
 * needs a confidence module shaped like src/protocol/nxdn/nxdn_confirm.c, not a threshold
 * on a counter nothing computes (#391).
 */
dsd_frame_verdict
dsd_dispatch_handle_provoice(dsd_opts* opts, dsd_state* state) {
    if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
        openMbeOutFile(opts, state);
    }
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " VOICE        ");
    processProVoice(opts, state);
    return DSD_FRAME_VERDICT_PRODUCTIVE;
}
