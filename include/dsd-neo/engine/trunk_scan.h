// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Engine-owned single-tuner trunk scan coordinator.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_SCAN_H_
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_SCAN_H_

#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/scan_entry.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#else
/* C needs a file-scope tag before the parameter declaration below. */
struct p25_bandplan_row;
#endif

enum {
    DSD_TRUNK_SCAN_DWELL_MIN_MS = 250,
    DSD_TRUNK_SCAN_DWELL_MAX_MS = 600000,
    DSD_TRUNK_SCAN_IDLE_DWELL_DEFAULT_MS = 3000,
    DSD_TRUNK_SCAN_ACTIVITY_HOLD_DEFAULT_MS = 1200,
    /* Ceiling on the memory the parked-target snapshots may occupy. See
     * dsd_trunk_scan_max_targets(). */
    DSD_TRUNK_SCAN_TARGET_MEMORY_BUDGET_BYTES = 256 * 1024 * 1024,
};

/**
 * @brief Largest number of trunk scan targets that fits in the target memory budget.
 *
 * There is no target-count limit as such: each parked target reserves a snapshot of decoder
 * state, and the cap is DSD_TRUNK_SCAN_TARGET_MEMORY_BUDGET_BYTES divided by that snapshot's
 * size, so it tracks the struct instead of being a constant that goes stale. Enforced while the
 * targets CSV is parsed - an unbounded row count would otherwise ask for a single allocation
 * large enough that Linux overcommit grants it and the process is killed while faulting the
 * pages in, rather than failing with a message the operator can act on.
 */
size_t dsd_trunk_scan_max_targets(void);

typedef enum {
    DSD_TRUNK_SCAN_TARGET_P25_TRUNK = 0,
    DSD_TRUNK_SCAN_TARGET_DMR_TRUNK = 1,
    DSD_TRUNK_SCAN_TARGET_DMR_CONVENTIONAL = 2,
    DSD_TRUNK_SCAN_TARGET_NXDN_TRUNK = 3,
    DSD_TRUNK_SCAN_TARGET_NXDN_CONVENTIONAL = 4,
    DSD_TRUNK_SCAN_TARGET_NXDN48_CONVENTIONAL = 5,
    DSD_TRUNK_SCAN_TARGET_NXDN48_TRUNK = 6,
    DSD_TRUNK_SCAN_TARGET_P25_CONVENTIONAL = 7,
    /** One analog NFM channel (issue #526): carrier activity, no frames, keys or symbol clock. */
    DSD_TRUNK_SCAN_TARGET_NFM_CONVENTIONAL = 8,
    /** One analog AM channel (issue #526): the AM monitor (-fM), otherwise as an nfm-conventional target. */
    DSD_TRUNK_SCAN_TARGET_AM_CONVENTIONAL = 9,
} dsd_trunk_scan_target_type;

typedef enum {
    DSD_TRUNK_SCAN_MODULATION_UNSET = DSD_SCAN_MODULATION_INHERIT,
    DSD_TRUNK_SCAN_MODULATION_AUTO = DSD_SCAN_MODULATION_AUTO,
    DSD_TRUNK_SCAN_MODULATION_C4FM = DSD_SCAN_MODULATION_C4FM,
    DSD_TRUNK_SCAN_MODULATION_CQPSK = DSD_SCAN_MODULATION_CQPSK,
    DSD_TRUNK_SCAN_MODULATION_GFSK = DSD_SCAN_MODULATION_GFSK,
} dsd_trunk_scan_modulation;

typedef struct {
    char id[64];
    /** The optional `label` column, sanitized (dsd_channel_label_sanitize()): names the channel while parked here. */
    char label[64];
    dsd_trunk_scan_target_type type;
    uint32_t frequency_hz;
    char chan_csv[1024];
    /* Per-target key files, resolved relative to the targets CSV at parse time.
     * Loaded into the target's key set at init; empty means the global keys. */
    char keys_hex_csv[1024];
    char keys_dec_csv[1024];
    /* Parsed direct `-H`/`-b` equivalents. Kept as fixed metadata so raw key
     * text never survives parsing or reaches a diagnostic. */
    dsd_key_scalars single_key_scalars;
    uint8_t single_keys_present;
    /** Nonsecret, validated per-target overrides. Group path is relative-resolved at import. */
    dsd_scan_option_values row_options;
    /* Per-target P25 band plan CSV (trunk targets only), resolved relative to the targets CSV at
     * parse time and loaded into the target's own band-plan store at init; empty means none. */
    char p25_bandplan_csv[1024];
    int dwell_ms;
    int activity_hold_ms;
    int dwell_is_set;
    int activity_hold_is_set;
    unsigned int csv_row;
    dsd_trunk_scan_modulation modulation;
    int rtl_gain_is_set;
    int rtl_gain_db;
} dsd_trunk_scan_target;

