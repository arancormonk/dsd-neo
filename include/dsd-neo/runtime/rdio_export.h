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
 * As dsd_rdio_export_call(); @p api_queued (may be NULL) gets 1 when an API upload of @p wav_path was queued, whose
 * worker may delete the file once it is uploaded.
 */
int dsd_rdio_export_call_ex(const dsd_opts* opts, const Event_History_I* event_struct, const char* wav_path,
                            time_t opened, int* api_queued);

/**
 * When a per-call recording began, the one rule the rdio-scanner sidecar's start_time and the recording journal
 * (issue #708) share: the row's event_start_time, or @p opened (the decode time the WAV was opened, 0 when unknown)
 * when that is later, as when per-call WAV is switched on mid-call. A row with no start falls back to its
 * event_time, unadjusted (a --playfiles row, stamped on the file's clock), then, as does a NULL @p event, to the
 * decode clock.
 */
time_t dsd_rdio_recording_start(const Event_History* event, time_t opened);

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
