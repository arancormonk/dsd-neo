// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-neutral analog channel width readout: the width in force under the configured analog preset,
 * whether the DSP rate bounds it, and the configured width the controls edit (issue #525).
 *
 * The analog preset is the configured one (dsd_scan_mode_configured_view()): a typed digital scan row on an analog
 * session does not end it, and the row's leave returns to the configured width. The M17 encoder rides the monitor
 * output without being the analog receiver, so it has no width. While a running stream's options in force run the
 * analog family on the monitor output, the width in force is the one the front end reports, flagged when the DSP rate
 * rather than the channel filter bounds it; otherwise (no stream, a typed digital row filtering with its own profile)
 * it is the configured width, the kind's default when none is set. An unset NFM default reads as what the monitor
 * runs at the DSP rate it returns to: at a rate below the one from which the default filters (20 kHz), or one that
 * cannot filter it, the rate itself, DSP-limited. That rate is the running stream's demod rate or, with no stream, the
 * rate the next start runs at where the input's RTL DSP bandwidth sets it (dsd_app_analog_rtl_bw_rate_hz()), which
 * also bounds the widths the controls offer. On PCM input no channel filter runs, so no width is in force.
 *
 * This view owns those decisions, the reading's text ("12.5 kHz", "16 kHz (default)", "12 kHz (DSP-limited)") and the
 * one spelling of a configured width ("12.5 kHz", "default"). The terminal's status field, RTL menu row and its
 * predicate, the width command's toast, the services that hold a DSP rate to the configured width, and the Qt/Android
 * Radio sheet all take them from here. A reading carries at most one note: "(default)" marks the unset default, which
 * a save leaves out and which keeps its own filter rule, apart from an explicit 16 kHz. A scan row that sets its own
 * width (#526) replaces that note with its own rather than adding a second one.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_ANALOG_WIDTH_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_ANALOG_WIDTH_VIEW_H_

#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Buffer size that holds every text this view formats. */
#define DSD_APP_ANALOG_WIDTH_TEXT_MAX 48

/** Analog channel width readout. Widths are the full RF channel in Hz (runtime/analog_channel.h). */
typedef struct {
    int kind;            /**< dsd_analog_demod of the configured preset. */
    int width_hz;        /**< The width in force; 0 when not shown or on PCM input. */
    int configured_hz;   /**< The configured width, 0 for the default: what the controls edit and a save writes. */
    int max_hz;          /**< The widest width the DSP rate filters (the running stream's demod rate, else the
                              rate the RTL DSP bandwidth sets); 0 when not known. */
    uint8_t shown;       /**< 1 under the configured analog preset (never for the M17 encoder's monitor). */
    uint8_t radio_input; /**< 1 on a radio input, where the width is a channel filter; 0 on PCM input. */
    uint8_t dsp_limited; /**< 1 when the DSP rate, not the channel filter, bounds width_hz (the unset default only). */
} dsd_app_analog_width_view;

/**
 * @brief Fill @p out from decoder state, or from a frontend snapshot pair and the metrics taken with it.
 *
 * @p metrics may be NULL, which reads as no running stream: the configured width then stands in, bounded by the rate
 * the RTL DSP bandwidth sets where it sets one. @p state may be NULL
 * (no snapshot published yet), which reads as no scan scope. Correct on the decoder thread while a scan scope is
 * suspended for a command, where dsd_opts holds the configured options; no scan row sets a width yet, so the options
 * always hold the configured one. Zeroes @p out first. configured_hz is filled whenever @p opts is given, for the kind
 * of the configured preset. Returns 0, or -1 when @p opts or @p out is NULL.
 */
int dsd_app_analog_width_view_get(const dsd_opts* opts, const dsd_state* state, const dsd_frontend_metrics* metrics,
                                  dsd_app_analog_width_view* out);

/**
 * @brief Render the width in force: "12.5 kHz", "16 kHz (default)" when the configured width is the default,
 * "12 kHz (DSP-limited)" when the rate bounds it (which only the default does), and "not used on PCM input" on PCM.
 * Empty when not shown. Returns 0, or -1 when @p view or @p out is NULL or @p out_size is zero.
 */
int dsd_app_analog_width_view_format(const dsd_app_analog_width_view* view, char* out, size_t out_size);

/**
 * @brief Render a configured width: "12.5 kHz", or "default" for 0. The spelling every frontend uses for the setting
 * (the menu row, the width command's toast, a pending request on the Radio sheet).
 * Returns 0, or -1 when @p out is NULL or @p out_size is zero.
 */
int dsd_app_analog_width_setting_format(int configured_hz, char* out, size_t out_size);

/**
 * @brief The DSP rate, in Hz, an RTL DSP bandwidth of @p rtl_bw_khz gives an input, where that bandwidth sets the rate.
 *
 * It does on an RTL-SDR or rtl_tcp input. That is an "rtl" or "rtltcp" spec, and also any other device string on an RTL
 * input (@p audio_in_type AUDIO_IN_RTL), which the stream opens as an RTL-SDR: the terminal's Input > Switch source >
 * RTL-SDR row, say, leaves "pulse" there. The result is 0 elsewhere: a SoapySDR or Airspy device may force another
 * rate, an I/Q replay runs at its capture's, and PCM input has none. It is 0 too for @p rtl_bw_khz <= 0. A value too
 * large for a rate in Hz (a loaded config keeps any integer) saturates at INT_MAX, a rate no channel width can be
 * filtered at.
 *
 * @param audio_in_dev  The input's device string (dsd_opts.audio_in_dev); NULL reads as an RTL-SDR, as the stream reads
 *                      it.
 * @param audio_in_type The input's type (dsd_opts.audio_in_type).
 * @param rtl_bw_khz    The RTL DSP bandwidth in kHz.
 */
int dsd_app_analog_rtl_bw_rate_hz(const char* audio_in_dev, int audio_in_type, int rtl_bw_khz);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_ANALOG_WIDTH_VIEW_H_ */
