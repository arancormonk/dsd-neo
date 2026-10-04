// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/squelch_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/squelch.h>
#include <stddef.h>

/* Under an AUTO setting in force: why it is off here, and what the tracker shows. */
static void
squelch_view_fill_auto(const dsd_opts* opts, const dsd_state* state, dsd_app_squelch_view* out) {
    if (!dsd_opts_input_is_radio(opts)) {
        out->auto_resolution = DSD_SQUELCH_RESOLVED_NO_RADIO;
    } else if (!dsd_opts_is_analog_family(opts)) {
        out->auto_resolution = DSD_SQUELCH_RESOLVED_DIGITAL;
    } else {
        out->auto_resolution = DSD_SQUELCH_RESOLVED_AS_SET;
    }
    out->auto_running = state->squelch_auto_active ? 1U : 0U;
    out->auto_learning = out->auto_running && state->squelch_auto_state == 0U ? 1U : 0U;
    out->auto_plan_valid = out->auto_running && state->squelch_auto_plan_valid ? 1U : 0U;
    out->auto_gate_open = out->auto_running && state->squelch_auto_gate_open ? 1U : 0U;
    out->auto_floor_db = (double)state->squelch_auto_floor_cdb / 100.0;
    out->effective_off =
        out->auto_resolution != DSD_SQUELCH_RESOLVED_AS_SET || (out->auto_running && !out->auto_plan_valid) ? 1U : 0U;
}

/* The configured default. Outside a scope, and while one is suspended, dsd_opts holds it. */
static void
squelch_view_fill_configured(const dsd_opts* opts, const dsd_scan_settings* configured, dsd_app_squelch_view* out) {
    const int mode = configured ? configured->rtl_squelch_mode : opts->rtl_squelch_mode;
    out->configured_level = configured ? configured->rtl_squelch_level : opts->rtl_squelch_level;
    out->configured_auto = mode == DSD_SQUELCH_MODE_AUTO ? 1U : 0U;
    out->configured_margin_db = configured ? configured->rtl_squelch_margin_db : opts->rtl_squelch_margin_db;
    out->configured_off = !out->configured_auto && dsd_squelch_is_off(out->configured_level) ? 1U : 0U;
}

/* The setting in force: a row's own, not dsd_opts (suspended for a command, dsd_opts reads the default), else
   dsd_opts. An AUTO row keeps the configured level beneath it. */
static void
squelch_view_fill_effective(const dsd_opts* opts, const dsd_scan_option_values* row, dsd_app_squelch_view* out) {
    if (out->row_override) {
        out->effective_auto = row->squelch_mode == DSD_SQUELCH_MODE_AUTO ? 1U : 0U;
        out->effective_margin_db = row->squelch_margin_db;
        out->effective_level =
            out->effective_auto ? out->configured_level : dsd_squelch_level_from_sql((double)row->squelch_db);
    } else {
        out->effective_auto = opts->rtl_squelch_mode == DSD_SQUELCH_MODE_AUTO ? 1U : 0U;
        out->effective_margin_db = opts->rtl_squelch_margin_db;
        out->effective_level = opts->rtl_squelch_level;
    }
    out->effective_off = dsd_squelch_is_off(out->effective_level) ? 1U : 0U;
}

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
    out->row_override = (row && (row->present & DSD_SCAN_OPT_SQUELCH)) ? 1U : 0U;
    squelch_view_fill_configured(opts, dsd_scan_mode_configured_view(state), out);
    squelch_view_fill_effective(opts, row, out);
    if (out->effective_auto) {
        squelch_view_fill_auto(opts, state, out);
    }
    return 0;
}

/* One setting as the readout names it: "auto +10 dB", "-60.0 dB" or "off". */
static void
squelch_view_setting_text(int is_auto, int margin_db, double level, char* out, size_t out_size) {
    if (is_auto) {
        const dsd_squelch_setting setting = dsd_squelch_setting_auto(margin_db);
        (void)dsd_squelch_setting_format(&setting, out, out_size);
        return;
    }
    (void)dsd_squelch_format(level, " dB", out, out_size);
}

int
dsd_app_squelch_view_auto_status(const dsd_app_squelch_view* view, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    if (!view) {
        return -1;
    }
    if (!view->effective_auto) {
        return 0;
    }
    if (view->auto_resolution == DSD_SQUELCH_RESOLVED_NO_RADIO) {
        DSD_SNPRINTF(out, out_size, "%s", "off: no radio input");
    } else if (view->auto_resolution == DSD_SQUELCH_RESOLVED_DIGITAL) {
        DSD_SNPRINTF(out, out_size, "%s", "off on digital");
    } else if (view->auto_running && !view->auto_plan_valid) {
        DSD_SNPRINTF(out, out_size, "%s", "off: no channel plan");
    } else if (!view->auto_running || view->auto_learning) {
        DSD_SNPRINTF(out, out_size, "%s", "learning");
    } else {
        DSD_SNPRINTF(out, out_size, "floor %.1f dB", view->auto_floor_db);
    }
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
    squelch_view_setting_text(view->effective_auto, view->effective_margin_db, view->effective_level, effective,
                              sizeof effective);
    char detail[32];
    (void)dsd_app_squelch_view_auto_status(view, detail, sizeof detail);
    if (!view->row_override) {
        if (detail[0] != '\0') {
            DSD_SNPRINTF(out, out_size, "%s (%s)", effective, detail);
        } else {
            DSD_SNPRINTF(out, out_size, "%s", effective);
        }
        return 0;
    }
    char configured[24];
    squelch_view_setting_text(view->configured_auto, view->configured_margin_db, view->configured_level, configured,
                              sizeof configured);
    DSD_SNPRINTF(out, out_size, "%s (%s%srow; default %s)", effective, detail, detail[0] != '\0' ? "; " : "",
                 configured);
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
    squelch_view_setting_text(view->configured_auto, view->configured_margin_db, view->configured_level, configured,
                              sizeof configured);
    if (!view->row_override) {
        DSD_SNPRINTF(out, out_size, "Applied: RTL squelch -> %s", configured);
        return 0;
    }
    char effective[24];
    squelch_view_setting_text(view->effective_auto, view->effective_margin_db, view->effective_level, effective,
                              sizeof effective);
    DSD_SNPRINTF(out, out_size, "Default squelch %s; this channel overrides it (%s)", configured, effective);
    return 0;
}

int
dsd_app_squelch_view_configured_text(const dsd_app_squelch_view* view, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    if (!view) {
        return -1;
    }
    if (view->configured_auto) {
        DSD_SNPRINTF(out, out_size, "auto+%d", dsd_squelch_setting_auto(view->configured_margin_db).margin_db);
    } else if (view->configured_off) {
        DSD_SNPRINTF(out, out_size, "%s", "off");
    } else {
        DSD_SNPRINTF(out, out_size, "%.1f", pwr_to_dB(view->configured_level));
    }
    return 0;
}

double
dsd_app_squelch_db_or_off(double level) {
    return dsd_squelch_is_off(level) ? 0.0 : pwr_to_dB(level);
}
