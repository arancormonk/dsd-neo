// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Control-channel candidate cache and neighbor helpers.
 * Keeps this logic separate from the trunking state machine so it can be
 * reused by tests and UI code without pulling in tuning policy.
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

int
p25_cc_add_candidate(dsd_state* state, long freq_hz, int bump_added) {
    if (!state || freq_hz == 0) {
        return 0;
    }
    if (freq_hz == state->p25_cc_freq) {
        return 0;
    }
    return dsd_trunk_cc_candidates_add_with_flags(state, freq_hz, bump_added, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);
}

void
p25_cc_record_neighbor_frequencies(const dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    if (count <= 0 || !state || !freqs) {
        return;
    }

    p25_cc_try_load_cache(opts, state);
    for (int i = 0; i < count; i++) {
        const long freq = freqs[i];
        if (freq == 0) {
            continue;
        }
        if (state->p25_cc_freq != 0 && freq == state->p25_cc_freq) {
            state->trunk_lcn_freq[0] = freq;
            if (state->lcn_freq_count < 1) {
                state->lcn_freq_count = 1;
            }
        }
        p25_nb_record_update(state, &(p25_neighbor_record_update_t){.freq = freq});
    }
}

int
p25_cc_next_candidate(dsd_state* state, long* out_freq) {
    if (!state || !out_freq) {
        return 0;
    }
    return dsd_trunk_cc_candidates_next_with_flags(state, dsd_time_now_monotonic_s(),
                                                   DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE, out_freq);
}

int
p25_cc_build_cache_path(const dsd_state* state, char* out, size_t out_len) {
    if (!state || !out || out_len == 0) {
        return 0;
    }
    if (state->p2_wacn == 0 || state->p2_sysid == 0) {
        return 0; // require system identity
    }

    char path[1024] = {0};
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    const char* root = (cfg && cfg->cache_dir[0] != '\0') ? cfg->cache_dir : ".dsdneo_cache";
    DSD_SNPRINTF(path, sizeof(path), "%s", root);

    dsd_stat_t st;
    if (dsd_stat_path(path, &st) != 0) {
        (void)dsd_mkdir(path, 0700);
    }

    int n = 0;
    if (state->p2_rfssid > 0 && state->p2_siteid > 0) {
        n = DSD_SNPRINTF(out, out_len, "%s/p25_cc_%05llX_%03llX_R%03llu_S%03llu.txt", path, state->p2_wacn,
                         state->p2_sysid, state->p2_rfssid, state->p2_siteid);
    } else {
        n = DSD_SNPRINTF(out, out_len, "%s/p25_cc_%05llX_%03llX.txt", path, state->p2_wacn, state->p2_sysid);
    }
    return (n > 0 && (size_t)n < out_len);
}

static int
p25_cc_cache_enabled(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    return cfg ? cfg->cc_cache_enable : 1;
}

static char*
p25_cc_cache_skip_ws(char* p) {
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static const char*
p25_cc_cache_line_payload(char* line) {
    char* p = p25_cc_cache_skip_ws(line);
    if (p[0] != 'c' || p[1] != 'c' || (p[2] != ' ' && p[2] != '\t')) {
        return NULL;
    }
    return p25_cc_cache_skip_ws(p + 2);
}

static int
p25_cc_parse_cache_line(char* line, long* out_freq_hz) {
    char* end = NULL;
    const char* p = p25_cc_cache_line_payload(line);
    if (!p) {
        return 0;
    }

    long freq_hz = strtol(p, &end, 10);
    if (end == p || freq_hz <= 0) {
        return 0;
    }

    *out_freq_hz = freq_hz;
    return 1;
}

static void
p25_cc_load_cache_lines(FILE* fp, dsd_state* state) {
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        long freq_hz = 0;
        if (p25_cc_parse_cache_line(line, &freq_hz)) {
            (void)p25_cc_add_candidate(state, freq_hz, 0);
        }
    }
}

static int
p25_cc_loaded_candidate_count(const dsd_state* state) {
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    if (!cc || cc->count <= 0 || cc->count > DSD_TRUNK_CC_CANDIDATES_MAX) {
        return 0;
    }
    return cc->count;
}

