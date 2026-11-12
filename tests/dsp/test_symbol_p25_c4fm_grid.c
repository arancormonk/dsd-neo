// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The symbol grid on a P25 Phase 1 C4FM carrier, through getSymbol() and the hunt's real crossing loop. The hunt reads
 * each data unit's sync word on a grid the crossing loop slips a sample a symbol, and the word's alternating last
 * crossings, acted on a symbol late, left it a sample past the symbols; the unit behind it was read there, open loop
 * (about a sample late on a clean signal). The frame sync now names each exact word to the sync anchor as it accepts it
 * (frame_sync_p25p1_anchor_note_sync()), and the report of a unit read whole arms the recording of the next one's word
 * (dsd_frame_sync_p25p1_frame_decoded()): from the second unit after acquisition on, every unit is read with the grid
 * on its symbols, at whole and fractional rates, either polarity, with the transmitter's clock off ours.
 *
 * The stream is generated: raised-cosine 4FSK (roll-off 0.2) at the decoder's input after its filter, the units laid
 * end to end as on a continuous carrier, a TSDU's null pad left to the hunt as its decoder leaves it. The test plays
 * the frame sync's and the decoders' parts: it accepts each word once the grid has read its last symbol, reads the unit
 * with have_sync set, and reports it. Where the grid sits comes from the samples each symbol took (the WAV input is
 * read one sample at a time), its decision centre symbolCenter samples into the symbol.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/p25p1_sync_anchor.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/platform/file_compat.h>
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
#include "symbol_anchor_internal.h"
#include "test_support.h"

#define TEST_PI      3.14159265358979323846
#define LEAD_SYMBOLS 40
#define MAX_UNITS    12
#define MAX_SYMBOLS  (LEAD_SYMBOLS + (MAX_UNITS * 864) + 16)
#define MAX_SAMPLES  (MAX_SYMBOLS * 21)
#define PULSE_REACH  10     /* symbols either side a pulse is summed over */
#define LEVEL        1000.0 /* one level unit, in WAV counts: the outer levels sit at +/-3000 */
#define NAC          0x1A6
#define HEAD_SYMBOLS                                                                                                   \
    64 /* the symbols past the NID the grid's offset is averaged over: a clock off ours walks it later on */

/* A data unit as sent and as its decoder reads it: @c read symbols from its sync, the rest (a TSDU's null pad) left to
   the hunt. */
typedef struct {
    int symbols;
    int read;
} unit_shape;

static const unit_shape k_ldu = {864, 864};
static const unit_shape k_tdu = {72, 72};
static const unit_shape k_hdu = {396, 396};
static const unit_shape k_tsdu1 = {180, 158};
static const unit_shape k_tsdu3 = {360, 360};

typedef struct {
    const char* name;
    int whole; /* the grid's span: whole + rem / den samples a symbol, as the RTL FSK timing alternates them */
    int rem;
    int den;
    int filtered;    /* the decoder's P25 matched filter switches on behind the first word */
    double ppm;      /* the transmitter's symbol clock against ours */
    double polarity; /* -1: the inverted word */
    double start;    /* where the first symbol's centre sits, in samples */
    const unit_shape* units[MAX_UNITS];
    int unit_count;
    int control_late; /* without the anchor the crossing loop leaves every unit a sample past the symbols */
} grid_case;

static uint64_t g_rng = 0x2545F4914F6CDD1DULL;

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

/* The stream as sent: each symbol's dibit, the first symbol of each unit's sync, and the WAV samples. */
typedef struct {
    int dibits[MAX_SYMBOLS];
    int symbol_count;
    int unit_start[MAX_UNITS];
    double period; /* the transmitter's symbol period, in samples */
    double start;
    short samples[MAX_SAMPLES];
    int sample_count;
} stream;

