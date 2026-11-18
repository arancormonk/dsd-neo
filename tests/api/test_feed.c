// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The API feed's scan avoid copy (issue #672) through its test hooks, against stand-ins for the server: a page reader
 * held between pinning the list and reading it while the decode thread swaps in a new list, and a failed collection of
 * a -Y list followed by a retry at the same key, and the cached records and copy dropped when no client is connected
 * and replaced at a decoder restart; the status record's tunable for each kind of input (issue #743); event rows'
 * positions and the positions topic (issue #800).
 */

#include <assert.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "json.h"

/*============================================================================
 * Stand-ins for the server: g_clients clients connected (one unless a test
 * says otherwise), subscribed to the topics in g_interest (none unless a test
 * says otherwise); the lines broadcast on the positions topic are kept
 *============================================================================*/

static int g_clients = 1;
static uint32_t g_interest = 0U;
static dsd_json_buf g_position_lines;

int
dsd_api_client_count(void) {
    return g_clients;
}

int
dsd_api_topic_interest(uint32_t topic) {
    return (topic & g_interest) != 0U;
}

void
dsd_api_broadcast(const char* line, size_t len, uint32_t topic) {
    (void)line;
    (void)len;
    assert(topic != DSD_API_TOPIC_POSITIONS); /* positions go through dsd_api_broadcast_positions() */
}

/* One subscriber that subscribed before every report: it gets the whole batch, whose index must describe it. */
void
dsd_api_broadcast_positions(const char* data, size_t len, const dsd_api_position_line* lines, size_t count) {
    assert(data != NULL && lines != NULL && count > 0U && lines[0].offset == 0U);
    for (size_t i = 0; i < count; i++) {
        const size_t end = (i + 1U < count) ? lines[i + 1U].offset : len;
        assert(lines[i].offset < end && data[end - 1U] == '\n' && lines[i].rev > 0U);
        assert(i == 0U || lines[i].rev > lines[i - 1U].rev);
    }
    assert(dsd_json_buf_append(&g_position_lines, data, len) == 0);
}

/*============================================================================
 * Helpers
 *============================================================================*/

static dsd_opts* g_opts = NULL;
static dsd_state* g_state = NULL;

/* One feed update: past the 250 ms interval, then a publish. */
static void
feed_tick(void) {
    dsd_sleep_ms(260);
    dsd_telemetry_publish_both_and_redraw(g_opts, g_state);
}

/* Replace the -Y list with @p rows rows named "<prefix> <row>", avoiding every @p every-th row from @p first. */
static void
fill_lcn_list(int rows, const char* prefix, int first, int every) {
    dsd_state_trunk_lcn_replace_begin(g_state);
    assert(dsd_state_trunk_lcn_reserve(g_state, (size_t)rows) == 0);
    for (int row = 0; row < rows; row++) {
        char name[32];
        DSD_SNPRINTF(name, sizeof name, "%s %d", prefix, row);
        *dsd_state_trunk_lcn_slot(g_state, row) = 450000000L + (long)row * 12500L;
        assert(dsd_state_trunk_lcn_name_set(g_state, (size_t)row, name) == 0);
    }
    g_state->lcn_freq_count = rows;
    for (int row = first; row < rows; row += every) {
        assert(dsd_state_trunk_lcn_avoid_set(g_state, (size_t)row, 1) == 0);
    }
}

/* One page as `get` writes its data value, parsed. */
static dsd_json_node*
page(uint64_t offset, size_t limit) {
    dsd_json_buf buf;
    dsd_json_buf_init(&buf);
    dsd_json_writer w;
    dsd_json_writer_init(&w, &buf);
    dsd_api_feed_scan_avoids_page(&w, offset, limit);
    assert(!dsd_json_writer_failed(&w) && buf.data != NULL);
    dsd_json_node* root = NULL;
    char err[96];
    assert(dsd_json_parse(buf.data, &root, err, sizeof err) == 0);
    dsd_json_buf_free(&buf);
    return root;
}

/* Every entry of @p data is one of "<prefix> <row>" for the rows first, first + every, ...; returns the count. */
static size_t
expect_entries(const dsd_json_node* data, const char* prefix, int first, int every) {
    const dsd_json_node* entries = dsd_json_obj_get(data, "entries");
    assert(entries != NULL && entries->type == DSD_JSON_ARRAY);
    for (size_t i = 0; i < entries->count; i++) {
        const int row = first + (int)i * every;
        char name[32];
        DSD_SNPRINTF(name, sizeof name, "%s %d", prefix, row);
        int64_t got_row = -1;
        assert(dsd_json_as_i64(dsd_json_obj_get(entries->items[i], "row"), &got_row) == 0 && got_row == row);
        const char* got_name = dsd_json_as_str(dsd_json_obj_get(entries->items[i], "name"));
        assert(got_name != NULL && strcmp(got_name, name) == 0);
    }
    return entries->count;
}

static uint64_t
data_u64(const dsd_json_node* data, const char* key) {
    uint64_t v = 0U;
    assert(dsd_json_as_u64(dsd_json_obj_get(data, key), &v) == 0);
    return v;
}

/*============================================================================
 * A reader pinned while the list is swapped
 *============================================================================*/

typedef struct {
    dsd_mutex_t mu;
    dsd_cond_t cond;
    int armed;    /* the next page call parks in the hook */
    int pinned;   /* the reader holds its reference and waits */
    int released; /* the test let it go on */
    int timed_out;
    dsd_json_node* result;
} pin_ctx;

static pin_ctx g_pin;

static void
on_page_pinned(void) {
    dsd_mutex_lock(&g_pin.mu);
    if (!g_pin.armed) {
        dsd_mutex_unlock(&g_pin.mu);
        return;
    }
    g_pin.armed = 0;
    g_pin.pinned = 1;
    (void)dsd_cond_broadcast(&g_pin.cond);
    /* Bounded, so a feed that formats under its lock fails the test instead of hanging it. */
    for (int i = 0; i < 50 && !g_pin.released; i++) {
        (void)dsd_cond_timedwait(&g_pin.cond, &g_pin.mu, 100U);
    }
    g_pin.timed_out = !g_pin.released;
    dsd_mutex_unlock(&g_pin.mu);
}

static DSD_THREAD_RETURN_TYPE
pinned_reader(void* arg) {
    (void)arg;
    dsd_json_node* root = page(0U, 64U);
    dsd_mutex_lock(&g_pin.mu);
    g_pin.result = root;
    dsd_mutex_unlock(&g_pin.mu);
    DSD_THREAD_RETURN;
}

/* A session thread that has pinned list X keeps reading X, whole, after the decode thread swapped in list Y and
   dropped its own reference to X; the swap and a fresh page of Y complete while that reader still holds X, so the
   feed formats outside its lock. X is freed by the reader (a leak shows under asan). */
static void
test_reader_keeps_pinned_list(void) {
    fill_lcn_list(60, "X", 0, 3); /* 20 avoided */
    feed_tick();
    dsd_json_node* root = page(0U, 64U);
    assert(expect_entries(root, "X", 0, 3) == 20U);
    dsd_json_free(root);

    DSD_MEMSET(&g_pin, 0, sizeof g_pin);
    assert(dsd_mutex_init(&g_pin.mu) == 0 && dsd_cond_init(&g_pin.cond) == 0);
    g_pin.armed = 1;
    dsd_api_feed_test_on_page_pinned(on_page_pinned);
    dsd_thread_t reader;
    assert(dsd_thread_create(&reader, pinned_reader, NULL) == 0);
    dsd_mutex_lock(&g_pin.mu);
    for (int i = 0; i < 50 && !g_pin.pinned; i++) {
        (void)dsd_cond_timedwait(&g_pin.cond, &g_pin.mu, 100U);
    }
    const int pinned = g_pin.pinned;
    dsd_mutex_unlock(&g_pin.mu);
    assert(pinned);

    fill_lcn_list(60, "Y", 1, 6); /* 10 avoided */
    feed_tick();                  /* swaps Y in and releases the list's reference to X */
    root = page(0U, 64U);
    assert(expect_entries(root, "Y", 1, 6) == 10U);
    dsd_json_free(root);

    dsd_mutex_lock(&g_pin.mu);
    g_pin.released = 1;
    (void)dsd_cond_broadcast(&g_pin.cond);
    dsd_mutex_unlock(&g_pin.mu);
    assert(dsd_thread_join(reader) == 0);
    dsd_api_feed_test_on_page_pinned(NULL);
    assert(!g_pin.timed_out && g_pin.result != NULL);
    assert(data_u64(g_pin.result, "total") == 20U);
    assert(expect_entries(g_pin.result, "X", 0, 3) == 20U);
    dsd_json_free(g_pin.result);
    (void)dsd_cond_destroy(&g_pin.cond);
    (void)dsd_mutex_destroy(&g_pin.mu);
}

/*============================================================================
 * A failed collection of a -Y list
 *============================================================================*/

/* The status record's scan_avoids object, as `get status` serves it. */
static void
expect_status_has(const char* needle) {
    dsd_json_buf buf;
    dsd_json_buf_init(&buf);
    assert(dsd_api_feed_latest("status", &buf) == 1);
    if (strstr(buf.data, needle) == NULL) {
        DSD_FPRINTF(stderr, "status lacks %s: %s\n", needle, buf.data);
        assert(0);
    }
    dsd_json_buf_free(&buf);
}

/* T2 for the -Y kind: list A collected, revision B whose copy cannot be allocated, then B again. Status and the page
   agree at each step; A's entries are never served for B; the retry happens at B's unchanged key. */
static void
test_lcn_failed_collection(void) {
    fill_lcn_list(30, "A", 0, 5); /* 6 avoided */
    feed_tick();
    dsd_json_node* root = page(0U, 64U);
    assert(dsd_json_obj_get(root, "available")->boolean == 1 && expect_entries(root, "A", 0, 5) == 6U);
    dsd_json_free(root);

    fill_lcn_list(30, "B", 2, 7); /* 4 avoided */
    char want[160];
    DSD_SNPRINTF(want, sizeof want, "\"scan_avoids\":{\"available\":false,\"kind\":\"lcn\",\"total\":null,");
    dsd_api_feed_test_fail_block_allocs(1);
    feed_tick();
    expect_status_has(want);
    root = page(0U, 64U);
    assert(dsd_json_obj_get(root, "available")->boolean == 0);
    assert(dsd_json_obj_get(root, "total")->type == DSD_JSON_NULL);
    assert(data_u64(root, "revision") == g_state->lcn_avoid_rev && expect_entries(root, "B", 2, 7) == 0U);
    dsd_json_free(root);

    feed_tick(); /* same key B, nothing failing now */
    expect_status_has("\"scan_avoids\":{\"available\":true,\"kind\":\"lcn\",\"total\":4,");
    root = page(0U, 64U);
    assert(data_u64(root, "revision") == g_state->lcn_avoid_rev && expect_entries(root, "B", 2, 7) == 4U);
    dsd_json_free(root);
}

/*============================================================================
 * Cached records across a stretch with no clients and a restart
 *============================================================================*/

static const char* const k_cached[] = {"status", "call", "system", "metrics", "quality"};

/* Every cached record is there (1) or none is (0); the scan avoid page agrees. */
static void
expect_cached(int want) {
    for (size_t i = 0; i < sizeof k_cached / sizeof k_cached[0]; i++) {
        dsd_json_buf buf;
        dsd_json_buf_init(&buf);
        const int have = dsd_api_feed_latest(k_cached[i], &buf);
        dsd_json_buf_free(&buf);
        if (have != want) {
            DSD_FPRINTF(stderr, "get %s: %d, want %d\n", k_cached[i], have, want);
            assert(0);
        }
    }
    dsd_json_node* root = page(0U, 64U);
    assert((root->type != DSD_JSON_NULL) == (want != 0));
    dsd_json_free(root);
}

/* No client connected: nothing refreshes the records, so the feed keeps none a later client could take for current.
   The first publish once a client connects runs the update at once. A restarted decoder (a new state instance)
   replaces every record at its first publish, without waiting out the interval. */
static void
test_cache_never_stale(void) {
    feed_tick();
    expect_cached(1);

    g_clients = 0;
    dsd_telemetry_publish_both_and_redraw(g_opts, g_state);
    expect_cached(0);
    dsd_telemetry_publish_both_and_redraw(g_opts, g_state);
    expect_cached(0);

    g_clients = 1;
    dsd_telemetry_publish_both_and_redraw(g_opts, g_state); /* no wait: the first update runs at this publish */
    expect_cached(1);

    const uint64_t before = g_state->state_instance;
    freeState(g_state);
    initState(g_state);
    assert(g_state->state_instance != before);
    dsd_telemetry_publish_both_and_redraw(g_opts, g_state); /* within the interval of the last update */
    char want[96];
    DSD_SNPRINTF(want, sizeof want,
                 "\"scan_avoids\":{\"available\":true,\"kind\":\"none\",\"total\":0,\"instance\":\"%llu\"",
                 (unsigned long long)g_state->state_instance);
    expect_status_has(want);
}

/*============================================================================
 * The status record's tunable (issue #743)
 *============================================================================*/

/* `tunable` follows dsd_opts_radio_can_tune(), beside `radio_input`: a live radio is tunable, an I/Q stream and an I/Q
   replay are radio inputs that are not, and audio input is neither. */
static void
test_status_tunable(void) {
    static const struct {
        int type;
        const char* dev;
        const char* want;
    } rows[] = {
        {AUDIO_IN_RTL, "rtl:0:851.0125M", "\"radio_input\":true,\"tunable\":true,"},
        {AUDIO_IN_RTL, "iqtcp:127.0.0.1:5000:240000:cs16", "\"radio_input\":true,\"tunable\":false,"},
        {AUDIO_IN_RTL, "iqudp:0.0.0.0:5000:48000:cu8", "\"radio_input\":true,\"tunable\":false,"},
        {AUDIO_IN_RTL, "iqreplay:capture.iq.json", "\"radio_input\":true,\"tunable\":false,"},
        {AUDIO_IN_PULSE, "pulse", "\"radio_input\":false,\"tunable\":false,"},
    };

    /* Audio input reads tunable false whether or not a rigctl peer is there to take a frequency: only radio_input set
       with tunable clear means the tuner commands are refused (docs/api.md). */

    const int type = g_opts->audio_in_type;
    char dev[sizeof g_opts->audio_in_dev];
    DSD_SNPRINTF(dev, sizeof dev, "%s", g_opts->audio_in_dev);
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        g_opts->audio_in_type = rows[i].type;
        DSD_SNPRINTF(g_opts->audio_in_dev, sizeof g_opts->audio_in_dev, "%s", rows[i].dev);
        feed_tick();
        expect_status_has(rows[i].want);
    }
    g_opts->use_rigctl = 1;
    feed_tick();
    expect_status_has("\"radio_input\":false,\"tunable\":false,");
    g_opts->use_rigctl = 0;
    g_opts->audio_in_type = type;
    DSD_SNPRINTF(g_opts->audio_in_dev, sizeof g_opts->audio_in_dev, "%s", dev);
}