typedef struct {
    dsd_trunk_scan_target* targets; /**< Owned heap array; NULL when empty. */
    size_t count;
    size_t capacity;
} dsd_trunk_scan_target_list;

/**
 * @brief Release a target list's owned storage and zero it.
 *
 * The list returned by a successful dsd_trunk_scan_load_targets_csv() owns its
 * `targets` array; callers must reset it once the coordinator has copied the
 * targets out. A failed load never writes `*out`, so nothing to reset there.
 *
 * @param list List to clear; NULL is ignored.
 */
void dsd_trunk_scan_target_list_reset(dsd_trunk_scan_target_list* list);

int dsd_trunk_scan_load_targets_csv(const char* path, const dsd_opts* opts, dsd_trunk_scan_target_list* out, char* err,
                                    size_t err_sz);

/**
 * @brief Whether target lists loaded one by one can run in one session (issue #697).
 *
 * No target id repeats across the lists (each list's own rules stay its parser's), and their union fits
 * dsd_trunk_scan_max_targets(). The error names the files from @p names[i] (list i's file) and both rows.
 *
 * @return 0 when they can; -1 otherwise.
 */
int dsd_trunk_scan_check_target_lists(const dsd_trunk_scan_target_list* lists, const char* const* names, size_t count,
                                      char* err, size_t err_sz);

/** Shared lexical splitter for target import/relocation. Modifies line in place. */
int dsd_trunk_scan_split_csv_fields(char* line, char** fields, size_t max_fields, size_t* out_count);

int dsd_engine_trunk_scan_init(dsd_opts* opts, dsd_state* state, char* err, size_t err_sz);
/**
 * @brief Detach the coordinator and put back the session's configured trunking and gain.
 *
 * A paused scan (issue #671) saves no live state into its target: the target was saved as it paused, and the live
 * state is the manual session's. The detach then runs on a session already detached from every target scope.
 */
void dsd_engine_trunk_scan_shutdown(dsd_opts* opts, dsd_state* state);

/** What starts a trunk-scan pause (issue #671). */
typedef enum {
    /** An accepted manual tune: the caller runs the TUNE carrier boundary after the pause, which forgets the carrier
     *  left (codes, decoding, received identity). */
    DSD_TRUNK_SCAN_PAUSE_TUNE = 0,
    /** No tune follows (`y`, a tuner release): the pause forgets the carrier's decoding itself, as a target switch
     *  does, so nothing decoded under the target's policy goes on into the manual session. */
    DSD_TRUNK_SCAN_PAUSE_IN_PLACE = 1,
} dsd_trunk_scan_pause_kind;

/**
 * @brief What a pause, or a resume that left the scan paused, asked of the receiver, for the caller to settle.
 *
 * The scope leave inside them asks the RTL front end back onto the configured receive profile, as a -Y stop does:
 * @p monitor_request is that request's result (1 queued, -1 refused at once, 0 none made), which app-control records
 * and reconciles for the decoder as it does for a -Y leave (issue #578). @p restart_gain: the target left ran a tuner
 * gain of its own other than the configured one, which the device keeps until the stream reopens with the configured
 * gain (now dsd_opts::rtl_gain_value) and autogain: a retune carries no gain profile outside a running scan.
 */
typedef struct {
    int monitor_request;
    int restart_gain;
} dsd_trunk_scan_leave_report;

