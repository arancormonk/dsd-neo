// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dsd_time.h>
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
_Static_assert(offsetof(Event_History_I, push_seq) == sizeof(Event_History) * 255U + sizeof(uint64_t),
               "event history push sequence must follow the revision");
_Static_assert(sizeof(Event_History_I) == sizeof(Event_History) * 255U + (2U * sizeof(uint64_t)),
               "event history bookkeeping must add exactly two 64-bit counters");

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
        if ((i & 7U) == 0U) {
            dsd_event_history_transaction transaction;
            dsd_event_history_transaction_begin(ctx->state, &transaction);
            DSD_SNPRINTF(ctx->history[0].Event_History_Items[0].text_message,
                         sizeof(ctx->history[0].Event_History_Items[0].text_message), "packet-%u", i);
            dsd_event_history_mark_dirty(&ctx->history[0]);
            dsd_event_history_transaction_end(&transaction);

            const dsd_call_observation data =
                dsd_call_observation_data(DSD_SYNC_DMR_BS_DATA_POS, 0U, 9000U + i, 10000U + i);
            if (dsd_event_emit_data_notice(ctx->opts, ctx->state, 0U, &data, "Concurrent packet data;") != 0
                || dsd_event_emit_system_notice(ctx->opts, ctx->state, 0U, "Concurrent system notice;") != 0) {
                ctx->writer_failed = 1;
                break;
            }
        }
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
static SNDFILE* g_open_wav_result;
static double g_observed_m;

