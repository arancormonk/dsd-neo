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
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <limits.h>
#include <stddef.h>

int
dsd_app_analog_rtl_bw_rate_hz(const char* audio_in_dev, int audio_in_type, int rtl_bw_khz) {
    /* detect_radio_source(): the stream opens every device string outside the SoapySDR, Airspy and I/Q replay specs
       as an RTL-SDR (rtl_tcp by its own spec), and runs it at the DSP bandwidth. */
    const int rtl = dsd_opts_audio_in_dev_is_rtl_spec(audio_in_dev)
                    || dsd_opts_audio_in_dev_is_rtltcp_spec(audio_in_dev)
                    || (audio_in_type == AUDIO_IN_RTL && !dsd_opts_audio_in_dev_is_soapy_spec(audio_in_dev)
                        && !dsd_opts_audio_in_dev_is_airspy_spec(audio_in_dev)
                        && !dsd_opts_audio_in_dev_is_iqreplay_spec(audio_in_dev));
    if (!rtl || rtl_bw_khz <= 0) {
        return 0;
    }
    return rtl_bw_khz > INT_MAX / 1000 ? INT_MAX : rtl_bw_khz * 1000;
}

/* The rate from which the unset NFM default runs a channel filter when DSD_NEO_CHANNEL_LPF does not say
   (demod_channel_lpf_default_enable()). */
#define ANALOG_WIDTH_VIEW_DEFAULT_FILTER_MIN_RATE_HZ 20000

/* Whether the unset NFM default runs a channel filter at DSP rate @p rate_hz. A running stream decided it when its
   configuration started (@p lpf_default, dsd_frontend_channel_lpf_default) and keeps that decision whatever rate the
   device then delivers; before a stream has said, it is predicted as a start decides it: as DSD_NEO_CHANNEL_LPF sets
   it, otherwise from the rate the filter starts at. */
static int
analog_width_view_default_filter_on(int rate_hz, int lpf_default) {
    if (lpf_default == DSD_FRONTEND_CHANNEL_LPF_DEFAULT_ON || lpf_default == DSD_FRONTEND_CHANNEL_LPF_DEFAULT_OFF) {
        return lpf_default == DSD_FRONTEND_CHANNEL_LPF_DEFAULT_ON;
    }
    const dsdneoRuntimeConfig* env = dsd_neo_get_config();
    if (env && env->channel_lpf_is_set) {
        return env->channel_lpf_enable != 0;
    }
    return rate_hz >= ANALOG_WIDTH_VIEW_DEFAULT_FILTER_MIN_RATE_HZ;
}

/* The unset NFM default reads as what the monitor runs at DSP rate @p rate_hz (rtl_demod_apply_analog_channel(), and
   the width the stream publishes for it): its own design where the channel filter runs and the rate realizes it;
   otherwise the rate bounds the channel, DSP-limited, through the legacy WIDE plan's passband while the filter runs
   (dsd_channel_lpf_legacy_wide_width_hz(): a 128 kHz replay protects about 78.5 kHz, not 128), or the rate itself with
   no channel filter (@p lpf_default: analog_width_view_default_filter_on()). Every RTL DSP bandwidth that cannot
   realize the default lies below the rate the filter starts at, so there the rate itself is the reading. */
static void
analog_width_view_take_default_rate(int rate_hz, int lpf_default, dsd_app_analog_width_view* out) {
    if (rate_hz <= 0 || out->kind != DSD_ANALOG_DEMOD_FM || out->configured_hz > 0 || out->row_override) {
        return;
    }
    const int filter_on = analog_width_view_default_filter_on(rate_hz, lpf_default);
    if (filter_on && dsd_analog_width_realizable(out->width_hz, rate_hz)) {
        return;
    }
    out->width_hz = filter_on ? dsd_channel_lpf_legacy_wide_width_hz(rate_hz) : rate_hz;
    out->dsp_limited = 1U;
}

