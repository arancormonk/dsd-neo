// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#ifndef DSD_NEO_SRC_PROTOCOL_P25_PHASE1_P25P1_TERMINATOR_END_H_
#define DSD_NEO_SRC_PROTOCOL_P25_PHASE1_P25P1_TERMINATOR_END_H_

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>
#include "dsd-neo/core/state_fwd.h"

/* How far a terminator may stand from where the one before it ended and still be part of the same end
 * (dsd_state::p25_p1_terminator_end_next_clock): the symbol grid's slips move a sync word by a symbol or two, while a
 * voice data unit between them, or a unit the decoder lost, moves it by far more (72 dibits at the least, a TDU). */
#define P25P1_TERMINATOR_SLIP_DIBITS 12U

/* Where a terminator stands: the symbol clock its sync and NID were read at, and the stream (clock epoch, carrier) it
 * was read on, all captured together before its tail is read. */
typedef struct {
    uint64_t clock;
    uint32_t clock_epoch;
    uint32_t carrier_seq;
} p25p1_terminator_place;

static inline p25p1_terminator_place
p25p1_terminator_place_now(const dsd_state* state) {
    const p25p1_terminator_place place = {state->symbol_clock, state->symbol_clock_epoch, state->carrier_seq};
    return place;
}

/* Whether the terminator at @p place follows the terminator that last ended a call back to back, on the stream that one
 * was read on. */
static inline int
p25p1_terminator_follows_the_last_end_at(const dsd_state* state, const p25p1_terminator_place* place) {
    if (state->p25_p1_terminator_end_epoch == 0U || state->p25_p1_terminator_end_clock_epoch != place->clock_epoch
        || state->p25_p1_terminator_end_carrier_seq != place->carrier_seq) {
        return 0;
    }
    const uint64_t want = state->p25_p1_terminator_end_next_clock;
    return place->clock + P25P1_TERMINATOR_SLIP_DIBITS >= want && place->clock <= want + P25P1_TERMINATOR_SLIP_DIBITS;
}

/* Whether the terminator read at @p clock, on the current stream, follows the terminator that last ended a call back to
 * back. */
static inline int
p25p1_terminator_follows_the_last_end(const dsd_state* state, uint64_t clock) {
    p25p1_terminator_place place = p25p1_terminator_place_now(state);
    place.clock = clock;
    return p25p1_terminator_follows_the_last_end_at(state, &place);
}

/* The epoch of the P25 Phase 1 call a terminator (a TDU or TDULC, @p unit_dibits long) at @p place speaks for once its
 * end has run (p25_sm_emit_tdu()), 0 for none: the call it ended, @p open_epoch, which was open before it, or, when it
 * ended none, the call the terminator it follows back to back ended (a TDULC after the TDU that ended the call, or a
 * repeat). Records the end for the next terminator, which @p place places, on the stream @p place was read on: a
 * terminator whose stream changed while its tail was read (a carrier boundary or a clock epoch change: a replay retune)
 * leaves no end a terminator on the next stream could repeat. */
static inline uint64_t
p25p1_terminator_note_end_at(dsd_state* state, uint64_t open_epoch, const p25p1_terminator_place* place,
                             uint32_t unit_dibits) {
    dsd_call_snapshot call;
    const int ended = dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED
                      && DSD_SYNC_IS_P25P1(call.protocol);
    const uint64_t speaks_for = open_epoch != 0U ? open_epoch
                                : p25p1_terminator_follows_the_last_end_at(state, place)
                                    ? state->p25_p1_terminator_end_epoch
                                    : 0U;
    const uint64_t epoch = ended && call.epoch == speaks_for ? speaks_for : 0U;
    state->p25_p1_terminator_end_epoch = epoch;
    state->p25_p1_terminator_end_next_clock = place->clock + (uint64_t)unit_dibits;
    state->p25_p1_terminator_end_clock_epoch = place->clock_epoch;
    state->p25_p1_terminator_end_carrier_seq = place->carrier_seq;
    return epoch;
}

/* p25p1_terminator_note_end_at() for a terminator read at @p clock on the current stream, nothing read since. */
static inline uint64_t
p25p1_terminator_note_end(dsd_state* state, uint64_t open_epoch, uint64_t clock, uint32_t unit_dibits) {
    p25p1_terminator_place place = p25p1_terminator_place_now(state);
    place.clock = clock;
    return p25p1_terminator_note_end_at(state, open_epoch, &place, unit_dibits);
}

#endif
