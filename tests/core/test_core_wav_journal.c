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
#include <dsd-neo/core/analog_mode.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/core/wav_journal.h>
#include <dsd-neo/core/wav_recording.h>
#include <dsd-neo/engine/analog_call.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <fcntl.h>
#include <sys/stat.h>
#endif
#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif
#include "../test_support/radio_policy_test.h"
#include "analog_call_test_support.h"
#include "commands_internal.h"
#include "edacs_internal.h"
#include "snapshot_internal.h"
#include "test_support.h"
#include "tetra_confirm.h"
#include "tetra_state.h"
#include "trunk_scan_internal.h"
#include "trunk_scan_test_support.h"
#include "wav_journal.h"
#include "wav_journal_test_support.h"
#include "wav_segment.h"
#include "wav_segment_test_support.h"
#include "wav_size_test_support.h"

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
static int g_sdrtrunk_refusals;
/* Set: a played SDRTrunk file's malformed voice frame (its warning) sets the exit flag, as Stop does mid-file. */
static int g_stop_at_malformed_frame;
static int g_unremoved_discards;

/* The process's one log tap (it is installed once and never replaced): the errors the tests count. */
static void
count_log_lines(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    if (level == LOG_LEVEL_ERROR && text && strstr(text, "Call recording journal") != NULL) {
        g_journal_errors++;
    }
    if (level == LOG_LEVEL_ERROR && text && strstr(text, "SDRTrunk MBE file") != NULL) {
        g_sdrtrunk_refusals++;
    }
    if (g_stop_at_malformed_frame && text && strstr(text, "Skipping malformed SDRTrunk MBE voice frame") != NULL) {
        dsd_exitflag_store(1);
    }
    if (text && strstr(text, "was to be discarded but could not be removed") != NULL) {
        g_unremoved_discards++;
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

/* @p path as a JSON string body, the form field() returns: '\\' and '"' escaped (a Windows path's separators). */
static const char*
json_path(const char* path) {
    static char v[4][LINE_CAP];
    static int next;
    char* out = v[next++ & 3];
    size_t n = 0;
    for (const char* p = path; *p && n + 2 < LINE_CAP; p++) {
        if (*p == '\\' || *p == '"') {
            out[n++] = '\\';
        }
        out[n++] = *p;
    }
    out[n] = '\0';
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
    /* No sound card: initOpts() picks the PulseAudio output, which a scan opens (dsd_engine_scan_ensure_output()), and
       libpulse then makes a runtime folder in TMPDIR wherever XDG_RUNTIME_DIR is unset. */
    r->opts->audio_out_type = 9;
    DSD_SNPRINTF(r->opts->wav_out_dir, sizeof r->opts->wav_out_dir, "%s", r->dir);
    (void)dsd_test_path_join(r->journal, sizeof r->journal, r->dir, "calls.jsonl");
    dsd_wav_recording_session_reset();
    dsd_wav_journal_test_fail_writes_after(-1);
    dsd_signal_search_visit_clear();
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
    dsd_signal_search_visit_clear();
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
begin_call_on(rig* r, uint8_t slot, int protocol, uint32_t tg, uint32_t src, dsd_call_boundary boundary) {
    const dsd_call_observation call = {.protocol = protocol,
                                       .slot = slot,
                                       .kind = DSD_CALL_KIND_GROUP_VOICE,
                                       .ota_target_id = tg,
                                       .policy_target_id = tg,
                                       .ota_source_id = src};
    (void)dsd_call_state_observe(r->state, &call, boundary);
    dsd_event_sync_slot(r->opts, r->state, slot);
}

static void
begin_call(rig* r, int protocol, uint32_t tg, uint32_t src, dsd_call_boundary boundary) {
    begin_call_on(r, 0U, protocol, tg, src, boundary);
}

static void
end_call_on(rig* r, uint8_t slot, dsd_call_end_reason reason) {
    (void)dsd_call_state_end_ex(r->state, slot, 0.0, reason);
    dsd_event_sync_slot(r->opts, r->state, slot);
}

static void
end_call(rig* r, dsd_call_end_reason reason) {
    end_call_on(r, 0U, reason);
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
    rc |= expect_str("the folder it was published in", field_s(pub, "folder"), json_path(r.canon));
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
    rc |= expect_str("in the folder it was recorded in", err ? field_s(err, "folder") : "", json_path(r.canon));
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

/* What a folder listing finds: the .wav files (the last one's name in @p name), and, counted apart, the per-call
   recordings still under their temp name (TEMP_..., with no extension). */
typedef struct {
    int count;
    int temps;
    char name[512];
} wav_names;

static int
collect_wav_cb(const char* name, void* user) {
    wav_names* names = (wav_names*)user;
    const size_t n = strlen(name);
    if (strncmp(name, "TEMP_", 5) == 0) {
        names->temps++;
    } else if (n > 4 && strcmp(name + n - 4, ".wav") == 0) {
        names->count++;
        DSD_SNPRINTF(names->name, sizeof names->name, "%s", name);
    }
    return 0;
}

/* A radio identity can hold a byte that starts no UTF-8 character (a D-STAR callsign is 8-bit text off the air). The
   journal writes such a byte as U+FFFD, so a file name keeping it would not be the one the journal names: the name has
   '_' there instead, and the journal's `file` is exactly the file published. The row keeps the identity as received. */
static int
test_non_utf8_identity_names_the_file_published(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_utf8");
    submit(&r, DSD_APP_CMD_WAV_START);
    dsd_call_observation call = {.protocol = DSD_SYNC_DSTAR_VOICE_POS, .slot = 0U, .kind = DSD_CALL_KIND_GROUP_VOICE};
    DSD_SNPRINTF(call.source_text, sizeof call.source_text, "%s",
                 "N0C\xFF"
                 "LL \xC3\xA9");
    DSD_SNPRINTF(call.target_text, sizeof call.target_text, "%s", "CQCQCQ");
    (void)dsd_call_state_observe(r.state, &call, DSD_CALL_BOUNDARY_BEGIN);
    dsd_event_sync_slot(r.opts, r.state, 0);
    voice_frames(&r, 0, 2);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    const char* pub = record(&g_j, "publish", 0);
    rc |= expect_true("utf8: published", pub != NULL);
    wav_names names;
    DSD_MEMSET(&names, 0, sizeof names);
    (void)dsd_dir_list(r.dir, collect_wav_cb, &names);
    rc |= expect_int("utf8: one recording", names.count, 1);
    if (pub && names.count == 1) {
        rc |= expect_str("utf8: the journal names the file published", field_s(pub, "file"), names.name);
        rc |= expect_true("utf8: the stray byte is '_' in the name, the UTF-8 kept",
                          strstr(names.name, "SRC_N0C_LL_\xC3\xA9.wav") != NULL);
    }
    const Event_History* row = &r.state->event_history_s[0].Event_History_Items[1];
    rc |= expect_str("utf8: the row keeps the identity as received", row->src_str,
                     "N0C\xFF"
                     "LL \xC3\xA9");
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

/* One NFM block. A reception becomes a call only once it ran DSD_ANALOG_CALL_MIN_MS (issue #667), so a block that
   would begin one comes after its tone check, as the symbol path hands it over under a tone filter: enough PENDING
   blocks (recorded nowhere) that this block brings the reception to the minimum. */
static void
nfm_block(dsd_opts* opts, dsd_state* state) {
    static float block[960];
    static const float checked[960] = {0};
    for (int i = 0; i < 960; i++) {
        block[i] = (float)((i % 48) * 400 - 9600);
    }
    if (!dsd_engine_analog_call_active(state)) {
        for (int i = 0; i < (DSD_ANALOG_CALL_MIN_MS * 48 - 960) / 960; i++) {
            dsd_test_analog_call_feed(opts, state, checked, 960, 48000, DSD_ANALOG_CALL_PENDING);
        }
    }
    dsd_test_analog_call_feed(opts, state, block, 960, 48000, DSD_ANALOG_CALL_RECEPTION);
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

/* Issue #756: an NFM call held through a pause is one recording. It is published once, pending on the call's epoch and
   linked by its one commit, named as the analog call with its service; it starts at the first burst and lasts both bursts
   and the silence written between them. */
static int
test_held_nfm_call_is_one_recording(void) {
    int rc = 0;
    rig r;
    rc |= rig_open(&r, "dsdneo_journal_nfm_hold");
    setup_nfm(r.opts, r.state);
    r.opts->analog_record_max_gap_ms = 1000;
    rc |= expect_int("tone named",
                     dsd_receive_names_parse("--tone-name '141.3=Dispatch'", &r.opts->receive.names, NULL, 0), 0);
    submit(&r, DSD_APP_CMD_WAV_START);
    /* Half a second in: the call's tone check and first block, all stamped here, lie in second k_t0 + 10. */
    clock_at_ms(k_t0 + 10, 500);
    nfm_block(r.opts, r.state);
    nfm_block(r.opts, r.state);
    const uint64_t epoch = call_epoch(&r);
    static const float closed[960] = {0};
    r.state->analog_rx.carrier_open = 0;
    for (int i = 0; i < 10; i++) {
        dsd_test_analog_call_feed(r.opts, r.state, closed, 960, 48000, DSD_ANALOG_CALL_HOLDABLE);
    }
    r.state->analog_rx.carrier_open = 1;
    clock_at(k_t0 + 11);
    nfm_block(r.opts, r.state);
    nfm_block(r.opts, r.state);
    rc |= expect_int("one call through the pause", (long long)call_epoch(&r), (long long)epoch);
    clock_at(k_t0 + 12);
    dsd_engine_analog_call_end(r.opts, r.state);
    reload(&r);
    int published = 0;
    char id[64] = {0};
    const char* pub = NULL;
    for (int i = 0; (pub = record(&g_j, "publish", i)) != NULL; i++) {
        if (field_ll(pub, "rate") == 48000) {
            published++;
            DSD_SNPRINTF(id, sizeof id, "%s", field_s(pub, "id"));
            rc |= expect_int("held nfm: both bursts and the pause", field_ll(pub, "duration_ms"), 40 + 200 + 40);
            rc |= expect_int("held nfm: starts at the first burst", field_ll(pub, "start"), (long long)k_t0 + 10);
            /* Heard on an audio input, whose mode is not known: the analog name (issue #667). */
            rc |=
                expect_true("held nfm: the analog name with its service",
                            strstr(field_s(pub, "file"), "_ANALOG_FREQ_UNKNOWN_CTCSS_141.3_SVC_Dispatch.wav") != NULL);
        }
    }
    rc |= expect_int("held nfm: published once", published, 1);
    rc |= expect_pending_then_linked("held nfm", id, epoch, &r.state->event_history_s[0],
                                     r.state->event_history_s[0].push_seq);
    rig_close(&r);
    return rc;
}

/* An NFM call's end leaves the slot files alone (issue #668): its audio goes to its own recording, so the empty 8 kHz
   slot file stays open and nothing is discarded per call. A digital call's end still rotates it. */
static int
test_nfm_call_end_keeps_the_slot_file(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_nfm_slot");
    setup_nfm(r.opts, r.state);
    submit(&r, DSD_APP_CMD_WAV_START);
    SNDFILE* const slot_file = r.opts->wav_out_f;
    char slot_name[sizeof r.opts->wav_out_file];
    DSD_SNPRINTF(slot_name, sizeof slot_name, "%s", r.opts->wav_out_file);
    rc |= expect_true("the slot file opened", slot_file != NULL);
    for (int call = 0; call < 3; call++) {
        nfm_block(r.opts, r.state);
        nfm_block(r.opts, r.state);
        dsd_engine_analog_call_end(r.opts, r.state);
    }
    reload(&r);
    rc |= expect_int("three NFM recordings published", count_records(&g_j, "publish"), 3);
    rc |= expect_int("no discard per NFM call", count_records(&g_j, "discard"), 0);
    rc |= expect_true("the slot file stays open", r.opts->wav_out_f == slot_file);
    rc |= expect_true("under its own name", strcmp(r.opts->wav_out_file, slot_name) == 0);

    /* A digital call after them still rotates it: the empty file is discarded under the id it had. */
    char id[64];
    slot1_id(&r, id, sizeof id);
    begin_call(&r, DSD_SYNC_P25P1_POS, 300U, 1U, DSD_CALL_BOUNDARY_BEGIN);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    rc |= expect_int("the digital call's end discards the empty slot file", count_for_id(&g_j, "discard", id), 1);
    rc |= expect_true("a new slot file opened",
                      r.opts->wav_out_f != NULL && strcmp(r.opts->wav_out_file, slot_name) != 0);
    rig_close(&r);
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
                /* An audio input's call, whose mode is not known (issue #667). */
                rc |= expect_true("nfm: published under the analog name",
                                  strstr(field_s(pub, "file"), "_ANALOG_FREQ_UNKNOWN") != NULL);
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

static int g_tune_refused; /* the stand-in tuner refuses every tune */

static dsd_trunk_tune_result
tune(dsd_opts* opts, dsd_state* state, long freq, int sps, uint64_t request_id) {
    (void)sps;
    (void)request_id;
    if (g_tune_refused) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
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
    DSD_SNPRINTF(r.opts->trunk_scan_lists[0].path, sizeof r.opts->trunk_scan_lists[0].path, "%s", targets);
    r.opts->trunk_scan_list_count = 1;
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

/* The publish line's start is the second the call began (issue #809 B1), on the capture's clock: a call heard from
   59.95 s into the capture and first rendered at 60.02 s began in second 59, not in second 60. */
static int
test_start_floors_once_on_the_replay_clock(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_floor");
    const int64_t anchor = 1262304000LL; /* 2010-01-01T00:00:00Z */
    dsd_decode_clock_use_replay(anchor);
    dsd_decode_clock_set_media_ns(5ULL * 1000000000ULL);
    submit(&r, DSD_APP_CMD_WAV_START);
    const dsd_call_observation call = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 903U,
        .policy_target_id = 903U,
        .ota_source_id = 93U,
    };
    dsd_decode_clock_set_media_ns(59950ULL * 1000000ULL);
    (void)dsd_call_state_observe(r.state, &call, DSD_CALL_BOUNDARY_BEGIN);
    dsd_decode_clock_set_media_ns(60020ULL * 1000000ULL);
    (void)dsd_call_state_observe(r.state, &call, DSD_CALL_BOUNDARY_CONTINUE);
    dsd_event_sync_slot(r.opts, r.state, 0U);
    voice_frames(&r, 0, 2);
    dsd_decode_clock_set_media_ns(65ULL * 1000000000ULL);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    const char* pub = record(&g_j, "publish", 0);
    rc |=
        expect_int("the start is the second the call began", pub ? field_ll(pub, "start") : -9, (long long)anchor + 59);
    rig_close(&r);
    return rc;
}

/* An NFM block as the finalizer hands it while -P records (issue #668): its end read on the decode clock's wall time,
   before any file work. */
static void
nfm_block_heard_now(dsd_opts* opts, dsd_state* state) {
    static float block[960];
    for (int i = 0; i < 960; i++) {
        block[i] = (float)((i % 48) * 400 - 9600);
    }
    const dsd_analog_call_block call = {
        .samples = block,
        .count = 960,
        .sample_rate = 48000,
        .input = DSD_ANALOG_CALL_RECEPTION,
        .ramp_end = 0,
        .qualified_carrier = 1,
        .wall_end_s = dsd_decode_now_realtime_s(),
    };
    dsd_engine_analog_call_block(opts, state, &call);
}

/* Clean RECEPTION blocks of the running reception until it ran DSD_ANALOG_CALL_MIN_MS and became a call (issue #667),
   so its recording publishes; what a test checks of the recording's start is its opening block's alone. */
static void
nfm_qualify(dsd_opts* opts, dsd_state* state) {
    static float block[960];
    for (int i = 0; i < 960; i++) {
        block[i] = 3000.0f;
    }
    for (int i = 0; i < DSD_ANALOG_CALL_MIN_MS / 20 && !dsd_engine_analog_call_active(state); i++) {
        dsd_test_analog_call_feed(opts, state, block, 960, 48000, DSD_ANALOG_CALL_RECEPTION);
    }
}

/* A recording close that takes three seconds of decode time, as a slow disk does under a live clock. */
static int
slow_close(SNDFILE* file) {
    dsd_decode_clock_test_set_ns(dsd_decode_now_mono_ns() + 3000000000ULL);
    return sf_close(file);
}

/* The start of the @p nth published 48 kHz recording (an NFM call's), or -9. */
static long long
nfm_start(int nth) {
    const char* pub = NULL;
    for (int i = 0, seen = 0; (pub = record(&g_j, "publish", i)) != NULL; i++) {
        if (field_ll(pub, "rate") == 48000 && seen++ == nth) {
            return field_ll(pub, "start");
        }
    }
    return -9;
}

/* Issue #668: an NFM recording starts where its first block began, the block's end in wall time (read before any file
   I/O) less its span, floored once to whole seconds. A block that ends 10 ms into a second began in the one before,
   which the row (accepted at the block's end) has not reached; a block that began exactly on a second is filed under
   it; and a recording opened after the call before it closed on a slow disk still starts at its block, not after the
   close. Before, the start was the later of the open time read after the open and the row's start. */
static int
test_nfm_start_is_its_first_block(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_nfm_start");
    setup_nfm(r.opts, r.state);
    submit(&r, DSD_APP_CMD_WAV_START);
    clock_at_ms(k_t0 + 10, 10);
    nfm_block_heard_now(r.opts, r.state);
    clock_at_ms(k_t0 + 10, 30);
    nfm_block_heard_now(r.opts, r.state);
    nfm_qualify(r.opts, r.state);
    clock_at(k_t0 + 12);
    dsd_engine_analog_call_end(r.opts, r.state);
    clock_at_ms(k_t0 + 20, 20);
    nfm_block_heard_now(r.opts, r.state);
    nfm_qualify(r.opts, r.state);
    clock_at(k_t0 + 22);
    dsd_engine_analog_call_end(r.opts, r.state);
    clock_at_ms(k_t0 + 30, 520);
    nfm_block_heard_now(r.opts, r.state);
    nfm_qualify(r.opts, r.state);
    /* A new tone ends that call and closes its recording, slowly, before the next one opens. */
    clock_at_ms(k_t0 + 31, 500);
    r.state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_DCS;
    r.state->analog_rx.dcs_code = 023;
    r.state->analog_rx.dcs_inverted = 1;
    dsd_wav_test_set_io(NULL, slow_close);
    nfm_block_heard_now(r.opts, r.state);
    dsd_wav_test_set_io(NULL, NULL);
    nfm_qualify(r.opts, r.state);
    clock_at(k_t0 + 40);
    dsd_engine_analog_call_end(r.opts, r.state);
    reload(&r);
    rc |= expect_int("a block that began in the second before", nfm_start(0), (long long)k_t0 + 9);
    rc |= expect_int("a block that began on a second", nfm_start(1), (long long)k_t0 + 20);
    rc |= expect_int("the call before the slow close", nfm_start(2), (long long)k_t0 + 30);
    rc |= expect_int("a slow close before the open does not move it", nfm_start(3), (long long)k_t0 + 31);
    rig_close(&r);
    return rc;
}

/* One NFM block of 960 at 48 kHz that ends now: @p input, eligible from @p eligible_from (silent before it). */
static void
nfm_block_ending_now(dsd_opts* opts, dsd_state* state, int input, size_t eligible_from) {
    static float block[960];
    for (size_t i = 0; i < 960; i++) {
        block[i] = i < eligible_from ? 0.0f : 3000.0f;
    }
    const dsd_analog_call_block call = {
        .samples = block,
        .heard = block,
        .count = 960,
        .sample_rate = 48000,
        .input = input,
        .eligible_from = eligible_from,
        .ramp_end = 0,
        .qualified_carrier = input == DSD_ANALOG_CALL_RECEPTION,
        .wall_end_s = dsd_decode_now_realtime_s(),
    };
    dsd_engine_analog_call_block(opts, state, &call);
}

/* Issue #668: a recording that opens with a pre-roll starts where its first sample was heard: 100 ms of pre-roll
   before a block that ends 50 ms into a second began in the second before. A block whose tone turned allowed inside it
   (its first 10 ms not) drops the pre-roll, and the recording starts at the eligible point: 5 ms into the second the
   block ended in, where its whole span reaches back into the one before. */
static int
test_nfm_start_moves_back_by_its_preroll(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_nfm_preroll");
    setup_nfm(r.opts, r.state);
    r.opts->analog_record_preroll_ms = 200;
    submit(&r, DSD_APP_CMD_WAV_START);
    for (int i = 0; i < 5; i++) {
        clock_at_ms(k_t0 + 39, 930 + (uint64_t)i * 20);
        nfm_block_ending_now(r.opts, r.state, DSD_ANALOG_CALL_HOLDABLE, 0);
    }
    clock_at_ms(k_t0 + 40, 50);
    nfm_block_ending_now(r.opts, r.state, DSD_ANALOG_CALL_RECEPTION, 0);
    nfm_qualify(r.opts, r.state);
    clock_at(k_t0 + 41);
    dsd_engine_analog_call_end(r.opts, r.state);
    clock_at_ms(k_t0 + 49, 990);
    nfm_block_ending_now(r.opts, r.state, DSD_ANALOG_CALL_HOLDABLE, 0);
    clock_at_ms(k_t0 + 50, 15);
    nfm_block_ending_now(r.opts, r.state, DSD_ANALOG_CALL_RECEPTION, 480);
    nfm_qualify(r.opts, r.state);
    clock_at(k_t0 + 51);
    dsd_engine_analog_call_end(r.opts, r.state);
    /* At pre-roll 0 an unclean opening is trimmed the same way, and stamped from its eligible point too. */
    r.opts->analog_record_preroll_ms = 0;
    clock_at_ms(k_t0 + 59, 990);
    nfm_block_ending_now(r.opts, r.state, DSD_ANALOG_CALL_HOLDABLE, 0);
    clock_at_ms(k_t0 + 60, 15);
    nfm_block_ending_now(r.opts, r.state, DSD_ANALOG_CALL_RECEPTION, 480);
    nfm_qualify(r.opts, r.state);
    clock_at(k_t0 + 61);
    dsd_engine_analog_call_end(r.opts, r.state);
    reload(&r);
    rc |= expect_int("the pre-roll's first sample", nfm_start(0), (long long)k_t0 + 39);
    rc |= expect_int("an unclean opening's eligible point", nfm_start(1), (long long)k_t0 + 50);
    rc |= expect_int("an unclean opening's eligible point at pre-roll 0", nfm_start(2), (long long)k_t0 + 60);
    rig_close(&r);
    return rc;
}

/* The start a recorder stamped itself (the NFM call, issue #668) is the one the journal and the rdio-scanner sidecar
   both record, over the open time and the row's start; with none, both keep the later of the two. */
static int
test_stamped_start_is_the_journal_and_sidecar_start(void) {
    /* Seconds after k_t0; a started offset of 0 means the recorder stamped no start. */
    static const struct {
        int opened;
        int started;
        int want;
    } cases[] = {
        {52, 47, 47},
        {52, 0, 52},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const time_t opened = (time_t)(k_t0 + (uint64_t)cases[i].opened);
        const time_t started = cases[i].started > 0 ? (time_t)(k_t0 + (uint64_t)cases[i].started) : 0;
        const long long want_start = (long long)k_t0 + cases[i].want;
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_stamped_start");
        r.opts->rdio_mode = DSD_RDIO_MODE_DIRWATCH;
        r.opts->rdio_system_id = 48;
        clock_at(k_t0 + 50);
        begin_call(&r, DSD_SYNC_P25P1_POS, 1201U, 77U, DSD_CALL_BOUNDARY_BEGIN);
        char temp[DSD_TEST_PATH_MAX];
        SNDFILE* wav = open_wav_file(r.dir, temp, sizeof temp, 48000, 0);
        rc |= expect_true("stamped start: file opened", wav != NULL);
        if (wav) {
            static short pcm[960];
            for (int k = 0; k < 960; k++) {
                pcm[k] = (short)(k * 16);
            }
            (void)sf_write_short(wav, pcm, 960);
            clock_at(k_t0 + 55);
            (void)close_and_publish_call_wav_ex(wav, r.opts, r.state, 0, temp, r.dir, opened, started);
        }
        reload(&r);
        rc |= expect_int("stamped start: the journal's", nfm_start(0), want_start);
        const char* pub = record(&g_j, "publish", 0);
        char sidecar[DSD_TEST_PATH_MAX * 2];
        char body[4096];
        DSD_MEMSET(body, 0, sizeof body);
        if (pub && dsd_test_path_join(sidecar, sizeof sidecar, r.dir, field_s(pub, "file")) == 0
            && strlen(sidecar) > 4) {
            const size_t stem = strlen(sidecar) - 4; /* the ".wav" the sidecar's ".json" replaces */
            DSD_SNPRINTF(sidecar + stem, sizeof sidecar - stem, "%s", ".json");
            FILE* f = dsd_fopen_existing_regular_file(sidecar, "rb");
            if (f) {
                const size_t got = fread(body, 1, sizeof body - 1, f);
                body[got] = '\0';
                (void)fclose(f);
            }
        }
        char want[64];
        DSD_SNPRINTF(want, sizeof want, "\"start_time\": %lld,", want_start);
        rc |= expect_true("stamped start: the sidecar's is the journal's", strstr(body, want) != NULL);
        rig_close(&r);
    }
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
        rc |= expect_str("in the recording folder", err ? field_s(err, "folder") : "", json_path(r.canon));
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

/* The sidecar beside the published recording @p pub names, read into @p body ("" when there is none). */
static void
read_published_sidecar(const rig* r, const char* pub, char* body, size_t body_size) {
    body[0] = '\0';
    char sidecar[DSD_TEST_PATH_MAX * 2];
    if (!pub || dsd_test_path_join(sidecar, sizeof sidecar, r->dir, field_s(pub, "file")) != 0
        || strlen(sidecar) <= 4) {
        return;
    }
    const size_t stem = strlen(sidecar) - 4; /* the ".wav" the sidecar's ".json" replaces */
    DSD_SNPRINTF(sidecar + stem, sizeof sidecar - stem, "%s", ".json");
    FILE* f = dsd_fopen_existing_regular_file(sidecar, "rb");
    if (f) {
        const size_t n = fread(body, 1, body_size - 1, f);
        body[n] = '\0';
        (void)fclose(f);
    }
}

/* Issue #931 B4: an API upload of a call with no system ID is skipped, not failed: the close logs no export failure,
   its journal record says no upload was queued, and the run says once why the call was not uploaded (each run anew,
   dsd_rdio_session_reset()). The sidecar is still put in place. */
static int
test_rdio_no_system_skip_is_no_failure(void) {
    int rc = 0;
    rig r;
    rc |= rig_open(&r, "dsdneo_journal_rdio_no_system");
    r.opts->rdio_mode = DSD_RDIO_MODE_API;
    r.opts->rdio_system_id = 0;
    r.opts->rdio_upload_timeout_ms = 100;
    r.opts->rdio_upload_retries = 1;
    DSD_SNPRINTF(r.opts->rdio_api_url, sizeof r.opts->rdio_api_url, "%s", "http://127.0.0.1:1");
    DSD_SNPRINTF(r.opts->rdio_api_key, sizeof r.opts->rdio_api_key, "%s", "test-key");
    dsd_rdio_session_reset();
    submit(&r, DSD_APP_CMD_WAV_START);
    begin_call(&r, DSD_SYNC_P25P1_POS, 1410U, 14U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 2);
    dsd_test_capture_stderr cap;
    const int capturing = dsd_test_capture_stderr_begin(&cap, "journal_no_system") == 0;
    end_call(&r, DSD_CALL_END_EXPLICIT);
    char out[8192] = "";
    if (capturing) {
        dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, out, sizeof out);
    }
    dsd_rdio_upload_shutdown();
    reload(&r);
    const char* pub = record(&g_j, "publish", 0);
    rc |= expect_true("no system ID: published", pub != NULL);
    rc |= expect_str("no system ID: no upload queued", pub ? field_s(pub, "rdio") : "", "false");
    rc |= expect_true("no system ID: stderr captured", capturing);
#ifdef USE_CURL
    rc |= expect_true("no system ID: no export failure", strstr(out, "Rdio export failed") == NULL);
    rc |= expect_true("no system ID: said why",
                      strstr(out, "Rdio API upload skipped: no system ID (set --rdio-system-id)") != NULL);
#endif
    char body[4096];
    read_published_sidecar(&r, pub, body, sizeof body);
    rc |= expect_true("no system ID: the sidecar is in place", strstr(body, "\"talkgroup\": 1410,") != NULL);
    r.opts->rdio_mode = DSD_RDIO_MODE_OFF;
    rig_close(&r);
    return rc;
}

/* Issue #668 and the #708 contract for NFM: an NFM call's recording is exported through the real closer, and the
   journal's publish record says whether its API upload was queued, which the exporter decides before the record is
   written. A radio input's call goes up under its frequency in kHz to the analog system, its sidecar beside it, and
   the journal's target stays empty (the row has none). An audio input's call is not exported without
   --rdio-analog-talkgroup, and is with it. */
static int
test_nfm_rdio_export_is_journaled(void) {
    static const struct {
        const char* label;
        int radio;
        int mode;
        int fallback;
        const char* talkgroup; /* NULL: no sidecar */
        int queued;
    } cases[] = {
        {"nfm on a radio input", 1, DSD_RDIO_MODE_BOTH, 0, "\"talkgroup\": 154437,", 1},
        {"nfm on an audio input", 0, DSD_RDIO_MODE_DIRWATCH, 0, NULL, 0},
        {"nfm on an audio input with a fallback", 0, DSD_RDIO_MODE_DIRWATCH, 4242, "\"talkgroup\": 4242,", 0},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_nfm_rdio");
        setup_nfm(r.opts, r.state);
        r.state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_NONE;
        if (cases[i].radio) {
            r.opts->audio_in_type = AUDIO_IN_RTL;
            r.opts->rtlsdr_center_freq = 154437500U;
            /* A squelch above -110 dB: radio input keeps analog calls only with one (issue #667 D2). */
            r.opts->rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
            r.opts->rtl_squelch_level = dsd_squelch_level_from_sql(-60.0);
        }
        r.opts->rdio_mode = cases[i].mode;
        r.opts->rdio_system_id = 7;
        r.opts->rdio_analog_system_id = 9;
        r.opts->rdio_analog_talkgroup = cases[i].fallback;
        r.opts->rdio_upload_timeout_ms = 100;
        r.opts->rdio_upload_retries = 1;
        r.opts->rdio_api_delete_after_upload = 1;
        DSD_SNPRINTF(r.opts->rdio_api_url, sizeof r.opts->rdio_api_url, "%s", "http://127.0.0.1:1");
        DSD_SNPRINTF(r.opts->rdio_api_key, sizeof r.opts->rdio_api_key, "%s", "test-key");
        submit(&r, DSD_APP_CMD_WAV_START);
        clock_at(k_t0 + 60);
        nfm_block(r.opts, r.state);
        nfm_block(r.opts, r.state);
        clock_at(k_t0 + 61);
        dsd_engine_analog_call_end(r.opts, r.state);
        dsd_rdio_upload_shutdown();
        reload(&r);
        const char* pub = NULL;
        for (int k = 0; (pub = record(&g_j, "publish", k)) != NULL; k++) {
            if (field_ll(pub, "rate") == 48000) {
                break;
            }
        }
        char tag[160];
        DSD_SNPRINTF(tag, sizeof tag, "%s: published", cases[i].label);
        rc |= expect_true(tag, pub != NULL);
#ifdef USE_CURL
        const char* want_rdio = cases[i].queued ? "true" : "false";
#else
        const char* want_rdio = "false";
#endif
        DSD_SNPRINTF(tag, sizeof tag, "%s: the journal says whether an upload was queued", cases[i].label);
        rc |= expect_str(tag, pub ? field_s(pub, "rdio") : "", want_rdio);
        DSD_SNPRINTF(tag, sizeof tag, "%s: no target on the row", cases[i].label);
        rc |= expect_str(tag, pub ? field_s(pub, "target") : "?", "");
        char body[4096];
        read_published_sidecar(&r, pub, body, sizeof body);
        DSD_SNPRINTF(tag, sizeof tag, "%s: sidecar", cases[i].label);
        if (cases[i].talkgroup == NULL) {
            rc |= expect_str(tag, body, "");
        } else {
            rc |= expect_true(tag, strstr(body, cases[i].talkgroup) != NULL && strstr(body, "\"system\": 9,") != NULL);
        }
        char path[DSD_TEST_PATH_MAX * 2];
        FILE* kept = (pub && dsd_test_path_join(path, sizeof path, r.dir, field_s(pub, "file")) == 0)
                         ? dsd_fopen_existing_regular_file(path, "rb")
                         : NULL;
        DSD_SNPRINTF(tag, sizeof tag, "%s: the recording is kept", cases[i].label);
        rc |= expect_true(tag, kept != NULL);
        if (kept) {
            (void)fclose(kept);
        }
        r.opts->rdio_mode = DSD_RDIO_MODE_OFF;
        rig_close(&r);
    }
    return rc;
}

/* --- Issue #667: analog recordings named by their mode, and the discard of one that is never published. --- */

/* An analog call on slot 0 heard in @p mode, as the tracker observes one: no identity, target 0. */
static void
begin_analog_call(rig* r, uint8_t mode) {
    const dsd_call_observation call = {
        .protocol = DSD_SYNC_ANALOG,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
        .frequency_hz = mode == DSD_ANALOG_MODE_UNKNOWN ? 0 : 154430000,
        .analog_mode = mode,
    };
    (void)dsd_call_state_observe(r->state, &call, DSD_CALL_BOUNDARY_BEGIN);
    dsd_event_sync_slot(r->opts, r->state, 0);
}

/* A per-call segment opened in the rig's folder and written with one 20 ms block; NULL when the open failed. */
static SNDFILE*
analog_segment(rig* r, char* path, size_t path_size) {
    SNDFILE* wav = open_wav_file(r->opts->wav_out_dir, path, path_size, 48000, 0);
    static short block[960];
    for (int i = 0; i < 960; i++) {
        block[i] = (short)((i % 48) * 400 - 9600);
    }
    dsd_wav_segment_write(wav, block, 960, "analog test");
    return wav;
}

typedef struct {
    int files;
    int named; /* published under the analog name of @c mode, at @c freq (NULL: FREQ_UNKNOWN), with no tone */
    const char* mode;
    const char* freq;
} analog_names;

static int
count_analog_names_cb(const char* name, void* user) {
    analog_names* names = (analog_names*)user;
    if (strstr(name, ".jsonl") != NULL) {
        return 0;
    }
    names->files++;
    names->named += dsd_test_analog_recording_name_is(name, names->mode, names->freq, NULL, NULL);
    return 0;
}

static analog_names
list_analog_names(const rig* r, const char* mode, const char* freq) {
    analog_names names = {0, 0, mode, freq};
    (void)dsd_dir_list(r->dir, count_analog_names_cb, &names);
    return names;
}

/* An analog recording is named by the mode its call was heard in, closed with its row (the row's mode) or without one
   (a teardown's, by the mode the call kept): _NFM, _AM, or _ANALOG on an audio input, where the mode is not known. Main
   named every one _NFM. */
static int
test_analog_recordings_are_named_by_mode(void) {
    static const struct {
        uint8_t mode;
        const char* token;
        const char* freq; /* begin_analog_call()'s frequency, NULL for none */
    } k_modes[] = {
        {DSD_ANALOG_MODE_NFM, "NFM", "154430000"},
        {DSD_ANALOG_MODE_AM, "AM", "154430000"},
        {DSD_ANALOG_MODE_UNKNOWN, "ANALOG", NULL},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof k_modes / sizeof k_modes[0]; i++) {
        char tag[96];
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_analog_mode");
        begin_analog_call(&r, k_modes[i].mode);
        char path[1024];
        SNDFILE* wav = analog_segment(&r, path, sizeof path);
        (void)close_and_publish_call_wav(wav, r.opts, r.state, 0, path, r.opts->wav_out_dir, dsd_decode_time());
        reload(&r);
        const char* pub = record(&g_j, "publish", 0);
        DSD_SNPRINTF(tag, sizeof tag, "%s: the row's recording is named by its mode", k_modes[i].token);
        rc |= expect_true(tag, pub
                                   && dsd_test_analog_recording_name_is(field_s(pub, "file"), k_modes[i].token,
                                                                        k_modes[i].freq, NULL, NULL));
        DSD_SNPRINTF(tag, sizeof tag, "%s: the journal names the file on disk", k_modes[i].token);
        rc |= expect_int(tag, list_analog_names(&r, k_modes[i].token, k_modes[i].freq).named, 1);
        end_call(&r, DSD_CALL_END_EXPLICIT);

        /* No row left: the mode and frequency the call kept. */
        wav = analog_segment(&r, path, sizeof path);
        const dsd_analog_wav_name kept = {
            .mode = k_modes[i].mode,
            .freq_hz = k_modes[i].freq ? 154430000 : 0,
        };
        (void)close_and_rename_analog_wav_file(wav, path, r.opts->wav_out_dir, &kept);
        const analog_names names = list_analog_names(&r, k_modes[i].token, k_modes[i].freq);
        DSD_SNPRINTF(tag, sizeof tag, "%s: a rowless recording is named by the mode the call kept", k_modes[i].token);
        rc |= expect_true(tag, names.files == 2 && names.named == 2);
        rig_close(&r);
    }
    return rc;
}

static int g_unremoved_warnings;
static int g_close_warnings;

static void
count_discard_log_lines(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    if (level != LOG_LEVEL_WARN || !text) {
        return;
    }
    g_unremoved_warnings += strstr(text, "could not be removed") != NULL;
    /* Only the publishing closer writes this line: counted so a discard routed through that closer fails the
       "nothing logged" checks below, not because a discard could write it. */
    g_close_warnings += strstr(text, "did not close cleanly") != NULL;
}

/* The close seam, which only the segment table's close reaches (wav_segment_close(), after it forgets the segment and
   its latch): counted. */
static int g_segment_closes;

static int
counting_close(SNDFILE* file) {
    g_segment_closes++;
    return sf_close(file);
}

static int
file_exists(const char* path) {
    FILE* f = dsd_fopen_existing_regular_file(path, "rb");
    if (!f) {
        return 0;
    }
    (void)fclose(f);
    return 1;
}

/* The recording failures of @p kind counted so far (a dsd_wav_recording_error), read into a local: an array member of
   a struct returned by value cannot be indexed on MSVC (C4223). */
static long long
recording_failures(int kind) {
    dsd_wav_recording_status status;
    DSD_MEMSET(&status, 0, sizeof status);
    dsd_wav_recording_status_get(&status);
    return (long long)status.failures[kind];
}

/* The temporary recording of an analog reception that never became a call is discarded: closed through the segment
   table, so a short write's latch goes with it, its file removed, nothing journaled, published or exported. A close
   that fails still removes it, and counts no failure. */
static int
test_unpublished_analog_recording_is_discarded(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_analog_discard");
    dsd_neo_log_set_tap(count_discard_log_lines, NULL);
    g_unremoved_warnings = 0;
    g_close_warnings = 0;
    begin_analog_call(&r, DSD_ANALOG_MODE_NFM);

    char path[1024];
    g_segment_closes = 0;
    dsd_wav_test_set_io(short_write, counting_close);
    SNDFILE* wav = analog_segment(&r, path, sizeof path);
    rc |= expect_true("discard: the segment opened", wav != NULL && file_exists(path));
    rc |= expect_int("discard: its short write latched it", recording_failures(DSD_WAV_RECORDING_ERROR_WRITE), 1);
    rc |= expect_true("discard: returns no handle", discard_analog_wav_file(wav, path) == NULL);
    dsd_wav_test_set_io(NULL, NULL);
    rc |= expect_int("discard: closed through the segment table", g_segment_closes, 1);
    /* What a write already reported stays reported (D1): the discard drops the latch, not the count. */
    rc |= expect_int("discard: the write failure stays counted", recording_failures(DSD_WAV_RECORDING_ERROR_WRITE), 1);
    rc |= expect_true("discard: the file is gone", !file_exists(path));

    dsd_wav_test_set_io(NULL, failing_close);
    wav = analog_segment(&r, path, sizeof path);
    (void)discard_analog_wav_file(wav, path);
    dsd_wav_test_set_io(NULL, NULL);
    rc |= expect_true("discard after a failed close: the file is gone", !file_exists(path));
    rc |= expect_int("discard after a failed close: counted as no close failure",
                     recording_failures(DSD_WAV_RECORDING_ERROR_CLOSE), 0);
    /* Neither the removal warning nor the publishing closer's (count_discard_log_lines()). */
    rc |= expect_int("discard after a failed close: nothing logged", g_close_warnings + g_unremoved_warnings, 0);

    reload(&r);
    rc |= expect_int("discard: nothing journaled", g_j.count, 0);
    rc |= expect_int("discard: no recording left", list_analog_names(&r, "NFM", "154430000").files, 0);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    dsd_neo_log_set_tap(NULL, NULL);
    rig_close(&r);
    return rc;
}

/* A segment whose open failed keeps its diagnostics (the log line, the journal's open error), and discarding it adds
   nothing: no second record, no file. */
static int
test_failed_analog_open_keeps_its_error_through_a_discard(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_analog_open_fail");
    begin_analog_call(&r, DSD_ANALOG_MODE_NFM);
    dsd_wav_test_fail_opens_after(0);
    char path[1024];
    SNDFILE* wav = analog_segment(&r, path, sizeof path);
    dsd_wav_test_fail_opens_after(-1);
    rc |= expect_true("open failure: no segment", wav == NULL && path[0] == '\0');
    rc |= expect_true("open failure: the discard returns no handle", discard_analog_wav_file(wav, path) == NULL);
    reload(&r);
    rc |= expect_int("open failure: one record", g_j.count, 1);
    const char* err = record(&g_j, "error", 0);
    rc |= expect_str("open failure: its open error", err ? field_s(err, "kind") : "", "open");
    rc |= expect_int("open failure: counted", recording_failures(DSD_WAV_RECORDING_ERROR_OPEN), 1);
    rc |= expect_int("open failure: no file", list_analog_names(&r, "NFM", "154430000").files, 0);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    rig_close(&r);
    return rc;
}

/* A radio list change applied after the call's last render, then WAV_STOP before the next render, as one command drain
   delivers them: the stop's close follows the policy in force (issue #773 D7). A radio row turned stream=off keeps the
   recording local (no DirWatch sidecar); with stream on the sidecar is written. */
static int
test_wav_stop_follows_a_fresh_stream_verdict(void) {
    int rc = 0;
    for (int blocked = 0; blocked <= 1; blocked++) {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_stream");
        r.opts->rdio_mode = DSD_RDIO_MODE_DIRWATCH;
        r.opts->rdio_system_id = 48;
        submit(&r, DSD_APP_CMD_WAV_START);
        begin_call(&r, DSD_SYNC_P25P1_POS, 1500U, 42U, DSD_CALL_BOUNDARY_BEGIN);
        voice_frames(&r, 0, 2);
        rc |= expect_int("radio row", dsd_test_install_radio_policy(r.state, 42U, "A", 1, 1, blocked ? 0 : 1), 0);
        submit(&r, DSD_APP_CMD_WAV_STOP);
        reload(&r);
        const char* pub = record(&g_j, "publish", 0);
        rc |= expect_true("the stopped recording is published", pub != NULL);
        if (pub) {
            char path[DSD_TEST_PATH_MAX * 2];
            (void)dsd_test_path_join(path, sizeof path, r.dir, field_s(pub, "file"));
            char sidecar[DSD_TEST_PATH_MAX * 2];
            DSD_SNPRINTF(sidecar, sizeof sidecar, "%s", path);
            char* ext = strrchr(sidecar, '.');
            if (ext != NULL && strcmp(ext, ".wav") == 0) {
                DSD_SNPRINTF(ext, sizeof sidecar - (size_t)(ext - sidecar), "%s", ".json");
            }
            FILE* f = dsd_fopen_existing_regular_file(sidecar, "rb");
            rc |= expect_true(blocked ? "a radio turned stream=off keeps the stopped recording local"
                                      : "with stream on the stopped recording is exported",
                              (f != NULL) == !blocked);
            if (f) {
                fclose(f);
                (void)remove(sidecar);
            }
        }
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

/* --- Signal search visits (issue #809): each recording names the visit it was received on. --- */

static const uint64_t k_visit_session = 1730000000000000ULL;

/* @p line names the visit "<session>:<hit>" as @p want. */
static int
expect_visit(const char* what, const char* line, const char* want) {
    char tag[160];
    DSD_SNPRINTF(tag, sizeof tag, "%s: a publish line", what);
    if (expect_true(tag, line != NULL)) {
        return 1;
    }
    DSD_SNPRINTF(tag, sizeof tag, "%s: names the visit", what);
    if (expect_true(tag, strstr(line, "\"search\":") != NULL)) {
        return 1;
    }
    return expect_str(what, field_s(line, "search"), want);
}

/* The @p nth (0-based) publish line at @p rate, or NULL. */
static const char*
publish_at_rate(int rate, int nth) {
    const char* line = NULL;
    for (int i = 0; (line = record(&g_j, "publish", i)) != NULL; i++) {
        if (field_ll(line, "rate") == rate && nth-- == 0) {
            return line;
        }
    }
    return NULL;
}

static int
count_publish_at_rate(int rate) {
    int n = 0;
    while (publish_at_rate(rate, n) != NULL) {
        n++;
    }
    return n;
}

/* A digital call's commit rotates its slot file under the visit: the key comes after `source`, two decimal numbers,
   and the whole uint64_t range is written as text. */
static int
test_digital_commit_names_visit(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_visit_commit");
    dsd_signal_search_visit_set(k_visit_session, 7U);
    submit(&r, DSD_APP_CMD_WAV_START);
    begin_call(&r, DSD_SYNC_P25P1_POS, 100U, 77U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 3);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    const char* pub = record(&g_j, "publish", 0);
    rc |= expect_int("one publish", count_records(&g_j, "publish"), 1);
    rc |= expect_true("the visit follows the source and ends the record",
                      pub && strstr(pub, ",\"source\":\"77\",\"search\":\"1730000000000000:7\"}") != NULL);

    dsd_signal_search_visit_set(UINT64_MAX, UINT64_MAX);
    begin_call(&r, DSD_SYNC_P25P1_POS, 101U, 78U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 3);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    rc |= expect_int("a second publish", count_records(&g_j, "publish"), 2);
    rc |=
        expect_visit("the largest ids, exact", record(&g_j, "publish", 1), "18446744073709551615:18446744073709551615");
    rig_close(&r);
    return rc;
}

/* An NFM call's recording, published at its end, names the visit it was heard on. */
static int
test_nfm_recording_names_visit(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_visit_nfm");
    setup_nfm(r.opts, r.state);
    dsd_signal_search_visit_set(k_visit_session, 8U);
    submit(&r, DSD_APP_CMD_WAV_START);
    nfm_block(r.opts, r.state);
    nfm_block(r.opts, r.state);
    dsd_engine_analog_call_end(r.opts, r.state);
    reload(&r);
    rc |= expect_int("one NFM recording", count_publish_at_rate(48000), 1);
    rc |= expect_visit("nfm", publish_at_rate(48000, 0), "1730000000000000:8");
    rig_close(&r);
    return rc;
}

/* DMR's two slots talking on one visit: both recordings name it. */
static int
test_both_slots_tagged(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_visit_slots");
    dsd_signal_search_visit_set(k_visit_session, 9U);
    submit(&r, DSD_APP_CMD_WAV_START);
    begin_call_on(&r, 0U, DSD_SYNC_DMR_BS_VOICE_POS, 200U, 20U, DSD_CALL_BOUNDARY_BEGIN);
    begin_call_on(&r, 1U, DSD_SYNC_DMR_BS_VOICE_POS, 201U, 21U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 2);
    voice_frames(&r, 1, 2);
    end_call_on(&r, 0U, DSD_CALL_END_EXPLICIT);
    end_call_on(&r, 1U, DSD_CALL_END_EXPLICIT);
    reload(&r);
    rc |= expect_int("one recording per slot", count_records(&g_j, "publish"), 2);
    for (int i = 0; i < 2; i++) {
        const char* pub = record(&g_j, "publish", i);
        rc |= expect_int("slot 1, then slot 2", pub ? field_ll(pub, "slot") : -9, i);
        rc |= expect_visit("both slots", pub, "1730000000000000:9");
    }
    rig_close(&r);
    return rc;
}

/* The closers that publish before their call's row commits name the visit too: a WAV stop mid-call and the EDACS
   analog rotation. */
static int
test_pending_closer_names_visit(void) {
    int rc = 0;
    {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_visit_stop");
        dsd_signal_search_visit_set(k_visit_session, 10U);
        submit(&r, DSD_APP_CMD_WAV_START);
        begin_call(&r, DSD_SYNC_P25P1_POS, 400U, 9U, DSD_CALL_BOUNDARY_BEGIN);
        voice_frames(&r, 0, 2);
        submit(&r, DSD_APP_CMD_WAV_STOP);
        end_call(&r, DSD_CALL_END_EXPLICIT);
        reload(&r);
        const char* pub = record(&g_j, "publish", 0);
        rc |= expect_true("wav stop: published pending", pub && strcmp(field_s(pub, "pending"), "true") == 0);
        rc |= expect_visit("wav stop", pub, "1730000000000000:10");
        rig_close(&r);
    }
    {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_visit_edacs");
        dsd_signal_search_visit_set(k_visit_session, 11U);
        submit(&r, DSD_APP_CMD_WAV_START);
        begin_call(&r, DSD_SYNC_EDACS_POS, 321U, 1234U, DSD_CALL_BOUNDARY_BEGIN);
        static short analog[3][960];
        for (int i = 0; i < 960; i++) {
            analog[0][i] = (short)(i * 8);
            analog[1][i] = (short)(-i * 8);
            analog[2][i] = (short)(i * 4);
        }
        edacs_write_analog_wav(r.opts, r.state, analog[0], analog[1], analog[2]);
        r.opts->wav_out_f = close_and_publish_call_wav(r.opts->wav_out_f, r.opts, r.state, 0, r.opts->wav_out_file,
                                                       r.opts->wav_out_dir, r.opts->wav_out_open_time);
        reload(&r);
        const char* pub = record(&g_j, "publish", 0);
        rc |= expect_true("edacs rotation: published pending", pub && strcmp(field_s(pub, "pending"), "true") == 0);
        rc |= expect_visit("edacs rotation", pub, "1730000000000000:11");
        rig_close(&r);
    }
    return rc;
}

/* Outside a visit no record carries the key: a digital commit, an NFM call and a pending closer. Nor does a record
   whose visit has a zero id, which a reader would reject: the writer never writes one. */
static int
test_no_visit_no_key(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_visit_none");
    setup_nfm(r.opts, r.state);
    submit(&r, DSD_APP_CMD_WAV_START);
    nfm_block(r.opts, r.state);
    nfm_block(r.opts, r.state);
    dsd_engine_analog_call_end(r.opts, r.state);
    begin_call(&r, DSD_SYNC_P25P1_POS, 300U, 1U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 2);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    begin_call(&r, DSD_SYNC_P25P1_POS, 301U, 2U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 2);
    submit(&r, DSD_APP_CMD_WAV_STOP);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    static const uint64_t zero_half[2][2] = {{0U, 5U}, {5U, 0U}};
    for (int i = 0; i < 2; i++) {
        wav_journal_publish_record rec;
        DSD_MEMSET(&rec, 0, sizeof rec);
        rec.path = r.journal;
        rec.owner.kind = DSD_WAV_OWNER_NONE;
        rec.owner.slot = DSD_WAV_OWNER_SLOT_UNKNOWN;
        rec.has_search = 1;
        rec.search_session = zero_half[i][0];
        rec.search_hit = zero_half[i][1];
        wav_journal_publish(&rec);
    }
    reload(&r);
    rc |= expect_int("an NFM, a commit, a pending publish and two zero-id visits", count_records(&g_j, "publish"), 5);
    for (int i = 0; i < g_j.count; i++) {
        rc |= expect_true("no record names a visit", strstr(g_j.line[i], "\"search\":") == NULL);
    }
    rig_close(&r);
    return rc;
}

/* A decoder session that ends during a visit (a Stop from the app) ends the search's hit and keeps the tag, and the
   teardown's final closes publish under it (dsd_engine_cleanup(): dsd_engine_finalize_calls() ends the analog call and
   the slots as a sync loss, dsd_engine_cleanup_close_wavs() publishes the slot files). The hit's end and the tag kept
   through the real teardown are pinned by ENGINE_SIGNAL_SEARCH and ENGINE_LIFECYCLE_FINALIZE, which run the search;
   here the final closes run in the teardown's order under the kept tag: an NFM call on slot 1 and a DMR call on
   slot 2, both talking when the session ends. */
static int
test_session_final_closes_keep_visit(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_visit_end");
    setup_nfm(r.opts, r.state);
    dsd_signal_search_visit_set(k_visit_session, 12U);
    submit(&r, DSD_APP_CMD_WAV_START);
    nfm_block(r.opts, r.state);
    nfm_block(r.opts, r.state);
    begin_call_on(&r, 1U, DSD_SYNC_DMR_BS_VOICE_POS, 210U, 22U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 1, 2);

    dsd_engine_analog_call_end(r.opts, r.state);
    end_call_on(&r, 1U, DSD_CALL_END_SYNC_LOSS);
    r.opts->wav_out_f = close_and_publish_call_wav(r.opts->wav_out_f, r.opts, r.state, 0, r.opts->wav_out_file,
                                                   r.opts->wav_out_dir, r.opts->wav_out_open_time);
    r.opts->wav_out_fR = close_and_publish_call_wav(r.opts->wav_out_fR, r.opts, r.state, 1, r.opts->wav_out_fileR,
                                                    r.opts->wav_out_dir, r.opts->wav_out_open_timeR);
    reload(&r);
    rc |= expect_int("the NFM call's recording", count_publish_at_rate(48000), 1);
    rc |= expect_visit("the NFM call at the session's end", publish_at_rate(48000, 0), "1730000000000000:12");
    rc |= expect_int("slot 2's recording", count_publish_at_rate(8000), 1);
    rc |= expect_visit("the slot 2 file at the session's end", publish_at_rate(8000, 0), "1730000000000000:12");
    rig_close(&r);
    return rc;
}

/* A visit's departure is a carrier boundary the engine runs under the visit's tag, which it clears right after; the
   next landing sets the next visit's. The real boundary closes what the visit left on the air (its NFM call, and the
   DMR call on slot 2 through the commit's rotation) under the departing visit, and the next visit's recording names
   the next one. */
static int
test_departure_boundary_tags_departing_visit(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_visit_depart");
    setup_nfm(r.opts, r.state);
    submit(&r, DSD_APP_CMD_WAV_START);
    dsd_signal_search_visit_set(k_visit_session, 13U);
    nfm_block(r.opts, r.state);
    nfm_block(r.opts, r.state);
    begin_call_on(&r, 1U, DSD_SYNC_DMR_BS_VOICE_POS, 220U, 23U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 1, 2);
    dsd_engine_carrier_boundary(r.opts, r.state, DSD_CARRIER_BOUNDARY_TUNE, 0);
    dsd_signal_search_visit_clear();
    reload(&r);
    rc |= expect_int("the boundary published the visit's NFM call", count_publish_at_rate(48000), 1);
    rc |= expect_int("and slot 2's call", count_publish_at_rate(8000), 1);

    dsd_signal_search_visit_set(k_visit_session, 14U);
    setup_nfm(r.opts, r.state);
    nfm_block(r.opts, r.state);
    nfm_block(r.opts, r.state);
    dsd_engine_carrier_boundary(r.opts, r.state, DSD_CARRIER_BOUNDARY_TUNE, 0);
    dsd_signal_search_visit_clear();
    reload(&r);
    rc |= expect_int("the next visit's NFM call", count_publish_at_rate(48000), 2);
    rc |= expect_visit("the departing visit's NFM call", publish_at_rate(48000, 0), "1730000000000000:13");
    rc |= expect_visit("the departing visit's slot 2 call", publish_at_rate(8000, 0), "1730000000000000:13");
    rc |= expect_visit("the next visit's NFM call", publish_at_rate(48000, 1), "1730000000000000:14");
    rig_close(&r);
    return rc;
}

/* --- Issue #809 B4: a slot file holding audio no call owns is closed at every carrier departure. --- */

/* The journal id slot @p slot (0 or 1) records under now. */
static void
slot_id(const rig* r, int slot, char* id, size_t cap) {
    id_of_temp(slot == 1 ? r->opts->wav_out_fileR : r->opts->wav_out_file, id, cap);
}

/* @p line is an orphan's publish: slot @p slot, @p duration_ms long, no row's, not pending and not exported. */
static int
expect_orphan(const char* what, const char* line, int slot, long long duration_ms) {
    char tag[160];
    DSD_SNPRINTF(tag, sizeof tag, "%s: a publish line", what);
    if (expect_true(tag, line != NULL)) {
        return 1;
    }
    int rc = 0;
    DSD_SNPRINTF(tag, sizeof tag, "%s: slot", what);
    rc |= expect_int(tag, field_ll(line, "slot"), slot);
    DSD_SNPRINTF(tag, sizeof tag, "%s: duration", what);
    rc |= expect_int(tag, field_ll(line, "duration_ms"), duration_ms);
    DSD_SNPRINTF(tag, sizeof tag, "%s: no row, not pending, not exported", what);
    rc |= expect_true(tag, strcmp(field_s(line, "row"), "null") == 0 && strcmp(field_s(line, "pending"), "false") == 0
                               && strcmp(field_s(line, "rdio"), "false") == 0);
    return rc;
}

/* Speech heard with no call (TETRA speech ahead of the signalling that admits a call), a tune, and more of it: the tune
   closes the first carrier's audio as its own recording, so the two halves never share a file. */
static int
test_orphan_audio_split_at_tune_boundary(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_orphan_tune");
    submit(&r, DSD_APP_CMD_WAV_START);
    char first[64];
    slot_id(&r, 0, first, sizeof first);
    voice_frames(&r, 0, 10);
    dsd_engine_carrier_boundary(r.opts, r.state, DSD_CARRIER_BOUNDARY_TUNE, 0);
    char second[64];
    slot_id(&r, 0, second, sizeof second);
    rc |= expect_true("the slot file reopened", r.opts->wav_out_f != NULL && strcmp(first, second) != 0);
    voice_frames(&r, 0, 5);
    submit(&r, DSD_APP_CMD_WAV_STOP);
    reload(&r);
    rc |= expect_int("two recordings, one per carrier", count_records(&g_j, "publish"), 2);
    const char* left = record(&g_j, "publish", 0);
    rc |= expect_orphan("the carrier left", left, 0, 200);
    rc |= expect_true("is the first segment", left && strcmp(field_s(left, "id"), first) == 0);
    const char* next = record(&g_j, "publish", 1);
    rc |= expect_int("the next carrier's half", next ? field_ll(next, "duration_ms") : -9, 100);
    rc |= expect_true("is the reopened segment", next && strcmp(field_s(next, "id"), second) == 0);
    rig_close(&r);
    return rc;
}

/* Both slots' files hold audio with no call: the tune closes each, and each reopens. */
static int
test_orphan_flush_both_slots(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_orphan_slots");
    submit(&r, DSD_APP_CMD_WAV_START);
    char before[2][64];
    slot_id(&r, 0, before[0], sizeof before[0]);
    slot_id(&r, 1, before[1], sizeof before[1]);
    voice_frames(&r, 0, 4);
    voice_frames(&r, 1, 6);
    dsd_engine_carrier_boundary(r.opts, r.state, DSD_CARRIER_BOUNDARY_TUNE, 0);
    reload(&r);
    rc |= expect_int("one recording per slot", count_records(&g_j, "publish"), 2);
    for (int slot = 0; slot < 2; slot++) {
        const char* pub = NULL;
        for (int i = 0; (pub = record(&g_j, "publish", i)) != NULL; i++) {
            if (strcmp(field_s(pub, "id"), before[slot]) == 0) {
                break;
            }
        }
        rc |= expect_orphan(slot == 0 ? "slot 1" : "slot 2", pub, slot, slot == 0 ? 80 : 120);
        char after[64];
        slot_id(&r, slot, after, sizeof after);
        rc |= expect_true("the slot file reopened", (slot == 0 ? r.opts->wav_out_f : r.opts->wav_out_fR) != NULL
                                                        && after[0] != '\0' && strcmp(after, before[slot]) != 0);
    }
    rig_close(&r);
    return rc;
}

/* Nothing to close: an empty slot file stays open as it is (no discard, no new segment), with -P off nothing opens, and
   a static WAV (-w), which holds every carrier by design, is never cut. */
static int
test_orphan_flush_skips_empty(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_orphan_empty");
    dsd_engine_carrier_boundary(r.opts, r.state, DSD_CARRIER_BOUNDARY_TUNE, 0);
    rc |= expect_true("-P off: no slot file opened", r.opts->wav_out_f == NULL && r.opts->wav_out_fR == NULL);

    submit(&r, DSD_APP_CMD_WAV_START);
    char before[2][64];
    slot_id(&r, 0, before[0], sizeof before[0]);
    slot_id(&r, 1, before[1], sizeof before[1]);
    const SNDFILE* files[2] = {r.opts->wav_out_f, r.opts->wav_out_fR};
    dsd_engine_carrier_boundary(r.opts, r.state, DSD_CARRIER_BOUNDARY_TUNE, 0);
    char after[2][64];
    slot_id(&r, 0, after[0], sizeof after[0]);
    slot_id(&r, 1, after[1], sizeof after[1]);
    rc |= expect_true("the empty files stay open", r.opts->wav_out_f == files[0] && r.opts->wav_out_fR == files[1]
                                                       && strcmp(after[0], before[0]) == 0
                                                       && strcmp(after[1], before[1]) == 0);
    reload(&r);
    rc |= expect_int("nothing published", count_records(&g_j, "publish"), 0);
    rc |= expect_int("nothing discarded", count_records(&g_j, "discard"), 0);
    submit(&r, DSD_APP_CMD_WAV_STOP);

    char path[DSD_TEST_PATH_MAX * 2];
    (void)dsd_test_path_join(path, sizeof path, r.dir, "static.wav");
    r.opts->static_wav_file = 1;
    r.opts->wav_out_f = open_static_wav_file(path);
    rc |= expect_true("the static WAV opens", r.opts->wav_out_f != NULL);
    const SNDFILE* static_wav = r.opts->wav_out_f;
    voice_frames(&r, 0, 3);
    dsd_event_flush_orphan_recordings(r.opts, r.state);
    dsd_engine_carrier_boundary(r.opts, r.state, DSD_CARRIER_BOUNDARY_TUNE, 0);
    rc |= expect_true("the static WAV is never cut", r.opts->wav_out_f == static_wav);
    reload(&r);
    rc |= expect_int("and not published", count_records(&g_j, "publish"), 0);
    rig_close(&r);
    return rc;
}

/* A slot whose call is active keeps its file: the call's own commit closes it, whole, under its row. The other slot's
   audio, which no call owns, is closed. */
static int
test_orphan_flush_skips_active_call(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_orphan_active");
    submit(&r, DSD_APP_CMD_WAV_START);
    char call_id[64];
    slot_id(&r, 0, call_id, sizeof call_id);
    begin_call(&r, DSD_SYNC_P25P1_POS, 300U, 30U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 5);
    voice_frames(&r, 1, 3);
    dsd_event_flush_orphan_recordings(r.opts, r.state);
    char after[64];
    slot_id(&r, 0, after, sizeof after);
    rc |= expect_true("the call's file stays open", strcmp(after, call_id) == 0);
    reload(&r);
    rc |= expect_int("only the other slot's audio closed", count_records(&g_j, "publish"), 1);
    rc |= expect_orphan("slot 2", record(&g_j, "publish", 0), 1, 60);

    voice_frames(&r, 0, 5);
    end_call(&r, DSD_CALL_END_EXPLICIT);
    reload(&r);
    const char* pub = record(&g_j, "publish", 1);
    rc |= expect_true("the call's commit publishes its file", pub && strcmp(field_s(pub, "id"), call_id) == 0);
    rc |= expect_int("whole", pub ? field_ll(pub, "duration_ms") : -9, 200);
    rc |= expect_int("under its row", pub ? field_ll(pub, "row") : -9, (long long)r.state->event_history_s[0].push_seq);
    rig_close(&r);
    return rc;
}

/* An ended epoch whose row is held back (an identity-less call that faded: its drop verdict waits out the reacquisition
   window) still owns its audio: the departure closes the file pending on that epoch, as a WAV stop would, and the
   drop that follows unlinks it rather than leaving it no row's from the start. */
static int
test_orphan_flush_held_epoch_is_pending(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_orphan_held");
    submit(&r, DSD_APP_CMD_WAV_START);
    begin_call(&r, DSD_SYNC_P25P1_POS, 0U, 0U, DSD_CALL_BOUNDARY_BEGIN);
    const uint64_t epoch = call_epoch(&r);
    voice_frames(&r, 0, 3);
    char id[64];
    slot_id(&r, 0, id, sizeof id);
    end_call(&r, DSD_CALL_END_SYNC_LOSS);
    reload(&r);
    rc |= expect_int("the faded epoch's row is held", count_records(&g_j, "publish"), 0);
    dsd_event_flush_orphan_recordings(r.opts, r.state);
    reload(&r);
    const char* pub = record(&g_j, "publish", 0);
    rc |= expect_true("its audio closed", pub && strcmp(field_s(pub, "id"), id) == 0);
    rc |= expect_true("pending on its epoch, not exported", pub && strcmp(field_s(pub, "pending"), "true") == 0
                                                                && strcmp(field_s(pub, "rdio"), "false") == 0);
    rc |= expect_int("the epoch", pub ? field_ll(pub, "epoch") : -9, (long long)epoch);
    /* Past the hold: the row is dropped, and the recording it waited for is unlinked. */
    clock_at(g_now_s + 2);
    dsd_event_sync_slot(r.opts, r.state, 0);
    clock_at(g_now_s + 2);
    dsd_event_sync_slot(r.opts, r.state, 0);
    reload(&r);
    rc |= expect_int("the drop unlinks it", count_for_id(&g_j, "unlink", id), 1);
    rc |= expect_int("and links nothing", count_records(&g_j, "link"), 0);
    rig_close(&r);
    return rc;
}

/* A stream gap leaves the receiver on its carrier: audio no call owns stays in its file across it. */
static int
test_stream_gap_keeps_orphan(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_orphan_gap");
    submit(&r, DSD_APP_CMD_WAV_START);
    char id[64];
    slot_id(&r, 0, id, sizeof id);
    voice_frames(&r, 0, 5);
    dsd_engine_carrier_boundary(r.opts, r.state, DSD_CARRIER_BOUNDARY_STREAM_GAP, 0);
    char after[64];
    slot_id(&r, 0, after, sizeof after);
    rc |= expect_true("the file stays open", strcmp(after, id) == 0);
    voice_frames(&r, 0, 5);
    submit(&r, DSD_APP_CMD_WAV_STOP);
    reload(&r);
    rc |= expect_int("one recording", count_records(&g_j, "publish"), 1);
    rc |= expect_orphan("across the gap", record(&g_j, "publish", 0), 0, 200);
    rig_close(&r);
    return rc;
}

/* An orphan closed by the boundary that leaves a Signal search visit names that visit, as everything the visit
   received does. */
static int
test_orphan_flush_names_the_visit(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_orphan_visit");
    submit(&r, DSD_APP_CMD_WAV_START);
    dsd_signal_search_visit_set(k_visit_session, 15U);
    voice_frames(&r, 0, 4);
    dsd_engine_carrier_boundary(r.opts, r.state, DSD_CARRIER_BOUNDARY_TUNE, 0);
    dsd_signal_search_visit_clear();
    voice_frames(&r, 0, 2);
    submit(&r, DSD_APP_CMD_WAV_STOP);
    reload(&r);
    rc |= expect_int("one recording per carrier", count_records(&g_j, "publish"), 2);
    rc |= expect_orphan("the visit's orphan", record(&g_j, "publish", 0), 0, 80);
    rc |= expect_visit("the visit's orphan", record(&g_j, "publish", 0), "1730000000000000:15");
    const char* after = record(&g_j, "publish", 1);
    rc |= expect_true("audio after the visit names none", after && strstr(after, "\"search\"") == NULL);
    rig_close(&r);
    return rc;
}

/* The orphan flush on the P25 watchdog's thread (a trunk control-channel return) beside the decoder thread's commit of
   the slot's ended call (dsd_event_sync_slot() from noCarrier()): the close seam holds the flush inside its close of
   the slot file until the commit closes that file too, or for a bound. A flush that rotates outside the call-state lock
   the commit holds lets both close one SNDFILE (a double close, and one reopened handle lost); serialized, the commit
   waits for the flush and then finds the slot's new, empty file. The seam never closes a handle twice, so the race
   shows as a count rather than a use after free. Run under TSan as well. */
static const SNDFILE* g_race_file;
/* Cleared once the flush's file is really closed: a later file may reuse its address. */
static atomic_int g_race_armed;
static atomic_int g_race_closes;
static atomic_int g_race_flush_closing;

static int
race_close(SNDFILE* file) {
    if (!atomic_load(&g_race_armed) || file != g_race_file) {
        return sf_close(file);
    }
    if (atomic_fetch_add(&g_race_closes, 1) > 0) {
        return 0; /* the second close of one handle: counted, not made */
    }
    atomic_store(&g_race_flush_closing, 1);
    const uint64_t deadline = dsd_realtime_mono_ms() + 500U;
    while (atomic_load(&g_race_closes) < 2 && dsd_realtime_mono_ms() < deadline) {
        dsd_sleep_ms(1U);
    }
    const int closed = sf_close(file);
    atomic_store(&g_race_armed, 0);
    return closed;
}

typedef struct {
    rig* r;
} race_flush_arg;

static DSD_THREAD_RETURN_TYPE
race_flush(void* arg) {
    const race_flush_arg* a = (const race_flush_arg*)arg;
    dsd_event_flush_orphan_recordings(a->r->opts, a->r->state);
    DSD_THREAD_RETURN;
}

static int
test_orphan_flush_serialized_with_commit(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_orphan_race");
    submit(&r, DSD_APP_CMD_WAV_START);
    begin_call(&r, DSD_SYNC_P25P1_POS, 300U, 30U, DSD_CALL_BOUNDARY_BEGIN);
    const uint64_t epoch = call_epoch(&r);
    voice_frames(&r, 0, 5);
    char id[64];
    slot_id(&r, 0, id, sizeof id);
    /* The call ended and its row waits for the decoder's next sync pass: no active call owns the file, so the flush
       takes it, and the commit would rotate it. */
    (void)dsd_call_state_end_ex(r.state, 0U, 0.0, DSD_CALL_END_EXPLICIT);
    g_race_file = r.opts->wav_out_f;
    atomic_store(&g_race_closes, 0);
    atomic_store(&g_race_flush_closing, 0);
    atomic_store(&g_race_armed, 1);
    dsd_wav_test_set_io(NULL, race_close);
    race_flush_arg arg = {&r};
    dsd_thread_t watchdog;
    rc |= expect_int("the watchdog's flush starts", dsd_thread_create(&watchdog, race_flush, &arg), 0);
    const uint64_t deadline = dsd_realtime_mono_ms() + 5000U;
    while (!atomic_load(&g_race_flush_closing) && dsd_realtime_mono_ms() < deadline) {
        dsd_sleep_ms(1U);
    }
    rc |= expect_true("the flush is closing the slot file", atomic_load(&g_race_flush_closing) != 0);
    dsd_event_sync_slot(r.opts, r.state, 0U);
    (void)dsd_thread_join(watchdog);
    dsd_wav_test_set_io(NULL, NULL);
    rc |= expect_int("the slot file closed once", atomic_load(&g_race_closes), 1);
    g_race_file = NULL;
    char after[64];
    slot_id(&r, 0, after, sizeof after);
    rc |= expect_true("the slot holds a new file", r.opts->wav_out_f != NULL && strcmp(after, id) != 0);
    reload(&r);
    rc |= expect_int("published once", count_for_id(&g_j, "publish", id), 1);
    rc |= expect_pending_then_linked("orphan flush beside the commit", id, epoch, &r.state->event_history_s[0],
                                     r.state->event_history_s[0].push_seq);
    rig_close(&r);
    return rc;
}

/* Trunk scan, two P25 targets at 851 and 852 MHz on a stand-in tuner. */
static int
scan_rig_start_with(rig* r, const char* csv) {
    char targets[DSD_TEST_PATH_MAX * 2];
    (void)dsd_test_path_join(targets, sizeof targets, r->dir, "targets.csv");
    FILE* f = dsd_fopen_private(targets, "wb");
    if (!f) {
        return 1;
    }
    const int wrote = fwrite(csv, 1, strlen(csv), f) == strlen(csv);
    int rc = expect_true("targets written", fclose(f) == 0 && wrote);
    r->opts->audio_in_type = AUDIO_IN_RTL;
    r->opts->use_rigctl = 1;
    r->opts->trunk_scan_enabled = 1;
    DSD_SNPRINTF(r->opts->trunk_scan_lists[0].path, sizeof r->opts->trunk_scan_lists[0].path, "%s", targets);
    r->opts->trunk_scan_list_count = 1;
    dsd_trunk_tuning_requests_reset();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = tune, .tune_to_cc_request = tune, .return_to_cc_request = return_to_cc});
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){.get_current_freq_hz = current_frequency});
    trunk_scan_test_set_now(0.0);
    char error[256] = {0};
    rc |= expect_int("scan starts", dsd_engine_trunk_scan_init(r->opts, r->state, error, sizeof error), 0);
    return rc;
}

static int
scan_rig_start(rig* r) {
    return scan_rig_start_with(r, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                                  "a,p25-trunk,851000000,,250,250,\n"
                                  "b,p25-trunk,852000000,,250,250,\n");
}

static void
scan_rig_stop(rig* r) {
    dsd_engine_trunk_scan_shutdown(r->opts, r->state);
    trunk_scan_test_clear_now();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){0});
    dsd_trunk_tuning_requests_reset();
    g_tune_refused = 0;
}

/* A trunk-scan target switch moves the tuner without the carrier boundary: the dwell's rotation (the switch ends the
   outgoing target's calls) and an operator's advance (the switch hands the parked carrier back) each close what the
   carrier left holds with no call. */
static int
test_trunk_scan_switch_flushes_orphans(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_orphan_scan");
    rc |= scan_rig_start(&r);
    rc |= expect_int("on target a", (long long)dsd_engine_trunk_scan_active_index(r.state), 0);
    submit(&r, DSD_APP_CMD_WAV_START);
    voice_frames(&r, 0, 6);
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(r.opts, r.state);
    rc |= expect_int("the dwell hopped to b", (long long)dsd_engine_trunk_scan_active_index(r.state), 1);
    reload(&r);
    rc |= expect_int("a's audio closed at the hop", count_records(&g_j, "publish"), 1);
    rc |= expect_orphan("a's audio", record(&g_j, "publish", 0), 0, 120);

    voice_frames(&r, 0, 4);
    rc |= expect_int("the operator advances",
                     dsd_engine_trunk_scan_control(r.opts, r.state, DSD_TRUNK_SCAN_CONTROL_ADVANCE), 0);
    rc |= expect_int("back on a", (long long)dsd_engine_trunk_scan_active_index(r.state), 0);
    reload(&r);
    rc |= expect_int("b's audio closed at the advance", count_records(&g_j, "publish"), 2);
    rc |= expect_orphan("b's audio", record(&g_j, "publish", 1), 0, 80);
    scan_rig_stop(&r);
    rig_close(&r);
    return rc;
}

/* A park that failed leaves the receiver where it was, and a call can be heard there before the retry's park: the retry
   leaves that carrier, so the call ends with it and its recording closes under its own row, rather than the target's
   restored state taking the slot from under it with the call's audio still in the file. */
static int
test_trunk_scan_retry_park_ends_the_call_left(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_scan_retry");
    g_tune_refused = 1;
    rc |= scan_rig_start_with(&r, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                                  "a,p25-trunk,851000000,,250,250,\n");
    submit(&r, DSD_APP_CMD_WAV_START);
    char id[64];
    slot_id(&r, 0, id, sizeof id);
    begin_call(&r, DSD_SYNC_P25P1_POS, 500U, 50U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 5);
    g_tune_refused = 0;
    trunk_scan_test_set_now(2.5);
    dsd_engine_trunk_scan_tick(r.opts, r.state);
    rc |= expect_int("the retry parked", (long long)g_frequency, 851000000);
    reload(&r);
    const char* pub = record(&g_j, "publish", 0);
    rc |= expect_true("the call's recording closed at the retry", pub && strcmp(field_s(pub, "id"), id) == 0);
    rc |= expect_int("whole", pub ? field_ll(pub, "duration_ms") : -9, 100);
    rc |= expect_int("under its row", pub ? field_ll(pub, "row") : -9, (long long)r.state->event_history_s[0].push_seq);
    rc |= expect_int("the call committed once", (long long)r.state->event_history_s[0].push_seq, 1);
    /* Nothing of it closes again: no second publish, link or unlink, now or at the next tick. */
    trunk_scan_test_set_now(2.6);
    dsd_engine_trunk_scan_tick(r.opts, r.state);
    reload(&r);
    rc |= expect_int("one recording", count_records(&g_j, "publish"), 1);
    rc |= expect_int("nothing pending to resolve", count_records(&g_j, "link") + count_records(&g_j, "unlink"), 0);
    scan_rig_stop(&r);
    rig_close(&r);
    return rc;
}

/* --- Issue #888 D7: which closes give a digital voice row with no talkgroup the fallback number. --- */

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
#define RDIO_CAPTURE 1

/* A stand-in rdio-scanner API on the loopback that keeps the one upload it is sent. */
typedef struct {
    dsd_socket_t sock;
    dsd_thread_t thread;
    char url[64];
    char request[65536];
    size_t used;
    int got;
} capture_server;

static const char*
find_bytes(const char* hay, size_t hay_len, const char* needle) {
    const size_t n = strlen(needle);
    for (size_t i = 0; n > 0U && i + n <= hay_len; i++) {
        if (memcmp(hay + i, needle, n) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

/* Read the request until its Content-Length is in, answering curl's "Expect: 100-continue" at once. */
static void
capture_read(capture_server* s, dsd_socket_t client) {
    size_t body_at = 0;
    long length = -1;
    int continued = 0;
    while (s->used < sizeof s->request - 1U) {
        const int n = dsd_socket_recv(client, s->request + s->used, sizeof s->request - 1U - s->used, 0);
        if (n <= 0) {
            return;
        }
        s->used += (size_t)n;
        s->request[s->used] = '\0';
        const char* end = body_at == 0 ? strstr(s->request, "\r\n\r\n") : NULL;
        if (end) {
            body_at = (size_t)(end - s->request) + 4U;
            const char* cl = strstr(s->request, "Content-Length:");
            char* stop = NULL;
            length = cl ? strtol(cl + strlen("Content-Length:"), &stop, 10) : -1;
            if (cl && stop == cl + strlen("Content-Length:")) {
                length = -1;
            }
        }
        if (body_at != 0 && !continued && strstr(s->request, "100-continue") != NULL) {
            static const char k_continue[] = "HTTP/1.1 100 Continue\r\n\r\n";
            (void)dsd_socket_send(client, k_continue, strlen(k_continue), 0);
            continued = 1;
        }
        if (body_at != 0 && length >= 0 && s->used >= body_at + (size_t)length) {
            s->got = 1;
            return;
        }
    }
}

static DSD_THREAD_RETURN_TYPE
capture_thread(void* arg) {
    capture_server* s = (capture_server*)arg;
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(s->sock, &readfds);
    struct timeval tv = {5, 0};
    if (select(s->sock + 1, &readfds, NULL, NULL, &tv) > 0) {
        dsd_socket_t client = dsd_socket_accept(s->sock, NULL, NULL);
        if (client != DSD_INVALID_SOCKET) {
            (void)dsd_socket_set_recv_timeout(client, 5000);
            capture_read(s, client);
            static const char k_ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
            (void)dsd_socket_send(client, k_ok, strlen(k_ok), 0);
            (void)dsd_socket_close(client);
        }
    }
    (void)dsd_socket_close(s->sock);
    DSD_THREAD_RETURN;
}

static int
capture_start(capture_server* s) {
    DSD_MEMSET(s, 0, sizeof *s);
    if (dsd_socket_init() != 0) {
        return 1;
    }
    s->sock = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (s->sock == DSD_INVALID_SOCKET) {
        return 1;
    }
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = (socklen_t)sizeof addr;
    if (dsd_socket_bind(s->sock, (const struct sockaddr*)&addr, (int)sizeof addr) != 0
        || dsd_socket_listen(s->sock, 1) != 0 || getsockname(s->sock, (struct sockaddr*)&addr, &len) != 0
        || dsd_thread_create(&s->thread, capture_thread, s) != 0) {
        (void)dsd_socket_close(s->sock);
        return 1;
    }
    DSD_SNPRINTF(s->url, sizeof s->url, "http://127.0.0.1:%u", (unsigned int)ntohs(addr.sin_port));
    return 0;
}

/* Whether the captured upload holds @p text (its multipart `meta` and `system` parts are plain text). */
static int
capture_has(const capture_server* s, const char* text) {
    return s->got && find_bytes(s->request, s->used, text) != NULL;
}
#endif

/* Rdio export on, DirWatch unless @p mode says otherwise, with --rdio-unknown-talkgroup 4242. */
static void
rdio_fallback_on(rig* r, int mode) {
    r->opts->rdio_mode = mode;
    r->opts->rdio_system_id = 48;
    r->opts->rdio_unknown_talkgroup = 4242;
    r->opts->rdio_upload_timeout_ms = 1000;
    r->opts->rdio_upload_retries = 1;
    DSD_SNPRINTF(r->opts->rdio_api_url, sizeof r->opts->rdio_api_url, "%s", "http://127.0.0.1:1");
    DSD_SNPRINTF(r->opts->rdio_api_key, sizeof r->opts->rdio_api_key, "%s", "test-key");
}

static void
rdio_fallback_off(rig* r) {
    r->opts->rdio_mode = DSD_RDIO_MODE_OFF;
    r->opts->rdio_unknown_talkgroup = 0;
}

/* A call on @p slot of @p kind, @p tg and @p src (0: none), heard on @p freq_hz (0: unknown). */
static void
begin_kind_call_on(rig* r, uint8_t slot, int protocol, dsd_call_kind kind, uint32_t tg, uint32_t src, int64_t freq_hz) {
    const dsd_call_observation call = {.protocol = protocol,
                                       .slot = slot,
                                       .kind = kind,
                                       .ota_target_id = tg,
                                       .policy_target_id = tg,
                                       .ota_source_id = src,
                                       .frequency_hz = freq_hz};
    (void)dsd_call_state_observe(r->state, &call, DSD_CALL_BOUNDARY_BEGIN);
    dsd_event_sync_slot(r->opts, r->state, slot);
}

/* The publish record of journal id @p id, or NULL. */
static const char*
publish_of(const char* id) {
    const char* line = NULL;
    for (int i = 0; (line = record(&g_j, "publish", i)) != NULL; i++) {
        if (strcmp(field_s(line, "id"), id) == 0) {
            return line;
        }
    }
    return NULL;
}

/* Recording @p id is published, and exported under talkgroup @p talkgroup (0: kept local, no sidecar). */
static int
expect_export(const char* what, const rig* r, const char* id, unsigned talkgroup) {
    char tag[200];
    const char* pub = publish_of(id);
    DSD_SNPRINTF(tag, sizeof tag, "%s: published", what);
    int rc = expect_true(tag, pub != NULL);
    char body[4096];
    read_published_sidecar(r, pub, body, sizeof body);
    DSD_SNPRINTF(tag, sizeof tag, "%s: %s", what, talkgroup ? "a sidecar under the fallback number" : "no sidecar");
    if (talkgroup == 0U) {
        rc |= expect_str(tag, body, "");
    } else {
        char want[64];
        DSD_SNPRINTF(want, sizeof want, "\"talkgroup\": %u,", talkgroup);
        rc |= expect_true(tag, strstr(body, want) != NULL);
    }
    return rc;
}

/* A WAV stop mid-call: a call that names its source (an NXDN unaddressed group call) is exported under its frequency
   in kHz, or the option's number with none, as is a call of a protocol whose voice never carries identity (X2-TDMA,
   standalone ProVoice: the event layer always keeps its row); a call heard with no identity at all on any other
   protocol is not (its keep verdict is not known at the stop). */
static int
test_d7_wav_stop_mid_call(void) {
    static const struct {
        const char* label;
        int protocol;
        uint32_t src;
        int64_t freq_hz;
        uint32_t channel; /* an EDACS grant's LCN, 0 for none */
        unsigned want;
        const char* tag; /* the sidecar's label, when checked */
        int standard;    /* EDACS standard addressing (ea_mode 0), whose rows carry placeholder strings */
    } cases[] = {
        {"source-only NXDN call heard on 452.3125 MHz", DSD_SYNC_NXDN_POS, 1234U, 452312500, 0U, 452312U, NULL, 0},
        {"source-only NXDN call on an audio input", DSD_SYNC_NXDN_POS, 1234U, 0, 0U, 4242U, NULL, 0},
        /* Anonymous-voice protocols, whose identity-less rows the event layer always keeps. */
        {"X2-TDMA call heard on 452.3125 MHz", DSD_SYNC_X2TDMA_VOICE_POS, 0U, 452312500, 0U, 452312U, NULL, 0},
        /* The event layer's standalone ProVoice row carries no EDACS grant's LCN, so its label keeps "ProVoice". */
        {"standalone ProVoice call heard on 452.3125 MHz", DSD_SYNC_PROVOICE_POS, 0U, 452312500, 0U, 452312U,
         "\"talkgroup_tag\": \"ProVoice 452.3125 MHz\",", 0},
        /* A ProVoice call an EDACS grant named on LCN 4 (an extended-addressing digital all-call) whose row kept no ID:
           not standalone ProVoice, so an early close does not vouch for it. */
        {"identity-less EDACS ProVoice call on LCN 4", DSD_SYNC_PROVOICE_POS, 0U, 452412500, 4U, 0U, NULL, 0},
        /* Standard addressing renders "LID_UNK" and "000_AFS_00_000" for parties it does not know: a digital all-call
           whose LID was unavailable (0x800, no source) names no call, and an EDACS row with no IDs neither. A source
           does. */
        {"standard-address EDACS ProVoice all-call on LCN 4", DSD_SYNC_PROVOICE_POS, 0U, 452412500, 4U, 0U, NULL, 1},
        {"standard-address EDACS call with no IDs", DSD_SYNC_EDACS_POS, 0U, 452412500, 4U, 0U, NULL, 1},
        {"standard-address EDACS ProVoice call from LID 1234", DSD_SYNC_PROVOICE_POS, 1234U, 452412500, 4U, 452412U,
         NULL, 1},
        {"identity-less DMR call", DSD_SYNC_DMR_BS_VOICE_POS, 0U, 452312500, 0U, 0U, NULL, 0},
        {"identity-less NXDN call", DSD_SYNC_NXDN_POS, 0U, 0, 0U, 0U, NULL, 0},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_d7_stop");
        rdio_fallback_on(&r, DSD_RDIO_MODE_DIRWATCH);
        if (cases[i].standard) {
            r.state->ea_mode = 0;
        }
        submit(&r, DSD_APP_CMD_WAV_START);
        const dsd_call_observation call = {.protocol = cases[i].protocol,
                                           .kind = DSD_CALL_KIND_GROUP_VOICE,
                                           .ota_source_id = cases[i].src,
                                           .frequency_hz = cases[i].freq_hz,
                                           .channel = cases[i].channel};
        (void)dsd_call_state_observe(r.state, &call, DSD_CALL_BOUNDARY_BEGIN);
        dsd_event_sync_slot(r.opts, r.state, 0);
        rc |= expect_int(cases[i].label, (long long)r.state->event_history_s[0].Event_History_Items[0].channel,
                         (long long)cases[i].channel);
        voice_frames(&r, 0, 2);
        char id[64];
        slot_id(&r, 0, id, sizeof id);
        submit(&r, DSD_APP_CMD_WAV_STOP);
        reload(&r);
        rc |= expect_export(cases[i].label, &r, id, cases[i].want);
        if (cases[i].tag != NULL) {
            char body[4096];
            read_published_sidecar(&r, publish_of(id), body, sizeof body);
            rc |= expect_true(cases[i].label, strstr(body, cases[i].tag) != NULL);
        }
        rdio_fallback_off(&r);
        rig_close(&r);
    }
    return rc;
}

/* A faded identity-less DMR call, its row held for the reacquisition window, owns the slot file still: a WAV stop and
   the orphan flush each close it unexported, though the option would number it. */
static int
test_d7_held_fade_stays_local(void) {
    int rc = 0;
    for (int flush = 0; flush <= 1; flush++) {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_d7_held");
        rdio_fallback_on(&r, DSD_RDIO_MODE_DIRWATCH);
        submit(&r, DSD_APP_CMD_WAV_START);
        begin_kind_call_on(&r, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 0U, 0U, 0);
        voice_frames(&r, 0, 3);
        char id[64];
        slot_id(&r, 0, id, sizeof id);
        end_call(&r, DSD_CALL_END_SYNC_LOSS);
        reload(&r);
        rc |= expect_int("the faded call's row is held", count_records(&g_j, "publish"), 0);
        if (flush) {
            dsd_event_flush_orphan_recordings(r.opts, r.state);
        } else {
            submit(&r, DSD_APP_CMD_WAV_STOP);
        }
        reload(&r);
        rc |= expect_export(flush ? "held fade, orphan flush" : "held fade, WAV stop", &r, id, 0U);
        rdio_fallback_off(&r);
        rig_close(&r);
    }
    return rc;
}

/* The engine's shutdown order: the slots end as sync loss and commit, then the leftover files close early. A
   source-only NXDN call commits with its recording, exported under the fallback; an identity-less one is held, and the
   leftover close does not export it. */
static int
test_d7_shutdown_commit(void) {
    int rc = 0;
    for (int named = 0; named <= 1; named++) {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_d7_shutdown");
        rdio_fallback_on(&r, DSD_RDIO_MODE_DIRWATCH);
        submit(&r, DSD_APP_CMD_WAV_START);
        begin_kind_call_on(&r, 0U, DSD_SYNC_NXDN_POS, DSD_CALL_KIND_GROUP_VOICE, 0U, named ? 1234U : 0U, 0);
        voice_frames(&r, 0, 3);
        char id[64];
        slot_id(&r, 0, id, sizeof id);
        end_call(&r, DSD_CALL_END_SYNC_LOSS);
        dsd_event_sync_slot(r.opts, r.state, 0);
        r.opts->wav_out_f = close_and_publish_call_wav(r.opts->wav_out_f, r.opts, r.state, 0, r.opts->wav_out_file,
                                                       r.opts->wav_out_dir, r.opts->wav_out_open_time);
        reload(&r);
        rc |= expect_export(named ? "shutdown, source-only NXDN call" : "shutdown, identity-less NXDN call", &r, id,
                            named ? 4242U : 0U);
        if (named) {
            const char* pub = publish_of(id);
            rc |= expect_true("the commit published it under its row", pub && strcmp(field_s(pub, "row"), "null") != 0);
        }
        rdio_fallback_off(&r);
        rig_close(&r);
    }
    return rc;
}

/* A cleared history leaves no row for a WAV stop to name the call by: the call's recording stays local. */
static int
test_d7_cleared_history_stays_local(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_d7_cleared");
    rdio_fallback_on(&r, DSD_RDIO_MODE_DIRWATCH);
    submit(&r, DSD_APP_CMD_WAV_START);
    begin_kind_call_on(&r, 0U, DSD_SYNC_NXDN_POS, DSD_CALL_KIND_GROUP_VOICE, 0U, 1234U, 0);
    voice_frames(&r, 0, 2);
    char id[64];
    slot_id(&r, 0, id, sizeof id);
    dsd_event_history_reset(r.state);
    submit(&r, DSD_APP_CMD_WAV_STOP);
    reload(&r);
    rc |= expect_export("clear history, then WAV stop", &r, id, 0U);
    rdio_fallback_off(&r);
    rig_close(&r);
    return rc;
}

/* A trunk-scan hop restores the next target's empty row: audio heard there with no call, closed by a WAV stop, stays
   local. */
static int
test_d7_restored_scan_row_stays_local(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_d7_scan");
    rc |= scan_rig_start(&r);
    rdio_fallback_on(&r, DSD_RDIO_MODE_DIRWATCH);
    submit(&r, DSD_APP_CMD_WAV_START);
    voice_frames(&r, 0, 2);
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(r.opts, r.state);
    rc |= expect_int("the dwell hopped to b", (long long)dsd_engine_trunk_scan_active_index(r.state), 1);
    voice_frames(&r, 0, 2);
    char id[64];
    slot_id(&r, 0, id, sizeof id);
    submit(&r, DSD_APP_CMD_WAV_STOP);
    reload(&r);
    rc |= expect_export("restored empty scan row, WAV stop", &r, id, 0U);
    rdio_fallback_off(&r);
    scan_rig_stop(&r);
    rig_close(&r);
    return rc;
}

/* The EDACS rotation closes the slot file early (close_and_publish_call_wav(), as edacs_prepare_voice_wav_output()
   does at a grant): a system all-call that names its caller is exported under the fallback, and an identity-less row's
   file, audio already in it before the row, is not. */
static int
test_d7_edacs_rotation(void) {
    int rc = 0;
    for (int named = 0; named <= 1; named++) {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_d7_edacs");
        rdio_fallback_on(&r, DSD_RDIO_MODE_DIRWATCH);
        submit(&r, DSD_APP_CMD_WAV_START);
        static short analog[3][960];
        for (int i = 0; i < 960; i++) {
            analog[0][i] = (short)(i * 8);
            analog[1][i] = (short)(-i * 8);
            analog[2][i] = (short)(i * 4);
        }
        if (!named) {
            edacs_write_analog_wav(r.opts, r.state, analog[0], analog[1], analog[2]);
        }
        begin_kind_call_on(&r, 0U, DSD_SYNC_EDACS_POS, DSD_CALL_KIND_GROUP_VOICE, 0U, named ? 1234U : 0U, 0);
        edacs_write_analog_wav(r.opts, r.state, analog[0], analog[1], analog[2]);
        char id[64];
        slot_id(&r, 0, id, sizeof id);
        r.opts->wav_out_f = close_and_publish_call_wav(r.opts->wav_out_f, r.opts, r.state, 0, r.opts->wav_out_file,
                                                       r.opts->wav_out_dir, r.opts->wav_out_open_time);
        reload(&r);
        rc |= expect_export(named ? "EDACS all-call naming its caller" : "EDACS identity-less, prefilled", &r, id,
                            named ? 4242U : 0U);
        rdio_fallback_off(&r);
        rig_close(&r);
    }
    return rc;
}

/* Both slots at a WAV stop, each judged on its own row: the slot whose call names its source is exported, the other
   not, whichever slot it is. */
static int
test_d7_both_slots(void) {
    int rc = 0;
    for (int named_slot = 0; named_slot <= 1; named_slot++) {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_d7_slots");
        rdio_fallback_on(&r, DSD_RDIO_MODE_DIRWATCH);
        submit(&r, DSD_APP_CMD_WAV_START);
        for (uint8_t slot = 0; slot < 2U; slot++) {
            begin_kind_call_on(&r, slot, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 0U,
                               slot == named_slot ? 4321U : 0U, 0);
            voice_frames(&r, slot, 2);
        }
        char id[2][64];
        slot_id(&r, 0, id[0], sizeof id[0]);
        slot_id(&r, 1, id[1], sizeof id[1]);
        submit(&r, DSD_APP_CMD_WAV_STOP);
        reload(&r);
        rc |= expect_export(named_slot == 0 ? "slot 1 named" : "slot 1 identity-less", &r, id[0],
                            named_slot == 0 ? 4242U : 0U);
        rc |= expect_export(named_slot == 1 ? "slot 2 named" : "slot 2 identity-less", &r, id[1],
                            named_slot == 1 ? 4242U : 0U);
        rdio_fallback_off(&r);
        rig_close(&r);
    }
    return rc;
}

/* The journal's `rdio` says whether an API job was queued for a fallback call: off and DirWatch never, API and both
   when it was (the upload carrying the fallback number and the system), an API config the queue refuses never, which
   leaves the pair; without libcurl never, with the sidecar still written. An API-only upload removes its sidecar only
   when it succeeds, which needs the stand-in server (RDIO_CAPTURE). */
static int
test_d7_fallback_journal_rdio_flag(void) {
    static const struct {
        const char* label;
        int mode;
        int no_key;
        int sidecar; /* the public pair keeps its sidecar after the upload ends */
        int queued;  /* with libcurl */
    } cases[] = {
        {"off", DSD_RDIO_MODE_OFF, 0, 0, 0},
        {"dirwatch", DSD_RDIO_MODE_DIRWATCH, 0, 1, 0},
        {"api", DSD_RDIO_MODE_API, 0, 0, 1},
        {"both", DSD_RDIO_MODE_BOTH, 0, 1, 1},
        {"api with no key", DSD_RDIO_MODE_API, 1, 1, 0},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_d7_flag");
        rdio_fallback_on(&r, cases[i].mode);
        if (cases[i].no_key) {
            r.opts->rdio_api_key[0] = '\0';
        }
#ifdef RDIO_CAPTURE
        capture_server server;
        int capturing = 0;
        if (cases[i].queued) {
            capturing = capture_start(&server) == 0;
            rc |= expect_true("capture server", capturing);
            if (capturing) {
                DSD_SNPRINTF(r.opts->rdio_api_url, sizeof r.opts->rdio_api_url, "%s", server.url);
            }
        }
#endif
        submit(&r, DSD_APP_CMD_WAV_START);
        begin_kind_call_on(&r, 0U, DSD_SYNC_NXDN_POS, DSD_CALL_KIND_GROUP_VOICE, 0U, 1234U, 0);
        voice_frames(&r, 0, 2);
        char id[64];
        slot_id(&r, 0, id, sizeof id);
        submit(&r, DSD_APP_CMD_WAV_STOP);
        dsd_rdio_upload_shutdown();
        reload(&r);
        const char* pub = publish_of(id);
        char tag[160];
#ifdef USE_CURL
        const int queued = cases[i].queued;
        /* The sidecar goes only with a successful upload: without the stand-in server (native Windows) a queued
           upload fails against the rig's dead endpoint and keeps it. */
#ifdef RDIO_CAPTURE
        const int uploaded = queued && capturing;
#else
        const int uploaded = 0;
#endif
        const int sidecar = cases[i].sidecar || (queued && !uploaded);
#else
        const int queued = 0;
        const int sidecar = cases[i].mode != DSD_RDIO_MODE_OFF;
#endif
        DSD_SNPRINTF(tag, sizeof tag, "%s: the journal's rdio flag", cases[i].label);
        rc |= expect_str(tag, pub ? field_s(pub, "rdio") : "", queued ? "true" : "false");
        DSD_SNPRINTF(tag, sizeof tag, "%s", cases[i].label);
        rc |= expect_export(tag, &r, id, sidecar ? 4242U : 0U);
#ifdef RDIO_CAPTURE
        if (capturing) {
            (void)dsd_thread_join(server.thread);
            DSD_SNPRINTF(tag, sizeof tag, "%s: the upload carries the fallback number and the system", cases[i].label);
            rc |=
                expect_true(tag, capture_has(&server, "\"talkgroup\": 4242,") && capture_has(&server, "name=\"system\"")
                                     && capture_has(&server, "\r\n\r\n48\r\n"));
        }
#endif
        rdio_fallback_off(&r);
        rig_close(&r);
    }
    return rc;
}

/* --- Issue #931: one rdio-scanner system per frequency, two talkgroup 1 calls in one session. --- */

enum { BY_FREQ_CALLS = 2 };

/* The two conventional DMR carriers the session hops between, as a -Y scan does. */
static const uint32_t k_by_freq_hz[BY_FREQ_CALLS] = {154437500U, 456950000U};

/* What one call of a session left: its published file and journal system, its sidecar, and whether its upload's form
   system part and meta part both named the system it was filed under (-1: no stand-in server to ask). */
typedef struct {
    char file[LINE_CAP];
    char system[LINE_CAP];
    char sidecar[4096];
    int upload_system;
} by_freq_call;

#ifdef RDIO_CAPTURE
static capture_server g_by_freq_servers[BY_FREQ_CALLS];
#endif

/* The system call @p c of a session is filed under: its carrier's kHz with the option on, else the base 48. */
static unsigned
by_freq_want_system(int by_frequency, int c) {
    return by_frequency ? k_by_freq_hz[c] / 1000U : 48U;
}

/* The raw value of @p key in the sidecar @p body, up to the comma or line end that ends it ("" when absent). */
static void
sidecar_value(const char* body, const char* key, char* out, size_t cap) {
    char pat[64];
    DSD_SNPRINTF(pat, sizeof pat, "\"%s\": ", key);
    out[0] = '\0';
    const char* p = strstr(body, pat);
    if (!p) {
        return;
    }
    p += strlen(pat);
    const size_t len = strcspn(p, ",\n");
    DSD_SNPRINTF(out, cap, "%.*s", (int)len, p);
}

/* A per-call file name, <date>_<time>_<n>_<call>.wav, without its third field, a number drawn at random for each file:
   what two runs of one session name alike. */
static void
name_without_draw(const char* name, char* out, size_t cap) {
    const char* first = strchr(name, '_');
    const char* second = first ? strchr(first + 1, '_') : NULL;
    const char* third = second ? strchr(second + 1, '_') : NULL;
    if (!third) {
        DSD_SNPRINTF(out, cap, "%s", name);
        return;
    }
    DSD_SNPRINTF(out, cap, "%.*s%s", (int)(second - name + 1), name, third + 1);
}

#ifdef RDIO_CAPTURE
/* Whether the upload @p s kept names @p system in its form system part and in its meta part. */
static int
capture_names_system(const capture_server* s, unsigned system) {
    char form[64];
    char meta[64];
    DSD_SNPRINTF(form, sizeof form, "name=\"system\"\r\n\r\n%u\r\n", system);
    DSD_SNPRINTF(meta, sizeof meta, "\"system\": %u,", system);
    return capture_has(s, form) && capture_has(s, meta);
}
#endif

/* One decoder session, one rig with no re-init between the calls, exporting to DirWatch and the API (both): a numbered
   DMR group call to talkgroup 1 on each carrier of k_by_freq_hz, with --rdio-system-by-frequency as @p by_frequency
   says. The scan hop to the second carrier writes its tune while the first call is still running, and the first call
   is rendered again on it before it ends, as a -Y commit does (channel_scan.c: the queued tune writes
   rtlsdr_center_freq before the outgoing calls are finalized). Each call's results go to @p out. */
static int
by_freq_session(int by_frequency, by_freq_call* out) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_by_freq");
    r.opts->audio_in_type = AUDIO_IN_RTL;
    r.opts->rtlsdr_center_freq = k_by_freq_hz[0];
    r.opts->rdio_mode = DSD_RDIO_MODE_BOTH;
    r.opts->rdio_system_id = 48;
    r.opts->rdio_system_by_frequency = by_frequency;
    r.opts->rdio_upload_timeout_ms = 1000;
    r.opts->rdio_upload_retries = 1;
    DSD_SNPRINTF(r.opts->rdio_api_url, sizeof r.opts->rdio_api_url, "%s", "http://127.0.0.1:1");
    DSD_SNPRINTF(r.opts->rdio_api_key, sizeof r.opts->rdio_api_key, "%s", "test-key");
    int capturing[BY_FREQ_CALLS] = {0, 0};
    char ids[BY_FREQ_CALLS][64];
    submit(&r, DSD_APP_CMD_WAV_START);
    for (int c = 0; c < BY_FREQ_CALLS; c++) {
#ifdef RDIO_CAPTURE
        /* One stand-in server a call: the job copies the URL when it is queued, at the call's close. */
        capturing[c] = capture_start(&g_by_freq_servers[c]) == 0;
        rc |= expect_true("capture server", capturing[c]);
        if (capturing[c]) {
            DSD_SNPRINTF(r.opts->rdio_api_url, sizeof r.opts->rdio_api_url, "%s", g_by_freq_servers[c].url);
        }
#endif
        clock_at(k_t0 + 100 + (uint64_t)c * 10);
        begin_kind_call_on(&r, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 1U, 100U + (uint32_t)c, 0);
        voice_frames(&r, 0, 3);
        slot_id(&r, 0, ids[c], sizeof ids[c]);
        if (c == 0) {
            r.opts->rtlsdr_center_freq = k_by_freq_hz[1];
            clock_at(k_t0 + 103);
            dsd_event_sync_slot(r.opts, r.state, 0U);
            voice_frames(&r, 0, 1);
        }
        clock_at(k_t0 + 105 + (uint64_t)c * 10);
        end_call(&r, DSD_CALL_END_EXPLICIT);
    }
    dsd_rdio_upload_shutdown();
    reload(&r);
    for (int c = 0; c < BY_FREQ_CALLS; c++) {
        const char* pub = publish_of(ids[c]);
        rc |= expect_true("each call is published", pub != NULL);
        DSD_SNPRINTF(out[c].file, sizeof out[c].file, "%s", pub ? field_s(pub, "file") : "");
        DSD_SNPRINTF(out[c].system, sizeof out[c].system, "%s", pub ? field_s(pub, "system") : "");
        read_published_sidecar(&r, pub, out[c].sidecar, sizeof out[c].sidecar);
        out[c].upload_system = -1;
#ifdef RDIO_CAPTURE
        if (capturing[c]) {
            (void)dsd_thread_join(g_by_freq_servers[c].thread);
            out[c].upload_system =
                capture_names_system(&g_by_freq_servers[c], by_freq_want_system(by_frequency, c)) ? 1 : 0;
        }
#endif
    }
    (void)capturing;
    rig_close(&r);
    return rc;
}

/* Issue #931: a user scanning conventional DMR channels hears talkgroup 1 on 154.4375 and on 456.9500 MHz. With
   --rdio-system-by-frequency the two calls of one session go to systems 154437 and 456950, named "System <kHz>", each
   upload's form system part and meta part naming its sidecar's system, the first call still filed under its own
   carrier although the hop's tune was written before it closed. The label, the file name and the journal's system are
   the ones the same session writes with the option off, where both calls go to --rdio-system-id 48. */
static int
test_rdio_system_by_frequency_in_one_session(void) {
    static by_freq_call on[BY_FREQ_CALLS];
    static by_freq_call off[BY_FREQ_CALLS];
    int rc = by_freq_session(1, on);
    rc |= by_freq_session(0, off);
    for (int c = 0; c < BY_FREQ_CALLS; c++) {
        char tag[200];
        char want[96];
        const unsigned khz = by_freq_want_system(1, c);
        DSD_SNPRINTF(tag, sizeof tag, "call %d, option on: the sidecar", c + 1);
        DSD_SNPRINTF(want, sizeof want, "\"system\": %u,", khz);
        rc |= expect_true(tag, strstr(on[c].sidecar, want) != NULL);
        DSD_SNPRINTF(want, sizeof want, "\"short_name\": \"System %u\",", khz);
        rc |= expect_true(tag, strstr(on[c].sidecar, want) != NULL);
        DSD_SNPRINTF(want, sizeof want, "\"freq\": %u,", k_by_freq_hz[c]);
        rc |= expect_true(tag, strstr(on[c].sidecar, want) != NULL);
        rc |= expect_true(tag, strstr(on[c].sidecar, "\"talkgroup\": 1,") != NULL);
        DSD_SNPRINTF(tag, sizeof tag, "call %d, option off: the sidecar", c + 1);
        DSD_SNPRINTF(want, sizeof want, "\"system\": %u,", by_freq_want_system(0, c));
        rc |= expect_true(tag, strstr(off[c].sidecar, want) != NULL);
        rc |= expect_true(tag, strstr(off[c].sidecar, "\"talkgroup\": 1,") != NULL);
        DSD_SNPRINTF(want, sizeof want, "\"short_name\": \"%s\",", off[c].system);
        DSD_SNPRINTF(tag, sizeof tag, "call %d, option off: short_name is the journal's system", c + 1);
        rc |= expect_true(tag, off[c].system[0] != '\0' && strstr(off[c].sidecar, want) != NULL);

        char on_tag[160];
        char off_tag[160];
        sidecar_value(on[c].sidecar, "talkgroup_tag", on_tag, sizeof on_tag);
        sidecar_value(off[c].sidecar, "talkgroup_tag", off_tag, sizeof off_tag);
        DSD_SNPRINTF(tag, sizeof tag, "call %d: the option leaves the label", c + 1);
        rc |= expect_true(tag, on_tag[0] != '\0');
        rc |= expect_str(tag, on_tag, off_tag);
        char on_name[LINE_CAP];
        char off_name[LINE_CAP];
        name_without_draw(on[c].file, on_name, sizeof on_name);
        name_without_draw(off[c].file, off_name, sizeof off_name);
        DSD_SNPRINTF(tag, sizeof tag, "call %d: the option leaves the file name", c + 1);
        rc |= expect_true(tag, strstr(on_name, "_TGT_1_") != NULL);
        rc |= expect_str(tag, on_name, off_name);
        DSD_SNPRINTF(tag, sizeof tag, "call %d: the option leaves the journal's system", c + 1);
        rc |= expect_str(tag, on[c].system, off[c].system);
#ifdef RDIO_CAPTURE
        DSD_SNPRINTF(tag, sizeof tag, "call %d, option on: the upload names the sidecar's system", c + 1);
        rc |= expect_int(tag, on[c].upload_system, 1);
        DSD_SNPRINTF(tag, sizeof tag, "call %d, option off: the upload names system 48", c + 1);
        rc |= expect_int(tag, off[c].upload_system, 1);
#endif
    }
    return rc;
}

/* The stream verdict holds over the fallback: a PRIVATE call from a radio set stream=off, and a source-only call whose
   radio is turned stream=off just before the WAV stop, stay local; with the radio's stream on the call is exported. */
static int
test_d7_stream_off_stays_local(void) {
    static const struct {
        const char* label;
        dsd_call_kind kind;
        int before; /* the radio row is installed before the call begins */
        int stream;
        unsigned want;
    } cases[] = {
        {"source-blocked PRIVATE call", DSD_CALL_KIND_PRIVATE_VOICE, 1, 0, 0U},
        {"radio turned stream=off before the stop", DSD_CALL_KIND_GROUP_VOICE, 0, 0, 0U},
        {"radio left stream=on", DSD_CALL_KIND_GROUP_VOICE, 0, 1, 4242U},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_d7_stream");
        rdio_fallback_on(&r, DSD_RDIO_MODE_DIRWATCH);
        submit(&r, DSD_APP_CMD_WAV_START);
        if (cases[i].before) {
            rc |= expect_int("radio row", dsd_test_install_radio_policy(r.state, 42U, "A", 1, 1, cases[i].stream), 0);
        }
        begin_kind_call_on(&r, 0U, DSD_SYNC_NXDN_POS, cases[i].kind, 0U, 42U, 0);
        voice_frames(&r, 0, 2);
        if (!cases[i].before) {
            rc |= expect_int("radio row", dsd_test_install_radio_policy(r.state, 42U, "A", 1, 1, cases[i].stream), 0);
        }
        char id[64];
        slot_id(&r, 0, id, sizeof id);
        submit(&r, DSD_APP_CMD_WAV_STOP);
        reload(&r);
        rc |= expect_export(cases[i].label, &r, id, cases[i].want);
        rdio_fallback_off(&r);
        rig_close(&r);
    }
    return rc;
}

/* --- Issue #888 B18: an EDACS grant closes the slot file's audio before it observes the incoming call. --- */

/* A grant on the frequency the receiver is on (the stand-in tuner flushes nothing, as a tune that moves nothing):
   LCN 4, analog voice read from an input EDACS takes none from, so the call's loop ends at once and the test writes its
   audio itself. */
static void
edacs_grant_rig(rig* r) {
    r->opts->rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    r->opts->rdio_system_id = 48;
    r->opts->trunk_enable = 1;
    r->opts->trunk_tune_group_calls = 1;
    r->opts->trunk_tune_private_calls = 1;
    r->opts->use_rigctl = 1;
    r->opts->audio_in_type = AUDIO_IN_NULL;
    r->state->ea_mode = 0;
    r->state->edacs_cc_lcn = 1;
    r->state->edacs_tuned_lcn = -1;
    r->state->trunk_lcn_freq[0] = 452312500L;
    r->state->trunk_lcn_freq[3] = 452412500L;
    r->state->trunk_cc_freq = r->state->trunk_lcn_freq[0];
    r->state->synctype = DSD_SYNC_EDACS_POS;
    g_tune_refused = 0;
    dsd_trunk_tuning_requests_reset();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = tune, .tune_to_cc_request = tune, .return_to_cc_request = return_to_cc});
}

static void
edacs_grant_rig_close(rig* r) {
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_trunk_tuning_requests_reset();
    r->opts->rdio_mode = DSD_RDIO_MODE_OFF;
    rig_close(r);
}

typedef enum {
    EDACS_GRANT_GROUP = 0, /* a standard analog group call: TG 321, LID 1234 */
    EDACS_GRANT_ALL_CALL,  /* a standard analog all-call: no target, LID 1010 */
    EDACS_GRANT_DIGITAL,   /* a standard digital (ProVoice) individual call: target 4321, source 2345 */
} edacs_grant_kind;

/* The grant's message pair, on LCN 4, as the control channel sends it. */
static void
edacs_grant_send(rig* r, edacs_grant_kind kind) {
    unsigned long long msg1 = 0ULL;
    unsigned long long msg2 = 0ULL;
    if (kind == EDACS_GRANT_GROUP) {
        msg1 = ((1234ULL & 0x3F80ULL) << 11U) | (4ULL << 12U) | (1ULL << 11U) | 321ULL;
        msg2 = (1234ULL & 0x7FULL) << 17U;
    } else if (kind == EDACS_GRANT_ALL_CALL) {
        msg1 = (7ULL << 25U) | (7ULL << 22U) | (0x0FULL << 17U) | (4ULL << 12U) | (1ULL << 9U) | (1010ULL & 0x7FULL);
        msg2 = ((1010ULL >> 7U) & 0x7FULL) << 1U;
    } else {
        msg1 = (7ULL << 25U) | (5ULL << 22U) | (1ULL << 21U) | (4ULL << 15U) | (1ULL << 14U) | 4321ULL;
        msg2 = 2345ULL;
    }
    edacs_process_valid_frame(r->opts, r->state, msg1, msg2);
}

/* What names the incoming call's recording: a part only its row puts in a file name. */
static const char*
edacs_grant_name_part(edacs_grant_kind kind) {
    if (kind == EDACS_GRANT_GROUP) {
        return "_TGT_321_";
    }
    return kind == EDACS_GRANT_ALL_CALL ? "_SRC_LID_1010" : "_TGT_4321_";
}

/* A published recording in the rig's folder, read back. */
typedef struct {
    const char* dir;
    const char* part; /* the incoming call's name part */
    int under_incoming;
    sf_count_t incoming_frames; /* of the one named for the incoming call */
    int prefill;                /* published recordings holding the prefill: 8 kHz, @c prefill_frames long */
    int prefill_named;          /* of those, named for the incoming call */
    int prefill_sidecar;        /* of those, with an rdio-scanner sidecar */
    sf_count_t prefill_frames;
} edacs_grant_files;

static int
edacs_grant_files_cb(const char* name, void* user) {
    edacs_grant_files* files = (edacs_grant_files*)user;
    const size_t len = strlen(name);
    if (len < 4 || strcmp(name + len - 4, ".wav") != 0 || strncmp(name, "TEMP_", 5) == 0) {
        return 0;
    }
    char path[DSD_TEST_PATH_MAX * 2];
    if (dsd_test_path_join(path, sizeof path, files->dir, name) != 0) {
        return 0;
    }
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof info);
    SNDFILE* f = sf_open(path, SFM_READ, &info);
    if (f != NULL) {
        (void)sf_close(f);
    }
    const int named = strstr(name, files->part) != NULL;
    if (named) {
        files->under_incoming++;
        files->incoming_frames = info.frames;
    }
    if (info.samplerate == 8000 && info.frames == files->prefill_frames) {
        files->prefill++;
        files->prefill_named += named;
        char sidecar[DSD_TEST_PATH_MAX * 2];
        DSD_SNPRINTF(sidecar, sizeof sidecar, "%.*s.json", (int)(strlen(path) - 4), path);
        FILE* s = dsd_fopen_existing_regular_file(sidecar, "rb");
        if (s != NULL) {
            files->prefill_sidecar++;
            (void)fclose(s);
        }
    }
    return 0;
}

static edacs_grant_files
edacs_grant_list(const rig* r, edacs_grant_kind kind, sf_count_t prefill_frames) {
    edacs_grant_files files;
    DSD_MEMSET(&files, 0, sizeof files);
    files.dir = r->dir;
    files.part = edacs_grant_name_part(kind);
    files.prefill_frames = prefill_frames;
    (void)dsd_dir_list(r->dir, edacs_grant_files_cb, &files);
    return files;
}

/* The incoming call's own audio, written after its grant: one analog triplet into the 48 kHz file the grant opened, or
   three 20 ms ProVoice frames into the slot file. */
static void
edacs_grant_incoming_audio(rig* r, edacs_grant_kind kind) {
    if (kind == EDACS_GRANT_DIGITAL) {
        voice_frames(r, 0, 3);
        return;
    }
    static short analog[3][960];
    for (int i = 0; i < 960; i++) {
        analog[0][i] = (short)(i * 8);
        analog[1][i] = (short)(-i * 8);
        analog[2][i] = (short)(i * 4);
    }
    edacs_write_analog_wav(r->opts, r->state, analog[0], analog[1], analog[2]);
}

/* The sidecar of the EDACS all-call recording @p pub (LCN 4, from LID @p lid): exported under LCN 4's frequency in kHz,
   its generated label and its caller, as a call with no talkgroup is (issue #888). */
static int
expect_edacs_all_call_sidecar(const char* what, const rig* r, const char* pub, unsigned lid) {
    char body[4096];
    read_published_sidecar(r, pub, body, sizeof body);
    char src[64];
    DSD_SNPRINTF(src, sizeof src, "\"srcList\": [{\"pos\":0,\"src\":%u}],", lid);
    const char* const want[] = {"\"talkgroup\": 452412,", "\"talkgroup_tag\": \"EDACS 452.4125 MHz\",", src,
                                "\"freq\": 452412500,"};
    int rc = 0;
    char tag[200];
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        DSD_SNPRINTF(tag, sizeof tag, "%s: the sidecar holds %s", what, want[i]);
        rc |= expect_true(tag, strstr(body, want[i]) != NULL);
    }
    if (rc != 0) {
        DSD_FPRINTF(stderr, "  %s sidecar: %s\n", what, body);
    }
    return rc;
}

/* Audio in slot 1's file that no call owns (100 ms at 8 kHz), then a grant on the carrier the receiver is on: the grant
   closes that audio as no row's, unexported, before it observes the incoming call, which records only its own audio
   under its row. Without it the grant's row named the earlier audio, and the analog rotation (or the digital call's
   commit) published and exported it as the incoming call's. With the journal on and off. */
static int
test_edacs_grant_closes_unowned_audio_first(void) {
    static const char* const k_kind[] = {"edacs group grant", "edacs all-call grant", "edacs digital grant"};
    int rc = 0;
    for (int kind = EDACS_GRANT_GROUP; kind <= EDACS_GRANT_DIGITAL; kind++) {
        for (int with_journal = 1; with_journal >= 0; with_journal--) {
            char tag[160];
            rig r;
            rc |= rig_open_ex(&r, "dsdneo_journal_edacs_grant", with_journal);
            edacs_grant_rig(&r);
            submit(&r, DSD_APP_CMD_WAV_START);
            char prefill[64];
            slot_id(&r, 0, prefill, sizeof prefill);
            voice_frames(&r, 0, 5);
            clock_at(g_now_s + 1);
            edacs_grant_send(&r, (edacs_grant_kind)kind);
            DSD_SNPRINTF(tag, sizeof tag, "%s: the grant made its call", k_kind[kind]);
            const uint64_t epoch = call_epoch(&r);
            rc |= expect_true(tag, epoch != 0U);
            char incoming[64];
            slot_id(&r, 0, incoming, sizeof incoming);
            edacs_grant_incoming_audio(&r, (edacs_grant_kind)kind);
            clock_at(g_now_s + 2);
            end_call(&r, DSD_CALL_END_EXPLICIT);

            const int digital = kind == EDACS_GRANT_DIGITAL;
            if (with_journal) {
                reload(&r);
                DSD_SNPRINTF(tag, sizeof tag, "%s: the earlier audio", k_kind[kind]);
                const char* left = NULL;
                const char* own = NULL;
                const char* pub = NULL;
                for (int i = 0; (pub = record(&g_j, "publish", i)) != NULL; i++) {
                    if (strcmp(field_s(pub, "id"), prefill) == 0) {
                        left = pub;
                    } else if (strcmp(field_s(pub, "id"), incoming) == 0) {
                        own = pub;
                    }
                }
                rc |= expect_orphan(tag, left, 0, 100);
                DSD_SNPRINTF(tag, sizeof tag, "%s: the incoming call's recording", k_kind[kind]);
                rc |= expect_true(tag, own != NULL);
                DSD_SNPRINTF(tag, sizeof tag, "%s: holds only its own audio", k_kind[kind]);
                rc |= expect_int(tag, own ? field_ll(own, "duration_ms") : -9, 60);
                DSD_SNPRINTF(tag, sizeof tag, "%s: at its own rate", k_kind[kind]);
                rc |= expect_int(tag, own ? field_ll(own, "rate") : -9, digital ? 8000 : EDACS_ANALOG_RATE_HZ);
                DSD_SNPRINTF(tag, sizeof tag, "%s: under its row", k_kind[kind]);
                rc |= expect_int(tag, own ? field_ll(own, "row") : -9, (long long)r.state->event_history_s[0].push_seq);
                if (kind == EDACS_GRANT_ALL_CALL) {
                    /* The all-call names no talkgroup: exported under LCN 4's frequency in kHz, by its generated
                       label, from its caller. */
                    rc |= expect_edacs_all_call_sidecar(k_kind[kind], &r, own, 1010U);
                }
            }
            const edacs_grant_files files = edacs_grant_list(&r, (edacs_grant_kind)kind, 800);
            DSD_SNPRINTF(tag, sizeof tag, "%s: the earlier audio is published once", k_kind[kind]);
            rc |= expect_int(tag, files.prefill, 1);
            DSD_SNPRINTF(tag, sizeof tag, "%s: not under the incoming call's name", k_kind[kind]);
            rc |= expect_int(tag, files.prefill_named, 0);
            DSD_SNPRINTF(tag, sizeof tag, "%s: and not exported", k_kind[kind]);
            rc |= expect_int(tag, files.prefill_sidecar, 0);
            DSD_SNPRINTF(tag, sizeof tag, "%s: one recording names the incoming call", k_kind[kind]);
            rc |= expect_int(tag, files.under_incoming, 1);
            DSD_SNPRINTF(tag, sizeof tag, "%s: and holds its audio alone", k_kind[kind]);
            rc |= expect_int(tag, (long long)files.incoming_frames, digital ? 480 : EDACS_ANALOG_TRIPLET_SAMPLES);
            edacs_grant_rig_close(&r);
        }
    }
    return rc;
}

/* The outgoing call is still active at the next grant: its audio is its own, closed under its row by the commit the
   incoming call's observation brings, and exported with it, as before. */
static int
test_edacs_grant_leaves_owned_audio_to_its_row(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_edacs_grant_owned");
    edacs_grant_rig(&r);
    submit(&r, DSD_APP_CMD_WAV_START);
    char outgoing[64];
    slot_id(&r, 0, outgoing, sizeof outgoing);
    begin_call(&r, DSD_SYNC_EDACS_POS, 100U, 50U, DSD_CALL_BOUNDARY_BEGIN);
    voice_frames(&r, 0, 5);
    clock_at(g_now_s + 1);
    edacs_grant_send(&r, EDACS_GRANT_GROUP);
    reload(&r);
    const char* pub = record(&g_j, "publish", 0);
    rc |= expect_true("owned: the outgoing call's audio", pub && strcmp(field_s(pub, "id"), outgoing) == 0);
    rc |= expect_int("owned: under the outgoing row", pub ? field_ll(pub, "row") : -9,
                     (long long)r.state->event_history_s[0].push_seq);
    rc |= expect_true("owned: that row is the outgoing call's",
                      r.state->event_history_s[0].Event_History_Items[1].target_id == 100U);
    rc |= expect_str("owned: not pending", pub ? field_s(pub, "pending") : "", "false");
    const edacs_grant_files files = edacs_grant_list(&r, EDACS_GRANT_GROUP, 800);
    rc |= expect_int("owned: published once", files.prefill, 1);
    rc |= expect_int("owned: exported with its row", files.prefill_sidecar, 1);
    rc |= expect_int("owned: not under the incoming call's name", files.prefill_named, 0);
    edacs_grant_rig_close(&r);
    return rc;
}

/* An all-call from LID 1010, then a group grant to TG 321 from the same LID on the same LCN: the call state merges the
   grant into the active all-call, so no orphan audio is left to close, and the analog rotation closes the all-call's
   segment early from its staged row (no target, source LID 1010). That row names a call, so the segment, all-call
   audio, is exported under LCN 4's frequency in kHz from LID 1010, pending, and linked by the merged call's commit. */
static int
test_edacs_same_lid_group_after_all_call(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_edacs_same_lid");
    edacs_grant_rig(&r);
    submit(&r, DSD_APP_CMD_WAV_START);
    edacs_grant_send(&r, EDACS_GRANT_ALL_CALL);
    const uint64_t epoch = call_epoch(&r);
    rc |= expect_true("same LID: the all-call made its call", epoch != 0U);
    char all_call[64];
    slot_id(&r, 0, all_call, sizeof all_call);
    edacs_grant_incoming_audio(&r, EDACS_GRANT_ALL_CALL);
    clock_at(g_now_s + 1);
    const unsigned long long msg1 = ((1010ULL & 0x3F80ULL) << 11U) | (4ULL << 12U) | (1ULL << 11U) | 321ULL;
    const unsigned long long msg2 = (1010ULL & 0x7FULL) << 17U;
    edacs_process_valid_frame(r.opts, r.state, msg1, msg2);
    rc |= expect_int("same LID: the group grant merged into the all-call", (long long)call_epoch(&r), (long long)epoch);
    edacs_grant_incoming_audio(&r, EDACS_GRANT_GROUP);
    clock_at(g_now_s + 2);
    end_call(&r, DSD_CALL_END_EXPLICIT);

    reload(&r);
    const char* pub = publish_of(all_call);
    rc |= expect_int("same LID: the all-call segment holds its own audio", pub ? field_ll(pub, "duration_ms") : -9, 60);
    rc |= expect_edacs_all_call_sidecar("same LID", &r, pub, 1010U);
    rc |= expect_pending_then_linked("same LID", all_call, epoch, &r.state->event_history_s[0],
                                     r.state->event_history_s[0].push_seq);
    edacs_grant_rig_close(&r);
    return rc;
}

/* --- Issue #888 B19: the first call TETRA admits on a slot does not take the audio its file held before. --- */

typedef enum {
    TETRA_FIRST_ANON =
        0,           /* trunked-mode speech no call record names, admitted once a CRC-16 block proves the carrier */
    TETRA_FIRST_DMO, /* direct-mode speech heard before the DM-SETUP that names its call */
} tetra_first_kind;

#define TETRA_FIRST_BURST_FRAMES 480LL /* one full slot: two 240-sample speech frames at 8 kHz */

/* Not trunking, the TDMA clock on timeslot 1, clear speech: what the speech path plays it also records. */
static void
tetra_first_rig(rig* r, tetra_first_kind kind) {
    r->opts->audio_out = 0;
    r->opts->rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    r->opts->rdio_system_id = 48;
    r->opts->rdio_unknown_talkgroup = 4242; /* the rig's input knows no frequency (issue #888 D2) */
    r->state->synctype = DSD_SYNC_TETRA_NDB_POS;
    tetra_state_t* ts = tetra_st(r->state);
    ts->tdma_valid = 1;
    ts->tn = 1;
    if (kind == TETRA_FIRST_DMO) {
        ts->dmo = 1;
        ts->dmo_crypt = TETRA_DMO_CRYPT_CLEAR;
    }
}

/* @p bursts good full-slot speech bursts through the speech path (the real codec, the record gate, the slot file). */
static void
tetra_first_speech(rig* r, int bursts) {
    uint8_t bits[TETRA_SPEECH_FULL_BITS];
    for (int i = 0; i < TETRA_SPEECH_FULL_BITS; i++) {
        bits[i] = (uint8_t)(((unsigned)i * 7U + 3U) >> 2U) & 1U;
    }
    for (int b = 0; b < bursts; b++) {
        tetra_voice_full_slot(r->opts, r->state, bits, 1);
    }
}

/* What admits the call: a CRC-16 signalling block proving the carrier, or the DM-SETUP of the direct-mode call. */
static void
tetra_first_admit(rig* r, tetra_first_kind kind) {
    if (kind == TETRA_FIRST_DMO) {
        tetra_state_t* ts = tetra_st(r->state);
        ts->calls.dmo.active = 1;
        ts->calls.dmo.dst = 1234U;
        ts->calls.dmo.src = 2345U;
        return;
    }
    tetra_confirm_burst_begin(r->state);
    tetra_confirm_note_signalling(r->state);
    (void)tetra_confirm_burst_end(r->state);
}

/* The published recordings in the rig's folder, by length. */
typedef struct {
    const char* dir;
    sf_count_t prefill_frames;
    sf_count_t call_frames;
    int prefill;             /* recordings @c prefill_frames long */
    int prefill_sidecar;     /* of those, with an rdio-scanner sidecar */
    int call;                /* recordings @c call_frames long */
    char call_sidecar[1024]; /* the call's rdio-scanner sidecar, empty when it has none */
    int other;               /* any other length: the call's audio with the earlier audio in front */
} tetra_first_files;

static int
tetra_first_files_cb(const char* name, void* user) {
    tetra_first_files* files = (tetra_first_files*)user;
    const size_t len = strlen(name);
    if (len < 4 || strcmp(name + len - 4, ".wav") != 0 || strncmp(name, "TEMP_", 5) == 0) {
        return 0;
    }
    char path[DSD_TEST_PATH_MAX * 2];
    if (dsd_test_path_join(path, sizeof path, files->dir, name) != 0) {
        return 0;
    }
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof info);
    SNDFILE* f = sf_open(path, SFM_READ, &info);
    if (f != NULL) {
        (void)sf_close(f);
    }
    if (info.frames == files->prefill_frames) {
        files->prefill++;
        char sidecar[DSD_TEST_PATH_MAX * 2];
        DSD_SNPRINTF(sidecar, sizeof sidecar, "%.*s.json", (int)(strlen(path) - 4), path);
        FILE* s = dsd_fopen_existing_regular_file(sidecar, "rb");
        if (s != NULL) {
            files->prefill_sidecar++;
            (void)fclose(s);
        }
    } else if (info.frames == files->call_frames) {
        files->call++;
        char sidecar[DSD_TEST_PATH_MAX * 2];
        DSD_SNPRINTF(sidecar, sizeof sidecar, "%.*s.json", (int)(strlen(path) - 4), path);
        FILE* s = dsd_fopen_existing_regular_file(sidecar, "rb");
        if (s != NULL) {
            const size_t got = fread(files->call_sidecar, 1, sizeof files->call_sidecar - 1U, s);
            files->call_sidecar[got] = '\0';
            (void)fclose(s);
        }
    } else {
        files->other++;
    }
    return 0;
}

