// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The slicer levels a P25 Phase 1 C4FM data unit is read with, through getSymbol(), get_dibit_and_analog_signal() and
 * the warm start each accepted sync runs. In a synced read every sample is clipped to [min, max]
 * (symbol_apply_sync_clip()), and the in-frame tracker in use_symbol() set max from the two largest of the last
 * ssize symbols: a clipped symbol can never exceed max, so max, and umid with it, could only fall inside a unit. Under
 * the -f1 sizes (36 symbols, 15 levels) a clean unit lost a fifth of its outer level within a few hundred symbols and
 * read its inner symbols as outer. With those sizes a unit is read on the levels its sync word set; the default sizes
 * (128/1024) keep tracking, and so does a unit whose sync set no levels.
 *
 * The stream is generated as in DSP_SYMBOL_P25_C4FM_GRID: raised-cosine 4FSK at the decoder's input after its filter,
 * units end to end, the frame sync's and the decoders' parts played by the test (the hunt reads each word, its accept
 * runs the production warm start and names the word to the anchor, the body is read with have_sync set, the report
 * arms the next word). Over a 3-block TSDU, an LDU and a long data PDU, under the -f1 sizes (either polarity, levels
 * held) and the default sizes (levels tracked), the outer levels and the inner/outer boundaries stay within a bound of
 * the levels sent. With the warm start off the
 * frame has no levels from its sync and the tracker still takes them from the signal.
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/p25p1_sync_anchor.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/exitflag.h>
#include <math.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"
#include "test_env.h"
#include "test_support.h"

#define TEST_PI      3.14159265358979323846
#define LEAD_SYMBOLS 40
#define MAX_UNITS    8
#define PDU_SYMBOLS  2592 /* a header and 24 data blocks: 56 + 25 * 98 dibits with their status, to the next status */
#define MAX_SYMBOLS  (LEAD_SYMBOLS + (MAX_UNITS * PDU_SYMBOLS) + 16)
#define MAX_SAMPLES  (MAX_SYMBOLS * 10)
#define PULSE_REACH  10
#define LEVEL        1000.0 /* one level unit, in WAV counts: the outer levels sit at +/-3000 */
#define NAC          0x1A6
#define BUF_LEN      100000

/* The bound: the outer level within an eighth of the outer level sent, the inner/outer boundary within an eighth of
   the boundary's own level (2). A window of five samples about the centre averages an outer symbol a little below its
   peak, and the warm start sets the boundary 5/8 of the way to the outer level. */
#define OUTER_BOUND  0.125
#define MID_BOUND    0.125

typedef struct {
    const char* name;
    int symbols;
} unit_shape;

static const unit_shape k_tsdu3 = {"3-block TSDU", 360};
static const unit_shape k_ldu = {"LDU", 864};
static const unit_shape k_pdu = {"24-block PDU", PDU_SYMBOLS};

typedef struct {
    const char* name;
    int ssize; /* the tracker sizes: -f1's 36/15 (kept when -fa follows -f1), or the default 128/1024 */
    int msize;
    double polarity;
    int warm_start_off; /* DSD_NEO_SYNC_WARMSTART=0: no levels from the sync */
} level_case;

static const unit_shape* const k_units[] = {&k_tsdu3, &k_tsdu3, &k_ldu, &k_ldu, &k_pdu, &k_tsdu3};
#define UNIT_COUNT ((int)(sizeof(k_units) / sizeof(k_units[0])))

static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;

static int
random_dibit(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (int)(g_rng >> 62);
}

static double
level_for_dibit(int dibit) {
    static const double levels[4] = {1.0, 3.0, -1.0, -3.0};
    return levels[dibit & 3];
}

static double
rc_pulse(double t) {
    const double a = 0.2;
    if (fabs(t) < 1e-9) {
        return 1.0;
    }
    const double s = sin(TEST_PI * t) / (TEST_PI * t);
    const double den = 1.0 - ((2.0 * a * t) * (2.0 * a * t));
    if (fabs(den) < 1e-9) {
        return (TEST_PI / 4.0) * s;
    }
    return s * cos(TEST_PI * a * t) / den;
}

typedef struct {
    int dibits[MAX_SYMBOLS];
    int symbol_count;
    int unit_start[MAX_UNITS];
    short samples[MAX_SAMPLES];
    int sample_count;
} stream;

#define SPS   10
#define START 4.0

static void
build_stream(double polarity, stream* s) {
    s->symbol_count = 0;
    for (int k = 0; k < LEAD_SYMBOLS; k++) {
        s->dibits[s->symbol_count++] = random_dibit();
    }
    for (int u = 0; u < UNIT_COUNT; u++) {
        s->unit_start[u] = s->symbol_count;
        for (int k = 0; k < k_units[u]->symbols; k++) {
            s->dibits[s->symbol_count++] = k < 24 ? (P25P1_SYNC[k] - '0') : random_dibit();
        }
    }
    for (int k = 0; k < 8; k++) {
        s->dibits[s->symbol_count++] = random_dibit();
    }
    assert(s->symbol_count <= MAX_SYMBOLS);
    s->sample_count = s->symbol_count * SPS;
    for (int n = 0; n < s->sample_count; n++) {
        const double sym = ((double)n - START) / (double)SPS;
        const int lo = (int)floor(sym) - PULSE_REACH;
        double v = 0.0;
        for (int k = lo < 0 ? 0 : lo; k <= lo + (2 * PULSE_REACH) && k < s->symbol_count; k++) {
            v += level_for_dibit(s->dibits[k]) * rc_pulse(sym - (double)k);
        }
        s->samples[n] = (short)lrint(polarity * LEVEL * v);
    }
}

