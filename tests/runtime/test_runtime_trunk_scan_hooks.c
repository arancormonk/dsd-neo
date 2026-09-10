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
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stddef.h>
#include <stdint.h>
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

static int g_activity_calls;

static void
fake_p25_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target, uint32_t source, int is_private,
                  int encrypted, int data_call) {
    assert(opts == g_last_opts);
    assert(state == g_last_state);
    assert(target == 1001U && source == 2002U);
    assert(is_private == 1 && encrypted == 1 && data_call == 0);
    g_activity_calls++;
}

static void
test_recovery_ownership(void) {
    static dsd_opts opts;
    static dsd_state state;
    opts.trunk_enable = 1;
    opts.frame_dmr = opts.frame_p25p1 = opts.frame_p25p2 = 1;
    state.p25_cc_freq = state.trunk_cc_freq = 451000000L;
    state.synctype = state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    assert(!dsd_trunk_dmr_recovery_allowed(&opts, &state)); // Raw sync cannot claim ownership.
    dsd_trunk_recovery_note_protocol(&state, DSD_TRUNK_RECOVERY_DMR);
    assert(dsd_trunk_dmr_recovery_allowed(&opts, &state));
    assert(!dsd_trunk_p25_recovery_allowed(&opts, &state));
    state.synctype = state.lastsynctype = DSD_SYNC_NONE;
    state.p25_cc_is_tdma = 2;
    assert(dsd_trunk_dmr_recovery_allowed(&opts, &state));
    assert(!dsd_trunk_p25_recovery_allowed(&opts, &state));
    dsd_trunk_recovery_note_protocol(&state, DSD_TRUNK_RECOVERY_P25);
    state.synctype = state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    assert(dsd_trunk_p25_recovery_allowed(&opts, &state)); // Stray sync cannot evict P25.
    assert(!dsd_trunk_dmr_recovery_allowed(&opts, &state));
    state.synctype = state.lastsynctype = DSD_SYNC_NONE;
    assert(dsd_trunk_p25_recovery_allowed(&opts, &state));
    opts.trunk_scan_enabled = 1;
    assert(!dsd_trunk_p25_recovery_allowed(&opts, &state)); // No installed owner during startup/shutdown.
    opts.trunk_scan_enabled = 0;
    opts.trunk_enable = 0;
    assert(!dsd_trunk_p25_recovery_allowed(&opts, &state));
    assert(!dsd_trunk_dmr_recovery_allowed(&opts, &state));
    assert(!dsd_trunk_p25_recovery_allowed(NULL, &state));
    assert(!dsd_trunk_dmr_recovery_allowed(&opts, NULL));
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
    dsd_trunk_scan_hook_p25_conventional_activity(&opts, &state, 1001U, 2002U, 1, 1, 0);
    assert(g_activity_calls == 0);

    /* Installed: every op reaches the implementation with its arguments and result intact. */
    dsd_trunk_scan_hooks hooks = {0};
    hooks.control = fake_control;
    hooks.p25_conventional_activity = fake_p25_activity;
    dsd_trunk_scan_hooks_set(hooks);

    g_control_result = 1;
    assert(dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE) == 1);
    assert(g_control_calls == 1);
    assert(g_last_op == DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE);
    assert(g_last_opts == &opts);
    assert(g_last_state == &state);
    dsd_trunk_scan_hook_p25_conventional_activity(&opts, &state, 1001U, 2002U, 1, 1, 0);
    assert(g_activity_calls == 1);

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
    dsd_trunk_scan_hook_p25_conventional_activity(&opts, &state, 1001U, 2002U, 1, 1, 0);
    assert(g_activity_calls == 1);
    test_recovery_ownership();
    return 0;
}