static void
p25_cc_log_cache_load(const dsd_opts* opts, const dsd_state* state) {
    const int count = p25_cc_loaded_candidate_count(state);
    if (opts && opts->verbose > 0 && count > 0) {
        DSD_FPRINTF(stderr, "\n  P25 SM: Loaded %d CC candidates from cache\n", count);
    }
}

void
p25_cc_try_load_cache(const dsd_opts* opts, dsd_state* state) {
    if (!state || state->p25_cc_cache_loaded) {
        return;
    }
    if (!p25_cc_cache_enabled()) {
        state->p25_cc_cache_loaded = 1;
        return;
    }

    char fpath[1024];
    if (!p25_cc_build_cache_path(state, fpath, sizeof(fpath))) {
        return;
    }
    FILE* fp = dsd_fopen_existing_regular_file(fpath, "r");
    if (!fp) {
        state->p25_cc_cache_loaded = 1;
        return;
    }

    p25_cc_load_cache_lines(fp, state);
    fclose(fp);
    state->p25_cc_cache_loaded = 1;
    p25_cc_log_cache_load(opts, state);
}

void
p25_cc_persist_cache(const dsd_opts* opts, const dsd_state* state) {
    if (!state) {
        return;
    }
    if (!p25_cc_cache_enabled()) {
        return;
    }

    char fpath[1024];
    if (!p25_cc_build_cache_path(state, fpath, sizeof(fpath))) {
        return;
    }
    FILE* fp = dsd_fopen_private(fpath, "w");
    if (!fp) {
        if (opts && opts->verbose > 1) {
            DSD_FPRINTF(stderr, "\n  P25 SM: Failed to open CC cache for write: %s (errno=%d)\n", fpath, errno);
        }
        return;
    }

    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    const int count = (cc && cc->count > 0 && cc->count <= DSD_TRUNK_CC_CANDIDATES_MAX) ? cc->count : 0;
    for (int i = 0; i < count; i++) {
        if (cc->candidates[i] != 0 && (cc->flags[i] & DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) != 0) {
            DSD_FPRINTF(fp, "cc %ld\n", cc->candidates[i]);
        }
    }
    fclose(fp);
}

#define P25_NB_TTL_SEC ((time_t)30 * 60)

static uint32_t
p25_neighbor_wacn20(const p25_neighbor_channel_announcement_t* announcement) {
    return announcement ? (announcement->wacn & 0xFFFFFU) : 0U;
}

static int
p25_nb_has_site_identity(const p25_neighbor_channel_announcement_t* announcement) {
    return announcement && (announcement->sysid != 0 || announcement->rfss != 0 || announcement->site != 0);
}

static int
p25_nb_sysid_matches(uint16_t entry_sysid, uint16_t announcement_sysid) {
    return entry_sysid == announcement_sysid || entry_sysid == 0 || announcement_sysid == 0;
}

static int
p25_nb_same_site_identity(const p25_nb_entry_t* entry, const p25_neighbor_channel_announcement_t* announcement) {
    if (!entry || !announcement) {
        return 0;
    }
    if (entry->rfss == 0 || entry->site == 0 || announcement->rfss == 0 || announcement->site == 0) {
        return entry->sysid == announcement->sysid && entry->rfss == announcement->rfss
               && entry->site == announcement->site;
    }
    return entry->rfss == announcement->rfss && entry->site == announcement->site
           && p25_nb_sysid_matches(entry->sysid, announcement->sysid);
}

static void
p25_nb_enrich_unknown_identity(p25_nb_entry_t* entry, const p25_neighbor_channel_announcement_t* announcement) {
    if (!entry || !announcement) {
        return;
    }
    if (entry->sysid == 0 && announcement->sysid != 0) {
        entry->sysid = announcement->sysid;
    }
}

static void
p25_nb_apply_record_update(p25_nb_entry_t* entry, const p25_neighbor_record_update_t* update, int copy_identity,
                           time_t now) {
    const p25_neighbor_channel_announcement_t* announcement = &update->announcement;
    entry->freq = update->freq;
    if (copy_identity) {
        entry->sysid = announcement->sysid;
        entry->rfss = announcement->rfss;
        entry->site = announcement->site;
    }
    if (announcement->wacn_valid) {
        entry->wacn = p25_neighbor_wacn20(announcement);
        entry->wacn_valid = 1;
    }
    if (announcement->lra_valid) {
        entry->lra = announcement->lra;
        entry->lra_valid = 1;
    }
    if (announcement->cfva_valid) {
        entry->cfva = announcement->cfva;
        entry->cfva_valid = 1;
    }
    entry->last_seen = now;
}

