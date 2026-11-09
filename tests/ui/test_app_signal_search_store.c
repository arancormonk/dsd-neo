// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Signal search store write failures: history and permanent avoids are separate files, and each stays owed, with the
   reported error, until its own write succeeds. A directory stands in for unavailable storage.
   Journal versions: the store is opened with the version 1 (DSS1) journal's path and keeps version 2 (DSS2) records,
   which add the checked site, slot and encryption, in its own file beside it (`<path>.v2`). With no version 2 journal
   yet, the version 1 rows load with those details empty and the first write creates the version 2 journal from them;
   the version 1 journal stays byte for byte as an earlier release left it, and once a version 2 journal exists it
   alone is read. Damaged or unknown records stop the load; the history revision moves only when a record's persisted
   content changes.
   Unreadable files (issue #828): a version 2 journal or avoid list that did not load whole is renamed to
   `<name>.unreadable-N` before its first replacement and is otherwise left byte for byte; a kept version 2 journal
   keeps the version 1 rows out; the version 1 journal is reported, never moved. Open and read errors come through the
   store's DSD_NEO_TEST_HOOKS seams. */

#include <assert.h>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search.h>
#include <dsd-neo/runtime/signal_search_avoids.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "signal_search_store_test_hooks.h"
#include "test_support.h"
#if !DSD_PLATFORM_WIN_NATIVE
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

typedef struct {
    char dir[DSD_TEST_PATH_MAX];
    char journal[DSD_TEST_PATH_MAX]; // the path the store opens: the version 1 journal
    char v2[DSD_TEST_PATH_MAX];      // the version 2 journal the store writes
    char avoids[DSD_TEST_PATH_MAX];
} store_paths;

static void
paths_init(store_paths* p, const char* root, const char* leaf) {
    assert(dsd_test_path_join(p->dir, sizeof(p->dir), root, leaf) == 0);
    assert(dsd_test_path_join(p->journal, sizeof(p->journal), p->dir, "history") == 0);
    int n = DSD_SNPRINTF(p->v2, sizeof(p->v2), "%s.v2", p->journal);
    assert(n > 0 && (size_t)n < sizeof(p->v2));
    n = DSD_SNPRINTF(p->avoids, sizeof(p->avoids), "%s.avoids.json", p->journal);
    assert(n > 0 && (size_t)n < sizeof(p->avoids));
}

static void
paths_remove(const store_paths* p) {
    static const char* const files[] = {"history", "history.v2", "history.avoids.json", NULL};
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

/* A status with no detection: session @p session in @p phase. */
static void
publish_phase(uint64_t session, int phase, uint64_t revision) {
    dsd_signal_search_status s;
    DSD_MEMSET(&s, 0, sizeof(s));
    s.session_id = session;
    s.revision = revision;
    s.phase = phase;
    s.owned = phase != DSD_SEARCH_STOPPED;
    dsd_app_signal_search_publish(&s, NULL, NULL);
}

/* The runtime's permanent avoid list, the one the running search reads; returns its length. */
static size_t
runtime_permanent(uint32_t* out, size_t cap) {
    uint64_t revision = 0;
    return dsd_signal_search_avoids_permanent_snapshot(out, cap, &revision);
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

/* A record whose checksum is off by @p sum_flip (0 for a sound one). */
static void
write_record_sum(FILE* f, uint32_t magic, const record_buf* body, uint32_t sum_flip) {
    record_buf header = {{0}, 0};
    put_le(&header, magic, 4);
    put_le(&header, body->n, 4);
    put_le(&header, fnv1a(body->bytes, body->n) ^ sum_flip, 4);
    assert(fwrite(header.bytes, 1, header.n, f) == header.n);
    assert(fwrite(body->bytes, 1, body->n, f) == body->n);
}

static void
write_record(FILE* f, uint32_t magic, const record_buf* body) {
    write_record_sum(f, magic, body, 0);
}

/* A version 2 body as the store writes one: version 1's, then site, slot and encryption. */
static void
detailed_v2_body(record_buf* r, uint64_t id) {
    const dsd_signal_search_hit h = {0};
    v1_body(r, id);
    put_field(r, "Site 1", sizeof(h.site));
    put_le(r, 1, 1);
    put_field(r, "Encrypted", sizeof(h.crypt));
}

static size_t
read_file(const char* path, unsigned char* buf, size_t capacity) {
    FILE* f = dsd_fopen_private(path, "rb");
    assert(f);
    size_t n = fread(buf, 1, capacity, f);
    assert(n < capacity && !ferror(f));
    (void)fclose(f);
    return n;
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

/* ---- Unreadable files (issue #828) ---- */

/* One version 2 record with its header, measured by the first case. */
static long g_record;

static void
write_bytes(const char* path, const void* data, size_t n) {
    FILE* f = dsd_fopen_private(path, "wb");
    assert(f);
    assert(fwrite(data, 1, n, f) == n);
    assert(fclose(f) == 0);
}

/* @p path is a regular file holding exactly the @p n bytes of @p data. */
static int
file_is(const char* path, const void* data, size_t n) {
    static unsigned char buf[16384];
    FILE* f = dsd_fopen_existing_regular_file(path, "rb");
    if (!f) {
        return 0;
    }
    const size_t got = fread(buf, 1, sizeof(buf), f);
    (void)fclose(f);
    return got == n && memcmp(buf, data, n) == 0;
}

static int
exists(const char* path) {
    return dsd_path_exists_nofollow(path) == 1;
}

static void
kept_path(char* out, size_t cap, const char* file, int n) {
    const int w = DSD_SNPRINTF(out, cap, "%s.unreadable-%d", file, n);
    assert(w > 0 && (size_t)w < cap);
}

/* @p which loaded as @p want, kept as the base name @p kept (NULL: none). */
static int
state_is(int which, dsd_app_search_file_load want, const char* kept) {
    dsd_app_search_file_load state = DSD_APP_SEARCH_FILE_OK;
    char name[256] = "x";
    if (dsd_app_signal_search_store_file_state(which, &state, NULL, name, sizeof(name)) != 0) {
        return 0;
    }
    return state == want && strcmp(name, kept ? kept : "") == 0;
}

static int
write_owed(int which) {
    int failed = -1;
    assert(dsd_app_signal_search_store_file_state(which, NULL, &failed, NULL, 0) == 0);
    return failed;
}

/* @p count version 2 records, hit ids 1..count; record @p damaged (1-based, 0 for none) fails its checksum. */
static void
write_v2_journal(const char* path, int count, int damaged) {
    FILE* f = dsd_fopen_private(path, "wb");
    assert(f);
    record_buf body;
    for (int i = 1; i <= count; ++i) {
        detailed_v2_body(&body, (uint64_t)i);
        write_record_sum(f, 0x32535344U, &body, i == damaged ? 1U : 0U);
    }
    assert(fclose(f) == 0);
}

static void
store_dir(store_paths* p, const char* root, const char* leaf) {
    paths_init(p, root, leaf);
    assert(dsd_mkdir(p->dir, 0700) == 0);
}

typedef struct {
    int open_err;    // the open fails with this errno, 0 for none
    long read_after; // reads fail after this many bytes, -1 for none
} load_fault;

static const load_fault no_fault = {0, -1};

static void
open_with(const store_paths* p, int which, load_fault fault) {
    dsd_app_signal_search_store_test_fail_open(which, fault.open_err);
    dsd_app_signal_search_store_test_fail_read(which, fault.read_after);
    assert(dsd_app_signal_search_store_open(p->journal) == 0);
    dsd_app_signal_search_store_test_fail_open(which, 0);
    dsd_app_signal_search_store_test_fail_read(which, -1);
}

/* An avoid list holding @p content that loads as unreadable (by itself or through @p fault) is left alone by a launch
   that writes nothing, then kept byte for byte as "….unreadable-1" by the first save, which writes only the new list;
   the next open reads that list. */
static void
expect_avoids_kept(const char* root, const char* leaf, const void* content, size_t n, load_fault fault) {
    store_paths p;
    store_dir(&p, root, leaf);
    write_bytes(p.avoids, content, n);
    uint32_t avoids[4], runtime[4];
    open_with(&p, DSD_APP_SEARCH_STORE_AVOIDS, fault);
    assert(!store_error()); // only a write failure is the store's message
    assert(state_is(DSD_APP_SEARCH_STORE_AVOIDS, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
    assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_MISSING, NULL));
    assert(dsd_app_signal_search_avoids_get(avoids, 4) == 0 && runtime_permanent(runtime, 4) == 0);
    dsd_app_signal_search_store_close();
    assert(file_is(p.avoids, content, n));
    open_with(&p, DSD_APP_SEARCH_STORE_AVOIDS, fault);
    assert(dsd_app_signal_search_permanent_avoid(462900000) == DSD_APP_SEARCH_AVOID_ADDED);
    dsd_app_signal_search_store_flush();
    char kept[DSD_TEST_PATH_MAX];
    kept_path(kept, sizeof(kept), p.avoids, 1);
    assert(!store_error() && file_is(kept, content, n));
    assert(file_is(p.avoids, "[462900000]\n", 12));
    assert(state_is(DSD_APP_SEARCH_STORE_AVOIDS, DSD_APP_SEARCH_FILE_KEPT, "history.avoids.json.unreadable-1"));
    assert(!write_owed(DSD_APP_SEARCH_STORE_AVOIDS));
    // Later saves only write.
    assert(dsd_app_signal_search_permanent_avoid(462925000) == DSD_APP_SEARCH_AVOID_ADDED);
    dsd_app_signal_search_store_flush();
    assert(file_is(p.avoids, "[462900000,462925000]\n", 22) && file_is(kept, content, n));
    dsd_app_signal_search_store_close();
    assert(dsd_app_signal_search_store_open(p.journal) == 0);
    assert(state_is(DSD_APP_SEARCH_STORE_AVOIDS, DSD_APP_SEARCH_FILE_OK, NULL));
    assert(dsd_app_signal_search_avoids_get(avoids, 4) == 2 && avoids[0] == 462900000);
    dsd_app_signal_search_store_close();
    assert(file_is(kept, content, n));
    static const char* const files[] = {"history.avoids.json", "history.avoids.json.unreadable-1", NULL};
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
}

/* A version 2 journal that loads @p rows before failing (by itself or through @p fault) is reported, shows those rows
   and stays byte for byte through a launch that writes nothing. The first save keeps it as "….unreadable-<kept_n>"
   and writes the rows shown plus the new one; the next open reads that journal. */
static void
expect_history_kept(const store_paths* p, load_fault fault, size_t rows, int kept_n) {
    static unsigned char before[16384];
    const size_t n = read_file(p->v2, before, sizeof(before));
    dsd_signal_search_hit hits[8];
    open_with(p, DSD_APP_SEARCH_STORE_HISTORY, fault);
    assert(!store_error() && state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
    assert(dsd_app_signal_search_history_get(hits, 8) == rows);
    dsd_app_signal_search_store_close();
    assert(file_is(p->v2, before, n));
    char kept[DSD_TEST_PATH_MAX], kept_name[64];
    kept_path(kept, sizeof(kept), p->v2, kept_n);
    assert(!exists(kept));
    open_with(p, DSD_APP_SEARCH_STORE_HISTORY, fault);
    publish(40);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && file_is(kept, before, n));
    const int w = DSD_SNPRINTF(kept_name, sizeof(kept_name), "history.v2.unreadable-%d", kept_n);
    assert(w > 0 && (size_t)w < sizeof(kept_name));
    assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_KEPT, kept_name));
    assert(!write_owed(DSD_APP_SEARCH_STORE_HISTORY));
    assert(count_v2_records(p->v2, (size_t)g_record - 12U) == (int)rows + 1);
    assert(reopen(p->journal, hits, 8) == rows + 1 && hits[0].hit_id == 40);
    assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_OK, NULL));
    dsd_app_signal_search_store_close();
    assert(file_is(kept, before, n));
}