/* The DSP rate the widths are held to: the running stream's demod rate, or with none the rate the next start runs at,
   where the RTL DSP bandwidth sets it; 0 when not known. */
static int
analog_width_view_rate_hz(const dsd_opts* opts, const dsd_frontend_metrics* metrics) {
    if (metrics && metrics->stream_active) {
        return metrics->demod_rate_hz;
    }
    return dsd_app_analog_rtl_bw_rate_hz(opts->audio_in_dev, opts->audio_in_type, opts->rtl_dsp_bw_khz);
}

/* With no stream running, an unset NFM default reads as what the next start publishes at the rate the RTL DSP
   bandwidth sets. */
static void
analog_width_view_take_rtl_rate(const dsd_opts* opts, dsd_app_analog_width_view* out) {
    const int rate_hz = dsd_app_analog_rtl_bw_rate_hz(opts->audio_in_dev, opts->audio_in_type, opts->rtl_dsp_bw_khz);
    if (rate_hz <= 0) {
        return;
    }
    analog_width_view_take_default_rate(rate_hz, DSD_FRONTEND_CHANNEL_LPF_DEFAULT_UNKNOWN, out);
}

/* The front end's width is the channel in force only while it runs the monitor for the analog family and kind the
   options in force select. Its mirror outlives a stopped stream, under a typed digital row (or CQPSK toggled on under
   -fA) it describes that profile's channel, and across an FM <-> AM switch that has not landed yet it is the other
   kind's (channel_analog_kind); the width in force is then the one the monitor returns to at the running demod rate
   (analog_width_view_take_default_rate() for the unset default, with the stream's own filter decision). */
static void
analog_width_view_take_front_end(const dsd_opts* opts, const dsd_frontend_metrics* metrics,
                                 dsd_app_analog_width_view* out) {
    if (!metrics || !metrics->stream_active) {
        analog_width_view_take_rtl_rate(opts, out);
        return;
    }
    if (dsd_opts_is_analog_family(opts) && metrics->output_kind == DSD_FRONTEND_RTL_OUTPUT_AUDIO_MONITOR
        && metrics->channel_bandwidth_hz > 0 && metrics->channel_analog_kind == out->kind) {
        out->width_hz = metrics->channel_bandwidth_hz;
        out->dsp_limited = metrics->channel_bandwidth_dsp_limited ? 1U : 0U;
        return;
    }
    analog_width_view_take_default_rate(metrics->demod_rate_hz, metrics->channel_lpf_default, out);
}

/* An analog scan row on air (issue #526) runs the analog family whatever the configured preset, and may set its own
   width, which is in force until the row leaves. Only an nfm row parses a width, so it is the NFM demodulator's. */
