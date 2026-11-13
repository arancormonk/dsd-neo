// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The scan quiet check (issue #834): one RF measurement of a landed conventional scan channel per visit.
 *
 * Owner-agnostic (a trunk-scan conventional target or a -Y row), decoder thread only, at most one check at a time. Its
 * state lives in the scan-owner extension (scan_owner.h), never in dsd_state fields. A check runs
 * SUSPENDED -> WAIT_CAPTURE -> REQUESTED -> READY (quiet) | DONE:
 *
 * - SUSPENDED: the visit is not eligible, or what it was published with changed; nothing runs until engine service
 *   publishes it again (scan_quiet_publish_eligibility()).
 * - WAIT_CAPTURE: waits for the tap's capture of the landed channel: a capture of the stream's live output generation,
 *   newer than the one read at the tune request, centred where the visit landed. Looked at no more often than every
 *   50 ms, for no longer than 1000 ms (realtime) after the landing was noted (a trunk-scan park still in flight: after
 *   the tuning ledger published its completion, which the hunt reads lock-free).
 * - REQUESTED: the tap measures; 400 ms (realtime) to answer. A lost, capture-changed or dropped request waits for the
 *   capture again, at most 3 times a visit; clipping, a non-finite sample, a failed transform or the deadline end it.
 * - READY: a quiet result the scan may skip on, for 250 ms.
 * - DONE: terminal for the visit (busy, failed, timed out, unsupported, activity, declined, applied): no new request
 *   until the visit changes.
 *
 * A visit is (owner, landing epoch, tune request). A sync (dsd_engine_scan_quiet_note_sync()) since the visit's first
 * request (kept across its retries) ends the check; a changed visit, or any counter its revision snapshot holds,
 * suspends it. The frame-sync hunt runs the check through dsd_frame_sync_hook_scan_quiet_service()
 * (dsd_engine_scan_quiet_service()), and engine service through scan_quiet_publish_eligibility(). RF power is never
 * activity: nothing here touches holds, activity stamps, history or events. Radio-off builds compile it as a no-op
 * service.
 */
#ifndef DSD_NEO_ENGINE_SCAN_QUIET_H
#define DSD_NEO_ENGINE_SCAN_QUIET_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <stddef.h>
#include <stdint.h>

/** Which scan a check measures for. */
enum { SCAN_QUIET_OWNER_NONE = 0, SCAN_QUIET_OWNER_TRUNK = 1, SCAN_QUIET_OWNER_Y = 2 };

/** Realtime bounds of a check, in ms. */
enum {
    SCAN_QUIET_CAPTURE_RECHECK_MS = 50,
    SCAN_QUIET_CAPTURE_WAIT_MS = 1000,
    SCAN_QUIET_REQUEST_DEADLINE_MS = 400,
    SCAN_QUIET_READY_MAX_AGE_MS = 250,
    SCAN_QUIET_MAX_RETRIES = 3,
};

typedef enum {
    SCAN_QUIET_SUSPENDED = 0,
    SCAN_QUIET_WAIT_CAPTURE,
    SCAN_QUIET_REQUESTED,
    SCAN_QUIET_READY,
    SCAN_QUIET_DONE,
} scan_quiet_phase;

/** The visit a check belongs to. */
typedef struct {
    int owner;        /**< SCAN_QUIET_OWNER_* */
    int index;        /**< the -Y row or trunk-scan target */
    uint32_t session; /**< -Y: dsd_state::lcn_scan_session; trunk scan: the coordinator's session */
    /** The landing's epoch (dsd_engine_scan_landing_epoch()); 0 for a trunk-scan park still in flight, adopted when
        the coordinator notes that park's landing (the same tune_request, the next epoch). */
    uint64_t epoch;
    uint64_t tune_request; /**< the landing's (or the park's) trunk-tuning request; 0 for a synchronous -Y tune */
} scan_quiet_visit_key;