static void
history_kept_cases(const char* root) {
    static const char* const files[] = {"history.v2", "history.v2.unreadable-1", "history.v2.unreadable-2", NULL};
    store_paths p;
    // A record that fails its checksum, with more after it: nothing after the damage is lost (decision D3).
    store_dir(&p, root, "damaged");
    write_v2_journal(p.v2, 3, 2);
    expect_history_kept(&p, no_fault, 1, 1);
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
    // A torn last record. A name already taken is skipped, not replaced.
    store_dir(&p, root, "torn");
    write_v2_journal(p.v2, 2, 0);
    {
        FILE* f = dsd_fopen_private(p.v2, "ab");
        assert(f && fwrite("DSS", 1, 3, f) == 3 && fclose(f) == 0);
    }
    char taken[DSD_TEST_PATH_MAX];
    kept_path(taken, sizeof(taken), p.v2, 1);
    write_bytes(taken, "older", 5);
    expect_history_kept(&p, no_fault, 2, 2);
    assert(file_is(taken, "older", 5));
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
    // A read error at a record boundary is not the end of the file (B-4), nor one inside a record damage.
    store_dir(&p, root, "readerr");
    write_v2_journal(p.v2, 3, 0);
    const load_fault at_boundary = {0, g_record};
    expect_history_kept(&p, at_boundary, 1, 1);
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
    store_dir(&p, root, "readmid");
    write_v2_journal(p.v2, 3, 0);
    const load_fault mid_record = {0, g_record + 100};
    expect_history_kept(&p, mid_record, 1, 1);
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
    // A journal that cannot be opened.
    store_dir(&p, root, "noopen");
    write_v2_journal(p.v2, 3, 0);
    const load_fault denied = {EACCES, -1};
    expect_history_kept(&p, denied, 0, 1);
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
}

