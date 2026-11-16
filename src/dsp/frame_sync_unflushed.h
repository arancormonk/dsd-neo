// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The unflushed-retune count (frame_sync_unflushed.c, issue #834), for the C readers that take it inline. */

#ifndef DSD_NEO_SRC_DSP_FRAME_SYNC_UNFLUSHED_H_
#define DSD_NEO_SRC_DSP_FRAME_SYNC_UNFLUSHED_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <stdint.h>

/* The unflushed-retune count off-thread tunes post (dsd_frame_sync_note_off_thread_unflushed_retune(), any thread)
   and the count the decoder thread has taken (decoder thread only). */
extern dsd_atomic_u64 dsd_frame_sync_unflushed_posted;
extern uint64_t dsd_frame_sync_unflushed_seen;

/** Take @p posted: move the symbol clock epoch and ask for the input drop. Decoder thread. */
void frame_sync_take_unflushed_retunes_now(const dsd_opts* opts, dsd_state* state, uint64_t posted);
/** A run boundary: every count posted so far is taken as seen (dsd_frame_sync_off_thread_tunes_reset()). */
void frame_sync_unflushed_retunes_reset(void);

/* The decoder thread's per-read check for a tune the P25 watchdog confirmed (issue #834): one relaxed load, so the
   epoch moves and the input's queued samples go at the next read after the post, not at the next reconcile. */
static inline void
frame_sync_take_unflushed_retunes(const dsd_opts* opts, dsd_state* state) {
    const uint64_t posted = dsd_atomic_u64_load_relaxed(&dsd_frame_sync_unflushed_posted);
    if (posted != dsd_frame_sync_unflushed_seen) {
        frame_sync_take_unflushed_retunes_now(opts, state, posted);
    }
}

#endif