/*============================================================================
 * Event rows carry the admitted position (issue #800)
 *============================================================================*/

/* A row as init_event_history() leaves it (the fields it sets apart from zero). */
static void
blank_row(Event_History* e) {
    DSD_MEMSET(e, 0, sizeof(*e));
    e->systype = -1;
    e->subtype = -1;
    e->severity = DSD_EVENT_SEVERITY_UNKNOWN;
    e->category = DSD_EVENT_CATEGORY_UNKNOWN;
    e->receive_identity.kind = (uint8_t)DSD_RECEIVE_NONE;
}

static void
expect_row_json(const Event_History* e, const char* want, int present) {
    static char json[65536];
    assert(dsd_api_feed_test_encode_event_row(e, json, sizeof json) == 0);
    dsd_json_node* root = NULL;
    char err[96];
    assert(dsd_json_parse(json, &root, err, sizeof err) == 0);
    dsd_json_free(root);
    if (present ? strstr(json, want) == NULL : strstr(json, want) != NULL) {
        DSD_FPRINTF(stderr, "row JSON %s '%s': %s\n", present ? "lacks" : "has", want, json);
        assert(0);
    }
}

/* The row's "pos" object joins the row JSON (lat/lon from the fixed point, seven decimals) and its fingerprint only
   while the row carries a position; a blank row's fingerprint is the one it always had. */