static void
avoids_kept_cases(const char* root) {
    static const char invalid[] = "[100,200,x]\n";
    expect_avoids_kept(root, "badavoids", invalid, sizeof(invalid) - 1, no_fault);
    // Text after a NUL is part of the file: the list is refused, not cut short (r1#12).
    static const char nul[] = "[462700000]\0[462725000]\n";
    expect_avoids_kept(root, "nulavoids", nul, sizeof(nul) - 1, no_fault);
    static const char good[] = "[462700000,462725000]\n";
    const load_fault eio = {EIO, -1};
    expect_avoids_kept(root, "openavoids", good, sizeof(good) - 1, eio);
    const load_fault cut = {0, 5};
    expect_avoids_kept(root, "readavoids", good, sizeof(good) - 1, cut);
}

/* Set by the seam below: the next file renamed aside finds a folder at its old name, so the replacement fails. */
static void
block_after_aside(const char* path, const char* kept) {
    (void)kept;
    assert(dsd_mkdir(path, 0700) == 0);
}

/* The move succeeds and the replacement fails: the file stays kept, the write owed, and the retry only writes. */
static void
expect_retry_after_aside(const char* root) {
    store_paths p;
    store_dir(&p, root, "aside");
    write_v2_journal(p.v2, 3, 2);
    static unsigned char before[16384];
    const size_t n = read_file(p.v2, before, sizeof(before));
    assert(dsd_app_signal_search_store_open(p.journal) == 0);
    dsd_app_signal_search_store_test_after_aside(block_after_aside);
    publish(41);
    dsd_app_signal_search_store_flush();
    dsd_app_signal_search_store_test_after_aside(NULL);
    char kept[DSD_TEST_PATH_MAX], second[DSD_TEST_PATH_MAX];
    kept_path(kept, sizeof(kept), p.v2, 1);
    kept_path(second, sizeof(second), p.v2, 2);
    assert(store_error() && write_owed(DSD_APP_SEARCH_STORE_HISTORY));
    assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_KEPT, "history.v2.unreadable-1"));
    assert(file_is(kept, before, n));
    // Whatever is at the name by the retry is replaced, never moved: the unreadable file was kept once already.
    assert(dsd_test_rmdir(p.v2) == 0);
    write_bytes(p.v2, "junk", 4);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && !write_owed(DSD_APP_SEARCH_STORE_HISTORY));
    assert(!exists(second) && file_is(kept, before, n));
    assert(count_v2_records(p.v2, (size_t)g_record - 12U) == 2);
    dsd_app_signal_search_store_close();
    static const char* const files[] = {"history.v2", "history.v2.unreadable-1", NULL};
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
}

/* A version 2 journal a crash left kept with no new one: the version 1 rows stay out, and the history reports the
   kept name (the newest of any). */
static void
expect_v1_blocked(const store_paths* p, const char* kept_name) {
    dsd_signal_search_hit hits[8];
    assert(dsd_app_signal_search_store_open(p->journal) == 0);
    assert(dsd_app_signal_search_history_get(hits, 8) == 0 && !store_error());
    assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, kept_name ? DSD_APP_SEARCH_FILE_KEPT : DSD_APP_SEARCH_FILE_UNREADABLE,
                    kept_name));
    assert(state_is(DSD_APP_SEARCH_STORE_V1, DSD_APP_SEARCH_FILE_MISSING, NULL));
    dsd_app_signal_search_store_close();
}

static void
v1_gate_cases(const char* root) {
    static const char* const files[] = {"history",
                                        "history.v2",
                                        "history.v2.unreadable-1",
                                        "history.v2.unreadable-7",
                                        "history.v2.unreadable-40",
                                        "history.v2.unreadable-99",
                                        NULL};
    store_paths p;
    store_dir(&p, root, "gate");
    write_v1_journal(p.journal, 3);
    static unsigned char v1_before[16384];
    const size_t v1_bytes = read_file(p.journal, v1_before, sizeof(v1_before));
    char kept[DSD_TEST_PATH_MAX];
    // A restart after the move and before the replacement: the kept journal and the version 1 journal stay as they are,
    // and the first save starts a new version 2 journal.
    kept_path(kept, sizeof(kept), p.v2, 1);
    write_v2_journal(kept, 2, 0);
    expect_v1_blocked(&p, "history.v2.unreadable-1");
    dsd_signal_search_hit hits[8];
    assert(dsd_app_signal_search_store_open(p.journal) == 0);
    publish(42);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && count_v2_records(p.v2, (size_t)g_record - 12U) == 1);
    assert(count_v2_records(kept, (size_t)g_record - 12U) == 2);
    assert(reopen(p.journal, hits, 8) == 1 && hits[0].hit_id == 42);
    dsd_app_signal_search_store_close();
    assert(file_is(p.journal, v1_before, v1_bytes));
    assert(remove(p.v2) == 0 && remove(kept) == 0);
    // Any number, holes included.
    kept_path(kept, sizeof(kept), p.v2, 7);
    write_bytes(kept, "x", 1);
    expect_v1_blocked(&p, "history.v2.unreadable-7");
    kept_path(kept, sizeof(kept), p.v2, 40);
    write_bytes(kept, "y", 1);
    expect_v1_blocked(&p, "history.v2.unreadable-40");
    assert(remove(kept) == 0);
    kept_path(kept, sizeof(kept), p.v2, 7);
    assert(remove(kept) == 0);
#if !DSD_PLATFORM_WIN_NATIVE
    // A dangling link is a name that exists.
    kept_path(kept, sizeof(kept), p.v2, 99);
    assert(symlink("nowhere", kept) == 0);
    expect_v1_blocked(&p, "history.v2.unreadable-99");
    assert(remove(kept) == 0);
