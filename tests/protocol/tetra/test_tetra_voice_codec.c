// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A codec library dsd-neo cannot use costs TETRA speech, never the decoding: the library is refused with one error,
 * nothing plays, and the speech still counts as reception. Covered: no --tetra-codec, a path with no library, a
 * library of another interface version, one missing a function, one that cannot make a decoder, and one whose
 * decodes fail.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/tetra/tetra_codec_abi.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "tetra_state.h"
#include "tetra_voice_state.h"

#if !defined(TETRA_TEST_CODEC_WRONG_ABI) || !defined(TETRA_TEST_CODEC_MISSING_SYMBOL)                                  \
    || !defined(TETRA_TEST_CODEC_NO_CREATE) || !defined(TETRA_TEST_CODEC_FAIL_DECODE)
#error the test codec library variants are required
#endif

static int g_fail = 0;

static void
expect(int ok, const char* what) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static size_t
count_occurrences(const char* text, const char* needle) {
    size_t n = 0;
    for (const char* p = strstr(text, needle); p; p = strstr(p + 1, needle)) {
        n++;
    }
    return n;
}

typedef struct {
    const char* name;
    const char* path; /* NULL: no --tetra-codec */
    const char* log;  /* what its error or notice says */
    size_t lines;     /* TETRA log lines: the one error or notice, and a "loaded" line for a library it accepted */
    uint32_t codec_errors;
    uint8_t status; /* tetra_codec_status after the bursts */
} codec_case;

static void
run_case(const codec_case* c) {
    char label[1024];
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    char out_path[DSD_TEST_PATH_MAX];
    const int out_fd = dsd_test_mkstemp(out_path, sizeof(out_path), "tetra_codec_out");
    if (!opts || !state || out_fd < 0) {
        expect(0, "fixture");
        free(opts);
        free(state);
        return;
    }
    initOpts(opts);
    initState(state);
    opts->audio_out = 1;
    opts->audio_out_type = 1;
    opts->audio_out_fd = out_fd;
    opts->pulse_digi_rate_out = 8000;
    opts->pulse_digi_out_channels = 1;
    if (c->path) {
        DSD_SNPRINTF(opts->tetra_codec_path, sizeof(opts->tetra_codec_path), "%s", c->path);
    }
    state->last_vc_sync_time = 0;
    state->last_vc_sync_time_m = 0.0;
    /* The bursts are TN1's, as a BSCH set the TDMA clock: the receiver hears speech the clock places. */
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        expect(0, "TETRA state");
        (void)dsd_close(out_fd);
        (void)remove(out_path);
        freeState(state);
        free(state);
        free(opts);
        return;
    }
    ts->tdma_valid = 1;
    ts->tn = 1;

    uint8_t bits[TETRA_SPEECH_FULL_BITS];
    DSD_MEMSET(bits, 0, sizeof(bits));
    dsd_test_capture_stderr cap;
    char log[4096];
    log[0] = '\0';
    if (dsd_test_capture_stderr_begin(&cap, "tetra_codec_log") == 0) {
        for (int i = 0; i < 3; i++) {
            tetra_voice_full_slot(opts, state, bits, 1);
        }
        (void)dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, log, sizeof(log));
    }

    const tetra_voice_t* v = &tetra_st(state)->voice;
    DSD_SNPRINTF(label, sizeof(label), "%s: status %u, expected %u", c->name, (unsigned)v->codec_status,
                 (unsigned)c->status);
    expect(v->codec_status == c->status, label);
    DSD_SNPRINTF(label, sizeof(label), "%s: %zu log line(s), one naming \"%s\", got:\n%s", c->name, c->lines, c->log,
                 log);
    expect(count_occurrences(log, "TETRA:") == c->lines && strstr(log, c->log) != NULL, label);
    DSD_SNPRINTF(label, sizeof(label), "%s: %u codec errors", c->name, (unsigned)v->codec_errors);
    expect(v->codec_errors == c->codec_errors && v->frames == 0, label);
    DSD_SNPRINTF(label, sizeof(label), "%s: the speech is still reception", c->name);
    expect(state->last_vc_sync_time != 0 && state->last_vc_sync_time_m > 0.0, label);

    int16_t out[16];
    FILE* in = fopen(out_path, "rb");
    const size_t written = in ? fread(out, sizeof(int16_t), 16, in) : 0U;
    if (in) {
        (void)fclose(in);
    }
    DSD_SNPRINTF(label, sizeof(label), "%s: nothing plays", c->name);
    expect(written == 0, label);

    (void)dsd_close(out_fd);
    (void)remove(out_path);
    freeState(state);
    free(state);
    free(opts);
}

int
main(void) {
    const codec_case cases[] = {
        {"no codec", NULL, "no codec library is loaded", 1, 0, TETRA_CODEC_STATUS_NONE},
        {"no such file", "dsd_neo_no_such_tetra_codec_library", "refused", 1, 0, TETRA_CODEC_STATUS_FAILED},
        {"wrong version", TETRA_TEST_CODEC_WRONG_ABI, "codec interface version 2", 1, 0, TETRA_CODEC_STATUS_FAILED},
        {
            "missing function",
            TETRA_TEST_CODEC_MISSING_SYMBOL,
            DSD_TETRA_CODEC_SYM_DESTROY,
            1,
            0,
            TETRA_CODEC_STATUS_FAILED,
        },
        {"no decoder", TETRA_TEST_CODEC_NO_CREATE, "could not create a decoder", 1, 0, TETRA_CODEC_STATUS_FAILED},
        {"failing decodes", TETRA_TEST_CODEC_FAIL_DECODE, "failed to decode", 2, 6, TETRA_CODEC_STATUS_READY},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_case(&cases[i]);
    }
    if (g_fail) {
        DSD_FPRINTF(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    puts("PASS test_tetra_voice_codec");
    return 0;
}
