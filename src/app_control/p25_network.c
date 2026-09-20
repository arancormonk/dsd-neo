// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/p25_network.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>

/* Stable insertion into a small index list keeps equal timestamps deterministic. */
static void
insert(int* indices, time_t* times, int n, int index, time_t seen) {
    int pos = n;
    while (pos > 0 && times[pos - 1] < seen) {
        indices[pos] = indices[pos - 1];
        times[pos] = times[pos - 1];
        --pos;
    }
    indices[pos] = index;
    times[pos] = seen;
}

static int
is_candidate(const dsd_trunk_cc_candidates* cc, long int freq) {
    const int count = cc && cc->count > 0 && cc->count <= DSD_TRUNK_CC_CANDIDATES_MAX ? cc->count : 0;
    for (int i = 0; i < count; ++i) {
        if (cc->candidates[i] == freq) {
            return 1;
        }
    }
    return 0;
}

int
dsd_app_p25_neighbors(const dsd_state* state, dsd_app_p25_neighbor* out, int capacity) {
    if (!state || !out || capacity <= 0) {
        return 0;
    }
    int indices[P25_NB_MAX];
    time_t times[P25_NB_MAX];
    int n = 0;
    for (int i = 0; i < state->p25_nb_count && i < P25_NB_MAX; ++i) {
        if (state->p25_nb_entries[i].freq > 0) {
            insert(indices, times, n++, i, state->p25_nb_entries[i].last_seen);
        }
    }
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    if (n > capacity) {
        n = capacity;
    }
    for (int i = 0; i < n; ++i) {
        const p25_nb_entry_t* e = &state->p25_nb_entries[indices[i]];
        dsd_app_p25_neighbor* r = &out[i];
        DSD_MEMSET(r, 0, sizeof(*r));
        r->freq_hz = e->freq;
        r->wacn = e->wacn;
        r->sysid = e->sysid;
        r->rfss = e->rfss;
        r->site = e->site;
        r->lra = e->lra;
        r->wacn_valid = e->wacn_valid;
        r->lra_valid = e->lra_valid;
        r->last_seen = e->last_seen;
        r->is_current_cc = e->freq == state->p25_cc_freq;
        r->is_candidate = is_candidate(cc, e->freq);
        if (e->cfva_valid) {
            (void)p25_format_adjacent_cfva(e->cfva, r->cfva_text, sizeof(r->cfva_text));
            if (!r->cfva_text[0]) {
                r->cfva_text[0] = '-';
            }
        } else {
            r->cfva_text[0] = '?';
        }
    }
    return n;
}

int
dsd_app_p25_patches(const dsd_state* state, dsd_app_p25_patch* out, int capacity) {
    if (!state || !out || capacity <= 0) {
        return 0;
    }
    const time_t now = time(NULL);
    int indices[8];
    time_t times[8];
    int n = 0;
    for (int i = 0; i < state->p25_patch_count && i < 8; ++i) {
        // Match p25_patch.c'state 20-second announcement lifetime without its
        // terminal formatter'state in-place stale sweep: this state is a snapshot.
        const time_t seen = state->p25_patch_last_update[i];
        if (seen > 0 && difftime(now, seen) > 20.0) {
            continue;
        }
        if (state->p25_patch_active[i] && state->p25_patch_sgid[i]) {
            insert(indices, times, n++, i, state->p25_patch_last_update[i]);
        }
    }
    if (n > capacity) {
        n = capacity;
    }
    for (int i = 0; i < n; ++i) {
        int k = indices[i];
        dsd_app_p25_patch* r = &out[i];
        DSD_MEMSET(r, 0, sizeof(*r));
        r->sgid = state->p25_patch_sgid[k];
        r->is_patch = state->p25_patch_is_patch[k];
        r->last_seen = state->p25_patch_last_update[k];
        r->group_count = state->p25_patch_wgid_count[k] > 8 ? 8 : state->p25_patch_wgid_count[k];
        r->radio_count = state->p25_patch_wuid_count[k] > 8 ? 8 : state->p25_patch_wuid_count[k];
        DSD_MEMCPY(r->groups, state->p25_patch_wgid[k], r->group_count * sizeof(r->groups[0]));
        DSD_MEMCPY(r->radios, state->p25_patch_wuid[k], r->radio_count * sizeof(r->radios[0]));
    }
    return n;
}

static int
affiliations(const dsd_state* state, dsd_app_p25_affiliation* out, int capacity, int groups) {
    if (!state || !out || capacity <= 0) {
        return 0;
    }
    int indices[512];
    time_t times[512];
    int n = 0;
    const uint32_t* rids = groups ? state->p25_ga_rid : state->p25_aff_rid;
    const time_t* seen = groups ? state->p25_ga_last_seen : state->p25_aff_last_seen;
    for (int i = 0; i < (groups ? 512 : 256); ++i) {
        if (rids[i] && (!groups || state->p25_ga_tg[i])) {
            insert(indices, times, n++, i, seen[i]);
        }
    }
    if (n > capacity) {
        n = capacity;
    }
    for (int i = 0; i < n; ++i) {
        int k = indices[i];
        DSD_MEMSET(&out[i], 0, sizeof(out[i]));
        out[i].rid = rids[k];
        out[i].tg = groups ? state->p25_ga_tg[k] : 0;
        out[i].last_seen = seen[k];
    }
    return n;
}

int
dsd_app_p25_group_affiliations(const dsd_state* state, dsd_app_p25_affiliation* out, int capacity) {
    return affiliations(state, out, capacity, 1);
}

int
dsd_app_p25_affiliated_rids(const dsd_state* state, dsd_app_p25_affiliation* out, int capacity) {
    return affiliations(state, out, capacity, 0);
}
