// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief rdio-scanner export helpers (DirWatch sidecar + optional API upload).
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RDIO_EXPORT_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RDIO_EXPORT_H_H

#include <dsd-neo/platform/platform.h>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

enum DSD_ATTR_PACKED {
    DSD_RDIO_MODE_OFF = 0,
    DSD_RDIO_MODE_DIRWATCH = 1,
    DSD_RDIO_MODE_API = 2,
    DSD_RDIO_MODE_BOTH = 3,
};

/**
 * Parse textual rdio mode: off|dirwatch|api|both.
 *
 * @param s Input mode string.
 * @param mode_out Parsed mode output.
 * @return 0 on success, -1 on invalid input.
 */
int dsd_rdio_mode_from_string(const char* s, int* mode_out);

/**
 * Convert rdio mode value to text.
 *
 * @param mode Mode value.
 * @return String name ("off" for unknown values).
 */
const char* dsd_rdio_mode_to_string(int mode);

/**
 * Export a completed per-call WAV for rdio-scanner integration.
 *
 * Uses trunk-recorder compatible JSON metadata sidecars for DirWatch, and
 * optionally performs API upload when built with libcurl.
 *
 * The sidecar's start_time is the call's start (the staged row's event_start_time), or @p opened
 * when the recording was opened after that, as when per-call WAV is switched on mid-call. A row
 * with no start falls back to its event_time, unadjusted, then to the decode clock. stop_time is
 * start_time plus the WAV's length.
 *
 * @param opts Decoder options with rdio settings.
 * @param event_struct Event metadata for this call.
 * @param wav_path Final renamed WAV file path.
 * @param opened Decode time the WAV was opened, or 0 when unknown.
 * @return 0 when work completed (or mode disabled), -1 on export failure.
 */
int dsd_rdio_export_call(const dsd_opts* opts, const Event_History_I* event_struct, const char* wav_path,
                         time_t opened);

/**
 * As dsd_rdio_export_call(), with @p started, the recording's own start when its recorder stamped it (0 when not; see
 * dsd_rdio_recording_start()). @p api_queued (may be NULL) gets 1 when an API upload of @p wav_path was queued, whose
 * worker may delete the file once it is uploaded.
 */
int dsd_rdio_export_call_ex(const dsd_opts* opts, const Event_History_I* event_struct, const char* wav_path,
                            time_t opened, time_t started, int* api_queued);

/**
 * As dsd_rdio_export_call_ex(), with @p allow_fallback (issue #888): when nonzero, a digital call with no talkgroup
 * number is exported under dsd_rdio_call_talkgroup()'s fallback number, labelled by its service name, else its channel
 * label, else "<protocol> <access code> <MHz> MHz" with the unknown parts left out. The earlier entries pass 0, so they
 * never give a digital call a synthetic number. A call with no number is not exported (-1, no sidecar): the caller asks
 * dsd_rdio_call_talkgroup() with the same @p allow_fallback first to skip such a call quietly.
 */
int dsd_rdio_export_call_fallback(const dsd_opts* opts, const Event_History_I* event_struct, const char* wav_path,
                                  time_t opened, time_t started, int allow_fallback, int* api_queued);

/**
 * When a per-call recording began, the one rule the rdio-scanner sidecar's start_time and the recording journal
 * (issue #708) share: @p started when it is above 0, the decode time of the recording's first sample as its recorder
 * stamped it (the NFM call, issue #668); else the row's event_start_time, or @p opened (the decode time the WAV was
 * opened, 0 when unknown) when that is later, as when per-call WAV is switched on mid-call. A row with no start falls
 * back to its event_time, unadjusted (a --playfiles row, stamped on the file's clock), then, as does a NULL @p event,
 * to the decode clock.
 */
time_t dsd_rdio_recording_start(const Event_History* event, time_t opened, time_t started);

/**
 * The rdio-scanner talkgroup a conventional analog call (a DSD_SYNC_ANALOG row, issue #668) is exported under: the
 * frequency it was heard on in kHz, rounded down (rdio-scanner's convention for conventional channels), else, for a
 * call with no frequency (audio input), @p opts->rdio_analog_talkgroup. 0 when it has neither, and for any other row.
 * Worked out at export time only, never stored on the row: Event_History::target_id stays 0 for analog calls.
 */
uint32_t dsd_rdio_analog_talkgroup(const dsd_opts* opts, const Event_History* event);

/**
 * The rdio-scanner talkgroup a call is exported under, 0 when it has none (it is then not exported); NULL @p event
 * gives 0. Worked out at export time only, never stored on the row: history, journal, file name and policy keep the
 * decoded target.
 * - An analog row (DSD_SYNC_ANALOG): dsd_rdio_analog_talkgroup(), whatever @p allow_fallback says.
 * - A digital row: its target_id when above 0, as decoded (an ID above 2147483647 included). Else, only when
 *   @p allow_fallback is nonzero, the row's category is DSD_EVENT_CATEGORY_VOICE, its sync type names a protocol
 *   (P25, DMR, NXDN, D-STAR, YSF, dPMR, M17, X2-TDMA, ProVoice, EDACS, TETRA; issue #888 D2, D7) and its
 *   fallback_blocked is 0 (an ID its decoder could not read, a YSF call not ended on its own terminator): the frequency
 *   it was heard on in whole kHz, rounded down, when that is 1..2147483647 (signed 32-bit, as rdio-scanner's databases
 *   store it), else @p opts->rdio_unknown_talkgroup when above 0 (audio input), else 0.
 */
