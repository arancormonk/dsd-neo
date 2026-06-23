// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(performance-no-int-to-ptr)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static int g_try_tick_calls;
static int g_release_calls;
static int g_eot_calls;
static int g_no_carrier_calls;
static dsd_opts* g_last_opts;
static dsd_state* g_last_state;

static void
reset_counters(void) {
    g_try_tick_calls = 0;
    g_release_calls = 0;
    g_eot_calls = 0;
    g_no_carrier_calls = 0;
    g_last_opts = NULL;
    g_last_state = NULL;
}

static void
record_args(dsd_opts* opts, dsd_state* state) {
    g_last_opts = opts;
    g_last_state = state;
}

static void
hook_try_tick(dsd_opts* opts, dsd_state* state) {
    g_try_tick_calls++;
    record_args(opts, state);
}

static void
hook_release(dsd_opts* opts, dsd_state* state) {
    g_release_calls++;
    record_args(opts, state);
}

static void
hook_eot(dsd_opts* opts, dsd_state* state) {
    g_eot_calls++;
    record_args(opts, state);
}

static void
hook_no_carrier(dsd_opts* opts, dsd_state* state) {
    g_no_carrier_calls++;
    record_args(opts, state);
}

static void
test_empty_hooks_are_noops(dsd_opts* opts, dsd_state* state) {
    dsd_frame_sync_hooks_set((dsd_frame_sync_hooks){0});
    reset_counters();

    dsd_frame_sync_hook_p25_sm_try_tick(opts, state);
    dsd_frame_sync_hook_p25_sm_on_release(opts, state);
    dsd_frame_sync_hook_eot_cc(opts, state);
    dsd_frame_sync_hook_no_carrier(opts, state);

    assert(g_try_tick_calls == 0);
    assert(g_release_calls == 0);
    assert(g_eot_calls == 0);
    assert(g_no_carrier_calls == 0);
    assert(g_last_opts == NULL);
    assert(g_last_state == NULL);
}

static void
test_installed_hooks_forward_args(dsd_opts* opts, dsd_state* state) {
    dsd_frame_sync_hooks hooks = {
        .p25_sm_try_tick = hook_try_tick,
        .p25_sm_on_release = hook_release,
        .eot_cc = hook_eot,
        .no_carrier = hook_no_carrier,
    };
    dsd_frame_sync_hooks_set(hooks);
    reset_counters();

    dsd_frame_sync_hook_p25_sm_try_tick(opts, state);
    assert(g_try_tick_calls == 1);
    assert(g_last_opts == opts);
    assert(g_last_state == state);

    dsd_frame_sync_hook_p25_sm_on_release(opts, state);
    assert(g_release_calls == 1);
    assert(g_last_opts == opts);
    assert(g_last_state == state);

    dsd_frame_sync_hook_eot_cc(opts, state);
    assert(g_eot_calls == 1);
    assert(g_last_opts == opts);
    assert(g_last_state == state);

    dsd_frame_sync_hook_no_carrier(opts, state);
    assert(g_no_carrier_calls == 1);
    assert(g_last_opts == opts);
    assert(g_last_state == state);
}

static void
test_partial_reinstall_replaces_table(dsd_opts* opts, dsd_state* state) {
    dsd_frame_sync_hooks_set((dsd_frame_sync_hooks){
        .p25_sm_try_tick = hook_try_tick,
        .no_carrier = hook_no_carrier,
    });
    reset_counters();

    dsd_frame_sync_hook_p25_sm_try_tick(opts, state);
    dsd_frame_sync_hook_p25_sm_on_release(opts, state);
    dsd_frame_sync_hook_eot_cc(opts, state);
    dsd_frame_sync_hook_no_carrier(opts, state);

    assert(g_try_tick_calls == 1);
    assert(g_release_calls == 0);
    assert(g_eot_calls == 0);
    assert(g_no_carrier_calls == 1);
    assert(g_last_opts == opts);
    assert(g_last_state == state);
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)(uintptr_t)0x1234U;
    dsd_state* state = (dsd_state*)(uintptr_t)0x5678U;

    test_empty_hooks_are_noops(opts, state);
    test_installed_hooks_forward_args(opts, state);
    test_partial_reinstall_replaces_table(opts, state);

    dsd_frame_sync_hooks_set((dsd_frame_sync_hooks){0});
    return 0;
}

// NOLINTEND(performance-no-int-to-ptr)
