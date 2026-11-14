// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Speech level parity (issue #518): one deterministic talker played as digital voice and as analog FM and AM must come
 * out at the speech AGC's active speech level, -20 dBFS (ITU-T P.56, test_support/p56.h), and the digital and analog
 * renditions at the same level.
 *
 *   dsd-neo_test_speech_parity_tool source RATE OUT.wav
 *       The talker as a 16-bit mono WAV at RATE. tools/build_iq_fixtures.py (--speech-synth-gen) modulates it into the
 *       speech_*_synth I/Q fixtures.
 *   dsd-neo_test_speech_parity_tool encode imbe|ambe2450|ambe2400 OUT
 *       The talker encoded by mbelib-neo's IMBE 4400, AMBE+2 2450 or AMBE 2400 encoder into the .imb, .amb or .dmb
 *       file -r plays (src/core/file/dsd_file.c: a four-byte cookie, then one record per 20 ms frame).
 *   dsd-neo_test_speech_parity_tool established [--max-error-db DB] [--max-spread-db DB] NAME:FILE:RATE:CH:LEAD_S...
 *       Each FILE is a decoder's raw 16-bit output (RATE Hz, CH interleaved channels, the louder channel is measured)
 *       in which the talker starts LEAD_S seconds in. Its established speech, from the end of the talker's first 2 s of
 *       speech to the end of the talker, must read within DB (default 3) of -20 dBFS, and every rendition within the
 *       spread (default 1.5 dB) of every other.
 *   dsd-neo_test_speech_parity_tool level [--min-dbfs DB] [--max-dbfs DB] [--toward FILE] FILE RATE CH
 *       The whole output's P.56 level, inside [min, max]. With --toward, FILE is the same capture decoded at the fixed
 *       reference gain (-g 1): the level must be nearer -20 dBFS than that one, or lie no more than 3 dB under and no
 *       more than 6 dB over -20 dBFS, and in either case within 10 dB of it, the startup bound at 1 s. Over: digital
 *       voice opens a reception from a talker it has not heard at +8 dB, and until the reception has established its
 *       estimate its loudest frames are held at -12 dBFS, so a short excerpt of a loud talker can read up to about
 *       6 dB over the target.
 *   dsd-neo_test_speech_parity_tool gated-nfm FIXED AUTO
 *       A capture with no speech in it (syllable-gated band-limited noise) replayed on narrowband FM: FIXED at -n 50,
 *       AUTO at the default auto gain, both 48 kHz mono. The tool runs FIXED, which is what the monitor's speech AGC
 *       took in, through a speech AGC set up as the analog chain sets its own (narrowband start gain, the coloured-noise
 *       veto) and requires its output to be AUTO's, so the AGC state it reads is the decoder's. That state must show no
 *       speech learned: no frame committed and the learned gain within 1 dB of the start gain throughout. The
 *       attenuator, which eases non-speech, may only move down at its 6 dB/s and stay at or under 0 dB, and the
 *       limiter may catch only the loudest few frames (the start gain plays the noise's bursts near the ceiling).
 *
 * Every FILE and OUT is one of the work file names the parity checks use (k_work_files: talker.wav, talker.imb,
 * talker.amb, talker.dmb, and the decoder outputs imbe.raw, ambe2450.raw, ambe2400.raw, fm_nb.raw, fm_wb.raw, am.raw,
 * voice.raw and reference.raw), opened in the working directory; an output must not exist yet.
 */

#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/analog_audio.h>
#include <dsd-neo/dsp/speech_agc.h>
#include <dsd-neo/platform/file_compat.h>
#include <math.h>
#include <mbelib-neo/mbelib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p56.h"
#include "speech_synth.h"

/* The talker: 9 s of running speech at a male pitch whose level steps down 4 dB before established speech starts and
   back up 3 dB inside it, so the AGC follows a level change on the way in and inside the measured span. Its nominal
   level, -26 dBFS, is the DVSI reference level digital voice plays at with -g 1. */
#define TALKER_SEED           0x5A17A1CEULL
#define TALKER_SECONDS        9.0
#define TALKER_LEVEL_DBFS     (-26.0)
#define TALKER_F0_HZ          115.0
/* Established speech starts after this much of the talker's speech (spec: 2 s of committed speech). */
#define ESTABLISHED_AFTER_S   2.0
/* A 20 ms frame counts as speech when it is no more than this far under the talker's nominal level: vowels, not the
   pauses, syllable edges and quiet fricatives the AGC does not learn from. */
#define SPEECH_FRAME_UNDER_DB 10.0
#define TARGET_DBFS           ((double)DSD_SPEECH_AGC_TARGET_DBFS)
/* The established bound, and the startup bound one second in (spec T2: every file within 10 dB). */
#define ESTABLISHED_BOUND_DB  3.0
#define STARTUP_BOUND_DB      10.0
/* How far over the target a short excerpt may read: until a reception has established its estimate, its loudest
   frames are held at -12 dBFS, 8 dB over the target, and a short loud excerpt's P.56 level sits a few dB under them. */
#define STARTUP_OVER_DB       6.0
#define FULL_SCALE            32768.0
#define MBE_RATE_HZ           8000
#define MBE_FRAME             160
#define MAX_RENDITIONS        6

static const dsd_test_level_step k_talker_steps[] = {
    {2.2, TALKER_LEVEL_DBFS - 4.0},
    {6.0, TALKER_LEVEL_DBFS - 1.0},
};

static void
talker_params(dsd_test_speech_params* p) {
    dsd_test_speech_defaults(p, TALKER_SEED);
    p->level_dbfs = TALKER_LEVEL_DBFS;
    p->f0_hz = TALKER_F0_HZ;
    p->steps = k_talker_steps;
    p->step_count = sizeof k_talker_steps / sizeof k_talker_steps[0];
}

static double
talker_level_at(double t) {
    double level = TALKER_LEVEL_DBFS;
    for (size_t i = 0; i < sizeof k_talker_steps / sizeof k_talker_steps[0]; i++) {
        if (t >= k_talker_steps[i].start_s) {
            level = k_talker_steps[i].level_dbfs;
        }
    }
    return level;
}

/* The talker at @p rate, in int16 units; NULL when out of memory. */
static float*
talker_samples(int rate, size_t* n_out) {
    const size_t n = (size_t)lround(TALKER_SECONDS * (double)rate);
    float* x = (float*)calloc(n, sizeof(float));
    dsd_test_speech_params p;
    talker_params(&p);
    if (x == NULL || dsd_test_synth_speech(x, n, rate, &p) != 0) {
        free(x);
        return NULL;
    }
    *n_out = n;
    return x;
}

/* Where established speech starts in the talker: the end of the frame that completes its first 2 s of speech frames. */
static int
established_from_s(double* from_s) {
    size_t n = 0;
    float* x = talker_samples(MBE_RATE_HZ, &n);
    if (x == NULL) {
        return -1;
    }
    const size_t want = (size_t)lround(ESTABLISHED_AFTER_S * MBE_RATE_HZ / MBE_FRAME);
    size_t speech = 0;
    *from_s = -1.0;
    for (size_t f = 0; (f + 1U) * MBE_FRAME <= n; f++) {
        double sum = 0.0;
        for (size_t i = 0; i < MBE_FRAME; i++) {
            const double v = (double)x[(f * MBE_FRAME) + i];
            sum += v * v;
        }
        const double rms_dbfs = 10.0 * log10((sum / MBE_FRAME / (FULL_SCALE * FULL_SCALE)) + 1e-30);
        const double t = (double)f * MBE_FRAME / MBE_RATE_HZ;
        if (rms_dbfs < talker_level_at(t) - SPEECH_FRAME_UNDER_DB) {
            continue;
        }
        speech++;
        if (speech == want) {
            *from_s = (double)(f + 1U) * MBE_FRAME / MBE_RATE_HZ;
            break;
        }
    }
    free(x);
    return *from_s > 0.0 ? 0 : -1;
}

/* ------------------------------------------------------------------------------------------------- files */

/* The files the parity checks (speech_parity_check.cmake, speech_level_check.cmake, tools/build_iq_fixtures.py) write
   and read in their work folder. The tool opens only these: a name on the command line picks one, and the path opened
   is the table's own string. */
static const char* const k_work_files[] = {
    "talker.wav",   "talker.imb", "talker.amb", "talker.dmb", "imbe.raw",  "ambe2450.raw",
    "ambe2400.raw", "fm_nb.raw",  "fm_wb.raw",  "am.raw",     "voice.raw", "reference.raw",
};

/* The work file @p name names, or NULL when it is not one. */
static const char*
work_file(const char* name) {
    for (size_t i = 0; name != NULL && i < sizeof k_work_files / sizeof k_work_files[0]; i++) {
        if (strcmp(name, k_work_files[i]) == 0) {
            return k_work_files[i];
        }
    }
    return NULL;
}

static FILE*
create_output(const char* name) {
    const char* path = work_file(name);
    if (path == NULL) {
        DSD_FPRINTF(stderr, "speech parity: %s is not one of the tool's work file names\n", name ? name : "(null)");
        return NULL;
    }
    FILE* f = dsd_fopen_private(path, "wbx");
    if (f == NULL) {
        DSD_FPRINTF(stderr, "speech parity: cannot create %s (it must not exist yet)\n", name);
    }
    return f;
}

static int
close_output(FILE* f, const char* name) {
    const int bad = ferror(f) != 0;
    if (fclose(f) != 0 || bad) {
        DSD_FPRINTF(stderr, "speech parity: cannot write %s\n", name);
        return 1;
    }
    return 0;
}

static void
put_le16(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFFU);
    p[1] = (unsigned char)((v >> 8) & 0xFFU);
}

