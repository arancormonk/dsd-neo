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
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/squelch.h>
#include <stddef.h>

/* Why a dynamic setting in force runs as it does here (dsd_squelch_resolution): the policy every surface shares. */
static uint8_t
squelch_view_resolution(const dsd_opts* opts, const dsd_app_squelch_view* view) {
    const dsd_squelch_setting in_force = dsd_squelch_setting_dynamic(
        view->effective_noise ? DSD_SQUELCH_MODE_NOISE : DSD_SQUELCH_MODE_AUTO, view->effective_margin_db);
    return (uint8_t)dsd_squelch_setting_resolve(&in_force, dsd_squelch_input_kind(opts),
                                                !dsd_opts_is_analog_family(opts),
                                                opts->analog_demod == DSD_ANALOG_DEMOD_AM, NULL);
}

/* What the stream shows: the noise squelch's gate and quieting when it runs, else the floor tracker's state. */
static void
squelch_view_fill_running(const dsd_state* state, dsd_app_squelch_view* out) {
    out->noise_running = state->squelch_noise_active ? 1U : 0U;
    out->noise_measured = out->noise_running && state->squelch_noise_measured ? 1U : 0U;
    out->noise_gate_open = out->noise_running && state->squelch_auto_gate_open ? 1U : 0U;
    out->noise_quieting_db = (double)state->squelch_noise_quieting_cdb / 100.0;
    out->noise_state = out->noise_running ? state->squelch_noise_state : (uint8_t)DSD_SQUELCH_NOISE_STATE_NONE;
    out->auto_running = state->squelch_auto_active && !out->noise_running ? 1U : 0U;
    out->auto_learning = out->auto_running && state->squelch_auto_state == 0U ? 1U : 0U;
    out->auto_plan_valid = out->auto_running && state->squelch_auto_plan_valid ? 1U : 0U;
    out->auto_gate_open = out->auto_running && state->squelch_auto_gate_open ? 1U : 0U;
    out->auto_floor_db = (double)state->squelch_auto_floor_cdb / 100.0;
}

/* Under a dynamic setting in force: why it is off here, how it runs, and what the tracker or the noise squelch shows. */
static void
squelch_view_fill_dynamic(const dsd_opts* opts, const dsd_state* state, dsd_app_squelch_view* out) {
    out->auto_resolution = squelch_view_resolution(opts, out);
    squelch_view_fill_running(state, out);
    out->noise_as_auto =
        out->effective_noise && (out->auto_resolution == DSD_SQUELCH_RESOLVED_AM_AUTO || out->auto_running) ? 1U : 0U;
    const int pcm_off =
        out->noise_state == DSD_SQUELCH_NOISE_STATE_NO_BAND || out->noise_state == DSD_SQUELCH_NOISE_STATE_NO_ROOM;
    const int off_here = out->auto_resolution == DSD_SQUELCH_RESOLVED_NO_RADIO
                         || out->auto_resolution == DSD_SQUELCH_RESOLVED_DIGITAL
                         || out->auto_resolution == DSD_SQUELCH_RESOLVED_AUDIO_AM || pcm_off;
    out->effective_off = off_here || (out->auto_running && !out->auto_plan_valid) ? 1U : 0U;
}

/* The configured default. Outside a scope, and while one is suspended, dsd_opts holds it. */
static void
squelch_view_fill_configured(const dsd_opts* opts, const dsd_scan_settings* configured, dsd_app_squelch_view* out) {
    const int mode = configured ? configured->rtl_squelch_mode : opts->rtl_squelch_mode;
    out->configured_level = configured ? configured->rtl_squelch_level : opts->rtl_squelch_level;
    out->configured_auto = mode == DSD_SQUELCH_MODE_AUTO ? 1U : 0U;
    out->configured_noise = mode == DSD_SQUELCH_MODE_NOISE ? 1U : 0U;
    out->configured_margin_db = configured ? configured->rtl_squelch_margin_db : opts->rtl_squelch_margin_db;
    out->configured_off = !dsd_squelch_mode_is_dynamic(mode) && dsd_squelch_is_off(out->configured_level) ? 1U : 0U;
}

/* The setting in force: a row's own, not dsd_opts (suspended for a command, dsd_opts reads the default), else
   dsd_opts. A dynamic row keeps the configured level beneath it. */