/* Speech written to slot 1's file before anything admits a call (300 ms: before signalling proves the carrier, or
   before the DM-SETUP), then the call's first speech admits it and its own audio follows (120 ms). The admission
   closes the earlier audio as no row's, unexported; the call's recording holds its own audio alone, under its row.
   Without it the first call's commit published both under the call's row. With the journal on and off. */
static int
test_tetra_first_call_leaves_unowned_audio(void) {
    static const char* const k_kind[] = {"tetra anonymous call", "tetra direct-mode call"};
    int rc = 0;
    for (int kind = TETRA_FIRST_ANON; kind <= TETRA_FIRST_DMO; kind++) {
        for (int with_journal = 1; with_journal >= 0; with_journal--) {
            char tag[160];
            rig r;
            rc |= rig_open_ex(&r, "dsdneo_journal_tetra_first", with_journal);
            tetra_first_rig(&r, (tetra_first_kind)kind);
            submit(&r, DSD_APP_CMD_WAV_START);
            char prefill[64];
            slot_id(&r, 0, prefill, sizeof prefill);
            tetra_first_speech(&r, 5);
            DSD_SNPRINTF(tag, sizeof tag, "%s: no call before it is admitted", k_kind[kind]);
            rc |= expect_int(tag, (long long)call_epoch(&r), 0);
            DSD_SNPRINTF(tag, sizeof tag, "%s: the earlier speech is in the slot file", k_kind[kind]);
            rc |= expect_int(tag, (long long)dsd_wav_segment_frames(r.opts->wav_out_f), 5LL * TETRA_FIRST_BURST_FRAMES);
            clock_at(g_now_s + 1);
            tetra_first_admit(&r, (tetra_first_kind)kind);
            tetra_first_speech(&r, 2);
            DSD_SNPRINTF(tag, sizeof tag, "%s: its speech admitted the call", k_kind[kind]);
            rc |= expect_true(tag, call_epoch(&r) != 0U);
            char own_id[64];
            slot_id(&r, 0, own_id, sizeof own_id);
            clock_at(g_now_s + 2);
            end_call(&r, DSD_CALL_END_TERMINATOR);

            if (with_journal) {
                reload(&r);
                const char* left = NULL;
                const char* own = NULL;
                const char* pub = NULL;
                for (int i = 0; (pub = record(&g_j, "publish", i)) != NULL; i++) {
                    if (strcmp(field_s(pub, "id"), prefill) == 0) {
                        left = pub;
                    } else if (strcmp(field_s(pub, "id"), own_id) == 0) {
                        own = pub;
                    }
                }
                DSD_SNPRINTF(tag, sizeof tag, "%s: the earlier speech", k_kind[kind]);
                rc |= expect_orphan(tag, left, 0, 300);
                DSD_SNPRINTF(tag, sizeof tag, "%s: the call records in a file of its own", k_kind[kind]);
                rc |= expect_true(tag, own != NULL && strcmp(own_id, prefill) != 0);
                DSD_SNPRINTF(tag, sizeof tag, "%s: holding only its own audio", k_kind[kind]);
                rc |= expect_int(tag, own ? field_ll(own, "duration_ms") : -9, 120);
                DSD_SNPRINTF(tag, sizeof tag, "%s: under its row", k_kind[kind]);
                rc |= expect_int(tag, own ? field_ll(own, "row") : -9, (long long)r.state->event_history_s[0].push_seq);
            }
            tetra_first_files files;
            DSD_MEMSET(&files, 0, sizeof files);
            files.dir = r.dir;
            files.prefill_frames = 5 * TETRA_FIRST_BURST_FRAMES;
            files.call_frames = 2 * TETRA_FIRST_BURST_FRAMES;
            (void)dsd_dir_list(r.dir, tetra_first_files_cb, &files);
            DSD_SNPRINTF(tag, sizeof tag, "%s: the earlier speech is published once", k_kind[kind]);
            rc |= expect_int(tag, files.prefill, 1);
            DSD_SNPRINTF(tag, sizeof tag, "%s: and not exported", k_kind[kind]);
            rc |= expect_int(tag, files.prefill_sidecar, 0);
            DSD_SNPRINTF(tag, sizeof tag, "%s: the call's recording holds its audio alone", k_kind[kind]);
            rc |= expect_int(tag, files.call, 1);
            DSD_SNPRINTF(tag, sizeof tag, "%s: no recording mixes the two", k_kind[kind]);
            rc |= expect_int(tag, files.other, 0);
            /* Issue #888: once admitted, the call is exported. The anonymous call names no talkgroup and is exported
               under --rdio-unknown-talkgroup, labelled by its protocol; the direct-mode call under its own. */
            const int anon = kind == TETRA_FIRST_ANON;
            DSD_SNPRINTF(tag, sizeof tag, "%s: the call is exported under %s", k_kind[kind],
                         anon ? "the fallback talkgroup" : "its talkgroup");
            rc |= expect_true(tag, strstr(files.call_sidecar, anon ? "\"talkgroup\": 4242," : "\"talkgroup\": 1234,")
                                       != NULL);
            if (anon) {
                DSD_SNPRINTF(tag, sizeof tag, "%s: labelled by its protocol", k_kind[kind]);
                rc |= expect_true(tag, strstr(files.call_sidecar, "\"talkgroup_tag\": \"TETRA\",") != NULL);
            }
            if (rc != 0) {
                DSD_FPRINTF(stderr, "  %s sidecar: %s\n", k_kind[kind], files.call_sidecar);
            }
            r.opts->rdio_mode = DSD_RDIO_MODE_OFF;
            r.opts->rdio_unknown_talkgroup = 0;
            rig_close(&r);
        }
    }
    return rc;
}

