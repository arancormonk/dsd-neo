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

/* Receive permission and the AACH run behind it go with the followed call's proof: they are about that call on this
 * carrier. */
static void
confirm_forget_rx(tetra_confirm_t* c) {
    c->rx_perm = 0;
    c->rx_run = 0;
    c->rx_adverse = 0;
    c->rx_chan_bad = 0;
    c->rx_run_tn = 0;
    c->rx_run_fn = 0;
    c->rx_qual_valid = 0;
    c->rx_run_start = 0;
    c->rx_qual_start = 0;
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
    confirm_forget_rx(c);
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
        confirm_forget_rx(&ts->confirm);
    }
}

void
tetra_confirm_set_followed_rx(dsd_state* state, int on) {
    int carrier_current = 0;
    tetra_confirm_t* const c = confirm_for_note(state, &carrier_current);
    if (!c || !carrier_current) {
        return;
    }
    if (!on) {
        confirm_forget_rx(c);
        return;
    }
    const tetra_state_t* const ts = tetra_st_peek(state);
    c->rx_perm = 1;
    c->rx_adverse = 0;
    /* T.212 runs from the authorisation; without the TDMA clock it runs from the next qualifying AACH. */
    c->rx_qual_valid = (uint8_t)(ts->tdma_valid != 0u);
    c->rx_qual_start = ts->tdma_slot_start;
}

int
tetra_confirm_followed_rx(const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    return ts && ts->confirm.rx_perm != 0;
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
tetra_confirm_restore_followed_call(dsd_state* state, uint32_t epoch, int proven, int rx_perm) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (ts && ts->confirm.carrier_epoch == epoch) {
        ts->confirm.followed_call = (uint8_t)(proven != 0);
        confirm_forget_rx(&ts->confirm);
        /* The run of AACHs starts over; the permission the call held there comes back, its T.212 from now. */
        ts->confirm.rx_perm = (uint8_t)(rx_perm != 0);
        ts->confirm.rx_qual_valid = (uint8_t)(rx_perm != 0 && ts->tdma_valid);
        ts->confirm.rx_qual_start = ts->tdma_slot_start;
    }
}
