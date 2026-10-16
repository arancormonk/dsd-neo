// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The TETRA speech decoder behind the speech path. mbelib-neo's decoder plays exactly what it makes of the speech
 * frames, in order, with the stolen and CRC-failed frames concealed, and a carrier boundary resets it. A decoder that
 * cannot be made or whose decodes fail costs the speech, never the decoding: one error, nothing plays, and the speech
 * still counts as reception.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>
#include <mbelib-neo/mbelib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "tetra_state.h"
#include "tetra_test_codec.h"
#include "tetra_voice_state.h"

#define MAX_OUT ((size_t)16 * TETRA_VOICE_FRAME_SAMPLES)

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
    dsd_opts* opts;
    dsd_state* state;
    char out_path[DSD_TEST_PATH_MAX];
    int out_fd;
} fixture;

static int
fixture_open(fixture* f, const tetra_voice_codec* codec) {
    DSD_MEMSET(f, 0, sizeof(*f));
    f->out_fd = -1;
    tetra_voice_set_codec_for_test(codec);
    f->opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    f->state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!f->opts || !f->state) {
        return -1;
    }
    initOpts(f->opts);
    initState(f->state);
    f->out_fd = dsd_test_mkstemp(f->out_path, sizeof(f->out_path), "tetra_codec_out");
    if (f->out_fd < 0) {
        return -1;
    }
    /* Raw 16-bit mono at 8 kHz, unity gain, no high-pass: the output carries the decoder's samples unchanged. */
    f->opts->audio_out = 1;
    f->opts->audio_out_type = 1;
    f->opts->audio_out_fd = f->out_fd;
    f->opts->pulse_digi_rate_out = 8000;
    f->opts->pulse_digi_out_channels = 1;
    f->opts->floating_point = 0;
    f->opts->audio_gain = -1.0F;
    f->opts->use_hpf_d = 0;
    f->opts->slot1_on = 1;
    f->state->last_vc_sync_time = 0;
    f->state->last_vc_sync_time_m = 0.0;
    /* The bursts are TN1's, as a BSCH set the TDMA clock: the receiver hears speech the clock places. */
    tetra_state_t* const ts = tetra_st(f->state);
    if (!ts) {
        return -1;
    }
    ts->tdma_valid = 1;
    ts->tn = 1;
    return 0;
}

static void
fixture_close(fixture* f) {
    if (f->out_fd >= 0) {
        (void)dsd_close(f->out_fd);
        (void)remove(f->out_path);
    }
    if (f->state) {
        freeState(f->state);
    }
    free(f->state);
    free(f->opts);
    tetra_voice_set_codec_for_test(NULL);
}

static size_t
fixture_output(const fixture* f, int16_t* samples, size_t max) {
    FILE* in = fopen(f->out_path, "rb");
    if (!in) {
        return 0;
    }
    const size_t n = fread(samples, sizeof(int16_t), max, in);
    (void)fclose(in);
    return n;
}

/* A recognisable bit pattern per burst. */
static void
burst_bits(uint8_t* bits, int n, unsigned seed) {
    for (int i = 0; i < n; i++) {
        bits[i] = (uint8_t)(((((unsigned)i * 7U) + seed) >> 2) & 1U);
    }
}

/* Decode @p count frames with a fresh mbelib-neo decoder, as the speech path should have. */
static int
reference_decode(const uint8_t* const* frames, const int* bad, size_t count, int16_t* pcm) {
    mbe_tetra_decoder* dec = mbe_tetraDecoderAlloc();
    if (!dec) {
        return -1;
    }
    int rc = 0;
    for (size_t i = 0; i < count && rc == 0; i++) {
        rc = mbe_tetraDecodeFrame(dec, bad[i] ? NULL : frames[i], bad[i], pcm + (i * TETRA_VOICE_FRAME_SAMPLES));
    }
    mbe_tetraDecoderFree(dec);
    return rc;
}

static int
any_nonzero(const int16_t* pcm, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (pcm[i] != 0) {
            return 1;
        }
    }
    return 0;
}

/* mbelib-neo's decoder, the one a session uses: a good burst's two frames, a CRC-failed burst concealed, a half slot
   whose stolen first frame is concealed, and after a carrier boundary the speech of a decoder reset. */
