// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "p25_site_snapshot.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * Grow the sparse channel-map arrays to hold at least @p needed entries, doubling from 64. Both arrays are grown
 * together so an index is valid in either or in neither; on failure the snapshot keeps the arrays it already had.
 */
static int
p25_site_snapshot_chan_map_reserve(dsd_p25_site_snapshot* snapshot, size_t needed) {
    if (needed <= snapshot->chan_map_capacity) {
        return 0;
    }
    size_t capacity = snapshot->chan_map_capacity > 0 ? snapshot->chan_map_capacity : 64;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(long int)) {
        return -1;
    }
    uint16_t* chans = (uint16_t*)realloc(snapshot->chan_map_chan, capacity * sizeof *chans);
    if (!chans) {
        return -1;
    }
    snapshot->chan_map_chan = chans;
    long int* freqs = (long int*)realloc(snapshot->chan_map_freq, capacity * sizeof *freqs);
    if (!freqs) {
        /* chan_map_chan already grew; capacity stays at the old value so the two arrays are
         * never both indexed past the shorter one. */
        return -1;
    }
    snapshot->chan_map_freq = freqs;
    snapshot->chan_map_capacity = capacity;
    return 0;
}

/*
 * Capture state's channel map as (channel, frequency) pairs. state->trunk_chan_map_used[] is
 * already the exact sorted set of mapped channels, so this walks it directly rather than
 * scanning 64K slots.
 */
static int
p25_site_snapshot_save_chan_map(const dsd_state* state, dsd_p25_site_snapshot* snapshot) {
    int rc = 0;
    uint32_t used = state->trunk_chan_map_used_count;
    if (used > (uint32_t)DSD_TRUNK_CHAN_MAP_SIZE) {
        used = (uint32_t)DSD_TRUNK_CHAN_MAP_SIZE;
    }
    if (used > 0 && p25_site_snapshot_chan_map_reserve(snapshot, (size_t)used) != 0) {
        used = 0;
        rc = -1;
    }
    for (uint32_t i = 0; i < used; i++) {
        const uint16_t chan = state->trunk_chan_map_used[i];
        snapshot->chan_map_chan[i] = chan;
        snapshot->chan_map_freq[i] = state->trunk_chan_map[chan];
    }
    snapshot->trunk_chan_map_used_count = used;
    snapshot->trunk_chan_map_seq = state->trunk_chan_map_seq;
    return rc;
}

/*
 * Reinstate a saved channel map. Only the mapped channels on the state are cleared - a
 * DSD_MEMSET of the whole 64K-entry map would cost half a megabyte on every restore.
 */
static void
p25_site_snapshot_restore_chan_map(dsd_state* state, const dsd_p25_site_snapshot* snapshot) {
    uint32_t stale = state->trunk_chan_map_used_count;
    if (stale > (uint32_t)DSD_TRUNK_CHAN_MAP_SIZE) {
        stale = (uint32_t)DSD_TRUNK_CHAN_MAP_SIZE;
    }
    for (uint32_t i = 0; i < stale; i++) {
        state->trunk_chan_map[state->trunk_chan_map_used[i]] = 0;
    }

    /* Bounded by all three of: what the snapshot claims, what it actually allocated, and what
     * state->trunk_chan_map_used[] can hold - this loop writes into that fixed-size array. */
    size_t used = snapshot->trunk_chan_map_used_count;
    if (!snapshot->chan_map_chan || !snapshot->chan_map_freq) {
        used = 0;
    }
    if (used > snapshot->chan_map_capacity) {
        used = snapshot->chan_map_capacity;
    }
    if (used > (size_t)DSD_TRUNK_CHAN_MAP_SIZE) {
        used = (size_t)DSD_TRUNK_CHAN_MAP_SIZE;
    }
    for (size_t i = 0; i < used; i++) {
        const uint16_t chan = snapshot->chan_map_chan[i];
        state->trunk_chan_map[chan] = snapshot->chan_map_freq[i];
        state->trunk_chan_map_used[i] = chan;
    }
    state->trunk_chan_map_used_count = (uint32_t)used;
    state->trunk_chan_map_seq = snapshot->trunk_chan_map_seq;
}

