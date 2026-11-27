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
 *   publishes it again (scan_quiet_publish_eligibility()). A visit that cannot be checked is SUSPENDED too, with its
 *   reason (dsd_scan_quiet_reason, issue #834 R6): its session's (an input with no check), its facts' (decoders with no
 *   known width, no NFM width), or its capture's (a centred capture, an uncalibrated rate or analog width). A capture's
 *   refusal is cached: published again with the same facts and revisions while the stream keeps the output generation
 *   it refused, the visit starts nothing and reads no capture; an edit, a rate or receive change starts it again.
 * - WAIT_CAPTURE: waits for the tap's capture of the landed channel: a capture of the stream's live output generation,
 *   newer than the one read at the tune request, centred where the visit landed. The hunt's legacy points look at it
 *   no more often than every 50 ms; the hunt's event service (dsd_engine_scan_quiet_event_service()) looks once
 *   whenever the tap's capture word moves. For no longer than 1000 ms (realtime) after the landing was noted (a trunk-scan park still in flight: after
 *   the tuning ledger published its completion, which the hunt reads lock-free), or after the check started again on a
 *   visit it already published. A frame boundary of the synced-frames loop services the check too
 *   (dsd_engine_scan_quiet_frame_service()): it reads the capture without moving that 50 ms schedule and requests only
 *   SCAN_QUIET_FRAME_SETTLE_MS after the first qualifying capture the service saw (a retry: after the retryable
 *   failure), and a capture seen in time is no capture failure: its deadline is max(origin + 1000 ms, settle start +
 *   125 ms), and a capture still there past it is requested.
 * - REQUESTED: the tap measures, from SCAN_QUIET_LEAD_MS after the capture; 400 ms (realtime) to answer. A lost, capture-changed or dropped request waits for the
 *   capture again, at most 3 times a visit; clipping, a non-finite sample, a failed transform or the deadline end it.
 * - READY: a quiet result the scan may skip on, for 250 ms.
 * - DONE: terminal for the visit (busy, failed, timed out, refused by the tap, activity, declined, applied): no new
 *   request until the visit changes.
 *
 * A visit is (owner, landing epoch, tune request). Activity anywhere in the visit ends its check, suspended or not, so
 * no check is made for it again: content that passed its protocol's CRC/FEC gate (a PROVEN content note,
 * dsd_engine_scan_quiet_note_evidence()) or the analog carrier (dsd_engine_scan_quiet_note_activity()). A raw sync is
 * no activity (issue #834 R1): a sync the frame sync returns waits for its frame's verdict
 * (dsd_engine_scan_quiet_note_frame_sync(), dsd_engine_scan_quiet_frame_end()), and a frame that left a PARTIAL note
 * waits up to its protocol's hold for the frame that completes it; while either is pending a READY result is neither
 * handed back, applied nor declined, and still ages out. A visit counts from its tune's issue
 * (dsd_engine_scan_landing_note_tune()), which its landing's note carries and a park still in flight reads at its first
 * publication (and keeps when its landing adopts it), so content heard before the landing is noted counts. A changed
 * visit, or any counter its revision snapshot holds, suspends it. The frame-sync hunt runs the check through
 * dsd_frame_sync_hook_scan_quiet_service() (dsd_engine_scan_quiet_service()), and engine service through
 * scan_quiet_publish_eligibility(). RF power is never activity: nothing here touches holds, activity stamps, history or
 * events. Radio-off builds compile it as a no-op service.
 */
#ifndef DSD_NEO_ENGINE_SCAN_QUIET_H
#define DSD_NEO_ENGINE_SCAN_QUIET_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
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
    /** A request a frame-boundary service starts waits this long (issue #834 R1): from the first qualifying capture the
        service saw, or from the retryable failure it collected. */
    SCAN_QUIET_FRAME_SETTLE_MS = 125,
    /** Every request's reading starts this many ms of samples after its capture's first block (issue #834 follow-up,
        D1-a): the request's lead, which the tap drops unmeasured. */
    SCAN_QUIET_LEAD_MS = 10,
    /** A reading set shorter than the calibrated one (--scan-skip-silent-ms) still judges this many sub-windows on a
        visit that can carry a single DMR radio: the fewest of at most 13.75 ms that always hold one wholly inside a
        burst of an ongoing normal-burst train (ETSI TS 102 361-1 clauses 4.2.2 and 10.2.3; IO_SIGNAL_MEASURE). */
    SCAN_QUIET_DMR_MIN_WINDOWS = 6,
    /** The answer deadline grows by this much for each sub-window a reading has above the calibrated nine. */
    SCAN_QUIET_DEADLINE_PER_WINDOW_MS = 12,
};

