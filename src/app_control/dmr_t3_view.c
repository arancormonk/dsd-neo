// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The frontends' read side of the DMR Tier III channel record (<dsd-neo/app_control/dmr_t3_view.h>): the snapshot's
 * slot and the ledger queries. The copies that fill it are dmr_t3_view_copy.c's.
 */

#include <dsd-neo/app_control/dmr_t3_view.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <stddef.h>
#include <stdint.h>

const dsd_ui_dmr_t3_view*
dsd_app_dmr_t3_view(const dsd_state* snapshot) {
    return snapshot != NULL ? (const dsd_ui_dmr_t3_view*)snapshot->state_ext[DSD_STATE_EXT_APP_DMR_T3_VIEW] : NULL;
}

int
dsd_app_dmr_t3_view_missing_has(const dsd_ui_dmr_t3_view* view, uint16_t lpcn) {
    if (view == NULL || lpcn == 0U || lpcn >= (uint16_t)DSD_UI_DMR_T3_ROWS) {
        return 0;
    }
    return (view->missing.missing[lpcn >> 3] >> (lpcn & 7U)) & 1U;
}

size_t
dsd_app_dmr_t3_view_missing_count(const dsd_ui_dmr_t3_view* view) {
    if (view == NULL) {
        return 0U;
    }
    size_t n = 0U;
    for (size_t i = 0; i < (size_t)DSD_UI_DMR_T3_SET_BYTES; i++) {
        for (uint8_t b = view->missing.missing[i]; b != 0U; b &= (uint8_t)(b - 1U)) {
            n++;
        }
    }
    return n;
}

int
dsd_app_dmr_t3_view_shows_lpcns(const dsd_ui_dmr_t3_view* view) {
    if (view == NULL || view->present == 0U) {
        return 0;
    }
    return view->tier3 != 0U || view->overlay_count != 0U || dsd_app_dmr_t3_view_missing_count(view) != 0U;
}