/**
 * @brief Pause the running trunk scan (issue #671): the coordinator stays, with its targets, snapshots, avoids, session
 * edits and operator hold, and the receiver goes to the operator.
 *
 * In order: the target's carrier is released (its calls end and commit under its own name, keys and policy; a P25
 * Phase 2 superframe in progress is delivered first); every tune request in flight for it is let go of
 * (dsd_trunk_tuning_request_disown(), never completed), captured before the release; the target is saved, idle; the
 * paused publication goes up (dsd_state::trunk_scan_paused, no target named, the configured gain, autogain and
 * trunking and the hold kept); an empty decoder state replaces the target's; the scopes are left (configured decoder,
 * receive profile and rigctl passband); the configured gain, trunking off, the configured keys, policy and maps. With
 * DSD_TRUNK_SCAN_PAUSE_IN_PLACE the carrier's decoding is then forgotten. While paused nothing the coordinator owns
 * acts (dsd_trunk_scan_owns_tuner() is 0): no tick, yield, hold, state machine, live decryption or control; target
 * edits are stored for the next visit.
 *
 * The caller holds the P25 SM tick guard, and nothing here takes it. A pending manual-tune record names no scan
 * (dsd_engine_manual_tune_supersede() is the tune's business, not the pause's). The app-control wrapper brackets this
 * call with its monitor-request record (svc_leave_channel_scan()'s): it reads whether a request ran before, calls
 * this, then records @p out->monitor_request, and reopens the stream for the configured gain when @p
 * out->restart_gain.
 *
 * @return 0 when the scan is paused (already paused included, nothing done, @p out zeroed), -1 when no trunk scan runs.
 */
int dsd_engine_trunk_scan_pause_ex_locked(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_pause_kind kind,
                                          dsd_trunk_scan_leave_report* out);
/** @brief dsd_engine_trunk_scan_pause_ex_locked() without the report. */
int dsd_engine_trunk_scan_pause_locked(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_pause_kind kind);

/**
 * @brief Resume a paused trunk scan (issue #671) on the target it left.
 *
 * The manual session's calls end first, under no target's name; then the target is parked again as a revisit: scopes
 * begun around the session as it is now, its snapshot from the pause restored, its keys, policy, maps, options and
 * trunking installed, fresh visit and acquisition clocks. The operator hold stays.
 *
 * A resume that cannot park leaves a coherent pause, the paused flags put back before anything is torn down: a
 * preparation failure (no memory for the scopes or keys) drops the scopes it began; a refused retune puts the manual
 * session's state and received identity back, then leaves the target's options, keys, policy and maps, then the
 * scopes, whose front-end request (@p out->monitor_request, recorded as for a pause) comes last, and
 * @p out->restart_gain is set as a pause sets it, for the target's own gain the retune may have applied. The status
 * line says why. A later resume tries again at once.
 *
 * The caller holds the P25 SM tick guard, and nothing here takes it.
 *
 * @return 0 when the scan runs again (or was running), 1 when it runs with the park still in flight (a second resume
 * then changes nothing), -1 when no trunk scan runs, -2 when the preparation failed, -3 when the retune was refused.
 */
int dsd_engine_trunk_scan_resume_ex_locked(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_leave_report* out);
/** @brief dsd_engine_trunk_scan_resume_ex_locked() without the report. */
int dsd_engine_trunk_scan_resume_locked(dsd_opts* opts, dsd_state* state);

/**
 * @brief What a go-to (dsd_engine_trunk_scan_goto_locked()) asked of the receiver, for the caller to settle and name.
 *
 * @p monitor_request and @p restart_gain are a paused scan's, as dsd_trunk_scan_leave_report's: a resume-at that
 * stayed paused left the target's scope (its front-end request, recorded as for a pause) and may have put the target's
 * own gain on the device. A running scan's go-to sets neither: the scan keeps the receiver, and its retune carries the
 * gain. @p park_hz is the frequency the target's park asked the tuner for, or would ask for: a trunked target's learned
 * control channel ahead of its CSV seed, a conventional target's own frequency; its CSV seed when its preparation
 * failed before any tune.
 */
typedef struct {
    int monitor_request;
    int restart_gain;
    long park_hz;
} dsd_trunk_scan_goto_report;

