// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 regroup/patch table state: the listening clock and entry freshness (see <dsd-neo/core/p25_patch.h>).
 */

#include <dsd-neo/core/p25_patch.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>

/* Floating-point slack on the 0.25 s gap bound, so a gap that is 0.25 s in clock ticks still counts. */
#define DSD_P25_PATCH_LISTEN_GAP_SLACK_S 1e-9
/* Floating-point slack on the TTL: a clock summed from many gaps that reads the TTL is still within it. */
#define DSD_P25_PATCH_TTL_SLACK_S        1e-6

static int
dsd_p25_patch_idx_valid(const dsd_state* state, int idx) {
    return (state && idx >= 0 && idx < DSD_P25_PATCH_MAX) ? 1 : 0;
}

static int
dsd_p25_patch_stamp_current(const dsd_state* state, double stamp) {
    return (state->p25_patch_listen_s - stamp <= (double)DSD_P25_PATCH_TTL_SECONDS + DSD_P25_PATCH_TTL_SLACK_S) ? 1 : 0;
}

void
dsd_p25_patch_listen_note(dsd_state* state, double now_m) {
    if (!state) {
        return;
    }
    const double last = state->p25_patch_listen_last_m;
    if (last > 0.0) {
        const double gap = now_m - last;
        if (gap > 0.0 && gap <= DSD_P25_PATCH_LISTEN_GAP_S + DSD_P25_PATCH_LISTEN_GAP_SLACK_S) {
            state->p25_patch_listen_s += gap;
        }
    }
    state->p25_patch_listen_last_m = now_m;
}

void
dsd_p25_patch_listen_break(dsd_state* state) {
    if (state) {
        state->p25_patch_listen_last_m = 0.0;
    }
}

int
dsd_p25_patch_entry_seen_current(const dsd_state* state, int idx) {
    if (!dsd_p25_patch_idx_valid(state, idx)) {
        return 0;
    }
    return dsd_p25_patch_stamp_current(state, state->p25_patch_seen_listen_s[idx]);
}

int
dsd_p25_patch_members_current(const dsd_state* state, int idx) {
    if (!dsd_p25_patch_idx_valid(state, idx)) {
        return 0;
    }
    return dsd_p25_patch_stamp_current(state, state->p25_patch_members_listen_s[idx]);
}

int
dsd_p25_patch_has_current_wgid(const dsd_state* state, int idx, uint16_t wgid) {
    if (!dsd_p25_patch_members_current(state, idx)) {
        return 0;
    }
    const int count = state->p25_patch_wgid_count[idx];
    for (int k = 0; k < count && k < DSD_P25_PATCH_GROUP_MAX; k++) {
        if (state->p25_patch_wgid[idx][k] == wgid) {
            return 1;
        }
    }
    return 0;
}

uint32_t
dsd_p25_patch_seq_age(const dsd_state* state, int idx) {
    if (!dsd_p25_patch_idx_valid(state, idx)) {
        return UINT32_MAX;
    }
    return state->p25_patch_seq_counter - state->p25_patch_seq[idx];
}

void
dsd_p25_patch_touch_seen(dsd_state* state, int idx) {
    if (!dsd_p25_patch_idx_valid(state, idx)) {
        return;
    }
    state->p25_patch_seen_listen_s[idx] = state->p25_patch_listen_s;
    state->p25_patch_seq_counter++;
    state->p25_patch_seq[idx] = state->p25_patch_seq_counter;
}

void
dsd_p25_patch_touch_members(dsd_state* state, int idx) {
    if (!dsd_p25_patch_idx_valid(state, idx)) {
        return;
    }
    dsd_p25_patch_touch_seen(state, idx);
    state->p25_patch_members_listen_s[idx] = state->p25_patch_listen_s;
}

void
dsd_p25_patch_expire_members(dsd_state* state, int idx) {
    if (!dsd_p25_patch_idx_valid(state, idx)) {
        return;
    }
    state->p25_patch_wgid_count[idx] = 0;
    state->p25_patch_wuid_count[idx] = 0;
    DSD_MEMSET(state->p25_patch_wgid[idx], 0, sizeof(state->p25_patch_wgid[idx]));
    DSD_MEMSET(state->p25_patch_wuid[idx], 0, sizeof(state->p25_patch_wuid[idx]));
    state->p25_patch_key[idx] = 0;
    state->p25_patch_alg[idx] = 0;
    state->p25_patch_ssn[idx] = 0;
    state->p25_patch_key_valid[idx] = 0;
    state->p25_patch_members_listen_s[idx] = DSD_P25_PATCH_STAMP_NEVER_S;
}