#endif
    // A name that cannot be checked may be a kept journal: no version 1 rows. The first save finds no version 2
    // journal to keep and writes one.
    dsd_app_signal_search_store_test_fail_probe(12);
    expect_v1_blocked(&p, NULL);
    assert(dsd_app_signal_search_store_open(p.journal) == 0);
    dsd_app_signal_search_store_test_fail_probe(0);
    publish(43);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_MISSING, NULL));
    assert(count_v2_records(p.v2, (size_t)g_record - 12U) == 1);
    dsd_app_signal_search_store_close();
    assert(file_is(p.journal, v1_before, v1_bytes));
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
}

/* A version 1 journal that cannot be read whole is reported and left where it is; its rows read before the failure
   are kept, and the first save writes the version 2 journal from them. */
static void
expect_v1_unreadable(const store_paths* p, load_fault fault, size_t rows) {
    dsd_signal_search_hit hits[8];
    open_with(p, DSD_APP_SEARCH_STORE_V1, fault);
    assert(!store_error() && dsd_app_signal_search_history_get(hits, 8) == rows);
    assert(state_is(DSD_APP_SEARCH_STORE_V1, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
    assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_MISSING, NULL));
    publish(44);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && count_v2_records(p->v2, (size_t)g_record - 12U) == (int)rows + 1);
    assert(state_is(DSD_APP_SEARCH_STORE_V1, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
    assert(!write_owed(DSD_APP_SEARCH_STORE_V1));
    dsd_app_signal_search_store_close();
    char kept[DSD_TEST_PATH_MAX];
    kept_path(kept, sizeof(kept), p->journal, 1);
    assert(!exists(kept));
}

static void
v1_unreadable_cases(const char* root) {
    static const char* const files[] = {"history", "history.v2", NULL};
    store_paths p;
    // Damaged part way: the rows before it.
    store_dir(&p, root, "v1damaged");
    {
        FILE* f = dsd_fopen_private(p.journal, "wb");
        assert(f);
        record_buf body;
        for (int i = 1; i <= 3; ++i) {
            v1_body(&body, (uint64_t)i);
            write_record_sum(f, 0x31535344U, &body, i == 2 ? 1U : 0U);
        }
        assert(fclose(f) == 0);
    }
    static unsigned char v1_before[16384];
    const size_t v1_bytes = read_file(p.journal, v1_before, sizeof(v1_before));
    expect_v1_unreadable(&p, no_fault, 1);
    assert(file_is(p.journal, v1_before, v1_bytes));
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
    // Open and read errors.
    store_dir(&p, root, "v1noopen");
    write_v1_journal(p.journal, 3);
    const load_fault denied = {EACCES, -1};
    expect_v1_unreadable(&p, denied, 0);
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
    store_dir(&p, root, "v1readerr");
    write_v1_journal(p.journal, 3);
    record_buf v1_record;
    v1_body(&v1_record, 1);
    const load_fault at_boundary = {0, (long)v1_record.n + 12};
    expect_v1_unreadable(&p, at_boundary, 1);
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
    // A folder in its place is not moved either.
    store_dir(&p, root, "v1dir");
    assert(dsd_mkdir(p.journal, 0700) == 0);
    expect_v1_unreadable(&p, no_fault, 0);
    assert(dsd_test_rmdir(p.journal) == 0);
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
}

/* Each file keeps its own load state, and only its own save changes it (B-6/7/8). */
static void
expect_per_file_states(const char* root) {
    static const char invalid[] = "[1,x]";
    store_paths p;
    store_dir(&p, root, "perfile");
    // Both unreadable: neither report replaces the other.
    write_v2_journal(p.v2, 2, 1);
    write_bytes(p.avoids, invalid, 5);
    assert(dsd_app_signal_search_store_open(p.journal) == 0);
    assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
    assert(state_is(DSD_APP_SEARCH_STORE_AVOIDS, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
    dsd_app_signal_search_store_close();
    assert(remove(p.v2) == 0);
    // A history write that fails and then recovers leaves the avoid list's state, and the file, alone.
    assert(dsd_app_signal_search_store_open(p.journal) == 0);
    assert(dsd_mkdir(p.v2, 0700) == 0);
    publish(45);
    dsd_app_signal_search_store_flush();
    assert(store_error() && write_owed(DSD_APP_SEARCH_STORE_HISTORY) && !write_owed(DSD_APP_SEARCH_STORE_AVOIDS));
    assert(dsd_test_rmdir(p.v2) == 0);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && !write_owed(DSD_APP_SEARCH_STORE_HISTORY));
    assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_MISSING, NULL));
    assert(state_is(DSD_APP_SEARCH_STORE_AVOIDS, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
    assert(file_is(p.avoids, invalid, 5));
    // The avoid list's own save settles it.
    assert(dsd_app_signal_search_permanent_avoid(463000000) == DSD_APP_SEARCH_AVOID_ADDED);
    dsd_app_signal_search_store_flush();
    assert(state_is(DSD_APP_SEARCH_STORE_AVOIDS, DSD_APP_SEARCH_FILE_KEPT, "history.avoids.json.unreadable-1"));
    assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_MISSING, NULL));
    // An unknown file is refused, and nothing reports for it.
    dsd_app_search_file_load state = DSD_APP_SEARCH_FILE_KEPT;
    assert(dsd_app_signal_search_store_file_state(3, &state, NULL, NULL, 0) == -1);
    assert(dsd_app_signal_search_store_file_state(-1, &state, NULL, NULL, 0) == -1);
    dsd_app_signal_search_store_close();
    static const char* const files[] = {"history.v2", "history.avoids.json", "history.avoids.json.unreadable-1", NULL};
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
}

/* Something other than a file at the avoid list's name when it loads is never moved or replaced: the write stays owed
   until it is gone. A FIFO there does not hang the open. */