static void
analog_width_view_take_row(const dsd_opts* opts, const dsd_state* state, dsd_app_analog_width_view* out) {
    out->row_analog = dsd_scan_mode_is_analog(dsd_scan_mode_active(state)) ? 1U : 0U;
    if (out->row_analog) {
        out->kind = opts->analog_demod; /* a live row's options are in force */
    }
    const dsd_scan_option_values* row = dsd_scan_mode_row_options(state);
    if (row && (row->present & DSD_SCAN_OPT_BANDWIDTH) && row->channel_bw_hz > 0 && out->kind == DSD_ANALOG_DEMOD_FM) {
        out->row_override = 1U;
        out->row_hz = row->channel_bw_hz;
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
    out->shown = (analog_only == 1 && opts->m17encoder != 1) ? 1U : 0U;
    analog_width_view_take_row(opts, state, out);
    out->configured_hz = dsd_scan_mode_configured_analog_width(opts, state, out->kind);
    out->radio_input = dsd_opts_input_is_radio(opts) ? 1U : 0U;
    if (!out->radio_input) {
        return 0;
    }
    /* The bound on the widths offered holds under any preset: a width set outside its kind is held to it at the
       switch. */
    const int rate_hz = analog_width_view_rate_hz(opts, metrics);
    out->max_hz = rate_hz > 0 ? dsd_analog_width_max_for_rate(rate_hz) : 0;
    if (!out->shown && !out->row_analog) {
        return 0;
    }
    out->width_hz = out->row_override ? out->row_hz : dsd_analog_width_effective_hz(out->kind, out->configured_hz);
    analog_width_view_take_front_end(opts, metrics, out);
    return 0;
}

int
dsd_app_analog_width_setting_hz(const dsd_opts* opts, int kind) {
    if (!opts) {
        return 0;
    }
    return (kind == DSD_ANALOG_DEMOD_AM) ? opts->analog_am_bandwidth_hz : opts->analog_nfm_bandwidth_hz;
}

int
dsd_app_analog_width_offered(const dsd_opts* opts, const dsd_state* state, const dsd_app_analog_width_view* view,
                             int kind) {
    if (!opts || !view || !view->radio_input || !dsd_analog_demod_is_valid(kind)) {
        return 0;
    }
    /* The configured preset's kind stays editable while an analog scan row on air runs another (an nfm row on an AM
       session, issue #526), and the row's kind is offered on any session. */
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(state);
    const int preset_kind = configured ? configured->analog_demod : opts->analog_demod;
    if ((view->shown && preset_kind == kind) || (view->row_analog && view->kind == kind)
        || dsd_scan_mode_configured_analog_width(opts, state, kind) > 0) {
        return 1;
    }
    /* The unset AM default is its 6 kHz filter, held to the rate as an explicit width is (the unset NFM default runs
       at any rate). Where the rate cannot filter it but filters a narrower AM width, a switch to AM is refused with
       word to narrow the width, which has to be possible before the switch. */
    const int default_hz = dsd_analog_width_default_hz(kind);
    return (kind == DSD_ANALOG_DEMOD_AM && view->max_hz > 0 && default_hz > view->max_hz
            && dsd_analog_width_min_hz(kind) <= view->max_hz)
               ? 1
               : 0;
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
    if (!view->shown && !view->row_analog) {
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
    if (view->row_override) {
        /* The configured width of the row's demodulator, which the row's own overrides and the width controls edit;
           the kind's default width when none is set. */
        char configured[DSD_ANALOG_WIDTH_TEXT_MAX];
        if (dsd_analog_width_format(dsd_analog_width_effective_hz(view->kind, view->configured_hz), configured,
                                    sizeof configured)
            != 0) {
            configured[0] = '\0';
        }
        DSD_SNPRINTF(out, out_size, "%s (row; default %s)", width, configured);
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
dsd_app_analog_width_edit_notice(const dsd_opts* opts, const dsd_state* state, int kind, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    dsd_app_analog_width_view view;
    if (!dsd_analog_demod_is_valid(kind) || dsd_app_analog_width_view_get(opts, state, NULL, &view) != 0) {
        return -1;
    }
    /* The kind the command edited, not the configured preset's: an NFM edit on an AM session names the NFM width. */
    char configured[DSD_APP_ANALOG_WIDTH_TEXT_MAX];
    (void)dsd_app_analog_width_setting_format(dsd_scan_mode_configured_analog_width(opts, state, kind), configured,
                                              sizeof configured);
    const char* label = dsd_analog_demod_label(kind);
    if (!view.row_override || view.kind != kind) {
        DSD_SNPRINTF(out, out_size, "Applied: %s bandwidth -> %s", label, configured);
        return 0;
    }
    char row[DSD_ANALOG_WIDTH_TEXT_MAX];
    if (dsd_analog_width_format(view.row_hz, row, sizeof row) != 0) {
        row[0] = '\0';
    }
    DSD_SNPRINTF(out, out_size, "Default %s bandwidth -> %s; this channel overrides it (%s)", label, configured, row);
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
