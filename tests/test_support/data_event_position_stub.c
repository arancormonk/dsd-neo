// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The position data notice (issue #800) for tests that compile position producers directly and capture notices through
 * dsd_event_emit_data_notice_with_gps() (their own, or the one in data_event_gps_shim.c): the admitted record's GPS
 * text goes there; the position itself is not modelled. An unadmitted record or no text is refused as the real writer
 * refuses it.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/position.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state.h"
#include "dsd-neo/core/state_fwd.h"

int
dsd_event_emit_data_notice_with_position(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                         const dsd_call_observation* observation, dsd_event_category category,
                                         const char* notice, const dsd_position_fix* fix, const char* gps_text) {
    (void)category;
    if (fix == NULL || fix->rev == 0U || gps_text == NULL) {
        return -1;
    }
    return dsd_event_emit_data_notice_with_gps(opts, state, slot, observation, notice, gps_text);
}