static int
p25_nb_update_by_site(dsd_state* state, const p25_neighbor_record_update_t* update, time_t now) {
    const p25_neighbor_channel_announcement_t* announcement = &update->announcement;
    for (int i = 0; i < state->p25_nb_count && i < P25_NB_MAX; i++) {
        p25_nb_entry_t* entry = &state->p25_nb_entries[i];
        if (p25_nb_same_site_identity(entry, announcement)) {
            if (announcement->wacn_valid && entry->wacn_valid && entry->wacn != p25_neighbor_wacn20(announcement)) {
                continue;
            }
            p25_nb_apply_record_update(entry, update, 0, now);
            p25_nb_enrich_unknown_identity(entry, announcement);
            return 1;
        }
    }
    return 0;
}

static int
p25_nb_update_by_freq(dsd_state* state, const p25_neighbor_record_update_t* update, int has_site_identity, time_t now) {
    for (int i = 0; i < state->p25_nb_count && i < P25_NB_MAX; i++) {
        p25_nb_entry_t* entry = &state->p25_nb_entries[i];
        if (entry->freq != update->freq) {
            continue;
        }
        if (has_site_identity && (entry->sysid != 0 || entry->rfss != 0 || entry->site != 0)) {
            continue;
        }
        p25_nb_apply_record_update(entry, update, has_site_identity, now);
        return 1;
    }
    return 0;
}

static int
p25_nb_select_insert_index(dsd_state* state) {
    int idx = state->p25_nb_count;
    if (idx < 0) {
        idx = 0;
    }
    if (idx < P25_NB_MAX) {
        state->p25_nb_count = idx + 1;
        return idx;
    }

    int repl = 0;
    time_t oldest = state->p25_nb_entries[0].last_seen;
    for (int i = 1; i < P25_NB_MAX; i++) {
        if (state->p25_nb_entries[i].last_seen < oldest) {
            oldest = state->p25_nb_entries[i].last_seen;
            repl = i;
        }
    }
    return repl;
}

static void
p25_nb_insert_record(dsd_state* state, const p25_neighbor_record_update_t* update, time_t now) {
    int idx = p25_nb_select_insert_index(state);
    p25_nb_entry_t* entry = &state->p25_nb_entries[idx];
    DSD_MEMSET(entry, 0, sizeof(*entry));
    p25_nb_apply_record_update(entry, update, 1, now);
}

void
p25_nb_record_update(dsd_state* state, const p25_neighbor_record_update_t* update) {
    if (!state || !update || update->freq <= 0) {
        return;
    }
    if (update->freq == state->p25_cc_freq) {
        return; /* Reject current CC as neighbor. */
    }

    int has_site_identity = p25_nb_has_site_identity(&update->announcement);
    time_t now = time(NULL);

    /* Structured neighbor broadcasts identify sites. Keep those entries keyed
     * by site identity so frequency reuse does not merge distinct neighbors. */
    if (has_site_identity && p25_nb_update_by_site(state, update, now)) {
        return;
    }

    /* Frequency-only updates refresh by frequency without clobbering any site
     * metadata learned from structured broadcasts. */
    if (p25_nb_update_by_freq(state, update, has_site_identity, now)) {
        return;
    }

    p25_nb_insert_record(state, update, now);
}

void
p25_nb_tick(dsd_state* state) {
    if (!state) {
        return;
    }
    time_t now = time(NULL);
    int w = 0;
    for (int i = 0; i < state->p25_nb_count && i < P25_NB_MAX; i++) {
        long f = state->p25_nb_entries[i].freq;
        time_t last = state->p25_nb_entries[i].last_seen;
        int keep = (f != 0) && (last == 0 || (now - last) <= P25_NB_TTL_SEC);
        if (keep) {
            if (w != i) {
                state->p25_nb_entries[w] = state->p25_nb_entries[i];
            }
            w++;
        }
    }
    for (int i = w; i < state->p25_nb_count && i < P25_NB_MAX; i++) {
        DSD_MEMSET(&state->p25_nb_entries[i], 0, sizeof(state->p25_nb_entries[i]));
    }
    state->p25_nb_count = w;
}