/**
 * @brief Send the trunk scan to target @p index and hold it there, marked an entry hold (issue #644 D1): the target a
 * frequency typed by hand picked (dsd_engine_scan_entry_match()).
 *
 * Only a target the rotation takes: in a list that is on, not avoided, and not an analog target skipped at every visit
 * for its width. Its retry cooldown does not keep it out: the pick is explicit. Anything else, and no trunk scan, is
 * REFUSED with nothing changed. A trunked target parks as any visit does, on its learned control channel first, and
 * follows its calls.
 * - A target confirmed on air (dsd_engine_trunk_scan_target_confirmed()) needs no tune: the hold only (HOLDING).
 * - A running scan whose park of that target is in flight waits on that park (PENDING).
 * - Otherwise a running scan switches to the target as `L` does, one target long: a parked visit's carrier is handed
 *   back and its calls end under its own name, then the target is parked. The park ends the calls of the target left
 *   before it tunes, so only a preparation that fails (SETUP_FAILED) moves nothing: the scan, its park in flight and an
 *   earlier entry stay as they were. A retune refused or deferred (REFUSED, DEFERRED) is rolled back as an advance's,
 *   with a recovery park of the target left; a recovery that fails too leaves it installed and cooling down, as a
 *   failed advance does. A single target works the same way.
 * - A paused scan resumes at the target as dsd_engine_trunk_scan_resume_ex_locked() resumes, with the target given
 *   in place of the one it paused at: every failure leaves it paused, with the toast, and with the target it paused at
 *   still its resume target; @p report carries what that failure asks of the caller.
 *
 * The hold (dsd_state::trunk_scan_hold) and its entry mark (dsd_state::trunk_scan_entry_hold) are set on the park's
 * confirmed completion only: at once (HOLDING), or when the park in flight (PENDING) lands in this scan session while
 * the entry is still the one in force; a P25 control channel the target's state machine recovered counts as that
 * landing. Every other park cancels it (an advance, a departure, a retry in place, a resume, a later go-to that gets
 * past its preparation), and so do `L`, `J` and an avoid whether or not they move, `Y`, a pause and the park's own
 * failure. A go-to from a target an entry holds takes that hold and its mark once it gets past its preparation, so a
 * park that fails late leaves the scan running unheld on its normal failure path; an ordinary hold carries over, as
 * `L` carries it. Running or paused, the caller holds the P25 SM tick guard, and nothing here takes it. Decoder
 * thread.
 *
 * @p report (may be NULL) is filled on every return.
 */
dsd_scan_goto_result dsd_engine_trunk_scan_goto_locked(dsd_opts* opts, dsd_state* state, int index,
                                                       dsd_trunk_scan_goto_report* report);

/**
 * @brief Release an entry hold (`y`, SCAN_RESUME; issue #644 D1): the hold and its entry mark go, and the target gets
 * a fresh idle dwell and visit cap as at `Y` off, so the scan continues from it. An entry still waiting on its park is
 * cancelled too. Nothing else without an entry hold: an ordinary hold stays. The caller holds the P25 SM tick guard.
 * Decoder thread.
 */
void dsd_engine_trunk_scan_entry_release_locked(const dsd_opts* opts, dsd_state* state);

/** @brief Whether an entry waits on its target's park (a go-to that answered PENDING): the park is still in flight in
 * this scan session and nothing has cancelled the entry. 0 without a coordinator. Decoder thread. */
int dsd_engine_trunk_scan_entry_pending(const dsd_state* state);

/** The radio stream is about to stop (issue #644). The stop fails the park it queued; a park of the target on air that
 * can still land is noted, with the frequency it asked for and the source of the stream that stops, for the reopen that
 * follows (dsd_engine_trunk_scan_stream_reopened()). Nothing else changes. Decoder thread, under the P25 SM tick
 * guard, before the stream stops. */
void dsd_engine_trunk_scan_stream_stopping(const dsd_opts* opts, const dsd_state* state);

/** The stream a restart stopped opened again (@p opened 1), or did not (0), the restart's or the recovery's after it
 * (issue #644). The park the stop noted, still the coordinator's, lands when the new stream opened on the same source,
 * not an I/Q replay, with the centre the frequency it asked for: its request is completed as landed whatever the stop
 * published (dsd_trunk_tuning_request_land_at_reopen()), so the next tick takes it as any landing, an entry waiting on
 * it held. Otherwise the note goes, and the stop's failure stands. Decoder thread, under the P25 SM tick guard. */
void dsd_engine_trunk_scan_stream_reopened(const dsd_opts* opts, const dsd_state* state, int opened);

/** @brief Whether a trunk-scan coordinator exists and is paused (issue #671). */
int dsd_engine_trunk_scan_paused(const dsd_state* state);

void dsd_engine_trunk_scan_tick(dsd_opts* opts, dsd_state* state);
/** Whether the idle dwell of the target on air has run out, so the next tick would rotate (discussion #717): the
 * frame-sync hunt asks every 300 symbols through dsd_trunk_scan_hook_dwell_due() and returns early, and the tick then
 * advances under its guard. Counts a park still unnoticed from its landing, as the tick arms it. 1 at most once per
 * dwell start. Decoder thread only. */
