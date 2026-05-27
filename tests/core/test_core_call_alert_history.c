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
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

struct sf_private_tag;

static int g_beeper_count;
static int g_last_beeper_id;

struct sf_private_tag*
open_wav_file(char* dir, char* temp_filename, size_t temp_filename_size, uint16_t sample_rate, uint8_t ext) {
    UNUSED(dir);
    UNUSED(temp_filename);
    UNUSED(temp_filename_size);
    UNUSED(sample_rate);
    UNUSED(ext);
    return NULL;
}

struct sf_private_tag*
close_and_rename_wav_file(struct sf_private_tag* wav_file, const dsd_opts* opts, const char* wav_out_filename,
                          const char* dir, const Event_History_I* event_struct) {
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
getAfsString(const dsd_state* state, char* buffer, int a, int f, int s) {
    UNUSED(state);
    return DSD_SNPRINTF(buffer, 7, "%02d-%02d%01d", a, f, s);
}

void
getTimeN_buf(time_t t, char out[9]) {
    UNUSED(t);
    DSD_SNPRINTF(out, 9, "00:00:00");
}

void
getDateN_buf(time_t t, char out[11]) {
    UNUSED(t);
    DSD_SNPRINTF(out, 11, "2026-04-30");
}

void
// NOLINTNEXTLINE(bugprone-reserved-identifier,misc-use-internal-linkage)
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
    DSD_MEMSET(opts, 0, sizeof *opts);
    DSD_MEMSET(state, 0, sizeof *state);
    DSD_MEMSET(event_history, 0, sizeof event_history[0] * 2);
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
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_has_substr(const char* label, const char* haystack, const char* needle) {
    if (haystack == NULL || needle == NULL || strstr(haystack, needle) == NULL) {
        DSD_FPRINTF(stderr, "%s: missing '%s' in '%s'\n", label, needle ? needle : "<null>",
                    haystack ? haystack : "<null>");
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

static int
test_edacs_service_string_appends_past_pointer_size(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    opts.p25_is_tuned = 1;
    state.lastsynctype = DSD_SYNC_EDACS_POS;
    state.lastsrc = 1201;
    state.lasttg = 0x0123;
    state.edacs_tuned_lcn = 7;
    state.edacs_site_id = 3;
    state.edacs_area_code = 1;
    state.edacs_sys_id = 0x2A;
    state.edacs_vc_call_type = 0x0A;
    state.edacs_a_shift = 7;
    state.edacs_f_shift = 3;
    state.edacs_a_mask = 0x0F;
    state.edacs_f_mask = 0x0F;
    state.edacs_s_mask = 0x07;

    watchdog_event_current(&opts, &state, 0);

    const Event_History* item = &state.event_history_s[0].Event_History_Items[0];
    int rc = 0;
    rc |= expect_has_substr("edacs sysid service suffix", item->sysid_string, "EDACS_SITE_003_Digital_Group_Call");
    rc |= expect_has_substr("edacs event service suffix", item->event_string, "Digital Group Call;");
    return rc;
}

static int
test_dmr_event_string_keeps_full_prefix_after_sprintf_hardening(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.lastsrc = 123456U;
    state.lasttg = 50061U;
    state.gi[0] = 0;
    state.dmr_color_code = 7U;
    state.dmr_t3_syscode = 0xABCU;

    watchdog_event_current(&opts, &state, 0);

    const Event_History* item = &state.event_history_s[0].Event_History_Items[0];
    int rc = 0;
    rc |= expect_has_substr("dmr event date/time prefix", item->event_string, "2026-04-30 00:00:00");
    rc |= expect_has_substr("dmr event voice prefix", item->event_string,
                            "TEST TGT: 00050061; SRC: 00123456; CC: 07; SYS: ABC;");
    rc |= expect_has_substr("dmr event call class", item->event_string, "Group;");
    return rc;
}

static int
test_p25_event_string_keeps_full_prefix_after_sprintf_hardening(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    state.lastsynctype = DSD_SYNC_P25P2_POS;
    state.lastsrc = 5790062U;
    state.lasttg = 50061U;
    state.gi[0] = 0;
    state.nac = 0x293;
    state.p2_wacn = 0x45564U;
    state.p2_sysid = 0x006U;
    state.p2_rfssid = 10U;
    state.p2_siteid = 10U;

    watchdog_event_current(&opts, &state, 0);

    const Event_History* item = &state.event_history_s[0].Event_History_Items[0];
    int rc = 0;
    rc |= expect_has_substr("p25 event date/time prefix", item->event_string, "2026-04-30 00:00:00");
    rc |= expect_has_substr("p25 event voice prefix", item->event_string,
                            "TEST TGT: 00050061; SRC: 05790062; NAC: 293; NET_STS: 45564:006:10.10;");
    rc |= expect_has_substr("p25 event call class", item->event_string, "Group;");
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_end_only_data_call_does_not_emit_voice_end_alert();
    rc |= test_data_only_data_call_emits_one_data_alert();
    rc |= test_source_less_data_call_does_not_suppress_next_voice_start_alert();
    rc |= test_voice_end_alert_still_emits_for_voice_history();
    rc |= test_edacs_service_string_appends_past_pointer_size();
    rc |= test_dmr_event_string_keeps_full_prefix_after_sprintf_hardening();
    rc |= test_p25_event_string_keeps_full_prefix_after_sprintf_hardening();

    if (rc == 0) {
        printf("CORE_CALL_ALERT_HISTORY: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