void
p25_store_system_service_broadcast(dsd_state* state, uint32_t available, uint32_t supported, uint8_t request_priority) {
    if (!state) {
        return;
    }
    state->p25_sys_services_valid = 1;
    state->p25_sys_services_available = available & 0xFFFFFFU;
    state->p25_sys_services_supported = supported & 0xFFFFFFU;
    state->p25_sys_services_request_priority = request_priority;
}

void
p25_store_site_lra(dsd_state* state, uint8_t lra) {
    if (!state) {
        return;
    }
    state->p25_site_lra = lra;
    state->p25_site_lra_valid = 1;
}

void
p25_store_site_network_active(dsd_state* state, uint8_t network_active) {
    if (!state) {
        return;
    }
    state->p25_site_network_active = network_active ? 1 : 0;
    state->p25_site_network_active_valid = 1;
}

void
p25_store_protected_control_channel(dsd_state* state, uint8_t algid) {
    if (!state) {
        return;
    }
    state->p25_cc_prot_valid = 1;
    state->p25_cc_prot_algid = algid;
}

static const char* const k_p25_system_service_names[24] = {
    NULL,
    NULL,
    "network active",
    NULL,
    "group voice",
    "individual voice",
    "PSTN-unit voice",
    "unit-PSTN voice",
    NULL,
    "group data",
    "individual data",
    NULL,
    "unit registration",
    "group affiliation",
    "group affiliation query",
    "authentication",
    "encryption",
    "user status",
    "user message",
    "unit status",
    "user status query",
    "unit status query",
    "unit page",
    "emergency alarm",
};

const char*
p25_system_service_name_for_bit(unsigned int service_bit) {
    if (service_bit < 1U || service_bit > 24U) {
        return NULL;
    }
    return k_p25_system_service_names[service_bit - 1U];
}

size_t
p25_format_system_service_names(uint32_t service_mask, char* out, size_t out_len) {
    if (out && out_len > 0) {
        out[0] = '\0';
    }
    size_t count = 0;
    size_t used = 0;
    for (unsigned int bit = 1; bit <= 24U; bit++) {
        const char* name = p25_system_service_name_for_bit(bit);
        if (!name || ((service_mask >> (24U - bit)) & 1U) == 0U) {
            continue;
        }
        count++;
        if (!out || out_len == 0 || used >= out_len - 1U) {
            continue;
        }
        const char* sep = (used > 0U) ? ", " : "";
        int wrote = DSD_SNPRINTF(out + used, out_len - used, "%s%s", sep, name);
        if (wrote < 0) {
            out[used] = '\0';
            continue;
        }
        if ((size_t)wrote >= out_len - used) {
            used = out_len - 1U;
            out[used] = '\0';
        } else {
            used += (size_t)wrote;
        }
    }
    return count;
}

const char*
p25_adjacent_cfva_flag_name(uint8_t flag_mask) {
    switch (flag_mask & 0x0FU) {
        case 0x8U: return "conventional";
        case 0x4U: return "site failure";
        case 0x2U: return "current";
        case 0x1U: return "network active";
        default: return NULL;
    }
}

static void
p25_append_tag(char* out, size_t out_len, size_t* used, size_t* count, const char* tag) {
    if (!tag) {
        return;
    }
    (*count)++;
    if (!out || out_len == 0 || *used >= out_len - 1U) {
        return;
    }
    const char* sep = (*used > 0U) ? "," : "";
    int wrote = DSD_SNPRINTF(out + *used, out_len - *used, "%s%s", sep, tag);
    if (wrote < 0) {
        out[*used] = '\0';
        return;
    }
    if ((size_t)wrote >= out_len - *used) {
        *used = out_len - 1U;
        out[*used] = '\0';
    } else {
        *used += (size_t)wrote;
    }
}

