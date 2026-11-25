// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The DMR module's channel query the app-control call view reads on a live state (dmr_t3_chan_effective_hz()), for
 * tests that compile the call view without the DMR module. It answers as the module does for a state it recorded
 * nothing for: the shared map row. APP_CONTROL_SNAPSHOT_EVENT_HISTORY links the real one.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/state_fwd.h"

long
dmr_t3_chan_effective_hz(const dsd_state* state, uint16_t lpcn) {
    if (state == NULL || lpcn == 0U || lpcn >= 0xFFFU) {
        return 0;
    }
    return state->trunk_chan_map[lpcn];
}
