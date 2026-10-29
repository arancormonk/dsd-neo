// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #708: the recording journal (`--wav-journal`) through the real queue, event layer, writers and trunk scan
 * coordinator: which records each per-call WAV gets (publish, discard, error), the row a recording is linked to on the
 * commit, merge and drop paths, the pending recordings closed before their call's row and how each resolves, the start
 * time the records carry, and the journal's own failure and torn-tail handling.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/recording_view.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/wav_journal.h>
#include <dsd-neo/core/wav_recording.h>
#include <dsd-neo/engine/analog_call.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "commands_internal.h"
#include "edacs_internal.h"
#include "snapshot_internal.h"
#include "test_support.h"
#include "trunk_scan_internal.h"
#include "trunk_scan_test_support.h"
#include "wav_journal.h"
#include "wav_journal_test_support.h"
#include "wav_segment_test_support.h"

static int
expect_true(const char* what, int ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

static int
expect_int(const char* what, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got %lld, want %lld\n", what, got, want);
        return 1;
    }
    return 0;
}

static int
expect_str(const char* what, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: got \"%s\", want \"%s\"\n", what, got, want);
        return 1;
    }
    return 0;
}

static int g_journal_errors;

static void
count_log_lines(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    if (level == LOG_LEVEL_ERROR && text && strstr(text, "Call recording journal") != NULL) {
        g_journal_errors++;
    }
}

/* --- The journal, read back. --- */

enum { MAX_LINES = 64, LINE_CAP = 4096 };

typedef struct {
    int count;
    char line[MAX_LINES][LINE_CAP];
    int torn; /* the file does not end in a newline */
} journal;

static journal g_j;

static void
read_journal(const char* path, journal* j) {
    DSD_MEMSET(j, 0, sizeof(*j));
    FILE* f = dsd_fopen_existing_regular_file(path, "rb");
    if (!f) {
        return;
    }
    static char buf[MAX_LINES * LINE_CAP];
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    j->torn = n > 0 && buf[n - 1] != '\n';
    const char* p = buf;
    while (*p && j->count < MAX_LINES) {
        const char* nl = strchr(p, '\n');
        const size_t len = nl ? (size_t)(nl - p) : strlen(p);
        DSD_SNPRINTF(j->line[j->count], LINE_CAP, "%.*s", (int)len, p);
        j->count++;
        p = nl ? nl + 1 : p + len;
    }
}

/* The raw JSON value of @p key in @p line: a string's body (escapes kept), a number, true, false or null. */
static int
field(const char* line, const char* key, char* out, size_t cap) {
    char pat[64];
    DSD_SNPRINTF(pat, sizeof pat, "\"%s\":", key);
    out[0] = '\0';
    const char* p = strstr(line, pat);
    if (!p) {
        return 0;
    }
    p += strlen(pat);
    size_t n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n + 2 < cap) {
            if (*p == '\\' && p[1]) {
                out[n++] = *p++;
            }
            out[n++] = *p++;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && n + 1 < cap) {
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
    return 1;
}

static long long
field_ll(const char* line, const char* key) {
    char v[64];
    if (!field(line, key, v, sizeof v) || v[0] == '\0' || strcmp(v, "null") == 0) {
        return -1;
    }
    char* end = NULL;
    const long long x = strtoll(v, &end, 10);
    return (end && *end == '\0') ? x : -2;
}

static const char*
field_s(const char* line, const char* key) {
    static char v[4][LINE_CAP];
    static int next;
    char* out = v[next++ & 3];
    (void)field(line, key, out, LINE_CAP);
    return out;
}

/* A segment id as the journal writes it: 16 uppercase hex digits, '-', a decimal sequence. */
static int
is_segment_id(const char* id) {
    const size_t len = strlen(id);
    if (len < 18 || id[16] != '-') {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = id[i];
        const int ok = i < 16 ? ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) : i == 16 || (c >= '0' && c <= '9');
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

/* The @p nth (0-based) record of @p type, or NULL. */
static const char*
record(const journal* j, const char* type, int nth) {
    char pat[48];
    DSD_SNPRINTF(pat, sizeof pat, "\"type\":\"%s\"", type);
    for (int i = 0; i < j->count; i++) {
        if (strstr(j->line[i], pat) != NULL && nth-- == 0) {
            return j->line[i];
        }
    }
    return NULL;
}

static int
count_records(const journal* j, const char* type) {
    int n = 0;
    while (record(j, type, n) != NULL) {
        n++;
    }
    return n;
}

/* Records of @p type naming @p id. */
static int
count_for_id(const journal* j, const char* type, const char* id) {
    int n = 0;
    const char* line = NULL;
    for (int i = 0; (line = record(j, type, i)) != NULL; i++) {
        if (strcmp(field_s(line, "id"), id) == 0) {
            n++;
        }
    }
    return n;
}

/* The journal id a per-call temp name stands for: TEMP_<date>_<time>_<token>_<seq> gives "<token>-<seq>". */
static void
id_of_temp(const char* temp, char* out, size_t cap) {
    out[0] = '\0';
    const char* seq = temp ? strrchr(temp, '_') : NULL;
    if (seq && seq - temp >= 17) {
        DSD_SNPRINTF(out, cap, "%.16s-%s", seq - 16, seq + 1);
    }
}

static void
hex64(uint64_t v, char* out, size_t cap) {
    DSD_SNPRINTF(out, cap, "%016llX", (unsigned long long)v);
}

/* --- The rig: options, state, a folder holding the recordings and the journal, a frozen decode clock. --- */

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    char dir[DSD_TEST_PATH_MAX];
    char canon[DSD_TEST_PATH_MAX];
    char journal[DSD_TEST_PATH_MAX * 2];
} rig;

static const uint64_t k_t0 = 1700000000ULL; /* the rig's decode clock at start, s */
static uint64_t g_now_s;

static void
clock_at(uint64_t s) {
    g_now_s = s;
    dsd_decode_clock_test_set_ns(s * 1000000000ULL);
}

/* @p ms past second @p s, for gaps the reacquisition window measures. */
static void
clock_at_ms(uint64_t s, uint64_t ms) {
    g_now_s = s;
    dsd_decode_clock_test_set_ns(s * 1000000000ULL + ms * 1000000ULL);
}

static int
rig_open_ex(rig* r, const char* prefix, int with_journal) {
    DSD_MEMSET(r, 0, sizeof(*r));
    if (!dsd_test_mkdtemp(r->dir, sizeof r->dir, prefix)) {
        DSD_FPRINTF(stderr, "FAIL: temp dir for %s\n", prefix);
        exit(2);
    }
    (void)dsd_path_canonical(r->dir, r->canon, sizeof r->canon);
    r->opts = (dsd_opts*)calloc(1, sizeof(*r->opts));
    r->state = (dsd_state*)calloc(1, sizeof(*r->state));
    if (!r->opts || !r->state) {
        DSD_FPRINTF(stderr, "FAIL: alloc for %s\n", prefix);
        exit(2);
    }
    dsd_decode_clock_use_test(k_t0 * 1000000000ULL);
    g_now_s = k_t0;
    initOpts(r->opts);
    initState(r->state);
    DSD_SNPRINTF(r->opts->wav_out_dir, sizeof r->opts->wav_out_dir, "%s", r->dir);
    (void)dsd_test_path_join(r->journal, sizeof r->journal, r->dir, "calls.jsonl");
    dsd_wav_recording_session_reset();
    dsd_wav_journal_test_fail_writes_after(-1);
    g_journal_errors = 0;
    int rc = 0;
    if (with_journal) {
        rc |= expect_int("journal opens", dsd_wav_journal_open(r->journal), 0);
    }
    dsd_app_frontend_runtime_start(r->opts, r->state);
    return rc;
}

