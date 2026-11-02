// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA trunking state machine: following one traffic channel off the control channel.
 *
 * The control channel of a TETRA cell (EN 300 392-2 clause 9.5.1, the MCCH on timeslot 1 of the main carrier) assigns
 * traffic channels with the channel allocation element of a MAC-RESOURCE or MAC-END PDU (clause 21.5.2, table 21.87):
 * a carrier and a timeslot bitmap. The decoder follows at most one of them. The caller decides which one (talkgroup
 * policy, priority) and hands it to tetra_sm_on_grant(); the state machine retunes through the runtime trunk-tuning
 * hooks, returns to the control channel when the followed call is released or falls silent, and keeps all of its
 * state in the session's TETRA state extension, so two sessions never share it.
 *
 *   TETRA_SM_IDLE       not trunking, or no control channel known yet, or off the main carrier and not moving there
 *   TETRA_SM_ACQUIRING  moving to the main carrier a SYSINFO heard elsewhere named, or on it until its own SYSINFO
 *   TETRA_SM_ON_CC      on the control channel, waiting for a grant
 *   TETRA_SM_TUNED      on the followed traffic channel (a tune in flight included)
 *
 * The engine ticks it between frames and after every frame, under the P25 SM tick guard. Decoder thread only.
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_TRUNK_SM_H
#define DSD_NEO_PROTOCOL_TETRA_TRUNK_SM_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TETRA_SM_IDLE = 0,  /**< not trunking, or no control channel known */
    TETRA_SM_ACQUIRING, /**< moving to the main carrier, or on it before its SYSINFO */
    TETRA_SM_ON_CC,     /**< on the control channel, awaiting grants */
    TETRA_SM_TUNED,     /**< on the followed traffic channel */
} tetra_sm_state_e;

/** One traffic channel the caller decided to follow (the caller has already applied talkgroup policy). */
typedef struct {
    long freq_hz;      /**< downlink frequency of the allocated carrier */
    uint16_t carrier;  /**< carrier number (12 bits), for key derivation and logging */
    uint8_t timeslots; /**< EN 300 392-2 table 21.80 bitmap: bit 3 = timeslot 1 ... bit 0 = timeslot 4 */
    uint16_t call_id;  /**< CMCE call identifier (14 bits), 0 when the allocation carried none */
} tetra_sm_grant;

/** Grant timeout: seconds a followed traffic channel is kept without decoded speech after the tune completes, or after
 * the last maintenance event before the first speech (tetra_sm_note_maintenance()). */
#define TETRA_SM_GRANT_TIMEOUT_S 4.0
/** Seconds without decoded speech on the followed timeslots after which the speech counts as ended and the hangtime
 * (opts->trunk_hangtime) starts: longer than the frame 18 gap of a speech timeslot (two frames, 113 ms). */
#define TETRA_SM_SPEECH_GAP_S    0.25
/** Seconds a move to the main carrier the backend deferred waits before a later SYSINFO may stage it again. */
#define TETRA_SM_MOVE_HOLD_S     2.0
/** Seconds on the main carrier without its SYSINFO after which the state machine says so, once per move. */
#define TETRA_SM_MOVE_SILENT_S   10.0

/**
 * @brief Start the session's state machine over: IDLE, no followed call, no tune in flight.
 *
 * The engine calls it at start and app-control when the decode mode changes. A tune in flight is let go of: one that
 * failed is retired as rolled back, one the backend has not answered stays the backend's, so its completion still
 * publishes the tuner boundary. A session without TETRA state already reads IDLE and gets none.
 */
void tetra_sm_init(dsd_opts* opts, dsd_state* state);