/** The longest a PARTIAL content note holds a quiet result back, in ms (decode clock): above every protocol's hold. */
enum { SCAN_QUIET_PENDING_MAX_MS = 1000 };

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

/** The monotonic counters a check snapshots and compares at every reconcile and at the decision. */
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
    uint32_t receive_change_seq;
    int nfm_width_hz;
    int am_width_hz;
} scan_quiet_revisions;

/** What engine service publishes for the visit on air. */
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
    /** The visit can carry a single DMR radio's bursts (scan_quiet_mode_dmr()): a shortened reading keeps
        SCAN_QUIET_DMR_MIN_WINDOWS sub-windows on it. */
    uint8_t dmr;
    /** Why the facts give the visit no check (dsd_scan_quiet_reason: MODES, NO_WIDTH or UNSUPPORTED_MODE,
        scan_quiet_facts_reason()),
        NONE when they give one, and for a trunked target, which is never checked by design. */
    uint8_t reason;
    scan_quiet_revisions revisions;
} scan_quiet_eligibility;

/** Read the counters a check snapshots. */
void scan_quiet_revisions_read(const dsd_opts* opts, const dsd_state* state, scan_quiet_revisions* out);

/** Whether a visit in @p mode can carry a single DMR radio: a DMR row or target, or one that inherits a decoder set with
 * DMR on (an untyped -Y row, or a typed one in mode inherit). */
uint8_t scan_quiet_mode_dmr(const dsd_opts* opts, dsd_scan_mode mode);

/** The visit fact scan_quiet_eligibility::dmr for @p mode: scan_quiet_mode_dmr() while --scan-skip-silent-ms shortens
 * readings (the only time the DMR floor matters), else 0, so the calibrated reading's facts are unchanged. */
uint8_t scan_quiet_visit_dmr(const dsd_opts* opts, dsd_scan_mode mode);

/** The sub-window count --scan-skip-silent-ms asks for (its ms rounded up to whole 10 ms sub-windows), 0 when unset:
 * the calibrated reading. */
uint32_t scan_quiet_setting_windows(const dsd_opts* opts);

/** The sub-window count a request asks for: 0 (the calibrated reading) with the setting unset, else the setting's,
 * and at least SCAN_QUIET_DMR_MIN_WINDOWS on a visit that can carry DMR (@p dmr). */
uint32_t scan_quiet_request_windows(uint32_t setting_windows, int dmr);

/** The answer deadline of a request judging @p windows sub-windows (0: the calibrated nine), in ms after it. */
uint32_t scan_quiet_request_deadline_ms(uint32_t windows);

/** Format the WARN line of a reading shortened by --scan-skip-silent-ms (@p setting_ms) to @p windows sub-windows at
 * capture rate @p rate_hz, with the span those sub-windows take over every checked width there; with @p dmr_floor the
 * line for a DMR visit read for @p windows sub-windows, the DMR floor, instead. Radio builds only. Returns the snprintf
 * result. */
#ifdef USE_RADIO
int scan_quiet_format_short_warn(char* out, size_t out_size, int setting_ms, uint32_t windows, uint32_t rate_hz,
                                 int dmr_floor);
#endif

/** The width classes of a typed row's or a conventional target's @p mode; 0 for a mode in no class. */
uint8_t scan_quiet_mode_classes(dsd_scan_mode mode);
/** The width the stream runs an analog channel of @p kind at for the configured @p explicit_width_hz
 * (rtl_demod_analog_requested_width_hz()): an explicit width as typed, the AM default when unset, and 0 (no check) for
 * an unset NFM width, which keeps the historical filter rule and WIDE fallback and so has no definite channel. Typed
 * analog rows and targets resolve their width with it, as the decoder set does. Radio-off: @p explicit_width_hz. */
