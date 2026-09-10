// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dmr_key_map.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stddef.h>
#include <stdint.h>

static dsd_trunk_scan_hooks g_trunk_scan_hooks = {0};

int
dsd_trunk_scan_hook_decryption_apply(dsd_opts* opts, dsd_state* state, const char* target_id, uint64_t generation,
                                     uint32_t fields, const dsd_key_set* keys, const dsd_dmr_key_map* map, int force) {
    return g_trunk_scan_hooks.decryption_apply
               ? g_trunk_scan_hooks.decryption_apply(opts, state, target_id, generation, fields, keys, map, force)
               : DSD_TRUNK_KEY_UNAVAILABLE;
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
