// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The recording ring (unit_activity_capture.h). It defines dsd_unit_activity_push(), dsd_alias_scope_current() and
 * dsd_source_label_lookup_scoped(), so a test linking dsd-neo_core must not link it too.
 */

#include "unit_activity_capture.h"

#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/unit_activity.h>

#include <stddef.h>
#include <stdint.h>

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

int
dsd_alias_scope_current(const dsd_state* state, dsd_alias_scope* out) {
    (void)state;
    if (out) {
        DSD_MEMSET(out, 0, sizeof *out);
    }
    return 0;
}

int
dsd_source_label_lookup_scoped(const dsd_state* state, const dsd_alias_scope* scope, uint32_t id, char* mode,
                               size_t mode_sz, char* name, size_t name_sz) {
    (void)state;
    (void)scope;
    (void)id;
    if (mode && mode_sz > 0U) {
        mode[0] = '\0';
    }
    if (name && name_sz > 0U) {
        name[0] = '\0';
    }
    return 0;
}
