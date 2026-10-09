// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Publisher for the Android notification's scanner readout.
 */

#include <assert.h>
#include <dsd-neo/app_control/call_view.h>
#include <dsd-neo/app_control/notification_status.h>
#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/audio/audio_activity_internal.h"

/* The record's shape, spelled out rather than left as a bare total: DecoderStatus.kt
   mirrors it as HEADER_FIELDS/SLOT_FIELDS, and a field added on one side without the
   other makes parse() return null for every record -- at which point the notification
   silently stops updating, with nothing logged on either side of JNI. Split the same way
   the Kotlin constants are so the two read as the same statement. */
enum {
    /** version, protocol, 3 flags, 3 frequencies, lead slot, audible stamp, audible age. */
    NOTIFICATION_HEADER_FIELDS = 11,
    NOTIFICATION_SLOT_FIELDS = 11, /**< state, name, tg, src, tg_id, enc, algid, kid, elapsed, emergency, priority. */
    NOTIFICATION_TOTAL_FIELDS = NOTIFICATION_HEADER_FIELDS + NOTIFICATION_SLOT_FIELDS * DSD_CALL_STATE_SLOT_COUNT,
};

/* Header fields read by index: the record grew at the end of its header, so a walk that counts up to "the last header
   field" would land somewhere else after the next change. */
enum {
    NOTIFICATION_FIELD_LEAD_SLOT = 8,
    NOTIFICATION_FIELD_AUDIBLE_STAMP = 9,
    NOTIFICATION_FIELD_AUDIBLE_AGE = 10,
};

/* The record test_encode_matches_expected_record_field_order() publishes, as the encoder must write it.
   tests/android/DecoderStatusTest.kt parses the same literal as its golden v3 record: keep the two byte-identical, so
   the C encoder and the Kotlin reader pin one format between them. */
#define NOTIFICATION_GOLDEN_V3_RECORD                                                                                  \
    "v3\tP25p2\t1\t0\t1\t851006250\t851012500\t851500000\t0\t5000000000\t250"                                          \
    "\t2\tRiverside Fire\t51023\t7654321\t51023\t1\t170\t4660\t1500\t1\t3"                                             \
    "\t0\t\t\t\t0\t0\t0\t0\t0\t0\t0"

/* The audible-audio stamp's clock while a test holds it (dsd_audio_activity_set_clock_for_test()). */
static uint64_t g_audio_now_ms = 0U;

static uint64_t
audio_test_clock(void) {
    return g_audio_now_ms;
}

/* Copies field @p index (0-based) of the tab-separated @p record into @p out. Returns 1 when the record has that field
   and it fits, else 0. */
static int
record_field(const char* record, int index, char* out, size_t out_size) {
    const char* start = record;
    for (int field = 0; field < index; field++) {
        start = strchr(start, '\t');
        if (start == NULL) {
            return 0;
        }
        start++;
    }
    const char* end = strchr(start, '\t');
    const size_t len = (end != NULL) ? (size_t)(end - start) : strlen(start);
    if (len + 1U > out_size) {
        return 0;
    }
    DSD_MEMCPY(out, start, len);
    out[len] = '\0';
    return 1;
}

/* Whether field @p index of @p record reads exactly @p want. */
static int
record_field_is(const char* record, int index, const char* want) {
    char field[DSD_APP_NOTIFICATION_RECORD_SIZE];
    return record_field(record, index, field, sizeof(field)) && strcmp(field, want) == 0;
}

static dsd_state*
make_state(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    assert(state != NULL);
    assert(dsd_call_state_ensure(state) > 0);
    state->synctype = DSD_SYNC_NONE;
    return state;
}

/* Counterpart to make_state(): dsd_call_state_ensure() hangs a ~17 KB call-state
   block off the extension table, so freeing the dsd_state alone leaks it. */
static void
destroy_state(dsd_state* state) {
    if (!state) {
        return;
    }
    dsd_state_ext_free_all(state);
    free(state->event_history_s);
    free(state);
}

/* MUST run first in main(): it is the only point at which nothing has been published
   into this process yet. Moving it later makes it assert the opposite of its name. */
static void
test_get_before_any_publish_reports_nothing(void) {
    dsd_app_notification_status status;
    /* Poisoned first, so "zeroed" is a claim about the callee rather than about calloc. */
    DSD_MEMSET(&status, 0xAB, sizeof(status));

    assert(dsd_app_notification_get(&status) == 0);
    /* Zeroed even when there is nothing to report, so a caller can render from it
       without checking the return value first. */
    assert(status.revision == 0U);
    assert(status.protocol[0] == '\0');
    assert(status.radio_input == 0U);
    assert(status.center_freq_hz == 0);
    assert(status.slots[0].state == DSD_APP_CALL_LINE_NONE);
    assert(status.slots[1].state == DSD_APP_CALL_LINE_NONE);
    /* The one field that is not simply zeroed: 0 is a real slot index, so a caller
       rendering straight from the struct would headline slot 0 on a process that has
       published nothing at all. */
    assert(status.lead_slot == -1);
    /* No audible audio yet: no stamp, and the age's "none" sentinel rather than an age of 0. */
    assert(status.audible_stamp == 0U);
    assert(status.audible_age_ms == -1);
}

/* Only the Android poll reads the record through the encoder, and only it turns the audible-audio stamp on: the API
   feed and the desktop frontends read through get(), and a session none of them watches must keep doing no stamp work
   per audio block. Arming is for the life of the process, so this runs before anything here encodes, and before
   anything publishes. */
