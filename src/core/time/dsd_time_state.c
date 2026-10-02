// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/decode_clock.h>

#include "dsd-neo/core/state_fwd.h"

void
dsd_mark_cc_sync(dsd_state* state) {
    if (!state) {
        return;
    }
    state->last_cc_sync_time = dsd_decode_time();
    state->last_cc_sync_time_m = dsd_decode_now_mono_s();
}

void
dsd_mark_vc_sync(dsd_state* state) {
    if (!state) {
        return;
    }
    state->last_vc_sync_time = dsd_decode_time();
    state->last_vc_sync_time_m = dsd_decode_now_mono_s();
}
