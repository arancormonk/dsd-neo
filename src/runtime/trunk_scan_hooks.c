// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dmr_key_map.h>
#include <dsd-neo/core/edacs_session.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
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
dsd_trunk_recovery_note_protocol(dsd_state* state, dsd_trunk_recovery_protocol protocol) {
    if (state) {
        state->trunk_recovery_protocol = (int)protocol;
    }
}

int
dsd_trunk_p25_recovery_allowed(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || opts->trunk_enable != 1) {
        return 0;
    }
    /* Conventional scanning and P25 recovery are exclusive: -Y row policy
     * stores rotate without a P25 tick in flight (#554). */
    if (opts->scanner_mode == 1) {
        return 0;
    }
    if (opts->trunk_scan_enabled == 1) {
        return dsd_trunk_scan_hook_p25_ctx() != NULL;
    }
    if (!opts->frame_p25p1 && !opts->frame_p25p2) {
        return 0;
    }
    return state->trunk_recovery_protocol == DSD_TRUNK_RECOVERY_P25
           || (state->trunk_recovery_protocol == DSD_TRUNK_RECOVERY_UNKNOWN && !opts->frame_dmr && !opts->frame_nxdn48
               && !opts->frame_nxdn96 && !opts->frame_provoice);
}

int
dsd_trunk_p25_cold_start_presumed(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || opts->trunk_enable != 1 || opts->scanner_mode == 1
        || dsd_trunk_scan_owns_tuner(opts, state)) {
        return 0;
    }
    if (state->trunk_recovery_protocol != DSD_TRUNK_RECOVERY_UNKNOWN || (!opts->frame_p25p1 && !opts->frame_p25p2)) {
        return 0;
    }
    /* EDACS control names no owner: a P25 window would hunt away from a valid EDACS control channel. */
    return !dsd_edacs_session(opts, state);
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
    return state->trunk_recovery_protocol == DSD_TRUNK_RECOVERY_DMR
           || (state->trunk_recovery_protocol == DSD_TRUNK_RECOVERY_UNKNOWN && !opts->frame_p25p1 && !opts->frame_p25p2
               && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice);
}

int
dsd_trunk_nxdn_recovery_allowed(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || opts->trunk_enable != 1 || opts->scanner_mode == 1
        || dsd_trunk_scan_owns_tuner(opts, state)) {
        return 0;
    }
    if (!opts->frame_nxdn48 && !opts->frame_nxdn96) {
        return 0;
    }
    return state->trunk_recovery_protocol == DSD_TRUNK_RECOVERY_NXDN
           || (state->trunk_recovery_protocol == DSD_TRUNK_RECOVERY_UNKNOWN && !opts->frame_p25p1 && !opts->frame_p25p2
               && !opts->frame_dmr && !opts->frame_provoice);
}

void
dsd_trunk_scan_hooks_set(const dsd_trunk_scan_hooks* hooks) {
    g_trunk_scan_hooks = hooks ? *hooks : (dsd_trunk_scan_hooks){0};
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

/* The confirmation reaches a signal search from every report; its IDs only from content whose own check verified (no
   -F RAS acceptance or relaxed failed CRC in the reporting slot's scope), which is stricter than the scan hold. */
static void
note_search_evidence(const dsd_state* state, const char* protocol, uint32_t target, uint32_t source) {
    const int checked = state != NULL && state->event_crc_invalid[state->currentslot & 1] == 0U;
    dsd_signal_search_note_evidence(state, protocol, checked ? target : 0U, checked ? source : 0U, NULL);
}

/* note_search_evidence() for a report that names the synctype and voice slot of its traffic (P25: a Phase 2 SACCH
   announces the opposite slot), which the search judges and reads the CRC scope of (issue #637). */
static void
note_search_evidence_at(const dsd_state* state, const char* protocol, int sync, int slot, uint32_t target,
                        uint32_t source) {
    const int lane = (slot >= 0 && slot <= 1) ? slot : (state ? state->currentslot & 1 : 0);
    const int checked = state != NULL && state->event_crc_invalid[lane] == 0U;
    dsd_signal_search_note_evidence_at(state, protocol, sync, lane, checked ? target : 0U, checked ? source : 0U, NULL);
}

void
dsd_trunk_scan_hook_dmr_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                              uint32_t source, int is_private, int encrypted, int data_call) {
    note_search_evidence(state, "DMR", target, source);
    if (g_trunk_scan_hooks.dmr_conventional_activity) {
        g_trunk_scan_hooks.dmr_conventional_activity(opts, state, target, source, is_private, encrypted, data_call);
    }
}

void
dsd_trunk_scan_hook_nxdn_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                               uint32_t source, int is_private, int encrypted, int data_call) {
    note_search_evidence(state, "NXDN", target, source);
    if (g_trunk_scan_hooks.nxdn_conventional_activity) {
        g_trunk_scan_hooks.nxdn_conventional_activity(opts, state, target, source, is_private, encrypted, data_call);
    }
}

void
dsd_trunk_scan_hook_p25_conventional_activity(const dsd_opts* opts, const dsd_state* state, int protocol, int slot,
                                              uint32_t target, uint32_t source, int is_private, int encrypted,
                                              int data_call) {
    note_search_evidence_at(state, "P25", protocol, slot, target, source);
    if (g_trunk_scan_hooks.p25_conventional_activity) {
        g_trunk_scan_hooks.p25_conventional_activity(opts, state, protocol, slot, target, source, is_private, encrypted,
                                                     data_call);
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
