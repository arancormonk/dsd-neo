// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/protocol/edacs/edacs_afs.h>
#include <dsd-neo/runtime/call_alert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

struct sf_private_tag;

static int g_beeper_count;
static int g_last_beeper_id;

struct sf_private_tag*
open_wav_file(char* dir, char* temp_filename, uint16_t sample_rate, uint8_t ext) {
    UNUSED(dir);
    UNUSED(temp_filename);
    UNUSED(sample_rate);
    UNUSED(ext);
    return NULL;
}

struct sf_private_tag*
close_and_rename_wav_file(struct sf_private_tag* wav_file, dsd_opts* opts, char* wav_out_filename, char* dir,
                          Event_History_I* event_struct) {
    UNUSED(wav_file);
    UNUSED(opts);
    UNUSED(wav_out_filename);
    UNUSED(dir);
    UNUSED(event_struct);
    return NULL;
}

void
dsd_frame_logf(dsd_opts* opts, const char* format, ...) {
    UNUSED(opts);
    UNUSED(format);
}

const char*
dsd_synctype_to_string(int synctype) {
    UNUSED(synctype);
    return "TEST";
}

int
getAfsString(dsd_state* state, char* buffer, int a, int f, int s) {
    UNUSED(state);
    return snprintf(buffer, 7, "%02d-%02d%01d", a, f, s);
}

void
getTimeN_buf(time_t t, char out[9]) {
    UNUSED(t);
    snprintf(out, 9, "00:00:00");
}

void
getDateN_buf(time_t t, char out[11]) {
    UNUSED(t);
    snprintf(out, 11, "2026-04-30");
}

void
// NOLINTNEXTLINE(bugprone-reserved-identifier)
__wrap_beeper(dsd_opts* opts, dsd_state* state, int lr, int id, int ad, int len) {
    UNUSED(opts);
    UNUSED(state);
    UNUSED(lr);
    UNUSED(ad);
    UNUSED(len);
    g_beeper_count++;
    g_last_beeper_id = id;
}

static void
reset_fixture(dsd_opts* opts, dsd_state* state, Event_History_I event_history[2]) {
    memset(opts, 0, sizeof *opts);
    memset(state, 0, sizeof *state);
    memset(event_history, 0, sizeof event_history[0] * 2);
    state->event_history_s = event_history;
    init_event_history(&state->event_history_s[0], 0, 255);
    init_event_history(&state->event_history_s[1], 0, 255);
    opts->call_alert = 1;
    g_beeper_count = 0;
    g_last_beeper_id = 0;
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
test_end_only_data_call_does_not_emit_voice_end_alert(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_END;

    watchdog_event_datacall(&opts, &state, 1234, 5678, "MNIS ARS;", 0);
    watchdog_event_history(&opts, &state, 0);

    return expect_int("end-only data call should not beep", g_beeper_count, 0);
}

static int
test_data_only_data_call_emits_one_data_alert(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_DATA;

    watchdog_event_datacall(&opts, &state, 1234, 5678, "MNIS ARS;", 0);
    watchdog_event_history(&opts, &state, 0);

    int rc = 0;
    rc |= expect_int("data-only data call should beep once", g_beeper_count, 1);
    rc |= expect_int("data-only data call should use data tone", g_last_beeper_id, 80);
    return rc;
}

static int
test_source_less_data_call_does_not_suppress_next_voice_start_alert(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START;

    watchdog_event_datacall(&opts, &state, 0, 0, "MNIS ARS;", 0);
    watchdog_event_history(&opts, &state, 0);

    int rc = expect_int("source-less data should not beep as voice start", g_beeper_count, 0);

    state.lastsrc = 1234;
    watchdog_event_history(&opts, &state, 0);

    rc |= expect_int("voice start after source-less data should beep once", g_beeper_count, 1);
    rc |= expect_int("voice start after source-less data should use voice tone", g_last_beeper_id, 40);
    return rc;
}

static int
test_voice_end_alert_still_emits_for_voice_history(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_END;
    state.event_history_s[0].Event_History_Items[0].source_id = 1234;
    state.event_history_s[0].Event_History_Items[0].subtype = 0;
    state.lastsrc = 0;

    watchdog_event_history(&opts, &state, 0);

    int rc = 0;
    rc |= expect_int("voice end should still beep once", g_beeper_count, 1);
    rc |= expect_int("voice end should use voice tone", g_last_beeper_id, 40);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_end_only_data_call_does_not_emit_voice_end_alert();
    rc |= test_data_only_data_call_emits_one_data_alert();
    rc |= test_source_less_data_call_does_not_suppress_next_voice_start_alert();
    rc |= test_voice_end_alert_still_emits_for_voice_history();

    if (rc == 0) {
        printf("CORE_CALL_ALERT_HISTORY: OK\n");
    }
    return rc;
}
