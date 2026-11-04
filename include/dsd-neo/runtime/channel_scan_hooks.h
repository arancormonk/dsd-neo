// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief What app-control does for the engine around a channel list: the pause of a -Y scan the engine pauses again on
 * its own (issue #671), and the late settling of a Next or Previous walk off any scan (issue #644).
 *
 * A resume whose row never lands leaves the scan paused: the engine runs the pause again over what the attempt began.
 * That pause asks the front end back onto the configured receive profile, as an interactive pause does, and the
 * decoder has to learn what became of that request (issue #578), which app-control records. App-control installs its
 * recording pause (svc_pause_channel_scan()) for its session; with none installed (headless runs, tests) the engine
 * pauses on its own, unrecorded.
 *
 * A Next or Previous walk (`L`/`J`) over a list no -Y scan runs tunes through app-control, which ends the calls, hands
 * the trunking state machines over and names the row once its tune lands. When the RTL tune times out the engine's
 * manual-tune poll learns its outcome later and hands it back through the walk hook app-control installs for its
 * session; with none installed nothing settles it (a walk is a command, so it needs the session).
 */

#ifndef DSD_NEO_RUNTIME_CHANNEL_SCAN_HOOKS_H
#define DSD_NEO_RUNTIME_CHANNEL_SCAN_HOOKS_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A -Y pause, as dsd_engine_channel_scan_pause() takes and returns it. */
typedef int (*dsd_channel_scan_pause_fn)(dsd_opts* opts, dsd_state* state, int in_place);

/** Install the pause the engine's own re-pause goes through (NULL: none), with the frontend session. Thread-safe. */
void dsd_channel_scan_pause_hook_set(dsd_channel_scan_pause_fn fn);

/** The pause installed, or NULL. Thread-safe; the engine reads it on the decoder thread. */
dsd_channel_scan_pause_fn dsd_channel_scan_pause_hook_get(void);

/**
 * A walk off any scan whose timed-out tune settled late (DSD_MANUAL_TUNE_LCN_CYCLE without scan_owned): @p row, walked
 * to in direction @p dir (1 Next, -1 Previous) on the list @p list_rev (dsd_state::lcn_list_rev) at @p requested_hz,
 * @p landed 1 when the tune landed and 0 when it failed. A drop is not passed on: it changes nothing. Called on the
 * decoder thread, outside the P25 SM tick guard.
 */
typedef void (*dsd_channel_walk_settle_fn)(dsd_opts* opts, dsd_state* state, int row, int dir, uint32_t requested_hz,
                                           uint64_t list_rev, int landed);

/** Install the walk's late settling (NULL: none), with the frontend session. Thread-safe. */
void dsd_channel_walk_settle_hook_set(dsd_channel_walk_settle_fn fn);

/** The walk's late settling installed, or NULL. Thread-safe; the engine reads it on the decoder thread. */
dsd_channel_walk_settle_fn dsd_channel_walk_settle_hook_get(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_RUNTIME_CHANNEL_SCAN_HOOKS_H */
