// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/sndfile_fwd.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/edacs/edacs_afs.h>
#include <dsd-neo/runtime/call_alert.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <math.h>
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
#include "test_support.h"

_Static_assert(DSD_EVENT_HISTORY_LEN == 255, "event history ring length is pinned by consumers of the snapshot");
_Static_assert(offsetof(Event_History_I, revision) == sizeof(Event_History) * 255U,
               "event history revision must follow the existing items");
_Static_assert(offsetof(Event_History_I, push_seq) == sizeof(Event_History) * 255U + sizeof(uint64_t),
               "event history push sequence must follow the revision");
_Static_assert(offsetof(Event_History_I, commit_rev) == sizeof(Event_History) * 255U + (2U * sizeof(uint64_t)),
               "event history commit revision must follow the push sequence");
_Static_assert(offsetof(Event_History_I, instance) == sizeof(Event_History) * 255U + (3U * sizeof(uint64_t)),
               "event history ring identity must follow the commit revision");
_Static_assert(sizeof(Event_History_I)
                   == sizeof(Event_History) * 255U + (4U * sizeof(uint64_t)) + sizeof(Event_History_Staged),
               "event history appends three counters, the ring identity and a separate staged data payload");

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
            char text[32];
            DSD_SNPRINTF(text, sizeof text, "packet-%u", i);
            dsd_event_stage_text(ctx->state, 0, text);

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
static int g_close_wav_export_count;
// Held as void* rather than SNDFILE*: this file is compiled twice, once against the real
// <sndfile.h> and once against the alt-tag stub header (CORE_CALL_ALERT_HISTORY_SNDFILE_ALT_TAG),
// so a SNDFILE*-typed static is two differently-typed declarations sharing one source location.
// Whole-program analyzers that merge the two translation units then see one of the pair with no
// reads at all and report it as dead (CodeQL cpp/unused-static-variable). A type that is identical
// in both builds keeps the stub's handle a single declaration.
static void* g_open_wav_result;
static double g_observed_m;

SNDFILE*
open_wav_file(char* dir, char* temp_filename, size_t temp_filename_size, uint16_t sample_rate, uint8_t ext) {
    UNUSED(dir);
    UNUSED(temp_filename);
    UNUSED(temp_filename_size);
    UNUSED(sample_rate);
    UNUSED(ext);
    g_open_wav_count++;
    return (SNDFILE*)g_open_wav_result;
}

SNDFILE*
close_and_rename_wav_file_ex(SNDFILE* wav_file, const dsd_opts* opts, const char* wav_out_filename, const char* dir,
                             const Event_History_I* event_struct, int export_call) {
    UNUSED(wav_file);
    UNUSED(opts);
    UNUSED(wav_out_filename);
    UNUSED(dir);
    UNUSED(event_struct);
    g_close_wav_count++;
    if (export_call) {
        g_close_wav_export_count++;
    }
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

// A stamp standing in for the one a replay timeline supplies (dsd_file.c), distinct from the wall
// clock the builders render their prefix from. It renders as its own date below so a test can tell
// which of the two a row was rendered from.
#define TEST_REPLAY_EVENT_TIME ((time_t)1000000000)

int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    // An unstamped time really does render as the epoch, so the stub has to say so: rendering a
    // plausible date for timestamp 0 would hide a row being restamped from an absent event_time.
    if (timestamp == 0) {
        const char* epoch = (format == DSD_LOCAL_DATETIME_DATE_HYPHEN) ? "1970-01-01" : "00:00:00";
        DSD_SNPRINTF(out, out_size, "%s", epoch);
        return 1;
    }
    if (timestamp == TEST_REPLAY_EVENT_TIME) {
        const char* replay = (format == DSD_LOCAL_DATETIME_DATE_HYPHEN) ? "2001-09-09" : "01:46:40";
        DSD_SNPRINTF(out, out_size, "%s", replay);
        return 1;
    }
    // Everything else is the live wall clock, which the fixture cannot pin; one stable rendering
    // keeps the prefix assertions deterministic.
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

// "The stamp was not rewritten". The tolerance is far below the smallest step any caller here
// advances the fixture clock by, so a restamp is still caught.
static int
same_instant(double a, double b) {
    return fabs(a - b) < 1e-9 ? 1 : 0;
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

// Once a test's temp file exists, its checks count failures with these helpers rather than assert(), so the test
// still removes the file when a check fails.
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

// Every line of a -J event log starts with the row's "YYYY-MM-DD HH:MM:SS" stamp (#469): the
// detail lines and a reacquired continuation included, so line-oriented tooling never meets an
// undated line.
static int
expect_every_line_stamped(const char* label, const char* buf, const char* stamp) {
    const size_t stamp_len = strlen(stamp);
    int lines = 0;
    for (const char* line = buf; line != NULL && *line != '\0';) {
        const char* end = strchr(line, '\n');
        const size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
        if (len > 0 && (len < stamp_len || strncmp(line, stamp, stamp_len) != 0)) {
            DSD_FPRINTF(stderr, "%s: line without stamp '%.*s'\n", label, (int)len, line);
            return 1;
        }
        lines++;
        line = end != NULL ? end + 1 : NULL;
    }
    if (lines == 0) {
        DSD_FPRINTF(stderr, "%s: log is empty\n", label);
        return 1;
    }
    return 0;
}

static int
event_history_item_equal(const Event_History* lhs, const Event_History* rhs) {
    return lhs->write == rhs->write && lhs->color_pair == rhs->color_pair && lhs->severity == rhs->severity
           && lhs->category == rhs->category && lhs->crc_invalid == rhs->crc_invalid && lhs->systype == rhs->systype
           && lhs->subtype == rhs->subtype && lhs->emergency == rhs->emergency && lhs->priority == rhs->priority
           && lhs->sys_id1 == rhs->sys_id1 && lhs->sys_id2 == rhs->sys_id2 && lhs->sys_id3 == rhs->sys_id3
           && lhs->sys_id4 == rhs->sys_id4 && lhs->sys_id5 == rhs->sys_id5 && lhs->gi == rhs->gi && lhs->enc == rhs->enc
           && lhs->enc_alg == rhs->enc_alg && lhs->enc_key == rhs->enc_key && lhs->mi == rhs->mi && lhs->svc == rhs->svc
           && lhs->source_id == rhs->source_id && lhs->target_id == rhs->target_id
           && memcmp(lhs->src_str, rhs->src_str, sizeof lhs->src_str) == 0
           && memcmp(lhs->tgt_str, rhs->tgt_str, sizeof lhs->tgt_str) == 0
           && memcmp(lhs->t_name, rhs->t_name, sizeof lhs->t_name) == 0
           && memcmp(lhs->s_name, rhs->s_name, sizeof lhs->s_name) == 0
           && memcmp(lhs->t_mode, rhs->t_mode, sizeof lhs->t_mode) == 0
           && memcmp(lhs->s_mode, rhs->s_mode, sizeof lhs->s_mode) == 0
           && memcmp(lhs->channel_label, rhs->channel_label, sizeof lhs->channel_label) == 0
           && lhs->channel_label_resolved == rhs->channel_label_resolved && lhs->channel == rhs->channel
           && lhs->freq_hz == rhs->freq_hz && lhs->access_code_kind == rhs->access_code_kind
           && lhs->access_code == rhs->access_code && lhs->event_time == rhs->event_time
           && lhs->event_start_time == rhs->event_start_time && memcmp(lhs->pdu, rhs->pdu, sizeof lhs->pdu) == 0
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
        if (strcmp(lhs[slot].staged.text_message, rhs[slot].staged.text_message) != 0
            || strcmp(lhs[slot].staged.gps_s, rhs[slot].staged.gps_s) != 0) {
            return 0;
        }
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

    // commit_rev only moves when committed rows do: a staged-row init leaves it
    // alone, a push (which shifts every committed row) advances it.
    rc |= expect_u64("staged-row init leaves commit_rev unchanged", histories[0].commit_rev, 0U);
    histories[0].Event_History_Items[0].source_id = 1234U;
    push_event_history(&histories[0]);
    rc |= expect_u64("push advances revision once", histories[0].revision, 2U);
    rc |= expect_u64("push advances commit_rev once", histories[0].commit_rev, 1U);
    rc |= expect_int("push copies the head row", (int)histories[0].Event_History_Items[1].source_id, 1234);

    dsd_event_history_mark_dirty(&histories[1]);
    rc |= expect_u64("explicit mark advances selected slot", histories[1].revision, 1U);
    rc |= expect_u64("explicit mark leaves other slot unchanged", histories[0].revision, 2U);
    rc |= expect_u64("mark-dirty leaves commit_rev unchanged", histories[1].commit_rev, 0U);
    init_event_history(&histories[1], 0, DSD_EVENT_HISTORY_LEN);
    rc |= expect_u64("full reset advances commit_rev", histories[1].commit_rev, 1U);
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

    // A change within the same epoch (the call turns emergency). A different source would open a new epoch, whose
    // render first commits the outgoing row.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 5678U, 1234U, 0x80U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 0);
    watchdog_event_current(&opts, &state, 0);
    rc |= expect_u64("semantic watchdog update advances revision", event_history[0].revision, first_revision + 1U);
    rc |= expect_u64("watchdog slot update leaves other slot unchanged", event_history[1].revision, 1U);
    return rc;
}

// A voice row's event_start_time and event_time are stamped from one wall-clock
// read, offset by the canonical epoch's own elapsed, so their difference is the
// call's measured duration — while the call runs and after it commits. A frontend
// reading the ring must never have to guess a duration from its own ingest time.
static int
test_voice_row_carries_call_start_time(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.dmr_color_code = 1U;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 5678U, 1234U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    // Adopt the epoch the way the per-frame loop does; without this the end-of-call
    // sync takes the startup-attach promote path, which re-inits the staged row.
    dsd_event_sync_slot(&opts, &state, 0U);
    // The BEGIN was observed at 1.0 and this CONTINUE lands at 8.1, so the active
    // epoch has run for 7.1 s on the fixture timeline when the row is rendered.
    // Same identity, so observe() answers 0 — continuing the epoch, not beginning
    // one — while still advancing the snapshot's updated_m.
    advance_test_clock(7.0);
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 5678U, 1234U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 0);
    watchdog_event_current(&opts, &state, 0);

    const Event_History* staged = &event_history[0].Event_History_Items[0];
    int rc = expect_int("active voice row carries a start time", staged->event_start_time > 0 ? 1 : 0, 1);
    rc |= expect_int("active row start-to-stamp span is the epoch's elapsed",
                     (int)(staged->event_time - staged->event_start_time), 7);

    // The stamp is derived once, on the epoch's first render. A later render must
    // not re-derive it — re-deriving from two independently truncated clocks makes
    // the stamp jitter by a second per pass — while event_time keeps advancing
    // with the epoch's elapsed.
    const time_t first_start = staged->event_start_time;
    advance_test_clock(1.0);
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 5678U, 1234U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 0);
    watchdog_event_current(&opts, &state, 0);
    rc |= expect_int("re-render keeps the start stamp fixed", staged->event_start_time == first_start ? 1 : 0, 1);
    rc |= expect_int("re-render extends the span with the epoch's elapsed",
                     (int)(staged->event_time - staged->event_start_time), 8);

    // Ended at 10.3 → 9.3 s total. The commit's final render must extend the span
    // through the end, and the committed row must carry the pair.
    advance_test_clock(1.0);
    assert(dsd_call_state_end_ex(&state, 0U, g_observed_m, DSD_CALL_END_TERMINATOR) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* committed = &event_history[0].Event_History_Items[1];
    rc |= expect_int("committed voice row keeps the start time", committed->event_start_time == first_start ? 1 : 0, 1);
    rc |= expect_int("committed row start-to-stamp span runs through the end",
                     (int)(committed->event_time - committed->event_start_time), 9);
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

// A notice for a call that is not the slot's canonical one -- the P25 encryption lockout builds a synthetic snapshot
// for a grant the slot's call does not match -- renders against the staged row of another epoch. That row's
// frequency and code describe the other call: the notice must read its own (here none) or the live receiver instead.
// The case: a canonical call pins the tuner on one voice channel, the trunking receiver follows a new grant to
// another, and the encrypted header there raises the lockout notice before the new call is published.
static int
test_noncanonical_notice_takes_no_staged_frequency_or_code(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    const char* detail = "Target: 200; has been locked out; Encryption Lock Out Enabled.";
    int rc = 0;

    for (int variant = 0; variant < 2; variant++) {
        reset_fixture(&opts, &state, event_history);
        opts.audio_in_type = AUDIO_IN_RTL;
        opts.rtlsdr_center_freq = 851012500U;
        state.lastsynctype = DSD_SYNC_P25P1_POS;
        state.nac = 0x293;
        assert(observe_test_call(&state, 0U, DSD_SYNC_P25P1_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0U, 0U,
                                 DSD_CALL_BOUNDARY_BEGIN)
               == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        const Event_History* staged = &event_history[0].Event_History_Items[0];
        rc |= expect_int("canonical call pins the first channel", staged->freq_hz == 851012500 ? 1 : 0, 1);
        rc |= expect_int("canonical call records its NAC",
                         staged->access_code_kind == (uint8_t)DSD_ACCESS_CODE_NAC && staged->access_code == 0x293U, 1);

        dsd_call_snapshot canonical;
        assert(dsd_call_state_get(&state, 0U, &canonical) == 1);
        opts.rtlsdr_center_freq = 852000000U;
        // Variant 1: the new carrier's NID decoded NAC 0x000 or 0xFFF, which leave nac at 0.
        state.nac = variant == 0 ? 0x3A1 : 0;

        dsd_call_snapshot synthetic;
        DSD_MEMSET(&synthetic, 0, sizeof synthetic);
        synthetic.epoch = canonical.epoch + 1U;
        synthetic.phase = DSD_CALL_PHASE_ACTIVE;
        synthetic.protocol = DSD_SYNC_P25P1_POS;
        synthetic.slot = 0U;
        synthetic.kind = DSD_CALL_KIND_GROUP_VOICE;
        synthetic.ota_target_id = 200U;
        synthetic.policy_target_id = 200U;
        synthetic.crypto = DSD_CALL_CRYPTO_ENCRYPTED;
        rc |= expect_int("noncanonical notice commits",
                         dsd_event_emit_call_notice_nonfinalizing(&opts, &state, 0U, &synthetic, detail), 1);

        const Event_History* notice = &event_history[0].Event_History_Items[1];
        rc |= expect_int("notice row is the synthetic call's", (int)notice->target_id, 200);
        if (notice->freq_hz != 852000000) {
            DSD_FPRINTF(stderr, "noncanonical notice (variant %d) took freq %lld, want the live 852000000\n", variant,
                        (long long)notice->freq_hz);
            rc = 1;
        }
        const uint8_t want_kind = variant == 0 ? (uint8_t)DSD_ACCESS_CODE_NAC : (uint8_t)DSD_ACCESS_CODE_NONE;
        const uint16_t want_code = variant == 0 ? 0x3A1U : 0U;
        if (notice->access_code_kind != want_kind || notice->access_code != want_code) {
            DSD_FPRINTF(stderr, "noncanonical notice (variant %d) took code kind %u 0x%X, want kind %u 0x%X\n", variant,
                        (unsigned)notice->access_code_kind, (unsigned)notice->access_code, (unsigned)want_kind,
                        (unsigned)want_code);
            rc = 1;
        }
        dsd_state_ext_free_all(&state);
    }
    return rc;
}

// The same holds for the scan-channel label: a notice for a call that is not the slot's canonical one names the
// channel it was heard on now, as a data notice does, never the label another epoch's staged row resolved on an
// earlier channel.
static int
test_noncanonical_notice_resolves_its_channel_label_live(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.trunk_scan_enabled = 1;
    DSD_SNPRINTF(state.trunk_scan_active_id, sizeof state.trunk_scan_active_id, "%s", "North");
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.nac = 0x293;

    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P1_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    int rc = expect_str_eq("canonical call is labelled with its channel",
                           event_history[0].Event_History_Items[0].channel_label, "North");

    dsd_call_snapshot canonical;
    assert(dsd_call_state_get(&state, 0U, &canonical) == 1);
    DSD_SNPRINTF(state.trunk_scan_active_id, sizeof state.trunk_scan_active_id, "%s", "South");

    dsd_call_snapshot synthetic;
    DSD_MEMSET(&synthetic, 0, sizeof synthetic);
    synthetic.epoch = canonical.epoch + 1U;
    synthetic.phase = DSD_CALL_PHASE_ACTIVE;
    synthetic.protocol = DSD_SYNC_P25P1_POS;
    synthetic.slot = 0U;
    synthetic.kind = DSD_CALL_KIND_GROUP_VOICE;
    synthetic.ota_target_id = 200U;
    synthetic.policy_target_id = 200U;
    synthetic.crypto = DSD_CALL_CRYPTO_ENCRYPTED;
    rc |=
        expect_int("noncanonical notice commits",
                   dsd_event_emit_call_notice_nonfinalizing(
                       &opts, &state, 0U, &synthetic, "Target: 200; has been locked out; Encryption Lock Out Enabled."),
                   1);

    const Event_History* notice = &event_history[0].Event_History_Items[1];
    rc |= expect_int("notice row is the synthetic call's", (int)notice->target_id, 200);
    rc |= expect_str_eq("noncanonical notice names the channel it was heard on", notice->channel_label, "South");
    rc |= expect_int("noncanonical notice resolved its label", notice->channel_label_resolved, 1);
    rc |= expect_has_substr("noncanonical notice renders the live label", notice->event_string, "[South] TEST");
    rc |=
        expect_no_substr("noncanonical notice does not adopt the other call's label", notice->event_string, "[North]");
    dsd_state_ext_free_all(&state);
    return rc;
}

// And for the row's start stamp, which a render takes once per epoch from the staged row: a notice for a call that is
// not the slot's canonical one starts when that call did, never when the other epoch's staged call did.
static int
test_noncanonical_notice_stamps_its_own_start(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    // A decode instant far from any wall-clock reading, so the stamps below can only come from the decode clock.
    const uint64_t t0_ns = 1000000000000000000ULL;
    const time_t t0_s = (time_t)1000000000;
    reset_fixture(&opts, &state, event_history);
    dsd_decode_clock_use_test(t0_ns);
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.nac = 0x293;

    // Call A, observed on the decode clock's own timeline (observed_m 0), starts at t0.
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 100U,
        .policy_target_id = 100U,
    };
    assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    int rc = expect_u64("canonical call is stamped with its start",
                        (uint64_t)event_history[0].Event_History_Items[0].event_start_time, (uint64_t)t0_s);

    // Seven seconds later call B, which the slot's call does not match, raises a notice.
    dsd_call_snapshot canonical;
    assert(dsd_call_state_get(&state, 0U, &canonical) == 1);
    dsd_decode_clock_test_set_ns(t0_ns + 7000000000ULL);
    dsd_call_snapshot synthetic;
    DSD_MEMSET(&synthetic, 0, sizeof synthetic);
    synthetic.epoch = canonical.epoch + 1U;
    synthetic.phase = DSD_CALL_PHASE_ACTIVE;
    synthetic.protocol = DSD_SYNC_P25P1_POS;
    synthetic.slot = 0U;
    synthetic.kind = DSD_CALL_KIND_GROUP_VOICE;
    synthetic.ota_target_id = 200U;
    synthetic.policy_target_id = 200U;
    synthetic.crypto = DSD_CALL_CRYPTO_ENCRYPTED;
    synthetic.started_m = dsd_decode_now_mono_s();
    synthetic.updated_m = synthetic.started_m;
    rc |=
        expect_int("noncanonical notice commits",
                   dsd_event_emit_call_notice_nonfinalizing(
                       &opts, &state, 0U, &synthetic, "Target: 200; has been locked out; Encryption Lock Out Enabled."),
                   1);

    const Event_History* notice = &event_history[0].Event_History_Items[1];
    rc |= expect_int("notice row is the synthetic call's", (int)notice->target_id, 200);
    rc |= expect_u64("noncanonical notice starts when its own call did", (uint64_t)notice->event_start_time,
                     (uint64_t)(t0_s + 7));
    rc |= expect_u64("noncanonical notice's end is anchored to its own start", (uint64_t)notice->event_time,
                     (uint64_t)(t0_s + 7));

    // A call whose own start is unknown is not given the other call's either.
    reset_fixture(&opts, &state, event_history);
    dsd_decode_clock_test_set_ns(t0_ns);
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(dsd_call_state_get(&state, 0U, &canonical) == 1);
    synthetic.epoch = canonical.epoch + 1U;
    synthetic.started_m = 0.0;
    synthetic.updated_m = 0.0;
    rc |=
        expect_int("unstarted noncanonical notice commits",
                   dsd_event_emit_call_notice_nonfinalizing(
                       &opts, &state, 0U, &synthetic, "Target: 200; has been locked out; Encryption Lock Out Enabled."),
                   1);
    rc |= expect_u64("noncanonical notice with no start of its own has none",
                     (uint64_t)event_history[0].Event_History_Items[1].event_start_time, 0U);

    dsd_decode_clock_use_system();
    dsd_state_ext_free_all(&state);
    return rc;
}

static int committed_history_rows(const Event_History_I* history);

// A notice for a call that is not the slot's canonical one leaves the canonical call's staged row exactly as it was.
// It used to render over that row and retire it: the notice row inherited the canonical call's talker alias, and the
// canonical call -- whose end P25 handle_enc() raises right after the lockout notice -- then rendered for the first
// time after its end, so its row committed with no access code, no tuned frequency and no alias.
static int
test_noncanonical_notice_leaves_the_canonical_staged_row_alone(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    static Event_History before;
    reset_fixture(&opts, &state, event_history);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtlsdr_center_freq = 851012500U;
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.nac = 0x293;

    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P1_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 101U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    dsd_call_snapshot canonical;
    assert(dsd_call_state_get(&state, 0U, &canonical) == 1);
    int rc =
        expect_int("canonical call takes its alias", dsd_event_enrich_alias(&state, 0U, canonical.epoch, "Unit 7"), 1);
    DSD_MEMCPY(&before, &event_history[0].Event_History_Items[0], sizeof before);

    opts.rtlsdr_center_freq = 852000000U;
    state.nac = 0;
    dsd_call_snapshot synthetic;
    DSD_MEMSET(&synthetic, 0, sizeof synthetic);
    synthetic.epoch = canonical.epoch + 1U;
    synthetic.phase = DSD_CALL_PHASE_ACTIVE;
    synthetic.protocol = DSD_SYNC_P25P1_POS;
    synthetic.slot = 0U;
    synthetic.kind = DSD_CALL_KIND_GROUP_VOICE;
    synthetic.ota_target_id = 200U;
    synthetic.policy_target_id = 200U;
    synthetic.crypto = DSD_CALL_CRYPTO_ENCRYPTED;
    rc |=
        expect_int("noncanonical notice commits",
                   dsd_event_emit_call_notice_nonfinalizing(
                       &opts, &state, 0U, &synthetic, "Target: 200; has been locked out; Encryption Lock Out Enabled."),
                   1);
    const Event_History* notice = &event_history[0].Event_History_Items[1];
    rc |= expect_int("notice row is the synthetic call's", (int)notice->target_id, 200);
    rc |= expect_str_eq("notice row does not inherit the canonical call's alias", notice->alias, "");
    rc |= expect_int("canonical staged row is untouched by the notice",
                     event_history_item_equal(&before, &event_history[0].Event_History_Items[0]), 1);

    // The canonical call ends, as handle_enc() ends it right after the notice.
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    const Event_History* committed = &event_history[0].Event_History_Items[1];
    rc |= expect_int("canonical call commits its own row", (int)committed->target_id, 100);
    rc |=
        expect_int("canonical row keeps its NAC",
                   committed->access_code_kind == (uint8_t)DSD_ACCESS_CODE_NAC && committed->access_code == 0x293U, 1);
    rc |= expect_int("canonical row keeps the frequency it was heard on", committed->freq_hz == 851012500 ? 1 : 0, 1);
    rc |= expect_str_eq("canonical row keeps its alias", committed->alias, "Unit 7");
    rc |= expect_int("notice and call leave two rows", committed_history_rows(&event_history[0]), 2);
    dsd_state_ext_free_all(&state);
    return rc;
}

// Committed rows (index >= 1) naming a target.
static int
committed_rows_for_target(const Event_History_I* history, uint32_t target_id) {
    int rows = 0;
    for (int i = 1; i < 255; i++) {
        const Event_History* item = &history->Event_History_Items[i];
        if (item->event_string[0] != '\0' && item->target_id == target_id) {
            rows++;
        }
    }
    return rows;
}

// A render outside dsd_event_sync_slot() -- the DMR and NXDN lockout notes, an operator's slot lockout or skip pumped
// between frames, the DMR P_CLEAR release -- can run after a protocol observed a new epoch but before any sync opened
// it: DMR's prepare_dmr_bs_voice_slot() observes the tuned call through the trunk SM's voice sync, a burst can end the
// superframe before its post-skip sync, and the control pump runs between processFrame() and the next getFrameSync().
// (D-STAR's header is not such a window: processDSTAR() syncs after every voice frame.) Rendering the new call over
// the outgoing epoch's staged row let the next sync commit that row as the outgoing epoch's: the new call got two rows,
// and an outgoing call still staged lost its own. One transmission leaves one row, the outgoing call keeps its own,
// and a note for the new call lands on the new call's row.
static int
test_lone_render_before_the_epoch_opens_keeps_one_row_per_call(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    int rc = 0;

    for (int variant = 0; variant < 4; variant++) {
        const int outgoing_still_staged = (variant & 1) != 0;
        const int with_note = (variant & 2) != 0;
        reset_fixture(&opts, &state, event_history);
        assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 101U, 0U, 0U,
                                 DSD_CALL_BOUNDARY_BEGIN)
               == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        if (!outgoing_still_staged) {
            assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
            dsd_event_sync_slot(&opts, &state, 0U);
        }

        // Call B is observed with no sync after it, then rendered alone, with or without a note.
        assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 200U, 201U, 0U, 0U,
                                 DSD_CALL_BOUNDARY_BEGIN)
               == 1);
        if (with_note) {
            dsd_event_note_current_call(&opts, &state, 0U, "Target: 200; call skipped.");
        } else {
            watchdog_event_current(&opts, &state, 0U);
        }

        // The next frame's sync, then B ends.
        dsd_event_sync_slot(&opts, &state, 0U);
        assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
        dsd_event_sync_slot(&opts, &state, 0U);

        char label[128];
        const char* name = outgoing_still_staged ? (with_note ? "staged, note" : "staged, render")
                                                 : (with_note ? "committed, note" : "committed, render");
        DSD_SNPRINTF(label, sizeof label, "%s: the outgoing call keeps one row", name);
        rc |= expect_int(label, committed_rows_for_target(&event_history[0], 100U), 1);
        DSD_SNPRINTF(label, sizeof label, "%s: the new call leaves one row", name);
        rc |= expect_int(label, committed_rows_for_target(&event_history[0], 200U), 1);
        const Event_History* newest = &event_history[0].Event_History_Items[1];
        DSD_SNPRINTF(label, sizeof label, "%s: the newest row is the new call's", name);
        rc |= expect_int(label, (int)newest->target_id, 200);
        DSD_SNPRINTF(label, sizeof label, "%s: the new call's row carries the note", name);
        rc |= expect_str_eq(label, newest->internal_str, with_note ? "Target: 200; call skipped." : "");
        DSD_SNPRINTF(label, sizeof label, "%s: the outgoing row carries no note", name);
        rc |= expect_str_eq(label, event_history[0].Event_History_Items[2].internal_str, "");
        dsd_state_ext_free_all(&state);
    }
    return rc;
}