static void
put_le32(unsigned char* p, uint32_t v) {
    put_le16(p, v & 0xFFFFU);
    put_le16(p + 2, v >> 16);
}

static int16_t
to_s16(float v) {
    long r = lroundf(v);
    if (r > 32767L) {
        r = 32767L;
    } else if (r < -32768L) {
        r = -32768L;
    }
    return (int16_t)r;
}

static int
cmd_source(int rate, const char* name) {
    size_t n = 0;
    float* x = talker_samples(rate, &n);
    if (x == NULL) {
        DSD_FPRINTF(stderr, "speech parity: cannot make the talker at %d Hz\n", rate);
        return 1;
    }
    FILE* f = create_output(name);
    if (f == NULL) {
        free(x);
        return 1;
    }
    const uint32_t data = (uint32_t)(n * sizeof(int16_t));
    unsigned char h[44];
    DSD_MEMSET(h, 0, sizeof h);
    DSD_MEMCPY(h, "RIFF", 4);
    put_le32(h + 4, 36U + data);
    DSD_MEMCPY(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16U);
    put_le16(h + 20, 1U); /* PCM */
    put_le16(h + 22, 1U); /* mono */
    put_le32(h + 24, (uint32_t)rate);
    put_le32(h + 28, (uint32_t)rate * 2U);
    put_le16(h + 32, 2U);
    put_le16(h + 34, 16U);
    DSD_MEMCPY(h + 36, "data", 4);
    put_le32(h + 40, data);
    (void)fwrite(h, 1, sizeof h, f);
    for (size_t i = 0; i < n; i++) {
        unsigned char s[2];
        put_le16(s, (uint32_t)(uint16_t)to_s16(x[i]));
        (void)fwrite(s, 1, sizeof s, f);
    }
    free(x);
    return close_output(f, name);
}