size_t
p25_format_adjacent_cfva(uint8_t cfva, char* out, size_t out_len) {
    if (out && out_len > 0) {
        out[0] = '\0';
    }
    size_t used = 0;
    size_t count = 0;
    if (cfva & 0x8U) {
        p25_append_tag(out, out_len, &used, &count, p25_adjacent_cfva_flag_name(0x8U));
    }
    if (cfva & 0x4U) {
        p25_append_tag(out, out_len, &used, &count, p25_adjacent_cfva_flag_name(0x4U));
    }
    p25_append_tag(out, out_len, &used, &count, (cfva & 0x2U) ? p25_adjacent_cfva_flag_name(0x2U) : "last known");
    if (cfva & 0x1U) {
        p25_append_tag(out, out_len, &used, &count, p25_adjacent_cfva_flag_name(0x1U));
    }
    return count;
}

static int
p25_pending_channel_valid(uint16_t channel) {
    return channel != 0xFFFFU;
}

static int
p25_sccb_matches_current_site(const dsd_state* state, uint8_t rfss, uint8_t site) {
    if (!state) {
        return 0;
    }
    if (state->p2_rfssid != 0 && rfss != (uint8_t)state->p2_rfssid) {
        return 0;
    }
    if (state->p2_siteid != 0 && site != (uint8_t)state->p2_siteid) {
        return 0;
    }
    return 1;
}

static void
p25_note_sccb_site(dsd_state* state, uint8_t rfss, uint8_t site) {
    if (!p25_sccb_matches_current_site(state, rfss, site)) {
        return;
    }
    if (state->p2_rfssid == 0) {
        state->p2_rfssid = rfss;
    }
    if (state->p2_siteid == 0) {
        state->p2_siteid = site;
    }
}

static int
p25_secondary_cc_select_insert_index(dsd_state* state) {
    int idx = state->p25_secondary_cc_count;
    if (idx < 0) {
        idx = 0;
    }
    if (idx < P25_SECONDARY_CC_MAX) {
        state->p25_secondary_cc_count = idx + 1;
        return idx;
    }

    int repl = 0;
    time_t oldest = state->p25_secondary_cc_entries[0].last_seen;
    for (int i = 1; i < P25_SECONDARY_CC_MAX; i++) {
        if (state->p25_secondary_cc_entries[i].last_seen < oldest) {
            oldest = state->p25_secondary_cc_entries[i].last_seen;
            repl = i;
        }
    }
    return repl;
}

static int
p25_secondary_cc_same_identity(const p25_secondary_cc_entry_t* entry, long freq, uint16_t channel, uint8_t rfss,
                               uint8_t site) {
    if (!entry || entry->freq == 0) {
        return 0;
    }
    if (entry->rfss != rfss || entry->site != site) {
        return 0;
    }
    return entry->channel == channel || entry->freq == freq;
}

static int
p25_secondary_cc_store(dsd_state* state, long freq, uint16_t channel, uint8_t rfss, uint8_t site, uint8_t ssc) {
    if (!state || freq <= 0 || !p25_pending_channel_valid(channel)) {
        return 0;
    }
    time_t now = time(NULL);
    int count = state->p25_secondary_cc_count;
    if (count < 0) {
        count = 0;
    }
    if (count > P25_SECONDARY_CC_MAX) {
        count = P25_SECONDARY_CC_MAX;
    }
    for (int i = 0; i < count; i++) {
        p25_secondary_cc_entry_t* entry = &state->p25_secondary_cc_entries[i];
        if (p25_secondary_cc_same_identity(entry, freq, channel, rfss, site)) {
            entry->freq = freq;
            entry->channel = channel;
            entry->ssc = ssc;
            entry->last_seen = now;
            return 1;
        }
    }

    int idx = p25_secondary_cc_select_insert_index(state);
    p25_secondary_cc_entry_t* entry = &state->p25_secondary_cc_entries[idx];
    DSD_MEMSET(entry, 0, sizeof(*entry));
    entry->freq = freq;
    entry->channel = channel;
    entry->rfss = rfss;
    entry->site = site;
    entry->ssc = ssc;
    entry->last_seen = now;
    return 1;
}

typedef struct {
    uint8_t kind;
    uint8_t ssc;
    p25_neighbor_channel_announcement_t announcement;
} p25_pending_update_t;

