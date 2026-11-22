// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Which system the receiver is on, as the DMR Tier III channel record reads it, and the frequency DMR Tier III uses for
 * a channel (dmr_t3_view_internal.h): the snapshot's view and the live decoder state give the same answer, so the
 * call view, the editor's command and the published record agree.
 */

#include <dsd-neo/app_control/dmr_t3_view.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>
#include <stddef.h>
#include <stdint.h>
#include "dmr_t3_view_internal.h"

uint8_t
dsd_app_dmr_t3_other_system_here(const dsd_state* state) {
    if (state == NULL) {
        return 0U;
    }
    const uint8_t family = state->rx_id.family;
    if (family == (uint8_t)DSD_RXID_FAMILY_NONE) {
        return 0U;
    }
    if (family != (uint8_t)DSD_RXID_FAMILY_DMR) {
        return 1U;
    }
    const uint8_t trunk = dsd_rxid_dmr_trunk_named(state);
    return trunk != (uint8_t)DSD_RXID_DMR_TRUNK_NONE && trunk != (uint8_t)DSD_RXID_DMR_TRUNK_TIER3 ? 1U : 0U;
}

long
dsd_app_dmr_t3_channel_hz(const dsd_state* state, uint32_t channel) {
    if (state == NULL || channel == 0U || channel >= (uint32_t)DSD_TRUNK_CHAN_MAP_SIZE) {
        return 0;
    }
    const long shared = state->trunk_chan_map[channel];
    /* Past the LPCNs, or on another system (a Connect Plus LCN, a Capacity Plus LSN): the shared map's row. */
    if (channel >= (uint32_t)(DSD_UI_DMR_T3_ROWS - 1)) {
        return shared;
    }
    const dsd_ui_dmr_t3_view* view = dsd_app_dmr_t3_view(state);
    if (view != NULL) {
        return view->present != 0U && view->other_system_here == 0U ? view->channel_hz[channel] : shared;
    }
    return dsd_app_dmr_t3_other_system_here(state) ? shared : dmr_t3_chan_effective_hz(state, (uint16_t)channel);
}
