// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief DMR trunking state-machine interfaces and constants.
 *
 * DMR Tier III trunking state machine:
 *   - Explicit 4-state model (IDLE, ON_CC, TUNED, HUNTING)
 *   - Event-driven transitions
 *   - Per-slot activity tracking with timestamps
 *   - Tick-based timeout handling
 */
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_TRUNK_SM_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_TRUNK_SM_H_H

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * State Machine States
 * ============================================================================ */

typedef enum {
    DMR_SM_IDLE = 0, // Not trunking or no CC known
    DMR_SM_ON_CC,    // Parked on control channel, listening for grants
    DMR_SM_TUNED,    // On voice channel (awaiting voice, active, or hangtime)
    DMR_SM_HUNTING,  // Lost CC, searching candidates
} dmr_sm_state_e;

/* ============================================================================
 * Events
 * ============================================================================ */

typedef enum {
    DMR_SM_EV_GRANT = 0,  // Voice channel grant (group or individual)
    DMR_SM_EV_VOICE_SYNC, // Voice frame sync detected on slot
    DMR_SM_EV_DATA_SYNC,  // Data frame sync detected on slot
    DMR_SM_EV_RELEASE,    // P_CLEAR or slot termination
    DMR_SM_EV_CC_SYNC,    // Raw sync: retain an established CC, never confirm acquisition
    DMR_SM_EV_SYNC_LOST,  // Sync lost
} dmr_sm_event_type_e;

/* Where a GRANT event came from: a channel grant, or a channel status the CSBK layer selected (Hytera XPT). A Tier III
 * C_MOVE is no grant (TS 102 361-4 Table 7.17 names only a TSCC and an MS address): it never reaches the machine. */
typedef enum {
    DMR_SM_ORIGIN_GRANT = 0,
    DMR_SM_ORIGIN_STATUS = 1,
} dmr_sm_origin_e;

/* What dmr_sm_submit_grant() did with a grant. */
typedef enum {
    DMR_SM_GRANT_FOLLOWED = 0, // the receiver was tuned to it
    DMR_SM_GRANT_IN_PLACE = 1, // already on its carrier: nothing to tune
    /* Policy, receive admission, channel map or trust refused it; or it named another call on the followed slot, which
       ends that slot's assignment (dmr_sm_note_grant_assignment()) and is not followed itself (its repeat is). */
    DMR_SM_GRANT_REFUSED = 2,
    DMR_SM_GRANT_BUSY = 3,     // the receiver holds a call the grant may neither hold over nor preempt
    DMR_SM_GRANT_DEFERRED = 4, // the tune was not accepted: nothing changed
} dmr_sm_grant_result_e;

typedef struct {
    dmr_sm_event_type_e type;
    int slot;     // 0 (left/TS1) or 1 (right/TS2), -1 if N/A
    long freq_hz; // Frequency in Hz (for GRANT, 0 to resolve from LPCN)
    int lpcn;     // The call's channel (for GRANT): a Tier III LPCN, a Connect Plus LCN, an XPT LSN; 0 for none
    /* dsd_call_channel_kind of lpcn (NONE without a channel), as its submitter numbered it. */
    uint8_t channel_kind;
    int tg;       // Talkgroup (for GRANT, 0 if individual)
    int src;      // Source RID
    int dst;      // Destination RID (for individual GRANT)
    int is_group; // 1 for group grant, 0 for individual

    /* GRANT only. The CSBK/status layer judges talkgroup policy and hands its verdict over: the machine never
       re-evaluates a grant. */
    dsd_call_kind kind;     // GROUP_VOICE / PRIVATE_VOICE / DATA as the CSBK layer classified it
    int identity_known;     // 0: the target is a hash or placeholder (XPT private): never held, matched or preempting
    int caller_busy;        // the caller's own legacy window says the receiver is busy (Con+, Cap+, XPT)
    dmr_sm_origin_e origin; // grant or status selection
    int policy_valid;       // the caller evaluated @ref policy for this grant
    dsd_tg_policy_decision policy; // its verdict, data-call and private classification included
} dmr_sm_event_t;

/* ============================================================================
 * Per-Slot Activity Context
 * ============================================================================ */

/* Where a slot's call identity came from. An LC always wins: a later channel status replaces only a seed. */
typedef enum {
    DMR_SM_ID_NONE = 0,  // no identity: the call on the slot (if any) is unknown
    DMR_SM_ID_SEED = 1,  // a Capacity Plus channel status named it
    DMR_SM_ID_GRANT = 2, // the grant the machine followed named it
    DMR_SM_ID_LC = 3,    // a voice LC that passed its check named it
} dmr_sm_identity_prov_e;