static void
test_only_encode_arms_the_audible_stamp(void) {
    dsd_app_notification_status status;
    (void)dsd_app_notification_get(&status);
    (void)dsd_app_notification_get(&status);
    assert(dsd_audio_activity_armed() == 0);
    dsd_audio_activity_note();
    (void)dsd_app_notification_get(&status);
    assert(status.audible_stamp == 0U);
    assert(status.audible_age_ms == -1);

    /* The service starts polling before the engine publishes anything, so its first poll arms the stamp even though it
       has no record to hand back yet. */
    char record[DSD_APP_NOTIFICATION_RECORD_SIZE];
    assert(dsd_app_notification_encode(record, sizeof(record)) == 0);
    assert(dsd_audio_activity_armed() == 1);
}

static void
test_publish_state_carries_protocol_and_call(void) {
    dsd_state* state = make_state();
    state->synctype = DSD_SYNC_P25P2_POS;

    dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 0U, 1234567U, 51023U);
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.frequency_hz = 851012500;
    observation.has_service_metadata = 1;
    observation.emergency = 1;
    observation.priority = 3;
    observation.observed_m = dsd_decode_now_mono_s();
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);

    dsd_app_notification_publish_state(state);

    dsd_app_notification_status status;
    assert(dsd_app_notification_get(&status) == 1);
    assert(strcmp(status.protocol, "P25p2") == 0);
    assert(status.vc_freq_hz == 851012500);
    assert(status.slots[0].state == DSD_APP_CALL_LINE_ACTIVE);
    assert(strcmp(status.slots[0].tg_text, "51023") == 0);
    destroy_state(state);
}

static void
test_unsynced_publishes_empty_protocol(void) {
    dsd_state* state = make_state();
    state->synctype = DSD_SYNC_NONE;
    /* The label is held for a few seconds after the last frame that carried one, so a
       session that has never synced has to start from a cleared hold rather than from
       whatever the test above left behind -- which, in a suite that runs in
       microseconds, is still inside the window. */
    dsd_app_notification_reset();
    dsd_app_notification_publish_state(state);

    dsd_app_notification_status status;
    assert(dsd_app_notification_get(&status) == 1);
    /* NONE and UNKNOWN are not labels worth showing; an empty string is what tells an
       unsynced session from a locked one. */
    assert(status.protocol[0] == '\0');
    destroy_state(state);
}

/* The hold itself: a frame that finds no sync must not blank a label the frame before it
   published. getFrameSync() answers NONE on most iterations of a hunt, so publishing the
   instantaneous value made the label blink -- and, because the record changed each time,
   made the Android service re-post its notification once a second on a silent channel. */
static void
test_sync_label_is_held_across_a_frame_with_no_sync(void) {
    dsd_state* state = make_state();
    dsd_app_notification_reset();

    state->synctype = DSD_SYNC_P25P2_POS;
    dsd_app_notification_publish_state(state);

    state->synctype = DSD_SYNC_NONE;
    dsd_app_notification_publish_state(state);

    dsd_app_notification_status status;
    assert(dsd_app_notification_get(&status) == 1);
    assert(strcmp(status.protocol, "P25p2") == 0);
    destroy_state(state);
}

/* The hold is display smoothing, so it runs on real time like the Qt panel's: a decode clock that
   moves on (a fast replay) must not expire it while no real time has passed. The call views beside
   it still age on the decode clock. */
static void
test_sync_label_hold_runs_on_real_time(void) {
    dsd_state* state = make_state();
    dsd_app_notification_reset();
    const uint64_t t0_ns = 1000000000000000000ULL; // 1e9 s: far from any platform clock reading.
    dsd_decode_clock_use_test(t0_ns);

    state->synctype = DSD_SYNC_P25P2_POS;
    dsd_app_notification_publish_state(state);

    dsd_decode_clock_test_set_ns(t0_ns + 100000000000ULL); // 100 s of decode time, no real time.
    state->synctype = DSD_SYNC_NONE;
    dsd_app_notification_publish_state(state);

    dsd_app_notification_status status;
    assert(dsd_app_notification_get(&status) == 1);
    assert(strcmp(status.protocol, "P25p2") == 0);
    dsd_decode_clock_use_system();
    destroy_state(state);
}

static void
test_publish_opts_carries_radio_and_trunking(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    assert(opts != NULL);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->rtlsdr_center_freq = 851012500U;

    dsd_app_notification_publish_opts(opts);

    dsd_app_notification_status status;
    assert(dsd_app_notification_get(&status) == 1);
    assert(status.radio_input == 1U);
    assert(status.trunking == 1U);
    assert(status.trunk_tuned == 1U);
    assert(status.center_freq_hz == 851012500);
    free(opts);
}

static void
test_non_radio_input_reports_no_centre(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    assert(opts != NULL);
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->rtlsdr_center_freq = 851012500U;

    dsd_app_notification_publish_opts(opts);

    dsd_app_notification_status status;
    assert(dsd_app_notification_get(&status) == 1);
    /* A WAV session has no tuner; publishing the centre would put a plausible reading
       on screen for a run that never had one. */
    assert(status.radio_input == 0U);
    assert(status.center_freq_hz == 0);
    free(opts);
}

/* Issue #575: during an I/Q replay the centre is the one the samples being decoded were captured on, which a recorded
   RETUNE moves while rtlsdr_center_freq keeps the capture's opening centre (dsd_opts_tuned_freq_hz()). */
