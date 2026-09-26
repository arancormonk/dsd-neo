// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The shared analog channel width readout (issue #525): the width in force under the configured analog preset, the
 * front end's while a running stream runs the monitor for it, the configured one otherwise; the DSP-limited flag; and
 * the one spelling of the reading and of a configured width that every frontend shows. Issue #526: an nfm scan row's
 * own width is in force over the configured one, which the reading names as the row's default. */

#include <assert.h>
#include <dsd-neo/app_control/analog_width_view.h>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void
expect_reading(const dsd_app_analog_width_view* view, const char* want) {
    char out[DSD_APP_ANALOG_WIDTH_TEXT_MAX];
    assert(dsd_app_analog_width_view_format(view, out, sizeof out) == 0);
    assert(strcmp(out, want) == 0);
}

static void
expect_setting(int configured_hz, const char* want) {
    char out[DSD_APP_ANALOG_WIDTH_TEXT_MAX];
    assert(dsd_app_analog_width_setting_format(configured_hz, out, sizeof out) == 0);
    assert(strcmp(out, want) == 0);
}

/* What a running stream on the analog monitor reports. */
static dsd_frontend_metrics
monitor_metrics(int width_hz, int dsp_limited, int demod_rate_hz) {
    dsd_frontend_metrics m;
    DSD_MEMSET(&m, 0, sizeof m);
    m.stream_active = 1;
    m.output_kind = DSD_FRONTEND_RTL_OUTPUT_AUDIO_MONITOR;
    m.channel_bandwidth_hz = width_hz;
    m.channel_bandwidth_dsp_limited = dsp_limited;
    m.demod_rate_hz = demod_rate_hz;
    return m;
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->wav_sample_rate = 48000;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtl_dsp_bw_khz = 48;
    opts->frame_dmr = 1;

    dsd_app_analog_width_view view;
    /* A digital session shows nothing, but the configured width is still configuration. */
    opts->analog_nfm_bandwidth_hz = 12500;
    assert(dsd_app_analog_width_view_get(opts, state, NULL, &view) == 0);
    assert(!view.shown && view.width_hz == 0 && view.configured_hz == 12500);
    expect_reading(&view, "");

    opts->frame_dmr = 0;
    opts->analog_only = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->analog_nfm_bandwidth_hz = 0;
    /* No stream: the configured width, the default here, marked as such. The RTL DSP bandwidth (48 kHz) is the rate
       the next start runs at and bounds the widths offered. */
    assert(dsd_app_analog_width_view_get(opts, state, NULL, &view) == 0);
    assert(view.shown && view.radio_input && view.kind == DSD_ANALOG_DEMOD_FM);
    assert(view.width_hz == 16000 && view.configured_hz == 0 && !view.dsp_limited && view.max_hz == 42000);
    expect_reading(&view, "16 kHz (default)");

    /* No stream at a 12 kHz DSP bandwidth: the default runs no channel filter there, so it reads as the rate, as the
       running stream will publish it, and the widest width offered is the one that rate filters. */
    opts->rtl_dsp_bw_khz = 12;
    assert(dsd_app_analog_width_view_get(opts, state, NULL, &view) == 0);
    assert(view.width_hz == 12000 && view.dsp_limited && view.configured_hz == 0 && view.max_hz == 9600);
    expect_reading(&view, "12 kHz (DSP-limited)");
    /* ...an explicit width the rate filters is the channel in force, not limited. */
    opts->analog_nfm_bandwidth_hz = 8000;
    assert(dsd_app_analog_width_view_get(opts, state, NULL, &view) == 0);
    assert(view.width_hz == 8000 && !view.dsp_limited && view.max_hz == 9600);
    expect_reading(&view, "8 kHz");
    opts->analog_nfm_bandwidth_hz = 0;
    /* The terminal's Input > Switch source > RTL-SDR leaves "pulse" on an RTL input: the stream opens it as an RTL-SDR,
       at this rate. */
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "pulse");
    assert(dsd_app_analog_width_view_get(opts, state, NULL, &view) == 0);
    assert(view.width_hz == 12000 && view.dsp_limited && view.max_hz == 9600);
    /* A SoapySDR device may force another rate: nothing is known before it runs. */
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy:driver=airspy");
    assert(dsd_app_analog_width_view_get(opts, state, NULL, &view) == 0);
    assert(view.width_hz == 16000 && !view.dsp_limited && view.max_hz == 0);
    expect_reading(&view, "16 kHz (default)");
    opts->audio_in_dev[0] = '\0';
    opts->rtl_dsp_bw_khz = 48;

    /* On the monitor, the front end's width and the widest its demod rate filters. */
    opts->analog_nfm_bandwidth_hz = 12500;
    dsd_frontend_metrics m = monitor_metrics(12500, 0, 24000);
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == 12500 && view.configured_hz == 12500 && view.max_hz == 20400 && !view.dsp_limited);
    expect_reading(&view, "12.5 kHz");
    /* The front end's report wins over the configured width: they are kept apart so the case shows which it took. */
    m.channel_bandwidth_hz = 11250;
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == 11250);

    /* The unset default below a 20 kHz DSP rate runs no channel filter: the rate bounds the channel. */
    opts->analog_nfm_bandwidth_hz = 0;
    m = monitor_metrics(12000, 1, 12000);
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == 12000 && view.dsp_limited && view.configured_hz == 0 && view.max_hz == 9600);
    expect_reading(&view, "12 kHz (DSP-limited)");

    /* The front end's mirror outlives a stopped stream, and describes another output than the monitor's. */
    m.stream_active = 0;
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == 16000 && !view.dsp_limited && view.max_hz == 42000);
    m = monitor_metrics(12500, 0, 48000);
    m.output_kind = DSD_FRONTEND_RTL_OUTPUT_SYMBOL_CQPSK;
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == 16000 && view.max_hz == 42000);
    /* ...and at a 12 kHz demod rate the unset default the monitor returns to runs no channel filter: the rate bounds
       it, as the front end will report once the monitor runs again. */
    m = monitor_metrics(12500, 0, 12000);
    m.output_kind = DSD_FRONTEND_RTL_OUTPUT_SYMBOL_CQPSK;
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == 12000 && view.dsp_limited && view.configured_hz == 0 && view.max_hz == 9600);
    expect_reading(&view, "12 kHz (DSP-limited)");
    /* At a 128 kHz demod rate (an I/Q replay, a SoapySDR or Airspy device) the channel filter runs, but the rate cannot
       realize the default's own design: the legacy WIDE plan runs instead, and its passband bounds the channel the
       monitor returns to, as the front end publishes it there, not the rate. */
    m = monitor_metrics(12500, 0, 128000);
    m.output_kind = DSD_FRONTEND_RTL_OUTPUT_SYMBOL_CQPSK;
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == dsd_channel_lpf_legacy_wide_width_hz(128000) && view.dsp_limited);
    assert(view.width_hz > 70000 && view.width_hz < 90000);
    /* ...and with DSD_NEO_CHANNEL_LPF=0 no channel filter runs there: the rate itself. */
    assert(dsd_setenv("DSD_NEO_CHANNEL_LPF", "0", 1) == 0);
    dsd_neo_config_init();
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == 128000 && view.dsp_limited);
    expect_reading(&view, "128 kHz (DSP-limited)");
    /* DSD_NEO_CHANNEL_LPF=1 turns the filter on below 20 kHz too, where the rate cannot realize the default either. */
    assert(dsd_setenv("DSD_NEO_CHANNEL_LPF", "1", 1) == 0);
    dsd_neo_config_init();
    m = monitor_metrics(12500, 0, 12000);
    m.output_kind = DSD_FRONTEND_RTL_OUTPUT_SYMBOL_CQPSK;
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == dsd_channel_lpf_legacy_wide_width_hz(12000) && view.width_hz < 12000 && view.dsp_limited);
    /* A running stream that has published its own decision is not second-guessed, by the environment either. */
    m.channel_lpf_default = DSD_FRONTEND_CHANNEL_LPF_DEFAULT_OFF;
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == 12000 && view.dsp_limited);
    assert(dsd_unsetenv("DSD_NEO_CHANNEL_LPF") == 0);
    dsd_neo_config_init();

    /* The stream decided the filter when its configuration started, from the rate it started at, and keeps that
       decision at whatever rate the device delivers. A SoapySDR device opened at a 16 kHz DSP bandwidth that delivers
       31.25 kHz runs the unset default with no channel filter, so the monitor it returns to is the rate, not the 16 kHz
       a start at 31.25 kHz would filter... */
    m = monitor_metrics(12500, 0, 31250);
    m.output_kind = DSD_FRONTEND_RTL_OUTPUT_SYMBOL_CQPSK;
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == 16000 && !view.dsp_limited); /* not published yet: predicted from the rate */
    m.channel_lpf_default = DSD_FRONTEND_CHANNEL_LPF_DEFAULT_OFF;
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == 31250 && view.dsp_limited && view.configured_hz == 0);
    expect_reading(&view, "31.25 kHz (DSP-limited)");
    /* ...and one that started at 48 kHz still runs it at 12 kHz, through the legacy WIDE plan. */
    m = monitor_metrics(12500, 0, 12000);
    m.output_kind = DSD_FRONTEND_RTL_OUTPUT_SYMBOL_CQPSK;
    m.channel_lpf_default = DSD_FRONTEND_CHANNEL_LPF_DEFAULT_ON;
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == dsd_channel_lpf_legacy_wide_width_hz(12000) && view.dsp_limited);
    /* The decision applies to the unset default only: an explicit width always runs the filter. */
    opts->analog_nfm_bandwidth_hz = 8000;
    m = monitor_metrics(12500, 0, 31250);
    m.output_kind = DSD_FRONTEND_RTL_OUTPUT_SYMBOL_CQPSK;
    m.channel_lpf_default = DSD_FRONTEND_CHANNEL_LPF_DEFAULT_OFF;
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == 8000 && !view.dsp_limited);
    opts->analog_nfm_bandwidth_hz = 0;

    /* A typed digital row on the analog session: the configured preset is still NFM, and the row's front end filters
       with the row's profile, so the configured width shows, the one the row's leave returns to. */
    opts->analog_nfm_bandwidth_hz = 20000;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR) == 0);
    assert(dsd_scan_mode_options(opts, state, NULL) == 0);
    assert(opts->analog_only == 0);
    m = monitor_metrics(12500, 0, 48000);
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.shown && view.width_hz == 20000 && view.configured_hz == 20000);
    expect_reading(&view, "20 kHz");
    /* A frontend snapshot pair reads the same as the live state. */
    dsd_state* copy = (dsd_state*)calloc(1, sizeof(*copy));
    assert(copy);
    dsd_scan_mode_copy_snapshot(copy, state);
    assert(dsd_app_analog_width_view_get(opts, copy, &m, &view) == 0);
    assert(view.shown && view.width_hz == 20000);
    /* The unset default under the row at a 12 kHz DSP rate reads as what the leave returns to: the rate itself,
       DSP-limited, not the 16 kHz the rate cannot filter. An explicit width the rate filters stays the width. Under a
       live scope the configured width is the scope's baseline, which the width command edits. */
    assert(dsd_scan_mode_set_configured_nfm_bandwidth(opts, state, 0) == 1);
    m = monitor_metrics(12500, 0, 12000);
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.shown && view.width_hz == 12000 && view.dsp_limited && view.configured_hz == 0);
    expect_reading(&view, "12 kHz (DSP-limited)");
    assert(dsd_scan_mode_set_configured_nfm_bandwidth(opts, state, 8000) == 1);
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == 8000 && !view.dsp_limited && view.max_hz == 9600);
    assert(dsd_scan_mode_set_configured_nfm_bandwidth(opts, state, 20000) == 1);
    dsd_scan_mode_leave(opts, state);
    assert(opts->analog_only == 1);

    /* Issue #526: an nfm row with its own width on the analog session. The row's width is in force (the front end's
       report while it runs the monitor, the row's own with no stream), and the reading names the configured width the
       leave returns to; the configured width stays what the controls edit, however dsd_opts reads under the row. */
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 12500;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_scan_mode_options(opts, state, &row) == 0);
    assert(opts->analog_nfm_bandwidth_hz == 12500);
    assert(dsd_app_analog_width_view_get(opts, state, NULL, &view) == 0);
    assert(view.shown && view.row_analog && view.row_override && view.row_hz == 12500);
    assert(view.width_hz == 12500 && view.configured_hz == 20000 && !view.dsp_limited);
    expect_reading(&view, "12.5 kHz (row; default 20 kHz)");
    m = monitor_metrics(12500, 0, 48000);
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == 12500 && view.max_hz == 42000);
    expect_reading(&view, "12.5 kHz (row; default 20 kHz)");
    char notice[96];
    assert(dsd_app_analog_width_edit_notice(opts, state, DSD_ANALOG_DEMOD_FM, notice, sizeof notice) == 0);
    assert(strcmp(notice, "Default NFM bandwidth -> 20 kHz; this channel overrides it (12.5 kHz)") == 0);
    /* The unset default under the row is named by the width it gives, and never read as the DSP-limited default. */
    assert(dsd_scan_mode_set_configured_nfm_bandwidth(opts, state, 0) == 0);
    m = monitor_metrics(12500, 0, 16000);
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.width_hz == 12500 && !view.dsp_limited && view.configured_hz == 0);
    expect_reading(&view, "12.5 kHz (row; default 16 kHz)");
    assert(dsd_app_analog_width_edit_notice(opts, state, DSD_ANALOG_DEMOD_FM, notice, sizeof notice) == 0);
    assert(strcmp(notice, "Default NFM bandwidth -> default; this channel overrides it (12.5 kHz)") == 0);
    /* A row without a width runs the configured one, which reads and notifies as it would outside a row. */
    assert(dsd_scan_mode_set_configured_nfm_bandwidth(opts, state, 20000) == 0);
    assert(dsd_scan_mode_options(opts, state, NULL) == 0);
    m = monitor_metrics(20000, 0, 48000);
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.row_analog && !view.row_override && view.width_hz == 20000 && view.configured_hz == 20000);
    expect_reading(&view, "20 kHz");
    assert(dsd_app_analog_width_edit_notice(opts, state, DSD_ANALOG_DEMOD_FM, notice, sizeof notice) == 0);
    assert(strcmp(notice, "Applied: NFM bandwidth -> 20 kHz") == 0);
    dsd_scan_mode_leave(opts, state);
    assert(opts->analog_only == 1 && opts->analog_nfm_bandwidth_hz == 20000);

    /* ...and on a digital session, where the nfm row is the only analog receiver: shown while it is on air, with the
       configured width it would otherwise run, and nothing once it leaves. */
    opts->analog_only = 0;
    opts->frame_dmr = 1;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_scan_mode_options(opts, state, &row) == 0);
    assert(dsd_app_analog_width_view_get(opts, state, NULL, &view) == 0);
    assert(!view.shown && view.row_analog && view.row_override && view.kind == DSD_ANALOG_DEMOD_FM);
    expect_reading(&view, "12.5 kHz (row; default 20 kHz)");
    dsd_scan_mode_leave(opts, state);
    assert(dsd_app_analog_width_view_get(opts, state, NULL, &view) == 0);
    assert(!view.shown && !view.row_analog && !view.row_override && view.width_hz == 0);
    expect_reading(&view, "");
    opts->analog_only = 1;
    opts->frame_dmr = 0;

    /* ...and the reverse: a digital configured preset under a row whose options read analog shows nothing. */
    opts->analog_only = 0;
    opts->frame_dmr = 1;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_INHERIT) == 0);
    opts->analog_only = 1;
    assert(dsd_app_analog_width_view_get(opts, state, NULL, &view) == 0);
    assert(!view.shown);
    dsd_scan_mode_leave(opts, state);
    opts->analog_only = 1;
    opts->frame_dmr = 0;

    /* No state yet (no snapshot published): no scan scope, the options decide. */
    assert(dsd_app_analog_width_view_get(opts, NULL, NULL, &view) == 0);
    assert(view.shown && view.width_hz == 20000);

    /* PCM input: the width filters nothing, so none is in force; the configured one is still published. */
    opts->audio_in_type = AUDIO_IN_PULSE;
    m = monitor_metrics(12500, 0, 48000);
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.shown && !view.radio_input && view.width_hz == 0 && view.max_hz == 0 && view.configured_hz == 20000);
    expect_reading(&view, "not used on PCM input");
    opts->audio_in_type = AUDIO_IN_RTL;

    /* The M17 encoder rides the monitor output without being the analog receiver. */
    opts->m17encoder = 1;
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(!view.shown && view.width_hz == 0);
    opts->m17encoder = 0;

    /* The rate the RTL DSP bandwidth gives an input where it sets one, classified as the stream classifies the
       input: RTL-SDR and rtl_tcp specs, and any other device string on an RTL input. */
    assert(dsd_app_analog_rtl_bw_rate_hz("rtl:0:146.52M", AUDIO_IN_NULL, 24) == 24000);
    assert(dsd_app_analog_rtl_bw_rate_hz("rtltcp:127.0.0.1:1234", AUDIO_IN_RTL, 16) == 16000);
    assert(dsd_app_analog_rtl_bw_rate_hz("pulse", AUDIO_IN_RTL, 12) == 12000);
    assert(dsd_app_analog_rtl_bw_rate_hz(NULL, AUDIO_IN_RTL, 48) == 48000);
    assert(dsd_app_analog_rtl_bw_rate_hz("pulse", AUDIO_IN_PULSE, 48) == 0);
    assert(dsd_app_analog_rtl_bw_rate_hz("soapy", AUDIO_IN_RTL, 48) == 0);
    assert(dsd_app_analog_rtl_bw_rate_hz("airspy:serial=0123456789abcdef", AUDIO_IN_RTL, 48) == 0);
    assert(dsd_app_analog_rtl_bw_rate_hz("iqreplay:/tmp/capture.iq.json", AUDIO_IN_RTL, 48) == 0);
    assert(dsd_app_analog_rtl_bw_rate_hz("rtl", AUDIO_IN_RTL, 0) == 0);
    assert(dsd_app_analog_rtl_bw_rate_hz("rtl", AUDIO_IN_RTL, -24) == 0);
    /* A loaded config keeps any integer: saturated, never overflowed. */
    assert(dsd_app_analog_rtl_bw_rate_hz("rtl", AUDIO_IN_RTL, INT_MAX / 1000) == (INT_MAX / 1000) * 1000);
    assert(dsd_app_analog_rtl_bw_rate_hz("rtl", AUDIO_IN_RTL, INT_MAX / 1000 + 1) == INT_MAX);
    assert(dsd_app_analog_rtl_bw_rate_hz("rtl", AUDIO_IN_RTL, INT_MAX) == INT_MAX);

    /* The configured width, spelled one way everywhere: the value, or "default". */
    expect_setting(12500, "12.5 kHz");
    expect_setting(11250, "11.25 kHz");
    expect_setting(16000, "16 kHz");
    expect_setting(0, "default");
    expect_setting(-1, "default");

    /* The edit notice outside a scope: the configured width as set. */
    char applied[96];
    opts->analog_nfm_bandwidth_hz = 11250;
    assert(dsd_app_analog_width_edit_notice(opts, state, DSD_ANALOG_DEMOD_FM, applied, sizeof applied) == 0);
    assert(strcmp(applied, "Applied: NFM bandwidth -> 11.25 kHz") == 0);
    opts->analog_nfm_bandwidth_hz = 0;
    assert(dsd_app_analog_width_edit_notice(opts, state, DSD_ANALOG_DEMOD_FM, applied, sizeof applied) == 0);
    assert(strcmp(applied, "Applied: NFM bandwidth -> default") == 0);
    /* The notice names the kind the command edited, not the configured preset's: an NFM width edit on an AM session
       reads the NFM width, never the AM one the view shows. */
    opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    opts->analog_am_bandwidth_hz = 10000;
    opts->analog_nfm_bandwidth_hz = 12500;
    assert(dsd_app_analog_width_view_get(opts, state, NULL, &view) == 0);
    assert(view.kind == DSD_ANALOG_DEMOD_AM && view.configured_hz == 10000);
    assert(dsd_app_analog_width_edit_notice(opts, state, DSD_ANALOG_DEMOD_FM, applied, sizeof applied) == 0);
    assert(strcmp(applied, "Applied: NFM bandwidth -> 12.5 kHz") == 0);
    assert(dsd_app_analog_width_edit_notice(opts, state, DSD_ANALOG_DEMOD_AM, applied, sizeof applied) == 0);
    assert(strcmp(applied, "Applied: AM bandwidth -> 10 kHz") == 0);
    /* ...and under an nfm row with its own width on that AM session, only the NFM edit is shadowed. */
    dsd_scan_option_values am_session_row = {0};
    am_session_row.present = DSD_SCAN_OPT_BANDWIDTH;
    am_session_row.channel_bw_hz = 20000;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_scan_mode_options(opts, state, &am_session_row) == 0);
    assert(dsd_app_analog_width_edit_notice(opts, state, DSD_ANALOG_DEMOD_FM, applied, sizeof applied) == 0);
    assert(strcmp(applied, "Default NFM bandwidth -> 12.5 kHz; this channel overrides it (20 kHz)") == 0);
    assert(dsd_app_analog_width_edit_notice(opts, state, DSD_ANALOG_DEMOD_AM, applied, sizeof applied) == 0);
    assert(strcmp(applied, "Applied: AM bandwidth -> 10 kHz") == 0);
    dsd_scan_mode_leave(opts, state);
    assert(opts->analog_demod == DSD_ANALOG_DEMOD_AM && opts->analog_nfm_bandwidth_hz == 12500);
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->analog_am_bandwidth_hz = 0;
    opts->analog_nfm_bandwidth_hz = 0;
    assert(dsd_app_analog_width_edit_notice(NULL, state, DSD_ANALOG_DEMOD_FM, applied, sizeof applied) == -1
           && applied[0] == '\0');
    assert(dsd_app_analog_width_edit_notice(opts, state, -1, applied, sizeof applied) == -1 && applied[0] == '\0');
    assert(dsd_app_analog_width_edit_notice(opts, state, DSD_ANALOG_DEMOD_FM, NULL, 0) == -1);
    /* Each kind's configured width reads the same whichever preset runs. */
    opts->analog_nfm_bandwidth_hz = 12500;
    opts->analog_am_bandwidth_hz = 8000;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    assert(dsd_app_analog_width_setting_hz(opts, DSD_ANALOG_DEMOD_FM) == 12500);
    assert(dsd_app_analog_width_setting_hz(opts, DSD_ANALOG_DEMOD_AM) == 8000);
    opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    assert(dsd_app_analog_width_setting_hz(opts, DSD_ANALOG_DEMOD_FM) == 12500);
    assert(dsd_app_analog_width_setting_hz(opts, DSD_ANALOG_DEMOD_AM) == 8000);
    assert(dsd_app_analog_width_setting_hz(NULL, DSD_ANALOG_DEMOD_AM) == 0);

    /* Issue #524: which width the controls offer. The width of the configured preset's kind always, on a radio; the
       other kind's while an explicit one is set, since a switch to that kind is held to it; and AM's unset default
       where the DSP rate cannot filter its 6 kHz channel but can filter a narrower AM width, so the width the refusal
       of a switch to AM says to narrow can be narrowed before the switch. The rate bounds the steps whatever the
       preset: the running stream's demod rate, else the rate the RTL DSP bandwidth sets. */
    opts->analog_nfm_bandwidth_hz = 0;
    opts->analog_am_bandwidth_hz = 0;
    opts->analog_only = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    m = monitor_metrics(12500, 0, 48000);
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(dsd_app_analog_width_offered(opts, &view, DSD_ANALOG_DEMOD_FM) == 1);
    assert(dsd_app_analog_width_offered(opts, &view, DSD_ANALOG_DEMOD_AM) == 0);
    /* An I/Q replay or a device forcing a 7.5 kHz demod rate filters up to 5.55 kHz: AM's default is refused there,
       an explicit 5 kHz is not. */
    m = monitor_metrics(12500, 0, 7500);
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(view.max_hz == dsd_analog_width_max_for_rate(7500) && view.max_hz == 5550);
    assert(dsd_app_analog_width_offered(opts, &view, DSD_ANALOG_DEMOD_AM) == 1);
    opts->analog_only = 0;
    opts->frame_dmr = 1;
    m.output_kind = DSD_FRONTEND_RTL_OUTPUT_SYMBOL_CQPSK;
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(!view.shown && view.width_hz == 0 && view.max_hz == 5550);
    assert(dsd_app_analog_width_offered(opts, &view, DSD_ANALOG_DEMOD_AM) == 1);
    assert(dsd_app_analog_width_offered(opts, &view, DSD_ANALOG_DEMOD_FM) == 0);
    /* A rate that filters no AM width at all: narrowing cannot help, and the refusal says to keep a wider rate. */
    m.demod_rate_hz = 6000;
    assert(dsd_app_analog_width_view_get(opts, state, &m, &view) == 0);
    assert(dsd_app_analog_width_offered(opts, &view, DSD_ANALOG_DEMOD_AM) == 0);
    /* No stream: an RTL-SDR input's DSP bandwidth sets the rate, whose widest width at 8 kHz is AM's 6 kHz default. */
    assert(dsd_app_analog_width_view_get(opts, state, NULL, &view) == 0);
    assert(view.max_hz == 42000 && dsd_app_analog_width_offered(opts, &view, DSD_ANALOG_DEMOD_AM) == 0);
    opts->rtl_dsp_bw_khz = 8;
    assert(dsd_app_analog_width_view_get(opts, state, NULL, &view) == 0);
    assert(view.max_hz == 6000 && dsd_app_analog_width_offered(opts, &view, DSD_ANALOG_DEMOD_AM) == 0);
    opts->rtl_dsp_bw_khz = 48;
    /* An explicit width of either kind is offered at any rate; on PCM input, and with no view, nothing is. */
    opts->analog_am_bandwidth_hz = 10000;
    opts->analog_nfm_bandwidth_hz = 12500;
    assert(dsd_app_analog_width_view_get(opts, state, NULL, &view) == 0);
    assert(dsd_app_analog_width_offered(opts, &view, DSD_ANALOG_DEMOD_AM) == 1);
    assert(dsd_app_analog_width_offered(opts, &view, DSD_ANALOG_DEMOD_FM) == 1);
    opts->audio_in_type = AUDIO_IN_PULSE;
    assert(dsd_app_analog_width_view_get(opts, state, NULL, &view) == 0);
    assert(view.max_hz == 0 && dsd_app_analog_width_offered(opts, &view, DSD_ANALOG_DEMOD_AM) == 0);
    opts->audio_in_type = AUDIO_IN_RTL;
    assert(dsd_app_analog_width_offered(opts, NULL, DSD_ANALOG_DEMOD_AM) == 0);
    assert(dsd_app_analog_width_offered(NULL, &view, DSD_ANALOG_DEMOD_AM) == 0);
    opts->analog_am_bandwidth_hz = 0;
    opts->analog_nfm_bandwidth_hz = 0;
    opts->frame_dmr = 0;
    opts->analog_only = 1;

    char out[8];
    assert(dsd_app_analog_width_view_get(NULL, state, NULL, &view) == -1 && !view.shown);
    assert(dsd_app_analog_width_view_get(opts, state, NULL, NULL) == -1);
    assert(dsd_app_analog_width_view_format(&view, NULL, 0) == -1);
    assert(dsd_app_analog_width_view_format(NULL, out, sizeof out) == -1 && out[0] == '\0');
    assert(dsd_app_analog_width_setting_format(0, NULL, 0) == -1);

    dsd_state_ext_free_all(copy);
    dsd_state_ext_free_all(state);
    free(copy);
    free(state);
    free(opts);
    return 0;
}