SNDFILE*
open_wav_file(char* dir, char* temp_filename, size_t temp_filename_size, uint16_t sample_rate, uint8_t ext) {
    UNUSED(dir);
    UNUSED(temp_filename);
    UNUSED(temp_filename_size);
    UNUSED(sample_rate);
    UNUSED(ext);
    g_open_wav_count++;
    return g_open_wav_result;
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

// The event builders render AFS from the bit widths captured with the row, not from live state.
int
getAfsStringFromBits(int a_bits, int f_bits, int s_bits, char* buffer, int a, int f, int s) {
    UNUSED(a_bits);
    UNUSED(f_bits);
    UNUSED(s_bits);
    return DSD_SNPRINTF(buffer, 7, "%02d-%02d%01d", a, f, s);
}

int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    // An unstamped time really does render as the epoch, so the stub has to say so: rendering a
    // plausible date for timestamp 0 would hide a row being restamped from an absent event_time.
    if (timestamp == 0) {
        const char* epoch = (format == DSD_LOCAL_DATETIME_DATE_HYPHEN) ? "1970-01-01" : "00:00:00";
        DSD_SNPRINTF(out, out_size, "%s", epoch);
        return 1;
    }
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
    g_open_wav_result = NULL;
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

// End the slot's call on the fixture's own timeline. Every reacquisition assertion is driven
// through observed_m rather than wall clock, so the window behaves identically under the
// sanitizer presets and a loaded parallel ctest run.
static int
end_test_call(dsd_state* state, uint8_t slot, dsd_call_end_reason reason) {
    const int rc = dsd_call_state_end_ex(state, slot, g_observed_m, reason);
    g_observed_m += 0.1;
    return rc;
}

static void
advance_test_clock(double seconds) {
    g_observed_m += seconds;
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
event_history_item_equal(const Event_History* lhs, const Event_History* rhs) {
    return lhs->write == rhs->write && lhs->color_pair == rhs->color_pair && lhs->severity == rhs->severity
           && lhs->category == rhs->category && lhs->systype == rhs->systype && lhs->subtype == rhs->subtype
           && lhs->sys_id1 == rhs->sys_id1 && lhs->sys_id2 == rhs->sys_id2 && lhs->sys_id3 == rhs->sys_id3
           && lhs->sys_id4 == rhs->sys_id4 && lhs->sys_id5 == rhs->sys_id5 && lhs->gi == rhs->gi && lhs->enc == rhs->enc
           && lhs->enc_alg == rhs->enc_alg && lhs->enc_key == rhs->enc_key && lhs->mi == rhs->mi && lhs->svc == rhs->svc
           && lhs->source_id == rhs->source_id && lhs->target_id == rhs->target_id
           && memcmp(lhs->src_str, rhs->src_str, sizeof lhs->src_str) == 0
           && memcmp(lhs->tgt_str, rhs->tgt_str, sizeof lhs->tgt_str) == 0
           && memcmp(lhs->t_name, rhs->t_name, sizeof lhs->t_name) == 0
           && memcmp(lhs->s_name, rhs->s_name, sizeof lhs->s_name) == 0
           && memcmp(lhs->t_mode, rhs->t_mode, sizeof lhs->t_mode) == 0
           && memcmp(lhs->s_mode, rhs->s_mode, sizeof lhs->s_mode) == 0 && lhs->channel == rhs->channel
           && lhs->event_time == rhs->event_time && memcmp(lhs->pdu, rhs->pdu, sizeof lhs->pdu) == 0
           && memcmp(lhs->sysid_string, rhs->sysid_string, sizeof lhs->sysid_string) == 0
           && memcmp(lhs->alias, rhs->alias, sizeof lhs->alias) == 0
           && memcmp(lhs->gps_s, rhs->gps_s, sizeof lhs->gps_s) == 0
           && memcmp(lhs->text_message, rhs->text_message, sizeof lhs->text_message) == 0
           && memcmp(lhs->event_string, rhs->event_string, sizeof lhs->event_string) == 0
           && memcmp(lhs->internal_str, rhs->internal_str, sizeof lhs->internal_str) == 0;
}

static int
event_histories_equal(const Event_History_I lhs[2], const Event_History_I rhs[2]) {
    for (size_t slot = 0U; slot < 2U; slot++) {
        if (lhs[slot].revision != rhs[slot].revision) {
            return 0;
        }
        for (size_t item = 0U; item < 255U; item++) {
            if (!event_history_item_equal(&lhs[slot].Event_History_Items[item], &rhs[slot].Event_History_Items[item])) {
                return 0;
            }
        }
    }
    return 1;
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
    const Event_History* current = &event_history[0].Event_History_Items[0];
    rc |= expect_int("staged data PDU is cleared from current row", current->pdu[0], 0);
    rc |= expect_int("staged data text is cleared from current row", current->text_message[0], '\0');
    rc |= expect_int("staged data GPS is cleared from current row", current->gps_s[0], '\0');
    return rc;
}

static int
test_classified_control_notice_preserves_data_notice_behavior(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_DATA;

    Event_History* decoded = &event_history[0].Event_History_Items[0];
    decoded->pdu[0] = 0x56U;
    decoded->pdu[1] = 0x78U;
    DSD_SNPRINTF(decoded->text_message, sizeof(decoded->text_message), "%s", "registration payload");
    DSD_SNPRINTF(decoded->gps_s, sizeof(decoded->gps_s), "%s", "staged location");
    const uint64_t revision_before = event_history[0].revision;

    const dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_DMR_BS_DATA_POS, 0U, 1234U, 5678U);
    assert(
        dsd_event_emit_data_notice_classified(&opts, &state, 0U, &observation, DSD_EVENT_CATEGORY_CONTROL, "MNIS ARS;")
        == 0);

    const Event_History* committed = &event_history[0].Event_History_Items[1];
    const Event_History* current = &event_history[0].Event_History_Items[0];
    int rc = 0;
    rc |= expect_int("classified control category", committed->category, DSD_EVENT_CATEGORY_CONTROL);
    rc |= expect_int("classified control severity", committed->severity, DSD_EVENT_SEVERITY_INFO);
    rc |= expect_int("classified control source", (int)committed->source_id, 1234);
    rc |= expect_int("classified control target", (int)committed->target_id, 5678);
    rc |= expect_int("classified control first payload byte", committed->pdu[0], 0x56);
    rc |= expect_int("classified control second payload byte", committed->pdu[1], 0x78);
    rc |= expect_str_eq("classified control text payload", committed->text_message, "registration payload");
    rc |= expect_str_eq("classified control GPS payload", committed->gps_s, "staged location");
    rc |= expect_has_substr("classified control notice", committed->event_string, "MNIS ARS;");
    rc |= expect_int("classified control clears staged PDU", current->pdu[0], 0);
    rc |= expect_int("classified control clears staged text", current->text_message[0], '\0');
    rc |= expect_int("classified control clears staged GPS", current->gps_s[0], '\0');
    rc |= expect_u64("classified control revision behavior", event_history[0].revision, revision_before + 3U);
    rc |= expect_int("classified control emits data alert", g_beeper_count, 1);
    rc |= expect_int("classified control uses data tone", g_last_beeper_id, 80);
    rc |= expect_int("classified control emits frame log", g_frame_log_count, 1);
    rc |= expect_has_substr("classified control frame log text", g_last_frame_log, "MNIS ARS;");
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_classified_data_notice_rejects_invalid_categories_without_mutation(void) {
    static const dsd_event_category invalid_categories[] = {
        DSD_EVENT_CATEGORY_UNKNOWN,
        DSD_EVENT_CATEGORY_STATUS,
        DSD_EVENT_CATEGORY_VOICE,
        DSD_EVENT_CATEGORY_SYSTEM,
    };
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    static Event_History_I before[2];
    reset_fixture(&opts, &state, event_history);

    event_history[0].Event_History_Items[0].pdu[0] = 0xABU;
    DSD_SNPRINTF(event_history[0].Event_History_Items[0].text_message,
                 sizeof(event_history[0].Event_History_Items[0].text_message), "%s", "staged text");
    DSD_MEMCPY(before, event_history, sizeof(before));
    const dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_DMR_BS_DATA_POS, 0U, 1234U, 5678U);

    int rc = 0;
    for (size_t i = 0U; i < sizeof(invalid_categories) / sizeof(invalid_categories[0]); i++) {
        rc |= expect_int("invalid classified category rejected",
                         dsd_event_emit_data_notice_classified(&opts, &state, 0U, &observation, invalid_categories[i],
                                                               "Rejected notice;"),
                         -1);
        rc |= expect_int("invalid classified category leaves history unchanged",
                         event_histories_equal(before, event_history), 1);
    }
    rc |= expect_int("invalid classified category emits no alert", g_beeper_count, 0);
    rc |= expect_int("invalid classified category emits no frame log", g_frame_log_count, 0);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_data_notice_with_gps_owns_payload_without_consuming_active_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    Event_History* active = &event_history[0].Event_History_Items[0];
    active->pdu[0] = 0xABU;
    DSD_SNPRINTF(active->text_message, sizeof(active->text_message), "%s", "active call text");
    DSD_SNPRINTF(active->gps_s, sizeof(active->gps_s), "%s", "active call GPS");

    const dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_NXDN_POS, 0U, 1234U, 5678U);
    assert(dsd_event_emit_data_notice_with_gps(&opts, &state, 0U, &observation, "GPS SRC: 1234; TGT: 5678;",
                                               "41.500000 -87.250000")
           == 0);

    const Event_History* committed = &event_history[0].Event_History_Items[1];
    const Event_History* current = &event_history[0].Event_History_Items[0];
    int rc = 0;
    rc |= expect_str_eq("explicit data GPS", committed->gps_s, "41.500000 -87.250000");
    rc |= expect_int("explicit GPS event does not inherit active PDU", committed->pdu[0], 0);
    rc |= expect_int("explicit GPS event does not inherit active text", committed->text_message[0], '\0');
    rc |= expect_int("explicit GPS event category", committed->category, DSD_EVENT_CATEGORY_DATA);
    rc |= expect_int("explicit GPS event source", (int)committed->source_id, 1234);
    rc |= expect_int("explicit GPS event target", (int)committed->target_id, 5678);
    rc |= expect_int("explicit GPS preserves active PDU", current->pdu[0], 0xAB);
    rc |= expect_str_eq("explicit GPS preserves active text", current->text_message, "active call text");
    rc |= expect_str_eq("explicit GPS preserves active GPS", current->gps_s, "active call GPS");

    reset_fixture(&opts, &state, event_history);
    active = &event_history[0].Event_History_Items[0];
    active->pdu[0] = 0xCDU;
    DSD_SNPRINTF(active->text_message, sizeof(active->text_message), "%s", "control-active text");
    assert(dsd_event_emit_data_notice_classified_with_gps(&opts, &state, 0U, &observation, DSD_EVENT_CATEGORY_CONTROL,
                                                          "Control GPS;", "42.000000 -88.000000")
           == 0);

    committed = &event_history[0].Event_History_Items[1];
    current = &event_history[0].Event_History_Items[0];
    rc |= expect_int("classified GPS event category", committed->category, DSD_EVENT_CATEGORY_CONTROL);
    rc |= expect_str_eq("classified GPS event payload", committed->gps_s, "42.000000 -88.000000");
    rc |= expect_int("classified GPS event does not inherit active PDU", committed->pdu[0], 0);
    rc |= expect_int("classified GPS preserves active PDU", current->pdu[0], 0xCD);
    rc |= expect_str_eq("classified GPS preserves active text", current->text_message, "control-active text");
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
test_canonical_data_call_uses_data_metadata_without_voice_start_alert(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START;

    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P1_POS, DSD_CALL_KIND_DATA, 5678U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* current = &event_history[0].Event_History_Items[0];
    int rc = expect_int("canonical data should not beep as voice start", g_beeper_count, 0);
    rc |= expect_int("canonical data category", current->category, DSD_EVENT_CATEGORY_DATA);
    rc |= expect_int("canonical data group/private marker", current->gi, -1);
    dsd_state_ext_free_all(&state);
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
test_canonical_voice_category_is_protocol_neutral(void) {
    static const int protocols[] = {
        DSD_SYNC_P25P1_POS, DSD_SYNC_P25P2_POS,        DSD_SYNC_DMR_BS_VOICE_POS, DSD_SYNC_DMR_MS_VOICE,
        DSD_SYNC_NXDN_POS,  DSD_SYNC_X2TDMA_VOICE_POS, DSD_SYNC_PROVOICE_POS,     DSD_SYNC_EDACS_POS,
        DSD_SYNC_YSF_POS,   DSD_SYNC_DSTAR_VOICE_POS,  DSD_SYNC_DPMR_FS1_POS,     DSD_SYNC_M17_STR_POS,
    };
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];

    int rc = 0;
    for (size_t i = 0U; i < sizeof(protocols) / sizeof(protocols[0]); i++) {
        reset_fixture(&opts, &state, event_history);
        rc |= expect_int("canonical voice starts",
                         observe_test_call(&state, 0U, protocols[i], DSD_CALL_KIND_GROUP_VOICE, 1000U + i, 2000U + i,
                                           0U, 0U, DSD_CALL_BOUNDARY_BEGIN),
                         1);
        watchdog_event_current(&opts, &state, 0U);

        const Event_History* current = &event_history[0].Event_History_Items[0];
        rc |= expect_int("canonical voice protocol metadata", current->systype, protocols[i]);
        rc |= expect_int("canonical voice severity metadata", current->severity, DSD_EVENT_SEVERITY_INFO);
        rc |= expect_int("canonical voice category metadata", current->category, DSD_EVENT_CATEGORY_VOICE);
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_provisional_voice_identity_does_not_commit_zero_row(void) {
    static const int protocols[] = {
        DSD_SYNC_P25P1_POS,       DSD_SYNC_P25P2_POS,    DSD_SYNC_DMR_BS_VOICE_POS,
        DSD_SYNC_NXDN_POS,        DSD_SYNC_PROVOICE_POS, DSD_SYNC_YSF_POS,
        DSD_SYNC_DSTAR_VOICE_POS, DSD_SYNC_DPMR_FS1_POS, DSD_SYNC_M17_STR_POS,
    };
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];

    int rc = 0;
    for (size_t i = 0U; i < sizeof(protocols) / sizeof(protocols[0]); i++) {
        reset_fixture(&opts, &state, event_history);
        opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_VOICE_END;

        rc |= expect_int(
            "provisional call starts epoch",
            observe_test_call(&state, 0U, protocols[i], DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U, DSD_CALL_BOUNDARY_BEGIN),
            1);
        dsd_event_sync_slot(&opts, &state, 0U);

        dsd_call_snapshot snapshot;
        assert(dsd_call_state_get(&state, 0U, &snapshot) == 1);
        const uint64_t provisional_epoch = snapshot.epoch;
        rc |= expect_int("provisional call emits one start alert", g_beeper_count, 1);

        rc |= expect_int("identity begin specializes provisional epoch",
                         observe_test_call(&state, 0U, protocols[i], DSD_CALL_KIND_GROUP_VOICE, 1000U + i, 2000U + i,
                                           0U, 0U, DSD_CALL_BOUNDARY_BEGIN),
                         0);
        dsd_event_sync_slot(&opts, &state, 0U);
        assert(dsd_call_state_get(&state, 0U, &snapshot) == 1);
        rc |= expect_u64("identity begin preserves provisional epoch", snapshot.epoch, provisional_epoch);

        const Event_History* current = &event_history[0].Event_History_Items[0];
        const Event_History* prior = &event_history[0].Event_History_Items[1];
        rc |= expect_int("specialized current row has target", (int)current->target_id, (int)(1000U + i));
        rc |= expect_int("specialized current row has source", (int)current->source_id, (int)(2000U + i));
        rc |= expect_int("canonical voice row has protocol-neutral category", current->category,
                         DSD_EVENT_CATEGORY_VOICE);
        rc |= expect_int("specialization does not commit provisional row", prior->event_string[0], '\0');
        rc |= expect_int("specialization does not repeat start alert", g_beeper_count, 1);

        assert(dsd_call_state_end(&state, 0U, 3.0) == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        const Event_History* committed = &event_history[0].Event_History_Items[1];
        rc |= expect_int("final row keeps identified target", (int)committed->target_id, (int)(1000U + i));
        rc |= expect_int("final row keeps identified source", (int)committed->source_id, (int)(2000U + i));
        rc |= expect_int("no zero-only row remains after finalization",
                         event_history[0].Event_History_Items[2].event_string[0], '\0');
        rc |= expect_int("identified call emits one end alert", g_beeper_count, 2);
    }
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

// Count committed rows (index 0 stages the call still in progress).
static int
committed_history_rows(const Event_History_I* history) {
    int rows = 0;
    for (int i = 1; i < 255; i++) {
        if (history->Event_History_Items[i].event_string[0] != '\0') {
            rows++;
        }
    }
    return rows;
}

// Sync loss ends the canonical call mid-transmission; the next burst that decodes reopens the
// epoch and its end commits the same row again. One transmission must leave one row however
// many times it is closed and reopened, and must beep START and END once each.
//
// The reopen here carries no identity at all -- the shape of mark_vocoder_call_media(), which
// observes BEGIN on every MBE frame while no ACTIVE call exists. That path is the most common
// way a transmission is reacquired, so the arming must not depend on the boundary token.
static int
test_reacquired_transmission_commits_one_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    static max_align_t wav_sentinel;
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_VOICE_END;
    opts.wav_out_f = (SNDFILE*)&wav_sentinel;
    g_open_wav_result = (SNDFILE*)&wav_sentinel;

    for (int pass = 0; pass < 3; pass++) {
        if (pass == 0) {
            assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U,
                                     0U, DSD_CALL_BOUNDARY_BEGIN)
                   == 1);
        } else {
            // Identity-less voice BEGIN, exactly as the vocoder emits it.
            assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                                     DSD_CALL_BOUNDARY_BEGIN)
                   == 1);
        }
        dsd_event_sync_slot(&opts, &state, 0U);
        assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
    }

    int rc = expect_int("reacquired transmission commits one row", committed_history_rows(&event_history[0]), 1);
    // Exactly one START, at the true start of the transmission. No END yet: every end so far was
    // a sync loss the call could still come back from, so announcing the end mid-transmission
    // would be wrong. The alert is held until the reacquisition window closes.
    rc |= expect_int("reacquired transmission alerts START once", g_beeper_count, 1);
    rc |= expect_int("reacquired transmission keeps committed target",
                     (int)event_history[0].Event_History_Items[1].target_id, 100);
    // The identity-less reopens inherit the ending call's identity instead of blanking it.
    rc |= expect_int("reacquired transmission keeps committed source",
                     (int)event_history[0].Event_History_Items[1].source_id, 200);
    // Rotation lives in the commit path and the rename metadata comes from the staged row, so a
    // merged transmission is one row referencing one recording per segment.
    rc |= expect_int("each reacquired segment finalizes its WAV", g_close_wav_count, 3);
    rc |= expect_int("each reacquired segment opens a fresh WAV", g_open_wav_count, 3);

    // A genuinely different call taking the slot proves the transmission really is over, so the
    // held END is retired first and the new call's START follows it.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 500U, 900U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("held END lands before the next call's START", g_beeper_count, 3);
    rc |= expect_int("a different call opens its own row", committed_history_rows(&event_history[0]), 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

// A DMR fade at the tail of a transmission ends the epoch by sync loss; the terminator that
// explains it decodes a moment later, after the epoch is already ENDED. That terminator is
// positive evidence the transmission is over, so it has to retract the reacquisition permission
// the sync-loss end granted -- otherwise a second PTT on the same identity inside the window
// (a routine double-tap) folds into the terminated call's row and loses its START. The held
// VOICE_END must also land at the terminator rather than waiting out the full window.
static int
test_terminator_after_sync_loss_end_blocks_reacquisition(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_VOICE_END;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    int rc = expect_int("call start alerts once", g_beeper_count, 1);

    // The fade. noCarrier() ends the epoch; the VOICE_END is held because this could still be the
    // middle of the transmission.
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("sync-loss end holds the END alert", g_beeper_count, 1);
    rc |= expect_int("sync-loss end commits the row", committed_history_rows(&event_history[0]), 1);

    // The Terminator-with-LC, arriving after the epoch already ended. dmr_flco_prepare_regular_state()
    // reaches dsd_call_state_end() with the slot in ENDED, so the reason is tightened in place.
    rc |= expect_int("terminator upgrades an already-ended epoch", end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("terminator retires the held END immediately", g_beeper_count, 2);
    {
        dsd_call_snapshot call;
        assert(dsd_call_state_get(&state, 0U, &call) == 1);
        rc |= expect_int("terminator leaves the epoch ENDED", (int)call.phase, (int)DSD_CALL_PHASE_ENDED);
        rc |= expect_int("terminator clears the sync-loss reason", (int)call.end_reason, (int)DSD_CALL_END_EXPLICIT);
    }

    // Second PTT on the same TG/SRC, well inside DSD_CALL_REACQUIRE_GAP_S. Positive termination
    // has already been decoded, so this is a new transmission and gets its own row and START.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("post-terminator PTT alerts its own START", g_beeper_count, 3);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("post-terminator PTT commits its own row", committed_history_rows(&event_history[0]), 2);

    dsd_state_ext_free_all(&state);
    return rc;
}

// The upgrade above is one-directional. A sync loss that follows an explicit teardown must not be
// able to re-arm reacquisition on an epoch that was already positively terminated, and no end
// reason may restamp ended_m once the epoch is closed -- the repeated noCarrier() calls that fire
// while unsynced would otherwise walk the reacquisition window forward indefinitely.
static int
test_end_reason_upgrade_is_one_directional(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_VOICE_END;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);

    dsd_call_snapshot after_explicit;
    assert(dsd_call_state_get(&state, 0U, &after_explicit) == 1);

    // Sync loss reported after the terminator: no-op, exactly as before.
    int rc =
        expect_int("sync loss after explicit end is a no-op", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 0);
    // A repeated terminator is also a no-op: the reason already is EXPLICIT.
    rc |= expect_int("repeated explicit end is a no-op", end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT), 0);

    dsd_call_snapshot after_repeats;
    assert(dsd_call_state_get(&state, 0U, &after_repeats) == 1);
    rc |= expect_int("explicit end reason survives a later sync loss", (int)after_repeats.end_reason,
                     (int)DSD_CALL_END_EXPLICIT);
    rc |=
        expect_int("repeated ends do not restamp ended_m", after_repeats.ended_m == after_explicit.ended_m ? 1 : 0, 1);

    // And the upgrade path itself must not restamp: the moment the transmission stopped is the
    // fade, not the terminator that explained it.
    reset_fixture(&opts, &state, event_history);
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_call_snapshot after_fade;
    assert(dsd_call_state_get(&state, 0U, &after_fade) == 1);
    advance_test_clock(0.2);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_call_snapshot after_upgrade;
    assert(dsd_call_state_get(&state, 0U, &after_upgrade) == 1);
    rc |= expect_int("upgrade keeps ended_m anchored at the fade", after_upgrade.ended_m == after_fade.ended_m ? 1 : 0,
                     1);

    dsd_state_ext_free_all(&state);
    return rc;
}