/* ------------------------------------------------------------------------------------------------- encode */

typedef enum { CODEC_IMBE, CODEC_AMBE2450, CODEC_AMBE2400 } codec_kind;

typedef struct {
    codec_kind kind;
    mbe_imbe4400_encoder* imbe;
    mbe_ambe2450_encoder* ambe2450;
    mbe_ambe2400_encoder* ambe2400;
    mbe_parms cur;
    mbe_parms prev;
    mbe_parms prev_enhanced;
} encoder;

static int
encoder_open(encoder* e, codec_kind kind) {
    DSD_MEMSET(e, 0, sizeof *e);
    e->kind = kind;
    if (kind == CODEC_IMBE) {
        e->imbe = mbe_imbe4400EncoderAlloc();
    } else if (kind == CODEC_AMBE2450) {
        e->ambe2450 = mbe_ambe2450EncoderAlloc();
    } else {
        e->ambe2400 = mbe_ambe2400EncoderAlloc();
    }
    mbe_initMbeParms(&e->cur, &e->prev, &e->prev_enhanced);
    return (e->imbe != NULL || e->ambe2450 != NULL || e->ambe2400 != NULL) ? 0 : -1;
}

static void
encoder_close(encoder* e) {
    mbe_imbe4400EncoderFree(e->imbe);
    mbe_ambe2450EncoderFree(e->ambe2450);
    mbe_ambe2400EncoderFree(e->ambe2400);
}

