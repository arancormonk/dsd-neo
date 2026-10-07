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
#include <dsd-neo/runtime/rigctl_passband.h>
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
   width, which is in force until the row leaves: an nfm row's is the NFM demodulator's, an am row's the AM one's
   (dsd_scan_option_values::channel_bw_kind), and it overrides the width of that kind only. */
static void
analog_width_view_take_row(const dsd_opts* opts, const dsd_state* state, dsd_app_analog_width_view* out) {
    out->row_analog = dsd_scan_mode_is_analog(dsd_scan_mode_active(state)) ? 1U : 0U;
    if (out->row_analog) {
        out->kind = opts->analog_demod; /* a live row's options are in force */
    }
    const dsd_scan_option_values* row = dsd_scan_mode_row_options(state);
    if (row && (row->present & DSD_SCAN_OPT_BANDWIDTH) && row->channel_bw_hz > 0
        && (row->channel_bw_kind == DSD_ANALOG_DEMOD_AM) == (out->kind == DSD_ANALOG_DEMOD_AM)) {
        out->row_override = 1U;
        out->row_hz = row->channel_bw_hz;
    }
}

int
dsd_app_analog_width_unset_hz(const dsd_app_analog_width_view* view, int kind) {
    if (!dsd_analog_demod_is_valid(kind)) {
        return 0;
    }
    /* On a rigctl peer session -B stands in for an unset NFM width (0: the peer's own passband, issue #621). */
    if (view && view->peer_passband && kind == DSD_ANALOG_DEMOD_FM) {
        return view->setmod_bw_hz;
    }
    return dsd_analog_width_default_hz(kind);
}

/* On audio input with a rigctl peer that demodulates it (issue #621) the reading is the passband the peer is asked
   for. While the analog monitor of the view's kind runs, that is the rule's request for it (dsd_rigctl_passband_of(),
   as the engine asks it): the row's own width, the configured width, the AM default, -B, the peer's own. Otherwise (a
   typed digital row on air under -fA, a digital session with an explicit width offered) every tune asks the peer to
   follow at -B, FM, and that is the reading, never the configured width, which nothing asks for. A digital preset
   always runs FM (dsd_apply_decode_mode_preset()), and AM never runs on PCM input, so the view's kind is FM there.
   Options and the copied scope only, so a snapshot reads the same. */
static void
analog_width_view_take_peer(const dsd_opts* opts, dsd_app_analog_width_view* out) {
    out->passband_in_force = (dsd_opts_is_analog_family(opts) && opts->analog_demod == out->kind) ? 1U : 0U;
    if (out->passband_in_force) {
        const dsd_rigctl_passband p = dsd_rigctl_passband_for_kind(out->kind, out->row_override ? out->row_hz : 0,
                                                                   out->configured_hz, out->setmod_bw_hz);
        out->width_hz = p.bandwidth_hz;
        out->passband_source = (uint8_t)p.source;
        return;
    }
    out->width_hz = out->setmod_bw_hz;
    out->passband_source =
        (uint8_t)(out->setmod_bw_hz > 0 ? DSD_RIGCTL_PASSBAND_SETMOD_BW : DSD_RIGCTL_PASSBAND_PEER_OWN);
}

/* What the options and the scan scope say before any width is read: the kind, whether the configured preset shows a
   width, the analog row on air and its own width, the configured width, the input and its rigctl peer, and what an
   unset setting stands for. Outside a scope, and while one is suspended, dsd_opts holds the configured options. No
   state (no snapshot published yet) has no scope either. */
static void
analog_width_view_take_session(const dsd_opts* opts, const dsd_state* state, dsd_app_analog_width_view* out) {
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(state);
    const int analog_only = configured ? configured->analog_only : opts->analog_only;
    out->kind = configured ? configured->analog_demod : opts->analog_demod;
    out->shown = (analog_only == 1 && opts->m17encoder != 1) ? 1U : 0U;
    analog_width_view_take_row(opts, state, out);
    out->configured_hz = dsd_scan_mode_configured_analog_width(opts, state, out->kind);
    out->radio_input = dsd_opts_input_is_radio(opts) ? 1U : 0U;
    out->peer_passband = dsd_opts_rigctl_peer_demodulates(opts) ? 1U : 0U;
    out->setmod_bw_hz = (out->peer_passband && opts->setmod_bw > 0) ? opts->setmod_bw : 0;
    out->unset_hz = dsd_app_analog_width_unset_hz(out, out->kind);
}

