// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/scan_state_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/tuner_owner.h>
#include <stdint.h>

/* A row count the published int cannot hold is clamped, never wrapped into a small one. */
static uint16_t
scan_state_row_count(int rows) {
    if (rows <= 0) {
        return 0U;
    }
    return rows > (int)UINT16_MAX ? (uint16_t)UINT16_MAX : (uint16_t)rows;
}

int
dsd_app_scan_state_view_get(const dsd_opts* opts, const dsd_state* state, dsd_app_scan_state_view* out) {
    if (out) {
        DSD_MEMSET(out, 0, sizeof(*out));
    }
    if (!opts || !state || !out) {
        return -1;
    }
    out->operator_tuned = state->operator_tuned ? 1U : 0U;
    if (opts->trunk_scan_enabled == 1) {
        out->kind = (uint8_t)DSD_APP_SCAN_KIND_TRUNK;
        out->running = dsd_trunk_scan_owns_tuner(opts, state) ? 1U : 0U;
        out->paused = out->running ? 0U : 1U;
        out->target_count = state->trunk_scan_target_count;
        return 0;
    }
    /* A resume waiting on its row keeps lcn_scan_paused set (and scanner_mode 0) until the row lands. */
    if (opts->scanner_mode == 1 || state->lcn_scan_paused) {
        out->kind = (uint8_t)DSD_APP_SCAN_KIND_CHANNEL;
        out->running = opts->scanner_mode == 1 ? 1U : 0U;
        out->paused = out->running ? 0U : 1U;
        out->resume_pending = (out->paused && state->lcn_scan_resume_pending) ? 1U : 0U;
        out->target_count = scan_state_row_count(state->lcn_freq_count);
    }
    return 0;
}

const char*
dsd_app_scan_kind_name(uint8_t kind) {
    switch (kind) {
        case DSD_APP_SCAN_KIND_CHANNEL: return "channel";
        case DSD_APP_SCAN_KIND_TRUNK: return "trunk";
        default: return "none";
    }
}

const char*
dsd_app_scan_state_name(const dsd_app_scan_state_view* view) {
    if (!view || view->kind == (uint8_t)DSD_APP_SCAN_KIND_NONE) {
        return "off";
    }
    return view->running ? "running" : "paused";
}
