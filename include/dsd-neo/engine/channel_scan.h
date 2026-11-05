// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/** @file @brief Transactional entry for typed conventional scan rows. */
#ifndef DSD_NEO_ENGINE_CHANNEL_SCAN_H
#define DSD_NEO_ENGINE_CHANNEL_SCAN_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/scan_entry.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Enter the next eligible row. 1 committed, 0 parked/pending, -1 failed. Ignores hold for manual use. */
int dsd_engine_channel_scan_step(dsd_opts* opts, dsd_state* state);
/** Manual next-row action: dsd_engine_channel_scan_step_manual_dir() forward. */
int dsd_engine_channel_scan_step_manual(dsd_opts* opts, dsd_state* state);
/** Manual step one row in direction @p dir (1 Next, -1 Previous; issue #666), skipping zero-frequency placeholders and
 * avoided rows in one bounded pass and keeping rows that share a frequency (each may run another mode, options or
 * keys). Forward starts at the cursor (lcn_freq_roll). Back starts before the row a refused Previous left to retry,
 * else before the row heard (the last row a step landed: dsd_state_lcn_on_air_row()), wrapping, else at the last row.
 * A row that lands moves the cursor past it either way; a refused one moves the cursor past it going forward, and
 * going back keeps the cursor and is the row the next Previous retries; a deferral changes nothing. While a row's tune
 * is outstanding or owed a restage, the press services it instead, either way. 1 committed, 0 parked/pending or
 * nothing to land on, -1 failed. */