uint32_t scan_quiet_analog_width_hz(int kind, int explicit_width_hz);

/** An analog row's or target's kind @p kind (dsd_analog_demod) and the width the stream runs it at, into
 * @p e->analog_kind and @p e->analog_width_hz: its own width (@p values with DSD_SCAN_OPT_BANDWIDTH; may be NULL), else
 * the configured one of its kind (dsd_scan_mode_configured_analog_width()), resolved by scan_quiet_analog_width_hz().
 * The one rule the trunk scan's targets and the -Y scan's typed rows share, so what is published, measured and compared
 * at the decision cannot drift apart. */
void scan_quiet_analog_facts(const dsd_opts* opts, const dsd_state* state, int kind,
                             const dsd_scan_option_values* values, scan_quiet_eligibility* e);

/** The width classes of the decoder set in force (an untyped row, or a typed row inheriting it): every class an enabled
 * mode has, or the analog class for an analog session; 0 when any enabled mode has none (X2-TDMA, ProVoice, TETRA).
 * With the analog monitor on beside a digital set (-8) the set is mixed: the analog class is judged too. The
 * analog class comes with the configured kind (dsd_opts::analog_demod) and the width the stream runs it at
 * (scan_quiet_analog_width_hz() of dsd_opts_analog_width_hz()) in @p analog_kind and @p analog_width_hz (either may be
 * NULL; 0 without it); no definite width (an unset NFM width) is no check. Outside the calibrated analog domain the
 * visit ends DONE at its capture. */
uint8_t scan_quiet_decoder_classes(const dsd_opts* opts, int* analog_kind, uint32_t* analog_width_hz);

/** Whether the decoder set in force holds a mode with no checked content to hear (X2-TDMA, ProVoice, TETRA: no PROVEN
 * content-evidence site, issue #834): the set scan_quiet_decoder_classes() gives no check. The -Y frame-boundary step
 * reads the frames instead (dsd_state::scan_y_unchecked_m), not the set. */
int scan_quiet_decoder_set_unchecked(const dsd_opts* opts);

/** The @p digital classes a digital decoder runs in, with the analog monitor's when it plays beside them (-8): the
 * mixed set's rule of scan_quiet_decoder_classes(), for a typed digital row or target as for the decoder set. The
 * analog class comes with the configured kind and width in @p analog_kind and @p analog_width_hz (either may be NULL;
 * left as they are without it); a monitor at no definite width (an unset NFM width) makes the set 0, no check. */
uint8_t scan_quiet_monitor_classes(const dsd_opts* opts, uint8_t digital, int* analog_kind, uint32_t* analog_width_hz);

/** Why the facts @p e give a conventional row or target of class @p mode no check (dsd_scan_quiet_reason), with its
 * classes and analog width worked out: NONE when they give one; for no class, or the analog class at no definite width,
 * UNSUPPORTED_MODE for a class the check has no calibration for (tetra-dmo, issue #890), MODES when the classes are the
 * decoder set's (@p mode INHERIT: an untyped -Y row, or a typed one inheriting it) and the set holds a mode with no
 * known width (X2-TDMA, ProVoice, TETRA), the M17 encoder, or no mode of a class at all, else NO_WIDTH (an NFM channel,
 * or the -8 monitor, with no width set). */
uint8_t scan_quiet_facts_reason(const dsd_opts* opts, const scan_quiet_eligibility* e, dsd_scan_mode mode);

/** Whether the session can make a check at all: the setting on, a radio build, an RTL-family input that is no I/Q
 * replay (neither the stream nor the decode clock). */
int scan_quiet_session_supported(const dsd_opts* opts);

