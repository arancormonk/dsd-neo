// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/scan_avoid_view.h>
#include <dsd-neo/app_control/scan_state_view.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "scan_avoid_view_internal.h"

/* The store's own fields: which store, its identity and revision, whether its copy is whole, and its exact count. */
static void
summary_fill_store(const dsd_state* state, uint8_t scan_kind, dsd_app_scan_avoid_summary* out) {
    if (scan_kind == (uint8_t)DSD_APP_SCAN_KIND_TRUNK) {
        out->kind = DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN;
        out->session = state->trunk_scan_session;
        out->revision = state->trunk_scan_avoid_rev;
        out->available = (state->scan_avoids_complete && state->trunk_scan_avoids_ok) ? 1U : 0U;
        out->total = out->available ? state->trunk_scan_avoid_count : 0U;
        return;
    }
    if (scan_kind == (uint8_t)DSD_APP_SCAN_KIND_CHANNEL || state->lcn_freq_count > 0) {
        /* -Y running or paused, or a list -Y left behind (-T's control-channel hunts still skip its avoided rows). */
        out->kind = DSD_APP_SCAN_AVOID_KIND_LCN;
        out->list_rev = state->lcn_list_rev;
        out->revision = state->lcn_avoid_rev;
        out->available = state->scan_avoids_complete ? 1U : 0U;
        out->total = out->available ? state->lcn_avoid_total : 0U;
        return;
    }
    out->kind = DSD_APP_SCAN_AVOID_KIND_NONE;
    out->available = 1U;
}

int
dsd_app_scan_avoid_summary_get(const dsd_opts* opts, const dsd_state* state, dsd_app_scan_avoid_summary* out) {
    if (out) {
        DSD_MEMSET(out, 0, sizeof(*out));
    }
    if (!opts || !state || !out) {
        return -1;
    }
    dsd_app_scan_state_view scan;
    (void)dsd_app_scan_state_view_get(opts, state, &scan);
    out->instance = state->state_instance;
    out->can_avoid = scan.running;
    summary_fill_store(state, scan.kind, out);
    out->total_known = out->available;
    const int manage = out->can_avoid || out->total > 0U || !out->available;
    out->can_manage = (out->kind != DSD_APP_SCAN_AVOID_KIND_NONE && manage) ? 1U : 0U;
    return 0;
}

int
dsd_app_scan_avoid_summary_same_key(const dsd_app_scan_avoid_summary* a, const dsd_app_scan_avoid_summary* b) {
    return a && b && a->kind == b->kind && a->instance == b->instance && a->revision == b->revision
           && a->available == b->available;
}

uint64_t
dsd_app_scan_avoid_lcn_row_hz(const dsd_state* state, int row) {
    if (!state || row < 0 || row >= state->lcn_freq_count) {
        return 0U;
    }
    /* A row past the embedded slots reads the heap tail only where the tail reaches (a snapshot holds what it copied). */
    if (row >= DSD_TRUNK_LCN_EMBEDDED
        && (!state->trunk_lcn_freq_ext
            || (size_t)(row - DSD_TRUNK_LCN_EMBEDDED) >= state->trunk_lcn_freq_ext_capacity)) {
        return 0U;
    }
    const long hz = *dsd_state_trunk_lcn_slot_const(state, row);
    return hz > 0 ? (uint64_t)hz : 0U;
}

int
dsd_app_scan_avoid_lcn_match(const dsd_state* state, const dsd_app_scan_avoid_remove_payload* id) {
    if (!state || !id || id->kind != DSD_APP_SCAN_AVOID_KIND_LCN || id->row < 0 || id->row >= state->lcn_freq_count) {
        return DSD_APP_SCAN_AVOID_MATCH_STALE;
    }
    if (id->instance != state->state_instance || id->list_rev != state->lcn_list_rev
        || id->frequency_hz != dsd_app_scan_avoid_lcn_row_hz(state, id->row)) {
        return DSD_APP_SCAN_AVOID_MATCH_STALE;
    }
    return dsd_state_trunk_lcn_avoid_get(state, (size_t)id->row) ? DSD_APP_SCAN_AVOID_MATCH_AVOIDED
                                                                 : DSD_APP_SCAN_AVOID_MATCH_NOT_AVOIDED;
}

/* The avoided rows within lcn_freq_count, ascending; at most out->summary.total of them (the flags past the store or
   the list are not avoids). */
static int
raw_collect_lcn(const dsd_state* state, dsd_app_scan_avoid_raw* out) {
    const size_t total = out->summary.total;
    if (total == 0U) {
        return 0;
    }
    out->entries = (dsd_app_scan_avoid_raw_entry*)calloc(total, sizeof(dsd_app_scan_avoid_raw_entry));
    if (!out->entries) {
        return -1;
    }
    size_t rows = state->lcn_freq_count > 0 ? (size_t)state->lcn_freq_count : 0U;
    if (rows > state->trunk_lcn_avoid_capacity) {
        rows = state->trunk_lcn_avoid_capacity;
    }
    for (size_t row = 0; row < rows && out->count < total; row++) {
        if (!dsd_state_trunk_lcn_avoid_get(state, row)) {
            continue;
        }
        dsd_app_scan_avoid_raw_entry* e = &out->entries[out->count++];
        e->identity.kind = DSD_APP_SCAN_AVOID_KIND_LCN;
        e->identity.instance = out->summary.instance;
        e->identity.list_rev = out->summary.list_rev;
        e->identity.row = (int32_t)row;
        e->identity.frequency_hz = dsd_app_scan_avoid_lcn_row_hz(state, (int)row);
        DSD_SNPRINTF(e->name, sizeof e->name, "%s", dsd_state_trunk_lcn_name_get(state, row));
    }
    return 0;
}