static void
test_replay_reports_the_capture_centre(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    assert(opts != NULL);
    opts->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", "iqreplay:capture.iq.json");
    opts->rtlsdr_center_freq = 851012500U;
    opts->iq_replay_center_freq = 851500000U;

    dsd_app_notification_publish_opts(opts);

    dsd_app_notification_status status;
    assert(dsd_app_notification_get(&status) == 1);
    assert(status.radio_input == 1U);
    assert(status.center_freq_hz == 851500000);

    /* Without a replay centre the tuned one reads again. */
    opts->iq_replay_center_freq = 0U;
    dsd_app_notification_publish_opts(opts);
    assert(dsd_app_notification_get(&status) == 1);
    assert(status.center_freq_hz == 851012500);
    free(opts);
}

/* The record's two halves are published by separate calls, and the opts half is the one
   that runs first -- dsd_telemetry_publish_both_and_redraw() publishes opts, then the whole
   snapshot body, then state. So there is a window at the start of every session in which
   g_have is already set while no slot has been written, and the sentinel has to survive it:
   a reader following the header's "or -1 for none" and indexing slots[lead_slot] would
   otherwise headline slot 0 on a session with nothing on the air. */
static void
test_opts_only_publish_keeps_the_no_slot_sentinel(void) {
    dsd_app_notification_reset();

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    assert(opts != NULL);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 851012500U;
    dsd_app_notification_publish_opts(opts);
    free(opts);

    dsd_app_notification_status status;
    assert(dsd_app_notification_get(&status) == 1);
    assert(status.slots[0].state == DSD_APP_CALL_LINE_NONE);
    assert(status.slots[1].state == DSD_APP_CALL_LINE_NONE);
    assert(status.lead_slot == -1);

    /* And through the encoder, which passes lead_slot out verbatim. By index: the audible age beside it also reads -1
       with no audio, so a search for the text would pass on the wrong field. */
    char record[DSD_APP_NOTIFICATION_RECORD_SIZE];
    assert(dsd_app_notification_encode(record, sizeof(record)) > 0);
    assert(record_field_is(record, NOTIFICATION_FIELD_LEAD_SLOT, "-1"));
}

static void
test_revision_advances_on_each_publish(void) {
    dsd_state* state = make_state();
    dsd_app_notification_publish_state(state);

    dsd_app_notification_status first;
    assert(dsd_app_notification_get(&first) == 1);

    dsd_app_notification_publish_state(state);
    dsd_app_notification_status second;
    assert(dsd_app_notification_get(&second) == 1);
    assert(second.revision > first.revision);
    destroy_state(state);
}

/* The headline choice rides the wire rather than being remade by the reader. DecoderStatus.kt
   used to pick for itself and picked differently from the Qt hero panel, so the shade and
   the app named different units at the same instant on a two-slot system. */