/* @p bursts good half-slot speech bursts: the first half stolen for signalling, the second speech. */
static void
tetra_first_half_speech(rig* r, int bursts) {
    uint8_t bits[TETRA_SPEECH_HALF_BITS];
    for (int i = 0; i < TETRA_SPEECH_HALF_BITS; i++) {
        bits[i] = (uint8_t)(((unsigned)i * 5U + 1U) >> 2U) & 1U;
    }
    for (int b = 0; b < bursts; b++) {
        tetra_voice_half_slot(r->opts, r->state, bits, 1);
    }
}

/* Anonymous speech that begins with a half slot leaves half an output frame (80 samples) of it waiting to be played
   when signalling proves the carrier. The admission drops it with the rest of the earlier audio: the admitted call's
   speech plays none of it, so after its two full slots nothing is carried, and its recording is its own two slots.
   Before, those 80 samples led the call's recording. */
static int
test_tetra_first_call_drops_carried_samples(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_tetra_carry");
    tetra_first_rig(&r, TETRA_FIRST_ANON);
    submit(&r, DSD_APP_CMD_WAV_START);
    tetra_first_half_speech(&r, 1);
    tetra_first_speech(&r, 5);
    const tetra_state_t* ts = tetra_st(r.state);
    rc |= expect_int("tetra carry: the earlier speech leaves half a frame waiting", (long long)ts->voice.carry_len, 80);
    /* The same second: the speech goes on with the same owner (voice_bind()), so only the admission can drop it. */
    tetra_first_admit(&r, TETRA_FIRST_ANON);
    tetra_first_speech(&r, 2);
    rc |= expect_true("tetra carry: its speech admitted the call", call_epoch(&r) != 0U);
    rc |= expect_int("tetra carry: the call's speech carries none of the earlier", (long long)ts->voice.carry_len, 0);
    clock_at(g_now_s + 2);
    end_call(&r, DSD_CALL_END_TERMINATOR);
    tetra_first_files files;
    DSD_MEMSET(&files, 0, sizeof files);
    files.dir = r.dir;
    files.prefill_frames = 5 * TETRA_FIRST_BURST_FRAMES + 160;
    files.call_frames = 2 * TETRA_FIRST_BURST_FRAMES;
    (void)dsd_dir_list(r.dir, tetra_first_files_cb, &files);
    rc |= expect_int("tetra carry: the earlier speech is published once", files.prefill, 1);
    rc |= expect_int("tetra carry: and not exported", files.prefill_sidecar, 0);
    rc |= expect_int("tetra carry: the call's recording is its own two slots", files.call, 1);
    rc |= expect_int("tetra carry: no other recording", files.other, 0);
    r.opts->rdio_mode = DSD_RDIO_MODE_OFF;
    r.opts->rdio_unknown_talkgroup = 0;
    rig_close(&r);
    return rc;
}

