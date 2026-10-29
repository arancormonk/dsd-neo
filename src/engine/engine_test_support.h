// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_ENGINE_ENGINE_TEST_SUPPORT_H_
#define DSD_NEO_SRC_ENGINE_ENGINE_TEST_SUPPORT_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

/* The live loop's synced-frames pass, run on the sync in state->synctype until it drops, as live_scanner_main_loop()
   runs it after a hunt found sync: for tests that stand in for the hunt (getFrameSync()) and the frame decoder
   (processFrame()). */
void dsd_engine_test_process_synced_frames(dsd_opts* opts, dsd_state* state);

#endif
