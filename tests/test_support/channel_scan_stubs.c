// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <stddef.h>

/* Action-only tests have no tuner; exercise the production scoped-settings teardown. */
void
dsd_engine_channel_scan_leave(dsd_opts* opts, dsd_state* state) {
    (void)dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_CHANNEL_SCAN, NULL, NULL);
    dsd_scan_mode_leave(opts, state);
}