static void
build_stream(const grid_case* c, stream* s) {
    s->symbol_count = 0;
    for (int k = 0; k < LEAD_SYMBOLS; k++) {
        s->dibits[s->symbol_count++] = random_dibit();
    }
    for (int u = 0; u < c->unit_count; u++) {
        const unit_shape* shape = c->units[u];
        s->unit_start[u] = s->symbol_count;
        for (int k = 0; k < shape->symbols; k++) {
            int dibit = random_dibit();
            if (k < 24) {
                dibit = P25P1_SYNC[k] - '0';
            } else if (k >= shape->read) {
                dibit = 0; /* the null pad: null bits, +1 */
            }
            s->dibits[s->symbol_count++] = dibit;
        }
    }
    for (int k = 0; k < 8; k++) {
        s->dibits[s->symbol_count++] = random_dibit();
    }
    assert(s->symbol_count <= MAX_SYMBOLS);
    s->period = ((double)c->whole + (c->den > 0 ? (double)c->rem / (double)c->den : 0.0)) * (1.0 + (c->ppm * 1e-6));
    s->start = c->start;
    s->sample_count = (int)((double)s->symbol_count * s->period);
    assert(s->sample_count <= MAX_SAMPLES);
    for (int n = 0; n < s->sample_count; n++) {
        const double sym = ((double)n - s->start) / s->period;
        const int lo = (int)floor(sym) - PULSE_REACH;
        double v = 0.0;
        for (int k = lo < 0 ? 0 : lo; k <= lo + (2 * PULSE_REACH) && k < s->symbol_count; k++) {
            v += level_for_dibit(s->dibits[k]) * rc_pulse(sym - (double)k);
        }
        s->samples[n] = (short)lrint(c->polarity * LEVEL * v);
    }
}

static int
write_wav(const stream* s, char* out_path, size_t out_path_size) {
    int fd = dsd_test_mkstemp(out_path, out_path_size, "dsdneo_p25_grid");
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

/* The decoder on the WAV, and where its grid is. */
typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    const grid_case* c;
    const stream* s;
    int accum; /* the fractional span's accumulator */
    long consumed;
    double last_centre; /* the last symbol's decision centre, in samples */
} reader;

static void
reader_open(reader* r, dsd_opts* opts, dsd_state* state, const grid_case* c, const stream* s, const char* wav_path) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_file_info = (SF_INFO*)calloc(1, sizeof(*opts->audio_in_file_info));
    assert(opts->audio_in_file_info != NULL);
    opts->audio_in_file = sf_open(wav_path, SFM_READ, opts->audio_in_file_info);
    assert(opts->audio_in_file != NULL);
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->audio_out_type = 1;
    opts->input_volume_multiplier = 1;
    opts->wav_sample_rate = 48000;
    /* The stream is what the decoder's filter would hand on; a symmetric filter on it moves no centre. */
    opts->use_cosine_filter = c->filtered;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", wav_path);

    state->rf_mod = 0;
    state->center = 0.0f;
    state->min = -1e6f; /* the synced clip and the crossing loop's rails must not touch anything */
    state->max = 1e6f;
    state->maxref = 1e6f;
    state->minref = -1e6f;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    state->jitter = -1;
    exitflag = 0;

    r->opts = opts;
    r->state = state;
    r->c = c;
    r->s = s;
    r->accum = 0;
    r->consumed = 0;
    r->last_centre = 0.0;
}

static void
reader_close(reader* r) {
    dsd_state_ext_free_all(r->state);
    sf_close(r->opts->audio_in_file);
    r->opts->audio_in_file = NULL;
    free(r->opts->audio_in_file_info);
    r->opts->audio_in_file_info = NULL;
}

/* Read one symbol; returns the index of the sent symbol whose centre the grid's decision centre is nearest. */
static int
read_symbol(reader* r, int have_sync) {
    int span = r->c->whole;
    r->accum += r->c->rem;
    if (r->c->den > 0 && r->accum >= r->c->den) {
        span++;
        r->accum -= r->c->den;
    }
    r->state->samplesPerSymbol = span;
    r->state->symbolCenter = (span - 1) / 2;
    (void)getSymbol(r->opts, r->state, have_sync);
    assert(exitflag == 0);
    const long consumed = (long)sf_seek(r->opts->audio_in_file, 0, SEEK_CUR);
    assert(consumed > r->consumed);
    /* The span index started where the crossing loop or the anchor put it, and ran to the span's end: the decision
       centre is symbolCenter samples into the span, counted back from the last sample taken, and through the matched
       filter its group delay before that. */
    r->last_centre = (double)(consumed - span + r->state->symbolCenter - r->state->matched_filter.delay);
    r->consumed = consumed;
    return (int)lround((r->last_centre - r->s->start) / r->s->period);
}

/* The grid's mean offset from the symbols' centres over the head of one unit's body, in samples (+: the grid is late).
   Read open loop, a unit then drifts by the clocks' difference (at 20 ppm a sixth of a sample over an LDU), which the
   next word measures. */