uint32_t dsd_rdio_call_talkgroup(const dsd_opts* opts, const Event_History* event, int allow_fallback);

/**
 * 1 when dsd_rdio_call_talkgroup() with the same @p allow_fallback numbers @p event by the fallback, or would with a
 * frequency or --rdio-unknown-talkgroup: a digital row with no target_id whose category is DSD_EVENT_CATEGORY_VOICE,
 * whose sync type names a protocol and whose fallback_blocked is 0, with @p allow_fallback nonzero. 0 for every other
 * row, NULL and analog rows included.
 * The WAV close asks it to tell a call skipped for want of a number (said once a run) from one the fallback never
 * numbers (issue #888 B3).
 */
int dsd_rdio_call_fallback_eligible(const Event_History* event, int allow_fallback);

/**
 * The rdio-scanner system a call is exported to, as the sidecar's `system` and the API upload's `system` part: @p
 * opts->rdio_analog_system_id for an analog row when it is set; else, with @p opts->rdio_system_by_frequency (issue
 * #931), the row's frequency in whole kHz, rounded down, when that is 1..2147483647; else @p opts->rdio_system_id. 0
 * for a NULL @p opts.
 */
int dsd_rdio_call_system_id(const dsd_opts* opts, const Event_History* event);

/**
 * Whether the rdio API upload queue still owns the per-call WAV at @p path: a job naming it is queued or in flight (from
 * the moment the worker takes it until its upload, and the delete after a successful one, are over). Issue #708: the
 * app's recording store never deletes such a file.
 *
 * Read under the queue's own lock, with no I/O, from any thread at any time. A job names the file when its WAV's final
 * path component is @p path's (without case on Windows); folders are not compared, since one folder can be reached by
 * several paths, so a file of the same name in another folder reads as held too, which only delays its deletion. A
 * session that ended dropped its queue (dsd_rdio_upload_shutdown()), so nothing is held after it. Always 0 in builds
 * without libcurl, and for a NULL or empty @p path or one ending in a separator.
 */
int dsd_rdio_upload_holds(const char* path);

/**
 * Whether a run's recording journal (`--wav-journal`, issue #708) keeps the per-call WAVs delete-after-upload would
 * remove: 1 when @p opts names a journal and turns delete-after-upload on, 0 otherwise (NULL @p opts included).
 *
 * The journal's owner (the app's recording store) manages those files' retention, so a file deleted after its upload
 * would vanish under playback or export. While a journal is set, a successful API-only upload keeps its WAV
 * (dsd_rdio_export_call() queues the job without the delete); the option itself keeps its configured value, so a
 * config saved from the run still has it.
 */
int dsd_rdio_journal_keeps_uploads(const dsd_opts* opts);

/**
 * Say once in the log that uploads are kept because a journal names them, when @p opts newly combines a journal with
 * delete-after-upload: @p kept_before is dsd_rdio_journal_keeps_uploads() before the change (0 at bootstrap). Called
 * after the config and the command line are applied, and after a live config apply. Returns 1 when it warned.
 */
int dsd_rdio_warn_journal_keeps_uploads(const dsd_opts* opts, int kept_before);

/**
 * Start a decoder run's once-a-run rdio warnings over: the first call an API upload skips for want of a system ID, the
 * first private upload input that cannot be removed, and delete-after-upload ignored beside DirWatch, are each said once
 * in the log per run. Called where a run sets up (the CLI's and the Android app's alike), before its first recording; a
 * no-op in builds without libcurl.
 */
void dsd_rdio_session_reset(void);

/**
 * Drain queued rdio API uploads and stop the background worker.
 *
 * Intended for process shutdown after final call files are closed/renamed.
 * Safe to call when the worker was never started.
 */
void dsd_rdio_upload_shutdown(void);

/**
 * Abandon the rdio API uploads still pending, for a stop the user asked for.
 *
 * This is the user-stop path only (the Android app's stops and restarts the user starts), which is why the shutdown's
 * log line reads "stopped by the user". Callers must not use it for any other kind of stop.
 *
 * Nonblocking and callable from any thread at any time, including while dsd_rdio_upload_shutdown() is already
 * draining on another thread. The worker takes no further jobs, stops between retry attempts and aborts a transfer
 * in flight; dsd_rdio_upload_shutdown() still owns the join, and logs once how many uploads were not sent. Their WAV
 * and sidecar files stay on disk, since deletion only ever follows a successful upload. Uploads queued after this
 * call, such as the final call closed during cleanup, are not sent either.
 *
 * Normal ends (end of a replay, input closed, device detached) and the CLI do not call this: their shutdown drains.
 * The request lasts until dsd_rdio_upload_shutdown() returns or dsd_rdio_upload_cancel_reset() is called. A no-op in
 * builds without libcurl.
 */
void dsd_rdio_upload_cancel(void);

/**
 * Clear a dsd_rdio_upload_cancel() request before a new session starts.
 *
 * dsd_rdio_upload_shutdown() clears it on return already; an embedding host that runs several sessions calls this
 * when it configures the next one, so a cancel that arrived after the last session's shutdown cannot leak into it.
 * Call only between sessions, never while a session is running. A no-op in builds without libcurl.
 */
void dsd_rdio_upload_cancel_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RDIO_EXPORT_H_H */
