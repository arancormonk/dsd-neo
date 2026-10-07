// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-neutral analog channel width readout: the width in force under the configured analog preset (NFM,
 * issue #525, or AM, issue #524), whether the DSP rate bounds it, and the configured width the controls edit.
 *
 * The analog preset is the configured one (dsd_scan_mode_configured_view()): a typed digital scan row on an analog
 * session does not end it, and the row's leave returns to the configured width. The M17 encoder rides the monitor
 * output without being the analog receiver, so it has no width. While a running stream's options in force run the
 * analog family on the monitor output with the configured kind, the width in force is the one the front end reports,
 * flagged when the DSP rate rather than the channel filter bounds it; otherwise (no stream, a typed digital row
 * filtering with its own profile, an FM <-> AM switch that has not landed yet) it is the configured width, the kind's
 * default when none is set. The unset AM default is its 6 kHz filter, which no DSP rate limits: a rate that cannot
 * filter it is refused rather than run without it. An unset NFM default reads as what the monitor
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
 * On PCM audio input with a rigctl peer (-U) that demodulates it (dsd_opts_rigctl_peer_demodulates(), issue #621) the
 * width is the passband the peer is asked for, and no DSP rate bounds it. There is a reading whenever the controls
 * offer a kind (dsd_app_analog_width_offered()), so a frontend that shows a passband beside them shows the one asked
 * for now. While the analog monitor of the view's kind runs, the reading is the request
 * dsd_rigctl_passband_for_kind() makes: the row's own width, the configured width, the AM default, or for FM -B
 * standing in for an unset width, else the peer's own passband. Off that monitor (a typed digital row on air under -fA,
 * a digital session with an explicit width set) the peer follows at -B, or its own passband without one, and that is
 * the reading, never the configured width, which nothing then asks for. The reading names its source ("12.5 kHz
 * (-B)", "peer's own"), and an unset NFM setting reads as what stands in for it ("-B 12.5 kHz"), since that is what an
 * edit to 0 asks for. Everything is computed from the options and the copied scan scope, so a frontend snapshot pair
 * reads as the decoder thread does.
 *
 * An analog scan row on air (an nfm or am row, issue #526) runs the analog family whatever the configured preset, so
 * its width is shown under a digital preset too. A row may set its own width (--nfm-bandwidth-hz, --am-bandwidth-hz):
 * that is the width in force until the row leaves, while the configured width stays what the controls edit and a save
 * writes.
 *
 * This view owns those decisions, the reading's text ("12.5 kHz", "16 kHz (default)", "12 kHz (DSP-limited)",
 * "12.5 kHz (row; default 16 kHz)", and on a rigctl peer "12.5 kHz (-B)", "peer's own"), the width command's notice
 * (for the kind the command edits), which kinds the controls offer, and the spelling of a configured width ("12.5 kHz",
 * "default"; on a rigctl peer an unset NFM width "-B 12.5 kHz" or "peer's own"). The terminal's status field, width
 * rows and their predicates, the width command's toast, the services that hold a DSP rate to the configured width, and
 * the Qt/Android sheets all take them from here. A reading carries at most one note: "(default)" marks the unset
 * default, which a save leaves out and which keeps its own filter rule, apart from an explicit 16 kHz. A scan row that
 * sets its own width replaces that note with its own, "(row; default X)", naming the configured width of the row's
 * demodulator, which the row overrides and the width controls edit, rather than adding a second one.
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
    int width_hz;         /**< The width in force; 0 when neither shown nor under an analog row, or on PCM input. On
                               a rigctl peer session the passband asked for whenever a kind is offered. */
    int configured_hz;    /**< The configured width, 0 for the default: what the controls edit and a save writes. */
    int max_hz;           /**< The widest width the DSP rate filters (the running stream's demod rate, else the
                               rate the RTL DSP bandwidth sets), under any preset on a radio input; 0 when not
                               known. */
    int row_hz;           /**< The scan row's own width while row_override is set, else 0. */
    uint8_t shown;        /**< 1 under the configured analog preset (never for the M17 encoder's monitor). */
    uint8_t radio_input;  /**< 1 on a radio input, where the width is a channel filter; 0 on PCM input. */
    uint8_t dsp_limited;  /**< 1 when the DSP rate, not the channel filter, bounds width_hz (the unset default only). */
    uint8_t row_analog;   /**< 1 while an analog scan row is on air, whatever the configured preset (issue #526). */
    uint8_t row_override; /**< 1 while the row on air sets its own width of @c kind (--nfm-bandwidth-hz or
                               --am-bandwidth-hz), in force over the configured one until the row leaves. */
    uint8_t peer_passband; /**< 1 on PCM audio input with a live rigctl peer, which demodulates it
                                (dsd_opts_rigctl_peer_demodulates(), issue #621): the width is the passband the peer
                                is asked for. radio_input, max_hz and dsp_limited are then 0. */
    int setmod_bw_hz;      /**< -B (dsd_opts::setmod_bw) on a peer session, the FM passband that stands in for an unset
                                NFM width; 0 when off, and on any other session. */
    uint8_t passband_source;   /**< dsd_rigctl_passband_source of the reading on a peer session whenever a kind is
                                    offered: ROW, CONFIGURED, AM_DEFAULT, SETMOD_BW or PEER_OWN; FOLLOW (0) with no
                                    reading, and on any other session. */
    uint8_t passband_in_force; /**< 1 on a peer session while the analog family runs @c kind now
                                    (dsd_opts_is_analog_family() and dsd_opts::analog_demod): an edit of the kind's
                                    width is asked of the peer at once. 0 while a typed digital row on air keeps the
                                    peer following at -B, and on any other session. */
    int unset_hz; /**< The width an unset setting of @c kind stands for (dsd_app_analog_width_unset_hz()): on a peer
                       session -B for NFM (0: the peer's own passband) and the 6 kHz default for AM; elsewhere the
                       kind's default width (16 kHz NFM, 6 kHz AM). */
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
 * @brief The channel width of analog @p kind (dsd_analog_demod) in @p opts, in Hz; 0 for its default.
 *
 * Whichever preset runs: the NFM width for DSD_ANALOG_DEMOD_FM, the AM width for DSD_ANALOG_DEMOD_AM. Outside a scan
 * scope, and while one is suspended, it is the setting the width commands edit and a save writes; under a live scan row
 * dsd_opts holds the options in force, a row's own width (issue #526) included, and the configured width is
 * dsd_scan_mode_configured_analog_width()'s. 0 when @p opts is NULL.
 */
int dsd_app_analog_width_setting_hz(const dsd_opts* opts, int kind);

/**
 * @brief Whether the controls offer the configured channel width of analog @p kind (dsd_analog_demod) for editing.
 *
 * On a radio input, where the width is a channel filter, and on audio input with a rigctl peer that demodulates it,
 * where it is the passband the peer is asked for (@c peer_passband, issue #621); never on other PCM input (@p view from
 * dsd_app_analog_width_view_get() for the same @p opts and @p state). There: the width of the configured preset's
 * kind, whatever an analog scan row on air runs meanwhile, and the kind that row runs (issue #526); the other kind's
 * (under a digital preset, both) while an explicit configured width of it is set
 * (dsd_scan_mode_configured_analog_width()), since a switch to that kind is held to it; and AM's unset default where
 * the DSP rate (@c max_hz) cannot filter its 6 kHz channel but filters a narrower AM width, since a switch to AM is
 * refused there with word to narrow the width (never on a peer session, where no rate is known). The unset NFM default
 * is never refused for its rate. The terminal's width rows and the Qt/Android sheets offer their controls by this.
 * @p state may be NULL (no scan scope). Returns 1 or 0 (0 for a NULL @p opts or @p view, or an invalid kind).
 */
int dsd_app_analog_width_offered(const dsd_opts* opts, const dsd_state* state, const dsd_app_analog_width_view* view,
                                 int kind);

/**
 * @brief Render the width in force: "12.5 kHz", "16 kHz (default)" when the configured width is the default,
 * "12 kHz (DSP-limited)" when the rate bounds it (which only the default does), "12.5 kHz (row; default 16 kHz)" while a
 * scan row sets its own width (the default named is the configured width, or the kind's default width when none is
 * set), and "not used on PCM input" on PCM without a rigctl peer. On a peer session (@c peer_passband) the passband
 * the peer is asked for, by its source: "12.5 kHz" (configured), "6 kHz (default)" (the AM default), "12.5 kHz (-B)",
 * "peer's own", and "12.5 kHz (row; default X)" with X the setting the row overrides as
 * dsd_app_analog_width_setting_text() spells it ("16 kHz", "-B 12.5 kHz", "peer's own"), the AM default as "6 kHz".
 * Empty off a peer session when neither shown nor under an analog row, and on a peer session when no kind is offered
 * (a reading exists there whenever one is, shown or not). Every text fits DSD_APP_ANALOG_WIDTH_TEXT_MAX. Returns 0, or
 * -1 when @p view or @p out is NULL or @p out_size is zero.
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
 * On a rigctl peer session while @p kind's monitor runs and no row width shadows it (@c passband_in_force, issue
 * #621), the edit is the passband the peer was asked for, named by the reading: "Applied: NFM passband -> 12.5 kHz",
 * "-> 12.5 kHz (-B)", "-> peer's own", "Applied: AM passband -> 6 kHz (default)".
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
 * @brief Render the configured width @p configured_hz of analog @p kind as the session reads it (issue #621): an
 * explicit width as dsd_app_analog_width_setting_format() does ("12.5 kHz"); an unset NFM width on a rigctl peer
 * session (@c peer_passband of @p view) as what stands in for it, "-B 12.5 kHz", or "peer's own" when -B is 0; any
 * other unset width "default". The spelling of the setting the terminal's rows and prompts and the Qt/Android sheets
 * show. Returns 0, or -1 (@p out empty where it can be) when @p view or @p out is NULL, @p out_size is zero or @p kind
 * is no analog demodulator.
 */
int dsd_app_analog_width_setting_text(const dsd_app_analog_width_view* view, int kind, int configured_hz, char* out,
                                      size_t out_size);

/**
 * @brief The width, in Hz, an unset setting of analog @p kind stands for in the session @p view describes (issue #621):
 * on a rigctl peer session (@c peer_passband) -B for NFM (0: the peer's own passband) and the 6 kHz default for AM;
 * elsewhere, and for a NULL @p view, the kind's default width (16 kHz NFM, 6 kHz AM). The width a stepper steps from
 * while a setting is unset, for either kind on one snapshot; @c unset_hz is this for @c kind. 0 for an invalid kind.
 */
int dsd_app_analog_width_unset_hz(const dsd_app_analog_width_view* view, int kind);

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