static int
raw_collect_trunk_scan(const dsd_state* state, dsd_app_scan_avoid_raw* out) {
    const size_t total = out->summary.total;
    if (total == 0U) {
        return 0;
    }
    if (!state->trunk_scan_avoids) {
        return -1;
    }
    out->entries = (dsd_app_scan_avoid_raw_entry*)calloc(total, sizeof(dsd_app_scan_avoid_raw_entry));
    if (!out->entries) {
        return -1;
    }
    for (size_t i = 0; i < total; i++) {
        const dsd_trunk_scan_avoid_entry* src = &state->trunk_scan_avoids[i];
        dsd_app_scan_avoid_raw_entry* e = &out->entries[i];
        e->identity.kind = DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN;
        e->identity.session = out->summary.session;
        e->identity.frequency_hz = src->frequency_hz;
        e->identity.row = -1;
        DSD_SNPRINTF(e->identity.target_id, sizeof e->identity.target_id, "%s", src->id);
        e->ordinal = src->ordinal;
        DSD_SNPRINTF(e->name, sizeof e->name, "%s", src->label);
    }
    out->count = total;
    return 0;
}

int
dsd_app_scan_avoid_raw_collect(const dsd_opts* opts, const dsd_state* state, dsd_app_scan_avoid_raw* out) {
    if (out) {
        DSD_MEMSET(out, 0, sizeof(*out));
    }
    if (dsd_app_scan_avoid_summary_get(opts, state, out ? &out->summary : NULL) != 0) {
        return -1;
    }
    if (!out->summary.available) {
        return 0;
    }
    int rc = 0;
    if (out->summary.kind == DSD_APP_SCAN_AVOID_KIND_LCN) {
        rc = raw_collect_lcn(state, out);
    } else if (out->summary.kind == DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN) {
        rc = raw_collect_trunk_scan(state, out);
    }
    if (rc != 0) {
        free(out->entries);
        out->entries = NULL;
        out->count = 0U;
        return rc;
    }
    /* What was found, not what the store counted: a writer that shrank lcn_freq_count without a recount leaves an
       avoided row past the list, which the walk skips, and a total that still counted it would name an entry no page
       or list can reach. */
    out->summary.total = out->count;
    return 0;
}

void
dsd_app_scan_avoid_raw_free(dsd_app_scan_avoid_raw* raw) {
    if (!raw) {
        return;
    }
    free(raw->entries);
    DSD_MEMSET(raw, 0, sizeof(*raw));
}

/* "851.012500 MHz" through the tune toasts' formatter (issue #670), or "frequency unknown" for a 0 Hz placeholder. */
static void
label_frequency(uint64_t hz, char* out, size_t out_size) {
    if (hz == 0U) {
        DSD_SNPRINTF(out, out_size, "%s", "frequency unknown");
        return;
    }
    char mhz[32];
    if (hz <= UINT32_MAX) {
        (void)dsd_freq_hz_mhz_text((uint32_t)hz, mhz, sizeof mhz);
    } else {
        DSD_SNPRINTF(mhz, sizeof mhz, "%" PRIu64 ".%06" PRIu64, hz / 1000000U, hz % 1000000U);
    }
    DSD_SNPRINTF(out, out_size, "%s MHz", mhz);
}

int
dsd_app_scan_avoid_raw_label(const dsd_app_scan_avoid_raw_entry* entry, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    if (!entry) {
        return -1;
    }
    char freq[48];
    label_frequency(entry->identity.frequency_hz, freq, sizeof freq);
    const char* sep = entry->name[0] ? "  " : "";
    if (entry->identity.kind == DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN) {
        DSD_SNPRINTF(out, out_size, "#%u  %s  %s%s%s", (unsigned)entry->ordinal, entry->identity.target_id, freq, sep,
                     entry->name);
    } else {
        DSD_SNPRINTF(out, out_size, "Row %ld  %s%s%s", (long)entry->identity.row + 1L, freq, sep, entry->name);
    }
    return 0;
}

int
dsd_app_scan_avoid_list_build(const dsd_opts* opts, const dsd_state* state, dsd_app_scan_avoid_list* out) {
    if (out) {
        DSD_MEMSET(out, 0, sizeof(*out));
    }
    if (!out) {
        return -1;
    }
    dsd_app_scan_avoid_raw raw;
    const int rc = dsd_app_scan_avoid_raw_collect(opts, state, &raw);
    out->summary = raw.summary;
    if (rc != 0 || raw.count == 0U) {
        dsd_app_scan_avoid_raw_free(&raw);
        return rc;
    }
    out->entries = (dsd_app_scan_avoid_entry*)calloc(raw.count, sizeof(dsd_app_scan_avoid_entry));
    if (!out->entries) {
        dsd_app_scan_avoid_raw_free(&raw);
        return -1;
    }
    for (size_t i = 0; i < raw.count; i++) {
        out->entries[i].identity = raw.entries[i].identity;
        (void)dsd_app_scan_avoid_raw_label(&raw.entries[i], out->entries[i].label, sizeof out->entries[i].label);
    }
    out->count = raw.count;
    dsd_app_scan_avoid_raw_free(&raw);
    return 0;
}

void
dsd_app_scan_avoid_list_free(dsd_app_scan_avoid_list* list) {
    if (!list) {
        return;
    }
    free(list->entries);
    DSD_MEMSET(list, 0, sizeof(*list));
}