typedef struct {
    int voice_active;     // 1 if voice currently active on this slot
    double last_active_m; // Monotonic timestamp of last activity

    /* The call on this slot of carrier_hz, as far as the machine knows it (decoder thread only). kind UNKNOWN: none.
       follow_m is when the machine started holding that call; it moves only when the call (its policy class, target
       or carrier) changes, so a preemption's dwell runs from the conversation's first follow. */
    uint32_t target;
    uint32_t source;
    uint8_t kind;         // dsd_call_kind
    uint8_t prov;         // dmr_sm_identity_prov_e
    uint8_t judged_group; // judged by group policy (a Tier III data grant followed as a group is)
    uint8_t data_call;
    /* DSD_CRC_* bits of what named the identity (a RAS-accepted grant, status or LC under -F, a Cap+ status's every
       fragment). */
    uint8_t crc_flags;
    int channel; // the channel the call was announced on (0: none)
    /* dsd_call_channel_kind of channel: a Tier III LPCN, a Connect Plus LCN, a Cap+ or XPT LSN. */
    uint8_t channel_kind;
    /* DSD_CRC_* bits of what named the channel (the grant's or status's burst; a status filling an LC's record):
       its own provenance, which an LC naming the call keeps with the channel and the channel's publication carries. */
    uint8_t channel_crc_flags;
    long carrier_hz; // the carrier the identity belongs to
    double follow_m; // decode clock

    /* The canonical call the identity names (dsd_call_snapshot_transmission() of the slot's call), so it never outlives
       that call: another call on the slot, or its end (on the followed carrier, past the hangtime), leaves the slot
       unknown. 0 while the identity awaits its call: the first one to begin on the slot after call_floor, the slot's
       epoch when it was named. */
    uint64_t call_epoch;
    uint64_t call_floor;
    double call_seen_m; // decode clock: when the named call was last seen active, or when it ended
    /* The named call is over and another (call_epoch) began on the followed carrier within the hangtime: the record
       names no call (unknown) and stays only so that naming that call as the same one keeps follow_m. */
    uint8_t gap;
} dmr_sm_slot_ctx_t;

/* How long a Capacity Plus channel-status seed waits for the voice it names (decode clock). Matches the status's own
 * follow gate, which only tunes after two seconds without voice. */
#define DMR_CAPPLUS_SEED_WINDOW_S 2.0

/* The talkgroup a Capacity Plus channel status named for one slot of the carrier Cap+ just followed it to. Cap+
 * tunes on the status alone, outside the grant path, so nothing else carries the call's identity until a voice LC
 * decodes; a call whose LCs all fail would otherwise play with no talkgroup at all. */
typedef struct {
    int armed;
    uint32_t tg;
    int lsn;
    long freq_hz;         // the LSN's channel-map frequency, as the call is published with
    uint32_t receiver_hz; // opts->rtlsdr_center_freq right after the follow tune (the tuner may round it)
    uint8_t crc_flags;    // DSD_CRC_* flags of every fragment of the status
    double armed_m;       // decode clock
    uint64_t generation;  // the tuning generation the arming tune completes into
} dmr_sm_capplus_seed_t;

/* ============================================================================
 * State Machine Context
 * ============================================================================ */

