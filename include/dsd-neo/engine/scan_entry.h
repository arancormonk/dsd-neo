// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/**
 * @file
 * @brief A frequency typed by hand that is a row of the scan in force (issue #644): which row it is, and whether that
 * row is the one confirmed on air.
 *
 * A typed frequency that exactly equals an eligible row of the -Y list or the trunk-scan target list is the operator's
 * pick of that row: the scan goes to it and holds it there, with the row's own settings. Read on the decoder thread,
 * from the command handler only: nothing here runs per frame. No USE_RADIO guard: a rigctl peer and a radio-off build
 * run both scans.
 */
#ifndef DSD_NEO_ENGINE_SCAN_ENTRY_H
#define DSD_NEO_ENGINE_SCAN_ENTRY_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/** The scan a typed frequency matched a row of. */
typedef enum {
    DSD_SCAN_ENTRY_NONE = 0,    /**< No eligible row: the entry is a plain manual tune. */
    DSD_SCAN_ENTRY_CHANNEL = 1, /**< A row of the -Y list (running, paused, or resuming). */
    DSD_SCAN_ENTRY_TRUNK = 2,   /**< A --trunk-scan target (running or paused). */
} dsd_scan_entry_scanner;

/** Why no row was chosen: the first row on the frequency typed, in list order, was not eligible for this reason. */
typedef enum {
    DSD_SCAN_ENTRY_OK = 0,   /**< A row was chosen. */
    DSD_SCAN_ENTRY_NO_ROW,   /**< No scan list in force, or none of its rows is on that frequency. */
    DSD_SCAN_ENTRY_AVOIDED,  /**< The operator avoided it for the session. */
    DSD_SCAN_ENTRY_LIST_OFF, /**< Its trunk-scan list is switched off. */
    DSD_SCAN_ENTRY_WIDTH,    /**< The width it runs does not fit the input, so the scan skips it at every visit. */
} dsd_scan_entry_reason;

/** dsd_engine_scan_entry_match()'s answer. */
typedef struct {
    dsd_scan_entry_scanner scanner;
    int row;          /**< The -Y row or the trunk-scan target index (0-based); -1 with none. */
    unsigned ordinal; /**< The chosen row's 1-based place among the eligible rows on the frequency, in list order. */
    unsigned count;   /**< How many eligible rows are on the frequency (a toast's "(1 of 2)"). */
    int on_air;       /**< The chosen row is confirmed on air: it needs no tune, only the hold. */
    int trunked;      /**< The chosen row is a trunked target, whose park is its control channel. */
    dsd_scan_entry_reason reason;
    char label[64]; /**< The row's name: its label, else its target id, else "row N" (1-based). */
} dsd_scan_entry_match_t;

/** What a scan's go-to of the row a typed frequency picked came to (issue #644 D1): the -Y scan's
 * (dsd_engine_channel_scan_goto_row(), dsd_engine_channel_scan_resume_at()) and the trunk scan's. */
typedef enum {
    DSD_SCAN_GOTO_HOLDING = 0,  /**< The row is on air and the scan holds it, marked an entry hold: it was confirmed
                                     on air already (the hold only), or its tune landed now. */
    DSD_SCAN_GOTO_PENDING,      /**< Its tune is outstanding (or the settings it lands with restage): the hold
                                     follows its confirmed landing, if the entry is still the one in force then. */
    DSD_SCAN_GOTO_REFUSED,      /**< Its tune was refused, or there is no such scan or row to go to: no hold. */
    DSD_SCAN_GOTO_DEFERRED,     /**< Its tune was deferred behind another owner's: no hold; a retry may land. */
    DSD_SCAN_GOTO_SETUP_FAILED, /**< The row's settings could not be prepared: nothing was tuned. */
} dsd_scan_goto_result;

/** An entry that failed after its go-to returned PENDING (dsd_engine_scan_entry_take_failure()). */
typedef struct {
    dsd_scan_entry_scanner scanner; /**< CHANNEL or TRUNK */
    int row;                        /**< the -Y row or trunk-scan target gone to (0-based) */
    dsd_scan_goto_result result;    /**< REFUSED, DEFERRED or SETUP_FAILED */
    char label[64];                 /**< its name when it failed, as the match names it: a -Y row's label, else
                                         "row N"; a target's label, else its id */
} dsd_scan_entry_failure_t;

/**
 * @brief Take the failure of an entry whose tune failed after its go-to returned (issue #644), once.
 *
 * The scan records one where it drops an entry for its own tune's failure: a -Y row's tune or restage refused or
 * deferred, an untyped walk's tune that failed, a -Y resume-at that stays paused (refused, deferred or not set up; an
 * interrupted one, which a stream reopen ends, is no failure), and a trunk-scan park that failed. A cancel records
 * none: `Y`, `L`, `J`, `b`, another entry, a pause, a stop, or another tune taking the tuner. The record names what
 * the entry went to as it stood when it failed, so an entry no toast began is named too. A newer failure replaces one
 * not taken. A failure inside the go-to's own call is recorded too; that call's caller, which names the outcome
 * it returned, takes and drops it. Decoder thread.
 *
 * @return 1 with @p out (may be NULL) filled when one was kept, else 0; none is kept after either way.
 */
int dsd_engine_scan_entry_take_failure(dsd_state* state, dsd_scan_entry_failure_t* out);

/**
 * @brief Find the row of the scan in force that the frequency @p hz typed by hand picks (issue #644 D4, D5).
 *
 * The scan in force is a trunk scan (dsd_opts::trunk_scan_enabled with a coordinator, running or paused), else a -Y
 * scan over a list of the user's own (dsd_state_trunk_lcn_user_list_present()) that runs, is paused or has a resume
 * pending. Channels learned over the air, a channel map under -T and a map a stopped scan left are no list to match.
 *
 * A row is on the frequency only when its own frequency equals @p hz exactly, compared as an unsigned 32-bit Hz value
 * before anything narrows either to a long; a placeholder (0 Hz) row never is. A row on the frequency is eligible when
 * the operator has not avoided it, its trunk-scan list is switched on, and the width it runs fits: its effective
 * width (a session edit, its own, else the configured width of its kind) at the published DSP rate, and an am row only
 * where something demodulates AM -- the rule by which the scan skips a row at every visit. A trunk-scan target cooling
 * down after a failed retune is eligible: the pick is explicit. In an untyped -Y list a run of adjacent rows on one
 * frequency is one channel (as `L` and `J` walk it), so that run counts once: the row of it the scan is confirmed on
 * speaks for it while that row itself is in the rotation, avoided neighbours or not (the timed step visits every row
 * of a run); otherwise its first row decides.
 *
 * Of several eligible rows the one confirmed on air (dsd_engine_channel_scan_row_confirmed(),
 * dsd_engine_trunk_scan_target_confirmed()) is chosen, else the first in list order (trunk scan: list number, then file
 * order; -Y: file order). @p out->ordinal and @p out->count place it among them.
 *
 * With none eligible @p out->scanner is DSD_SCAN_ENTRY_NONE and @p out->reason says why the first row on the
 * frequency, in list order, was not (NO_ROW when no row is on it, or no scan list is in force); @p out->row,
 * @p out->label and @p out->trunked then name that row, or -1, "" and 0.
 *
 * Read-only. Decoder thread.
 *
 * @return 1 when a row was chosen, else 0 (NULL arguments included; @p out, when given, is then filled as above).
 */
int dsd_engine_scan_entry_match(const dsd_opts* opts, const dsd_state* state, uint32_t hz, dsd_scan_entry_match_t* out);

/**
 * @brief Whether -Y row @p row is confirmed on air (issue #644): the last landing the scan confirmed is that row's, in
 * this scan session and on this list, the row still holds the frequency landed, and no movement is outstanding (no
 * typed row's request or retry, no untyped step's or walk's tune pending, no pause or resume pending).
 *
 * Every later tune attempt (a step, a walk, a resume, the avoided-row departure; failed, partial or pending), every
 * carrier boundary that moves the receiver (a manual tune), every radio stream start, a rigctl connection change and
 * the scan's pause or stop forget the landing, so a row whose frequency is still tuned yet was not landed since is not
 * confirmed. 0 for a NULL @p state. Decoder thread.
 */
int dsd_engine_channel_scan_row_confirmed(const dsd_state* state, int row);

/**
 * @brief Whether trunk-scan target @p index is confirmed on air (issue #644): it is the target parked on, the scan is
 * not paused, its park's retune completed (no park in flight), and no later switch, retune, carrier boundary, stream
 * start or pause has forgotten that landing. A trunked target's following of its own calls does not unconfirm it. 0
 * without a coordinator. Decoder thread.
 */
int dsd_engine_trunk_scan_target_confirmed(const dsd_state* state, int index);

#ifdef __cplusplus
}
#endif
#endif
