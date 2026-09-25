// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/**
 * @file
 * @brief Private engine helpers the -Y scanner and the trunk-scan coordinator share for analog rows (issue #526).
 *
 * Defined in channel_scan.c; only channel_scan.c and trunk_scan.c call them.
 */
#ifndef DSD_NEO_SRC_ENGINE_SCAN_ANALOG_INTERNAL_H_
#define DSD_NEO_SRC_ENGINE_SCAN_ANALOG_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_options.h>

/** Open the audio sink the row now on air plays through: the analog monitor's for an analog row, the digital voice
 * output otherwise. A list mixing the families needs whichever the session did not open; each is opened once,
 * idempotently, and a failure is logged once and leaves that row silent. Call after the row's options. */
void dsd_engine_scan_ensure_output(dsd_opts* opts);

/** Warn, as one WARNING line beginning with @p label ("Scan channel 2 (154.430000 MHz)", "Trunk scan target 'fire'"),
 * about an analog (nfm) scan row or target whose squelch -- its own --squelch-db, else the configured one -- is off or
 * at -100 dB or below: noise then holds it on air until the visit cap or a manual advance or avoid moves on. Said once
 * when a scan (or a new map or target list) starts; it does not depend on the DSP rate. @p row may be NULL (no
 * options). Returns 1 when it warned, else 0. */
int dsd_engine_scan_warn_analog_squelch(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row,
                                        const char* label);

/** Warn, in the same form, about the width an analog row runs, which is then skipped at every visit: its own
 * --nfm-bandwidth-hz on an input with no demodulator to apply it, or one @p dsp_rate_hz cannot filter; or, for a row
 * that sets none, the configured NFM width it runs (dsd_engine_scan_configured_nfm_width_hz()) where @p dsp_rate_hz
 * cannot filter that. Said when a scan starts, again whenever the DSP rate changes, and for the rows without a width of
 * their own whenever the configured NFM width does; @p dsp_rate_hz 0 skips the rate check. @p row may be NULL (no
 * options). Returns 1 when it warned. */
int dsd_engine_scan_warn_analog_width(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row,
                                      int dsp_rate_hz, const char* label);

/** The explicit NFM width an analog row without one of its own runs: the configured width, from the scan scope's
 * configured view while one is live and dsd_opts otherwise; 0 for the unset default, which no DSP rate refuses, and
 * when @p opts is NULL. */
int dsd_engine_scan_configured_nfm_width_hz(const dsd_opts* opts, const dsd_state* state);

#endif /* DSD_NEO_SRC_ENGINE_SCAN_ANALOG_INTERNAL_H_ */
