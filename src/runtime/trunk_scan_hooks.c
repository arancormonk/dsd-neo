// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static dsd_trunk_scan_hooks g_trunk_scan_hooks = {0};

static int
trunk_sync_is_non_p25(int sync) {
    return DSD_SYNC_IS_DMR(sync) || DSD_SYNC_IS_NXDN(sync) || DSD_SYNC_IS_EDACS(sync) || DSD_SYNC_IS_X2TDMA(sync);
}

int
dsd_trunk_p25_recovery_allowed(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || opts->trunk_enable != 1) {
        return 0;
    }
    if (opts->trunk_scan_enabled == 1) {
        return dsd_trunk_scan_hook_p25_ctx() != NULL;
    }
    if (!opts->frame_p25p1 && !opts->frame_p25p2) {
        return 0;
    }
    const int sync = state->synctype != DSD_SYNC_NONE ? state->synctype : state->lastsynctype;
    if (trunk_sync_is_non_p25(sync)) {
        return 0;
    }
    /* Generic trunk return hints can outlive their protocol. A known P25
     * channel format, or an explicitly P25-only trunk decoder, can bridge sync
     * loss; the shared frequency cache alone cannot do that in AUTO. */
    const int known_p25_cc = state->p25_cc_is_tdma == 0 || state->p25_cc_is_tdma == 1;
    return DSD_SYNC_IS_P25(sync) || (state->p25_cc_freq > 0 && (known_p25_cc || !opts->frame_dmr));
}

int
dsd_trunk_dmr_recovery_allowed(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || opts->trunk_enable != 1) {
        return 0;
    }
    if (opts->trunk_scan_enabled == 1) {
        return dsd_trunk_scan_hook_dmr_ctx() != NULL;
    }
    if (!opts->frame_dmr) {
        return 0;
    }
    const int sync = state->synctype != DSD_SYNC_NONE ? state->synctype : state->lastsynctype;
    if (sync != DSD_SYNC_NONE) {
        return DSD_SYNC_IS_DMR(sync);
    }
    return !opts->frame_p25p1 && !opts->frame_p25p2 && state->rf_mod == 2 && state->trunk_cc_freq > 0;
}

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
dsd_trunk_scan_hook_dmr_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                              uint32_t source, int is_private, int encrypted, int data_call) {
    if (g_trunk_scan_hooks.dmr_conventional_activity) {
        g_trunk_scan_hooks.dmr_conventional_activity(opts, state, target, source, is_private, encrypted, data_call);
    }
}

void
dsd_trunk_scan_hook_nxdn_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                               uint32_t source, int is_private, int encrypted, int data_call) {
    if (g_trunk_scan_hooks.nxdn_conventional_activity) {
        g_trunk_scan_hooks.nxdn_conventional_activity(opts, state, target, source, is_private, encrypted, data_call);
    }
}

void
dsd_trunk_scan_hook_p25_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                              uint32_t source, int is_private, int encrypted, int data_call) {
    if (g_trunk_scan_hooks.p25_conventional_activity) {
        g_trunk_scan_hooks.p25_conventional_activity(opts, state, target, source, is_private, encrypted, data_call);
    }
}

const char*
dsd_trunk_scan_hook_active_chan_csv(const dsd_state* state) {
    return g_trunk_scan_hooks.active_chan_csv ? g_trunk_scan_hooks.active_chan_csv(state) : NULL;
}

void
dsd_trunk_scan_hook_enc_lockout_clear_snapshots(const dsd_state* state) {
    if (g_trunk_scan_hooks.enc_lockout_clear_snapshots) {
        g_trunk_scan_hooks.enc_lockout_clear_snapshots(state);
    }
}

int
dsd_trunk_scan_hook_control(dsd_opts* opts, dsd_state* state, int op) {
    if (!g_trunk_scan_hooks.control) {
        return DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE;
    }
    return g_trunk_scan_hooks.control(opts, state, op);
}
