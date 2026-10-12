// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The audio sinks' shared write path (issue #659); see <dsd-neo/core/audio_sink.h>. */

#include <dsd-neo/core/audio_sink.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"

/* At most one failure line a minute per output; the ones held back are counted into the next. */
#define DSD_AUDIO_SINK_REPORT_INTERVAL_MS 60000U

typedef struct {
    const dsd_audio_stream* stream; /* the stream the counts below were read from */
    uint64_t failures_seen;
    uint64_t recoveries_seen;
    uint64_t last_report_ms; /* 0: none yet */
    uint64_t held_back;      /* failures not reported since the last line */
    int episode_reported;    /* the latest failure was reported, so its recovery is too */
} dsd_audio_sink_device_report;

typedef struct {
    int disabled_fd; /* -1: none; else the descriptor a failed write turned off */
    int dropping;    /* a full non-blocking descriptor dropped a block and that was reported */
    uint64_t dropped_blocks;
    uint64_t last_drop_report_ms;
} dsd_audio_sink_fd_report;

static dsd_audio_sink_device_report g_device_report[DSD_AUDIO_SINK_DEVICE_COUNT];
static dsd_audio_sink_fd_report g_fd_report = {-1, 0, 0U, 0U};

static const char*
dsd_audio_sink_device_name(dsd_audio_sink_device which) {
    return which == DSD_AUDIO_SINK_RAW ? "analog" : "digital";
}

static int
dsd_audio_sink_report_due(uint64_t last_ms, uint64_t now_ms) {
    return last_ms == 0U || now_ms - last_ms >= DSD_AUDIO_SINK_REPORT_INTERVAL_MS;
}

static void
dsd_audio_sink_report_device(dsd_audio_sink_device which, const dsd_audio_stream* stream,
                             const dsd_audio_output_health* health) {
    dsd_audio_sink_device_report* r = &g_device_report[which];
    if (r->stream != stream || health->failures < r->failures_seen || health->recoveries < r->recoveries_seen) {
        /* Another stream (a reopen by the session, or a new one at a freed one's address): its counts start at 0. */
        DSD_MEMSET(r, 0, sizeof(*r));
        r->stream = stream;
    }

    if (health->failures > r->failures_seen) {
        const uint64_t episodes = health->failures - r->failures_seen;
        r->failures_seen = health->failures;
        const uint64_t now_ms = dsd_realtime_mono_ms();
        if (dsd_audio_sink_report_due(r->last_report_ms, now_ms)) {
            const uint64_t held_back = r->held_back + episodes - 1U;
            if (held_back > 0U) {
                LOG_WARN("Audio output (%s device): write failed: %s; reopening it, decoding continues (%llu more "
                         "failures since the last report)\n",
                         dsd_audio_sink_device_name(which), health->error, (unsigned long long)held_back);
            } else {
                LOG_WARN("Audio output (%s device): write failed: %s; reopening it, decoding continues\n",
                         dsd_audio_sink_device_name(which), health->error);
            }
            r->last_report_ms = now_ms == 0U ? 1U : now_ms;
            r->held_back = 0U;
            r->episode_reported = 1;
        } else {
            r->held_back += episodes;
            r->episode_reported = 0;
        }
    }

    if (health->recoveries > r->recoveries_seen) {
        r->recoveries_seen = health->recoveries;
        /* Episodes run one at a time, each ended by one recovery: the latest is over only when they balance. A device
           that is reopened but has not taken a write yet has not recovered. */
        if (r->episode_reported && health->recoveries >= health->failures) {
            LOG_INFO("Audio output (%s device): reopened, playing again\n", dsd_audio_sink_device_name(which));
            r->episode_reported = 0;
        }
    }
}

int
dsd_audio_sink_write_device(dsd_audio_sink_device which, dsd_audio_stream* stream, const int16_t* samples,
                            size_t frames) {
    if (stream == NULL || samples == NULL || frames == 0U || (int)which < 0 || which >= DSD_AUDIO_SINK_DEVICE_COUNT) {
        return 0;
    }
    const int rc = dsd_audio_write(stream, samples, frames);
    dsd_audio_output_health health;
    if (dsd_audio_output_health_get(stream, &health) == 0) {
        dsd_audio_sink_report_device(which, stream, &health);
    }
    return rc;
}

void
dsd_audio_sink_device_reset(void) {
    DSD_MEMSET(g_device_report, 0, sizeof(g_device_report));
}

int
dsd_audio_sink_fd_usable(int fd) {
    return fd >= 0 && fd != g_fd_report.disabled_fd;
}

void
dsd_audio_sink_fd_reset(void) {
    g_fd_report.disabled_fd = -1;
    g_fd_report.dropping = 0;
    g_fd_report.dropped_blocks = 0U;
    g_fd_report.last_drop_report_ms = 0U;
}

static void
dsd_audio_sink_note_fd_drop(void) {
    g_fd_report.dropped_blocks++;
    const uint64_t now_ms = dsd_realtime_mono_ms();
    if (!g_fd_report.dropping) {
        LOG_WARN("Audio output (stdout): the reader is not keeping up; dropping whole audio blocks while it is full\n");
        g_fd_report.dropping = 1;
        g_fd_report.dropped_blocks = 0U;
        g_fd_report.last_drop_report_ms = now_ms == 0U ? 1U : now_ms;
        return;
    }
    if (dsd_audio_sink_report_due(g_fd_report.last_drop_report_ms, now_ms)) {
        LOG_WARN("Audio output (stdout): dropped %llu more audio blocks the reader had no room for\n",
                 (unsigned long long)g_fd_report.dropped_blocks);
        g_fd_report.dropped_blocks = 0U;
        g_fd_report.last_drop_report_ms = now_ms == 0U ? 1U : now_ms;
    }
}

int
dsd_audio_sink_write_fd(int fd, const void* data, size_t bytes, size_t frame_bytes) {
    if (!dsd_audio_sink_fd_usable(fd)) {
        return -1;
    }
    if (data == NULL || bytes == 0U) {
        return 0;
    }
    size_t written = 0;
    const dsd_write_frames_status status = dsd_write_frames(fd, data, bytes, frame_bytes, &written);
    if (status == DSD_WRITE_FRAMES_OK) {
        return 0;
    }
    if (status == DSD_WRITE_FRAMES_WOULD_BLOCK) {
        dsd_audio_sink_note_fd_drop();
        return -1;
    }
    const int err = errno;
    g_fd_report.disabled_fd = fd;
    LOG_ERROR("Audio output (stdout): write failed after %zu of %zu bytes: %s; stdout audio is off for the rest of "
              "the session, decoding and recording continue\n",
              written, bytes, strerror(err));
    return -1;
}

int
dsd_audio_sink_monitor_takes_fd(const dsd_opts* opts) {
    return opts != NULL && opts->audio_out_type == 1 && opts->analog_only == 1;
}