static void
test_lead_slot_is_published_and_survives_encoding(void) {
    dsd_state* state = make_state();
    dsd_app_notification_reset();

    dsd_app_notification_status status;
    dsd_app_notification_publish_state(state);
    assert(dsd_app_notification_get(&status) == 1);
    /* -1, not 0: 0 is a real slot index, and a reader rendering straight from the struct
       would headline slot 0 on a session with nothing on the air. */
    assert(status.lead_slot == -1);

    /* Slot 1 alone: the lead is the slot with the call, not simply the lowest one. */
    dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 1U, 1234567U, 51023U);
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.observed_m = dsd_decode_now_mono_s() - 1.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_app_notification_publish_state(state);
    assert(dsd_app_notification_get(&status) == 1);
    assert(status.lead_slot == 1);

    /* Both up: the earlier call keeps the headline, matching MonitorScreen.qml's hero. */
    dsd_call_observation other = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 0U, 7654321U, 51024U);
    other.kind = DSD_CALL_KIND_GROUP_VOICE;
    other.observed_m = dsd_decode_now_mono_s();
    assert(dsd_call_state_observe(state, &other, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_app_notification_publish_state(state);
    assert(dsd_app_notification_get(&status) == 1);
    assert(status.lead_slot == 1);

    /* And it reaches the reader: the field sits after the three frequencies, ahead of the
       audible-audio pair. */
    char record[DSD_APP_NOTIFICATION_RECORD_SIZE];
    assert(dsd_app_notification_encode(record, sizeof(record)) > 0);
    assert(record_field_is(record, NOTIFICATION_FIELD_LEAD_SLOT, "1"));

    assert(dsd_call_state_end(state, 1U, dsd_decode_now_mono_s()) == 1);
    dsd_app_notification_publish_state(state);
    assert(dsd_app_notification_get(&status) == 1);
    assert(status.slots[1].state == DSD_APP_CALL_LINE_ENDED);
    assert(status.slots[0].state == DSD_APP_CALL_LINE_ACTIVE);
    assert(status.lead_slot == 0);
    assert(dsd_app_notification_encode(record, sizeof(record)) > 0);
    assert(record_field_is(record, NOTIFICATION_FIELD_LEAD_SLOT, "0"));

    destroy_state(state);
}

static void
test_null_arguments_are_safe(void) {
    dsd_app_notification_publish_state(NULL);
    dsd_app_notification_publish_opts(NULL);
    assert(dsd_app_notification_get(NULL) == 0);
}

/* Byte-exact rather than strchr(): strchr() takes an int and converts it to char, so a
   high byte written as a literal reads differently depending on whether char is signed. */
static int
record_has_byte(const char* record, unsigned char byte) {
    for (const char* p = record; *p != '\0'; p++) {
        if ((unsigned char)*p == byte) {
            return 1;
        }
    }
    return 0;
}

static size_t
count_fields(const char* record) {
    size_t fields = 1;
    for (const char* p = record; *p != '\0'; p++) {
        if (*p == '\t') {
            fields++;
        }
    }
    return fields;
}

static void
test_encode_has_version_and_field_count(void) {
    dsd_state* state = make_state();
    state->synctype = DSD_SYNC_P25P2_POS;
    dsd_app_notification_publish_state(state);

    char record[1024];
    const size_t written = dsd_app_notification_encode(record, sizeof(record));
    assert(written > 0);
    assert(strncmp(record, "v3\t", 3) == 0);
    assert(count_fields(record) == (size_t)NOTIFICATION_TOTAL_FIELDS);
    /* One line: a newline would break the reader's single-record assumption. */
    assert(strchr(record, '\n') == NULL);
    destroy_state(state);
}

/* Publishes an opts half, which is all get() needs to report a record. */
static void
publish_any_opts(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    assert(opts != NULL);
    dsd_app_notification_publish_opts(opts);
    free(opts);
}

/* A record with no audible audio behind it: no stamp, and the age's "none" sentinel, on the struct and on the wire. */
static void
test_audible_pair_reads_none_before_any_audio(void) {
    dsd_app_notification_reset();
    publish_any_opts();

    dsd_app_notification_status status;
    DSD_MEMSET(&status, 0xAB, sizeof(status));
    assert(dsd_app_notification_get(&status) == 1);
    assert(status.audible_stamp == 0U);
    assert(status.audible_age_ms == -1);

    char record[DSD_APP_NOTIFICATION_RECORD_SIZE];
    assert(dsd_app_notification_encode(record, sizeof(record)) > 0);
    assert(record_field_is(record, NOTIFICATION_FIELD_AUDIBLE_STAMP, "0"));
    assert(record_field_is(record, NOTIFICATION_FIELD_AUDIBLE_AGE, "-1"));
}

/* Audible audio noted while armed: the record carries the stamp it stored and that stamp's age. */
static void
test_audible_pair_follows_a_note(void) {
    publish_any_opts();
    assert(dsd_audio_activity_armed() == 1);
    dsd_audio_activity_note();
    uint64_t noted = 0U;
    dsd_audio_activity_read(&noted, NULL);
    assert(noted > 0U);

    dsd_app_notification_status status;
    DSD_MEMSET(&status, 0xAB, sizeof(status));
    assert(dsd_app_notification_get(&status) == 1);
    assert(status.audible_stamp == noted);
    assert(status.audible_age_ms >= 0 && status.audible_age_ms <= DSD_AUDIO_ACTIVITY_MAX_AGE_MS);

    char record[DSD_APP_NOTIFICATION_RECORD_SIZE];
    assert(dsd_app_notification_encode(record, sizeof(record)) > 0);
    char want[32];
    DSD_SNPRINTF(want, sizeof(want), "%llu", (unsigned long long)noted);
    assert(record_field_is(record, NOTIFICATION_FIELD_AUDIBLE_STAMP, want));
    char age_text[32];
    assert(record_field(record, NOTIFICATION_FIELD_AUDIBLE_AGE, age_text, sizeof(age_text)));
    long age = -1;
    assert(dsd_parse_long_strict(age_text, 10, 0L, (long)DSD_AUDIO_ACTIVITY_MAX_AGE_MS, &age) == 0);
    assert(count_fields(record) == (size_t)NOTIFICATION_TOTAL_FIELDS);
}

static void
test_encode_sanitises_control_characters(void) {
    dsd_state* state = make_state();
    state->synctype = DSD_SYNC_P25P2_POS;

    dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 0U, 1234567U, 51023U);
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.observed_m = dsd_decode_now_mono_s();
    DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "Metro\tFire\nDispatch");
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_app_notification_publish_state(state);

    char record[1024];
    assert(dsd_app_notification_encode(record, sizeof(record)) > 0);
    /* Text arrives from CSV imports and off the air; an embedded tab would invent a
       field and desynchronise every field after it. */
    assert(count_fields(record) == (size_t)NOTIFICATION_TOTAL_FIELDS);
    assert(strstr(record, "Metro Fire Dispatch") != NULL);
    destroy_state(state);
}

/* Publishes one call whose target text is @p target verbatim, then encodes it into
   @p record. Callsigns arrive off the air as raw decoded octets and CSV names arrive
   unvalidated, so this is the shape every charset case below takes. */
static void
encode_with_target_text(const char* target, char* record, size_t record_size) {
    dsd_state* state = make_state();
    state->synctype = DSD_SYNC_P25P2_POS;

    dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 0U, 1234567U, 51023U);
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.observed_m = dsd_decode_now_mono_s();
    DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", target);
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_app_notification_publish_state(state);

    assert(dsd_app_notification_encode(record, record_size) > 0);
    destroy_state(state);
}

/* The record is handed straight to JNI's NewStringUTF(), which requires modified UTF-8
   and aborts the process under CheckJNI when it does not get it. Nothing upstream
   filters for charset: D-STAR and YSF callsigns are raw decoded octets off the air, so
   any byte value at all can reach here. */