typedef struct {
    dmr_sm_state_e state;
    int initialized;

    // Per-slot activity (index 0 = TS1/left, index 1 = TS2/right)
    dmr_sm_slot_ctx_t slots[2];

    // Voice channel context (valid when state == DMR_SM_TUNED)
    long vc_freq_hz;
    int vc_lpcn;             // the followed call's channel (0: none), named once by the grant that tuned it
    uint8_t vc_channel_kind; // dsd_call_channel_kind of vc_lpcn
    int vc_tg;
    int vc_dst;
    int vc_src;
    int vc_slot;
    int vc_is_group;
    int vc_identity_published;
    int vc_status_follow; // a Capacity Plus status followed it: its release goes to the latest announced rest channel
    /* Bit s: the status that followed this carrier selected or seeded slot s. Before the first voice those slots hold
       the carrier whatever their identity is now (a private LSN, or a seed a later status made unknown, holds it as a
       call of unknown identity). Cleared with the slots' records (every follow, release and acquisition). */
    int vc_acquire_slots;
    // DSD_CRC_* bits of the CSBK or MBC that granted this channel (a RAS grant under -F). The grant's identity is
    // published later, at voice sync and outside that burst's scope, so its provenance travels with it.
    int cc_hunt_index;
    uint8_t vc_crc_flags;

    // Timing (monotonic only)
    double t_tune_m; // Time of last VC tune
    /* When the receiver arrived on the followed carrier: the follow tune that left another carrier, 0 when the follow
       adopted the carrier the receiver was already on (every call heard there is that carrier's). */
    double t_carrier_m;
    double receive_check_started_m;        // Bounded acquisition while receive admission is pending/rejected
    dsd_receive_carriers receive_carriers; // Colour code last verified on each voice channel
    long receive_check_freq_hz;            // The voice channel that window belongs to (a new one starts its own)
    int receive_codes_fresh;               // Its codes were forgotten at its tune: what it holds now it proved
    int vc_data_call;                      // the grant was a data call (its policy was judged with data_call set)
    /* vc_data_call says what the followed call's media is: a grant, status or LC named it. 0 for a companion slot
       adopted for a retired slot while nothing identified its call (dmr_sm_end_changed_assignment()): its media is
       unknown, and no grant's media contradicts it, until a grant there names its call (in place). */
    int vc_media_known;
    double t_voice_m;   // Time of last voice activity
    double t_cc_sync_m; // Time of last CC sync
    /* The followed grant's own media (voice for a voice grant, data for a data grant) was heard on its slot since the
       follow: from then on its acquisition no longer holds that slot in the busy view. */
    int vc_media_heard;

    /* CC probes do not replace the confirmed/configured return destination.
     * These fields live in each scan target's existing DMR context. */
    long cc_freq_hz;
    long cc_probe_freq_hz;
    long cc_rx_freq_hz; // Completed tune or decoded attribution at cc_rx_generation
    uint64_t cc_rx_generation;
    double cc_acquire_start_m;
    double cc_retry_after_m;
    double cc_tune_deadline_m;
    uint64_t cc_tune_request_id;
    /* A tracked tune's carrier judgement (dmr_sm_note_tracked_carrier_tune(), issue #834): the request it waits on (0:
       none), the receiver the tune would leave and where it goes. Judged once that request lands, dropped when the
       backend reports it failed (the receiver stayed). Decoder thread only. */
    uint64_t cc_leave_request_id;
    long cc_leave_from_hz;
    long cc_leave_to_hz;
    int cc_acquiring;
    int cc_confirmed;

    // Startup control-channel hunt (issue #703): a single-system session whose start frequency yields no validated
    // control within cc_grace_s walks its channel map with the ordinary hunt. Decoder thread only.
    double cold_start_armed_m;   // Monotonic time the startup window opened; 0 while unarmed
    int cold_start_hunt;         // 1 while a hunt the startup window started runs and no owner is named
    int cold_start_park_pending; // 0 none; 1 park keeping the window, 2 park unarmed, once settled

    // Hytera XPT, which has no dedicated control channel: validated XPT content identified the site, so recovery never
    // walks the map; it only listens where it is, or retunes the frequency it parks on. System-scoped: kept across a
    // same-system re-init (dmr_sm_begin_cc_acquisition()), cleared by dmr_sm_note_system_replaced().
    int site_without_cc;
    int vc_identity_known; // vc_tg/vc_dst/vc_src name the call (not an XPT placeholder or a Cap+ status follow)

    // An operator move (dmr_sm_cold_start_note_retune()) outside a startup hunt: the listening window on the new
    // frequency starts at cc_moved_m, or later when frames were still gated by a tune the move superseded, which
    // cc_tune_superseded marks until it settles.
    double cc_moved_m;
    int cc_tune_superseded;
    int cc_settle_waited; // frames stayed gated past the move: the window starts when the settle released them

    // HUNTING was entered because the control-channel window ran out (tick_on_cc()): validated content arriving before
    // the next hunt step opens that window again. 0 in every other state and for any other way into HUNTING.
    int cc_window_lost;
    int vc_status_grant; // a channel status (XPT) selected the grant: acquisition protection is the status window's

    // The receiver frequency last read from rigctl at rx_seen_generation, so a burst does not query it again: dropped
    // at an operator move, and stale once a trunk tune moves the generation on.
    long rx_seen_hz;
    uint64_t rx_seen_generation;
    double rx_query_retry_m; // real monotonic time a failed rigctl read may be tried again (0: no failure pending)

    // Configuration
    double hangtime_s;      // Hangtime after voice ends (default 2.0s)
    double grant_timeout_s; // Max wait for voice after grant (default 4.0s)
    double cc_grace_s;      // Confirmed CC loss grace (default 6.0s; probes get 2.0s)

    // Capacity Plus channel-status identity, per slot (index 0 = TS1). Decoder thread only.
    dmr_sm_capplus_seed_t capplus_seed[2];

    // Initialized flag
} dmr_sm_ctx_t;

/* ============================================================================
 * Public API - Core State Machine
 * ============================================================================ */

/**
 * @brief Initialize the DMR state machine context.
 */
void dmr_sm_init_ctx(dmr_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state);

/** Hand an accepted CC tune to the DMR owner. Clears outgoing voice/probe state;
 * a nonzero request ID is resolved before starting the acquisition deadline. */
