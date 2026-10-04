// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/** @file @brief Transactional entry for typed conventional scan rows. */
#ifndef DSD_NEO_ENGINE_CHANNEL_SCAN_H
#define DSD_NEO_ENGINE_CHANNEL_SCAN_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <stddef.h>
#include <stdint.h>
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
/** Cancel row ownership and restore configured settings. Late completions cannot adopt a row. Under an analog preset
 * on an RTL input, an active scan's leave asks the front end back onto the configured analog profile (kind and width);
 * that is the last receive request the leave makes, so rtl_stream_receive_request_seq() names it right after. Returns 1
 * when that request was queued, -1 when the front end refused it at once at the rate it publishes (a retune moved the
 * rate since the width was held to it; also with no front end hook installed), and 0 when the leave asked nothing of
 * the monitor: NULL arguments, no active scan, not RTL input, or a digital configured family. The interactive leaves
 * go through svc_leave_channel_scan(), which records the result for the decoder (issue #578). */
int dsd_engine_channel_scan_leave(dsd_opts* opts, dsd_state* state);
/** Count the analog (nfm or am) rows of the loaded channel map whose width the RTL front end refuses at DSP rate
 * @p dsp_rate_hz (issue #526): the row's own width of its kind (--nfm-bandwidth-hz, --am-bandwidth-hz), else the
 * configured width of its kind it runs (the AM default, 6 kHz, included), which that rate cannot filter, and any held
 * width -- an explicit one, or the AM default -- while DSD_NEO_CHANNEL_LPF=0 turns the channel filter off (with
 * @p dsp_rate_hz 0, only that). A placeholder row (frequency 0) is never tuned and never counted. The scanner skips
 * such a row at every visit. @p first_row (optional) receives the first one's index, and @p brief a short reason for it
 * ("NFM 20 kHz does not fit the 16 kHz DSP rate", "AM 6 kHz does not fit the 7.5 kHz DSP rate"). Read-only and silent:
 * a scan start names each row in the log. 0 on audio input, which applies no width. */
int dsd_engine_channel_scan_refused_rows(const dsd_opts* opts, const dsd_state* state, int dsp_rate_hz, int* first_row,
                                         char* brief, size_t brief_size);
/** Whether the -Y list has somewhere to take traffic the tone policy rejected (issue #527): a row other than the one on
 * air that a step can land on -- one with a frequency, not avoided, and not an analog row whose width the front end
 * refuses (skipped at every visit, like the rows dsd_engine_channel_scan_refused_rows() counts) -- and that would not
 * judge the same traffic the same way: one on another frequency, or on the frequency on air, one of another class (a
 * digital or am row, or one inheriting a decode mode other than the FM monitor) or whose tone policy passes the
 * traffic. A row on the frequency on air that runs the FM monitor under a policy that rejects the traffic too (the
 * same policy, as every row of a list without modes runs) is nowhere to go. The row on air is the one whose tune last
 * landed on this map, which a failed start does not change; for a list without modes, the one before lcn_freq_roll.
 * Without such a row a step would only land on the row on air, or on a row that rejects the traffic alike, end the
 * reception and judge the same traffic once more, so the scanner keeps it, muted. Works on both the typed and the
 * untyped (legacy) -Y list. 0 for NULL arguments. */
int dsd_engine_channel_scan_has_other_row(const dsd_opts* opts, const dsd_state* state);
/**
 * @brief Edit one field of a typed -Y row's own settings until the scanner leaves ("this channel", issue #518).
 *
 * @p session and @p row name the row as the frontends saw it (dsd_state::scan_row_session and scan_row_index): the
 * session is this scan's, so an edit opened before a leave or a map change, which leaves first, never lands on the next
 * scan. @p field is one DSD_SCAN_ROW_FIELD_* the row's class takes (dsd_scan_row_edit_fields(); no gain: a -Y retune
 * carries none), @p action a dsd_scan_row_edit_action with @p value for SET. A width the published DSP rate cannot
 * filter is refused. The row whose scope is in force runs the edit at once (its squelch reaches the demod; @p out says
 * whether to publish the width, and holds the previous edit for dsd_engine_channel_scan_restore_row_edit()); any other
 * row runs it from its next visit, and a tune staged meanwhile restages. Nothing is written to the map or saved.
 * Decoder thread only.
 *
 * @return a dsd_scan_row_edit_status: APPLIED, STORED, REFUSED (with @p out->err), STALE or UNAVAILABLE (no -Y scan).
 */
int dsd_engine_channel_scan_edit_row(dsd_opts* opts, dsd_state* state, uint32_t session, int row, uint32_t field,
                                     int action, const dsd_scan_row_edit_value* value, dsd_scan_row_edit_result* out);

/** Put the @p fields (DSD_SCAN_ROW_FIELD_*) of a -Y row's session edit back to those of @p edit, keeping its other
 * fields as they are now, and apply it as dsd_engine_channel_scan_edit_row() does: for a caller whose follow-up (the
 * width request) failed, which keeps any edit of another field made since. Same session and thread rules. */
int dsd_engine_channel_scan_restore_row_edit(dsd_opts* opts, dsd_state* state, uint32_t session, int row,
                                             uint32_t fields, const dsd_scan_row_edit* edit,
                                             dsd_scan_row_edit_result* out);

#ifdef __cplusplus
}
#endif
#endif
