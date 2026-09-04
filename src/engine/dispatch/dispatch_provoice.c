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
 * 736 dibits per call on the same 9600/2 profile EDACS uses, and nothing inside them can
 * fail a check: no CRC, no BCH, no parity, and IMBE correction counts that never report a
 * bad frame. So processProVoice() reports the one thing that is checkable -- that a second
 * frame arrived behind its own exact 32-symbol sync word -- and a lone false match buys the
 * profile nothing, the way EDACS's BCH verdict beside it already does (#421).
 */
dsd_frame_verdict
dsd_dispatch_handle_provoice(dsd_opts* opts, dsd_state* state) {
    if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
        openMbeOutFile(opts, state);
    }
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " VOICE        ");
    return processProVoice(opts, state) != 0 ? DSD_FRAME_VERDICT_PRODUCTIVE : DSD_FRAME_VERDICT_UNPRODUCTIVE;
}
