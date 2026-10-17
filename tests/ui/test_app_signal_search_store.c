// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Signal search store write failures: history and permanent avoids are separate files, and each stays owed, with the
   reported error, until its own write succeeds. A directory stands in for unavailable storage.
   Journal versions: a version 1 (DSS1) journal loads with no checked site, slot or encryption and is rewritten as
   version 2 (DSS2), which keeps them; damaged or unknown records stop the load; the history revision moves only when a
   record's persisted content changes. */

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
#include <string.h>
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

static void
publish_hit(const dsd_signal_search_hit* hit) {
    dsd_signal_search_status s;
    DSD_MEMSET(&s, 0, sizeof(s));
    s.session_id = hit->session_id;
    s.revision = hit->hit_id;
    s.phase = DSD_SEARCH_LISTENING;
    s.hit = *hit;
    dsd_app_signal_search_publish(&s, &s.hit, NULL);
}

static void
detailed_hit(dsd_signal_search_hit* h, uint64_t id) {
    DSD_MEMSET(h, 0, sizeof(*h));
    h->session_id = 31;
    h->hit_id = id;
    h->frequency_hz = 851000000U + (uint32_t)id * 12500U;
    h->bandwidth_hz = 12500;
    h->confirmed = 1;
    DSD_SNPRINTF(h->protocol, sizeof(h->protocol), "%s", "P25 Phase 1");
    DSD_SNPRINTF(h->site, sizeof(h->site), "%s", "WACN BEE00 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 Site 2");
    h->slot = 2;
    DSD_SNPRINTF(h->crypt, sizeof(h->crypt), "%s", "ALG 84 \xC2\xB7 KID 0001");
    DSD_SNPRINTF(h->target_text, sizeof(h->target_text), "%s", "Dispatch");
}

/* A version 1 journal record, written field by field as version 1 laid it out (all little endian). */
typedef struct {
    unsigned char bytes[2048];
    size_t n;
} record_buf;

static void
put_le(record_buf* r, uint64_t value, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        assert(r->n < sizeof(r->bytes));
        r->bytes[r->n++] = (unsigned char)(value >> (8 * i));
    }
}

static void
put_field(record_buf* r, const char* text, size_t size) {
    assert(r->n + size <= sizeof(r->bytes));
    DSD_MEMSET(r->bytes + r->n, 0, size);
    DSD_MEMCPY(r->bytes + r->n, text, strlen(text));
    r->n += size;
}

static uint32_t
fnv1a(const unsigned char* b, size_t n) {
    uint32_t sum = 2166136261U;
    for (size_t i = 0; i < n; ++i) {
        sum = (sum ^ b[i]) * 16777619U;
    }
    return sum;
}

static void
v1_body(record_buf* r, uint64_t id) {
    const dsd_signal_search_hit h = {0};
    r->n = 0;
    put_le(r, 31, 8);                                 // session
    put_le(r, id, 8);                                 // hit
    put_le(r, 1000U * id, 8);                         // started
    put_le(r, 1000U * id + 500U, 8);                  // ended
    put_le(r, 851000000U + (uint32_t)id * 12500U, 4); // frequency
    put_le(r, 12500, 4);                              // bandwidth
    put_le(r, 12500, 4);                              // step numerator
    put_le(r, 1, 4);                                  // step denominator
    put_le(r, 0, 4);                                  // range index
    put_le(r, (uint32_t)(int32_t)-41500, 4);          // power, mdBFS
    put_le(r, (uint32_t)(int32_t)-90000, 4);          // noise, mdBFS
    put_le(r, 1, 1);                                  // confirmed
    put_le(r, 1, 1);                                  // ended
    put_le(r, 0, 1);                                  // am
    put_le(r, 0, 1);                                  // unresolved
    put_le(r, 280, 4);                                // gain, tenth dB
    put_le(r, 0, 1);                                  // gain auto
    put_le(r, 1001, 4);                               // target
    put_le(r, 2002, 4);                               // source
    put_field(r, "Old range", sizeof(h.range_name));
    put_field(r, "DMR", sizeof(h.protocol));
    put_field(r, "CC 1", sizeof(h.access));
    put_field(r, "", sizeof(h.tone));
    put_field(r, "Fire", sizeof(h.target_text));
    put_field(r, "Engine 7", sizeof(h.source_text));
    put_field(r, "rtl:0", sizeof(h.input_spec));
    put_le(r, 3, 4); // ppm
    put_le(r, 1, 1); // bias tee
    put_field(r, "", sizeof(h.airspy.serial));
    put_le(r, 0, 4); // airspy sample rate
    for (int i = 0; i < 9; ++i) {
        put_le(r, 0, 4);
    }
}

