// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/** @file @brief Transactional entry for typed conventional scan rows. */
#ifndef DSD_NEO_ENGINE_CHANNEL_SCAN_H
#define DSD_NEO_ENGINE_CHANNEL_SCAN_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
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
/** Count the nfm rows of the loaded channel map whose width the RTL front end refuses at DSP rate @p dsp_rate_hz
 * (issue #526): the row's own --nfm-bandwidth-hz, else the configured NFM width it runs, which that rate cannot filter,
 * and any explicit width while DSD_NEO_CHANNEL_LPF=0 turns the channel filter off (with @p dsp_rate_hz 0, only that).
 * The scanner skips such a row at every visit. @p first_row (optional) receives the first one's index, and @p brief a
 * short reason for it ("NFM 20 kHz does not fit the 16 kHz DSP rate"). Read-only and silent: a scan start names each
 * row in the log. 0 on audio input, which applies no width. */
int dsd_engine_channel_scan_refused_rows(const dsd_opts* opts, const dsd_state* state, int dsp_rate_hz, int* first_row,
                                         char* brief, size_t brief_size);
#ifdef __cplusplus
}
#endif
#endif