/**
 * @brief CRC-passed MCCH signalling named the control channel (SYSINFO with a valid main carrier, after the caller set
 * state->trunk_cc_freq to it).
 *
 * Every carrier of a cell broadcasts the SYSINFO of its main carrier (EN 300 392-2 clauses 3.1, 21.4.4.1), so it says
 * where the control channel is, not where the receiver is. While trunking is on:
 * - On the main carrier (tetra_sm_receiver_freq() equals it; with no frequency known, an audio input, unless the state
 *   machine can move the receiver, when only its own completed move says so), IDLE and ACQUIRING become ON_CC, the
 *   control channel is a trunking candidate and its sync is stamped (dsd_mark_cc_sync()).
 * - Off it, nothing of that happens, and the move there is staged for the end of the burst (tetra_sm_after_burst()),
 *   but only on a carrier a CRC-passed downlink block proved before this SYSINFO's own block, so a SYSINFO never
 *   vouches for itself.
 * - While TUNED a traffic channel's SYSINFO only keeps the control channel a candidate.
 */
void tetra_sm_on_cc_sync(dsd_opts* opts, dsd_state* state);

/**
 * @brief The end of a burst whose SYSINFO staged a move to the main carrier (tetra_sm_on_cc_sync()); the decoder calls
 * it once the burst's bookkeeping is done (tetra.c), only while a move is staged.
 *
 * The move is the trunk-tuning hooks' return to the control channel, made as a release's is (the P25 alias cleared for
 * it), and is asked for only when all of these hold: no carrier reset since the SYSINFO; trunking on a downlink
 * (not --tetra-uplink); a receiver the state machine can move (a live radio stream, not an I/Q replay, or a connected
 * rigctl peer behind an audio input); nothing followed and no request of its own in flight; the receiver not on the main
 * carrier; the main carrier still the control channel; the tuner free (dsd_trunk_tuning_hook_tuner_free(): no manual or
 * correlated tune pending, no scan owning it); and the main carrier's history allowing it. The history keeps
 * TETRA_SM_MOVES main carriers: one moved to is never moved to again (so a move there and back stops), a refused or
 * failed move waits 5 s, 15 s, then 60 s at most before the next to the same carrier, a deferred one
 * TETRA_SM_MOVE_HOLD_S and counts as none, and with every entry taken nothing more is moved to (said once). An accepted
 * move enters ACQUIRING with no control-channel sync stamped (the engine's stamp is taken back); its completion forgets
 * the carrier left (tetra_forget_carrier()) and keeps the history. ACQUIRING becomes ON_CC at the main carrier's own
 * SYSINFO; a SYSINFO there naming another main carrier is one more move. A failure puts back what the request changed,
 * the receiver included (tetra_sm_settle()), and counts. Nothing moves back to the carrier left.
 */
void tetra_sm_after_burst(dsd_opts* opts, dsd_state* state);

/**
 * @brief Forget the history of moves to the main carrier (tetra_sm_after_burst()): the session now receives another
 * system or through another receiver (a RadioReference import applied, another input or radio source). A move in
 * flight stays the state machine's. tetra_sm_init(), a decode-mode change (which drops the TETRA state) and trunking
 * turned back on forget it too; a tune elsewhere does not.
 */
void tetra_sm_forget_moves(dsd_state* state);

/**
 * @brief Follow @p grant.
 *
 * Ignored unless trunking is on, a control channel is known, the grant names a frequency, no tune is in flight and a
 * CRC-passed BSCH or MAC block was decoded on the carrier the receiver is on before the block that carried the grant
 * (tetra_confirm). A grant for the call already followed is ignored. A grant on the carrier the receiver is on
 * (tetra_sm_receiver_freq()) is followed without a retune; any other is tuned to with no symbol-timing override
 * (ted_sps 0). A refused or deferred tune changes nothing.
 *
 * @return 1 when the state machine now follows @p grant (tuned, tune in flight, or retargeted on the same carrier);
 *         0 when it ignored it.
 */
int tetra_sm_on_grant(dsd_opts* opts, dsd_state* state, const tetra_sm_grant* grant);