static void
expect_not_a_file(const char* root, const char* leaf, int fifo) {
    store_paths p;
    store_dir(&p, root, leaf);
#if !DSD_PLATFORM_WIN_NATIVE
    if (fifo) {
        assert(mkfifo(p.avoids, 0600) == 0 && mkfifo(p.journal, 0600) == 0);
    }
#endif
    if (!fifo) {
        assert(dsd_mkdir(p.avoids, 0700) == 0);
    }
    assert(dsd_app_signal_search_store_open(p.journal) == 0);
    assert(state_is(DSD_APP_SEARCH_STORE_AVOIDS, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
    assert(
        state_is(DSD_APP_SEARCH_STORE_V1, fifo ? DSD_APP_SEARCH_FILE_UNREADABLE : DSD_APP_SEARCH_FILE_MISSING, NULL));
    assert(dsd_app_signal_search_permanent_avoid(463025000) == DSD_APP_SEARCH_AVOID_ADDED);
    dsd_app_signal_search_store_flush();
    assert(store_error() && write_owed(DSD_APP_SEARCH_STORE_AVOIDS));
    assert(state_is(DSD_APP_SEARCH_STORE_AVOIDS, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
    char kept[DSD_TEST_PATH_MAX];
    kept_path(kept, sizeof(kept), p.avoids, 1);
    assert(!exists(kept) && exists(p.avoids));
    assert(fifo ? remove(p.avoids) == 0 : dsd_test_rmdir(p.avoids) == 0);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && file_is(p.avoids, "[463025000]\n", 12));
    assert(state_is(DSD_APP_SEARCH_STORE_AVOIDS, DSD_APP_SEARCH_FILE_MISSING, NULL));
    dsd_app_signal_search_store_close();
    static const char* const files[] = {"history", "history.avoids.json", NULL};
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
}

/* The sweep lists the folder the store's files are in: a root keeps its separator, since a drive letter alone ("C:")
   names that drive's current folder. */
static int
folder_is(const char* path, const char* want) {
    char folder[DSD_TEST_PATH_MAX];
    dsd_app_signal_search_store_test_folder_of(path, folder, sizeof(folder));
    return strcmp(folder, want) == 0;
}

static void
expect_sweep_folder(void) {
    assert(folder_is("history", "."));
    assert(folder_is("dir/history", "dir"));
    assert(folder_is("/a/b/history", "/a/b"));
    assert(folder_is("/history", "/"));
    assert(folder_is("C:/history", "C:/"));
#if DSD_PLATFORM_WIN_NATIVE
    assert(folder_is("C:\\history", "C:\\"));
    assert(folder_is("C:\\Users\\history", "C:\\Users"));
#endif
}

/* Opening removes this store's own temporary files a crash left, and nothing else. */
static void
expect_temp_sweep(const char* root) {
    store_paths p;
    store_dir(&p, root, "sweep");
    static const char* const leaked[] = {"history.v2.tmp.Ab12Cd", "history.avoids.json.tmp.zz9Zz9"};
    static const char* const kept[] = {"history.v2.tmpx", "history.tmp.123456", "history.v2.unreadable-3.tmp.aaaaaa",
                                       "history.v2.tmp.Ab12Cd.mine"};
    char path[DSD_TEST_PATH_MAX];
    for (size_t i = 0; i < 2; ++i) {
        assert(dsd_test_path_join(path, sizeof(path), p.dir, leaked[i]) == 0);
        write_bytes(path, "t", 1);
    }
    for (size_t i = 0; i < 4; ++i) {
        assert(dsd_test_path_join(path, sizeof(path), p.dir, kept[i]) == 0);
        write_bytes(path, "k", 1);
    }
    assert(dsd_app_signal_search_store_open(p.journal) == 0);
    for (size_t i = 0; i < 2; ++i) {
        assert(dsd_test_path_join(path, sizeof(path), p.dir, leaked[i]) == 0);
        assert(!exists(path));
    }
    for (size_t i = 0; i < 4; ++i) {
        assert(dsd_test_path_join(path, sizeof(path), p.dir, kept[i]) == 0);
        assert(file_is(path, "k", 1));
    }
    dsd_app_signal_search_store_close();
    static const char* const files[] = {"history.v2.tmpx", "history.tmp.123456", "history.v2.unreadable-3.tmp.aaaaaa",
                                        "history.v2.tmp.Ab12Cd.mine", NULL};
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
}

#if !DSD_PLATFORM_WIN_NATIVE
/* A FIFO that took a cleanly loaded journal's name: the append, and the retry that then rewrites the history, neither
   wait on it nor replace it; the write stays owed and reported until it is gone. Run in a child, so a store that
   waits on the FIFO fails the test within the budget instead of hanging it. */
static void
append_finds_fifo(const char* root) {
    store_paths p;
    store_dir(&p, root, "appendfifo");
    write_v2_journal(p.v2, 3, 0);
    assert(dsd_app_signal_search_store_open(p.journal) == 0);
    assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_OK, NULL));
    // The run's first write rewrites the journal; the next one appends to it.
    publish(44);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && count_v2_records(p.v2, (size_t)g_record - 12U) == 4);
    assert(remove(p.v2) == 0 && mkfifo(p.v2, 0600) == 0);
    publish(45);
    dsd_app_signal_search_store_flush();
    assert(store_error() && write_owed(DSD_APP_SEARCH_STORE_HISTORY));
    // The failed append leaves the retry rewriting every record, which does not replace the FIFO either.
    publish(46);
    dsd_app_signal_search_store_flush();
    assert(store_error() && write_owed(DSD_APP_SEARCH_STORE_HISTORY));
    struct stat st;
    assert(lstat(p.v2, &st) == 0 && S_ISFIFO(st.st_mode));
    assert(remove(p.v2) == 0);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && !write_owed(DSD_APP_SEARCH_STORE_HISTORY));
    assert(count_v2_records(p.v2, (size_t)g_record - 12U) == 6);
    dsd_app_signal_search_store_close();
    static const char* const files[] = {"history.v2", NULL};
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
}