/* On a radio input the width is a channel filter. The bound on the widths offered holds under any preset: a width set
   outside its kind is held to it at the switch. The width in force is read only when shown or under an analog row: the
   row's own, else the configured one, as the front end reports it while it runs the monitor. */
static void
analog_width_view_take_radio(const dsd_opts* opts, const dsd_frontend_metrics* metrics,
                             dsd_app_analog_width_view* out) {
    const int rate_hz = analog_width_view_rate_hz(opts, metrics);
    out->max_hz = rate_hz > 0 ? dsd_analog_width_max_for_rate(rate_hz) : 0;
    if (!out->shown && !out->row_analog) {
        return;
    }
    out->width_hz = out->row_override ? out->row_hz : dsd_analog_width_effective_hz(out->kind, out->configured_hz);
    analog_width_view_take_front_end(opts, metrics, out);
}

/* Whether the controls offer either kind's width (dsd_app_analog_width_offered()): on a rigctl peer session the
   reading is then the passband asked for now, so the frontends that show a passband beside the controls (the
   terminal's field, the Qt/Android Passband row) never show a setting the peer is not asked for. */
static int
analog_width_view_offers_any(const dsd_opts* opts, const dsd_state* state, const dsd_app_analog_width_view* view) {
    return (dsd_app_analog_width_offered(opts, state, view, DSD_ANALOG_DEMOD_FM)
            || dsd_app_analog_width_offered(opts, state, view, DSD_ANALOG_DEMOD_AM))
               ? 1
               : 0;
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
    analog_width_view_take_session(opts, state, out);
    if (out->radio_input) {
        analog_width_view_take_radio(opts, metrics, out);
    } else if (out->peer_passband && analog_width_view_offers_any(opts, state, out)) {
        /* Other PCM input filters nothing, and no width is in force there. */
        analog_width_view_take_peer(opts, out);
    }
    return 0;
}

int
dsd_app_analog_width_setting_hz(const dsd_opts* opts, int kind) {
    if (!opts) {
        return 0;
    }
    return (kind == DSD_ANALOG_DEMOD_AM) ? opts->analog_am_bandwidth_hz : opts->analog_nfm_bandwidth_hz;
}

/* Whether the session runs or holds the width of analog @p kind. The configured preset's kind stays editable while an
   analog scan row on air runs another (an nfm row on an AM session, issue #526), and the row's kind is offered on any
   session, as is a kind with an explicit configured width, since a switch to that kind is held to it. */
static int
analog_width_view_kind_in_use(const dsd_opts* opts, const dsd_state* state, const dsd_app_analog_width_view* view,
                              int kind) {
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(state);
    const int preset_kind = configured ? configured->analog_demod : opts->analog_demod;
    return ((view->shown && preset_kind == kind) || (view->row_analog && view->kind == kind)
            || dsd_scan_mode_configured_analog_width(opts, state, kind) > 0)
               ? 1
               : 0;
}

/* The unset AM default is its 6 kHz filter, held to the rate as an explicit width is (the unset NFM default runs at
   any rate). Where the rate cannot filter it but filters a narrower AM width, a switch to AM is refused with word to
   narrow the width, which has to be possible before the switch. */
static int
analog_width_view_am_default_narrowable(const dsd_app_analog_width_view* view, int kind) {
    const int default_hz = dsd_analog_width_default_hz(kind);
    return (kind == DSD_ANALOG_DEMOD_AM && view->max_hz > 0 && default_hz > view->max_hz
            && dsd_analog_width_min_hz(kind) <= view->max_hz)
               ? 1
               : 0;
}