void dmr_sm_begin_cc_acquisition(dmr_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, long freq_hz,
                                 uint64_t request_id);
/** Return to the explicit DMR control/rest frequency, clearing an obsolete P25
 * alias only on acceptance. Preserve the previous destination on failure. */
dsd_trunk_tune_result dmr_sm_return_to_cc(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, long freq_hz);
/** End the tuned call and come to rest on the control channel WITHOUT tuning: the release
 * teardown minus the CC return, for a caller that owns the tuner and is already moving it
 * (trunk scan evicting a target that reached its per-visit limit, issue #507). Tuning here
 * would fight that move, so nothing is asked of the tuner. */
void dmr_sm_abandon_carrier(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason);
/** Accepted control/rest-channel evidence. Zero frequency queries the active
 * tuner only if no completed tune/decoded attribution is available for the
 * current generation; nonzero frequency must match the received channel. */
void dmr_sm_note_cc_activity(const dsd_opts* opts, dsd_state* state, long freq_hz);
/** Retain an already confirmed CC only; never validate a probe from raw/relaxed evidence. */
void dmr_sm_note_cc_heartbeat(const dsd_opts* opts, dsd_state* state);

/**
 * @brief Process an event and update state machine.
 */
void dmr_sm_event(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const dmr_sm_event_t* ev);

/**
 * @brief Periodic tick for timeout-based transitions.
 *
 * Call at ~1-10 Hz. Handles hangtime expiry, grant timeout, CC loss detection.
 */
void dmr_sm_tick_ctx(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state);

/**
 * @brief Note an operator move of the receiver (issue #703).
 *
 * Restarts the startup window at the move; a running startup hunt drops its probe attribution and is marked to park
 * keeping that window once the tuning layer is settled. Any other machine past IDLE gives up a call it followed,
 * forgets where the receiver was and listens on the new frequency for a full window, which starts once a tune the move
 * superseded has settled. Decoder thread only.
 */
void dmr_sm_cold_start_note_retune(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state);

/** @brief Note that validated Hytera XPT content (a CRC-valid FID 0x68 site status or adjacent-site CSBK, or a completed
 *  XPT SLCO) identified the site: DMR owns recovery, which never walks the channel map there. */
void dmr_sm_note_xpt_site(const dsd_opts* opts, dsd_state* state);

/** @brief The session was pointed at another system (a RadioReference apply, a replaced channel map): forget what the
 *  old one identified itself as. */
void dmr_sm_note_system_replaced(dmr_sm_ctx_t* ctx);

/** @brief The trunk hangtime option changed: the machine's own follows the edit, unless the DMR hangtime override
 *  (DSD_NEO_DMR_HANGTIME) is in force, as dmr_sm_init_ctx() resolves it. */
void dmr_sm_note_hangtime(dmr_sm_ctx_t* ctx, const dsd_opts* opts);

/** @brief The receiver may be somewhere the machine has not seen (a rigctl connection replaced): forget where it was. */
void dmr_sm_note_receiver_unknown(dmr_sm_ctx_t* ctx);

/** @brief Whether the receiver is on @p freq_hz, or a tune the machine tracks is taking it there. Reads the tuner only
 *  while nothing attributes the receiver frequency, at most once per tuning generation for rigctl. */
int dmr_sm_receiver_on(const dsd_opts* opts, long freq_hz);

/** @brief Where the receiver is now as the machine knows it: the carrier it follows, else the one it listens on (as
 *  dmr_sm_receiver_on() reads it); 0 when nothing names it. Read before a follow's tune, it says whether the follow
 *  leaves that carrier. */
long dmr_sm_receiver_frequency(const dsd_opts* opts);

/** @brief After a DMR tune that is not the machine's follow or status follow (a Capacity Plus rest move, a
 *  control channel probe or settle) took the receiver from @p receiver_hz (where it was
 *  before the tune, as dmr_sm_receiver_frequency() reads it; 0 unknown) to @p freq_hz: when it left that carrier, the left
 *  calls' privacy bit and talkgroup keys (dmr_forget_call_decryption()), ALG, key ID and MI and each slot's MI chain
 *  are forgotten, as a release forgets them, so the next carrier's frames before their own PI or LC never decrypt with
 *  them, and their voice is held until their calls say whether they are encrypted (dmr_enc_class_hold()). The FID is
 *  left. A tune that stays on the carrier keeps everything: this judgement alone decides, never the engine's reading
 *  of the tuner. */
void dmr_sm_note_carrier_tune(dsd_state* state, long receiver_hz, long freq_hz);

