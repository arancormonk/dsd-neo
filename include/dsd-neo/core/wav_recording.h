// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief What went wrong with the per-call WAV recordings this decoder run (issue #708).
 *
 * The per-call writers and closers (`open_wav_file()`, the segment writer, the close and publish) count each failure
 * here by kind, so a frontend can say that call recordings are failing (a full disk, a folder that went away) instead
 * of the files just not appearing. The counts cover one decoder run: the engine resets them when it starts. The
 * static (`-w`) and raw (`-6`) WAVs are not counted.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_WAV_RECORDING_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_WAV_RECORDING_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The kind of a per-call recording failure; values are stable (frontends show them). */
typedef enum {
    DSD_WAV_RECORDING_ERROR_NONE = 0,
    DSD_WAV_RECORDING_ERROR_OPEN = 1,    /**< a segment's temp file did not open */
    DSD_WAV_RECORDING_ERROR_WRITE = 2,   /**< a short write latched a segment failed */
    DSD_WAV_RECORDING_ERROR_CLOSE = 3,   /**< sf_close() failed; the segment stays under its temp name */
    DSD_WAV_RECORDING_ERROR_PUBLISH = 4, /**< the rename to the final name failed; the temp name stays */
} dsd_wav_recording_error;

enum { DSD_WAV_RECORDING_ERROR_KINDS = 5 };

typedef struct {
    uint32_t failures[DSD_WAV_RECORDING_ERROR_KINDS]; /**< by dsd_wav_recording_error; [NONE] stays 0 */
    int32_t last_error;                               /**< the kind of the latest failure, NONE for none */
} dsd_wav_recording_status;

/** Copy this run's counts. Any thread. */
void dsd_wav_recording_status_get(dsd_wav_recording_status* out);

/** Count one failure of @p kind. Returns 1 when it is the first of its kind this run (the caller reports that one),
 * 0 after it, and 0 for an unknown kind. Any thread. */
int dsd_wav_recording_note_failure(dsd_wav_recording_error kind);

/** Start a decoder run's counts from zero. The engine calls it before it opens the run's recordings. */
void dsd_wav_recording_session_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_WAV_RECORDING_H_ */
