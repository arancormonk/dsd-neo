// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/frame_sync.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

dsd_nxdn_variant
dsd_frame_sync_active_nxdn_variant(const dsd_opts* opts, const dsd_state* state) {
    if (!opts) {
        return DSD_NXDN_VARIANT_NONE;
    }

    const int nxdn48_enabled = opts->frame_nxdn48 == 1;
    const int nxdn96_enabled = opts->frame_nxdn96 == 1;
    if (nxdn48_enabled && !nxdn96_enabled) {
        return DSD_NXDN_VARIANT_48;
    }
    if (nxdn96_enabled && !nxdn48_enabled) {
        return DSD_NXDN_VARIANT_96;
    }
    if (!state || !nxdn48_enabled || !nxdn96_enabled) {
        return DSD_NXDN_VARIANT_NONE;
    }
    if (state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_2400_4) {
        return DSD_NXDN_VARIANT_48;
    }
    if (state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4) {
        return DSD_NXDN_VARIANT_96;
    }
    return DSD_NXDN_VARIANT_NONE;
}
