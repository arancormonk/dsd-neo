// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * No-op dsd_event_sync_slot() for tests that compile call_state.c without the core event code. A test that links
 * dsd_events.c (through dsd-neo_core) must not link this too: both define the function, and only GNU ld and ld64 let
 * a weak definition give way, so it is a plain definition that the tests needing it link explicitly.
 */

#include <dsd-neo/core/events.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

void
dsd_event_sync_slot(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}
