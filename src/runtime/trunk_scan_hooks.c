// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static dsd_trunk_scan_hooks g_trunk_scan_hooks = {0};

void
dsd_trunk_scan_hooks_set(dsd_trunk_scan_hooks hooks) {
    g_trunk_scan_hooks = hooks;
}

void*
dsd_trunk_scan_hook_p25_ctx(void) {
    return g_trunk_scan_hooks.p25_ctx ? g_trunk_scan_hooks.p25_ctx() : 0;
}

void*
dsd_trunk_scan_hook_dmr_ctx(void) {
    return g_trunk_scan_hooks.dmr_ctx ? g_trunk_scan_hooks.dmr_ctx() : 0;
}

void
dsd_trunk_scan_hook_tick(dsd_opts* opts, dsd_state* state) {
    if (g_trunk_scan_hooks.tick) {
        g_trunk_scan_hooks.tick(opts, state);
    }
}

void
dsd_trunk_scan_hook_dmr_conventional_activity(const dsd_opts* opts, dsd_state* state, uint32_t target, uint32_t source,
                                              int is_private, int encrypted, int data_call) {
    if (g_trunk_scan_hooks.dmr_conventional_activity) {
        g_trunk_scan_hooks.dmr_conventional_activity(opts, state, target, source, is_private, encrypted, data_call);
    }
}