/** The scan visit on air, as a timing writer names it (issue #834 R6). */
typedef struct {
    int owner;         /**< SCAN_QUIET_OWNER_TRUNK or SCAN_QUIET_OWNER_Y */
    int index;         /**< the target or row on air; -1 with none */
    uint32_t session;  /**< the coordinator's session, or dsd_state::lcn_scan_session */
    uint64_t list_id;  /**< the list's identity: the coordinator's session, or dsd_state::lcn_list_rev */
    uint64_t list_rev; /**< its revision: the trunk scan's target edits and lists; 0 for -Y (list_id moves) */
    const char* name;  /**< a target's label or id; NULL for a -Y row, named "row N" */
    int shows_status;  /**< the stay on air is one the status is shown on (scan_quiet_stay_shows_status()) */
    int conventional;  /**< 0 for a trunked target: never checked, so nothing is said about it */
} scan_quiet_visit_ref;

/** Whether the Scan Timing row shows the check's status on a visit whose stay is @p stay_reason (dsd_scan_stay_reason):
 * the idle dwell (trunk scan, and a -Y visit still qualifying under the voice gate) and -Y's hangtime, the windows a
 * skip ends early. Every other stay explains the visit by itself. */
int scan_quiet_stay_shows_status(uint8_t stay_reason);

/** Fill @p report's quiet_status and quiet_reason for the visit on air @p ref (issue #834 R6), on the decoder thread,
 * from what the check knows of it: NONE with the setting off, for a trunked target, for a stay not shown, and for a
 * check of another visit or one that ended; NO_CHECK with its reason, the receiver's first (an input with no check,
 * in a radio-off build too; the visit's own capture centred or at an uncalibrated rate, or a centred capture the
 * last read showed in this stream session), then the channel's (decoders, width), and on -Y only while the row has
 * carried no call this visit; CHECKING while the check waits for its capture, settles, measures or holds a quiet
 * result. A
 * reason the visit has is logged as a WARN line once per key, whatever the stay: the receiver's once per (input,
 * device, rate, reason), the channel's once per (list, row or target, kind, width, decoder set and -8 monitor width,
 * reason); the keys are compared before anything is formatted, and turning the setting on again logs them anew. */
void scan_quiet_timing_fill(const dsd_opts* opts, dsd_state* state, const scan_quiet_visit_ref* ref,
                            dsd_scan_timing_publication* report);

/** Format the WARN line for @p reason into @p out (the one table of its wording). The receiver's reasons take @p detail
 * (SCAN_QUIET_WARN_AUDIO .. _STREAM), and RATE the detail (SCAN_QUIET_WARN_RATE .. _AIRSPY_RATE) and the capture rate @p rate_hz; the channel's take @p name (NULL or empty:
 * -Y row @p row, 0-based), the frequency @p hz, @p detail (scan_quiet_channel_detail()), and for WIDTH the analog
 * @p kind and @p width_hz. Returns the snprintf result. */
int scan_quiet_format_warn(char* out, size_t out_size, uint8_t reason, int detail, uint32_t rate_hz, const char* name,
                           int row, uint32_t hz, int kind, uint32_t width_hz);

/** The analog widths of @p kind (DSD_ANALOG_DEMOD_AM, else NFM) the WARN line names as checked, @p count of them: the
 * calibration's admitted widths, which a test holds to dsd_signal_quiet_analog_supported(). */
const uint32_t* scan_quiet_warn_widths(int kind, size_t* count);

/** The receiver detail scan_quiet_format_warn() takes: why the input, the tuner or the rate has no check. */
enum {
    SCAN_QUIET_WARN_AUDIO = 0,        /**< INPUT: audio input */
    SCAN_QUIET_WARN_REPLAY = 1,       /**< INPUT: an I/Q replay */
    SCAN_QUIET_WARN_NO_RADIO = 2,     /**< INPUT: a build without radio support */
    SCAN_QUIET_WARN_AIRSPY = 3,       /**< TUNER: a native Airspy capture that is not centred */
    SCAN_QUIET_WARN_CENTRED = 4,      /**< TUNER: offset tuning, or the Fs/4 shift off */
    SCAN_QUIET_WARN_STREAM = 5,       /**< INPUT: an I/Q stream (iqtcp:, iqudp:) */
    SCAN_QUIET_WARN_RATE = 6,         /**< RATE: a rate the calibration did not validate (any other detail too) */
    SCAN_QUIET_WARN_SOAPY_AIRSPY = 7, /**< RATE: an Airspy through SoapySDR, at any rate */
    SCAN_QUIET_WARN_SOAPY_UNKNOWN =
        8, /**< RATE: a SoapySDR device that may be an Airspy (a proxy, unknown keys), at any rate */
    SCAN_QUIET_WARN_AIRSPY_RATE = 9, /**< RATE: a native Airspy at a rate its centred calibration did not validate */
};

