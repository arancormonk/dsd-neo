// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/p25_network.h>
#include <dsd-neo/core/p25_patch.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/utf16.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <stddef.h>
#include <string.h>

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

/* Stable insertion by recency (smaller sequence age first) keeps equal ages deterministic. */
static void
insert_by_age(int* indices, uint32_t* ages, int n, int index, uint32_t age) {
    int pos = n;
    while (pos > 0 && ages[pos - 1] > age) {
        indices[pos] = indices[pos - 1];
        ages[pos] = ages[pos - 1];
        --pos;
    }
    indices[pos] = index;
    ages[pos] = age;
}

static void
copy_patch_row(const dsd_state* state, int k, dsd_app_p25_patch* r) {
    DSD_MEMSET(r, 0, sizeof(*r));
    r->sgid = state->p25_patch_sgid[k];
    r->is_patch = state->p25_patch_is_patch[k];
    r->last_seen = state->p25_patch_last_update[k];
    // A row lasts while its SG is named; its member lists only while a membership message keeps them current.
    if (!dsd_p25_patch_members_current(state, k)) {
        return;
    }
    const uint8_t groups = state->p25_patch_wgid_count[k];
    const uint8_t radios = state->p25_patch_wuid_count[k];
    r->group_count = groups > DSD_P25_PATCH_GROUP_MAX ? DSD_P25_PATCH_GROUP_MAX : groups;
    r->radio_count = radios > DSD_P25_PATCH_RADIO_MAX ? DSD_P25_PATCH_RADIO_MAX : radios;
    DSD_MEMCPY(r->groups, state->p25_patch_wgid[k], r->group_count * sizeof(r->groups[0]));
    DSD_MEMCPY(r->radios, state->p25_patch_wuid[k], r->radio_count * sizeof(r->radios[0]));
}

int
dsd_app_p25_patches(const dsd_state* state, dsd_app_p25_patch* out, int capacity) {
    if (!state || !out || capacity <= 0) {
        return 0;
    }
    int indices[DSD_P25_PATCH_MAX];
    uint32_t ages[DSD_P25_PATCH_MAX];
    int n = 0;
    for (int i = 0; i < state->p25_patch_count && i < DSD_P25_PATCH_MAX; ++i) {
        if (state->p25_patch_active[i] && state->p25_patch_sgid[i] && dsd_p25_patch_entry_seen_current(state, i)) {
            insert_by_age(indices, ages, n++, i, dsd_p25_patch_seq_age(state, i));
        }
    }
    if (n > capacity) {
        n = capacity;
    }
    for (int i = 0; i < n; ++i) {
        copy_patch_row(state, indices[i], &out[i]);
    }
    return n;
}

/* Cut @p s before a character its end leaves incomplete: a list's name can be cut mid-character (49 bytes). */
static void
keep_whole_characters(char* s) {
    const size_t n = strlen(s);
    size_t i = 0;
    while (i < n) {
        unsigned char lo;
        unsigned char hi;
        const size_t length = dsd_utf8_lead((unsigned char)s[i], &lo, &hi);
        if (length == 0U || n - i < length) {
            s[i] = '\0';
            return;
        }
        i += length;
    }
}

/* A row's names from the held snapshot's lists: the radio's in the current reception's scope, the talkgroup's label. */
static void
name_row(const dsd_state* state, dsd_app_p25_affiliation* row) {
    if (dsd_source_label_lookup(state, row->rid, NULL, 0U, row->name, sizeof row->name) == 1) {
        keep_whole_characters(row->name);
    }
    if (row->tg != 0U
        && dsd_tg_policy_lookup_group_label(state, row->tg, NULL, 0U, row->tg_name, sizeof row->tg_name)) {
        keep_whole_characters(row->tg_name);
    }
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
        name_row(state, &out[i]);
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