static void
write_record(FILE* f, uint32_t magic, const record_buf* body) {
    record_buf header = {{0}, 0};
    put_le(&header, magic, 4);
    put_le(&header, body->n, 4);
    put_le(&header, fnv1a(body->bytes, body->n), 4);
    assert(fwrite(header.bytes, 1, header.n, f) == header.n);
    assert(fwrite(body->bytes, 1, body->n, f) == body->n);
}

static void
write_v1_journal(const char* path, int count) {
    FILE* f = dsd_fopen_private(path, "wb");
    assert(f);
    record_buf body;
    for (int i = 1; i <= count; ++i) {
        v1_body(&body, (uint64_t)i);
        write_record(f, 0x31535344U, &body);
    }
    assert(fclose(f) == 0);
}

/* Every record in @p path is version 2 and @p size bytes long; returns how many there are. */
static int
count_v2_records(const char* path, size_t size) {
    FILE* f = dsd_fopen_private(path, "rb");
    assert(f);
    int count = 0;
    unsigned char header[12];
    static unsigned char body[4096];
    size_t got = 0;
    while ((got = fread(header, 1, sizeof(header), f)) == sizeof(header)) {
        uint32_t magic =
            (uint32_t)header[0] | (uint32_t)header[1] << 8 | (uint32_t)header[2] << 16 | (uint32_t)header[3] << 24;
        uint32_t n =
            (uint32_t)header[4] | (uint32_t)header[5] << 8 | (uint32_t)header[6] << 16 | (uint32_t)header[7] << 24;
        assert(magic == 0x32535344U && n == size && n <= sizeof(body));
        assert(fread(body, 1, n, f) == n);
        ++count;
    }
    assert(got == 0);
    (void)fclose(f);
    return count;
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
    assert(dsd_app_signal_search_permanent_avoid(462575000) == DSD_APP_SEARCH_AVOID_ADDED);
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
    assert(dsd_app_signal_search_permanent_avoid(462600000) == DSD_APP_SEARCH_AVOID_ADDED);
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
    assert(dsd_app_signal_search_permanent_avoid(462625000) == DSD_APP_SEARCH_AVOID_ADDED);
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
    assert(dsd_app_signal_search_permanent_avoid(462650000) == DSD_APP_SEARCH_AVOID_ADDED);
    dsd_app_signal_search_store_flush();
    assert(store_error());
    assert(dsd_mkdir(closing.dir, 0700) == 0);
    assert(reopen(closing.journal, hits, 8) == 1 && hits[0].hit_id == 6);
    assert(dsd_app_signal_search_avoids_get(avoids, 4) == 1 && avoids[0] == 462650000);
    assert(file_size(closing.journal) == record);
    dsd_app_signal_search_store_close();

    // Each permanent avoid says what it did: added, already there, no room, or no channel at all.
    store_paths limits;
    paths_init(&limits, root, "limits");
    assert(dsd_mkdir(limits.dir, 0700) == 0);
    assert(dsd_app_signal_search_store_open(limits.journal) == 0);
    const uint64_t first_revision = dsd_app_signal_search_avoids_revision();
    assert(dsd_app_signal_search_permanent_avoid(450000000) == DSD_APP_SEARCH_AVOID_ADDED);
    const uint64_t added_revision = dsd_app_signal_search_avoids_revision();
    assert(added_revision != first_revision);
    assert(dsd_app_signal_search_permanent_avoid(450000000) == DSD_APP_SEARCH_AVOID_DUPLICATE);
    assert(dsd_app_signal_search_permanent_avoid(0) == DSD_APP_SEARCH_AVOID_INVALID);
    assert(dsd_app_signal_search_avoids_revision() == added_revision);
    for (uint32_t i = 1; i < DSD_SEARCH_MAX_AVOIDS; ++i) {
        assert(dsd_app_signal_search_permanent_avoid(450000000U + i * 12500U) == DSD_APP_SEARCH_AVOID_ADDED);
    }
    const uint64_t full_revision = dsd_app_signal_search_avoids_revision();
    assert(dsd_app_signal_search_permanent_avoid(470000000) == DSD_APP_SEARCH_AVOID_FULL);
    // A full list still recognises what it holds, and a refusal leaves it as it was.
    assert(dsd_app_signal_search_permanent_avoid(450012500) == DSD_APP_SEARCH_AVOID_DUPLICATE);
    assert(dsd_app_signal_search_avoids_revision() == full_revision);
    static uint32_t all[DSD_SEARCH_MAX_AVOIDS + 1];
    assert(dsd_app_signal_search_avoids_get(all, DSD_SEARCH_MAX_AVOIDS + 1) == DSD_SEARCH_MAX_AVOIDS);
    // Lifting one makes room again.
    assert(dsd_app_signal_search_avoid_remove(450000000) == 0);
    assert(dsd_app_signal_search_permanent_avoid(470000000) == DSD_APP_SEARCH_AVOID_ADDED);
    dsd_app_signal_search_store_flush();
    assert(!store_error());
    dsd_app_signal_search_store_close();

    // A version 1 journal loads every row, with no checked site, slot or encryption, and changes on disk only when
    // the store writes next: then the compaction rewrites all of it as version 2.
    store_paths migrate;
    paths_init(&migrate, root, "migrate");
    assert(dsd_mkdir(migrate.dir, 0700) == 0);
    write_v1_journal(migrate.journal, 3);
    const long v1_journal = file_size(migrate.journal);
    assert(dsd_app_signal_search_store_open(migrate.journal) == 0);
    assert(!store_error());
    assert(dsd_app_signal_search_history_get(hits, 8) == 3);
    for (int i = 0; i < 3; ++i) {
        const dsd_signal_search_hit* h = &hits[i];
        assert(h->session_id == 31 && h->hit_id == (uint64_t)(3 - i) && h->ended);
        assert(h->frequency_hz == 851000000U + (uint32_t)(3 - i) * 12500U && h->target == 1001 && h->source == 2002);
        assert(strcmp(h->protocol, "DMR") == 0 && strcmp(h->access, "CC 1") == 0);
        assert(strcmp(h->target_text, "Fire") == 0 && strcmp(h->source_text, "Engine 7") == 0);
        assert(strcmp(h->input_spec, "rtl:0") == 0 && h->ppm == 3 && h->bias_tee == 1 && h->gain_tenth_db == 280);
        assert(h->site[0] == '\0' && h->slot == 0 && h->crypt[0] == '\0');
    }
    assert(file_size(migrate.journal) == v1_journal);
    dsd_signal_search_hit detailed;
    detailed_hit(&detailed, 4);
    publish_hit(&detailed);
    dsd_app_signal_search_store_flush();
    assert(!store_error());
    assert(count_v2_records(migrate.journal, (size_t)record - 12U) == 4);
    assert(file_size(migrate.journal) == 4 * record);
    assert(reopen(migrate.journal, hits, 8) == 4);
    assert(hits[0].hit_id == 4 && hits[3].hit_id == 1 && strcmp(hits[3].target_text, "Fire") == 0);
    assert(hits[3].site[0] == '\0' && hits[3].slot == 0 && hits[3].crypt[0] == '\0');
    dsd_app_signal_search_store_close();

    // Version 2 keeps the checked site, slot and encryption through a reload, and the history revision moves only
    // when one of them, or anything else persisted, changes.
    store_paths details;
    paths_init(&details, root, "details");
    assert(dsd_mkdir(details.dir, 0700) == 0);
    assert(dsd_app_signal_search_store_open(details.journal) == 0);
    detailed_hit(&detailed, 9);
    publish_hit(&detailed);
    uint64_t revision = dsd_app_signal_search_history_revision();
    publish_hit(&detailed);
    assert(dsd_app_signal_search_history_revision() == revision);
    // Bytes behind a shorter text are not part of the record: the same text republished changes nothing.
    dsd_signal_search_hit tail = detailed;
    DSD_SNPRINTF(tail.crypt, sizeof(tail.crypt), "%s", "ALG 84 \xC2\xB7 KID 0001 \xC2\xB7 leftover");
    tail.crypt[strlen(detailed.crypt)] = '\0';
    assert(strcmp(tail.crypt, detailed.crypt) == 0);
    DSD_SNPRINTF(tail.target_text, sizeof(tail.target_text), "%s", "Dispatch and a longer name before it");
    tail.target_text[strlen(detailed.target_text)] = '\0';
    assert(strcmp(tail.target_text, detailed.target_text) == 0);
    publish_hit(&tail);
    assert(dsd_app_signal_search_history_revision() == revision);
    dsd_signal_search_hit changed = detailed;
    DSD_SNPRINTF(changed.site, sizeof(changed.site), "%s",
                 "WACN BEE00 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 Site 3");
    publish_hit(&changed);
    assert(dsd_app_signal_search_history_revision() > revision);
    revision = dsd_app_signal_search_history_revision();
    assert(dsd_app_signal_search_history_get(hits, 8) == 1 && strstr(hits[0].site, "Site 3") != NULL);
    publish_hit(&changed);
    assert(dsd_app_signal_search_history_revision() == revision);
    changed.slot = 1;
    publish_hit(&changed);
    assert(dsd_app_signal_search_history_revision() > revision);
    revision = dsd_app_signal_search_history_revision();
    assert(dsd_app_signal_search_history_get(hits, 8) == 1 && hits[0].slot == 1);
    publish_hit(&changed);
    assert(dsd_app_signal_search_history_revision() == revision);
    DSD_SNPRINTF(changed.crypt, sizeof(changed.crypt), "%s", "Encrypted");
    publish_hit(&changed);
    assert(dsd_app_signal_search_history_revision() > revision);
    revision = dsd_app_signal_search_history_revision();
    assert(dsd_app_signal_search_history_get(hits, 8) == 1 && strcmp(hits[0].crypt, "Encrypted") == 0);
    publish_hit(&changed);
    assert(dsd_app_signal_search_history_revision() == revision);
    dsd_app_signal_search_store_flush();
    assert(!store_error());
    assert(reopen(details.journal, hits, 8) == 1);
    assert(strcmp(hits[0].site, changed.site) == 0 && hits[0].slot == 1 && strcmp(hits[0].crypt, "Encrypted") == 0);
    assert(strcmp(hits[0].target_text, "Dispatch") == 0 && strcmp(hits[0].protocol, "P25 Phase 1") == 0);
    dsd_app_signal_search_store_close();

    // What a journal says is not trusted: an out-of-range slot loads as none and a text without its NUL is cut at
    // its last byte. A record of an unknown version, or whose size does not match its version, stops the load at
    // that record with an error, keeping what came before it.
    store_paths hostile;
    paths_init(&hostile, root, "hostile");
    assert(dsd_mkdir(hostile.dir, 0700) == 0);
    {
        record_buf body;
        v1_body(&body, 1);
        const size_t v1_n = body.n;
        const dsd_signal_search_hit h = {0};
        char site[sizeof(h.site)];
        DSD_MEMSET(site, 'S', sizeof(site));
        DSD_MEMCPY(body.bytes + body.n, site, sizeof(site));
        body.n += sizeof(site);
        put_le(&body, 7, 1);
        char crypt[sizeof(h.crypt)];
        DSD_MEMSET(crypt, 'C', sizeof(crypt));
        DSD_MEMCPY(body.bytes + body.n, crypt, sizeof(crypt));
        body.n += sizeof(crypt);
        assert(body.n == (size_t)record - 12U);
        FILE* f = dsd_fopen_private(hostile.journal, "wb");
        assert(f);
        write_record(f, 0x32535344U, &body);
        v1_body(&body, 2);
        write_record(f, 0x33535344U, &body); // DSS3: not a version this store knows
        assert(fclose(f) == 0);
        assert(dsd_app_signal_search_store_open(hostile.journal) == 0);
        assert(store_error());
        assert(dsd_app_signal_search_history_get(hits, 8) == 1 && hits[0].hit_id == 1 && hits[0].slot == 0);
        assert(strlen(hits[0].site) == sizeof(h.site) - 1 && strlen(hits[0].crypt) == sizeof(h.crypt) - 1);
        dsd_app_signal_search_store_close();
        // A version 2 magic in front of a version 1 body is refused, not read past its end.
        v1_body(&body, 3);
        assert(body.n == v1_n);
        f = dsd_fopen_private(hostile.journal, "wb");
        assert(f);
        write_record(f, 0x32535344U, &body);
        assert(fclose(f) == 0);
        assert(dsd_app_signal_search_store_open(hostile.journal) == 0);
        assert(store_error());
        assert(dsd_app_signal_search_history_get(hits, 8) == 0);
        dsd_app_signal_search_store_close();
    }

    // Failed replacements removed their temporary files.
    paths_remove(&migrate);
    paths_remove(&details);
    paths_remove(&hostile);
    paths_remove(&limits);
    paths_remove(&clean);
    paths_remove(&late);
    paths_remove(&blocked);
    paths_remove(&stuck);
    paths_remove(&closing);
    static const char* const none[] = {NULL};
    assert(dsd_test_remove_temp_dir(root, none) == 0);
    return 0;
}