/** @brief dmr_sm_note_carrier_tune() for a tune the machine tracks by @p request_id (a control channel probe, a settle
 *  tune, the Capacity Plus rest move), whose @p result may be PENDING: the receiver has not moved yet, and an RTL tune
 *  that later fails leaves it where it was. A pending tune that would leave the carrier is judged once its request
 *  lands, before the machine or a DMR frame reads anything the carrier it reached sends (dmr_sm_resolve_tracked_tune()),
 *  and the judgement is dropped if the backend reports it failed, so the calls of the carrier the receiver never left
 *  keep their decryption. A request the machine gives up on before it resolves (its deadline, a follow) is judged as
 *  landed, the safe side: the receiver may still reach it. Any other result is judged at once. An earlier judgement
 *  still waiting is settled first, by its own request. */
void dmr_sm_note_tracked_carrier_tune(dmr_sm_ctx_t* ctx, dsd_state* state, dsd_trunk_tune_result result,
                                      uint64_t request_id, long receiver_hz, long freq_hz);

/** @brief Before a DMR frame is read: a tracked tune whose judgement still waits (dmr_sm_note_tracked_carrier_tune())
 *  and whose request has resolved is settled now, so the frame of the carrier it reached is never read with the calls
 *  it left. One load while nothing waits. */
void dmr_sm_resolve_tracked_tune(const dsd_opts* opts, dsd_state* state);

/** @brief An operator's move the tuner accepted, from @p receiver_hz (dmr_sm_receiver_frequency() read before the tune;
 *  0 unknown) to @p freq_hz: a manual return to the control channel, a lockout or skip return, a channel or candidate
 *  cycle. Called once the move ended the left calls (their rows keep the FID): judged as the machine's own follows
 *  are, so a move that left the carrier forgets the left calls' privacy, ALG, key ID, MI and per-slot talkgroup key and
 *  holds the next carrier's voice until its calls say whether they are encrypted (dmr_sm_leave_carrier()). A move to
 *  the carrier the receiver is on keeps everything. */
void dmr_sm_note_operator_leave(dsd_opts* opts, dsd_state* state, long receiver_hz, long freq_hz);

/** @brief An operator's move on the io_control leg under trunking another protocol owns (P25, NXDN), with DMR decoding
 *  on, from @p receiver_hz (the engine's reading before the tune, dsd_engine_receiver_frequency(); 0 unknown) to
 *  @p freq_hz. Called once the move ended the left calls: judged as the engine's tune hook judges another machine's
 *  tune, by that reading, never the machine's own, so a move that left the carrier forgets the left calls' privacy bit,
 *  FID and per-slot talkgroup key and holds the next carrier's voice until its calls say whether they are encrypted.
 *  The machine, which does not own the trunk, is left as it is. */
void dmr_sm_note_foreign_operator_leave(dsd_state* state, long receiver_hz, long freq_hz);

/** @brief The DMR machine's own tunes: dsd_trunk_tuning_hook_tune_to_freq() / _to_cc() (no TED SPS override) inside a
 *  dsd_trunk_tuning_caller_judges_begin() / _end() pair, so the engine's tune hook leaves the judgement of a leave, for
 *  the DMR calls' privacy and the hold, to the machine (dmr_sm_leave_carrier(), dmr_sm_note_carrier_tune()). */
static inline dsd_trunk_tune_result
dmr_sm_tune_to_freq(dsd_opts* opts, dsd_state* state, long freq_hz, uint64_t* out_request_id) {
    dsd_trunk_tuning_caller_judges_begin();
    const dsd_trunk_tune_result result = dsd_trunk_tuning_hook_tune_to_freq(opts, state, freq_hz, 0, out_request_id);
    dsd_trunk_tuning_caller_judges_end();
    return result;
}

static inline dsd_trunk_tune_result
dmr_sm_tune_to_cc(dsd_opts* opts, dsd_state* state, long freq_hz, uint64_t* out_request_id) {
    dsd_trunk_tuning_caller_judges_begin();
    const dsd_trunk_tune_result result = dsd_trunk_tuning_hook_tune_to_cc(opts, state, freq_hz, 0, out_request_id);
    dsd_trunk_tuning_caller_judges_end();
    return result;
}

/** @brief Whether the machine follows a call on @p freq_hz (TUNED there). */
int dmr_sm_follows(const dmr_sm_ctx_t* ctx, long freq_hz);

/** @brief Hand a call that a Capacity Plus channel status followed through the tuning hooks (already tuned to
 *  @p freq_hz) to the machine, which then owns its hangtime, grant timeout and return as for a grant. @p receiver_hz is
 *  where the receiver was before the tune (dmr_sm_receiver_frequency(); 0 unknown): a follow that leaves that carrier
 *  ends its calls (EXPLICIT) there, one that stays on it keeps the calls on the air and their identities. @p slot is the
 *  selected LSN's slot (-1: none): it holds the carrier during acquisition, whatever its identity. @p result and
 *  @p request_id are the follow tune's: one still in flight (PENDING under a request) ends the left calls now but
 *  judges their decryption once it lands, and a tune that fails takes nothing from them, as for a grant's follow. */
