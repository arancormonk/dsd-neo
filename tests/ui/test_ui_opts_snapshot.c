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

#include <dsd-neo/app_control/notification_status.h>
#include <dsd-neo/app_control/snapshot.h>
#include "snapshot_internal.h"

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

int
main(void) {
    test_initial_snapshot_is_absent();
    test_publish_copies_latest_options();
    test_republish_updates_stable_consumer_copy();
    test_publish_also_feeds_the_notification_record();
    printf("UI_OPTS_SNAPSHOT: OK\n");
    return 0;
}