static void
test_encode_replaces_a_lone_continuation_byte(void) {
    char record[DSD_APP_NOTIFICATION_RECORD_SIZE];
    /* 0x9F with no lead byte in front of it: not part of any sequence. */
    encode_with_target_text("KD\x9F"
                            "ABC",
                            record, sizeof(record));
    assert(strstr(record, "KD?ABC") != NULL);
    assert(!record_has_byte(record, 0x9FU));
}

static void
test_encode_drops_a_truncated_trailing_sequence(void) {
    char record[DSD_APP_NOTIFICATION_RECORD_SIZE];
    /* U+20AC is E2 82 AC; the field ends one byte short of it. Emitting the stump would
       hand NewStringUTF() a lead byte with a missing continuation -- the exact shape
       CheckJNI aborts on -- and a '?' per byte would invent characters the sender never
       sent. */
    encode_with_target_text("NET\xE2\x82", record, sizeof(record));
    assert(strstr(record, "\tNET\t") != NULL);
    assert(!record_has_byte(record, 0xE2U));
    assert(!record_has_byte(record, 0x82U));
}

static void
test_encode_replaces_an_overlong_encoding(void) {
    char record[DSD_APP_NOTIFICATION_RECORD_SIZE];
    /* C0 AF is a non-minimal encoding of '/'. Decoders that accept it are how a filter
       that only looks at single bytes gets walked past, so both bytes are rejected. */
    encode_with_target_text("A\xC0\xAF"
                            "B",
                            record, sizeof(record));
    assert(strstr(record, "A??B") != NULL);
    assert(!record_has_byte(record, 0xC0U));
    assert(!record_has_byte(record, 0xAFU));
}

static void
test_encode_keeps_valid_multibyte_text(void) {
    char record[DSD_APP_NOTIFICATION_RECORD_SIZE];
    /* Two-byte (U+00C7), three-byte (U+20AC) and four-byte (U+1F525) sequences, all
       well-formed: the filter must pass them through byte for byte. A charset check
       that rejected everything above 0x7F would be just as wrong as no check at all. */
    encode_with_target_text("A\xC3\x87 \xE2\x82\xAC \xF0\x9F\x94\xA5", record, sizeof(record));
    assert(strstr(record, "A\xC3\x87 \xE2\x82\xAC \xF0\x9F\x94\xA5") != NULL);
    assert(strchr(record, '?') == NULL);
}

/* Event_History::t_name is a char[200] and dsd_app_slot_call::name must match it: it is
   the Qt monitor's heroName, which returned the alias unbounded before it moved behind
   app-control. Sizing it like tg_text/src_text (char[64], and correctly so, since those
   come from the canonical call state) cut every CSV alias at 63 characters. */
static void
test_encode_round_trips_a_long_group_name(void) {
    dsd_state* state = make_state();
    Event_History_I* history = (Event_History_I*)calloc(DSD_CALL_STATE_SLOT_COUNT, sizeof(Event_History_I));
    assert(history != NULL);
    state->event_history_s = history;
    state->synctype = DSD_SYNC_P25P2_POS;

    dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 0U, 1234567U, 51023U);
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.observed_m = dsd_decode_now_mono_s();
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);

    /* 199 characters, with a distinct tail so a truncation cannot pass on a prefix
       match, and comfortably past the 63 the old sizing allowed. */
    char alias[200];
    DSD_MEMSET(alias, 'A', sizeof(alias) - 1U);
    alias[sizeof(alias) - 1U] = '\0';
    DSD_MEMCPY(alias + 190, "TAILEND", 7U);
    history[0].Event_History_Items[0].target_id = 51023U;
    DSD_SNPRINTF(history[0].Event_History_Items[0].t_name, sizeof(history[0].Event_History_Items[0].t_name), "%s",
                 alias);

    dsd_app_notification_publish_state(state);

    dsd_app_notification_status status;
    assert(dsd_app_notification_get(&status) == 1);
    assert(strcmp(status.slots[0].name, alias) == 0);

    char record[DSD_APP_NOTIFICATION_RECORD_SIZE];
    assert(dsd_app_notification_encode(record, sizeof(record)) > 0);
    /* Tab-delimited on both sides: a truncated alias would still match a bare strstr
       of its prefix. */
    char expected[sizeof(alias) + 2U];
    DSD_SNPRINTF(expected, sizeof(expected), "\t%s\t", alias);
    assert(strstr(record, expected) != NULL);
    /* The record must still fit, which is the other half of the fix. */
    assert(count_fields(record) == (size_t)NOTIFICATION_TOTAL_FIELDS);

    destroy_state(state);
}

static void
test_encode_rejects_a_short_buffer(void) {
    dsd_state* state = make_state();
    dsd_app_notification_publish_state(state);

    char tiny[8];
    /* Truncation would hand the reader a record it could parse as a shorter one. */
    assert(dsd_app_notification_encode(tiny, sizeof(tiny)) == 0);
    assert(dsd_app_notification_encode(NULL, 0) == 0);
    destroy_state(state);
}