void dmr_sm_note_status_follow(dsd_opts* opts, dsd_state* state, long freq_hz, int slot, long receiver_hz,
                               dsd_trunk_tune_result result, uint64_t request_id);

/* ----------------------------------------------------------------------------
 * Talkgroup priority and preemption
 *
 * Every function below runs on the decoder thread under the P25 SM tick guard (frame dispatch and the DMR tick hold
 * it): they write the talkgroup policy's followed-call ledger, whose contract requires that guard, and never take it.
 * ---------------------------------------------------------------------------- */

/* The calls the receiver holds, as the machine's own retention rule sees them. */
typedef struct {
    int held;              // a call holds the receiver: an ordinary grant elsewhere is not followed
    long carrier_hz;       // the carrier held (TUNED: the followed voice channel; ON_CC: where it listens); 0 unknown
    int resident;          // held by calls heard on a parked carrier (composite TSCC, XPT parking), not a followed one
    int unknown_identity;  // a held call has no known identity: only Hold displaces it
    int holds_hold_target; // a held call is on the Hold talkgroup
    int members;           // bit s: slot s holds a call (0x3: both, or the grant's slotless call)
} dmr_sm_busy_view_t;

/* How a candidate on another carrier may take the receiver from the calls it holds. */
typedef enum {
    DMR_SM_DISPLACE_NONE = 0,
    DMR_SM_DISPLACE_HOLD = 1,    // the candidate is on the Hold talkgroup and no held call is
    DMR_SM_DISPLACE_PREEMPT = 2, // dsd_tg_policy_should_preempt() over every held call
} dmr_sm_displace_e;

/**
 * @brief Describe the calls the receiver holds and record them in the talkgroup policy's followed-call ledger.
 *
 * TUNED: held while the machine's retention rule keeps the carrier (live voice, hangtime tail, acquisition before the
 * first voice or a pending receive check; a status-selected carrier's acquisition only for
 * DMR_CAPPLUS_SEED_WINDOW_S). ON_CC: held by calls heard on the carrier it listens on whose identity the policy admits.
 * Each held slot is described (dsd_tg_policy_describe_followed_call(), start = its follow time) and every other ledger
 * slot is cleared, so dsd_tg_policy_should_preempt() compares a candidate with exactly what is held.
 *
 * @return 1 when held.
 */
int dmr_sm_describe_held_calls(dmr_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, double now_m,
                               dmr_sm_busy_view_t* out);

/**
 * @brief Whether a candidate on another carrier may take the receiver from @p view (read-only).
 *
 * Hold: the candidate matches Hold and no held call does. Preempt: a decision the caller evaluated for a known identity,
 * no held call of unknown identity, and dsd_tg_policy_should_preempt() against the ledger dmr_sm_describe_held_calls()
 * wrote. Evaluating several candidates stamps nothing.
 */
dmr_sm_displace_e dmr_sm_may_displace(const dmr_sm_busy_view_t* view, const dsd_opts* opts, const dsd_state* state,
                                      const dsd_tg_policy_call_route* candidate, const dsd_tg_policy_decision* decision,
                                      int policy_valid, int identity_known, double now_m);

/**
 * @brief Submit a grant (or a status selection) to the machine.
 *
 * A candidate on another carrier while the receiver is held (or @ref dmr_sm_event_t::caller_busy) is followed only by
 * Hold over a non-held call or by preemption; a decision-less event never is. One on the carrier already followed
 * (IN_PLACE) names the call of its slot. One that names another call on the followed slot (another target, address
 * class or media) ends that slot's assignment before any admission (dmr_sm_note_grant_assignment()) and is REFUSED:
 * its repeat is followed anew.
 */
dmr_sm_grant_result_e dmr_sm_submit_grant(dsd_opts* opts, dsd_state* state, const dmr_sm_event_t* ev);

/**
 * A checked channel grant names the assignment of its slot whether or not the receiver follows it: a caller that
 * refuses a grant before submitting it (a call-type switch, data following off, the talkgroup policy) still hands it
 * here. One for the followed slot of the followed carrier that names another call than the slot is followed for
 * (another target, address class or media; a repeat of the followed grant or of the call the slot names, whatever its
 * caller, is the same call; a channel status never is another) ends that assignment: its call ends and the receiver stays only for a companion slot whose call still holds the carrier, else it returns to
 * the control channel (retried at the next tick when the tuner refuses the return). Anything else changes nothing.
 * Returns 1 when the grant ended the assignment: it is not followed itself (its repeat is). Called inside the grant's
 * CRC scope. Decoder thread only.
 */
