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
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/platform/sndfile_fwd.h>
#include <dsd-neo/protocol/edacs/edacs_afs.h>
#include <dsd-neo/runtime/call_alert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int g_beeper_count;
static int g_last_beeper_id;
static int g_frame_log_count;
static char g_last_frame_log[512];
static int g_open_wav_count;
static int g_close_wav_count;

enum {
    TEST_DMR_DATA_BURST = 6,
};

SNDFILE*
open_wav_file(char* dir, char* temp_filename, size_t temp_filename_size, uint16_t sample_rate, uint8_t ext) {
    UNUSED(dir);
    UNUSED(temp_filename);
    UNUSED(temp_filename_size);
    UNUSED(sample_rate);
    UNUSED(ext);
    g_open_wav_count++;
    return NULL;
}

SNDFILE*
close_and_rename_wav_file(SNDFILE* wav_file, const dsd_opts* opts, const char* wav_out_filename, const char* dir,
                          const Event_History_I* event_struct) {
    UNUSED(wav_file);
    UNUSED(opts);
    UNUSED(wav_out_filename);
    UNUSED(dir);
    UNUSED(event_struct);
    g_close_wav_count++;
    return NULL;
}

void
dsd_frame_logf(dsd_opts* opts, const char* format, ...) {
    UNUSED(opts);
    g_frame_log_count++;
    va_list ap;
    va_start(ap, format);
    (void)DSD_VSNPRINTF(g_last_frame_log, sizeof g_last_frame_log, format, ap);
    va_end(ap);
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

int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    UNUSED(timestamp);
    const char* value = (format == DSD_LOCAL_DATETIME_DATE_HYPHEN) ? "2026-04-30" : "00:00:00";
    DSD_SNPRINTF(out, out_size, "%s", value);
    return 1;
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
    g_frame_log_count = 0;
    g_last_frame_log[0] = '\0';
    g_open_wav_count = 0;
    g_close_wav_count = 0;
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
expect_no_substr(const char* label, const char* haystack, const char* needle) {
    if (haystack != NULL && needle != NULL && strstr(haystack, needle) != NULL) {
        DSD_FPRINTF(stderr, "%s: unexpected '%s' in '%s'\n", label, needle, haystack);
        return 1;
    }
    return 0;
}

static int
expect_str_eq(const char* label, const char* got, const char* want) {
    if (got == NULL || want == NULL || strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", label, got ? got : "<null>", want ? want : "<null>");
        return 1;
    }
    return 0;
}

static int
append_policy_label(dsd_state* state, uint32_t id, const char* mode, const char* name) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_exact_entry(id, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return -1;
    }
    return dsd_tg_policy_append_exact(state, &row);
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
test_data_call_emits_frame_log_record(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    watchdog_event_datacall(&opts, &state, 1234, 5678, "MNIS ARS;", 0);

    int rc = 0;
    rc |= expect_int("data call should emit one frame log", g_frame_log_count, 1);
    rc |= expect_has_substr("data call frame log should identify data", g_last_frame_log, "FRAME DATA slot=1");
    rc |= expect_has_substr("data call frame log should keep source", g_last_frame_log, "src=1234");
    rc |= expect_has_substr("data call frame log should keep target", g_last_frame_log, "dst=5678");
    return rc;
}

static int
test_status_event_is_not_data_call_or_frame_log(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_DATA;

    watchdog_event_status(&state, "DSD-neo Started and Event History Initialized;", 0);

    const Event_History* current = &state.event_history_s[0].Event_History_Items[0];
    int rc = 0;
    rc |= expect_has_substr("status current should include message", current->event_string, "DSD-neo Started");
    rc |= expect_int("status source remains zero", (int)current->source_id, 0);
    rc |= expect_int("status target remains zero", (int)current->target_id, 0);
    rc |= expect_int("status subtype remains neutral", (int)current->subtype, -1);
    rc |= expect_int("status systype remains neutral", (int)current->systype, -1);
    rc |= expect_int("status event time should be set", current->event_time > 0 ? 1 : 0, 1);
    rc |= expect_int("status should not emit frame log", g_frame_log_count, 0);
    rc |= expect_int("status should not emit data alert", g_beeper_count, 0);

    push_event_history(&state.event_history_s[0]);
    init_event_history(&state.event_history_s[0], 0, 1);
    const Event_History* stored = &state.event_history_s[0].Event_History_Items[1];
    rc |= expect_has_substr("status can be stored in history", stored->event_string, "DSD-neo Started");
    rc |= expect_int("stored status subtype remains neutral", (int)stored->subtype, -1);
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
test_source_less_data_call_is_preserved_in_history(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    watchdog_event_datacall(&opts, &state, 0, 0, "MNIS ARS;", 0);
    watchdog_event_history(&opts, &state, 0);

    const Event_History* current = &state.event_history_s[0].Event_History_Items[0];
    const Event_History* stored = &state.event_history_s[0].Event_History_Items[1];
    int rc = 0;
    rc |= expect_int("source-less data current should be cleared", current->event_string[0], '\0');
    rc |= expect_int("source-less data source remains zero", (int)stored->source_id, 0);
    rc |= expect_has_substr("source-less data should be stored", stored->event_string, "MNIS ARS;");
    return rc;
}

static int
test_source_less_dmr_data_current_event_is_not_preserved_in_history(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.dmr_color_code = 1U;

    state.dmrburstL = TEST_DMR_DATA_BURST;
    watchdog_event_current(&opts, &state, 0);
    watchdog_event_history(&opts, &state, 0);

    state.dmrburstR = TEST_DMR_DATA_BURST;
    watchdog_event_current(&opts, &state, 1);
    watchdog_event_history(&opts, &state, 1);

    int rc = 0;
    rc |= expect_int("slot 1 source-less DMR current should remain current",
                     state.event_history_s[0].Event_History_Items[0].event_string[0] != '\0', 1);
    rc |= expect_int("slot 1 source-less DMR current should not be stored",
                     state.event_history_s[0].Event_History_Items[1].event_string[0], '\0');
    rc |= expect_int("slot 2 source-less DMR current should remain current",
                     state.event_history_s[1].Event_History_Items[0].event_string[0] != '\0', 1);
    rc |= expect_int("slot 2 source-less DMR current should not be stored",
                     state.event_history_s[1].Event_History_Items[1].event_string[0], '\0');
    return rc;
}

static int
test_sourced_dmr_data_current_event_does_not_emit_voice_end_alert(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_END;

    state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.dmr_color_code = 1U;

    state.lastsrc = 1234;
    state.lasttg = 5678;
    state.dmrburstL = TEST_DMR_DATA_BURST;
    watchdog_event_current(&opts, &state, 0);
    state.lastsrc = 0;
    watchdog_event_history(&opts, &state, 0);

    int rc = 0;
    rc |= expect_int("slot 1 sourced DMR data end should not beep", g_beeper_count, 0);
    rc |= expect_int("slot 1 sourced DMR data should be stored",
                     state.event_history_s[0].Event_History_Items[1].event_string[0] != '\0', 1);

    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_END;
    state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.dmr_color_code = 1U;

    state.lastsrcR = 2345;
    state.lasttgR = 6789;
    state.dmrburstR = TEST_DMR_DATA_BURST;
    watchdog_event_current(&opts, &state, 1);
    state.lastsrcR = 0;
    watchdog_event_history(&opts, &state, 1);

    rc |= expect_int("slot 2 sourced DMR data end should not beep", g_beeper_count, 0);
    rc |= expect_int("slot 2 sourced DMR data should be stored",
                     state.event_history_s[1].Event_History_Items[1].event_string[0] != '\0', 1);
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

    opts.trunk_is_tuned = 1;
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

static int
test_source_less_current_event_updates_history_metadata(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    state.lastsynctype = DSD_SYNC_P25P2_POS;
    state.lastsrc = 0U;
    state.lasttg = 21001U;
    state.gi[0] = 0;
    state.nac = 0x006;
    state.p2_wacn = 0x45564U;
    state.p2_sysid = 0x006U;
    state.p2_rfssid = 10U;
    state.p2_siteid = 10U;

    watchdog_event_current(&opts, &state, 0);

    const Event_History* item = &state.event_history_s[0].Event_History_Items[0];
    int rc = 0;
    rc |= expect_int("source-less current source remains zero", (int)item->source_id, 0);
    rc |= expect_int("source-less current target should update", (int)item->target_id, 21001);
    rc |= expect_int("source-less current event time should update", item->event_time > 0 ? 1 : 0, 1);
    rc |=
        expect_has_substr("source-less current string should include source zero", item->event_string, "SRC: 00000000");
    return rc;
}

static int
test_event_log_writes_optional_metadata_lines(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    char path[] = "/tmp/dsd-neo-events-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        DSD_FPRINTF(stderr, "mkstemp failed for event log test\n");
        return 1;
    }
    close(fd);
    remove(path);
    DSD_SNPRINTF(opts.event_out_file, sizeof opts.event_out_file, "%s", path);

    DSD_SNPRINTF(state.event_history_s[1].Event_History_Items[0].text_message,
                 sizeof state.event_history_s[1].Event_History_Items[0].text_message, "%s", "hello text");
    DSD_SNPRINTF(state.event_history_s[1].Event_History_Items[0].alias,
                 sizeof state.event_history_s[1].Event_History_Items[0].alias, "%s", "Unit 7");
    DSD_SNPRINTF(state.event_history_s[1].Event_History_Items[0].gps_s,
                 sizeof state.event_history_s[1].Event_History_Items[0].gps_s, "%s", "41.500000 -87.250000");
    DSD_SNPRINTF(state.event_history_s[1].Event_History_Items[0].internal_str,
                 sizeof state.event_history_s[1].Event_History_Items[0].internal_str, "%s", "status detail");

    char event_string[] = "2026-04-30 00:00:00 TEST EVENT;";
    write_event_to_log_file(&opts, &state, 1, 1, event_string);

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        remove(path);
        DSD_FPRINTF(stderr, "event log was not created\n");
        return 1;
    }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    remove(path);
    buf[n] = '\0';

    int rc = 0;
    rc |= expect_has_substr("event log main line", buf, "TEST EVENT; Slot 2;");
    rc |= expect_has_substr("event log text", buf, "hello text");
    rc |= expect_has_substr("event log alias", buf, "Talker Alias: Unit 7");
    rc |= expect_has_substr("event log gps", buf, "GPS: 41.500000 -87.250000");
    rc |= expect_has_substr("event log internal", buf, "DSD-neo: status detail");
    return rc;
}

static int
test_source_transition_rotates_slot_wav_files(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    opts.wav_out_f = (SNDFILE*)0x1;
    opts.wav_out_fR = (SNDFILE*)0x2;
    state.event_history_s[0].Event_History_Items[0].source_id = 1234U;
    DSD_SNPRINTF(state.event_history_s[0].Event_History_Items[0].event_string,
                 sizeof state.event_history_s[0].Event_History_Items[0].event_string, "%s", "slot one voice");
    state.lastsrc = 5678U;
    watchdog_event_history(&opts, &state, 0);

    int rc = 0;
    rc |= expect_int("slot 1 wav close", g_close_wav_count, 1);
    rc |= expect_int("slot 1 wav reopen", g_open_wav_count, 1);
    rc |= expect_has_substr("slot 1 transition stored", state.event_history_s[0].Event_History_Items[1].event_string,
                            "slot one voice");

    g_open_wav_count = 0;
    g_close_wav_count = 0;
    state.event_history_s[1].Event_History_Items[0].source_id = 2222U;
    DSD_SNPRINTF(state.event_history_s[1].Event_History_Items[0].event_string,
                 sizeof state.event_history_s[1].Event_History_Items[0].event_string, "%s", "slot two voice");
    state.lastsrcR = 3333U;
    watchdog_event_history(&opts, &state, 1);

    rc |= expect_int("slot 2 wav close", g_close_wav_count, 1);
    rc |= expect_int("slot 2 wav reopen", g_open_wav_count, 1);
    rc |= expect_has_substr("slot 2 transition stored", state.event_history_s[1].Event_History_Items[1].event_string,
                            "slot two voice");
    return rc;
}

static int
test_ysf_current_sanitizes_ids_and_text_message(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    state.lastsynctype = DSD_SYNC_YSF_POS;
    DSD_MEMSET(state.ysf_src, ' ', sizeof state.ysf_src);
    DSD_MEMSET(state.ysf_tgt, ' ', sizeof state.ysf_tgt);
    DSD_MEMCPY(state.ysf_src,
               "SRC\x01"
               "CALL",
               8);
    DSD_MEMCPY(state.ysf_tgt, "TG*ROOM", 7);
    for (int i = 4; i < 8; i++) {
        for (int j = 0; j < 20; j++) {
            state.ysf_txt[i][j] = (j % 2 == 0) ? '*' : (char)('A' + i);
        }
    }

    watchdog_event_current(&opts, &state, 0);

    const Event_History* item = &state.event_history_s[0].Event_History_Items[0];
    int rc = 0;
    rc |= expect_str_eq("ysf sysid", item->sysid_string, "YSF");
    rc |= expect_has_substr("ysf sanitized source", item->src_str, "SRC_CALL");
    rc |= expect_has_substr("ysf target", item->tgt_str, "TG*ROOM");
    rc |= expect_has_substr("ysf event target", item->event_string, "TGT: TG*ROOM");
    rc |= expect_has_substr("ysf text star becomes space", item->text_message, " E E");
    return rc;
}

static int
test_m17_dstar_dpmr_current_strings(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    state.lastsynctype = DSD_SYNC_M17_LSF_POS;
    state.m17_dst = 0xFFFFFFFFFFFFULL;
    state.m17_src = 12345ULL;
    state.m17_can = 4U;
    DSD_SNPRINTF(state.m17_src_csd, sizeof state.m17_src_csd, "%s", "SRC CSD");
    DSD_SNPRINTF(state.m17_dst_csd, sizeof state.m17_dst_csd, "%s", "DST CSD");
    DSD_SNPRINTF(state.m17_src_str, sizeof state.m17_src_str, "%s", "SRCSTR");
    DSD_SNPRINTF(state.m17_dst_str, sizeof state.m17_dst_str, "%s", "DSTSTR");
    watchdog_event_current(&opts, &state, 0);

    int rc = 0;
    const Event_History* item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_str_eq("m17 src csd", item->src_str, "SRC CSD");
    rc |= expect_has_substr("m17 broadcast event", item->event_string, "TGT: BROADCAST SRC: SRCSTR CAN: 04;");

    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_DSTAR_VOICE_POS;
    DSD_MEMSET(state.dstar_src, ' ', sizeof state.dstar_src);
    DSD_MEMSET(state.dstar_dst, ' ', sizeof state.dstar_dst);
    DSD_MEMCPY(state.dstar_src, "N0CALL\x02/RPT", 11);
    DSD_MEMCPY(state.dstar_dst, "CQCQCQ", 6);
    watchdog_event_current(&opts, &state, 0);
    item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_str_eq("dstar sysid", item->sysid_string, "DSTAR");
    rc |= expect_has_substr("dstar sanitized source", item->src_str, "N0CALL_/RPT");
    rc |= expect_has_substr("dstar event", item->event_string, "TGT: CQCQCQ");

    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_DPMR_FS2_POS;
    state.dpmr_color_code = 9U;
    DSD_SNPRINTF(state.dpmr_caller_id, sizeof state.dpmr_caller_id, "%s", "CALLER7");
    DSD_SNPRINTF(state.dpmr_target_id, sizeof state.dpmr_target_id, "%s", "TARGET9");
    state.dPMRVoiceFS2Frame.Version[0] = 3U;
    watchdog_event_current(&opts, &state, 0);
    item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_str_eq("dpmr sysid", item->sysid_string, "DPMR_CC_9");
    rc |= expect_has_substr("dpmr event ids", item->event_string, "CC: 09; TGT: TARGET9; SRC: CALLER7;");
    rc |= expect_has_substr("dpmr scrambler", item->event_string, "Scrambler Enc;");
    return rc;
}

static int
test_nxdn_current_includes_channel_encryption_and_policy_labels(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    state.lastsynctype = DSD_SYNC_NXDN_POS;
    state.gi[0] = 1;
    state.nxdn_last_rid = 41001U;
    state.nxdn_last_tg = 51002U;
    state.nxdn_last_ran = 23U;
    state.nxdn_location_site_code = 5U;
    state.nxdn_location_sys_code = 12U;
    state.nxdn_cipher_type = 3U;
    state.nxdn_key = 0x2AU;
    state.nxdn_grant_chan = 198U;
    state.nxdn_grant_freq = 453212500U;
    if (append_policy_label(&state, 51002U, "D", "Dispatch") != 0
        || append_policy_label(&state, 41001U, "A", "Unit 41001") != 0) {
        DSD_FPRINTF(stderr, "failed to append NXDN policy labels\n");
        return 1;
    }

    watchdog_event_current(&opts, &state, 0);

    const Event_History* item = &state.event_history_s[0].Event_History_Items[0];
    int rc = 0;
    rc |= expect_str_eq("nxdn sysid", item->sysid_string, "NXDN_12_5_RAN_23");
    rc |= expect_has_substr("nxdn channel freq", item->event_string, "CH: 198; FREQ: 453.212500 MHz;");
    rc |= expect_has_substr("nxdn encryption", item->event_string, "ENC; ALG: 3; KID: 2A;");
    rc |= expect_has_substr("nxdn private", item->event_string, "Private;");
    rc |= expect_has_substr("nxdn target label", item->event_string, "TName: Dispatch; Mode: D;");
    rc |= expect_has_substr("nxdn source label", item->event_string, "SName: Unit 41001; Mode: A;");
    return rc;
}

static int
test_edacs_ea_mode_current_event_and_unknown_lid(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    opts.trunk_is_tuned = 1;
    state.lastsynctype = DSD_SYNC_EDACS_POS;
    state.lastsrc = 0x800U;
    state.lasttg = 0x0123U;
    state.edacs_tuned_lcn = 11U;
    state.edacs_site_id = 12U;
    state.edacs_area_code = 3U;
    state.edacs_sys_id = 0x45U;
    state.edacs_vc_call_type = 0x01U;
    state.edacs_a_shift = 7;
    state.edacs_f_shift = 3;
    state.edacs_a_mask = 0x0F;
    state.edacs_f_mask = 0x0F;
    state.edacs_s_mask = 0x07;

    watchdog_event_current(&opts, &state, 0);
    int rc = 0;
    rc |= expect_has_substr("edacs unknown lid", state.event_history_s[0].Event_History_Items[0].event_string,
                            "LID: __UNK;");

    reset_fixture(&opts, &state, event_history);
    opts.trunk_is_tuned = 1;
    state.lastsynctype = DSD_SYNC_EDACS_POS;
    state.ea_mode = 1;
    state.lastsrc = 77001U;
    state.lasttg = 88002U;
    state.edacs_tuned_lcn = 12U;
    state.edacs_site_id = 7U;
    state.edacs_area_code = 2U;
    state.edacs_sys_id = 0x1234U;
    state.edacs_vc_call_type = 0x48U;

    watchdog_event_current(&opts, &state, 0);
    const Event_History* item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_has_substr("edacs ea target", item->event_string, "TGT: 0088002; SRC: 0077001;");
    rc |= expect_has_substr("edacs ea site", item->event_string, "SITE: 7:2.1234;");
    rc |= expect_has_substr("edacs ea flags", item->event_string, "Analog Group INTER Call;");
    return rc;
}

static int
test_p25_and_dmr_current_append_security_flags(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.lastsrc = 123456U;
    state.lasttg = 50061U;
    state.gi[0] = 1;
    state.dmr_color_code = 7U;
    state.dmr_fid = 0x10U;
    state.dmr_so = 0x80U | 0x40U | 0x30U | 0x08U | 0x04U | 0x03U;
    state.payload_algid = 0x21U;
    state.payload_keyid = 0x34U;
    watchdog_event_current(&opts, &state, 0);

    int rc = 0;
    const Event_History* item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_has_substr("dmr enc flag", item->event_string, "ENC;");
    rc |= expect_has_substr("dmr alg key", item->event_string, "ALG: 21; KID: 34;");
    rc |= expect_has_substr("dmr emergency", item->event_string, "Emergency;");
    rc |= expect_has_substr("dmr broadcast", item->event_string, "Broadcast;");
    rc |= expect_has_substr("dmr ovcm", item->event_string, "OVCM;");
    rc |= expect_has_substr("dmr private", item->event_string, "Private;");
    rc |= expect_has_substr("dmr txi", item->event_string, "TXI;");
    rc |= expect_has_substr("dmr priority", item->event_string, "PRIORITY;");

    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_P25P2_POS;
    state.lastsrc = 5790062U;
    state.lasttg = 50061U;
    state.gi[0] = 1;
    state.nac = 0x293;
    state.payload_algid = 0x84U;
    state.payload_keyid = 0x2222U;
    state.dmr_so = 0x80U;
    state.p25_service_options_valid[0] = 1;
    watchdog_event_current(&opts, &state, 0);
    item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_has_substr("p25 enc flag", item->event_string, "ENC; ALG: 84; KID: 2222;");
    rc |= expect_has_substr("p25 emergency", item->event_string, "Emergency;");
    rc |= expect_has_substr("p25 private", item->event_string, "Private;");

    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.lastp25type = 3;
    state.lastsrc = 5790062U;
    state.lasttg = 50061U;
    state.gi[0] = 0;
    state.nac = 0x293;
    state.payload_algid = 0xBBU;
    state.payload_keyid = 0xC021U;
    state.dmr_so = 0;
    watchdog_event_current(&opts, &state, 0);
    item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_no_substr("p25 clear grant ignores stale enc", item->event_string, "ENC;");
    rc |= expect_no_substr("p25 clear grant ignores stale alg", item->event_string, "ALG:");
    rc |= expect_int("p25 clear grant clears event alg", item->enc_alg, 0);
    rc |= expect_int("p25 clear grant remains clear", item->enc, 0);

    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.lastp25type = 3;
    state.lastsrc = 5790062U;
    state.lasttg = 50061U;
    state.gi[0] = 0;
    state.nac = 0x293;
    state.payload_algid = 0;
    state.payload_keyid = 0;
    state.dmr_so = 0x40U;
    state.p25_service_options_valid[0] = 0;
    watchdog_event_current(&opts, &state, 0);
    item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_no_substr("p25 stale service option ignores enc", item->event_string, "ENC;");
    rc |= expect_int("p25 stale service option clears svc", item->svc, 0);
    rc |= expect_int("p25 stale service option remains clear", item->enc, 0);

    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.lastp25type = 3;
    state.lastsrc = 5790062U;
    state.lasttg = 50061U;
    state.gi[0] = 0;
    state.nac = 0x293;
    state.payload_algid = 0xBBU;
    state.payload_keyid = 0xC021U;
    state.dmr_so = 0x40U;
    state.p25_service_options_valid[0] = 1;
    watchdog_event_current(&opts, &state, 0);
    item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_has_substr("p25 grant service option keeps enc", item->event_string, "ENC;");
    rc |= expect_no_substr("p25 grant service option omits stale alg", item->event_string, "ALG:");
    rc |= expect_int("p25 grant service option clears event alg", item->enc_alg, 0);
    rc |= expect_int("p25 grant service option encrypted", item->enc, 1);

    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.lastp25type = 2;
    state.lastsrc = 5790062U;
    state.lasttg = 50061U;
    state.gi[0] = 0;
    state.nac = 0x293;
    state.payload_algid = 0x84U;
    state.payload_keyid = 0x2222U;
    state.dmr_so = 0;
    watchdog_event_current(&opts, &state, 0);
    item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_has_substr("p25 validated voice alg renders", item->event_string, "ENC; ALG: 84; KID: 2222;");
    rc |= expect_int("p25 validated voice alg marks encrypted", item->enc, 1);
    rc |= expect_int("p25 validated voice alg kept", item->enc_alg, 0x84);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_end_only_data_call_does_not_emit_voice_end_alert();
    rc |= test_data_only_data_call_emits_one_data_alert();
    rc |= test_data_call_emits_frame_log_record();
    rc |= test_status_event_is_not_data_call_or_frame_log();
    rc |= test_source_less_data_call_does_not_suppress_next_voice_start_alert();
    rc |= test_source_less_data_call_is_preserved_in_history();
    rc |= test_source_less_dmr_data_current_event_is_not_preserved_in_history();
    rc |= test_sourced_dmr_data_current_event_does_not_emit_voice_end_alert();
    rc |= test_voice_end_alert_still_emits_for_voice_history();
    rc |= test_edacs_service_string_appends_past_pointer_size();
    rc |= test_dmr_event_string_keeps_full_prefix_after_sprintf_hardening();
    rc |= test_p25_event_string_keeps_full_prefix_after_sprintf_hardening();
    rc |= test_source_less_current_event_updates_history_metadata();
    rc |= test_event_log_writes_optional_metadata_lines();
    rc |= test_source_transition_rotates_slot_wav_files();
    rc |= test_ysf_current_sanitizes_ids_and_text_message();
    rc |= test_m17_dstar_dpmr_current_strings();
    rc |= test_nxdn_current_includes_channel_encryption_and_policy_labels();
    rc |= test_edacs_ea_mode_current_event_and_unknown_lid();
    rc |= test_p25_and_dmr_current_append_security_flags();

    if (rc == 0) {
        printf("CORE_CALL_ALERT_HISTORY: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