static int
write_wav(const stream* s, char* out_path, size_t out_path_size) {
    int fd = dsd_test_mkstemp(out_path, out_path_size, "dsdneo_p25_levels");
    if (fd < 0) {
        return -1;
    }
    dsd_close(fd);
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.samplerate = 48000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* wav = sf_open(out_path, SFM_WRITE, &info);
    if (wav == NULL) {
        return -1;
    }
    const int ok = sf_write_short(wav, s->samples, s->sample_count) == s->sample_count;
    sf_close(wav);
    return ok ? 0 : -1;
}

/* The datascope's call line, which nothing here prints. */
int
dsd_call_state_get(const dsd_state* state, uint8_t slot, dsd_call_snapshot* out) {
    (void)state;
    (void)slot;
    (void)out;
    return -1;
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    float history[DSD_SYMBOL_HISTORY_SIZE];
    long consumed;
    double last_centre;
} reader;

static void
reader_open(reader* r, const level_case* c, const char* wav_path) {
    r->opts = (dsd_opts*)calloc(1, sizeof(*r->opts));
    r->state = (dsd_state*)calloc(1, sizeof(*r->state));
    assert(r->opts != NULL && r->state != NULL);
    dsd_opts* opts = r->opts;
    dsd_state* state = r->state;
    opts->audio_in_file_info = (SF_INFO*)calloc(1, sizeof(*opts->audio_in_file_info));
    assert(opts->audio_in_file_info != NULL);
    opts->audio_in_file = sf_open(wav_path, SFM_READ, opts->audio_in_file_info);
    assert(opts->audio_in_file != NULL);
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->audio_out_type = 1;
    opts->input_volume_multiplier = 1;
    opts->wav_sample_rate = 48000;
    opts->use_cosine_filter = 0;
    opts->ssize = c->ssize;
    opts->msize = c->msize;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", wav_path);

    state->dibit_buf = (int*)calloc(BUF_LEN, sizeof(int));
    state->dmr_payload_buf = (int*)calloc(BUF_LEN, sizeof(int));
    state->dmr_soft_buf = (dsd_dibit_soft_t*)calloc(BUF_LEN, sizeof(dsd_dibit_soft_t));
    assert(state->dibit_buf != NULL && state->dmr_payload_buf != NULL && state->dmr_soft_buf != NULL);
    state->dibit_buf_p = state->dibit_buf + 200;
    state->dmr_payload_p = state->dmr_payload_buf + 200;
    state->dmr_soft_p = state->dmr_soft_buf + 200;
    DSD_MEMSET(r->history, 0, sizeof(r->history));
    state->symbol_history = r->history;
    state->symbol_history_size = DSD_SYMBOL_HISTORY_SIZE;

    state->rf_mod = 0;
    state->samplesPerSymbol = SPS;
    state->symbolCenter = (SPS - 1) / 2;
    state->max = 15000.0f; /* initState()'s levels until the first sync */
    state->min = -15000.0f;
    state->center = 0.0f;
    state->umid = 0.0f;
    state->lmid = 0.0f;
    state->maxref = 12000.0f;
    state->minref = -12000.0f;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    state->jitter = -1;
    exitflag = 0;
    r->consumed = 0;
}

static void
reader_close(reader* r) {
    dsd_state_ext_free_all(r->state);
    sf_close(r->opts->audio_in_file);
    free(r->opts->audio_in_file_info);
    free(r->state->dibit_buf);
    free(r->state->dmr_payload_buf);
    free(r->state->dmr_soft_buf);
    free(r->state);
    free(r->opts);
}

/* The index of the sent symbol the grid's decision centre was nearest. */
static int
grid_symbol(reader* r) {
    const long consumed = (long)sf_seek(r->opts->audio_in_file, 0, SEEK_CUR);
    assert(consumed > r->consumed);
    r->consumed = consumed;
    const double centre = (double)(consumed - SPS + r->state->symbolCenter);
    return (int)lround((centre - START) / (double)SPS);
}

/* One hunt symbol: read without sync, kept in the window the hunt keeps (frame_sync_update_symbol_ring()). */
static int
hunt_symbol(reader* r) {
    dsd_state* state = r->state;
    const float symbol = getSymbol(r->opts, state, 0);
    assert(exitflag == 0);
    state->sbuf[state->sidx] = symbol;
    state->sidx = (state->sidx >= r->opts->ssize - 1) ? 0 : state->sidx + 1;
    return grid_symbol(r);
}

