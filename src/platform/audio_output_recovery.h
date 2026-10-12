// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief When an output stream whose device failed may reopen it, and what to report about it.
 *
 * The PulseAudio and PortAudio backends close a device whose write fails and reopen it behind the same stream handle,
 * so the decoder's handle never goes stale and a sound-server restart costs a gap, not the rest of the session. This
 * is the bookkeeping they share; it reads no clock and takes no lock, the callers pass the time and hold the stream's
 * lock where it has one.
 *
 * - The first failure after healthy playback retries on the next attempt: an ordinary hiccup recovers at once.
 * - Each further failure, of a reopen or of the first writes after one, doubles the wait from
 *   DSD_AUDIO_RECOVERY_BACKOFF_MIN_MS up to DSD_AUDIO_RECOVERY_BACKOFF_MAX_MS, so a server that is down is not
 *   hammered and a device that opens but cannot be written does not cycle at the write rate.
 * - The wait goes back to zero only once a reopened device has taken writes for DSD_AUDIO_RECOVERY_HEALTHY_MS.
 * - A failure episode counts as recovered at the first write the device takes after it, never when a write is only
 *   queued for the device.
 */

#ifndef DSD_NEO_SRC_PLATFORM_AUDIO_OUTPUT_RECOVERY_H
#define DSD_NEO_SRC_PLATFORM_AUDIO_OUTPUT_RECOVERY_H

#include <dsd-neo/core/string_utils.h>
#include <stdint.h>

enum {
    DSD_AUDIO_RECOVERY_BACKOFF_MIN_MS = 250,
    DSD_AUDIO_RECOVERY_BACKOFF_MAX_MS = 4000,
    DSD_AUDIO_RECOVERY_HEALTHY_MS = 2000,
    DSD_AUDIO_RECOVERY_ERROR_CAPACITY = 256,
};

typedef struct {
    int down;                 /* the device is unusable and a reopen is owed (PortAudio closes it at the reopen) */
    int episode_open;         /* a failure no device write has followed yet */
    unsigned int streak;      /* failures since the device last stayed healthy */
    uint64_t retry_at_ns;     /* monotonic time the next reopen may start */
    uint64_t healthy_from_ns; /* monotonic time the open device first took a write; 0 before that */
    uint64_t failures;        /* failure episodes since the stream opened */
    uint64_t recoveries;      /* episodes a device write ended */
    char error[DSD_AUDIO_RECOVERY_ERROR_CAPACITY];
} dsd_audio_output_recovery;

static inline uint64_t
audio_recovery_backoff_ns(unsigned int streak) {
    uint64_t ms = DSD_AUDIO_RECOVERY_BACKOFF_MIN_MS;
    for (unsigned int i = 1; i < streak && ms < (uint64_t)DSD_AUDIO_RECOVERY_BACKOFF_MAX_MS; i++) {
        ms *= 2U;
    }
    if (ms > (uint64_t)DSD_AUDIO_RECOVERY_BACKOFF_MAX_MS) {
        ms = DSD_AUDIO_RECOVERY_BACKOFF_MAX_MS;
    }
    return ms * 1000000ULL;
}

static inline void
audio_recovery_note_error(dsd_audio_output_recovery* r, const char* msg) {
    DSD_STRNCPY(r->error, msg ? msg : "", sizeof(r->error) - 1U);
    r->error[sizeof(r->error) - 1U] = '\0';
}

static inline void
audio_recovery_bump_streak(dsd_audio_output_recovery* r) {
    if (r->streak < 64U) {
        r->streak++;
    }
}

/* A device write failed or the device went away: the caller has closed it, or (a PortAudio async stream) left it to
   the writer's thread to close before the reopen. */
static inline void
audio_recovery_note_device_failed(dsd_audio_output_recovery* r, uint64_t now_ns, const char* msg) {
    if (!r->episode_open) {
        r->episode_open = 1;
        r->failures++;
    }
    audio_recovery_note_error(r, msg);
    r->down = 1;
    r->healthy_from_ns = 0;
    r->retry_at_ns = r->streak == 0U ? now_ns : now_ns + audio_recovery_backoff_ns(r->streak);
    audio_recovery_bump_streak(r);
}

/* Whether a closed device may be reopened now. */
static inline int
audio_recovery_reopen_due(const dsd_audio_output_recovery* r, uint64_t now_ns) {
    return r->down && now_ns >= r->retry_at_ns;
}

static inline void
audio_recovery_note_reopen_failed(dsd_audio_output_recovery* r, uint64_t now_ns, const char* msg) {
    audio_recovery_note_error(r, msg);
    r->retry_at_ns = now_ns + audio_recovery_backoff_ns(r->streak == 0U ? 1U : r->streak);
    audio_recovery_bump_streak(r);
}

static inline void
audio_recovery_note_reopened(dsd_audio_output_recovery* r) {
    r->down = 0;
    r->healthy_from_ns = 0;
}

/* The device took a write. */
static inline void
audio_recovery_note_write_ok(dsd_audio_output_recovery* r, uint64_t now_ns) {
    if (r->episode_open) {
        r->episode_open = 0;
        r->recoveries++;
    }
    if (r->healthy_from_ns == 0U) {
        r->healthy_from_ns = now_ns == 0U ? 1U : now_ns;
    } else if (r->streak != 0U && now_ns - r->healthy_from_ns >= (uint64_t)DSD_AUDIO_RECOVERY_HEALTHY_MS * 1000000ULL) {
        r->streak = 0;
    }
}

#endif /* DSD_NEO_SRC_PLATFORM_AUDIO_OUTPUT_RECOVERY_H */
