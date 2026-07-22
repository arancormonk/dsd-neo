// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/platform/sndfile_fwd.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/edacs/edacs_afs.h>
#include <dsd-neo/runtime/call_alert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

_Static_assert(offsetof(Event_History_I, revision) == sizeof(Event_History) * 255U,
               "event history revision must follow the existing items");
_Static_assert(sizeof(Event_History_I) == sizeof(Event_History) * 255U + sizeof(uint64_t),
               "event history revision must add exactly eight bytes");

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int g_beeper_count;
static int g_last_beeper_id;
static int g_frame_log_count;
static char g_last_frame_log[512];

typedef struct canonical_snapshot_race_ctx {
    dsd_opts* opts;
    dsd_state* state;
    Event_History_I* history;
    int writer_failed;
    int reader_failed;
} canonical_snapshot_race_ctx;

static DSD_THREAD_RETURN_TYPE
canonical_snapshot_writer(void* arg) {
    canonical_snapshot_race_ctx* ctx = (canonical_snapshot_race_ctx*)arg;
    for (uint32_t i = 0U; i < 256U; i++) {
        const uint32_t target = 7000U + (i & 1U);
        const dsd_call_observation observation = {
            .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
            .slot = 0U,
            .kind = DSD_CALL_KIND_GROUP_VOICE,
            .ota_target_id = target,
            .policy_target_id = target,
            .ota_source_id = 8000U + i,
            .observed_m = (double)i,
        };
        if (dsd_call_state_observe(ctx->state, &observation, DSD_CALL_BOUNDARY_BEGIN) < 0) {
            ctx->writer_failed = 1;
            break;
        }
        dsd_event_sync_slot(ctx->opts, ctx->state, 0U);
    }
    DSD_THREAD_RETURN;
}

static DSD_THREAD_RETURN_TYPE
canonical_snapshot_reader(void* arg) {
    canonical_snapshot_race_ctx* ctx = (canonical_snapshot_race_ctx*)arg;
    dsd_state* snapshot = (dsd_state*)calloc(1U, sizeof(*snapshot));
    Event_History_I* copied_history = (Event_History_I*)calloc(2U, sizeof(*copied_history));
    if (snapshot == NULL || copied_history == NULL) {
        ctx->reader_failed = 1;
    } else {
        for (uint32_t i = 0U; i < 64U; i++) {
            if (dsd_event_state_copy_snapshot(snapshot, ctx->state, copied_history) < 0) {
                ctx->reader_failed = 1;
                break;
            }
            dsd_call_snapshot call;
            if (dsd_call_state_get(snapshot, 0U, &call) <= 0 || call.epoch == 0U) {
                ctx->reader_failed = 1;
                break;
            }
        }
    }
    dsd_state_ext_free_all(snapshot);
    free(copied_history);
    free(snapshot);
    DSD_THREAD_RETURN;
}

static int g_open_wav_count;
static int g_close_wav_count;
static double g_observed_m;

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
    dsd_state_ext_free_all(state);
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
    g_observed_m = 1.0;
}

static int
observe_test_call(dsd_state* state, uint8_t slot, int protocol, dsd_call_kind kind, uint64_t target_id,
                  uint64_t source_id, uint16_t service_options, uint32_t channel, dsd_call_boundary boundary) {
    const dsd_call_observation observation = {
        .protocol = protocol,
        .slot = slot,
        .kind = kind,
        .ota_target_id = target_id,
        .policy_target_id = target_id,
        .ota_source_id = source_id,
        .channel = channel,
        .service_options = service_options,
        .has_service_metadata = 1U,
        .observed_m = g_observed_m,
    };
    g_observed_m += 0.1;
    return dsd_call_state_observe(state, &observation, boundary);
}

static int
update_test_crypto(dsd_state* state, uint8_t slot, dsd_call_crypto_state classification, uint8_t algid, uint16_t kid,
                   uint8_t audio_permitted) {
    const dsd_call_crypto_update update = {
        .classification = classification,
        .algid = algid,
        .kid = kid,
        .audio_permitted = audio_permitted,
        .observed_m = g_observed_m,
    };
    g_observed_m += 0.1;
    return dsd_call_state_update_crypto(state, slot, &update);
}

