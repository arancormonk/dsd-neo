// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/dstar/dstar.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

int
dsd_dispatch_matches_dstar(int synctype) {
    return DSD_SYNC_IS_DSTAR(synctype);
}

dsd_frame_verdict
dsd_dispatch_handle_dstar(dsd_opts* opts, dsd_state* state) {
    if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
        openMbeOutFile(opts, state);
    }

    if (state->synctype == DSD_SYNC_DSTAR_VOICE_POS || state->synctype == DSD_SYNC_DSTAR_VOICE_NEG) {
        DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " VOICE        ");
        processDSTAR(opts, state);
        /* Productive by default, and this is the hole #391 measures. processDSTAR() spends
         * 1992 symbols with no return value, no error accumulator and no frame-count check;
         * it overwrites state->errs/errs2 21 times and reads them never, and its only CRC
         * (dstar_slow_data.c) is consulted on the sd_bytes[0] == 0x55 branch alone, about 1
         * match in 256 against noise. Nothing here can honestly say "unproductive", and a
         * threshold invented on state->errs would be the guess this contract forbids. */
        return DSD_FRAME_VERDICT_PRODUCTIVE;
    }

    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " DATA         ");
    /* The header CRC-16/X.25 runs before processDSTAR() consumes the voice frame behind it,
     * so a header that fails condemns the whole 2652-symbol call. */
    return processDSTAR_HD(opts, state) != 0 ? DSD_FRAME_VERDICT_PRODUCTIVE : DSD_FRAME_VERDICT_UNPRODUCTIVE;
}