// The reacquisition test above relies on the ending epoch naming a call. The inverse must not
// coalesce: an identity-less epoch is compatible with every observation, so if it were allowed to
// reacquire, the next unrelated call on the slot would be folded into its row and lose its START.
static int
test_identityless_ended_epoch_does_not_reacquire(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START;

    // Provisional voice epoch with no identity at all -- mark_vocoder_call_media() before any
    // header decodes -- ended by sync loss.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    // A completely different call, well inside the reacquisition window.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 500U, 900U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_int("unrelated call after an identity-less end gets its own row",
                        committed_history_rows(&event_history[0]), 2);
    rc |=
        expect_int("unrelated call keeps its own target", (int)event_history[0].Event_History_Items[1].target_id, 500);
    rc |= expect_int("unrelated call still alerts START", g_beeper_count, 2);
    dsd_state_ext_free_all(&state);
    return rc;
}

// Route text counts as identity when deciding whether an ended epoch is anchored enough to be
// reacquired, so it has to count when deciding whether an observation contradicts that epoch too.
// A late-entry D-STAR call that learned only its repeater pair would otherwise be an anchor nothing
// could reject, and the next call through a different repeater -- or an identity-less vocoder mark
// -- would be folded into its row.
static int
test_route_only_identity_does_not_reacquire_unrelated_call(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START;
    state.lastsynctype = DSD_SYNC_DSTAR_VOICE_POS;

    dsd_call_observation observation = {
        .protocol = DSD_SYNC_DSTAR_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    DSD_SNPRINTF(observation.route_text[0], sizeof(observation.route_text[0]), "%s", "RPT1AAA");
    DSD_SNPRINTF(observation.route_text[1], sizeof(observation.route_text[1]), "%s", "RPT2AAA");
    observation.observed_m = g_observed_m;
    g_observed_m += 0.1;
    assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(committed_history_rows(&event_history[0]) == 1);

    // A different transmission through a different repeater pair, well inside the window.
    dsd_call_observation other = observation;
    DSD_SNPRINTF(other.route_text[0], sizeof(other.route_text[0]), "%s", "RPT1BBB");
    DSD_SNPRINTF(other.route_text[1], sizeof(other.route_text[1]), "%s", "RPT2BBB");
    other.observed_m = g_observed_m;
    g_observed_m += 0.1;
    assert(dsd_call_state_observe(&state, &other, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_int("a different route commits its own row", committed_history_rows(&event_history[0]), 2);
    rc |= expect_int("the second call alerts its own START", g_beeper_count, 2);
    dsd_state_ext_free_all(&state);
    return rc;
}

// The same route that ended is still one transmission resuming, so the anchor must keep working in
// the direction it was added for. Guards the fix above against being over-applied.
static int
test_route_identity_reacquisition_still_coalesces(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_DSTAR_VOICE_POS;

    dsd_call_observation observation = {
        .protocol = DSD_SYNC_DSTAR_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    DSD_SNPRINTF(observation.route_text[0], sizeof(observation.route_text[0]), "%s", "RPT1AAA");
    DSD_SNPRINTF(observation.route_text[1], sizeof(observation.route_text[1]), "%s", "RPT2AAA");

    for (int pass = 0; pass < 2; pass++) {
        observation.observed_m = g_observed_m;
        g_observed_m += 0.1;
        assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
    }

    int rc = expect_int("the same route reacquires into one row", committed_history_rows(&event_history[0]), 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

// The gap test compares an end against a reopen on one clock, but only one side of that comparison
// reads the clock itself: every production end site passes a timeline derived from
// dsd_time_now_monotonic_s(), while a reopening observation usually passes 0.0 and takes the
// fallback. If the fallback truncates to whole milliseconds it can land *behind* an end stamped at
// nanosecond resolution moments earlier, and the reacquisition is rejected for going backwards --
// committing a second row and a spurious START for one transmission.
//
// Bracketing the fallback between two reads of the same clock catches exactly that: a truncating
// fallback can fall up to a millisecond below the lower bound, while a full-resolution one can
// never leave the bracket.
static int
test_observed_fallback_matches_end_site_clock_resolution(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const double before = dsd_time_now_monotonic_s();
    // observed_m of 0.0 is what the protocol end paths pass, so this takes the fallback.
    assert(dsd_call_state_end_ex(&state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS) == 1);
    const double after = dsd_time_now_monotonic_s();

    dsd_call_snapshot ended;
    assert(dsd_call_state_get(&state, 0U, &ended) > 0);
    int rc = expect_int("the fallback clock is not behind the end-site clock", ended.ended_m >= before, 1);
    rc |= expect_int("the fallback clock is not ahead of the end-site clock", ended.ended_m <= after, 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

// The re-announcing protocols (P25 Phase 2, NXDN, dPMR, D-STAR, EDACS, X2-TDMA, DMR embedded
// LCs) reopen with CONTINUE. They must arm the same way the identity-less BEGIN path does.
static int
test_reacquired_transmission_via_continue_commits_one_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_int("continue reacquisition commits one row", committed_history_rows(&event_history[0]), 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

// A deliberate teardown is not a reacquisition. A subscriber double-tapping PTT on the same
// talkgroup is routine on trunked P25 Phase 2 and DMR, and each press is its own transmission.
static int
test_back_to_back_same_identity_calls_commit_two_rows(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_END;

    for (int pass = 0; pass < 2; pass++) {
        assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                                 DSD_CALL_BOUNDARY_BEGIN)
               == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
    }

    int rc =
        expect_int("back-to-back same-identity calls commit two rows", committed_history_rows(&event_history[0]), 2);
    rc |= expect_int("back-to-back calls each emit a call end alert", g_beeper_count, 2);
    rc |= expect_int("newest same-identity row keeps target", (int)event_history[0].Event_History_Items[1].target_id,
                     100);
    rc |=
        expect_int("prior same-identity row keeps target", (int)event_history[0].Event_History_Items[2].target_id, 100);
    dsd_state_ext_free_all(&state);
    return rc;
}

// A terminator followed by an identity-less reopen inside the window must still commit twice:
// the transmission that terminated is over, whatever decodes next is new.
static int
test_explicit_end_then_identityless_reopen_commits_two_rows(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_int("explicit end never coalesces", committed_history_rows(&event_history[0]), 2);
    dsd_state_ext_free_all(&state);
    return rc;
}

// The merge must not swallow a different talker or a different target.
static int
test_changed_identity_still_commits_its_own_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_int("changed source commits its own row", committed_history_rows(&event_history[0]), 2);
    rc |= expect_int("newest row carries the new source", (int)event_history[0].Event_History_Items[1].source_id, 201);
    rc |= expect_int("prior row keeps the first source", (int)event_history[0].Event_History_Items[2].source_id, 200);
    dsd_state_ext_free_all(&state);
    return rc;
}

// A gap wider than the window is two transmissions, whatever ended the first one.
static int
test_reacquisition_gap_beyond_window_commits_two_rows(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    advance_test_clock(1.0);
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_int("gap beyond the window commits two rows", committed_history_rows(&event_history[0]), 2);
    dsd_state_ext_free_all(&state);
    return rc;
}

// The window bounds the gap, not the transmission. A reacquired segment that then runs for
// well over the window is still one transmission.
static int
test_reacquired_segment_may_outlast_the_window(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    advance_test_clock(1.2);
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 0);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_int("long reacquired segment commits one row", committed_history_rows(&event_history[0]), 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

// A badly flapping signal produces many short segments. They belong to one transmission.
static int
test_flapping_segments_commit_one_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    for (int pass = 0; pass < 5; pass++) {
        assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS,
                                 pass == 0 ? DSD_CALL_KIND_GROUP_VOICE : DSD_CALL_KIND_VOICE, pass == 0 ? 100U : 0U,
                                 pass == 0 ? 200U : 0U, 0U, 0U, DSD_CALL_BOUNDARY_BEGIN)
               == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        advance_test_clock(0.2);
        assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        advance_test_clock(0.1);
    }

    int rc = expect_int("five flapping segments commit one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_int("flapping transmission keeps its target", (int)event_history[0].Event_History_Items[1].target_id,
                     100);
    dsd_state_ext_free_all(&state);
    return rc;
}

// DMR alternates sync polarity burst to burst. Comparing raw synctypes would call that a
// different call; the store compares protocol families instead.
static int
test_dmr_sync_polarity_flip_still_coalesces(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_NEG, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_int("dmr polarity flip commits one row", committed_history_rows(&event_history[0]), 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

// M17, YSF, D-STAR and dPMR express identity as text and leave the numeric ids at zero. A
// numeric-only comparison would refuse to coalesce for all four.
static int
test_textual_identity_reacquisition_coalesces(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_M17_STR_POS;

    dsd_call_observation observation = {
        .protocol = DSD_SYNC_M17_STR_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    DSD_SNPRINTF(observation.source_text, sizeof(observation.source_text), "%s", "N0CALL");
    DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", "BROADCAST");

    for (int pass = 0; pass < 2; pass++) {
        observation.observed_m = g_observed_m;
        g_observed_m += 0.1;
        assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
    }

    int rc = expect_int("textual identity reacquisition commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_str_eq("merged row keeps textual source", event_history[0].Event_History_Items[1].src_str, "N0CALL");
    dsd_state_ext_free_all(&state);
    return rc;
}

// Late entry commits a first segment knowing only the talkgroup. When the reacquired segment
// decodes the source, the surviving row has to carry it -- dropping the second commit would
// leave a row reading SRC 00000000 forever.
static int
test_reacquired_segment_contributes_late_source(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(event_history[0].Event_History_Items[1].source_id == 0U);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* merged = &event_history[0].Event_History_Items[1];
    int rc = expect_int("late source merges into one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_int("merged row carries the late source", (int)merged->source_id, 201);
    rc |= expect_has_substr("merged row renders the late source", merged->event_string, "SRC: 00000201;");
    dsd_state_ext_free_all(&state);
    return rc;
}

// The PI/ESS header can decode only after the reacquisition. Dropping that commit would leave
// the sole surviving row marked clear for a call that was encrypted.
static int
test_reacquired_segment_contributes_crypto(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(event_history[0].Event_History_Items[1].enc == 0U);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    assert(update_test_crypto(&state, 0U, DSD_CALL_CRYPTO_ENCRYPTED, 0x21U, 0x1234U, 0U) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* merged = &event_history[0].Event_History_Items[1];
    int rc = expect_int("late crypto merges into one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_int("merged row is marked encrypted", merged->enc, 1);
    rc |= expect_int("merged row keeps the algorithm", merged->enc_alg, 0x21);
    rc |= expect_int("merged row keeps the key id", (int)merged->enc_key, 0x1234);
    rc |= expect_has_substr("merged row renders the crypto", merged->event_string, "ENC; ALG: 21; KID: 1234;");
    dsd_state_ext_free_all(&state);
    return rc;
}

// Alias and GPS learned during the reacquired segment must reach the surviving row.
static int
test_reacquired_segment_contributes_alias_and_gps(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_call_snapshot reacquired;
    assert(dsd_call_state_get(&state, 0U, &reacquired) > 0);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(dsd_event_enrich_alias(&state, 0U, reacquired.epoch, "UNIT 12") == 1);
    assert(dsd_event_enrich_gps(&state, 0U, reacquired.epoch, "lat 1.0 lon 2.0") == 1);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* merged = &event_history[0].Event_History_Items[1];
    int rc = expect_int("enriched reacquisition commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_str_eq("merged row keeps the alias", merged->alias, "UNIT 12");
    rc |= expect_str_eq("merged row keeps the gps", merged->gps_s, "lat 1.0 lon 2.0");
    dsd_state_ext_free_all(&state);
    return rc;
}

// A data notice pushed between the two voice commits shifts the ring. The merge has to find
// the voice row at its new depth and leave the notice alone.
static int
test_interleaved_data_notice_does_not_misdirect_merge(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    // Late entry: the first segment knows the talkgroup only.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    assert(emit_test_data_notice(&opts, &state, 300U, 400U, "LRRP SRC: 300; (1.0, 2.0)", 0U) == 0);
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    // Row 1 is the notice; row 2 is the voice row the reacquisition merged into.
    int rc = expect_int("notice plus merged voice leave two rows", committed_history_rows(&event_history[0]), 2);
    rc |= expect_has_substr("notice row is untouched", event_history[0].Event_History_Items[1].event_string, "LRRP");
    rc |= expect_int("merged voice row keeps its target", (int)event_history[0].Event_History_Items[2].target_id, 100);
    rc |=
        expect_int("merged voice row learned the source", (int)event_history[0].Event_History_Items[2].source_id, 201);
    dsd_state_ext_free_all(&state);
    return rc;
}

// Clearing Activity history mid-call must not leave the next segment merging into a row that
// no longer exists: the transmission still has to reach history.
static int
test_history_reset_mid_reacquisition_still_commits(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    dsd_event_history_reset(&state);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_int("reset then reacquisition still reaches history", committed_history_rows(&event_history[0]), 1);
    rc |= expect_int("row after reset keeps its target", (int)event_history[0].Event_History_Items[1].target_id, 100);
    dsd_state_ext_free_all(&state);
    return rc;
}

// Clearing history also clears the bookkeeping that points into it.
static int
test_history_reset_clears_commit_bookkeeping(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(committed_history_rows(&event_history[0]) == 1);

    dsd_event_history_reset(&state);

    int rc = expect_int("reset clears every row", committed_history_rows(&event_history[0]), 0);
    dsd_call_context_snapshot context;
    assert(dsd_call_context_copy_snapshot(&state, &context) > 0);
    rc |= expect_int("reset clears committed_valid", context.events[0].committed_valid, 0);
    rc |= expect_u64("reset clears committed_seq", context.events[0].committed_seq, 0U);
    rc |= expect_u64("reset clears reacquired_epoch", context.events[0].reacquired_epoch, 0U);
    dsd_state_ext_free_all(&state);
    return rc;
}

// Both halves of the render-environment pair have to go when the lifecycle is invalidated. A row
// staged directly by a protocol never passes through the renderer, so a staged_env left behind by
// the reset would be promoted into committed_env when that row commits, and a later merge would
// re-render the row against a decoder context from before the operator cleared history.
static int
test_history_reset_clears_staged_environment(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    state.nxdn_grant_chan = 12U;
    state.nxdn_grant_freq = 851012500;
    assert(observe_test_call(&state, 0U, DSD_SYNC_NXDN_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    dsd_call_context_snapshot before;
    assert(dsd_call_context_copy_snapshot(&state, &before) > 0);
    assert(before.events[0].staged_env.nxdn_grant_chan == 12U);

    dsd_event_history_reset(&state);

    dsd_call_context_snapshot after;
    assert(dsd_call_context_copy_snapshot(&state, &after) > 0);
    int rc =
        expect_int("reset clears the staged render environment", (int)after.events[0].staged_env.nxdn_grant_chan, 0);
    rc |= expect_int("reset clears the committed render environment",
                     (int)after.events[0].committed_env.nxdn_grant_chan, 0);
    dsd_state_ext_free_all(&state);
    return rc;
}

// The reacquisition marker names the epoch it belongs to and must survive until that epoch's
// staged row is flushed. If a later, different call clears it first, the staged row commits a
// second time and the reacquired transmission ends up with two rows -- the exact duplicate this
// whole path exists to prevent.
static int
test_reacquired_stage_superseded_by_new_call_still_merges(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    // Segment 1 commits its row.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    // Segment 2 reopens and stages, but is superseded by a different call before it ends.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 500U, 900U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    // Segment 2 folded into segment 1's row; the new call got its own. Two rows, not three.
    int rc = expect_int("superseded reacquisition does not duplicate", committed_history_rows(&event_history[0]), 2);
    rc |= expect_int("new call owns the newest row", (int)event_history[0].Event_History_Items[1].target_id, 500);
    rc |= expect_int("reacquired transmission still owns one row",
                     (int)event_history[0].Event_History_Items[2].target_id, 100);
    dsd_state_ext_free_all(&state);
    return rc;
}

// An epoch that ends without pushing a row leaves the commit reference pointing at an older
// epoch. A later reacquisition of that empty epoch must not fold into the unrelated row it still
// names -- the merge target has to be the row the reopened epoch itself committed.
//
// X2-TDMA is the vehicle: it has no per-protocol event builder, so its rows render no string and
// have no content to commit. That makes it the reachable shape of "an epoch that committed
// nothing", which is otherwise hard to construct.
static int
test_reacquisition_after_uncommitted_epoch_does_not_merge_stale_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    // Call A is DMR and commits a row.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(committed_history_rows(&event_history[0]) == 1);

    // Call B renders nothing, so its sync-loss end commits no row and the slot's commit
    // reference still names call A.
    assert(observe_test_call(&state, 0U, DSD_SYNC_X2TDMA_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 300U, 400U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(committed_history_rows(&event_history[0]) == 1);

    // B is reacquired inside the window. Whatever it contributes must not land in call A's row.
    assert(observe_test_call(&state, 0U, DSD_SYNC_X2TDMA_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 300U, 400U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_call_snapshot reacquired;
    assert(dsd_call_state_get(&state, 0U, &reacquired) > 0);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(dsd_event_enrich_alias(&state, 0U, reacquired.epoch, "B UNIT") == 1);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    // Two rows: call A's, and a fresh one for the reacquired B that had nothing to merge into.
    // Counted by identity rather than by rendered string, since an X2-TDMA row renders none.
    const Event_History* newest = &event_history[0].Event_History_Items[1];
    const Event_History* call_a = &event_history[0].Event_History_Items[2];
    int rc = expect_int("the reacquired segment gets its own row", (int)newest->target_id, 300);
    rc |= expect_str_eq("the reacquired segment keeps its alias", newest->alias, "B UNIT");
    // The decisive assertions: call A's row is untouched by a transmission that is not its own.
    rc |= expect_int("call A keeps its identity", (int)call_a->target_id, 100);
    rc |= expect_str_eq("call A's row does not absorb the reacquired segment", call_a->alias, "");
    dsd_state_ext_free_all(&state);
    return rc;
}

// A restored context describes a different trunk-scan target while the event history is
// global, so a commit reference must never survive the hop.
static int
test_context_restore_invalidates_commit_bookkeeping(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    dsd_call_context_snapshot saved;
    assert(dsd_call_context_copy_snapshot(&state, &saved) > 0);
    assert(saved.events[0].committed_valid == 1U);
    // The end reason rides along with the snapshot, so a hop cannot turn a sync loss into a
    // teardown or the other way round.
    int rc =
        expect_int("snapshot preserves the end reason", saved.calls.slots[0].end_reason, (int)DSD_CALL_END_SYNC_LOSS);

    assert(dsd_call_context_restore_snapshot(&state, &saved) > 0);
    dsd_call_context_snapshot restored;
    assert(dsd_call_context_copy_snapshot(&state, &restored) > 0);
    rc |= expect_int("restore clears committed_valid", restored.events[0].committed_valid, 0);
    rc |= expect_u64("restore clears committed_seq", restored.events[0].committed_seq, 0U);
    rc |= expect_u64("restore clears reacquired_epoch", restored.events[0].reacquired_epoch, 0U);
    // Clearing the commit reference blocks the row merge, but reacquisition has two other
    // effects -- seeding the new epoch with the old identity and suppressing its START alert --
    // that a bare commit invalidation would not stop. The monotonic clock is global, so a slot
    // saved mid-fade would still satisfy the gap test against a call on the new target. A
    // restored end is a hop, never a resumable fade, so it comes back as a deliberate teardown.
    rc |= expect_int("restore downgrades a sync-loss end to explicit", restored.calls.slots[0].end_reason,
                     (int)DSD_CALL_END_EXPLICIT);
    dsd_state_ext_free_all(&state);
    return rc;
}

// The first segment logs its line as usual. A merge that materially changes the rendered row
// adds a continuation line; a merge that changes nothing user-visible stays silent.
static int
test_merge_logs_continuation_only_when_render_changes(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    char path[] = "/tmp/dsd-neo-reacquire-events-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        DSD_FPRINTF(stderr, "mkstemp failed for reacquisition event log test\n");
        return 1;
    }
    close(fd);
    (void)remove(path);
    DSD_SNPRINTF(opts.event_out_file, sizeof opts.event_out_file, "%s", path);

    // Segment 1 knows the talkgroup only; segment 2 decodes the source, so the render changes.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    // Segment 3 adds nothing the row does not already say.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        (void)remove(path);
        DSD_FPRINTF(stderr, "reacquisition event log was not created\n");
        return 1;
    }
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    (void)remove(path);
    buf[n] = '\0';

    int reacquired_lines = 0;
    for (const char* cursor = strstr(buf, " Reacquired: "); cursor != NULL;
         cursor = strstr(cursor + 1, " Reacquired: ")) {
        reacquired_lines++;
    }

    int rc = expect_int("merged transmission commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_has_substr("first segment logged its own line", buf, "SRC: 00000000;");
    rc |= expect_has_substr("continuation reports the learned source", buf, " Reacquired: ");
    rc |= expect_int("only the informative merge logs a continuation", reacquired_lines, 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

// The slot annotation on a continuation describes the row being continued, so it has to come from
// that row rather than from the live decoder. A DMR-BS transmission reacquired after the decoder
// has resynced elsewhere -- or after no_carrier_reset_decode_state() cleared lastsynctype -- would
// otherwise log its two halves with different annotations.
static int
test_merge_continuation_annotates_from_the_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    char path[] = "/tmp/dsd-neo-continuation-slot-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        DSD_FPRINTF(stderr, "mkstemp failed for continuation slot test\n");
        return 1;
    }
    close(fd);
    (void)remove(path);
    DSD_SNPRINTF(opts.event_out_file, sizeof opts.event_out_file, "%s", path);

    // Slot 1 of a DMR-BS call: the first commit is annotated "Slot 2;".
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    assert(observe_test_call(&state, 1U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 1U);
    assert(end_test_call(&state, 1U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 1U);

    // The decoder loses the system entirely across the gap, as noCarrier() leaves it.
    state.lastsynctype = DSD_SYNC_NONE;
    assert(observe_test_call(&state, 1U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 1U);
    assert(end_test_call(&state, 1U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 1U);

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        (void)remove(path);
        DSD_FPRINTF(stderr, "continuation slot event log was not created\n");
        return 1;
    }
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    (void)remove(path);
    buf[n] = '\0';

    const char* continuation = strstr(buf, " Reacquired: ");
    int rc = expect_int("the reacquired segment merged into one row", committed_history_rows(&event_history[1]), 1);
    if (continuation == NULL) {
        DSD_FPRINTF(stderr, "expected a continuation line in the event log\n");
        rc |= 1;
    } else {
        rc |= expect_has_substr("continuation keeps the row's slot annotation", continuation, "Slot 2;");
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

// Optional detail a reacquired segment contributes reaches the event log even when it does not
// change the rendered row -- otherwise it would show in the UI history and be missing from the
// log. The continuation also carries the slot annotation that every normal commit carries.
static int
test_merge_logs_metadata_the_segment_added(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    // Two-slot system, so commits carry a "Slot N;" annotation.
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;

    char path[] = "/tmp/dsd-neo-reacquire-meta-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        DSD_FPRINTF(stderr, "mkstemp failed for reacquisition metadata log test\n");
        return 1;
    }
    close(fd);
    (void)remove(path);
    DSD_SNPRINTF(opts.event_out_file, sizeof opts.event_out_file, "%s", path);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    // The reacquired segment adds an alias and a GPS fix but no new identity, so the rendered
    // row is unchanged and only the metadata lines have anything to report.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_call_snapshot reacquired;
    assert(dsd_call_state_get(&state, 0U, &reacquired) > 0);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(dsd_event_enrich_alias(&state, 0U, reacquired.epoch, "UNIT 12 FIRE") == 1);
    assert(dsd_event_enrich_gps(&state, 0U, reacquired.epoch, "lat 3.0 lon 4.0") == 1);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        (void)remove(path);
        DSD_FPRINTF(stderr, "reacquisition metadata log was not created\n");
        return 1;
    }
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    (void)remove(path);
    buf[n] = '\0';

    int rc = expect_int("metadata-only merge commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_has_substr("merged alias reaches the log", buf, " Talker Alias: UNIT 12 FIRE");
    rc |= expect_has_substr("merged gps reaches the log", buf, " GPS: lat 3.0 lon 4.0");
    rc |= expect_str_eq("merged row keeps the alias", event_history[0].Event_History_Items[1].alias, "UNIT 12 FIRE");
    dsd_state_ext_free_all(&state);
    return rc;
}

// A partially decoded alias must be upgraded by a later segment that decoded more of it, the way
// enrichment upgrades a committed row. Filling only blanks would freeze the first fragment.
static int
test_merge_upgrades_partial_alias(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_call_snapshot first;
    assert(dsd_call_state_get(&state, 0U, &first) > 0);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(dsd_event_enrich_alias(&state, 0U, first.epoch, "UNIT") == 1);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(expect_str_eq("first segment logs the partial alias", event_history[0].Event_History_Items[1].alias, "UNIT")
           == 0);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_call_snapshot second;
    assert(dsd_call_state_get(&state, 0U, &second) > 0);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(dsd_event_enrich_alias(&state, 0U, second.epoch, "UNIT 12 FIRE") == 1);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_int("alias upgrade commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_str_eq("merge takes the fuller alias", event_history[0].Event_History_Items[1].alias, "UNIT 12 FIRE");
    dsd_state_ext_free_all(&state);
    return rc;
}

// System identifiers decoded only by a later segment have to reach the merged row. The first
// segment's placeholder sysid_string is non-empty, so a fill-if-blank merge would strand both
// the string and the numeric ids that structured consumers read.
static int
test_merge_carries_late_system_identifiers(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    // Late entry: no NAC/WACN yet, so the row renders from all-zero system ids.
    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P1_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(event_history[0].Event_History_Items[1].sys_id1 == 0U);

    // The reacquired segment decodes the network status.
    state.p2_wacn = 0xBEE00U;
    state.p2_sysid = 0x123U;
    state.nac = 0x321U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P1_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* merged = &event_history[0].Event_History_Items[1];
    int rc = expect_int("late system identity commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_int("merged row gains the wacn", (int)merged->sys_id1, 0xBEE00);
    rc |= expect_int("merged row gains the sysid", (int)merged->sys_id2, 0x123);
    rc |= expect_int("merged row gains the nac", (int)merged->sys_id3, 0x321);
    // The rendered string follows the ids rather than keeping the placeholder.
    rc |= expect_has_substr("merged row renders the network status", merged->event_string, "NET_STS:");
    dsd_state_ext_free_all(&state);
    return rc;
}

// A canonical notice raised during a reacquired segment describes the transmission already in
// history, so it must fold into that row rather than pushing a second one.
static int
test_notice_during_reacquisition_merges(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(committed_history_rows(&event_history[0]) == 1);

    // The segment reopens and only then is the call found to be encrypted.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    dsd_call_snapshot reacquired;
    assert(dsd_call_state_get(&state, 0U, &reacquired) > 0);
    assert(dsd_event_emit_call_notice(&opts, &state, 0U, &reacquired, "ENC LO") == 1);

    int rc = expect_int("notice during reacquisition does not duplicate the row",
                        committed_history_rows(&event_history[0]), 1);
    rc |= expect_str_eq("merged row carries the notice detail", event_history[0].Event_History_Items[1].internal_str,
                        "ENC LO");
    rc |= expect_int("merged row keeps its identity", (int)event_history[0].Event_History_Items[1].target_id, 100);
    dsd_state_ext_free_all(&state);
    return rc;
}

// The notice above merges and, in doing so, makes the reacquired epoch the owner of the committed
// row. When that same epoch later ends it commits a second time, and that commit has to land in the
// row the epoch already owns. Matching only the interrupted epoch's row would reject it and push a
// duplicate -- one transmission, two history rows, which is what the merge path exists to prevent.
static int
test_notice_then_end_in_reacquired_epoch_commits_one_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(committed_history_rows(&event_history[0]) == 1);

    // The segment is reacquired and a notice fires mid-segment, merging into the committed row.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    dsd_call_snapshot reacquired;
    assert(dsd_call_state_get(&state, 0U, &reacquired) > 0);
    assert(dsd_event_emit_call_notice_nonfinalizing(&opts, &state, 0U, &reacquired, "ENC LO") == 1);
    assert(committed_history_rows(&event_history[0]) == 1);

    // The merge cleared the staged row, so the segment keeps decoding and stages it again. Ending
    // then commits a second time within the one epoch.
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_int("notice then end in one reacquired epoch commits one row",
                        committed_history_rows(&event_history[0]), 1);
    rc |= expect_str_eq("surviving row keeps the notice detail", event_history[0].Event_History_Items[1].internal_str,
                        "ENC LO");
    rc |= expect_int("surviving row keeps its identity", (int)event_history[0].Event_History_Items[1].target_id, 100);
    dsd_state_ext_free_all(&state);
    return rc;
}

// A sync-loss end whose staged row rendered nothing put no row in history, so there is no
// transmission for a VOICE_END to be about. The FINAL disposition cannot alert in that case --
// its beep lives inside the commit -- and the deferred one must not either. X2-TDMA is the
// concrete case: no builder covers it, so its rows render empty.
static int
test_contentless_sync_loss_end_arms_no_alert(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_VOICE_END;
    state.lastsynctype = DSD_SYNC_X2TDMA_VOICE_POS;

    assert(observe_test_call(&state, 0U, DSD_SYNC_X2TDMA_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    const int start_alerts = g_beeper_count;

    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_int("a row that renders nothing commits nothing", committed_history_rows(&event_history[0]), 0);
    // Asserted on the lifecycle rather than by waiting out the deadline: the alert is held against
    // the real monotonic clock, which the fixture's timeline does not drive.
    dsd_call_context_snapshot context;
    assert(dsd_call_context_copy_snapshot(&state, &context) > 0);
    rc |= expect_int("no VOICE_END is held for a transmission with no row", context.events[0].end_alert_pending, 0);
    rc |= expect_int("nothing beeped past the START", g_beeper_count, start_alerts);
    dsd_state_ext_free_all(&state);
    return rc;
}

// A row committed while reading a file may carry no stamped event_time -- the replay timeline
// supplies it, and it is legitimately absent when that timeline has no timestamp. A merge must
// then recover the prefix the row already displays rather than restamping it from epoch zero.
static int
test_merge_without_event_time_keeps_the_row_timestamp(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.playfiles = 1; // as -r does: event_time is the replay timeline's to set, not the clock's

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(event_history[0].Event_History_Items[1].event_time == 0);

    // Segment 2 learns the source, so the row is re-rendered.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* merged = &event_history[0].Event_History_Items[1];
    int rc = expect_int("replayed merge commits one row", committed_history_rows(&event_history[0]), 1);
    // The stub clock renders 2026-04-30; an event_time of 0 would render 1970-01-01 instead.
    rc |= expect_has_substr("merged row keeps its original date", merged->event_string, "2026-04-30");
    rc |= expect_int("merged row still gained the source", (int)merged->source_id, 201);
    dsd_state_ext_free_all(&state);
    return rc;
}

// The per-protocol builders read decoder state the history row does not carry. A merge must
// re-render against the values captured when the row was committed, not against a decoder that
// has since retuned -- otherwise a committed row is rewritten with a channel the call never used.
static int
test_merge_rerenders_against_committed_environment(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    state.nxdn_grant_chan = 12U;
    state.nxdn_grant_freq = 851012500;
    assert(observe_test_call(&state, 0U, DSD_SYNC_NXDN_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(expect_has_substr("first segment renders its granted channel",
                             event_history[0].Event_History_Items[1].event_string, "CH: 12;")
           == 0);

    // The trunk SM retunes before the segment is reacquired.
    state.nxdn_grant_chan = 44U;
    state.nxdn_grant_freq = 852000000;
    assert(observe_test_call(&state, 0U, DSD_SYNC_NXDN_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* merged = &event_history[0].Event_History_Items[1];
    int rc = expect_int("retuned merge commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_has_substr("merged row keeps the channel it was committed under", merged->event_string, "CH: 12;");
    rc |= expect_int("merged row still gained the source", (int)merged->source_id, 201);
    dsd_state_ext_free_all(&state);
    return rc;
}

// The test above commits the first segment's row from its own end, while the decoder still
// describes it. A row is also committed from the other direction -- the next epoch opening finds
// a staged row and pushes it -- and by then the canonical layer has already moved to the incoming
// call. Capturing the environment at that moment reads the new call's channel, so the row is
// re-rendered under a channel the transmission it describes never used.
static int
test_epoch_change_commit_keeps_staged_environment(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    state.nxdn_grant_chan = 12U;
    state.nxdn_grant_freq = 851012500;
    assert(observe_test_call(&state, 0U, DSD_SYNC_NXDN_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    // Sync loss with no further pass: the staged row is left for the next epoch to commit, which
    // is the path watchdog_event_history_authoritative() takes.
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);

    // The decoder retunes before the segment is reacquired, so the live grant channel now
    // describes the incoming call rather than the staged row.
    state.nxdn_grant_chan = 44U;
    state.nxdn_grant_freq = 852000000;
    assert(observe_test_call(&state, 0U, DSD_SYNC_NXDN_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_has_substr("row committed at the epoch change keeps its own channel",
                               event_history[0].Event_History_Items[1].event_string, "CH: 12;");

    // Ending the reacquired segment merges it into that row and re-renders it. The environment
    // the merge renders against is the one captured with the row, not the retuned decoder.
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* merged = &event_history[0].Event_History_Items[1];
    rc |= expect_int("epoch-change merge commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_has_substr("merged row keeps the channel it was staged under", merged->event_string, "CH: 12;");
    rc |= expect_int("merged row still gained the source", (int)merged->source_id, 201);
    dsd_state_ext_free_all(&state);
    return rc;
}

// A merged row is re-rendered from its own fields plus the captured environment, and that render
// has to agree with the one the unmerged path produces. Encryption is the case that matters: a
// row carries only the derived flag, so the classification the builders also test must be rebuilt
// from it rather than left unset.
static int
test_merged_row_keeps_encryption_marker(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P1_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    assert(update_test_crypto(&state, 0U, DSD_CALL_CRYPTO_ENCRYPTED, 0x84U, 0x1234U, 0U) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    int rc = expect_has_substr("first segment marks encryption", event_history[0].Event_History_Items[1].event_string,
                               "ENC;");

    // Reacquired segment folds into that row and re-renders it.
    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P1_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    rc |= expect_int("encrypted reacquisition commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_has_substr("merged row keeps the encryption marker",
                            event_history[0].Event_History_Items[1].event_string, "ENC;");
    dsd_state_ext_free_all(&state);
    return rc;
}

// Data calls describe distinct receptions and are never coalesced, even when the canonical
// layer flags the epoch as reacquired.
static int
test_repeated_data_notices_are_not_coalesced(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;

    for (int pass = 0; pass < 2; pass++) {
        assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_DATA_POS, DSD_CALL_KIND_DATA, 400U, 300U, 0U, 0U,
                                 DSD_CALL_BOUNDARY_BEGIN)
               == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
    }

    int rc = expect_int("repeated data calls both reach history", committed_history_rows(&event_history[0]), 2);
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
    event_history[0].Event_History_Items[0].pdu[0] = 0xABU;
    DSD_SNPRINTF(event_history[0].Event_History_Items[0].text_message,
                 sizeof(event_history[0].Event_History_Items[0].text_message), "%s", "packet text");
    DSD_SNPRINTF(event_history[0].Event_History_Items[0].gps_s, sizeof(event_history[0].Event_History_Items[0].gps_s),
                 "%s", "packet GPS");
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
    rc |= expect_int("active voice row drops staged data PDU", current->pdu[0], 0);
    rc |= expect_int("active voice row drops staged data text", current->text_message[0], '\0');
    rc |= expect_int("active voice row drops staged data GPS", current->gps_s[0], '\0');
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

// A sync-loss VOICE_END is held for the length of the reacquisition window. At shutdown there is
// no further audio to reacquire with, and the per-frame drain only fires once that window has
// elapsed, so an end armed in the last half second before exit would never be heard. The
// force-flush entry point the engine calls after its final snapshot pass has to retire it.
static int
test_pending_end_alert_is_flushed_at_shutdown(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_VOICE_END;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    // Still held: the deadline is on the monotonic clock and has not passed.
    int rc = expect_int("sync-loss end is still held before shutdown", g_beeper_count, 1);
    dsd_event_flush_pending_alerts(&opts, &state);
    rc |= expect_int("shutdown retires the held END", g_beeper_count, 2);
    // Idempotent: the engine's cleanup path is not the only thing that may run at exit.
    dsd_event_flush_pending_alerts(&opts, &state);
    rc |= expect_int("shutdown flush does not double-beep", g_beeper_count, 2);

    dsd_state_ext_free_all(&state);
    return rc;
}

// Clearing the event history destroys the rows the lifecycle points at. A VOICE_END held against
// a reacquisition that will never come must go with them, or it beeps for a transmission the
// operator can no longer see.
static int
test_history_reset_drops_pending_end_alert(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_VOICE_END;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    int rc = expect_int("sync-loss end is held before reset", g_beeper_count, 1);
    rc |= expect_int("sync-loss end committed a row", committed_history_rows(&event_history[0]), 1);

    dsd_event_history_reset(&state);
    rc |= expect_int("reset clears the rows", committed_history_rows(&event_history[0]), 0);

    // Neither the deadline passing nor an explicit flush may resurrect it.
    dsd_event_sync_slot(&opts, &state, 0U);
    dsd_event_flush_pending_alerts(&opts, &state);
    rc |= expect_int("reset drops the held END with the rows it described", g_beeper_count, 1);

    // The ended call must not be re-rendered into the cleared history either: the row the
    // operator deleted stays deleted.
    rc |= expect_int("reset does not resurrect the committed row", committed_history_rows(&event_history[0]), 0);

    // The next call still behaves normally.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 500U, 900U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("call after reset alerts START", g_beeper_count, 2);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("call after reset commits its row", committed_history_rows(&event_history[0]), 1);
    rc |=
        expect_int("call after reset keeps its identity", (int)event_history[0].Event_History_Items[1].target_id, 500);

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
    rc |= test_classified_control_notice_preserves_data_notice_behavior();
    rc |= test_classified_data_notice_rejects_invalid_categories_without_mutation();
    rc |= test_data_notice_with_gps_owns_payload_without_consuming_active_row();
    rc |= test_system_notice_is_not_attributed_as_radio_data();
    rc |= test_status_event_is_not_data_call_or_frame_log();
    rc |= test_source_less_data_call_does_not_suppress_next_voice_start_alert();
    rc |= test_canonical_data_call_uses_data_metadata_without_voice_start_alert();
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
    rc |= test_canonical_voice_category_is_protocol_neutral();
    rc |= test_provisional_voice_identity_does_not_commit_zero_row();
    rc |= test_new_canonical_epoch_commits_prior_canonical_call();
    rc |= test_reacquired_transmission_commits_one_row();
    rc |= test_terminator_after_sync_loss_end_blocks_reacquisition();
    rc |= test_end_reason_upgrade_is_one_directional();
    rc |= test_pending_end_alert_is_flushed_at_shutdown();
    rc |= test_history_reset_drops_pending_end_alert();
    rc |= test_identityless_ended_epoch_does_not_reacquire();
    rc |= test_route_only_identity_does_not_reacquire_unrelated_call();
    rc |= test_route_identity_reacquisition_still_coalesces();
    rc |= test_observed_fallback_matches_end_site_clock_resolution();
    rc |= test_contentless_sync_loss_end_arms_no_alert();
    rc |= test_reacquired_stage_superseded_by_new_call_still_merges();
    rc |= test_reacquisition_after_uncommitted_epoch_does_not_merge_stale_row();
    rc |= test_reacquired_transmission_via_continue_commits_one_row();
    rc |= test_back_to_back_same_identity_calls_commit_two_rows();
    rc |= test_explicit_end_then_identityless_reopen_commits_two_rows();
    rc |= test_changed_identity_still_commits_its_own_row();
    rc |= test_reacquisition_gap_beyond_window_commits_two_rows();
    rc |= test_reacquired_segment_may_outlast_the_window();
    rc |= test_flapping_segments_commit_one_row();
    rc |= test_dmr_sync_polarity_flip_still_coalesces();
    rc |= test_textual_identity_reacquisition_coalesces();
    rc |= test_reacquired_segment_contributes_late_source();
    rc |= test_reacquired_segment_contributes_crypto();
    rc |= test_reacquired_segment_contributes_alias_and_gps();
    rc |= test_interleaved_data_notice_does_not_misdirect_merge();
    rc |= test_history_reset_mid_reacquisition_still_commits();
    rc |= test_history_reset_clears_commit_bookkeeping();
    rc |= test_context_restore_invalidates_commit_bookkeeping();
    rc |= test_merge_logs_continuation_only_when_render_changes();
    rc |= test_merge_logs_metadata_the_segment_added();
    rc |= test_merge_upgrades_partial_alias();
    rc |= test_merge_carries_late_system_identifiers();
    rc |= test_notice_during_reacquisition_merges();
    rc |= test_notice_then_end_in_reacquired_epoch_commits_one_row();
    rc |= test_merge_continuation_annotates_from_the_row();
    rc |= test_history_reset_clears_staged_environment();
    rc |= test_merge_without_event_time_keeps_the_row_timestamp();
    rc |= test_merge_rerenders_against_committed_environment();
    rc |= test_epoch_change_commit_keeps_staged_environment();
    rc |= test_merged_row_keeps_encryption_marker();
    rc |= test_repeated_data_notices_are_not_coalesced();
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