static void
test_event_row_position(void) {
    static Event_History e;
    blank_row(&e);
    /* Pinned: the blank row's fingerprint before rows carried positions. */
    assert(dsd_api_feed_test_row_fingerprint(&e) == UINT64_C(0xb5a24928254104bb));
    DSD_SNPRINTF(e.gps_s, sizeof e.gps_s, "%s", "GPS A");
    e.pos.scope.kind = (uint8_t)DSD_POS_SCOPE_P25; /* a stamped sender identity is not a position */
    e.pos.scope.a = 0xA1234U;
    expect_row_json(&e, "\"pos\"", 0);
    const uint64_t text_only = dsd_api_feed_test_row_fingerprint(&e);

    e.pos.valid = 1U;
    e.pos.lat_e7 = -225000000;
    e.pos.lon_e7 = 450000000;
    e.pos.err_m = 10U;
    e.pos.flags = (uint16_t)DSD_POS_FLAG_TARGET_GROUP;
    e.pos.rev = 7U;
    expect_row_json(&e,
                    "\"gps\":\"GPS A\",\"pos\":{\"lat\":-22.5000000,\"lon\":45.0000000,\"err_m\":10,\"flags\":4,"
                    "\"rev\":7},\"text\":",
                    1);
    const uint64_t first = dsd_api_feed_test_row_fingerprint(&e);
    assert(first != text_only);

    /* A position-only update (a newer report, same text and coordinates) is a change a client must see. */
    e.pos.rev = 8U;
    assert(dsd_api_feed_test_row_fingerprint(&e) != first);
    e.pos.rev = 7U;
    assert(dsd_api_feed_test_row_fingerprint(&e) == first);
    e.pos.lat_e7 = -225000001;
    assert(dsd_api_feed_test_row_fingerprint(&e) != first);

    /* The extremes and the smallest step keep every digit. */
    e.pos.lat_e7 = -5;
    e.pos.lon_e7 = -1800000000;
    expect_row_json(&e, "\"lat\":-0.0000005,\"lon\":-180.0000000,", 1);
    e.pos.lat_e7 = 900000000;
    e.pos.lon_e7 = 1800000000;
    expect_row_json(&e, "\"lat\":90.0000000,\"lon\":180.0000000,", 1);
}

