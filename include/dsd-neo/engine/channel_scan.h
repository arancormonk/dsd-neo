// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/** @file @brief Transactional entry for typed conventional scan rows. */
#ifndef DSD_NEO_ENGINE_CHANNEL_SCAN_H
#define DSD_NEO_ENGINE_CHANNEL_SCAN_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_options.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Enter the next eligible row. 1 committed, 0 parked/pending, -1 failed. Ignores hold for manual use. */
int dsd_engine_channel_scan_step(dsd_opts* opts, dsd_state* state);
/** Manual next-row action: skip zero-frequency placeholders in one bounded pass. */
int dsd_engine_channel_scan_step_manual(dsd_opts* opts, dsd_state* state);
/** Inspect row ownership without servicing the tune; nonzero while a request or retry is outstanding. */
int dsd_engine_channel_scan_waiting(const dsd_state* state);
/** Resolve an outstanding request before reading samples. Returns 1 while unsettled. */
int dsd_engine_channel_scan_pending(dsd_opts* opts, dsd_state* state);
/** Service a row transaction before dispatch/next sync search. Returns 1 when ready;
 * servicing any outstanding transaction clears synctype and returns 0 for a fresh hunt.
 * A completed tune without a row commit keeps decoding gated even when a subsequent
 * retry is rejected or deferred; ordinary scanning may still advance to recover. */
int dsd_engine_channel_scan_service_sync(dsd_opts* opts, dsd_state* state);
/** Cancel row ownership and restore configured settings. Late completions cannot adopt a row. */
void dsd_engine_channel_scan_leave(dsd_opts* opts, dsd_state* state);
/** Open the audio sink the row now on air plays through (issue #526): the analog monitor's for an analog row, the
 * digital voice output otherwise. A list mixing the families needs whichever the session did not open; each is opened
 * once, idempotently, and a failure is logged once and leaves that row silent. Call after the row's options. */
void dsd_engine_scan_ensure_output(dsd_opts* opts);
/** Warn, as one WARNING line beginning with @p label ("Scan channel 2 (154.430000 MHz)", "Trunk scan target 'fire'"),
 * about an analog (nfm) scan row or target whose squelch -- its own --squelch-db, else the configured one -- is off or at
 * -100 dB or below (issue #526): noise then holds it on air until the visit cap or a manual advance or avoid moves on.
 * Said once when a scan (or a new map or target list) starts; it does not depend on the DSP rate. @p row may be NULL
 * (no options). Returns 1 when it warned, else 0. */
int dsd_engine_scan_warn_analog_squelch(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row,
                                        const char* label);
/** Warn, in the same form, about an analog row's own width (--nfm-bandwidth-hz, issue #526): on an input with no
 * demodulator to apply it, or one @p dsp_rate_hz cannot filter, which is then skipped at every visit. Said when a scan
 * starts and again whenever the DSP rate changes; @p dsp_rate_hz 0 skips the rate check. Returns 1 when it warned. */
int dsd_engine_scan_warn_analog_width(const dsd_opts* opts, const dsd_scan_option_values* row, int dsp_rate_hz,
                                      const char* label);
#ifdef __cplusplus
}
#endif
#endif
