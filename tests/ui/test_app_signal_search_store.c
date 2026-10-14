// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Signal search store write failures: history and permanent avoids are separate files, and each stays owed, with the
   reported error, until its own write succeeds. A directory stands in for unavailable storage. */

#include <assert.h>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "test_support.h"

typedef struct {
    char dir[DSD_TEST_PATH_MAX];
    char journal[DSD_TEST_PATH_MAX];
    char avoids[DSD_TEST_PATH_MAX];
} store_paths;

static void
paths_init(store_paths* p, const char* root, const char* leaf) {
    assert(dsd_test_path_join(p->dir, sizeof(p->dir), root, leaf) == 0);
    assert(dsd_test_path_join(p->journal, sizeof(p->journal), p->dir, "history") == 0);
    int n = DSD_SNPRINTF(p->avoids, sizeof(p->avoids), "%s.avoids.json", p->journal);
    assert(n > 0 && (size_t)n < sizeof(p->avoids));
}

static void
paths_remove(const store_paths* p) {
    static const char* const files[] = {"history", "history.avoids.json", NULL};
    assert(dsd_test_remove_temp_dir(p->dir, files) == 0);
}

static void
publish(uint64_t id) {
    dsd_signal_search_status s;
    DSD_MEMSET(&s, 0, sizeof(s));
    s.session_id = 23;
    s.revision = id;
    s.phase = DSD_SEARCH_LISTENING;
    s.hit.session_id = 23;
    s.hit.hit_id = id;
    s.hit.frequency_hz = 462550000U + (uint32_t)id * 25000U;
    s.hit.bandwidth_hz = 12500;
    s.hit.started_ms = 1000U * id;
    dsd_app_signal_search_publish(&s, &s.hit, NULL);
}

static int
store_error(void) {
    char error[256];
    dsd_app_signal_search_status_get(NULL, error, sizeof(error));
    return error[0] != '\0';
}

/* Wait for the worker without a flush, which would itself retry a failed write. */
static int
wait_error_cleared(void) {
    const uint64_t deadline = dsd_realtime_mono_ms() + 10000U;
    while (store_error()) {
        if (dsd_realtime_mono_ms() > deadline) {
            return 0;
        }
        dsd_sleep_ms(1);
    }
    return 1;
}

static long
file_size(const char* path) {
    FILE* f = dsd_fopen_private(path, "rb");
    if (!f) {
        return -1;
    }
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        size = ftell(f);
    }
    (void)fclose(f);
    return size;
}

/* Reload from disk: only what was written survives. */
static size_t
reopen(const char* journal, dsd_signal_search_hit* hits, size_t capacity) {
    dsd_app_signal_search_store_close();
    assert(dsd_app_signal_search_store_open(journal) == 0);
    assert(!store_error());
    return dsd_app_signal_search_history_get(hits, capacity);
}

int
main(void) {
    char root[DSD_TEST_PATH_MAX];
    assert(dsd_test_mkdtemp(root, sizeof(root), "dsdneo_search_store") != NULL);
    dsd_signal_search_hit hits[8];
    uint32_t avoids[4];

    // A cleanly saved detection gives the size of one journal record.
    store_paths clean;
    paths_init(&clean, root, "clean");
    assert(dsd_mkdir(clean.dir, 0700) == 0);
    assert(dsd_app_signal_search_store_open(clean.journal) == 0);
    publish(1);
    dsd_app_signal_search_store_flush();
    assert(!store_error());
    const long record = file_size(clean.journal);
    assert(record > 0);
    dsd_app_signal_search_store_close();

    // History written while the journal directory is missing must survive a later save that only changes an avoid.
    store_paths late;
    paths_init(&late, root, "late");
    assert(dsd_app_signal_search_store_open(late.journal) == 0);
    publish(2);
    dsd_app_signal_search_store_flush();
    assert(store_error());
    assert(dsd_mkdir(late.dir, 0700) == 0);
    dsd_app_signal_search_permanent_avoid(462575000);
    assert(wait_error_cleared());
    assert(file_size(late.journal) == record);
    assert(file_size(late.avoids) > 0);
    // The retry rewrote the journal; later detections append to it without repeating it.
    publish(3);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && file_size(late.journal) == 2 * record);
    assert(reopen(late.journal, hits, 8) == 2 && hits[0].hit_id == 3 && hits[1].hit_id == 2);
    assert(dsd_app_signal_search_avoids_get(avoids, 4) == 1 && avoids[0] == 462575000);
    dsd_app_signal_search_store_close();

    // A journal that still cannot be written stays reported after the avoid beside it saves. A directory in its place
    // refuses both an append and the compacted replacement.
    store_paths blocked;
    paths_init(&blocked, root, "blocked");
    assert(dsd_mkdir(blocked.dir, 0700) == 0);
    assert(dsd_mkdir(blocked.journal, 0700) == 0);
    assert(dsd_app_signal_search_store_open(blocked.journal) == 0);
    publish(4);
    dsd_app_signal_search_store_flush();
    assert(store_error());
    dsd_app_signal_search_permanent_avoid(462600000);
    dsd_app_signal_search_store_flush();
    assert(store_error());
    assert(file_size(blocked.avoids) > 0);
    assert(dsd_test_rmdir(blocked.journal) == 0);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && file_size(blocked.journal) == record);
    assert(reopen(blocked.journal, hits, 8) == 1 && hits[0].hit_id == 4);
    assert(dsd_app_signal_search_avoids_get(avoids, 4) == 1 && avoids[0] == 462600000);
    dsd_app_signal_search_store_close();

    // The reverse: avoids that cannot be replaced stay reported after history saves, and their retry leaves the
    // journal alone. Block them only after opening, which would otherwise report an unreadable avoid store.
    store_paths stuck;
    paths_init(&stuck, root, "stuck");
    assert(dsd_mkdir(stuck.dir, 0700) == 0);
    assert(dsd_app_signal_search_store_open(stuck.journal) == 0);
    assert(dsd_mkdir(stuck.avoids, 0700) == 0);
    dsd_app_signal_search_permanent_avoid(462625000);
    dsd_app_signal_search_store_flush();
    assert(store_error());
    publish(5);
    dsd_app_signal_search_store_flush();
    assert(store_error() && file_size(stuck.journal) == record);
    assert(dsd_test_rmdir(stuck.avoids) == 0);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && file_size(stuck.journal) == record);
    assert(reopen(stuck.journal, hits, 8) == 1 && hits[0].hit_id == 5);
    assert(dsd_app_signal_search_avoids_get(avoids, 4) == 1 && avoids[0] == 462625000);
    dsd_app_signal_search_store_close();

    // Closing makes one last attempt at both files after storage recovers.
    store_paths closing;
    paths_init(&closing, root, "closing");
    assert(dsd_app_signal_search_store_open(closing.journal) == 0);
    publish(6);
    dsd_app_signal_search_permanent_avoid(462650000);
    dsd_app_signal_search_store_flush();
    assert(store_error());
    assert(dsd_mkdir(closing.dir, 0700) == 0);
    assert(reopen(closing.journal, hits, 8) == 1 && hits[0].hit_id == 6);
    assert(dsd_app_signal_search_avoids_get(avoids, 4) == 1 && avoids[0] == 462650000);
    assert(file_size(closing.journal) == record);
    dsd_app_signal_search_store_close();

    // Failed replacements removed their temporary files.
    paths_remove(&clean);
    paths_remove(&late);
    paths_remove(&blocked);
    paths_remove(&stuck);
    paths_remove(&closing);
    static const char* const none[] = {NULL};
    assert(dsd_test_remove_temp_dir(root, none) == 0);
    return 0;
}