/* Packs @p count bits MSB first into bytes, as saveImbe4400Data() and save_ambe_record() do. */
static void
pack_bits(const char* bits, size_t count, unsigned char* out) {
    for (size_t i = 0; i < count / 8U; i++) {
        unsigned char b = 0;
        for (size_t j = 0; j < 8U; j++) {
            b = (unsigned char)((b << 1) | (unsigned char)(bits[(i * 8U) + j] & 1));
        }
        out[i] = b;
    }
}

/* One record: the error byte (0, a clean frame), then the 88 IMBE bits in 11 bytes, or the 49 AMBE bits in 6 bytes and
   a byte holding the last bit. */
static int
encode_frame(encoder* e, const float* pcm, FILE* f) {
    char bits[88];
    unsigned char rec[12];
    size_t len = 0;
    int rc = 0;
    rec[0] = 0;
    if (e->kind == CODEC_IMBE) {
        rc = mbe_encodeImbe4400Parms(e->imbe, pcm, bits, &e->cur, &e->prev);
        pack_bits(bits, 88U, rec + 1);
        len = 12U;
    } else {
        rc = e->kind == CODEC_AMBE2450 ? mbe_encodeAmbe2450Parms(e->ambe2450, pcm, bits, &e->cur, &e->prev)
                                       : mbe_encodeAmbe2400Parms(e->ambe2400, pcm, bits, &e->cur, &e->prev);
        pack_bits(bits, 48U, rec + 1);
        rec[7] = (unsigned char)(bits[48] & 1);
        len = 8U;
    }
    if (rc < 0) {
        DSD_FPRINTF(stderr, "speech parity: the encoder refused a frame (%d)\n", rc);
        return -1;
    }
    mbe_moveMbeParms(&e->cur, &e->prev);
    return fwrite(rec, 1, len, f) == len ? 0 : -1;
}

static int
encode_talker(encoder* e, FILE* f) {
    size_t n = 0;
    float* x = talker_samples(MBE_RATE_HZ, &n);
    if (x == NULL) {
        return -1;
    }
    float pcm[MBE_FRAME];
    int rc = 0;
    /* Whole frames, then one of zeros that flushes the encoder's 10 ms lag. */
    for (size_t at = 0; rc == 0 && at < n + MBE_FRAME; at += MBE_FRAME) {
        for (size_t i = 0; i < MBE_FRAME; i++) {
            pcm[i] = at + i < n ? x[at + i] / (float)FULL_SCALE : 0.0f;
        }
        rc = encode_frame(e, pcm, f);
    }
    free(x);
    return rc;
}

static int
cmd_encode(const char* codec, const char* name) {
    codec_kind kind = CODEC_IMBE;
    const char* cookie = ".imb";
    if (strcmp(codec, "ambe2450") == 0) {
        kind = CODEC_AMBE2450;
        cookie = ".amb";
    } else if (strcmp(codec, "ambe2400") == 0) {
        kind = CODEC_AMBE2400;
        cookie = ".dmb";
    } else if (strcmp(codec, "imbe") != 0) {
        DSD_FPRINTF(stderr, "speech parity: unknown codec %s (imbe, ambe2450 or ambe2400)\n", codec);
        return 2;
    }
    encoder e;
    if (encoder_open(&e, kind) != 0) {
        encoder_close(&e);
        DSD_FPRINTF(stderr, "speech parity: cannot allocate the %s encoder\n", codec);
        return 1;
    }
    FILE* f = create_output(name);
    int rc = f == NULL;
    if (f != NULL) {
        rc = fwrite(cookie, 1, 4, f) != 4U || encode_talker(&e, f) != 0;
        rc |= close_output(f, name);
    }
    encoder_close(&e);
    return rc;
}

/* ------------------------------------------------------------------------------------------------- measure */

typedef struct {
    float* x; /* the louder channel, int16 units */
    size_t n;
    int rate;
} audio;