static int
p25_pending_same_identity(const p25_pending_announcement_t* entry, const p25_pending_update_t* update) {
    const p25_neighbor_channel_announcement_t* announcement = &update->announcement;
    if (!entry->populated || entry->kind != update->kind || entry->channel != announcement->channel
        || entry->sysid != announcement->sysid || entry->rfss != announcement->rfss
        || entry->site != announcement->site) {
        return 0;
    }
    if (entry->wacn_valid && announcement->wacn_valid) {
        return entry->wacn == p25_neighbor_wacn20(announcement);
    }
    return 1;
}

static p25_pending_announcement_t*
p25_pending_find(dsd_state* state, const p25_pending_update_t* update) {
    int count = state->p25_pending_announcement_count;
    if (count < 0) {
        count = 0;
    }
    if (count > P25_PENDING_ANNOUNCEMENT_MAX) {
        count = P25_PENDING_ANNOUNCEMENT_MAX;
    }
    for (int i = 0; i < count; i++) {
        p25_pending_announcement_t* entry = &state->p25_pending_announcements[i];
        if (p25_pending_same_identity(entry, update)) {
            return entry;
        }
    }
    return NULL;
}

static p25_pending_announcement_t*
p25_pending_select_slot(dsd_state* state) {
    int count = state->p25_pending_announcement_count;
    if (count < 0) {
        count = 0;
    }
    if (count < P25_PENDING_ANNOUNCEMENT_MAX) {
        state->p25_pending_announcement_count = count + 1;
        return &state->p25_pending_announcements[count];
    }

    int repl = 0;
    time_t oldest = state->p25_pending_announcements[0].last_seen;
    for (int i = 1; i < P25_PENDING_ANNOUNCEMENT_MAX; i++) {
        if (state->p25_pending_announcements[i].last_seen < oldest) {
            oldest = state->p25_pending_announcements[i].last_seen;
            repl = i;
        }
    }
    return &state->p25_pending_announcements[repl];
}

static void
p25_pending_apply_update(p25_pending_announcement_t* entry, const p25_pending_update_t* update, time_t now) {
    const p25_neighbor_channel_announcement_t* announcement = &update->announcement;
    entry->populated = 1;
    entry->kind = update->kind;
    entry->rfss = announcement->rfss;
    entry->site = announcement->site;
    entry->ssc = update->ssc;
    entry->sysid = announcement->sysid;
    entry->channel = announcement->channel;
    if (announcement->wacn_valid) {
        entry->wacn = p25_neighbor_wacn20(announcement);
        entry->wacn_valid = 1U;
    }
    if (announcement->lra_valid) {
        entry->lra = announcement->lra;
        entry->lra_valid = 1U;
    }
    if (announcement->cfva_valid) {
        entry->cfva = announcement->cfva;
        entry->cfva_valid = 1U;
    }
    entry->last_seen = now;
}

static void
p25_pending_store(dsd_state* state, const p25_pending_update_t* update) {
    if (!state || !update || !p25_pending_channel_valid(update->announcement.channel)) {
        return;
    }

    time_t now = time(NULL);
    p25_pending_announcement_t* entry = p25_pending_find(state, update);
    if (!entry) {
        entry = p25_pending_select_slot(state);
        DSD_MEMSET(entry, 0, sizeof(*entry));
    }

    p25_pending_apply_update(entry, update, now);
}

static int
p25_promote_secondary_cc_freq(const dsd_opts* opts, dsd_state* state, long freq, uint16_t channel, uint8_t rfss,
                              uint8_t site, uint8_t ssc) {
    if (!state || freq <= 0) {
        return 0;
    }
    if (!p25_sccb_matches_current_site(state, rfss, site)) {
        return 0;
    }

    (void)p25_secondary_cc_store(state, freq, channel, rfss, site, ssc);
    p25_cc_try_load_cache(opts, state);
    p25_cc_add_candidate(state, freq, 1);

    const long notify[1] = {freq};
    p25_cc_record_neighbor_frequencies(opts, state, notify, 1);
    p25_note_sccb_site(state, rfss, site);
    return 1;
}

