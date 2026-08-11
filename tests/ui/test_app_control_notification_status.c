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
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/threading.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static dsd_state*
make_state(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    assert(state != NULL);
    assert(dsd_call_state_ensure(state) > 0);
    state->synctype = DSD_SYNC_NONE;
    return state;
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
}

static void
test_publish_state_carries_protocol_and_call(void) {
    dsd_state* state = make_state();
    state->synctype = DSD_SYNC_P25P2_POS;

    dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 0U, 1234567U, 51023U);
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.frequency_hz = 851012500;
    observation.observed_m = dsd_app_notification_test_now_m();
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);

    dsd_app_notification_publish_state(state);

    dsd_app_notification_status status;
    assert(dsd_app_notification_get(&status) == 1);
    assert(strcmp(status.protocol, "P25p2") == 0);
    assert(status.vc_freq_hz == 851012500);
    assert(status.slots[0].state == DSD_APP_CALL_LINE_ACTIVE);
    assert(strcmp(status.slots[0].tg_text, "51023") == 0);
    free(state);
}

static void
test_unsynced_publishes_empty_protocol(void) {
    dsd_state* state = make_state();
    state->synctype = DSD_SYNC_NONE;
    dsd_app_notification_publish_state(state);

    dsd_app_notification_status status;
    assert(dsd_app_notification_get(&status) == 1);
    /* NONE and UNKNOWN are not labels worth showing; an empty string is what tells an
       unsynced session from a locked one. */
    assert(status.protocol[0] == '\0');
    free(state);
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
    free(state);
}

static void
test_null_arguments_are_safe(void) {
    dsd_app_notification_publish_state(NULL);
    dsd_app_notification_publish_opts(NULL);
    assert(dsd_app_notification_get(NULL) == 0);
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
    free(state);
}

int
main(void) {
    test_get_before_any_publish_reports_nothing();
    test_publish_state_carries_protocol_and_call();
    test_unsynced_publishes_empty_protocol();
    test_publish_opts_carries_radio_and_trunking();
    test_non_radio_input_reports_no_centre();
    test_revision_advances_on_each_publish();
    test_null_arguments_are_safe();
    test_concurrent_publish_and_get_never_tears();
    printf("APP_CONTROL_NOTIFICATION_STATUS ok\n");
    return 0;
}
