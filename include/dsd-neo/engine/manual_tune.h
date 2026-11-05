// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief A manual tune whose RTL wait timed out, followed to its completion (issue #671).
 *
 * io_control_set_freq_tagged() hands back the ID of an RTL tune that timed out: the controller still owns the request
 * and reports its completion later. The decoder notes one such tune at a time, and its loop polls for the completion
 * while one is noted. A late failure puts the cached tuner frequency back on the centre the stream applied, so the
 * decoder never goes on naming a frequency the receiver did not reach. Any newer tune of the receiver, and a stream
 * stop or open, drops the noted tune, so an old completion never overwrites a newer state; the next poll reports the
 * drop, so what the tune was for (an untyped -Y step's row, a resume's) is settled, never left waiting. Decoder thread
 * only.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_MANUAL_TUNE_H
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_MANUAL_TUNE_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What a noted manual tune was. */
typedef enum {
    DSD_MANUAL_TUNE_ENTRY = 1,
    DSD_MANUAL_TUNE_UNTYPED_STEP = 2,
    DSD_MANUAL_TUNE_RR_IMPORT = 3, /**< A RadioReference import's tune (issue #803). */
    DSD_MANUAL_TUNE_LCN_CYCLE =
        4, /**< A Next or Previous walk's tune over a list without modes (`L`/`J`, issue #644). */
} dsd_manual_tune_kind;

/** dsd_engine_manual_tune_poll() results. */
enum {
    DSD_MANUAL_TUNE_NONE = 0,    /**< Nothing noted, or no completion for it yet. */
    DSD_MANUAL_TUNE_LANDED = 1,  /**< The noted tune landed; the record is gone. */
    DSD_MANUAL_TUNE_FAILED = 2,  /**< The noted tune failed; the record is gone, the cached centre put back. */
    DSD_MANUAL_TUNE_DROPPED = 3, /**< The noted tune was superseded, or its stream session ended; the record is gone. */
};

/**
 * @brief Note the manual tune that timed out with @p id, replacing any tune noted before it.
 *
 * @param state            Decoder state.
 * @param kind             What the tune was.
 * @param id               The ID io_control_set_freq_tagged() handed back; 0 notes nothing (and keeps no older tune).
 * @param requested_hz     The frequency asked for, which a failure names.
 * @param prior_applied_hz The centre in force before the tune, where a failure goes back to when the stream has none.
 * @param row              The scan row the tune enters, -1 for an entry.
 * @param scan_session     The -Y scan session the row belongs to (dsd_state::lcn_scan_session; 0 for an entry).
 * @param list_rev         The -Y list the row belongs to (dsd_state::lcn_list_rev; 0 for an entry).
 */
void dsd_engine_manual_tune_note_pending(dsd_state* state, dsd_manual_tune_kind kind, uint64_t id,
                                         uint32_t requested_hz, uint32_t prior_applied_hz, int row,
                                         uint32_t scan_session, uint64_t list_rev);

/**
 * @brief Note a Next or Previous walk's tune that timed out with @p id (`L`/`J` over a list without modes, issue #644):
 * what dsd_engine_manual_tune_note_pending() notes for DSD_MANUAL_TUNE_LCN_CYCLE, with the walk's direction and owner.
 *
 * The row, its keys and the call boundary wait for the landing. The record carries the scan session and the list
 * (dsd_state::lcn_scan_session, dsd_state::lcn_list_rev) the row belongs to now, so a landing after either moved names
 * no row. Its drop changes nothing (what dropped it decides the receiver), so a newer tune noted over it keeps no
 * report of it.
 *
 * @param state            Decoder state.
 * @param id               The ID the RTL leg handed back; 0 notes nothing and leaves any older tune noted.
 * @param requested_hz     The row's frequency.
 * @param prior_applied_hz The centre in force before the tune.
 * @param row              The row walked to.
 * @param dir              The walk's direction: 1 Next, -1 Previous (a late failure passes or retries the row by it).
 * @param scan_owned       1 when the running -Y scan owns the walk (its landing commits the row as the scan's step
 *                         does), 0 for a walk off any scan (app-control settles it through the walk hook,
 *                         <dsd-neo/runtime/channel_scan_hooks.h>).
 */