/** The channel detail scan_quiet_format_warn() takes (bits): what in the session gives MODES (the ProVoice, X2-TDMA
 * and TETRA decoders on, or the M17 encoder; none of them: no mode of a class), or a NO_WIDTH that is the -8 monitor's.
 */
enum {
    SCAN_QUIET_WARN_PROVOICE = 1,
    SCAN_QUIET_WARN_X2TDMA = 2,
    SCAN_QUIET_WARN_TETRA = 4,
    SCAN_QUIET_WARN_ENCODER = 8,
    SCAN_QUIET_WARN_MONITOR = 16,
};

/** The channel detail of @p reason for a visit judged in @p classes, from the session @p opts in force. */
uint8_t scan_quiet_channel_detail(const dsd_opts* opts, uint8_t reason, uint8_t classes);

/** Whether engine service has eligibility to work out for @p owner: 0 with the setting off or the visit on air's check
 * DONE, so the caller returns before any of its walk. */
int scan_quiet_wants_eligibility(const dsd_opts* opts, const dsd_state* state, int owner);

/** Publish the visit on air @p e for @p owner_kind (@p e->key.owner), then run one service step (start, collect,
 * expire). It reconciles the check first, adopting a park measured or suspended in flight once its landing is noted. A
 * DONE visit stays DONE. Checked content (a PROVEN note) or a carrier heard since the visit's activity baseline (its
 * tune's issue) ends the visit DONE at once, eligible or not, with nothing requested; a raw sync ends nothing.
 * Otherwise an ineligible visit is SUSPENDED (keeping what its landing adopts it by); a new visit, one republished
 * after a suspension, or one published with other facts than were measured starts again. No-op with the setting off
 * or in a radio-off build. */
void scan_quiet_publish_eligibility(const dsd_opts* opts, dsd_state* state, int owner_kind,
                                    const scan_quiet_eligibility* e);

#ifdef DSD_NEO_TEST_HOOKS
/** Tests only: the realtime clock the check reads (ms), NULL for dsd_realtime_mono_ms(). A test that runs everywhere
 * sets it instead of wrapping the platform clock at link time. */
extern uint64_t (*scan_quiet_test_realtime_ms)(void);
#endif

/** The skip decision's quiet half: 1 when @p owner_kind's READY result is still the visit's, its revisions unchanged
 * and @p current (the classes, analog kind and width, and DSP rate in force now, as engine service works them out for
 * its eligibility) equal to what was measured, its capture and output generation unchanged with no tune pending, at
 * most 250 ms old, with no checked content in the visit, no analog carrier heard and (trunk scan) no allowed activity
 * in the visit; the result is consumed (DONE: one skip per visit). Any other READY result is declined (DONE), a NULL
 * @p current among them, and 0 returned; 0 too with no READY result, one whose trunk-scan park has not been noted as
 * landed yet, or one held while a frame's verdict is pending, which stays READY. */
int scan_quiet_apply(const dsd_opts* opts, dsd_state* state, int owner_kind, const scan_quiet_eligibility* current);

/** The engine saw @p owner_kind's READY result and will not apply it: declined (DONE), unless @p guard_lost (the tick
 * lost the coordinator's guard) or a frame's verdict is pending, either of which leaves it READY so a later point hands
 * back again. Reconciles either way. */
void scan_quiet_reconcile_declined(const dsd_opts* opts, dsd_state* state, int owner_kind, int guard_lost);

/** Format the skip's NOTICE line into @p out: "NOTICE: scan: skipped quiet <name | row N> <freq> MHz:
 * <power> dBFS, floor <floor> dBFS (<ratio> dB)\n". @p name NULL or empty names -Y row @p row (0-based). Returns the
 * snprintf result. */
int scan_quiet_format_skip(char* out, size_t out_size, const char* name, int row, uint32_t hz, float power_dbfs,
                           float floor_dbfs, float ratio_db);