/* A field-count check cannot catch two same-typed fields swapped (say, algid and kid,
   or cc_freq_hz and vc_freq_hz) -- only a comparison against an independently built
   expected record can, and only if the two fields actually hold different values.
   Every *adjacent* same-typed pair below is deliberately given different values, so a
   transposition of neighboring encoder arguments -- the shape a copy/paste or
   reordering slip actually takes -- changes the string:
    - radio_input=1, trunking=0, trunk_tuned=1: neighbors differ (1,0) and (0,1).
      radio_input and trunk_tuned do land on the same value, but they are not
      neighbors -- trunking sits between them -- and with only two possible values for
      three boolean flags, some pair has to repeat.
    - name and tg_text differ because a CSV-imported group name is staged on the slot's
      history row below; without one, dsd_app_slot_call_view() (call_view.c) makes name
      fall back to tg_text verbatim, which would make that swap invisible.
    - lead_slot=0, audible_stamp=5000000000, audible_age_ms=250 and slot 0's state=2:
      the header's last three numbers and the first slot field all differ. The stamp
      sits past 2^32, so a 32-bit conversion anywhere would show too.
   Both clocks the record reads are held for the test -- the decode clock that ages the
   call (elapsed_ms) and the audible-audio stamp's -- so every field is a fixed function
   of the inputs set here, and the whole record is a literal,
   NOTIFICATION_GOLDEN_V3_RECORD, that the Kotlin reader's test parses as well. */
static void
test_encode_matches_expected_record_field_order(void) {
    /* 1000 s, then 1.5 s later: both exact in a double, so elapsed_ms is exactly 1500. */
    const uint64_t t0_ns = 1000000000000ULL;
    dsd_decode_clock_use_test(t0_ns);
    dsd_audio_activity_set_clock_for_test(audio_test_clock);
    g_audio_now_ms = 5000000000ULL;
    dsd_audio_activity_note();

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    assert(opts != NULL);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 0;
    opts->trunk_is_tuned = 1;
    opts->rtlsdr_center_freq = 851500000U;
    dsd_app_notification_publish_opts(opts);
    free(opts);

    dsd_state* state = make_state();
    /* Attaches a staged history row purely so the CSV-group-name lookup below has
       something to read; see dsd_app_slot_call_view()'s staged_group_name(). */
    Event_History_I* history = (Event_History_I*)calloc(DSD_CALL_STATE_SLOT_COUNT, sizeof(Event_History_I));
    assert(history != NULL);
    state->event_history_s = history;
    state->synctype = DSD_SYNC_P25P2_POS;
    state->trunk_cc_freq = 851006250L;

    dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 0U, 7654321U, 51023U);
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.frequency_hz = 851012500;
    observation.has_service_metadata = 1;
    observation.emergency = 1;
    observation.priority = 3;
    observation.observed_m = dsd_decode_now_mono_s();
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);

    /* Staged on the same talkgroup id the observation above carries, so
       staged_group_name() actually uses it for slot 0's name instead of falling back
       to tg_text. */
    history[0].Event_History_Items[0].target_id = 51023U;
    DSD_SNPRINTF(history[0].Event_History_Items[0].t_name, sizeof(history[0].Event_History_Items[0].t_name), "%s",
                 "Riverside Fire");

    dsd_call_crypto_update crypto;
    DSD_MEMSET(&crypto, 0, sizeof(crypto));
    crypto.classification = DSD_CALL_CRYPTO_ENCRYPTED;
    crypto.algid = 0xAAU;
    crypto.kid = 0x1234U;
    crypto.observed_m = dsd_decode_now_mono_s();
    assert(dsd_call_state_update_crypto(state, 0U, &crypto) > 0);

    dsd_decode_clock_test_set_ns(t0_ns + 1500000000ULL);
    dsd_app_notification_publish_state(state);
    g_audio_now_ms = 5000000250ULL;

    dsd_app_notification_status status;
    assert(dsd_app_notification_get(&status) == 1);
    assert(status.slots[0].enc == 1U);
    assert(status.slots[0].algid == 0xAAU);
    assert(status.slots[0].kid == 0x1234U);
    assert(strcmp(status.slots[0].name, "Riverside Fire") == 0);
    assert(strcmp(status.slots[0].tg_text, "51023") == 0);
    assert(status.slots[0].elapsed_ms == 1500U);
    assert(status.audible_stamp == 5000000000ULL);
    assert(status.audible_age_ms == 250);

    char expected[DSD_APP_NOTIFICATION_RECORD_SIZE];
    const int n =
        DSD_SNPRINTF(expected, sizeof(expected),
                     "v3\t%s\t%u\t%u\t%u\t%lld\t%lld\t%lld\t%d\t%llu\t%ld"
                     "\t%d\t%s\t%s\t%s\t%llu\t%u\t%u\t%u\t%u\t%u\t%u"
                     "\t%d\t%s\t%s\t%s\t%llu\t%u\t%u\t%u\t%u\t%u\t%u",
                     "P25p2", 1U, 0U, 1U, 851006250LL, 851012500LL, 851500000LL, 0, 5000000000ULL, 250L,
                     DSD_APP_CALL_LINE_ACTIVE, "Riverside Fire", "51023", "7654321", 51023ULL, 1U, 0xAAU, 0x1234U,
                     1500U, 1U, 3U, DSD_APP_CALL_LINE_NONE, "", "", "", 0ULL, 0U, 0U, 0U, 0U, 0U, 0U);
    assert(n > 0 && (size_t)n < sizeof(expected));

    char record[DSD_APP_NOTIFICATION_RECORD_SIZE];
    assert(dsd_app_notification_encode(record, sizeof(record)) > 0);
    assert(strcmp(record, expected) == 0);
    /* The same record, pinned as the literal the Kotlin side reads. */
    assert(strcmp(record, NOTIFICATION_GOLDEN_V3_RECORD) == 0);

    dsd_audio_activity_set_clock_for_test(NULL);
    dsd_audio_activity_reset();
    dsd_decode_clock_use_system();
    destroy_state(state);
}

