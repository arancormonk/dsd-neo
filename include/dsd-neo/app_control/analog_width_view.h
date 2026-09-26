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
 * runs at the DSP rate it returns to: where no channel filter runs there, the rate itself, and where the filter runs
 * but the rate cannot realize the default, the passband of the legacy WIDE plan that runs instead
 * (dsd_channel_lpf_legacy_wide_width_hz()), both DSP-limited. Whether the filter runs is the running stream's own
 * decision (dsd_frontend_metrics::channel_lpf_default), made from the rate its configuration started at and kept
 * whatever rate the device then delivers; before a stream has published it, the view predicts it as a start makes it
 * (DSD_NEO_CHANNEL_LPF, else a rate of 20 kHz or more). That rate is the
 * running stream's demod rate or, with no stream, the rate the next start runs at where the input's RTL DSP bandwidth
 * sets it (dsd_app_analog_rtl_bw_rate_hz()), which also bounds the widths the controls offer. On PCM input no channel
 * filter runs, so no width is in force.
 *
 * An analog scan row on air (an nfm row, issue #526) runs the analog family whatever the configured preset, so its
 * width is shown under a digital preset too. A row may set its own width (--nfm-bandwidth-hz): that is the width in
 * force until the row leaves, while the configured width stays what the controls edit and a save writes.
 *
 * This view owns those decisions, the reading's text ("12.5 kHz", "16 kHz (default)", "12 kHz (DSP-limited)",
 * "12.5 kHz (row; default 16 kHz)"), the width command's notice (for the kind the command edits), and the one spelling
 * of a configured width ("12.5 kHz", "default"). The terminal's status field, RTL menu row and its predicate, the width
 * command's toast, the services that hold a DSP rate to the configured width, and the Qt/Android Radio sheet all take
 * them from here. A reading carries at most one note: "(default)" marks the unset default, which a save leaves out and
 * which keeps its own filter rule, apart from an explicit 16 kHz. A scan row that sets its own width replaces that note
 * with its own, "(row; default X)", naming the configured width of the row's demodulator, which the row overrides and
 * the width controls edit, rather than adding a second one.
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
    int kind;             /**< dsd_analog_demod of the configured preset, or of the analog scan row on air. */
    int width_hz;         /**< The width in force; 0 when neither shown nor under an analog row, or on PCM input. */
    int configured_hz;    /**< The configured width, 0 for the default: what the controls edit and a save writes. */
    int max_hz;           /**< The widest width the DSP rate filters (the running stream's demod rate, else the
                               rate the RTL DSP bandwidth sets); 0 when not known. */
    int row_hz;           /**< The scan row's own width while row_override is set, else 0. */
    uint8_t shown;        /**< 1 under the configured analog preset (never for the M17 encoder's monitor). */
    uint8_t radio_input;  /**< 1 on a radio input, where the width is a channel filter; 0 on PCM input. */
    uint8_t dsp_limited;  /**< 1 when the DSP rate, not the channel filter, bounds width_hz (the unset default only). */
    uint8_t row_analog;   /**< 1 while an analog scan row is on air, whatever the configured preset (issue #526). */
    uint8_t row_override; /**< 1 while the row on air sets its own width (--nfm-bandwidth-hz), in force over the
                               configured one until the row leaves. */
} dsd_app_analog_width_view;

/**
 * @brief Fill @p out from decoder state, or from a frontend snapshot pair and the metrics taken with it.
 *
 * @p metrics may be NULL, which reads as no running stream: the configured width (or the row's own) then stands in,
 * bounded by the rate the RTL DSP bandwidth sets where it sets one. @p state may be NULL (no snapshot published yet),
 * which reads as no scan scope. The configured width comes from the scan scope's configured view while one is live,
 * since a row's own width runs over dsd_opts, and from dsd_opts otherwise; correct on the decoder thread while a scope
 * is suspended for a command, where dsd_opts holds the configured options and the row's width comes from its installed
 * options. Zeroes @p out first. configured_hz is filled whenever @p opts is given. Returns 0, or -1 when @p opts or
 * @p out is NULL.
 */
int dsd_app_analog_width_view_get(const dsd_opts* opts, const dsd_state* state, const dsd_frontend_metrics* metrics,
                                  dsd_app_analog_width_view* out);

/**
 * @brief Render the width in force: "12.5 kHz", "16 kHz (default)" when the configured width is the default,
 * "12 kHz (DSP-limited)" when the rate bounds it (which only the default does), "12.5 kHz (row; default 16 kHz)" while a
 * scan row sets its own width (the default named is the configured width, or the kind's default width when none is
 * set), and "not used on PCM input" on PCM. Empty when neither shown nor under an analog row. Returns 0, or -1 when
 * @p view or @p out is NULL or @p out_size is zero.
 */
int dsd_app_analog_width_view_format(const dsd_app_analog_width_view* view, char* out, size_t out_size);

/**
 * @brief Render the notice after the configured width of demodulator @p kind was edited.
 *
 * The width command names the kind it edits (DSD_APP_CMD_NFM_BANDWIDTH_SET edits DSD_ANALOG_DEMOD_FM's), whatever kind
 * the configured preset runs: the notice names that kind and its configured width, read as
 * dsd_app_analog_width_view_get() reads it (the scan scope's configured view while one is live, dsd_opts otherwise).
 * "Applied: NFM bandwidth -> 12.5 kHz" ("-> default" for the unset default), or, while the scan row on air sets its own
 * width of @p kind and so shadows the edit, "Default NFM bandwidth -> 16 kHz; this channel overrides it (12.5 kHz)".
 * @p state may be NULL (no scan scope). Returns 0, or -1 when @p opts or @p out is NULL, @p out_size is zero or @p kind
 * is no analog demodulator (@p out is then empty where it can be).
 */
int dsd_app_analog_width_edit_notice(const dsd_opts* opts, const dsd_state* state, int kind, char* out,
                                     size_t out_size);

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