int dmr_sm_note_grant_assignment(dsd_opts* opts, dsd_state* state, const dmr_sm_event_t* ev);

/**
 * @brief Record the followed call in the talkgroup policy ledger after an accepted follow.
 *
 * Clears the ledger, notes @p route, and commits the takeover's cooldown when @p preempted.
 */
void dmr_sm_policy_note_follow(dsd_state* state, const dsd_tg_policy_call_route* route,
                               const dsd_tg_policy_decision* decision, int preempted, double now_m);

/**
 * @brief A voice LC that passed its check named the call on @p slot: the slot's identity, with LC provenance, bound to
 * the canonical call the LC was just published on (it ends with that call).
 *
 * Recorded for the carrier the machine follows (TUNED) or listens on (ON_CC). A same-call LC keeps the follow time.
 */
void dmr_sm_note_lc_identity(const dsd_opts* opts, const dsd_state* state, int slot, dsd_call_kind kind,
                             uint32_t target, uint32_t source);

/**
 * @brief A Capacity Plus status names group @p tg on @p lsn: a status-followed slot whose identity a seed gave on that
 * LSN, whose named call is over (an LC's included: its authority ends with its call), or that has no identity at all
 * (one that expired with its call while the companion slot kept the carrier), takes the group, with a new follow time
 * unless it is the same call, and the status's CRC provenance (@p crc_flags, DSD_CRC_*: every fragment's). An @p active LSN with no group (@p tg 0: a private or data call) leaves that
 * slot's identity unknown and its activity as it was; an idle one (neither) changes nothing. A current checked LC's
 * record that names no channel takes the LSN alone, with the status's provenance.
 */
void dmr_sm_status_identity_reconcile(dmr_sm_ctx_t* ctx, const dsd_state* state, int slot, int lsn, uint32_t tg,
                                      int active, uint8_t crc_flags);

/**
 * @brief A channel status (Hytera XPT) names the call on @p slot (LSN @p lsn) of carrier @p freq_hz: group @p tg, or
 * (@p tg 0, @p active) a call of no known identity (a private call). On the carrier the machine follows, a slot whose
 * identity a grant gave (not a checked LC naming the call still there) takes the group on its own LSN, with a new
 * follow time when the call changes, or becomes unknown; on the followed slot the grant's identity follows it, so it no
 * longer stands for the slot's call either. A checked LC's record that names no channel yet takes the LSN alone. An
 * idle slot (neither) changes nothing.
 */
void dmr_sm_status_slot_reconcile(dmr_sm_ctx_t* ctx, const dsd_state* state, int slot, int lsn, long freq_hz,
                                  uint32_t tg, int active);

/** @brief Return 1 while the startup window is armed or a startup hunt runs (issue #703). */
int dmr_sm_cold_start_active(const dmr_sm_ctx_t* ctx);

/** @brief Return 1 while the decoder's DMR tick has startup work: the window armed or a hunt running, or an IDLE
 *  admitted machine whose window the next tick arms (issue #703). Decoder thread, under the P25 SM tick guard. */
