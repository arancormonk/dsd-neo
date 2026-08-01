// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Session-permanent encrypted-target lockout ledger (see header).
 */

#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdio.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static const dsd_enc_lockout_entry*
enc_lockout_find_const(const dsd_state* state, uint32_t target, int is_group) {
    if (!state || target == 0U) {
        return NULL;
    }
    const uint8_t want_group = is_group ? 1U : 0U;
    for (int i = 0; i < DSD_ENC_LOCKOUT_MAX; i++) {
        const dsd_enc_lockout_entry* e = &state->enc_lockout_entries[i];
        if (e->in_use && e->target == target && e->is_group == want_group) {
            return e;
        }
    }
    return NULL;
}

static dsd_enc_lockout_entry*
enc_lockout_find(dsd_state* state, uint32_t target, int is_group) {
    if (!state || target == 0U) {
        return NULL;
    }
    const uint8_t want_group = is_group ? 1U : 0U;
    for (int i = 0; i < DSD_ENC_LOCKOUT_MAX; i++) {
        dsd_enc_lockout_entry* e = &state->enc_lockout_entries[i];
        if (e->in_use && e->target == target && e->is_group == want_group) {
            return e;
        }
    }
    return NULL;
}

static dsd_enc_lockout_entry*
enc_lockout_choose_slot(dsd_state* state) {
    // Eviction order: free slot, then a stale-epoch entry (no longer blocking
    // until re-confirmed, so dropping it only costs the one probe it already
    // owed), then the least recently confirmed current-epoch entry.
    dsd_enc_lockout_entry* victim = NULL;
    for (int i = 0; i < DSD_ENC_LOCKOUT_MAX; i++) {
        dsd_enc_lockout_entry* e = &state->enc_lockout_entries[i];
        if (!e->in_use) {
            return e;
        }
        const int e_stale = (e->key_epoch != state->enc_lockout_key_epoch);
        const int victim_stale = victim && (victim->key_epoch != state->enc_lockout_key_epoch);
        if (!victim || (e_stale && !victim_stale) || (e_stale == victim_stale && e->last_seen < victim->last_seen)) {
            victim = e;
        }
    }
    if (victim) {
        DSD_FPRINTF(stderr, "enc lockout: ledger full (%d entries); evicting %s %u -- it re-probes on its next grant\n",
                    (int)DSD_ENC_LOCKOUT_MAX, victim->is_group ? "TG" : "target", victim->target);
    }
    return victim;
}

int
dsd_enc_lockout_note(dsd_state* state, uint32_t target, int is_group, int algid, int keyid) {
    if (!state || target == 0U) {
        return 0;
    }

    const time_t now = time(NULL);
    dsd_enc_lockout_entry* e = enc_lockout_find(state, target, is_group);
    int newly_locking = 0;
    if (!e) {
        e = enc_lockout_choose_slot(state);
        if (!e) {
            return 0;
        }
        // in_use drops first: UI threads read the ledger unsynchronized, and
        // memset clears the identity fields before the trailing in_use flag.
        e->in_use = 0;
        DSD_MEMSET(e, 0, sizeof(*e));
        e->target = target;
        e->is_group = is_group ? 1U : 0U;
        e->algid = (int16_t)DSD_ENC_LOCKOUT_ALGID_UNKNOWN;
        newly_locking = 1;
    } else if (e->key_epoch != state->enc_lockout_key_epoch) {
        // Re-confirmed after a key-material change: the probe failed, so the
        // target locks again for this epoch.
        newly_locking = 1;
    }

    e->in_use = 1;
    e->key_epoch = state->enc_lockout_key_epoch;
    e->last_seen = now;
    if (e->hits < UINT32_MAX) {
        e->hits++;
    }
    if (algid >= 0) {
        e->algid = (int16_t)(algid & 0xFF);
        e->keyid = (uint16_t)(keyid & 0xFFFF);
    }
    return newly_locking;
}

int
dsd_enc_lockout_entry_active(const dsd_state* state, uint32_t target, int is_group) {
    const dsd_enc_lockout_entry* e = enc_lockout_find_const(state, target, is_group);
    return (e && e->key_epoch == state->enc_lockout_key_epoch) ? 1 : 0;
}

int
dsd_enc_lockout_is_blocked(const dsd_opts* opts, const dsd_state* state, uint32_t target, int is_group) {
    if (!opts || !state || opts->trunk_tune_enc_calls != 0) {
        return 0;
    }
    return dsd_enc_lockout_entry_active(state, target, is_group);
}

int
dsd_enc_lockout_lookup(const dsd_state* state, uint32_t target, int is_group, dsd_enc_lockout_entry* out) {
    const dsd_enc_lockout_entry* e = enc_lockout_find_const(state, target, is_group);
    if (!e) {
        return 0;
    }
    if (out) {
        *out = *e;
    }
    return 1;
}

int
dsd_enc_lockout_release(dsd_state* state, uint32_t target, int is_group) {
    if (!state || target == 0U) {
        return 0;
    }
    dsd_enc_lockout_entry* e = enc_lockout_find(state, target, is_group);
    if (e) {
        // in_use drops first so unsynchronized UI readers never see a live
        // flag beside a partially cleared identity.
        e->in_use = 0;
        DSD_MEMSET(e, 0, sizeof(*e));
        return 1;
    }
    return 0;
}

void
dsd_enc_lockout_bump_key_epoch(dsd_state* state) {
    if (!state) {
        return;
    }
    state->enc_lockout_key_epoch++;
}

void
dsd_enc_lockout_clear_all(dsd_state* state) {
    if (!state) {
        return;
    }
    DSD_MEMSET(state->enc_lockout_entries, 0, sizeof(state->enc_lockout_entries));
}

int
dsd_enc_lockout_active_count(const dsd_state* state) {
    if (!state) {
        return 0;
    }
    int count = 0;
    for (int i = 0; i < DSD_ENC_LOCKOUT_MAX; i++) {
        const dsd_enc_lockout_entry* e = &state->enc_lockout_entries[i];
        if (e->in_use && e->key_epoch == state->enc_lockout_key_epoch) {
            count++;
        }
    }
    return count;
}

void
dsd_enc_lockout_init(dsd_state* state) {
    if (!state) {
        return;
    }
    DSD_MEMSET(state->enc_lockout_entries, 0, sizeof(state->enc_lockout_entries));
    state->enc_lockout_key_epoch = 1U;
}
