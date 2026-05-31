// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/protocol_dispatch.h>

#include <stddef.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

static int
dsd_dispatch_known_frame(dsd_opts* opts, dsd_state* state) {
    const int synctype = state->synctype;

    if (dsd_dispatch_matches_nxdn(synctype)) {
        dsd_dispatch_handle_nxdn(opts, state);
        return 1;
    }
    if (dsd_dispatch_matches_dstar(synctype)) {
        dsd_dispatch_handle_dstar(opts, state);
        return 1;
    }
    if (dsd_dispatch_matches_dmr(synctype)) {
        dsd_dispatch_handle_dmr(opts, state);
        return 1;
    }
    if (dsd_dispatch_matches_x2tdma(synctype)) {
        dsd_dispatch_handle_x2tdma(opts, state);
        return 1;
    }
    if (dsd_dispatch_matches_provoice(synctype)) {
        dsd_dispatch_handle_provoice(opts, state);
        return 1;
    }
    if (dsd_dispatch_matches_edacs(synctype)) {
        dsd_dispatch_handle_edacs(opts, state);
        return 1;
    }
    if (dsd_dispatch_matches_ysf(synctype)) {
        dsd_dispatch_handle_ysf(opts, state);
        return 1;
    }
    if (dsd_dispatch_matches_m17(synctype)) {
        dsd_dispatch_handle_m17(opts, state);
        return 1;
    }
    if (dsd_dispatch_matches_p25p2(synctype)) {
        dsd_dispatch_handle_p25p2(opts, state);
        return 1;
    }
    if (dsd_dispatch_matches_dpmr(synctype)) {
        dsd_dispatch_handle_dpmr(opts, state);
        return 1;
    }
    if (dsd_dispatch_matches_p25p1(synctype)) {
        dsd_dispatch_handle_p25p1(opts, state);
        return 1;
    }

    return 0;
}

void
processFrame(dsd_opts* opts, dsd_state* state) {

    if (state->rf_mod == 1) {
        state->maxref = state->max * 0.80F;
        state->minref = state->min * 0.80F;
    } else {
        state->maxref = state->max;
        state->minref = state->min;
    }

    if (dsd_dispatch_known_frame(opts, state)) {
        return;
    }

    dsd_dispatch_handle_p25p1(opts, state);
}

const dsd_protocol_handler dsd_protocol_handlers[] = {
    {"NXDN", dsd_dispatch_matches_nxdn, dsd_dispatch_handle_nxdn, NULL},
    {"D-STAR", dsd_dispatch_matches_dstar, dsd_dispatch_handle_dstar, NULL},
    {"DMR", dsd_dispatch_matches_dmr, dsd_dispatch_handle_dmr, NULL},
    {"X2-TDMA", dsd_dispatch_matches_x2tdma, dsd_dispatch_handle_x2tdma, NULL},
    {"ProVoice", dsd_dispatch_matches_provoice, dsd_dispatch_handle_provoice, NULL},
    {"EDACS", dsd_dispatch_matches_edacs, dsd_dispatch_handle_edacs, NULL},
    {"YSF", dsd_dispatch_matches_ysf, dsd_dispatch_handle_ysf, NULL},
    {"M17", dsd_dispatch_matches_m17, dsd_dispatch_handle_m17, NULL},
    {"P25P2", dsd_dispatch_matches_p25p2, dsd_dispatch_handle_p25p2, NULL},
    {"dPMR", dsd_dispatch_matches_dpmr, dsd_dispatch_handle_dpmr, NULL},
    {"P25P1", dsd_dispatch_matches_p25p1, dsd_dispatch_handle_p25p1, NULL},
    {0, 0, 0, 0},
};