static int
emit_test_data_notice(dsd_opts* opts, dsd_state* state, uint64_t source_id, uint64_t target_id, const char* notice,
                      uint8_t slot) {
    const dsd_call_observation observation = dsd_call_observation_data(state->lastsynctype, slot, source_id, target_id);
    return dsd_event_emit_data_notice(opts, state, slot, &observation, notice);
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
expect_u64(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %llu want %llu\n", label, (unsigned long long)got, (unsigned long long)want);
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
test_event_history_revision_primitives(void) {
    static Event_History_I histories[2];
    DSD_MEMSET(histories, 0, sizeof(histories));

    int rc = 0;
    init_event_history(&histories[0], 3, 3);
    rc |= expect_u64("empty init leaves revision unchanged", histories[0].revision, 0U);

    init_event_history(&histories[0], 0, 1);
    rc |= expect_u64("non-empty init advances revision", histories[0].revision, 1U);
    rc |= expect_int("init sets default color", histories[0].Event_History_Items[0].color_pair, 4);
    rc |= expect_int("init sets neutral systype", histories[0].Event_History_Items[0].systype, -1);
    rc |= expect_u64("slot revisions are independent after init", histories[1].revision, 0U);

    histories[0].Event_History_Items[0].source_id = 1234U;
    push_event_history(&histories[0]);
    rc |= expect_u64("push advances revision once", histories[0].revision, 2U);
    rc |= expect_int("push copies the head row", (int)histories[0].Event_History_Items[1].source_id, 1234);

    dsd_event_history_mark_dirty(&histories[1]);
    rc |= expect_u64("explicit mark advances selected slot", histories[1].revision, 1U);
    rc |= expect_u64("explicit mark leaves other slot unchanged", histories[0].revision, 2U);
    dsd_event_history_mark_dirty(NULL);

    histories[1].revision = UINT64_MAX;
    dsd_event_history_mark_dirty(&histories[1]);
    rc |= expect_u64("revision wrap skips zero", histories[1].revision, 1U);
    return rc;
}

static int
test_watchdog_current_marks_only_semantic_changes(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.playfiles = 1;
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.dmr_color_code = 1U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 5678U, 1234U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);

    const uint64_t initial_revision = event_history[0].revision;
    watchdog_event_current(&opts, &state, 0);
    const uint64_t first_revision = event_history[0].revision;

    int rc = expect_u64("first watchdog update advances revision", first_revision, initial_revision + 1U);
    watchdog_event_current(&opts, &state, 0);
    rc |= expect_u64("identical watchdog update leaves revision unchanged", event_history[0].revision, first_revision);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 5678U, 4321U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    watchdog_event_current(&opts, &state, 0);
    rc |= expect_u64("semantic watchdog update advances revision", event_history[0].revision, first_revision + 1U);
    rc |= expect_u64("watchdog slot update leaves other slot unchanged", event_history[1].revision, 1U);
    return rc;
}

static int
test_nonfinalizing_call_notice_defers_call_end_side_effects(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    static max_align_t wav_sentinel;
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_END;
    opts.wav_out_f = (SNDFILE*)&wav_sentinel;

    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P1_POS, DSD_CALL_KIND_GROUP_VOICE, 1234U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);

    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0U, &call) == 1);
    const char* detail = "Target: 1234; has been locked out; Encryption Lock Out Enabled.";

    int rc = expect_int("nonfinalizing notice committed",
                        dsd_event_emit_call_notice_nonfinalizing(&opts, &state, 0U, &call, detail), 1);
    rc |= expect_has_substr("nonfinalizing notice stored", event_history[0].Event_History_Items[1].internal_str,
                            "Target: 1234");
    rc |= expect_int("nonfinalizing notice does not beep", g_beeper_count, 0);
    rc |= expect_int("nonfinalizing notice does not close WAV", g_close_wav_count, 0);
    rc |= expect_int("nonfinalizing notice does not open WAV", g_open_wav_count, 0);
    assert(dsd_call_state_get(&state, 0U, &call) == 1);
    rc |= expect_int("nonfinalizing notice preserves active call", call.phase, DSD_CALL_PHASE_ACTIVE);
    rc |= expect_int("nonfinalizing notice preserves call kind", call.kind, DSD_CALL_KIND_GROUP_VOICE);

    dsd_event_sync_slot(&opts, &state, 0U);
    assert(dsd_call_state_end(&state, 0U, 2.0) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    rc |= expect_int("later call end beeps", g_beeper_count, 1);
    rc |= expect_int("later call end closes WAV", g_close_wav_count, 1);
    rc |= expect_int("later call end opens WAV", g_open_wav_count, 1);
    rc |=
        expect_int("later call end commits rebuilt row", (int)event_history[0].Event_History_Items[1].target_id, 1234);
    rc |= expect_has_substr("nonfinalizing notice remains in history",
                            event_history[0].Event_History_Items[2].internal_str, "Target: 1234");
    return rc;
}

