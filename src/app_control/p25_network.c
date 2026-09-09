// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/p25_network.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
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

int
dsd_app_p25_neighbors(const dsd_state* s, dsd_app_p25_neighbor* out, int capacity) {
    if (!s || !out || capacity <= 0) {
        return 0;
    }
    int indices[P25_NB_MAX];
    time_t times[P25_NB_MAX];
    int n = 0;
    for (int i = 0; i < s->p25_nb_count && i < P25_NB_MAX; ++i) {
        if (s->p25_nb_entries[i].freq > 0) {
            insert(indices, times, n++, i, s->p25_nb_entries[i].last_seen);
        }
    }
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(s);
    int count = cc && cc->count > 0 && cc->count <= DSD_TRUNK_CC_CANDIDATES_MAX ? cc->count : 0;
    if (n > capacity) {
        n = capacity;
    }
    for (int i = 0; i < n; ++i) {
        const p25_nb_entry_t* e = &s->p25_nb_entries[indices[i]];
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
        r->is_current_cc = e->freq == s->p25_cc_freq;
        for (int j = 0; j < count; ++j) {
            if (cc->candidates[j] == e->freq) {
                r->is_candidate = 1;
            }
        }
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
dsd_app_p25_patches(const dsd_state* s, dsd_app_p25_patch* out, int capacity) {
    if (!s || !out || capacity <= 0) {
        return 0;
    }
    const time_t now = time(NULL);
    int indices[8];
    time_t times[8];
    int n = 0;
    for (int i = 0; i < s->p25_patch_count && i < 8; ++i) {
        // Match p25_patch.c's 20-second announcement lifetime without its
        // terminal formatter's in-place stale sweep: this state is a snapshot.
        const time_t seen = s->p25_patch_last_update[i];
        if (seen > 0 && difftime(now, seen) > 20.0) {
            continue;
        }
        if (s->p25_patch_active[i] && s->p25_patch_sgid[i]) {
            insert(indices, times, n++, i, s->p25_patch_last_update[i]);
        }
    }
    if (n > capacity) {
        n = capacity;
    }
    for (int i = 0; i < n; ++i) {
        int k = indices[i];
        dsd_app_p25_patch* r = &out[i];
        DSD_MEMSET(r, 0, sizeof(*r));
        r->sgid = s->p25_patch_sgid[k];
        r->is_patch = s->p25_patch_is_patch[k];
        r->last_seen = s->p25_patch_last_update[k];
        r->group_count = s->p25_patch_wgid_count[k] > 8 ? 8 : s->p25_patch_wgid_count[k];
        r->radio_count = s->p25_patch_wuid_count[k] > 8 ? 8 : s->p25_patch_wuid_count[k];
        DSD_MEMCPY(r->groups, s->p25_patch_wgid[k], r->group_count * sizeof(r->groups[0]));
        DSD_MEMCPY(r->radios, s->p25_patch_wuid[k], r->radio_count * sizeof(r->radios[0]));
    }
    return n;
}

static int
affiliations(const dsd_state* s, dsd_app_p25_affiliation* out, int capacity, int groups) {
    if (!s || !out || capacity <= 0) {
        return 0;
    }
    int indices[512];
    time_t times[512];
    int n = 0;
    const uint32_t* rids = groups ? s->p25_ga_rid : s->p25_aff_rid;
    const time_t* seen = groups ? s->p25_ga_last_seen : s->p25_aff_last_seen;
    for (int i = 0; i < (groups ? 512 : 256); ++i) {
        if (rids[i] && (!groups || s->p25_ga_tg[i])) {
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
        out[i].tg = groups ? s->p25_ga_tg[k] : 0;
        out[i].last_seen = seen[k];
    }
    return n;
}

int
dsd_app_p25_group_affiliations(const dsd_state* s, dsd_app_p25_affiliation* out, int capacity) {
    return affiliations(s, out, capacity, 1);
}

int
dsd_app_p25_affiliated_rids(const dsd_state* s, dsd_app_p25_affiliation* out, int capacity) {
    return affiliations(s, out, capacity, 0);
}
