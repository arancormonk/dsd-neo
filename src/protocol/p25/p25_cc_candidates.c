// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Control-channel candidate cache and neighbor helpers.
 * Keeps this logic separate from the trunking state machine so it can be
 * reused by tests and UI code without pulling in tuning policy.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
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
    return dsd_trunk_cc_candidates_add(state, freq_hz, bump_added);
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

    struct stat st;
    if (stat(path, &st) != 0) {
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

void
p25_cc_try_load_cache(const dsd_opts* opts, dsd_state* state) {
    if (!state || state->p25_cc_cache_loaded) {
        return;
    }
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int enable = cfg ? cfg->cc_cache_enable : 1;
    if (!enable) {
        state->p25_cc_cache_loaded = 1;
        return;
    }

    char fpath[1024];
    if (!p25_cc_build_cache_path(state, fpath, sizeof(fpath))) {
        return;
    }
    FILE* fp = fopen(fpath, "r");
    if (!fp) {
        state->p25_cc_cache_loaded = 1;
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char* end = NULL;
        long f = strtol(line, &end, 10);
        if (end == line) {
            continue;
        }
        (void)p25_cc_add_candidate(state, f, 0);
    }
    fclose(fp);
    state->p25_cc_cache_loaded = 1;

    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    const int count = (cc && cc->count > 0 && cc->count <= DSD_TRUNK_CC_CANDIDATES_MAX) ? cc->count : 0;
    if (opts && opts->verbose > 0 && count > 0) {
        DSD_FPRINTF(stderr, "\n  P25 SM: Loaded %d CC candidates from cache\n", count);
    }
}

void
p25_cc_persist_cache(const dsd_opts* opts, const dsd_state* state) {
    if (!state) {
        return;
    }
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int enable = cfg ? cfg->cc_cache_enable : 1;
    if (!enable) {
        return;
    }

    char fpath[1024];
    if (!p25_cc_build_cache_path(state, fpath, sizeof(fpath))) {
        return;
    }
    FILE* fp = fopen(fpath, "w");
    if (!fp) {
        if (opts && opts->verbose > 1) {
            DSD_FPRINTF(stderr, "\n  P25 SM: Failed to open CC cache for write: %s (errno=%d)\n", fpath, errno);
        }
        return;
    }

    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    const int count = (cc && cc->count > 0 && cc->count <= DSD_TRUNK_CC_CANDIDATES_MAX) ? cc->count : 0;
    for (int i = 0; i < count; i++) {
        if (cc->candidates[i] != 0) {
            DSD_FPRINTF(fp, "%ld\n", cc->candidates[i]);
        }
    }
    fclose(fp);
}

#define P25_NB_TTL_SEC ((time_t)30 * 60)

static int
p25_nb_has_site_identity(uint16_t sysid, uint8_t rfss, uint8_t site) {
    return sysid != 0 || rfss != 0 || site != 0;
}

static int
p25_nb_update_by_site(dsd_state* state, long freq, uint16_t sysid, uint8_t rfss, uint8_t site, uint8_t cfva,
                      time_t now) {
    for (int i = 0; i < state->p25_nb_count && i < P25_NB_MAX; i++) {
        p25_nb_entry_t* entry = &state->p25_nb_entries[i];
        if (entry->sysid == sysid && entry->rfss == rfss && entry->site == site) {
            entry->freq = freq;
            entry->cfva = cfva;
            entry->last_seen = now;
            return 1;
        }
    }
    return 0;
}

static int
p25_nb_update_by_freq(dsd_state* state, long freq, uint16_t sysid, uint8_t rfss, uint8_t site, uint8_t cfva,
                      int has_site_identity, time_t now) {
    for (int i = 0; i < state->p25_nb_count && i < P25_NB_MAX; i++) {
        p25_nb_entry_t* entry = &state->p25_nb_entries[i];
        if (entry->freq != freq) {
            continue;
        }
        if (has_site_identity && (entry->sysid != 0 || entry->rfss != 0 || entry->site != 0)) {
            continue;
        }
        if (has_site_identity) {
            entry->sysid = sysid;
            entry->rfss = rfss;
            entry->site = site;
            entry->cfva = cfva;
        }
        entry->last_seen = now;
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

void
p25_nb_add_ex(dsd_state* state, long freq, uint16_t sysid, uint8_t rfss, uint8_t site, uint8_t cfva) {
    if (!state || freq <= 0) {
        return;
    }
    if (freq == state->p25_cc_freq) {
        return; /* Reject current CC as neighbor. */
    }

    int has_site_identity = p25_nb_has_site_identity(sysid, rfss, site);
    time_t now = time(NULL);

    /* Structured neighbor broadcasts identify sites. Keep those entries keyed
     * by site identity so frequency reuse does not merge distinct neighbors. */
    if (has_site_identity && p25_nb_update_by_site(state, freq, sysid, rfss, site, cfva, now)) {
        return;
    }

    /* Legacy frequency-only updates refresh by frequency without clobbering
     * any site metadata learned from structured broadcasts. */
    if (p25_nb_update_by_freq(state, freq, sysid, rfss, site, cfva, has_site_identity, now)) {
        return;
    }

    int idx = p25_nb_select_insert_index(state);
    state->p25_nb_entries[idx].freq = freq;
    state->p25_nb_entries[idx].sysid = sysid;
    state->p25_nb_entries[idx].rfss = rfss;
    state->p25_nb_entries[idx].site = site;
    state->p25_nb_entries[idx].cfva = cfva;
    state->p25_nb_entries[idx].last_seen = now;
}

void
p25_nb_add(dsd_state* state, long freq_hz) {
    p25_nb_add_ex(state, freq_hz, 0, 0, 0, 0);
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
