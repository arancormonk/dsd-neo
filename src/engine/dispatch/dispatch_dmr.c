// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

static void
dmr_update_branding(dsd_state* state) {
    // 0x10 intentionally does not update branding.
    if (state->dmr_mfid == 0x68) {
        DSD_SNPRINTF(state->dmr_branding, sizeof(state->dmr_branding), "%s", "  Hytera");
    } else if (state->dmr_mfid == 0x58) {
        DSD_SNPRINTF(state->dmr_branding, sizeof(state->dmr_branding), "%s", "    Tait");
    }
}

static int
dmr_is_voice_synctype(int synctype) {
    return synctype == DSD_SYNC_DMR_BS_VOICE_NEG || synctype == DSD_SYNC_DMR_BS_VOICE_POS
           || synctype == DSD_SYNC_DMR_MS_VOICE;
}

static int
dmr_is_ms_or_rc_data_synctype(int synctype) {
    return synctype == DSD_SYNC_DMR_MS_DATA || synctype == DSD_SYNC_DMR_RC_DATA;
}

static void
dmr_set_slot_lights(dsd_state* state) {
    DSD_SNPRINTF(state->slot1light, sizeof(state->slot1light), " slot1 ");
    DSD_SNPRINTF(state->slot2light, sizeof(state->slot2light), " slot2 ");
}

static void
dmr_open_mbe_out_if_needed(dsd_opts* opts, dsd_state* state) {
    if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
        openMbeOutFile(opts, state);
    }
}

static void
dmr_close_mbe_out_if_open(dsd_opts* opts, dsd_state* state) {
    if (opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
    if (opts->mbe_out_fR != NULL) {
        closeMbeOutFileR(opts, state);
    }
}

static void
dmr_bootstrap_ms_if_enabled(dsd_opts* opts, dsd_state* state) {
    dmr_open_mbe_out_if_needed(opts, state);
    if (opts->trunk_enable == 0) {
        dmrMSBootstrap(opts, state);
    }
}

static void
dmr_handle_voice(dsd_opts* opts, dsd_state* state) {
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " VOICE        ");
    if (opts->dmr_mono == 1) {
        state->dmr_stereo = 0;
        dmr_set_slot_lights(state);
        dmr_bootstrap_ms_if_enabled(opts, state);
        return;
    }
    if (opts->dmr_stereo == 0 && state->synctype < DSD_SYNC_DMR_MS_VOICE) {
        dmr_set_slot_lights(state);
        dmr_bootstrap_ms_if_enabled(opts, state);
    }
    if (opts->dmr_stereo == 1) {
        state->dmr_stereo = 1;
        if (state->synctype >= DSD_SYNC_DMR_MS_VOICE) {
            dmr_bootstrap_ms_if_enabled(opts, state);
        } else {
            dmrBSBootstrap(opts, state);
        }
    }
}

static void
dmr_handle_ms_or_rc_data(dsd_opts* opts, dsd_state* state) {
    dmr_close_mbe_out_if_open(opts, state);
    if (opts->trunk_enable == 0) {
        dmrMSData(opts, state);
    }
}

static void
dmr_handle_other_data(dsd_opts* opts, dsd_state* state) {
    if (opts->dmr_stereo == 0) {
        dmr_close_mbe_out_if_open(opts, state);
        state->err_str[0] = 0;
        dmr_set_slot_lights(state);
        dmr_data_sync(opts, state);
    }
    if (opts->dmr_stereo == 1) {
        dmr_close_mbe_out_if_open(opts, state);
        state->dmr_stereo = 0;
        dmr_set_slot_lights(state);
        dmr_data_sync(opts, state);
    }
}

int
dsd_dispatch_matches_dmr(int synctype) {
    return DSD_SYNC_IS_DMR(synctype);
}

void
dsd_dispatch_handle_dmr(dsd_opts* opts, dsd_state* state) {
    if (!DSD_SYNC_IS_DMR(state->synctype)) {
        return;
    }

    dmr_update_branding(state);
    state->nac = 0;

    if (dmr_is_voice_synctype(state->synctype)) {
        dmr_handle_voice(opts, state);
        return;
    }
    if (dmr_is_ms_or_rc_data_synctype(state->synctype)) {
        dmr_handle_ms_or_rc_data(opts, state);
        return;
    }
    dmr_handle_other_data(opts, state);
}
