// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * The frequency a scan last confirmed it landed on (issue #667 D9), for an analog call on audio input, where
 * dsd_opts_tuned_freq_hz() is 0: a rigctl peer tuned by the -Y scan or the trunk scan demodulates what this client
 * hears, and only a tune that succeeded says where it is.
 *
 * The -Y scan notes a row's frequency once the row committed after a tune that landed (a typed row's commit, an untyped
 * step's, and a manual Next or Previous on an untyped list whose tune the front end confirmed, which installs its row
 * as the step does: dsd_engine_channel_scan_walk_row()), and the trunk scan a conventional target's once its retune
 * completed. The record is forgotten before every row or target tune (a manual untyped step's included), at every
 * carrier boundary that moves the receiver (a manual tune among them; a step to a row of the list is the scan's
 * own, which notes its row again after it) and at the -Y scan's release, which a pause, a stop and a trunk-scan stop
 * run, so the operator's own tune (`=`) leaves none; a placeholder row, a failed, pending or unconfirmed tune and a
 * rollback note none. It reads as none once its scan no longer owns the tuner, and a -Y landing once the scan
 * session or the channel map changed (a map replacement).
 *
 * Analog rows only: the tracker takes it at a reception's first block when the input names no frequency, and never for
 * a digital row (the frequency rule in dsd_opts_tuned_freq_hz() stays for those).
 */
#ifndef DSD_NEO_ENGINE_SCAN_LANDING_H
#define DSD_NEO_ENGINE_SCAN_LANDING_H
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/tuner_owner.h>
#include <stddef.h>
#include <stdint.h>
#include "scan_owner.h"

enum { DSD_ENGINE_SCAN_LANDING_NONE = 0, DSD_ENGINE_SCAN_LANDING_ROW = 1, DSD_ENGINE_SCAN_LANDING_TARGET = 2 };

/** No landing is in force. A state with no scan owner has none to forget, and gets no owner made for it. */
static inline void
dsd_engine_scan_landing_forget(dsd_state* state) {
    if (!state || !dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN)) {
        return;
    }
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_ensure(state);
    if (owner) {
        owner->landing = (dsd_engine_scan_landing){0};
    }
}

static inline void
dsd_engine_scan_landing_note(dsd_state* state, uint8_t by, long freq_hz) {
    if (!state || freq_hz <= 0 || (unsigned long)freq_hz > UINT32_MAX) {
        dsd_engine_scan_landing_forget(state);
        return;
    }
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_ensure(state);
    if (owner) {
        owner->landing =
            (dsd_engine_scan_landing){(uint32_t)freq_hz, by, state->lcn_scan_session, state->trunk_chan_map_seq};
    }
}

/** The -Y scan committed the row at @p freq_hz after a tune that landed. */
static inline void
dsd_engine_scan_landing_note_row(dsd_state* state, long freq_hz) {
    dsd_engine_scan_landing_note(state, (uint8_t)DSD_ENGINE_SCAN_LANDING_ROW, freq_hz);
}

/** The trunk scan's retune to the conventional target at @p freq_hz completed. */
static inline void
dsd_engine_scan_landing_note_target(dsd_state* state, long freq_hz) {
    dsd_engine_scan_landing_note(state, (uint8_t)DSD_ENGINE_SCAN_LANDING_TARGET, freq_hz);
}

/** The confirmed landing still in force, in Hz, or 0 for none. */
static inline uint32_t
dsd_engine_scan_landing_hz(const dsd_opts* opts, const dsd_state* state) {
    const dsd_engine_scan_owner* owner =
        state ? (const dsd_engine_scan_owner*)dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN) : NULL;
    if (!opts || !owner) {
        return 0U;
    }
    const dsd_engine_scan_landing* landing = &owner->landing;
    switch (landing->by) {
        case DSD_ENGINE_SCAN_LANDING_ROW:
            return dsd_channel_scan_owns_tuner(opts, state) && opts->trunk_scan_enabled != 1
                           && landing->session == state->lcn_scan_session
                           && landing->map_seq == state->trunk_chan_map_seq
                       ? landing->hz
                       : 0U;
        case DSD_ENGINE_SCAN_LANDING_TARGET: return dsd_trunk_scan_owns_tuner(opts, state) ? landing->hz : 0U;
        default: return 0U;
    }
}
#endif