static int
rig_open(rig* r, const char* prefix) {
    return rig_open_ex(r, prefix, 1);
}

static int
remove_file_cb(const char* name, void* user) {
    char path[DSD_TEST_PATH_MAX * 2];
    if (dsd_test_path_join(path, sizeof path, (const char*)user, name) == 0) {
        (void)remove(path);
    }
    return 0;
}

static void
rig_close(rig* r) {
    dsd_app_frontend_runtime_stop();
    dsd_wav_journal_close();
    dsd_wav_journal_test_fail_writes_after(-1);
    if (r->opts) {
        r->opts->wav_out_f = close_wav_file(r->opts->wav_out_f);
        r->opts->wav_out_fR = close_wav_file(r->opts->wav_out_fR);
    }
    if (r->state) {
        freeState(r->state);
    }
    free(r->state);
    free(r->opts);
    (void)dsd_dir_list(r->dir, remove_file_cb, r->dir);
    (void)dsd_test_rmdir(r->dir);
    dsd_decode_clock_use_system();
}

static void
submit(rig* r, int id) {
    (void)dsd_app_command_submit(id, NULL, 0);
    (void)dsd_app_drain_cmds(r->opts, r->state);
}

static void
reload(const rig* r) {
    read_journal(r->journal, &g_j);
}

static void
voice_frames(rig* r, int slot, int frames) {
    for (int f = 0; f < frames; f++) {
        for (int i = 0; i < 160; i++) {
            r->state->audio_out_temp_buf[i] = (float)((i % 32) * 512 - 8192);
        }
        dsd_write_mono_voice_to_slot_wav(r->opts, r->state, slot);
    }
}

static void
begin_call(rig* r, int protocol, uint32_t tg, uint32_t src, dsd_call_boundary boundary) {
    const dsd_call_observation call = {.protocol = protocol,
                                       .slot = 0U,
                                       .kind = DSD_CALL_KIND_GROUP_VOICE,
                                       .ota_target_id = tg,
                                       .policy_target_id = tg,
                                       .ota_source_id = src};
    (void)dsd_call_state_observe(r->state, &call, boundary);
    dsd_event_sync_slot(r->opts, r->state, 0);
}

static void
end_call(rig* r, dsd_call_end_reason reason) {
    (void)dsd_call_state_end_ex(r->state, 0, 0.0, reason);
    dsd_event_sync_slot(r->opts, r->state, 0);
}

static uint64_t
call_epoch(const rig* r) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof call);
    (void)dsd_call_state_get(r->state, 0, &call);
    return call.epoch;
}

/* The temp name slot 1 records under now, and its journal id. */
static void
slot1_id(const rig* r, char* id, size_t cap) {
    id_of_temp(r->opts->wav_out_file, id, cap);
}

static int expect_pending_then_linked(const char* what, const char* id, uint64_t epoch, const Event_History_I* ring,
                                      uint64_t row);

/* --- Tests. --- */

/* A call's end commits its row and publishes the slot's recording under it: every publish field, the row stamp the
   pushed row has, and the start of a call that began after the file opened (idle before the first call). */
static int
test_commit_publishes_under_the_pushed_row(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_commit");
    submit(&r, DSD_APP_CMD_WAV_START);
    char id[64];
    slot1_id(&r, id, sizeof id);
    clock_at(k_t0 + 100);
    begin_call(&r, DSD_SYNC_P25P1_POS, 100U, 77U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 3);
    clock_at(k_t0 + 105);
    end_call(&r, DSD_CALL_END_EXPLICIT);

    reload(&r);
    const Event_History_I* ring = &r.state->event_history_s[0];
    const char* pub = record(&g_j, "publish", 0);
    rc |= expect_int("one publish", count_records(&g_j, "publish"), 1);
    rc |= expect_true("the journal holds whole lines", !g_j.torn);
    if (!pub) {
        rig_close(&r);
        return rc | 1;
    }
    char hex[24];
    hex64(ring->instance, hex, sizeof hex);
    rc |= expect_true("every record starts with its version", strncmp(pub, "{\"v\":1,", 7) == 0);
    rc |= expect_str("the id is the temp name's token and sequence", field_s(pub, "id"), id);
    rc |= expect_int("stamped on the decode clock", field_ll(pub, "t"), (long long)k_t0 + 105);
    rc |= expect_int("the row is the pushed row's stamp", field_ll(pub, "row"), (long long)ring->push_seq);
    rc |= expect_true("that row is the call's", ring->Event_History_Items[1].target_id == 100U);
    rc |= expect_str("the ring instance", field_s(pub, "instance"), hex);
    rc |= expect_int("slot 1", field_ll(pub, "slot"), 0);
    rc |= expect_str("not pending", field_s(pub, "pending"), "false");
    rc |= expect_int("no epoch", field_ll(pub, "epoch"), 0);
    rc |= expect_int("the call's start, not the file's open", field_ll(pub, "start"), (long long)k_t0 + 100);
    rc |= expect_int("three frames of 160 samples at 8 kHz", field_ll(pub, "duration_ms"), 60);
    rc |= expect_int("8 kHz", field_ll(pub, "rate"), 8000);
    rc |= expect_str("no rdio upload", field_s(pub, "rdio"), "false");
    rc |= expect_str("no short write", field_s(pub, "failed"), "false");
    rc |= expect_str("target as shown", field_s(pub, "target"), "100");
    rc |= expect_str("source as shown", field_s(pub, "source"), "77");
    rc |= expect_str("the folder it was published in", field_s(pub, "folder"), r.canon);
    char path[DSD_TEST_PATH_MAX * 2];
    (void)dsd_test_path_join(path, sizeof path, r.dir, field_s(pub, "file"));
    FILE* f = dsd_fopen_existing_regular_file(path, "rb");
    rc |= expect_true("the file is named relative to the journal's folder", f != NULL);
    if (f) {
        fclose(f);
    }
    rc |= expect_true("a final name", strncmp(field_s(pub, "file"), "TEMP_", 5) != 0);
    rc |= expect_int("nothing else", g_j.count, 1);
    rig_close(&r);
    return rc;
}

/* A sync-loss-interrupted call reacquired twice is one row: every segment's recording names it, and the later
   segments start when their files opened (each opened at the previous segment's commit), not at the call's start. */
static int
test_merge_chain_names_the_retained_row(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_merge");
    submit(&r, DSD_APP_CMD_WAV_START);
    clock_at(k_t0 + 10);
    begin_call(&r, DSD_SYNC_P25P1_POS, 200U, 88U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 2);
    clock_at(k_t0 + 12);
    end_call(&r, DSD_CALL_END_SYNC_LOSS);
    const uint64_t row = r.state->event_history_s[0].push_seq;
    /* A notice pushes the call's row one deeper: its stamp, not the ring's newest, is the row. */
    rc |= expect_int("notice pushed", dsd_event_emit_system_notice(r.opts, r.state, 0, "System notice;"), 0);
    time_t staged_start[2] = {0, 0};
    time_t opened[2] = {0, 0};
    for (int seg = 0; seg < 2; seg++) {
        /* Each segment resumes 300 ms after the fade, inside the reacquisition window. */
        opened[seg] = r.opts->wav_out_open_time;
        clock_at_ms(k_t0 + 12 + (uint64_t)seg * 2, 300);
        begin_call(&r, DSD_SYNC_P25P1_POS, 200U, 88U, DSD_CALL_BOUNDARY_CONTINUE);
        voice_frames(&r, 0, 2);
        staged_start[seg] = r.state->event_history_s[0].Event_History_Items[0].event_start_time;
        clock_at(k_t0 + 14 + (uint64_t)seg * 2);
        end_call(&r, DSD_CALL_END_SYNC_LOSS);
    }
    rc |= expect_int("one row for the whole transmission, and the notice",
                     (long long)r.state->event_history_s[0].push_seq, (long long)row + 1);
    reload(&r);
    rc |= expect_int("three segments published", count_records(&g_j, "publish"), 3);
    for (int i = 0; i < 3; i++) {
        const char* pub = record(&g_j, "publish", i);
        rc |= expect_int("each names the retained row", pub ? field_ll(pub, "row") : -9, (long long)row);
    }
    const char* first = record(&g_j, "publish", 0);
    rc |= expect_int("the first starts with the call", first ? field_ll(first, "start") : -9, (long long)k_t0 + 10);
    for (int seg = 0; seg < 2; seg++) {
        const char* pub = record(&g_j, "publish", seg + 1);
        const time_t want = opened[seg] > staged_start[seg] ? opened[seg] : staged_start[seg];
        rc |= expect_int("a reacquired segment starts at max(open, its row's start)", pub ? field_ll(pub, "start") : -9,
                         (long long)want);
        rc |= expect_true("not at the call's start", pub && field_ll(pub, "start") > (long long)k_t0 + 10);
    }
    rc |= expect_int("no pending records", count_records(&g_j, "link") + count_records(&g_j, "unlink"), 0);
    rig_close(&r);
    return rc;
}

