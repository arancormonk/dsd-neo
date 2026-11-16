// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The decoder thread's side of the unflushed-retune count (issue #834; the count is in unflushed_retune.c): a
 * rigctl peer's confirmed retune that no stream flush follows, posted off the decoder thread. The decoder thread takes
 * it at its next input read (frame_sync_take_unflushed_retunes() in frame_sync_unflushed.h) or at its next reconcile of
 * that tune's notice (dsd_frame_sync_apply_off_thread_tunes()), whichever comes first. Kept apart from the P25p1
 * comparison so the symbol reader links it alone.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/platform/input_backlog.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_unflushed.h"

void
frame_sync_take_unflushed_retunes_now(const dsd_opts* opts, dsd_state* state, uint64_t posted) {
    dsd_frame_sync_unflushed_seen = posted;
    /* A post an input replacement retired moves nothing (dsd_frame_sync_forget_unflushed_retunes()). */
    dsd_input_cut cut;
    if (!dsd_input_backlog_take(&cut)) {
        return;
    }
    state->symbol_clock_epoch++;
    /* And what the input held at the post goes before the next read (dsd_symbol_apply_input_drop()). */
    dsd_frame_sync_request_input_drop(opts, state, &cut);
}

void
dsd_frame_sync_request_input_drop(const dsd_opts* opts, dsd_state* state, const dsd_input_cut* cut) {
    state->input_drop_pending = 1U;
    state->input_drop_generation = opts ? opts->pcm_input_generation : 0U;
    state->input_drop_cut = *cut;
}

void
dsd_frame_sync_take_off_thread_unflushed_retunes(const dsd_opts* opts, dsd_state* state) {
    if (state) {
        frame_sync_take_unflushed_retunes(opts, state);
    }
}
