// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/notification_status.h>
#include <dsd-neo/app_control/recording_view.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/wav_journal.h>
#include <dsd-neo/core/wav_recording.h>
#include "recording_view_internal.h"
#include "snapshot_internal.h"
#include "test_support.h"

static void
test_initial_snapshot_is_absent(void) {
    assert(dsd_app_get_latest_opts_snapshot() == NULL);
}

static void
test_publish_copies_latest_options(void) {
    static dsd_opts opts;
    const dsd_opts* snap;

    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.audio_out = 1;
    opts.rtlsdr_center_freq = 851012500U;
    opts.rtlsdr_ppm_error = -2;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.0125M");

    dsd_app_telemetry_publish_opts_snapshot(&opts);
    opts.audio_in_type = AUDIO_IN_TCP;
    opts.rtlsdr_center_freq = 0U;
    opts.audio_in_dev[0] = '\0';

    snap = dsd_app_get_latest_opts_snapshot();
    assert(snap != NULL);
    assert(snap->audio_in_type == AUDIO_IN_RTL);
    assert(snap->audio_out == 1);
    assert(snap->rtlsdr_center_freq == 851012500U);
    assert(snap->rtlsdr_ppm_error == -2);
    assert(strcmp(snap->audio_in_dev, "rtl:0:851.0125M") == 0);
}

static void
test_republish_updates_stable_consumer_copy(void) {
    static dsd_opts opts;
    const dsd_opts* first;
    const dsd_opts* second;

    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    dsd_app_telemetry_publish_opts_snapshot(&opts);
    first = dsd_app_get_latest_opts_snapshot();
    assert(first != NULL);
    assert(first->audio_in_type == AUDIO_IN_WAV);

    opts.audio_in_type = AUDIO_IN_UDP;
    opts.udp_portno = 7355;
    dsd_app_telemetry_publish_opts_snapshot(&opts);
    second = dsd_app_get_latest_opts_snapshot();
    assert(second == first);
    assert(second->audio_in_type == AUDIO_IN_UDP);
    assert(second->udp_portno == 7355);
}

/* This publisher also feeds the notification record, which the Android foreground service
   reads with no Qt in the picture. That fan-out lives at the tail of
   dsd_app_telemetry_publish_opts_snapshot() and had no coverage anywhere: deleting the
   call left the whole suite green while the notification went permanently blank. */
static void
test_publish_also_feeds_the_notification_record(void) {
    static dsd_opts opts;

    /* The notification publishers stay dormant until a reader has asked once, so arm them
       before publishing or the fan-out below is a no-op for a reason that has nothing to
       do with the wiring under test. */
    dsd_app_notification_status status;
    (void)dsd_app_notification_get(&status);

    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.trunk_enable = 1;
    opts.rtlsdr_center_freq = 851012500U;
    dsd_app_telemetry_publish_opts_snapshot(&opts);

    assert(dsd_app_notification_get(&status) == 1);
    assert(status.radio_input == 1U);
    assert(status.trunking == 1U);
    assert(status.center_freq_hz == 851012500);
}

/* Issue #708: the recording view goes out with the options snapshot. A command is pending from its admission until a
   view built after it left the queue, which shows its outcome with the mode it left; a dropped one shows at once. */
