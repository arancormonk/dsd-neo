// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The unflushed-retune count (issue #834): what a tune off the decoder thread posts for a rigctl peer's
 * confirmed retune that no stream flush follows (dsd_frame_sync_note_off_thread_unflushed_retune()): the input's mark,
 * taken before the tune was asked for, held for the decoder (dsd_input_backlog_post()) and a count, the decoder thread's cue to take it, and the
 * input-replacement side that retires a post the new input does not hold (dsd_frame_sync_forget_unflushed_retunes()).
 * The decoder thread's take, which moves the epoch and asks for the input drop, is in frame_sync_unflushed.c. Nothing
 * here reads the options or the decoder state.
 */

#include <dsd-neo/dsp/unflushed_retune.h>

#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/input_backlog.h>
#include <stdint.h>
#include "frame_sync_unflushed.h"

/* Posted from any thread; the count taken, by the decoder thread only. */
dsd_atomic_u64 dsd_frame_sync_unflushed_posted;
uint64_t dsd_frame_sync_unflushed_seen = 0U;

void
dsd_frame_sync_note_off_thread_unflushed_retune(const dsd_input_cut* cut) {
    /* The mark first: a decoder thread that sees the count finds it held. */
    dsd_input_backlog_post(cut);
    (void)dsd_atomic_u64_fetch_add_release(&dsd_frame_sync_unflushed_posted, 1U);
}

void
frame_sync_unflushed_retunes_reset(void) {
    dsd_frame_sync_unflushed_seen = dsd_atomic_u64_load_acquire(&dsd_frame_sync_unflushed_posted);
    dsd_input_backlog_retire();
}

void
dsd_frame_sync_forget_unflushed_retunes(void) {
    dsd_input_backlog_forget();
}