/**
 * @brief Settle the tune or return in flight once the backend answered it: a completion commits it, a failure puts
 * back what the request staged and then completes it. The decoder calls it once per burst, at the burst's entry and
 * before any of its blocks (tetra.c), so every block of a burst is judged against one state machine: inside a burst
 * the other entry points settle nothing, and an answer that comes during it is taken at the next burst or tick.
 * Outside a burst they settle as the tick does. Never creates TETRA state.
 *
 * @return 1 when no request is in flight any more, 0 while the backend has not answered one.
 */
int tetra_sm_settle(dsd_opts* opts, dsd_state* state);

/**
 * @brief The call @p call_id was released (D-RELEASE, D-DISCONNECT, or a timeslot-0000 allocation to its party).
 *
 * Only the followed call releases: TUNED with a followed call id equal to @p call_id returns to the control channel
 * (or, for a traffic timeslot of the control channel's carrier, just stops following). Any other id is ignored. A
 * return the backend refuses or defers keeps the call; the next tick tries again. With trunking off nothing returns:
 * the receiver stays where it is, as tetra_sm_tick() leaves it.
 */
void tetra_sm_on_release(dsd_opts* opts, dsd_state* state, uint16_t call_id);

/**
 * @brief Speech whose CRC passed was decoded on timeslot @p tn (1..4; 0 when the TDMA clock is not running).
 *
 * Reception activity (state->last_vc_sync_time and _m) in any state. While TUNED, speech on a followed timeslot
 * restarts the hangtime and ends the grant timeout; speech the decoder cannot place on a timeslot (0) may be any
 * timeslot's, so it does neither.
 */
void tetra_sm_note_voice(dsd_state* state, uint8_t tn);

/**
 * @brief The followed channel is maintained for the followed call (EN 300 392-2 clause 23.5.6.1.2, T.209): an
 * ACCESS-ASSIGN PDU of a followed timeslot carries the call's traffic usage marker, or a PDU there is addressed to one
 * of its parties.
 *
 * While TUNED and before the call's first speech, the grant timeout runs from the last such event instead of the tune:
 * a call set up and silent is not left while its channel is kept for it. Speech, and with it the hangtime, is
 * unaffected.
 */
void tetra_sm_note_maintenance(dsd_state* state);

/**
 * @brief Time-driven transitions: settle the tune in flight once the backend answered it (until then nothing else
 * happens: the request stays the backend's and is never timed out here), then while TUNED end the followed call when
 * another owner released the shared tuning state (no tune), on the grant timeout or after the hangtime that follows the
 * last speech. With trunking off the receiver stays on the followed channel, as DMR's and P25's machines leave it (the
 * engine's return tunes nothing without trunking): TUNED keeps the channel and its timers, no call record (as
 * tetra_sm_retire_call() leaves it), and asks for no return until trunking is on again. ON_CC and ACQUIRING drop to
 * IDLE once trunking is off or no control channel is known (a BSCH of another cell dropped it); trunking on again
 * forgets the history of moves to the main carrier (tetra_sm_after_burst()). ACQUIRING on the main carrier without its
 * SYSINFO for TETRA_SM_MOVE_SILENT_S says so once, and moves nowhere. The session's call records age on the same tick
 * (tetra_calls_tick()), trunking or not.
 */
void tetra_sm_tick(dsd_opts* opts, dsd_state* state);

/**
 * @brief The engine already returned the receiver to the control channel (no-carrier return, manual Return to CC):
 * drop the followed call, let go of any tune in flight (as tetra_sm_init() does), forget the traffic carrier, and come
 * to rest ON_CC (IDLE without a control channel). No tuning.
 */
void tetra_sm_on_external_cc_return(const dsd_opts* opts, dsd_state* state);

/**
 * @brief A carrier boundary moves the receiver elsewhere: drop the followed call, let go of any tune in flight (as
 * tetra_sm_init() does) and release the shared tuning state, without asking the tuner for anything (the caller owns
 * it). Rests ON_CC, or IDLE without a control channel or with trunking off.
 */