static void
test_publish_carries_the_recording_view(void) {
    static dsd_opts opts;
    dsd_app_recording_view view;

    DSD_MEMSET(&opts, 0, sizeof opts);
    dsd_wav_recording_session_reset();
    dsd_app_telemetry_publish_opts_snapshot(&opts);
    assert(dsd_app_recording_view_get(&view) == 1);
    assert(view.mode == DSD_APP_RECORDING_MODE_OFF && view.per_call == 0);
    assert(view.command_state == DSD_APP_RECORDING_COMMAND_NONE && view.command_id == 0);
    assert(view.unknown_only == 0 && view.journal_state == DSD_APP_RECORDING_JOURNAL_NONE);
    assert(view.last_error == DSD_WAV_RECORDING_ERROR_NONE);

    /* Not a recording command: no state. */
    dsd_app_recording_command_submitted(DSD_APP_CMD_TOGGLE_MUTE);
    assert(dsd_app_recording_view_get(&view) == 1 && view.command_state == DSD_APP_RECORDING_COMMAND_NONE);

    dsd_app_recording_command_submitted(DSD_APP_CMD_WAV_START);
    assert(dsd_app_recording_view_get(&view) == 1);
    assert(view.command_state == DSD_APP_RECORDING_COMMAND_PENDING && view.command_id == DSD_APP_CMD_WAV_START);
    dsd_app_recording_command_resolved(DSD_APP_CMD_WAV_START, DSD_APP_RECORDING_COMMAND_APPLIED);
    assert(dsd_app_recording_view_get(&view) == 1 && view.command_state == DSD_APP_RECORDING_COMMAND_PENDING);
    opts.dmr_stereo_wav = 1;
    opts.wav_out_fR = (SNDFILE*)&opts;
    dsd_app_telemetry_publish_opts_snapshot(&opts);
    assert(dsd_app_recording_view_get(&view) == 1);
    assert(view.command_state == DSD_APP_RECORDING_COMMAND_APPLIED && view.command_id == DSD_APP_CMD_WAV_START);
    assert(view.mode == DSD_APP_RECORDING_MODE_PER_CALL && view.per_call == 1 && view.slot_files == 2);

    /* Refused (Signal search), then a failure count in the next view. */
    dsd_app_recording_command_submitted(DSD_APP_CMD_WAV_STOP);
    dsd_app_recording_command_resolved(DSD_APP_CMD_WAV_STOP, DSD_APP_RECORDING_COMMAND_REFUSED);
    (void)dsd_wav_recording_note_failure(DSD_WAV_RECORDING_ERROR_PUBLISH);
    dsd_app_telemetry_publish_opts_snapshot(&opts);
    assert(dsd_app_recording_view_get(&view) == 1);
    assert(view.command_state == DSD_APP_RECORDING_COMMAND_REFUSED && view.command_id == DSD_APP_CMD_WAV_STOP);
    assert(view.failures[DSD_WAV_RECORDING_ERROR_PUBLISH] == 1U && view.last_error == DSD_WAV_RECORDING_ERROR_PUBLISH);
    assert(view.mode == DSD_APP_RECORDING_MODE_PER_CALL);

    /* Dropped from the queue: shown without another publication. */
    dsd_app_recording_command_submitted(DSD_APP_CMD_WAV_STATIC_OPEN);
    dsd_app_recording_command_resolved(DSD_APP_CMD_WAV_STATIC_OPEN, DSD_APP_RECORDING_COMMAND_DROPPED);
    assert(dsd_app_recording_view_get(&view) == 1);
    assert(view.command_state == DSD_APP_RECORDING_COMMAND_DROPPED && view.command_id == DSD_APP_CMD_WAV_STATIC_OPEN);

    /* A new run's counts start clean. */
    dsd_wav_recording_session_reset();
    opts.dmr_stereo_wav = 0;
    opts.static_wav_file = 1;
    dsd_app_telemetry_publish_opts_snapshot(&opts);
    assert(dsd_app_recording_view_get(&view) == 1);
    assert(view.failures[DSD_WAV_RECORDING_ERROR_PUBLISH] == 0U && view.last_error == DSD_WAV_RECORDING_ERROR_NONE);
    assert(view.mode == DSD_APP_RECORDING_MODE_STATIC && view.slot_files == 0);
    assert(dsd_app_recording_view_get(NULL) == -1);
}

/* Issue #708: the view carries the recording journal's state: none without a journal, on while it is written, failed
   when it did not open (or a write failed), until the next run's journal opens. */
static void
test_publish_carries_the_journal_state(void) {
    static dsd_opts opts;
    dsd_app_recording_view view;
    DSD_MEMSET(&opts, 0, sizeof opts);

    assert(dsd_wav_journal_open(NULL) == 0);
    dsd_app_telemetry_publish_opts_snapshot(&opts);
    assert(dsd_app_recording_view_get(&view) == 1 && view.journal_state == DSD_APP_RECORDING_JOURNAL_NONE);

    char path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(path, sizeof path, "dsdneo_view_journal");
    assert(fd >= 0);
    (void)dsd_close(fd);
    assert(dsd_wav_journal_open(path) == 0);
    dsd_app_telemetry_publish_opts_snapshot(&opts);
    assert(dsd_app_recording_view_get(&view) == 1 && view.journal_state == DSD_APP_RECORDING_JOURNAL_ON);

    char missing[DSD_TEST_PATH_MAX * 2];
    assert(dsd_test_path_join(missing, sizeof missing, path, "no-such-folder/calls.jsonl") == 0);
    assert(dsd_wav_journal_open(missing) == -1);
    dsd_app_telemetry_publish_opts_snapshot(&opts);
    assert(dsd_app_recording_view_get(&view) == 1 && view.journal_state == DSD_APP_RECORDING_JOURNAL_FAILED);

    assert(dsd_wav_journal_open("") == 0);
    dsd_app_telemetry_publish_opts_snapshot(&opts);
    assert(dsd_app_recording_view_get(&view) == 1 && view.journal_state == DSD_APP_RECORDING_JOURNAL_NONE);
    (void)remove(path);
}

int
main(void) {
    test_initial_snapshot_is_absent();
    test_publish_copies_latest_options();
    test_republish_updates_stable_consumer_copy();
    test_publish_also_feeds_the_notification_record();
    test_publish_carries_the_recording_view();
    test_publish_carries_the_journal_state();
    printf("UI_OPTS_SNAPSHOT: OK\n");
    return 0;
}