static int
test_event_state_snapshot_copy_accepts_aliased_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    static Event_History_I copied_history[2];
    reset_fixture(&opts, &state, event_history);
    DSD_MEMSET(copied_history, 0, sizeof(copied_history));

    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 2201U,
        .policy_target_id = 2201U,
        .ota_source_id = 3301U,
        .observed_m = 1.0,
    };
    assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_int("aliased event-state snapshot copy succeeds",
                        dsd_event_state_copy_snapshot(&state, &state, copied_history), 1);
    rc |= expect_int("aliased event-state snapshot copies target",
                     (int)copied_history[0].Event_History_Items[0].target_id, 2201);
    rc |= expect_u64("aliased event-state snapshot copies revision", copied_history[0].revision,
                     event_history[0].revision);

    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0U, &call) == 1);
    rc |= expect_int("aliased event-state snapshot preserves canonical target", (int)call.ota_target_id, 2201);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_end_only_data_call_does_not_emit_voice_end_alert(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_END;

    (void)emit_test_data_notice(&opts, &state, 1234, 5678, "MNIS ARS;", 0);
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

    (void)emit_test_data_notice(&opts, &state, 1234, 5678, "MNIS ARS;", 0);
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

    (void)emit_test_data_notice(&opts, &state, 1234, 5678, "MNIS ARS;", 0);

    int rc = 0;
    rc |= expect_int("data call should emit one frame log", g_frame_log_count, 1);
    rc |= expect_has_substr("data call frame log should identify data", g_last_frame_log, "FRAME DATA slot=1");
    rc |= expect_has_substr("data call frame log should keep source", g_last_frame_log, "src=1234");
    rc |= expect_has_substr("data call frame log should keep target", g_last_frame_log, "dst=5678");
    return rc;
}

static int
test_data_notice_preserves_decoded_payload_fields(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    Event_History* decoded = &event_history[0].Event_History_Items[0];
    decoded->pdu[0] = 0x12U;
    decoded->pdu[1] = 0x34U;
    DSD_SNPRINTF(decoded->text_message, sizeof(decoded->text_message), "%s", "$GPRMC,validated");
    DSD_SNPRINTF(decoded->gps_s, sizeof(decoded->gps_s), "%s", "41.500000 -87.250000");

    assert(emit_test_data_notice(&opts, &state, 1234U, 5678U, "NMEA SRC: 1234; TGT: 5678;", 0U) == 0);

    const Event_History* committed = &event_history[0].Event_History_Items[1];
    int rc = 0;
    rc |= expect_int("data payload first byte", committed->pdu[0], 0x12);
    rc |= expect_int("data payload second byte", committed->pdu[1], 0x34);
    rc |= expect_str_eq("data payload text", committed->text_message, "$GPRMC,validated");
    rc |= expect_str_eq("data payload GPS", committed->gps_s, "41.500000 -87.250000");
    rc |= expect_int("data payload category", committed->category, DSD_EVENT_CATEGORY_DATA);
    rc |= expect_has_substr("data payload notice", committed->event_string, "NMEA SRC: 1234; TGT: 5678;");
    return rc;
}