/*============================================================================
 * The positions topic (issue #800)
 *============================================================================*/

/* Admit @p count verified reports from made-up radio 1234 to talkgroup 99, report k at 22.5 N + k * 1e-7, 45.0 E. */
static void
admit_reports(uint32_t first, uint32_t count) {
    for (uint32_t k = first; k < first + count; k++) {
        dsd_position_fix f;
        dsd_position_fix_init(&f);
        f.kind = DSD_POS_DMR_LRRP;
        f.protocol = DSD_SYNC_DMR_BS_DATA_POS;
        f.lat_e7 = 225000000 + (int32_t)k;
        f.lon_e7 = 450000000;
        f.err_m = 10U;
        f.heading_ddeg = 900U;
        f.source_id = 1234U;
        f.target_id = 99U;
        f.flags = DSD_POS_FLAG_TARGET_GROUP;
        assert(dsd_position_admit(NULL, g_state, &f, 1, 1, NULL) == DSD_POSITION_ADMITTED_KEPT);
    }
}

/* Publish once and return the position lines it broadcast, parsed and checked: each a whole "position" object with the
   stream as a decimal string, in pos_seq order from @p first_seq, of report pos_seq + @p report_offset. */
static size_t
publish_positions(uint64_t stream, uint64_t first_seq, uint32_t report_offset) {
    dsd_json_buf_reset(&g_position_lines);
    dsd_telemetry_publish_both_and_redraw(g_opts, g_state);
    size_t lines = 0U;
    char want_stream[32];
    DSD_SNPRINTF(want_stream, sizeof want_stream, "%llu", (unsigned long long)stream);
    const char* at = g_position_lines.data;
    const char* end = at != NULL ? at + g_position_lines.len : NULL;
    while (at != NULL && at < end) {
        const char* nl = memchr(at, '\n', (size_t)(end - at));
        assert(nl != NULL);
        char line[2048];
        assert((size_t)(nl - at) < sizeof line);
        DSD_MEMCPY(line, at, (size_t)(nl - at));
        line[nl - at] = '\0';
        dsd_json_node* root = NULL;
        char err[96];
        assert(dsd_json_parse(line, &root, err, sizeof err) == 0);
        const char* type = dsd_json_as_str(dsd_json_obj_get(root, "type"));
        assert(type != NULL && strcmp(type, "position") == 0);
        const dsd_json_node* pos_stream = dsd_json_obj_get(root, "pos_stream");
        assert(pos_stream != NULL && pos_stream->type == DSD_JSON_STRING);
        assert(strcmp(dsd_json_as_str(pos_stream), want_stream) == 0);
        const dsd_json_node* pos_seq = dsd_json_obj_get(root, "pos_seq");
        assert(pos_seq != NULL && pos_seq->type == DSD_JSON_NUMBER);
        uint64_t seq = 0U;
        assert(dsd_json_as_u64(pos_seq, &seq) == 0 && seq == first_seq + lines);
        uint64_t global_seq = 0U;
        assert(dsd_json_as_u64(dsd_json_obj_get(root, "seq"), &global_seq) == 0 && global_seq > 0U);
        char want[160];
        const uint64_t report = seq + report_offset;
        DSD_SNPRINTF(want, sizeof want, "\"lat\":22.50%05llu,\"lon\":45.0000000,\"alt_dm\":null,\"err_m\":10,",
                     (unsigned long long)report);
        if (strstr(line, want) == NULL) {
            DSD_FPRINTF(stderr, "position line lacks %s: %s\n", want, line);
            assert(0);
        }
        dsd_json_free(root);
        lines++;
        at = nl + 1;
    }
    return lines;
}