/* The whole of @p name, one of the work files; NULL when it cannot be read or is empty. */
static unsigned char*
read_file(const char* name, size_t* len) {
    *len = 0;
    const char* path = work_file(name);
    /* Binary: the local-file opener reads text on Windows. */
    FILE* f = path != NULL ? dsd_fopen_existing_regular_file(path, "rb") : NULL;
    if (f == NULL) {
        DSD_FPRINTF(stderr, "speech parity: cannot open %s\n", name ? name : "(null)");
        return NULL;
    }
    size_t cap = 1U << 16;
    unsigned char* buf = (unsigned char*)malloc(cap);
    while (buf != NULL) {
        *len += fread(buf + *len, 1, cap - *len, f);
        if (*len < cap) {
            break;
        }
        unsigned char* grown = (unsigned char*)realloc(buf, cap * 2U);
        if (grown == NULL) {
            free(buf);
            buf = NULL;
        } else {
            buf = grown;
            cap *= 2U;
        }
    }
    if (buf != NULL && (ferror(f) || *len == 0U)) {
        free(buf);
        buf = NULL;
    }
    (void)fclose(f);
    if (buf == NULL) {
        DSD_FPRINTF(stderr, "speech parity: %s holds no audio\n", name);
    }
    return buf;
}

static int16_t
sample_at(const unsigned char* raw, size_t index) {
    return (int16_t)(uint16_t)((unsigned)raw[2U * index] | ((unsigned)raw[(2U * index) + 1U] << 8));
}

/* Reads @p name's raw 16-bit little-endian frames of @p channels and keeps the channel with the most energy. */
static int
read_audio(const char* name, int rate, int channels, audio* a) {
    DSD_MEMSET(a, 0, sizeof *a);
    size_t len = 0;
    unsigned char* raw = read_file(name, &len);
    const size_t width = (size_t)channels;
    const size_t frames = len / (2U * width);
    if (raw == NULL || frames == 0U) {
        free(raw);
        return -1;
    }
    double energy[2] = {0.0, 0.0};
    for (size_t i = 0; i < frames * width; i++) {
        const double v = (double)sample_at(raw, i);
        energy[i % width] += v * v;
    }
    const size_t keep = (width == 2U && energy[1] > energy[0]) ? 1U : 0U;
    a->x = (float*)malloc(frames * sizeof(float));
    if (a->x != NULL) {
        for (size_t i = 0; i < frames; i++) {
            a->x[i] = (float)sample_at(raw, (i * width) + keep);
        }
        a->n = frames;
        a->rate = rate;
    }
    free(raw);
    return a->x != NULL ? 0 : -1;
}

static int
parse_rate_channels(const char* rate_text, const char* ch_text, int* rate, int* channels) {
    if (dsd_parse_int_strict(rate_text, 10, 8000, 192000, rate) != 0
        || dsd_parse_int_strict(ch_text, 10, 1, 2, channels) != 0) {
        DSD_FPRINTF(stderr, "speech parity: bad rate %s or channel count %s\n", rate_text, ch_text);
        return -1;
    }
    return 0;
}

typedef struct {
    char spec[256];
    const char* name;
    const char* file;
    int rate;
    int channels;
    double lead_s;
    double level;
} rendition;

/* NAME:FILE:RATE:CH:LEAD_S */
static int
parse_rendition(const char* text, rendition* r) {
    DSD_MEMSET(r, 0, sizeof *r);
    if (DSD_SNPRINTF(r->spec, sizeof r->spec, "%s", text) >= (int)sizeof r->spec) {
        return -1;
    }
    char* field[5];
    char* p = r->spec;
    for (int i = 0; i < 5; i++) {
        field[i] = p;
        p = p != NULL ? strchr(p, ':') : NULL;
        if (p != NULL && i < 4) {
            *p++ = '\0';
        } else if (i < 4) {
            return -1;
        }
    }
    r->name = field[0];
    r->file = field[1];
    return (parse_rate_channels(field[2], field[3], &r->rate, &r->channels) != 0
            || dsd_parse_double_strict(field[4], 0.0, 60.0, &r->lead_s) != 0)
               ? -1
               : 0;
}

static int
measure_established(rendition* r, double from_s) {
    audio a;
    if (read_audio(r->file, r->rate, r->channels, &a) != 0) {
        return -1;
    }
    const size_t from = (size_t)lround((from_s + r->lead_s) * (double)r->rate);
    const size_t to = (size_t)lround((TALKER_SECONDS + r->lead_s) * (double)r->rate);
    int rc = 0;
    /* The output must hold the whole talker: a decoder that dropped its end must not pass on what is left. */
    if (a.n < to || to < from + (size_t)r->rate) {
        DSD_FPRINTF(stderr, "speech parity: %s: %s ends at %.2f s, before the talker's end at %.2f s\n", r->name,
                    r->file, (double)a.n / (double)r->rate, (double)to / (double)r->rate);
        rc = -1;
    } else {
        r->level = dsd_test_p56_level_dbfs(a.x + from, to - from, r->rate, NULL);
    }
    free(a.x);
    return rc;
}

