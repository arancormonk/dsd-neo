// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * The row or target a scan last confirmed it landed on, and its frequency.
 *
 * The frequency (issue #667 D9) is for an analog call on audio input, where dsd_opts_tuned_freq_hz() is 0: a rigctl
 * peer tuned by the -Y scan or the trunk scan demodulates what this client hears, and only a tune that succeeded says
 * where it is. The row or target (issue #644) proves which of the scan's rows is on air: a frequency typed by hand that
 * is that row's needs no tune (dsd_engine_channel_scan_row_confirmed(), dsd_engine_trunk_scan_target_confirmed()).
 * Equal frequencies alone never prove it, nor does the row a scan last committed.
 *
 * The -Y scan notes a row once it committed after a tune that landed (a typed row's commit, an untyped step's, and a
 * manual Next or Previous on an untyped list whose tune the front end confirmed, which installs its row as the step
 * does: dsd_engine_channel_scan_walk_row()), and the trunk scan a target once its retune completed: a conventional
 * target with its frequency, a trunked one with none (its park is its control channel, never a call's frequency). The
 * record is forgotten before every row or target tune (a manual untyped step's included), at every carrier boundary
 * that moves the receiver (a manual tune among them; a step to a row of the list is the scan's own, which notes its row
 * again after it), at every radio stream start (a reopened stream starts wherever it is told, which no landing
 * confirmed), and at the -Y scan's release, which a pause, a stop and a trunk-scan pause or stop run, so the operator's
 * own tune (`=`) leaves none; a placeholder row, a failed, pending or unconfirmed tune and a rollback note none. Its
 * frequency reads as none once its scan no longer owns the tuner, and a -Y landing once the scan session or the channel
 * map changed (a map replacement).
 *
 * Analog rows only take the frequency: the tracker takes it at a reception's first block when the input names no
 * frequency, and never for a digital row (the frequency rule in dsd_opts_tuned_freq_hz() stays for those).
 *
 * The entry intent (issue #644): a frequency typed by hand that is a -Y row sends the scan to that row, and the scan
 * holds it there, marked an entry hold, only once that row lands in that scan session and on that list
 * (dsd_engine_scan_entry_landed(), at every -Y landing). Every landing spends the intent. Any other tune of the scan
 * cancels it before it starts (a step, Next or Previous, an avoid's step), and so do a pause, a stop, `Y` and the
 * failure of its own tune, so a tune that lands after the intent was cancelled or replaced holds nothing.
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

/** The -Y scan committed @p row, on @p freq_hz, after a tune that landed. A placeholder (0 Hz) row notes none. */
static inline void
dsd_engine_scan_landing_note_row(dsd_state* state, int row, long freq_hz) {
    if (!state || row < 0 || freq_hz <= 0 || (unsigned long)freq_hz > UINT32_MAX) {
        dsd_engine_scan_landing_forget(state);
        return;
    }
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_ensure(state);
    if (owner) {
        const dsd_engine_scan_landing landing = {.hz = (uint32_t)freq_hz,
                                                 .by = (uint8_t)DSD_ENGINE_SCAN_LANDING_ROW,
                                                 .session = state->lcn_scan_session,
                                                 .map_seq = state->trunk_chan_map_seq,
                                                 .list_rev = state->lcn_list_rev,
                                                 .index = row};
        owner->landing = landing;
    }
}

/** The trunk scan's retune to target @p index of the coordinator of session @p session completed: a conventional
 * target on its own frequency @p freq_hz, a trunked one (@p freq_hz 0) on its control channel. */
static inline void
dsd_engine_scan_landing_note_target(dsd_state* state, int index, uint32_t session, uint32_t freq_hz) {
    if (!state || index < 0) {
        dsd_engine_scan_landing_forget(state);
        return;
    }
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_ensure(state);
    if (owner) {
        const dsd_engine_scan_landing landing = {
            .hz = freq_hz, .by = (uint8_t)DSD_ENGINE_SCAN_LANDING_TARGET, .session = session, .index = index};
        owner->landing = landing;
    }
}

/* The landing record, or NULL with no scan owner. */
static inline const dsd_engine_scan_landing*
dsd_engine_scan_landing_get(const dsd_state* state) {
    const dsd_engine_scan_owner* owner =
        state ? (const dsd_engine_scan_owner*)dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN) : NULL;
    return owner ? &owner->landing : NULL;
}

/** Whether the landing in force is the -Y scan's on @p row: noted in this scan session, on this list, and the row
 * still holds the frequency landed (not rewritten in place). */
static inline int
dsd_engine_scan_landing_is_row(const dsd_state* state, int row) {
    const dsd_engine_scan_landing* landing = dsd_engine_scan_landing_get(state);
    return landing && landing->by == DSD_ENGINE_SCAN_LANDING_ROW && landing->index == row && row >= 0
           && row < state->lcn_freq_count && landing->session == state->lcn_scan_session
           && landing->list_rev == state->lcn_list_rev
           && *dsd_state_trunk_lcn_slot_const(state, row) == (long)landing->hz;
}

/** Whether the landing in force is the trunk scan's on target @p index of the coordinator of session @p session. */
static inline int
dsd_engine_scan_landing_is_target(const dsd_state* state, int index, uint32_t session) {
    const dsd_engine_scan_landing* landing = dsd_engine_scan_landing_get(state);
    return landing && landing->by == DSD_ENGINE_SCAN_LANDING_TARGET && landing->index == index
           && landing->session == session;
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

/* The entry intent in force; one with active 0 for none. */
static inline dsd_engine_scan_entry_intent
dsd_engine_scan_entry_intent_get(const dsd_state* state) {
    const dsd_engine_scan_owner* owner =
        state ? (const dsd_engine_scan_owner*)dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN) : NULL;
    if (owner) {
        return owner->entry;
    }
    const dsd_engine_scan_entry_intent none = {0};
    return none;
}

/** @p intent is the entry intent in force (a go-to refused before any backend moved puts back the one before it). A
 * state with no scan owner gets none made for no intent. */
static inline void
dsd_engine_scan_entry_intent_put(dsd_state* state, const dsd_engine_scan_entry_intent* intent) {
    if (!state || !intent || (!intent->active && !dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN))) {
        return;
    }
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_ensure(state);
    if (owner) {
        owner->entry = *intent;
    }
}

/** An entry now waits on -Y row @p row's landing in the current scan session and on the current list. */
static inline void
dsd_engine_scan_entry_intent_set(dsd_state* state, int row) {
    if (!state) {
        return;
    }
    const dsd_engine_scan_entry_intent intent = {
        .active = 1U, .row = row, .session = state->lcn_scan_session, .list_rev = state->lcn_list_rev};
    dsd_engine_scan_entry_intent_put(state, &intent);
}

/** No entry waits on a landing any more. */
static inline void
dsd_engine_scan_entry_intent_cancel(dsd_state* state) {
    const dsd_engine_scan_entry_intent none = {0};
    dsd_engine_scan_entry_intent_put(state, &none);
}

/** Whether an entry waits on -Y row @p row's landing in the current scan session and on the current list. */
static inline int
dsd_engine_scan_entry_intent_is(const dsd_state* state, int row) {
    const dsd_engine_scan_entry_intent intent = dsd_engine_scan_entry_intent_get(state);
    return intent.active && intent.row == row && intent.session == state->lcn_scan_session
           && intent.list_rev == state->lcn_list_rev;
}

/** The tune of -Y row @p row ended without landing: an entry waiting on that row is cancelled. */
static inline void
dsd_engine_scan_entry_intent_drop_row(dsd_state* state, int row) {
    if (state && dsd_engine_scan_entry_intent_is(state, row)) {
        dsd_engine_scan_entry_intent_cancel(state);
    }
}

/** -Y row @p row landed and committed: an entry waiting on it holds the scan there (lcn_scan_hold), marked an entry
 * hold (lcn_scan_entry_hold). The landing spends the intent whichever row it is. */
static inline void
dsd_engine_scan_entry_landed(dsd_state* state, int row) {
    if (!state) {
        return;
    }
    const int ours = dsd_engine_scan_entry_intent_is(state, row);
    dsd_engine_scan_entry_intent_cancel(state);
    if (ours) {
        state->lcn_scan_hold = 1U;
        state->lcn_scan_entry_hold = 1U;
    }
}
#endif
