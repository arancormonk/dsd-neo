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
 * digital family when the front end runs the analog one and the configured mode is digital. A row width the front end
 * refuses at the rate it runs fails the tune before any backend moves.
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
