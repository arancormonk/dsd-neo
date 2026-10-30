// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Who pauses a -Y scan the engine pauses again on its own (issue #671).
 *
 * A resume whose row never lands leaves the scan paused: the engine runs the pause again over what the attempt began.
 * That pause asks the front end back onto the configured receive profile, as an interactive pause does, and the
 * decoder has to learn what became of that request (issue #578), which app-control records. App-control installs its
 * recording pause (svc_pause_channel_scan()) for its session; with none installed (headless runs, tests) the engine
 * pauses on its own, unrecorded.
 */

#ifndef DSD_NEO_RUNTIME_CHANNEL_SCAN_HOOKS_H
#define DSD_NEO_RUNTIME_CHANNEL_SCAN_HOOKS_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A -Y pause, as dsd_engine_channel_scan_pause() takes and returns it. */
typedef int (*dsd_channel_scan_pause_fn)(dsd_opts* opts, dsd_state* state, int in_place);

/** Install the pause the engine's own re-pause goes through (NULL: none), with the frontend session. Thread-safe. */
void dsd_channel_scan_pause_hook_set(dsd_channel_scan_pause_fn fn);

/** The pause installed, or NULL. Thread-safe; the engine reads it on the decoder thread. */
dsd_channel_scan_pause_fn dsd_channel_scan_pause_hook_get(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_RUNTIME_CHANNEL_SCAN_HOOKS_H */