static void
p25_site_snapshot_save_broadcasts(const dsd_state* state, dsd_p25_site_snapshot* snapshot) {
    snapshot->p25_pending_announcement_count = state->p25_pending_announcement_count;
    DSD_MEMCPY(snapshot->p25_pending_announcements, state->p25_pending_announcements,
               sizeof(snapshot->p25_pending_announcements));
    snapshot->p25_secondary_cc_count = state->p25_secondary_cc_count;
    DSD_MEMCPY(snapshot->p25_secondary_cc_entries, state->p25_secondary_cc_entries,
               sizeof(snapshot->p25_secondary_cc_entries));
    snapshot->p25_sys_services_valid = state->p25_sys_services_valid;
    snapshot->p25_sys_services_available = state->p25_sys_services_available;
    snapshot->p25_sys_services_supported = state->p25_sys_services_supported;
    snapshot->p25_sys_services_request_priority = state->p25_sys_services_request_priority;
    snapshot->p25_cc_prot_valid = state->p25_cc_prot_valid;
    snapshot->p25_cc_prot_algid = state->p25_cc_prot_algid;
    snapshot->p25_site_lra_valid = state->p25_site_lra_valid;
    snapshot->p25_site_lra = state->p25_site_lra;
    snapshot->p25_site_network_active_valid = state->p25_site_network_active_valid;
    snapshot->p25_site_network_active = state->p25_site_network_active;
}

static void
p25_site_snapshot_restore_broadcasts(dsd_state* state, const dsd_p25_site_snapshot* snapshot) {
    state->p25_pending_announcement_count = snapshot->p25_pending_announcement_count;
    DSD_MEMCPY(state->p25_pending_announcements, snapshot->p25_pending_announcements,
               sizeof(state->p25_pending_announcements));
    state->p25_secondary_cc_count = snapshot->p25_secondary_cc_count;
    DSD_MEMCPY(state->p25_secondary_cc_entries, snapshot->p25_secondary_cc_entries,
               sizeof(state->p25_secondary_cc_entries));
    state->p25_sys_services_valid = snapshot->p25_sys_services_valid;
    state->p25_sys_services_available = snapshot->p25_sys_services_available;
    state->p25_sys_services_supported = snapshot->p25_sys_services_supported;
    state->p25_sys_services_request_priority = snapshot->p25_sys_services_request_priority;
    state->p25_cc_prot_valid = snapshot->p25_cc_prot_valid;
    state->p25_cc_prot_algid = snapshot->p25_cc_prot_algid;
    state->p25_site_lra_valid = snapshot->p25_site_lra_valid;
    state->p25_site_lra = snapshot->p25_site_lra;
    state->p25_site_network_active_valid = snapshot->p25_site_network_active_valid;
    state->p25_site_network_active = snapshot->p25_site_network_active;
}

