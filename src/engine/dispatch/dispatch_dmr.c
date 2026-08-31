// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
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
dmr_is_ms_data_synctype(int synctype) {
    return synctype == DSD_SYNC_DMR_MS_DATA;
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

/*
 * Trunking declines the direct-mode paths outright: a trunked system's traffic is on the
 * channels the control channel grants, not on MS/DM. Nothing is read, so say the frame was
 * withheld rather than let it read as a productive frame that happened to consume nothing --
 * the SPS hunt refuses consumption credit below a frame's worth, so the search that found
 * the sync would stand charged with nothing ever paying it back (#392).
 */
static dsd_frame_verdict
dmr_bootstrap_ms_if_enabled(dsd_opts* opts, dsd_state* state) {
    dmr_open_mbe_out_if_needed(opts, state);
    if (opts->trunk_enable == 0) {
        dmrMSBootstrap(opts, state);
        return DSD_FRAME_VERDICT_PRODUCTIVE;
    }
    return DSD_FRAME_VERDICT_WITHHELD;
}

static dsd_frame_verdict
dmr_bootstrap_mono(dsd_opts* opts, dsd_state* state) {
    if (opts->trunk_enable == 1 && DSD_SYNC_IS_DMR_BS(state->synctype)) {
        state->dmr_stereo = 1;
        dmrBSBootstrap(opts, state);
        state->dmr_stereo = 0;
        return DSD_FRAME_VERDICT_PRODUCTIVE;
    }
    state->dmr_mono_slot = 0;
    state->dmr_stereo = 0;
    return dmr_bootstrap_ms_if_enabled(opts, state);
}

static dsd_frame_verdict
dmr_handle_voice(dsd_opts* opts, dsd_state* state) {
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " VOICE        ");
    if (opts->dmr_mono == 1) {
        dmr_set_slot_lights(state);
        return dmr_bootstrap_mono(opts, state);
    }
    if (opts->dmr_stereo == 0 && state->synctype < DSD_SYNC_DMR_MS_VOICE) {
        dmr_set_slot_lights(state);
        return dmr_bootstrap_ms_if_enabled(opts, state);
    }
    if (opts->dmr_stereo == 1) {
        state->dmr_stereo = 1;
        if (state->synctype >= DSD_SYNC_DMR_MS_VOICE) {
            return dmr_bootstrap_ms_if_enabled(opts, state);
        }
        dmrBSBootstrap(opts, state);
        return DSD_FRAME_VERDICT_PRODUCTIVE;
    }
    /* Stereo off on an MS voice sync with mono off: nothing above ran, and nothing read the
     * frame. Productive is the status quo for a path that consumes nothing either way. */
    return DSD_FRAME_VERDICT_PRODUCTIVE;
}

static dsd_frame_verdict
dmr_handle_ms_data(dsd_opts* opts, dsd_state* state) {
    dmr_close_mbe_out_if_open(opts, state);
    if (opts->trunk_enable == 0) {
        dmrMSData(opts, state);
        return DSD_FRAME_VERDICT_PRODUCTIVE;
    }
    return DSD_FRAME_VERDICT_WITHHELD;
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

/*
 * Productive wherever a burst is actually read. DMR's per-burst verdicts live in the colour-
 * code lock and voice-open counters of src/protocol/dmr/dmr_confidence.c, which answer "is
 * this transmission real" across bursts rather than "did this burst validate"; a burst that
 * fails the lock is not the same thing as a burst that decoded nothing, and mapping one onto
 * the other would be guesswork. Left productive until DMR grows a per-burst answer (#391).
 *
 * The direct-mode paths under trunking are a different question and do have an answer: they
 * are not a burst this decoded badly, they are a burst it was told not to decode, so they
 * report WITHHELD (#392). That distinction is the whole of what is claimed here -- it says
 * where the symbols went, not whether the profile is right.
 */
dsd_frame_verdict
dsd_dispatch_handle_dmr(dsd_opts* opts, dsd_state* state) {
    if (!DSD_SYNC_IS_DMR(state->synctype)) {
        return DSD_FRAME_VERDICT_PRODUCTIVE;
    }

    /* A standalone RC burst is a one-shot 10 ms event: decode it without
     * touching slot lights, MBE output files, or per-slot call state. */
    if (state->synctype == DSD_SYNC_DMR_RC_DATA) {
        dmrRC(opts, state);
        return DSD_FRAME_VERDICT_PRODUCTIVE;
    }

    dmr_update_branding(state);
    state->nac = 0;

    if (dmr_is_voice_synctype(state->synctype)) {
        return dmr_handle_voice(opts, state);
    }
    if (dmr_is_ms_data_synctype(state->synctype)) {
        return dmr_handle_ms_data(opts, state);
    }
    dmr_handle_other_data(opts, state);
    return DSD_FRAME_VERDICT_PRODUCTIVE;
}