int dsd_engine_trunk_scan_dwell_due(const dsd_opts* opts, const dsd_state* state);
/** Maintain the visit clock and report expiry without tuning or releasing the call.
 * Decoder thread only; the caller must hold the SM guard. Used by long protocol loops
 * to unwind before the next coordinator tick performs the advance. */
int dsd_engine_trunk_scan_visit_expired(const dsd_opts* opts, dsd_state* state);
/** Whether the receive policy lets go of the conventional target on air (issue #637): it admits none of its traffic
 * past the acquisition window, no operator hold keeps it, and another target can take the receiver. Read-only, so
 * the frame loop can yield on it mid-transmission; the coordinator's tick then advances. */
int dsd_engine_trunk_scan_receive_expired(const dsd_opts* opts, const dsd_state* state);
/**
 * @brief Apply an operator scan control (a dsd_trunk_scan_control_op from
 * runtime/trunk_scan_hooks.h) to the parked target list.
 *
 * Hold keeps the idle dwell on the active target from expiring without stopping its state machine;
 * avoid flags the active target for the session and moves on; clear puts every avoided
 * target back; advance moves to the next eligible target now and retreat to the previous one
 * (issue #666). Avoid and every automatic rotation walk forward; only the hold toggle changes the hold.
 *
 * Returns the codes documented beside the op enum; DSD_TRUNK_SCAN_CONTROL_PAUSED for every op but
 * clear while the scan is paused (issue #671; clear only edits flags, issue #672). Avoid answers
 * DSD_TRUNK_SCAN_CONTROL_ALREADY_AVOIDED, doing nothing, when the active target is avoided already (the
 * receiver fell back onto it). Avoid, advance and retreat return 0 when the active
 * target changed and 1 when it did not: no other target was eligible (all avoided or cooling
 * down after a failed retune), no switch could be prepared (its key sets did not allocate), or
 * every alternate's retune failed and the receiver fell back onto the original. The caller cannot
 * tell these apart, so its message names only the outcome. Advance and retreat answer
 * DSD_TRUNK_SCAN_CONTROL_REFUSED on a single-target list and avoid when it would leave no
 * usable target; DSD_TRUNK_SCAN_CONTROL_BUSY when the P25 tick guard is held.
 * A control that succeeds rebuilds the published avoid list when it changed an avoid
 * (dsd_state::trunk_scan_avoids, under a new trunk_scan_avoid_rev), and publishes the timing only with the P25 tick
 * guard held: when it is held elsewhere the next tick publishes it. A paused scan's timing stays cleared.
 * Decoder thread only: avoid, advance and retreat retune.
 */
int dsd_engine_trunk_scan_control(dsd_opts* opts, dsd_state* state, int op);
/**
 * @brief Put one avoided target back into the rotation (issue #672): the runtime hook
 * dsd_trunk_scan_hook_unavoid() and its return codes.
 *
 * @p session and @p target_id name the target (the published dsd_state::trunk_scan_session and the target's id);
 * DSD_TRUNK_SCAN_CONTROL_STALE when the session is another scan's (or 0) or has no such target,
 * DSD_TRUNK_SCAN_CONTROL_NOT_AVOIDED when it is not avoided, DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE with no coordinator.
 * On DSD_TRUNK_SCAN_CONTROL_OK only the flag changed: no retune, and the hold, every retry cooldown and the dwell stay;
 * the visit cap restarts only when the target is the first eligible alternate again, as clear's does. The avoid list
 * and the target are republished, and the timing as a control publishes it. Allowed while paused. Decoder thread only.
 * On OK and NOT_AVOIDED, @p name (may be NULL) receives the target's 1-based list position and label column.
 */
int dsd_engine_trunk_scan_unavoid_target(const dsd_opts* opts, dsd_state* state, uint32_t session,
                                         const char* target_id, dsd_trunk_scan_target_name* name);
/**
 * @brief Switch a target list on or off for the session (issue #697): the coordinator's side of the runtime
 * `list_set` hook (dsd_trunk_scan_hook_list_set(), whose rules and results it implements).
 *
 * A list switched off keeps its targets, snapshots, avoids and edits; the rotation passes them over until it is on
 * again. Every candidate walk and count reads one predicate, in the rotation = list on and not avoided. Decoder
 * thread; takes the P25 SM tick guard (try-enter) itself.
 */
int dsd_engine_trunk_scan_list_set(dsd_opts* opts, dsd_state* state, uint32_t session, int list, int enabled,
                                   uint32_t request);
