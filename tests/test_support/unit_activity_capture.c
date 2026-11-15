// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The recording ring (unit_activity_capture.h). It defines dsd_unit_activity_push() and
 * dsd_unit_activity_freeze_name() (which names nothing), so a test linking dsd-neo_core must not link it too.
 */

#include "unit_activity_capture.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/unit_activity.h>

static dsd_unit_activity g_last;
static int g_count;

void
dsd_test_ua_capture_reset(void) {
    DSD_MEMSET(&g_last, 0, sizeof g_last);
    g_count = 0;
}

int
dsd_test_ua_capture_count(void) {
    return g_count;
}

const dsd_unit_activity*
dsd_test_ua_capture_last(void) {
    return &g_last;
}

int
dsd_unit_activity_push(dsd_state* state, const dsd_unit_activity* row) {
    if (!state || !row) {
        return -1;
    }
    g_last = *row;
    g_count++;
    return 1;
}

void
dsd_unit_activity_freeze_name(const dsd_state* state, dsd_unit_activity* row) {
    (void)state;
    (void)row;
}