static int
parse_db_option(int argc, char** argv, int* i, const char* opt, double lo, double hi, double* out) {
    if (strcmp(argv[*i], opt) != 0) {
        return 0;
    }
    if (*i + 1 >= argc || dsd_parse_double_strict(argv[*i + 1], lo, hi, out) != 0) {
        DSD_FPRINTF(stderr, "speech parity: %s needs a value in %.0f..%.0f\n", opt, lo, hi);
        return -1;
    }
    *i += 2;
    return 1;
}

static int
cmd_established(int argc, char** argv) {
    double max_error = ESTABLISHED_BOUND_DB;
    double max_spread = 1.5;
    rendition r[MAX_RENDITIONS];
    int count = 0;
    for (int i = 0; i < argc;) {
        const int e = parse_db_option(argc, argv, &i, "--max-error-db", 0.0, 40.0, &max_error);
        const int s = e != 0 ? e : parse_db_option(argc, argv, &i, "--max-spread-db", 0.0, 40.0, &max_spread);
        if (s < 0) {
            return 2;
        }
        if (s > 0) {
            continue;
        }
        if (count == MAX_RENDITIONS || parse_rendition(argv[i], &r[count]) != 0) {
            DSD_FPRINTF(stderr, "speech parity: bad rendition %s (NAME:FILE:RATE:CH:LEAD_S, at most %d)\n", argv[i],
                        MAX_RENDITIONS);
            return 2;
        }
        count++;
        i++;
    }
    double from_s = 0.0;
    if (count == 0 || established_from_s(&from_s) != 0) {
        DSD_FPRINTF(stderr, "speech parity: no renditions, or the talker has no established speech\n");
        return 2;
    }
    DSD_FPRINTF(stderr, "speech parity: established speech from %.2f s of the talker to its end (%.2f s)\n", from_s,
                TALKER_SECONDS);
    int fail = 0;
    double lo = INFINITY;
    double hi = -INFINITY;
    for (int k = 0; k < count; k++) {
        if (measure_established(&r[k], from_s) != 0) {
            fail = 1;
            continue;
        }
        const double err = r[k].level - TARGET_DBFS;
        const int ok = isfinite(r[k].level) && fabs(err) <= max_error;
        DSD_FPRINTF(stderr, "speech parity: %-12s established P.56 %7.2f dBFS (%+.2f dB)%s\n", r[k].name, r[k].level,
                    err, ok ? "" : "  FAIL: outside the target window");
        fail |= !ok;
        lo = fmin(lo, r[k].level);
        hi = fmax(hi, r[k].level);
    }
    const int spread_ok = !fail && hi - lo <= max_spread;
    if (!fail) {
        DSD_FPRINTF(stderr, "speech parity: spread %.2f dB (limit %.2f)%s\n", hi - lo, max_spread,
                    spread_ok ? "" : "  FAIL");
    }
    if (fail || !spread_ok) {
        return 1;
    }
    DSD_FPRINTF(stderr, "SPEECH PARITY OK\n");
    return 0;
}

static int
whole_level(const char* file, int rate, int channels, double* level) {
    audio a;
    if (read_audio(file, rate, channels, &a) != 0) {
        return -1;
    }
    *level = dsd_test_p56_level_dbfs(a.x, a.n, rate, NULL);
    free(a.x);
    return isfinite(*level) ? 0 : -1;
}

/* Which clause of the --toward rule @p level passes by against the reference gain's @p ref, or NULL when it fails. */
static const char*
toward_verdict(double level, double ref) {
    const double err = level - TARGET_DBFS;
    if (fabs(err) > STARTUP_BOUND_DB) {
        return NULL;
    }
    if (fabs(err) < fabs(ref - TARGET_DBFS)) {
        return "nearer the target than at the reference gain";
    }
    if (err >= -ESTABLISHED_BOUND_DB && err <= STARTUP_OVER_DB) {
        return "not nearer, but within -3..+6 dB of the target";
    }
    return NULL;
}

