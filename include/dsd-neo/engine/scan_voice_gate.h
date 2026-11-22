// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Voice-gated scan for -Y and --trunk-scan (issue #381).
 *
 * The -Y scanner used to treat any sync as activity, so a repeater streaming
 * DMR IDLE/CSBK parked the scan indefinitely. This gate steps on unless decoded
 * voice frames hold it: qualify = window after sync in which voice must appear
 * or the scan moves on; hold = time to stay after the last voice frame.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_SCAN_VOICE_GATE_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_SCAN_VOICE_GATE_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Policy-allowed checked voice media visible to a scanner tick. */
typedef struct {
    /** Newest checked media time on an active call with media latched, or < 0 when none. */
    double active_media_m;
    /** Newest checked media time on an active or ended call, or < 0 when none. */
    double retained_media_m;
} dsd_scan_voice_probe_result;

/**
 * Probe both call slots for policy-allowed checked voice media (issue #890).
 *
 * Only a call's checked media counts: the frames its decoder output as speech, successfully and unconcealed, clear or
 * decrypted (dsd_call_state_note_checked_media()), at least 100 ms of it, never a frame's media mark before
 * it is decoded; and only media read on the scan visit and carrier in force now (dsd_state_scan_observation_is_current()):
 * a call another visit or a tune still in flight heard, ended or not, counts nothing, even with the same decode times.
 * Within the visit the timestamps are raw call-state anchors and may predate the hold window: callers apply their own
 * freshness bound, and a trunk-scan caller its own check that the visit is its target's landing. The encrypted-call
 * tuning rule applies before any talkgroup is needed: under it an encrypted call counts nothing. Both result fields
 * are initialized to -1. Returns -1 for invalid arguments, 0 when no qualifying media exists, and 1 when retained media
 * exists.
 */
int dsd_scan_receive_should_leave(const dsd_opts* opts, const dsd_state* state, double arrived_m, double now_m);
int dsd_scan_voice_probe(const dsd_opts* opts, const dsd_state* state, dsd_scan_voice_probe_result* out);

/**
 * The scan's policy table (issue #890): whether activity of @p protocol (a DSD_SYNC_* value) with @p identity may hold
 * a scan row. Both hold paths ask it, a protocol's activity report and checked voice media; the receive policy (DCC,
 * RAN, NAC, slot, tone) is the caller's to apply first.
 *
 * - A restriction that depends on a field @p identity does not know holds nothing (fail closed): data calls not
 *   followed (--no-data-calls, the default) with the data field unknown, encrypted calls not followed (--enc-lockout)
 *   with the encryption unknown, group or private calls not followed with the party unknown.
 * - Callsign families (YSF, D-STAR, M17) take no talkgroup policy: their callsigns are no talkgroup or radio IDs, so
 *   only the known data, party and encryption restrictions apply.
 * - Every other family is judged by its numeric IDs: a known target through the talkgroup policy (a private call by
 *   both IDs, a group call or one of unknown party by its target and source); with no target known, nothing holds
 *   under a talkgroup Hold or the allow list, nor a source the radio list blocks, and the known restrictions apply.
 *
 * 0 for a NULL @p identity.
 */
int dsd_scan_activity_policy_allows(const dsd_opts* opts, const dsd_state* state, int protocol,
                                    const dsd_trunk_scan_activity_identity* identity);
/** Whether @p protocol (a DSD_SYNC_* value) names its calls by callsign, outside talkgroup policy (YSF, D-STAR, M17). */
int dsd_scan_activity_protocol_is_callsign(int protocol);
/** Whether a call of @p protocol (a DSD_SYNC_* value) belongs to scan class @p mode (a dsd_scan_mode): every protocol
 * for the inherited class (an untyped row), the class's own family otherwise (P25 Phase 1 and 2 for p25, either NXDN
 * rate for nxdn48 and nxdn96), and none for an analog class. TETRA calls are tetra-dmo's; whether they were direct mode
 * is the caller's to ask of the call (dsd_call_snapshot::checked_media_tetra_direct). */
int dsd_scan_mode_family_has_protocol(int mode, int protocol);