/* The topic follows the live feed at every publish: no backlog at a subscription, but a report kept between the
   subscription and the next publish is sent; a burst larger than one page arrives whole and in order; a new run is a
   new stream from its first record; an overrun between two publishes is a gap in pos_seq. */
static void
test_positions_topic(void) {
    dsd_json_buf_init(&g_position_lines);
    assert(dsd_position_feed_start(g_state) == 0);
    const uint64_t s1 = dsd_position_feed_stream_id(g_state);
    admit_reports(1U, 2U); /* nobody subscribed */
    assert(publish_positions(s1, 0U, 0U) == 0U);

    g_interest = DSD_API_TOPIC_POSITIONS; /* subscribed */
    admit_reports(3U, 1U);                /* kept before the next publish */
    assert(publish_positions(s1, 3U, 0U) == 1U);
    char line[2048];
    DSD_SNPRINTF(line, sizeof line, "%.*s", (int)g_position_lines.len, g_position_lines.data);
    assert(strstr(line, "\"heading_ddeg\":900,\"age_class\":null,\"sats\":null,\"fix_time\":null,\"source_id\":1234,"
                        "\"target_id\":99,\"source_text\":\"\",")
           != NULL);
    assert(strstr(line, "\"kind\":1,") != NULL && strstr(line, "\"flags\":4,") != NULL);
    assert(publish_positions(s1, 0U, 0U) == 0U); /* nothing new */

    admit_reports(4U, 40U); /* more than one page */
    assert(publish_positions(s1, 4U, 0U) == 40U);

    /* A new run: its stream, from seq 1. */
    assert(dsd_position_feed_start(g_state) == 0);
    const uint64_t s2 = dsd_position_feed_stream_id(g_state);
    assert(s2 != s1);
    admit_reports(101U, 2U);
    assert(publish_positions(s2, 1U, 100U) == 2U);

    /* Unsubscribed: nothing, and the report kept meanwhile is not sent after a new subscription. */
    g_interest = 0U;
    admit_reports(103U, 1U);
    assert(publish_positions(s2, 0U, 0U) == 0U);
    g_interest = DSD_API_TOPIC_POSITIONS;
    assert(publish_positions(s2, 0U, 0U) == 0U);

    /* The ring overran between two publishes: seqs 4..1033 kept, 10..1033 still held, a gap before 10. */
    admit_reports(104U, 1030U);
    assert(publish_positions(s2, 10U, 100U) == (size_t)DSD_POSITION_FEED_CAPACITY);

    g_interest = 0U;
    dsd_json_buf_free(&g_position_lines);
}