int
dsd_p25_patch_expire_stale_members(dsd_state* state, int idx) {
    if (!dsd_p25_patch_idx_valid(state, idx) || dsd_p25_patch_members_current(state, idx)) {
        return 0;
    }
    dsd_p25_patch_expire_members(state, idx);
    return 1;
}

void
dsd_p25_patch_clear_all(dsd_state* state) {
    if (!state) {
        return;
    }
    state->p25_patch_count = 0;
    DSD_MEMSET(state->p25_patch_sgid, 0, sizeof(state->p25_patch_sgid));
    DSD_MEMSET(state->p25_patch_is_patch, 0, sizeof(state->p25_patch_is_patch));
    DSD_MEMSET(state->p25_patch_active, 0, sizeof(state->p25_patch_active));
    DSD_MEMSET(state->p25_patch_last_update, 0, sizeof(state->p25_patch_last_update));
    DSD_MEMSET(state->p25_patch_seen_listen_s, 0, sizeof(state->p25_patch_seen_listen_s));
    DSD_MEMSET(state->p25_patch_members_listen_s, 0, sizeof(state->p25_patch_members_listen_s));
    DSD_MEMSET(state->p25_patch_seq, 0, sizeof(state->p25_patch_seq));
    state->p25_patch_seq_counter = 0U;
    state->p25_patch_listen_s = 0.0;
    state->p25_patch_listen_last_m = 0.0;
    state->p25_patch_owner_wacn = 0U;
    state->p25_patch_owner_sysid = 0U;
    state->p25_patch_owner_known = 0U;
    DSD_MEMSET(state->p25_patch_wgid_count, 0, sizeof(state->p25_patch_wgid_count));
    DSD_MEMSET(state->p25_patch_wgid, 0, sizeof(state->p25_patch_wgid));
    DSD_MEMSET(state->p25_patch_wuid_count, 0, sizeof(state->p25_patch_wuid_count));
    DSD_MEMSET(state->p25_patch_wuid, 0, sizeof(state->p25_patch_wuid));
    DSD_MEMSET(state->p25_patch_key, 0, sizeof(state->p25_patch_key));
    DSD_MEMSET(state->p25_patch_alg, 0, sizeof(state->p25_patch_alg));
    DSD_MEMSET(state->p25_patch_ssn, 0, sizeof(state->p25_patch_ssn));
    DSD_MEMSET(state->p25_patch_key_valid, 0, sizeof(state->p25_patch_key_valid));
}

int
dsd_p25_patch_note_system(dsd_state* state, uint32_t wacn, uint16_t sysid) {
    if (!state || (wacn == 0U && sysid == 0U)) {
        return 0;
    }
    if (state->p25_patch_owner_known && state->p25_patch_owner_wacn == wacn && state->p25_patch_owner_sysid == sysid) {
        return 0;
    }
    const int cleared = state->p25_patch_owner_known ? 1 : 0;
    if (cleared) {
        dsd_p25_patch_clear_all(state);
    }
    state->p25_patch_owner_wacn = wacn;
    state->p25_patch_owner_sysid = sysid;
    state->p25_patch_owner_known = 1U;
    return cleared;
}