/* A recording a stop closed during a reacquired segment waits on that segment's epoch, and the merge links it to the
   row the transmission already has, as it does the segment's rotated recording. */
static int
test_merge_links_pending_to_the_retained_row(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_merge_pending");
    submit(&r, DSD_APP_CMD_WAV_START);
    begin_call(&r, DSD_SYNC_P25P1_POS, 210U, 89U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 2);
    clock_at(k_t0 + 2);
    end_call(&r, DSD_CALL_END_SYNC_LOSS);
    const uint64_t row = r.state->event_history_s[0].push_seq;
    rc |= expect_int("notice pushed", dsd_event_emit_system_notice(r.opts, r.state, 0, "System notice;"), 0);
    clock_at_ms(k_t0 + 2, 300);
    begin_call(&r, DSD_SYNC_P25P1_POS, 210U, 89U, DSD_CALL_BOUNDARY_CONTINUE);
    const uint64_t epoch = call_epoch(&r);
    voice_frames(&r, 0, 2);
    char id[64];
    slot1_id(&r, id, sizeof id);
    submit(&r, DSD_APP_CMD_WAV_STOP);
    submit(&r, DSD_APP_CMD_WAV_START);
    voice_frames(&r, 0, 2);
    clock_at(k_t0 + 4);
    end_call(&r, DSD_CALL_END_SYNC_LOSS);
    rc |= expect_int("one row, and the notice", (long long)r.state->event_history_s[0].push_seq, (long long)row + 1);
    reload(&r);
    rc |= expect_pending_then_linked("merge", id, epoch, &r.state->event_history_s[0], row);
    const char* rotated = record(&g_j, "publish", 2);
    rc |= expect_int("the reacquired segment's rotation names the retained row",
                     rotated ? field_ll(rotated, "row") : -9, (long long)row);
    rig_close(&r);
    return rc;
}

/* An identity-less voice epoch is dropped: its rotation publishes no row's recording, and the recording a stop closed
   earlier in the epoch (pending on it) is unlinked. */
static int
test_drop_unlinks_the_epoch_recordings(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_drop");
    submit(&r, DSD_APP_CMD_WAV_START);
    begin_call(&r, DSD_SYNC_P25P1_POS, 0U, 0U, DSD_CALL_BOUNDARY_BEGIN);
    const uint64_t epoch = call_epoch(&r);
    voice_frames(&r, 0, 2);
    char pending_id[64];
    slot1_id(&r, pending_id, sizeof pending_id);
    submit(&r, DSD_APP_CMD_WAV_STOP);
    submit(&r, DSD_APP_CMD_WAV_START);
    voice_frames(&r, 0, 2);
    char rotated_id[64];
    slot1_id(&r, rotated_id, sizeof rotated_id);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    rc |= expect_int("no row", (long long)r.state->event_history_s[0].push_seq, 0);

    reload(&r);
    const char* pending = record(&g_j, "publish", 0);
    const char* rotated = record(&g_j, "publish", 1);
    rc |=
        expect_true("the stop's recording published pending", pending && strcmp(field_s(pending, "id"), pending_id) == 0
                                                                  && strcmp(field_s(pending, "pending"), "true") == 0);
    rc |= expect_int("on the call's epoch", pending ? field_ll(pending, "epoch") : -9, (long long)epoch);
    rc |= expect_true("pending names no row", pending && strcmp(field_s(pending, "row"), "null") == 0);
    rc |= expect_true("the dropped epoch's rotation names no row",
                      rotated && strcmp(field_s(rotated, "id"), rotated_id) == 0
                          && strcmp(field_s(rotated, "row"), "null") == 0
                          && strcmp(field_s(rotated, "pending"), "false") == 0);
    rc |= expect_int("the pending recording unlinked", count_for_id(&g_j, "unlink", pending_id), 1);
    rc |= expect_int("and never linked", count_records(&g_j, "link"), 0);
    rig_close(&r);
    return rc;
}

/* A segment with no audio (a 44-byte WAV) is deleted, and the journal says so under the id its open had. */
static int
test_empty_segment_is_discarded(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_discard");
    submit(&r, DSD_APP_CMD_WAV_START);
    char id[64];
    slot1_id(&r, id, sizeof id);
    begin_call(&r, DSD_SYNC_P25P1_POS, 300U, 1U, DSD_CALL_BOUNDARY_BEGIN);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    rc |= expect_int("nothing published", count_records(&g_j, "publish"), 0);
    rc |= expect_int("the segment discarded", count_for_id(&g_j, "discard", id), 1);
    rig_close(&r);
    return rc;
}

/* A rename that fails keeps the temp file, and the journal records the error with the temp name and its folder. */
static int
test_rename_failure_is_an_error_record(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_rename");
    submit(&r, DSD_APP_CMD_WAV_START);
    char id[64];
    slot1_id(&r, id, sizeof id);
    char temp[1024];
    DSD_SNPRINTF(temp, sizeof temp, "%s", r.opts->wav_out_file);
    begin_call(&r, DSD_SYNC_P25P1_POS, 301U, 2U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 2);
    char missing[DSD_TEST_PATH_MAX * 2];
    (void)dsd_test_path_join(missing, sizeof missing, r.dir, "missing-folder");
    DSD_SNPRINTF(r.opts->wav_out_dir, sizeof r.opts->wav_out_dir, "%s", missing);
    submit(&r, DSD_APP_CMD_WAV_STOP);
    reload(&r);
    const char* err = record(&g_j, "error", 0);
    rc |= expect_true("one error for slot 1's recording", err && strcmp(field_s(err, "id"), id) == 0);
    rc |= expect_str("a rename failure", err ? field_s(err, "kind") : "", "publish");
    rc |= expect_str("the temp name it keeps", err ? field_s(err, "temp") : "", temp + strlen(r.dir) + 1);
    rc |= expect_str("in the folder it was recorded in", err ? field_s(err, "folder") : "", r.canon);
    FILE* f = dsd_fopen_existing_regular_file(temp, "rb");
    rc |= expect_true("the temp file is kept", f != NULL);
    if (f) {
        fclose(f);
    }
    rc |= expect_int("nothing published for it", count_for_id(&g_j, "publish", id), 0);
    DSD_SNPRINTF(r.opts->wav_out_dir, sizeof r.opts->wav_out_dir, "%s", r.dir);
    rig_close(&r);
    return rc;
}

/* B3: a callsign with a '/' names the file with '_' in its place, and the journal carries the file it was published
   under and the source as received. */