static void
squelch_view_fill_effective(const dsd_opts* opts, const dsd_scan_option_values* row, dsd_app_squelch_view* out) {
    const int mode = out->row_override ? row->squelch_mode : opts->rtl_squelch_mode;
    out->effective_auto = mode == DSD_SQUELCH_MODE_AUTO ? 1U : 0U;
    out->effective_noise = mode == DSD_SQUELCH_MODE_NOISE ? 1U : 0U;
    if (out->row_override) {
        out->effective_margin_db = row->squelch_margin_db;
        out->effective_level = dsd_squelch_mode_is_dynamic(mode) ? out->configured_level
                                                                 : dsd_squelch_level_from_sql((double)row->squelch_db);
    } else {
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
    if (out->effective_auto || out->effective_noise) {
        squelch_view_fill_dynamic(opts, state, out);
    }
    return 0;
}

/* The mode a view's flags name. */
static int
squelch_view_mode(uint8_t is_auto, uint8_t is_noise) {
    if (is_noise) {
        return DSD_SQUELCH_MODE_NOISE;
    }
    return is_auto ? DSD_SQUELCH_MODE_AUTO : DSD_SQUELCH_MODE_LEVEL;
}

/* One setting as the readout names it: "auto +10 dB", "noise +10 dB", "-60.0 dB" or "off". */
static void
squelch_view_setting_text(int mode, int margin_db, double level, char* out, size_t out_size) {
    if (dsd_squelch_mode_is_dynamic(mode)) {
        const dsd_squelch_setting setting = dsd_squelch_setting_dynamic(mode, margin_db);
        (void)dsd_squelch_setting_format(&setting, out, out_size);
        return;
    }
    (void)dsd_squelch_format(level, " dB", out, out_size);
}

/* What the floor tracker shows: its floor, "learning", or "off: no channel plan". */
static void
squelch_view_tracker_status(const dsd_app_squelch_view* view, char* out, size_t out_size) {
    if (view->auto_running && !view->auto_plan_valid) {
        DSD_SNPRINTF(out, out_size, "%s", "off: no channel plan");
    } else if (!view->auto_running || view->auto_learning) {
        DSD_SNPRINTF(out, out_size, "%s", "learning");
    } else {
        DSD_SNPRINTF(out, out_size, "floor %.1f dB", view->auto_floor_db);
    }
}

/* Why the dynamic squelch the view shows is off, or NULL while it runs. */
static const char*
squelch_view_off_reason(const dsd_app_squelch_view* view) {
    if (view->auto_resolution == DSD_SQUELCH_RESOLVED_NO_RADIO) {
        return "off: no radio input";
    }
    if (view->auto_resolution == DSD_SQUELCH_RESOLVED_DIGITAL) {
        return "off on digital";
    }
    if (view->auto_resolution == DSD_SQUELCH_RESOLVED_AUDIO_AM) {
        return "off on AM audio";
    }
    if (view->noise_state == DSD_SQUELCH_NOISE_STATE_NO_BAND) {
        return "off: no band above voice";
    }
    if (view->noise_state == DSD_SQUELCH_NOISE_STATE_NO_ROOM) {
        return "off: no room above voice";
    }
    return NULL;
}

int
dsd_app_squelch_view_dynamic_status(const dsd_app_squelch_view* view, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    if (!view) {
        return -1;
    }
    if (!view->effective_auto && !view->effective_noise) {
        return 0;
    }
    const char* off = squelch_view_off_reason(view);
    if (off) {
        DSD_SNPRINTF(out, out_size, "%s", off);
    } else if (view->noise_state == DSD_SQUELCH_NOISE_STATE_LEARNING
               || (view->noise_state != DSD_SQUELCH_NOISE_STATE_NONE && !view->noise_measured)) {
        DSD_SNPRINTF(out, out_size, "%s", "learning");
    } else if (view->noise_as_auto) {
        char tracker[24];
        squelch_view_tracker_status(view, tracker, sizeof tracker);
        DSD_SNPRINTF(out, out_size, "as auto: %s", tracker);
    } else if (view->effective_noise) {
        if (view->noise_measured) {
            DSD_SNPRINTF(out, out_size, "quieting %.0f dB", view->noise_quieting_db);
        } else {
            DSD_SNPRINTF(out, out_size, "%s", "starting");
        }
    } else {
        squelch_view_tracker_status(view, out, out_size);
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
    squelch_view_setting_text(squelch_view_mode(view->effective_auto, view->effective_noise), view->effective_margin_db,
                              view->effective_level, effective, sizeof effective);
    char detail[40];
    (void)dsd_app_squelch_view_dynamic_status(view, detail, sizeof detail);
    if (!view->row_override) {
        if (detail[0] != '\0') {
            DSD_SNPRINTF(out, out_size, "%s (%s)", effective, detail);
        } else {
            DSD_SNPRINTF(out, out_size, "%s", effective);
        }
        return 0;
    }
    char configured[24];
    squelch_view_setting_text(squelch_view_mode(view->configured_auto, view->configured_noise),
                              view->configured_margin_db, view->configured_level, configured, sizeof configured);
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
    squelch_view_setting_text(squelch_view_mode(view->configured_auto, view->configured_noise),
                              view->configured_margin_db, view->configured_level, configured, sizeof configured);
    if (!view->row_override) {
        DSD_SNPRINTF(out, out_size, "Applied: squelch -> %s", configured);
        return 0;
    }
    char effective[24];
    squelch_view_setting_text(squelch_view_mode(view->effective_auto, view->effective_noise), view->effective_margin_db,
                              view->effective_level, effective, sizeof effective);
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
    if (view->configured_auto || view->configured_noise) {
        DSD_SNPRINTF(out, out_size, "%s+%d", view->configured_noise ? "noise" : "auto",
                     dsd_squelch_setting_auto(view->configured_margin_db).margin_db);
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
