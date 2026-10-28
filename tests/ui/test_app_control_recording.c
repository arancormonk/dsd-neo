// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #708: the WAV recording commands through the real queue and decoder services (start, stop, toggle, static and
 * raw open), the per-call recording failure policy in each writer (digital slots, EDACS analog, conventional NFM), and
 * the recording view a frontend reads both from.
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
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/wav_recording.h>
#include <dsd-neo/engine/analog_call.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/telemetry.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "commands_internal.h"
#include "edacs_internal.h"
#include "test_support.h"

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

/* Log lines the failure policy writes, counted through the log tap. */
static int g_write_warnings;
static int g_close_warnings;
static int g_open_errors;

static void
count_log_lines(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    if (!text) {
        return;
    }
    if (level == LOG_LEVEL_WARN && strstr(text, "samples to a call recording") != NULL) {
        g_write_warnings++;
    } else if (level == LOG_LEVEL_WARN && strstr(text, "did not close cleanly") != NULL) {
        g_close_warnings++;
    } else if (level == LOG_LEVEL_ERROR && strstr(text, "could not open wav output file") != NULL) {
        g_open_errors++;
    }
}

/* The libsndfile seam: write call @c g_short_write_at falls short (half its samples), and every close reports failure
   while @c g_fail_close is set, after closing the file for real. */
static int g_write_calls;
static int g_short_write_at = -1;
static long long g_frames_written;
static int g_fail_close;

static long long
seam_write(SNDFILE* file, const short* samples, long long count) {
    const int call = g_write_calls++;
    const sf_count_t want = (call == g_short_write_at) ? (sf_count_t)(count / 2) : (sf_count_t)count;
    const sf_count_t written = sf_write_short(file, samples, want);
    g_frames_written += written;
    return (long long)written;
}

static int
seam_close(SNDFILE* file) {
    const int rc = sf_close(file);
    return g_fail_close ? SF_ERR_SYSTEM : rc;
}

static void
seam_reset(void) {
    dsd_wav_test_set_io(seam_write, seam_close);
    dsd_wav_test_fail_opens_after(-1);
    g_write_calls = 0;
    g_short_write_at = -1;
    g_frames_written = 0;
    g_fail_close = 0;
    g_write_warnings = 0;
    g_close_warnings = 0;
    g_open_errors = 0;
    dsd_wav_recording_session_reset();
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    char dir[DSD_TEST_PATH_MAX];
} rig;

static int
rig_open(rig* r, const char* prefix) {
    DSD_MEMSET(r, 0, sizeof(*r));
    if (!dsd_test_mkdtemp(r->dir, sizeof r->dir, prefix)) {
        DSD_FPRINTF(stderr, "FAIL: temp dir for %s\n", prefix);
        return 1;
    }
    r->opts = (dsd_opts*)calloc(1, sizeof(*r->opts));
    r->state = (dsd_state*)calloc(1, sizeof(*r->state));
    if (!r->opts || !r->state) {
        DSD_FPRINTF(stderr, "FAIL: alloc for %s\n", prefix);
        return 1;
    }
    initOpts(r->opts);
    initState(r->state);
    DSD_SNPRINTF(r->opts->wav_out_dir, sizeof r->opts->wav_out_dir, "%s", r->dir);
    seam_reset();
    dsd_app_frontend_runtime_start(r->opts, r->state);
    return 0;
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
    if (r->opts) {
        dsd_wav_test_set_io(NULL, NULL);
        r->opts->wav_out_f = close_wav_file(r->opts->wav_out_f);
        r->opts->wav_out_fR = close_wav_file(r->opts->wav_out_fR);
        r->opts->wav_out_raw = close_wav_file(r->opts->wav_out_raw);
    }
    if (r->state) {
        freeState(r->state);
    }
    free(r->state);
    free(r->opts);
    (void)dsd_dir_list(r->dir, remove_file_cb, r->dir);
    (void)dsd_test_rmdir(r->dir);
    dsd_wav_test_set_io(NULL, NULL);
    dsd_wav_test_fail_opens_after(-1);
}