static int
test_b3_name_and_raw_identity(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_b3");
    submit(&r, DSD_APP_CMD_WAV_START);
    dsd_call_observation call = {.protocol = DSD_SYNC_M17_STR_POS, .slot = 0U, .kind = DSD_CALL_KIND_GROUP_VOICE};
    DSD_SNPRINTF(call.source_text, sizeof call.source_text, "%s", "AB1CD/P");
    DSD_SNPRINTF(call.target_text, sizeof call.target_text, "%s", "ALL");
    (void)dsd_call_state_observe(r.state, &call, DSD_CALL_BOUNDARY_BEGIN);
    dsd_event_sync_slot(r.opts, r.state, 0);
    voice_frames(&r, 0, 2);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    const char* pub = record(&g_j, "publish", 0);
    rc |= expect_true("published", pub != NULL);
    if (pub) {
        rc |= expect_str("the source as received", field_s(pub, "source"), "AB1CD/P");
        rc |= expect_str("the target as received", field_s(pub, "target"), "ALL");
        const char* file = field_s(pub, "file");
        rc |= expect_true("the file name holds the sanitized callsign", strstr(file, "SRC_AB1CD_P.wav") != NULL);
        rc |= expect_true("and no folder", strchr(file, '/') == NULL);
        char path[DSD_TEST_PATH_MAX * 2];
        (void)dsd_test_path_join(path, sizeof path, r.dir, file);
        FILE* f = dsd_fopen_existing_regular_file(path, "rb");
        rc |= expect_true("which exists", f != NULL);
        if (f) {
            fclose(f);
        }
    }
    rig_close(&r);
    return rc;
}

static void
setup_nfm(dsd_opts* opts, dsd_state* state) {
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->audio_out = 0;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    state->analog_rx.carrier_open = 1;
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    state->analog_rx.ctcss_tenths_hz = 1413;
}

static void
nfm_block(dsd_opts* opts, dsd_state* state) {
    static float block[960];
    for (int i = 0; i < 960; i++) {
        block[i] = (float)((i % 48) * 400 - 9600);
    }
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
}

/* The pending publish of @p id on @p epoch, then exactly one link of it to @p row in @p ring, slot 1. */
static int
expect_pending_then_linked(const char* what, const char* id, uint64_t epoch, const Event_History_I* ring,
                           uint64_t row) {
    char tag[160];
    int rc = 0;
    const char* pub = NULL;
    const char* line = NULL;
    for (int i = 0; (line = record(&g_j, "publish", i)) != NULL; i++) {
        if (strcmp(field_s(line, "id"), id) == 0) {
            pub = line;
        }
    }
    DSD_SNPRINTF(tag, sizeof tag, "%s: published pending", what);
    rc |= expect_true(tag, pub && strcmp(field_s(pub, "pending"), "true") == 0);
    DSD_SNPRINTF(tag, sizeof tag, "%s: on the call's epoch", what);
    rc |= expect_int(tag, pub ? field_ll(pub, "epoch") : -9, (long long)epoch);
    DSD_SNPRINTF(tag, sizeof tag, "%s: linked once", what);
    rc |= expect_int(tag, count_for_id(&g_j, "link", id), 1);
    DSD_SNPRINTF(tag, sizeof tag, "%s: never unlinked", what);
    rc |= expect_int(tag, count_for_id(&g_j, "unlink", id), 0);
    const char* link = record(&g_j, "link", 0);
    char hex[24];
    hex64(ring->instance, hex, sizeof hex);
    DSD_SNPRINTF(tag, sizeof tag, "%s: to the row the call committed", what);
    rc |= expect_int(tag, link ? field_ll(link, "row") : -9, (long long)row);
    DSD_SNPRINTF(tag, sizeof tag, "%s: in this ring, slot 1", what);
    rc |= expect_true(tag, link && strcmp(field_s(link, "instance"), hex) == 0 && field_ll(link, "slot") == 0);
    return rc;
}

/* The closers that run before the call's row commits publish pending on the call's epoch, and that epoch's commit
   links them: an NFM call's end, a WAV stop mid-call, and the EDACS analog rotation (as a grant closes slot 1's file
   once it synced the call). */
static int
test_pending_closers_are_linked_at_the_commit(void) {
    int rc = 0;
    {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_nfm");
        setup_nfm(r.opts, r.state);
        submit(&r, DSD_APP_CMD_WAV_START);
        nfm_block(r.opts, r.state);
        nfm_block(r.opts, r.state);
        const uint64_t epoch = call_epoch(&r);
        char id[64] = {0};
        dsd_engine_analog_call_end(r.opts, r.state);
        reload(&r);
        const char* pub = NULL;
        for (int i = 0; (pub = record(&g_j, "publish", i)) != NULL; i++) {
            if (field_ll(pub, "rate") == 48000) {
                DSD_SNPRINTF(id, sizeof id, "%s", field_s(pub, "id"));
                rc |= expect_int("nfm: two 20 ms blocks", field_ll(pub, "duration_ms"), 40);
                rc |= expect_true("nfm: published under the NFM name", strstr(field_s(pub, "file"), "_NFM") != NULL);
            }
        }
        rc |= expect_true("nfm: the call's recording published", id[0] != '\0');
        rc |= expect_pending_then_linked("nfm", id, epoch, &r.state->event_history_s[0],
                                         r.state->event_history_s[0].push_seq);
        rig_close(&r);
    }
    {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_stop");
        submit(&r, DSD_APP_CMD_WAV_START);
        begin_call(&r, DSD_SYNC_P25P1_POS, 400U, 9U, DSD_CALL_BOUNDARY_BEGIN);
        const uint64_t epoch = call_epoch(&r);
        voice_frames(&r, 0, 2);
        char id[64];
        slot1_id(&r, id, sizeof id);
        submit(&r, DSD_APP_CMD_WAV_STOP);
        end_call(&r, DSD_CALL_END_EXPLICIT);
        reload(&r);
        rc |= expect_pending_then_linked("wav stop", id, epoch, &r.state->event_history_s[0],
                                         r.state->event_history_s[0].push_seq);
        rig_close(&r);
    }
    {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_edacs");
        submit(&r, DSD_APP_CMD_WAV_START);
        begin_call(&r, DSD_SYNC_EDACS_POS, 321U, 1234U, DSD_CALL_BOUNDARY_BEGIN);
        const uint64_t epoch = call_epoch(&r);
        static short analog[3][960];
        for (int i = 0; i < 960; i++) {
            analog[0][i] = (short)(i * 8);
            analog[1][i] = (short)(-i * 8);
            analog[2][i] = (short)(i * 4);
        }
        edacs_write_analog_wav(r.opts, r.state, analog[0], analog[1], analog[2]);
        char id[64];
        slot1_id(&r, id, sizeof id);
        r.opts->wav_out_f = close_and_publish_call_wav(r.opts->wav_out_f, r.opts, r.state, 0, r.opts->wav_out_file,
                                                       r.opts->wav_out_dir, r.opts->wav_out_open_time);
        end_call(&r, DSD_CALL_END_EXPLICIT);
        reload(&r);
        rc |= expect_pending_then_linked("edacs rotation", id, epoch, &r.state->event_history_s[0],
                                         r.state->event_history_s[0].push_seq);
        rig_close(&r);
    }
    return rc;
}

/* A pending recording whose epoch moves on without committing its row is unlinked, and the next epoch's commit does
   not claim it. */
static int
test_epoch_moving_on_unlinks(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_moved_on");
    submit(&r, DSD_APP_CMD_WAV_START);
    begin_call(&r, DSD_SYNC_P25P1_POS, 500U, 5U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 2);
    char id[64];
    slot1_id(&r, id, sizeof id);
    submit(&r, DSD_APP_CMD_WAV_STOP);
    /* The staged row is consumed without a commit, so nothing commits the outgoing epoch when the next one begins. */
    init_event_history(&r.state->event_history_s[0], 0, 1);
    begin_call(&r, DSD_SYNC_P25P1_POS, 501U, 6U, DSD_CALL_BOUNDARY_BEGIN);
    reload(&r);
    rc |= expect_int("unlinked when the next epoch begins", count_for_id(&g_j, "unlink", id), 1);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    rc |= expect_int("the next epoch's commit does not claim it", count_for_id(&g_j, "link", id), 0);
    rc |= expect_int("unlinked once", count_for_id(&g_j, "unlink", id), 1);
    rig_close(&r);
    return rc;
}