void
dsd_p25_patch_snapshot_save(const dsd_state* state, dsd_p25_patch_snapshot* snapshot) {
    if (!state || !snapshot) {
        return;
    }
    snapshot->listen_s = state->p25_patch_listen_s;
    DSD_MEMCPY(snapshot->seen_listen_s, state->p25_patch_seen_listen_s, sizeof(snapshot->seen_listen_s));
    DSD_MEMCPY(snapshot->members_listen_s, state->p25_patch_members_listen_s, sizeof(snapshot->members_listen_s));
    DSD_MEMCPY(snapshot->last_update, state->p25_patch_last_update, sizeof(snapshot->last_update));
    snapshot->count = state->p25_patch_count;
    DSD_MEMCPY(snapshot->wuid, state->p25_patch_wuid, sizeof(snapshot->wuid));
    DSD_MEMCPY(snapshot->seq, state->p25_patch_seq, sizeof(snapshot->seq));
    snapshot->seq_counter = state->p25_patch_seq_counter;
    snapshot->owner_wacn = state->p25_patch_owner_wacn;
    DSD_MEMCPY(snapshot->sgid, state->p25_patch_sgid, sizeof(snapshot->sgid));
    DSD_MEMCPY(snapshot->key, state->p25_patch_key, sizeof(snapshot->key));
    DSD_MEMCPY(snapshot->wgid, state->p25_patch_wgid, sizeof(snapshot->wgid));
    snapshot->owner_sysid = state->p25_patch_owner_sysid;
    DSD_MEMCPY(snapshot->is_patch, state->p25_patch_is_patch, sizeof(snapshot->is_patch));
    DSD_MEMCPY(snapshot->active, state->p25_patch_active, sizeof(snapshot->active));
    DSD_MEMCPY(snapshot->wgid_count, state->p25_patch_wgid_count, sizeof(snapshot->wgid_count));
    DSD_MEMCPY(snapshot->wuid_count, state->p25_patch_wuid_count, sizeof(snapshot->wuid_count));
    DSD_MEMCPY(snapshot->alg, state->p25_patch_alg, sizeof(snapshot->alg));
    DSD_MEMCPY(snapshot->ssn, state->p25_patch_ssn, sizeof(snapshot->ssn));
    DSD_MEMCPY(snapshot->key_valid, state->p25_patch_key_valid, sizeof(snapshot->key_valid));
    snapshot->owner_known = state->p25_patch_owner_known;
}

void
dsd_p25_patch_snapshot_restore(dsd_state* state, const dsd_p25_patch_snapshot* snapshot) {
    if (!state || !snapshot) {
        return;
    }
    state->p25_patch_listen_s = snapshot->listen_s;
    DSD_MEMCPY(state->p25_patch_seen_listen_s, snapshot->seen_listen_s, sizeof(state->p25_patch_seen_listen_s));
    DSD_MEMCPY(state->p25_patch_members_listen_s, snapshot->members_listen_s,
               sizeof(state->p25_patch_members_listen_s));
    DSD_MEMCPY(state->p25_patch_last_update, snapshot->last_update, sizeof(state->p25_patch_last_update));
    state->p25_patch_count = snapshot->count;
    DSD_MEMCPY(state->p25_patch_wuid, snapshot->wuid, sizeof(state->p25_patch_wuid));
    DSD_MEMCPY(state->p25_patch_seq, snapshot->seq, sizeof(state->p25_patch_seq));
    state->p25_patch_seq_counter = snapshot->seq_counter;
    state->p25_patch_owner_wacn = snapshot->owner_wacn;
    DSD_MEMCPY(state->p25_patch_sgid, snapshot->sgid, sizeof(state->p25_patch_sgid));
    DSD_MEMCPY(state->p25_patch_key, snapshot->key, sizeof(state->p25_patch_key));
    DSD_MEMCPY(state->p25_patch_wgid, snapshot->wgid, sizeof(state->p25_patch_wgid));
    state->p25_patch_owner_sysid = snapshot->owner_sysid;
    DSD_MEMCPY(state->p25_patch_is_patch, snapshot->is_patch, sizeof(state->p25_patch_is_patch));
    DSD_MEMCPY(state->p25_patch_active, snapshot->active, sizeof(state->p25_patch_active));
    DSD_MEMCPY(state->p25_patch_wgid_count, snapshot->wgid_count, sizeof(state->p25_patch_wgid_count));
    DSD_MEMCPY(state->p25_patch_wuid_count, snapshot->wuid_count, sizeof(state->p25_patch_wuid_count));
    DSD_MEMCPY(state->p25_patch_alg, snapshot->alg, sizeof(state->p25_patch_alg));
    DSD_MEMCPY(state->p25_patch_ssn, snapshot->ssn, sizeof(state->p25_patch_ssn));
    DSD_MEMCPY(state->p25_patch_key_valid, snapshot->key_valid, sizeof(state->p25_patch_key_valid));
    state->p25_patch_owner_known = snapshot->owner_known;
    // The previous reading was taken on the carrier the receiver was on before the restore.
    dsd_p25_patch_listen_break(state);
}
