// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/wav_journal.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "wav_journal.h"
#include "wav_journal_test_support.h"

/* The journal is one file per process, appended a line at a time: each record is built whole, then written with one
   fwrite() and flushed, so a reader sees whole lines and a crash tears at most the last one. No fsync: the records
   are page-cache writes, the same I/O class as the rename and the size reads the close already makes. The decoder,
   the watchdog and the command queue all reach it (closes, commits, a target restore), so it has its own lock, taken
   last: nothing here calls back out of the journal. */

static dsd_mutex_t s_mu;
/* 0 = uninitialized, 1 = initialization in flight, 2 = ready. */
static atomic_int s_mu_state;
static atomic_int s_state; /* dsd_wav_journal_state */

static FILE* s_fp;
static char s_path[1024];
/* The journal's own folder, canonical: a recording inside it is named relative to it. */
static char s_dir[1024];
/* One record, built under the lock. Every field is bounded (paths 2000 bytes, row strings 200), and a string escapes
   to at most 6 bytes a byte, so a record fits. */
static char s_line[32768];
static long s_test_writes_left = -1;
static int s_test_pending_growth_fails;

/* Recordings published before their call's row committed (the NFM call end, a WAV stop mid-call, the EDACS analog
   rotation): each waits in its slot's list for that epoch's commit, which links it, or for the row to be gone, which
   unlinks it. A list grows with the call's segments. */
typedef struct {
    char id[48];
    uint64_t epoch;
} wav_journal_pending;

enum { WAV_JOURNAL_SLOTS = 2 };

typedef struct {
    wav_journal_pending* items;
    size_t count;
    size_t cap;
} wav_journal_pending_list;

static wav_journal_pending_list s_pending[WAV_JOURNAL_SLOTS];

static void
wav_journal_lock(void) {
    if (atomic_load(&s_mu_state) != 2) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&s_mu_state, &expected, 1)) {
            (void)dsd_mutex_init(&s_mu);
            atomic_store(&s_mu_state, 2);
        } else {
            while (atomic_load(&s_mu_state) != 2) {
                dsd_thread_yield();
            }
        }
    }
    dsd_mutex_lock(&s_mu);
}

static void
wav_journal_unlock(void) {
    dsd_mutex_unlock(&s_mu);
}

void
dsd_wav_journal_test_fail_writes_after(long successful_writes) {
    wav_journal_lock();
    s_test_writes_left = successful_writes;
    wav_journal_unlock();
}

void
dsd_wav_journal_test_fail_pending_growth(int fail) {
    wav_journal_lock();
    s_test_pending_growth_fails = fail;
    wav_journal_unlock();
}

int
wav_journal_active(void) {
    return atomic_load(&s_state) == DSD_WAV_JOURNAL_ON;
}

dsd_wav_journal_state
dsd_wav_journal_state_get(void) {
    return (dsd_wav_journal_state)atomic_load(&s_state);
}

static void
wav_journal_pending_clear_locked(void) {
    for (int slot = 0; slot < WAV_JOURNAL_SLOTS; slot++) {
        free(s_pending[slot].items);
        s_pending[slot].items = NULL;
        s_pending[slot].count = 0;
        s_pending[slot].cap = 0;
    }
}

/* The run's journal is done with (closed, or latched off by a failure). */
static void
wav_journal_close_locked(dsd_wav_journal_state next) {
    if (s_fp) {
        (void)fclose(s_fp);
        s_fp = NULL;
    }
    wav_journal_pending_clear_locked();
    atomic_store(&s_state, (int)next);
}

/* A write that failed turns the journal off for the rest of the run: a journal with a hole in it would link the wrong
   rows, and a full disk fails every write after the first. The recording view reports the state. */
static void
wav_journal_fail_locked(const char* what) {
    LOG_ERROR("Call recording journal %s: %s failed; no more records are written this run\n", s_path, what);
    wav_journal_close_locked(DSD_WAV_JOURNAL_FAILED);
}