static void
expect_append_finds_fifo(const char* root) {
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        append_finds_fifo(root);
        _exit(0);
    }
    const uint64_t deadline = dsd_realtime_mono_ms() + 10000U;
    int status = 0;
    pid_t done = 0;
    while ((done = waitpid(pid, &status, WNOHANG)) == 0 && dsd_realtime_mono_ms() < deadline) {
        dsd_sleep_ms(5);
    }
    if (done == 0) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, &status, 0);
        (void)DSD_FPRINTF(stderr, "FAIL: the store waited on a FIFO at the journal's name\n");
    }
    assert(done == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* The seam's stand-in for a no-replace rename whose link worked and whose unlink failed (out_source_left): the old
   name still names the kept file. */
static void
link_back_after_aside(const char* path, const char* kept) {
    assert(link(kept, path) == 0);
}

/* The move succeeds and the replacement fails, then a FIFO takes the name before the retry: the retry, which only
   writes, never replaces it (a rename over it would delete it), and the write stays owed until it is gone. */
static void
expect_retry_finds_fifo(const char* root) {
    store_paths p;
    store_dir(&p, root, "asidefifo");
    write_v2_journal(p.v2, 3, 2);
    static unsigned char before[16384];
    const size_t n = read_file(p.v2, before, sizeof(before));
    assert(dsd_app_signal_search_store_open(p.journal) == 0);
    dsd_app_signal_search_store_test_after_aside(block_after_aside);
    publish(43);
    dsd_app_signal_search_store_flush();
    dsd_app_signal_search_store_test_after_aside(NULL);
    char kept[DSD_TEST_PATH_MAX], second[DSD_TEST_PATH_MAX];
    kept_path(kept, sizeof(kept), p.v2, 1);
    kept_path(second, sizeof(second), p.v2, 2);
    assert(store_error() && write_owed(DSD_APP_SEARCH_STORE_HISTORY));
    assert(dsd_test_rmdir(p.v2) == 0 && mkfifo(p.v2, 0600) == 0);
    dsd_app_signal_search_store_flush();
    assert(store_error() && write_owed(DSD_APP_SEARCH_STORE_HISTORY));
    assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_KEPT, "history.v2.unreadable-1"));
    {
        struct stat st;
        assert(lstat(p.v2, &st) == 0 && S_ISFIFO(st.st_mode));
    }
    assert(!exists(second) && file_is(kept, before, n));
    assert(remove(p.v2) == 0);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && !write_owed(DSD_APP_SEARCH_STORE_HISTORY));
    assert(!exists(second) && file_is(kept, before, n));
    assert(count_v2_records(p.v2, (size_t)g_record - 12U) == 2);
    dsd_app_signal_search_store_close();
    static const char* const files[] = {"history.v2", "history.v2.unreadable-1", NULL};
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
}

/* POSIX only: a mode that refuses reading (as a user other than root), a symbolic link at the journal's name (refused,
   and kept as the link itself), a source left after the move, which counts as kept and is replaced, and a FIFO at a
   kept file's name by the retry or at a clean journal's name by an append. */
static void
posix_cases(const char* root) {
    static const char* const files[] = {"history.v2",          "history.v2.unreadable-1",          "target",
                                        "history.avoids.json", "history.avoids.json.unreadable-1", NULL};
    store_paths p;
    dsd_signal_search_hit hits[8];
    expect_append_finds_fifo(root); // first: the fork's child finds no other store thread
    if (geteuid() != 0) {
        store_dir(&p, root, "mode000");
        write_v2_journal(p.v2, 3, 0);
        static unsigned char before[16384];
        const size_t n = read_file(p.v2, before, sizeof(before));
        assert(chmod(p.v2, 0) == 0);
        assert(dsd_app_signal_search_store_open(p.journal) == 0);
        assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
        publish(46);
        dsd_app_signal_search_store_flush();
        assert(!store_error() && count_v2_records(p.v2, (size_t)g_record - 12U) == 1);
        dsd_app_signal_search_store_close();
        char kept[DSD_TEST_PATH_MAX];
        kept_path(kept, sizeof(kept), p.v2, 1);
        assert(chmod(kept, 0600) == 0 && file_is(kept, before, n));
        assert(dsd_test_remove_temp_dir(p.dir, files) == 0);
    } else {
        (void)DSD_FPRINTF(stderr, "skipping the unreadable-mode case: running as root\n");
    }

    store_dir(&p, root, "symlink");
    char target[DSD_TEST_PATH_MAX], kept[DSD_TEST_PATH_MAX], link_text[DSD_TEST_PATH_MAX];
    assert(dsd_test_path_join(target, sizeof(target), p.dir, "target") == 0);
    write_v2_journal(target, 2, 0);
    static unsigned char target_before[16384];
    const size_t target_bytes = read_file(target, target_before, sizeof(target_before));
    assert(symlink("target", p.v2) == 0);
    assert(dsd_app_signal_search_store_open(p.journal) == 0);
    assert(dsd_app_signal_search_history_get(hits, 8) == 0);
    assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
    publish(47);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && count_v2_records(p.v2, (size_t)g_record - 12U) == 1);
    dsd_app_signal_search_store_close();
    kept_path(kept, sizeof(kept), p.v2, 1);
    const ssize_t len = readlink(kept, link_text, sizeof(link_text) - 1);
    assert(len == 6 && memcmp(link_text, "target", 6) == 0);
    assert(file_is(target, target_before, target_bytes));
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);

    // A link to a folder is still a link: it is moved aside itself, the folder is never touched, and the write goes
    // through.
    store_dir(&p, root, "linkdir");
    assert(dsd_test_path_join(target, sizeof(target), p.dir, "target") == 0);
    assert(dsd_mkdir(target, 0700) == 0);
    assert(symlink("target", p.v2) == 0);
    assert(dsd_app_signal_search_store_open(p.journal) == 0);
    assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
    publish(48);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && !write_owed(DSD_APP_SEARCH_STORE_HISTORY));
    assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_KEPT, "history.v2.unreadable-1"));
    assert(count_v2_records(p.v2, (size_t)g_record - 12U) == 1);
    dsd_app_signal_search_store_close();
    kept_path(kept, sizeof(kept), p.v2, 1);
    const ssize_t dir_len = readlink(kept, link_text, sizeof(link_text) - 1);
    assert(dir_len == 6 && memcmp(link_text, "target", 6) == 0);
    {
        struct stat st;
        assert(lstat(target, &st) == 0 && S_ISDIR(st.st_mode));
    }
    assert(dsd_test_rmdir(target) == 0);
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);

    store_dir(&p, root, "sourceleft");
    write_bytes(p.avoids, "[x]", 3);
    assert(dsd_app_signal_search_store_open(p.journal) == 0);
    dsd_app_signal_search_store_test_after_aside(link_back_after_aside);
    assert(dsd_app_signal_search_permanent_avoid(463050000) == DSD_APP_SEARCH_AVOID_ADDED);
    dsd_app_signal_search_store_flush();
    dsd_app_signal_search_store_test_after_aside(NULL);
    kept_path(kept, sizeof(kept), p.avoids, 1);
    assert(!store_error() && file_is(kept, "[x]", 3) && file_is(p.avoids, "[463050000]\n", 12));
    assert(state_is(DSD_APP_SEARCH_STORE_AVOIDS, DSD_APP_SEARCH_FILE_KEPT, "history.avoids.json.unreadable-1"));
    dsd_app_signal_search_store_close();
    assert(dsd_test_remove_temp_dir(p.dir, files) == 0);

    expect_not_a_file(root, "fifo", 1);
    expect_retry_finds_fifo(root);
}
#endif

