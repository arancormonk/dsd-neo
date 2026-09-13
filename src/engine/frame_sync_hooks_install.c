// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>

#include <dsd-neo/protocol/edacs/edacs.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "engine_hooks_install.h"

static int
scan_visit_should_yield_from_frame(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return 0;
    }
    if (opts->trunk_scan_enabled == 1) {
        return dsd_engine_trunk_scan_visit_expired(opts, state);
    }
    const double now_m = dsd_time_now_monotonic_s();
    dsd_engine_scan_visit_tick(opts, state, now_m);
    dsd_engine_scan_y_timing_tick(opts, state, now_m, dsd_time_now_realtime_s());
    return dsd_engine_scan_visit_expired(opts, state, now_m);
}

static void
p25_sm_release_from_frame_sync(dsd_opts* opts, dsd_state* state) {
    if (!dsd_trunk_p25_recovery_allowed(opts, state)) {
        return;
    }
    state->p25_sm_force_release = 1;
    p25_sm_release(p25_sm_get_ctx(), opts, state, "frame-sync-no-sync");
}

static void
p25_sm_vc_sync_from_frame_sync(dsd_opts* opts, const dsd_state* state) {
    if (!dsd_trunk_p25_recovery_allowed(opts, state)) {
        return;
    }
    p25_sm_note_vc_frame_sync(p25_sm_get_ctx(), opts, state);
}

static void
p25_sm_vc_no_sync_from_frame_sync(dsd_opts* opts, const dsd_state* state) {
    if (!dsd_trunk_p25_recovery_allowed(opts, state)) {
        return;
    }
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_note_cc_no_sync_pass(ctx, opts, state);
    p25_sm_note_vc_no_sync_pass(ctx, opts, state);
}

void
dsd_engine_frame_sync_hooks_install(void) {
    dsd_frame_sync_hooks hooks = {0};
    hooks.p25_sm_try_tick = p25_sm_try_tick;
    hooks.p25_sm_release = p25_sm_release_from_frame_sync;
    hooks.p25_sm_vc_sync = p25_sm_vc_sync_from_frame_sync;
    hooks.p25_sm_vc_no_sync = p25_sm_vc_no_sync_from_frame_sync;
    hooks.eot_cc = eot_cc;
    hooks.no_carrier = noCarrier;
    hooks.scan_visit_should_yield = scan_visit_should_yield_from_frame;
    dsd_frame_sync_hooks_set(hooks);
}
