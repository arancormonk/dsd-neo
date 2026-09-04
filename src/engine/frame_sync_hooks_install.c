// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>

#include <dsd-neo/protocol/edacs/edacs.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "engine_hooks_install.h"

static void
p25_sm_release_from_frame_sync(dsd_opts* opts, dsd_state* state) {
    p25_sm_release(p25_sm_get_ctx(), opts, state, "frame-sync-no-sync");
}

static void
p25_sm_vc_sync_from_frame_sync(dsd_opts* opts, const dsd_state* state) {
    p25_sm_note_vc_frame_sync(p25_sm_get_ctx(), opts, state);
}

static void
p25_sm_vc_no_sync_from_frame_sync(dsd_opts* opts, const dsd_state* state) {
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
    dsd_frame_sync_hooks_set(hooks);
}