/* A full history reset takes the rows pending recordings wait for: they are unlinked, and the call's later commit
   does not claim them. */
static int
test_full_reset_unlinks_pending(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_reset");
    submit(&r, DSD_APP_CMD_WAV_START);
    begin_call(&r, DSD_SYNC_P25P1_POS, 600U, 7U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 2);
    char id[64];
    slot1_id(&r, id, sizeof id);
    submit(&r, DSD_APP_CMD_WAV_STOP);
    dsd_event_history_reset(r.state);
    reload(&r);
    rc |= expect_int("unlinked by the reset", count_for_id(&g_j, "unlink", id), 1);
    begin_call(&r, DSD_SYNC_P25P1_POS, 600U, 7U, DSD_CALL_BOUNDARY_CONTINUE);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    rc |= expect_int("the call's later commit does not claim it", count_for_id(&g_j, "link", id), 0);
    rc |= expect_int("unlinked once", count_for_id(&g_j, "unlink", id), 1);
    rig_close(&r);
    return rc;
}

/* --- Trunk scan: one tuner, two targets, the per-target call context restored at each hop. --- */

static long g_frequency;

static long
current_frequency(const dsd_opts* opts) {
    (void)opts;
    return g_frequency;
}

static dsd_trunk_tune_result
tune(dsd_opts* opts, dsd_state* state, long freq, int sps, uint64_t request_id) {
    (void)sps;
    (void)request_id;
    g_frequency = freq;
    opts->rtlsdr_center_freq = (uint32_t)freq;
    state->trunk_cc_freq = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

/* A departure ends the outgoing target's calls, which resolves the recordings waiting for their rows: here an
   identity-less epoch that faded, whose held row the departure drops, so its recording is unlinked by the time the
   scan is on B. B's commit does not claim it, and A's row, restored on the revisit and resolved again, neither links
   nor unlinks it a second time. A recording still waiting when the restore runs (one the departure left, here a row
   no call on A will commit) loses its row at the restore itself, not at B's next call. */
static int
test_trunk_scan_hop_and_revisit(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_scan");
    char targets[DSD_TEST_PATH_MAX * 2];
    (void)dsd_test_path_join(targets, sizeof targets, r.dir, "targets.csv");
    FILE* f = dsd_fopen_private(targets, "wb");
    if (!f) {
        rig_close(&r);
        return 1;
    }
    const char* csv = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                      "a,p25-trunk,851000000,,250,250,\n"
                      "b,p25-trunk,852000000,,250,250,\n";
    const int wrote = fwrite(csv, 1, strlen(csv), f) == strlen(csv);
    rc |= expect_true("targets written", fclose(f) == 0 && wrote);
    r.opts->audio_in_type = AUDIO_IN_RTL;
    r.opts->use_rigctl = 1;
    r.opts->trunk_scan_enabled = 1;
    DSD_SNPRINTF(r.opts->trunk_scan_targets_csv, sizeof r.opts->trunk_scan_targets_csv, "%s", targets);
    dsd_trunk_tuning_requests_reset();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = tune, .tune_to_cc_request = tune, .return_to_cc_request = return_to_cc});
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){.get_current_freq_hz = current_frequency});
    trunk_scan_test_set_now(0.0);
    char error[256] = {0};
    rc |= expect_int("scan starts", dsd_engine_trunk_scan_init(r.opts, r.state, error, sizeof error), 0);
    rc |= expect_int("on target a", (long long)dsd_engine_trunk_scan_active_index(r.state), 0);

    submit(&r, DSD_APP_CMD_WAV_START);
    begin_call(&r, DSD_SYNC_P25P1_POS, 0U, 0U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 2);
    char id[64];
    slot1_id(&r, id, sizeof id);
    submit(&r, DSD_APP_CMD_WAV_STOP);
    end_call(&r, DSD_CALL_END_SYNC_LOSS);
    reload(&r);
    rc |= expect_int("a's row is held, its recording still pending",
                     count_records(&g_j, "link") + count_records(&g_j, "unlink"), 0);

    wav_journal_publish_record left;
    DSD_MEMSET(&left, 0, sizeof left);
    left.id = "00000000000000AB-1";
    left.path = r.journal;
    left.owner.kind = DSD_WAV_OWNER_PENDING;
    left.owner.slot = 0;
    left.owner.epoch = 999999U;
    wav_journal_publish(&left);

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(r.opts, r.state);
    rc |= expect_int("hopped to b", (long long)dsd_engine_trunk_scan_active_index(r.state), 1);
    reload(&r);
    rc |= expect_int("a's recording resolved by the hop", count_for_id(&g_j, "unlink", id), 1);
    rc |= expect_int("the one left waiting unlinked by the restore", count_for_id(&g_j, "unlink", left.id), 1);

    begin_call(&r, DSD_SYNC_P25P1_POS, 800U, 80U, DSD_CALL_BOUNDARY_BEGIN);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    rc |= expect_int("b's commit does not claim it", count_for_id(&g_j, "link", id), 0);

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(r.opts, r.state);
    rc |= expect_int("back on a", (long long)dsd_engine_trunk_scan_active_index(r.state), 0);
    /* Past the hold: a's restored row is resolved. */
    clock_at(g_now_s + 2);
    dsd_event_sync_slot(r.opts, r.state, 0);
    clock_at(g_now_s + 2);
    dsd_event_sync_slot(r.opts, r.state, 0);
    reload(&r);
    rc |= expect_int("a's own commit on the revisit does not claim it", count_for_id(&g_j, "link", id), 0);
    rc |= expect_int("no second unlink", count_for_id(&g_j, "unlink", id), 1);
    rc |= expect_int("nor of the one left waiting", count_for_id(&g_j, "unlink", left.id), 1);
    rc |= expect_int("which nothing linked", count_for_id(&g_j, "link", left.id), 0);

    dsd_engine_trunk_scan_shutdown(r.opts, r.state);
    trunk_scan_test_clear_now();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){0});
    dsd_trunk_tuning_requests_reset();
    rig_close(&r);
    return rc;
}

/* --- Start times. --- */

/* Per-call recording switched on mid-call: the recording starts when its file opened. */
static int
test_start_after_mid_call_enable(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_mid_call");
    clock_at(k_t0 + 10);
    begin_call(&r, DSD_SYNC_P25P1_POS, 900U, 90U, DSD_CALL_BOUNDARY_BEGIN);
    clock_at(k_t0 + 40);
    submit(&r, DSD_APP_CMD_WAV_START);
    voice_frames(&r, 0, 2);
    clock_at(k_t0 + 45);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    const char* pub = record(&g_j, "publish", 0);
    rc |=
        expect_int("starts at the open, not the call's start", pub ? field_ll(pub, "start") : -9, (long long)k_t0 + 40);
    rig_close(&r);
    return rc;
}

/* A call across midnight: the file is named on the day it ended (event_time), and the record's start is the call's
   start, the day before. */
