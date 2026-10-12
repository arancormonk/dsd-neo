// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The audio sinks' write path (issue #659): a device stream's failures and recoveries are reported once each, at most
 * one failure line a minute per stream; a failed -o - descriptor logs one error and stays off until the session binds
 * it again; a full non-blocking one drops whole blocks with one warning and a count a minute after it. The platform
 * writes and the log are stubbed here.
 */

#include <assert.h>
#include <dsd-neo/core/audio_sink.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"

static int g_warn_lines;
static int g_info_lines;
static int g_error_lines;
static char g_last_line[512];

void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    (void)DSD_VSNPRINTF(g_last_line, sizeof(g_last_line), format, ap);
    va_end(ap);
    if (level == LOG_LEVEL_WARN) {
        g_warn_lines++;
    } else if (level == LOG_LEVEL_INFO) {
        g_info_lines++;
    } else if (level == LOG_LEVEL_ERROR) {
        g_error_lines++;
    }
}

static uint64_t g_now_ms = 10000U;

uint64_t
dsd_realtime_mono_ms(void) {
    return g_now_ms;
}

/* The device stream: what a write returns and what its health reads. */
static int g_audio_write_calls;
static int g_audio_write_result;
static dsd_audio_output_health g_health;

int
dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames) {
    (void)stream;
    (void)buffer;
    g_audio_write_calls++;
    return g_audio_write_result < 0 ? g_audio_write_result : (int)frames;
}

int
dsd_audio_output_health_get(dsd_audio_stream* stream, dsd_audio_output_health* out) {
    (void)stream;
    *out = g_health;
    return 0;
}

/* The descriptor: what the next write does. */
static int g_write_frames_calls;
static dsd_write_frames_status g_write_frames_status;
static int g_write_frames_errno;

dsd_write_frames_status
dsd_write_frames(int fd, const void* buf, size_t bytes, size_t frame_bytes, size_t* written) {
    (void)fd;
    (void)buf;
    (void)frame_bytes;
    g_write_frames_calls++;
    if (written != NULL) {
        *written = g_write_frames_status == DSD_WRITE_FRAMES_OK ? bytes : 0U;
    }
    errno = g_write_frames_errno;
    return g_write_frames_status;
}

static void
reset_logs(void) {
    g_warn_lines = 0;
    g_info_lines = 0;
    g_error_lines = 0;
    g_last_line[0] = '\0';
}

static void
set_health(uint64_t failures, uint64_t recoveries, int down, const char* error) {
    DSD_MEMSET(&g_health, 0, sizeof(g_health));
    g_health.failures = failures;
    g_health.recoveries = recoveries;
    g_health.down = down;
    DSD_SNPRINTF(g_health.error, sizeof(g_health.error), "%s", error);
}