/** Log the NOTICE line for the result scan_quiet_apply() just consumed. */
void scan_quiet_log_skip(const dsd_state* state, const char* name, int row);

/** The decode-mono time of the visit's activity baseline, its tune's issue (trunk scan: allowed activity at or after it
 * declines); 0 with none. */
double scan_quiet_visit_decode_m(const dsd_state* state);
/** The check's phase (SUSPENDED with none). */
scan_quiet_phase scan_quiet_phase_get(const dsd_state* state);
/** The check's visit key (all 0 with none). */
scan_quiet_visit_key scan_quiet_key_get(const dsd_state* state);

/** The frame-sync hunt's service (dsd_frame_sync_hook_scan_quiet_service()): reconcile, then start, collect or expire
 * the check. DSD_SCAN_QUIET_HAND_BACK only for a READY result that passes scan_quiet_apply()'s checks with no frame's
 * verdict pending, without consuming it; DSD_SCAN_QUIET_LIVE for a check still under way (waiting for its capture,
 * requested, or a READY result held back or failing those checks); DSD_SCAN_QUIET_NONE at once for a SUSPENDED or DONE
 * check, or none, and for one the step ended. Decoder thread. */
int dsd_engine_scan_quiet_service(const dsd_opts* opts, dsd_state* state);

/* Content evidence (issue #834 R1; the content_evidence hook of dsd_frame_sync_hooks). Only while skip-silent is on
   (the runtime wrapper and the frame loop's guards), on the decoder thread; a state with no scan owner has no check and
   gets no owner made for it. */
/** A protocol's gate verdict on the frame being decoded (dsd_frame_sync_hook_content_evidence()): PROVEN moves the
 * activity sequence once a frame and drops any wait (it dominates PARTIAL, in the frame or before it); PARTIAL makes
 * the frame wait up to @p hold_ms (at most SCAN_QUIET_PENDING_MAX_MS) for its verdict, from the frame's end, or from
 * now when no frame is pending. SYNC_ONLY (issue #890) is taken as PARTIAL: a wait, never activity. */
void dsd_engine_scan_quiet_note_evidence(dsd_state* state, int level, uint32_t hold_ms);
/** The frame sync returned a sync: its frame's verdict is pending until dsd_engine_scan_quiet_frame_end(). Moves no
 * activity. */
void dsd_engine_scan_quiet_note_frame_sync(dsd_state* state);
/** The frame the last returned sync opened has been decoded (or withheld): a PARTIAL frame waits for its hold; a frame
 * that checked nothing leaves any wait already running, since a rejected sync never cancels a verdict the protocol
 * still holds. */
void dsd_engine_scan_quiet_frame_end(dsd_state* state);
/** Nothing is waited for any more: the frame loop left before a frame's end, or a boundary ended what it waited on. */
void dsd_engine_scan_quiet_pending_reset(dsd_state* state);
/** The service at a frame boundary of the synced-frames loop (issue #834 R1), for @p owner_kind's check: as the hunt's
 * (reconcile, then start, collect or expire), except that a request it starts waits SCAN_QUIET_FRAME_SETTLE_MS from
 * the first qualifying capture the service saw, or from the retryable failure, and that it never moves the hunt's
 * capture schedule. Syncs the protocol's gate rejects keep the hunts short, so they rarely reach the hunt's
 * 300-symbol point: this is what runs the check through them. DSD_SCAN_QUIET_NONE at once with the setting off (one
 * load), for another owner's check, or none. */
int dsd_engine_scan_quiet_frame_service(const dsd_opts* opts, dsd_state* state, int owner_kind);
/** The event service the frame-sync hunt runs before each symbol while a check is under way
 * (dsd_frame_sync_hook_scan_quiet_event(), issue #834 follow-up): it acts as soon as the tap's capture word moves
 * (WAIT_CAPTURE), the request's status word leaves pending (REQUESTED) or a READY result is free of a frame verdict,
 * and otherwise does nothing but those lock-free loads. It never ends a check for time (the hunt's 300-symbol point,
 * engine publications and frame boundaries keep every expiry), never moves the 50 ms legacy schedule, and requests a
 * capture no earlier than a frame boundary may once a settle is in force. A READY result is handed back by events at
 * most once. DSD_SCAN_QUIET_NONE with the setting off (one load) or no check under way. */