/**
 * @brief Whether the target on air owes a departure that is due now (issue #697): it is out of the rotation (its list
 * is off, or it is the avoided fallback) and another target can take the receiver, or could when it was last asked.
 *
 * Joins the frame loop's yield (scan_visit_should_yield) beside visit expiry and receive rejection, so a continuously
 * keyed DMR carrier unwinds at a burst boundary and the next coordinator tick departs after protocol cleanup. Reads
 * two coordinator fields and the decode clock, never the target list, since it runs every frame. 0 while paused.
 */
int dsd_engine_trunk_scan_departure_due(const dsd_opts* opts, const dsd_state* state);
void* dsd_engine_trunk_scan_active_p25_ctx(void);
void* dsd_engine_trunk_scan_active_dmr_ctx(void);
/**
 * @brief Channel-map path of the currently parked scan target, or NULL when it has none.
 *
 * Backs the `active_chan_csv` runtime hook: per-target `chan_csv` files are imported through
 * throwaway options, so this is the only place the path survives for protocol code to name.
 */
const char* dsd_engine_trunk_scan_active_chan_csv(const dsd_state* state);
/**
 * @brief Report decoded conventional activity so the active target keeps its park.
 *
 * Each entry point only acts when the target currently parked belongs to its own
 * conventional family; anything else (including trunk targets, or trunk scan not
 * being installed) is ignored. The NXDN entry point serves both NXDN conventional
 * types: NXDN48 and NXDN96 share a sync word and every decoded element, so the
 * protocol layer cannot tell them apart and must not have to. The call identity is run through the global
 * talkgroup policy, and only an allowed call refreshes the target's
 * activity hold, so allow/block lists, private-call, data-call and
 * encrypted-call tuning controls all apply to the park decision.
 *
 * Callers must pass identity that has already cleared the protocol's own
 * FEC/CRC gate: an unverified header would park the coordinator on noise.
 *
 * @param opts       Decoder options (policy and hold controls).
 * @param state      Decoder state owning the scan coordinator.
 * @param target     Destination talkgroup (group call) or destination unit (private call).
 * @param source     Source unit id, or 0 when unknown.
 * @param is_private Non-zero for a private/individual call, zero for a group call.
 * @param encrypted  Non-zero when the call is encrypted; pass the protocol's corroborated
 *                   classification, not a single frame's raw cipher field.
 * @param data_call  Non-zero for data traffic rather than voice.
 */
void dsd_engine_trunk_scan_dmr_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                                     uint32_t source, int is_private, int encrypted, int data_call);
/** @copydoc dsd_engine_trunk_scan_dmr_conventional_activity */
void dsd_engine_trunk_scan_nxdn_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                                      uint32_t source, int is_private, int encrypted, int data_call);
/** @copydoc dsd_engine_trunk_scan_dmr_conventional_activity
 * @param protocol The call's DSD_SYNC_P25* protocol (Phase 1 or Phase 2), judged by the receive policy.
 * @param slot     The call's slot: 0 for Phase 1, 0/1 for Phase 2. */
void dsd_engine_trunk_scan_p25_conventional_activity(const dsd_opts* opts, const dsd_state* state, int protocol,
                                                     int slot, uint32_t target, uint32_t source, int is_private,
                                                     int encrypted, int data_call);
size_t dsd_engine_trunk_scan_target_count(const dsd_state* state);
/**
 * @brief Whether the scan running now visits an analog row that runs the configured NFM width (issue #526).
 *
 * True while the trunk-scan coordinator holds an nfm-conventional target, or the conventional -Y scanner runs a channel
 * map with an nfm row, that sets no --nfm-bandwidth-hz of its own: such a row runs the configured NFM width whenever it
 * is on air, on any session. The width controls, a loaded config, the RTL DSP bandwidth and Input > Switch source hold
 * the configured width to the DSP rate while this holds, as they do under -fA, so a row is not skipped for a width an
 * edit made while another row was on air. Read-only; 0 when @p opts or @p state is NULL.
 */
int dsd_engine_scan_runs_configured_nfm_width(const dsd_opts* opts, const dsd_state* state);
/**
 * @brief dsd_engine_scan_runs_configured_nfm_width() for the configured AM width: whether the scan running now visits
 * an am row or am-conventional target that sets no --am-bandwidth-hz of its own, and so runs the configured AM width
 * (its 6 kHz default when none is set, which a DSP rate holds too) whenever it is on air, on any session (issue #526).
 */
