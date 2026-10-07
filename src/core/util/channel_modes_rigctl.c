// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Whether a passband asked of a rigctl peer is the session's or a scan row's; see
 * <dsd-neo/core/channel_mode.h>.
 */

#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/runtime/scan_mode.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

int
dsd_channel_modes_rigctl_request_is_session(const dsd_opts* opts, const dsd_state* state) {
    if (!dsd_scan_mode_rigctl_request_is_session(opts, state)) {
        return 0;
    }
    /* The scope cannot tell a typed list's first row, tuned before its scope is entered, from a request off any scan;
       the configuration can. */
    return (state && opts->scanner_mode == 1 && dsd_channel_modes_present(state)) ? 0 : 1;
}