static int
test_start_across_midnight(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_midnight");
    struct tm before = {0};
    before.tm_year = 2024 - 1900;
    before.tm_mon = 5;
    before.tm_mday = 1;
    before.tm_hour = 23;
    before.tm_min = 59;
    before.tm_sec = 50;
    before.tm_isdst = -1;
    const time_t start = mktime(&before);
    clock_at((uint64_t)start - 5);
    submit(&r, DSD_APP_CMD_WAV_START);
    clock_at((uint64_t)start);
    begin_call(&r, DSD_SYNC_P25P1_POS, 901U, 91U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 2);
    clock_at((uint64_t)start + 20);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    const char* pub = record(&g_j, "publish", 0);
    rc |= expect_int("the start is the call's, before midnight", pub ? field_ll(pub, "start") : -9, (long long)start);
    char day_before[9];
    char day_after[9];
    (void)dsd_format_local_datetime(start, DSD_LOCAL_DATETIME_DATE_COMPACT, day_before, sizeof day_before);
    (void)dsd_format_local_datetime(start + 20, DSD_LOCAL_DATETIME_DATE_COMPACT, day_after, sizeof day_after);
    rc |= expect_true("the call crosses midnight", strcmp(day_before, day_after) != 0);
    rc |= expect_true("the file is named on the end's day", pub && strncmp(field_s(pub, "file"), day_after, 8) == 0);
    rig_close(&r);
    return rc;
}

/* Under I/Q replay the decode clock follows the capture: the start and the stamp are the capture's, not now. */
static int
test_start_on_the_replay_clock(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_replay");
    const int64_t anchor = 1262304000LL; /* 2010-01-01T00:00:00Z */
    dsd_decode_clock_use_replay(anchor);
    dsd_decode_clock_set_media_ns(5ULL * 1000000000ULL);
    submit(&r, DSD_APP_CMD_WAV_START);
    dsd_decode_clock_set_media_ns(60ULL * 1000000000ULL);
    begin_call(&r, DSD_SYNC_P25P1_POS, 902U, 92U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 2);
    dsd_decode_clock_set_media_ns(65ULL * 1000000000ULL);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    const char* pub = record(&g_j, "publish", 0);
    rc |= expect_int("the call's start on the capture's clock", pub ? field_ll(pub, "start") : -9,
                     (long long)anchor + 60);
    rc |= expect_int("stamped on the capture's clock", pub ? field_ll(pub, "t") : -9, (long long)anchor + 65);
    rig_close(&r);
    return rc;
}

/* --- The journal itself. --- */

static int
write_text(const char* path, const char* text) {
    FILE* f = dsd_fopen_private(path, "wb");
    if (!f) {
        return 1;
    }
    const int ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    return (fclose(f) != 0 || !ok) ? 1 : 0;
}

/* A journal that ends inside a line (a crash tore the last write) gets a newline before the next record, so that
   record parses; one that ends in a newline gets none. */
static int
test_torn_tail_gets_a_newline(void) {
    rig r;
    int rc = rig_open_ex(&r, "dsdneo_journal_torn", 0);
    rc |= expect_int("torn journal written", write_text(r.journal, "{\"v\":1,\"type\":\"publ"), 0);
    rc |= expect_int("journal opens", dsd_wav_journal_open(r.journal), 0);
    submit(&r, DSD_APP_CMD_WAV_START);
    begin_call(&r, DSD_SYNC_P25P1_POS, 310U, 3U, DSD_CALL_BOUNDARY_BEGIN);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    rc |= expect_int("the torn line and the record", g_j.count, 2);
    rc |= expect_str("the torn line kept as it was", g_j.line[0], "{\"v\":1,\"type\":\"publ");
    rc |=
        expect_true("the record on a line of its own", strncmp(g_j.line[1], "{\"v\":1,\"type\":\"discard\"", 23) == 0);
    rc |= expect_true("whole lines after", !g_j.torn);
    rc |= expect_int("reopen", dsd_wav_journal_open(r.journal), 0);
    reload(&r);
    rc |= expect_int("a journal ending in a newline gets none", g_j.count, 2);
    rig_close(&r);
    return rc;
}

static uint8_t
view_journal_state(const rig* r) {
    dsd_app_telemetry_publish_opts_snapshot(r->opts);
    dsd_app_recording_view v;
    DSD_MEMSET(&v, 0, sizeof v);
    (void)dsd_app_recording_view_get(&v);
    return v.journal_state;
}

/* A journal write that fails (a full disk) turns the journal off for the rest of the run, logged once and reported by
   the recording view; recording goes on. The next run's journal opens again. */
static int
test_write_failure_latches_the_journal_off(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_write_fail");
    dsd_neo_log_set_tap(count_log_lines, NULL);
    rc |= expect_int("the view reports the journal on", view_journal_state(&r), DSD_APP_RECORDING_JOURNAL_ON);
    submit(&r, DSD_APP_CMD_WAV_START);
    dsd_wav_journal_test_fail_writes_after(0);
    for (uint32_t tg = 1000U; tg < 1003U; tg++) {
        begin_call(&r, DSD_SYNC_P25P1_POS, tg, 10U, DSD_CALL_BOUNDARY_BEGIN);
        voice_frames(&r, 0, 2);
        end_call(&r, DSD_CALL_END_EXPLICIT);
    }
    rc |= expect_int("the journal is off", dsd_wav_journal_state_get(), DSD_WAV_JOURNAL_FAILED);
    rc |= expect_int("the view reports it", view_journal_state(&r), DSD_APP_RECORDING_JOURNAL_FAILED);
    rc |= expect_int("logged once", g_journal_errors, 1);
    reload(&r);
    rc |= expect_int("nothing was written", g_j.count, 0);
    int wavs = 0;
    for (int i = 0; i < 3; i++) {
        wavs += (int)(r.state->event_history_s[0].Event_History_Items[1 + i].target_id != 0U);
    }
    rc |= expect_int("the calls still committed", wavs, 3);
    dsd_wav_journal_close();
    rc |= expect_int("still failed after the run's close", dsd_wav_journal_state_get(), DSD_WAV_JOURNAL_FAILED);
    dsd_wav_journal_test_fail_writes_after(-1);
    rc |= expect_int("the next run's journal opens", dsd_wav_journal_open(r.journal), 0);
    rc |= expect_int("and is on", view_journal_state(&r), DSD_APP_RECORDING_JOURNAL_ON);
    dsd_neo_log_set_tap(NULL, NULL);
    rig_close(&r);
    return rc;
}

static int
count_journal_cb(const char* name, void* user) {
    int* n = (int*)user;
    if (strstr(name, ".jsonl") != NULL || strstr(name, "journal") != NULL) {
        (*n)++;
    }
    return 0;
}

/* Without --wav-journal nothing is written and no file appears; recording is unchanged. */
static int
test_no_journal_no_file(void) {
    rig r;
    int rc = rig_open_ex(&r, "dsdneo_journal_none", 0);
    rc |= expect_int("no journal this run", dsd_wav_journal_open(r.opts->wav_journal_file), 0);
    rc |= expect_int("off", dsd_wav_journal_state_get(), DSD_WAV_JOURNAL_OFF);
    rc |= expect_int("the view reports none", view_journal_state(&r), DSD_APP_RECORDING_JOURNAL_NONE);
    submit(&r, DSD_APP_CMD_WAV_START);
    begin_call(&r, DSD_SYNC_P25P1_POS, 1100U, 11U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 2);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    int journals = 0;
    (void)dsd_dir_list(r.dir, count_journal_cb, &journals);
    rc |= expect_int("no journal file", journals, 0);
    rig_close(&r);
    return rc;
}