/** The monotonic counters a check snapshots and compares at every reconcile and at the decision (spec §3.5). */
typedef struct {
    uint64_t chan_map_seq;
    uint64_t lcn_list_rev;
    uint64_t lcn_avoid_rev;
    uint64_t trunk_scan_avoid_rev;
    uint64_t quiet_inv_seq;
    uint32_t lcn_scan_session;
    uint32_t trunk_scan_lists_seq;
    uint32_t trunk_edit_seq;
    uint32_t y_edits_epoch;
    uint32_t pcm_input_generation;
    uint32_t receive_request_seq;
    int nfm_width_hz;
    int am_width_hz;
} scan_quiet_revisions;

/** What engine service publishes for the visit on air (spec §3.2). */
typedef struct {
    scan_quiet_visit_key key;
    /** The owner's own conditions hold (landed or a park in flight, no hold or stay, an eligible alternate). The
        session's (the setting, a radio build, an RTL input that is no replay) and a class to judge are checked here. */
    int eligible;
    uint8_t classes;          /**< DSD_SIGNAL_QUIET_CLASS_* bits; 0: a mode in no class, so no check */
    int analog_kind;          /**< dsd_analog_demod, with the analog class */
    uint32_t analog_width_hz; /**< the analog class's configured width; 0 without it */
    uint32_t dsp_rate_hz;     /**< the DSP rate in force */
    uint32_t frequency_hz;    /**< where the visit lands */
    scan_quiet_revisions revisions;
} scan_quiet_eligibility;

/** Read the counters a check snapshots. */
void scan_quiet_revisions_read(const dsd_opts* opts, const dsd_state* state, scan_quiet_revisions* out);

/** The width classes of a typed row's or a conventional target's @p mode; 0 for a mode in no class. */
uint8_t scan_quiet_mode_classes(dsd_scan_mode mode);
/** The width the stream runs an analog channel of @p kind at for the configured @p explicit_width_hz
 * (rtl_demod_analog_requested_width_hz()): an explicit width as typed, the AM default when unset, and 0 (no check) for
 * an unset NFM width, which keeps the historical filter rule and WIDE fallback and so has no definite channel. Typed
 * analog rows and targets resolve their width with it, as the decoder set does. Radio-off: @p explicit_width_hz. */
uint32_t scan_quiet_analog_width_hz(int kind, int explicit_width_hz);

/** The width classes of the decoder set in force (an untyped row, or a typed row inheriting it): every class an enabled
 * mode has, or the analog class for an analog session; 0 when any enabled mode has none (X2-TDMA, ProVoice, TETRA).
 * With the analog monitor on beside a digital set (-8) the set is mixed (D4): the analog class is judged too. The
 * analog class comes with the configured kind (dsd_opts::analog_demod) and the width the stream runs it at
 * (scan_quiet_analog_width_hz() of dsd_opts_analog_width_hz()) in @p analog_kind and @p analog_width_hz (either may be
 * NULL; 0 without it); no definite width (an unset NFM width) is no check. Outside the calibrated analog domain the
 * visit ends DONE at its capture. */
uint8_t scan_quiet_decoder_classes(const dsd_opts* opts, int* analog_kind, uint32_t* analog_width_hz);

/** Whether the session can make a check at all: the setting on, a radio build, an RTL-family input that is no I/Q
 * replay (neither the stream nor the decode clock). */
int scan_quiet_session_supported(const dsd_opts* opts);

/** Whether engine service has eligibility to work out for @p owner: 0 with the setting off or the visit on air's check
 * DONE, so the caller returns before any of its walk. */
int scan_quiet_wants_eligibility(const dsd_opts* opts, const dsd_state* state, int owner);

/** Publish the visit on air @p e for @p owner_kind (@p e->key.owner), then run one service step (start, collect,
 * expire). An ineligible visit is SUSPENDED; a new visit, or one republished after a suspension, starts again; a DONE
 * visit stays DONE. No-op with the setting off or in a radio-off build. */
void scan_quiet_publish_eligibility(const dsd_opts* opts, dsd_state* state, int owner_kind,
                                    const scan_quiet_eligibility* e);