/* Anonymous speech before signalling proves the carrier leaves the slot's speech decoder confirmed, with its history.
   When the admitted call's first burst is a half slot, its stolen first frame is not concealed from that earlier
   speech: the admission starts the speech over, so the burst plays its own speech frame alone (240 samples, 160 out and
   80 carried) and nothing concealed. Before, the stolen frame was concealed from the earlier speech and led the call's
   recording by 240 samples (30 ms). */
static int
test_tetra_first_call_conceals_nothing_before_it(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_tetra_conceal");
    tetra_first_rig(&r, TETRA_FIRST_ANON);
    submit(&r, DSD_APP_CMD_WAV_START);
    tetra_first_speech(&r, 5);
    const tetra_state_t* ts = tetra_st(r.state);
    rc |= expect_true("tetra conceal: the earlier speech confirmed the decoder", ts->voice.confirmed != 0U);
    tetra_first_admit(&r, TETRA_FIRST_ANON);
    const uint32_t concealed = ts->voice.concealed;
    tetra_first_half_speech(&r, 1);
    rc |= expect_true("tetra conceal: the half slot admitted the call", call_epoch(&r) != 0U);
    rc |=
        expect_int("tetra conceal: the stolen frame is not concealed", (long long)(ts->voice.concealed - concealed), 0);
    rc |= expect_int("tetra conceal: the speech frame alone, half a frame carried", (long long)ts->voice.carry_len, 80);
    tetra_first_speech(&r, 1);
    clock_at(g_now_s + 2);
    end_call(&r, DSD_CALL_END_TERMINATOR);
    tetra_first_files files;
    DSD_MEMSET(&files, 0, sizeof files);
    files.dir = r.dir;
    files.prefill_frames = 5 * TETRA_FIRST_BURST_FRAMES;
    files.call_frames = 160 + TETRA_FIRST_BURST_FRAMES;
    (void)dsd_dir_list(r.dir, tetra_first_files_cb, &files);
    rc |= expect_int("tetra conceal: the earlier speech is published once", files.prefill, 1);
    rc |= expect_int("tetra conceal: the call's recording is its own speech", files.call, 1);
    rc |= expect_int("tetra conceal: no other recording", files.other, 0);
    r.opts->rdio_mode = DSD_RDIO_MODE_OFF;
    r.opts->rdio_unknown_talkgroup = 0;
    rig_close(&r);
    return rc;
}