/* Radio text can hold anything: the strings a record carries stay valid JSON. */
static int
test_strings_are_escaped(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_escape");
    char path[DSD_TEST_PATH_MAX * 2];
    (void)dsd_test_path_join(path, sizeof path, r.dir, "a\"b.wav");
    wav_journal_publish_record rec;
    DSD_MEMSET(&rec, 0, sizeof rec);
    rec.id = "00000000000000AB-7";
    rec.path = path;
    rec.owner.kind = DSD_WAV_OWNER_NONE;
    rec.owner.slot = DSD_WAV_OWNER_SLOT_UNKNOWN;
    rec.system = "S\\Y\tS";
    rec.target = "T\x01\x7F\n";
    rec.source = "caf\xC3\xA9 \xFF\xE2\x80\xA8!";
    wav_journal_publish(&rec);
    reload(&r);
    const char* line = g_j.line[0];
    rc |= expect_true("quote escaped", strstr(line, "\"file\":\"a\\\"b.wav\"") != NULL);
    rc |= expect_true("backslash and tab escaped", strstr(line, "\"system\":\"S\\\\Y\\tS\"") != NULL);
    rc |= expect_true("control characters escaped", strstr(line, "\"target\":\"T\\u0001\\u007F\\n\"") != NULL);
    rc |= expect_true("UTF-8 kept, a stray byte replaced, U+2028 escaped",
                      strstr(line, "\"source\":\"caf\xC3\xA9 \xEF\xBF\xBD\\u2028!\"") != NULL);
    rc |= expect_true("no slot", strstr(line, "\"slot\":null") != NULL);
    rc |= expect_true("no ring", strstr(line, "\"instance\":null") != NULL);
    rc |= expect_int("one line", g_j.count, 1);
    rig_close(&r);
    return rc;
}

/* A pending line is always resolved later: a recording whose pending entry has no room (out of memory) or no id (a
   temp name open_wav_file() did not make) is published as no row's, and neither a commit nor the journal's close writes
   anything more for it. */
static int
test_pending_without_room_or_id_is_published_as_no_rows(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_pending_fallback");
    submit(&r, DSD_APP_CMD_WAV_START);
    begin_call(&r, DSD_SYNC_P25P1_POS, 1200U, 12U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 2);
    char id[64];
    slot1_id(&r, id, sizeof id);
    dsd_wav_journal_test_fail_pending_growth(1);
    submit(&r, DSD_APP_CMD_WAV_STOP);
    dsd_wav_journal_test_fail_pending_growth(0);

    /* A file under a name of its own, closed by a pending closer while the call is still on. */
    char foreign[DSD_TEST_PATH_MAX * 2];
    (void)dsd_test_path_join(foreign, sizeof foreign, r.dir, "foreign-recording");
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof info);
    info.samplerate = 8000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* f = sf_open(foreign, SFM_WRITE, &info);
    rc |= expect_true("foreign file opened", f != NULL);
    if (f) {
        static short pcm[800];
        for (int i = 0; i < 800; i++) {
            pcm[i] = (short)(i * 16);
        }
        (void)sf_write_short(f, pcm, 800);
        (void)close_and_publish_call_wav(f, r.opts, r.state, 0, foreign, r.dir, r.opts->wav_out_open_time);
    }
    end_call(&r, DSD_CALL_END_EXPLICIT);
    dsd_wav_journal_close();
    reload(&r);
    const char* no_room = NULL;
    const char* no_id = NULL;
    const char* line = NULL;
    for (int i = 0; (line = record(&g_j, "publish", i)) != NULL; i++) {
        if (strcmp(field_s(line, "id"), id) == 0) {
            no_room = line;
        } else if (strcmp(field_s(line, "id"), "null") == 0) {
            no_id = line;
        }
    }
    rc |= expect_true("no room: published as no row's", no_room && strcmp(field_s(no_room, "pending"), "false") == 0
                                                            && strcmp(field_s(no_room, "row"), "null") == 0
                                                            && field_ll(no_room, "epoch") == 0);
    rc |= expect_true("no id: published as no row's", no_id && strcmp(field_s(no_id, "pending"), "false") == 0
                                                          && strcmp(field_s(no_id, "row"), "null") == 0);
    rc |= expect_int("no link for either", count_records(&g_j, "link"), 0);
    rc |= expect_int("no unlink for either, not even at the close", count_records(&g_j, "unlink"), 0);
    rig_close(&r);
    return rc;
}

/* The libsndfile seams (wav_segment_test_support.h): a write that falls short, a close that fails. */
static long long
short_write(SNDFILE* file, const short* samples, long long count) {
    return (long long)sf_write_short(file, samples, (sf_count_t)(count / 2));
}

static int
failing_close(SNDFILE* file) {
    (void)sf_close(file);
    return SF_ERR_SYSTEM;
}

/* The record shapes the store parses: `error` kind open and close, `"failed":true` after a short write, and
   `"rdio":true` when an rdio-scanner API upload was queued. */
static int
test_error_failed_and_rdio_shapes(void) {
    int rc = 0;
    {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_open_error");
        dsd_wav_test_fail_opens_after(0);
        submit(&r, DSD_APP_CMD_WAV_START);
        dsd_wav_test_fail_opens_after(-1);
        reload(&r);
        /* The start stops at its first open that fails. */
        rc |= expect_int("the failed open recorded", count_records(&g_j, "error"), 1);
        const char* err = record(&g_j, "error", 0);
        rc |= expect_str("kind open", err ? field_s(err, "kind") : "", "open");
        rc |= expect_true("it names the segment", err && is_segment_id(field_s(err, "id")));
        rc |= expect_true("and the temp name it would have had", err && strncmp(field_s(err, "temp"), "TEMP_", 5) == 0);
        rc |= expect_str("in the recording folder", err ? field_s(err, "folder") : "", r.canon);
        rig_close(&r);
    }
    {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_close_error");
        submit(&r, DSD_APP_CMD_WAV_START);
        char id[64];
        slot1_id(&r, id, sizeof id);
        char temp[1024];
        DSD_SNPRINTF(temp, sizeof temp, "%s", r.opts->wav_out_file);
        voice_frames(&r, 0, 2);
        dsd_wav_test_set_io(NULL, failing_close);
        submit(&r, DSD_APP_CMD_WAV_STOP);
        dsd_wav_test_set_io(NULL, NULL);
        reload(&r);
        const char* err = NULL;
        const char* line = NULL;
        for (int i = 0; (line = record(&g_j, "error", i)) != NULL; i++) {
            if (strcmp(field_s(line, "id"), id) == 0) {
                err = line;
            }
        }
        rc |= expect_str("kind close", err ? field_s(err, "kind") : "", "close");
        rc |= expect_str("the temp name it keeps", err ? field_s(err, "temp") : "", temp + strlen(r.dir) + 1);
        rc |= expect_int("nothing published for it", count_for_id(&g_j, "publish", id), 0);
        rig_close(&r);
    }
    {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_short_write");
        submit(&r, DSD_APP_CMD_WAV_START);
        begin_call(&r, DSD_SYNC_P25P1_POS, 1300U, 13U, DSD_CALL_BOUNDARY_BEGIN);
        dsd_wav_test_set_io(short_write, NULL);
        voice_frames(&r, 0, 3);
        dsd_wav_test_set_io(NULL, NULL);
        end_call(&r, DSD_CALL_END_EXPLICIT);
        reload(&r);
        const char* pub = record(&g_j, "publish", 0);
        rc |= expect_str("a short write marks the recording failed", pub ? field_s(pub, "failed") : "", "true");
        rc |=
            expect_int("it ends where the write fell short: half a frame", pub ? field_ll(pub, "duration_ms") : -9, 10);
        rig_close(&r);
    }
    {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_rdio");
        r.opts->rdio_mode = DSD_RDIO_MODE_BOTH;
        r.opts->rdio_system_id = 48;
        r.opts->rdio_upload_timeout_ms = 100;
        r.opts->rdio_upload_retries = 1;
        DSD_SNPRINTF(r.opts->rdio_api_url, sizeof r.opts->rdio_api_url, "%s", "http://127.0.0.1:1");
        DSD_SNPRINTF(r.opts->rdio_api_key, sizeof r.opts->rdio_api_key, "%s", "test-key");
        submit(&r, DSD_APP_CMD_WAV_START);
        begin_call(&r, DSD_SYNC_P25P1_POS, 1400U, 14U, DSD_CALL_BOUNDARY_BEGIN);
        voice_frames(&r, 0, 2);
        end_call(&r, DSD_CALL_END_EXPLICIT);
        dsd_rdio_upload_shutdown();
        reload(&r);
        const char* pub = record(&g_j, "publish", 0);
#ifdef USE_CURL
        rc |= expect_str("an upload was queued", pub ? field_s(pub, "rdio") : "", "true");
#else
        rc |= expect_str("no upload without libcurl", pub ? field_s(pub, "rdio") : "", "false");
#endif
        r.opts->rdio_mode = DSD_RDIO_MODE_OFF;
        rig_close(&r);
    }
    return rc;
}