/**
 * Non-zero while the analog channel on air has carrier (issue #526), the activity that holds an analog
 * row in both scanners.
 *
 * It reads the received-tone tap's carrier (dsd_state::analog_rx, issue #522) rather than deriving one:
 * the squelch open over the monitor audio, above the tap's level floor, held through its 200 ms hangover,
 * at every input rate, and dropped at every reset the tap makes -- a retune, a stream generation or
 * receive-profile change, a stream pause -- and while a retune is unresolved, so a block that straddles a
 * retune never counts for the new channel (dsd_analog_rx_carrier_open_now()). So it holds only
 * while the analog monitor runs, FM or AM (dsd_analog_monitor_tap_active(), issue #524), never on a
 * publication past its stale_after_ms (dsd_analog_rx_stale_now(), on the clock the tap set it on), never while a
 * digital carrier is flagged (dsd_state::carrier) and never while a trunking state machine owns the channel. Whether
 * audio is played has no part in it: -o null or a muted UI still holds the row. Under the CTCSS/DCS receive policy
 * (issue #527) it is only the traffic the policy passes (dsd_analog_tone_gate_passes(): no policy in force, or
 * ALLOWED), the one rule the monitor's -Y hangtime stamp
 * follows too: traffic it rejected holds nothing, and traffic it is still checking is no activity either -- it holds
 * the row while it lasts through its verdict (dsd_scan_analog_tone_gate()), but restarts no hold and leaves no tail.
 * Read-only and null-safe.
 */
int dsd_scan_analog_carrier_open(const dsd_opts* opts, const dsd_state* state);

/**
 * The CTCSS/DCS receive policy's verdict (dsd_analog_tone_gate, issue #527) on the analog carrier on air: OFF when
 * there is no such carrier by the rules of dsd_scan_analog_carrier_open() before the verdict is weighed (or no policy
 * is in force), else the verdict the tap published for it (dsd_analog_tone_gate_in_force()). PENDING holds a row while
 * its carrier lasts, with the stay reason DSD_SCAN_STAY_TONE_PENDING and no window: the -Y step holds it at every
 * pass, whatever -t says, and the trunk-scan tick holds it with the idle dwell running as on a quiet channel (armed at
 * the check's first tick past an activity or operator hold that had disarmed it). It is no activity: the monitor
 * stamps no hangtime anchor for it and the tick restarts no activity hold, so a check its carrier ends before a verdict
 * leaves the -t, hold and dwell clocks where they were, and short bursts the policy never passed cannot park a
 * scanner. The policy's window bounds each check. ALLOWED is activity like any carrier, and the -Y step holds it at
 * every pass too: the monitor stamps the anchor only as a block ends, and a verdict that turns ALLOWED part-way
 * through one would otherwise meet an anchor from before the check. REJECTED holds nothing and, on the scanner's next
 * pass, advances it unless the operator holds the row or there is nowhere else to go. Read-only and null-safe.
 */
int dsd_scan_analog_tone_gate(const dsd_opts* opts, const dsd_state* state);

/**
 * Non-zero when traffic the CTCSS/DCS receive policy rejected (issue #527) has ended on the channel on air with no
 * carrier heard since, by the rules of dsd_scan_analog_carrier_open() before the verdict is weighed: its carrier's
 * hangover ran out, or its input paused (dsd_analog_rx_rejection_ended_now()), or an input that stopped delivering left
 * a rejection published past its deadline. A scanner whose pass or tick comes after such traffic ended releases the
 * row as it would have while the carrier lasted, when there is somewhere else to go, rather than waiting out a window
 * still running from before it: -t since the row landed or since traffic the policy passed (earlier traffic, or this
 * traffic before a blocked or unlisted value was confirmed), or a trunk-scan activity hold. Read-only and null-safe.
 */
int dsd_scan_analog_tone_rejection_ended(const dsd_opts* opts, const dsd_state* state);

/**
 * Non-zero when the operator's talkgroup hold is on a call that is being followed right now:
 * some slot carries an active, non-data call whose target -- after policy remapping -- is the
 * held talkgroup, or, on a private call, whose source is. A P25 group call on a live patch's
 * supergroup is judged by the patch resolver's admission under @p opts: the held talkgroup must be
 * the supergroup or a member the patch carries now, with no final block on the supergroup.
 *
 * Both scanners suspend the per-visit limit while this holds (issue #507), so the limit cannot
 * cut short the very call the hold exists to follow, and a fresh limit starts once that call
 * ends. Read-only and null-safe.
 */
int dsd_scan_tg_hold_call_active(const dsd_opts* opts, const dsd_state* state);

/**
 * Restart the per-visit gate memory on a scan hop, and open the visit the per-visit cap
 * measures (issue #507): this is the one place a successful park stamps that anchor.
 */
void dsd_scan_voice_gate_note_retune(dsd_state* state, double now_m);

/** Per-frame tick while parked on a -Y row; a no-op unless scanner_mode is on. The gate never owns an analog
 * row (the analog FM monitor): there is no decoded voice to wait for, and the row's carrier holds it under the
 * hangtime rule instead, so a global --scan-voice-only does not block analog rows. */
void dsd_scan_voice_gate_tick(const dsd_opts* opts, dsd_state* state, int synced, double now_m);

