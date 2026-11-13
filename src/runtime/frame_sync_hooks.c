// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/frame_sync_hooks.h>

#include <dsd-neo/core/opts.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static dsd_frame_sync_hooks g_frame_sync_hooks = {0};

int
dsd_frame_sync_hook_scan_visit_should_yield(const dsd_opts* opts, dsd_state* state) {
    return g_frame_sync_hooks.scan_visit_should_yield ? g_frame_sync_hooks.scan_visit_should_yield(opts, state) : 0;
}

void
dsd_frame_sync_hooks_set(const dsd_frame_sync_hooks* hooks) {
    g_frame_sync_hooks = hooks ? *hooks : (dsd_frame_sync_hooks){0};
}

void
dsd_frame_sync_hooks_get(dsd_frame_sync_hooks* out) {
    if (out) {
        *out = g_frame_sync_hooks;
    }
}

void
dsd_frame_sync_note_replay_center(dsd_opts* opts, dsd_state* state, uint32_t center_hz) {
    if (dsd_opts_note_iq_replay_center(opts, center_hz) && g_frame_sync_hooks.replay_retune) {
        g_frame_sync_hooks.replay_retune(opts, state);
    }
}

void
dsd_frame_sync_hook_p25_sm_try_tick(dsd_opts* opts, dsd_state* state) {
    if (!g_frame_sync_hooks.p25_sm_try_tick) {
        return;
    }
    g_frame_sync_hooks.p25_sm_try_tick(opts, state);
}

void
dsd_frame_sync_hook_p25_sm_release(dsd_opts* opts, dsd_state* state) {
    if (!g_frame_sync_hooks.p25_sm_release) {
        return;
    }
    g_frame_sync_hooks.p25_sm_release(opts, state);
}

void
dsd_frame_sync_hook_p25_sm_vc_sync(dsd_opts* opts, const dsd_state* state) {
    if (!g_frame_sync_hooks.p25_sm_vc_sync) {
        return;
    }
    g_frame_sync_hooks.p25_sm_vc_sync(opts, state);
}

void
dsd_frame_sync_hook_p25_sm_vc_no_sync(dsd_opts* opts, const dsd_state* state) {
    if (!g_frame_sync_hooks.p25_sm_vc_no_sync) {
        return;
    }
    g_frame_sync_hooks.p25_sm_vc_no_sync(opts, state);
}

void
dsd_frame_sync_hook_eot_cc(dsd_opts* opts, dsd_state* state) {
    if (!g_frame_sync_hooks.eot_cc) {
        return;
    }
    g_frame_sync_hooks.eot_cc(opts, state);
}

void
dsd_frame_sync_hook_no_carrier(dsd_opts* opts, dsd_state* state) {
    if (!g_frame_sync_hooks.no_carrier) {
        return;
    }
    g_frame_sync_hooks.no_carrier(opts, state);
}

int
dsd_frame_sync_hook_decoder_ticks_due(const dsd_opts* opts, const dsd_state* state) {
    return g_frame_sync_hooks.decoder_ticks_due ? g_frame_sync_hooks.decoder_ticks_due(opts, state) : 0;
}

int
dsd_frame_sync_hook_scan_owns_hunt(const dsd_opts* opts, const dsd_state* state) {
    return g_frame_sync_hooks.scan_owns_hunt ? g_frame_sync_hooks.scan_owns_hunt(opts, state) : 0;
}

int
dsd_frame_sync_hook_scan_quiet_service(const dsd_opts* opts, dsd_state* state) {
    /* The setting off: this one load is the hunt's whole cost. */
    if (!opts->scan_skip_silent) {
        return 0;
    }
    return g_frame_sync_hooks.scan_quiet_service ? g_frame_sync_hooks.scan_quiet_service(opts, state) : 0;
}

int
dsd_frame_sync_hook_tick_guard_enter(void) {
    return g_frame_sync_hooks.tick_guard_enter ? g_frame_sync_hooks.tick_guard_enter() : 0;
}

void
dsd_frame_sync_hook_tick_guard_leave(int taken) {
    if (taken && g_frame_sync_hooks.tick_guard_leave) {
        g_frame_sync_hooks.tick_guard_leave();
    }
}

void
dsd_scan_quiet_setting_changed(dsd_state* state, int turned_on) {
    if (g_frame_sync_hooks.scan_quiet_setting_changed) {
        g_frame_sync_hooks.scan_quiet_setting_changed(state, turned_on);
    }
}

int
dsd_frame_sync_hook_p25_probe_in_flight(const dsd_opts* opts, const dsd_state* state) {
    return g_frame_sync_hooks.p25_probe_in_flight ? g_frame_sync_hooks.p25_probe_in_flight(opts, state) : 0;
}

int
dsd_frame_sync_hook_signal_search_probe(const dsd_opts* opts, const dsd_state* state) {
    return g_frame_sync_hooks.signal_search_probe ? g_frame_sync_hooks.signal_search_probe(opts, state) : 0;
}

int
dsd_frame_sync_hook_signal_search_should_yield(const dsd_opts* opts, dsd_state* state) {
    return g_frame_sync_hooks.signal_search_should_yield ? g_frame_sync_hooks.signal_search_should_yield(opts, state)
                                                         : 0;
}

void
dsd_frame_sync_hook_analog_call_block(dsd_opts* opts, dsd_state* state, const dsd_analog_call_block* block) {
    if (g_frame_sync_hooks.analog_call_block && block) {
        g_frame_sync_hooks.analog_call_block(opts, state, block);
    }
}

void
dsd_frame_sync_hook_receive_policy_boundary(dsd_opts* opts, dsd_state* state, int filters_were_enabled) {
    if (g_frame_sync_hooks.receive_policy_boundary) {
        g_frame_sync_hooks.receive_policy_boundary(opts, state, filters_were_enabled);
    }
}
