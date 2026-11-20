// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* The link seams of the -Y call-hold replay host (y_call_hold_replay.c): its link wraps getFrameSync,
   dsd_engine_channel_scan_step and dsd_engine_scan_untyped_find_row (GNU ld --wrap), so the engine's calls of each reach
   the host's wrapper, which calls the real function through its other name (tests/test_support/link_seam.h). */
#ifndef DSD_NEO_TESTS_ENGINE_Y_CALL_HOLD_REPLAY_SEAMS_H
#define DSD_NEO_TESTS_ENGINE_Y_CALL_HOLD_REPLAY_SEAMS_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include "../test_support/link_seam.h"

int yhold_get_frame_sync(dsd_opts* opts, dsd_state* state) DSD_TEST_LINK_NAME(__wrap_getFrameSync);
int yhold_real_get_frame_sync(dsd_opts* opts, dsd_state* state) DSD_TEST_LINK_NAME(__real_getFrameSync);
int yhold_channel_scan_step(dsd_opts* opts, dsd_state* state) DSD_TEST_LINK_NAME(__wrap_dsd_engine_channel_scan_step);
int yhold_real_channel_scan_step(dsd_opts* opts, dsd_state* state)
    DSD_TEST_LINK_NAME(__real_dsd_engine_channel_scan_step);
int yhold_untyped_find_row(const dsd_state* state, int from_roll, int skip_placeholders)
    DSD_TEST_LINK_NAME(__wrap_dsd_engine_scan_untyped_find_row);
int yhold_real_untyped_find_row(const dsd_state* state, int from_roll, int skip_placeholders)
    DSD_TEST_LINK_NAME(__real_dsd_engine_scan_untyped_find_row);

#endif
