// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #661: a recording never lands on a name another one holds. Capture names (date, time, a 16-bit nonce) and
 * per-call WAV final names (date, time, a 16-bit number, the call) can repeat within a second; the writer creates a
 * capture exclusively and publishes a per-call WAV without replacing anything, each drawing another number when the
 * name is taken, and per-call WAV temp names are unique by construction. GNU ld --wrap pins the nonce and the decode
 * time so the colliding names are known in advance.
 */

#include <assert.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/platform/file_compat.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

/* The decode time every name is stamped with, and the numbers dsd_nonce_u16() hands out in turn. */
static const time_t k_decode_time = (time_t)1700000000;
static uint16_t g_nonces[8];
static int g_nonce_count;
static int g_nonce_pos;
static const uint16_t k_nonce_after_script = 0x7777U;

// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage): ld --wrap names.
uint16_t __wrap_dsd_nonce_u16(void);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage): ld --wrap names.
time_t __wrap_dsd_decode_time(void);

uint16_t
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage): ld --wrap names.
__wrap_dsd_nonce_u16(void) {
    return g_nonce_pos < g_nonce_count ? g_nonces[g_nonce_pos++] : k_nonce_after_script;
}

time_t
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage): ld --wrap names.
__wrap_dsd_decode_time(void) {
    return k_decode_time;
}

static void
script_nonces(const uint16_t* values, int count) {
    assert(count <= (int)(sizeof g_nonces / sizeof g_nonces[0]));
    for (int i = 0; i < count; i++) {
        g_nonces[i] = values[i];
    }
    g_nonce_count = count;
    g_nonce_pos = 0;
}

static void
decode_stamp(char datestr[9], char timestr[7]) {
    (void)dsd_format_local_datetime(k_decode_time, DSD_LOCAL_DATETIME_DATE_COMPACT, datestr, 9);
    (void)dsd_format_local_datetime(k_decode_time, DSD_LOCAL_DATETIME_TIME_COMPACT, timestr, 7);
}

static void
write_text(const char* path, const char* text) {
    FILE* f = dsd_fopen_private(path, "wb");
    assert(f != NULL);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static int
text_is(const char* path, const char* text) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    char buf[64];
    size_t n = fread(buf, 1, sizeof buf - 1U, f);
    buf[n] = '\0';
    fclose(f);
    return strcmp(buf, text) == 0;
}

static long
size_of(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    (void)fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    return size;
}

static int
remove_entry(const char* name, void* user) {
    char path[DSD_TEST_PATH_MAX * 2];
    DSD_SNPRINTF(path, sizeof path, "%s/%s", (const char*)user, name);
    (void)remove(path);
    return 0;
}

static void
remove_dir_tree(const char* dir) {
    (void)dsd_dir_list(dir, remove_entry, (void*)dir);
    (void)dsd_test_rmdir(dir);
}

/* A capture whose first name is taken (the same second, the same nonce) is created under another number, and the file
   that held the name keeps its bytes. Driven through SDRTrunk JSON playback, which captures its voice frames. */