static int
wav_journal_write_locked(const char* data, size_t len) {
    if (!s_fp) {
        return -1;
    }
    int ok = 1;
    if (s_test_writes_left == 0) {
        ok = 0;
    } else {
        if (s_test_writes_left > 0) {
            s_test_writes_left--;
        }
        ok = fwrite(data, 1, len, s_fp) == len && fflush(s_fp) == 0;
    }
    if (!ok) {
        wav_journal_fail_locked("a write");
        return -1;
    }
    return 0;
}

/* --- One record, built in s_line. --- */

typedef struct {
    size_t len;
    int overflow;
} wav_journal_line;

static void
line_put(wav_journal_line* l, const char* data, size_t n) {
    if (l->overflow || n >= sizeof s_line - l->len) {
        l->overflow = 1;
        return;
    }
    DSD_MEMCPY(s_line + l->len, data, n);
    l->len += n;
}

static void
line_raw(wav_journal_line* l, const char* text) {
    line_put(l, text, strlen(text));
}

static void
line_u_escape(wav_journal_line* l, unsigned int cp) {
    char esc[8];
    (void)DSD_SNPRINTF(esc, sizeof esc, "\\u%04X", cp & 0xFFFFU);
    line_raw(l, esc);
}

/* Length of the well-formed UTF-8 sequence at p, or 0 when the bytes there are not one. */
static size_t
utf8_seq_len(const unsigned char* p, const unsigned char* end) {
    const unsigned char c = p[0];
    unsigned char lo = 0x80U;
    unsigned char hi = 0xBFU;
    size_t seq = 0;
    if (c >= 0xC2U && c <= 0xDFU) {
        seq = 2;
    } else if (c >= 0xE0U && c <= 0xEFU) {
        lo = (c == 0xE0U) ? 0xA0U : 0x80U;
        hi = (c == 0xEDU) ? 0x9FU : 0xBFU;
        seq = 3;
    } else if (c >= 0xF0U && c <= 0xF4U) {
        lo = (c == 0xF0U) ? 0x90U : 0x80U;
        hi = (c == 0xF4U) ? 0x8FU : 0xBFU;
        seq = 4;
    }
    if (seq == 0 || (size_t)(end - p) < seq || p[1] < lo || p[1] > hi) {
        return 0;
    }
    for (size_t i = 2; i < seq; i++) {
        if ((p[i] & 0xC0U) != 0x80U) {
            return 0;
        }
    }
    return seq;
}

/* A JSON string. Radio text can hold anything: quotes, backslashes and control characters are escaped, U+2028 and
   U+2029 too (they end a line for some readers), and a byte that starts no UTF-8 character (Latin-1 off the air)
   becomes U+FFFD, so the line stays valid JSON. */
static void
line_string(wav_journal_line* l, const char* text) {
    line_raw(l, "\"");
    const unsigned char* p = (const unsigned char*)(text ? text : "");
    const unsigned char* end = p + strlen((const char*)p);
    while (p < end) {
        const unsigned char c = *p;
        if (c == '"' || c == '\\') {
            const char esc[2] = {'\\', (char)c};
            line_put(l, esc, 2);
            p++;
        } else if (c == '\n') {
            line_raw(l, "\\n");
            p++;
        } else if (c == '\r') {
            line_raw(l, "\\r");
            p++;
        } else if (c == '\t') {
            line_raw(l, "\\t");
            p++;
        } else if (c < 0x20U || c == 0x7FU) {
            line_u_escape(l, c);
            p++;
        } else if (c < 0x80U) {
            line_put(l, (const char*)p, 1);
            p++;
        } else {
            const size_t seq = utf8_seq_len(p, end);
            if (seq == 0) {
                line_raw(l, "\xEF\xBF\xBD");
                p++;
            } else if (seq == 3 && p[0] == 0xE2U && p[1] == 0x80U && (p[2] == 0xA8U || p[2] == 0xA9U)) {
                line_u_escape(l, 0x2000U | ((unsigned int)p[2] & 0x3FU));
                p += 3;
            } else {
                line_put(l, (const char*)p, seq);
                p += seq;
            }
        }
    }
    line_raw(l, "\"");
}

