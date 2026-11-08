// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/scan_list_view.h>
#include <dsd-neo/app_control/scan_state_view.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/trunk_scan_lists.h>
#include <dsd-neo/engine/scan_entry.h>
#include <stddef.h>
#include <stdint.h>

_Static_assert(sizeof(((dsd_app_scan_list_entry*)0)->name) == sizeof(((dsd_trunk_scan_list_publication*)0)->name),
               "a list entry holds every published name");

/* List @p number as published, marked on air when it holds the target on air (@p on_air_lists, every list holding it;
   0 with none on air). */
static void
list_entry_fill(dsd_app_scan_list_entry* e, const dsd_trunk_scan_list_publication* src, uint8_t number,
                unsigned int on_air_lists) {
    e->number = number;
    e->enabled = src->enabled ? 1U : 0U;
    e->has_active = (uint8_t)((on_air_lists >> (number - 1U)) & 1U);
    e->targets = src->targets;
    DSD_MEMCPY(e->name, src->name, sizeof e->name);
    e->name[sizeof e->name - 1U] = '\0';
}

/* Every list of @p out (list_count and active_list set), the membership counts, and the lists on and their distinct
   targets. */
static void
scan_list_view_fill_lists(dsd_app_scan_list_view* out, const dsd_state* state) {
    /* Nothing on air (or a list past the count named): no list is marked. */
    const unsigned int on_air_lists = out->active_list != 0U ? state->trunk_scan_active_lists : 0U;
    unsigned int on_mask = 0U;
    for (uint8_t i = 0; i < out->list_count; i++) {
        dsd_app_scan_list_entry* e = &out->lists[i];
        list_entry_fill(e, &state->trunk_scan_lists[i], (uint8_t)(i + 1U), on_air_lists);
        if (e->enabled) {
            out->lists_on++;
            on_mask |= 1U << i;
        }
    }
    DSD_MEMCPY(out->membership_counts, state->trunk_scan_membership_counts, sizeof out->membership_counts);
    const unsigned int targets_on = dsd_app_scan_list_view_targets_on(out, on_mask);
    out->targets_on = (uint16_t)(targets_on < UINT16_MAX ? targets_on : UINT16_MAX);
}

int
dsd_app_scan_list_view_get(const dsd_opts* opts, const dsd_state* state, dsd_app_scan_list_view* out) {
    if (out) {
        DSD_MEMSET(out, 0, sizeof(*out));
    }
    if (!opts || !state || !out) {
        return -1;
    }
    dsd_app_scan_state_view scan;
    (void)dsd_app_scan_state_view_get(opts, state, &scan);
    if (scan.kind != (uint8_t)DSD_APP_SCAN_KIND_TRUNK) {
        return 0;
    }
    out->session = state->trunk_scan_session;
    out->lists_seq = state->trunk_scan_lists_seq;
    out->running = scan.running;
    out->paused = scan.paused;
    out->list_count = state->trunk_scan_list_count < DSD_TRUNK_SCAN_MAX_LISTS ? state->trunk_scan_list_count
                                                                              : (uint8_t)DSD_TRUNK_SCAN_MAX_LISTS;
    out->active_list = state->trunk_scan_active_list <= out->list_count ? state->trunk_scan_active_list : 0U;
    out->active_list_off = (out->active_list != 0U && state->trunk_scan_active_list_off) ? 1U : 0U;
    scan_list_view_fill_lists(out, state);
    out->toggleable = (out->running && !out->paused && out->list_count >= 2U) ? 1U : 0U;
    return 0;
}

/* The distinct targets of the lists of @p on_mask: every membership mask that meets it, each target counted once. */
static unsigned int
membership_total(const uint16_t* counts, unsigned int on_mask) {
    unsigned int total = 0U;
    for (unsigned int mask = 1U; mask <= (unsigned int)DSD_TRUNK_SCAN_MEMBERSHIP_MASKS; mask++) {
        total += (mask & on_mask) != 0U ? counts[mask - 1U] : 0U;
    }
    return total;
}

unsigned int
dsd_app_scan_lists_targets_on(const dsd_state* state, unsigned int on_mask) {
    return state != NULL ? membership_total(state->trunk_scan_membership_counts, on_mask) : 0U;
}

unsigned int
dsd_app_scan_list_view_targets_on(const dsd_app_scan_list_view* view, unsigned int on_mask) {
    return view != NULL ? membership_total(view->membership_counts, on_mask) : 0U;
}

/* The published entry reasons while the scan in force is a trunk scan, else none. */
static size_t
scan_entry_published_count(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || !state->trunk_scan_entry_reasons) {
        return 0U;
    }
    dsd_app_scan_state_view scan;
    (void)dsd_app_scan_state_view_get(opts, state, &scan);
    return scan.kind == (uint8_t)DSD_APP_SCAN_KIND_TRUNK ? state->trunk_scan_entry_reason_count : 0U;
}

int
dsd_app_scan_entry_summary_get(const dsd_opts* opts, const dsd_state* state, dsd_app_scan_entry_summary* out) {
    if (out) {
        DSD_MEMSET(out, 0, sizeof(*out));
    }
    if (!opts || !state || !out) {
        return -1;
    }
    out->count = scan_entry_published_count(opts, state);
    out->revision = state->trunk_scan_entry_rev;
    const int32_t on_air = state->trunk_scan_entry_on_air;
    out->on_air = on_air >= 0 && (size_t)on_air < out->count ? on_air : -1;
    return 0;
}

int
dsd_app_scan_entry_eligible(const dsd_opts* opts, const dsd_state* state, size_t index) {
    if (index >= scan_entry_published_count(opts, state)) {
        return -1;
    }
    return state->trunk_scan_entry_reasons[index] == (uint8_t)DSD_SCAN_ENTRY_OK ? 1 : 0;
}