static int
drain(rig* r) {
    return dsd_app_drain_cmds(r->opts, r->state);
}

static void
submit(int id) {
    (void)dsd_app_command_submit(id, NULL, 0);
}

typedef struct {
    int files;
    int temp;
    int published;
    char published_name[512];
} dir_count;

static int
count_cb(const char* name, void* user) {
    dir_count* c = (dir_count*)user;
    c->files++;
    if (strncmp(name, "TEMP_", 5) == 0) {
        c->temp++;
    } else if (strstr(name, ".wav") != NULL) {
        c->published++;
        DSD_SNPRINTF(c->published_name, sizeof c->published_name, "%s", name);
    }
    return 0;
}

static dir_count
count_dir(const char* dir) {
    dir_count c;
    DSD_MEMSET(&c, 0, sizeof c);
    (void)dsd_dir_list(dir, count_cb, &c);
    return c;
}

/* The frames a WAV's header says it holds: libsndfile reads the header's own data length, which only a close writes. */
static long long
wav_frames(const char* path) {
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof info);
    SNDFILE* f = sf_open(path, SFM_READ, &info);
    if (!f) {
        return -1;
    }
    sf_close(f);
    return (long long)info.frames;
}

/* The data chunk's length in a WAV's header, in bytes; -1 when there is none. A file nobody closed still says 0. */
static long
wav_header_data_bytes(const char* path) {
    FILE* f = dsd_fopen_existing_regular_file(path, "rb");
    if (!f) {
        return -1;
    }
    unsigned char h[8];
    long result = -1;
    if (fseek(f, 12, SEEK_SET) == 0) {
        while (fread(h, 1, 8, f) == 8) {
            const long size = (long)h[4] | ((long)h[5] << 8) | ((long)h[6] << 16) | ((long)h[7] << 24);
            if (memcmp(h, "data", 4) == 0) {
                result = size;
                break;
            }
            if (fseek(f, size + (size & 1), SEEK_CUR) != 0) {
                break;
            }
        }
    }
    fclose(f);
    return result;
}

static int
published_frames(const rig* r, const dir_count* c, long long* frames) {
    char path[DSD_TEST_PATH_MAX * 2];
    if (dsd_test_path_join(path, sizeof path, r->dir, c->published_name) != 0) {
        return 1;
    }
    *frames = wav_frames(path);
    return 0;
}

static dsd_app_recording_view
view_now(void) {
    dsd_app_recording_view v;
    DSD_MEMSET(&v, 0, sizeof v);
    (void)dsd_app_recording_view_get(&v);
    return v;
}

static void
fill_voice_frame(dsd_state* state) {
    for (int i = 0; i < 160; i++) {
        state->audio_out_temp_buf[i] = (float)((i % 32) * 512 - 8192);
    }
}

/* B4: a start with per-call recording on and one slot file missing (its open failed) opens that slot's file; main
   reported success and left the slot unrecorded. */
static int
test_start_opens_a_missing_slot_file(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_partial");
    submit(DSD_APP_CMD_WAV_START);
    rc |= expect_int("start drained", drain(&r), 1);
    SNDFILE* left = r.opts->wav_out_f;
    rc |= expect_true("start opened both slots", left != NULL && r.opts->wav_out_fR != NULL);
    /* Slot 2's file fails, as a full disk fails the reopen at a call end. */
    r.opts->wav_out_fR = close_wav_file(r.opts->wav_out_fR);
    (void)remove(r.opts->wav_out_fileR);
    r.opts->wav_out_fileR[0] = '\0';
    r.opts->wav_out_reopen = 2;

    submit(DSD_APP_CMD_WAV_START);
    rc |= expect_int("second start drained", drain(&r), 1);
    rc |= expect_true("start kept slot 1's open file", r.opts->wav_out_f == left);
    rc |= expect_true("start opened the missing slot 2 file", r.opts->wav_out_fR != NULL);
    rc |= expect_int("no slot waits to reopen", r.opts->wav_out_reopen, 0);
    rc |= expect_int("two slot files on disk", count_dir(r.dir).temp, 2);
    rc |= expect_int("the start reads as applied", view_now().command_state, DSD_APP_RECORDING_COMMAND_APPLIED);
    rig_close(&r);
    return rc;
}

