// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/ysf/ysf.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

int
dsd_dispatch_matches_ysf(int synctype) {
    return DSD_SYNC_IS_YSF(synctype);
}

void
dsd_dispatch_handle_ysf(dsd_opts* opts, dsd_state* state) {
    processYSF(opts, state);
}
