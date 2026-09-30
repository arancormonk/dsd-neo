// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Engine-owned trunk tuning policy entry points.
 *
 * These functions implement retune policy and bookkeeping and are installed
 * into the runtime trunk tuning hook table during engine startup.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_TUNING_H_
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_TUNING_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

dsd_trunk_tune_result dsd_engine_trunk_tune_to_freq_request(dsd_opts* opts, dsd_state* state, long int freq,
                                                            int ted_sps, uint64_t request_id);
dsd_trunk_tune_result dsd_engine_trunk_tune_to_cc_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                                          uint64_t request_id);
dsd_trunk_tune_result dsd_engine_return_to_cc_request(dsd_opts* opts, dsd_state* state, uint64_t request_id);
/**
 * @brief Tune a scanner row or target (the -Y channel scan, --trunk-scan).
 *
 * Queues the receive profile the row runs on, bound to @p freq: an analog row (the analog FM monitor in @p opts,
 * issue #526) gets the analog family at its width and no symbol profile; a digital row its symbol profile, and the
 * digital family when the configured mode is digital and the row was timed for that family's landing or the front end
 * runs the analog family or outstanding work lands a family (dsd_engine_scan_retune_attaches_family(), issue #583). A
 * row width the front end refuses at the rate it runs fails the tune before any backend moves.
 */
dsd_trunk_tune_result dsd_engine_scan_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                                   uint64_t* out_request_id);
/** @brief DSP (demodulator) rate of a running RTL-family stream in Hz, which an analog row width must fit: the rate the
 * stream holds an analog request to (rtl_stream_get_request_rate_hz()), published from its start; 0 when no RTL stream
 * runs. */
int dsd_engine_scan_dsp_rate_hz(const dsd_opts* opts, const dsd_state* state);
/** @brief The live receive-family requests an RTL-family front end has accepted (rtl_stream_live_family_request_count()),
 * or 0 on any other input. A scanner row's retune queued before this number moved no longer lands the receive family,
 * or the symbol profile, that dsd_engine_scan_tune_to_freq() attached to it (issue #526). */
uint32_t dsd_engine_scan_family_requests(const dsd_opts* opts);
/** @brief The supersedes the stream has counted for retunes queued with no receive family
 * (rtl_stream_familyless_retune_supersedes(): every live analog family request, and every scan leave), or 0 on any
 * other input. A scanner row's retune that attaches no receive family (a typed digital row on an analog session),
 * queued before this number moved, no longer lands the symbol profile dsd_engine_scan_tune_to_freq() queued with it: a
 * live analog request asked for the monitor (issue #582). */
uint32_t dsd_engine_scan_familyless_retune_supersedes(const dsd_opts* opts);
/** @brief Supersede the retunes queued with no receive family so far (rtl_stream_supersede_familyless_retunes()), on an
 * RTL-family input; a no-op on any other. The -Y leave calls it before its own requests (issue #582). */
void dsd_engine_scan_supersede_familyless_retunes(const dsd_opts* opts);
/** @brief Whether dsd_engine_scan_tune_to_freq(), called with the options now in force and @p ted_sps, attaches a
 * receive family to the row's retune: an analog row's always does, and a digital row's with a symbol clock
 * (@p ted_sps > 0) when the configured mode is digital and the row was timed for the digital family's landing
 * (dsd_scan_mode_timed_digital_family(), issue #583), or the front end runs the analog family, or the retunes and
 * requests already outstanding land a family: the analog one, or the digital one, which a retune carrying it lands on
 * the digital family's prediction (rtl_stream_family_landing_after_pending()). That is the decision the tune's
 * preparation makes, and spends; this only reads it. Such a retune is superseded by any live family request made
 * before it lands (dsd_engine_scan_family_requests(), issue #526). One without a family lands its symbol profile
 * whatever the digital requests; only a live analog request, which asks for the monitor, or a scan leave supersedes it
 * (dsd_engine_scan_familyless_retune_supersedes(), issue #582). 0 on any input but an RTL-family one. */
int dsd_engine_scan_retune_attaches_family(const dsd_opts* opts, const dsd_state* state, int ted_sps);
/** @brief Once a scanner has left its rows, put a rigctl peer back (issue #526): a scan tune asks a peer that
 * demodulates audio input for an am row's AM, or an nfm row's own passband, and nothing else undoes that once the
 * scanner stops tuning. RestoreScanModulation() sends back each passband a row changed, as read from the peer before
 * the change, then asks for what the restored settings run: FM at -B, which without -B is the peer's own FM passband,
 * sent only when this client changed the peer. Best-effort; a no-op without rigctl. */
void dsd_engine_scan_rigctl_restore(const dsd_opts* opts, const dsd_state* state);
/** @brief Forget the frequency and -B the legacy rigctl leg last sent (issue #589): the untyped -Y step and the
 * direct control-channel return skip a repeat of either, which holds only for the connection they went out on. Call it
 * whenever opts->rigctl_sockfd becomes a new connection (the rigctl reconnect service does), since the peer of a new
 * connection, another one or the same one restarted, was sent neither. Engine start empties it too. */
void dsd_engine_rigctl_tune_cache_forget(void);
/**
 * @brief Release the call state a tuned voice channel owns, without tuning.
 *
 * Ends the canonical call rows with DSD_CALL_END_EXPLICIT, clears the VC frequency mirrors and
 * the crypto/audio gating state, and drops trunk_is_tuned; does not tune.
 */
void dsd_engine_release_tuned_call_state(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_TUNING_H_ */