/* The samples of the one published recording in a folder that is @p frames long. */
typedef struct {
    char dir[DSD_TEST_PATH_MAX];
    sf_count_t frames;
    short pcm[2 * TETRA_FIRST_BURST_FRAMES];
    int found;
} tetra_call_pcm;

static int
tetra_call_pcm_cb(const char* name, void* user) {
    tetra_call_pcm* want = (tetra_call_pcm*)user;
    const size_t len = strlen(name);
    char path[DSD_TEST_PATH_MAX * 2];
    if (len < 4 || strcmp(name + len - 4, ".wav") != 0 || strncmp(name, "TEMP_", 5) == 0
        || dsd_test_path_join(path, sizeof path, want->dir, name) != 0) {
        return 0;
    }
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof info);
    SNDFILE* f = sf_open(path, SFM_READ, &info);
    if (f == NULL) {
        return 0;
    }
    if (info.frames == want->frames && info.channels == 1 && want->frames <= (sf_count_t)(sizeof want->pcm / 2U)
        && sf_readf_short(f, want->pcm, want->frames) == want->frames) {
        want->found++;
    }
    (void)sf_close(f);
    return 0;
}

/* The admitted call's recording, its two full slots, with the digital high-pass on (-v 0x9): after @p prefill bursts
   of speech no call owned, or none. */
static int
tetra_first_call_hpf_run(int prefill, tetra_call_pcm* out) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_tetra_hpf");
    tetra_first_rig(&r, TETRA_FIRST_ANON);
    r.opts->use_hpf_d = 1;
    submit(&r, DSD_APP_CMD_WAV_START);
    tetra_first_speech(&r, prefill);
    clock_at(g_now_s + 1);
    tetra_first_admit(&r, TETRA_FIRST_ANON);
    tetra_first_speech(&r, 2);
    rc |= expect_true("tetra hpf: its speech admitted the call", call_epoch(&r) != 0U);
    clock_at(g_now_s + 2);
    end_call(&r, DSD_CALL_END_TERMINATOR);
    DSD_MEMSET(out, 0, sizeof *out);
    DSD_SNPRINTF(out->dir, sizeof out->dir, "%s", r.dir);
    out->frames = 2 * TETRA_FIRST_BURST_FRAMES;
    (void)dsd_dir_list(r.dir, tetra_call_pcm_cb, out);
    rc |= expect_int("tetra hpf: the call's recording", out->found, 1);
    r.opts->rdio_mode = DSD_RDIO_MODE_OFF;
    r.opts->rdio_unknown_talkgroup = 0;
    rig_close(&r);
    return rc;
}

/* Issue #888: with the digital high-pass on, the speech no call owned before the admission leaves nothing in the
   admitted call's audio: the slot's filter starts over with the call, so its recording is sample for sample the one
   it makes with no speech before it. Before, the filter kept the earlier speech's state, which the call's first
   samples carried. */
static int
test_tetra_first_call_starts_the_high_pass_over(void) {
    static tetra_call_pcm alone;
    static tetra_call_pcm after;
    int rc = tetra_first_call_hpf_run(0, &alone);
    rc |= tetra_first_call_hpf_run(5, &after);
    int differ = 0;
    for (sf_count_t i = 0; i < alone.frames; i++) {
        differ += alone.pcm[i] != after.pcm[i] ? 1 : 0;
    }
    rc |= expect_int("tetra hpf: the call's audio is the same after the earlier speech", differ, 0);
    return rc;
}

/* An identity-less TETRA call (anonymous, admitted once signalling proved the carrier) fades: its row is held for the
   reacquisition window, the drop verdict pending, and its audio stays in slot 1's file. Speech then admits a new call
   on the slot: the admission closes the held call's audio as its own recording, pending on its epoch and unexported
   (unlinked when that row is dropped), and the new call's recording holds only its own audio, under its row, exported
   as its own. */
static int
test_tetra_held_call_then_readmitted(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_tetra_held");
    tetra_first_rig(&r, TETRA_FIRST_ANON);
    submit(&r, DSD_APP_CMD_WAV_START);
    tetra_first_admit(&r, TETRA_FIRST_ANON);
    tetra_first_speech(&r, 3);
    const uint64_t held_epoch = call_epoch(&r);
    rc |= expect_true("tetra held: its speech admitted the first call", held_epoch != 0U);
    char held[64];
    slot_id(&r, 0, held, sizeof held);
    clock_at(g_now_s + 1);
    end_call(&r, DSD_CALL_END_SYNC_LOSS);
    rc |= expect_int("tetra held: the faded call's audio stays in the slot file",
                     (long long)dsd_wav_segment_frames(r.opts->wav_out_f), 3LL * TETRA_FIRST_BURST_FRAMES);
    clock_at(g_now_s + 1);
    tetra_first_admit(&r, TETRA_FIRST_ANON);
    tetra_first_speech(&r, 2);
    const uint64_t epoch = call_epoch(&r);
    rc |= expect_true("tetra held: speech admitted a new call", epoch != 0U && epoch != held_epoch);
    char own_id[64];
    slot_id(&r, 0, own_id, sizeof own_id);
    clock_at(g_now_s + 2);
    end_call(&r, DSD_CALL_END_TERMINATOR);

    reload(&r);
    const char* left = publish_of(held);
    const char* own = publish_of(own_id);
    rc |= expect_true("tetra held: the held call's audio is published", left != NULL);
    rc |= expect_true("tetra held: in a file of its own", own != NULL && strcmp(own_id, held) != 0);
    rc |= expect_true("tetra held: pending on its epoch, unexported",
                      left && strcmp(field_s(left, "pending"), "true") == 0
                          && field_ll(left, "epoch") == (long long)held_epoch
                          && strcmp(field_s(left, "rdio"), "false") == 0);
    rc |= expect_int("tetra held: holding its audio", left ? field_ll(left, "duration_ms") : -9, 180);
    rc |= expect_int("tetra held: the new call's recording holds only its audio",
                     own ? field_ll(own, "duration_ms") : -9, 120);
    rc |= expect_int("tetra held: under the new call's row", own ? field_ll(own, "row") : -9,
                     (long long)r.state->event_history_s[0].push_seq);
    rc |= expect_int("tetra held: never linked to a row", count_for_id(&g_j, "link", held), 0);
    rc |=
        expect_int("tetra held: unlinked once its identity-less row is dropped", count_for_id(&g_j, "unlink", held), 1);
    char body[4096];
    read_published_sidecar(&r, left, body, sizeof body);
    rc |= expect_str("tetra held: no sidecar for the held call's audio", body, "");
    read_published_sidecar(&r, own, body, sizeof body);
    rc |= expect_true("tetra held: the new call is exported under the fallback talkgroup",
                      strstr(body, "\"talkgroup\": 4242,") != NULL);
    r.opts->rdio_mode = DSD_RDIO_MODE_OFF;
    r.opts->rdio_unknown_talkgroup = 0;
    rig_close(&r);
    return rc;
}

