// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The symbol grid's side of the sync anchor (symbol_anchor.c): what dsd_symbol.c records and applies.
 */

#ifndef DSD_NEO_SRC_DSP_SYMBOL_ANCHOR_INTERNAL_H
#define DSD_NEO_SRC_DSP_SYMBOL_ANCHOR_INTERNAL_H

#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsd_symbol_anchor dsd_symbol_anchor;

/** The state's anchor, allocated on first use; NULL without a state or memory (the grid then runs as before). */
dsd_symbol_anchor* dsd_symbol_anchor_get(dsd_state* state);

/** Forget every sample and centre: the stream the grid read them from has ended. No-op before first use. */
void dsd_symbol_anchor_reset(dsd_state* state);

/**
 * The move a named sync word asks for, measured now, at the matched filter's switch-on right behind it: the switch
 * discards the recording, and the seam takes the move as part of the samples it owes (dsd_symbol.c). Whole samples,
 * at most two either way; 0 when no sync word is pending or the recording cannot place it. Clears the pending word.
 */
int dsd_symbol_anchor_take_switch_offset(const dsd_state* state);

/** Whether the anchor still needs the grid: recording, or holding a move for it. Once not, the grid drops it from
    dsd_state::symbol_anchor_hot and touches nothing of it until the next dsd_symbol_arm_sync_anchor(). */
int dsd_symbol_anchor_hot(const dsd_symbol_anchor* anchor);

/** Whether the anchor is recording (armed for a sync word); only then do the two calls below apply. */
int dsd_symbol_anchor_recording(const dsd_symbol_anchor* anchor);

/** Whether the anchor is recording for a word the hunt reads (dsd_symbol_arm_hunt_sync_anchor()): the grid then takes
    no crossing slip, as inside a frame. A @p lastsynctype of another protocol than the one whose frame armed the hold
    (the hunt accepted its sync, or noCarrier forgot the last) ends the hold for the rest of the recording. */
int dsd_symbol_anchor_holds_grid(dsd_symbol_anchor* anchor, int lastsynctype);

/** One post-filter sample the grid consumed; @p is_centre marks the sample it took as the symbol's centre. */
void dsd_symbol_anchor_push_sample(dsd_symbol_anchor* anchor, float sample, int is_centre);

/** The symbol whose samples were just pushed is decided; measures a named sync word once it has look-ahead. */
void dsd_symbol_anchor_end_symbol(dsd_symbol_anchor* anchor, int samples_per_symbol);

/** The slip a measurement queued for the next symbol (+1 one sample more, -1 one fewer, 0 none), consumed. */
int dsd_symbol_anchor_take_slip(dsd_symbol_anchor* anchor);

/** Whether the slip queued now was measured on the matched-filter seam's raw history (a P25 Phase 1 word the hunt read
    with no recording armed, dsd_symbol_note_p25p1_sync_anchor()): a filter switch keeps it, as the seam keeps the
    content position, where it discards a slip measured on the anchor's own samples. */
int dsd_symbol_anchor_slip_from_raw(const dsd_symbol_anchor* anchor);

#ifdef DSD_NEO_TEST_HOOKS
/** Tests: queue @p slip as a measurement would, on an armed anchor. */
void dsd_symbol_anchor_test_queue_slip(dsd_state* state, int slip);
/** Tests: mark the grid placed, as a measured word does, so the next dsd_symbol_arm_hunt_sync_anchor() holds it. */
void dsd_symbol_anchor_test_place_grid(dsd_state* state);
/** Tests: whether the last named word was measured (the grid is where a sync word put it). */
int dsd_symbol_anchor_test_placed(const dsd_state* state);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_DSP_SYMBOL_ANCHOR_INTERNAL_H */
