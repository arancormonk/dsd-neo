// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/squelch_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stddef.h>

int
dsd_app_squelch_view_get(const dsd_opts* opts, const dsd_state* state, dsd_app_squelch_view* out) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!opts || !state) {
        return -1;
    }
    const dsd_scan_option_values* row = dsd_scan_mode_row_options(state);
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(state);
    out->row_override = (row && (row->present & DSD_SCAN_OPT_SQUELCH)) ? 1U : 0U;
    /* Outside a scope, and while one is suspended, dsd_opts holds the configured default. */
    out->configured_level = configured ? configured->rtl_squelch_level : opts->rtl_squelch_level;
    /* The row's own value, not dsd_opts: suspended for a command, dsd_opts reads the default. */
    out->effective_level =
        out->row_override ? dsd_squelch_level_from_sql((double)row->squelch_db) : opts->rtl_squelch_level;
    return 0;
}

int
dsd_app_squelch_view_format(const dsd_app_squelch_view* view, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    if (!view) {
        return -1;
    }
    char effective[24];
    (void)dsd_squelch_format(view->effective_level, " dB", effective, sizeof effective);
    if (!view->row_override) {
        DSD_SNPRINTF(out, out_size, "%s", effective);
        return 0;
    }
    char configured[24];
    (void)dsd_squelch_format(view->configured_level, " dB", configured, sizeof configured);
    DSD_SNPRINTF(out, out_size, "%s (row; default %s)", effective, configured);
    return 0;
}

int
dsd_app_squelch_view_edit_notice(const dsd_app_squelch_view* view, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    if (!view) {
        return -1;
    }
    char configured[24];
    (void)dsd_squelch_format(view->configured_level, " dB", configured, sizeof configured);
    if (!view->row_override) {
        DSD_SNPRINTF(out, out_size, "Applied: RTL squelch -> %s", configured);
        return 0;
    }
    char effective[24];
    (void)dsd_squelch_format(view->effective_level, " dB", effective, sizeof effective);
    DSD_SNPRINTF(out, out_size, "Default squelch %s; this channel overrides it (%s)", configured, effective);
    return 0;
}

double
dsd_app_squelch_db_or_off(double level) {
    return dsd_squelch_is_off(level) ? 0.0 : pwr_to_dB(level);
}