/* --- Issue #888 B20: each file -r plays is a recording of its own; none joins another file's audio. --- */

/* A clean AMBE 3600x2450 frame as SDRTrunk records DMR voice (36 dibits, two per hex digit; see CORE_MBE_FILE_IO). */
static const char k_playback_dmr_hex[] = "3ABF032D9ADEE4CB50";

enum {
    PLAYBACK_FILES = 4,
    PLAYBACK_NAMED = 2, /* the SDRTrunk export: the named call */
};

/* The files -r plays, in order: raw AMBE, raw AMBE, an SDRTrunk export naming a call, raw AMBE. Each has its own
   frame count, so a recording's length tells whose audio it holds. */
static const char* const k_playback_names[PLAYBACK_FILES] = {"first.amb", "second.amb", "named.mbe", "last.amb"};
static const int k_playback_frames[PLAYBACK_FILES] = {5, 2, 3, 4}; /* the export's 3: its three frames */

/* The SDRTrunk export: a clear DMR group call to talkgroup 1234 from radio 5678, three voice frames. */
static int
playback_write_named(FILE* f) {
    char json[512];
    const int len =
        DSD_SNPRINTF(json, sizeof json,
                     "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
                     "\"encrypted\":false,\"frames\":[{\"hex\":\"%s\"},{\"hex\":\"%s\"},{\"hex\":\"%s\"}]}",
                     k_playback_dmr_hex, k_playback_dmr_hex, k_playback_dmr_hex);
    return (len <= 0 || (size_t)len >= sizeof json || fwrite(json, 1, (size_t)len, f) != (size_t)len) ? 1 : 0;
}

/* A raw AMBE file of @p frames frames: the cookie, then one 8-byte record (an error byte, the 49 parameter bits) per
   frame. */
static int
playback_write_raw(FILE* f, int frames) {
    static const unsigned char frame[8] = {0x80U, 0x5AU, 0x3CU, 0x71U, 0x0FU, 0xA5U, 0x2EU, 0x01U};
    int rc = fwrite(".amb", 1, 4, f) != 4 ? 1 : 0;
    for (int i = 0; i < frames; i++) {
        rc |= fwrite(frame, 1, sizeof frame, f) != sizeof frame ? 1 : 0;
    }
    return rc;
}

/* Write file @p n of the playback run into the rig's folder at @p path. */
static int
playback_write_file(const rig* r, int n, char* path, size_t cap) {
    if (dsd_test_path_join(path, cap, r->dir, k_playback_names[n]) != 0) {
        return 1;
    }
    FILE* f = dsd_fopen_private(path, "wb");
    if (f == NULL) {
        return 1;
    }
    int rc = n == PLAYBACK_NAMED ? playback_write_named(f) : playback_write_raw(f, k_playback_frames[n]);
    rc |= fclose(f) != 0 ? 1 : 0;
    return rc;
}

/* The published recordings in the rig's folder: how many hold each file's audio alone (and of those, how many have an
   rdio-scanner sidecar), and how many hold anything else (two files' audio together). */
typedef struct {
    const char* dir;
    int file[PLAYBACK_FILES];
    int sidecar[PLAYBACK_FILES];
    int other;
} playback_files;

static int
playback_files_cb(const char* name, void* user) {
    playback_files* files = (playback_files*)user;
    const size_t len = strlen(name);
    if (len < 4 || strcmp(name + len - 4, ".wav") != 0 || strncmp(name, "TEMP_", 5) == 0) {
        return 0;
    }
    char path[DSD_TEST_PATH_MAX * 2];
    if (dsd_test_path_join(path, sizeof path, files->dir, name) != 0) {
        return 0;
    }
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof info);
    SNDFILE* f = sf_open(path, SFM_READ, &info);
    if (f != NULL) {
        (void)sf_close(f);
    }
    for (int n = 0; n < PLAYBACK_FILES; n++) {
        if (info.frames == (sf_count_t)k_playback_frames[n] * 160) {
            files->file[n]++;
            char sidecar[DSD_TEST_PATH_MAX * 2];
            DSD_SNPRINTF(sidecar, sizeof sidecar, "%.*s.json", (int)(strlen(path) - 4), path);
            FILE* s = dsd_fopen_existing_regular_file(sidecar, "rb");
            if (s != NULL) {
                files->sidecar[n]++;
                (void)fclose(s);
            }
            return 0;
        }
    }
    files->other++;
    return 0;
}

/* The run ends as the engine ends it: the last calls finalize at no carrier, then the slot files close and publish. */
static void
playback_end_run(rig* r) {
    dsd_event_sync_slot(r->opts, r->state, 0);
    dsd_event_sync_slot(r->opts, r->state, 1);
    noCarrier(r->opts, r->state);
    dsd_event_sync_slot(r->opts, r->state, 0);
    dsd_event_sync_slot(r->opts, r->state, 1);
    dsd_event_flush_pending_alerts(r->opts, r->state);
    r->opts->wav_out_f = close_and_publish_call_wav(r->opts->wav_out_f, r->opts, r->state, 0, r->opts->wav_out_file,
                                                    r->opts->wav_out_dir, r->opts->wav_out_open_time);
    r->opts->wav_out_fR = close_and_publish_call_wav(r->opts->wav_out_fR, r->opts, r->state, 1, r->opts->wav_out_fileR,
                                                     r->opts->wav_out_dir, r->opts->wav_out_open_timeR);
}

/* The journal's publish lines: the named call's own audio under its row; each raw file's audio once, as no row's,
   unexported. */
static int
playback_check_journal(const rig* r, uint64_t named_row) {
    reload(r);
    int rc = 0;
    int seen[PLAYBACK_FILES] = {0};
    const char* pub = NULL;
    for (int i = 0; (pub = record(&g_j, "publish", i)) != NULL; i++) {
        const long long ms = field_ll(pub, "duration_ms");
        int n = 0;
        while (n < PLAYBACK_FILES && ms != (long long)k_playback_frames[n] * 20) {
            n++;
        }
        char tag[160];
        DSD_SNPRINTF(tag, sizeof tag, "-r playback: the %lld ms recording holds one file's audio (row %s)", ms,
                     field_s(pub, "row"));
        if (n >= PLAYBACK_FILES) {
            rc |= expect_true(tag, 0);
            continue;
        }
        seen[n]++;
        DSD_SNPRINTF(tag, sizeof tag, "-r playback: %s", k_playback_names[n]);
        if (n == PLAYBACK_NAMED) {
            rc |= expect_int(tag, field_ll(pub, "row"), (long long)named_row);
        } else {
            rc |= expect_orphan(tag, pub, 0, (long long)k_playback_frames[n] * 20);
        }
    }
    for (int n = 0; n < PLAYBACK_FILES; n++) {
        char tag[160];
        DSD_SNPRINTF(tag, sizeof tag, "-r playback: %s published once", k_playback_names[n]);
        rc |= expect_int(tag, seen[n], 1);
    }
    return rc;
}

/* -P -r first.amb second.amb named.mbe last.amb: raw playback writes the slot file with no call, so before the fix
   the first two files' audio shared one file, and the named call's commit published it with the named call's own
   audio as one recording, exported under the call's row. Each file boundary now closes the audio no call owns, so
   every file is a recording of its own and the named call's export holds its own audio alone. Journal on and off. */
static int
test_playback_files_record_apart(void) {
    int rc = 0;
    for (int with_journal = 1; with_journal >= 0; with_journal--) {
        rig r;
        rc |= rig_open_ex(&r, "dsdneo_journal_playback", with_journal);
        r.opts->audio_out = 0;
        r.opts->playfiles = 1;
        r.opts->rdio_mode = DSD_RDIO_MODE_DIRWATCH;
        r.opts->rdio_system_id = 48;
        submit(&r, DSD_APP_CMD_WAV_START);
        char paths[PLAYBACK_FILES][DSD_TEST_PATH_MAX * 2];
        char* argv[PLAYBACK_FILES + 1] = {"dsd-neo"};
        for (int n = 0; n < PLAYBACK_FILES; n++) {
            rc |= expect_int("-r playback: input written", playback_write_file(&r, n, paths[n], sizeof paths[n]), 0);
            argv[n + 1] = paths[n];
        }
        r.state->optind = 1;
        playMbeFiles(r.opts, r.state, PLAYBACK_FILES + 1, argv);
        playback_end_run(&r);

        if (with_journal) {
            rc |= playback_check_journal(&r, r.state->event_history_s[0].push_seq);
        }
        playback_files files;
        DSD_MEMSET(&files, 0, sizeof files);
        files.dir = r.dir;
        (void)dsd_dir_list(r.dir, playback_files_cb, &files);
        for (int n = 0; n < PLAYBACK_FILES; n++) {
            char tag[160];
            DSD_SNPRINTF(tag, sizeof tag, "-r playback (journal %s): %s is one recording of its own",
                         with_journal ? "on" : "off", k_playback_names[n]);
            rc |= expect_int(tag, files.file[n], 1);
            DSD_SNPRINTF(tag, sizeof tag, "-r playback (journal %s): %s %s", with_journal ? "on" : "off",
                         k_playback_names[n], n == PLAYBACK_NAMED ? "is exported" : "is not exported");
            rc |= expect_int(tag, files.sidecar[n], n == PLAYBACK_NAMED ? 1 : 0);
        }
        rc |= expect_int(with_journal ? "-r playback (journal on): no recording joins two files"
                                      : "-r playback (journal off): no recording joins two files",
                         files.other, 0);
        r.opts->rdio_mode = DSD_RDIO_MODE_OFF;
        rig_close(&r);
    }
    return rc;
}

/* --- Issue #888 B21: a played SDRTrunk export is filed at its first frame. --- */

enum {
    STAMPED_FRAMES = 100, /* 2 s of audio: the sidecar's duration */
};

/* The recording's clock, not the rig's: its first frame in Unix milliseconds, 20 ms a frame after it. Its last frame
   falls two seconds on, in second k_stamped_first_ms / 1000 + 2. */
static const uint64_t k_stamped_first_ms = 1600000000250ULL;

/* An SDRTrunk export of one record: @p head, the record's members up to and including its "frames":[, then the voice
   frames, each with its own Unix-millisecond "time" (a JSON number) before its "hex", and the record's close. */
static int
stamped_write_record(const char* path, const char* head) {
    static char json[STAMPED_FRAMES * 64 + 512];
    size_t len = 0;
    int n = DSD_SNPRINTF(json, sizeof json, "%s", head);
    if (n <= 0 || (size_t)n >= sizeof json) {
        return 1;
    }
    len = (size_t)n;
    for (int i = 0; i < STAMPED_FRAMES; i++) {
        n = DSD_SNPRINTF(json + len, sizeof json - len, "%s{\"time\":%llu,\"hex\":\"%s\"}", i > 0 ? "," : "",
                         (unsigned long long)(k_stamped_first_ms + (uint64_t)i * 20ULL), k_playback_dmr_hex);
        if (n <= 0 || (size_t)n >= sizeof json - len) {
            return 1;
        }
        len += (size_t)n;
    }
    n = DSD_SNPRINTF(json + len, sizeof json - len, "]}");
    if (n <= 0 || (size_t)n >= sizeof json - len) {
        return 1;
    }
    len += (size_t)n;
    FILE* f = dsd_fopen_private(path, "wb");
    if (f == NULL) {
        return 1;
    }
    int rc = fwrite(json, 1, len, f) != len ? 1 : 0;
    rc |= fclose(f) != 0 ? 1 : 0;
    return rc;
}

/* An SDRTrunk export as SDRTrunk writes it: a clear DMR group call from 5678 to @p to. */
static int
stamped_write_export(const char* path, const char* to) {
    char head[256];
    const int n = DSD_SNPRINTF(head, sizeof head,
                               "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\","
                               "\"to\":\"%s\",\"encrypted\":false,\"frames\":[",
                               to);
    if (n <= 0 || (size_t)n >= sizeof head) {
        return 1;
    }
    return stamped_write_record(path, head);
}

/* The sidecar of the journal's publish record @p pub, read into @p body (empty when there is none). */
static void
read_sidecar_of(const rig* r, const char* pub, char* body, size_t cap) {
    body[0] = '\0';
    char sidecar[DSD_TEST_PATH_MAX * 2];
    if (pub == NULL || dsd_test_path_join(sidecar, sizeof sidecar, r->dir, field_s(pub, "file")) != 0
        || strlen(sidecar) <= 4) {
        return;
    }
    const size_t stem = strlen(sidecar) - 4; /* the ".wav" the sidecar's ".json" replaces */
    DSD_SNPRINTF(sidecar + stem, sizeof sidecar - stem, "%s", ".json");
    FILE* f = dsd_fopen_existing_regular_file(sidecar, "rb");
    if (f != NULL) {
        const size_t got = fread(body, 1, cap - 1, f);
        body[got] = '\0';
        (void)fclose(f);
    }
}

/* The sidecar of the journal's first published recording. */
static void
stamped_read_sidecar(const rig* r, char* body, size_t cap) {
    read_sidecar_of(r, record(&g_j, "publish", 0), body, cap);
}

/* -P -r named.mbe, an export whose frames span two seconds of its own clock: the recording is filed at its first
   frame's second, through the run's end and the real close and export, and its stop is that start plus the WAV's
   length. Before, every frame's stamp replaced the row's, so the recording started at its last frame and ended a
   duration after that. The rig's clock is years from the recording's: the WAV's open time on it is not the start. */
static int
test_playback_export_starts_at_its_first_frame(void) {
    int rc = 0;
    rig r;
    rc |= rig_open(&r, "dsdneo_journal_playback_start");
    r.opts->audio_out = 0;
    r.opts->playfiles = 1;
    r.opts->rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    r.opts->rdio_system_id = 48;
    submit(&r, DSD_APP_CMD_WAV_START);
    char path[DSD_TEST_PATH_MAX * 2];
    rc |= expect_int(
        "-r export start: input written",
        dsd_test_path_join(path, sizeof path, r.dir, "named.mbe") == 0 ? stamped_write_export(path, "1234") : 1, 0);
    char* argv[2] = {"dsd-neo", path};
    r.state->optind = 1;
    playMbeFiles(r.opts, r.state, 2, argv);
    playback_end_run(&r);

    reload(&r);
    const long long start = (long long)(k_stamped_first_ms / 1000ULL);
    const char* pub = record(&g_j, "publish", 0);
    rc |= expect_true("-r export start: one recording published", pub != NULL && record(&g_j, "publish", 1) == NULL);
    rc |= expect_int("-r export start: the recording holds every frame", pub ? field_ll(pub, "duration_ms") : -1,
                     (long long)STAMPED_FRAMES * 20);
    rc |= expect_int("-r export start: the journal's start is the first frame's second",
                     pub ? field_ll(pub, "start") : -1, start);
    static char body[8192];
    stamped_read_sidecar(&r, body, sizeof body);
    char want[64];
    DSD_SNPRINTF(want, sizeof want, "\"start_time\": %lld,", start);
    rc |= expect_true("-r export start: the sidecar's start_time is the first frame's second",
                      strstr(body, want) != NULL);
    DSD_SNPRINTF(want, sizeof want, "\"stop_time\": %lld,", start + (long long)(STAMPED_FRAMES * 20 / 1000));
    rc |= expect_true("-r export start: the sidecar's stop_time is its start plus the WAV's length",
                      strstr(body, want) != NULL);
    if (strstr(body, "\"start_time\"") == NULL) {
        DSD_FPRINTF(stderr, "  sidecar: none\n");
    } else if (rc != 0) {
        DSD_FPRINTF(stderr, "  sidecar: %s\n", body);
    }
    r.opts->rdio_mode = DSD_RDIO_MODE_OFF;
    rig_close(&r);
    return rc;
}

/* Issue #888: -P -r unnumbered.mbe, an export of a call from 5678 that names no talkgroup, played while the receiver
   sits on 452.3125 MHz with colour code 3 decoded. A played row takes no tuner frequency or code, so with
   --rdio-unknown-talkgroup the commit exports it under that number, labelled by its protocol alone, at the recording's
   first frame, with freq 0 and no freqList; without the option it is published and kept local. Through playMbeFiles(),
   the run's end and the real close and export. */
static int
test_playback_unnumbered_call_export(void) {
    int rc = 0;
    for (int with_option = 1; with_option >= 0; with_option--) {
        char tag[160];
        const char* leg = with_option ? "-r unnumbered, option set" : "-r unnumbered, no option";
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_playback_unnumbered");
        r.opts->audio_out = 0;
        r.opts->playfiles = 1;
        r.opts->audio_in_type = AUDIO_IN_RTL;
        r.opts->rtlsdr_center_freq = 452312500U;
        r.state->dmr_color_code = 3U;
        r.opts->rdio_mode = DSD_RDIO_MODE_DIRWATCH;
        r.opts->rdio_system_id = 48;
        r.opts->rdio_unknown_talkgroup = with_option ? 4242 : 0;
        submit(&r, DSD_APP_CMD_WAV_START);
        char path[DSD_TEST_PATH_MAX * 2];
        DSD_SNPRINTF(tag, sizeof tag, "%s: input written", leg);
        rc |= expect_int(
            tag,
            dsd_test_path_join(path, sizeof path, r.dir, "unnumbered.mbe") == 0 ? stamped_write_export(path, "0") : 1,
            0);
        char* argv[2] = {"dsd-neo", path};
        r.state->optind = 1;
        playMbeFiles(r.opts, r.state, 2, argv);
        playback_end_run(&r);

        reload(&r);
        const char* pub = record(&g_j, "publish", 0);
        DSD_SNPRINTF(tag, sizeof tag, "%s: one recording published under the call's row", leg);
        rc |= expect_true(tag, pub != NULL && record(&g_j, "publish", 1) == NULL
                                   && strcmp(field_s(pub, "row"), "null") != 0);
        DSD_SNPRINTF(tag, sizeof tag, "%s: the row keeps no talkgroup", leg);
        rc |= expect_str(tag, pub ? field_s(pub, "target") : "?", "");
        static char body[4096];
        read_sidecar_of(&r, pub, body, sizeof body);
        if (!with_option) {
            DSD_SNPRINTF(tag, sizeof tag, "%s: no sidecar", leg);
            rc |= expect_str(tag, body, "");
        } else {
            static const char* const k_want[] = {
                "\"talkgroup\": 4242,",
                "\"talkgroup_tag\": \"DMR\",",
                "\"srcList\": [{\"pos\":0,\"src\":5678}],",
                "\"freq\": 0,",
            };
            for (size_t i = 0; i < sizeof k_want / sizeof k_want[0]; i++) {
                DSD_SNPRINTF(tag, sizeof tag, "%s: the sidecar holds %s", leg, k_want[i]);
                rc |= expect_true(tag, strstr(body, k_want[i]) != NULL);
            }
            DSD_SNPRINTF(tag, sizeof tag, "%s: no freqList", leg);
            rc |= expect_true(tag, strstr(body, "freqList") == NULL);
            char want[64];
            DSD_SNPRINTF(want, sizeof want, "\"start_time\": %lld,", (long long)(k_stamped_first_ms / 1000ULL));
            DSD_SNPRINTF(tag, sizeof tag, "%s: filed at the recording's first frame", leg);
            rc |= expect_true(tag, strstr(body, want) != NULL);
        }
        if (rc != 0) {
            DSD_FPRINTF(stderr, "  %s sidecar: %s\n", leg, body);
        }
        r.opts->rdio_mode = DSD_RDIO_MODE_OFF;
        r.opts->rdio_unknown_talkgroup = 0;
        rig_close(&r);
    }
    return rc;
}

/* One record of a played SDRTrunk export: a DMR group call to @p tg (0 for none) from @p src (0: no "from" key), of
   @p frames voice frames stamped on the recording's clock from @p first_ms, 20 ms apart (0: no "time" key). */
typedef struct {
    uint32_t tg;
    uint32_t src;
    int frames;
    uint64_t first_ms;
} playback_record;

/* An export of @p n records, one a line, written at @p path. */
static int
records_write(const char* path, const playback_record* recs, int n) {
    static char json[4096];
    size_t len = 0;
    for (int c = 0; c < n; c++) {
        char from[32] = "";
        if (recs[c].src != 0U) {
            DSD_SNPRINTF(from, sizeof from, "\"from\":\"%u\",", recs[c].src);
        }
        int k = DSD_SNPRINTF(json + len, sizeof json - len,
                             "%s{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",%s\"to\":\"%u\","
                             "\"encrypted\":false,\"frames\":[",
                             c > 0 ? "\n" : "", from, recs[c].tg);
        if (k <= 0 || (size_t)k >= sizeof json - len) {
            return 1;
        }
        len += (size_t)k;
        for (int i = 0; i < recs[c].frames; i++) {
            char stamp[48] = "";
            if (recs[c].first_ms != 0U) {
                DSD_SNPRINTF(stamp, sizeof stamp, "\"time\":%llu,",
                             (unsigned long long)(recs[c].first_ms + (uint64_t)i * 20ULL));
            }
            k = DSD_SNPRINTF(json + len, sizeof json - len, "%s{%s\"hex\":\"%s\"}", i > 0 ? "," : "", stamp,
                             k_playback_dmr_hex);
            if (k <= 0 || (size_t)k >= sizeof json - len) {
                return 1;
            }
            len += (size_t)k;
        }
        k = DSD_SNPRINTF(json + len, sizeof json - len, "]}");
        if (k <= 0 || (size_t)k >= sizeof json - len) {
            return 1;
        }
        len += (size_t)k;
    }
    FILE* f = dsd_fopen_private(path, "wb");
    if (f == NULL) {
        return 1;
    }
    int rc = fwrite(json, 1, len, f) != len ? 1 : 0;
    rc |= fclose(f) != 0 ? 1 : 0;
    return rc;
}

/* The journal's publish record of @p ms of audio, or NULL; *@p count gets how many there are. */
static const char*
publish_of_duration(long long ms, int* count) {
    const char* found = NULL;
    *count = 0;
    const char* pub = NULL;
    for (int i = 0; (pub = record(&g_j, "publish", i)) != NULL; i++) {
        if (field_ll(pub, "duration_ms") == ms) {
            found = pub;
            (*count)++;
        }
    }
    return found;
}

/* Plays @p n records from one file, with --rdio-unknown-talkgroup 4242 and DirWatch, through playMbeFiles(), the run's
   end and the real close and export, then reloads the journal. */
static int
playback_records_run(rig* r, const char* leaf, const playback_record* recs, int n) {
    r->opts->audio_out = 0;
    r->opts->playfiles = 1;
    rdio_fallback_on(r, DSD_RDIO_MODE_DIRWATCH);
    submit(r, DSD_APP_CMD_WAV_START);
    char path[DSD_TEST_PATH_MAX * 2];
    const int rc =
        expect_int("records: input written",
                   dsd_test_path_join(path, sizeof path, r->dir, leaf) == 0 ? records_write(path, recs, n) : 1, 0);
    char* argv[2] = {"dsd-neo", path};
    r->state->optind = 1;
    playMbeFiles(r->opts, r->state, 2, argv);
    playback_end_run(r);
    reload(r);
    return rc;
}

/* The recording of @p rec's frames: published once, and exported with @p want_tg, @p want_src and @p rec's first frame's
   second in its sidecar. */
static int
expect_record_export(const rig* r, const char* label, const playback_record* rec, unsigned want_tg, unsigned want_src) {
    int rc = 0;
    char tag[200];
    int count = 0;
    const char* pub = publish_of_duration((long long)rec->frames * 20, &count);
    DSD_SNPRINTF(tag, sizeof tag, "%s: one recording of all %d frames", label, rec->frames);
    rc |= expect_int(tag, count, 1);
    static char body[4096];
    read_sidecar_of(r, pub, body, sizeof body);
    char want[96];
    DSD_SNPRINTF(want, sizeof want, "\"talkgroup\": %u,", want_tg);
    DSD_SNPRINTF(tag, sizeof tag, "%s: exported under %u", label, want_tg);
    rc |= expect_true(tag, strstr(body, want) != NULL);
    DSD_SNPRINTF(want, sizeof want, "\"srcList\": [{\"pos\":0,\"src\":%u}],", want_src);
    DSD_SNPRINTF(tag, sizeof tag, "%s: from %u", label, want_src);
    rc |= expect_true(tag, strstr(body, want) != NULL);
    DSD_SNPRINTF(want, sizeof want, "\"start_time\": %lld,", (long long)(rec->first_ms / 1000ULL));
    DSD_SNPRINTF(tag, sizeof tag, "%s: filed at the stamped record's first frame", label);
    rc |= expect_true(tag, strstr(body, want) != NULL);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "  %s sidecar: %s\n", label, body);
    }
    return rc;
}

/* A played file that is not in SDRTrunk's own shape: played, but no recording of it is published or left (no WAV, no
   temp file, no sidecar), and the run's one line said. */
static int
expect_played_not_recorded(const rig* r, const char* label) {
    char tag[200];
    DSD_SNPRINTF(tag, sizeof tag, "%s: nothing published", label);
    int rc = expect_int(tag, count_records(&g_j, "publish"), 0);
    wav_names names;
    DSD_MEMSET(&names, 0, sizeof names);
    (void)dsd_dir_list(r->dir, collect_wav_cb, &names);
    DSD_SNPRINTF(tag, sizeof tag, "%s: no WAV", label);
    rc |= expect_int(tag, names.count, 0);
    DSD_SNPRINTF(tag, sizeof tag, "%s: no temp file", label);
    rc |= expect_int(tag, names.temps, 0);
    DSD_SNPRINTF(tag, sizeof tag, "%s: said once this run", label);
    rc |= expect_int(tag, wav_recording_played_file_unrecorded_first_this_run(), 0);
    return rc;
}

/* -P -r of one record whose header also carries members the reader does not know: a name that starts with a key's name
   ("total_frames" before "to", "from_site" after "from", "protocol_name" after "protocol") is not that key, and a
   "time" that is no frame's (a record member, a member of another object, a frame of another object's "frames") does
   not stamp the call. Each file plays as one call, whose row is talkgroup 1234 from 5678, filed at its first frame's
   second. SDRTrunk writes none of these members, so the file gets no per-call recording (issue #888). Before, a key
   matched by its first letters, so "total_frames":2 named talkgroup 2 and the real one opened a second call, and any
   "time" before the frames stamped the call. */
static int
test_playback_record_reads_only_its_own_members(void) {
    static const struct {
        const char* label;
        const char* head;
    } k_cases[] = {
        {"key-prefixed members",
         "{\"protocol\":\"DMR\",\"protocol_name\":\"NXDN\",\"version\":2,\"call_type\":\"GROUP\","
         "\"from\":\"5678\",\"from_site\":\"99\",\"total_frames\":2,\"to\":\"1234\",\"encrypted\":false,"
         "\"frames\":["},
        {"metadata time before the frames",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"metadata\":{\"time\":1700000000000},\"frames\":["},
        {"record time before the frames",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"time\":1700000000000,\"frames\":["},
        {"another object's frames",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"metadata\":{\"frames\":[{\"time\":1700000000000}]},\"frames\":["},
    };

    int rc = 0;
    for (size_t c = 0; c < sizeof k_cases / sizeof k_cases[0]; c++) {
        char tag[200];
        const char* label = k_cases[c].label;
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_playback_members");
        r.opts->audio_out = 0;
        r.opts->playfiles = 1;
        rdio_fallback_on(&r, DSD_RDIO_MODE_DIRWATCH);
        submit(&r, DSD_APP_CMD_WAV_START);
        char path[DSD_TEST_PATH_MAX * 2];
        DSD_SNPRINTF(tag, sizeof tag, "%s: input written", label);
        rc |= expect_int(tag,
                         dsd_test_path_join(path, sizeof path, r.dir, "members.mbe") == 0
                             ? stamped_write_record(path, k_cases[c].head)
                             : 1,
                         0);
        char* argv[2] = {"dsd-neo", path};
        r.state->optind = 1;
        playMbeFiles(r.opts, r.state, 2, argv);
        playback_end_run(&r);
        reload(&r);
        DSD_SNPRINTF(tag, sizeof tag, "%s: one history row", label);
        rc |= expect_int(tag, (long long)r.state->event_history_s[0].push_seq, 1);
        const Event_History* row = &r.state->event_history_s[0].Event_History_Items[1];
        DSD_SNPRINTF(tag, sizeof tag, "%s: the row's talkgroup and source", label);
        rc |= expect_true(tag, row->target_id == 1234U && row->source_id == 5678U);
        DSD_SNPRINTF(tag, sizeof tag, "%s: the row is filed at the first frame", label);
        rc |= expect_int(tag, (long long)row->event_time, (long long)(k_stamped_first_ms / 1000ULL));
        rc |= expect_played_not_recorded(&r, label);
        r.opts->rdio_mode = DSD_RDIO_MODE_OFF;
        rig_close(&r);
    }
    return rc;
}