/* B4: a start that cannot open both slot files changes nothing and reports failure. Main kept the file that opened,
   switched per-call recording on with one slot dead, and left nothing to retry it. */
static int
test_start_that_cannot_open_changes_nothing(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_rollback");
    dsd_wav_test_fail_opens_after(1);
    submit(DSD_APP_CMD_WAV_START);
    rc |= expect_int("failing start drained", drain(&r), 1);
    rc |= expect_int("per-call recording stays off", r.opts->dmr_stereo_wav, 0);
    rc |= expect_true("no slot file kept", r.opts->wav_out_f == NULL && r.opts->wav_out_fR == NULL);
    rc |= expect_int("the file that opened is gone", count_dir(r.dir).files, 0);
    dsd_app_recording_view v = view_now();
    rc |= expect_int("the start reads as failed", v.command_state, DSD_APP_RECORDING_COMMAND_FAILED);
    rc |= expect_int("the failed start is the one shown", v.command_id, DSD_APP_CMD_WAV_START);
    rc |= expect_int("the open failure is counted", v.failures[DSD_WAV_RECORDING_ERROR_OPEN], 1);
    rc |= expect_int("the last error is the open", v.last_error, DSD_WAV_RECORDING_ERROR_OPEN);
    rc |= expect_int("the view says per-call recording is off", v.per_call, 0);

    /* With per-call recording on and slot 2 missing, a start that still cannot open it keeps slot 1 recording. */
    dsd_wav_test_fail_opens_after(-1);
    submit(DSD_APP_CMD_WAV_START);
    (void)drain(&r);
    SNDFILE* left = r.opts->wav_out_f;
    r.opts->wav_out_fR = close_wav_file(r.opts->wav_out_fR);
    (void)remove(r.opts->wav_out_fileR);
    r.opts->wav_out_fileR[0] = '\0';
    r.opts->wav_out_reopen = 2;
    dsd_wav_test_fail_opens_after(0);
    submit(DSD_APP_CMD_WAV_START);
    (void)drain(&r);
    rc |= expect_int("per-call recording stays on", r.opts->dmr_stereo_wav, 1);
    rc |= expect_true("slot 1 keeps recording", r.opts->wav_out_f == left && r.opts->wav_out_fR == NULL);
    rc |= expect_int("slot 2 still waits to reopen", r.opts->wav_out_reopen, 2);
    v = view_now();
    rc |= expect_int("the partial start reads as failed", v.command_state, DSD_APP_RECORDING_COMMAND_FAILED);
    rc |= expect_int("the view shows slot 1's file only", v.slot_files, 1);
    rig_close(&r);
    return rc;
}

