// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/cleanup.h>
#include <dsd-neo/runtime/shutdown.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

void
cleanupAndExit(dsd_opts* opts, dsd_state* state) {
    dsd_request_shutdown(opts, state);
}