typedef struct {
    double offset[MAX_UNITS];
    int measured[MAX_UNITS]; /* the unit's word was measured (dsd_symbol_anchor_test_placed()) */
} grid_trace;

static void
run_case(const grid_case* c, const stream* s, const char* wav_path, int anchored, grid_trace* out) {
    static dsd_opts opts;
    static dsd_state state;
    reader r;
    reader_open(&r, &opts, &state, c, s, wav_path);
    const int synctype = c->polarity < 0.0 ? DSD_SYNC_P25P1_NEG : DSD_SYNC_P25P1_POS;
    int k = -1;
    for (int u = 0; u < c->unit_count; u++) {
        /* The hunt, up to the word's last symbol. */
        const int word_end = s->unit_start[u] + 23;
        while (k < word_end) {
            k = read_symbol(&r, 0);
        }
        assert(k == word_end);
        state.synctype = synctype;
        state.lastsynctype = synctype;
        if (anchored) {
            frame_sync_p25p1_anchor_note_sync(&state, synctype, DSD_P25P1_SYNC_EXACT, -1, 0);
        }
        /* The unit's decoders, from the NID on. */
        double sum = 0.0;
        int count = 0;
        for (int m = 24; m < c->units[u]->read; m++) {
            k = read_symbol(&r, 1);
            if (m == 25) {
                out->measured[u] = dsd_symbol_anchor_test_placed(&state);
            }
            if (m >= 57 && m < 57 + HEAD_SYMBOLS) { /* past the NID: the move the word asked for is in */
                sum += r.last_centre - (s->start + ((double)k * s->period));
                count++;
            }
        }
        out->offset[u] = count > 0 ? sum / (double)count : 0.0;
        if (anchored) {
            assert(dsd_frame_sync_p25p1_frame_decoded(&state, NAC, (uint32_t)c->units[u]->symbols)
                   == DSD_P25P1_GRID_ARMED);
        }
    }
    reader_close(&r);
}

/* Every unit's word but the first is measured, and every unit from the third on is read with the grid on its symbols,
   within half a sample: the second unit's word is the first the recording holds, read on the crossing loop's grid and
   measured against the grid without the loop's slips. The grid moves in whole samples and stands within half a sample
   of the word: at a fractional rate its whole-sample centres sit up to half a sample either side of the symbols' own,
   and with the clocks apart the symbols walk on, so a word's 23 and a unit's head average that a little differently: a
   tenth of a sample more is allowed there. The word that acquires the transmission is measured on the raw samples the
   matched-filter seam keeps, at a whole rate, and moves the grid when it sits three quarters of a sample off or more,
   so the unit after it is read on its symbols too, with the filter switching on behind the word or not; nearer, and at
   a fractional rate, whose spans alternate, that unit is the hunt's, where it was before. Without the anchor, the
   crossing loop leaves the units off the symbols on these streams; at 48 and 24 kHz it leaves every one a sample past
   them. */
static void
check_case(const grid_case* c) {
    static stream s;
    build_stream(c, &s);
    char wav_path[DSD_TEST_PATH_MAX];
    assert(write_wav(&s, wav_path, sizeof(wav_path)) == 0);

    grid_trace anchored;
    grid_trace control;
    DSD_MEMSET(&anchored, 0, sizeof(anchored));
    DSD_MEMSET(&control, 0, sizeof(control));
    run_case(c, &s, wav_path, 1, &anchored);
    run_case(c, &s, wav_path, 0, &control);

    int late = 0;
    for (int u = 0; u < c->unit_count; u++) {
        DSD_FPRINTF(stderr,
                    "%s: unit %d (%d symbols): grid %+.2f samples off the symbols%s (without the anchor %+.2f)\n",
                    c->name, u, c->units[u]->symbols, anchored.offset[u], anchored.measured[u] ? ", word measured" : "",
                    control.offset[u]);
        if (u >= 1) {
            assert(anchored.measured[u]);
        }
        if (u >= 2 || (u == 0 && c->den == 0 && fabs(control.offset[0]) >= 0.8)) {
            assert(fabs(anchored.offset[u]) <= ((c->den > 0 || fabs(c->ppm) > 0.0) ? 0.6 : 0.5));
        }
        late += control.offset[u] > 0.5;
    }
    /* The hunt word's move places no grid for a hold: the second unit's word is read on the crossing loop's grid. */
    assert(!anchored.measured[0]);
    assert((c->den == 0 && fabs(control.offset[0]) >= 0.7) || fabs(anchored.offset[0] - control.offset[0]) < 1e-9);
    /* The control: the hunt's crossing loop leaves the units off the symbols, so the anchor moved the grid. */
    assert(!c->control_late || late == c->unit_count);
    remove(wav_path);
}