static void
line_key(wav_journal_line* l, const char* key) {
    line_raw(l, ",\"");
    line_raw(l, key);
    line_raw(l, "\":");
}

static void
line_key_string(wav_journal_line* l, const char* key, const char* value) {
    line_key(l, key);
    line_string(l, value);
}

static void
line_key_u64(wav_journal_line* l, const char* key, uint64_t value) {
    char num[24];
    (void)DSD_SNPRINTF(num, sizeof num, "%llu", (unsigned long long)value);
    line_key(l, key);
    line_raw(l, num);
}

static void
line_key_i64(wav_journal_line* l, const char* key, int64_t value) {
    char num[24];
    (void)DSD_SNPRINTF(num, sizeof num, "%lld", (long long)value);
    line_key(l, key);
    line_raw(l, num);
}

/* A 64-bit nonce as 16 hex digits: a JSON number past 2^53 does not survive a reader that parses doubles. */
static void
line_key_hex64(wav_journal_line* l, const char* key, uint64_t value) {
    char hex[24];
    (void)DSD_SNPRINTF(hex, sizeof hex, "%016llX", (unsigned long long)value);
    line_key_string(l, key, hex);
}

static void
line_key_bool(wav_journal_line* l, const char* key, int value) {
    line_key(l, key);
    line_raw(l, value ? "true" : "false");
}

/* {"v":1,"type":...,"t":... plus the id: every record starts so. */
static void
line_begin(wav_journal_line* l, const char* type, const char* id) {
    l->len = 0;
    l->overflow = 0;
    line_raw(l, "{\"v\":1");
    line_key_string(l, "type", type);
    line_key_i64(l, "t", (int64_t)dsd_decode_time());
    line_key(l, "id");
    if (id && id[0] != '\0') {
        line_string(l, id);
    } else {
        line_raw(l, "null");
    }
}

static void
line_end_and_write_locked(wav_journal_line* l) {
    line_raw(l, "}\n");
    if (l->overflow) {
        /* Not reachable with the bounded fields above; a record cut short would not parse, so it counts as a failure. */
        wav_journal_fail_locked("a record that did not fit");
        return;
    }
    (void)wav_journal_write_locked(s_line, l->len);
}

/* --- Paths. --- */

/* The host's path rules for the helpers below, which take them as a flag so a test can run the Windows forms anywhere. */
#if DSD_PLATFORM_WIN_NATIVE
enum { WAV_JOURNAL_WIN_PATHS = 1 };
#else
enum { WAV_JOURNAL_WIN_PATHS = 0 };
#endif

static int
is_sep(char c, int win) {
    return c == '/' || (win && c == '\\');
}

const char*
wav_journal_path_base(const char* path, int win) {
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (is_sep(*p, win)) {
            base = p + 1;
        }
    }
    return base;
}

void
wav_journal_path_folder(const char* path, int win, char* out, size_t out_size) {
    const char* base = wav_journal_path_base(path, win);
    size_t keep = (size_t)(base - path);
    if (keep > 1) {
        keep--; /* the separator before the name; a file in "/" keeps it */
    }
    if (win && keep == 2 && path[1] == ':') {
        keep = 3; /* and "C:\\" for a file in a drive's root: "C:" alone is that drive's current folder */
    }
    (void)DSD_SNPRINTF(out, out_size, "%.*s", (int)keep, path);
}

/* @p folder (canonical) relative to @p journal_dir: "" for the folder itself, "sub\\dir" below it; NULL outside. */
static const char*
path_inside(const char* journal_dir, const char* folder, int win) {
    const size_t n = strlen(journal_dir);
    if (n == 0 || strncmp(folder, journal_dir, n) != 0) {
        return NULL;
    }
    if (folder[n] == '\0') {
        return "";
    }
    if (is_sep(journal_dir[n - 1], win)) {
        return folder + n; /* the journal sits in a root folder */
    }
    return is_sep(folder[n], win) ? folder + n + 1 : NULL;
}