// Rows pushed into the slot's ring since the fixture reset: every call commit, merge excepted, and every notice.
static uint64_t
pushed_rows(const Event_History_I* history) {
    return history->push_seq;
}

// Detail decoded for a call that has not been observed yet -- a D-STAR text or APRS block whose RF header failed its
// CRC, an NXDN alias ahead of the VCALL -- has no row to go to. Written into the blank staged row anyway, it was
// committed at the next epoch open as a detail-only row with no summary (a blank history entry with a Text:/GPS:
// line, an empty -J line followed by unstamped detail lines), and the call it belonged to lacked it. Detail now
// reaches a row only through an open, active call; the slow data or alias repeats, and lands once the call is open.
static int
test_detail_before_the_call_opens_leaves_no_orphan_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    // Call A is committed, which blanks the staged row.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 101U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    int rc = expect_u64("call A commits one row", pushed_rows(&event_history[0]), 1U);

    // No call is open: a protocol's detail is declined.
    rc |= expect_int("text with no open call is declined",
                     dsd_event_set_open_call_detail(&state, 0U, DSD_EVENT_DETAIL_TEXT, "CQ CQ"), 0);
    rc |= expect_int("GPS with no open call is declined",
                     dsd_event_set_open_call_detail(&state, 0U, DSD_EVENT_DETAIL_GPS, "APRS - Lat: 41d"), 0);
    rc |= expect_int("alias with no open call is declined",
                     dsd_event_set_open_call_detail(&state, 0U, DSD_EVENT_DETAIL_ALIAS, "CSM 123; "), 0);
    rc |= expect_str_eq("the staged row stays blank", event_history[0].Event_History_Items[0].text_message, "");

    // Call B is observed but no sync has opened its epoch: enrichment for B is declined too.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 200U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_call_snapshot call_b;
    assert(dsd_call_state_get(&state, 0U, &call_b) == 1);
    rc |= expect_int("enrichment before the epoch opens is declined",
                     dsd_event_enrich_gps(&state, 0U, call_b.epoch, "APRS - Lat: 41d"), 0);
    rc |= expect_int("direct detail before the epoch opens is declined",
                     dsd_event_set_open_call_detail(&state, 0U, DSD_EVENT_DETAIL_TEXT, "CQ CQ"), 0);

    // The sync opens B's epoch and leaves no detail-only row behind.
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_u64("opening B pushes no orphan row", pushed_rows(&event_history[0]), 1U);

    // The detail repeats once B is open and lands on B's own row.
    rc |= expect_int("enrichment of the open call lands",
                     dsd_event_enrich_gps(&state, 0U, call_b.epoch, "APRS - Lat: 41d"), 1);
    rc |= expect_int("direct detail for the open call lands",
                     dsd_event_set_open_call_detail(&state, 0U, DSD_EVENT_DETAIL_TEXT, "CQ CQ"), 1);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    const Event_History* row_b = &event_history[0].Event_History_Items[1];
    rc |= expect_u64("A and B leave two rows", pushed_rows(&event_history[0]), 2U);
    rc |= expect_int("the newest row is B's", (int)row_b->target_id, 200);
    rc |= expect_str_eq("B's row carries its GPS", row_b->gps_s, "APRS - Lat: 41d");
    rc |= expect_str_eq("B's row carries its text", row_b->text_message, "CQ CQ");
    rc |= expect_int("A's row is next", (int)event_history[0].Event_History_Items[2].target_id, 100);
    rc |= expect_str_eq("A's row carries none of B's detail", event_history[0].Event_History_Items[2].gps_s, "");

    // Enrichment still reaches the call's committed row once it ended and committed.
    rc |= expect_int("late enrichment reaches B's committed row", dsd_event_enrich_text(&state, 0U, call_b.epoch, "73"),
                     1);
    rc |= expect_str_eq("late text on the committed row", event_history[0].Event_History_Items[1].text_message, "73");
    dsd_state_ext_free_all(&state);
    return rc;
}

// Enrichment that names a call whose epoch no sync has opened yet is declined even when the staged row is not blank:
// that row is still the outgoing call's, and taking the new call's alias or position would commit them on the
// outgoing call's row. Covered with the outgoing call ended but not yet committed, and with it still active when the
// new call's identity forks the epoch.
static int
test_enrichment_before_the_epoch_opens_spares_the_outgoing_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    int rc = 0;

    for (int outgoing_ended = 0; outgoing_ended < 2; outgoing_ended++) {
        const char* name = outgoing_ended ? "outgoing ended" : "outgoing active";
        char label[128];
        reset_fixture(&opts, &state, event_history);
        assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 101U, 0U, 0U,
                                 DSD_CALL_BOUNDARY_BEGIN)
               == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        if (outgoing_ended) {
            assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
        }
        const Event_History* staged = &event_history[0].Event_History_Items[0];
        DSD_SNPRINTF(label, sizeof label, "%s: the staged row is the outgoing call's", name);
        rc |= expect_int(label, (int)staged->target_id, 100);

        // Call B, with a different identity, is observed with no sync after it.
        assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 200U, 201U, 0U, 0U,
                                 DSD_CALL_BOUNDARY_BEGIN)
               == 1);
        dsd_call_snapshot call_b;
        assert(dsd_call_state_get(&state, 0U, &call_b) == 1);
        DSD_SNPRINTF(label, sizeof label, "%s: B's alias is declined", name);
        rc |= expect_int(label, dsd_event_enrich_alias(&state, 0U, call_b.epoch, "Unit 201"), 0);
        DSD_SNPRINTF(label, sizeof label, "%s: B's position is declined", name);
        rc |= expect_int(label, dsd_event_enrich_gps(&state, 0U, call_b.epoch, "LIP: 22.5S 45.0W"), 0);
        DSD_SNPRINTF(label, sizeof label, "%s: the outgoing staged alias stays empty", name);
        rc |= expect_str_eq(label, staged->alias, "");
        DSD_SNPRINTF(label, sizeof label, "%s: the outgoing staged GPS stays empty", name);
        rc |= expect_str_eq(label, staged->gps_s, "");

        // The sync opens B's epoch and commits the outgoing row.
        dsd_event_sync_slot(&opts, &state, 0U);
        const Event_History* row_a = &event_history[0].Event_History_Items[1];
        DSD_SNPRINTF(label, sizeof label, "%s: the outgoing call commits its row", name);
        rc |= expect_int(label, (int)row_a->target_id, 100);
        DSD_SNPRINTF(label, sizeof label, "%s: the outgoing row carries none of B's alias", name);
        rc |= expect_str_eq(label, row_a->alias, "");
        DSD_SNPRINTF(label, sizeof label, "%s: the outgoing row carries none of B's GPS", name);
        rc |= expect_str_eq(label, row_a->gps_s, "");
        dsd_state_ext_free_all(&state);
    }
    return rc;
}

// Direct detail goes only to an active call's row. A call that has ended but not committed yet -- its finalize pass
// still to run -- is over: detail decoded now belongs to whatever keyed up next, not to the ended call's row. While
// the ended call is still active, detail cannot be told apart from the next transmission's, since neither the D-STAR
// slow data nor the NXDN alias names its call; that window closes when the next call is observed.
static int
test_direct_detail_skips_an_ended_call(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 101U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    int rc = expect_int("detail after the call ended is declined",
                        dsd_event_set_open_call_detail(&state, 0U, DSD_EVENT_DETAIL_GPS, "APRS - Lat: 41d"), 0);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_u64("the ended call commits one row", pushed_rows(&event_history[0]), 1U);
    rc |= expect_str_eq("the ended call's row carries no later detail", event_history[0].Event_History_Items[1].gps_s,
                        "");
    dsd_state_ext_free_all(&state);
    return rc;
}

// A note for the slot's call (a lockout, a skip) is declined when no call is there to carry it: no call at all, or a
// call that ended and committed. Written into the blank staged row, it became a note-only row at the next epoch open.
static int
test_note_without_an_authoritative_call_leaves_no_orphan_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    int rc = 0;

    // No call has ever been observed on the slot.
    reset_fixture(&opts, &state, event_history);
    dsd_event_note_current_call(&opts, &state, 0U, "Target: 100; call skipped.");
    rc |= expect_str_eq("no call: the note is declined", event_history[0].Event_History_Items[0].internal_str, "");
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 300U, 301U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_u64("no call: only the next call's row", pushed_rows(&event_history[0]), 1U);
    rc |= expect_str_eq("no call: the next call's row carries no note",
                        event_history[0].Event_History_Items[1].internal_str, "");

    // The slot's call ended and committed.
    reset_fixture(&opts, &state, event_history);
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 101U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    dsd_event_note_current_call(&opts, &state, 0U, "Target: 100; has been locked out; Session Only.");
    rc |=
        expect_str_eq("committed call: the note is declined", event_history[0].Event_History_Items[0].internal_str, "");
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 300U, 301U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |=
        expect_u64("committed call: opening the next call pushes no note-only row", pushed_rows(&event_history[0]), 1U);

    // An active call takes the note, as before.
    dsd_event_note_current_call(&opts, &state, 0U, "Target: 300; call skipped.");
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_str_eq("active call: the note lands on its row", event_history[0].Event_History_Items[1].internal_str,
                        "Target: 300; call skipped.");
    dsd_state_ext_free_all(&state);
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
    dsd_event_stage_text(&state, 0, "$GPRMC,validated");
    dsd_event_stage_gps(&state, 0, "41.500000 -87.250000");

    assert(emit_test_data_notice(&opts, &state, 1234U, 5678U, "NMEA SRC: 1234; TGT: 5678;", 0U) == 0);

    const Event_History* committed = &event_history[0].Event_History_Items[1];
    int rc = 0;
    rc |= expect_int("data payload first byte", committed->pdu[0], 0);
    rc |= expect_int("data payload second byte", committed->pdu[1], 0);
    rc |= expect_str_eq("data payload text", committed->text_message, "$GPRMC,validated");
    rc |= expect_str_eq("data payload GPS", committed->gps_s, "41.500000 -87.250000");
    rc |= expect_int("data payload category", committed->category, DSD_EVENT_CATEGORY_DATA);
    rc |= expect_has_substr("data payload notice", committed->event_string, "NMEA SRC: 1234; TGT: 5678;");
    const Event_History* current = &event_history[0].Event_History_Items[0];
    rc |= expect_int("active PDU sentinel is preserved", current->pdu[0], 0x12);
    rc |= expect_int("consumed staged data text is cleared", dsd_event_staged_text(&state, 0)[0], '\0');
    rc |= expect_int("consumed staged data GPS is cleared", dsd_event_staged_gps(&state, 0)[0], '\0');
    rc |= expect_int("second active PDU sentinel is preserved", current->pdu[1], 0x34);
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
    dsd_event_stage_text(&state, 0, "registration payload");
    dsd_event_stage_gps(&state, 0, "staged location");
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
    rc |= expect_int("classified control first payload byte", committed->pdu[0], 0);
    rc |= expect_int("classified control second payload byte", committed->pdu[1], 0);
    rc |= expect_str_eq("classified control text payload", committed->text_message, "registration payload");
    rc |= expect_str_eq("classified control GPS payload", committed->gps_s, "staged location");
    rc |= expect_has_substr("classified control notice", committed->event_string, "MNIS ARS;");
    rc |= expect_int("classified control preserves active PDU", current->pdu[0], 0x56);
    rc |= expect_int("classified control clears staged text", dsd_event_staged_text(&state, 0)[0], '\0');
    rc |= expect_int("classified control clears staged GPS", dsd_event_staged_gps(&state, 0)[0], '\0');
    rc |= expect_u64("classified control revision behavior", event_history[0].revision, revision_before + 3U);
    rc |= expect_int("classified control emits data alert", g_beeper_count, 1);
    rc |= expect_int("classified control uses data tone", g_last_beeper_id, 80);
    rc |= expect_int("classified control emits frame log", g_frame_log_count, 1);
    rc |= expect_has_substr("classified control frame log text", g_last_frame_log, "MNIS ARS;");
    dsd_state_ext_free_all(&state);
    rc |= expect_int("classified control preserves second active PDU byte", current->pdu[1], 0x78);
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
    dsd_event_stage_text(&state, 0, "staged text");
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
    state.p2_cc = 0x293ULL;    /* a Phase 2 call's NAC is the one its descrambler runs on, */
    state.p2_cc_verified = 1U; /* proven on this carrier */
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

    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dsd-neo-events");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed for event log test\n");
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
    rc |= expect_has_substr("event log main line", buf, "2026-04-30 00:00:00 TEST EVENT; Slot 2;");
    rc |= expect_has_substr("event log text", buf, "\n2026-04-30 00:00:00 Text: hello text \n");
    rc |= expect_has_substr("event log alias", buf, "\n2026-04-30 00:00:00 Talker Alias: Unit 7 \n");
    rc |= expect_has_substr("event log gps", buf, "\n2026-04-30 00:00:00 GPS: 41.500000 -87.250000 \n");
    rc |= expect_has_substr("event log internal", buf, "\n2026-04-30 00:00:00 DSD-neo: status detail \n");
    rc |= expect_every_line_stamped("event log detail lines carry the header's stamp", buf, "2026-04-30 00:00:00 ");
    return rc;
}