/* A line that cannot be built or appended stops the batch there: the cursor stays before it, so the next publish sends
   it and the rest, within a page and across pages, and no record still in the ring is skipped. */
static void
test_positions_failed_line(void) {
    dsd_json_buf_init(&g_position_lines);
    assert(dsd_position_feed_start(g_state) == 0);
    const uint64_t s = dsd_position_feed_stream_id(g_state);
    g_interest = DSD_API_TOPIC_POSITIONS;
    admit_reports(1U, 5U);
    dsd_api_feed_test_fail_position_line(3);
    assert(publish_positions(s, 1U, 0U) == 2U);
    assert(publish_positions(s, 3U, 0U) == 3U);

    admit_reports(6U, 40U); /* more than one page */
    dsd_api_feed_test_fail_position_line(35);
    assert(publish_positions(s, 6U, 0U) == 34U);
    assert(publish_positions(s, 40U, 0U) == 6U);
    assert(publish_positions(s, 0U, 0U) == 0U);

    g_interest = 0U;
    dsd_json_buf_free(&g_position_lines);
}

int
main(void) {
    g_opts = (dsd_opts*)calloc(1U, sizeof(*g_opts));
    g_state = (dsd_state*)calloc(1U, sizeof(*g_state));
    assert(g_opts != NULL && g_state != NULL);
    initOpts(g_opts);
    initState(g_state);
    dsd_app_frontend_runtime_start(g_opts, g_state);
    dsd_api_feed_start();

    test_reader_keeps_pinned_list();
    test_lcn_failed_collection();
    test_cache_never_stale();
    test_status_tunable();
    test_event_row_position();
    test_positions_topic();
    test_positions_failed_line();

    dsd_api_feed_stop();
    dsd_app_frontend_runtime_stop();
    freeState(g_state);
    free(g_state);
    free(g_opts);
    printf("api feed tests passed\n");
    return 0;
}