void
wav_journal_path_name(const char* journal_dir, const char* folder, const char* base, int win, char* out,
                      size_t out_size) {
    if (!base || base[0] == '\0') {
        out[0] = '\0';
        return;
    }
    const char* inside = path_inside(journal_dir, folder, win);
    if (inside) {
        /* Relative names are '/'-separated on every platform. */
        (void)DSD_SNPRINTF(out, out_size, "%s%s%s", inside, (inside[0] != '\0') ? "/" : "", base);
        for (char* p = out; *p; p++) {
            if (win && *p == '\\') {
                *p = '/';
            }
        }
        return;
    }
    const size_t n = strlen(folder);
    const int ends_in_sep = n > 0 && is_sep(folder[n - 1], win);
    (void)DSD_SNPRINTF(out, out_size, "%s%s%s", folder, (n == 0 || ends_in_sep) ? "" : (win ? "\\" : "/"), base);
}

/* "file" (under @p file_key) and "folder" for @p path: the folder the file is in (@p dir for a bare name), canonical,
   or as given when it cannot be resolved; the file relative to the journal's folder when it lies inside it, else the
   folder plus the file's base name. */
static void
line_file_and_folder(wav_journal_line* l, const char* file_key, const char* path, const char* dir) {
    path = path ? path : "";
    const char* base = wav_journal_path_base(path, WAV_JOURNAL_WIN_PATHS);
    char given[1024];
    if (base != path) {
        wav_journal_path_folder(path, WAV_JOURNAL_WIN_PATHS, given, sizeof given);
    } else {
        (void)DSD_SNPRINTF(given, sizeof given, "%s", dir ? dir : "");
    }
    char folder[1024];
    if (given[0] == '\0' || dsd_path_canonical(given, folder, sizeof folder) != 0) {
        (void)DSD_SNPRINTF(folder, sizeof folder, "%s", given);
    }
    char file[3200];
    wav_journal_path_name(s_dir, folder, base, WAV_JOURNAL_WIN_PATHS, file, sizeof file);
    line_key_string(l, file_key, file);
    line_key_string(l, "folder", folder);
}

/* --- Pending recordings. --- */

/* Room for one more entry in @p slot's list: 0 when there is, -1 when it could not grow. */
static int
pending_reserve_locked(uint8_t slot) {
    wav_journal_pending_list* list = &s_pending[slot];
    if (list->count < list->cap) {
        return 0;
    }
    if (s_test_pending_growth_fails) {
        return -1;
    }
    const size_t cap = list->cap ? list->cap * 2 : 4;
    wav_journal_pending* items = (wav_journal_pending*)realloc(list->items, cap * sizeof(*items));
    if (!items) {
        return -1;
    }
    list->items = items;
    list->cap = cap;
    return 0;
}

/* After pending_reserve_locked() made room. */
static void
pending_add_locked(uint8_t slot, const char* id, uint64_t epoch) {
    wav_journal_pending_list* list = &s_pending[slot];
    if (list->count >= list->cap) {
        return;
    }
    wav_journal_pending* entry = &list->items[list->count++];
    (void)DSD_SNPRINTF(entry->id, sizeof entry->id, "%s", id);
    entry->epoch = epoch;
}

static void
write_link_locked(const wav_journal_pending* entry, uint8_t slot, uint64_t instance, uint64_t stamp) {
    wav_journal_line l;
    line_begin(&l, "link", entry->id);
    line_key_hex64(&l, "instance", instance);
    line_key_u64(&l, "slot", slot);
    line_key_u64(&l, "row", stamp);
    line_end_and_write_locked(&l);
}

static void
write_unlink_locked(const wav_journal_pending* entry) {
    wav_journal_line l;
    line_begin(&l, "unlink", entry->id);
    line_end_and_write_locked(&l);
}

typedef enum { PENDING_LINK, PENDING_UNLINK_EPOCH, PENDING_UNLINK_OTHERS } pending_action;