int
p25_announce_neighbor_channel(const dsd_opts* opts, dsd_state* state,
                              const p25_neighbor_channel_announcement_t* announcement) {
    if (!state || !announcement || !p25_pending_channel_valid(announcement->channel)) {
        return 0;
    }

    long freq = process_channel_to_freq(opts, state, announcement->channel);
    if (freq > 0) {
        const p25_neighbor_record_update_t update = {.freq = freq, .announcement = *announcement};
        p25_nb_record_update(state, &update);
        return 1;
    }

    const p25_pending_update_t pending = {
        .kind = P25_PENDING_ANNOUNCEMENT_NEIGHBOR,
        .announcement = *announcement,
    };
    p25_pending_store(state, &pending);
    return 0;
}

int
p25_announce_secondary_cc_channel(const dsd_opts* opts, dsd_state* state, uint16_t channel, uint8_t rfss, uint8_t site,
                                  uint8_t ssc) {
    if (!state || !p25_pending_channel_valid(channel)) {
        return 0;
    }
    if (!p25_sccb_matches_current_site(state, rfss, site)) {
        return 0;
    }
    p25_note_sccb_site(state, rfss, site);

    long freq = process_channel_to_freq(opts, state, channel);
    if (freq > 0) {
        return p25_promote_secondary_cc_freq(opts, state, freq, channel, rfss, site, ssc);
    }

    const p25_pending_update_t pending = {
        .kind = P25_PENDING_ANNOUNCEMENT_SECONDARY_CC,
        .ssc = ssc,
        .announcement =
            {
                .channel = channel,
                .rfss = rfss,
                .site = site,
            },
    };
    p25_pending_store(state, &pending);
    return 0;
}

static p25_neighbor_channel_announcement_t
p25_pending_to_neighbor_announcement(const p25_pending_announcement_t* pending) {
    p25_neighbor_channel_announcement_t announcement = {
        .channel = pending->channel,
        .wacn = pending->wacn,
        .sysid = pending->sysid,
        .rfss = pending->rfss,
        .site = pending->site,
        .lra = pending->lra,
        .cfva = pending->cfva,
        .wacn_valid = pending->wacn_valid,
        .lra_valid = pending->lra_valid,
        .cfva_valid = pending->cfva_valid,
    };
    return announcement;
}

static int
p25_pending_try_resolve(const dsd_opts* opts, dsd_state* state, const p25_pending_announcement_t* pending) {
    if (!state || !pending || !pending->populated || !p25_pending_channel_valid(pending->channel)) {
        return 1;
    }

    if (pending->kind == P25_PENDING_ANNOUNCEMENT_SECONDARY_CC
        && !p25_sccb_matches_current_site(state, pending->rfss, pending->site)) {
        return 1;
    }

    long freq = process_channel_to_freq(opts, state, pending->channel);
    if (freq <= 0) {
        return 0;
    }

    if (pending->kind == P25_PENDING_ANNOUNCEMENT_NEIGHBOR) {
        const p25_neighbor_record_update_t update = {
            .freq = freq,
            .announcement = p25_pending_to_neighbor_announcement(pending),
        };
        p25_nb_record_update(state, &update);
        return 1;
    }
    if (pending->kind == P25_PENDING_ANNOUNCEMENT_SECONDARY_CC) {
        (void)p25_promote_secondary_cc_freq(opts, state, freq, pending->channel, pending->rfss, pending->site,
                                            pending->ssc);
        return 1;
    }
    return 1;
}

void
p25_resolve_pending_announcements(const dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return;
    }
    int count = state->p25_pending_announcement_count;
    if (count < 0) {
        count = 0;
    }
    if (count > P25_PENDING_ANNOUNCEMENT_MAX) {
        count = P25_PENDING_ANNOUNCEMENT_MAX;
    }

    int w = 0;
    for (int i = 0; i < count; i++) {
        p25_pending_announcement_t pending = state->p25_pending_announcements[i];
        if (!pending.populated) {
            continue;
        }
        if (!p25_pending_try_resolve(opts, state, &pending)) {
            if (w != i) {
                state->p25_pending_announcements[w] = pending;
            }
            w++;
        }
    }
    for (int i = w; i < count; i++) {
        DSD_MEMSET(&state->p25_pending_announcements[i], 0, sizeof(state->p25_pending_announcements[i]));
    }
    state->p25_pending_announcement_count = w;
}