/** The skip decision's quiet half (spec §3.4 (1)-(4)): 1 when @p owner_kind's READY result is still the visit's, its
 * revisions unchanged and @p current (the classes, analog kind and width, and DSP rate in force now, as engine service
 * works them out for its eligibility) equal to what was measured, its capture and output generation unchanged with no
 * tune pending, at most 250 ms old, with no sync since the request, no analog carrier heard and (trunk scan) no allowed
 * activity since; the result is consumed (DONE: one skip per visit). Any other READY result is declined (DONE), a
 * NULL @p current among them, and 0 returned; 0 too with no READY result, or one whose trunk-scan park has not been
 * noted as landed yet. */
int scan_quiet_apply(const dsd_opts* opts, dsd_state* state, int owner_kind, const scan_quiet_eligibility* current);

/** The engine saw @p owner_kind's READY result and will not apply it: declined (DONE), unless @p guard_lost (the tick
 * lost the coordinator's guard), which leaves it READY so the next hunt point hands back again. Reconciles either
 * way. */
void scan_quiet_reconcile_declined(const dsd_opts* opts, dsd_state* state, int owner_kind, int guard_lost);

/** Format the skip's NOTICE line (spec §3.4) into @p out: "NOTICE: scan: skipped quiet <name | row N> <freq> MHz:
 * <power> dBFS, floor <floor> dBFS (<ratio> dB)\n". @p name NULL or empty names -Y row @p row (0-based). Returns the
 * snprintf result. */
int scan_quiet_format_skip(char* out, size_t out_size, const char* name, int row, uint32_t hz, float power_dbfs,
                           float floor_dbfs, float ratio_db);
/** Log the NOTICE line for the result scan_quiet_apply() just consumed. */
void scan_quiet_log_skip(const dsd_state* state, const char* name, int row);

/** The decode-mono time the check's request was made (trunk scan: allowed activity at or after it declines); 0 with
 * none. */
double scan_quiet_requested_decode_m(const dsd_state* state);
/** The check's phase (SUSPENDED with none). */
scan_quiet_phase scan_quiet_phase_get(const dsd_state* state);
/** The check's visit key (all 0 with none). */
scan_quiet_visit_key scan_quiet_key_get(const dsd_state* state);

/** The frame-sync hunt's service (dsd_frame_sync_hook_scan_quiet_service()): reconcile, then start, collect or expire
 * the check; 1 only for a READY result that passes scan_quiet_apply()'s checks, without consuming it. 0 at once for a
 * SUSPENDED or DONE check, or none. Decoder thread. */
int dsd_engine_scan_quiet_service(const dsd_opts* opts, dsd_state* state);
/** Something a check depends on changed (dsd_engine_scan_quiet_note_change(), now): a check under way is cancelled
 * (its request ticket-matched) and SUSPENDED until republished. A DONE check stays DONE for its visit. */
void dsd_engine_scan_quiet_invalidate(dsd_state* state);
/** The setting changed (dsd_scan_quiet_setting_changed()): as dsd_engine_scan_quiet_invalidate(). */
void dsd_engine_scan_quiet_setting_changed(dsd_state* state);

/* Facts the scans own, read by the check. */
/** The trunk-scan coordinator's session-edit counter (0 with none). trunk_scan.c. */
uint32_t dsd_engine_trunk_scan_quiet_edit_seq(const dsd_state* state);
/** Whether the trunk-scan target on air saw allowed activity at or after @p since_m (decode mono). trunk_scan.c. */
int dsd_engine_trunk_scan_quiet_activity_since(const dsd_state* state, double since_m);
/** The -Y scan's row-edit counter (0 with none). channel_scan.c. */
uint32_t dsd_engine_channel_scan_quiet_edits_epoch(const dsd_state* state);

/* Engine service. */
/** The outer loop's pass owes the trunk scan's quiet check its engine service: the next coordinator tick under the
 * guard publishes the visit on air (spec 3.2), so the per-frame ticks of a carrier never walk for it. Called by the live
 * loop before its pass's tick, with skip-silent on. trunk_scan.c. */
void dsd_engine_trunk_scan_quiet_service_pass(const dsd_state* state);

#endif
