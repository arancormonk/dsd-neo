// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * CRC evidence for the TETRA carrier on air and the burst being decoded (tetra_confirm.h).
 */

#include "tetra_confirm.h"

#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <stddef.h>
#include <stdint.h>

#include "tetra_state.h"

/* The evidence a note adds to: the burst's always (its verdict is about what it decoded, wherever it was sent), the
 * carrier's only while the open burst still belongs to it -- the carrier left during a burst gets no new evidence, and
 * the carrier the receiver moved to gets none from a burst sent on the other. */
static tetra_confirm_t*
confirm_for_note(dsd_state* state, int* carrier_current) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return NULL;
    }
    *carrier_current = !(ts->confirm.burst_open && ts->confirm.burst_stale);
    return &ts->confirm;
}

void
tetra_confirm_reset(dsd_state* state) {
    /* A session that never decoded TETRA has nothing to forget, and gets no TETRA state for it. */
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    tetra_confirm_t* const c = &ts->confirm;
    c->signalling_blocks = 0;
    c->carrier_blocks = 0;
    c->carrier_epoch++;
    c->followed_call = 0;
    c->burst_stale = c->burst_open;
}

void
tetra_confirm_burst_begin(dsd_state* state) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return;
    }
    ts->confirm.burst_open = 1;
    ts->confirm.burst_stale = 0;
    ts->confirm.burst_proven = 0;
}

void
tetra_confirm_burst_begin_captured(dsd_state* state, uint32_t epoch) {
    tetra_confirm_burst_begin(state);
    tetra_state_t* const ts = tetra_st_existing(state);
    if (ts) {
        ts->confirm.burst_stale = (uint8_t)(ts->confirm.carrier_epoch != epoch);
    }
}

int
tetra_confirm_burst_end(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return 0;
    }
    ts->confirm.burst_open = 0;
    ts->confirm.burst_stale = 0;
    return ts->confirm.burst_proven != 0;
}

int
tetra_confirm_burst_current(const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    return ts && !(ts->confirm.burst_open && ts->confirm.burst_stale);
}

/* A CRC-16 signalling block of the carrier: evidence for publication, and with @p downlink of a control channel. */
static void
confirm_note_carrier_block(dsd_state* state, int downlink) {
    int carrier_current = 0;
    tetra_confirm_t* const c = confirm_for_note(state, &carrier_current);
    if (!c) {
        return;
    }
    c->burst_proven = 1;
    if (!carrier_current) {
        return;
    }
    if (c->carrier_blocks < UINT32_MAX) {
        c->carrier_blocks++;
    }
    if (downlink && c->signalling_blocks < UINT32_MAX) {
        c->signalling_blocks++;
    }
}

void
tetra_confirm_note_signalling(dsd_state* state) {
    confirm_note_carrier_block(state, 1);
}

void
tetra_confirm_note_uplink_signalling(dsd_state* state) {
    confirm_note_carrier_block(state, 0);
}

void
tetra_confirm_note_block(dsd_state* state) {
    int carrier_current = 0;
    tetra_confirm_t* const c = confirm_for_note(state, &carrier_current);
    if (c) {
        c->burst_proven = 1;
    }
}

int
tetra_confirm_carrier_proven(const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    return ts && ts->confirm.carrier_blocks != 0;
}

int
tetra_confirm_downlink_proven(const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    return ts && ts->confirm.signalling_blocks != 0;
}

void
tetra_confirm_note_followed_call(dsd_state* state) {
    int carrier_current = 0;
    tetra_confirm_t* const c = confirm_for_note(state, &carrier_current);
    if (c && carrier_current) {
        c->followed_call = 1;
    }
}

void
tetra_confirm_forget_followed_call(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (ts) {
        ts->confirm.followed_call = 0;
    }
}

int
tetra_confirm_followed_call_proven(const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    return ts && ts->confirm.followed_call != 0;
}

uint32_t
tetra_confirm_carrier_epoch(const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    return ts ? ts->confirm.carrier_epoch : 0U;
}

void
tetra_confirm_restore_followed_call(dsd_state* state, uint32_t epoch, int proven) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (ts && ts->confirm.carrier_epoch == epoch) {
        ts->confirm.followed_call = (uint8_t)(proven != 0);
    }
}
