// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/analog_width_view.h>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <stddef.h>

/* The front end's width is the channel in force only while it runs the monitor for the analog family the options in
   force select. Its mirror outlives a stopped stream, and under a typed digital row it describes the row's channel. */
static void
analog_width_view_take_front_end(const dsd_opts* opts, const dsd_frontend_metrics* metrics,
                                 dsd_app_analog_width_view* out) {
    if (!metrics || !metrics->stream_active) {
        return;
    }
    out->max_hz = metrics->demod_rate_hz > 0 ? dsd_analog_width_max_for_rate(metrics->demod_rate_hz) : 0;
    if (dsd_opts_is_analog_family(opts) && metrics->output_kind == DSD_FRONTEND_RTL_OUTPUT_AUDIO_MONITOR
        && metrics->channel_bandwidth_hz > 0) {
        out->width_hz = metrics->channel_bandwidth_hz;
        out->dsp_limited = metrics->channel_bandwidth_dsp_limited ? 1U : 0U;
    }
}

int
dsd_app_analog_width_view_get(const dsd_opts* opts, const dsd_state* state, const dsd_frontend_metrics* metrics,
                              dsd_app_analog_width_view* out) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!opts) {
        return -1;
    }
    /* Outside a scope, and while one is suspended, dsd_opts holds the configured options. No state (no snapshot
       published yet) has no scope either. */
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(state);
    const int analog_only = configured ? configured->analog_only : opts->analog_only;
    out->kind = configured ? configured->analog_demod : opts->analog_demod;
    const int configured_hz =
        (out->kind == DSD_ANALOG_DEMOD_AM) ? opts->analog_am_bandwidth_hz : opts->analog_nfm_bandwidth_hz;
    out->configured_hz = configured_hz > 0 ? configured_hz : 0;
    out->radio_input = dsd_opts_input_is_radio(opts) ? 1U : 0U;
    out->shown = (analog_only == 1 && opts->m17encoder != 1) ? 1U : 0U;
    if (!out->shown || !out->radio_input) {
        return 0;
    }
    out->width_hz = dsd_analog_width_effective_hz(out->kind, out->configured_hz);
    analog_width_view_take_front_end(opts, metrics, out);
    return 0;
}

int
dsd_app_analog_width_view_format(const dsd_app_analog_width_view* view, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    if (!view) {
        return -1;
    }
    if (!view->shown) {
        return 0;
    }
    if (!view->radio_input) {
        DSD_SNPRINTF(out, out_size, "%s", "not used on PCM input");
        return 0;
    }
    char width[DSD_ANALOG_WIDTH_TEXT_MAX];
    if (dsd_analog_width_format(view->width_hz, width, sizeof width) != 0) {
        return 0;
    }
    const char* note = "";
    if (view->dsp_limited) {
        note = " (DSP-limited)";
    } else if (view->configured_hz <= 0) {
        note = " (default)";
    }
    DSD_SNPRINTF(out, out_size, "%s%s", width, note);
    return 0;
}

int
dsd_app_analog_width_setting_format(int configured_hz, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    char width[DSD_ANALOG_WIDTH_TEXT_MAX];
    if (configured_hz > 0 && dsd_analog_width_format(configured_hz, width, sizeof width) == 0) {
        DSD_SNPRINTF(out, out_size, "%s", width);
    } else {
        DSD_SNPRINTF(out, out_size, "%s", "default");
    }
    return 0;
}
