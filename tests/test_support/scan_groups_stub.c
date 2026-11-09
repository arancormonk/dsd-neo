// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <stddef.h>

/* For tests that build the policy store without the scan-row module: no -Y row or trunk-scan target holds a list. */
int
dsd_scan_groups_held_store(const dsd_state* state, dsd_tg_policy_store** held) {
    (void)state;
    if (held) {
        *held = NULL;
    }
    return 0;
}