/* Resolve the slot's entries @p action picks, in the order they were published, and keep the rest. A write that fails
   clears every list (wav_journal_fail_locked()), which ends the walk. */
static void
pending_resolve(uint8_t slot, pending_action action, uint64_t epoch, uint64_t instance, uint64_t stamp) {
    if (slot >= WAV_JOURNAL_SLOTS || !wav_journal_active()) {
        return;
    }
    wav_journal_lock();
    wav_journal_pending_list* list = &s_pending[slot];
    size_t kept = 0;
    for (size_t i = 0; i < list->count && wav_journal_active(); i++) {
        const wav_journal_pending entry = list->items[i];
        const int same_epoch = entry.epoch == epoch;
        const int picked = (action == PENDING_UNLINK_OTHERS) ? !same_epoch : same_epoch;
        if (!picked) {
            list->items[kept++] = entry;
        } else if (action == PENDING_LINK) {
            write_link_locked(&entry, slot, instance, stamp);
        } else {
            write_unlink_locked(&entry);
        }
    }
    if (wav_journal_active()) {
        list->count = kept;
    }
    wav_journal_unlock();
}

void
wav_journal_link_pending(uint8_t slot, uint64_t epoch, uint64_t instance, uint64_t stamp) {
    pending_resolve(slot, PENDING_LINK, epoch, instance, stamp);
}

void
wav_journal_unlink_pending_epoch(uint8_t slot, uint64_t epoch) {
    pending_resolve(slot, PENDING_UNLINK_EPOCH, epoch, 0, 0);
}

void
wav_journal_unlink_pending_except(uint8_t slot, uint64_t epoch) {
    pending_resolve(slot, PENDING_UNLINK_OTHERS, epoch, 0, 0);
}

void
dsd_wav_journal_unlink_pending(void) {
    for (uint8_t slot = 0; slot < WAV_JOURNAL_SLOTS; slot++) {
        wav_journal_unlink_pending_except(slot, 0);
    }
}

/* --- Records. --- */

void
wav_journal_publish(const wav_journal_publish_record* rec) {
    if (!rec || !wav_journal_active()) {
        return;
    }
    wav_journal_lock();
    if (!wav_journal_active()) {
        wav_journal_unlock();
        return;
    }
    /* A recording is published pending only when its slot's list holds it: every pending line is resolved later by a
       link or an unlink. One without an id (a temp name open_wav_file() did not make), a slot, or room in the list
       (out of memory) is published as no row's instead. */
    const uint8_t slot = rec->owner.slot;
    int pending = rec->owner.kind == DSD_WAV_OWNER_PENDING;
    if (pending && (!rec->id || rec->id[0] == '\0' || slot >= WAV_JOURNAL_SLOTS || pending_reserve_locked(slot) != 0)) {
        pending = 0;
    }
    wav_journal_line l;
    line_begin(&l, "publish", rec->id);
    line_file_and_folder(&l, "file", rec->path, rec->dir);
    line_key_i64(&l, "start", (int64_t)rec->start);
    line_key_i64(&l, "duration_ms", rec->duration_ms);
    line_key_i64(&l, "rate", rec->rate);
    if (rec->has_instance) {
        line_key_hex64(&l, "instance", rec->instance);
    } else {
        line_key(&l, "instance");
        line_raw(&l, "null");
    }
    if (rec->owner.slot < WAV_JOURNAL_SLOTS) {
        line_key_u64(&l, "slot", rec->owner.slot);
    } else {
        line_key(&l, "slot");
        line_raw(&l, "null");
    }
    if (rec->owner.kind == DSD_WAV_OWNER_ROW) {
        line_key_u64(&l, "row", rec->owner.stamp);
    } else {
        line_key(&l, "row");
        line_raw(&l, "null");
    }
    line_key_bool(&l, "pending", pending);
    line_key_u64(&l, "epoch", pending ? rec->owner.epoch : 0U);
    line_key_bool(&l, "rdio", rec->rdio_queued);
    line_key_bool(&l, "failed", rec->failed);
    line_key_string(&l, "system", rec->system);
    line_key_string(&l, "target", rec->target);
    line_key_string(&l, "source", rec->source);
    line_end_and_write_locked(&l);
    if (pending && wav_journal_active()) {
        pending_add_locked(slot, rec->id, rec->owner.epoch);
    }
    wav_journal_unlock();
}