static int
open_static(rig* r, const char* path) {
    int rc = expect_int("static open queued", dsd_app_command_set_string(DSD_APP_CMD_WAV_STATIC_OPEN, path),
                        DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("static open drained", drain(r), 1);
    rc |= expect_true("static WAV open", r->opts->wav_out_f != NULL && r->opts->static_wav_file == 1);
    short block[320];
    for (int i = 0; i < 320; i++) {
        block[i] = (short)(i * 16);
    }
    dsd_wav_segment_write(r->opts->wav_out_f, block, 320, "static test");
    return rc;
}

/* B4: a start while a static WAV records closes it as a static file (its header finalized, never renamed or exported)
   and clears static_wav_file, which would suppress per-call rotation. Main leaked the static handle open. */
static int
test_start_closes_a_static_wav_as_static(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_static_start");
    char static_path[DSD_TEST_PATH_MAX];
    char static_dir[DSD_TEST_PATH_MAX];
    rc |= expect_true("static dir", dsd_test_mkdtemp(static_dir, sizeof static_dir, "dsdneo_rec_static_s") != NULL);
    rc |= expect_int("static path", dsd_test_path_join(static_path, sizeof static_path, static_dir, "static.wav"), 0);
    rc |= open_static(&r, static_path);
    rc |= expect_int("the view says static", view_now().mode, DSD_APP_RECORDING_MODE_STATIC);

    submit(DSD_APP_CMD_WAV_START);
    rc |= expect_int("start drained", drain(&r), 1);
    rc |= expect_int("static mode cleared", r.opts->static_wav_file, 0);
    rc |= expect_int("per-call mode on", r.opts->dmr_stereo_wav, 1);
    rc |= expect_true("both slot files open", r.opts->wav_out_f != NULL && r.opts->wav_out_fR != NULL);
    rc |= expect_int("the static file closed with its header written", wav_header_data_bytes(static_path), 640);
    rc |= expect_int("only the slot files in the per-call folder", count_dir(r.dir).files, 2);
    rc |= expect_int("the view says per-call", view_now().mode, DSD_APP_RECORDING_MODE_PER_CALL);
    (void)remove(static_path);
    (void)dsd_test_rmdir(static_dir);
    rig_close(&r);
    return rc;
}

/* B4: a stop closes a static WAV as one. Main passed it through the per-call rename and rdio export, moving the
   operator's file into the per-call folder under a call's name. */
static int
test_stop_closes_a_static_wav_as_static(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_static_stop");
    char static_path[DSD_TEST_PATH_MAX];
    char static_dir[DSD_TEST_PATH_MAX];
    rc |= expect_true("static dir", dsd_test_mkdtemp(static_dir, sizeof static_dir, "dsdneo_rec_static_t") != NULL);
    rc |= expect_int("static path", dsd_test_path_join(static_path, sizeof static_path, static_dir, "static.wav"), 0);
    rc |= open_static(&r, static_path);

    submit(DSD_APP_CMD_WAV_STOP);
    rc |= expect_int("stop drained", drain(&r), 1);
    rc |= expect_true("static handle closed", r.opts->wav_out_f == NULL);
    rc |= expect_int("static mode off", r.opts->static_wav_file, 0);
    rc |= expect_int("the static file stays where it was, finalized", wav_header_data_bytes(static_path), 640);
    rc |= expect_int("nothing published into the per-call folder", count_dir(r.dir).files, 0);
    rc |= expect_int("the view says off", view_now().mode, DSD_APP_RECORDING_MODE_OFF);
    (void)remove(static_path);
    (void)dsd_test_rmdir(static_dir);
    rig_close(&r);
    return rc;
}

/* B4: a static open while per-call files record closes them with their publication. Main overwrote slot 1's handle
   (its file left as TEMP) and left slot 2 open behind the static mode. */
static int
test_static_open_closes_per_call_files(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_static_over");
    submit(DSD_APP_CMD_WAV_START);
    (void)drain(&r);
    fill_voice_frame(r.state);
    dsd_write_mono_voice_to_slot_wav(r.opts, r.state, 0);
    char static_path[DSD_TEST_PATH_MAX];
    char static_dir[DSD_TEST_PATH_MAX];
    rc |= expect_true("static dir", dsd_test_mkdtemp(static_dir, sizeof static_dir, "dsdneo_rec_static_o") != NULL);
    rc |= expect_int("static path", dsd_test_path_join(static_path, sizeof static_path, static_dir, "static.wav"), 0);
    rc |= open_static(&r, static_path);
    rc |= expect_true("slot 2's file closed", r.opts->wav_out_fR == NULL);
    rc |= expect_int("per-call mode off", r.opts->dmr_stereo_wav, 0);
    const dir_count c = count_dir(r.dir);
    rc |= expect_int("no temp file left behind", c.temp, 0);
    rc |= expect_int("slot 1's recording published", c.published, 1);
    r.opts->wav_out_f = close_wav_file(r.opts->wav_out_f);
    (void)remove(static_path);
    (void)dsd_test_rmdir(static_dir);
    rig_close(&r);
    return rc;
}

/* The raw WAV open closes the raw WAV it replaces, finalizing its header. Main leaked the handle. */
static int
test_raw_open_closes_the_previous_raw_wav(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_raw");
    char first[DSD_TEST_PATH_MAX];
    char second[DSD_TEST_PATH_MAX];
    rc |= expect_int("raw path 1", dsd_test_path_join(first, sizeof first, r.dir, "raw1.wav"), 0);
    rc |= expect_int("raw path 2", dsd_test_path_join(second, sizeof second, r.dir, "raw2.wav"), 0);
    rc |= expect_int("raw open 1 queued", dsd_app_command_set_string(DSD_APP_CMD_WAV_RAW_OPEN, first),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    (void)drain(&r);
    rc |= expect_true("raw 1 open", r.opts->wav_out_raw != NULL);
    short block[480] = {0};
    if (r.opts->wav_out_raw) {
        rc |= expect_int("raw 1 written", (long long)sf_write_short(r.opts->wav_out_raw, block, 480), 480);
    }
    rc |= expect_int("raw open 2 queued", dsd_app_command_set_string(DSD_APP_CMD_WAV_RAW_OPEN, second),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    (void)drain(&r);
    rc |= expect_true("raw 2 open", r.opts->wav_out_raw != NULL);
    rc |= expect_int("raw 1 closed with its header written", wav_header_data_bytes(first), 960);
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

/* B4: a stop closes and publishes a conventional NFM call's recording at once, under the call's row, and the call goes
   on with its epoch. Main left the recording open until the next block. */
static int
test_stop_publishes_the_nfm_recording_at_once(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_nfm_stop");
    setup_nfm(r.opts, r.state);
    rc |= expect_true("tone detection on", dsd_analog_tone_detection_active(r.opts));
    submit(DSD_APP_CMD_WAV_START);
    (void)drain(&r);
    nfm_block(r.opts, r.state);
    rc |= expect_true("nfm call active", dsd_engine_analog_call_active(r.state));
    dsd_call_snapshot before;
    DSD_MEMSET(&before, 0, sizeof before);
    rc |= expect_int("nfm call tracked", dsd_call_state_get(r.state, 0, &before), 1);

    submit(DSD_APP_CMD_WAV_STOP);
    rc |= expect_int("stop drained", drain(&r), 1);
    const dir_count c = count_dir(r.dir);
    rc |= expect_int("no temp file left", c.temp, 0);
    rc |= expect_int("the nfm recording published", c.published, 1);
    rc |= expect_true("published under the nfm name", strstr(c.published_name, "_NFM") != NULL);
    rc |= expect_true("the call goes on", dsd_engine_analog_call_active(r.state));
    dsd_call_snapshot after;
    DSD_MEMSET(&after, 0, sizeof after);
    rc |= expect_int("nfm call still tracked", dsd_call_state_get(r.state, 0, &after), 1);
    rc |= expect_true("the call keeps its epoch", after.epoch == before.epoch && after.epoch != 0);
    nfm_block(r.opts, r.state);
    rc |= expect_int("nothing reopens with recording off", count_dir(r.dir).files, 1);
    rig_close(&r);
    return rc;
}

/* B4: the toggle follows the mode. With per-call recording on and both slot files failed, main started again instead
   of stopping, so the operator could not switch it off. */
static int
test_toggle_follows_the_mode(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_toggle");
    submit(DSD_APP_CMD_WAV_TOGGLE);
    (void)drain(&r);
    rc |= expect_int("toggle from off starts", r.opts->dmr_stereo_wav, 1);
    r.opts->wav_out_f = close_wav_file(r.opts->wav_out_f);
    r.opts->wav_out_fR = close_wav_file(r.opts->wav_out_fR);
    (void)remove(r.opts->wav_out_file);
    (void)remove(r.opts->wav_out_fileR);
    r.opts->wav_out_file[0] = '\0';
    r.opts->wav_out_fileR[0] = '\0';
    r.opts->wav_out_reopen = 3;
    submit(DSD_APP_CMD_WAV_TOGGLE);
    (void)drain(&r);
    rc |= expect_int("toggle with recording on stops", r.opts->dmr_stereo_wav, 0);
    rc |= expect_int("no slot waits to reopen", r.opts->wav_out_reopen, 0);
    rc |= expect_int("nothing opened", count_dir(r.dir).files, 0);
    rig_close(&r);
    return rc;
}

/* The view's command state: pending from the submit until a published view shows the command applied, then its
   outcome; a command the session drops reads as dropped. */
static int
test_view_reports_pending_and_outcome(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_view");
    submit(DSD_APP_CMD_WAV_START);
    dsd_app_recording_view v = view_now();
    rc |= expect_int("submitted start pends", v.command_state, DSD_APP_RECORDING_COMMAND_PENDING);
    rc |= expect_int("pending command named", v.command_id, DSD_APP_CMD_WAV_START);
    (void)drain(&r);
    v = view_now();
    rc |= expect_int("view published", v.published, 1);
    rc |= expect_int("applied start", v.command_state, DSD_APP_RECORDING_COMMAND_APPLIED);
    rc |= expect_int("per-call on", v.per_call, 1);
    rc |= expect_int("both slot files", v.slot_files, 3);
    rc |= expect_int("no unknown-only rule yet", v.unknown_only, 0);
    rc |= expect_int("no journal yet", v.journal_state, DSD_APP_RECORDING_JOURNAL_NONE);

    submit(DSD_APP_CMD_WAV_STOP);
    rc |= expect_int("stop pends", view_now().command_state, DSD_APP_RECORDING_COMMAND_PENDING);
    dsd_app_frontend_runtime_stop();
    v = view_now();
    rc |= expect_int("stop dropped with the session", v.command_state, DSD_APP_RECORDING_COMMAND_DROPPED);
    rc |= expect_int("dropped stop named", v.command_id, DSD_APP_CMD_WAV_STOP);
    dsd_app_frontend_runtime_start(r.opts, r.state);
    submit(DSD_APP_CMD_WAV_STOP);
    (void)drain(&r);
    rc |= expect_int("stop applied in the next session", view_now().command_state, DSD_APP_RECORDING_COMMAND_APPLIED);
    rig_close(&r);
    return rc;
}

/* Per-call recording failures: which writer a case drives. */
enum { WRITER_DIGITAL = 0, WRITER_EDACS = 1, WRITER_NFM = 2, WRITER_COUNT = 3 };

static const char*
writer_name(int writer) {
    static const char* const k_names[WRITER_COUNT] = {"digital slot", "EDACS analog", "NFM"};
    return (writer >= 0 && writer < WRITER_COUNT) ? k_names[writer] : "?";
}

static int
writer_begin(rig* r, int writer) {
    int rc = 0;
    if (writer == WRITER_NFM) {
        setup_nfm(r->opts, r->state);
    }
    submit(DSD_APP_CMD_WAV_START);
    rc |= expect_int("start drained", drain(r), 1);
    if (writer == WRITER_EDACS) {
        const dsd_call_observation call = {.protocol = DSD_SYNC_EDACS_POS,
                                           .slot = 0U,
                                           .kind = DSD_CALL_KIND_GROUP_VOICE,
                                           .ota_target_id = 321U,
                                           .policy_target_id = 321U,
                                           .ota_source_id = 1234U};
        rc |= expect_int("edacs call observed", dsd_call_state_observe(r->state, &call, DSD_CALL_BOUNDARY_BEGIN), 1);
    }
    return rc;
}

/* One unit of the writer's audio: a vocoder frame, an EDACS triplet, an NFM block. */
static void
writer_write(rig* r, int writer) {
    if (writer == WRITER_DIGITAL) {
        fill_voice_frame(r->state);
        dsd_write_mono_voice_to_slot_wav(r->opts, r->state, 0);
    } else if (writer == WRITER_EDACS) {
        static short analog[3][960];
        for (int i = 0; i < 960; i++) {
            analog[0][i] = (short)(i * 8);
            analog[1][i] = (short)(-i * 8);
            analog[2][i] = (short)(i * 4);
        }
        edacs_write_analog_wav(r->opts, r->state, analog[0], analog[1], analog[2]);
    } else {
        nfm_block(r->opts, r->state);
    }
}

/* Close the writer's segment the way its call ends: a stop for the slot files, the call's end for NFM. */
static void
writer_end(rig* r, int writer) {
    if (writer == WRITER_NFM) {
        dsd_engine_analog_call_end(r->opts, r->state);
    } else {
        submit(DSD_APP_CMD_WAV_STOP);
        (void)drain(r);
    }
}

/* B5: the first short write latches the segment failed: nothing more reaches it, one warning, and the segment is
   published as usual with what was written. Main went on writing after the short write. */
static int
test_short_write_latches_the_segment(int writer) {
    rig r;
    char tag[96];
    int rc = rig_open(&r, "dsdneo_rec_short");
    rc |= writer_begin(&r, writer);
    writer_write(&r, writer);
    const int calls_before = g_write_calls;
    g_short_write_at = g_write_calls;
    writer_write(&r, writer);
    writer_write(&r, writer);
    writer_write(&r, writer);
    DSD_SNPRINTF(tag, sizeof tag, "%s: nothing written after the short write", writer_name(writer));
    rc |= expect_int(tag, g_write_calls, calls_before + 1);
    DSD_SNPRINTF(tag, sizeof tag, "%s: one warning", writer_name(writer));
    rc |= expect_int(tag, g_write_warnings, 1);
    writer_end(&r, writer);
    const dir_count c = count_dir(r.dir);
    DSD_SNPRINTF(tag, sizeof tag, "%s: the segment published", writer_name(writer));
    rc |= expect_true(tag, c.published == 1 && c.temp == (writer == WRITER_NFM ? 2 : 0));
    long long frames = -1;
    rc |= published_frames(&r, &c, &frames);
    DSD_SNPRINTF(tag, sizeof tag, "%s: the published file holds what was written", writer_name(writer));
    rc |= expect_int(tag, frames, g_frames_written);
    dsd_wav_recording_status status;
    dsd_wav_recording_status_get(&status);
    DSD_SNPRINTF(tag, sizeof tag, "%s: one write failure counted", writer_name(writer));
    rc |= expect_int(tag, status.failures[DSD_WAV_RECORDING_ERROR_WRITE], 1);
    DSD_SNPRINTF(tag, sizeof tag, "%s: the last error is the write", writer_name(writer));
    rc |= expect_int(tag, status.last_error, DSD_WAV_RECORDING_ERROR_WRITE);

    /* The next segment opens and records normally. */
    if (writer == WRITER_NFM) {
        const int calls = g_write_calls;
        nfm_block(r.opts, r.state);
        DSD_SNPRINTF(tag, sizeof tag, "%s: the next segment is written", writer_name(writer));
        rc |= expect_int(tag, g_write_calls, calls + 1);
    } else {
        submit(DSD_APP_CMD_WAV_START);
        (void)drain(&r);
        const int calls = g_write_calls;
        writer_write(&r, writer);
        DSD_SNPRINTF(tag, sizeof tag, "%s: the next segment is written", writer_name(writer));
        rc |= expect_true(tag, g_write_calls > calls);
    }
    dsd_telemetry_publish_opts_snapshot(r.opts);
    const dsd_app_recording_view v = view_now();
    DSD_SNPRINTF(tag, sizeof tag, "%s: the view counts the write failure", writer_name(writer));
    rc |= expect_int(tag, v.failures[DSD_WAV_RECORDING_ERROR_WRITE], 1);
    rig_close(&r);
    return rc;
}

/* B5: a close that fails keeps the segment under its temp name and counts it. Main ignored sf_close()'s result and
   published a file whose header may not hold what was written. */
static int
test_close_failure_keeps_the_temp_file(int writer) {
    rig r;
    char tag[96];
    int rc = rig_open(&r, "dsdneo_rec_close");
    rc |= writer_begin(&r, writer);
    writer_write(&r, writer);
    g_fail_close = 1;
    writer_end(&r, writer);
    g_fail_close = 0;
    const dir_count c = count_dir(r.dir);
    DSD_SNPRINTF(tag, sizeof tag, "%s: nothing published", writer_name(writer));
    rc |= expect_int(tag, c.published, 0);
    DSD_SNPRINTF(tag, sizeof tag, "%s: the recording keeps its temp name", writer_name(writer));
    rc |= expect_true(tag, c.temp >= 1);
    dsd_wav_recording_status status;
    dsd_wav_recording_status_get(&status);
    DSD_SNPRINTF(tag, sizeof tag, "%s: the close failure counted", writer_name(writer));
    rc |= expect_true(tag, status.failures[DSD_WAV_RECORDING_ERROR_CLOSE] >= 1);
    DSD_SNPRINTF(tag, sizeof tag, "%s: the last error is the close", writer_name(writer));
    rc |= expect_int(tag, status.last_error, DSD_WAV_RECORDING_ERROR_CLOSE);
    DSD_SNPRINTF(tag, sizeof tag, "%s: one close warning", writer_name(writer));
    rc |= expect_int(tag, g_close_warnings, 1);
    rig_close(&r);
    return rc;
}

static void
end_p25_call(rig* r, uint32_t tg) {
    const dsd_call_observation call = {.protocol = DSD_SYNC_P25P1_POS,
                                       .slot = 0U,
                                       .kind = DSD_CALL_KIND_GROUP_VOICE,
                                       .ota_target_id = tg,
                                       .policy_target_id = tg,
                                       .ota_source_id = 77U};
    (void)dsd_call_state_observe(r->state, &call, DSD_CALL_BOUNDARY_BEGIN);
    dsd_event_sync_slot(r->opts, r->state, 0);
    (void)dsd_call_state_end_ex(r->state, 0, 0.0, DSD_CALL_END_EXPLICIT);
    dsd_event_sync_slot(r->opts, r->state, 0);
}

/* B5: a slot whose reopen failed at a call end opens again at the next one, every failure counted and one logged. Main
   left the slot unrecorded for the rest of the run. */
static int
test_failed_reopen_retries_at_the_next_call_end(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_reopen");
    submit(DSD_APP_CMD_WAV_START);
    (void)drain(&r);
    dsd_wav_test_fail_opens_after(0);
    end_p25_call(&r, 100U);
    rc |= expect_true("slot 1's reopen failed", r.opts->wav_out_f == NULL);
    rc |= expect_int("slot 1 waits to reopen", r.opts->wav_out_reopen & 1, 1);
    end_p25_call(&r, 101U);
    rc |= expect_true("still failing", r.opts->wav_out_f == NULL);
    dsd_wav_recording_status status;
    dsd_wav_recording_status_get(&status);
    rc |= expect_int("each failed open counted", status.failures[DSD_WAV_RECORDING_ERROR_OPEN], 2);
    rc |= expect_int("one open error logged", g_open_errors, 1);
    dsd_wav_test_fail_opens_after(-1);
    end_p25_call(&r, 102U);
    rc |= expect_true("slot 1 records again", r.opts->wav_out_f != NULL);
    rc |= expect_int("nothing waits to reopen", r.opts->wav_out_reopen & 1, 0);
    rig_close(&r);
    return rc;
}

int
main(void) {
    dsd_neo_log_set_tap(count_log_lines, NULL);
    int rc = 0;
    rc |= test_start_opens_a_missing_slot_file();
    rc |= test_start_that_cannot_open_changes_nothing();
    rc |= test_start_closes_a_static_wav_as_static();
    rc |= test_stop_closes_a_static_wav_as_static();
    rc |= test_static_open_closes_per_call_files();
    rc |= test_raw_open_closes_the_previous_raw_wav();
    rc |= test_stop_publishes_the_nfm_recording_at_once();
    rc |= test_toggle_follows_the_mode();
    rc |= test_view_reports_pending_and_outcome();
    for (int writer = 0; writer < WRITER_COUNT; writer++) {
        rc |= test_short_write_latches_the_segment(writer);
        rc |= test_close_failure_keeps_the_temp_file(writer);
    }
    rc |= test_failed_reopen_retries_at_the_next_call_end();
    if (rc == 0) {
        printf("APP_CONTROL_RECORDING: OK\n");
    }
    return rc;
}