// A rendered line with no parseable prefix, on a row with no stamp of its own, has nothing to
// stamp the detail lines with. They keep their pre-#469 shape -- a leading space and the label --
// rather than an invented time.
static int
test_event_log_unstamped_row_keeps_bare_detail_lines(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dsd-neo-events");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed for unstamped event log test\n");
        return 1;
    }
    close(fd);
    remove(path);
    DSD_SNPRINTF(opts.event_out_file, sizeof opts.event_out_file, "%s", path);

    Event_History* row = &state.event_history_s[1].Event_History_Items[0];
    row->event_time = 0;
    row->event_string[0] = '\0';
    DSD_SNPRINTF(row->text_message, sizeof row->text_message, "%s", "hello text");
    DSD_SNPRINTF(row->alias, sizeof row->alias, "%s", "Unit 7");

    char event_string[] = "TEST EVENT;";
    write_event_to_log_file(&opts, &state, 1, 1, event_string);

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        remove(path);
        DSD_FPRINTF(stderr, "unstamped event log was not created\n");
        return 1;
    }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    remove(path);
    buf[n] = '\0';

    int rc = 0;
    rc |= expect_has_substr("unstamped main line", buf, "TEST EVENT; Slot 2;");
    rc |= expect_has_substr("unstamped text line keeps its label", buf, "\n Text: hello text \n");
    rc |= expect_has_substr("unstamped alias line keeps its shape", buf, "\n Talker Alias: Unit 7 \n");
    rc |= expect_no_substr("no time is invented for an unstamped row", buf, "2026-04-30");
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