int dsd_engine_channel_scan_step_manual_dir(dsd_opts* opts, dsd_state* state, int dir);
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
 * the monitor: NULL arguments, no active scan, not RTL input, or a digital configured family. A paused scan
 * (dsd_state::lcn_scan_paused, no resume pending; issue #671) is not active: its pause already left the row, so its
 * leave only drops the list state the pause kept, and ends no call, resets no acquisition and asks the monitor for
 * nothing of the manual session. A running untyped list of the user's own (issue #644 B-3) has no scope, yet its leave
 * ends its calls, and changes nothing else (its rows ran the configured settings); a caller that stops the scan keeps
 * scanner_mode and the row keys until after the leave, so the calls end under the row heard and its keys. An entry
 * waiting on its row's landing is cancelled, and an entry hold goes with the scan (lcn_scan_hold and its mark off;
 * issue #644), while an ordinary hold stays as it always has. The interactive leaves go through
 * svc_leave_channel_scan(), which records the result for the decoder (issue #578) and then clears the pause. */
int dsd_engine_channel_scan_leave(dsd_opts* opts, dsd_state* state);
/** End the calls of a running -Y list of the user's own, typed or untyped, now, as its leave would (issue #644): for a
 * caller about to change what names, keys or judges them (a config apply that turns the scan off suspends a typed row's
 * scope, writes its own keys and policy and takes the scanner flag down before the leave runs). The calls commit under
 * the row heard and whatever row settings are in force. 1 when it ended them; 0 with no such list running (a paused
 * one, a resume still pending, learned channels only, a trunk scan), when it does nothing. Decoder thread, under the
 * P25 SM tick guard a guarded command holds. */
int dsd_engine_channel_scan_end_running_calls(dsd_opts* opts, dsd_state* state);
/** Pause a running -Y scan, typed or untyped (issue #671): the operator takes the tuner and the scan waits for a
 * resume. The calls heard end and commit under the row on air's label, keys and policy, whatever the list (an
 * untyped one has no scope); with @p in_place (no tune follows: a tuner release) the carrier's decoding is forgotten
 * next, as a carrier boundary would, since none runs. Then the configured decoder, receive profile and rigctl
 * passband come back as at a leave, and the row keys leave. Kept: the list, the cursor (lcn_freq_roll) and the row
 * heard (issue #666), the avoids, the session edits and their session. Dropped: a row tune staged or pending (nothing
 * commits after the pause, lcn_scan_session moves), a retry, a commit owed, the row on air and its staged options and
 * keys, an operator hold and its entry mark, and an entry waiting on its row's landing (issue #644). Leaves
 * scanner_mode 0, lcn_scan_paused and operator_tuned 1. Also takes back a resume
 * still pending. Nothing to do (0) without a -Y scan running or resuming (dsd_channel_scan_owns_tuner()). Returns
 * what the return to the configured profile asked of the monitor, as dsd_engine_channel_scan_leave() does; the
 * interactive pause goes through svc_pause_channel_scan(). Safe under the P25 SM tick guard: nothing under it takes
 * the guard its caller holds. Decoder thread. */
int dsd_engine_channel_scan_pause(dsd_opts* opts, dsd_state* state, int in_place);

/** Start a stopped -Y scan (`y`; issue #644 B-4): the calls heard so far end and commit under the attribution they
 * began with (the operator's tuner after a manual tune, no row), then the scan owns the tuner: scanner_mode 1 and
 * operator_tuned 0, so the calls heard next are the scan's, and frontends show a scan, not a manual session. The scan's
 * first step tunes as any timed step does. With no list of the user's own (dsd_state_trunk_lcn_user_list_present())
 * the scan never moves the receiver: only scanner_mode is set, no call ends and operator_tuned stays as it is until
 * the scan first lands a row (every landing clears it, after the boundary has ended the operator's calls). A
 * paused scan, or one whose resume is pending, is refused (nothing changes): it resumes through
 * dsd_engine_channel_scan_resume(). Under the P25 SM tick guard the guarded scanner commands hold. Decoder thread. */
void dsd_engine_channel_scan_start(dsd_opts* opts, dsd_state* state);

/** dsd_engine_channel_scan_resume() results. */
enum {
    DSD_SCAN_RESUME_LANDED = 0,  /**< The row's tune landed: the scan runs (scanner_mode 1, no longer paused). */
    DSD_SCAN_RESUME_PENDING = 1, /**< The row's tune is outstanding: the scan stays paused, resume pending, until it
                                      lands (then it runs) or fails (then it stays paused, with a toast). */
    DSD_SCAN_RESUME_NO_ROW = 2,  /**< No row to resume on (every row avoided or blank): nothing changed. */
    DSD_SCAN_RESUME_REFUSED = 3, /**< The row's tune was refused: the scan stays paused, its row attempt undone. */
};

/** Resume a paused -Y scan on the next row from the kept cursor that a step can land on (issue #671): one with a
 * frequency, not avoided, and in a typed list not an analog row the front end refuses before any backend moves. With
 * none it never starts: NO_ROW, and the manual session's calls go on. Otherwise the manual session's calls end first
 * under no row's name, trunking stays off and the legacy tune caches are forgotten, then the row is tuned as a step
 * would tune it (a typed row through its staged scope, an untyped one through the untyped step), with a new
 * lcn_scan_session. Its landing runs the outgoing boundary, installs the row with fresh scan clocks, and only then
 * publishes the running flags (scanner_mode 1, lcn_scan_paused and operator_tuned 0). Until then the scan is
 * resume-pending: still paused, no row named. A refused tune, or settings that could not be set up, leave it paused
 * (REFUSED): a typed attempt is undone by the pause, through the hook app-control installs to record its return to
 * the monitor (<dsd-neo/runtime/channel_scan_hooks.h>), keeping the session edits and the cursor. A resume already
 * pending answers PENDING and changes nothing; a scan already running answers LANDED and changes nothing. NO_ROW and
 * every REFUSED, a scan neither paused nor running and one under a trunk scan included, put the reason on the status
 * line. Safe under the P25 SM tick guard the guarded scanner commands hold: nothing under it takes the guard again.
 * Decoder thread. */
int dsd_engine_channel_scan_resume(dsd_opts* opts, dsd_state* state);

/** Send the running -Y scan to @p row and hold it there (issue #644 D1): the row a frequency typed by hand picked
 * (dsd_engine_scan_entry_match()). Only a row a step can land on: one with a frequency, not avoided, and not an analog
 * row skipped at every visit for its width. Any other row, and a scan that is not running (paused or a resume pending:
 * see dsd_engine_channel_scan_resume_at(); stopped; under a trunk scan; no list of the user's own), is REFUSED, and
 * nothing changes.
 * - A row confirmed on air (dsd_engine_channel_scan_row_confirmed()) needs no tune: the hold only (HOLDING).
 * - A typed list stages the row apart from any row or tune still outstanding, and retires that one only once its own
 *   tune is taken or moved a backend: a preparation that fails (SETUP_FAILED) or a tune refused or deferred before
 *   any backend moved (REFUSED, DEFERRED) leaves the scan, its row in flight and an earlier entry as they were.
 * - An untyped list walks to the row as `L` does (dsd_engine_channel_scan_walk_row(): both front ends, the row
 *   committed on its confirmed landing); a refusal or deferral before any leg moved changes nothing either (a
 *   refusal its RTL leg answered still lets go of a tune noted before it, as Next's does).
 * - Either way, a refusal or deferral before any backend moved keeps the row confirmed on air confirmed: the receiver
 *   is still on it. Only a tune taken, or a leg that moved, forgets that landing.
 * - A refusal after a leg moved is the step's: the outgoing calls end, a refusal passes the row (the cursor past it).
 * The hold (lcn_scan_hold) and its entry mark (lcn_scan_entry_hold) are set on the row's confirmed landing only: at
 * once (HOLDING), or when the tune outstanding (PENDING) lands while the entry is still the one in force. Another
 * step, Next, Previous or an avoid's step, `Y` (dsd_engine_channel_scan_entry_cancel()), a later go-to that is taken, a
 * pause and a stop cancel it, and so does that tune's failure. The row lands as a step's does, so the scan continues
 * from it. Under the P25 SM tick guard the scanner commands hold. Decoder thread. */
dsd_scan_goto_result dsd_engine_channel_scan_goto_row(dsd_opts* opts, dsd_state* state, int row);

/** Resume the paused -Y scan at @p row and hold it there (issue #644 D1), as dsd_engine_channel_scan_resume() resumes
 * on the row it picks: the same row rule as dsd_engine_channel_scan_goto_row() (else REFUSED, nothing changed), the
 * manual session's calls ended, a new scan session, and failures that leave it paused with the reason on the status
 * line (a typed attempt undone through the pause hook). HOLDING once the row landed, PENDING while its tune is
 * outstanding (the scan is resume-pending until it lands, then runs, held), REFUSED, DEFERRED or SETUP_FAILED when it
 * stays paused. Over a resume still pending: a typed list stages the row apart, so a preparation that fails or a tune
 * refused or deferred before any backend moved keeps that resume as it was; once the row's tune is taken it replaces
 * that resume, and a failure after a backend moved leaves the scan paused. An untyped list tunes the row as its resume
 * does: landed, the scan runs, held; pending, it replaces that resume (a new scan session its tune is rebound to, so
 * the earlier resume's tune, whatever it comes to, is no longer the scan's); refused or deferred before any leg moved,
 * that resume stands (a refusal its RTL leg answered lets go of the earlier tune, whose drop then ends that resume);
 * after a leg moved, the scan stays paused. Under the P25 SM tick guard the scanner commands hold. Decoder thread. */
dsd_scan_goto_result dsd_engine_channel_scan_resume_at(dsd_opts* opts, dsd_state* state, int row);

/** Release an entry hold (`y`, SCAN_RESUME; issue #644 D1): the hold and its entry mark go, and the dwell, visit and
 * voice-gate windows start again as at `Y` off, so the row gets a full hangtime and the scan continues from it. An
 * entry still waiting on its row's landing is cancelled too. Nothing else without an entry hold: an ordinary hold
 * stays. Decoder thread. */
void dsd_engine_channel_scan_entry_release(dsd_opts* opts, dsd_state* state);

/** `Y` over an entry (issue #644): an entry still waiting on its row's landing is cancelled, so its landing holds
 * nothing, and the entry mark goes; the hold itself is the caller's to toggle. Decoder thread. */
void dsd_engine_channel_scan_entry_cancel(dsd_state* state);

/** Whether an entry waits on its row's landing in this scan session (a go-to or resume-at that is PENDING): its tune
 * has neither landed nor failed, and nothing has cancelled it. 0 for a NULL @p state. Decoder thread. */
int dsd_engine_channel_scan_entry_pending(const dsd_state* state);

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
 * landed on this list, which a failed start does not change; for a list without modes, the row heard
 * (dsd_engine_channel_scan_on_air_row()).
 * Without such a row a step would only land on the row on air, or on a row that rejects the traffic alike, end the
 * reception and judge the same traffic once more, so the scanner keeps it, muted. Works on both the typed and the
 * untyped (legacy) -Y list. 0 for NULL arguments, and without a list of the user's own
 * (dsd_state_trunk_lcn_user_list_present()): channels learned over the air are nowhere to go. */
int dsd_engine_channel_scan_has_other_row(const dsd_opts* opts, const dsd_state* state);
/** Whether the -Y list can rotate at all: it is the user's own (dsd_state_trunk_lcn_user_list_present()), so not
 * channels learned over the air, and at least two of its rows are ones a step can land on -- with a frequency, not
 * avoided, and not an analog row skipped at every visit for a width the front end refuses at the current DSP rate, as
 * dsd_engine_channel_scan_has_other_row() judges them. The per-visit cap neither runs nor expires otherwise. 0 for
 * NULL arguments. */
int dsd_engine_channel_scan_can_rotate(const dsd_opts* opts, const dsd_state* state);
/** How many rows of the -Y list a step can land on, by the rule dsd_engine_channel_scan_can_rotate() counts them
 * with: a frequency, not avoided, and not an analog row skipped at every visit for its width at the current DSP rate.
 * The `b` avoid refuses to leave fewer than one. 0 for NULL arguments. */
int dsd_engine_channel_scan_reachable_rows(const dsd_opts* opts, const dsd_state* state);
/** The -Y list row the receiver is on (issue #666): on a typed list the row whose tune last landed on the current map,
 * which a failed or pending start does not move; otherwise, and before a typed list's first landing, the row heard
 * (dsd_state_lcn_heard_row()), which a placeholder park does not move. -1 with none, and for a NULL state. */
int dsd_engine_channel_scan_on_air_row(const dsd_state* state);
/** Whether the -Y row on air (dsd_engine_channel_scan_on_air_row()) is one the operator avoided (issue #644): the `b`
 * step off it was refused or failed late, or it was avoided while a tune elsewhere had not landed. One read while
 * nothing is avoided. 0 for a NULL state. */
int dsd_engine_channel_scan_on_avoided_row(const dsd_state* state);
/** Whether the operator hold (dsd_state::lcn_scan_hold, `Y`) keeps the -Y scan on the row on air: held, and the row is
 * not one the operator avoided (issue #644). Every rule an operator hold suspends reads this: the timed step, the
 * receive-policy leave and its frame-loop yields, the voice gate, the per-visit cap and the timing view. A hold over an
 * avoided row suspends none of them, so the scan leaves it; the hold stays and keeps the row it lands on. */
int dsd_engine_channel_scan_hold_keeps_row(const dsd_state* state);
/** Whether a -Y scan sitting on an avoided row (dsd_engine_channel_scan_on_avoided_row()) steps off it now (issue
 * #644): ahead of its carrier, its dwell and an operator hold, as a trunk scan's departure goes. Only with somewhere to
 * go (a row a step can land on: dsd_engine_channel_scan_reachable_rows()), no tune of the scan's still in flight (a
 * typed row's request or retry, an untyped step's or walk's), and at the step cadence: not before
 * dsd_state::lcn_avoid_depart_m, which every step taken off an avoided row moves on
 * (dsd_engine_channel_scan_departure_note()). The frame loop yields for it while a carrier holds sync, so the
 * no-carrier step takes it; that step's timing rule answers due for it too. 0 for NULL arguments, without -Y running,
 * under a trunk scan, and without a list of the user's own. Decoder thread; one read while nothing is avoided. */
int dsd_engine_channel_scan_departure_due(const dsd_opts* opts, const dsd_state* state, double now_m);
/** A step off an avoided row is being taken at @p now_m (decode monotonic clock): the next departure waits the step
 * cadence (-t, at least 1 s), whatever this one comes to (issue #644). */
void dsd_engine_channel_scan_departure_note(const dsd_opts* opts, dsd_state* state, double now_m);

/** dsd_engine_channel_scan_walk_row() results. */
typedef enum {
    DSD_SCAN_WALK_LANDED = 0,   /**< Every front end took the row: it is committed (see below). */
    DSD_SCAN_WALK_PENDING = 1,  /**< The RTL tune timed out: noted (DSD_MANUAL_TUNE_LCN_CYCLE); nothing commits yet. */
    DSD_SCAN_WALK_REFUSED = 2,  /**< A front end refused the row (or none is configured): passed or kept for a retry. */
    DSD_SCAN_WALK_DEFERRED = 3, /**< The RTL tune was deferred behind another owner's: nothing changes. */
} dsd_scan_walk_result;

/** The walk's own row rules, after its tune of @p row in direction @p dir (1 Next, -1 Previous; issue #666) ended:
 * @p landed 1 (the cursor moves past the row, which becomes the row heard) or 0, refused (going back, the row is the
 * one the next Previous retries and the cursor stays; going forward, the cursor moves past it, so a row the tuner never
 * takes cannot hold Next on it). A refusal keeps a recorded row heard, and with none the row heard is none rather than
 * the row before the cursor. The scan's visit timers read any cursor move as a retune, which a refusal is not, so each
 * one that had seen the cursor is told the new one and keeps its windows; one that had not is left alone. A deferral
 * changes nothing, so it never comes here. Shared by the walk the -Y scan owns and the one app-control makes off any
 * scan (issue #644). Nothing for a NULL @p state or a row out of range. Decoder thread. */
void dsd_engine_channel_walk_note_outcome(dsd_state* state, int row, int dir, int landed);

/** Walk the running untyped -Y scan (scanner_mode 1, no trunk scan, a list without modes) to @p row in direction
 * @p dir (1 Next, -1 Previous): the operator's `L`/`J` (issue #666), and a go-to of a chosen row (issue #644). It tunes
 * as the scan's own step does, every configured front end, rigctl first, then RTL, and commits nothing it has not
 * confirmed (issue #644 B-2). The row on air is forgotten at once (dsd_engine_scan_landing_forget()).
 * - LANDED: the SCAN_STEP carrier boundary ends the calls of the row left, under its label and keys, then the row is
 *   installed as a step's is: the cursor past it, the row heard, its keys, the analog reset, the landing the scan
 *   confirmed, fresh scan clocks.
 * - PENDING: the rigctl leg (if any) moved, the RTL tune timed out. Noted (dsd_engine_manual_tune_note_lcn_cycle());
 *   the cursor, the row heard, the keys and the clocks stay. The engine loop's poll (dsd_engine_scan_untyped_poll())
 *   then commits the row as above when it lands while the scan is still that one (same session and list, the row on the
 *   frequency tuned), applies the refusal rules when it fails late (dsd_engine_channel_walk_note_outcome()), and
 *   changes nothing when it is dropped; a landing after the scan changed only runs the boundary of the move. Meanwhile
 *   the timed step waits, and the next walk goes on from this row (dsd_engine_manual_tune_pending_row()).
 * - REFUSED: the refusal rules for @p dir. A rigctl leg that moved before the RTL leg failed already ran the outgoing
 *   boundary and the analog reset: the receiver left the row on air.
 * - DEFERRED: nothing but a rigctl move's boundary, as above; the next press tries the same row.
 * Placeholder (0 Hz) rows are the caller's to skip. Takes no P25 SM tick guard (conventional scanning, trunking off).
 * Decoder thread. */
dsd_scan_walk_result dsd_engine_channel_scan_walk_row(dsd_opts* opts, dsd_state* state, int row, int dir);
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
