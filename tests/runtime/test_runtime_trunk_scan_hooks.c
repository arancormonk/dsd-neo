// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The trunk-scan control hook: app_control drives hold / avoid / clear / advance on the
 * engine-owned coordinator through this slot, so the thunk has to say "unavailable" when
 * nothing is installed and pass the op through untouched when something is.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stddef.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

/* The four ops and three failure codes are distinct, so a caller can switch on them. */
_Static_assert(DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE != DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE
                   && DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE != DSD_TRUNK_SCAN_CONTROL_AVOID_CLEAR
                   && DSD_TRUNK_SCAN_CONTROL_AVOID_CLEAR != DSD_TRUNK_SCAN_CONTROL_ADVANCE,
               "trunk scan control ops must be distinct");
_Static_assert(DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE < 0 && DSD_TRUNK_SCAN_CONTROL_BUSY < 0
                   && DSD_TRUNK_SCAN_CONTROL_REFUSED < 0,
               "trunk scan control failure codes must be negative");
_Static_assert(DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE != DSD_TRUNK_SCAN_CONTROL_BUSY
                   && DSD_TRUNK_SCAN_CONTROL_BUSY != DSD_TRUNK_SCAN_CONTROL_REFUSED,
               "trunk scan control failure codes must be distinct");

static int g_control_calls = 0;
static int g_last_op = -1;
static const dsd_opts* g_last_opts = NULL;
static const dsd_state* g_last_state = NULL;
static int g_control_result = 0;

static int
fake_control(dsd_opts* opts, dsd_state* state, int op) {
    g_control_calls++;
    g_last_op = op;
    g_last_opts = opts;
    g_last_state = state;
    return g_control_result;
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    /* Nothing installed (headless, tests, -Y): the control op reports unavailable. */
    dsd_trunk_scan_hooks none = {0};
    dsd_trunk_scan_hooks_set(none);
    assert(dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE)
           == DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE);
    assert(g_control_calls == 0);

    /* Installed: every op reaches the implementation with its arguments and result intact. */
    dsd_trunk_scan_hooks hooks = {0};
    hooks.control = fake_control;
    dsd_trunk_scan_hooks_set(hooks);

    g_control_result = 1;
    assert(dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE) == 1);
    assert(g_control_calls == 1);
    assert(g_last_op == DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE);
    assert(g_last_opts == &opts);
    assert(g_last_state == &state);

    g_control_result = DSD_TRUNK_SCAN_CONTROL_REFUSED;
    assert(dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE)
           == DSD_TRUNK_SCAN_CONTROL_REFUSED);
    assert(g_last_op == DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE);

    g_control_result = 2;
    assert(dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_AVOID_CLEAR) == 2);
    assert(g_last_op == DSD_TRUNK_SCAN_CONTROL_AVOID_CLEAR);

    g_control_result = DSD_TRUNK_SCAN_CONTROL_BUSY;
    assert(dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == DSD_TRUNK_SCAN_CONTROL_BUSY);
    assert(g_last_op == DSD_TRUNK_SCAN_CONTROL_ADVANCE);
    assert(g_control_calls == 4);

    /* Uninstalling restores the unavailable answer. */
    dsd_trunk_scan_hooks_set(none);
    assert(dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_ADVANCE)
           == DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE);
    assert(g_control_calls == 4);
    return 0;
}
