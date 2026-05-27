// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

int
dsd_dispatch_matches_dpmr(int synctype) {
    return DSD_SYNC_IS_DPMR(synctype);
}

void
dsd_dispatch_handle_dpmr(dsd_opts* opts, dsd_state* state) {

    //dPMR
    if ((state->synctype == DSD_SYNC_DPMR_FS1_POS) || (state->synctype == DSD_SYNC_DPMR_FS1_NEG)) {
        /* dPMR Frame Sync 1 */
        DSD_FPRINTF(stderr, "dPMR Frame Sync 1 ");
        if (opts->mbe_out_f != NULL) {
            closeMbeOutFile(opts, state);
        }
    } else if ((state->synctype == DSD_SYNC_DPMR_FS2_POS) || (state->synctype == DSD_SYNC_DPMR_FS2_NEG)) {
        /* dPMR Frame Sync 2 */
        DSD_FPRINTF(stderr, "dPMR Frame Sync 2 ");

        state->nac = 0;
        state->lastsrc = 0;
        state->lasttg = 0;
        state->nac = 0;

        if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
            openMbeOutFile(opts, state);
        }
        DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " VOICE        ");
        processdPMRvoice(opts, state);

        return;

    } else if ((state->synctype == DSD_SYNC_DPMR_FS3_POS) || (state->synctype == DSD_SYNC_DPMR_FS3_NEG)) {
        /* dPMR Frame Sync 3 */
        DSD_FPRINTF(stderr, "dPMR Frame Sync 3 ");
        if (opts->mbe_out_f != NULL) {
            closeMbeOutFile(opts, state);
        }
    } else if ((state->synctype == DSD_SYNC_DPMR_FS4_POS) || (state->synctype == DSD_SYNC_DPMR_FS4_NEG)) {
        /* dPMR Frame Sync 4 */
        DSD_FPRINTF(stderr, "dPMR Frame Sync 4 ");
        if (opts->mbe_out_f != NULL) {
            closeMbeOutFile(opts, state);
        }
    }
    //dPMR
}