static const grid_case k_cases[] = {
    {"48 kHz voice", 10, 0, 0, 0, 0.0, 1.0, 4.0, {&k_hdu, &k_ldu, &k_ldu, &k_ldu, &k_ldu, &k_tdu}, 6, 1},
    {"48 kHz control channel, inverted",
     10,
     0,
     0,
     0,
     0.0,
     -1.0,
     4.3,
     {&k_tsdu3, &k_tsdu3, &k_tsdu1, &k_tsdu3, &k_tsdu1, &k_tsdu1, &k_tsdu3},
     7,
     1},
    {"24 kHz control channel", 5, 0, 0, 0, 0.0, 1.0, 2.0, {&k_tsdu3, &k_tsdu1, &k_tsdu3, &k_tsdu3, &k_tsdu1}, 5, 1},
    {"24 kHz voice, inverted", 5, 0, 0, 0, 0.0, -1.0, 2.0, {&k_hdu, &k_ldu, &k_ldu, &k_tdu}, 4, 1},
    {"48 kHz voice, inverted", 10, 0, 0, 0, 0.0, -1.0, 4.0, {&k_hdu, &k_ldu, &k_ldu, &k_tdu}, 4, 1},
    {"48 kHz voice, matched filter", 10, 0, 0, 1, 0.0, 1.0, 4.0, {&k_hdu, &k_ldu, &k_ldu, &k_tdu}, 4, 1},
    {"48 kHz voice, inverted, matched filter", 10, 0, 0, 1, 0.0, -1.0, 4.0, {&k_ldu, &k_ldu, &k_ldu, &k_tdu}, 4, 1},
    {"24 kHz voice, matched filter", 5, 0, 0, 1, 0.0, 1.0, 2.0, {&k_ldu, &k_ldu, &k_ldu, &k_tdu}, 4, 1},
    {"24 kHz control channel, inverted, matched filter",
     5,
     0,
     0,
     1,
     0.0,
     -1.0,
     2.0,
     {&k_tsdu3, &k_tsdu1, &k_tsdu3, &k_tsdu3},
     4,
     1},
    {"96 kHz voice, inverted", 20, 0, 0, 0, 0.0, -1.0, 9.0, {&k_ldu, &k_ldu, &k_ldu, &k_tdu}, 4, 0},
    {"72 kHz voice", 15, 0, 0, 0, 0.0, 1.0, 7.0, {&k_ldu, &k_ldu, &k_ldu, &k_tdu}, 4, 0},
    {"50 kHz voice", 10, 2000, 4800, 0, 0.0, 1.0, 4.5, {&k_ldu, &k_ldu, &k_ldu, &k_ldu}, 4, 0},
    {"50 kHz voice, a quarter sample on", 10, 2000, 4800, 0, 0.0, 1.0, 4.75, {&k_ldu, &k_ldu, &k_ldu, &k_ldu}, 4, 0},
    {"50 kHz control channel, inverted",
     10,
     2000,
     4800,
     0,
     0.0,
     -1.0,
     5.0,
     {&k_tsdu3, &k_tsdu1, &k_tsdu3, &k_tsdu3},
     4,
     0},
    {"48 kHz voice, transmitter 20 ppm fast",
     10,
     0,
     0,
     0,
     -20.0,
     1.0,
     4.0,
     {&k_ldu, &k_ldu, &k_ldu, &k_ldu, &k_ldu},
     5,
     0},
    {"48 kHz voice, transmitter 20 ppm slow",
     10,
     0,
     0,
     0,
     20.0,
     -1.0,
     4.0,
     {&k_ldu, &k_ldu, &k_ldu, &k_ldu, &k_ldu},
     5,
     0},
};

int
main(void) {
    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        check_case(&k_cases[i]);
    }
    DSD_FPRINTF(stderr, "symbol P25 C4FM grid: OK\n");
    return 0;
}