static void
test_device_failures_reported_once(void) {
    static int stream_a_token;
    static int stream_b_token;
    dsd_audio_stream* stream_a = (dsd_audio_stream*)&stream_a_token;
    dsd_audio_stream* stream_b = (dsd_audio_stream*)&stream_b_token;
    const int16_t samples[8] = {0};

    dsd_audio_sink_device_reset();
    reset_logs();
    set_health(0, 0, 0, "");

    /* No stream open: nothing is written. */
    g_audio_write_calls = 0;
    assert(dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, NULL, samples, 4) == 0);
    assert(g_audio_write_calls == 0);
    assert(dsd_audio_sink_write_device(DSD_AUDIO_SINK_DEVICE_COUNT, stream_a, samples, 4) == 0);
    assert(g_audio_write_calls == 0);

    /* A healthy stream: written, nothing logged. */
    assert(dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, stream_a, samples, 4) == 4);
    assert(g_audio_write_calls == 1);
    assert(g_warn_lines + g_info_lines + g_error_lines == 0);

    /* The device fails: one warning with the backend's reason, however many writes fail meanwhile. */
    g_audio_write_result = -1;
    set_health(1, 0, 1, "Connection terminated");
    for (int i = 0; i < 50; i++) {
        assert(dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, stream_a, samples, 4) == -1);
        g_now_ms += 20U;
    }
    assert(g_warn_lines == 1);
    assert(strstr(g_last_line, "digital device") != NULL);
    assert(strstr(g_last_line, "Connection terminated") != NULL);

    /* Reopened and playing: one line. */
    g_audio_write_result = 0;
    set_health(1, 1, 0, "Connection terminated");
    assert(dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, stream_a, samples, 4) == 4);
    assert(dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, stream_a, samples, 4) == 4);
    assert(g_info_lines == 1);
    assert(strstr(g_last_line, "playing again") != NULL);

    /* Failing again inside the minute: held back, and so is its recovery. */
    set_health(2, 1, 1, "Connection refused");
    (void)dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, stream_a, samples, 4);
    set_health(2, 2, 0, "Connection refused");
    (void)dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, stream_a, samples, 4);
    assert(g_warn_lines == 1);
    assert(g_info_lines == 1);

    /* A minute after the last line: reported again, with the one held back counted. */
    g_now_ms += 60000U;
    set_health(3, 2, 1, "Connection refused");
    (void)dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, stream_a, samples, 4);
    assert(g_warn_lines == 2);
    assert(strstr(g_last_line, "1 more") != NULL);

    set_health(3, 3, 0, "Connection refused");
    (void)dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, stream_a, samples, 4);
    assert(g_info_lines == 2);

    /* A minute on, a failure and its recovery between two writes: both lines. */
    g_now_ms += 60000U;
    set_health(4, 3, 0, "Connection reset");
    reset_logs();
    set_health(4, 4, 0, "Connection reset");
    (void)dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, stream_a, samples, 4);
    assert(g_warn_lines == 1 && g_info_lines == 1);

    /* The analog stream is reported on its own. */
    set_health(1, 0, 1, "Broken pipe");
    (void)dsd_audio_sink_write_device(DSD_AUDIO_SINK_RAW, stream_b, samples, 4);
    assert(g_warn_lines == 2);
    assert(strstr(g_last_line, "analog device") != NULL);

    /* Between two writes, an episode recovered and the next one failed and was reopened, not yet written: the first
       is reported, the second has not recovered, so there is no "playing again" until the device takes a write. */
    {
        static int stream_c_token;
        dsd_audio_stream* stream_c = (dsd_audio_stream*)&stream_c_token;
        reset_logs();
        set_health(0, 0, 0, "");
        (void)dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, stream_c, samples, 4);
        set_health(2, 1, 0, "Connection refused");
        (void)dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, stream_c, samples, 4);
        assert(g_warn_lines == 1);
        assert(g_info_lines == 0);
        set_health(2, 2, 0, "Connection refused");
        (void)dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, stream_c, samples, 4);
        assert(g_info_lines == 1);
    }

    /* A stream the session reopened starts over: its first failure is reported at once. */
    reset_logs();
    set_health(0, 0, 0, "");
    (void)dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, stream_b, samples, 4);
    set_health(1, 0, 1, "Timeout");
    (void)dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, stream_b, samples, 4);
    assert(g_warn_lines == 1);

    /* closeAudioOutput() forgets it all. */
    dsd_audio_sink_device_reset();
    reset_logs();
    set_health(1, 0, 1, "Timeout");
    (void)dsd_audio_sink_write_device(DSD_AUDIO_SINK_DIGITAL, stream_b, samples, 4);
    assert(g_warn_lines == 1);
    set_health(0, 0, 0, "");
    g_audio_write_result = 0;
    dsd_audio_sink_device_reset();
}