int dsd_engine_scan_runs_configured_am_width(const dsd_opts* opts, const dsd_state* state);
/**
 * @brief Whether anything in the session runs received-tone detection, so the configured tone policy can act (#527).
 *
 * Under the trunk-scan coordinator, an nfm-conventional target: every target runs its own type's mode, never the
 * configured one. A paused coordinator answers for the configured session in force, as below (issue #671). Otherwise
 * dsd_channel_modes_conventional_hear_tones(): on the conventional -Y scanner with a channel
 * map, a row that runs the FM monitor (dsd_channel_modes_hear_tones()); without rows, the configured decode mode is the
 * analog FM monitor (dsd_scan_mode_configured_fm_monitor()). A --trunk-scan session whose coordinator has not started
 * counts as hearing them: its targets are judged when the scan starts. Read-only; 0 when @p opts or @p state is NULL.
 */
int dsd_engine_scan_hears_tones(const dsd_opts* opts, const dsd_state* state);
/**
 * @brief The tuner autogain a target without its own manual gain runs: the session's at scan start, or the latest
 * live edit (dsd_engine_trunk_scan_set_configured_autogain()). Returns 1 with @p out_on set, 0 when no trunk scan runs
 * or the value was never known (@p out_on 0).
 */
int dsd_engine_trunk_scan_saved_tuner_autogain(const dsd_state* state, int* out_on);
/**
 * @brief Make @p gain the tuner gain the trunk scan restores at every target switch and at shutdown (issue #518
 * follow-up: a live gain edit used to be put back to the scan-start value at the next switch).
 *
 * The configured gain is what targets without an rtl_gain column run; a target with its own keeps it while it is on
 * air, as a row's --squelch-db overrides the configured squelch. Publishes the result for the frontends. Decoder
 * thread, with the P25 SM tick guard held: the watchdog's control-channel returns read the configured gain and
 * autogain through the retune gain profile under it.
 *
 * @return 1 when the gain is in force now (written to opts->rtl_gain_value), 0 when the parked target's own rtl_gain
 *         overrides it (@p opts unchanged), -1 when no trunk scan runs (nothing changed).
 */
int dsd_engine_trunk_scan_set_configured_gain(dsd_opts* opts, dsd_state* state, int gain);
/**
 * @brief Make @p on the tuner autogain the trunk scan restores at every retune (the same rule as the configured gain).
 * Decoder thread, with the P25 SM tick guard held.
 *
 * @return 1 when it is in force now (the gain in force is AGC; the caller applies it to the stream), 0 when a manual
 *         gain suspends it (every trunk-scan retune forces the supervisor off then), -1 when no trunk scan runs.
 */
int dsd_engine_trunk_scan_set_configured_autogain(const dsd_opts* opts, dsd_state* state, int on);
/**
 * @brief Make @p enabled the trunking setting (dsd_opts::trunk_enable) the trunk scan restores at shutdown, and publish
 * it as state->trunk_scan_configured_trunk_enable for the config save (issue #671).
 *
 * Each target's type sets the live flag while it is parked, so the configured setting lives in the coordinator; the
 * live flag is never written here. Decoder thread.
 *
 * @return 0 when it is stored, -1 when no trunk scan runs (nothing changed).
 */
int dsd_engine_trunk_scan_set_configured_trunk_enable(dsd_state* state, int enabled);
int dsd_engine_trunk_scan_active_p25_cqpsk_request(const dsd_state* state, int* out_enable);
/**
 * @brief Symbol rate of the parked four-level GFSK scan target, or 0 when there is none.
 *
 * The tuning layer cannot derive this on its own. `state->rf_mod == 2` is true for both the
 * 4800 sym/s targets (DMR, NXDN96) and the 2400 sym/s ones (NXDN48), and `state->sps_hunt_idx`
 * is rotated by the no-sync SPS hunt, so a plain `-T` session can be sitting on the 2400 profile
 * during dead air. The coordinator's own target type is the only stable answer.
 *
 * The tuning layer treats a non-zero answer as authoritative for the RTL chain regardless of
 * `rf_mod`, so a target whose modulation column is empty under a global `-m` lock still gets its
 * own symbol rate and channel filter.
 *
 * @param state Decoder state owning the scan coordinator.
 * @return 2400 or 4800 for a parked GFSK-family target; 0 when trunk scan is not installed or the
 *         parked target is P25.
 */