/* -P -r with --rdio-unknown-talkgroup: a file of two records is played as one stream, with one history row. The
   records' headers fold into the open call one key at a time, as on air, so the row ends with the last ID each endpoint
   was given (a 0 or a missing ID clears none), and the row is stamped from the first valid frame of the last call the
   call-state layer opened: the second record's where its source or talkgroup changed, else the first's. Such a file is
   not in SDRTrunk's own shape (one record): the second record's IDs come after the first record's frames, so it gets
   no per-call recording and no export under any number (issue #888). */
static int
test_playback_records_play_as_one_call(void) {
    static const struct {
        const char* label;
        const char* leaf;
        playback_record recs[2];
        uint32_t row_tg;  /* the row's talkgroup */
        uint32_t row_src; /* the row's source */
        int stamped;      /* the record whose first frame stamps the row */
    } k_cases[] = {
        {"two talkgroups",
         "two.mbe",
         {{1234U, 5678U, 3, 1600000100250ULL}, {4321U, 777U, 5, 1600000200750ULL}},
         4321U,
         777U,
         1},
        {"zero to after a talkgroup",
         "zero_to.mbe",
         {{1234U, 5678U, 3, 1600000100250ULL}, {0U, 777U, 4, 1600000200750ULL}},
         1234U,
         777U,
         1},
        {"identity-less first",
         "anonymous_first.mbe",
         {{0U, 0U, 3, 1600000300250ULL}, {0U, 111U, 4, 1600000400750ULL}},
         0U,
         111U,
         0},
    };

    int rc = 0;
    for (size_t c = 0; c < sizeof k_cases / sizeof k_cases[0]; c++) {
        char tag[200];
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_playback_records");
        rc |= playback_records_run(&r, k_cases[c].leaf, k_cases[c].recs, 2);
        DSD_SNPRINTF(tag, sizeof tag, "%s: one history row", k_cases[c].label);
        rc |= expect_int(tag, (long long)r.state->event_history_s[0].push_seq, 1);
        const Event_History* row = &r.state->event_history_s[0].Event_History_Items[1];
        DSD_SNPRINTF(tag, sizeof tag, "%s: the row's talkgroup", k_cases[c].label);
        rc |= expect_int(tag, (long long)row->target_id, (long long)k_cases[c].row_tg);
        DSD_SNPRINTF(tag, sizeof tag, "%s: the row's source", k_cases[c].label);
        rc |= expect_int(tag, (long long)row->source_id, (long long)k_cases[c].row_src);
        DSD_SNPRINTF(tag, sizeof tag, "%s: the row is filed at the stamped record's first frame", k_cases[c].label);
        rc |= expect_int(tag, (long long)row->event_time,
                         (long long)(k_cases[c].recs[k_cases[c].stamped].first_ms / 1000ULL));
        rc |= expect_played_not_recorded(&r, k_cases[c].label);
        rdio_fallback_off(&r);
        rig_close(&r);
    }
    return rc;
}

/* A played file whose first record is from a radio set stream=off and whose second is from one with no restriction:
   the second record's call is the last the file opened, so its row allows the export, but the file's audio holds the
   first record's too. A file of two records gets no per-call recording and no export (issue #888). Before, it was
   exported under the second radio. The unrestricted radio's record played alone is exported as before. */
static int
test_playback_stream_off_record_stays_local(void) {
    static const struct {
        const char* label;
        playback_record recs[2];
        int n;
    } k_cases[] = {
        {"stream=off record, then an unrestricted one",
         {{0U, 5678U, 3, 1600000100250ULL}, {0U, 777U, 4, 1600000200750ULL}},
         2},
        {"the unrestricted record alone", {{0U, 777U, 4, 1600000200750ULL}}, 1},
    };

    int rc = 0;
    for (size_t c = 0; c < sizeof k_cases / sizeof k_cases[0]; c++) {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_playback_stream_off");
        rc |= expect_int("stream=off radio row", dsd_test_install_radio_policy(r.state, 5678U, "A", 1, 1, 0), 0);
        rc |= playback_records_run(&r, "stream_off.mbe", k_cases[c].recs, k_cases[c].n);
        if (k_cases[c].n == 2) {
            rc |= expect_played_not_recorded(&r, k_cases[c].label);
        } else {
            const playback_record last = {0U, 0U, 4, 1600000200750ULL};
            rc |= expect_record_export(&r, k_cases[c].label, &last, 4242U, 777U);
        }
        rdio_fallback_off(&r);
        rig_close(&r);
    }
    return rc;
}

/* A played file's second call (another source) whose frames carry no "time": its row is not filed at the first call's
   stamp. The new call clears the staged stamp with the latch, so the row keeps none and falls back to the decode clock.
   Before, it took the first record's date. */
static int
test_playback_new_call_without_time_takes_the_decode_clock(void) {
    static const playback_record k_recs[2] = {{0U, 5678U, 3, 1600000100250ULL}, {0U, 777U, 4, 0U}};
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_playback_unstamped");
    rc |= playback_records_run(&r, "unstamped.mbe", k_recs, 2);
    rc |= expect_int("unstamped second call: one history row", (long long)r.state->event_history_s[0].push_seq, 1);
    const long long start = (long long)r.state->event_history_s[0].Event_History_Items[1].event_time;
    rc |= expect_true("unstamped second call: no stamp of the first call's, so filed on the decode clock",
                      start == 0 || (start >= (long long)k_t0 && start <= (long long)k_t0 + 60));
    if (rc != 0) {
        DSD_FPRINTF(stderr, "  unstamped second call: start %lld\n", start);
    }
    rig_close(&r);
    return rc;
}

/* Writes @p count stamped frames of the clear DMR call to @p f, the first numbered @p first (for its "time"), each but
   the file's first after a comma. */
static int
long_write_frames(FILE* f, int first, int count) {
    int rc = 0;
    for (int i = first; i < first + count; i++) {
        rc |= DSD_FPRINTF(f, "%s{\"time\":%llu,\"hex\":\"%s\"}", i > 0 ? "," : "",
                          (unsigned long long)(k_stamped_first_ms + (uint64_t)i * 20ULL), k_playback_dmr_hex)
                      < 0
                  ? 1
                  : 0;
    }
    return rc;
}

/* An SDRTrunk export of one clear DMR group call to 1234 from 5678, STAMPED_FRAMES stamped frames, with @p pad bytes
   of white space between its first @p head frames and the rest (as a long call's text grows past any one read). With
   @p sparse_to nonzero, the file is then extended to that many bytes with a hole after the record. */
static int
long_write_export(const char* path, int head, size_t pad, long sparse_to) {
    FILE* f = dsd_fopen_private(path, "wb");
    if (f == NULL) {
        return 1;
    }
    int rc = fputs("{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
                   "\"encrypted\":false,\"frames\":[",
                   f) < 0
                 ? 1
                 : 0;
    rc |= long_write_frames(f, 0, head);
    static char spaces[4096];
    DSD_MEMSET(spaces, ' ', sizeof spaces);
    for (size_t done = 0; done < pad; done += sizeof spaces) {
        const size_t n = pad - done < sizeof spaces ? pad - done : sizeof spaces;
        rc |= fwrite(spaces, 1, n, f) != n ? 1 : 0;
    }
    rc |= long_write_frames(f, head, STAMPED_FRAMES - head);
    rc |= fputs("]}", f) < 0 ? 1 : 0;
    if (sparse_to > 0) {
        rc |= fseek(f, sparse_to - 1L, SEEK_SET) != 0 || fputc('\n', f) == EOF ? 1 : 0;
    }
    rc |= fclose(f) != 0 ? 1 : 0;
    return rc;
}

/* Issue #888: -P -r of one SDRTrunk export longer than the reader's old 1 MiB buffer is one call, played once, whole.
   Before, the reader filled the buffer, the file was not at EOF, and playMbeFiles() called the reader again, which
   rewound and replayed the same first MiB over and over: one export after another of the first frames, the rest of the
   file and the files after it never reached. An export over the reader's cap (DSD_SDRTRUNK_JSON_MAX_BYTES) is refused
   with one log line before anything of it plays or is recorded. */
static int
test_playback_long_export_plays_once(void) {
    int rc = 0;
    {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_playback_long");
        r.opts->audio_out = 0;
        r.opts->playfiles = 1;
        rdio_fallback_on(&r, DSD_RDIO_MODE_DIRWATCH);
        submit(&r, DSD_APP_CMD_WAV_START);
        char path[DSD_TEST_PATH_MAX * 2];
        rc |= expect_int("long export: input written",
                         dsd_test_path_join(path, sizeof path, r.dir, "long.mbe") == 0
                             ? long_write_export(path, STAMPED_FRAMES / 2, (size_t)0x100000 + 4096U, 0L)
                             : 1,
                         0);
        char* argv[2] = {"dsd-neo", path};
        r.state->optind = 1;
        playMbeFiles(r.opts, r.state, 2, argv);
        playback_end_run(&r);
        reload(&r);
        rc |= expect_int("long export: one recording", count_records(&g_j, "publish"), 1);
        rc |= expect_int("long export: one history row", (long long)r.state->event_history_s[0].push_seq, 1);
        const playback_record rec = {1234U, 5678U, STAMPED_FRAMES, k_stamped_first_ms};
        rc |= expect_record_export(&r, "long export", &rec, 1234U, 5678U);
        rdio_fallback_off(&r);
        rig_close(&r);
    }
    {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_playback_over_cap");
        r.opts->audio_out = 0;
        r.opts->playfiles = 1;
        rdio_fallback_on(&r, DSD_RDIO_MODE_DIRWATCH);
        submit(&r, DSD_APP_CMD_WAV_START);
        char path[DSD_TEST_PATH_MAX * 2];
        rc |= expect_int("over-cap export: input written",
                         dsd_test_path_join(path, sizeof path, r.dir, "over_cap.mbe") == 0
                             ? long_write_export(path, STAMPED_FRAMES, 0U, (long)DSD_SDRTRUNK_JSON_MAX_BYTES + 1L)
                             : 1,
                         0);
        char* argv[2] = {"dsd-neo", path};
        r.state->optind = 1;
        dsd_neo_log_set_tap(count_log_lines, NULL);
        g_sdrtrunk_refusals = 0;
        playMbeFiles(r.opts, r.state, 2, argv);
        playback_end_run(&r);
        reload(&r);
        rc |= expect_int("over-cap export: refused with one log line", g_sdrtrunk_refusals, 1);
        rc |= expect_int("over-cap export: nothing recorded", count_records(&g_j, "publish"), 0);
        rc |= expect_int("over-cap export: no history row", (long long)r.state->event_history_s[0].push_seq, 0);
        rdio_fallback_off(&r);
        rig_close(&r);
    }
    return rc;
}

/* Plays the SDRTrunk export @p json from one file, as playback_records_run() does. */
static int
playback_json_run(rig* r, const char* leaf, const char* json) {
    r->opts->audio_out = 0;
    r->opts->playfiles = 1;
    rdio_fallback_on(r, DSD_RDIO_MODE_DIRWATCH);
    submit(r, DSD_APP_CMD_WAV_START);
    char path[DSD_TEST_PATH_MAX * 2];
    int rc = dsd_test_path_join(path, sizeof path, r->dir, leaf) != 0 ? 1 : 0;
    FILE* f = rc == 0 ? dsd_fopen_private(path, "wb") : NULL;
    if (f != NULL) {
        rc |= fputs(json, f) < 0 ? 1 : 0;
        rc |= fclose(f) != 0 ? 1 : 0;
    } else {
        rc = 1;
    }
    rc = expect_int("json: input written", rc, 0);
    char* argv[2] = {"dsd-neo", path};
    r->state->optind = 1;
    playMbeFiles(r->opts, r->state, 2, argv);
    playback_end_run(r);
    reload(r);
    return rc;
}

#define K_GOOD_FRAME "{\"hex\":\"3ABF032D9ADEE4CB50\"}"
#define K_DMR_HEAD                                                                                                     \
    "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","                   \
    "\"encrypted\":false,\"frames\":["

/* Issue #888: a voice frame whose "hex" is not one SDRTrunk writes (18 hex digits, 36 for P25 Phase 1: empty, short,
   long, or with a character that is no hex digit) is passed over, so it plays no audio and records none. A frame of
   hex digits of the wrong length leaves the file in SDRTrunk's shape, so its good frames alone are recorded; an empty
   frame, or one with a character that is no hex digit, is not in that shape, so the file is played with no per-call
   recording. Before, the decoders read 18 or 36 characters whatever the value's length: an empty final frame played
   a frame of audio from the text after it, and a file of nothing else was exported as a call. */
static int
test_playback_malformed_frames_play_nothing(void) {
    static const struct {
        const char* label;
        const char* json;
        int frames; /* the good frames: the recording's length; 0 for none */
    } k_cases[] = {
        {"short and long frames",
         K_DMR_HEAD K_GOOD_FRAME ",{\"hex\":\"3ABF032D9ADEE4CB5\"}," K_GOOD_FRAME
                                 ",{\"hex\":\"3ABF032D9ADEE4CB5000\"}," K_GOOD_FRAME "]}",
         3},
        {"an empty final frame", K_DMR_HEAD K_GOOD_FRAME "," K_GOOD_FRAME "," K_GOOD_FRAME ",{\"hex\":\"\"}]}", 0},
        {"a frame with a character that is no hex digit",
         K_DMR_HEAD K_GOOD_FRAME ",{\"hex\":\"3ABF032D9ADEE4CB5Z\"}," K_GOOD_FRAME "]}", 0},
        {"only an empty frame",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"0\","
         "\"encrypted\":false,\"frames\":[{\"hex\":\"\"}]}",
         0},
        {"P25 Phase 1 frames of 18 digits",
         "{\"protocol\":\"APCO25-PHASE1\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"frames\":[" K_GOOD_FRAME "," K_GOOD_FRAME "]}",
         0},
    };

    int rc = 0;
    for (size_t c = 0; c < sizeof k_cases / sizeof k_cases[0]; c++) {
        char tag[200];
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_playback_malformed");
        rc |= playback_json_run(&r, "malformed.mbe", k_cases[c].json);
        const int want = k_cases[c].frames;
        DSD_SNPRINTF(tag, sizeof tag, "%s: %s", k_cases[c].label, want ? "one recording" : "nothing recorded");
        rc |= expect_int(tag, count_records(&g_j, "publish"), want ? 1 : 0);
        if (want) {
            int count = 0;
            (void)publish_of_duration((long long)want * 20, &count);
            DSD_SNPRINTF(tag, sizeof tag, "%s: of its %d good frames alone", k_cases[c].label, want);
            rc |= expect_int(tag, count, 1);
        }
        if (rc != 0) {
            const char* pub = record(&g_j, "publish", 0);
            DSD_FPRINTF(stderr, "  %s: first recording %lld ms\n", k_cases[c].label,
                        pub ? field_ll(pub, "duration_ms") : -1LL);
        }
        rdio_fallback_off(&r);
        rig_close(&r);
    }
    return rc;
}

/* Writes @p json to @p leaf in the rig's folder, its path in @p path. */
static int
json_write_leaf(const rig* r, const char* leaf, const char* json, char* path, size_t path_size) {
    if (dsd_test_path_join(path, path_size, r->dir, leaf) != 0) {
        return 1;
    }
    FILE* f = dsd_fopen_private(path, "wb");
    if (f == NULL) {
        return 1;
    }
    int rc = fputs(json, f) < 0 ? 1 : 0;
    rc |= fclose(f) != 0 ? 1 : 0;
    return rc;
}

/* A talkgroup row for @p tg with @p record and @p stream, audio on. */
static int
install_tg_row(rig* r, uint32_t tg, int record, int stream) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_exact_entry(tg, "A", "", DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return -1;
    }
    row.audio = 1U;
    row.record = record ? 1U : 0U;
    row.stream = stream ? 1U : 0U;
    return dsd_tg_policy_append_exact(r->state, &row);
}

#define K_THREE_FRAMES  K_GOOD_FRAME "," K_GOOD_FRAME "," K_GOOD_FRAME
#define K_FOUR_FRAMES   K_THREE_FRAMES "," K_GOOD_FRAME
#define K_SHORT_FRAME   "{\"hex\":\"3ABF032D9ADEE4CB5\"}"
#define K_STOPPED_AFTER K_THREE_FRAMES "," K_SHORT_FRAME "," K_GOOD_FRAME

/* What a played file's case expects: the recording of @p frames frames (0: none, and no WAV at all), exported under
   @p tg from @p src (tg 0: no sidecar, no upload; src 0: not checked). No recording is left under its temp name. */
static int
expect_played_file(const rig* r, const char* label, int frames, unsigned tg, unsigned src) {
    char tag[200];
    int rc = 0;
    DSD_SNPRINTF(tag, sizeof tag, "%s: %s", label, frames ? "one recording" : "nothing published");
    rc |= expect_int(tag, count_records(&g_j, "publish"), frames ? 1 : 0);
    wav_names names;
    DSD_MEMSET(&names, 0, sizeof names);
    (void)dsd_dir_list(r->dir, collect_wav_cb, &names);
    DSD_SNPRINTF(tag, sizeof tag, "%s: %s", label, frames ? "one WAV" : "no WAV");
    rc |= expect_int(tag, names.count, frames ? 1 : 0);
    DSD_SNPRINTF(tag, sizeof tag, "%s: no temp file", label);
    rc |= expect_int(tag, names.temps, 0);
    if (!frames) {
        return rc;
    }
    int count = 0;
    const char* pub = publish_of_duration((long long)frames * 20, &count);
    DSD_SNPRINTF(tag, sizeof tag, "%s: of all %d frames", label, frames);
    rc |= expect_int(tag, count, 1);
    static char body[4096];
    read_sidecar_of(r, pub, body, sizeof body);
    if (tg == 0U) {
        DSD_SNPRINTF(tag, sizeof tag, "%s: kept local, no sidecar", label);
        rc |= expect_str(tag, body, "");
        DSD_SNPRINTF(tag, sizeof tag, "%s: no upload", label);
        rc |= expect_str(tag, pub ? field_s(pub, "rdio") : "", "false");
    } else {
        char want[96];
        DSD_SNPRINTF(want, sizeof want, "\"talkgroup\": %u,", tg);
        DSD_SNPRINTF(tag, sizeof tag, "%s: exported under %u", label, tg);
        rc |= expect_true(tag, strstr(body, want) != NULL);
        if (src != 0U) {
            DSD_SNPRINTF(want, sizeof want, "\"srcList\": [{\"pos\":0,\"src\":%u}],", src);
            DSD_SNPRINTF(tag, sizeof tag, "%s: from %u", label, src);
            rc |= expect_true(tag, strstr(body, want) != NULL);
        }
    }
    if (rc != 0) {
        DSD_FPRINTF(stderr, "  %s sidecar: %s\n", label, body);
    }
    return rc;
}

/* The policy rows a played file's case installs. */
typedef struct {
    int radio;     /* radio 5678's row: 0 none, 1 record=off, 2 stream=off */
    int tg_row;    /* a talkgroup row ... */
    uint32_t tg;   /* ... for this talkgroup (0 included) ... */
    int tg_record; /* ... with these record and stream settings */
    int tg_stream;
} played_policy;

static int
install_played_policy(rig* r, const played_policy* policy) {
    int rc = 0;
    if (policy->radio != 0) {
        rc |= expect_int("radio row",
                         dsd_test_install_radio_policy(r->state, 5678U, "A", 1, policy->radio == 1 ? 0 : 1,
                                                       policy->radio == 2 ? 0 : 1),
                         0);
    }
    if (policy->tg_row) {
        rc |= expect_int("talkgroup row", install_tg_row(r, policy->tg, policy->tg_record, policy->tg_stream), 0);
    }
    return rc;
}

/* Issue #888 (fail closed): a played SDRTrunk file is checked, before it plays, against the shape SDRTrunk's writer
   gives a call (sdrtrunk_json_shape(): one record, only its known members, in its order, "frames" last, each value of
   its type, every frame with its hex). A file in that shape is recorded and exported as its policies allow, Stop
   during it included, since its call is known before any of its audio. Any other file is played, but none of its audio
   reaches a per-call recording and nothing is exported (one line a run). An SDRTrunk patch group's "to"
   ("P:<group>", with its patched talkgroups after it) is that talkgroup. A "to" or "from" string the reader cannot
   read (an SDRTrunk roaming or ISSI identity, "Q:77") leaves the call recorded but gives it no fallback talkgroup: with
   no talkgroup it is not exported, with one it is exported under it. An ID is read by its value, leading zeros and
   all. Before, the file's shape was judged by where its IDs fell, so a nested object's frames, an escaped key, an
   unclosed record, an outer array, a "to" array, a frame with no hex or a late "encrypted" exported audio its policies
   never judged (under the fallback talkgroup 4242), and a padded source lost its radio's stream=off. */
static int
test_playback_file_shape_decides_its_recording(void) {
    static const struct {
        const char* label;
        const char* json;
        played_policy policy;
        int stop; /* Stop at the file's short frame */
        int frames;
        unsigned tg; /* exported under (0: not exported) */
        unsigned src;
    } k_cases[] = {
        {"SDRTrunk's order", K_DMR_HEAD K_THREE_FRAMES "]}", {0}, 0, 3, 1234U, 5678U},
        {"Stop in SDRTrunk's order", K_DMR_HEAD K_STOPPED_AFTER "]}", {0}, 1, 3, 1234U, 5678U},
        {"from after the frames, its radio record=off",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"to\":\"0\",\"encrypted\":false,"
         "\"frames\":[" K_THREE_FRAMES "],\"from\":\"5678\"}",
         {1, 0, 0U, 0, 0},
         0,
         0,
         0U,
         0U},
        {"from after the frames, record and stream on",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"to\":\"0\",\"encrypted\":false,"
         "\"frames\":[" K_THREE_FRAMES "],\"from\":\"5678\"}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a stream=off talkgroup 0 record, then a private one from the same radio",
         "{\"protocol\":\"DMR\",\"version\":2,\"from\":\"5678\",\"to\":\"0\",\"encrypted\":false,"
         "\"frames\":[" K_THREE_FRAMES "]}\n"
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"PRIVATE\",\"from\":\"5678\",\"to\":\"0\","
         "\"encrypted\":false,\"frames\":[" K_FOUR_FRAMES "]}",
         {0, 1, 0U, 1, 0},
         0,
         0,
         0U,
         0U},
        {"a stream=off talkgroup 0 record alone",
         "{\"protocol\":\"DMR\",\"version\":2,\"from\":\"5678\",\"to\":\"0\",\"encrypted\":false,"
         "\"frames\":[" K_THREE_FRAMES "]}",
         {0, 1, 0U, 1, 0},
         0,
         3,
         0U,
         0U},
        {"Stop before a to after the frames",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"encrypted\":false,"
         "\"frames\":[" K_STOPPED_AFTER "],\"to\":\"99\"}",
         {0, 1, 99U, 0, 0},
         1,
         0,
         0U,
         0U},
        {"a second record of frames alone",
         K_DMR_HEAD K_THREE_FRAMES "]}\n{\"frames\":[" K_FOUR_FRAMES "]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"frames before the protocol, then the same call",
         "{\"frames\":[" K_THREE_FRAMES "],\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\","
         "\"from\":\"5678\",\"to\":\"1234\"}\n" K_DMR_HEAD K_FOUR_FRAMES "]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a metadata from after the frames",
         K_DMR_HEAD K_THREE_FRAMES "],\"metadata\":{\"from\":\"9999\"}}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a nested object with frames",
         K_DMR_HEAD K_THREE_FRAMES "],\"metadata\":{\"from\":\"9999\",\"frames\":[" K_THREE_FRAMES "]}}",
         {0},
         0,
         0,
         0U,
         0U},
        {"an escaped key",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"encrypted\":false,"
         "\"frames\":[" K_THREE_FRAMES "],\"\\u0074o\":\"1234\"}",
         {0, 1, 1234U, 1, 0},
         0,
         0,
         0U,
         0U},
        {"an unclosed record",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"encrypted\":false,"
         "\"frames\":[" K_THREE_FRAMES "]",
         {0},
         0,
         0,
         0U,
         0U},
        {"an outer array", "[" K_DMR_HEAD K_THREE_FRAMES "]},[" K_DMR_HEAD K_FOUR_FRAMES "]}]]", {0}, 0, 0, 0U, 0U},
        {"a to that is an array",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":[],"
         "\"encrypted\":false,\"frames\":[" K_THREE_FRAMES "]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a frame without hex", K_DMR_HEAD K_THREE_FRAMES ",{\"time\":1700000000000}]}", {0}, 0, 0, 0U, 0U},
        {"a late record-level encrypted",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"frames\":[" K_THREE_FRAMES "],\"encrypted\":true}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a zero-padded source whose radio has stream=off",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"00000005678\",\"to\":\"1234\","
         "\"encrypted\":false,\"frames\":[" K_THREE_FRAMES "]}",
         {2, 0, 0U, 0, 0},
         0,
         3,
         0U,
         0U},
        {"an escaped source whose radio has stream=off",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"56\\u00378\",\"to\":\"1234\","
         "\"encrypted\":false,\"frames\":[" K_THREE_FRAMES "]}",
         {2, 0, 0U, 0, 0},
         0,
         0,
         0U,
         0U},
        {"an escaped talkgroup with stream=off",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"12\\u00334\","
         "\"encrypted\":false,\"frames\":[" K_THREE_FRAMES "]}",
         {0, 1, 1234U, 1, 0},
         0,
         0,
         0U,
         0U},
        {"a system with an escape JSON has not",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"system\":\"A\\q\",\"frames\":[" K_THREE_FRAMES "]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a site with a short unicode escape",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"site\":\"S\\u00e\",\"frames\":[" K_THREE_FRAMES "]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a system that is not UTF-8",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"system\":\"Caf\xE9\",\"frames\":[" K_THREE_FRAMES "]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a system and site with every JSON escape and UTF-8",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"system\":\"\\\"A\\\" \\\\ \\/ \\b\\f\\n\\r\\t "
         "\\u00e9\\u00C9\",\"site\":\"Caf\xC3\xA9\","
         "\"frames\":[" K_THREE_FRAMES "]}",
         {0},
         0,
         3,
         1234U,
         5678U},
        {"a version, then a site after the frames",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"system\":\"North\",\"frames\":[" K_THREE_FRAMES "],\"site\":\"Site 1\"}",
         {0},
         0,
         0,
         0U,
         0U},
        {"no version, and a system before the frames",
         "{\"protocol\":\"DMR\",\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\",\"encrypted\":false,"
         "\"system\":\"North\",\"frames\":[" K_THREE_FRAMES "]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a version with a leading zero",
         "{\"protocol\":\"DMR\",\"version\":02,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"frames\":[" K_THREE_FRAMES "]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a version past a Java Integer",
         "{\"protocol\":\"DMR\",\"version\":2147483648,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"frames\":[" K_THREE_FRAMES "]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a time past a Java long",
         K_DMR_HEAD K_GOOD_FRAME "," K_GOOD_FRAME ",{\"time\":9223372036854775808,\"hex\":\"3ABF032D9ADEE4CB50\"}]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a negative version",
         "{\"protocol\":\"DMR\",\"version\":-2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"frames\":[" K_THREE_FRAMES "]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a negative time",
         K_DMR_HEAD K_GOOD_FRAME "," K_GOOD_FRAME ",{\"time\":-5,\"hex\":\"3ABF032D9ADEE4CB50\"}]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a negative key id",
         K_DMR_HEAD K_GOOD_FRAME "," K_GOOD_FRAME ",{\"encryption_key_id\":-1,\"hex\":\"3ABF032D9ADEE4CB50\"}]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a key id of minus zero",
         K_DMR_HEAD K_GOOD_FRAME "," K_GOOD_FRAME ",{\"encryption_key_id\":-0,\"hex\":\"3ABF032D9ADEE4CB50\"}]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a version past what the reader takes",
         "{\"protocol\":\"DMR\",\"version\":65536,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"frames\":[" K_THREE_FRAMES "]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"the most version the reader takes",
         "{\"protocol\":\"DMR\",\"version\":65535,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"frames\":[" K_THREE_FRAMES "]}",
         {0},
         0,
         3,
         1234U,
         5678U},
        {"an algorithm past 8 bits",
         K_DMR_HEAD K_GOOD_FRAME "," K_GOOD_FRAME ",{\"encryption_algorithm\":256,\"hex\":\"3ABF032D9ADEE4CB50\"}]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a key id past 16 bits",
         K_DMR_HEAD K_GOOD_FRAME "," K_GOOD_FRAME ",{\"encryption_key_id\":65536,\"hex\":\"3ABF032D9ADEE4CB50\"}]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"no version, a tagged frame, the site after the frames",
         "{\"protocol\":\"DMR\",\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\",\"encrypted\":false,"
         "\"frames\":[" K_GOOD_FRAME "," K_GOOD_FRAME ",{\"tag\":\"UNKNOWN\",\"hex\":\"3ABF032D9ADEE4CB50\"}],"
         "\"site\":\"Site 1\"}",
         {0},
         0,
         0,
         0U,
         0U},
        {"no version and a tagged frame",
         "{\"protocol\":\"DMR\",\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\",\"encrypted\":false,"
         "\"frames\":[" K_GOOD_FRAME "," K_GOOD_FRAME ",{\"tag\":\"UNKNOWN\",\"hex\":\"3ABF032D9ADEE4CB50\"}]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a SACCH tag with an escaped place",
         "{\"protocol\":\"NXDN\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"frames\":[" K_GOOD_FRAME "," K_GOOD_FRAME
         ",{\"tag\":\"SACCH \\u0031\",\"hex\":\"3ABF032D9ADEE4CB50\"}]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a SACCH tag with an escaped tab before its place",
         "{\"protocol\":\"NXDN\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"frames\":[" K_GOOD_FRAME "," K_GOOD_FRAME
         ",{\"tag\":\"SACCH \\t1\",\"hex\":\"3ABF032D9ADEE4CB50\"}]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a SACCH tag with an escaped name",
         "{\"protocol\":\"NXDN\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"1234\","
         "\"encrypted\":false,\"frames\":[" K_GOOD_FRAME "," K_GOOD_FRAME
         ",{\"tag\":\"\\u0053ACCH 1\",\"hex\":\"3ABF032D9ADEE4CB50\"}]}",
         {0},
         0,
         0,
         0U,
         0U},
        {"a patch group whose talkgroup has stream=off",
         "{\"protocol\":\"APCO25-PHASE2\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"P:1234\","
         "\"encrypted\":false,\"frames\":[" K_THREE_FRAMES "]}",
         {0, 1, 1234U, 1, 0},
         0,
         3,
         0U,
         0U},
        {"a patch group with its patched talkgroups",
         "{\"protocol\":\"APCO25-PHASE2\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\","
         "\"to\":\"P:1234 [5001, 5002]\",\"encrypted\":false,\"frames\":[" K_THREE_FRAMES "]}",
         {0},
         0,
         3,
         1234U,
         5678U},
        {"a to the reader cannot read",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"Q:77\","
         "\"encrypted\":false,\"frames\":[" K_THREE_FRAMES "]}",
         {0},
         0,
         3,
         0U,
         0U},
        {"a roaming source to an ISSI talkgroup",
         "{\"protocol\":\"APCO25-PHASE2\",\"version\":2,\"call_type\":\"GROUP\","
         "\"from\":\"ROAM 5678(BEE01.2A3.5678)\",\"to\":\"ISSI BEE01.2A3.1234\",\"encrypted\":false,"
         "\"frames\":[" K_THREE_FRAMES "]}",
         {0},
         0,
         3,
         0U,
         0U},
        {"a roaming source to talkgroup 1234",
         "{\"protocol\":\"APCO25-PHASE2\",\"version\":2,\"call_type\":\"GROUP\","
         "\"from\":\"ROAM 5678(BEE01.2A3.5678)\",\"to\":\"1234\",\"encrypted\":false,"
         "\"frames\":[" K_THREE_FRAMES "]}",
         {0},
         0,
         3,
         1234U,
         0U},
    };

    int rc = 0;
    dsd_neo_log_set_tap(count_log_lines, NULL);
    for (size_t c = 0; c < sizeof k_cases / sizeof k_cases[0]; c++) {
        char tag[200];
        const char* label = k_cases[c].label;
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_playback_shape");
        rc |= install_played_policy(&r, &k_cases[c].policy);
        g_stop_at_malformed_frame = k_cases[c].stop;
        rc |= playback_json_run(&r, "shape.mbe", k_cases[c].json);
        g_stop_at_malformed_frame = 0;
        dsd_exitflag_store(0);
        rc |= expect_played_file(&r, label, k_cases[c].frames, k_cases[c].tg, k_cases[c].src);
        DSD_SNPRINTF(tag, sizeof tag, "%s: %s", label, k_cases[c].frames ? "nothing said" : "said once this run");
        rc |= expect_int(tag, wav_recording_played_file_unrecorded_first_this_run(), k_cases[c].frames ? 1 : 0);
        rdio_fallback_off(&r);
        rig_close(&r);
    }
    dsd_neo_log_set_tap(NULL, NULL);
    return rc;
}

