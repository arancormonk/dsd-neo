// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/wav_recording.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <stdint.h>

/* The decoder and the watchdog both open, write and publish recordings, and a frontend's snapshot publication reads
   them: every field is atomic, and a reader can see one count move before the last error kind does. */
static atomic_int s_failures[DSD_WAV_RECORDING_ERROR_KINDS];
static atomic_int s_reported[DSD_WAV_RECORDING_ERROR_KINDS];
static atomic_int s_last_error;

void
dsd_wav_recording_status_get(dsd_wav_recording_status* out) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    for (int kind = 0; kind < DSD_WAV_RECORDING_ERROR_KINDS; kind++) {
        out->failures[kind] = (uint32_t)atomic_load(&s_failures[kind]);
    }
    out->last_error = (int32_t)atomic_load(&s_last_error);
}

int
dsd_wav_recording_note_failure(dsd_wav_recording_error kind) {
    if ((int)kind <= (int)DSD_WAV_RECORDING_ERROR_NONE || (int)kind >= DSD_WAV_RECORDING_ERROR_KINDS) {
        return 0;
    }
    (void)atomic_fetch_add(&s_failures[kind], 1);
    atomic_store(&s_last_error, (int)kind);
    return atomic_exchange(&s_reported[kind], 1) == 0;
}

void
dsd_wav_recording_session_reset(void) {
    for (int kind = 0; kind < DSD_WAV_RECORDING_ERROR_KINDS; kind++) {
        atomic_store(&s_failures[kind], 0);
        atomic_store(&s_reported[kind], 0);
    }
    atomic_store(&s_last_error, DSD_WAV_RECORDING_ERROR_NONE);
}