void dsd_engine_manual_tune_note_lcn_cycle(dsd_state* state, uint64_t id, uint32_t requested_hz,
                                           uint32_t prior_applied_hz, int row, int dir, int scan_owned);

/**
 * @brief The row a list tune still awaits on @p state's current list and scan session (dsd_state::lcn_list_rev,
 * dsd_state::lcn_scan_session), or -1 with none: a Next or Previous walk's (DSD_MANUAL_TUNE_LCN_CYCLE) or the untyped
 * -Y step's (DSD_MANUAL_TUNE_UNTYPED_STEP), noted and not yet settled.
 *
 * A press while it is pending goes on from that row in its own direction, so quick presses and key auto-repeat keep
 * advancing (issue #644), and the -Y timed step waits while a walk the scan owns is pending (it would supersede it).
 *
 * @param state          Decoder state; NULL reads as none.
 * @param out_scan_walk  Optional; set to 1 when the tune is a walk the running -Y scan owns, else 0 (a walk off any
 *                       scan, or the step's own tune).
 * @return The row, or -1. A flag read and one atomic load while nothing is noted. Decoder thread.
 */
int dsd_engine_manual_tune_pending_row(const dsd_state* state, int* out_scan_walk);

/**
 * @brief The -Y scan starting over a Next or Previous walk off the scan still awaiting its tune takes that walk as its
 * own (issue #644).
 *
 * The record turns into a walk the scan owns, in the current scan session, so its landing commits its row as the scan's
 * walk does, and its late failure or drop follows that walk's rules; the walk no longer holds the tuner against the
 * state machines (dsd_engine_manual_tune_operator_walk_noted() reads 0). Only a walk on @p state's current list
 * (dsd_state::lcn_list_rev) whose row still holds the frequency walked to, and only on a list with no modes (a typed
 * row lands with settings a walk never tuned). A `y` start (dsd_engine_channel_scan_start()) and a config that turns
 * the scanner on take it this way.
 *
 * @return 1 when taken, 0 otherwise (nothing changes). Decoder thread.
 */
int dsd_engine_manual_tune_adopt_walk(const dsd_state* state);

/**
 * @brief The radio stream is about to stop (issue #644): a walk's (DSD_MANUAL_TUNE_LCN_CYCLE) or the untyped -Y step's
 * (DSD_MANUAL_TUNE_UNTYPED_STEP, a timed step's or a resume's) tune still pending, whose timeout cached its target as
 * the centre, would have the next stream open on that target while nothing names it: the stop drops the record, and no
 * landing ever commits its row.
 *
 * While @p opts->rtlsdr_center_freq is still that target (nothing set another since), it goes back to the frequency
 * the receiver was confirmed on before the tune: for the running -Y scan's own tune of a row of its current list, the
 * row heard's; otherwise (a walk off any scan, a resume's) the centre last confirmed before the tune, the walk's
 * origin, carried across tunes that replaced one another while pending (never a replaced tune's speculative target).
 * The stream then reopens where the row heard and a hold say the receiver is; no tune is made and nothing lands. An
 * entry's or an import's tune reopens on its target, which the operator asked for. Decoder thread, before the stream
 * stops.
 */
void dsd_engine_manual_tune_stream_stopping(dsd_opts* opts, const dsd_state* state);

/**
 * @brief The operator's own control-channel tune (app-control's Return to CC, a block transition) runs while a walk off
 * the scan is pending (issue #644): with @p yields 1 the walk does not defer the engine's trunk tunes
 * (dsd_engine_manual_tune_operator_walk_noted() reads 0), and with 0, set right after that tune, it does again. The
 * caller lets go of the walk once its tune is taken; a refusal with nothing moved leaves it. Decoder thread, under the
 * P25 SM tick guard (a guarded command), so no watchdog tick reads the flag between the two.
 */