typedef struct {
    double outer_err;     /* the largest |max - 3| (and |min + 3|) over the unit's body, in levels */
    double mid_err;       /* the largest |umid - 2| (and |lmid + 2|) */
    double end_outer_err; /* |max - 3| (and |min + 3|) at the unit's last symbol */
    int tracked;          /* max moved off the value the unit's sync left */
} unit_levels;

static void
note_levels(const dsd_state* state, double polarity, unit_levels* out) {
    /* The thresholds are in the received polarity: an inverted unit's outer level sits at min. */
    const double up = (polarity > 0.0 ? state->max : -state->min) / LEVEL;
    const double down = (polarity > 0.0 ? -state->min : state->max) / LEVEL;
    const double umid = (polarity > 0.0 ? state->umid : -state->lmid) / LEVEL;
    const double lmid = (polarity > 0.0 ? -state->lmid : state->umid) / LEVEL;
    const double outer = fmax(fabs(up - 3.0), fabs(down - 3.0));
    const double mid = fmax(fabs(umid - 2.0), fabs(lmid - 2.0));
    out->outer_err = fmax(out->outer_err, outer);
    out->end_outer_err = outer;
    out->mid_err = fmax(out->mid_err, mid);
}

static void
run_case(const level_case* c, const stream* s, const char* wav_path, unit_levels out[UNIT_COUNT]) {
    static reader r;
    reader_open(&r, c, wav_path);
    dsd_state* state = r.state;
    const int synctype = c->polarity < 0.0 ? DSD_SYNC_P25P1_NEG : DSD_SYNC_P25P1_POS;
    int k = -1;
    for (int u = 0; u < UNIT_COUNT; u++) {
        const int word_end = s->unit_start[u] + 23;
        while (k < word_end) {
            k = hunt_symbol(&r);
        }
        assert(k == word_end);
        /* frame_sync_accept_p25p1() on C4FM: the warm start from the word, then the word named to the anchor. */
        state->synctype = synctype;
        state->lastsynctype = synctype;
        frame_sync_p25p1_c4fm_warm_start(r.opts, state);
        assert(state->p25_p1_c4fm_sync_levels == (c->warm_start_off ? 0U : 1U));
        frame_sync_p25p1_anchor_note_sync(state, synctype, -1, 0);
        DSD_MEMSET(&out[u], 0, sizeof(out[u]));
        const float sync_max = state->max;
        for (int m = 24; m < k_units[u]->symbols; m++) {
            (void)get_dibit_and_analog_signal(r.opts, state, NULL);
            assert(exitflag == 0);
            k = grid_symbol(&r);
            note_levels(state, c->polarity, &out[u]);
            out[u].tracked |= fabsf(state->max - sync_max) > 1.0f;
        }
        (void)dsd_frame_sync_p25p1_frame_decoded(state, NAC, (uint32_t)k_units[u]->symbols);
    }
    reader_close(&r);
}

static void
check_case(const level_case* c) {
    static stream s;
    build_stream(c->polarity, &s);
    char wav_path[DSD_TEST_PATH_MAX];
    assert(write_wav(&s, wav_path, sizeof(wav_path)) == 0);
    if (c->warm_start_off) {
        (void)dsd_test_setenv("DSD_NEO_SYNC_WARMSTART", "0", 1);
        dsd_neo_config_init();
    }
    unit_levels levels[UNIT_COUNT];
    run_case(c, &s, wav_path, levels);
    if (c->warm_start_off) {
        (void)dsd_test_unsetenv("DSD_NEO_SYNC_WARMSTART");
        dsd_neo_config_init();
    }
    int ok = 1;
    for (int u = 0; u < UNIT_COUNT; u++) {
        DSD_FPRINTF(stderr, "%s: unit %d (%s): outer level off by up to %.3f, inner/outer boundary by up to %.3f\n",
                    c->name, u, k_units[u]->name, levels[u].outer_err, levels[u].mid_err);
        if (c->warm_start_off) {
            /* No sync levels: the tracker takes them from the signal, off initState()'s 15 outer levels within the
               first unit. Held, they would stay there (12 levels off). */
            ok &= levels[u].end_outer_err <= 1.5;
            continue;
        }
        /* The first two words are read on the hunt's walking grid, before the anchor holds one
           (DSP_SYMBOL_P25_C4FM_GRID), and the levels the warm start takes from them with it. */
        if (u >= 2) {
            ok &= levels[u].outer_err <= 3.0 * OUTER_BOUND;
            ok &= levels[u].mid_err <= 2.0 * MID_BOUND;
        }
        /* -f1's windows hold the sync's levels through the unit; the default windows keep tracking. */
        ok &= levels[u].tracked == (c->ssize >= 128 && c->msize >= 1024);
    }
    remove(wav_path);
    assert(ok);
}

static const level_case k_cases[] = {
    {"-f1 sizes", 36, 15, 1.0, 0},
    {"-f1 sizes, inverted", 36, 15, -1.0, 0},
    {"default sizes", 128, 1024, 1.0, 0},
    {"-f1 sizes, warm start off", 36, 15, 1.0, 1},
};

int
main(void) {
    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        check_case(&k_cases[i]);
    }
    DSD_FPRINTF(stderr, "symbol P25 C4FM levels: OK\n");
    return 0;
}
