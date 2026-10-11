// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Initialization helpers for core option/state structures.
 *
 * Declares default-initialization helpers implemented in core.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_INIT_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_INIT_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initialize decoder options to defaults. */
void initOpts(dsd_opts* opts);
/** @brief Initialize decoder runtime state to defaults. */
void initState(dsd_state* state);
/** @brief Free dynamic allocations owned by @p state (does not free @p state itself). */
void freeState(dsd_state* state);
/**
 * @brief Restamp, from the decode clock's now, the decode-domain times initOpts() and initState() seed with "now".
 *
 * The one list of those stamps: `opts->symbol_out_file_creation_time`, and `state->last_cc_sync_time`,
 * `last_vc_sync_time` and `last_t3_tune_time`. initOpts() and initState() seed theirs through it, and the engine calls
 * it with both when it moves the decode clock onto an I/Q replay's capture clock and back
 * (dsd_engine_decode_clock_enter_replay()). A capture recorded before the session started then finds no init stamp
 * ahead of its now, so no elapsed time measured from one goes negative (issue #572). It also pulls the per-call WAV
 * open stamps (`opts->wav_out_open_time`, `wav_out_open_timeR`) back to now when they are ahead of it, so a recording
 * opened before the clock moved is never dated after the calls it records. Either argument may be NULL.
 */
void dsd_state_rebase_decode_timestamps(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_INIT_H_ */