/* The stamp is unsigned on the wire: the largest one must print as itself, not as the -1 a signed conversion makes of
   it. DecoderStatusTest.kt reads the same value back as ULong.MAX_VALUE. */
static void
test_encode_writes_the_audible_stamp_unsigned(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    assert(opts != NULL);
    dsd_app_notification_publish_opts(opts);
    free(opts);

    dsd_audio_activity_set_clock_for_test(audio_test_clock);
    g_audio_now_ms = UINT64_MAX;
    dsd_audio_activity_note();

    char record[DSD_APP_NOTIFICATION_RECORD_SIZE];
    assert(dsd_app_notification_encode(record, sizeof(record)) > 0);
    assert(record_field_is(record, NOTIFICATION_FIELD_AUDIBLE_STAMP, "18446744073709551615"));
    assert(record_field_is(record, NOTIFICATION_FIELD_AUDIBLE_AGE, "0"));
    assert(count_fields(record) == (size_t)NOTIFICATION_TOTAL_FIELDS);

    dsd_audio_activity_set_clock_for_test(NULL);
    dsd_audio_activity_reset();
}

/* Below: the multi-consumer property itself. Every test above drives publish and get
   from a single thread, which can never race itself. The publisher's entire reason to
   exist is that dsd_app_notification_get() is safe to call from any thread, and from
   several at once, concurrently with the decode thread publishing. This is the test
   that gives the TSan run something to say no to. */

#define NOTIFICATION_RACE_WRITER_ITERATIONS 4000
#define NOTIFICATION_RACE_READER_ITERATIONS 8000
#define NOTIFICATION_RACE_FREQ_P25P2        851000000L
#define NOTIFICATION_RACE_FREQ_DMR          852000000L

typedef struct {
    dsd_state* state;
} notification_race_writer_ctx;

typedef struct {
    int mismatches; /* Any observed (protocol, freq) pair outside the two published ones. */
    int matches;    /* How many gets actually landed on one of the two profiles below. */
} notification_race_reader_ctx;

static DSD_THREAD_RETURN_TYPE
notification_race_writer(void* arg) {
    notification_race_writer_ctx* ctx = (notification_race_writer_ctx*)arg;
    for (int i = 0; i < NOTIFICATION_RACE_WRITER_ITERATIONS; i++) {
        /* Two distinct, fully-formed profiles. protocol and vc_freq_hz are written
           together under dsd_app_notification_publish_state()'s single critical
           section, so any observer must see one whole profile or the other -- never a
           protocol string from one paired with a frequency from the other. */
        if ((i & 1) == 0) {
            ctx->state->synctype = DSD_SYNC_P25P2_POS;
            ctx->state->trunk_vc_freq[0] = NOTIFICATION_RACE_FREQ_P25P2;
        } else {
            ctx->state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
            ctx->state->trunk_vc_freq[0] = NOTIFICATION_RACE_FREQ_DMR;
        }
        dsd_app_notification_publish_state(ctx->state);
    }
    DSD_THREAD_RETURN;
}

static DSD_THREAD_RETURN_TYPE
notification_race_reader(void* arg) {
    notification_race_reader_ctx* ctx = (notification_race_reader_ctx*)arg;
    for (int i = 0; i < NOTIFICATION_RACE_READER_ITERATIONS; i++) {
        dsd_app_notification_status status;
        /* Poisoned so a get() that forgot to fill a field would show up as garbage
           rather than as a zero that might accidentally match nothing and pass anyway. */
        DSD_MEMSET(&status, 0x7E, sizeof(status));
        if (dsd_app_notification_get(&status) == 0) {
            continue; /* Should not happen once the writer has seeded, but stay safe. */
        }
        const int is_p25 = strcmp(status.protocol, "P25p2") == 0;
        const int is_dmr = strcmp(status.protocol, "DMR") == 0;
        /* test_concurrent_publish_and_get_never_tears() seeds g_status with the P25p2
           profile synchronously before any thread is created, and dsd_thread_create()'s
           happens-before guarantee means no reader here can observe anything older than
           that seed. From then on the writer only ever publishes the P25p2 or DMR
           profile, so every get() from here to the end of the race must land on one of
           the two -- an empty string, a third protocol, or a truncated/corrupted one can
           only mean bytes from two different publishes were observed in one record. */
        if (!is_p25 && !is_dmr) {
            ctx->mismatches++;
            continue;
        }
        ctx->matches++;
        if (is_p25 && status.vc_freq_hz != NOTIFICATION_RACE_FREQ_P25P2) {
            ctx->mismatches++;
        }
        if (is_dmr && status.vc_freq_hz != NOTIFICATION_RACE_FREQ_DMR) {
            ctx->mismatches++;
        }
    }
    DSD_THREAD_RETURN;
}

