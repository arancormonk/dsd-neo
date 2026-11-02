// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The decoder side of the scan avoid view (issue #672): what DSD_APP_CMD_SCAN_AVOID_REMOVE checks an LCN identity
   against, read the same way the view reads it when it lists the row. */

#ifndef DSD_NEO_SRC_APP_CONTROL_SCAN_AVOID_VIEW_INTERNAL_H_
#define DSD_NEO_SRC_APP_CONTROL_SCAN_AVOID_VIEW_INTERNAL_H_

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What an LCN identity names in @p state. */
enum {
    DSD_APP_SCAN_AVOID_MATCH_AVOIDED = 0,     /**< this instance, list, row and frequency, and the row is avoided */
    DSD_APP_SCAN_AVOID_MATCH_STALE = 1,       /**< another kind, instance, list, row or frequency: names nothing here */
    DSD_APP_SCAN_AVOID_MATCH_NOT_AVOIDED = 2, /**< the row it names is not avoided (already put back) */
};

/** The frequency of LCN row @p row as an identity carries it: Hz, 0 for a placeholder, a row outside the list or a
 * tail the state does not hold. */
uint64_t dsd_app_scan_avoid_lcn_row_hz(const dsd_state* state, int row);

/** Check the LCN identity @p id against @p state; returns a DSD_APP_SCAN_AVOID_MATCH_*. The row is range-checked
 * before it indexes anything. */
int dsd_app_scan_avoid_lcn_match(const dsd_state* state, const dsd_app_scan_avoid_remove_payload* id);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_APP_CONTROL_SCAN_AVOID_VIEW_INTERNAL_H_ */