/** Non-zero when the gate has a visit anchor and owns the scanner's step timing; never without a list of the user's
 * own (dsd_state_trunk_lcn_user_list_present()), as channels learned over the air are no list to step through. */
int dsd_scan_voice_gate_owns_step(const dsd_opts* opts, const dsd_state* state);

/** Non-zero while the gate owns the step (dsd_scan_voice_gate_owns_step()) on a visit that has heard no voice yet: it
 * is still qualifying the row, so a scan quiet check's result may step it before its qualification expires (issue
 * #834). A visit that heard voice (VOICE or TAIL) is the gate's alone. */
int dsd_scan_voice_gate_qualifying(const dsd_opts* opts, const dsd_state* state);

/** Non-zero when the gate says the -Y visit is over and the scan should step. */
int dsd_scan_voice_gate_should_step(const dsd_opts* opts, const dsd_state* state, double now_m);

/** Take the scan timing publication back down (scan off, shutdown, between targets). */
void dsd_scan_timing_clear(dsd_state* state);

/** Publish one scan timing report for the row on air (issue #508). */
void dsd_scan_timing_publish(dsd_state* state, const dsd_scan_timing_publication* report);

/**
 * Publish the -Y scanner's stay reason and step deadline. A no-op unless
 * scanner_mode == 1 && trunk_scan_enabled != 1: under --trunk-scan the coordinator owns
 * the publication, exactly as it owns scan_voice_gate_phase.
 *
 * The legacy hangtime rule anchors on the wall-clock last_cc_sync_time (which NXDN can
 * stamp two seconds into the future), so the publisher takes both clocks, sampled
 * together, to express that anchor as a monotonic deadline the renderer can difference.
 */
void dsd_engine_scan_y_timing_tick(const dsd_opts* opts, dsd_state* state, double now_m, double now_wall_s);

/**
 * Maintain the -Y per-visit anchor (issue #507). A no-op unless
 * scanner_mode == 1 && trunk_scan_enabled != 1: under --trunk-scan the coordinator keeps its
 * own per-target anchor.
 *
 * Separate from dsd_scan_voice_gate_tick() because the cap applies with the voice gate off as
 * well as on, and that tick returns early there. Parking and operator hold release also reset
 * the anchor; sync and voice activity never touch it. While the limit cannot count -- suspended
 * by a hold, or with no second row to advance to (dsd_engine_channel_scan_can_rotate()) -- it
 * remembers the suspension. The first eligible tick starts a fresh interval even if input
 * stalled since the last suspended tick.
 */
void dsd_engine_scan_visit_tick(const dsd_opts* opts, dsd_state* state, double now_m);

/**
 * The monotonic instant the visit on air runs out, or < 0 when no cap is counting: the cap is
 * disabled, no visit is anchored, a hold suspends it, a row transaction is outstanding, the row on
 * air has changed since the last tick, or there is no second eligible row to advance to
 * (dsd_engine_channel_scan_can_rotate()). Pure; the predicate and the publication both read it
 * so the countdown on screen is the one that fires.
 */
double dsd_engine_scan_visit_deadline_m(const dsd_opts* opts, const dsd_state* state);

/** Non-zero when the per-visit cap says the -Y visit is over and the scan must advance. */
int dsd_engine_scan_visit_expired(const dsd_opts* opts, const dsd_state* state, double now_m);

/** Whether the -Y row on air holds only on activity the scan's policy table admits (issue #890; docs/trunk-scan.md, "A
 * typed row follows the same rules as the target of its type"): a typed row of a simplex digital class (ysf, dstar,
 * dpmr, m17, tetra-dmo), whose frame-boundary content time moves only with an admitted report
 * (dsd_engine_scan_y_typed_activity()), not with every checked frame. An untyped row, and a row of any other
 * class, holds on what its protocols checked, as before. One scope read. 0 for a NULL state. */
int dsd_engine_scan_y_row_holds_on_admitted(const dsd_state* state);
/** A simplex digital family's checked activity report (the -Y row hook, dsd_trunk_scan_row_activity_hook_set(); issue
 * #890): noted as the -Y row's admitted activity when the -Y scan runs (no trunk scan), the row on air is a typed row of
 * the report's family (dsd_engine_scan_y_row_holds_on_admitted()), the report is an observation of that row's
 * landed visit and current carrier (dsd_state_scan_observation_origin()), the receive policy lets @p protocol and
 * @p slot through, and the scan's policy table admits @p identity (dsd_scan_activity_policy_allows()). Decoder thread;
 * a few loads otherwise. */
void dsd_engine_scan_y_typed_activity(const dsd_opts* opts, const dsd_state* state, int protocol, int slot,
                                      const dsd_trunk_scan_activity_identity* identity);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_SCAN_VOICE_GATE_H_H */