static void
test_fd_failure_turns_stdout_audio_off(void) {
    const uint8_t block[640] = {0};
    dsd_audio_sink_fd_reset();
    reset_logs();

    g_write_frames_calls = 0;
    assert(dsd_audio_sink_write_fd(-1, block, sizeof(block), 4) == -1);
    assert(g_write_frames_calls == 0);
    assert(!dsd_audio_sink_fd_usable(-1));

    g_write_frames_status = DSD_WRITE_FRAMES_OK;
    assert(dsd_audio_sink_fd_usable(1));
    assert(dsd_audio_sink_write_fd(1, block, sizeof(block), 4) == 0);
    assert(dsd_audio_sink_write_fd(1, block, 0, 4) == 0);
    assert(g_write_frames_calls == 1);

    /* The reader went away: one error, then the descriptor is left alone. */
    g_write_frames_status = DSD_WRITE_FRAMES_ERROR;
    g_write_frames_errno = EPIPE;
    assert(dsd_audio_sink_write_fd(1, block, sizeof(block), 4) == -1);
    assert(g_error_lines == 1);
    assert(strstr(g_last_line, "stdout") != NULL);
    assert(strstr(g_last_line, strerror(EPIPE)) != NULL);
    assert(!dsd_audio_sink_fd_usable(1));
    for (int i = 0; i < 100; i++) {
        assert(dsd_audio_sink_write_fd(1, block, sizeof(block), 4) == -1);
    }
    assert(g_write_frames_calls == 2);
    assert(g_error_lines == 1);
    assert(g_warn_lines == 0);

    /* Another descriptor is not affected; a new session's binding starts over. */
    g_write_frames_status = DSD_WRITE_FRAMES_OK;
    assert(dsd_audio_sink_fd_usable(5));
    dsd_audio_sink_fd_reset();
    assert(dsd_audio_sink_fd_usable(1));
    assert(dsd_audio_sink_write_fd(1, block, sizeof(block), 4) == 0);
}

static void
test_fd_full_drops_blocks_with_one_warning(void) {
    const uint8_t block[640] = {0};
    dsd_audio_sink_fd_reset();
    reset_logs();

    g_write_frames_status = DSD_WRITE_FRAMES_WOULD_BLOCK;
    for (int i = 0; i < 200; i++) {
        assert(dsd_audio_sink_write_fd(1, block, sizeof(block), 4) == -1);
        g_now_ms += 20U;
    }
    assert(g_warn_lines == 1);
    assert(strstr(g_last_line, "not keeping up") != NULL);
    /* Still usable: a full reader is not a failed one. */
    assert(dsd_audio_sink_fd_usable(1));
    g_write_frames_status = DSD_WRITE_FRAMES_OK;
    assert(dsd_audio_sink_write_fd(1, block, sizeof(block), 4) == 0);

    /* A minute on, the next drop reports how many went since. */
    g_now_ms += 60000U;
    g_write_frames_status = DSD_WRITE_FRAMES_WOULD_BLOCK;
    assert(dsd_audio_sink_write_fd(1, block, sizeof(block), 4) == -1);
    assert(g_warn_lines == 2);
    assert(strstr(g_last_line, "200 more") != NULL);
    assert(g_error_lines == 0);
    g_write_frames_status = DSD_WRITE_FRAMES_OK;
    dsd_audio_sink_fd_reset();
}

static void
test_monitor_takes_fd_only_in_analog_only_modes(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.audio_out_type = 1;
    opts.monitor_input_audio = 1;
    opts.analog_only = 1;
    assert(dsd_audio_sink_monitor_takes_fd(&opts) == 1);
    opts.analog_only = 0;
    assert(dsd_audio_sink_monitor_takes_fd(&opts) == 0);
    opts.analog_only = 1;
    opts.audio_out_type = 8;
    assert(dsd_audio_sink_monitor_takes_fd(&opts) == 0);
    assert(dsd_audio_sink_monitor_takes_fd(NULL) == 0);
}

int
main(void) {
    test_device_failures_reported_once();
    test_fd_failure_turns_stdout_audio_off();
    test_fd_full_drops_blocks_with_one_warning();
    test_monitor_takes_fd_only_in_analog_only_modes();
    printf("CORE_AUDIO_SINK: OK\n");
    return 0;
}