/* -r unread.mbe next.mbe: a roaming source to an ISSI talkgroup, then a call to 1234. At the file boundary the event
   layer drops the first call's row, which names nothing, and rotates its recording unexported: it is kept local, and
   the run says so once, as an early close of it does. Before, that close said nothing. */
static int
playback_unread_ids_then_a_call(void) {
    static const char k_unread[] = "{\"protocol\":\"APCO25-PHASE2\",\"version\":2,\"call_type\":\"GROUP\","
                                   "\"from\":\"ROAM 5678(BEE01.2A3.5678)\",\"to\":\"ISSI BEE01.2A3.1234\","
                                   "\"encrypted\":false,\"frames\":[" K_THREE_FRAMES "]}";
    static const char k_next[] = K_DMR_HEAD K_FOUR_FRAMES "]}";
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_playback_unread_then_call");
    r.opts->audio_out = 0;
    r.opts->playfiles = 1;
    rdio_fallback_on(&r, DSD_RDIO_MODE_DIRWATCH);
    submit(&r, DSD_APP_CMD_WAV_START);
    char unread[DSD_TEST_PATH_MAX * 2];
    char next[DSD_TEST_PATH_MAX * 2];
    rc |= expect_int("unread then a call: inputs written",
                     json_write_leaf(&r, "unread.mbe", k_unread, unread, sizeof unread)
                         | json_write_leaf(&r, "next.mbe", k_next, next, sizeof next),
                     0);
    char* argv[3] = {"dsd-neo", unread, next};
    r.state->optind = 1;
    playMbeFiles(r.opts, r.state, 3, argv);
    rc |= expect_int("unread then a call: said once this run at the file boundary",
                     wav_recording_fallback_blocked_first_this_run(), 0);
    playback_end_run(&r);
    reload(&r);
    int count = 0;
    const char* pub = publish_of_duration(3LL * 20, &count);
    rc |= expect_int("unread then a call: the unread call recorded once", count, 1);
    rc |= expect_str("unread then a call: the unread call not uploaded", pub ? field_s(pub, "rdio") : "", "false");
    static char body[4096];
    read_sidecar_of(&r, pub, body, sizeof body);
    rc |= expect_str("unread then a call: the unread call kept local, no sidecar", body, "");
    read_sidecar_of(&r, publish_of_duration(4LL * 20, &count), body, sizeof body);
    rc |= expect_int("unread then a call: the next call recorded once", count, 1);
    rc |= expect_true("unread then a call: the next call exported under 1234",
                      strstr(body, "\"talkgroup\": 1234,") != NULL);
    rdio_fallback_off(&r);
    rig_close(&r);
    return rc;
}

/* Issue #888: a played call whose IDs the reader cannot read, with no talkgroup, is recorded but stays local, and the
   run says so once, whether or not its close could give it the fallback talkgroup: also for a roaming source to an
   ISSI talkgroup, which leaves the row no ID and no text, so the last file's early close at the run's end may not.
   Before, that call stayed local with nothing said. */
static int
test_playback_unread_ids_say_they_stay_local(void) {
    static const struct {
        const char* label;
        const char* json;
    } k_cases[] = {
        {"a to the reader cannot read",
         "{\"protocol\":\"DMR\",\"version\":2,\"call_type\":\"GROUP\",\"from\":\"5678\",\"to\":\"Q:77\","
         "\"encrypted\":false,\"frames\":[" K_THREE_FRAMES "]}"},
        {"a roaming source to an ISSI talkgroup",
         "{\"protocol\":\"APCO25-PHASE2\",\"version\":2,\"call_type\":\"GROUP\","
         "\"from\":\"ROAM 5678(BEE01.2A3.5678)\",\"to\":\"ISSI BEE01.2A3.1234\",\"encrypted\":false,"
         "\"frames\":[" K_THREE_FRAMES "]}"},
    };

    int rc = 0;
    for (size_t c = 0; c < sizeof k_cases / sizeof k_cases[0]; c++) {
        char tag[200];
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_playback_unread_ids");
        rc |= playback_json_run(&r, "unread.mbe", k_cases[c].json);
        rc |= expect_played_file(&r, k_cases[c].label, 3, 0U, 0U);
        DSD_SNPRINTF(tag, sizeof tag, "%s: said once this run", k_cases[c].label);
        rc |= expect_int(tag, wav_recording_fallback_blocked_first_this_run(), 0);
        rdio_fallback_off(&r);
        rig_close(&r);
    }
    rc |= playback_unread_ids_then_a_call();
    return rc;
}

/* Appends @p line and then @p eol to the @p *len bytes of @p out; 1 when they do not fit in @p cap. */
static int
append_line(char* out, size_t cap, size_t* len, const char* line, const char* eol) {
    const int n = DSD_SNPRINTF(out + *len, cap - *len, "%s%s", line, eol);
    if (n <= 0 || (size_t)n >= cap - *len) {
        return 1;
    }
    *len += (size_t)n;
    return 0;
}

/* One call as SDRTrunk's writer writes it (Jackson's default pretty printer over an MBECallSequence: two spaces a
   level, " : " after a name, "} ]" and "}, {" around frames, @p eol ending each line): a group call to 1234 from 5678,
   @p frames frames of @p hex, the first stamped 1700000000000, each 20 ms on. With @p old_writer, the shape before
   2025-11: no "version", and "system" and "site" after "frames". @p tag (NULL: none) is each frame's "tag", as the NXDN
   writer puts its SACCH place. The system's name holds escaped quotes, as a value may. */
static int
sdrtrunk_written_call(char* out, size_t cap, const char* protocol, const char* hex, const char* tag, int frames,
                      int old_writer, const char* eol) {
    char line[160];
    size_t len = 0;
    int rc = append_line(out, cap, &len, "{", eol);
    DSD_SNPRINTF(line, sizeof line, "  \"protocol\" : \"%s\",", protocol);
    rc |= append_line(out, cap, &len, line, eol);
    if (!old_writer) {
        rc |= append_line(out, cap, &len, "  \"version\" : 2,", eol);
    }
    rc |= append_line(out, cap, &len, "  \"call_type\" : \"GROUP\",", eol);
    rc |= append_line(out, cap, &len, "  \"from\" : \"5678\",", eol);
    rc |= append_line(out, cap, &len, "  \"to\" : \"1234\",", eol);
    rc |= append_line(out, cap, &len, "  \"encrypted\" : false,", eol);
    if (!old_writer) {
        rc |= append_line(out, cap, &len, "  \"system\" : \"Example \\\"North\\\" System\",", eol);
        rc |= append_line(out, cap, &len, "  \"site\" : \"Site 1\",", eol);
    }
    rc |= append_line(out, cap, &len, "  \"frames\" : [ {", eol);
    for (int i = 0; i < frames; i++) {
        if (tag != NULL) {
            DSD_SNPRINTF(line, sizeof line, "    \"tag\" : \"%s\",", tag);
            rc |= append_line(out, cap, &len, line, eol);
        }
        DSD_SNPRINTF(line, sizeof line, "    \"time\" : %llu,", 1700000000000ULL + (unsigned long long)i * 20ULL);
        rc |= append_line(out, cap, &len, line, eol);
        DSD_SNPRINTF(line, sizeof line, "    \"hex\" : \"%s\"", hex);
        rc |= append_line(out, cap, &len, line, eol);
        const char* frames_end = old_writer ? "  } ]," : "  } ]";
        rc |= append_line(out, cap, &len, i + 1 < frames ? "  }, {" : frames_end, eol);
    }
    if (old_writer) {
        rc |= append_line(out, cap, &len, "  \"system\" : \"Example System\",", eol);
        rc |= append_line(out, cap, &len, "  \"site\" : \"Site 1\"", eol);
    }
    rc |= append_line(out, cap, &len, "}", "");
    return rc;
}

/* Issue #888: a call exactly as each of SDRTrunk's writers writes it (DMR, P25 Phase 1 and 2, NXDN with its SACCH
   tags; the writer before 2025-11, with no "version" and its "system" and "site" after "frames"; Windows line ends) is
   in SDRTrunk's shape: recorded whole and exported under its talkgroup, from its radio. */
static int
test_playback_sdrtrunk_written_files_record(void) {
    static const char k_imbe_hex[] = "0123456789ABCDEF0123456789ABCDEF0123";

    static const struct {
        const char* label;
        const char* protocol;
        const char* hex;
        const char* tag;
        int old_writer;
        const char* eol;
    } k_cases[] = {
        {"DMR", "DMR", "3ABF032D9ADEE4CB50", NULL, 0, "\n"},
        {"P25 Phase 1", "APCO25-PHASE1", k_imbe_hex, NULL, 0, "\n"},
        {"P25 Phase 2", "APCO25-PHASE2", "3ABF032D9ADEE4CB50", NULL, 0, "\n"},
        {"NXDN", "NXDN", "3ABF032D9ADEE4CB50", "SACCH 1", 0, "\n"},
        {"NXDN with an escaped free-text tag", "NXDN", "3ABF032D9ADEE4CB50", "UN\\u004bNOWN", 0, "\n"},
        {"NXDN with an escaped SACCH tag of no place", "NXDN", "3ABF032D9ADEE4CB50", "SACCH UN\\u004bNOWN", 0, "\n"},
        {"NXDN with an escaped SACCH tag past the places", "NXDN", "3ABF032D9ADEE4CB50", "SACCH \\u0035", 0, "\n"},
        {"NXDN with escaped quotes after SACCH", "NXDN", "3ABF032D9ADEE4CB50", "SACCH \\\"1\\\"", 0, "\n"},
        {"DMR with an escaped SACCH tag", "DMR", "3ABF032D9ADEE4CB50", "SACCH \\u0031", 0, "\n"},
        {"the writer before 2025-11", "APCO25-PHASE1", k_imbe_hex, NULL, 1, "\n"},
        {"Windows line ends", "DMR", "3ABF032D9ADEE4CB50", NULL, 0, "\r\n"},
    };

    int rc = 0;
    for (size_t c = 0; c < sizeof k_cases / sizeof k_cases[0]; c++) {
        static char json[8192];
        char tag[200];
        DSD_SNPRINTF(tag, sizeof tag, "%s: written", k_cases[c].label);
        rc |= expect_int(tag,
                         sdrtrunk_written_call(json, sizeof json, k_cases[c].protocol, k_cases[c].hex, k_cases[c].tag,
                                               5, k_cases[c].old_writer, k_cases[c].eol),
                         0);
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_playback_written");
        rc |= playback_json_run(&r, "written.mbe", json);
        rc |= expect_played_file(&r, k_cases[c].label, 5, 1234U, 5678U);
        DSD_SNPRINTF(tag, sizeof tag, "%s: nothing said", k_cases[c].label);
        rc |= expect_int(tag, wav_recording_played_file_unrecorded_first_this_run(), 1);
        rdio_fallback_off(&r);
        rig_close(&r);
    }
    return rc;
}

/* Issue #888: a played file with no recording of its own (no call: "[]"; a call of no frames whose source does not
   read) leaves the next file's recording alone: -P -r empty.mbe normal.mbe records and exports normal.mbe's call
   under its talkgroup. Before, the first file marked the recording still open, empty, for discard (or kept it local),
   and the next file's audio went into it: the call was deleted at its close (or not exported). */
static int
test_playback_empty_file_leaves_the_next_recording(void) {
    static const struct {
        const char* label;
        const char* json;
    } k_cases[] = {
        {"an empty array", "[]"},
        {"a call of no frames from an unread source",
         "{\"protocol\":\"APCO25-PHASE2\",\"version\":2,\"call_type\":\"GROUP\","
         "\"from\":\"ROAM 5678(BEE01.2A3.5678)\",\"encrypted\":false,\"frames\":[]}"},
    };

    int rc = 0;
    for (size_t c = 0; c < sizeof k_cases / sizeof k_cases[0]; c++) {
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_playback_empty_first");
        r.opts->audio_out = 0;
        r.opts->playfiles = 1;
        rdio_fallback_on(&r, DSD_RDIO_MODE_DIRWATCH);
        submit(&r, DSD_APP_CMD_WAV_START);
        char first[DSD_TEST_PATH_MAX * 2];
        char second[DSD_TEST_PATH_MAX * 2];
        rc |= expect_int("empty first: inputs written",
                         json_write_leaf(&r, "empty.mbe", k_cases[c].json, first, sizeof first)
                             | json_write_leaf(&r, "normal.mbe", K_DMR_HEAD K_THREE_FRAMES "]}", second, sizeof second),
                         0);
        char* argv[3] = {"dsd-neo", first, second};
        r.state->optind = 1;
        playMbeFiles(r.opts, r.state, 3, argv);
        playback_end_run(&r);
        reload(&r);
        rc |= expect_played_file(&r, k_cases[c].label, 3, 1234U, 5678U);
        rdio_fallback_off(&r);
        rig_close(&r);
    }
    return rc;
}

/* Whether a read-only folder (chmod 0500) keeps a file in @p dir from being removed here: not as root, nor on a mount
   whose permissions are fixed by its options. The folder is left writable and empty of the probe. */
static int
read_only_dir_denies_removal(const char* dir) {
#if DSD_PLATFORM_WIN_NATIVE
    (void)dir;
    return 0;
#else
    char probe[DSD_TEST_PATH_MAX * 2];
    if (dsd_test_path_join(probe, sizeof probe, dir, "ro_probe") != 0) {
        return 0;
    }
    FILE* f = dsd_fopen_private(probe, "wb");
    if (f == NULL) {
        return 0;
    }
    (void)fclose(f);
    // The folder made read-only and writable again through one descriptor, so both act on the folder opened here.
    const int dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const int denied = dir_fd >= 0 && dsd_fchmod(dir_fd, 0500) == 0 && remove(probe) != 0;
    if (dir_fd >= 0) {
        (void)dsd_fchmod(dir_fd, 0700);
        (void)dsd_close(dir_fd);
    }
    (void)remove(probe);
    return denied;
#endif
}

#if !DSD_PLATFORM_WIN_NATIVE
/* The folder of the recording the discard hook is handed, made read-only by it; whether the hook ran, and its verdict
   on the file. */
static char g_discard_dir[DSD_TEST_PATH_MAX];
static int g_discard_hook_ran;
static int g_discard_modes_set;
static int g_discard_unreadable;

/* The discard seam (dsd_wav_test_set_discard_hook()), after the close read the recording's size: the recording made
   unreadable and its folder read-only, so neither its removal nor a read of it can succeed. */
static void
discard_unreadable_hook(const char* path) {
    g_discard_hook_ran = 1;
    // Each mode set through a descriptor on the file or folder opened here.
    const int file_fd = open(path, O_RDONLY | O_CLOEXEC);
    const int dir_fd = open(g_discard_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    g_discard_modes_set =
        file_fd >= 0 && dir_fd >= 0 && dsd_fchmod(file_fd, 0000) == 0 && dsd_fchmod(dir_fd, 0500) == 0;
    if (file_fd >= 0) {
        (void)dsd_close(file_fd);
    }
    if (dir_fd >= 0) {
        (void)dsd_close(dir_fd);
    }
    g_discard_unreadable = dsd_wav_file_size_or_negative(path) < 0;
}
#endif

/* Issue #888: an empty recording (the header alone) is journaled `discard` only once it is gone. One whose removal
   fails (here a read-only folder; then a read-only folder and a recording that can no longer be read, made so after
   the close read its size) stays, with a warning and an `error` record of kind `discard` naming it: the removal's own
   error decides, not whether the file can still be read. Before, a failed removal of a file that could not be read
   was journaled as a discard. */
static int
test_empty_segment_discard_checks_the_removal(void) {
    int rc = 0;
    for (int unreadable = 0; unreadable <= 1; unreadable++) {
        const char* leg = unreadable ? "discard of an unreadable file" : "discard in a read-only folder";
        char tag[200];
        rig r;
        rc |= rig_open(&r, "dsdneo_journal_discard_read_only");
        if (!read_only_dir_denies_removal(r.dir)) {
            DSD_FPRINTF(stderr, "SKIP: %s: a read-only folder does not stop a removal here\n", leg);
            rig_close(&r);
            return rc;
        }
        submit(&r, DSD_APP_CMD_WAV_START);
        char id[64];
        slot1_id(&r, id, sizeof id);
        char temp[DSD_TEST_PATH_MAX * 2];
        DSD_SNPRINTF(temp, sizeof temp, "%s", r.opts->wav_out_file);
        begin_call(&r, DSD_SYNC_P25P1_POS, 300U, 1U, DSD_CALL_BOUNDARY_BEGIN);
        dsd_neo_log_set_tap(count_log_lines, NULL);
        g_unremoved_discards = 0;
#if !DSD_PLATFORM_WIN_NATIVE
        g_discard_hook_ran = 0;
        g_discard_modes_set = 0;
        g_discard_unreadable = 0;
        if (unreadable) {
            DSD_SNPRINTF(g_discard_dir, sizeof g_discard_dir, "%s", r.dir);
            dsd_wav_test_set_discard_hook(discard_unreadable_hook);
        } else {
            (void)chmod(r.dir, 0500);
        }
#endif
        end_call(&r, DSD_CALL_END_EXPLICIT);
#if !DSD_PLATFORM_WIN_NATIVE
        dsd_wav_test_set_discard_hook(NULL);
        (void)chmod(temp, 0600);
        (void)chmod(r.dir, 0700);
        if (unreadable) {
            DSD_SNPRINTF(tag, sizeof tag, "%s: the discard hook ran", leg);
            rc |= expect_int(tag, g_discard_hook_ran, 1);
            DSD_SNPRINTF(tag, sizeof tag, "%s: the discard hook set the file's and folder's modes", leg);
            rc |= expect_int(tag, g_discard_modes_set, 1);
        }
        if (unreadable && g_discard_hook_ran && g_discard_modes_set && !g_discard_unreadable) {
            DSD_FPRINTF(stderr, "SKIP: %s: a file of mode 0000 can still be read here\n", leg);
            dsd_neo_log_set_tap(NULL, NULL);
            rig_close(&r);
            return rc;
        }
#endif
        dsd_neo_log_set_tap(NULL, NULL);
        reload(&r);
        DSD_SNPRINTF(tag, sizeof tag, "%s: the recording survives", leg);
        rc |= expect_true(tag, file_exists(temp));
        DSD_SNPRINTF(tag, sizeof tag, "%s: not journaled as discarded", leg);
        rc |= expect_int(tag, count_for_id(&g_j, "discard", id), 0);
        const char* err = NULL;
        const char* line = NULL;
        for (int i = 0; (line = record(&g_j, "error", i)) != NULL; i++) {
            if (strcmp(field_s(line, "id"), id) == 0) {
                err = line;
            }
        }
        DSD_SNPRINTF(tag, sizeof tag, "%s: an error of kind discard", leg);
        rc |= expect_str(tag, err ? field_s(err, "kind") : "", "discard");
        DSD_SNPRINTF(tag, sizeof tag, "%s: it names the file left", leg);
        rc |= expect_str(tag, err ? field_s(err, "temp") : "", temp + strlen(r.dir) + 1);
        DSD_SNPRINTF(tag, sizeof tag, "%s: a warning", leg);
        rc |= expect_int(tag, g_unremoved_discards, 1);
        DSD_SNPRINTF(tag, sizeof tag, "%s: nothing published", leg);
        rc |= expect_int(tag, count_records(&g_j, "publish"), 0);
        rig_close(&r);
    }
    return rc;
}

/* Issue #888: a file's call takes no stamp staged before the file. An export of a protocol the reader does not play
   stamps the staged row from its frame's "time" but opens no call; the next file's call, whose frames carry no "time",
   is filed on the decode clock, in the journal and in its file name. Before, it took the earlier file's date. */
static int
test_playback_next_file_takes_no_earlier_stamp(void) {
    static const char k_unplayed[] = "{\"protocol\":\"LTR\",\"version\":2,\"call_type\":\"GROUP\",\"to\":\"77\","
                                     "\"frames\":[{\"time\":1262304000000,\"hex\":\"3ABF032D9ADEE4CB50\"}]}";
    rig r;
    int rc = rig_open(&r, "dsdneo_journal_playback_next_file");
    r.opts->audio_out = 0;
    r.opts->playfiles = 1;
    rdio_fallback_on(&r, DSD_RDIO_MODE_DIRWATCH);
    submit(&r, DSD_APP_CMD_WAV_START);
    char first[DSD_TEST_PATH_MAX * 2];
    char second[DSD_TEST_PATH_MAX * 2];
    rc |= expect_int("next file: inputs written",
                     json_write_leaf(&r, "unplayed.mbe", k_unplayed, first, sizeof first)
                         | json_write_leaf(&r, "unstamped.mbe", K_DMR_HEAD K_THREE_FRAMES "]}", second, sizeof second),
                     0);
    char* argv[3] = {"dsd-neo", first, second};
    r.state->optind = 1;
    playMbeFiles(r.opts, r.state, 3, argv);
    playback_end_run(&r);
    reload(&r);
    int count = 0;
    const char* pub = publish_of_duration(3LL * 20LL, &count);
    rc |= expect_int("next file: one recording of its call", count, 1);
    const long long start = pub ? field_ll(pub, "start") : -9;
    rc |= expect_true("next file: filed on the decode clock, not the earlier file's stamp",
                      start >= (long long)k_t0 && start <= (long long)k_t0 + 60);
    char earlier_date[16] = "";
    (void)dsd_format_local_datetime((time_t)1262304000, DSD_LOCAL_DATETIME_DATE_COMPACT, earlier_date,
                                    sizeof earlier_date);
    const char* file = pub ? field_s(pub, "file") : "";
    rc |= expect_true("next file: its name is not the earlier file's date",
                      earlier_date[0] != '\0' && strstr(file, earlier_date) == NULL);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "  next file: start %lld, file %s\n", start, file);
    }
    rdio_fallback_off(&r);
    rig_close(&r);
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
    rc |= test_nfm_call_end_keeps_the_slot_file();
    rc |= test_rename_failure_is_an_error_record();
    rc |= test_b3_name_and_raw_identity();
    rc |= test_non_utf8_identity_names_the_file_published();
    rc |= test_pending_closers_are_linked_at_the_commit();
    rc |= test_held_nfm_call_is_one_recording();
    rc |= test_epoch_moving_on_unlinks();
    rc |= test_full_reset_unlinks_pending();
    rc |= test_trunk_scan_hop_and_revisit();
    rc |= test_start_after_mid_call_enable();
    rc |= test_start_across_midnight();
    rc |= test_start_on_the_replay_clock();
    rc |= test_start_floors_once_on_the_replay_clock();
    rc |= test_nfm_start_is_its_first_block();
    rc |= test_nfm_start_moves_back_by_its_preroll();
    rc |= test_stamped_start_is_the_journal_and_sidecar_start();
    rc |= test_torn_tail_gets_a_newline();
    rc |= test_write_failure_latches_the_journal_off();
    rc |= test_no_journal_no_file();
    rc |= test_strings_are_escaped();
    rc |= test_pending_without_room_or_id_is_published_as_no_rows();
    rc |= test_error_failed_and_rdio_shapes();
    rc |= test_rdio_no_system_skip_is_no_failure();
    rc |= test_nfm_rdio_export_is_journaled();
    rc |= test_wav_stop_follows_a_fresh_stream_verdict();
    rc |= test_analog_recordings_are_named_by_mode();
    rc |= test_unpublished_analog_recording_is_discarded();
    rc |= test_failed_analog_open_keeps_its_error_through_a_discard();
    rc |= test_path_forms();
    rc |= test_temp_token_is_the_names_token();
    rc |= test_digital_commit_names_visit();
    rc |= test_nfm_recording_names_visit();
    rc |= test_both_slots_tagged();
    rc |= test_pending_closer_names_visit();
    rc |= test_no_visit_no_key();
    rc |= test_session_final_closes_keep_visit();
    rc |= test_departure_boundary_tags_departing_visit();
    rc |= test_orphan_audio_split_at_tune_boundary();
    rc |= test_orphan_flush_both_slots();
    rc |= test_orphan_flush_skips_empty();
    rc |= test_orphan_flush_skips_active_call();
    rc |= test_orphan_flush_held_epoch_is_pending();
    rc |= test_stream_gap_keeps_orphan();
    rc |= test_orphan_flush_names_the_visit();
    rc |= test_orphan_flush_serialized_with_commit();
    rc |= test_trunk_scan_switch_flushes_orphans();
    rc |= test_trunk_scan_retry_park_ends_the_call_left();
    rc |= test_d7_wav_stop_mid_call();
    rc |= test_d7_held_fade_stays_local();
    rc |= test_d7_shutdown_commit();
    rc |= test_d7_cleared_history_stays_local();
    rc |= test_d7_restored_scan_row_stays_local();
    rc |= test_d7_edacs_rotation();
    rc |= test_d7_both_slots();
    rc |= test_d7_fallback_journal_rdio_flag();
    rc |= test_rdio_system_by_frequency_in_one_session();
    rc |= test_d7_stream_off_stays_local();
    rc |= test_edacs_grant_closes_unowned_audio_first();
    rc |= test_edacs_grant_leaves_owned_audio_to_its_row();
    rc |= test_edacs_same_lid_group_after_all_call();
    rc |= test_tetra_first_call_leaves_unowned_audio();
    rc |= test_tetra_first_call_drops_carried_samples();
    rc |= test_tetra_first_call_conceals_nothing_before_it();
    rc |= test_tetra_held_call_then_readmitted();
    rc |= test_tetra_first_call_starts_the_high_pass_over();
    rc |= test_playback_files_record_apart();
    rc |= test_playback_export_starts_at_its_first_frame();
    rc |= test_playback_record_reads_only_its_own_members();
    rc |= test_playback_unnumbered_call_export();
    rc |= test_playback_records_play_as_one_call();
    rc |= test_playback_stream_off_record_stays_local();
    rc |= test_playback_new_call_without_time_takes_the_decode_clock();
    rc |= test_playback_malformed_frames_play_nothing();
    rc |= test_playback_file_shape_decides_its_recording();
    rc |= test_playback_sdrtrunk_written_files_record();
    rc |= test_playback_empty_file_leaves_the_next_recording();
    rc |= test_empty_segment_discard_checks_the_removal();
    rc |= test_playback_unread_ids_say_they_stay_local();
    rc |= test_playback_next_file_takes_no_earlier_stamp();
    rc |= test_playback_long_export_plays_once();
    if (rc == 0) {
        printf("CORE_WAV_JOURNAL: OK\n");
    }
    return rc;
}
