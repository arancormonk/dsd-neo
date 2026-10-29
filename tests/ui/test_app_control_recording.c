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
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/wav_recording.h>
#include <dsd-neo/engine/analog_call.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <dsd-neo/runtime/telemetry.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "commands_internal.h"
#include "edacs_internal.h"
#include "test_support.h"
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

/* Log lines the failure policy writes, counted through the log tap. */
static int g_write_warnings;
static int g_close_warnings;
static int g_open_errors;
static int g_table_full_warnings;

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
    } else if (level == LOG_LEVEL_WARN && strstr(text, "recordings open at once") != NULL) {
        g_table_full_warnings++;
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
    g_table_full_warnings = 0;
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
    /* A stop closes both slot files, and both keep their temp names. An NFM call's end closes its own recording, and
       its commit rotates slot 1: that close fails too, and a new slot 1 file opens beside slot 2's. */
    DSD_SNPRINTF(tag, sizeof tag, "%s: the recordings keep their temp names", writer_name(writer));
    rc |= expect_int(tag, c.temp, writer == WRITER_NFM ? 4 : 2);
    dsd_wav_recording_status status;
    dsd_wav_recording_status_get(&status);
    DSD_SNPRINTF(tag, sizeof tag, "%s: each close failure counted", writer_name(writer));
    rc |= expect_int(tag, status.failures[DSD_WAV_RECORDING_ERROR_CLOSE], 2);
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

/* A path no file can be created at: its folder does not exist. */
static void
unopenable_path(const rig* r, char* out, size_t out_size) {
    char missing[DSD_TEST_PATH_MAX];
    (void)dsd_test_path_join(missing, sizeof missing, r->dir, "missing-folder");
    (void)dsd_test_path_join(out, out_size, missing, "out.wav");
}

/* B4: a static open that does not open leaves the recording that runs, and the mode, as they were. Main stopped it
   first and then left static mode set with no file. */
static int
test_failed_static_open_keeps_the_running_recording(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_static_fail");
    submit(DSD_APP_CMD_WAV_START);
    (void)drain(&r);
    SNDFILE* left = r.opts->wav_out_f;
    SNDFILE* right = r.opts->wav_out_fR;
    char bad[DSD_TEST_PATH_MAX * 2];
    unopenable_path(&r, bad, sizeof bad);
    rc |= expect_int("static open queued", dsd_app_command_set_string(DSD_APP_CMD_WAV_STATIC_OPEN, bad),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    (void)drain(&r);
    rc |= expect_int("per-call recording stays on", r.opts->dmr_stereo_wav, 1);
    rc |= expect_int("static mode stays off", r.opts->static_wav_file, 0);
    rc |= expect_true("the slot files keep recording", r.opts->wav_out_f == left && r.opts->wav_out_fR == right);
    rc |= expect_int("both slot files still on disk", count_dir(r.dir).temp, 2);
    const dsd_app_recording_view v = view_now();
    rc |= expect_int("the static open reads as failed", v.command_state, DSD_APP_RECORDING_COMMAND_FAILED);
    rc |= expect_int("the view still says per-call", v.mode, DSD_APP_RECORDING_MODE_PER_CALL);
    rig_close(&r);
    return rc;
}

/* A raw open that does not open leaves the raw WAV that runs. */
static int
test_failed_raw_open_keeps_the_running_raw_wav(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_raw_fail");
    char first[DSD_TEST_PATH_MAX];
    rc |= expect_int("raw path", dsd_test_path_join(first, sizeof first, r.dir, "raw1.wav"), 0);
    (void)dsd_app_command_set_string(DSD_APP_CMD_WAV_RAW_OPEN, first);
    (void)drain(&r);
    SNDFILE* raw = r.opts->wav_out_raw;
    rc |= expect_true("raw open", raw != NULL);
    char bad[DSD_TEST_PATH_MAX * 2];
    unopenable_path(&r, bad, sizeof bad);
    (void)dsd_app_command_set_string(DSD_APP_CMD_WAV_RAW_OPEN, bad);
    (void)drain(&r);
    rc |= expect_true("the raw WAV keeps recording", r.opts->wav_out_raw == raw);
    rc |= expect_true("its path stays", strcmp(r.opts->wav_out_file_raw, first) == 0);
    rig_close(&r);
    return rc;
}

/* START backstop: with the mode off, a per-call handle still open (a mode change that closed nothing) is published,
   not overwritten and leaked. */
static int
test_start_closes_handles_the_mode_does_not_cover(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_backstop");
    submit(DSD_APP_CMD_WAV_START);
    (void)drain(&r);
    fill_voice_frame(r.state);
    dsd_write_mono_voice_to_slot_wav(r.opts, r.state, 0);
    r.opts->dmr_stereo_wav = 0;
    submit(DSD_APP_CMD_WAV_START);
    (void)drain(&r);
    const dir_count c = count_dir(r.dir);
    rc |= expect_int("only the new slot files are open", c.temp, 2);
    rc |= expect_int("the old slot 1 recording is published", c.published, 1);
    rc |= expect_int("per-call recording on", r.opts->dmr_stereo_wav, 1);
    rig_close(&r);
    return rc;
}

static int
apply_recording_config_in(rig* r, int per_call, const char* static_path, const char* dir) {
    static dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_recording = 1;
    cfg.per_call_wav = per_call;
    DSD_SNPRINTF(cfg.per_call_wav_dir, sizeof cfg.per_call_wav_dir, "%s", dir);
    if (static_path) {
        DSD_SNPRINTF(cfg.static_wav_path, sizeof cfg.static_wav_path, "%s", static_path);
    }
    int rc = expect_int("config queued", dsd_app_command_apply_config(&cfg), DSD_APP_COMMAND_SUBMIT_QUEUED);
    return rc | expect_int("config drained", drain(r), 1);
}

static int
apply_recording_config(rig* r, int per_call, const char* static_path) {
    return apply_recording_config_in(r, per_call, static_path, r->dir);
}

/* Per-call recording on in the rig's folder with slot 1 holding audio; a second, empty folder in @p other. */
static int
start_with_audio_and_second_folder(rig* r, char* other, size_t other_size) {
    int rc = expect_true("second folder", dsd_test_mkdtemp(other, other_size, "dsdneo_rec_cfg_dir") != NULL);
    submit(DSD_APP_CMD_WAV_START);
    (void)drain(r);
    fill_voice_frame(r->state);
    dsd_write_mono_voice_to_slot_wav(r->opts, r->state, 0);
    return rc;
}

static void
remove_folder(const char* dir) {
    (void)dsd_dir_list(dir, remove_file_cb, (void*)dir);
    (void)dsd_test_rmdir(dir);
}

/* A config that moves the per-call folder publishes the files recorded so far in the folder they were recorded in, and
   per-call recording goes on in the new one. Main left them open; each call end then filed them across folders, which
   fails between filesystems and leaves the temp file behind. */
static int
test_config_moving_the_folder_publishes_in_the_old_one(void) {
    rig r;
    char other[DSD_TEST_PATH_MAX];
    int rc = rig_open(&r, "dsdneo_rec_cfg_move");
    rc |= start_with_audio_and_second_folder(&r, other, sizeof other);
    rc |= apply_recording_config_in(&r, 1, NULL, other);
    dir_count old_dir = count_dir(r.dir);
    dir_count new_dir = count_dir(other);
    rc |= expect_int("the old folder keeps no temp file", old_dir.temp, 0);
    rc |= expect_int("slot 1's recording published in the old folder", old_dir.published, 1);
    rc |= expect_int("a new pair opens in the new folder", new_dir.temp, 2);
    rc |= expect_true("the folder is the config's", strcmp(r.opts->wav_out_dir, other) == 0);
    rc |= expect_int("per-call recording on", r.opts->dmr_stereo_wav, 1);
    rc |=
        expect_true("the slot files are the new folder's", strstr(r.opts->wav_out_file, other) == r.opts->wav_out_file);
    rig_close(&r);
    remove_folder(other);
    return rc;
}

/* The same with per-call recording turned off, and with a static WAV in its place: the files are published in their
   own folder, and nothing lands in the new one. */
static int
test_config_moving_the_folder_and_stopping_publishes_in_the_old_one(void) {
    int rc = 0;
    for (int to_static = 0; to_static < 2; to_static++) {
        rig r;
        char other[DSD_TEST_PATH_MAX];
        char static_path[DSD_TEST_PATH_MAX * 2];
        rc |= rig_open(&r, "dsdneo_rec_cfg_move_off");
        rc |= start_with_audio_and_second_folder(&r, other, sizeof other);
        (void)dsd_test_path_join(static_path, sizeof static_path, other, "static.wav");
        rc |= apply_recording_config_in(&r, 0, to_static ? static_path : NULL, other);
        const dir_count old_dir = count_dir(r.dir);
        const dir_count new_dir = count_dir(other);
        const char* what = to_static ? "static" : "off";
        char tag[96];
        DSD_SNPRINTF(tag, sizeof tag, "%s: the old folder keeps no temp file", what);
        rc |= expect_int(tag, old_dir.temp, 0);
        DSD_SNPRINTF(tag, sizeof tag, "%s: slot 1's recording published in the old folder", what);
        rc |= expect_int(tag, old_dir.published, 1);
        DSD_SNPRINTF(tag, sizeof tag, "%s: nothing per-call in the new folder", what);
        rc |= expect_int(tag, new_dir.temp, 0);
        rc |= expect_int(tag, new_dir.published, to_static ? 1 : 0); /* the static file itself */
        DSD_SNPRINTF(tag, sizeof tag, "%s: the folder is the config's", what);
        rc |= expect_true(tag, strcmp(r.opts->wav_out_dir, other) == 0);
        DSD_SNPRINTF(tag, sizeof tag, "%s: the mode is the config's", what);
        rc |= expect_true(tag, r.opts->dmr_stereo_wav == 0 && r.opts->static_wav_file == to_static);
        r.opts->wav_out_f = close_wav_file(r.opts->wav_out_f);
        rig_close(&r);
        remove_folder(other);
    }
    return rc;
}

/* A config that changes nothing about recording runs nothing: a slot waiting to reopen waits for its call end, and the
   apply does not fail on it. */
static int
test_config_that_changes_no_recording_leaves_it(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_cfg_same");
    submit(DSD_APP_CMD_WAV_START);
    (void)drain(&r);
    r.opts->wav_out_fR = close_wav_file(r.opts->wav_out_fR);
    (void)remove(r.opts->wav_out_fileR);
    r.opts->wav_out_fileR[0] = '\0';
    r.opts->wav_out_reopen = 2;
    dsd_wav_test_fail_opens_after(0);
    r.state->ui_msg[0] = '\0';
    rc |= apply_recording_config(&r, 1, NULL);
    dsd_wav_recording_status status;
    dsd_wav_recording_status_get(&status);
    rc |= expect_int("no open tried", status.failures[DSD_WAV_RECORDING_ERROR_OPEN], 0);
    rc |= expect_true("no recording toast", strstr(r.state->ui_msg, "did not open") == NULL);
    rc |= expect_int("slot 2 still waits for its call end", r.opts->wav_out_reopen, 2);
    rc |= expect_int("per-call recording on", r.opts->dmr_stereo_wav, 1);
    rig_close(&r);
    return rc;
}

/* The recording transition through config applies, table-driven: every from-mode (off, per-call, static) to every
   to-mode, the per-call folder kept or moved, the static path kept or new, the raw path kept or new, and each new
   output's open succeeding or failing. A failure changes nothing; a success leaves exactly the target running, the
   outputs it replaced closed as their own kind in their own folder, and the ones it kept untouched. */
enum { REC_OFF = 0, REC_PER_CALL = 1, REC_STATIC = 2 };

enum { FAIL_NONE = 0, FAIL_PRIMARY = 1, FAIL_RAW = 2 };

typedef struct {
    int from;
    int to;
    int new_dir;    /* to per-call: the config names folder B */
    int new_static; /* to static: S2 instead of the running S1 */
    int new_raw;    /* the config names R2 instead of the running R1 */
    int fail;
} transition_case;

static int
file_exists(const char* path) {
    FILE* f = dsd_fopen_existing_regular_file(path, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

static int
run_transition_case(const transition_case* tc) {
    rig r;
    char tag[192];
    char other[DSD_TEST_PATH_MAX];
    char side[DSD_TEST_PATH_MAX];
    char s1[DSD_TEST_PATH_MAX * 2];
    char s2[DSD_TEST_PATH_MAX * 2];
    char r1[DSD_TEST_PATH_MAX * 2];
    char r2[DSD_TEST_PATH_MAX * 2];
    char bad[DSD_TEST_PATH_MAX * 2];
    static const char* const k_modes[3] = {"off", "per-call", "static"};
    char name[96];
    DSD_SNPRINTF(name, sizeof name, "%s->%s%s%s%s%s", k_modes[tc->from], k_modes[tc->to], tc->new_dir ? " new-dir" : "",
                 tc->new_static ? " new-static" : "", tc->new_raw ? " new-raw" : "",
                 tc->fail == FAIL_PRIMARY ? " fail" : (tc->fail == FAIL_RAW ? " raw-fail" : ""));
    int rc = rig_open(&r, "dsdneo_rec_table_t");
    rc |= expect_true("folder B", dsd_test_mkdtemp(other, sizeof other, "dsdneo_rec_table_b") != NULL);
    rc |= expect_true("side folder", dsd_test_mkdtemp(side, sizeof side, "dsdneo_rec_table_s") != NULL);
    (void)dsd_test_path_join(s1, sizeof s1, side, "s1.wav");
    (void)dsd_test_path_join(s2, sizeof s2, side, "s2.wav");
    (void)dsd_test_path_join(r1, sizeof r1, side, "r1.wav");
    (void)dsd_test_path_join(r2, sizeof r2, side, "r2.wav");
    unopenable_path(&r, bad, sizeof bad);

    /* The from-state: the raw WAV R1 always runs, with audio; per-call slot 1 or the static S1 holds audio. */
    (void)dsd_app_command_set_string(DSD_APP_CMD_WAV_RAW_OPEN, r1);
    (void)drain(&r);
    short raw_block[480] = {0};
    if (r.opts->wav_out_raw) {
        (void)sf_write_short(r.opts->wav_out_raw, raw_block, 480);
    }
    if (tc->from == REC_PER_CALL) {
        submit(DSD_APP_CMD_WAV_START);
        (void)drain(&r);
        fill_voice_frame(r.state);
        dsd_write_mono_voice_to_slot_wav(r.opts, r.state, 0);
    } else if (tc->from == REC_STATIC) {
        rc |= open_static(&r, s1);
    }
    const int before_per_call = r.opts->dmr_stereo_wav;
    const int before_static = r.opts->static_wav_file;
    SNDFILE* const before_f = r.opts->wav_out_f;
    SNDFILE* const before_fr = r.opts->wav_out_fR;
    SNDFILE* const before_raw = r.opts->wav_out_raw;
    const dir_count before_a = count_dir(r.dir);

    if (tc->fail == FAIL_PRIMARY && tc->to == REC_PER_CALL) {
        dsd_wav_test_fail_opens_after(0);
    }
    static dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_recording = 1;
    cfg.per_call_wav = tc->to == REC_PER_CALL;
    DSD_SNPRINTF(cfg.per_call_wav_dir, sizeof cfg.per_call_wav_dir, "%s", tc->new_dir ? other : r.dir);
    if (tc->to == REC_STATIC) {
        DSD_SNPRINTF(cfg.static_wav_path, sizeof cfg.static_wav_path, "%s",
                     tc->fail == FAIL_PRIMARY ? bad : (tc->new_static ? s2 : s1));
    }
    DSD_SNPRINTF(cfg.raw_wav_path, sizeof cfg.raw_wav_path, "%s", tc->fail == FAIL_RAW ? bad : (tc->new_raw ? r2 : r1));
    r.state->ui_msg[0] = '\0';
    (void)dsd_app_command_apply_config(&cfg);
    (void)drain(&r);
    dsd_wav_test_fail_opens_after(-1);
    const dir_count a = count_dir(r.dir);
    const dir_count b = count_dir(other);

    if (tc->fail != FAIL_NONE) {
        DSD_SNPRINTF(tag, sizeof tag, "%s: nothing changed", name);
        rc |= expect_true(tag, r.opts->dmr_stereo_wav == before_per_call && r.opts->static_wav_file == before_static
                                   && r.opts->wav_out_f == before_f && r.opts->wav_out_fR == before_fr
                                   && r.opts->wav_out_raw == before_raw && strcmp(r.opts->wav_out_dir, r.dir) == 0
                                   && strcmp(r.opts->wav_out_file_raw, r1) == 0);
        DSD_SNPRINTF(tag, sizeof tag, "%s: the files on disk are as they were", name);
        rc |= expect_true(tag, a.temp == before_a.temp && a.published == before_a.published && b.files == 0
                                   && !file_exists(s2));
        DSD_SNPRINTF(tag, sizeof tag, "%s: the toast says so", name);
        rc |= expect_true(tag, strstr(r.state->ui_msg, "did not open") != NULL);
    } else {
        const int kept_per_call = tc->from == REC_PER_CALL && tc->to == REC_PER_CALL && !tc->new_dir;
        const int kept_static = tc->from == REC_STATIC && tc->to == REC_STATIC && !tc->new_static;
        DSD_SNPRINTF(tag, sizeof tag, "%s: the mode is the target's", name);
        rc |= expect_true(tag, r.opts->dmr_stereo_wav == (tc->to == REC_PER_CALL)
                                   && r.opts->static_wav_file == (tc->to == REC_STATIC));
        DSD_SNPRINTF(tag, sizeof tag, "%s: the folder is the target's", name);
        rc |= expect_true(tag, strcmp(r.opts->wav_out_dir, tc->new_dir ? other : r.dir) == 0);
        if (tc->to == REC_PER_CALL) {
            const char* dir = tc->new_dir ? other : r.dir;
            DSD_SNPRINTF(tag, sizeof tag, "%s: both slot files open in the target folder", name);
            rc |= expect_true(tag, r.opts->wav_out_f && r.opts->wav_out_fR
                                       && strstr(r.opts->wav_out_file, dir) == r.opts->wav_out_file
                                       && strstr(r.opts->wav_out_fileR, dir) == r.opts->wav_out_fileR);
            DSD_SNPRINTF(tag, sizeof tag, "%s: two slot files in the target folder", name);
            rc |= expect_int(tag, tc->new_dir ? b.temp : a.temp, 2);
        } else if (tc->to == REC_STATIC) {
            DSD_SNPRINTF(tag, sizeof tag, "%s: the static WAV open at its path", name);
            rc |= expect_true(tag, r.opts->wav_out_f != NULL && r.opts->wav_out_fR == NULL
                                       && strcmp(r.opts->wav_out_file, tc->new_static ? s2 : s1) == 0);
        } else {
            DSD_SNPRINTF(tag, sizeof tag, "%s: nothing records", name);
            rc |= expect_true(tag, r.opts->wav_out_f == NULL && r.opts->wav_out_fR == NULL);
        }
        if (kept_per_call || kept_static) {
            DSD_SNPRINTF(tag, sizeof tag, "%s: what did not change is untouched", name);
            rc |= expect_true(tag, r.opts->wav_out_f == before_f && r.opts->wav_out_fR == before_fr);
        }
        if (tc->from == REC_PER_CALL && !kept_per_call) {
            DSD_SNPRINTF(tag, sizeof tag, "%s: the old per-call files published in their own folder", name);
            rc |= expect_true(tag, a.published == 1 && a.temp == (tc->to == REC_PER_CALL && !tc->new_dir ? 2 : 0));
        }
        if (tc->from == REC_STATIC && !kept_static) {
            DSD_SNPRINTF(tag, sizeof tag, "%s: the old static WAV closed as one, where it was", name);
            rc |= expect_int(tag, wav_header_data_bytes(s1), 640);
            rc |= expect_int(tag, a.published + b.published, 0);
        }
        DSD_SNPRINTF(tag, sizeof tag, "%s: the raw WAV is the target's", name);
        rc |=
            expect_true(tag, r.opts->wav_out_raw != NULL && strcmp(r.opts->wav_out_file_raw, tc->new_raw ? r2 : r1) == 0
                                 && (tc->new_raw || r.opts->wav_out_raw == before_raw));
        if (tc->new_raw) {
            DSD_SNPRINTF(tag, sizeof tag, "%s: the old raw WAV closed", name);
            rc |= expect_int(tag, wav_header_data_bytes(r1), 960);
        }
    }
    if (r.opts->static_wav_file == 1) {
        r.opts->wav_out_f = close_wav_file(r.opts->wav_out_f);
    }
    rig_close(&r);
    remove_folder(other);
    remove_folder(side);
    return rc;
}

/* A live config that turns delete-after-upload on leaves it off in a run that names a recording journal, whose owner
   manages the recordings' retention; without a journal the config's choice stands. */
static int
test_config_delete_after_upload_yields_to_the_journal(void) {
    int rc = 0;
    for (int journaled = 0; journaled < 2; journaled++) {
        rig r;
        rc |= rig_open(&r, "dsdneo_rec_cfg_rdio");
        if (journaled) {
            DSD_SNPRINTF(r.opts->wav_journal_file, sizeof r.opts->wav_journal_file, "%s/recordings.jsonl", r.dir);
        }
        static dsdneoUserConfig cfg;
        DSD_MEMSET(&cfg, 0, sizeof cfg);
        cfg.has_recording = 1;
        DSD_SNPRINTF(cfg.per_call_wav_dir, sizeof cfg.per_call_wav_dir, "%s", r.dir);
        cfg.rdio_mode = DSD_RDIO_MODE_API;
        cfg.rdio_api_delete_after_upload = 1;
        rc |= expect_int("config queued", dsd_app_command_apply_config(&cfg), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int("config drained", drain(&r), 1);
        rc |= expect_int(journaled ? "journaled: delete-after-upload off" : "no journal: delete-after-upload on",
                         r.opts->rdio_api_delete_after_upload, journaled ? 0 : 1);
        rig_close(&r);
    }
    return rc;
}

/* A config whose only recording change is the raw WAV switches the raw WAV and leaves the per-call files alone: a slot
   waiting to reopen still waits for its call end, and no per-call open is tried (opens failing here would count one). */
static int
test_config_changing_only_the_raw_wav_leaves_a_waiting_slot(void) {
    rig r;
    char side[DSD_TEST_PATH_MAX];
    char r1[DSD_TEST_PATH_MAX * 2];
    char r2[DSD_TEST_PATH_MAX * 2];
    int rc = rig_open(&r, "dsdneo_rec_cfg_raw");
    rc |= expect_true("side folder", dsd_test_mkdtemp(side, sizeof side, "dsdneo_rec_cfg_raw_s") != NULL);
    (void)dsd_test_path_join(r1, sizeof r1, side, "r1.wav");
    (void)dsd_test_path_join(r2, sizeof r2, side, "r2.wav");
    (void)dsd_app_command_set_string(DSD_APP_CMD_WAV_RAW_OPEN, r1);
    (void)drain(&r);
    submit(DSD_APP_CMD_WAV_START);
    (void)drain(&r);
    SNDFILE* const slot1 = r.opts->wav_out_f;
    r.opts->wav_out_fR = close_wav_file(r.opts->wav_out_fR);
    (void)remove(r.opts->wav_out_fileR);
    r.opts->wav_out_fileR[0] = '\0';
    r.opts->wav_out_reopen = 2;
    dsd_wav_test_fail_opens_after(0);
    r.state->ui_msg[0] = '\0';

    static dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_recording = 1;
    cfg.per_call_wav = 1;
    DSD_SNPRINTF(cfg.per_call_wav_dir, sizeof cfg.per_call_wav_dir, "%s", r.dir);
    DSD_SNPRINTF(cfg.raw_wav_path, sizeof cfg.raw_wav_path, "%s", r2);
    rc |= expect_int("config queued", dsd_app_command_apply_config(&cfg), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("config drained", drain(&r), 1);
    dsd_wav_test_fail_opens_after(-1);

    dsd_wav_recording_status status;
    dsd_wav_recording_status_get(&status);
    rc |= expect_int("no per-call open tried", status.failures[DSD_WAV_RECORDING_ERROR_OPEN], 0);
    rc |= expect_true("no recording toast", strstr(r.state->ui_msg, "did not open") == NULL);
    rc |= expect_int("slot 2 still waits for its call end", r.opts->wav_out_reopen, 2);
    rc |= expect_true("slot 1 keeps its file", r.opts->wav_out_f == slot1 && r.opts->wav_out_fR == NULL);
    rc |= expect_int("per-call recording on", r.opts->dmr_stereo_wav, 1);
    rc |= expect_true("the raw WAV switched", r.opts->wav_out_raw != NULL && strcmp(r.opts->wav_out_file_raw, r2) == 0);
    rc |= expect_true("the new raw WAV is on disk", file_exists(r2));
    r.opts->wav_out_raw = close_wav_file(r.opts->wav_out_raw);
    rig_close(&r);
    remove_folder(side);
    return rc;
}

static int
test_recording_transition_table(void) {
    int rc = 0;
    int cases = 0;
    for (int from = REC_OFF; from <= REC_STATIC; from++) {
        for (int to = REC_OFF; to <= REC_STATIC; to++) {
            for (int variant = 0; variant < 2; variant++) {
                for (int new_raw = 0; new_raw < 2; new_raw++) {
                    for (int fail = FAIL_NONE; fail <= FAIL_RAW; fail++) {
                        transition_case tc = {from, to, 0, 0, new_raw, fail};
                        /* The variant moves the folder for per-call, names a new file for static; off has none. */
                        if (variant && to == REC_OFF) {
                            continue;
                        }
                        tc.new_dir = variant && to == REC_PER_CALL;
                        tc.new_static = variant && to == REC_STATIC;
                        const int primary_opens = (to == REC_PER_CALL && !(from == REC_PER_CALL && !tc.new_dir))
                                                  || (to == REC_STATIC && !(from == REC_STATIC && !tc.new_static));
                        if ((fail == FAIL_PRIMARY && !primary_opens) || (fail == FAIL_RAW && !new_raw)) {
                            continue;
                        }
                        rc |= run_transition_case(&tc);
                        cases++;
                    }
                }
            }
        }
    }
    return rc | expect_true("the table ran its cases", cases >= 40);
}

/* A static WAV whose file is not open (its open at start failed) is a component a raw open does not touch; a static
   open of its path opens it. */
static int
test_raw_open_leaves_a_static_wav_without_a_file(void) {
    rig r;
    char side[DSD_TEST_PATH_MAX];
    char s1[DSD_TEST_PATH_MAX * 2];
    char r1[DSD_TEST_PATH_MAX * 2];
    int rc = rig_open(&r, "dsdneo_rec_static_nofile");
    rc |= expect_true("side folder", dsd_test_mkdtemp(side, sizeof side, "dsdneo_rec_static_nf") != NULL);
    (void)dsd_test_path_join(s1, sizeof s1, side, "s1.wav");
    (void)dsd_test_path_join(r1, sizeof r1, side, "r1.wav");
    r.opts->static_wav_file = 1;
    DSD_SNPRINTF(r.opts->wav_out_file, sizeof r.opts->wav_out_file, "%s", s1);
    (void)dsd_app_command_set_string(DSD_APP_CMD_WAV_RAW_OPEN, r1);
    (void)drain(&r);
    rc |= expect_true("the raw WAV opens", r.opts->wav_out_raw != NULL);
    rc |= expect_true("the static WAV is left alone", r.opts->wav_out_f == NULL && !file_exists(s1));
    rc |= expect_int("static mode kept", r.opts->static_wav_file, 1);
    (void)dsd_app_command_set_string(DSD_APP_CMD_WAV_STATIC_OPEN, s1);
    (void)drain(&r);
    rc |= expect_true("a static open of its path opens it", r.opts->wav_out_f != NULL && file_exists(s1));
    r.opts->wav_out_f = close_wav_file(r.opts->wav_out_f);
    rig_close(&r);
    remove_folder(side);
    return rc;
}

/* The two failures re-review 2 traced: a config turning per-call on from a static WAV whose per-call open fails, and a
   folder move whose new pair fails to open, each change nothing. */
static int
test_config_per_call_failures_change_nothing(void) {
    int rc = 0;
    const transition_case from_static = {REC_STATIC, REC_PER_CALL, 0, 0, 0, FAIL_PRIMARY};
    const transition_case moved = {REC_PER_CALL, REC_PER_CALL, 1, 0, 0, FAIL_PRIMARY};
    rc |= run_transition_case(&from_static);
    rc |= run_transition_case(&moved);
    return rc;
}

/* More segments open at once than the table holds is reported once per run, like the failures. */
static int
test_full_segment_table_is_reported_once_per_run(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_table");

    enum { OPENED = 18 };

    for (int run = 0; run < 2; run++) {
        static SNDFILE* files[OPENED];
        static char names[OPENED][DSD_TEST_PATH_MAX * 2];
        dsd_wav_recording_session_reset();
        for (int i = 0; i < OPENED; i++) {
            files[i] = open_wav_file(r.dir, names[i], sizeof names[i], 8000, 0);
        }
        for (int i = 0; i < OPENED; i++) {
            files[i] = close_wav_file(files[i]);
            (void)remove(names[i]);
        }
        rc |= expect_int(run == 0 ? "the first run reports a full table once" : "the next run reports it again",
                         g_table_full_warnings, run + 1);
    }
    rig_close(&r);
    return rc;
}

/* An NFM recording stopped after the folder moved is published in the folder it was opened in. */
static int
test_stop_publishes_the_nfm_recording_in_its_folder(void) {
    rig r;
    char other[DSD_TEST_PATH_MAX];
    int rc = rig_open(&r, "dsdneo_rec_nfm_dir");
    rc |= expect_true("second folder", dsd_test_mkdtemp(other, sizeof other, "dsdneo_rec_nfm_other") != NULL);
    setup_nfm(r.opts, r.state);
    submit(DSD_APP_CMD_WAV_START);
    (void)drain(&r);
    nfm_block(r.opts, r.state);
    DSD_SNPRINTF(r.opts->wav_out_dir, sizeof r.opts->wav_out_dir, "%s", other);
    submit(DSD_APP_CMD_WAV_STOP);
    (void)drain(&r);
    const dir_count c = count_dir(r.dir);
    rc |= expect_true("published in its own folder", c.published == 1 && strstr(c.published_name, "_NFM") != NULL);
    rc |= expect_int("nothing in the new folder", count_dir(other).files, 0);
    rig_close(&r);
    remove_folder(other);
    return rc;
}

/* A config whose [recording] turns per-call recording off closes the slot files with their publication. Main changed
   the mode alone: the files stayed open and rotating, and the next start overwrote their handles. */
static int
test_config_turning_per_call_off_closes_the_slot_files(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_cfg_off");
    submit(DSD_APP_CMD_WAV_START);
    (void)drain(&r);
    fill_voice_frame(r.state);
    dsd_write_mono_voice_to_slot_wav(r.opts, r.state, 0);
    rc |= apply_recording_config(&r, 0, NULL);
    rc |= expect_int("per-call recording off", r.opts->dmr_stereo_wav, 0);
    rc |= expect_true("the slot files closed", r.opts->wav_out_f == NULL && r.opts->wav_out_fR == NULL);
    dir_count c = count_dir(r.dir);
    rc |= expect_int("no temp file left", c.temp, 0);
    rc |= expect_int("slot 1's recording published", c.published, 1);
    submit(DSD_APP_CMD_WAV_TOGGLE);
    (void)drain(&r);
    c = count_dir(r.dir);
    rc |= expect_int("the toggle opens one new pair", c.temp, 2);
    rig_close(&r);
    return rc;
}

/* A config whose [recording] turns per-call recording on while a static WAV records closes the static file as one. Main
   left it in slot 1, so the next call end published the operator's file into the per-call folder. */
static int
test_config_turning_per_call_on_closes_the_static_wav(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_cfg_on");
    char static_path[DSD_TEST_PATH_MAX];
    char static_dir[DSD_TEST_PATH_MAX];
    rc |= expect_true("static dir", dsd_test_mkdtemp(static_dir, sizeof static_dir, "dsdneo_rec_cfg_s") != NULL);
    rc |= expect_int("static path", dsd_test_path_join(static_path, sizeof static_path, static_dir, "static.wav"), 0);
    rc |= open_static(&r, static_path);
    rc |= apply_recording_config(&r, 1, NULL);
    rc |= expect_int("per-call recording on", r.opts->dmr_stereo_wav, 1);
    rc |= expect_int("static mode off", r.opts->static_wav_file, 0);
    rc |= expect_true("both slot files open", r.opts->wav_out_f != NULL && r.opts->wav_out_fR != NULL);
    rc |= expect_int("the static file closed as one", wav_header_data_bytes(static_path), 640);
    end_p25_call(&r, 200U);
    rc |= expect_int("the static file stays where it was", wav_header_data_bytes(static_path), 640);
    rc |= expect_int("no published recording", count_dir(r.dir).published, 0);
    (void)remove(static_path);
    (void)dsd_test_rmdir(static_dir);
    rig_close(&r);
    return rc;
}

/* A config naming a static WAV that does not open leaves per-call recording running and fails the apply. */
static int
test_config_static_wav_that_fails_keeps_the_recording(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_cfg_fail");
    submit(DSD_APP_CMD_WAV_START);
    (void)drain(&r);
    SNDFILE* left = r.opts->wav_out_f;
    char bad[DSD_TEST_PATH_MAX * 2];
    unopenable_path(&r, bad, sizeof bad);
    rc |= apply_recording_config(&r, 0, bad);
    rc |= expect_int("per-call recording stays on", r.opts->dmr_stereo_wav, 1);
    rc |= expect_int("static mode stays off", r.opts->static_wav_file, 0);
    rc |= expect_true("slot 1 keeps recording", r.opts->wav_out_f == left);
    rc |= expect_true("the toast says so", strstr(r.state->ui_msg, "did not open") != NULL);
    rig_close(&r);
    return rc;
}

/* A recording command with a malformed payload reads as failed at once, not pending until some later publication. */
static int
test_invalid_recording_command_reads_as_failed(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_invalid");
    const int32_t junk = 1;
    rc |= expect_int("malformed start queued", dsd_app_command_submit(DSD_APP_CMD_WAV_START, &junk, sizeof junk),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    (void)drain(&r);
    const dsd_app_recording_view v = view_now();
    rc |= expect_int("the malformed start reads as failed", v.command_state, DSD_APP_RECORDING_COMMAND_FAILED);
    rc |= expect_int("nothing opened", count_dir(r.dir).files, 0);
    rig_close(&r);
    return rc;
}

/* A config carrying [recording] is a recording command: the view reports its recording's outcome, so a start that
   failed earlier no longer reads as the state of a recording the config turned on. A config without [recording] leaves
   the view's command state alone. Main left the earlier command's state in the view. */
static int
test_config_recording_reports_its_outcome(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_cfg_view");
    dsd_wav_test_fail_opens_after(0);
    submit(DSD_APP_CMD_WAV_START);
    (void)drain(&r);
    rc |= expect_int("the start failed", view_now().command_state, DSD_APP_RECORDING_COMMAND_FAILED);
    dsd_wav_test_fail_opens_after(-1);

    static dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_recording = 1;
    cfg.per_call_wav = 1;
    DSD_SNPRINTF(cfg.per_call_wav_dir, sizeof cfg.per_call_wav_dir, "%s", r.dir);
    rc |= expect_int("config queued", dsd_app_command_apply_config(&cfg), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("the config's recording pends", view_now().command_state, DSD_APP_RECORDING_COMMAND_PENDING);
    (void)drain(&r);
    dsd_app_recording_view v = view_now();
    rc |= expect_int("the config's recording applied", v.command_state, DSD_APP_RECORDING_COMMAND_APPLIED);
    rc |= expect_int("the config is the command shown", v.command_id, DSD_APP_CMD_CONFIG_APPLY);
    rc |= expect_int("per-call recording on", v.mode, DSD_APP_RECORDING_MODE_PER_CALL);

    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_alerts = 1;
    rc |= expect_int("a config without [recording] queued", dsd_app_command_apply_config(&cfg),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("it is no recording command", view_now().command_state, DSD_APP_RECORDING_COMMAND_APPLIED);
    (void)drain(&r);
    v = view_now();
    rc |= expect_int("the recording's state is kept", v.command_state, DSD_APP_RECORDING_COMMAND_APPLIED);
    rc |= expect_int("still the config's", v.command_id, DSD_APP_CMD_CONFIG_APPLY);
    rc |= expect_int("still per-call", v.mode, DSD_APP_RECORDING_MODE_PER_CALL);

    char other[DSD_TEST_PATH_MAX];
    rc |= expect_true("second folder", dsd_test_mkdtemp(other, sizeof other, "dsdneo_rec_cfg_view_dir") != NULL);
    dsd_wav_test_fail_opens_after(0);
    rc |= apply_recording_config_in(&r, 1, NULL, other);
    dsd_wav_test_fail_opens_after(-1);
    v = view_now();
    rc |=
        expect_int("a config whose recording fails reads as failed", v.command_state, DSD_APP_RECORDING_COMMAND_FAILED);
    rc |= expect_int("the failed config is the command shown", v.command_id, DSD_APP_CMD_CONFIG_APPLY);
    rig_close(&r);
    remove_folder(other);
    return rc;
}

#ifdef DSD_TEST_WRAP_SIGNAL_SEARCH
/* Signal search, as the queue sees it: the link wraps the engine's answer so a case can run with a search active. */
static int g_signal_search_active;

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol name.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __real_dsd_engine_signal_search_active(const dsd_state* state);
int __wrap_dsd_engine_signal_search_active(const dsd_state* state);

int
__wrap_dsd_engine_signal_search_active(const dsd_state* state) {
    return g_signal_search_active || __real_dsd_engine_signal_search_active(state);
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
#endif

/* Record only unknown talkgroups (issue #708): the switch is a recording command, pending until the view that shows it
   applied; a value other than 0 or 1 fails and changes nothing; a config's [recording] sets it too. */
static int
test_unknown_only_set_applies_and_reports(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_unknown_only");
    rc |= expect_int("unknown-only queued", dsd_app_command_set_i32(DSD_APP_CMD_RECORD_UNKNOWN_ONLY_SET, 1),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    dsd_app_recording_view v = view_now();
    rc |= expect_int("unknown-only pends", v.command_state, DSD_APP_RECORDING_COMMAND_PENDING);
    rc |= expect_int("pending unknown-only named", v.command_id, DSD_APP_CMD_RECORD_UNKNOWN_ONLY_SET);
    rc |= expect_int("not applied before the drain", r.opts->record_unknown_only, 0);
    rc |= expect_int("unknown-only drained", drain(&r), 1);
    v = view_now();
    rc |= expect_int("unknown-only applied", v.command_state, DSD_APP_RECORDING_COMMAND_APPLIED);
    rc |= expect_int("the switch is on", r.opts->record_unknown_only, 1);
    rc |= expect_int("the view shows it", v.unknown_only, 1);
    rc |= expect_int("recording itself stays off", v.mode, DSD_APP_RECORDING_MODE_OFF);

    (void)dsd_app_command_set_i32(DSD_APP_CMD_RECORD_UNKNOWN_ONLY_SET, 2);
    (void)drain(&r);
    v = view_now();
    rc |= expect_int("a value out of range fails", v.command_state, DSD_APP_RECORDING_COMMAND_FAILED);
    rc |= expect_int("and changes nothing", r.opts->record_unknown_only, 1);
    rc |= expect_int("the view still shows it", v.unknown_only, 1);

    (void)dsd_app_command_set_i32(DSD_APP_CMD_RECORD_UNKNOWN_ONLY_SET, 0);
    (void)drain(&r);
    v = view_now();
    rc |= expect_int("off applied", v.command_state, DSD_APP_RECORDING_COMMAND_APPLIED);
    rc |= expect_int("the switch is off", r.opts->record_unknown_only, 0);
    rc |= expect_int("the view shows it off", v.unknown_only, 0);

    static dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_recording = 1;
    cfg.unknown_talkgroups_only = 1;
    DSD_SNPRINTF(cfg.per_call_wav_dir, sizeof cfg.per_call_wav_dir, "%s", r.dir);
    rc |= expect_int("config queued", dsd_app_command_apply_config(&cfg), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("config drained", drain(&r), 1);
    rc |= expect_int("a config's [recording] sets the switch", r.opts->record_unknown_only, 1);
    rc |= expect_int("and the view shows it", view_now().unknown_only, 1);
    rig_close(&r);
    return rc;
}

#ifdef DSD_TEST_WRAP_SIGNAL_SEARCH
/* While Signal search runs, the unknown-only switch is refused like the WAV commands, with the search's notice, and
   changes nothing; once the search stops it applies. */
static int
test_unknown_only_set_refused_during_signal_search(void) {
    rig r;
    int rc = rig_open(&r, "dsdneo_rec_unknown_only_search");
    if (rc != 0) {
        rig_close(&r);
        return rc;
    }
    g_signal_search_active = 1;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_RECORD_UNKNOWN_ONLY_SET, 1);
    (void)drain(&r);
    dsd_app_recording_view v = view_now();
    rc |= expect_int("unknown-only refused during signal search", v.command_state, DSD_APP_RECORDING_COMMAND_REFUSED);
    rc |= expect_int("refused unknown-only named", v.command_id, DSD_APP_CMD_RECORD_UNKNOWN_ONLY_SET);
    rc |= expect_int("the switch stays off", r.opts->record_unknown_only, 0);
    rc |= expect_true("the search's notice", strstr(r.state->ui_msg, "Stop Signal search") != NULL);
    submit(DSD_APP_CMD_WAV_START);
    (void)drain(&r);
    v = view_now();
    rc |= expect_int("a WAV start is refused the same way", v.command_state, DSD_APP_RECORDING_COMMAND_REFUSED);
    rc |= expect_int("and records nothing", r.opts->dmr_stereo_wav, 0);
    rc |= expect_int("nothing opened", count_dir(r.dir).files, 0);
    g_signal_search_active = 0;
    (void)dsd_app_command_set_i32(DSD_APP_CMD_RECORD_UNKNOWN_ONLY_SET, 1);
    (void)drain(&r);
    rc |= expect_int("applied once the search stops", view_now().command_state, DSD_APP_RECORDING_COMMAND_APPLIED);
    rc |= expect_int("the switch is on", r.opts->record_unknown_only, 1);
    rig_close(&r);
    return rc;
}
#endif

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
    rc |= test_failed_static_open_keeps_the_running_recording();
    rc |= test_failed_raw_open_keeps_the_running_raw_wav();
    rc |= test_start_closes_handles_the_mode_does_not_cover();
    rc |= test_config_turning_per_call_off_closes_the_slot_files();
    rc |= test_config_turning_per_call_on_closes_the_static_wav();
    rc |= test_config_static_wav_that_fails_keeps_the_recording();
    rc |= test_invalid_recording_command_reads_as_failed();
    rc |= test_config_moving_the_folder_publishes_in_the_old_one();
    rc |= test_config_moving_the_folder_and_stopping_publishes_in_the_old_one();
    rc |= test_config_that_changes_no_recording_leaves_it();
    rc |= test_config_changing_only_the_raw_wav_leaves_a_waiting_slot();
    rc |= test_config_delete_after_upload_yields_to_the_journal();
    rc |= test_stop_publishes_the_nfm_recording_in_its_folder();
    rc |= test_full_segment_table_is_reported_once_per_run();
    rc |= test_recording_transition_table();
    rc |= test_config_per_call_failures_change_nothing();
    rc |= test_raw_open_leaves_a_static_wav_without_a_file();
    rc |= test_unknown_only_set_applies_and_reports();
    rc |= test_config_recording_reports_its_outcome();
#ifdef DSD_TEST_WRAP_SIGNAL_SEARCH
    rc |= test_unknown_only_set_refused_during_signal_search();
#endif
    if (rc == 0) {
        printf("APP_CONTROL_RECORDING: OK\n");
    }
    return rc;
}
