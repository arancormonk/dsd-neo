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

/* Whether the terminator read at @p clock follows the terminator that last ended a call back to back, on the stream
 * that one was read on. */
static inline int
p25p1_terminator_follows_the_last_end(const dsd_state* state, uint64_t clock) {
    if (state->p25_p1_terminator_end_epoch == 0U
        || state->p25_p1_terminator_end_clock_epoch != state->symbol_clock_epoch
        || state->p25_p1_terminator_end_carrier_seq != state->carrier_seq) {
        return 0;
    }
    const uint64_t want = state->p25_p1_terminator_end_next_clock;
    return clock + P25P1_TERMINATOR_SLIP_DIBITS >= want && clock <= want + P25P1_TERMINATOR_SLIP_DIBITS;
}

/* The epoch of the P25 Phase 1 call a terminator (a TDU or TDULC, @p unit_dibits long) speaks for once its end has run
 * (p25_sm_emit_tdu()), 0 for none: the call it ended, @p open_epoch, which was open before it, or, when it ended none,
 * the call the terminator it follows back to back ended (a TDULC after the TDU that ended the call, or a repeat).
 * Records the end for the next terminator, which @p clock (where this one's sync and NID were read) places. */
static inline uint64_t
p25p1_terminator_note_end(dsd_state* state, uint64_t open_epoch, uint64_t clock, uint32_t unit_dibits) {
    dsd_call_snapshot call;
    const int ended = dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED
                      && DSD_SYNC_IS_P25P1(call.protocol);
    const uint64_t speaks_for = open_epoch != 0U ? open_epoch
                                : p25p1_terminator_follows_the_last_end(state, clock)
                                    ? state->p25_p1_terminator_end_epoch
                                    : 0U;
    const uint64_t epoch = ended && call.epoch == speaks_for ? speaks_for : 0U;
    state->p25_p1_terminator_end_epoch = epoch;
    state->p25_p1_terminator_end_next_clock = clock + (uint64_t)unit_dibits;
    state->p25_p1_terminator_end_clock_epoch = state->symbol_clock_epoch;
    state->p25_p1_terminator_end_carrier_seq = state->carrier_seq;
    return epoch;
}

#endif