static int
cmd_level(int argc, char** argv) {
    double lo = -200.0;
    double hi = 0.0;
    const char* toward = NULL;
    int i = 0;
    while (i < argc && strncmp(argv[i], "--", 2) == 0) {
        if (strcmp(argv[i], "--toward") == 0 && i + 1 < argc) {
            toward = argv[i + 1];
            i += 2;
            continue;
        }
        const int m = parse_db_option(argc, argv, &i, "--min-dbfs", -200.0, 0.0, &lo);
        const int x = m != 0 ? m : parse_db_option(argc, argv, &i, "--max-dbfs", -200.0, 0.0, &hi);
        if (x <= 0) {
            DSD_FPRINTF(stderr, "speech parity: bad option %s\n", argv[i]);
            return 2;
        }
    }
    int rate = 0;
    int channels = 0;
    double level = 0.0;
    if (argc - i != 3 || parse_rate_channels(argv[i + 1], argv[i + 2], &rate, &channels) != 0) {
        DSD_FPRINTF(stderr, "speech parity: level [--min-dbfs DB] [--max-dbfs DB] [--toward FILE] FILE RATE CH\n");
        return 2;
    }
    if (whole_level(argv[i], rate, channels, &level) != 0) {
        DSD_FPRINTF(stderr, "speech parity: %s has no measurable speech level\n", argv[i]);
        return 1;
    }
    int fail = !(level >= lo && level <= hi);
    DSD_FPRINTF(stderr, "speech parity: %s P.56 %.2f dBFS (window %.2f..%.2f)%s\n", argv[i], level, lo, hi,
                fail ? "  FAIL" : "");
    if (toward != NULL) {
        double ref = 0.0;
        if (whole_level(toward, rate, channels, &ref) != 0) {
            DSD_FPRINTF(stderr, "speech parity: %s has no measurable speech level\n", toward);
            return 1;
        }
        const char* verdict = toward_verdict(level, ref);
        DSD_FPRINTF(stderr, "speech parity: %s at the reference gain: P.56 %.2f dBFS (%+.2f dB), now %+.2f dB: %s\n",
                    toward, ref, ref - TARGET_DBFS, level - TARGET_DBFS,
                    verdict != NULL ? verdict : "FAIL: not moved toward the target");
        fail |= verdict == NULL;
    }
    if (fail) {
        return 1;
    }
    DSD_FPRINTF(stderr, "SPEECH LEVEL OK\n");
    return 0;
}

/* ------------------------------------------------------------------------------------------------- gated noise */

#define GATED_RATE_HZ          48000
#define GATED_FRAME            960
/* The attenuator falls 6 dB/s: this much per 20 ms frame, with room for float rounding. */
#define GATED_ATT_STEP_DB      0.1201
#define GATED_MAX_GAIN_MOVE_DB 1.0
/* The replica reads the -n 50 output rounded to int16, the decoder the float samples: a few LSB apart. */
#define GATED_MAX_DIFF_LSB     4.0
/* Frames in which the limiter acts on any sample (measured 7-15 of 99-124 on the nine fixtures). */
#define GATED_MAX_LIMITED      20

typedef struct {
    size_t frames;
    int commits;
    double gain_lo;
    double gain_hi;
    double atten_min;
    int atten_fast;
    int limited;
    double max_diff;
} gated_result;

static void
gated_frame(dsd_speech_agc* agc, const audio* fixed, const audio* auto_out, size_t at, gated_result* r) {
    float buf[GATED_FRAME];
    const float atten_before = agc->learned.atten_db;
    DSD_MEMCPY(buf, fixed->x + at, sizeof buf);
    /* One sample per call (the output is the same for any split), so a limiter that acts and releases within the
       frame still counts. */
    int limited = 0;
    for (size_t i = 0; i < GATED_FRAME; i++) {
        dsd_speech_agc_process(agc, buf + i, 1U, 1, 1, 0);
        limited |= agc->g_lim < 1.0f;
        r->max_diff = fmax(r->max_diff, fabs((double)buf[i] - (double)auto_out->x[at + i]));
    }
    const double g = (double)agc->learned.gain_db;
    r->gain_lo = fmin(r->gain_lo, g);
    r->gain_hi = fmax(r->gain_hi, g);
    r->atten_min = fmin(r->atten_min, (double)agc->learned.atten_db);
    r->atten_fast += (double)agc->learned.atten_db < (double)atten_before - GATED_ATT_STEP_DB;
    r->limited += limited;
    r->frames++;
}

