// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief What call recording is doing, for every frontend alike (issue #708).
 *
 * The decoder publishes this view with each options snapshot: the recording mode, whether the recording command a
 * frontend submitted last has been applied (WAV commands and the unknown-only switch wait while a decoder holds a
 * carrier in a loop of its own, and are refused while Signal search runs), and what failed this decoder run. A frontend
 * shows "applying" while @c command_state is PENDING and the outcome after, and says that call recordings are failing
 * when a count moves, instead of the files just not appearing.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RECORDING_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RECORDING_VIEW_H_

#include <dsd-neo/core/wav_recording.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What the decoder records to; values are stable. */
enum {
    DSD_APP_RECORDING_MODE_OFF = 0,
    DSD_APP_RECORDING_MODE_STATIC = 1,   /**< one static WAV (-w) */
    DSD_APP_RECORDING_MODE_PER_CALL = 2, /**< per-call WAVs (-P) */
};

/** The last recording command a frontend submitted (WAV start, stop, toggle, static open, unknown-only set, or a config
 *  carrying [recording], whose outcome is its recording's); values are stable. */
enum {
    DSD_APP_RECORDING_COMMAND_NONE = 0,    /**< none submitted in this process */
    DSD_APP_RECORDING_COMMAND_PENDING = 1, /**< submitted, not applied yet */
    DSD_APP_RECORDING_COMMAND_APPLIED = 2,
    /** applied, and it failed (a file did not open, or the config carrying it was refused); nothing changed */
    DSD_APP_RECORDING_COMMAND_FAILED = 3,
    DSD_APP_RECORDING_COMMAND_REFUSED = 4, /**< not applied: refused (Signal search runs, or not supported here) */
    DSD_APP_RECORDING_COMMAND_DROPPED = 5, /**< not applied: dropped from a full queue, or the session ended first */
};

/** The recording journal's state (`--wav-journal`, <dsd-neo/core/wav_journal.h>); values are stable. */
enum {
    DSD_APP_RECORDING_JOURNAL_NONE = 0,   /**< no journal this decoder run */
    DSD_APP_RECORDING_JOURNAL_ON = 1,     /**< records are being written */
    DSD_APP_RECORDING_JOURNAL_FAILED = 2, /**< the open or a write failed: off for the rest of the run */
};

typedef struct {
    uint8_t published;     /**< 0 until the decoder publishes a view; then every field below is set */
    uint8_t mode;          /**< DSD_APP_RECORDING_MODE_* */
    uint8_t per_call;      /**< 1 when @c mode is PER_CALL: per-call recording is on */
    uint8_t slot_files;    /**< per-call slot files open now: bit 0 slot 1, bit 1 slot 2 */
    uint8_t unknown_only;  /**< 1 while only unknown talkgroups record (opts->record_unknown_only) */
    uint8_t journal_state; /**< DSD_APP_RECORDING_JOURNAL_* */
    /** 1 when the run names a recording journal (--wav-journal), however its open went: the Qt app names one exactly
     *  when it has a folder of its own to record into, so 0 there means no storage for call recordings */
    uint8_t journal_named;
    uint8_t command_state; /**< DSD_APP_RECORDING_COMMAND_* */
    int32_t command_id;    /**< the dsd_app_command_id @c command_state is about; 0 for NONE */
    uint32_t failures[DSD_WAV_RECORDING_ERROR_KINDS]; /**< this decoder run's failures by dsd_wav_recording_error */
    int32_t last_error;                               /**< the latest failure's kind, NONE for none */
} dsd_app_recording_view;

/**
 * @brief The latest published view, with @c command_state PENDING for a command submitted since.
 *
 * Any thread. Returns 1 once the decoder has published one, 0 before (@p out then holds no recording state, only the
 * last command's pending or dropped outcome), -1 for a NULL @p out. Like the snapshots, the view stays readable after the session stops, at its last values.
 */
int dsd_app_recording_view_get(dsd_app_recording_view* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RECORDING_VIEW_H_ */
