// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static dsd_input_level_snapshot
snapshot_for(dsd_input_level_source source, double rms_dbfs, double peak_dbfs, double clip_pct, time_t updated) {
    dsd_input_level_snapshot snapshot = {
        .status = DSD_INPUT_LEVEL_UNKNOWN,
        .source = source,
        .rms_dbfs = rms_dbfs,
        .peak_dbfs = peak_dbfs,
        .clip_pct = clip_pct,
        .sample_count = 1000U,
        .updated = updated,
    };
    return snapshot;
}

static void
test_classifier_thresholds(void) {
    dsd_input_level_snapshot low = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -50.0, -20.0, 0.0, 1);
    dsd_input_level_classify(&low, -40.0);
    assert(low.status == DSD_INPUT_LEVEL_LOW);

    dsd_input_level_snapshot hot = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -20.0, -0.5, 0.0, 1);
    dsd_input_level_classify(&hot, -40.0);
    assert(hot.status == DSD_INPUT_LEVEL_HOT);

    dsd_input_level_snapshot clip = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -20.0, -0.5, 0.1, 1);
    dsd_input_level_classify(&clip, -40.0);
    assert(clip.status == DSD_INPUT_LEVEL_CLIPPING);

    dsd_input_level_snapshot ok = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -25.0, -3.0, 0.0, 1);
    dsd_input_level_classify(&ok, -40.0);
    assert(ok.status == DSD_INPUT_LEVEL_OK);
}

static void
test_pcm_publish_backfills_rtl_power_but_rf_publish_does_not(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts != NULL);
    assert(state != NULL);
    opts->input_warn_db = -40.0;
    opts->input_warn_cooldown_sec = 10;
    opts->rtl_pwr = 0.125;

    dsd_input_level_snapshot rf_clip = snapshot_for(DSD_INPUT_LEVEL_SOURCE_RTL_CU8, -6.0, -0.1, 0.2, 50);
    dsd_input_level_publish(opts, state, &rf_clip, DSD_INPUT_LEVEL_NOTIFY_RF);
    assert(opts->rtl_pwr == 0.125);
    assert(state->input_level.source == DSD_INPUT_LEVEL_SOURCE_RTL_CU8);
    assert(state->input_level.status == DSD_INPUT_LEVEL_CLIPPING);

    dsd_input_level_snapshot pcm_ok = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -20.0, -3.0, 0.0, 60);
    dsd_input_level_publish(opts, state, &pcm_ok, DSD_INPUT_LEVEL_NOTIFY_ALL);
    assert(fabs(opts->rtl_pwr - 0.01) < 0.000001);

    free(state);
    free(opts);
}

static void
test_toast_throttle_and_transition(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts != NULL);
    assert(state != NULL);
    opts->input_warn_db = -40.0;
    opts->input_warn_cooldown_sec = 10;

    dsd_input_level_snapshot low = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -50.0, -20.0, 0.0, 100);
    dsd_input_level_publish(opts, state, &low, DSD_INPUT_LEVEL_NOTIFY_ALL);
    assert(state->input_level.status == DSD_INPUT_LEVEL_LOW);
    assert(strstr(state->ui_msg, "Input Level LOW") != NULL);
    assert(strstr(state->ui_msg, "raise source/input volume") != NULL);
    assert(state->input_level_last_toast_time == 100);

    DSD_SNPRINTF(state->ui_msg, sizeof(state->ui_msg), "%s", "sentinel");
    low.updated = 105;
    dsd_input_level_publish(opts, state, &low, DSD_INPUT_LEVEL_NOTIFY_ALL);
    assert(strcmp(state->ui_msg, "sentinel") == 0);

    low.updated = 111;
    dsd_input_level_publish(opts, state, &low, DSD_INPUT_LEVEL_NOTIFY_ALL);
    assert(strstr(state->ui_msg, "Input Level LOW") != NULL);
    assert(state->input_level_last_toast_time == 111);

    dsd_input_level_snapshot clip = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -20.0, -0.2, 0.2, 112);
    dsd_input_level_publish(opts, state, &clip, DSD_INPUT_LEVEL_NOTIFY_ALL);
    assert(strstr(state->ui_msg, "Input Level CLIP") != NULL);
    assert(state->input_level_last_toast_time == 112);

    free(state);
    free(opts);
}