int
dsd_app_analog_width_offered(const dsd_opts* opts, const dsd_state* state, const dsd_app_analog_width_view* view,
                             int kind) {
    /* A radio input's channel filter, or the passband a rigctl peer that demodulates audio input is asked for (issue
       #621); other PCM input filters nothing. */
    if (!opts || !view || (!view->radio_input && !view->peer_passband) || !dsd_analog_demod_is_valid(kind)) {
        return 0;
    }
    return (analog_width_view_kind_in_use(opts, state, view, kind)
            || analog_width_view_am_default_narrowable(view, kind))
               ? 1
               : 0;
}

/* The passband a rigctl peer is asked for, by its source (issue #621): a width alone when configured, noted as the AM
   default or as -B, the peer's own passband by name, and a row's own width naming the setting it overrides, the AM
   default as its width. */
static void
analog_width_view_format_peer(const dsd_app_analog_width_view* view, char* out, size_t out_size) {
    char width[DSD_ANALOG_WIDTH_TEXT_MAX];
    if (view->passband_source == DSD_RIGCTL_PASSBAND_PEER_OWN || view->width_hz <= 0
        || dsd_analog_width_format(view->width_hz, width, sizeof width) != 0) {
        DSD_SNPRINTF(out, out_size, "%s", "peer's own");
        return;
    }
    switch (view->passband_source) {
        case DSD_RIGCTL_PASSBAND_ROW: {
            char setting[DSD_APP_ANALOG_WIDTH_TEXT_MAX];
            if (view->configured_hz <= 0 && view->kind == DSD_ANALOG_DEMOD_AM) {
                (void)dsd_analog_width_format(dsd_analog_width_default_hz(view->kind), setting, sizeof setting);
            } else {
                (void)dsd_app_analog_width_setting_text(view, view->kind, view->configured_hz, setting, sizeof setting);
            }
            DSD_SNPRINTF(out, out_size, "%s (row; default %s)", width, setting);
            return;
        }
        case DSD_RIGCTL_PASSBAND_AM_DEFAULT: DSD_SNPRINTF(out, out_size, "%s (default)", width); return;
        case DSD_RIGCTL_PASSBAND_SETMOD_BW: DSD_SNPRINTF(out, out_size, "%s (-B)", width); return;
        default: DSD_SNPRINTF(out, out_size, "%s", width); return;
    }
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
    if (view->peer_passband) {
        /* A reading whenever a kind is offered (analog_width_view_take_peer()); none otherwise. */
        if (view->passband_source != DSD_RIGCTL_PASSBAND_FOLLOW) {
            analog_width_view_format_peer(view, out, out_size);
        }
        return 0;
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
    if (view.peer_passband && view.passband_in_force && view.kind == kind && !view.row_override) {
        /* The edit is the passband the rigctl peer was asked for (issue #621): named as the reading names it. */
        char reading[DSD_APP_ANALOG_WIDTH_TEXT_MAX];
        (void)dsd_app_analog_width_view_format(&view, reading, sizeof reading);
        DSD_SNPRINTF(out, out_size, "Applied: %s passband -> %s", label, reading);
        return 0;
    }
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

int
dsd_app_analog_width_setting_text(const dsd_app_analog_width_view* view, int kind, int configured_hz, char* out,
                                  size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    if (!view || !dsd_analog_demod_is_valid(kind)) {
        return -1;
    }
    if (configured_hz > 0 || !view->peer_passband || kind == DSD_ANALOG_DEMOD_AM) {
        return dsd_app_analog_width_setting_format(configured_hz, out, out_size);
    }
    /* An unset NFM width on a rigctl peer: -B stands in for it, else the peer keeps its own passband (issue #621). */
    char width[DSD_ANALOG_WIDTH_TEXT_MAX];
    if (view->setmod_bw_hz > 0 && dsd_analog_width_format(view->setmod_bw_hz, width, sizeof width) == 0) {
        DSD_SNPRINTF(out, out_size, "-B %s", width);
    } else {
        DSD_SNPRINTF(out, out_size, "%s", "peer's own");
    }
    return 0;
}