static int
test_system_notice_is_not_attributed_as_radio_data(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_DATA;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 5678U, 1234U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_int("system notice emits", dsd_event_emit_system_notice(&opts, &state, 0U, "Capture rotated;"), 0);
    const Event_History* current = &state.event_history_s[0].Event_History_Items[0];
    const Event_History* stored = &state.event_history_s[0].Event_History_Items[1];
    rc |= expect_int("system notice preserves current voice target", (int)current->target_id, 5678);
    rc |= expect_int("system notice category", stored->category, DSD_EVENT_CATEGORY_SYSTEM);
    rc |= expect_int("system notice severity", stored->severity, DSD_EVENT_SEVERITY_INFO);
    rc |= expect_int("system notice neutral subtype", stored->subtype, -1);
    rc |= expect_int("system notice neutral systype", stored->systype, DSD_SYNC_NONE);
    rc |= expect_int("system notice source", (int)stored->source_id, 0);
    rc |= expect_int("system notice target", (int)stored->target_id, 0);
    rc |= expect_has_substr("system notice text", stored->event_string, "Capture rotated;");
    rc |= expect_int("system notice does not alert as data", g_beeper_count, 0);
    rc |= expect_int("system notice emits one frame log", g_frame_log_count, 1);
    rc |= expect_has_substr("system notice frame log category", g_last_frame_log, "FRAME SYSTEM slot=1");

    dsd_state_ext_free_all(&state);
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

    (void)emit_test_data_notice(&opts, &state, 0, 0, "MNIS ARS;", 0);
    watchdog_event_history(&opts, &state, 0);

    int rc = expect_int("source-less data should not beep as voice start", g_beeper_count, 0);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 5678U, 1234U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
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

    (void)emit_test_data_notice(&opts, &state, 0, 0, "MNIS ARS;", 0);
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
test_source_less_dmr_data_notices_are_preserved_in_history(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.dmr_color_code = 1U;

    (void)emit_test_data_notice(&opts, &state, 0U, 0U, "DMR slot 1 data;", 0U);
    watchdog_event_history(&opts, &state, 0);

    (void)emit_test_data_notice(&opts, &state, 0U, 0U, "DMR slot 2 data;", 1U);
    watchdog_event_history(&opts, &state, 1);

    int rc = 0;
    rc |= expect_has_substr("slot 1 source-less DMR data stored",
                            state.event_history_s[0].Event_History_Items[1].event_string, "DMR slot 1 data;");
    rc |= expect_has_substr("slot 2 source-less DMR data stored",
                            state.event_history_s[1].Event_History_Items[1].event_string, "DMR slot 2 data;");
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

    (void)emit_test_data_notice(&opts, &state, 1234U, 5678U, "DMR slot 1 data;", 0U);
    watchdog_event_history(&opts, &state, 0);

    int rc = 0;
    rc |= expect_int("slot 1 sourced DMR data end should not beep", g_beeper_count, 0);
    rc |= expect_int("slot 1 sourced DMR data should be stored",
                     state.event_history_s[0].Event_History_Items[1].event_string[0] != '\0', 1);

    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_END;
    state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.dmr_color_code = 1U;

    (void)emit_test_data_notice(&opts, &state, 2345U, 6789U, "DMR slot 2 data;", 1U);
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
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 5678U, 1234U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(dsd_call_state_end(&state, 0U, 2.0) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

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
    state.edacs_tuned_lcn = 7;
    state.edacs_site_id = 3;
    state.edacs_area_code = 1;
    state.edacs_sys_id = 0x2A;
    state.edacs_a_shift = 7;
    state.edacs_f_shift = 3;
    state.edacs_a_mask = 0x0F;
    state.edacs_f_mask = 0x0F;
    state.edacs_s_mask = 0x07;
    assert(observe_test_call(&state, 0U, DSD_SYNC_EDACS_POS, DSD_CALL_KIND_GROUP_VOICE, 0x0123U, 1201U, 0x0AU, 7U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);

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
    state.dmr_color_code = 7U;
    state.dmr_t3_syscode = 0xABCU;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 50061U, 123456U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);

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
    state.nac = 0x293;
    state.p2_wacn = 0x45564U;
    state.p2_sysid = 0x006U;
    state.p2_rfssid = 10U;
    state.p2_siteid = 10U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 50061U, 5790062U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);

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
    state.nac = 0x006;
    state.p2_wacn = 0x45564U;
    state.p2_sysid = 0x006U;
    state.p2_rfssid = 10U;
    state.p2_siteid = 10U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 21001U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);

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
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 1234U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    g_open_wav_count = 0;
    g_close_wav_count = 0;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 5678U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = 0;
    rc |= expect_int("slot 1 wav close", g_close_wav_count, 1);
    rc |= expect_int("slot 1 wav reopen", g_open_wav_count, 1);
    rc |= expect_int("slot 1 transition stored prior source",
                     (int)state.event_history_s[0].Event_History_Items[1].source_id, 1234);

    g_open_wav_count = 0;
    g_close_wav_count = 0;
    assert(observe_test_call(&state, 1U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 200U, 2222U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 1U);
    g_open_wav_count = 0;
    g_close_wav_count = 0;
    assert(observe_test_call(&state, 1U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 200U, 3333U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 1U);

    rc |= expect_int("slot 2 wav close", g_close_wav_count, 1);
    rc |= expect_int("slot 2 wav reopen", g_open_wav_count, 1);
    rc |= expect_int("slot 2 transition stored prior source",
                     (int)state.event_history_s[1].Event_History_Items[1].source_id, 2222);
    return rc;
}

static int
test_ysf_current_sanitizes_ids_and_text_message(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    state.lastsynctype = DSD_SYNC_YSF_POS;
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_YSF_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
    };
    DSD_MEMCPY(observation.source_text,
               "SRC\x01"
               "CALL",
               8);
    DSD_MEMCPY(observation.target_text, "TG*ROOM", 7);
    (void)dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN);
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
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_M17_LSF_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
        .ota_target_id = 0xFFFFFFFFFFFFULL,
        .ota_source_id = 12345ULL,
        .service_options = 4U,
        .has_service_metadata = 1U,
    };
    DSD_SNPRINTF(observation.source_text, sizeof(observation.source_text), "%s", "SRCSTR");
    DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", "BROADCAST");
    (void)dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    watchdog_event_current(&opts, &state, 0);

    int rc = 0;
    const Event_History* item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_str_eq("m17 source text", item->src_str, "SRCSTR");
    rc |= expect_has_substr("m17 broadcast event", item->event_string, "TGT: BROADCAST SRC: SRCSTR CAN: 04;");

    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_DSTAR_VOICE_POS;
    DSD_MEMSET(&observation, 0, sizeof(observation));
    observation.protocol = DSD_SYNC_DSTAR_VOICE_POS;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_VOICE;
    DSD_MEMCPY(observation.source_text, "N0CALL\x02/RPT", 11);
    DSD_MEMCPY(observation.target_text, "CQCQCQ", 6);
    (void)dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    watchdog_event_current(&opts, &state, 0);
    item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_str_eq("dstar sysid", item->sysid_string, "DSTAR");
    rc |= expect_has_substr("dstar sanitized source", item->src_str, "N0CALL_/RPT");
    rc |= expect_has_substr("dstar event", item->event_string, "TGT: CQCQCQ");

    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_DPMR_FS2_POS;
    state.dpmr_color_code = 9U;
    DSD_MEMSET(&observation, 0, sizeof(observation));
    observation.protocol = DSD_SYNC_DPMR_FS2_POS;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_VOICE;
    observation.channel = 9U;
    observation.service_options = 3U << 8U;
    observation.has_service_metadata = 1U;
    DSD_SNPRINTF(observation.source_text, sizeof(observation.source_text), "%s", "CALLER7");
    DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", "TARGET9");
    (void)dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    const dsd_call_crypto_update crypto = {
        .classification = DSD_CALL_CRYPTO_ENCRYPTED,
        .audio_permitted = 0U,
    };
    (void)dsd_call_state_update_crypto(&state, 0U, &crypto);
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
    state.nxdn_last_ran = 23U;
    state.nxdn_location_site_code = 5U;
    state.nxdn_location_sys_code = 12U;
    state.nxdn_cipher_type = 3U;
    state.nxdn_key = 0x2AU;
    state.nxdn_grant_chan = 198U;
    state.nxdn_grant_freq = 453212500U;
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_NXDN_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_PRIVATE_VOICE,
        .ota_target_id = 51002U,
        .policy_target_id = 51002U,
        .ota_source_id = 41001U,
        .channel = 198U,
        .frequency_hz = 453212500,
    };
    (void)dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    const dsd_call_crypto_update crypto = {
        .classification = DSD_CALL_CRYPTO_ENCRYPTED,
        .algid = 3U,
        .kid = 0x2AU,
        .audio_permitted = 0U,
    };
    (void)dsd_call_state_update_crypto(&state, 0U, &crypto);
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
    state.edacs_tuned_lcn = 11U;
    state.edacs_site_id = 12U;
    state.edacs_area_code = 3U;
    state.edacs_sys_id = 0x45U;
    state.edacs_a_shift = 7;
    state.edacs_f_shift = 3;
    state.edacs_a_mask = 0x0F;
    state.edacs_f_mask = 0x0F;
    state.edacs_s_mask = 0x07;
    assert(observe_test_call(&state, 0U, DSD_SYNC_EDACS_POS, DSD_CALL_KIND_GROUP_VOICE, 0x0123U, 0U, 0x01U, 11U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);

    watchdog_event_current(&opts, &state, 0);
    int rc = 0;
    rc |= expect_has_substr("edacs unknown lid", state.event_history_s[0].Event_History_Items[0].event_string,
                            "LID: __UNK;");

    reset_fixture(&opts, &state, event_history);
    opts.trunk_is_tuned = 1;
    state.lastsynctype = DSD_SYNC_EDACS_POS;
    state.ea_mode = 1;
    state.edacs_tuned_lcn = 12U;
    state.edacs_site_id = 7U;
    state.edacs_area_code = 2U;
    state.edacs_sys_id = 0x1234U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_EDACS_POS, DSD_CALL_KIND_GROUP_VOICE, 88002U, 77001U, 0x48U, 12U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);

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
    state.dmr_color_code = 7U;
    state.dmr_fid = 0x10U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_PRIVATE_VOICE, 50061U, 123456U, 0xFFU,
                             0U, DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    assert(update_test_crypto(&state, 0U, DSD_CALL_CRYPTO_ENCRYPTED, 0x21U, 0x34U, 0U) == 1);
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
    state.nac = 0x293;
    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_PRIVATE_VOICE, 50061U, 5790062U, 0x80U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    assert(update_test_crypto(&state, 0U, DSD_CALL_CRYPTO_ENCRYPTED, 0x84U, 0x2222U, 0U) == 1);
    watchdog_event_current(&opts, &state, 0);
    item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_has_substr("p25 enc flag", item->event_string, "ENC; ALG: 84; KID: 2222;");
    rc |= expect_has_substr("p25 emergency", item->event_string, "Emergency;");
    rc |= expect_has_substr("p25 private", item->event_string, "Private;");

    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.nac = 0x293;
    /* Decoder scratch must not leak into a canonically clear call. */
    state.payload_algid = 0xBBU;
    state.payload_keyid = 0xC021U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P1_POS, DSD_CALL_KIND_GROUP_VOICE, 50061U, 5790062U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    assert(update_test_crypto(&state, 0U, DSD_CALL_CRYPTO_CLEAR, 0U, 0U, 1U) == 1);
    watchdog_event_current(&opts, &state, 0);
    item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_no_substr("p25 clear grant ignores stale enc", item->event_string, "ENC;");
    rc |= expect_no_substr("p25 clear grant ignores stale alg", item->event_string, "ALG:");
    rc |= expect_int("p25 clear grant clears event alg", item->enc_alg, 0);
    rc |= expect_int("p25 clear grant remains clear", item->enc, 0);

    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.nac = 0x293;
    state.payload_algid = 0;
    state.payload_keyid = 0;
    state.dmr_so = 0x40U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P1_POS, DSD_CALL_KIND_GROUP_VOICE, 50061U, 5790062U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    watchdog_event_current(&opts, &state, 0);
    item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_no_substr("p25 stale service option ignores enc", item->event_string, "ENC;");
    rc |= expect_int("p25 stale service option clears svc", item->svc, 0);
    rc |= expect_int("p25 stale service option remains clear", item->enc, 0);

    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.nac = 0x293;
    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P1_POS, DSD_CALL_KIND_GROUP_VOICE, 50061U, 5790062U, 0x40U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    assert(update_test_crypto(&state, 0U, DSD_CALL_CRYPTO_ENCRYPTED_PENDING, 0U, 0U, 0U) == 1);
    watchdog_event_current(&opts, &state, 0);
    item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_has_substr("p25 grant service option keeps enc", item->event_string, "ENC;");
    rc |= expect_no_substr("p25 grant service option omits stale alg", item->event_string, "ALG:");
    rc |= expect_int("p25 grant service option clears event alg", item->enc_alg, 0);
    rc |= expect_int("p25 grant service option encrypted", item->enc, 1);

    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.nac = 0x293;
    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P1_POS, DSD_CALL_KIND_GROUP_VOICE, 50061U, 5790062U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    assert(update_test_crypto(&state, 0U, DSD_CALL_CRYPTO_ENCRYPTED, 0x84U, 0x2222U, 0U) == 1);
    watchdog_event_current(&opts, &state, 0);
    item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_has_substr("p25 validated voice alg renders", item->event_string, "ENC; ALG: 84; KID: 2222;");
    rc |= expect_int("p25 validated voice alg marks encrypted", item->enc, 1);
    rc |= expect_int("p25 validated voice alg kept", item->enc_alg, 0x84);

    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.nac = 0x798;
    state.payload_algid = 0xA0U;
    state.payload_keyid = 0x0064U;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;
    state.p25_p1_crypto_conflict.active = 1U;
    state.p25_p1_crypto_conflict.algid = 0xA0U;
    state.p25_p1_crypto_conflict.keyid = 0x0064U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P1_POS, DSD_CALL_KIND_GROUP_VOICE, 3069U, 4009646U, 0x04U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    assert(update_test_crypto(&state, 0U, DSD_CALL_CRYPTO_CLEAR, 0U, 0U, 1U) == 1);
    watchdog_event_current(&opts, &state, 0);
    item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_no_substr("p25 pending conflict stays clear", item->event_string, "ENC;");
    rc |= expect_no_substr("p25 pending conflict omits candidate alg", item->event_string, "ALG:");
    rc |= expect_int("p25 pending conflict event remains clear", item->enc, 0);
    rc |= expect_int("p25 pending conflict event clears alg", item->enc_alg, 0);
    return rc;
}