int dsd_engine_trunk_scan_active_gfsk_symbol_rate(const dsd_state* state);
/**
 * @brief Non-zero while the parked target is p25-trunk or p25-conventional.
 *
 * Identifies the demodulation class, not ownership of a P25 trunk state machine.
 * Returns 0 when no target is parked for the supplied decoder state.
 */
int dsd_engine_trunk_scan_active_is_p25_class(const dsd_state* state);
/**
 * @brief Append the parked targets' learned IDEN entries as band-plan rows.
 *
 * Walks every target except the active one (whose tables are live in @p state and are the
 * caller's to collect; a paused scan's active target is walked too, its tables saved at the
 * pause) and appends each parked snapshot's ready FDMA/TDMA entries through
 * dsd_p25_bandplan_append_tables(), which reconciles by identifier, table and WACN/SYS.
 * Serves the band-plan export; a session without a coordinator returns @p count unchanged.
 *
 * @param state Decoder state owning the scan coordinator.
 * @param rows  Row array of at least @p cap entries.
 * @param count Rows already in @p rows.
 * @param cap   Capacity of @p rows.
 * @param skipped Accumulates entries with no room; may be NULL.
 * @return The new row count.
 */
int dsd_engine_trunk_scan_append_p25_idens(const dsd_state* state, struct p25_bandplan_row* rows, int count, int cap,
                                           int* skipped);

/**
 * @brief Edit one field of a target's own settings for the rest of the session ("this channel", issue #518).
 *
 * @p session and @p target_id name the target as the frontends saw it (dsd_state::scan_row_session and
 * trunk_scan_active_id): an edit opened on one coordinator never lands on the next, and one that outlives the visit
 * still reaches the target it named. @p field is one DSD_SCAN_ROW_FIELD_*, which the target's type must take
 * (dsd_scan_row_edit_fields(): the gain only on an RTL-family input other than an Airspy); @p action a
 * dsd_scan_row_edit_action, with @p value for SET. A width the published DSP rate cannot filter is refused, as it would
 * skip the target at every visit.
 *
 * A parked target runs the edit at once (a paused scan's stores it for the target's next visit, issue #671): its scope
 * takes the merged options again (the squelch reaches the demod), and @p out says whether the caller must hand the
 * front end the new width (publish_width) or reopen the stream for the new gain (restart_gain), and holds the edit
 * before this one for dsd_engine_trunk_scan_restore_target_edit() should that fail. Any other target runs it from its
 * next visit. Nothing is written to the list or saved.
 *
 * Takes the P25 SM tick guard (try-enter) around the change, so the caller must not hold it; decoder thread only. A
 * width or gain edit of the parked target waits for its retune to land: only the request the caller makes after this
 * call can be put back should the front end refuse it.
 *
 * @return a dsd_scan_row_edit_status: APPLIED, STORED, REFUSED (with @p out->err), STALE, UNAVAILABLE (no trunk scan)
 * or BUSY (the guard was held, or the parked target is still retuning; nothing changed).
 */
int dsd_engine_trunk_scan_edit_target(dsd_opts* opts, dsd_state* state, uint32_t session, const char* target_id,
                                      uint32_t field, int action, const dsd_scan_row_edit_value* value,
                                      dsd_scan_row_edit_result* out);

/**
 * @brief Put the @p fields (DSD_SCAN_ROW_FIELD_*) of a target's session edit back to those of @p edit (the previous
 * one dsd_engine_trunk_scan_edit_target() returned), keeping its other fields as they are now, and apply it as that
 * call does. For a caller whose follow-up -- the width request, the stream restart -- failed: an edit of another field
 * made since stays. Same session and thread rules, no checks of the values beyond those, but the P25 SM tick guard is
 * waited for (entered, not tried): the rollback must not be left undone because a watchdog tick held it.
 */
int dsd_engine_trunk_scan_restore_target_edit(dsd_opts* opts, dsd_state* state, uint32_t session, const char* target_id,
                                              uint32_t fields, const dsd_scan_row_edit* edit,
                                              dsd_scan_row_edit_result* out);

/**
 * @brief dsd_engine_trunk_scan_restore_target_edit() for a caller that holds the P25 SM tick guard already: the
 * rollback of a gain inside the one hold that covers a failed stream reopen and the recovery start after it.
 */
int dsd_engine_trunk_scan_restore_target_edit_locked(dsd_opts* opts, dsd_state* state, uint32_t session,
                                                     const char* target_id, uint32_t fields,
                                                     const dsd_scan_row_edit* edit, dsd_scan_row_edit_result* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_SCAN_H_ */