static void
test_capture_never_truncates_an_existing_file(void) {
    char dir[DSD_TEST_PATH_MAX];
    assert(dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_capture_collision") != NULL);
    char datestr[9];
    char timestr[7];
    decode_stamp(datestr, timestr);
    char taken[DSD_TEST_PATH_MAX * 2];
    char fresh[DSD_TEST_PATH_MAX * 2];
    DSD_SNPRINTF(taken, sizeof taken, "%s/%s_%s_1234_S1.amb", dir, datestr, timestr);
    DSD_SNPRINTF(fresh, sizeof fresh, "%s/%s_%s_5678_S1.amb", dir, datestr, timestr);
    write_text(taken, "keep");

    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(history, 0, sizeof history);
    DSD_SNPRINTF(opts.mbe_out_dir, sizeof opts.mbe_out_dir, "%s", dir);
    opts.playfiles = 1;
    opts.floating_point = 1;
    state.event_history_s = history;
    /* The gain stage's output buffers: JSON playback runs it for every frame in either output format (issue #652). */
    static short out_buf[100 + (160 * 64)];
    state.audio_out_buf = out_buf;
    state.audio_out_buf_p = out_buf + 100;
    state.aout_max_buf_p = state.aout_max_buf;

    static const char json[] =
        "{\"version\":\"2\",\"protocol\":\"DMR\",\"call_type\":\"GROUP\",\"encrypted\":\"false\","
        "\"hex\":\"000000000000000000\"}";
    FILE* in = tmpfile();
    assert(in != NULL);
    assert(fwrite(json, 1, strlen(json), in) == strlen(json));
    /* Past the four-byte cookie openMbeInFile() reads. */
    assert(fseek(in, 4L, SEEK_SET) == 0);
    opts.mbe_in_f = in;
    const uint16_t nonces[] = {0x1234U, 0x5678U};
    script_nonces(nonces, 2);
    read_sdrtrunk_json_format(&opts, &state);
    fclose(in);
    opts.mbe_in_f = NULL;

    assert(text_is(taken, "keep"));
    assert(size_of(fresh) == 4 + 8);
    dsd_state_ext_free_all(&state);
    remove_dir_tree(dir);
}

/* A per-call WAV whose final name is taken is published under another number, never over the recording that holds
   the name. */
static void
test_call_wav_publication_never_replaces_a_recording(void) {
    char dir[DSD_TEST_PATH_MAX];
    assert(dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_wav_collision") != NULL);
    char datestr[9];
    char timestr[7];
    decode_stamp(datestr, timestr);
    char temp[DSD_TEST_PATH_MAX * 2];
    char taken[DSD_TEST_PATH_MAX * 2];
    char fresh[DSD_TEST_PATH_MAX * 2];
    DSD_SNPRINTF(temp, sizeof temp, "%s/TEMP_call.wav", dir);
    DSD_SNPRINTF(taken, sizeof taken, "%s/%s_%s_%05u__GROUP_TGT_0_SRC_0.wav", dir, datestr, timestr, 0x1234U);
    DSD_SNPRINTF(fresh, sizeof fresh, "%s/%s_%s_%05u__GROUP_TGT_0_SRC_0.wav", dir, datestr, timestr, 0x5678U);
    write_text(taken, "keep");

    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof info);
    info.samplerate = 8000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* wav = sf_open(temp, SFM_WRITE, &info);
    assert(wav != NULL);
    short samples[160];
    for (int i = 0; i < 160; i++) {
        samples[i] = (short)(i * 10);
    }
    assert(sf_write_short(wav, samples, 160) == 160);

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    const uint16_t nonces[] = {0x1234U, 0x5678U};
    script_nonces(nonces, 2);
    assert(close_and_rename_wav_file(wav, &opts, temp, dir, NULL, 0) == NULL);

    assert(text_is(taken, "keep"));
    assert(size_of(fresh) > 44);
    assert(size_of(temp) < 0);
    remove_dir_tree(dir);
}

/* Two per-call WAV temp files opened in the same second, whatever the nonce, never share a name: libsndfile's
   SFM_RDWR would append the second call to the first. */
static void
test_call_wav_temp_names_never_repeat(void) {
    char dir[DSD_TEST_PATH_MAX];
    assert(dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_wav_temp") != NULL);
    const uint16_t nonces[] = {0x1234U, 0x1234U, 0x1234U, 0x1234U};
    script_nonces(nonces, 4);
    char first[1024];
    char second[1024];
    SNDFILE* a = open_wav_file(dir, first, sizeof first, 8000, 0);
    SNDFILE* b = open_wav_file(dir, second, sizeof second, 8000, 0);
    assert(a != NULL && b != NULL);
    assert(strcmp(first, second) != 0);
    (void)close_wav_file(a);
    (void)close_wav_file(b);
    remove_dir_tree(dir);
}

int
main(void) {
    test_capture_never_truncates_an_existing_file();
    test_call_wav_publication_never_replaces_a_recording();
    test_call_wav_temp_names_never_repeat();
    printf("CORE_MBE_OUT_COLLISION: OK\n");
    return 0;
}