int dmr_sm_cold_start_due(const dmr_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Remember the group a Capacity Plus channel status named for @p slot of the carrier it was followed to.
 *
 * Called right after the follow tune was accepted, with the tuning generation read BEFORE that tune was requested and
 * the receiver frequency the tune left in opts->rtlsdr_center_freq. @p tg 0 clears the slot. The seed only ever names
 * a call whose voice LCs have not: see dmr_sm_emit_voice_sync().
 */
void dmr_sm_capplus_seed_arm(dmr_sm_ctx_t* ctx, const dsd_state* state, int slot, uint32_t tg, int lsn, long freq_hz,
                             uint32_t receiver_hz, uint8_t crc_flags, uint64_t generation_before_tune);

/** @brief Forget the Capacity Plus channel-status seed of @p slot (-1: both). */
void dmr_sm_capplus_seed_clear(dmr_sm_ctx_t* ctx, int slot);

/** @brief Forget the seed of @p slot unless it still names @p tg on @p lsn, as a later channel status reports it. */
void dmr_sm_capplus_seed_reconcile(dmr_sm_ctx_t* ctx, int slot, int lsn, uint32_t tg);

/**
 * @brief Get current state machine state.
 */
static inline dmr_sm_state_e
dmr_sm_get_state(const dmr_sm_ctx_t* ctx) {
    return ctx ? ctx->state : DMR_SM_IDLE;
}

/**
 * @brief Get human-readable state name.
 */
const char* dmr_sm_state_name(dmr_sm_state_e state);

/**
 * @brief Access the global singleton state machine instance.
 */
dmr_sm_ctx_t* dmr_sm_get_ctx(void);

/* ============================================================================
 * Public API - Convenience Emit Functions (use global singleton)
 * ============================================================================ */

/**
 * @brief Emit voice sync event for a slot.
 */
void dmr_sm_emit_voice_sync(dsd_opts* opts, dsd_state* state, int slot);

/**
 * @brief Emit data sync event for a slot.
 */
void dmr_sm_emit_data_sync(dsd_opts* opts, dsd_state* state, int slot);

/**
 * @brief Emit release event.
 */
void dmr_sm_emit_release(dsd_opts* opts, dsd_state* state, int slot);

/**
 * @brief Release the one assignment a slot termination naming @p target, received on @p slot, ends (Connect Plus
 * opcode 0x0C): the slot of the followed carrier whose known call has that target, else the slot it arrived on. Never
 * the whole carrier, so the other slot's followed call or transfer stays live.
 */
void dmr_sm_emit_slot_termination(dsd_opts* opts, dsd_state* state, int slot, uint32_t target);

/**
 * @brief Emit a group voice grant event.
 *
 * The emit_*_grant wrappers carry no policy decision: while the receiver holds a call they are busy, never a Hold or
 * preemption candidate (dmr_sm_submit_grant()); with nothing held they are followed as before.
 */
void dmr_sm_emit_group_grant(dsd_opts* opts, dsd_state* state, long freq_hz, int lpcn, int tg, int src);

/**
 * @brief Emit a group voice grant with its advertised TDMA slot.
 */
void dmr_sm_emit_group_grant_slot(dsd_opts* opts, dsd_state* state, long freq_hz, int lpcn, int slot, int tg, int src);

/**
 * @brief Emit an individual voice grant event.
 */
void dmr_sm_emit_indiv_grant(dsd_opts* opts, dsd_state* state, long freq_hz, int lpcn, int dst, int src);

/**
 * @brief Emit an individual voice grant with its advertised TDMA slot.
 */
void dmr_sm_emit_indiv_grant_slot(dsd_opts* opts, dsd_state* state, long freq_hz, int lpcn, int slot, int dst, int src);

/**
 * @brief Initialize the global state machine singleton.
 */
void dmr_sm_init(const dsd_opts* opts, const dsd_state* state);

/* ============================================================================
 * Public API - Neighbor/CC Candidate Management
 * ============================================================================ */

/**
 * @brief Update neighbor/alternate control channel list.
 */
void dmr_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count);

/* ============================================================================
 * Helper: Create events
 * ============================================================================ */

static inline dmr_sm_event_t
dmr_sm_ev_group_grant(long freq_hz, int lpcn, int tg, int src) {
    dmr_sm_event_t ev = {0};
    ev.type = DMR_SM_EV_GRANT;
    ev.slot = -1;
    ev.freq_hz = freq_hz;
    ev.lpcn = lpcn;
    ev.tg = tg;
    ev.src = src;
    ev.is_group = 1;
    ev.kind = DSD_CALL_KIND_GROUP_VOICE;
    ev.identity_known = 1;
    return ev;
}

static inline dmr_sm_event_t
dmr_sm_ev_indiv_grant(long freq_hz, int lpcn, int dst, int src) {
    dmr_sm_event_t ev = {0};
    ev.type = DMR_SM_EV_GRANT;
    ev.slot = -1;
    ev.freq_hz = freq_hz;
    ev.lpcn = lpcn;
    ev.dst = dst;
    ev.src = src;
    ev.is_group = 0;
    ev.kind = DSD_CALL_KIND_PRIVATE_VOICE;
    ev.identity_known = 1;
    return ev;
}

static inline dmr_sm_event_t
dmr_sm_ev_voice_sync(int slot) {
    dmr_sm_event_t ev = {0};
    ev.type = DMR_SM_EV_VOICE_SYNC;
    ev.slot = slot;
    return ev;
}

static inline dmr_sm_event_t
dmr_sm_ev_data_sync(int slot) {
    dmr_sm_event_t ev = {0};
    ev.type = DMR_SM_EV_DATA_SYNC;
    ev.slot = slot;
    return ev;
}

static inline dmr_sm_event_t
dmr_sm_ev_release(int slot) {
    dmr_sm_event_t ev = {0};
    ev.type = DMR_SM_EV_RELEASE;
    ev.slot = slot;
    return ev;
}

static inline dmr_sm_event_t
dmr_sm_ev_cc_sync(void) {
    dmr_sm_event_t ev = {0};
    ev.type = DMR_SM_EV_CC_SYNC;
    ev.slot = -1;
    return ev;
}

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_TRUNK_SM_H_H */