static void
test_toast_cooldown_survives_ok_sample(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts != NULL);
    assert(state != NULL);
    opts->input_warn_db = -40.0;
    opts->input_warn_cooldown_sec = 10;

    dsd_input_level_snapshot low = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -50.0, -20.0, 0.0, 100);
    dsd_input_level_publish(opts, state, &low, DSD_INPUT_LEVEL_NOTIFY_ALL);
    assert(strstr(state->ui_msg, "Input Level LOW") != NULL);
    assert(state->input_level_last_toast_time == 100);

    dsd_input_level_snapshot ok = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -30.0, -3.0, 0.0, 105);
    dsd_input_level_publish(opts, state, &ok, DSD_INPUT_LEVEL_NOTIFY_ALL);
    assert(state->input_level.status == DSD_INPUT_LEVEL_OK);

    DSD_SNPRINTF(state->ui_msg, sizeof(state->ui_msg), "%s", "sentinel");
    low.updated = 106;
    dsd_input_level_publish(opts, state, &low, DSD_INPUT_LEVEL_NOTIFY_ALL);
    assert(strcmp(state->ui_msg, "sentinel") == 0);
    assert(state->input_level_last_toast_time == 100);

    low.updated = 111;
    dsd_input_level_publish(opts, state, &low, DSD_INPUT_LEVEL_NOTIFY_ALL);
    assert(strstr(state->ui_msg, "Input Level LOW") != NULL);
    assert(state->input_level_last_toast_time == 111);

    free(state);
    free(opts);
}

static void
test_toast_escalation_survives_ok_sample(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts != NULL);
    assert(state != NULL);
    opts->input_warn_db = -40.0;
    opts->input_warn_cooldown_sec = 10;

    dsd_input_level_snapshot low = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -50.0, -20.0, 0.0, 100);
    dsd_input_level_publish(opts, state, &low, DSD_INPUT_LEVEL_NOTIFY_ALL);
    assert(strstr(state->ui_msg, "Input Level LOW") != NULL);
    assert(state->input_level_last_toast_time == 100);

    dsd_input_level_snapshot ok = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -30.0, -3.0, 0.0, 105);
    dsd_input_level_publish(opts, state, &ok, DSD_INPUT_LEVEL_NOTIFY_ALL);
    assert(state->input_level.status == DSD_INPUT_LEVEL_OK);

    dsd_input_level_snapshot clip = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -20.0, -0.2, 0.2, 106);
    dsd_input_level_publish(opts, state, &clip, DSD_INPUT_LEVEL_NOTIFY_ALL);
    assert(strstr(state->ui_msg, "Input Level CLIP") != NULL);
    assert(state->input_level_last_toast_time == 106);
    assert(state->input_level_last_toast_status == DSD_INPUT_LEVEL_CLIPPING);

    free(state);
    free(opts);
}

static void
test_toast_cooldown_suppresses_hot_clip_oscillation(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts != NULL);
    assert(state != NULL);
    opts->input_warn_db = -40.0;
    opts->input_warn_cooldown_sec = 10;

    dsd_input_level_snapshot hot = snapshot_for(DSD_INPUT_LEVEL_SOURCE_RTL_CU8, -15.0, -0.5, 0.0, 200);
    dsd_input_level_publish(opts, state, &hot, DSD_INPUT_LEVEL_NOTIFY_RF);
    assert(strstr(state->ui_msg, "RF Level HOT") != NULL);
    assert(state->input_level_last_toast_time == 200);
    assert(state->input_level_last_toast_status == DSD_INPUT_LEVEL_HOT);

    dsd_input_level_snapshot clip = snapshot_for(DSD_INPUT_LEVEL_SOURCE_RTL_CU8, -15.0, -0.1, 0.2, 201);
    dsd_input_level_publish(opts, state, &clip, DSD_INPUT_LEVEL_NOTIFY_RF);
    assert(strstr(state->ui_msg, "RF Level CLIP") != NULL);
    assert(state->input_level_last_toast_time == 201);
    assert(state->input_level_last_toast_status == DSD_INPUT_LEVEL_CLIPPING);

    DSD_SNPRINTF(state->ui_msg, sizeof(state->ui_msg), "%s", "sentinel");
    hot.updated = 202;
    dsd_input_level_publish(opts, state, &hot, DSD_INPUT_LEVEL_NOTIFY_RF);
    assert(state->input_level.status == DSD_INPUT_LEVEL_HOT);
    assert(strcmp(state->ui_msg, "sentinel") == 0);
    assert(state->input_level_last_toast_time == 201);

    clip.updated = 203;
    dsd_input_level_publish(opts, state, &clip, DSD_INPUT_LEVEL_NOTIFY_RF);
    assert(state->input_level.status == DSD_INPUT_LEVEL_CLIPPING);
    assert(strcmp(state->ui_msg, "sentinel") == 0);
    assert(state->input_level_last_toast_time == 201);

    free(state);
    free(opts);
}

static void
test_rf_low_suppressed_but_clip_notifies(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts != NULL);
    assert(state != NULL);
    opts->input_warn_db = -40.0;
    opts->input_warn_cooldown_sec = 10;

    dsd_input_level_snapshot low = snapshot_for(DSD_INPUT_LEVEL_SOURCE_RTL_CU8, -80.0, -20.0, 0.0, 200);
    dsd_input_level_publish(opts, state, &low, DSD_INPUT_LEVEL_NOTIFY_RF);
    assert(state->input_level.status == DSD_INPUT_LEVEL_LOW);
    assert(state->ui_msg[0] == '\0');

    dsd_input_level_snapshot clip = snapshot_for(DSD_INPUT_LEVEL_SOURCE_RTL_CU8, -15.0, -0.1, 0.2, 201);
    dsd_input_level_publish(opts, state, &clip, DSD_INPUT_LEVEL_NOTIFY_RF);
    assert(strstr(state->ui_msg, "RF Level CLIP") != NULL);
    assert(strstr(state->ui_msg, "lower RF gain or add filtering/attenuation") != NULL);

    free(state);
    free(opts);
}

