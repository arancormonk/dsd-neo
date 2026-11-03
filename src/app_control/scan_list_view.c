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
#include <stdint.h>

_Static_assert(sizeof(((dsd_app_scan_list_entry*)0)->name) == sizeof(((dsd_trunk_scan_list_publication*)0)->name),
               "a list entry holds every published name");

/* List @p number as published, named on air when the target on air is its. */
static void
list_entry_fill(dsd_app_scan_list_entry* e, const dsd_trunk_scan_list_publication* src, uint8_t number,
                uint8_t active_list) {
    e->number = number;
    e->enabled = src->enabled ? 1U : 0U;
    e->has_active = active_list == number ? 1U : 0U;
    e->targets = src->targets;
    DSD_MEMCPY(e->name, src->name, sizeof e->name);
    e->name[sizeof e->name - 1U] = '\0';
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
    for (uint8_t i = 0; i < out->list_count; i++) {
        dsd_app_scan_list_entry* e = &out->lists[i];
        list_entry_fill(e, &state->trunk_scan_lists[i], (uint8_t)(i + 1U), out->active_list);
        if (e->enabled) {
            out->lists_on++;
            out->targets_on = (uint16_t)(out->targets_on + e->targets);
        }
    }
    out->toggleable = (out->running && !out->paused && out->list_count >= 2U) ? 1U : 0U;
    return 0;
}