/* The process token the app reads (dsd_wav_temp_token(), its text form) is the one the temp names and journal ids
   carry: the app tells this process's temp files, which may still be open, from the ones an earlier process left. */
static int
test_temp_token_is_the_names_token(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_token");
    char text[DSD_WAV_TEMP_TOKEN_TEXT_SIZE];
    rc |= expect_int("the text form fits its size", dsd_wav_temp_token_text(text, sizeof text), 0);
    char hex[24];
    hex64(dsd_wav_temp_token(), hex, sizeof hex);
    rc |= expect_str("16 uppercase hex digits of the token", text, hex);
    hex64(dsd_wav_temp_token(), hex, sizeof hex);
    rc |= expect_str("drawn once: the same token again", text, hex);
    char small[DSD_WAV_TEMP_TOKEN_TEXT_SIZE - 1U];
    small[0] = 'x';
    rc |= expect_int("a buffer one short is refused", dsd_wav_temp_token_text(small, sizeof small), -1);
    rc |= expect_str("and left empty", small, "");
    rc |= expect_int("no buffer is refused", dsd_wav_temp_token_text(NULL, 64U), -1);

    submit(&r, DSD_APP_CMD_WAV_START);
    char want[DSD_WAV_TEMP_TOKEN_TEXT_SIZE + 2U];
    DSD_SNPRINTF(want, sizeof want, "_%s_", text);
    rc |= expect_true("the temp name carries the token", strstr(r.opts->wav_out_file, want) != NULL);
    clock_at(k_t0 + 100);
    begin_call(&r, DSD_SYNC_P25P1_POS, 100U, 77U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 3);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    const char* pub = record(&g_j, "publish", 0);
    DSD_SNPRINTF(want, sizeof want, "%s-", text);
    rc |= expect_true("the journal id starts with the token", pub && strncmp(field_s(pub, "id"), want, 17) == 0);
    rig_close(&r);
    return rc;
}

/* The `file` and folder forms, with POSIX and Windows rules on any host: a file in the journal's folder or below it is
   named '/'-separated relative to it, one elsewhere by its folder; a file in a root keeps the root as its folder. */
static int
test_path_forms(void) {
    int rc = 0;
    char out[256];
    rc |= expect_str("posix base", wav_journal_path_base("/a/b/c.wav", 0), "c.wav");
    rc |= expect_str("posix keeps a backslash in a name", wav_journal_path_base("/a/b\\c.wav", 0), "b\\c.wav");
    rc |= expect_str("windows base", wav_journal_path_base("C:\\a/b\\c.wav", 1), "c.wav");
    wav_journal_path_folder("/a/b/c.wav", 0, out, sizeof out);
    rc |= expect_str("posix folder", out, "/a/b");
    wav_journal_path_folder("/c.wav", 0, out, sizeof out);
    rc |= expect_str("posix root folder", out, "/");
    wav_journal_path_folder("c.wav", 0, out, sizeof out);
    rc |= expect_str("bare name", out, "");
    wav_journal_path_folder("C:\\a\\c.wav", 1, out, sizeof out);
    rc |= expect_str("windows folder", out, "C:\\a");
    wav_journal_path_folder("C:\\c.wav", 1, out, sizeof out);
    rc |= expect_str("windows drive root keeps its separator", out, "C:\\");
    wav_journal_path_folder("C:/rec/c.wav", 1, out, sizeof out);
    rc |= expect_str("windows forward slash", out, "C:/rec");

    wav_journal_path_name("/j", "/j", "c.wav", 0, out, sizeof out);
    rc |= expect_str("posix in the journal folder", out, "c.wav");
    wav_journal_path_name("/j", "/j/sub/dir", "c.wav", 0, out, sizeof out);
    rc |= expect_str("posix below it", out, "sub/dir/c.wav");
    wav_journal_path_name("/j", "/jx", "c.wav", 0, out, sizeof out);
    rc |= expect_str("posix a sibling with the same prefix is outside", out, "/jx/c.wav");
    wav_journal_path_name("/", "/rec", "c.wav", 0, out, sizeof out);
    rc |= expect_str("posix journal in the root", out, "rec/c.wav");
    wav_journal_path_name("C:\\j", "C:\\j\\sub\\dir", "c.wav", 1, out, sizeof out);
    rc |= expect_str("windows below the journal: '/'-separated", out, "sub/dir/c.wav");
    wav_journal_path_name("C:\\j", "C:\\j", "c.wav", 1, out, sizeof out);
    rc |= expect_str("windows in the journal folder", out, "c.wav");
    wav_journal_path_name("C:\\", "C:\\rec", "c.wav", 1, out, sizeof out);
    rc |= expect_str("windows journal in a drive root", out, "rec/c.wav");
    wav_journal_path_name("C:\\j", "D:\\rec", "c.wav", 1, out, sizeof out);
    rc |= expect_str("windows outside", out, "D:\\rec\\c.wav");
    wav_journal_path_name("C:\\Rec\\J", "c:\\rec\\j\\Sub", "c.wav", 1, out, sizeof out);
    rc |= expect_str("windows compares folders without case, keeps the file's", out, "Sub/c.wav");
    wav_journal_path_name("/Rec/J", "/rec/j/Sub", "c.wav", 0, out, sizeof out);
    rc |= expect_str("posix folders differing in case are different folders", out, "/rec/j/Sub/c.wav");
    wav_journal_path_name("C:\\j", "D:\\", "c.wav", 1, out, sizeof out);
    rc |= expect_str("windows outside, in a drive root", out, "D:\\c.wav");
    wav_journal_path_name("/j", "/j", "", 0, out, sizeof out);
    rc |= expect_str("no name", out, "");
    return rc;
}

int
main(void) {
    dsd_neo_config_init();
    int rc = 0;
    rc |= test_commit_publishes_under_the_pushed_row();
    rc |= test_merge_chain_names_the_retained_row();
    rc |= test_merge_links_pending_to_the_retained_row();
    rc |= test_drop_unlinks_the_epoch_recordings();
    rc |= test_empty_segment_is_discarded();
    rc |= test_rename_failure_is_an_error_record();
    rc |= test_b3_name_and_raw_identity();
    rc |= test_pending_closers_are_linked_at_the_commit();
    rc |= test_epoch_moving_on_unlinks();
    rc |= test_full_reset_unlinks_pending();
    rc |= test_trunk_scan_hop_and_revisit();
    rc |= test_start_after_mid_call_enable();
    rc |= test_start_across_midnight();
    rc |= test_start_on_the_replay_clock();
    rc |= test_torn_tail_gets_a_newline();
    rc |= test_write_failure_latches_the_journal_off();
    rc |= test_no_journal_no_file();
    rc |= test_strings_are_escaped();
    rc |= test_pending_without_room_or_id_is_published_as_no_rows();
    rc |= test_error_failed_and_rdio_shapes();
    rc |= test_path_forms();
    rc |= test_temp_token_is_the_names_token();
    if (rc == 0) {
        printf("CORE_WAV_JOURNAL: OK\n");
    }
    return rc;
}
