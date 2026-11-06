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
 * record is forgotten before every row or target tune (a manual untyped step's included; a -Y go-to's only once its
 * tune is taken or a leg moved, below), at every carrier boundary
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
 * The -Y scan's go-to (issue #644) is the one row tune that keeps the landing when it is refused or deferred before
 * any backend moved: the receiver is still on the row landed, so that row is still confirmed on air. It forgets the
 * landing once its tune is taken or a leg moved.
 *
 * The entry intent (issue #644): a frequency typed by hand that is a -Y row sends the scan to that row, and the scan
 * holds it there, marked an entry hold, only once that row lands from the intent's own tune in that scan session and
 * on that list (dsd_engine_scan_entry_landed(), at every -Y landing). The tune is named by its request: a typed row's
 * first request, which its restages keep, or an untyped row's noted manual tune; a landing within the go-to's own
 * call holds the row directly (dsd_engine_scan_entry_hold_now()). Every landing spends the intent. Any other tune of
 * the scan cancels it before it starts (a step, Next or Previous, an avoid's step, a plain resume), and so do a pause,
 * a stop, `Y` and the failure of its own tune. A new site that starts a -Y row tune must cancel the intent before it
 * tunes (dsd_engine_scan_entry_intent_cancel()); the request check is the second guard.
 */
#ifndef DSD_NEO_ENGINE_SCAN_LANDING_H
#define DSD_NEO_ENGINE_SCAN_LANDING_H
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/scan_entry.h>
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

/** An entry now waits on -Y row @p row's landing in the current scan session and on the current list, from the tune
 * @p request names (0: the tune about to begin, which dsd_engine_scan_entry_intent_bind() names). */
static inline void
dsd_engine_scan_entry_intent_set(dsd_state* state, int row, uint64_t request) {
    if (!state) {
        return;
    }
    const dsd_engine_scan_entry_intent intent = {.active = 1U,
                                                 .by = (uint8_t)DSD_ENGINE_SCAN_LANDING_ROW,
                                                 .row = row,
                                                 .session = state->lcn_scan_session,
                                                 .list_rev = state->lcn_list_rev,
                                                 .request = request};
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
    return intent.active && intent.by == DSD_ENGINE_SCAN_LANDING_ROW && intent.row == row
           && intent.session == state->lcn_scan_session && intent.list_rev == state->lcn_list_rev;
}

/** The tune @p request of -Y row @p row began: an entry waiting on that row whose tune was not named yet is that
 * tune's. */
static inline void
dsd_engine_scan_entry_intent_bind(dsd_state* state, int row, uint64_t request) {
    dsd_engine_scan_entry_intent intent = dsd_engine_scan_entry_intent_get(state);
    if (state && request != 0U && intent.request == 0U && dsd_engine_scan_entry_intent_is(state, row)) {
        intent.request = request;
        dsd_engine_scan_entry_intent_put(state, &intent);
    }
}

/** The tune @p request ended without landing: an entry waiting on it is cancelled. */
static inline void
dsd_engine_scan_entry_intent_drop_request(dsd_state* state, uint64_t request) {
    const dsd_engine_scan_entry_intent intent = dsd_engine_scan_entry_intent_get(state);
    if (state && intent.active && request != 0U && intent.request == request) {
        dsd_engine_scan_entry_intent_cancel(state);
    }
}

/** The entry @p intent failed, coming to @p result (DSD_SCAN_GOTO_REFUSED, _DEFERRED or _SETUP_FAILED): kept, with
 * the name of what it went to, for app-control to name once (dsd_engine_scan_entry_take_failure()), whether or not a
 * toast began it. @p label names a trunk-scan target (its label, else its id); a -Y row is named here, as the match
 * names it (its label, else "row N"). A newer failure replaces one not taken. */
static inline void
dsd_engine_scan_entry_note_failure(dsd_state* state, const dsd_engine_scan_entry_intent* intent,
                                   dsd_scan_goto_result result, const char* label) {
    dsd_engine_scan_owner* owner = state && intent ? dsd_engine_scan_owner_ensure(state) : NULL;
    if (!owner) {
        return;
    }
    dsd_engine_scan_entry_failure failed = {.set = 1U, .by = intent->by, .result = (uint8_t)result, .row = intent->row};
    const char* row_name = intent->by == DSD_ENGINE_SCAN_LANDING_ROW && intent->row >= 0
                               ? dsd_state_trunk_lcn_name_get(state, (size_t)intent->row)
                               : NULL;
    if (label && label[0] != '\0') {
        DSD_SNPRINTF(failed.label, sizeof failed.label, "%s", label);
    } else if (row_name && row_name[0] != '\0') {
        DSD_SNPRINTF(failed.label, sizeof failed.label, "%s", row_name);
    } else if (intent->by == DSD_ENGINE_SCAN_LANDING_ROW) {
        DSD_SNPRINTF(failed.label, sizeof failed.label, "row %d", intent->row + 1);
    }
    owner->entry_failed = failed;
}

/** The tune @p request failed after the go-to that began it returned (@p result and @p label as above, @p label NULL
 * for a -Y row): an entry waiting on it is cancelled, and its failure kept. A tune dropped because something else took
 * the tuner is no failure: that one cancels with dsd_engine_scan_entry_intent_drop_request(). */
static inline void
dsd_engine_scan_entry_intent_fail_request(dsd_state* state, uint64_t request, dsd_scan_goto_result result,
                                          const char* label) {
    const dsd_engine_scan_entry_intent intent = dsd_engine_scan_entry_intent_get(state);
    if (state && intent.active && request != 0U && intent.request == request) {
        dsd_engine_scan_entry_note_failure(state, &intent, result, label);
        dsd_engine_scan_entry_intent_cancel(state);
    }
}

/** A -Y resume an entry rides failed (@p result as above): the entry waiting on it is cancelled, and its failure kept.
 */
static inline void
dsd_engine_scan_entry_intent_fail(dsd_state* state, dsd_scan_goto_result result) {
    const dsd_engine_scan_entry_intent intent = dsd_engine_scan_entry_intent_get(state);
    if (state && intent.active) {
        dsd_engine_scan_entry_note_failure(state, &intent, result, NULL);
        dsd_engine_scan_entry_intent_cancel(state);
    }
}

/** Take the entry failure kept, if any, into @p out (may be NULL); none is kept after. Returns 1 when there was one. */
static inline int
dsd_engine_scan_entry_failure_take(dsd_state* state, dsd_engine_scan_entry_failure* out) {
    const dsd_engine_scan_owner* kept =
        state ? (const dsd_engine_scan_owner*)dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN) : NULL;
    if (!kept || !kept->entry_failed.set) {
        return 0;
    }
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_ensure(state); /* the one just read */
    if (!owner) {
        return 0;
    }
    if (out) {
        *out = owner->entry_failed;
    }
    owner->entry_failed = (dsd_engine_scan_entry_failure){0};
    return 1;
}

/** The scan holds the row on air, marked an entry hold, and no entry waits on a landing any more: a go-to whose row was
 * on air, or landed within its own call. */
static inline void
dsd_engine_scan_entry_hold_now(dsd_state* state) {
    if (!state) {
        return;
    }
    dsd_engine_scan_entry_intent_cancel(state);
    state->lcn_scan_hold = 1U;
    state->lcn_scan_entry_hold = 1U;
}

/** A -Y go-to's tune is taken, or a backend moved: the entry hold it replaces goes, hold and mark, and only its own
 * row's confirmed landing sets them again, so a mark never stays on a row whose landing is not confirmed. An ordinary
 * hold carries over, as a step carries it. */
static inline void
dsd_engine_scan_entry_hold_replaced(dsd_state* state) {
    if (state && state->lcn_scan_entry_hold) {
        state->lcn_scan_hold = 0U;
        state->lcn_scan_entry_hold = 0U;
    }
}

/** -Y row @p row landed and committed from the tune @p request (0: one no entry waits on): an entry waiting on that
 * tune of that row holds the scan there (lcn_scan_hold), marked an entry hold (lcn_scan_entry_hold). The landing spends
 * the intent whichever row and tune it is. */
static inline void
dsd_engine_scan_entry_landed(dsd_state* state, int row, uint64_t request) {
    if (!state) {
        return;
    }
    const dsd_engine_scan_entry_intent intent = dsd_engine_scan_entry_intent_get(state);
    const int ours = dsd_engine_scan_entry_intent_is(state, row) && request != 0U && intent.request == request;
    dsd_engine_scan_entry_intent_cancel(state);
    if (ours) {
        state->lcn_scan_hold = 1U;
        state->lcn_scan_entry_hold = 1U;
    }
}

/*
 * The trunk scan's entry intent: the same record, waiting on a target's park in the coordinator's session rather than
 * on a -Y row. Its tune is the park's request; a P25 target whose park failed while its state machine's own
 * control-channel tune recovered it hands the intent to that tune when the coordinator adopts it
 * (dsd_engine_scan_entry_intent_rebind()), and a recovery found complete is the park's own landing. Every park cancels
 * the intent once past its preparation (trunk_scan_switch_to()), so only the go-to's own park, set after it, can land
 * it; the coordinator holds the target itself, as the hold is its own (dsd_trunk_scan_coord::hold_active).
 */

/** An entry now waits on the park @p request of trunk-scan target @p index in coordinator session @p session. */
static inline void
dsd_engine_scan_entry_intent_set_target(dsd_state* state, int index, uint32_t session, uint64_t request) {
    const dsd_engine_scan_entry_intent intent = {.active = 1U,
                                                 .by = (uint8_t)DSD_ENGINE_SCAN_LANDING_TARGET,
                                                 .row = index,
                                                 .session = session,
                                                 .request = request};
    dsd_engine_scan_entry_intent_put(state, &intent);
}

/** Whether an entry waits on a park of trunk-scan target @p index in coordinator session @p session. */
static inline int
dsd_engine_scan_entry_intent_is_target(const dsd_state* state, int index, uint32_t session) {
    const dsd_engine_scan_entry_intent intent = dsd_engine_scan_entry_intent_get(state);
    return intent.active && intent.by == DSD_ENGINE_SCAN_LANDING_TARGET && intent.row == index
           && intent.session == session;
}

/** The tune @p request an entry waits on was replaced by @p next (a P25 recovery tune the coordinator adopted): the
 * entry waits on that one now. */
static inline void
dsd_engine_scan_entry_intent_rebind(dsd_state* state, uint64_t request, uint64_t next) {
    dsd_engine_scan_entry_intent intent = dsd_engine_scan_entry_intent_get(state);
    if (state && intent.active && request != 0U && next != 0U && intent.request == request) {
        intent.request = next;
        dsd_engine_scan_entry_intent_put(state, &intent);
    }
}

/** The park @p request of trunk-scan target @p index in coordinator session @p session completed: whether an entry
 * waited on it, so the coordinator holds the target, marked an entry hold. The landing spends the intent either way. */
static inline int
dsd_engine_scan_entry_landed_target(dsd_state* state, int index, uint32_t session, uint64_t request) {
    const dsd_engine_scan_entry_intent intent = dsd_engine_scan_entry_intent_get(state);
    const int ours =
        dsd_engine_scan_entry_intent_is_target(state, index, session) && request != 0U && intent.request == request;
    dsd_engine_scan_entry_intent_cancel(state);
    return ours;
}

/** Whether the stream a restart reopened is where a tune dropped at its stop was going (issue #644): @p opts' radio
 * input, not an I/Q replay, running on the source @p source_key the stopped stream had (one noted: 0 is none), with
 * @p hz its centre. */
static inline int
dsd_engine_stream_reopened_on(const dsd_opts* opts, const dsd_state* state, uint64_t source_key, uint32_t hz) {
    return opts && state && hz != 0U && source_key != 0U && opts->audio_in_type == AUDIO_IN_RTL && state->rtl_ctx
           && !dsd_opts_audio_in_dev_is_iqreplay_spec(opts->audio_in_dev) && state->carrier_source_key == source_key
           && opts->rtlsdr_center_freq == hz;
}

#endif