int dsd_engine_scan_quiet_event_service(const dsd_opts* opts, dsd_state* state);

/** The -Y frame-boundary yield of the synced-frames loop: 1 when the scan is -Y with skip-silent on and the -Y check's
 * frame-boundary service answers DSD_SCAN_QUIET_HAND_BACK, so the loop ends and noCarrier() decides as after the hunt's
 * hand-back. */
int dsd_engine_scan_quiet_frame_yield(const dsd_opts* opts, dsd_state* state);
/** Whether @p owner_kind's check owes a frame boundary its republication (issue #834 R5): it was suspended (a revision
 * changed, such as a receive request the stream numbered, or a note of change) while its visit's landing was still in
 * force, or it is SUSPENDED with a reason it cannot be checked and a revision it was published with has moved since
 * (a width or row edit; once per change, R6), so it starts again on the same visit. Under syncs that keep the
 * synced-frames loop running no outer pass or noCarrier() comes to publish it again, so the frame boundary does, once:
 * 1 at most once per suspension or change, which this clears; a publication or a new start clears it too. 0 with the
 * setting off (one load), for another owner, or for a check not suspended. */
int dsd_engine_scan_quiet_take_republish(const dsd_opts* opts, dsd_state* state, int owner_kind);
/** Something a check depends on changed (dsd_engine_scan_quiet_note_change(), now): a check under way is cancelled
 * (its request ticket-matched) and SUSPENDED until republished. A DONE check stays DONE for its visit. */
void dsd_engine_scan_quiet_invalidate(dsd_state* state);
/** The setting changed (dsd_scan_quiet_setting_changed()): as dsd_engine_scan_quiet_invalidate(). With @p turned_on
 * (off before, on now) the setting applies from the next visit, that is from the next scan tune: the visit landed now
 * is DONE, and so is any visit tuned before, in flight or landed but not yet noted, at its publication. With
 * @p reading_changed (the switch or the reading length differs from before) a shortened reading's WARN lines are
 * logged again. */
void dsd_engine_scan_quiet_setting_changed(dsd_state* state, int turned_on, int reading_changed);

/* Facts the scans own, read by the check. */
/** The trunk-scan coordinator's session-edit counter (0 with none). trunk_scan.c. */
uint32_t dsd_engine_trunk_scan_quiet_edit_seq(const dsd_state* state);
/** Whether the trunk-scan target on air saw allowed activity at or after @p since_m (decode mono). trunk_scan.c. */
int dsd_engine_trunk_scan_quiet_activity_since(const dsd_state* state, double since_m);
/** The -Y scan's row-edit counter (0 with none). channel_scan.c. */
uint32_t dsd_engine_channel_scan_quiet_edits_epoch(const dsd_state* state);

/* Engine service. */
/** A -Y row landed and committed (issue #834): the row is published for its check at once, as the trunk scan publishes
 * a target at its tick's tail after an advance, so its capture wait and request run from the hunt that follows rather
 * than from the next noCarrier(). Every committed landing calls it: a typed row's commit (the timed step, a tune that
 * completed late, a manual step, a go-to, a resume) and an untyped row's install (the timed step, a tune that landed
 * late, a walk the scan owns, a go-to, a resume once it is published). The visit's activity baseline stays its tune's
 * issue, which the landing carries. A tune that failed or was superseded lands nothing, so it arms nothing. No-op with
 * skip-silent off, outside a running -Y scan (a resume still pending publishes its row when it lands) or with no row
 * landed. engine.c. */
void dsd_engine_scan_quiet_publish_row(const dsd_opts* opts, dsd_state* state);
/** The outer loop's pass owes the trunk scan's quiet check its engine service: the next coordinator tick under the
 * guard publishes the visit on air, so the per-frame ticks of a carrier never walk for it. Called by the live
 * loop before its pass's tick, with skip-silent on. trunk_scan.c. */
void dsd_engine_trunk_scan_quiet_service_pass(const dsd_state* state);

#endif