static int
test_canonical_call_lifecycle_is_epoch_driven(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_VOICE_END;

    dsd_call_observation observation = {0};
    observation.protocol = DSD_SYNC_P25P2_POS;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = 100U;
    observation.policy_target_id = 900U;
    observation.ota_source_id = 200U;
    observation.frequency_hz = 851012500L;
    observation.observed_m = 1.0;
    assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);

    dsd_call_crypto_update crypto = {0};
    crypto.classification = DSD_CALL_CRYPTO_CLEAR;
    crypto.audio_permitted = 1U;
    crypto.observed_m = 1.1;
    assert(dsd_call_state_update_crypto(&state, 0U, &crypto) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = 0;
    Event_History* current = &event_history[0].Event_History_Items[0];
    rc |= expect_int("canonical start target", (int)current->target_id, 100);
    rc |= expect_int("canonical start source", (int)current->source_id, 200);
    rc |= expect_int("canonical clear state", current->enc, 0);
    rc |= expect_int("canonical start alert once", g_beeper_count, 1);

    crypto.classification = DSD_CALL_CRYPTO_ENCRYPTED;
    crypto.algid = 0x84U;
    crypto.kid = 0x2222U;
    crypto.audio_permitted = 0U;
    crypto.observed_m = 1.2;
    assert(dsd_call_state_update_crypto(&state, 0U, &crypto) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    current = &event_history[0].Event_History_Items[0];
    rc |= expect_int("crypto refinement stays current", (int)event_history[0].Event_History_Items[1].target_id, 0);
    rc |= expect_int("crypto refinement marks encrypted", current->enc, 1);
    rc |= expect_int("crypto refinement keeps alg", current->enc_alg, 0x84);
    rc |= expect_int("crypto refinement does not alert", g_beeper_count, 1);

    assert(dsd_call_state_end(&state, 0U, 2.0) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    const uint64_t ended_revision = event_history[0].revision;
    rc |= expect_int("ended epoch clears head", event_history[0].Event_History_Items[0].event_string[0], '\0');
    rc |= expect_int("ended epoch stored target", (int)event_history[0].Event_History_Items[1].target_id, 100);
    rc |= expect_int("ended epoch stored encrypted status", event_history[0].Event_History_Items[1].enc, 1);
    rc |= expect_int("canonical end alert once", g_beeper_count, 2);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_u64("repeated end sync is idempotent", event_history[0].revision, ended_revision);
    rc |= expect_int("repeated end sync does not alert", g_beeper_count, 2);

    observation.observed_m = 3.0;
    assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_call_snapshot snapshot;
    assert(dsd_call_state_get(&state, 0U, &snapshot) == 1);
    rc |= expect_u64("identical PTT after end advances epoch", snapshot.epoch, 2U);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("identical PTT gets one new start alert", g_beeper_count, 3);

    assert(dsd_call_state_end(&state, 0U, 3.5) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    observation.ota_target_id = observation.policy_target_id = 300U;
    observation.ota_source_id = 0U;
    observation.observed_m = 4.0;
    assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(dsd_call_state_get(&state, 0U, &snapshot) == 1);
    const uint64_t late_identity_epoch = snapshot.epoch;
    observation.ota_source_id = 400U;
    observation.observed_m = 4.1;
    assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(dsd_call_state_get(&state, 0U, &snapshot) == 1);
    rc |= expect_u64("late source keeps epoch", snapshot.epoch, late_identity_epoch);
    observation.kind = DSD_CALL_KIND_PRIVATE_VOICE;
    observation.ota_target_id = observation.policy_target_id = 0xABCDEFU;
    observation.observed_m = 4.2;
    assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("known target change rotates prior row", (int)event_history[0].Event_History_Items[1].target_id,
                     300);
    assert(dsd_call_state_get(&state, 0U, &snapshot) == 1);
    rc |= expect_int("canonical rotation preserves live private identity", snapshot.kind, DSD_CALL_KIND_PRIVATE_VOICE);

    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_new_canonical_epoch_commits_prior_canonical_call(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    static max_align_t wav_sentinel;
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_END;
    opts.wav_out_f = (SNDFILE*)&wav_sentinel;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 300U,
        .policy_target_id = 300U,
        .ota_source_id = 400U,
        .observed_m = 2.0,
    };
    assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* current = &event_history[0].Event_History_Items[0];
    const Event_History* committed = &event_history[0].Event_History_Items[1];
    int rc = expect_int("new canonical epoch keeps current target", (int)current->target_id, 300);
    rc |= expect_int("new canonical epoch commits prior target", (int)committed->target_id, 100);
    rc |= expect_int("new canonical epoch commits prior source", (int)committed->source_id, 200);
    rc |= expect_int("new canonical epoch commits prior protocol", committed->systype, DSD_SYNC_DMR_BS_VOICE_POS);
    rc |= expect_int("new canonical epoch emits prior call end alert", g_beeper_count, 1);
    rc |= expect_int("new canonical epoch closes prior call WAV", g_close_wav_count, 1);
    rc |= expect_int("new canonical epoch opens next call WAV", g_open_wav_count, 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_late_source_enriches_matching_canonical_call(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    static max_align_t wav_sentinel;
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_END;
    opts.wav_out_f = (SNDFILE*)&wav_sentinel;

    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 100U,
        .policy_target_id = 100U,
        .ota_source_id = 200U,
        .observed_m = 2.0,
    };
    assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* current = &event_history[0].Event_History_Items[0];
    const Event_History* committed = &event_history[0].Event_History_Items[1];
    int rc = expect_int("late source keeps target", (int)current->target_id, 100);
    rc |= expect_int("late source is adopted", (int)current->source_id, 200);
    rc |= expect_int("late source avoids duplicate history", (int)committed->target_id, 0);
    rc |= expect_int("late source avoids call end alert", g_beeper_count, 0);
    rc |= expect_int("late source keeps WAV open", g_close_wav_count, 0);
    rc |= expect_int("late source avoids new WAV", g_open_wav_count, 0);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_active_canonical_call_does_not_suppress_explicit_data(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    dsd_call_observation observation = {0};
    observation.protocol = DSD_SYNC_P25P1_POS;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = 100U;
    observation.policy_target_id = 100U;
    observation.ota_source_id = 200U;
    observation.observed_m = 1.0;
    assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    state.lastsynctype = DSD_SYNC_P25P1_POS;
    (void)emit_test_data_notice(&opts, &state, 700U, 800U, "P25 packet data;", 0U);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* current = &event_history[0].Event_History_Items[0];
    const Event_History* committed = &event_history[0].Event_History_Items[1];
    int rc = 0;
    rc |= expect_int("active call is restored after explicit data", (int)current->target_id, 100);
    rc |= expect_int("active-call data target is preserved", (int)committed->target_id, 800);
    rc |= expect_int("active-call data source is preserved", (int)committed->source_id, 700);
    rc |= expect_int("active-call data subtype is preserved", committed->subtype, INT8_MAX);
    rc |= expect_has_substr("active-call data detail is preserved", committed->event_string, "P25 packet data");
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_ended_canonical_call_does_not_suppress_later_data(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    dsd_call_observation observation = {0};
    observation.protocol = DSD_SYNC_P25P1_POS;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = 100U;
    observation.policy_target_id = 100U;
    observation.ota_source_id = 200U;
    observation.observed_m = 1.0;
    assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(dsd_call_state_end(&state, 0U, 2.0) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    (void)emit_test_data_notice(&opts, &state, 700U, 800U, "DMR packet data;", 0U);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* committed = &event_history[0].Event_History_Items[1];
    int rc = 0;
    rc |= expect_int("post-P25 data target is preserved", (int)committed->target_id, 800);
    rc |= expect_int("post-P25 data source is preserved", (int)committed->source_id, 700);
    rc |= expect_int("post-P25 data subtype is preserved", committed->subtype, INT8_MAX);
    rc |= expect_has_substr("post-P25 data detail is preserved", committed->event_string, "DMR packet data");
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_concurrent_call_history_snapshot_copy(void) {
    canonical_snapshot_race_ctx* ctx = (canonical_snapshot_race_ctx*)calloc(1U, sizeof(*ctx));
    if (ctx == NULL) {
        return 1;
    }
    ctx->opts = (dsd_opts*)calloc(1U, sizeof(*ctx->opts));
    ctx->state = (dsd_state*)calloc(1U, sizeof(*ctx->state));
    ctx->history = (Event_History_I*)calloc(2U, sizeof(*ctx->history));
    if (ctx->opts == NULL || ctx->state == NULL || ctx->history == NULL) {
        free(ctx->opts);
        free(ctx->state);
        free(ctx->history);
        free(ctx);
        return 1;
    }
    ctx->state->event_history_s = ctx->history;

    const dsd_call_observation initial = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 7000U,
        .policy_target_id = 7000U,
        .ota_source_id = 8000U,
    };
    int rc = dsd_call_state_observe(ctx->state, &initial, DSD_CALL_BOUNDARY_BEGIN) < 0;
    dsd_event_sync_slot(ctx->opts, ctx->state, 0U);

    dsd_thread_t writer;
    dsd_thread_t reader;
    const int writer_created = dsd_thread_create(&writer, canonical_snapshot_writer, ctx) == 0;
    const int reader_created = dsd_thread_create(&reader, canonical_snapshot_reader, ctx) == 0;
    if (writer_created) {
        rc |= dsd_thread_join(writer) != 0;
    }
    if (reader_created) {
        rc |= dsd_thread_join(reader) != 0;
    }
    rc |= !writer_created || !reader_created || ctx->writer_failed || ctx->reader_failed;

    dsd_state_ext_free_all(ctx->state);
    free(ctx->opts);
    free(ctx->state);
    free(ctx->history);
    free(ctx);
    return rc;
}

static int
test_call_context_snapshot_restores_committed_end(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_END;

    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(dsd_call_state_end(&state, 0U, 2.0) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    dsd_call_context_snapshot saved = {0};
    assert(dsd_call_context_copy_snapshot(&state, &saved) == 1);
    assert(saved.events[0].epoch == saved.calls.slots[0].epoch);
    assert(saved.events[0].ended_committed == 1U);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 300U, 400U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(dsd_call_context_restore_snapshot(&state, &saved) == 1);

    const uint64_t revision_before_resync = event_history[0].revision;
    const int beeps_before_resync = g_beeper_count;
    const int closes_before_resync = g_close_wav_count;
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = 0;
    rc |= expect_u64("restored committed end is not replayed", event_history[0].revision, revision_before_resync);
    rc |= expect_int("restored committed end does not beep again", g_beeper_count, beeps_before_resync);
    rc |= expect_int("restored committed end does not rotate WAV again", g_close_wav_count, closes_before_resync);
    dsd_state_ext_free_all(&state);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_event_history_revision_primitives();
    rc |= test_watchdog_current_marks_only_semantic_changes();
    rc |= test_nonfinalizing_call_notice_defers_call_end_side_effects();
    rc |= test_event_state_snapshot_copy_accepts_aliased_state();
    rc |= test_end_only_data_call_does_not_emit_voice_end_alert();
    rc |= test_data_only_data_call_emits_one_data_alert();
    rc |= test_data_call_emits_frame_log_record();
    rc |= test_data_notice_preserves_decoded_payload_fields();
    rc |= test_system_notice_is_not_attributed_as_radio_data();
    rc |= test_status_event_is_not_data_call_or_frame_log();
    rc |= test_source_less_data_call_does_not_suppress_next_voice_start_alert();
    rc |= test_source_less_data_call_is_preserved_in_history();
    rc |= test_source_less_dmr_data_notices_are_preserved_in_history();
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
    rc |= test_canonical_call_lifecycle_is_epoch_driven();
    rc |= test_new_canonical_epoch_commits_prior_canonical_call();
    rc |= test_late_source_enriches_matching_canonical_call();
    rc |= test_active_canonical_call_does_not_suppress_explicit_data();
    rc |= test_ended_canonical_call_does_not_suppress_later_data();
    rc |= test_concurrent_call_history_snapshot_copy();
    rc |= test_call_context_snapshot_restores_committed_end();

    if (rc == 0) {
        printf("CORE_CALL_ALERT_HISTORY: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