static int
cmd_gated_nfm(const char* fixed_name, const char* auto_name) {
    audio fixed;
    audio auto_out;
    if (read_audio(fixed_name, GATED_RATE_HZ, 1, &fixed) != 0) {
        return 1;
    }
    if (read_audio(auto_name, GATED_RATE_HZ, 1, &auto_out) != 0) {
        free(fixed.x);
        return 1;
    }
    dsd_speech_agc* agc = (dsd_speech_agc*)calloc(1U, sizeof *agc);
    const float start = DSD_ANALOG_AUDIO_START_GAIN_FM_NARROW_DB;
    int rc = 1;
    if (agc != NULL && fixed.n == auto_out.n
        && dsd_speech_agc_init(agc, GATED_RATE_HZ, start, DSD_SPEECH_AGC_TARGET_DBFS, DSD_SPEECH_AGC_CEILING_DBFS)
               == 0) {
        dsd_speech_agc_set_noise_veto(agc, 1);
        gated_result r = {0U, 0, INFINITY, -INFINITY, 0.0, 0, 0, 0.0};
        for (size_t at = 0; at + GATED_FRAME <= fixed.n; at += GATED_FRAME) {
            gated_frame(agc, &fixed, &auto_out, at, &r);
        }
        r.commits = agc->learned.commits;
        const int same = r.max_diff <= GATED_MAX_DIFF_LSB;
        const int learned_none = r.commits == 0 && r.gain_lo >= (double)start - GATED_MAX_GAIN_MOVE_DB
                                 && r.gain_hi <= (double)start + GATED_MAX_GAIN_MOVE_DB;
        const int eased_ok = r.atten_min <= 0.0 && r.atten_fast == 0 && r.limited <= GATED_MAX_LIMITED;
        DSD_FPRINTF(stderr,
                    "speech parity: gated noise, %zu frames: output %s the decoder's (max %.1f LSB apart); %d commits, "
                    "learned gain %+.2f..%+.2f dB (start %+.2f); attenuator down to %.2f dB, %d frames faster than "
                    "6 dB/s; limiter acted on %d frames\n",
                    r.frames, same ? "is" : "FAIL: is not", r.max_diff, r.commits, r.gain_lo, r.gain_hi, (double)start,
                    r.atten_min, r.atten_fast, r.limited);
        if (!learned_none) {
            DSD_FPRINTF(stderr, "speech parity: FAIL: the AGC learned from noise\n");
        }
        if (!eased_ok) {
            DSD_FPRINTF(stderr, "speech parity: FAIL: the attenuator or the limiter misbehaved\n");
        }
        rc = (same && learned_none && eased_ok && r.frames > 0U) ? 0 : 1;
    } else {
        DSD_FPRINTF(stderr, "speech parity: gated noise: %zu and %zu samples, or no AGC\n", fixed.n, auto_out.n);
    }
    free(agc);
    free(fixed.x);
    free(auto_out.x);
    if (rc == 0) {
        DSD_FPRINTF(stderr, "GATED NOISE OK\n");
    }
    return rc;
}

int
main(int argc, char** argv) {
    if (argc == 4 && strcmp(argv[1], "source") == 0) {
        int rate = 0;
        if (dsd_parse_int_strict(argv[2], 10, 8000, 192000, &rate) != 0) {
            DSD_FPRINTF(stderr, "speech parity: source RATE OUT.wav, RATE 8000-192000\n");
            return 2;
        }
        return cmd_source(rate, argv[3]);
    }
    if (argc == 4 && strcmp(argv[1], "encode") == 0) {
        return cmd_encode(argv[2], argv[3]);
    }
    if (argc >= 3 && strcmp(argv[1], "established") == 0) {
        return cmd_established(argc - 2, argv + 2);
    }
    if (argc == 4 && strcmp(argv[1], "gated-nfm") == 0) {
        return cmd_gated_nfm(argv[2], argv[3]);
    }
    if (argc >= 5 && strcmp(argv[1], "level") == 0) {
        return cmd_level(argc - 2, argv + 2);
    }
    DSD_FPRINTF(stderr,
                "usage: %s source RATE OUT.wav | encode imbe|ambe2450|ambe2400 OUT | established ... | level ... | "
                "gated-nfm FIXED AUTO\n",
                argv[0]);
    return 2;
}