void
wav_journal_discard(const char* id) {
    if (!wav_journal_active()) {
        return;
    }
    wav_journal_lock();
    if (wav_journal_active()) {
        wav_journal_line l;
        line_begin(&l, "discard", id);
        line_end_and_write_locked(&l);
    }
    wav_journal_unlock();
}

void
wav_journal_error(const char* id, const char* kind, const char* temp_path, const char* dir) {
    if (!wav_journal_active()) {
        return;
    }
    wav_journal_lock();
    if (wav_journal_active()) {
        wav_journal_line l;
        line_begin(&l, "error", id);
        line_key_string(&l, "kind", kind);
        line_file_and_folder(&l, "temp", temp_path, dir);
        line_end_and_write_locked(&l);
    }
    wav_journal_unlock();
}

/* --- Open and close. --- */

/* The canonical folder @p path lies in. */
static int
journal_folder(const char* path, char* out, size_t out_size) {
    char dir[1024];
    wav_journal_path_folder(path, WAV_JOURNAL_WIN_PATHS, dir, sizeof dir);
    if (dir[0] == '\0') {
        (void)DSD_SNPRINTF(dir, sizeof dir, "%s", ".");
    }
    return dsd_path_canonical(dir, out, out_size);
}

/* A journal whose last byte is not a newline ends in a torn line: a newline first, so the next record starts a line of
   its own. */
static int
journal_end_torn_line_locked(void) {
    if (dsd_fseek64(s_fp, 0, SEEK_END) != 0) {
        return -1;
    }
    const int64_t size = dsd_ftell64(s_fp);
    if (size <= 0) {
        return size == 0 ? 0 : -1;
    }
    if (dsd_fseek64(s_fp, -1, SEEK_END) != 0) {
        return -1;
    }
    const int last = fgetc(s_fp);
    /* An update stream must seek between a read and a write; append mode writes at the end whatever the position. */
    if (dsd_fseek64(s_fp, 0, SEEK_END) != 0) {
        return -1;
    }
    if (last == '\n') {
        return 0;
    }
    return wav_journal_write_locked("\n", 1);
}

int
dsd_wav_journal_open(const char* path) {
    wav_journal_lock();
    wav_journal_close_locked(DSD_WAV_JOURNAL_OFF);
    if (!path || path[0] == '\0') {
        s_path[0] = '\0';
        wav_journal_unlock();
        return 0;
    }
    (void)DSD_SNPRINTF(s_path, sizeof s_path, "%s", path);
    s_fp = dsd_fopen_private(path, "a+b");
    if (!s_fp || journal_folder(path, s_dir, sizeof s_dir) != 0) {
        LOG_ERROR("Call recording journal %s did not open; no records are written this run\n", path);
        wav_journal_close_locked(DSD_WAV_JOURNAL_FAILED);
        wav_journal_unlock();
        return -1;
    }
    atomic_store(&s_state, DSD_WAV_JOURNAL_ON);
    /* A failure here latched the journal off and logged it. */
    if (journal_end_torn_line_locked() != 0 && wav_journal_active()) {
        wav_journal_fail_locked("reading its end");
    }
    const int rc = wav_journal_active() ? 0 : -1;
    wav_journal_unlock();
    return rc;
}

void
dsd_wav_journal_close(void) {
    dsd_wav_journal_unlink_pending();
    wav_journal_lock();
    /* A failure stays reported until the next run opens (or turns off) its journal. */
    wav_journal_close_locked(dsd_wav_journal_state_get() == DSD_WAV_JOURNAL_FAILED ? DSD_WAV_JOURNAL_FAILED
                                                                                   : DSD_WAV_JOURNAL_OFF);
    wav_journal_unlock();
}