static void
test_concurrent_publish_and_get_never_tears(void) {
    dsd_state* state = make_state();
    /* Seed with one of the two profiles before spawning anything: otherwise the very
       first gets could legitimately observe whatever an earlier test in this process
       last published (some other protocol label entirely) and this test would have no
       way to tell that apart from a real torn read. */
    state->synctype = DSD_SYNC_P25P2_POS;
    state->trunk_vc_freq[0] = NOTIFICATION_RACE_FREQ_P25P2;
    dsd_app_notification_publish_state(state);

    notification_race_writer_ctx writer_ctx;
    writer_ctx.state = state;
    notification_race_reader_ctx reader_ctx_a;
    reader_ctx_a.mismatches = 0;
    reader_ctx_a.matches = 0;
    notification_race_reader_ctx reader_ctx_b;
    reader_ctx_b.mismatches = 0;
    reader_ctx_b.matches = 0;

    dsd_thread_t writer;
    dsd_thread_t reader_a;
    dsd_thread_t reader_b;
    /* One writer, matching the documented contract (the decode thread is the only
       publisher); two readers, to exercise "any number of them concurrently" rather
       than just a single second thread. */
    assert(dsd_thread_create(&writer, notification_race_writer, &writer_ctx) == 0);
    assert(dsd_thread_create(&reader_a, notification_race_reader, &reader_ctx_a) == 0);
    assert(dsd_thread_create(&reader_b, notification_race_reader, &reader_ctx_b) == 0);

    assert(dsd_thread_join(writer) == 0);
    assert(dsd_thread_join(reader_a) == 0);
    assert(dsd_thread_join(reader_b) == 0);

    assert(reader_ctx_a.mismatches == 0);
    assert(reader_ctx_b.mismatches == 0);
    /* Not vacuous: both readers must have actually landed inside the race window, not
       only ever seen the pre-seeded value before the writer got scheduled. */
    assert(reader_ctx_a.matches > 0);
    assert(reader_ctx_b.matches > 0);
    destroy_state(state);
}

/* MUST run last in main(): it puts the module back to its never-published state, which
   is the one condition every other test here needs not to be in. */
static void
test_reset_forgets_the_published_status(void) {
    dsd_state* state = make_state();
    state->synctype = DSD_SYNC_P25P2_POS;
    state->trunk_vc_freq[0] = 851012500L;
    dsd_app_notification_publish_state(state);
    dsd_audio_activity_note();

    dsd_app_notification_status before;
    assert(dsd_app_notification_get(&before) == 1);
    assert(before.protocol[0] != '\0');
    assert(before.audible_stamp != 0U);

    dsd_app_notification_reset();

    dsd_app_notification_status after;
    /* Poisoned, so "zeroed" is a claim about the callee. */
    DSD_MEMSET(&after, 0xAB, sizeof(after));
    /* Back to reporting nothing, not merely to a stale record with a fresh revision:
       the record is a module static that outlives its session, and the Android service
       polls it before the next session has published anything. */
    assert(dsd_app_notification_get(&after) == 0);
    assert(after.revision == 0U);
    assert(after.protocol[0] == '\0');
    assert(after.vc_freq_hz == 0);
    assert(after.slots[0].state == DSD_APP_CALL_LINE_NONE);
    /* The audible-audio stamp goes with the session too, or the next session's first record would report the previous
       one's last audio as recent. It stays armed: the reader that armed it is still polling. */
    assert(after.audible_stamp == 0U);
    assert(after.audible_age_ms == -1);
    uint64_t stamp = 1U;
    dsd_audio_activity_read(&stamp, NULL);
    assert(stamp == 0U);
    assert(dsd_audio_activity_armed() == 1);

    /* And the encoder, the only thing the service actually reads, has nothing to give. */
    char record[DSD_APP_NOTIFICATION_RECORD_SIZE];
    assert(dsd_app_notification_encode(record, sizeof(record)) == 0);
    destroy_state(state);
}

/* Both publishers early-return until a reader has asked once (g_wanted, notification_status.c).
   Every publish below therefore depends on a get() having happened first -- which today is
   incidental, because test_get_before_any_publish_reports_nothing() happens to run first.
   Stated here instead so the dependency does not ride on call order: reordering main() would
   otherwise turn the new first test's publish into a silent no-op and surface as an assertion
   about the getter, pointing nowhere near the cause. */
static void
arm_publishers(void) {
    dsd_app_notification_status probe;
    /* Deliberately before test_get_before_any_publish_reports_nothing(): arming is all this
       does, and get() reports nothing until something is actually published. */
    (void)dsd_app_notification_get(&probe);
}

int
main(void) {
    arm_publishers();
    test_get_before_any_publish_reports_nothing();
    test_only_encode_arms_the_audible_stamp();
    test_publish_state_carries_protocol_and_call();
    test_unsynced_publishes_empty_protocol();
    test_sync_label_is_held_across_a_frame_with_no_sync();
    test_sync_label_hold_runs_on_real_time();
    test_publish_opts_carries_radio_and_trunking();
    test_non_radio_input_reports_no_centre();
    test_replay_reports_the_capture_centre();
    test_opts_only_publish_keeps_the_no_slot_sentinel();
    test_revision_advances_on_each_publish();
    test_null_arguments_are_safe();
    test_lead_slot_is_published_and_survives_encoding();
    test_encode_has_version_and_field_count();
    test_audible_pair_reads_none_before_any_audio();
    test_audible_pair_follows_a_note();
    test_encode_sanitises_control_characters();
    test_encode_replaces_a_lone_continuation_byte();
    test_encode_drops_a_truncated_trailing_sequence();
    test_encode_replaces_an_overlong_encoding();
    test_encode_keeps_valid_multibyte_text();
    test_encode_round_trips_a_long_group_name();
    test_encode_rejects_a_short_buffer();
    test_encode_matches_expected_record_field_order();
    test_encode_writes_the_audible_stamp_unsigned();
    test_concurrent_publish_and_get_never_tears();
    test_reset_forgets_the_published_status();
    printf("APP_CONTROL_NOTIFICATION_STATUS ok\n");
    return 0;
}