int
dsd_p25_site_snapshot_save(const dsd_state* state, dsd_p25_site_snapshot* snapshot) {
    if (!state || !snapshot) {
        return -1;
    }
    snapshot->p2_wacn = state->p2_wacn;
    snapshot->p2_sysid = state->p2_sysid;
    snapshot->p2_cc = state->p2_cc;
    snapshot->p2_siteid = state->p2_siteid;
    snapshot->p2_rfssid = state->p2_rfssid;
    snapshot->p2_hardset = state->p2_hardset;
    snapshot->p25_cc_freq = state->p25_cc_freq;
    snapshot->trunk_cc_freq = state->trunk_cc_freq;
    snapshot->p25_cc_is_tdma = state->p25_cc_is_tdma;
    snapshot->p25_sys_is_tdma = state->p25_sys_is_tdma;
    snapshot->p25_chan_iden = state->p25_chan_iden;
    DSD_MEMCPY(snapshot->p25_chan_tdma_explicit, state->p25_chan_tdma_explicit,
               sizeof(snapshot->p25_chan_tdma_explicit));
    DSD_MEMCPY(snapshot->p25_iden_fdma, state->p25_iden_fdma, sizeof(snapshot->p25_iden_fdma));
    DSD_MEMCPY(snapshot->p25_iden_tdma, state->p25_iden_tdma, sizeof(snapshot->p25_iden_tdma));
    DSD_MEMCPY(snapshot->p25_bandplan_rows, state->p25_bandplan_rows, sizeof(snapshot->p25_bandplan_rows));
    snapshot->p25_bandplan_row_count = state->p25_bandplan_row_count;
    p25_site_snapshot_save_broadcasts(state, snapshot);
    return p25_site_snapshot_save_chan_map(state, snapshot);
}

void
dsd_p25_site_snapshot_restore(dsd_state* state, const dsd_p25_site_snapshot* snapshot) {
    if (!state || !snapshot) {
        return;
    }
    state->p2_wacn = snapshot->p2_wacn;
    state->p2_sysid = snapshot->p2_sysid;
    state->p2_cc = snapshot->p2_cc;
    state->p2_siteid = snapshot->p2_siteid;
    state->p2_rfssid = snapshot->p2_rfssid;
    state->p2_hardset = snapshot->p2_hardset;
    state->p25_cc_freq = snapshot->p25_cc_freq;
    state->trunk_cc_freq = snapshot->trunk_cc_freq;
    state->p25_cc_is_tdma = snapshot->p25_cc_is_tdma;
    state->p25_sys_is_tdma = snapshot->p25_sys_is_tdma;
    state->p25_chan_iden = snapshot->p25_chan_iden;
    DSD_MEMCPY(state->p25_chan_tdma_explicit, snapshot->p25_chan_tdma_explicit, sizeof(state->p25_chan_tdma_explicit));
    DSD_MEMCPY(state->p25_iden_fdma, snapshot->p25_iden_fdma, sizeof(state->p25_iden_fdma));
    DSD_MEMCPY(state->p25_iden_tdma, snapshot->p25_iden_tdma, sizeof(state->p25_iden_tdma));
    DSD_MEMCPY(state->p25_bandplan_rows, snapshot->p25_bandplan_rows, sizeof(state->p25_bandplan_rows));
    state->p25_bandplan_row_count = snapshot->p25_bandplan_row_count;
    p25_site_snapshot_restore_broadcasts(state, snapshot);
    p25_site_snapshot_restore_chan_map(state, snapshot);
}

void
dsd_p25_site_snapshot_clear(dsd_p25_site_snapshot* snapshot) {
    if (!snapshot) {
        return;
    }
    free(snapshot->chan_map_chan);
    free(snapshot->chan_map_freq);
    DSD_MEMSET(snapshot, 0, sizeof(*snapshot));
    snapshot->p25_cc_is_tdma = 2; /* unknown, as initState() leaves it */
}

/* chan_map_chan[] is kept in the ascending order state->trunk_chan_map_used[] maintains, so this binary-searches it. */
long int
dsd_p25_site_snapshot_chan_freq(const dsd_p25_site_snapshot* snapshot, uint16_t channel) {
    if (!snapshot || !snapshot->chan_map_chan || !snapshot->chan_map_freq) {
        return 0;
    }
    size_t lo = 0;
    size_t hi = snapshot->trunk_chan_map_used_count;
    if (hi > snapshot->chan_map_capacity) {
        hi = snapshot->chan_map_capacity;
    }
    while (lo < hi) {
        const size_t mid = lo + ((hi - lo) / 2U);
        const uint16_t at = snapshot->chan_map_chan[mid];
        if (at == channel) {
            return snapshot->chan_map_freq[mid];
        }
        if (at < channel) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return 0;
}