static void
test_mbelib_decoder_plays_the_speech(void) {
    fixture f;
    if (fixture_open(&f, NULL) != 0) {
        expect(0, "mbelib: fixture");
        fixture_close(&f);
        return;
    }
    uint8_t b1[TETRA_SPEECH_FULL_BITS];
    uint8_t b2[TETRA_SPEECH_FULL_BITS];
    uint8_t h1[TETRA_SPEECH_HALF_BITS];
    uint8_t s1[2 * TETRA_VOICE_FRAME_BITS];
    uint8_t s2[2 * TETRA_VOICE_FRAME_BITS];
    uint8_t sh[TETRA_VOICE_FRAME_BITS];
    burst_bits(b1, TETRA_SPEECH_FULL_BITS, 1U);
    burst_bits(b2, TETRA_SPEECH_FULL_BITS, 9U);
    burst_bits(h1, TETRA_SPEECH_HALF_BITS, 4U);
    tetra_voice_reorder_full(b1, s1);
    tetra_voice_reorder_full(b2, s2);
    tetra_voice_reorder_half(h1, sh);

    tetra_voice_full_slot(f.opts, f.state, b1, 1);
    tetra_voice_full_slot(f.opts, f.state, b2, 0);
    tetra_voice_half_slot(f.opts, f.state, h1, 1);
    tetra_voice_full_slot(f.opts, f.state, b2, 1);

    const uint8_t* const frames[] = {s1, s1 + TETRA_VOICE_FRAME_BITS, NULL, NULL, NULL, sh,
                                     s2, s2 + TETRA_VOICE_FRAME_BITS};
    const int bad[] = {0, 0, 1, 1, 1, 0, 0, 0};

    enum { FRAMES = sizeof(bad) / sizeof(bad[0]) };

    static int16_t want[FRAMES * TETRA_VOICE_FRAME_SAMPLES];
    static int16_t out[MAX_OUT];
    expect(reference_decode(frames, bad, FRAMES, want) == 0, "mbelib: reference decode");
    const size_t n = fixture_output(&f, out, MAX_OUT);
    /* 8 frames are 1920 samples: 12 whole output blocks of 160. */
    expect(n == (size_t)FRAMES * TETRA_VOICE_FRAME_SAMPLES, "mbelib: every frame plays");
    expect(n > 0 && memcmp(out, want, n * sizeof(int16_t)) == 0, "mbelib: the output is the decoder's, in order");
    expect(any_nonzero(out, n), "mbelib: the speech is not silence");
    const tetra_voice_t* v = &tetra_st(f.state)->voice;
    expect(v->frames == FRAMES && v->concealed == 3 && v->codec_errors == 0 && v->last_decode_failed == 0,
           "mbelib: eight frames, three concealed, no errors");

    /* A carrier boundary resets the decoder: the next speech decodes as a fresh decoder's. */
    tetra_voice_reset(f.state);
    tetra_voice_full_slot(f.opts, f.state, b2, 1);
    const uint8_t* const again[] = {s2, s2 + TETRA_VOICE_FRAME_BITS};
    const int good[] = {0, 0};
    expect(reference_decode(again, good, 2, want) == 0, "mbelib: reference decode after reset");
    const size_t m = fixture_output(&f, out, MAX_OUT);
    expect(m == n + ((size_t)2 * TETRA_VOICE_FRAME_SAMPLES), "mbelib: after reset both frames play");
    expect(m > n && memcmp(out + n, want, (m - n) * sizeof(int16_t)) == 0,
           "mbelib: after reset the speech decodes from a reset decoder");
    fixture_close(&f);
}

typedef struct {
    const char* name;
    tetra_test_codec_variant variant;
    const char* log; /* what its one error says */
} failure_case;

static void
run_failure_case(const failure_case* c) {
    char label[1024];
    fixture f;
    if (fixture_open(&f, tetra_test_codec(c->variant)) != 0) {
        expect(0, "fixture");
        fixture_close(&f);
        return;
    }
    uint8_t bits[TETRA_SPEECH_FULL_BITS];
    DSD_MEMSET(bits, 0, sizeof(bits));
    dsd_test_capture_stderr cap;
    char log[4096];
    log[0] = '\0';
    if (dsd_test_capture_stderr_begin(&cap, "tetra_codec_log") == 0) {
        for (int i = 0; i < 3; i++) {
            tetra_voice_full_slot(f.opts, f.state, bits, 1);
        }
        (void)dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, log, sizeof(log));
    }

    const tetra_voice_t* v = &tetra_st(f.state)->voice;
    DSD_SNPRINTF(label, sizeof(label), "%s: one log line naming \"%s\", got:\n%s", c->name, c->log, log);
    expect(count_occurrences(log, "TETRA:") == 1 && strstr(log, c->log) != NULL, label);
    DSD_SNPRINTF(label, sizeof(label), "%s: %u errors, %u frames, last failed %u", c->name, (unsigned)v->codec_errors,
                 (unsigned)v->frames, (unsigned)v->last_decode_failed);
    expect(v->codec_errors == 6 && v->frames == 0 && v->last_decode_failed == 1, label);
    DSD_SNPRINTF(label, sizeof(label), "%s: the speech is still reception", c->name);
    expect(f.state->last_vc_sync_time != 0 && f.state->last_vc_sync_time_m > 0.0, label);

    int16_t out[16];
    DSD_SNPRINTF(label, sizeof(label), "%s: nothing plays", c->name);
    expect(fixture_output(&f, out, 16) == 0, label);
    fixture_close(&f);
}

int
main(void) {
    test_mbelib_decoder_plays_the_speech();
    const failure_case cases[] = {
        {"no decoder", TETRA_TEST_CODEC_NO_DECODER, "no TETRA speech decoder could be made"},
        {"failing decodes", TETRA_TEST_CODEC_FAIL_DECODE, "failed to decode"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_failure_case(&cases[i]);
    }
    if (g_fail) {
        DSD_FPRINTF(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    puts("PASS test_tetra_voice_codec");
    return 0;
}