static void
test_tcp_silence_low_updates_state_without_toast(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts != NULL);
    assert(state != NULL);
    opts->audio_in_type = AUDIO_IN_TCP;
    opts->input_warn_db = -40.0;
    opts->input_warn_cooldown_sec = 10;

    DSD_SNPRINTF(state->ui_msg, sizeof(state->ui_msg), "%s", "sentinel");
    dsd_input_level_snapshot silent = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -120.0, -120.0, 0.0, 300);
    dsd_input_level_publish(opts, state, &silent, DSD_INPUT_LEVEL_NOTIFY_ALL);
    assert(state->input_level.status == DSD_INPUT_LEVEL_LOW);
    assert(state->input_level.source == DSD_INPUT_LEVEL_SOURCE_PCM);
    assert(strcmp(state->ui_msg, "sentinel") == 0);
    assert(state->input_level_last_toast_time == 0);

    free(state);
    free(opts);
}

static void
test_tcp_low_non_silent_still_notifies(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts != NULL);
    assert(state != NULL);
    opts->audio_in_type = AUDIO_IN_TCP;
    opts->input_warn_db = -40.0;
    opts->input_warn_cooldown_sec = 10;

    dsd_input_level_snapshot quiet = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -55.0, -30.0, 0.0, 310);
    dsd_input_level_publish(opts, state, &quiet, DSD_INPUT_LEVEL_NOTIFY_ALL);
    assert(state->input_level.status == DSD_INPUT_LEVEL_LOW);
    assert(strstr(state->ui_msg, "Input Level LOW") != NULL);
    assert(strstr(state->ui_msg, "if signal is present") != NULL);
    assert(state->input_level_last_toast_time == 310);

    free(state);
    free(opts);
}

static void
test_tcp_silence_clip_still_notifies(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts != NULL);
    assert(state != NULL);
    opts->audio_in_type = AUDIO_IN_TCP;
    opts->input_warn_db = -40.0;
    opts->input_warn_cooldown_sec = 10;

    dsd_input_level_snapshot clip = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -120.0, -120.0, 0.2, 320);
    dsd_input_level_publish(opts, state, &clip, DSD_INPUT_LEVEL_NOTIFY_ALL);
    assert(state->input_level.status == DSD_INPUT_LEVEL_CLIPPING);
    assert(strstr(state->ui_msg, "Input Level CLIP") != NULL);
    assert(state->input_level_last_toast_time == 320);

    free(state);
    free(opts);
}

static void
test_non_tcp_silence_low_still_notifies(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts != NULL);
    assert(state != NULL);
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->input_warn_db = -40.0;
    opts->input_warn_cooldown_sec = 10;

    dsd_input_level_snapshot silent = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -120.0, -120.0, 0.0, 330);
    dsd_input_level_publish(opts, state, &silent, DSD_INPUT_LEVEL_NOTIFY_ALL);
    assert(state->input_level.status == DSD_INPUT_LEVEL_LOW);
    assert(strstr(state->ui_msg, "Input Level LOW") != NULL);
    assert(state->input_level_last_toast_time == 330);

    free(state);
    free(opts);
}

static void
test_advisory_text_selection(void) {
    char msg[128];
    dsd_input_level_snapshot pcm_low = snapshot_for(DSD_INPUT_LEVEL_SOURCE_PCM, -55.0, -30.0, 0.0, 1);
    dsd_input_level_classify(&pcm_low, -40.0);
    assert(dsd_input_level_format_advisory(&pcm_low, msg, sizeof(msg)) == 0);
    assert(strstr(msg, "raise source/input volume") != NULL);

    dsd_input_level_snapshot rf_clip = snapshot_for(DSD_INPUT_LEVEL_SOURCE_SOAPY_CF32, -5.0, -0.1, 0.2, 1);
    dsd_input_level_classify(&rf_clip, -40.0);
    assert(dsd_input_level_format_advisory(&rf_clip, msg, sizeof(msg)) == 0);
    assert(strstr(msg, "lower RF gain or add filtering/attenuation") != NULL);
}

int
main(void) {
    test_classifier_thresholds();
    test_pcm_publish_backfills_rtl_power_but_rf_publish_does_not();
    test_toast_throttle_and_transition();
    test_toast_cooldown_survives_ok_sample();
    test_toast_escalation_survives_ok_sample();
    test_toast_cooldown_suppresses_hot_clip_oscillation();
    test_rf_low_suppressed_but_clip_notifies();
    test_tcp_silence_low_updates_state_without_toast();
    test_tcp_low_non_silent_still_notifies();
    test_tcp_silence_clip_still_notifies();
    test_non_tcp_silence_low_still_notifies();
    test_advisory_text_selection();
    return 0;
}
