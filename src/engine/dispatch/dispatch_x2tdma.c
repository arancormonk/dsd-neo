// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/x2tdma/x2tdma.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

int
dsd_dispatch_matches_x2tdma(int synctype) {
    return DSD_SYNC_IS_X2TDMA(synctype);
}

void
dsd_dispatch_handle_x2tdma(dsd_opts* opts, dsd_state* state) {
    state->nac = 0;
    if (opts->errorbars == 1) {
        printFrameInfo(opts, state);
    }

    if (state->synctype == DSD_SYNC_X2TDMA_VOICE_NEG || state->synctype == DSD_SYNC_X2TDMA_VOICE_POS) {
        if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
            openMbeOutFile(opts, state);
        }
        DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " VOICE        ");
        processX2TDMAvoice(opts, state);
        return;
    }

    if (opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
    state->err_str[0] = 0;
    processX2TDMAdata(opts, state);
    const uint8_t slot = (uint8_t)(state->currentslot == 1 ? 1 : 0);
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, slot, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
        && DSD_SYNC_IS_X2TDMA(call.protocol) && dsd_call_state_end(state, slot, 0.0) > 0) {
        dsd_event_sync_slot(opts, state, slot);
    }
}
