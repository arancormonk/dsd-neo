// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/rtl_gain_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>

int
dsd_app_rtl_gain_view_get(const dsd_opts* opts, const dsd_state* state, dsd_app_rtl_gain_view* out) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!opts || !state) {
        return -1;
    }
    out->effective_gain = opts->rtl_gain_value;
    out->configured_gain = opts->rtl_gain_value;
    /* The coordinator publishes the configured gain while a target list is loaded. */
    if (opts->trunk_scan_enabled == 1 && state->trunk_scan_target_count > 0U) {
        out->scan_configured = 1U;
        out->configured_gain = state->trunk_scan_configured_gain;
        out->row_override = state->trunk_scan_gain_override ? 1U : 0U;
        out->configured_autogain = state->trunk_scan_configured_autogain ? 1U : 0U;
    }
    return 0;
}

static int
gain_text(int gain, char* out, size_t out_size) {
    if (gain <= 0) {
        DSD_SNPRINTF(out, out_size, "%s", "AGC");
    } else {
        DSD_SNPRINTF(out, out_size, "%d", gain);
    }
    return 0;
}

int
dsd_app_rtl_gain_view_label(const dsd_app_rtl_gain_view* view, char* out, size_t out_size) {
    if (!view || !out || out_size == 0U) {
        return -1;
    }
    char configured[16];
    (void)gain_text(view->configured_gain, configured, sizeof configured);
    if (view->row_override) {
        char effective[16];
        (void)gain_text(view->effective_gain, effective, sizeof effective);
        DSD_SNPRINTF(out, out_size, "Gain... [%s] (target: %s)", configured, effective);
    } else {
        DSD_SNPRINTF(out, out_size, "Gain... [%s]", configured);
    }
    return 0;
}

int
dsd_app_rtl_gain_view_edit_notice(const dsd_app_rtl_gain_view* view, char* out, size_t out_size) {
    if (!view || !out || out_size == 0U) {
        return -1;
    }
    char configured[16];
    (void)gain_text(view->configured_gain, configured, sizeof configured);
    if (view->row_override) {
        char effective[16];
        (void)gain_text(view->effective_gain, effective, sizeof effective);
        DSD_SNPRINTF(out, out_size, "Default RTL gain -> %s; this channel overrides it (%s)", configured, effective);
    } else {
        DSD_SNPRINTF(out, out_size, "Applied: RTL gain -> %s", configured);
    }
    return 0;
}

int
dsd_app_rtl_autogain_view_label(const dsd_app_rtl_gain_view* view, int live_on, char* out, size_t out_size) {
    if (!view || !out || out_size == 0U) {
        return -1;
    }
    if (!view->scan_configured) {
        DSD_SNPRINTF(out, out_size, "Tuner autogain [%s]", live_on ? "On" : "Off");
        return 0;
    }
    if (view->effective_gain > 0) {
        DSD_SNPRINTF(out, out_size, "Tuner autogain [%s] (suspended: gain %d)",
                     view->configured_autogain ? "On" : "Off", view->effective_gain);
    } else {
        DSD_SNPRINTF(out, out_size, "Tuner autogain [%s]", view->configured_autogain ? "On" : "Off");
    }
    return 0;
}

int
dsd_app_rtl_autogain_view_edit_notice(const dsd_app_rtl_gain_view* view, char* out, size_t out_size) {
    if (!view || !out || out_size == 0U) {
        return -1;
    }
    const char* on = view->configured_autogain ? "On" : "Off";
    if (view->effective_gain > 0) {
        DSD_SNPRINTF(out, out_size, "Default tuner autogain -> %s; manual gain %d suspends it", on,
                     view->effective_gain);
    } else {
        DSD_SNPRINTF(out, out_size, "Applied: tuner autogain -> %s", on);
    }
    return 0;
}