void tetra_sm_abandon_carrier(dsd_opts* opts, dsd_state* state);

/**
 * @brief The frequency in Hz the receiver is on, 0 when it is not known: while TUNED the followed channel, whose tune
 * completed (or whose grant was taken where the receiver was); else the frequency it is tuned to
 * (dsd_opts_tuned_freq_hz(), 0 off a radio input); 0 while a tune or return is in flight, between carriers. Never the
 * main carrier SYSINFO names (state->trunk_cc_freq): every carrier of the cell broadcasts the same SYSINFO, so it says
 * where the control channel is, not where the receiver is, and ON_CC can rest on a secondary carrier a session started
 * on. What the decoder reads as the carrier being received goes by it (tetra_calls.c).
 */
long tetra_sm_receiver_freq(const dsd_opts* opts, const dsd_state* state);

/** @brief The session's state; IDLE for a NULL @p state or a session without TETRA state. */
tetra_sm_state_e tetra_sm_get_state(const dsd_state* state);

/**
 * @brief 1 while the state machine owns the receiver's way back to the control channel: TUNED (a tune in flight, and a
 * grant still waiting for its first speech, included), ACQUIRING (a move to the main carrier in flight or completed and
 * waiting for its SYSINFO), or a return of its own in flight. Its tick then decides when to return (the grant timeout,
 * the hangtime after the last speech), so the engine's generic no-carrier recovery must not return first, and a tune
 * elsewhere lets go of it (tetra_sm_on_external_cc_return(), tetra_sm_abandon_carrier()). 0 otherwise, and for a
 * session without TETRA state.
 */
int tetra_sm_owns_return(const dsd_state* state);

/** @brief 1 and the followed grant in @p out (when not NULL) while TUNED; 0 otherwise. The grant is the channel the
 * receiver follows, retired or not (tetra_sm_retire_call()). */
int tetra_sm_followed(const dsd_state* state, tetra_sm_grant* out);

/**
 * @brief The call record the followed channel belongs to: 1 and its call identifier in @p call_id (when not NULL)
 * while TUNED on a call that was not retired (tetra_sm_retire_call()); 0 otherwise.
 */
int tetra_sm_followed_call_id(const dsd_state* state, uint16_t* call_id);

/**
 * @brief Call @p call_id's record left the call records (its release, a D-SETUP or D-INFO that gave its identifier to
 * another call, its ageing), so the identifier names no call any more, while the state machine still follows it, or
 * puts it back if a tune in flight fails: a return was refused, deferred or is pending. From here it follows the
 * channel but no call record (tetra_sm_followed_call_id() reports none) and the followed call's proof on the carrier is
 * gone, so nothing of a call that takes the identifier counts as the followed call's and no grant of it is taken for
 * the followed call. Following ends as it would have: the grant timeout or the hangtime retries the return; a grant the
 * call records hand over is followed as a new call. Any other identifier changes nothing.
 */
void tetra_sm_retire_call(dsd_state* state, uint16_t call_id);

/**
 * @brief The followed call took the call identifier @p new_id in place of @p old_id (a D-INFO or a D-CALL RESTORE,
 * EN 300 392-2 clauses 14.7.1.8 and 14.7.1.3): the state machine follows it under @p new_id, so a release of it still
 * ends following, and so does what a tune in flight would put back. A rename names the call followed already, on the
 * channel it is followed on: it is no grant, so nothing a new grant is admitted by applies (tetra_sm_on_grant(): a
 * carrier proved before the block, trunking on, a control channel known) and no timer starts over. A retired followed
 * call (tetra_sm_retire_call()) or another identifier changes nothing.
 */
void tetra_sm_on_call_renamed(dsd_state* state, uint16_t old_id, uint16_t new_id);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_TRUNK_SM_H */
