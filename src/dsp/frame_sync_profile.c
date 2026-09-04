// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/frame_sync.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"

/* Keep this order in sync with dsd_state::sps_hunt_idx. */
static const frame_sync_sps_profile k_frame_sync_sps_profiles[DSD_FRAME_SYNC_SPS_PROFILE_COUNT] = {
    {4800, 4}, {2400, 4}, {9600, 2}, {6000, 4}, {4800, 2},
};

const frame_sync_sps_profile*
frame_sync_sps_profile_for_index(int index) {
    if (index < 0 || index >= DSD_FRAME_SYNC_SPS_PROFILE_COUNT) {
        return &k_frame_sync_sps_profiles[DSD_FRAME_SYNC_SPS_PROFILE_4800_4];
    }
    return &k_frame_sync_sps_profiles[index];
}

/**
 * @brief Symbol rate of the SPS hunt profile the decoder is currently slicing for.
 *
 * Deliberately the decoder's own view rather than the front end's published rate:
 * a hunt step only queues its RTL profile request, which the demod thread applies
 * between input blocks, so the published rate lags the hunt by at least one block
 * on live input and never catches up at all under fast I/Q replay of a fixture
 * that fits in the output ring.
 */
int
dsd_frame_sync_active_profile_symbol_rate_hz(const dsd_state* state) {
    const int profile_index = state ? state->sps_hunt_idx : DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    return frame_sync_sps_profile_for_index(profile_index)->symbol_rate_hz;
}

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
