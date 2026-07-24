// SPDX-License-Identifier: GPL-3.0-or-later

#include <dsd-neo/core/events.h>
#include <dsd-neo/platform/platform.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

DSD_ATTR_WEAK void
dsd_event_sync_slot(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}