// A voice epoch that ends without ever learning an identity is noise, not a
// call: a stray voice sync opened it and no header named anyone. Committing it
// surfaced "TGT: 00000000; SRC: 00000000" rows in Activity history. The drop
// must not arm the deferred VOICE_END alert either, matching the
// empty-staged-row rule that an alert needs a row the operator can see, and
// must leave the following identified call committing normally.
static int
test_identityless_voice_epoch_commits_no_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_VOICE_END;

    int rc = 0;
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.dmr_color_code = 13;

    rc |= expect_int("provisional call starts epoch",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                                       DSD_CALL_BOUNDARY_BEGIN),
                     1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("zero-identity row is staged",
                     state.event_history_s[0].Event_History_Items[0].event_string[0] != '\0', 1);
    rc |= expect_int("sync loss ends the epoch", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("identity-less row does not reach history",
                     event_history[0].Event_History_Items[1].event_string[0], '\0');
    dsd_call_context_snapshot context;
    rc |= expect_int("context snapshot copies", dsd_call_context_copy_snapshot(&state, &context) > 0, 1);
    rc |= expect_int("dropped row arms no end alert", context.events[0].end_alert_pending, 0);
    rc |= expect_int("dropped row emits only the start alert", g_beeper_count, 1);

    // The next identified call on the slot is unaffected by the drop.
    rc |= expect_int("identified call begins",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 1234U, 5678U,
                                       0U, 0U, DSD_CALL_BOUNDARY_BEGIN),
                     1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("identified call ends", end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("identified row reaches history", (int)event_history[0].Event_History_Items[1].target_id, 1234);
    rc |= expect_int("only the identified row is in history", event_history[0].Event_History_Items[2].event_string[0],
                     '\0');
    rc |= expect_int("identified call emits start and end alerts", g_beeper_count, 3);

    dsd_state_ext_free_all(&state);
    return rc;
}

// An identity-less voice epoch that carried decoded audio and ended on a
// terminator is a real transmission, not noise: on a RAS DMR system under the
// aggressive-framesync default every link control fails the masked CRC, so a
// short PTT whose embedded LC never reassembled plays audio yet never names a
// call. Its row must reach history -- as the zero-ID record that a
// transmission occurred -- and its deferred VOICE_END must fire once the
// terminator repeat corroborates the end. A media-carrying epoch that merely
// faded out (sync loss, no terminator) stays droppable: that is the
// stray-sync noise shape.
static int
test_media_terminated_identityless_voice_epoch_commits_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_VOICE_END;

    int rc = 0;
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;

    rc |= expect_int("provisional call starts epoch",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                                       DSD_CALL_BOUNDARY_BEGIN),
                     1);
    rc |= expect_int("voice media runs on the epoch", dsd_call_state_update_media(&state, 0U, 1, g_observed_m), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("terminator ends the epoch unverified",
                     end_test_call(&state, 0U, DSD_CALL_END_UNVERIFIED_TERMINATOR), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("audible terminated row reaches history",
                     event_history[0].Event_History_Items[1].event_string[0] != '\0', 1);
    rc |= expect_int("row records that no one was named", (int)event_history[0].Event_History_Items[1].target_id, 0);
    dsd_call_context_snapshot context;
    rc |= expect_int("context snapshot copies", dsd_call_context_copy_snapshot(&state, &context) > 0, 1);
    rc |= expect_int("recoverable end holds the VOICE_END alert", context.events[0].end_alert_pending, 1);
    rc |= expect_int("only the start alert has fired", g_beeper_count, 1);

    // The hangtime repeat -- itself unverified on a RAS system -- corroborates the end into
    // EXPLICIT, releasing the held alert.
    rc |= expect_int("corroborating terminator tightens the end",
                     end_test_call(&state, 0U, DSD_CALL_END_UNVERIFIED_TERMINATOR), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("released VOICE_END alert fires", g_beeper_count, 2);

    // A media-carrying epoch that fades out without a terminator is still the
    // stray-sync noise shape and leaves no row.
    rc |= expect_int("noise epoch starts",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                                       DSD_CALL_BOUNDARY_BEGIN),
                     1);
    rc |= expect_int("noise media runs", dsd_call_state_update_media(&state, 0U, 1, g_observed_m), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("sync loss ends the noise epoch", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("noise row does not reach history", event_history[0].Event_History_Items[2].event_string[0], '\0');

    dsd_state_ext_free_all(&state);
    return rc;
}

static int committed_history_rows(const Event_History_I* history);

// The engine ends epochs EXPLICIT on every retune and teardown --
// no_carrier_clear_voice_tune_state() fires one each time the trunker returns
// to the control channel -- so an EXPLICIT end is not evidence a transmission
// ended over the air. A stray-sync noise epoch that carried media and was then
// ended by a retune must drop exactly like the faded shape; only the
// terminator reasons vouch for an identity-less audible row.
static int
test_retune_explicit_end_drops_identityless_media_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    int rc = 0;
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;

    rc |= expect_int("provisional call starts epoch",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                                       DSD_CALL_BOUNDARY_BEGIN),
                     1);
    rc |= expect_int("voice media runs on the epoch", dsd_call_state_update_media(&state, 0U, 1, g_observed_m), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("retune ends the epoch explicitly", end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("retune-ended noise row does not reach history", committed_history_rows(&event_history[0]), 0);

    // The verified-terminator shape is the one that vouches: same epoch shape,
    // ended by positive over-the-air evidence, keeps its row.
    rc |= expect_int("audible epoch starts",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                                       DSD_CALL_BOUNDARY_BEGIN),
                     1);
    rc |= expect_int("voice media runs again", dsd_call_state_update_media(&state, 0U, 1, g_observed_m), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("verified terminator ends the epoch", end_test_call(&state, 0U, DSD_CALL_END_TERMINATOR), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("terminator-ended audible row reaches history", committed_history_rows(&event_history[0]), 1);

    dsd_state_ext_free_all(&state);
    return rc;
}

// The drop path still rotates the WAV -- leaving it open would let the noise segment's audio
// lead the next transmission's recording -- but must not export it: the recording of an epoch
// with no history row or log line would upload to rdio-scanner under all-zero metadata the
// operator has nothing local to correlate against. A committed row's rotation still exports.
static int
test_dropped_identityless_row_rotates_wav_without_export(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    int rc = 0;
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    g_close_wav_count = 0;
    g_close_wav_export_count = 0;

    opts.wav_out_f = (SNDFILE*)0x1;
    rc |= expect_int("noise epoch starts",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                                       DSD_CALL_BOUNDARY_BEGIN),
                     1);
    rc |= expect_int("noise media runs", dsd_call_state_update_media(&state, 0U, 1, g_observed_m), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("sync loss ends the noise epoch", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("dropped row leaves no history", committed_history_rows(&event_history[0]), 0);
    // The keep-or-drop verdict is held while the recoverable end could still be explained by a
    // late terminator, so the WAV has not rotated yet; a new call taking the slot resolves the
    // hold, and the drop's rotation must not export.
    rc |= expect_int("keep-or-drop is held while the end may still be explained", g_close_wav_count, 0);
    rc |= expect_int("identified call begins",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 1234U, 5678U,
                                       0U, 0U, DSD_CALL_BOUNDARY_BEGIN),
                     1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("held drop resolves when a new call takes the slot", g_close_wav_count, 1);
    rc |= expect_int("dropped row's rotation does not export", g_close_wav_export_count, 0);
    rc |= expect_int("resolved drop still leaves no history", committed_history_rows(&event_history[0]), 0);

    // The committed shape keeps exporting: same rotation path, row reached history.
    opts.wav_out_f = (SNDFILE*)0x1;
    g_close_wav_count = 0;
    rc |= expect_int("identified call ends", end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("committed row rotates the WAV", g_close_wav_count, 1);
    rc |= expect_int("committed row's rotation exports", g_close_wav_export_count, 1);

    dsd_state_ext_free_all(&state);
    return rc;
}

// The keep-or-drop verdict for an identity-less audible row must not be irrevocable at the
// first ended-sync pass: a fade often beats the terminator that explains it, and the hangtime
// terminator then retracts the sync-loss end to TERMINATOR in the canonical layer. The held
// verdict re-reads the staged environment on the next pass, sees the end became positive, and
// commits the row the immediate drop would have deleted with nothing left to re-render.
static int
test_terminator_after_fade_rescues_identityless_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    int rc = 0;
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;

    rc |= expect_int("audible epoch starts",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                                       DSD_CALL_BOUNDARY_BEGIN),
                     1);
    rc |= expect_int("voice media runs", dsd_call_state_update_media(&state, 0U, 1, g_observed_m), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("carrier fades before the terminator", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("verdict is held, not dropped", committed_history_rows(&event_history[0]), 0);

    // The hangtime terminator decodes after the fade; a verified terminator retracts the
    // recoverable end, and the next sync pass commits the row it now vouches for.
    rc |=
        expect_int("late terminator retracts the sync-loss end", end_test_call(&state, 0U, DSD_CALL_END_TERMINATOR), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("rescued audible row reaches history", committed_history_rows(&event_history[0]), 1);

    dsd_state_ext_free_all(&state);
    return rc;
}

// D-STAR has no terminator to decode -- its transmissions only ever end by sync loss or an
// engine teardown -- so the media-plus-terminator vouch would be structurally unsatisfiable for
// it. A sync-loss end after audible media must count as the positive end evidence the mode can
// never produce, keeping the reception's row where a DMR noise epoch's still drops.
static int
test_dstar_sync_loss_after_media_keeps_identityless_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    int rc = 0;
    state.lastsynctype = DSD_SYNC_DSTAR_VOICE_POS;

    rc |= expect_int("identity-less D-STAR epoch starts",
                     observe_test_call(&state, 0U, DSD_SYNC_DSTAR_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                                       DSD_CALL_BOUNDARY_BEGIN),
                     1);
    rc |= expect_int("voice media runs", dsd_call_state_update_media(&state, 0U, 1, g_observed_m), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("sync loss ends the reception", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("audible D-STAR row reaches history", committed_history_rows(&event_history[0]), 1);

    dsd_state_ext_free_all(&state);
    return rc;
}

// M17, YSF and NXDN each define an end marker but send it exactly once, behind a CRC or FICH
// error check: an M17 stream whose transmitter drops carrier before the EOT, a YSF tail burst
// whose FICH is corrupt, or a missed NXDN release SACCH all leave an audible reception ending by
// sync loss. Demanding a terminator there deleted the row, the log line and the end alert and
// left the rotated WAV an orphan, so a sync-loss end after media must vouch for them the way it
// does for D-STAR -- while DMR, P25 and dPMR, whose end signaling repeats or rides the sync
// correlator, keep dropping the same shape as the noise it is.
static int
test_unreliable_terminator_modes_keep_audible_identityless_rows(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];

    static const int lenient[] = {DSD_SYNC_M17_STR_POS, DSD_SYNC_YSF_POS, DSD_SYNC_NXDN_POS};
    static const int strict[] = {DSD_SYNC_DMR_BS_VOICE_POS, DSD_SYNC_P25P1_POS, DSD_SYNC_DPMR_FS2_POS};

    int rc = 0;
    for (size_t i = 0; i < sizeof(lenient) / sizeof(lenient[0]); i++) {
        reset_fixture(&opts, &state, event_history);
        state.lastsynctype = lenient[i];
        rc |= expect_int(
            "identity-less epoch starts",
            observe_test_call(&state, 0U, lenient[i], DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U, DSD_CALL_BOUNDARY_BEGIN), 1);
        rc |= expect_int("voice media runs", dsd_call_state_update_media(&state, 0U, 1, g_observed_m), 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        rc |= expect_int("carrier drops before the end marker", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        rc |= expect_int("audible row survives the missed end marker", committed_history_rows(&event_history[0]), 1);
        dsd_state_ext_free_all(&state);
    }

    for (size_t i = 0; i < sizeof(strict) / sizeof(strict[0]); i++) {
        reset_fixture(&opts, &state, event_history);
        state.lastsynctype = strict[i];
        rc |= expect_int(
            "identity-less epoch starts",
            observe_test_call(&state, 0U, strict[i], DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U, DSD_CALL_BOUNDARY_BEGIN), 1);
        rc |= expect_int("voice media runs", dsd_call_state_update_media(&state, 0U, 1, g_observed_m), 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        rc |= expect_int("sync loss ends the noise epoch", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        rc |= expect_int("noise row still drops", committed_history_rows(&event_history[0]), 0);
        dsd_state_ext_free_all(&state);
    }
    return rc;
}

// The held VOICE_END deadline must be selected by the same reason-keyed rule the canonical layer
// applies to reacquisition: an unverified-terminator end stops accepting a heal after
// DSD_CALL_TERMINATOR_HEAL_GAP_S, so its alert must not be withheld for the full sync-loss
// window on top of that. Both deadlines are bracketed between two reads of the same clock the
// arming site uses, so the assertions hold at any scheduler pace.
static int
test_end_alert_deadline_matches_reacquire_window(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_VOICE_END;

    int rc = 0;
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    dsd_call_context_snapshot context;

    // Sync-loss end: held for the full reacquisition gap.
    rc |= expect_int("identified call begins",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 1234U, 5678U,
                                       0U, 0U, DSD_CALL_BOUNDARY_BEGIN),
                     1);
    dsd_event_sync_slot(&opts, &state, 0U);
    double before = dsd_decode_now_mono_s();
    rc |= expect_int("sync loss ends it", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    double after = dsd_decode_now_mono_s();
    rc |= expect_int("context snapshot copies", dsd_call_context_copy_snapshot(&state, &context) > 0, 1);
    rc |= expect_int("sync-loss end holds the alert", context.events[0].end_alert_pending, 1);
    rc |= expect_int("sync-loss deadline is not below its window",
                     context.events[0].end_alert_due_m >= before + DSD_CALL_REACQUIRE_GAP_S, 1);
    rc |= expect_int("sync-loss deadline is not above its window",
                     context.events[0].end_alert_due_m <= after + DSD_CALL_REACQUIRE_GAP_S, 1);
    dsd_event_flush_pending_alerts(&opts, &state);

    // Unverified-terminator end: held only for the tighter heal gap.
    rc |= expect_int("second identified call begins",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 4321U, 8765U,
                                       0U, 0U, DSD_CALL_BOUNDARY_BEGIN),
                     1);
    dsd_event_sync_slot(&opts, &state, 0U);
    before = dsd_decode_now_mono_s();
    rc |= expect_int("unverified terminator ends it", end_test_call(&state, 0U, DSD_CALL_END_UNVERIFIED_TERMINATOR), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    after = dsd_decode_now_mono_s();
    rc |= expect_int("second context snapshot copies", dsd_call_context_copy_snapshot(&state, &context) > 0, 1);
    rc |= expect_int("unverified end holds the alert", context.events[0].end_alert_pending, 1);
    rc |= expect_int("terminator deadline is not below the heal window",
                     context.events[0].end_alert_due_m >= before + DSD_CALL_TERMINATOR_HEAL_GAP_S, 1);
    rc |= expect_int("terminator deadline is not above the heal window",
                     context.events[0].end_alert_due_m <= after + DSD_CALL_TERMINATOR_HEAL_GAP_S, 1);
    dsd_event_flush_pending_alerts(&opts, &state);

    dsd_state_ext_free_all(&state);
    return rc;
}

// The heal window after an unverified terminator is tighter than the sync-loss
// reacquisition window: the mis-typed voice burst it exists for is followed by
// the transmission's next media mark within a burst or two, while a real end
// followed by a fast re-key whose headers fail to decode can put an
// identity-less media mark on the slot well inside the sync-loss window -- and
// folding that in would hand the new call the terminated call's identity (and,
// in DMR, its restored crypto).
static int
test_unverified_terminator_heal_window_is_tight(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    int rc = 0;
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;

    rc |= expect_int("identified call begins",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 1234U, 5678U,
                                       0U, 0U, DSD_CALL_BOUNDARY_BEGIN),
                     1);
    rc |= expect_int("unverified terminator ends it", end_test_call(&state, 0U, DSD_CALL_END_UNVERIFIED_TERMINATOR), 1);
    // Beyond the heal window but still inside the sync-loss window: proves the
    // tighter gate is what rejects the reopen.
    advance_test_clock(0.3);
    rc |= expect_int("late identity-less continuation begins",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                                       DSD_CALL_BOUNDARY_BEGIN),
                     1);
    dsd_call_snapshot call;
    rc |= expect_int("late continuation opens a fresh epoch", dsd_call_state_get(&state, 0U, &call) > 0, 1);
    rc |= expect_int("fresh epoch is not seeded with the ended call's identity", (int)call.ota_target_id, 0);
    // Retire the fresh identity-less epoch so the next identified BEGIN opens
    // its own epoch instead of specializing into it.
    rc |= expect_int("fresh epoch retires", end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT), 1);

    // Inside the heal window the mis-typed burst still heals.
    rc |= expect_int("second identified call begins",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 4321U, 8765U,
                                       0U, 0U, DSD_CALL_BOUNDARY_BEGIN),
                     1);
    rc |= expect_int("unverified terminator ends the second call",
                     end_test_call(&state, 0U, DSD_CALL_END_UNVERIFIED_TERMINATOR), 1);
    rc |= expect_int("prompt identity-less continuation begins",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                                       DSD_CALL_BOUNDARY_BEGIN),
                     1);
    rc |= expect_int("prompt continuation heals the epoch", dsd_call_state_get(&state, 0U, &call) > 0, 1);
    rc |= expect_int("healed epoch keeps the terminated call's identity", (int)call.ota_target_id, 4321);

    dsd_state_ext_free_all(&state);
    return rc;
}

// A voice epoch whose only decoded knowledge is its crypto -- encrypted, this
// alg, this key -- is not noise. P25 late entry deliberately opens an
// identity-less epoch so ESS crypto can attach; when the signal fades before
// any LC names a talkgroup or source, the record that an encrypted
// transmission occurred must still reach history.
static int
test_crypto_only_voice_epoch_commits_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    int rc = 0;
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    rc |= expect_int(
        "provisional call starts epoch",
        observe_test_call(&state, 0U, DSD_SYNC_P25P1_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U, DSD_CALL_BOUNDARY_BEGIN),
        1);
    rc |= expect_int("crypto attaches to the identity-less epoch",
                     update_test_crypto(&state, 0U, DSD_CALL_CRYPTO_ENCRYPTED_PENDING, 0x84U, 0x1234U, 0U), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("sync loss ends the epoch", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("crypto-only row reaches history", event_history[0].Event_History_Items[1].event_string[0] != '\0',
                     1);
    rc |= expect_int("row records the algorithm", event_history[0].Event_History_Items[1].enc_alg, 0x84);
    rc |= expect_int("row records the key id", event_history[0].Event_History_Items[1].enc_key, 0x1234);
    dsd_state_ext_free_all(&state);
    return rc;
}

// Route text is identity the row strings never carry: a D-STAR transmission
// where only the repeater pair decoded stages a row whose every checked field
// is empty. The identity verdict is captured at render time, so the row must
// survive commit on every path -- including the epoch-change path, where the
// outgoing epoch's canonical snapshot is already gone.
static int
test_route_text_only_row_survives_epoch_change_commit(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    int rc = 0;
    state.lastsynctype = DSD_SYNC_DSTAR_VOICE_POS;
    dsd_call_observation route_only = {
        .protocol = DSD_SYNC_DSTAR_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
        .observed_m = g_observed_m,
    };
    DSD_SNPRINTF(route_only.route_text[0], sizeof route_only.route_text[0], "%s", "RPT1CALL");
    g_observed_m += 0.1;
    rc |= expect_int("route-only call begins", dsd_call_state_observe(&state, &route_only, DSD_CALL_BOUNDARY_BEGIN), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("route-only row is staged",
                     state.event_history_s[0].Event_History_Items[0].event_string[0] != '\0', 1);

    // The next transmission's BEGIN forks the epoch before any terminator, so the leftover row
    // commits through the epoch-change path.
    rc |= expect_int("next call begins",
                     observe_test_call(&state, 0U, DSD_SYNC_DSTAR_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 3333U, 0U, 0U,
                                       DSD_CALL_BOUNDARY_BEGIN),
                     1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("route-only row reaches history", event_history[0].Event_History_Items[1].event_string[0] != '\0',
                     1);
    dsd_state_ext_free_all(&state);
    return rc;
}

// Standalone ProVoice never parses its voice traffic into a talkgroup or
// source, so an all-zero row is the protocol's whole story -- the channel
// carried voice -- and must keep reaching history exactly as X2-TDMA's rows
// do. (EDACS-trunked ProVoice rows carry AFS/LID strings and are unaffected.)
static int
test_standalone_provoice_zero_id_row_commits(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    int rc = 0;
    state.lastsynctype = DSD_SYNC_PROVOICE_POS;
    rc |= expect_int("provoice call starts epoch",
                     observe_test_call(&state, 0U, DSD_SYNC_PROVOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                                       DSD_CALL_BOUNDARY_BEGIN),
                     1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("sync loss ends the epoch", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("standalone provoice row reaches history",
                     event_history[0].Event_History_Items[1].event_string[0] != '\0', 1);
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

// The scan-channel label a row carries comes from the same resolver the frontends use, so these
// helpers arm exactly what an operator's scan leaves in state: a -Y scan list parked on a named
// row, or a --trunk-scan rotation sitting on a named target. lcn_freq_roll is advanced past the
// row just tuned, so the row on air is roll - 1, and that row needs a usable frequency: the
// scanner steps over a 0 slot without retuning, and the resolver leaves such a row unnamed.
static int
arm_scanner_label(dsd_state* state, dsd_opts* opts, int idx, const char* name) {
    opts->scanner_mode = 1;
    state->lcn_freq_count = idx + 2;
    state->lcn_freq_roll = idx + 1;
    *dsd_state_trunk_lcn_slot(state, idx) = 851000000L + (12500L * idx);
    return dsd_state_trunk_lcn_name_set(state, (size_t)idx, name);
}

// The published ordinal and target count feed a frontend's "n of m" readout, not the label, so
// arming the id alone is the whole of what a row's label depends on.
static void
arm_trunk_scan_label(dsd_state* state, dsd_opts* opts, const char* id) {
    opts->trunk_scan_enabled = 1;
    DSD_SNPRINTF(state->trunk_scan_active_id, sizeof state->trunk_scan_active_id, "%s", id);
}

// Read back everything an event log holds, or return -1. The caller owns the path.
static int
read_event_log(const char* path, char* out, size_t out_sz) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    size_t n = fread(out, 1U, out_sz - 1U, f);
    fclose(f);
    out[n] = '\0';
    return 0;
}

// Scanning a -Y list, the operator has to be able to tell which channel a row was heard on. The
// label renders between the timestamp and the protocol token, so the 20-character timestamp
// prefix every frontend parses stays exactly where it was, and the -J event log carries it too.
static int
test_scanner_mode_row_carries_channel_label(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dsd-neo-label");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed for channel label event log test\n");
        return 1;
    }
    close(fd);
    remove(path);
    DSD_SNPRINTF(opts.event_out_file, sizeof opts.event_out_file, "%s", path);

    int rc = expect_int("scan list name stores", arm_scanner_label(&state, &opts, 0, "Fire Dispatch"), 0);

    state.lastsynctype = DSD_SYNC_P25P2_POS;
    state.nac = 0x293;
    state.p2_cc = 0x293ULL;    /* a Phase 2 call's NAC is the one its descrambler runs on, */
    state.p2_cc_verified = 1U; /* proven on this carrier */
    state.p2_wacn = 0x45564U;
    state.p2_sysid = 0x006U;
    state.p2_rfssid = 10U;
    state.p2_siteid = 10U;
    rc |= expect_int("scanner call begins",
                     observe_test_call(&state, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 50061U, 5790062U, 0U,
                                       0U, DSD_CALL_BOUNDARY_BEGIN),
                     1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_str_eq("scanner row keeps the channel label", item->channel_label, "Fire Dispatch");
    rc |= expect_str_eq("scanner row renders the label after the timestamp", item->event_string,
                        "2026-04-30 00:00:00 [Fire Dispatch] TEST TGT: 00050061; SRC: 05790062; NAC: 293; "
                        "NET_STS: 45564:006:10.10; Group; ");

    rc |= expect_int("scanner call ends", end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT), 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    char log[4096];
    if (read_event_log(path, log, sizeof log) != 0) {
        DSD_FPRINTF(stderr, "channel label event log was not created\n");
        rc = 1;
    } else {
        rc |= expect_has_substr("event log line carries the label", log,
                                "2026-04-30 00:00:00 [Fire Dispatch] TEST TGT: 00050061;");
    }
    remove(path);

    dsd_state_trunk_lcn_name_free(&state);
    dsd_state_ext_free_all(&state);
    return rc;
}

// Rotating --trunk-scan targets, the label is the target's own id.
static int
test_trunk_scan_row_carries_target_id(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    arm_trunk_scan_label(&state, &opts, "SiteA");

    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.dmr_color_code = 7U;
    state.dmr_t3_syscode = 0xABCU;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 50061U, 123456U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* item = &state.event_history_s[0].Event_History_Items[0];
    int rc = expect_str_eq("trunk scan row keeps the target id", item->channel_label, "SiteA");
    rc |= expect_has_substr("trunk scan row renders the target id", item->event_string,
                            "2026-04-30 00:00:00 [SiteA] TEST TGT: 00050061; SRC: 00123456; CC: 07; SYS: ABC;");
    dsd_state_ext_free_all(&state);
    return rc;
}

// The channel label and the CSV talkgroup labels answer different questions -- where the call was
// heard, and who it was -- so a row that has both shows both.
static int
test_channel_label_coexists_with_policy_label(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    int rc = expect_int("scan list name stores", arm_scanner_label(&state, &opts, 0, "Fire Dispatch"), 0);
    rc |= expect_int("policy label appends", append_policy_label(&state, 50061U, "D", "Dispatch"), 0);

    state.lastsynctype = DSD_SYNC_P25P2_POS;
    state.nac = 0x293;
    state.p2_cc = 0x293ULL;    /* a Phase 2 call's NAC is the one its descrambler runs on, */
    state.p2_cc_verified = 1U; /* proven on this carrier */
    state.p2_wacn = 0x45564U;
    state.p2_sysid = 0x006U;
    state.p2_rfssid = 10U;
    state.p2_siteid = 10U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 50061U, 5790062U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* item = &state.event_history_s[0].Event_History_Items[0];
    rc |= expect_has_substr("labelled row keeps the channel prefix", item->event_string, "[Fire Dispatch] TEST TGT:");
    rc |= expect_has_substr("labelled row keeps the policy label", item->event_string, "TName: Dispatch; Mode: D;");

    dsd_state_trunk_lcn_name_free(&state);
    dsd_state_ext_free_all(&state);
    return rc;
}

// The label is an addition, not a reshuffle: a receiver that is not scanning a named channel must
// render exactly the string it rendered before, byte for byte.
static int
test_unlabelled_row_string_is_unchanged(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    state.lastsynctype = DSD_SYNC_P25P2_POS;
    state.nac = 0x293;
    state.p2_cc = 0x293ULL;    /* a Phase 2 call's NAC is the one its descrambler runs on, */
    state.p2_cc_verified = 1U; /* proven on this carrier */
    state.p2_wacn = 0x45564U;
    state.p2_sysid = 0x006U;
    state.p2_rfssid = 10U;
    state.p2_siteid = 10U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 50061U, 5790062U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* item = &state.event_history_s[0].Event_History_Items[0];
    int rc = expect_int("unlabelled row carries no label", item->channel_label[0], '\0');
    rc |= expect_str_eq("unlabelled row string is unchanged", item->event_string,
                        "2026-04-30 00:00:00 TEST TGT: 00050061; SRC: 05790062; NAC: 293; "
                        "NET_STS: 45564:006:10.10; Group; ");
    dsd_state_ext_free_all(&state);
    return rc;
}

// A reacquired segment is the same transmission on the same channel, so the label survives both
// the ring shift that an interleaved notice causes and the merge that folds the segment in -- even
// though the scanner has already rolled on to the next channel by then.
static int
test_channel_label_survives_push_and_merge(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    int rc = expect_int("scan list name stores", arm_scanner_label(&state, &opts, 0, "Fire Dispatch"), 0);

    // Late entry: the first segment knows the talkgroup only.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_has_substr("first segment is labelled", event_history[0].Event_History_Items[1].event_string,
                            "[Fire Dispatch] TEST");

    // A system notice shifts the whole ring: the committed voice row moves to index 2.
    assert(dsd_event_emit_system_notice(&opts, &state, 0U, "System notice;") == 0);

    // The scanner rolls on while the transmission is still flapping.
    rc |= expect_int("next scan list name stores", dsd_state_trunk_lcn_name_set(&state, 1U, "PD Tac"), 0);
    state.lcn_freq_count = 3;
    state.lcn_freq_roll = 2;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* merged = &event_history[0].Event_History_Items[2];
    rc |= expect_int("notice plus merged voice leave two rows", committed_history_rows(&event_history[0]), 2);
    rc |= expect_has_substr("merged row carries the late source", merged->event_string, "SRC: 00000201;");
    rc |= expect_str_eq("shifted row keeps its label", merged->channel_label, "Fire Dispatch");
    rc |= expect_has_substr("merged row re-renders the original label", merged->event_string, "[Fire Dispatch] TEST");
    rc |= expect_no_substr("merged row does not adopt the new channel", merged->event_string, "[PD Tac]");

    dsd_state_trunk_lcn_name_free(&state);
    dsd_state_ext_free_all(&state);
    return rc;
}

// The label is stamped once per call epoch, on the epoch's first render. Under -Y the scanner
// advances lcn_freq_roll before the call ends, and the row is rendered once more on the finalize
// pass; re-resolving there would relabel a committed row with a channel it was never heard on.
static int
test_channel_label_is_frozen_at_first_render(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    int rc = expect_int("scan list name stores", arm_scanner_label(&state, &opts, 0, "Fire Dispatch"), 0);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    rc |= expect_int("next scan list name stores", dsd_state_trunk_lcn_name_set(&state, 1U, "PD Tac"), 0);
    state.lcn_freq_count = 3;
    state.lcn_freq_roll = 2;

    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* committed = &event_history[0].Event_History_Items[1];
    rc |= expect_int("frozen label commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_str_eq("committed row keeps the label it was heard on", committed->channel_label, "Fire Dispatch");
    rc |= expect_has_substr("finalize pass does not relabel the row", committed->event_string, "[Fire Dispatch] TEST");
    rc |= expect_no_substr("finalize pass does not adopt the new channel", committed->event_string, "[PD Tac]");

    dsd_state_trunk_lcn_name_free(&state);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
expect_i64(const char* label, int64_t got, int64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %lld want %lld\n", label, (long long)got, (long long)want);
        return 1;
    }
    return 0;
}

// A row's access code, kind and value together.
static int
expect_access_code(const char* label, const Event_History* item, dsd_access_code_kind kind, uint16_t value) {
    if (item->access_code_kind != (uint8_t)kind || item->access_code != value) {
        DSD_FPRINTF(stderr, "%s: got kind %u code 0x%X want kind %u code 0x%X\n", label,
                    (unsigned)item->access_code_kind, (unsigned)item->access_code, (unsigned)kind, (unsigned)value);
        return 1;
    }
    return 0;
}

// The receiver an RTL-family input puts behind the session, tuned to `center_hz`.
static void
arm_radio_input(dsd_opts* opts, uint32_t center_hz) {
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = center_hz;
}

// A group voice observation carrying a frequency of its own, as a grant or a followed voice channel gives one.
static int
observe_test_call_freq(dsd_state* state, int protocol, uint64_t target_id, uint64_t source_id, int64_t frequency_hz,
                       dsd_call_boundary boundary) {
    const dsd_call_observation observation = {
        .protocol = protocol,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = target_id,
        .policy_target_id = target_id,
        .ota_source_id = source_id,
        .frequency_hz = frequency_hz,
        .observed_m = g_observed_m,
    };
    g_observed_m += 0.1;
    return dsd_call_state_observe(state, &observation, boundary);
}

// A row records the frequency its call was heard on. With no frequency of its own the call takes the receiver's, and
// takes it once, at the epoch's first active render, the way the channel label is pinned. Re-reading the tuner while
// the call is live would label it with the next channel: a typed -Y row's queued tune writes rtlsdr_center_freq before
// the scan commit ends the outgoing calls, and command handlers render the still-live call in between.
static int
test_row_frequency_is_pinned_at_first_active_render(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    arm_radio_input(&opts, 851012500U);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    int rc = expect_i64("first active render takes the tuned frequency",
                        event_history[0].Event_History_Items[0].freq_hz, 851012500);

    // Still the same epoch: the observation continues the live call.
    opts.rtlsdr_center_freq = 852000000U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 0);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_i64("a later active render keeps the pinned frequency",
                     event_history[0].Event_History_Items[0].freq_hz, 851012500);

    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    const Event_History* committed = &event_history[0].Event_History_Items[1];
    rc |= expect_int("pinned frequency commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_i64("committed row keeps the frequency it was heard on", committed->freq_hz, 851012500);

    // A call first rendered after it ended takes no tuner reading: by then the receiver may be on the next channel,
    // and unknown is safer than wrong.
    reset_fixture(&opts, &state, event_history);
    arm_radio_input(&opts, 852000000U);
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("ended first render commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_i64("ended first render takes no tuner reading", event_history[0].Event_History_Items[1].freq_hz, 0);

    dsd_state_ext_free_all(&state);
    return rc;
}

// The call's own frequency -- a grant's channel, the voice channel a trunking receiver followed -- names the carrier
// exactly, so it outranks the tuner on every render, also when it is decoded after the tuner value was pinned and
// when the first render is the ended one.
static int
test_call_frequency_outranks_the_tuner(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    arm_radio_input(&opts, 851012500U);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    int rc =
        expect_i64("tuner frequency before the call's own", event_history[0].Event_History_Items[0].freq_hz, 851012500);
    assert(observe_test_call_freq(&state, DSD_SYNC_DMR_BS_VOICE_POS, 100U, 201U, 853037500, DSD_CALL_BOUNDARY_CONTINUE)
           == 0);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_i64("a later call frequency replaces the pinned tuner value",
                     event_history[0].Event_History_Items[0].freq_hz, 853037500);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_i64("committed row keeps the call's frequency", event_history[0].Event_History_Items[1].freq_hz,
                     853037500);

    reset_fixture(&opts, &state, event_history);
    arm_radio_input(&opts, 851012500U);
    assert(observe_test_call_freq(&state, DSD_SYNC_DMR_BS_VOICE_POS, 100U, 201U, 853037500, DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_i64("an ended first render still takes the call's frequency",
                     event_history[0].Event_History_Items[1].freq_hz, 853037500);

    dsd_state_ext_free_all(&state);
    return rc;
}

// Off a radio input there is no tuner reading to take, rigctl on an audio input included: rtlsdr_center_freq then
// holds a made-up 850 MHz default, or a frequency changed on the peer is invisible. A call's own frequency still
// shows.
static int
test_audio_input_row_takes_no_tuner_value(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    int rc = 0;
    const int audio_inputs[] = {AUDIO_IN_PULSE, AUDIO_IN_UDP};
    for (size_t i = 0; i < sizeof audio_inputs / sizeof audio_inputs[0]; i++) {
        reset_fixture(&opts, &state, event_history);
        opts.audio_in_type = audio_inputs[i];
        opts.use_rigctl = 1;
        opts.rtlsdr_center_freq = 850000000U;
        assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                                 DSD_CALL_BOUNDARY_BEGIN)
               == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        rc |= expect_i64("audio input row has no frequency", event_history[0].Event_History_Items[0].freq_hz, 0);
        assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        rc |= expect_i64("audio input committed row has no frequency", event_history[0].Event_History_Items[1].freq_hz,
                         0);

        assert(observe_test_call_freq(&state, DSD_SYNC_DMR_BS_VOICE_POS, 300U, 401U, 853037500, DSD_CALL_BOUNDARY_BEGIN)
               == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        rc |= expect_i64("audio input row still shows the call's frequency",
                         event_history[0].Event_History_Items[0].freq_hz, 853037500);
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

// While the call is live a valid code replaces the row's, so a colour code that locks after the call opened still
// lands. Once the call has ended the code is frozen: a finalize pass can run after a scan hop or a trunk-scan
// snapshot restore, and must not label the call with the next carrier's code.
static int
test_access_code_refreshes_while_live_and_freezes_at_the_end(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    state.dmr_color_code = 16U;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    int rc = expect_access_code("unlocked colour code is unknown", &event_history[0].Event_History_Items[0],
                                DSD_ACCESS_CODE_NONE, 0U);

    state.dmr_color_code = 3U;
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_access_code("a colour code locked while live lands", &event_history[0].Event_History_Items[0],
                             DSD_ACCESS_CODE_COLOR_CODE, 3U);

    // A re-lock onto a different code while the call is still live -- the BS lock follows sustained evidence of a
    // new colour code -- replaces the known one: refreshed, not filled once.
    state.dmr_color_code = 5U;
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_access_code("a different live code replaces a known one", &event_history[0].Event_History_Items[0],
                             DSD_ACCESS_CODE_COLOR_CODE, 5U);

    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    state.dmr_color_code = 7U;
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("frozen code commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_access_code("ended render keeps the code the call was heard with",
                             &event_history[0].Event_History_Items[1], DSD_ACCESS_CODE_COLOR_CODE, 5U);

    dsd_state_ext_free_all(&state);
    return rc;
}

// Frozen means frozen both ways: a call that ended with no code known keeps none, rather than taking whatever the
// decoder locks onto next.
static int
test_unknown_access_code_stays_unknown_after_the_end(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    state.dmr_color_code = 16U;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    state.dmr_color_code = 5U;
    dsd_event_sync_slot(&opts, &state, 0U);
    int rc = expect_int("unknown code commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_access_code("an ended call takes no code", &event_history[0].Event_History_Items[1],
                             DSD_ACCESS_CODE_NONE, 0U);

    // A call first rendered after it ended takes no code either, valid live reading or not: by then the decoder may
    // hold the next carrier's code.
    reset_fixture(&opts, &state, event_history);
    state.dmr_color_code = 3U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("ended first render commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_access_code("an ended first render takes no code", &event_history[0].Event_History_Items[1],
                             DSD_ACCESS_CODE_NONE, 0U);

    dsd_state_ext_free_all(&state);
    return rc;
}

// A staged code is this call's only when its kind is the one the call's protocol carries; a code of another kind is
// treated as unknown, while the call is live and once it has ended. The canonical path never stages one (a protocol
// family change opens a new epoch, and the row is cleared between epochs), so the test writes it into the staged row
// directly, standing in for any row that reached the render from elsewhere.
static int
test_staged_access_code_of_another_kind_is_not_reused(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    state.dmr_color_code = 16U;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    Event_History* staged = &event_history[0].Event_History_Items[0];
    staged->access_code_kind = (uint8_t)DSD_ACCESS_CODE_NAC;
    staged->access_code = 0x293U;
    dsd_event_sync_slot(&opts, &state, 0U);
    int rc = expect_access_code("a live render drops a staged code of another kind", staged, DSD_ACCESS_CODE_NONE, 0U);

    staged->access_code_kind = (uint8_t)DSD_ACCESS_CODE_NAC;
    staged->access_code = 0x293U;
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("kind-mismatch row commits", committed_history_rows(&event_history[0]), 1);
    rc |= expect_access_code("an ended render drops a staged code of another kind",
                             &event_history[0].Event_History_Items[1], DSD_ACCESS_CODE_NONE, 0U);

    dsd_state_ext_free_all(&state);
    return rc;
}

// The sentinels a carrier boundary writes -- DMR 16, P25 nac 0 on a no-carrier pass -- say "nothing decoded now",
// not "no code", so they never erase a code the live call already has.
static int
test_invalid_live_access_code_never_erases_a_known_one(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    state.dmr_color_code = 3U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    state.dmr_color_code = 16U;
    dsd_event_sync_slot(&opts, &state, 0U);
    int rc = expect_access_code("DMR 16 keeps the known colour code", &event_history[0].Event_History_Items[0],
                                DSD_ACCESS_CODE_COLOR_CODE, 3U);

    reset_fixture(&opts, &state, event_history);
    state.nac = 0x293;
    assert(observe_test_call(&state, 0U, DSD_SYNC_P25P1_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    state.nac = 0;
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_access_code("P25 nac 0 keeps the known NAC", &event_history[0].Event_History_Items[0],
                             DSD_ACCESS_CODE_NAC, 0x293U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_access_code("committed row keeps the known NAC", &event_history[0].Event_History_Items[1],
                             DSD_ACCESS_CODE_NAC, 0x293U);

    dsd_state_ext_free_all(&state);
    return rc;
}

// A reacquired segment is the same transmission, so it may fill a frequency or code the first segment never learned,
// but never replace one: by the time it merges the receiver may have moved on.
static int
test_reacquisition_merge_fills_frequency_and_code_only(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    arm_radio_input(&opts, 851012500U);
    state.dmr_color_code = 3U;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    opts.rtlsdr_center_freq = 852000000U;
    state.dmr_color_code = 5U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* merged = &event_history[0].Event_History_Items[1];
    int rc = expect_int("reacquired transmission stays one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_i64("merge never overwrites a known frequency", merged->freq_hz, 851012500);
    rc |= expect_access_code("merge never overwrites a known code", merged, DSD_ACCESS_CODE_COLOR_CODE, 3U);

    // The other direction: a first segment that learned neither takes the reacquired segment's.
    reset_fixture(&opts, &state, event_history);
    state.dmr_color_code = 16U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_i64("first segment learned no frequency", event_history[0].Event_History_Items[1].freq_hz, 0);

    arm_radio_input(&opts, 852000000U);
    state.dmr_color_code = 5U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    merged = &event_history[0].Event_History_Items[1];
    rc |= expect_int("filled reacquisition stays one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_i64("merge fills an unknown frequency", merged->freq_hz, 852000000);
    rc |= expect_access_code("merge fills an unknown code", merged, DSD_ACCESS_CODE_COLOR_CODE, 5U);

    dsd_state_ext_free_all(&state);
    return rc;
}

// Render one active call of `protocol` into the staged row with the decoder holding the given codes.
static const Event_History*
render_code_row(dsd_opts* opts, dsd_state* state, Event_History_I event_history[2], int protocol) {
    assert(observe_test_call(state, 0U, protocol, DSD_CALL_KIND_GROUP_VOICE, 51002U, 41001U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    watchdog_event_current(opts, state, 0);
    return &event_history[0].Event_History_Items[0];
}

// A code the decoder never decoded reads as its sentinel -- DMR 16, NXDN (unsigned)-1, dPMR -1, which every carrier
// boundary now leaves (issue #575) -- and the system identity string and the event line print "--" for it, as the
// terminal does, rather than the sentinel. NXDN's stand-ins (an IDAS area bit or site type, DCR's fixed 7) are no RAN
// either. The numeric sys ids keep the raw value: 0 is a code.
static int
test_unknown_access_codes_render_as_dashes(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    int rc = 0;

    reset_fixture(&opts, &state, event_history);
    state.dmr_color_code = 16U;
    const Event_History* item = render_code_row(&opts, &state, event_history, DSD_SYNC_DMR_BS_VOICE_POS);
    rc |= expect_str_eq("unknown DMR CC sysid", item->sysid_string, "DMR_CC_--");
    rc |= expect_has_substr("unknown DMR CC event", item->event_string, "SRC: 00041001; CC: --; ");
    rc |= expect_int("unknown DMR CC keeps its numeric id", (int)item->sys_id2, 16);

    reset_fixture(&opts, &state, event_history);
    state.dmr_color_code = 16U;
    state.dmr_t3_syscode = 0xABCU;
    item = render_code_row(&opts, &state, event_history, DSD_SYNC_DMR_MS_VOICE);
    rc |= expect_str_eq("unknown DMR CC sysid with system code", item->sysid_string, "DMR_ABC_CC_--");
    rc |= expect_has_substr("unknown DMR CC event with system code", item->event_string, "CC: --; SYS: ABC;");

    reset_fixture(&opts, &state, event_history);
    state.dmr_color_code = 0U;
    item = render_code_row(&opts, &state, event_history, DSD_SYNC_DMR_BS_VOICE_POS);
    rc |= expect_str_eq("DMR CC 0 sysid", item->sysid_string, "DMR_CC_0");
    rc |= expect_has_substr("DMR CC 0 event", item->event_string, "CC: 00; ");

    reset_fixture(&opts, &state, event_history);
    state.nxdn_last_ran = (unsigned int)-1;
    item = render_code_row(&opts, &state, event_history, DSD_SYNC_NXDN_POS);
    rc |= expect_str_eq("unknown NXDN RAN sysid", item->sysid_string, "NXDN_RAN_--");
    rc |= expect_has_substr("unknown NXDN RAN event", item->event_string, "SRC: 00041001; RAN: --; ");
    rc |= expect_u64("unknown NXDN RAN keeps its numeric id", item->sys_id3, 0xFFFFFFFFU);

    reset_fixture(&opts, &state, event_history);
    state.nxdn_last_ran = (unsigned int)-1;
    state.nxdn_location_site_code = 3U;
    state.nxdn_location_sys_code = 12U;
    item = render_code_row(&opts, &state, event_history, DSD_SYNC_NXDN_NEG);
    rc |= expect_str_eq("unknown NXDN RAN sysid with site", item->sysid_string, "NXDN_12_3_RAN_--");
    rc |= expect_has_substr("unknown NXDN RAN event with site", item->event_string, "RAN: --; SYS: 12.3; ");

    reset_fixture(&opts, &state, event_history);
    state.nxdn_last_ran = 7U;
    state.nxdn_last_ran_stand_in = 1U;
    item = render_code_row(&opts, &state, event_history, DSD_SYNC_NXDN_POS);
    rc |= expect_str_eq("DCR stand-in sysid", item->sysid_string, "NXDN_RAN_--");
    rc |= expect_has_substr("DCR stand-in event", item->event_string, "RAN: --; ");
    rc |= expect_int("DCR stand-in keeps its numeric id", (int)item->sys_id3, 7);

    reset_fixture(&opts, &state, event_history);
    state.nxdn_last_ran = 0U;
    item = render_code_row(&opts, &state, event_history, DSD_SYNC_NXDN_POS);
    rc |= expect_str_eq("NXDN RAN 0 sysid", item->sysid_string, "NXDN_RAN_0");
    rc |= expect_has_substr("NXDN RAN 0 event", item->event_string, "RAN: 00; ");

    reset_fixture(&opts, &state, event_history);
    state.dpmr_color_code = -1;
    item = render_code_row(&opts, &state, event_history, DSD_SYNC_DPMR_FS2_POS);
    rc |= expect_str_eq("unknown dPMR CC sysid", item->sysid_string, "DPMR_CC_--");
    reset_fixture(&opts, &state, event_history);
    state.dpmr_color_code = 0;
    item = render_code_row(&opts, &state, event_history, DSD_SYNC_DPMR_FS2_POS);
    rc |= expect_str_eq("dPMR CC 0 sysid", item->sysid_string, "DPMR_CC_0");

    dsd_state_ext_free_all(&state);
    return rc;
}

// The dPMR event line names the row's decoded colour code. It used to print the call's channel, which the protocol
// sets to 0 when no colour code decoded, so an unknown code read as CC 0 (issue #575). The channel field is untouched.
static int
test_dpmr_event_line_prints_the_decoded_colour_code(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    int rc = 0;

    reset_fixture(&opts, &state, event_history);
    state.dpmr_color_code = -1;
    const Event_History* item = render_code_row(&opts, &state, event_history, DSD_SYNC_DPMR_FS2_POS);
    rc |= expect_has_substr("undecoded dPMR CC event", item->event_string, "TEST CC: --; TGT: ");
    rc |= expect_no_substr("undecoded dPMR CC is not CC 0", item->event_string, "CC: 00");
    rc |= expect_int("undecoded dPMR CC row channel", (int)item->channel, 0);

    reset_fixture(&opts, &state, event_history);
    state.dpmr_color_code = 0;
    item = render_code_row(&opts, &state, event_history, DSD_SYNC_DPMR_FS2_POS);
    rc |= expect_has_substr("dPMR CC 0 event", item->event_string, "TEST CC: 00; TGT: ");

    reset_fixture(&opts, &state, event_history);
    state.dpmr_color_code = 42;
    item = render_code_row(&opts, &state, event_history, DSD_SYNC_DPMR_FS2_POS);
    rc |= expect_has_substr("dPMR CC 42 event", item->event_string, "TEST CC: 42; TGT: ");
    rc |= expect_int("dPMR row channel stays the call's", (int)item->channel, 0);

    dsd_state_ext_free_all(&state);
    return rc;
}

// Render one active call of `protocol` with the given service options into the staged row.
static const Event_History*
render_svc_row(dsd_opts* opts, dsd_state* state, Event_History_I event_history[2], int protocol, uint16_t svc,
               uint8_t has_svc) {
    const dsd_call_observation observation = {
        .protocol = protocol,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 51002U,
        .policy_target_id = 51002U,
        .ota_source_id = 41001U,
        .service_options = svc,
        .has_service_metadata = has_svc,
        .observed_m = g_observed_m,
    };
    g_observed_m += 0.1;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    watchdog_event_current(opts, state, 0);
    return &event_history[0].Event_History_Items[0];
}

static void
set_p25_site(dsd_state* state) {
    state->p2_wacn = 0x45564U;
    state->p2_sysid = 0x006U;
    state->p2_rfssid = 10U;
    state->p2_siteid = 10U;
}

// The P25 NAC and the M17 CAN in the system identity string and the event line follow the row's access code, as the
// other protocols' codes do: a Phase 1 call takes nac only, so a p2_cc an earlier Phase 2 carrier left is no code of
// its, and an M17 call names a CAN only once its service options were observed. An unknown NAC prints "---", three
// wide like the hex code, so the long form keeps its field positions. The numeric sys ids keep the raw value.
static int
test_unknown_p25_and_m17_codes_render_as_dashes(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    int rc = 0;

    reset_fixture(&opts, &state, event_history);
    state.nac = 0;
    state.p2_cc = 0x293ULL;
    const Event_History* item = render_svc_row(&opts, &state, event_history, DSD_SYNC_P25P1_POS, 0U, 0U);
    rc |= expect_access_code("P25p1 with nac 0 has no code", item, DSD_ACCESS_CODE_NONE, 0U);
    rc |= expect_str_eq("P25p1 unknown NAC sysid", item->sysid_string, "P25_---");
    rc |= expect_has_substr("P25p1 unknown NAC event", item->event_string, "SRC: 00041001; NAC: ---; ");
    rc |= expect_int("P25p1 unknown NAC keeps its numeric id", (int)item->sys_id3, 0x293);

    reset_fixture(&opts, &state, event_history);
    state.nac = 0;
    state.p2_cc = 0x293ULL;
    set_p25_site(&state);
    item = render_svc_row(&opts, &state, event_history, DSD_SYNC_P25P1_NEG, 0U, 0U);
    rc |= expect_str_eq("P25p1 unknown NAC long sysid", item->sysid_string, "P25_45564006---_10_10");
    rc |= expect_has_substr("P25p1 unknown NAC long event", item->event_string, "NAC: ---; NET_STS: 45564:006:10.10; ");

    reset_fixture(&opts, &state, event_history);
    state.nac = 0x293;
    item = render_svc_row(&opts, &state, event_history, DSD_SYNC_P25P1_POS, 0U, 0U);
    rc |= expect_str_eq("P25p1 NAC sysid", item->sysid_string, "P25_293");
    rc |= expect_has_substr("P25p1 NAC event", item->event_string, "NAC: 293; ");

    reset_fixture(&opts, &state, event_history);
    state.p2_cc = 0x293ULL;
    state.p2_cc_verified = 1U;
    set_p25_site(&state);
    item = render_svc_row(&opts, &state, event_history, DSD_SYNC_P25P2_POS, 0U, 0U);
    rc |= expect_str_eq("P25p2 NAC long sysid", item->sysid_string, "P25_45564006293_10_10");
    rc |= expect_has_substr("P25p2 NAC long event", item->event_string, "NAC: 293; NET_STS: 45564:006:10.10; ");
    reset_fixture(&opts, &state, event_history);
    state.p2_cc = 0x293ULL;
    state.p2_cc_verified = 1U;
    item = render_svc_row(&opts, &state, event_history, DSD_SYNC_P25P2_NEG, 0U, 0U);
    rc |= expect_str_eq("P25p2 NAC sysid", item->sysid_string, "P25_293");
    /* A seed no burst on this carrier has proven (-X, or another carrier's) is no NAC the call was heard with. */
    reset_fixture(&opts, &state, event_history);
    state.p2_cc = 0x293ULL;
    item = render_svc_row(&opts, &state, event_history, DSD_SYNC_P25P2_POS, 0U, 0U);
    rc |= expect_access_code("P25p2 unproven seed has no code", item, DSD_ACCESS_CODE_NONE, 0U);
    rc |= expect_str_eq("P25p2 unproven seed sysid", item->sysid_string, "P25_---");
    rc |= expect_has_substr("P25p2 unproven seed event", item->event_string, "NAC: ---; ");

    reset_fixture(&opts, &state, event_history);
    item = render_svc_row(&opts, &state, event_history, DSD_SYNC_M17_LSF_POS, 0x13U, 0U);
    rc |= expect_str_eq("M17 without service options sysid", item->sysid_string, "M17_CAN_--");
    rc |= expect_has_substr("M17 without service options event", item->event_string, " CAN: --;");

    reset_fixture(&opts, &state, event_history);
    item = render_svc_row(&opts, &state, event_history, DSD_SYNC_M17_LSF_POS, 0x13U, 1U);
    rc |= expect_str_eq("M17 CAN sysid", item->sysid_string, "M17_CAN_3");
    rc |= expect_has_substr("M17 CAN event", item->event_string, " CAN: 03;");

    dsd_state_ext_free_all(&state);
    return rc;
}

// One segment of a transmission whose access code rides in the observation or in the decoder state, then the
// reacquired segment after a sync loss. Returns the merged row.
static const Event_History*
merge_observed_segments(dsd_opts* opts, dsd_state* state, Event_History_I event_history[2], int protocol,
                        uint8_t first_has_svc, int first_nac, uint8_t second_has_svc, int second_nac) {
    dsd_call_observation observation = {
        .protocol = protocol,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 100U,
        .policy_target_id = 100U,
        .ota_source_id = 201U,
        .service_options = 0x13U,
        .has_service_metadata = first_has_svc,
        .observed_m = g_observed_m,
    };
    g_observed_m += 0.1;
    state->nac = first_nac;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(opts, state, 0U);
    assert(end_test_call(state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(opts, state, 0U);

    observation.has_service_metadata = second_has_svc;
    observation.observed_m = g_observed_m;
    g_observed_m += 0.1;
    state->nac = second_nac;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 1);
    dsd_event_sync_slot(opts, state, 0U);
    assert(end_test_call(state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(opts, state, 0U);
    assert(committed_history_rows(&event_history[0]) == 1);
    return &event_history[0].Event_History_Items[1];
}

// A reacquired segment that decoded the NAC or the CAN fills the row's access code, and the system identity string and
// the event line name it.
static int
test_reacquisition_merge_names_a_p25_or_m17_code(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    int rc = 0;

    reset_fixture(&opts, &state, event_history);
    state.p2_cc = 0x293ULL; /* left by an earlier Phase 2 carrier: no code of this Phase 1 call's */
    const Event_History* merged =
        merge_observed_segments(&opts, &state, event_history, DSD_SYNC_P25P1_POS, 1U, 0, 1U, 0x2A1);
    rc |= expect_access_code("merge fills the NAC", merged, DSD_ACCESS_CODE_NAC, 0x2A1U);
    rc |= expect_str_eq("merged P25 sysid names the NAC", merged->sysid_string, "P25_2A1");
    rc |= expect_has_substr("merged P25 event names the NAC", merged->event_string, "NAC: 2A1; ");

    reset_fixture(&opts, &state, event_history);
    merged = merge_observed_segments(&opts, &state, event_history, DSD_SYNC_M17_STR_POS, 0U, 0, 1U, 0);
    rc |= expect_access_code("merge fills the CAN", merged, DSD_ACCESS_CODE_CAN, 3U);
    rc |= expect_str_eq("merged M17 sysid names the CAN", merged->sysid_string, "M17_CAN_3");
    rc |= expect_has_substr("merged M17 event names the CAN", merged->event_string, " CAN: 03;");

    dsd_state_ext_free_all(&state);
    return rc;
}

// What one segment of a transmission decoded: its access code (DMR colour code, NXDN RAN or dPMR colour code, each
// at its own sentinel when undecoded), NXDN's stand-in mark, and the system ids beside it.
typedef struct {
    int code;
    uint8_t stand_in;
    uint32_t dmr_syscode;
    uint16_t nxdn_site_code;
    uint32_t nxdn_sys_code;
} code_segment;

static void
set_code_segment(dsd_state* state, int protocol, const code_segment* segment) {
    if (DSD_SYNC_IS_NXDN(protocol)) {
        state->nxdn_last_ran = (unsigned int)segment->code;
        state->nxdn_last_ran_stand_in = segment->stand_in;
        state->nxdn_location_site_code = segment->nxdn_site_code;
        state->nxdn_location_sys_code = segment->nxdn_sys_code;
    } else if (DSD_SYNC_IS_DPMR(protocol)) {
        state->dpmr_color_code = segment->code;
    } else {
        state->dmr_color_code = (unsigned int)segment->code;
        state->dmr_t3_syscode = segment->dmr_syscode;
    }
}

// One transmission in two segments, the second reacquiring the first after a sync loss. Returns the merged row.
static const Event_History*
merge_segments(dsd_opts* opts, dsd_state* state, Event_History_I event_history[2], int protocol,
               const code_segment* first, const code_segment* second) {
    set_code_segment(state, protocol, first);
    assert(
        observe_test_call(state, 0U, protocol, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U, DSD_CALL_BOUNDARY_BEGIN)
        == 1);
    dsd_event_sync_slot(opts, state, 0U);
    assert(end_test_call(state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(opts, state, 0U);

    set_code_segment(state, protocol, second);
    assert(observe_test_call(state, 0U, protocol, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(opts, state, 0U);
    assert(end_test_call(state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(opts, state, 0U);
    assert(committed_history_rows(&event_history[0]) == 1);
    return &event_history[0].Event_History_Items[1];
}

// The same, for segments that differ only in their access code.
static const Event_History*
merge_code_segments(dsd_opts* opts, dsd_state* state, Event_History_I event_history[2], int protocol,
                    unsigned int first_code, uint8_t first_stand_in, unsigned int second_code) {
    const code_segment first = {.code = (int)first_code, .stand_in = first_stand_in};
    const code_segment second = {.code = (int)second_code};
    return merge_segments(opts, state, event_history, protocol, &first, &second);
}

// A dPMR row's system identity is its colour code. A reacquired segment that decoded the code the first one did not
// fills the row's access code, and the system identity string follows it, as the event line does.
static int
test_reacquisition_merge_names_a_dpmr_code_in_the_system_identity(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    const code_segment first = {.code = -1};
    const code_segment second = {.code = 9};
    const Event_History* merged = merge_segments(&opts, &state, event_history, DSD_SYNC_DPMR_FS2_POS, &first, &second);
    int rc = expect_access_code("merge fills the dPMR colour code", merged, DSD_ACCESS_CODE_COLOR_CODE, 9U);
    rc |= expect_has_substr("merged dPMR event names the code", merged->event_string, "TEST CC: 09; TGT: ");
    rc |= expect_str_eq("merged dPMR sysid names the code", merged->sysid_string, "DPMR_CC_9");
    dsd_state_ext_free_all(&state);
    return rc;
}

// A reacquired segment can decode a system id the first one did not while missing the code the first one decoded.
// The row keeps the code and takes the system id, and its system identity string names both.
static int
test_reacquisition_merge_keeps_the_code_and_takes_a_later_system_id(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    int rc = 0;

    reset_fixture(&opts, &state, event_history);
    const code_segment dmr_first = {.code = 3};
    const code_segment dmr_second = {.code = 16, .dmr_syscode = 0xABCU};
    const Event_History* merged =
        merge_segments(&opts, &state, event_history, DSD_SYNC_DMR_BS_VOICE_POS, &dmr_first, &dmr_second);
    rc |= expect_int("DMR merge keeps the colour code", (int)merged->sys_id2, 3);
    rc |= expect_int("DMR merge takes the system code", (int)merged->sys_id1, 0xABC);
    rc |= expect_str_eq("merged DMR sysid names both", merged->sysid_string, "DMR_ABC_CC_3");
    rc |= expect_has_substr("merged DMR event names both", merged->event_string, "CC: 03; SYS: ABC;");

    reset_fixture(&opts, &state, event_history);
    const code_segment nxdn_first = {.code = 5};
    const code_segment nxdn_second = {.code = -1, .nxdn_site_code = 3U, .nxdn_sys_code = 12U};
    merged = merge_segments(&opts, &state, event_history, DSD_SYNC_NXDN_POS, &nxdn_first, &nxdn_second);
    rc |= expect_int("NXDN merge keeps the RAN", (int)merged->sys_id3, 5);
    rc |= expect_int("NXDN merge takes the site code", (int)merged->sys_id1, 3);
    rc |= expect_str_eq("merged NXDN sysid names both", merged->sysid_string, "NXDN_12_3_RAN_5");
    rc |= expect_has_substr("merged NXDN event names both", merged->event_string, "RAN: 05; SYS: 12.3;");

    dsd_state_ext_free_all(&state);
    return rc;
}

// A reacquired segment that decoded the code fills a system identity the first segment rendered without one, 0
// included (a code for DMR and NXDN alike); a segment that decoded none never replaces one, 0 included.
static int
test_reacquisition_merge_upgrades_an_unknown_system_code(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    int rc = 0;

    reset_fixture(&opts, &state, event_history);
    const Event_History* merged =
        merge_code_segments(&opts, &state, event_history, DSD_SYNC_DMR_BS_VOICE_POS, 16U, 0U, 5U);
    rc |= expect_int("merge fills an unknown DMR CC", (int)merged->sys_id2, 5);
    rc |= expect_str_eq("merged DMR sysid names the code", merged->sysid_string, "DMR_CC_5");
    rc |= expect_has_substr("merged DMR event names the code", merged->event_string, "CC: 05; ");

    reset_fixture(&opts, &state, event_history);
    merged = merge_code_segments(&opts, &state, event_history, DSD_SYNC_DMR_BS_VOICE_POS, 16U, 0U, 0U);
    rc |= expect_int("merge fills an unknown DMR CC with 0", (int)merged->sys_id2, 0);
    rc |= expect_str_eq("merged DMR sysid names CC 0", merged->sysid_string, "DMR_CC_0");
    rc |= expect_has_substr("merged DMR event names CC 0", merged->event_string, "CC: 00; ");

    reset_fixture(&opts, &state, event_history);
    merged = merge_code_segments(&opts, &state, event_history, DSD_SYNC_DMR_BS_VOICE_POS, 0U, 0U, 16U);
    rc |= expect_int("an undecoded segment keeps DMR CC 0", (int)merged->sys_id2, 0);
    rc |= expect_str_eq("an undecoded segment keeps the DMR sysid", merged->sysid_string, "DMR_CC_0");
    rc |= expect_has_substr("an undecoded segment keeps the DMR event code", merged->event_string, "CC: 00; ");

    reset_fixture(&opts, &state, event_history);
    merged = merge_code_segments(&opts, &state, event_history, DSD_SYNC_NXDN_POS, (unsigned int)-1, 0U, 0U);
    rc |= expect_int("merge fills an unknown NXDN RAN with 0", (int)merged->sys_id3, 0);
    rc |= expect_str_eq("merged NXDN sysid names RAN 0", merged->sysid_string, "NXDN_RAN_0");
    rc |= expect_has_substr("merged NXDN event names RAN 0", merged->event_string, "RAN: 00; ");

    reset_fixture(&opts, &state, event_history);
    merged = merge_code_segments(&opts, &state, event_history, DSD_SYNC_NXDN_POS, 0U, 0U, (unsigned int)-1);
    rc |= expect_int("an undecoded segment keeps NXDN RAN 0", (int)merged->sys_id3, 0);
    rc |= expect_str_eq("an undecoded segment keeps the NXDN sysid", merged->sysid_string, "NXDN_RAN_0");

    // The first segment's stand-in mark was left by an earlier DCR transmission beside an undecoded RAN; the RAN the
    // reacquired segment decoded is one.
    reset_fixture(&opts, &state, event_history);
    merged = merge_code_segments(&opts, &state, event_history, DSD_SYNC_NXDN_POS, (unsigned int)-1, 1U, 5U);
    rc |= expect_int("merge fills an unknown NXDN RAN", (int)merged->sys_id3, 5);
    rc |= expect_str_eq("merged NXDN sysid names the RAN", merged->sysid_string, "NXDN_RAN_5");
    rc |= expect_has_substr("merged NXDN event names the RAN", merged->event_string, "RAN: 05; ");

    dsd_state_ext_free_all(&state);
    return rc;
}

// A data notice is rendered once, while the receiver is still on the carrier that delivered it, so it reads the
// frequency and code live: its own frequency when the PDU named one, else the tuner's.
static int
test_data_notice_carries_frequency_and_access_code(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    arm_radio_input(&opts, 851012500U);
    state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.dmr_color_code = 9U;

    assert(emit_test_data_notice(&opts, &state, 1234U, 5678U, "Data SRC: 1234; TGT: 5678;", 0U) == 0);
    const Event_History* row = &event_history[0].Event_History_Items[1];
    int rc = expect_i64("data notice takes the tuned frequency", row->freq_hz, 851012500);
    rc |= expect_access_code("data notice takes the live colour code", row, DSD_ACCESS_CODE_COLOR_CODE, 9U);
    rc |= expect_str_eq("data notice string is unchanged", row->event_string,
                        "2026-04-30 00:00:00 Data SRC: 1234; TGT: 5678;");

    dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_P25P1_POS, 0U, 1234U, 5678U);
    observation.frequency_hz = 853037500;
    state.nac = 0x293;
    assert(dsd_event_emit_data_notice(&opts, &state, 0U, &observation, "P25 data;") == 0);
    row = &event_history[0].Event_History_Items[1];
    rc |= expect_i64("data notice's own frequency outranks the tuner", row->freq_hz, 853037500);
    rc |= expect_access_code("data notice takes the live NAC", row, DSD_ACCESS_CODE_NAC, 0x293U);

    observation = dsd_call_observation_data(DSD_SYNC_M17_PKT_POS, 0U, 1234U, 5678U);
    observation.service_options = 0x13U;
    observation.has_service_metadata = 1U;
    assert(dsd_event_emit_data_notice(&opts, &state, 0U, &observation, "M17 packet;") == 0);
    rc |= expect_access_code("M17 data notice takes its CAN", &event_history[0].Event_History_Items[1],
                             DSD_ACCESS_CODE_CAN, 3U);

    // Off a radio input, nothing to take but the codes.
    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.dmr_color_code = 16U;
    assert(emit_test_data_notice(&opts, &state, 1234U, 5678U, "Data;", 0U) == 0);
    rc |= expect_i64("audio input data notice has no frequency", event_history[0].Event_History_Items[1].freq_hz, 0);
    rc |= expect_access_code("unlocked colour code leaves the notice without one",
                             &event_history[0].Event_History_Items[1], DSD_ACCESS_CODE_NONE, 0U);

    dsd_state_ext_free_all(&state);
    return rc;
}

// Playing back files (--playfiles), the decoder state describes no carrier: the SDRTrunk JSON reader writes
// dmr_color_code = 0 per file, and the tuner is not what the recording was heard on. Rows take neither; a call's own
// frequency still shows.
static int
test_playfiles_rows_take_no_tuner_value_or_code(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.playfiles = 1;
    arm_radio_input(&opts, 851012500U);
    state.dmr_color_code = 0U;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    const Event_History* row = &event_history[0].Event_History_Items[1];
    int rc = expect_int("playfiles row commits", committed_history_rows(&event_history[0]), 1);
    rc |= expect_i64("playfiles row takes no tuner value", row->freq_hz, 0);
    rc |= expect_access_code("playfiles row takes no code", row, DSD_ACCESS_CODE_NONE, 0U);

    state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    assert(emit_test_data_notice(&opts, &state, 1234U, 5678U, "Data;", 0U) == 0);
    row = &event_history[0].Event_History_Items[1];
    rc |= expect_i64("playfiles data notice takes no tuner value", row->freq_hz, 0);
    rc |= expect_access_code("playfiles data notice takes no code", row, DSD_ACCESS_CODE_NONE, 0U);

    assert(observe_test_call_freq(&state, DSD_SYNC_DMR_BS_VOICE_POS, 300U, 401U, 853037500, DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_i64("playfiles row still shows the call's frequency", event_history[0].Event_History_Items[0].freq_hz,
                     853037500);

    dsd_state_ext_free_all(&state);
    return rc;
}

// An NXDN row names only its own channel and frequency. The last grant decoded is global: a duplicate assignment for
// another call decoded while tuned, or a -Y control-channel row's last grant carried onto a conventional row, would
// otherwise put another call's channel on this row.
static int
test_nxdn_row_names_only_its_own_channel(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_NXDN_POS;
    state.nxdn_last_ran = 23U;
    state.nxdn_grant_chan = 198U;
    state.nxdn_grant_freq = 453212500;

    assert(observe_test_call(&state, 0U, DSD_SYNC_NXDN_POS, DSD_CALL_KIND_GROUP_VOICE, 51002U, 41001U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    const Event_History* item = &event_history[0].Event_History_Items[0];
    int rc = expect_str_eq("conventional NXDN row does not name an unrelated grant", item->event_string,
                           "2026-04-30 00:00:00 TEST TGT: 00051002; SRC: 00041001; RAN: 23; Group; ");
    rc |= expect_access_code("conventional NXDN row records its RAN", item, DSD_ACCESS_CODE_RAN, 23U);

    // A followed grant: the call carries the channel and frequency it was granted. A later grant for another call,
    // decoded while tuned, moves the globals but not the receiver.
    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_NXDN_POS;
    state.nxdn_last_ran = 23U;
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_NXDN_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 51002U,
        .policy_target_id = 51002U,
        .ota_source_id = 41001U,
        .channel = 198U,
        .frequency_hz = 453212500,
    };
    assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    state.nxdn_grant_chan = 77U;
    state.nxdn_grant_freq = 451000000;
    dsd_event_sync_slot(&opts, &state, 0U);
    item = &event_history[0].Event_History_Items[0];
    rc |= expect_str_eq("followed NXDN grant names its own channel and frequency", item->event_string,
                        "2026-04-30 00:00:00 TEST TGT: 00051002; SRC: 00041001; RAN: 23; CH: 198; "
                        "FREQ: 453.212500 MHz; Group; ");

    // A conventional NXDN carrier on a radio input: the frequency it was heard on, with no channel to name.
    reset_fixture(&opts, &state, event_history);
    arm_radio_input(&opts, 453212500U);
    state.lastsynctype = DSD_SYNC_NXDN_POS;
    state.nxdn_last_ran = 23U;
    state.nxdn_grant_chan = 198U;
    state.nxdn_grant_freq = 451000000;
    assert(observe_test_call(&state, 0U, DSD_SYNC_NXDN_POS, DSD_CALL_KIND_GROUP_VOICE, 51002U, 41001U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_str_eq("conventional NXDN row on a radio names its frequency only",
                        event_history[0].Event_History_Items[0].event_string,
                        "2026-04-30 00:00:00 TEST TGT: 00051002; SRC: 00041001; RAN: 23; FREQ: 453.212500 MHz; "
                        "Group; ");

    dsd_state_ext_free_all(&state);
    return rc;
}

// A label says where the receiver was pointing, never who the call was: an identity-less voice
// epoch is still noise and must still be dropped. Rescuing it would put the stray-sync rows the
// identity gate was added to suppress straight back into history on every scanning receiver.
static int
test_channel_label_does_not_rescue_identityless_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    int rc = expect_int("scan list name stores", arm_scanner_label(&state, &opts, 0, "Fire Dispatch"), 0);
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.dmr_color_code = 13;

    rc |= expect_int("provisional call starts epoch",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                                       DSD_CALL_BOUNDARY_BEGIN),
                     1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_has_substr("labelled zero-identity row is staged",
                            state.event_history_s[0].Event_History_Items[0].event_string, "[Fire Dispatch] TEST");
    rc |= expect_int("sync loss ends the epoch", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("a label does not rescue an identity-less row", committed_history_rows(&event_history[0]), 0);

    dsd_state_trunk_lcn_name_free(&state);
    dsd_state_ext_free_all(&state);
    return rc;
}

// An unnamed scan-list row resolves to no label, and that is an answer, not a missing one: the
// epoch must not go on asking. A -Y list with only some rows named is the ordinary case, so
// without the resolved verdict the finalize pass relabels a call heard on an unnamed channel with
// whichever channel the scanner has since hopped to.
static int
test_unnamed_channel_epoch_is_not_relabelled_by_a_hop(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    // Parked on row 0, which has no name; row 1 does.
    opts.scanner_mode = 1;
    state.lcn_freq_count = 3;
    state.lcn_freq_roll = 1;
    int rc = expect_int("named scan list row stores", dsd_state_trunk_lcn_name_set(&state, 1U, "PD Tac"), 0);

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    state.lcn_freq_roll = 2;

    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* committed = &event_history[0].Event_History_Items[1];
    rc |= expect_int("unnamed channel commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_int("unnamed channel leaves the label empty", committed->channel_label[0], '\0');
    rc |= expect_no_substr("unnamed channel row renders no prefix at all", committed->event_string, "[");

    dsd_state_trunk_lcn_name_free(&state);
    dsd_state_ext_free_all(&state);
    return rc;
}

// The merge's other direction. A row whose epoch already answered "no label" keeps that answer
// when a reacquired segment merges in, even though the segment resolved a name of its own -- by
// then the scanner has rolled on, and the segment's channel is not where the transmission was
// heard. The complementary fill -- a blank retained row taking the segment's label -- is
// unreachable from here: every rendered row carries a resolved verdict, so that arm only ever
// serves a row a protocol staged directly without rendering it.
static int
test_unlabelled_row_is_not_relabelled_by_a_reacquired_segment(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    // Nothing named on air for the first segment.
    opts.scanner_mode = 1;
    state.lcn_freq_count = 3;
    state.lcn_freq_roll = 1;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    int rc = expect_int("first segment is unlabelled", event_history[0].Event_History_Items[1].channel_label[0], '\0');

    // The scanner rolls onto a named channel before the transmission resumes.
    rc |= expect_int("named scan list row stores", dsd_state_trunk_lcn_name_set(&state, 1U, "PD Tac"), 0);
    state.lcn_freq_roll = 2;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* merged = &event_history[0].Event_History_Items[1];
    rc |= expect_int("reacquisition commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_has_substr("merged row carries the late source", merged->event_string, "SRC: 00000201;");
    rc |= expect_int("merged row keeps its empty label", merged->channel_label[0], '\0');
    rc |= expect_no_substr("merged row renders no prefix at all", merged->event_string, "[");

    dsd_state_trunk_lcn_name_free(&state);
    dsd_state_ext_free_all(&state);
    return rc;
}

// Data notices name their channel too, resolved live: the PDU decodes while the receiver is still
// tuned to the channel that carried it.
static int
test_data_notice_carries_channel_label(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    arm_trunk_scan_label(&state, &opts, "SiteA");

    assert(emit_test_data_notice(&opts, &state, 1234U, 5678U, "NMEA SRC: 1234; TGT: 5678;", 0U) == 0);

    const Event_History* committed = &event_history[0].Event_History_Items[1];
    int rc = expect_str_eq("data notice keeps the channel label", committed->channel_label, "SiteA");
    rc |= expect_str_eq("data notice renders the label after the timestamp", committed->event_string,
                        "2026-04-30 00:00:00 [SiteA] NMEA SRC: 1234; TGT: 5678;");

    reset_fixture(&opts, &state, event_history);
    assert(emit_test_data_notice(&opts, &state, 1234U, 5678U, "NMEA SRC: 1234; TGT: 5678;", 0U) == 0);
    const Event_History* unlabelled = &event_history[0].Event_History_Items[1];
    rc |= expect_int("unlabelled data notice carries no label", unlabelled->channel_label[0], '\0');
    rc |= expect_str_eq("unlabelled data notice string is unchanged", unlabelled->event_string,
                        "2026-04-30 00:00:00 NMEA SRC: 1234; TGT: 5678;");

    dsd_state_ext_free_all(&state);
    return rc;
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
    g_open_wav_result = &wav_sentinel;

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

// The merge keeps the committed row's event_start_time, and it must symmetrically carry the newest
// segment's event_time: the pair is a frontend's duration, and an end stamp frozen at the first
// fragment's last render truncates a reacquired transmission to its opening seconds.
static int
test_merged_row_end_stamp_advances(void) {
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

    // Age the committed fragment's stamps as a real gapped transmission would read
    // by the time the reacquired segment commits: its render happened long before
    // the merge, while the segment below stamps the render clock's "now".
    Event_History* committed = &event_history[0].Event_History_Items[1];
    assert(committed->event_string[0] != '\0');
    const time_t aged_end = committed->event_time - 100;
    const time_t aged_start = committed->event_start_time - 100;
    committed->event_time = aged_end;
    committed->event_start_time = aged_start;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    advance_test_clock(5.0);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_int("reacquired transmission stays one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_int("merged row keeps the committed start", committed->event_start_time == aged_start ? 1 : 0, 1);
    rc |= expect_int("merged row's end advances to the newest segment's stamp",
                     committed->event_time > aged_end ? 1 : 0, 1);
    // Committed start to newest end: the whole transmission, never just fragment one.
    rc |= expect_int("merged span covers the reacquisition gap",
                     (committed->event_time - committed->event_start_time) >= 100 ? 1 : 0, 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

// A reacquired segment derives its own start stamp independently, and that derivation can land a
// second to either side of the committed row's stamp. The merge must never move a nonzero
// committed start — frontends key their history mirrors on it, and a stamp that shifts under a
// committed row reads back as a brand-new call (duplicate rows, leaked dedup state).
static int
test_merged_row_start_stamp_is_frozen(void) {
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

    // Shift the committed stamp two seconds later than the reacquired segment's own derivation
    // will land, so an earliest-wins merge would visibly rewrite it backwards.
    Event_History* committed = &event_history[0].Event_History_Items[1];
    assert(committed->event_start_time > 0);
    const time_t shifted_start = committed->event_start_time + 2;
    committed->event_start_time = shifted_start;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    advance_test_clock(5.0);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_int("reacquired transmission stays one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_int("merge does not move a nonzero committed start",
                     committed->event_start_time == shifted_start ? 1 : 0, 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

// A decode instant far from any platform clock reading (1e9 s), so a due time or a row stamp taken
// from the platform clocks instead of the decode clock could not come out at the expected value.
#define ALERT_TEST_DECODE_T0_NS 1000000000000000000ULL
#define ALERT_TEST_DECODE_T0_S  1000000000.0

static void
alert_decode_clock_at_ms(uint64_t offset_ms) {
    dsd_decode_clock_test_set_ns(ALERT_TEST_DECODE_T0_NS + (offset_ms * 1000000ULL));
}

// Observe on the store's own clock (observed_m 0), as the vocoder's media mark and the engine's
// no-carrier end do. A zero target is the vocoder's identity-less BEGIN.
static int
observe_unstamped_call(dsd_state* state, uint64_t target_id, uint64_t source_id) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = target_id != 0U ? DSD_CALL_KIND_GROUP_VOICE : DSD_CALL_KIND_VOICE,
        .ota_target_id = target_id,
        .policy_target_id = target_id,
        .ota_source_id = source_id,
    };
    return dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN);
}

// The VOICE_END alert a sync-loss end holds open is due one reacquisition window after the end on
// the decode clock, and is released by decode time alone: the real clock does not move here.
static int
test_end_alert_due_runs_on_the_decode_clock(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_VOICE_END;
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    dsd_decode_clock_use_test(ALERT_TEST_DECODE_T0_NS);

    int rc = expect_int("identified call begins", observe_unstamped_call(&state, 1234U, 5678U), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    alert_decode_clock_at_ms(2000U);
    rc |= expect_int("sync loss ends it", dsd_call_state_end_ex(&state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    alert_decode_clock_at_ms(2400U);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("END alert still held 0.4 s after the end", g_beeper_count, 1);
    dsd_call_context_snapshot context;
    rc |= expect_int("context snapshot copies", dsd_call_context_copy_snapshot(&state, &context) > 0, 1);
    rc |= expect_int("sync-loss end holds the alert", context.events[0].end_alert_pending, 1);
    rc |= expect_int(
        "alert is due one window after the decode-time end",
        same_instant(context.events[0].end_alert_due_m, ALERT_TEST_DECODE_T0_S + 2.0 + DSD_CALL_REACQUIRE_GAP_S), 1);
    alert_decode_clock_at_ms(2600U);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("END alert fires 0.6 s after the end", g_beeper_count, 2);

    dsd_decode_clock_use_system();
    dsd_state_ext_free_all(&state);
    return rc;
}

// An identity-less audible row faded by a sync loss is held, not dropped, for one reacquisition
// window on the decode clock, and dropped once decode time alone has passed its due time.
static int
test_drop_hold_due_runs_on_the_decode_clock(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    dsd_decode_clock_use_test(ALERT_TEST_DECODE_T0_NS);

    int rc = expect_int("audible identity-less epoch starts", observe_unstamped_call(&state, 0U, 0U), 1);
    rc |= expect_int("voice media runs", dsd_call_state_update_media(&state, 0U, 1, 0.0), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    alert_decode_clock_at_ms(1000U);
    rc |= expect_int("carrier fades", dsd_call_state_end_ex(&state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    alert_decode_clock_at_ms(1400U);
    dsd_event_sync_slot(&opts, &state, 0U);
    dsd_call_context_snapshot context;
    rc |= expect_int("context snapshot copies", dsd_call_context_copy_snapshot(&state, &context) > 0, 1);
    rc |= expect_int("verdict still held 0.4 s after the fade", context.events[0].drop_hold_pending, 1);
    rc |= expect_int("held row is not finalized", context.events[0].ended_committed, 0);
    rc |= expect_int(
        "hold is due one window after the decode-time fade",
        same_instant(context.events[0].drop_hold_due_m, ALERT_TEST_DECODE_T0_S + 1.0 + DSD_CALL_REACQUIRE_GAP_S), 1);
    alert_decode_clock_at_ms(1600U);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("second context snapshot copies", dsd_call_context_copy_snapshot(&state, &context) > 0, 1);
    rc |= expect_int("hold released 0.6 s after the fade", context.events[0].drop_hold_pending, 0);
    rc |= expect_int("released verdict finalizes the epoch", context.events[0].ended_committed, 1);
    rc |= expect_int("the unexplained row is dropped", committed_history_rows(&event_history[0]), 0);

    dsd_decode_clock_use_system();
    dsd_state_ext_free_all(&state);
    return rc;
}

// A transmission faded and reacquired twice commits one row whose span is the decode-time span of
// the whole transmission: the start of the first segment to the end of the last, 9.25 s here,
// which the whole-second stamps carry as 9. The real clock does not move here.
static int
test_merged_history_duration_runs_on_the_decode_clock(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    dsd_decode_clock_use_test(ALERT_TEST_DECODE_T0_NS);

    int rc = expect_int("first segment begins", observe_unstamped_call(&state, 100U, 200U), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    alert_decode_clock_at_ms(2000U);
    rc |= expect_int("first segment fades", dsd_call_state_end_ex(&state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    alert_decode_clock_at_ms(2250U);
    rc |= expect_int("second segment reacquires", observe_unstamped_call(&state, 0U, 0U), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    alert_decode_clock_at_ms(5000U);
    rc |= expect_int("second segment fades", dsd_call_state_end_ex(&state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    alert_decode_clock_at_ms(5250U);
    rc |= expect_int("third segment reacquires", observe_unstamped_call(&state, 0U, 0U), 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    alert_decode_clock_at_ms(9250U);
    rc |= expect_int("third segment fades", dsd_call_state_end_ex(&state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* row = &event_history[0].Event_History_Items[1];
    rc |= expect_int("the transmission commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_int("the row names the first segment's call", (int)row->target_id, 100);
    rc |=
        expect_u64("merged duration is the decode-time span", (uint64_t)(row->event_time - row->event_start_time), 9U);
    rc |= expect_u64("row starts at the first segment's decode start", (uint64_t)row->event_start_time,
                     (uint64_t)ALERT_TEST_DECODE_T0_S);

    dsd_decode_clock_use_system();
    dsd_state_ext_free_all(&state);
    return rc;
}

// commit_rev is the committed-rows-only change counter frontends gate their ring rescans on:
// staged-row renders must leave it alone, while commits, merges and late enrichment advance it.
static int
test_commit_rev_tracks_committed_rows_only(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    const uint64_t base = event_history[0].commit_rev;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    int rc = expect_u64("staged render leaves commit_rev unchanged", event_history[0].commit_rev, base);

    advance_test_clock(1.0);
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 0);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_u64("repeated staged renders leave commit_rev unchanged", event_history[0].commit_rev, base);

    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    const uint64_t committed_rev = event_history[0].commit_rev;
    rc |= expect_int("commit advances commit_rev", committed_rev > base ? 1 : 0, 1);

    // Late enrichment of the committed row is a committed-row mutation too.
    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0U, &call) == 1);
    rc |= expect_int("late alias enriches the committed row",
                     dsd_event_enrich_alias(&state, 0U, call.epoch, "LATE ALIAS"), 1);
    const uint64_t enriched_rev = event_history[0].commit_rev;
    rc |= expect_int("enrichment advances commit_rev", enriched_rev > committed_rev ? 1 : 0, 1);

    // A reacquisition merge mutates the committed row in place and must advance it as well.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    advance_test_clock(5.0);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("reacquisition merge advances commit_rev", event_history[0].commit_rev > enriched_rev ? 1 : 0, 1);
    rc |= expect_int("merge stays one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_u64("other slot's commit_rev is untouched", event_history[1].commit_rev, base);
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
    rc |= expect_int("repeated ends do not restamp ended_m",
                     same_instant(after_repeats.ended_m, after_explicit.ended_m), 1);

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
    rc |= expect_int("upgrade keeps ended_m anchored at the fade",
                     same_instant(after_upgrade.ended_m, after_fade.ended_m), 1);

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

    // The identity-less epoch's own row is dropped -- it never named a call -- so only the
    // unrelated call's row reaches history, un-coalesced and with its own START.
    int rc = expect_int("only the unrelated call's row reaches history", committed_history_rows(&event_history[0]), 1);
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
// dsd_decode_now_mono_s(), while a reopening observation usually passes 0.0 and takes the
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

    const double before = dsd_decode_now_mono_s();
    // observed_m of 0.0 is what the protocol end paths pass, so this takes the fallback.
    assert(dsd_call_state_end_ex(&state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS) == 1);
    const double after = dsd_decode_now_mono_s();

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

// A terminator followed by an identity-less reopen inside the window: the transmission that
// terminated is over, and whatever decodes next is a new epoch, never a merge into the ended
// row. The reopen itself never names a call, so it leaves no row of its own -- the terminated
// call's row must survive untouched as the only one.
static int
test_explicit_end_then_identityless_reopen_leaves_single_row(void) {
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

    int rc = expect_int("only the terminated call's row is in history", committed_history_rows(&event_history[0]), 1);
    rc |= expect_int("terminated call's row is not merged into or replaced",
                     (int)event_history[0].Event_History_Items[1].target_id, 100);
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

// A merge re-renders the surviving row, and that render has to reuse the timestamp the row is
// already displaying rather than the stamp beside it -- the two are not always the same clock.
// Every builder renders the prefix from the wall clock, but watchdog_event_current_update_item()
// only stamps event_time when opts->playfiles == 0; replaying an sdrtrunk recording leaves it at
// the recording's own time instead (dsd_file.c). Preferring the stamp would rewrite the row's
// visible date and time to a value none of its siblings use, purely because it was merged.
static int
test_merge_preserves_row_timestamp_under_playfiles(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.playfiles = 1;
    // What the sdrtrunk JSON reader stamps on the staging row, decades from the wall clock the
    // prefix is rendered from.
    event_history[0].Event_History_Items[0].event_time = TEST_REPLAY_EVENT_TIME;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* committed = &event_history[0].Event_History_Items[1];
    assert(committed->event_time == TEST_REPLAY_EVENT_TIME);
    char prefix_before[20];
    DSD_SNPRINTF(prefix_before, sizeof prefix_before, "%s", committed->event_string);

    // A late source guarantees the merge really does re-render rather than declining to.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* merged = &event_history[0].Event_History_Items[1];
    char prefix_after[20];
    DSD_SNPRINTF(prefix_after, sizeof prefix_after, "%s", merged->event_string);

    int rc = expect_int("playfiles merge commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_has_substr("playfiles merge re-rendered the row", merged->event_string, "SRC: 00000201;");
    rc |= expect_str_eq("merge keeps the row's rendered timestamp", prefix_after, prefix_before);
    // Pins which of the two clocks survived: the wall clock the row was rendered from, not the
    // replay stamp sitting beside it.
    rc |= expect_str_eq("merged row still shows the rendered clock", prefix_after, "2026-04-30 00:00:00");
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

    state.dmr_fid = 0x10U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    dsd_call_context_snapshot before;
    assert(dsd_call_context_copy_snapshot(&state, &before) > 0);
    assert(before.events[0].staged_env.mfid == 0x10U);

    dsd_event_history_reset(&state);

    dsd_call_context_snapshot after;
    assert(dsd_call_context_copy_snapshot(&state, &after) > 0);
    int rc = expect_int("reset clears the staged render environment", (int)after.events[0].staged_env.mfid, 0);
    rc |= expect_int("reset clears the committed render environment", (int)after.events[0].committed_env.mfid, 0);
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
// A generic digital sync is the vehicle: no per-protocol builder claims DSD_SYNC_DIGITAL, so its
// rows render no string and have no content to commit. That makes it the reachable shape of "an
// epoch that committed nothing", which is otherwise hard to construct. Every protocol the decoder
// can actually classify has a builder, so an unclassified signal is the only one left that
// legitimately renders nothing.
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
    assert(observe_test_call(&state, 0U, DSD_SYNC_DIGITAL, DSD_CALL_KIND_GROUP_VOICE, 300U, 400U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(committed_history_rows(&event_history[0]) == 1);

    // B is reacquired inside the window. Whatever it contributes must not land in call A's row.
    assert(observe_test_call(&state, 0U, DSD_SYNC_DIGITAL, DSD_CALL_KIND_GROUP_VOICE, 300U, 400U, 0U, 0U,
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

    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dsd-neo-reacquire-events");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed for reacquisition event log test\n");
        return 1;
    }
    close(fd);
    (void)remove(path);
    DSD_SNPRINTF(opts.event_out_file, sizeof opts.event_out_file, "%s", path);

    // Segment 1 knows the talkgroup only; segment 2 decodes the source, so the render changes.
    int rc = expect_int("segment 1 begins",
                        observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U,
                                          0U, 0U, DSD_CALL_BOUNDARY_BEGIN),
                        1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("segment 1 ends", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    rc |= expect_int("segment 2 continues",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U,
                                       0U, DSD_CALL_BOUNDARY_CONTINUE),
                     1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("segment 2 ends", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    // Segment 3 adds nothing the row does not already say.
    rc |= expect_int("segment 3 continues",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U,
                                       0U, DSD_CALL_BOUNDARY_CONTINUE),
                     1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("segment 3 ends", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 1);
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

    rc |= expect_int("merged transmission commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_has_substr("first segment logged its own line", buf, "SRC: 00000000;");
    rc |= expect_has_substr("continuation reports the learned source", buf, " Reacquired: ");
    rc |= expect_has_substr("continuation marker follows the row's stamp", buf, "\n2026-04-30 00:00:00 Reacquired: ");
    rc |= expect_every_line_stamped("continuation log lines all start with the stamp", buf, "2026-04-30 00:00:00 ");
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

    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dsd-neo-continuation-slot");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed for continuation slot test\n");
        return 1;
    }
    close(fd);
    (void)remove(path);
    DSD_SNPRINTF(opts.event_out_file, sizeof opts.event_out_file, "%s", path);

    // Slot 1 of a DMR-BS call: the first commit is annotated "Slot 2;".
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    int rc = expect_int("slot 2 segment begins",
                        observe_test_call(&state, 1U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U,
                                          0U, 0U, DSD_CALL_BOUNDARY_BEGIN),
                        1);
    dsd_event_sync_slot(&opts, &state, 1U);
    rc |= expect_int("slot 2 segment ends", end_test_call(&state, 1U, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 1U);

    // The decoder loses the system entirely across the gap, as noCarrier() leaves it.
    state.lastsynctype = DSD_SYNC_NONE;
    rc |= expect_int("reacquired slot 2 segment continues",
                     observe_test_call(&state, 1U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0U,
                                       0U, DSD_CALL_BOUNDARY_CONTINUE),
                     1);
    dsd_event_sync_slot(&opts, &state, 1U);
    rc |= expect_int("reacquired slot 2 segment ends", end_test_call(&state, 1U, DSD_CALL_END_SYNC_LOSS), 1);
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
    rc |= expect_int("the reacquired segment merged into one row", committed_history_rows(&event_history[1]), 1);
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

    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dsd-neo-reacquire-meta");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed for reacquisition metadata log test\n");
        return 1;
    }
    close(fd);
    (void)remove(path);
    DSD_SNPRINTF(opts.event_out_file, sizeof opts.event_out_file, "%s", path);

    int rc = expect_int("first segment begins",
                        observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U,
                                          0U, 0U, DSD_CALL_BOUNDARY_BEGIN),
                        1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("first segment ends", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    // The reacquired segment adds an alias and a GPS fix but no new identity, so the rendered
    // row is unchanged and only the metadata lines have anything to report.
    rc |= expect_int("reacquired segment continues",
                     observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U,
                                       0U, DSD_CALL_BOUNDARY_CONTINUE),
                     1);
    dsd_call_snapshot reacquired;
    DSD_MEMSET(&reacquired, 0, sizeof reacquired);
    rc |= expect_int("reacquired segment has a call snapshot", dsd_call_state_get(&state, 0U, &reacquired) > 0, 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("reacquired segment takes the alias",
                     dsd_event_enrich_alias(&state, 0U, reacquired.epoch, "UNIT 12 FIRE"), 1);
    rc |= expect_int("reacquired segment takes the gps fix",
                     dsd_event_enrich_gps(&state, 0U, reacquired.epoch, "lat 3.0 lon 4.0"), 1);
    rc |= expect_int("reacquired segment ends", end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS), 1);
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

    rc |= expect_int("metadata-only merge commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_has_substr("merged alias reaches the log", buf, "\n2026-04-30 00:00:00 Talker Alias: UNIT 12 FIRE \n");
    rc |= expect_has_substr("merged gps reaches the log", buf, "\n2026-04-30 00:00:00 GPS: lat 3.0 lon 4.0 \n");
    rc |= expect_every_line_stamped("detail-only continuation carries the row's stamp", buf, "2026-04-30 00:00:00 ");
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
// its beep lives inside the commit -- and the deferred one must not either. An unclassified
// digital sync is the concrete case: no builder claims it, so its rows render empty.
static int
test_contentless_sync_loss_end_arms_no_alert(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_VOICE_END;
    state.lastsynctype = DSD_SYNC_DIGITAL;

    assert(observe_test_call(&state, 0U, DSD_SYNC_DIGITAL, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
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

// X2-TDMA voice reached history as an empty row: no builder covered it, so the transmission was
// absent from the UI and from the log with nothing to say it had happened. It decodes no call
// identity -- the link control it collects is never parsed into a talkgroup or a source -- so the
// row reports what the protocol does know: that the slot carried voice, on which timeslot, and
// whether it was encrypted.
static int
test_x2tdma_voice_commits_a_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_X2TDMA_VOICE_POS;

    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dsd-neo-x2tdma-events");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed for x2tdma event log test\n");
        return 1;
    }
    close(fd);
    (void)remove(path);
    DSD_SNPRINTF(opts.event_out_file, sizeof opts.event_out_file, "%s", path);

    // Slot 1 and identity-less, exactly as x2tdma_voice.c publishes it.
    int rc = expect_int("x2tdma voice is observed",
                        observe_test_call(&state, 1U, DSD_SYNC_X2TDMA_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                                          DSD_CALL_BOUNDARY_CONTINUE),
                        1);
    dsd_event_sync_slot(&opts, &state, 1U);
    rc |= expect_int("x2tdma voice ends", end_test_call(&state, 1U, DSD_CALL_END_EXPLICIT), 1);
    dsd_event_sync_slot(&opts, &state, 1U);

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        (void)remove(path);
        DSD_FPRINTF(stderr, "x2tdma event log was not created\n");
        return 1;
    }
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    (void)remove(path);
    buf[n] = '\0';

    const Event_History* committed = &event_history[1].Event_History_Items[1];
    rc |= expect_int("x2tdma voice commits a row", committed_history_rows(&event_history[1]), 1);
    // The protocol name itself comes from dsd_synctype_to_string(), stubbed here for every
    // protocol; what matters is that the builder rendered at all rather than leaving the row blank.
    rc |= expect_int("x2tdma row is not blank", committed->event_string[0] != '\0', 1);
    // Zeros rather than omitted: the decoder genuinely never read them, and the row shape stays
    // the same as every other protocol's.
    rc |= expect_has_substr("x2tdma row reports the unknown identity", committed->event_string, "TGT: 00000000;");
    // A NAC would be fabricated -- X2-TDMA has none -- so the P25 builder is deliberately not reused.
    rc |= expect_int("x2tdma row invents no NAC", strstr(committed->event_string, "NAC:") == NULL, 1);
    // Two-slot, so the log line has to say which timeslot carried the call.
    rc |= expect_has_substr("x2tdma log line carries the slot annotation", buf, "Slot 2;");
    dsd_state_ext_free_all(&state);
    return rc;
}

// Encryption is the one call attribute X2-TDMA does publish, so it has to reach the row.
static int
test_x2tdma_encrypted_voice_reports_enc(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    state.lastsynctype = DSD_SYNC_X2TDMA_VOICE_POS;

    assert(observe_test_call(&state, 0U, DSD_SYNC_X2TDMA_VOICE_POS, DSD_CALL_KIND_VOICE, 0U, 0U, 0U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    assert(update_test_crypto(&state, 0U, DSD_CALL_CRYPTO_ENCRYPTED, 0x84U, 0x1234U, 0U) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* committed = &event_history[0].Event_History_Items[1];
    int rc = expect_int("encrypted x2tdma commits a row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_has_substr("x2tdma row reports the algorithm", committed->event_string, "ENC; ALG: 84; KID: 1234;");
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

// The per-protocol builders read decoder state the history row does not carry -- here the DMR
// manufacturer feature id that decides whether service options render as TXI and PRIORITY. A merge
// must re-render against the values captured when the row was committed, not against a decoder
// that has since retuned -- otherwise a committed row is rewritten with a context the call never
// ran under.
static int
test_merge_rerenders_against_committed_environment(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    state.dmr_fid = 0x10U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0x30U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(expect_has_substr("first segment renders under its feature id",
                             event_history[0].Event_History_Items[1].event_string, "TXI;")
           == 0);

    // The receiver moves to a system with another manufacturer before the segment is reacquired.
    state.dmr_fid = 0x00U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0x30U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* merged = &event_history[0].Event_History_Items[1];
    int rc = expect_int("retuned merge commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_has_substr("merged row keeps the context it was committed under", merged->event_string, "TXI;");
    rc |= expect_int("merged row still gained the source", (int)merged->source_id, 201);
    dsd_state_ext_free_all(&state);
    return rc;
}

// The test above commits the first segment's row from its own end, while the decoder still
// describes it. A row is also committed from the other direction -- the next epoch opening finds
// a staged row and pushes it -- and by then the canonical layer has already moved to the incoming
// call. Capturing the environment at that moment reads the new call's context, so the row is
// re-rendered under a context the transmission it describes never ran under.
static int
test_epoch_change_commit_keeps_staged_environment(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);

    state.dmr_fid = 0x10U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 0U, 0x30U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    // Sync loss with no further pass: the staged row is left for the next epoch to commit, which
    // is the path watchdog_event_history_authoritative() takes.
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);

    // The decoder moves on before the segment is reacquired, so the live feature id now
    // describes the incoming call rather than the staged row.
    state.dmr_fid = 0x00U;
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 201U, 0x30U, 0U,
                             DSD_CALL_BOUNDARY_CONTINUE)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    int rc = expect_has_substr("row committed at the epoch change keeps its own context",
                               event_history[0].Event_History_Items[1].event_string, "TXI;");

    // Ending the reacquired segment merges it into that row and re-renders it. The environment
    // the merge renders against is the one captured with the row, not the moved decoder.
    assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);

    const Event_History* merged = &event_history[0].Event_History_Items[1];
    rc |= expect_int("epoch-change merge commits one row", committed_history_rows(&event_history[0]), 1);
    rc |= expect_has_substr("merged row keeps the context it was staged under", merged->event_string, "TXI;");
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
    dsd_event_stage_text(&state, 0, "packet text");
    dsd_event_stage_gps(&state, 0, "packet GPS");
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
    rc |= expect_int("active voice row preserves PDU sentinel", current->pdu[0], 0xAB);
    rc |= expect_int("explicit notice consumes staged data text", dsd_event_staged_text(&state, 0)[0], '\0');
    rc |= expect_int("explicit notice consumes staged data GPS", dsd_event_staged_gps(&state, 0)[0], '\0');
    dsd_state_ext_free_all(&state);
    rc |= expect_int("explicit notice does not inherit active PDU", committed->pdu[0], 0);
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

/* Writes @p text to the temp file @p path; returns 0, or -1 when a write failed. */
static int
write_alias_file(const char* path, const char* text) {
    FILE* fp = dsd_fopen_private(path, "w");
    if (!fp) {
        return -1;
    }
    int ok = fputs(text, fp) >= 0;
    return fclose(fp) == 0 && ok ? 0 : -1;
}

static int
test_source_alias_collision(int protocol) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    Event_History_I* history = calloc(2, sizeof(*history));
    assert(opts && state && history);
    reset_fixture(opts, state, history);
    char group_path[DSD_TEST_PATH_MAX], source_path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(group_path, sizeof(group_path), "alias-collision-group");
    assert(fd >= 0);
    int failed = expect_int("group alias file closes", dsd_close(fd), 0);
    fd = dsd_test_mkstemp(source_path, sizeof(source_path), "alias-collision-source");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed for alias collision source file\n");
        failed = 1;
    } else {
        failed |= expect_int("source alias file closes", dsd_close(fd), 0);
        failed |= expect_int("group alias file written",
                             write_alias_file(group_path, "id,mode,name\n1201,A,Talkgroup 1201\n"), 0);
        failed |= expect_int("source alias file written",
                             write_alias_file(source_path, "id,name\n1201,Radio 1201\n1202,Unit X\n"), 0);
        failed |= expect_int("group aliases import", csvGroupImportPath(group_path, state), 0);
        failed |= expect_int("source aliases import", csvSrcImportPath(source_path, state), 0);
        failed |= expect_int("source alias file removed", remove(source_path), 0);
    }
    failed |= expect_int("group alias file removed", remove(group_path), 0);
    if (failed) {
        dsd_state_ext_free_all(state);
        free(history);
        free(state);
        free(opts);
        return 1;
    }
    state->lastsynctype = protocol;
    assert(observe_test_call(state, 0U, protocol, DSD_CALL_KIND_GROUP_VOICE, 1201, 1201, 0, 0, DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    watchdog_event_current(opts, state, 0);
    const Event_History* item = &history[0].Event_History_Items[0];
    assert(strcmp(item->t_name, "Talkgroup 1201") == 0 && strcmp(item->s_name, "Radio 1201") == 0);
    assert(strstr(item->event_string, "TName: Talkgroup 1201; Mode: A;"));
    assert(strstr(item->event_string, "SName: Radio 1201; Mode: A;"));
    assert(observe_test_call(state, 0U, protocol, DSD_CALL_KIND_GROUP_VOICE, 9999, 1202, 0, 0, DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    watchdog_event_current(opts, state, 0);
    assert(strstr(item->event_string, "SName: Unit X; "));
    assert(!strstr(item->event_string, "Mode:"));
    assert(dsd_source_alias_clear(state) == 0);
    assert(observe_test_call(state, 0U, protocol, DSD_CALL_KIND_GROUP_VOICE, 9999, 1201, 0, 0, DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    watchdog_event_current(opts, state, 0);
    assert(strstr(item->event_string, "SName: Talkgroup 1201; Mode: A;"));
    dsd_state_ext_free_all(state);
    free(history);
    free(state);
    free(opts);
    return 0;
}

static int
test_crc_invalid_data_notice_isolation(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "crc-events");
    assert(fd >= 0);
    int rc = expect_int("crc event log closes", dsd_close(fd), 0);
    DSD_SNPRINTF(opts.event_out_file, sizeof opts.event_out_file, "%s", path);
    dsd_event_stage_text(&state, 0, "decoded text");
    dsd_event_stage_gps(&state, 0, "decoded position");
    char notice[4096];
    DSD_MEMSET(notice, 'X', sizeof notice - 1U);
    notice[sizeof notice - 1U] = '\0';
    dsd_call_observation data = dsd_call_observation_data(DSD_SYNC_DMR_BS_DATA_POS, 0U, 100U, 200U);
    state.event_crc_invalid[0] = 1U;
    rc |= expect_int("failed data notice emits", dsd_event_emit_data_notice(&opts, &state, 0U, &data, notice), 0);
    rc |= expect_has_substr("long failed notice retains prefix", event_history[0].Event_History_Items[1].event_string,
                            " [CRC ERR] ");
    rc |= expect_int("failed data warns", event_history[0].Event_History_Items[1].severity, DSD_EVENT_SEVERITY_WARNING);
    rc |=
        expect_int("failed GPS notice emits",
                   dsd_event_emit_data_notice_with_gps(&opts, &state, 0U, &data, "GPS packet", "explicit position"), 0);
    rc |= expect_has_substr("GPS variant marks history", event_history[0].Event_History_Items[1].event_string,
                            "[CRC ERR]");
    char buf[8192] = {0};
    rc |= expect_int("crc event log reads", read_event_log(path, buf, sizeof buf), 0);
    rc |= expect_has_substr("failed text detail marked", buf, "[CRC ERR] Text: decoded text");
    rc |= expect_has_substr("failed GPS detail marked", buf, "[CRC ERR] GPS: decoded position");
    rc |= expect_has_substr("explicit GPS detail marked", buf, "[CRC ERR] GPS: explicit position");
    for (const char* line = buf; *line != '\0';) {
        const char* end = strchr(line, '\n');
        const char* marker = strstr(line, "[CRC ERR]");
        rc |= expect_int("every failed log line marked", marker != NULL && (end == NULL || marker < end), 1);
        if (end == NULL) {
            break;
        }
        line = end + 1;
    }
    data.slot = 1U;
    rc |= expect_int("other slot notice emits", dsd_event_emit_data_notice(&opts, &state, 1U, &data, "other slot"), 0);
    rc |= expect_int("other slot not tainted",
                     strstr(event_history[1].Event_History_Items[1].event_string, "[CRC ERR]") != NULL, 0);
    state.event_crc_invalid[0] = 0U;
    data.slot = 0U;
    rc |= expect_int("clean notice emits", dsd_event_emit_data_notice(&opts, &state, 0U, &data, "clean next"), 0);
    rc |= expect_int("next notice not tainted",
                     strstr(event_history[0].Event_History_Items[1].event_string, "[CRC ERR]") != NULL, 0);
    rc |= expect_int("next notice informational", event_history[0].Event_History_Items[1].severity,
                     DSD_EVENT_SEVERITY_INFO);
    remove(path);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_crc_invalid_voice_delayed_and_reacquired(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    reset_fixture(&opts, &state, event_history);
    int rc = 0;
    // A clean first fragment is upgraded by a failed reacquired header, then remains
    // marked through another clean reacquisition and enrichment. Emergency and
    // priority metadata learned on that fragment must survive the same merges.
    for (int pass = 0; pass < 3; pass++) {
        state.event_crc_invalid[0] = pass == 1;
        const dsd_call_observation observation = {
            .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
            .slot = 0U,
            .kind = DSD_CALL_KIND_GROUP_VOICE,
            .ota_target_id = 100U,
            .policy_target_id = 100U,
            .ota_source_id = 200U,
            .has_service_metadata = 1U,
            .emergency = pass == 1,
            .priority = pass == 1 ? 3U : 0U,
            .observed_m = g_observed_m,
        };
        g_observed_m += 0.1;
        assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
        state.event_crc_invalid[0] = 0U;
        // Publication deliberately happens after dispatch restored its CRC scope.
        dsd_event_sync_slot(&opts, &state, 0U);
        dsd_call_snapshot call;
        assert(dsd_call_state_get(&state, 0U, &call) == 1);
        assert(dsd_event_enrich_gps(&state, 0U, call.epoch, "voice position") == 1);
        assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        if (pass != 0) {
            rc |= expect_has_substr("delayed merged voice retains CRC marker",
                                    event_history[0].Event_History_Items[1].event_string, "[CRC ERR]");
            rc |= expect_int("delayed merged voice warns", event_history[0].Event_History_Items[1].severity,
                             DSD_EVENT_SEVERITY_WARNING);
            rc |= expect_int("delayed merged voice retains emergency",
                             event_history[0].Event_History_Items[1].emergency, 1);
            rc |= expect_int("delayed merged voice retains highest priority",
                             event_history[0].Event_History_Items[1].priority, 3);
        }
    }
    rc |= expect_int("failed reacquisition does not duplicate voice", committed_history_rows(&event_history[0]), 1);
    assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 500U, 900U, 0U, 0U,
                             DSD_CALL_BOUNDARY_BEGIN)
           == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_int("fresh voice epoch not tainted",
                     strstr(event_history[0].Event_History_Items[0].event_string, "[CRC ERR]") != NULL, 0);
    rc |= expect_int("fresh voice epoch informational", event_history[0].Event_History_Items[0].severity,
                     DSD_EVENT_SEVERITY_INFO);
    rc |= expect_int("fresh voice epoch has no emergency", event_history[0].Event_History_Items[0].emergency, 0);
    rc |= expect_int("fresh voice epoch has no priority", event_history[0].Event_History_Items[0].priority, 0);
    // PI headers can update a verified call's crypto without another identity observation.
    // Even an otherwise identical carrier repeat must not hide a newly failed CRC.
    const dsd_call_crypto_update crypto = {
        .classification = DSD_CALL_CRYPTO_ENCRYPTED, .algid = 0x24, .kid = 7, .mi = 0x12345678};
    assert(dsd_call_state_update_crypto(&state, 0U, &crypto) == 1);
    state.event_crc_invalid[0] = 1;
    assert(dsd_call_state_update_crypto(&state, 0U, &crypto) == 1);
    state.event_crc_invalid[0] = 0;
    assert(end_test_call(&state, 0U, DSD_CALL_END_TERMINATOR) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    rc |= expect_has_substr("metadata-only CRC failure marks delayed voice",
                            event_history[0].Event_History_Items[1].event_string, "[CRC ERR]");
    rc |= expect_int("metadata-only CRC failure warns", event_history[0].Event_History_Items[1].severity,
                     DSD_EVENT_SEVERITY_WARNING);
    // A later clean transmission must not inherit the old history depth's CRC flag on push.
    for (int pass = 0; pass < 2; pass++) {
        assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 600U, 901U, 0U, 0U,
                                 DSD_CALL_BOUNDARY_BEGIN)
               == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        rc |= expect_int("clean committed/reacquired call has no stale CRC marker",
                         strstr(event_history[0].Event_History_Items[1].event_string, "[CRC ERR]") != NULL, 0);
        rc |= expect_int("clean committed/reacquired call remains informational",
                         event_history[0].Event_History_Items[1].severity, DSD_EVENT_SEVERITY_INFO);
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_crc_invalid_enrichment_marks_voice(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    int rc = 0;
    for (int late = 0; late < 2; late++) {
        reset_fixture(&opts, &state, event_history);
        assert(observe_test_call(&state, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 100U, 200U, 0U, 0U,
                                 DSD_CALL_BOUNDARY_BEGIN)
               == 1);
        dsd_event_sync_slot(&opts, &state, 0U);
        dsd_call_snapshot call;
        assert(dsd_call_state_get(&state, 0U, &call) == 1);
        if (late) {
            assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
            dsd_event_sync_slot(&opts, &state, 0U);
        }
        state.event_crc_invalid[0] = 1;
        assert(dsd_event_enrich_gps(&state, 0U, call.epoch, "lat 1.0 lon 2.0") == 1);
        state.event_crc_invalid[0] = 0;
        if (!late) {
            assert(end_test_call(&state, 0U, DSD_CALL_END_SYNC_LOSS) == 1);
            dsd_event_sync_slot(&opts, &state, 0U);
        }
        rc |= expect_has_substr("failed GPS enrichment marks voice",
                                event_history[0].Event_History_Items[1].event_string, "[CRC ERR]");
        rc |= expect_has_substr("GPS enrichment remains available", event_history[0].Event_History_Items[1].gps_s,
                                "lat 1.0 lon 2.0");
        rc |= expect_int("failed GPS enrichment warns", event_history[0].Event_History_Items[1].severity,
                         DSD_EVENT_SEVERITY_WARNING);
        dsd_event_sync_slot(&opts, &state, 0U);
        rc |= expect_has_substr("CRC enrichment survives later rendering",
                                event_history[0].Event_History_Items[1].event_string, "[CRC ERR]");
        dsd_state_ext_free_all(&state);
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_source_alias_collision(DSD_SYNC_NXDN_POS);
    rc |= test_source_alias_collision(DSD_SYNC_P25P1_POS);
    rc |= test_source_alias_collision(DSD_SYNC_DMR_BS_VOICE_POS);
    rc |= test_crc_invalid_data_notice_isolation();
    rc |= test_crc_invalid_voice_delayed_and_reacquired();
    rc |= test_crc_invalid_enrichment_marks_voice();

    rc |= test_event_history_revision_primitives();
    rc |= test_watchdog_current_marks_only_semantic_changes();
    rc |= test_voice_row_carries_call_start_time();
    rc |= test_nonfinalizing_call_notice_defers_call_end_side_effects();
    rc |= test_noncanonical_notice_takes_no_staged_frequency_or_code();
    rc |= test_noncanonical_notice_resolves_its_channel_label_live();
    rc |= test_noncanonical_notice_stamps_its_own_start();
    rc |= test_noncanonical_notice_leaves_the_canonical_staged_row_alone();
    rc |= test_lone_render_before_the_epoch_opens_keeps_one_row_per_call();
    rc |= test_detail_before_the_call_opens_leaves_no_orphan_row();
    rc |= test_enrichment_before_the_epoch_opens_spares_the_outgoing_row();
    rc |= test_direct_detail_skips_an_ended_call();
    rc |= test_note_without_an_authoritative_call_leaves_no_orphan_row();
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
    rc |= test_event_log_unstamped_row_keeps_bare_detail_lines();
    rc |= test_source_transition_rotates_slot_wav_files();
    rc |= test_ysf_current_sanitizes_ids_and_text_message();
    rc |= test_m17_dstar_dpmr_current_strings();
    rc |= test_nxdn_current_includes_channel_encryption_and_policy_labels();
    rc |= test_edacs_ea_mode_current_event_and_unknown_lid();
    rc |= test_p25_and_dmr_current_append_security_flags();
    rc |= test_scanner_mode_row_carries_channel_label();
    rc |= test_trunk_scan_row_carries_target_id();
    rc |= test_channel_label_coexists_with_policy_label();
    rc |= test_unlabelled_row_string_is_unchanged();
    rc |= test_channel_label_survives_push_and_merge();
    rc |= test_channel_label_is_frozen_at_first_render();
    rc |= test_row_frequency_is_pinned_at_first_active_render();
    rc |= test_call_frequency_outranks_the_tuner();
    rc |= test_audio_input_row_takes_no_tuner_value();
    rc |= test_access_code_refreshes_while_live_and_freezes_at_the_end();
    rc |= test_unknown_access_code_stays_unknown_after_the_end();
    rc |= test_staged_access_code_of_another_kind_is_not_reused();
    rc |= test_invalid_live_access_code_never_erases_a_known_one();
    rc |= test_reacquisition_merge_fills_frequency_and_code_only();
    rc |= test_unknown_access_codes_render_as_dashes();
    rc |= test_dpmr_event_line_prints_the_decoded_colour_code();
    rc |= test_reacquisition_merge_upgrades_an_unknown_system_code();
    rc |= test_reacquisition_merge_names_a_dpmr_code_in_the_system_identity();
    rc |= test_reacquisition_merge_keeps_the_code_and_takes_a_later_system_id();
    rc |= test_unknown_p25_and_m17_codes_render_as_dashes();
    rc |= test_reacquisition_merge_names_a_p25_or_m17_code();
    rc |= test_data_notice_carries_frequency_and_access_code();
    rc |= test_playfiles_rows_take_no_tuner_value_or_code();
    rc |= test_nxdn_row_names_only_its_own_channel();
    rc |= test_unnamed_channel_epoch_is_not_relabelled_by_a_hop();
    rc |= test_unlabelled_row_is_not_relabelled_by_a_reacquired_segment();
    rc |= test_channel_label_does_not_rescue_identityless_row();
    rc |= test_data_notice_carries_channel_label();
    rc |= test_canonical_call_lifecycle_is_epoch_driven();
    rc |= test_canonical_voice_category_is_protocol_neutral();
    rc |= test_provisional_voice_identity_does_not_commit_zero_row();
    rc |= test_identityless_voice_epoch_commits_no_row();
    rc |= test_media_terminated_identityless_voice_epoch_commits_row();
    rc |= test_retune_explicit_end_drops_identityless_media_row();
    rc |= test_dropped_identityless_row_rotates_wav_without_export();
    rc |= test_terminator_after_fade_rescues_identityless_row();
    rc |= test_dstar_sync_loss_after_media_keeps_identityless_row();
    rc |= test_unreliable_terminator_modes_keep_audible_identityless_rows();
    rc |= test_end_alert_deadline_matches_reacquire_window();
    rc |= test_end_alert_due_runs_on_the_decode_clock();
    rc |= test_drop_hold_due_runs_on_the_decode_clock();
    rc |= test_unverified_terminator_heal_window_is_tight();
    rc |= test_crypto_only_voice_epoch_commits_row();
    rc |= test_route_text_only_row_survives_epoch_change_commit();
    rc |= test_standalone_provoice_zero_id_row_commits();
    rc |= test_new_canonical_epoch_commits_prior_canonical_call();
    rc |= test_reacquired_transmission_commits_one_row();
    rc |= test_merged_row_end_stamp_advances();
    rc |= test_merged_row_start_stamp_is_frozen();
    rc |= test_merged_history_duration_runs_on_the_decode_clock();
    rc |= test_commit_rev_tracks_committed_rows_only();
    rc |= test_terminator_after_sync_loss_end_blocks_reacquisition();
    rc |= test_end_reason_upgrade_is_one_directional();
    rc |= test_pending_end_alert_is_flushed_at_shutdown();
    rc |= test_history_reset_drops_pending_end_alert();
    rc |= test_identityless_ended_epoch_does_not_reacquire();
    rc |= test_route_only_identity_does_not_reacquire_unrelated_call();
    rc |= test_route_identity_reacquisition_still_coalesces();
    rc |= test_observed_fallback_matches_end_site_clock_resolution();
    rc |= test_contentless_sync_loss_end_arms_no_alert();
    rc |= test_x2tdma_voice_commits_a_row();
    rc |= test_x2tdma_encrypted_voice_reports_enc();
    rc |= test_reacquired_stage_superseded_by_new_call_still_merges();
    rc |= test_reacquisition_after_uncommitted_epoch_does_not_merge_stale_row();
    rc |= test_reacquired_transmission_via_continue_commits_one_row();
    rc |= test_back_to_back_same_identity_calls_commit_two_rows();
    rc |= test_explicit_end_then_identityless_reopen_leaves_single_row();
    rc |= test_changed_identity_still_commits_its_own_row();
    rc |= test_reacquisition_gap_beyond_window_commits_two_rows();
    rc |= test_reacquired_segment_may_outlast_the_window();
    rc |= test_flapping_segments_commit_one_row();
    rc |= test_dmr_sync_polarity_flip_still_coalesces();
    rc |= test_textual_identity_reacquisition_coalesces();
    rc |= test_reacquired_segment_contributes_late_source();
    rc |= test_merge_preserves_row_timestamp_under_playfiles();
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
