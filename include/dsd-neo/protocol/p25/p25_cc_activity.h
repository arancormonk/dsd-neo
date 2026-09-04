// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief P25 control-channel activity timestamp helpers.
 */
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_CC_ACTIVITY_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_CC_ACTIVITY_H

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/state.h>
#include <time.h>
#include "dsd-neo/core/state_fwd.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mark a decoded P25 control-channel message.
 *
 * This is stricter than generic frame sync/tune timestamps: call it only after
 * a validated P25 control-channel block or LCCH MAC has decoded.
 *
 * @param state Decoder state.
 */
static inline void
p25_sm_note_cc_activity(dsd_state* state) {
    if (!state) {
        return;
    }
    const time_t now = time(NULL);
    const double now_m = dsd_time_now_monotonic_s();
    state->last_cc_sync_time = now;
    state->last_cc_sync_time_m = now_m;
    state->p25_last_cc_msg_time = now;
    state->p25_last_cc_msg_time_m = now_m;
}

#ifdef __cplusplus
}
#endif

#endif // DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_CC_ACTIVITY_H