static void
unreadable_cases(const char* root) {
    history_kept_cases(root);
    avoids_kept_cases(root);
    expect_retry_after_aside(root);
    v1_gate_cases(root);
    v1_unreadable_cases(root);
    expect_per_file_states(root);
    expect_not_a_file(root, "diravoids", 0);
    expect_sweep_folder();
    expect_temp_sweep(root);
#if !DSD_PLATFORM_WIN_NATIVE
    posix_cases(root);
#endif
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
    const long record = file_size(clean.v2);
    assert(record > 0);
    g_record = record;
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
    assert(file_size(late.v2) == record);
    assert(file_size(late.avoids) > 0);
    // The retry rewrote the journal; later detections append to it without repeating it.
    publish(3);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && file_size(late.v2) == 2 * record);
    assert(reopen(late.journal, hits, 8) == 2 && hits[0].hit_id == 3 && hits[1].hit_id == 2);
    assert(dsd_app_signal_search_avoids_get(avoids, 4) == 1 && avoids[0] == 462575000);
    dsd_app_signal_search_store_close();

    // A journal that still cannot be written stays reported after the avoid beside it saves. A directory in its place
    // refuses both an append and the compacted replacement, and is never moved aside; once it is gone the mark it
    // earned at the load clears and the write goes through.
    store_paths blocked;
    paths_init(&blocked, root, "blocked");
    assert(dsd_mkdir(blocked.dir, 0700) == 0);
    assert(dsd_mkdir(blocked.v2, 0700) == 0);
    assert(dsd_app_signal_search_store_open(blocked.journal) == 0);
    assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
    publish(4);
    dsd_app_signal_search_store_flush();
    assert(store_error());
    assert(dsd_app_signal_search_permanent_avoid(462600000) == DSD_APP_SEARCH_AVOID_ADDED);
    dsd_app_signal_search_store_flush();
    assert(store_error());
    assert(file_size(blocked.avoids) > 0);
    {
        char kept[DSD_TEST_PATH_MAX];
        kept_path(kept, sizeof(kept), blocked.v2, 1);
        assert(!exists(kept) && write_owed(DSD_APP_SEARCH_STORE_HISTORY));
        assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
    }
    assert(dsd_test_rmdir(blocked.v2) == 0);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && file_size(blocked.v2) == record);
    assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_MISSING, NULL));
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
    assert(store_error() && file_size(stuck.v2) == record);
    assert(dsd_test_rmdir(stuck.avoids) == 0);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && file_size(stuck.v2) == record);
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
    assert(file_size(closing.v2) == record);
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

    // Issue #828: the store alone writes the runtime's permanent avoid list, which a running search reads at its next
    // tick: the list as loaded, then at every add and remove; closing the store withdraws it. A file that repeats a
    // frequency loads it once (A-B6), so a single removal lifts it everywhere.
    store_paths shared;
    paths_init(&shared, root, "shared");
    assert(dsd_mkdir(shared.dir, 0700) == 0);
    {
        FILE* f = dsd_fopen_private(shared.avoids, "wb");
        assert(f);
        assert(fputs("[462700000, 462725000,462700000]\n", f) >= 0);
        assert(fclose(f) == 0);
    }
    uint32_t runtime[4];
    assert(dsd_app_signal_search_store_open(shared.journal) == 0);
    assert(!store_error());
    assert(dsd_app_signal_search_avoids_get(avoids, 4) == 2 && avoids[0] == 462700000 && avoids[1] == 462725000);
    assert(runtime_permanent(runtime, 4) == 2 && runtime[0] == 462700000 && runtime[1] == 462725000);
    assert(dsd_app_signal_search_permanent_avoid(462750000) == DSD_APP_SEARCH_AVOID_ADDED);
    assert(runtime_permanent(runtime, 4) == 3 && runtime[2] == 462750000);
    const uint64_t permanent_revision = dsd_signal_search_avoids_permanent_revision();
    assert(dsd_app_signal_search_permanent_avoid(462750000) == DSD_APP_SEARCH_AVOID_DUPLICATE);
    assert(dsd_signal_search_avoids_permanent_revision() == permanent_revision);
    assert(dsd_app_signal_search_avoid_remove(462700000) == 0);
    assert(runtime_permanent(runtime, 4) == 2 && runtime[0] == 462725000 && runtime[1] == 462750000);
    assert(dsd_app_signal_search_avoid_remove(462700000) == -1);
    assert(dsd_app_signal_search_avoids_get(avoids, 4) == 2);
    dsd_app_signal_search_store_close();
    assert(runtime_permanent(runtime, 4) == 0 && dsd_app_signal_search_avoids_get(avoids, 4) == 0);
    assert(dsd_app_signal_search_store_open(shared.journal) == 0);
    assert(runtime_permanent(runtime, 4) == 2 && runtime[0] == 462725000 && runtime[1] == 462750000);

    // The session list is the engine's, read from the runtime, and shown only for the store's own running session:
    // never another session's, nor one that failed or stopped, even while the runtime still holds it. A change to the
    // list, or to whether it is shown, moves the avoid revision the frontends copy by.
    const uint32_t session_list[] = {462800000, 462825000};
    publish_phase(41, DSD_SEARCH_SEARCHING, 1);
    uint64_t avoid_revision = dsd_app_signal_search_avoids_revision();
    dsd_signal_search_avoids_session_set(41, session_list, 2);
    assert(dsd_app_signal_search_avoids_revision() != avoid_revision);
    assert(dsd_app_signal_search_session_avoids_get(avoids, 4) == 2 && avoids[0] == 462800000);
    assert(dsd_app_signal_search_session_avoids_get(avoids, 1) == 1 && avoids[0] == 462800000);
    dsd_signal_search_avoids_session_set(42, session_list, 1);
    assert(dsd_app_signal_search_session_avoids_get(avoids, 4) == 0);
    dsd_signal_search_avoids_session_set(41, session_list + 1, 1);
    assert(dsd_app_signal_search_session_avoids_get(avoids, 4) == 1 && avoids[0] == 462825000);
    avoid_revision = dsd_app_signal_search_avoids_revision();
    publish_phase(41, DSD_SEARCH_PAUSED, 2);
    assert(dsd_app_signal_search_avoids_revision() == avoid_revision);
    assert(dsd_app_signal_search_session_avoids_get(avoids, 4) == 1);
    publish_phase(41, DSD_SEARCH_ERROR, 3);
    assert(dsd_app_signal_search_avoids_revision() != avoid_revision);
    assert(dsd_app_signal_search_session_avoids_get(avoids, 4) == 0);
    avoid_revision = dsd_app_signal_search_avoids_revision();
    publish_phase(41, DSD_SEARCH_PAUSED, 4);
    assert(dsd_app_signal_search_avoids_revision() != avoid_revision);
    assert(dsd_app_signal_search_session_avoids_get(avoids, 4) == 1);
    publish_phase(41, DSD_SEARCH_STOPPED, 5);
    assert(dsd_app_signal_search_session_avoids_get(avoids, 4) == 0);
    publish_phase(41, DSD_SEARCH_LISTENING, 6);
    assert(dsd_app_signal_search_session_avoids_get(avoids, 4) == 1);
    avoid_revision = dsd_app_signal_search_avoids_revision();
    dsd_app_signal_search_session_ended();
    assert(dsd_app_signal_search_avoids_revision() != avoid_revision);
    assert(dsd_app_signal_search_session_avoids_get(avoids, 4) == 0);
    dsd_signal_search_avoids_session_set(0, NULL, 0);
    dsd_app_signal_search_store_close();

    // A version 1 journal loads every row, with no checked site, slot or encryption. The first write creates the
    // version 2 journal holding all of them; the version 1 journal is never touched, so an earlier release still
    // reads its own history.
    store_paths migrate;
    paths_init(&migrate, root, "migrate");
    assert(dsd_mkdir(migrate.dir, 0700) == 0);
    write_v1_journal(migrate.journal, 3);
    static unsigned char v1_before[16384], v1_after[16384];
    const size_t v1_bytes = read_file(migrate.journal, v1_before, sizeof(v1_before));
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
    assert(file_size(migrate.v2) == -1);
    dsd_signal_search_hit detailed;
    detailed_hit(&detailed, 4);
    publish_hit(&detailed);
    dsd_app_signal_search_store_flush();
    assert(!store_error());
    assert(count_v2_records(migrate.v2, (size_t)record - 12U) == 4);
    assert(file_size(migrate.v2) == 4 * record);
    assert(read_file(migrate.journal, v1_after, sizeof(v1_after)) == v1_bytes);
    assert(memcmp(v1_before, v1_after, v1_bytes) == 0);
    assert(reopen(migrate.journal, hits, 8) == 4);
    assert(hits[0].hit_id == 4 && hits[3].hit_id == 1 && strcmp(hits[3].target_text, "Fire") == 0);
    assert(hits[3].site[0] == '\0' && hits[3].slot == 0 && hits[3].crypt[0] == '\0');
    dsd_app_signal_search_store_close();
    // Both exist: the version 2 journal wins. Rows an earlier release added to its own journal since stay there.
    write_v1_journal(migrate.journal, 6);
    const size_t v1_grown = read_file(migrate.journal, v1_before, sizeof(v1_before));
    assert(reopen(migrate.journal, hits, 8) == 4 && hits[0].hit_id == 4);
    publish(7);
    dsd_app_signal_search_store_flush();
    assert(!store_error() && file_size(migrate.v2) == 5 * record);
    dsd_app_signal_search_store_close();
    assert(read_file(migrate.journal, v1_after, sizeof(v1_after)) == v1_grown);
    assert(memcmp(v1_before, v1_after, v1_grown) == 0);
    // Even an empty version 2 journal wins: what it holds is the history, and nothing is migrated again.
    FILE* empty = dsd_fopen_private(migrate.v2, "wb");
    assert(empty && fclose(empty) == 0);
    assert(reopen(migrate.journal, hits, 8) == 0);
    dsd_app_signal_search_store_close();
    // A version 1 journal holding a version 2 record is refused at that record, as any unknown record is.
    {
        assert(remove(migrate.v2) == 0);
        record_buf body;
        detailed_v2_body(&body, 1);
        FILE* f = dsd_fopen_private(migrate.journal, "wb");
        assert(f);
        write_record(f, 0x32535344U, &body);
        assert(fclose(f) == 0);
        assert(dsd_app_signal_search_store_open(migrate.journal) == 0);
        assert(state_is(DSD_APP_SEARCH_STORE_V1, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
        assert(dsd_app_signal_search_history_get(hits, 8) == 0);
        dsd_app_signal_search_store_close();
        assert(file_size(migrate.v2) == -1);
    }

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
    // its last byte. A record of another version, or whose size does not match its version, stops the load at that
    // record with an error, keeping what came before it.
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
        FILE* f = dsd_fopen_private(hostile.v2, "wb");
        assert(f);
        write_record(f, 0x32535344U, &body);
        v1_body(&body, 2);
        write_record(f, 0x31535344U, &body); // DSS1: the version 2 journal holds only version 2 records
        assert(fclose(f) == 0);
        assert(dsd_app_signal_search_store_open(hostile.journal) == 0);
        assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
        assert(dsd_app_signal_search_history_get(hits, 8) == 1 && hits[0].hit_id == 1 && hits[0].slot == 0);
        assert(strlen(hits[0].site) == sizeof(h.site) - 1 && strlen(hits[0].crypt) == sizeof(h.crypt) - 1);
        dsd_app_signal_search_store_close();
        // A version 2 magic in front of a version 1 body is refused, not read past its end.
        v1_body(&body, 3);
        assert(body.n == v1_n);
        f = dsd_fopen_private(hostile.v2, "wb");
        assert(f);
        write_record(f, 0x32535344U, &body);
        assert(fclose(f) == 0);
        assert(dsd_app_signal_search_store_open(hostile.journal) == 0);
        assert(state_is(DSD_APP_SEARCH_STORE_HISTORY, DSD_APP_SEARCH_FILE_UNREADABLE, NULL));
        assert(dsd_app_signal_search_history_get(hits, 8) == 0);
        dsd_app_signal_search_store_close();
        // Closing writes nothing, so nothing was moved.
        assert(file_size(hostile.v2) == (long)body.n + 12);
    }

    unreadable_cases(root);

    // Failed replacements removed their temporary files.
    paths_remove(&migrate);
    paths_remove(&details);
    paths_remove(&hostile);
    paths_remove(&limits);
    paths_remove(&shared);
    paths_remove(&clean);
    paths_remove(&late);
    paths_remove(&blocked);
    paths_remove(&stuck);
    paths_remove(&closing);
    static const char* const none[] = {NULL};
    assert(dsd_test_remove_temp_dir(root, none) == 0);
    return 0;
}