void dsd_engine_manual_tune_walk_yields(int yields);

/**
 * @brief Note a RadioReference import's tune that timed out with @p id (issue #803): what
 * dsd_engine_manual_tune_note_pending() notes for DSD_MANUAL_TUNE_RR_IMPORT, with what its outcome is to say.
 *
 * Called after the import posted its own toast, which names the pending tune. The poll then posts each outcome: a late
 * failure posts @p failure_text; a landing (@p landed_text), or a drop by a newer tune or by the stream stopping,
 * replaces the import's toast only while that toast is still up, never a newer message or a cleared line. A drop by the
 * owner's own continuation (dsd_engine_manual_tune_owner_takes_over()) says nothing. The engine's synced-frames loop ends
 * a reception at once on an announced drop or a stream stop, so each report reaches the poll while the toast is up.
 *
 * @param state            Decoder state; its ui_msg is the import's toast.
 * @param id               The ID io_control_set_freq_tagged() handed back; 0 notes nothing and leaves any older tune
 *                         noted (a tune that was not queued supersedes nothing).
 * @param requested_hz     The frequency asked for.
 * @param prior_applied_hz The centre in force before the tune.
 * @param failure_text     The toast a late failure posts; NULL or empty for the plain tune failure.
 * @param landed_text      The toast a landing posts; NULL or empty for the plain landing.
 */
void dsd_engine_manual_tune_note_rr_import(dsd_state* state, uint64_t id, uint32_t requested_hz,
                                           uint32_t prior_applied_hz, const char* failure_text,
                                           const char* landed_text);

/**
 * @brief Say that the tune about to supersede the noted one is its owner's continuation: a trunking state machine's or
 * a scan's own tune, after the RadioReference import that turned it on. The import's drop is then not announced (it
 * would read as a failed import). Called right before dsd_engine_manual_tune_supersede(), from any thread that tunes
 * (the P25 watchdog's included); it matters only to an RR import's record, and the next note clears it.
 */
void dsd_engine_manual_tune_owner_takes_over(void);

/**
 * @brief Drop the noted tune: another tune of the receiver now decides where the receiver is. The next
 * dsd_engine_manual_tune_poll() reports the drop (DSD_MANUAL_TUNE_DROPPED), unless a newer tune is noted first, which
 * then owns the receiver: an untyped -Y step's drop is still reported then, ahead of the newer tune, since a resume may
 * wait on its row. Nothing when no tune is noted.
 */
void dsd_engine_manual_tune_supersede(dsd_state* state);

/**
 * @brief Take the noted tune's completion, if it has come.
 *
 * Costs a flag read and one atomic load while nothing is noted. A noted record is settled under the P25 SM tick guard
 * (taken here unless the caller holds it), so a late failure never puts back a centre over a newer watchdog tune's. A record whose stream session has ended (rtl_stream_session_generation())
 * is dropped, and so reported, as one superseded is (dsd_engine_manual_tune_supersede()): the cached centre stays, for
 * the next stream to open on or as the newer tune left it. On a failure, @p opts->rtlsdr_center_freq goes back to the
 * centre the stream last applied (or the prior one noted), and for an entry a toast names the frequency that failed; on
 * a landing it is the tune's target, which a later tune's refusal may have put back meanwhile (a -Y scan's own row tune
 * outlives a refusal, issue #644).
 * A RadioReference import's tune posts every outcome (dsd_engine_manual_tune_note_rr_import()).
 * An untyped -Y step's failure is not toasted, nor a walk's (DSD_MANUAL_TUNE_LCN_CYCLE): the scan passes the row, as
 * after a step that failed in its wait (issue #666), and the engine's loop settles the row
 * (dsd_engine_scan_untyped_poll()).
 *
 * @return DSD_MANUAL_TUNE_NONE, DSD_MANUAL_TUNE_LANDED, DSD_MANUAL_TUNE_FAILED or DSD_MANUAL_TUNE_DROPPED.
 */
int dsd_engine_manual_tune_poll(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_MANUAL_TUNE_H */
