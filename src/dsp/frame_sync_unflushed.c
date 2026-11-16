// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The unflushed-retune count tunes off the decoder thread post (issue #834): a rigctl peer's confirmed retune
 * that no stream flush follows. The decoder thread takes it at its next input read
 * (frame_sync_take_unflushed_retunes() in frame_sync_unflushed.h) or at its next reconcile of that tune's notice
 * (dsd_frame_sync_apply_off_thread_tunes()), whichever comes first. Kept apart from the P25p1 comparison so the symbol
 * reader links it alone.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <stdint.h>
#include "frame_sync_unflushed.h"

/* Posted from any thread; the count taken, by the decoder thread only. */
dsd_atomic_u64 dsd_frame_sync_unflushed_posted;
uint64_t dsd_frame_sync_unflushed_seen = 0U;

void
dsd_frame_sync_note_off_thread_unflushed_retune(void) {
    (void)dsd_atomic_u64_fetch_add_release(&dsd_frame_sync_unflushed_posted, 1U);
}

void
frame_sync_take_unflushed_retunes_now(const dsd_opts* opts, dsd_state* state, uint64_t posted) {
    dsd_frame_sync_unflushed_seen = posted;
    state->symbol_clock_epoch++;
    /* And the input's queued samples go before the next read (dsd_symbol_apply_input_drop()). */
    state->input_drop_pending = 1U;
    state->input_drop_generation = opts ? opts->pcm_input_generation : 0U;
}

void
frame_sync_unflushed_retunes_reset(void) {
    dsd_frame_sync_unflushed_seen = dsd_atomic_u64_load_acquire(&dsd_frame_sync_unflushed_posted);
}

void
dsd_frame_sync_take_off_thread_unflushed_retunes(const dsd_opts* opts, dsd_state* state) {
    if (state) {
        frame_sync_take_unflushed_retunes(opts, state);
    }
}
