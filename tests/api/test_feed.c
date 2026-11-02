// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The API feed's scan avoid copy (issue #672) through its test hooks, against stand-ins for the server: a page reader
 * held between pinning the list and reading it while the decode thread swaps in a new list, and a failed collection of
 * a -Y list followed by a retry at the same key, and the cached records and copy dropped when no client is connected
 * and replaced at a decoder restart.
 */

#include <assert.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
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
 * says otherwise), nobody subscribed
 *============================================================================*/

static int g_clients = 1;

int
dsd_api_client_count(void) {
    return g_clients;
}

int
dsd_api_topic_interest(uint32_t topic) {
    (void)topic;
    return 0;
}

void
dsd_api_broadcast(const char* line, size_t len, uint32_t topic) {
    (void)line;
    (void)len;
    (void)topic;
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

    dsd_api_feed_stop();
    dsd_app_frontend_runtime_stop();
    freeState(g_state);
    free(g_state);
    free(g_opts);
    printf("api feed tests passed\n");
    return 0;
}
