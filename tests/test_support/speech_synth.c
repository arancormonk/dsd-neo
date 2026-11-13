// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "speech_synth.h"

#include <dsd-neo/core/safe_api.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Everything the continuous-time models make stays below this, under the 4 kHz Nyquist frequency of 8 kHz audio.
   Harmonics fade out over the last 400 Hz, so one that crosses it as the pitch moves does not click. */
#define SYNTH_BAND_TOP_HZ    3400.0
#define SYNTH_TAPER_HZ       3000.0
#define SYNTH_INT16_FULL     32768.0
/* A 0 dB-stress vowel plays this far above the talker's nominal P.56 level. Pauses, edges and stress put the meter's
   reading of long speech about this far under the vowels (measured over seeds and pitches, and checked by
   CORE_SPEECH_SYNTH). */
#define SYNTH_SPEECH_CAL_DB  3.9
#define SYNTH_FRICATIVE_DB   (-12.0)
#define SYNTH_VIBRATO_HZ     5.0
#define SYNTH_NOISE_WARMUP_S 0.05
#define SYNTH_FOREVER_S      1e9

enum {
    SYNTH_MAX_HARMONICS = 96,
    SYNTH_FRIC_PARTIALS = 24,
    SYNTH_SEG_RING = 4,
    SYNTH_VOWELS = 10,
};

/* --------------------------------------------------------------------------------------------- random numbers */

typedef struct {
    uint64_t s;
} synth_rng;

static uint64_t
synth_splitmix64(uint64_t* x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Independent streams of one seed: the speech plan, the pitch periods and the noise never share draws, so how one
   stream advances cannot depend on where the sample times fall against another's events. */
static void
synth_rng_seed(synth_rng* rng, uint64_t seed, uint64_t stream) {
    uint64_t x = seed ^ (stream * 0xD1B54A32D192ED03ULL);
    rng->s = synth_splitmix64(&x);
    if (rng->s == 0U) {
        rng->s = 1U;
    }
}

static uint64_t
synth_rng_next(synth_rng* rng) {
    uint64_t x = rng->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng->s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static double
synth_uniform(synth_rng* rng) {
    return (double)(synth_rng_next(rng) >> 11) * (1.0 / 9007199254740992.0);
}

static double
synth_range(synth_rng* rng, double lo, double hi) {
    return lo + ((hi - lo) * synth_uniform(rng));
}

static int
synth_int(synth_rng* rng, int lo, int hi) {
    return lo + (int)(synth_uniform(rng) * (double)(hi - lo + 1));
}

/* Standard normal (Box-Muller, one value per call). */
static double
synth_gauss(synth_rng* rng) {
    double u1 = synth_uniform(rng);
    if (u1 < 1e-300) {
        u1 = 1e-300;
    }
    const double u2 = synth_uniform(rng);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

static double
synth_gauss_clipped(synth_rng* rng) {
    return fmax(-3.0, fmin(3.0, synth_gauss(rng)));
}

static double
synth_amp(double dbfs) {
    return SYNTH_INT16_FULL * pow(10.0, dbfs / 20.0);
}

static int
synth_rate_ok(const float* out, int rate_hz) {
    return out != NULL && rate_hz >= DSD_TEST_SYNTH_MIN_RATE_HZ;
}

/* ------------------------------------------------------------------------------------------------ speech plan */

enum { SEG_PAUSE = 0, SEG_VOICED = 1, SEG_FRICATIVE = 2 };

/* One stretch of the plan, in model seconds. A pause or fricative holds the formants of the vowel before it, so the
   formants are defined at every time a pitch period can start. */
typedef struct {
    int kind;
    double t0;
    double t1;
    double attack;
    double release;
    double gain; /* plateau RMS, int16 units */
    double form_from[3];
    double form_to[3];
    double accent;    /* peak pitch accent, fraction */
    double phrase_t0; /* declination start; < 0 outside a phrase */
    double nu[SYNTH_FRIC_PARTIALS];
    double theta[SYNTH_FRIC_PARTIALS];
} synth_seg;

enum { NEXT_PHRASE_PAUSE, NEXT_SYLLABLE, NEXT_VOICED, NEXT_GAP, NEXT_SILENT };

typedef struct {
    synth_rng rng;
    const dsd_test_speech_params* speech; /* NULL: a single vowel */
    double formant_scale;
    double end; /* end of the newest segment */
    int next;   /* what the plan emits next */
    int syllables_left;
    double phrase_t0;
    double last_form[3];
    synth_seg ring[SYNTH_SEG_RING];
    unsigned count;
} synth_plan;

/* Peterson-Barney style F1-F3 of ten vowels (adult male), Hz. */
static const double k_vowels[SYNTH_VOWELS][3] = {
    {270.0, 2290.0, 3010.0}, {390.0, 1990.0, 2550.0}, {530.0, 1840.0, 2480.0}, {660.0, 1720.0, 2410.0},
    {730.0, 1090.0, 2440.0}, {570.0, 840.0, 2410.0},  {440.0, 1020.0, 2240.0}, {300.0, 870.0, 2240.0},
    {640.0, 1190.0, 2390.0}, {490.0, 1350.0, 1690.0},
};
static const double k_bandwidths[3] = {90.0, 110.0, 170.0};

static synth_seg*
plan_push(synth_plan* plan, int kind, double dur) {
    synth_seg* seg = &plan->ring[plan->count % SYNTH_SEG_RING];
    plan->count++;
    seg->kind = kind;
    seg->t0 = plan->end;
    seg->t1 = plan->end + dur;
    seg->attack = 0.0;
    seg->release = 0.0;
    seg->gain = 0.0;
    seg->accent = 0.0;
    seg->phrase_t0 = plan->phrase_t0;
    for (int i = 0; i < 3; i++) {
        seg->form_from[i] = plan->last_form[i];
        seg->form_to[i] = plan->last_form[i];
    }
    plan->end = seg->t1;
    return seg;
}

/* The talker level in force at @p t, plus the plan's calibration. */
static double
plan_level_gain(const synth_plan* plan, double t) {
    const dsd_test_speech_params* sp = plan->speech;
    double level = sp->level_dbfs;
    for (size_t i = 0; sp->steps != NULL && i < sp->step_count; i++) {
        if (sp->steps[i].start_s <= t) {
            level = sp->steps[i].level_dbfs;
        }
    }
    return synth_amp(level + SYNTH_SPEECH_CAL_DB);
}

static void
plan_fricative(synth_plan* plan) {
    synth_seg* seg = plan_push(plan, SEG_FRICATIVE, synth_range(&plan->rng, 0.06, 0.14));
    seg->attack = 0.015;
    seg->release = 0.025;
    seg->gain = plan_level_gain(plan, seg->t0) * pow(10.0, SYNTH_FRICATIVE_DB / 20.0);
    for (int k = 0; k < SYNTH_FRIC_PARTIALS; k++) {
        seg->nu[k] = synth_range(&plan->rng, 1800.0, SYNTH_BAND_TOP_HZ);
        seg->theta[k] = synth_range(&plan->rng, 0.0, 2.0 * M_PI);
    }
}

static void
plan_voiced(synth_plan* plan) {
    synth_seg* seg = plan_push(plan, SEG_VOICED, synth_range(&plan->rng, 0.15, 0.30));
    const int vowel = synth_int(&plan->rng, 0, SYNTH_VOWELS - 1);
    const int locus = synth_int(&plan->rng, 0, SYNTH_VOWELS - 1);
    for (int i = 0; i < 3; i++) {
        const double target = k_vowels[vowel][i] * plan->formant_scale;
        seg->form_from[i] = (0.6 * target) + (0.4 * k_vowels[locus][i] * plan->formant_scale);
        seg->form_to[i] = target;
        plan->last_form[i] = target;
    }
    seg->gain = plan_level_gain(plan, seg->t0) * pow(10.0, synth_range(&plan->rng, -4.0, 2.0) / 20.0);
    seg->accent = synth_range(&plan->rng, 0.0, 0.15);
    seg->attack = synth_range(&plan->rng, 0.015, 0.04);
    seg->release = synth_range(&plan->rng, 0.03, 0.08);
}

/* A syllable's vowel; after the phrase's last one comes the pause between phrases. */
static void
plan_syllable_vowel(synth_plan* plan) {
    plan_voiced(plan);
    plan->syllables_left--;
    plan->next = plan->syllables_left > 0 ? NEXT_GAP : NEXT_PHRASE_PAUSE;
}

/* Emit the speech plan's next segment. */
static void
plan_speech_step(synth_plan* plan) {
    switch (plan->next) {
        case NEXT_PHRASE_PAUSE:
            plan->phrase_t0 = -1.0;
            (void)plan_push(plan, SEG_PAUSE, plan->count == 0U ? 0.05 : synth_range(&plan->rng, 0.4, 1.0));
            plan->phrase_t0 = plan->end;
            plan->syllables_left = synth_int(&plan->rng, 3, 7);
            plan->next = NEXT_SYLLABLE;
            break;
        case NEXT_SYLLABLE:
            if (synth_uniform(&plan->rng) < (1.0 / 3.0)) {
                plan_fricative(plan);
                plan->next = NEXT_VOICED;
            } else {
                plan_syllable_vowel(plan);
            }
            break;
        case NEXT_VOICED: plan_syllable_vowel(plan); break;
        case NEXT_GAP:
            (void)plan_push(plan, SEG_PAUSE, synth_range(&plan->rng, 0.02, 0.15));
            plan->next = NEXT_SYLLABLE;
            break;
        default: (void)plan_push(plan, SEG_PAUSE, SYNTH_FOREVER_S); break;
    }
}

/* Plan until the newest segment covers @p t. Segments last at least 20 ms, far longer than a sample or pitch period,
   so the ring always still holds the segment of every time the generator looks up. */
static void
plan_cover(synth_plan* plan, double t) {
    while (plan->count == 0U || plan->end <= t) {
        plan_speech_step(plan);
    }
}

static const synth_seg*
plan_lookup(const synth_plan* plan, double t) {
    const unsigned held = plan->count < SYNTH_SEG_RING ? plan->count : SYNTH_SEG_RING;
    const synth_seg* seg = &plan->ring[(plan->count - 1U) % SYNTH_SEG_RING];
    for (unsigned back = 0; back < held; back++) {
        const synth_seg* s = &plan->ring[(plan->count - 1U - back) % SYNTH_SEG_RING];
        seg = s;
        if (s->t0 <= t) {
            break;
        }
    }
    return seg;
}

static double
seg_envelope(const synth_seg* seg, double t) {
    if (seg->kind == SEG_PAUSE || t < seg->t0 || t >= seg->t1) {
        return 0.0;
    }
    const double rise = t - seg->t0;
    const double fall = seg->t1 - t;
    if (rise < seg->attack) {
        return 0.5 - (0.5 * cos(M_PI * rise / seg->attack));
    }
    if (fall < seg->release) {
        return 0.5 - (0.5 * cos(M_PI * fall / seg->release));
    }
    return 1.0;
}

static void
seg_formants(const synth_seg* seg, double t, double form[3]) {
    double u = 1.0;
    if (seg->kind == SEG_VOICED) {
        u = fmin(1.0, fmax(0.0, (t - seg->t0) / (0.4 * (seg->t1 - seg->t0))));
        u = u * u * (3.0 - (2.0 * u));
    }
    for (int i = 0; i < 3; i++) {
        form[i] = seg->form_from[i] + ((seg->form_to[i] - seg->form_from[i]) * u);
    }
}

/* The pitch contour's slow part at @p t: phrase declination and the syllable's accent. */
static double
seg_pitch_factor(const synth_seg* seg, double t) {
    double f = 1.0;
    if (seg->phrase_t0 >= 0.0) {
        f = fmax(0.88, 1.08 - (0.08 * (t - seg->phrase_t0)));
    }
    if (seg->kind == SEG_VOICED) {
        f *= 1.0 + (seg->accent * sin(M_PI * (t - seg->t0) / (seg->t1 - seg->t0)));
    }
    return f;
}

/* ------------------------------------------------------------------------------------------------ voice source */

typedef struct {
    synth_rng rng;
    double f0;
    double jitter;
    double vibrato;
    double shimmer;
    double epoch;  /* start of the current pitch period */
    double period; /* its length, s */
    double scale;  /* its shimmer factor */
    int harmonics;
    double amp[SYNTH_MAX_HARMONICS];
} synth_voice;

/* Harmonic amplitudes of one period: -6 dB/octave through the formant resonators, faded out near the band top, then
   normalised to unit RMS so the level never depends on the pitch or the vowel. */
static void
voice_shape(synth_voice* v, double f, const double form[3]) {
    double power = 0.0;
    int h = 0;
    while (h < SYNTH_MAX_HARMONICS && (double)(h + 1) * f < SYNTH_BAND_TOP_HZ) {
        const double fh = (double)(h + 1) * f;
        double a = 1.0 / (double)(h + 1);
        for (int i = 0; i < 3; i++) {
            const double x = fh / form[i];
            const double re = 1.0 - (x * x);
            const double im = fh * k_bandwidths[i] / (form[i] * form[i]);
            a /= sqrt((re * re) + (im * im));
        }
        if (fh > SYNTH_TAPER_HZ) {
            const double c = cos(0.5 * M_PI * (fh - SYNTH_TAPER_HZ) / (SYNTH_BAND_TOP_HZ - SYNTH_TAPER_HZ));
            a *= c * c;
        }
        v->amp[h] = a;
        power += 0.5 * a * a;
        h++;
    }
    v->harmonics = h;
    const double norm = power > 0.0 ? 1.0 / sqrt(power) : 0.0;
    for (int k = 0; k < h; k++) {
        v->amp[k] *= norm;
    }
}

/* Start the pitch period that begins at @p te: its length and shape come from the contour at that instant. */
static void
voice_start_period(synth_voice* v, synth_plan* plan, double te) {
    plan_cover(plan, te);
    const synth_seg* seg = plan_lookup(plan, te);
    double form[3];
    seg_formants(seg, te, form);
    const double f = v->f0 * seg_pitch_factor(seg, te) * (1.0 + (v->vibrato * sin(2.0 * M_PI * SYNTH_VIBRATO_HZ * te)));
    v->epoch = te;
    v->period = (1.0 + (v->jitter * synth_gauss_clipped(&v->rng))) / f;
    v->scale = 1.0 + (v->shimmer * synth_gauss_clipped(&v->rng));
    voice_shape(v, f, form);
}

static double
voice_value(const synth_voice* v, double t) {
    const double phi = 2.0 * M_PI * (t - v->epoch) / v->period;
    /* sin(h phi) by the Chebyshev recurrence: one sin and one cos per sample. */
    const double c2 = 2.0 * cos(phi);
    double s_prev = 0.0;
    double s = sin(phi);
    double y = 0.0;
    for (int h = 0; h < v->harmonics; h++) {
        y += v->amp[h] * s;
        const double s_next = (c2 * s) - s_prev;
        s_prev = s;
        s = s_next;
    }
    return y * v->scale;
}

static double
fricative_value(const synth_seg* seg, double t) {
    double y = 0.0;
    for (int k = 0; k < SYNTH_FRIC_PARTIALS; k++) {
        y += sin((2.0 * M_PI * seg->nu[k] * t) + seg->theta[k]);
    }
    return y * sqrt(2.0 / (double)SYNTH_FRIC_PARTIALS);
}

typedef struct {
    synth_plan plan;
    synth_voice voice;
} synth_talker;

static void
talker_render(synth_talker* tk, float* out, size_t n, int rate_hz, double start_s) {
    voice_start_period(&tk->voice, &tk->plan, 0.0);
    while (tk->voice.epoch + tk->voice.period <= start_s) {
        voice_start_period(&tk->voice, &tk->plan, tk->voice.epoch + tk->voice.period);
    }
    for (size_t m = 0; m < n; m++) {
        const double t = start_s + ((double)m / (double)rate_hz);
        plan_cover(&tk->plan, t);
        while (tk->voice.epoch + tk->voice.period <= t) {
            voice_start_period(&tk->voice, &tk->plan, tk->voice.epoch + tk->voice.period);
        }
        const synth_seg* seg = plan_lookup(&tk->plan, t);
        const double env = seg_envelope(seg, t);
        double y = 0.0;
        if (env > 0.0 && seg->kind == SEG_VOICED) {
            y = seg->gain * env * voice_value(&tk->voice, t);
        } else if (env > 0.0) {
            y = seg->gain * env * fricative_value(seg, t);
        }
        out[m] = (float)y;
    }
}

static void
talker_init(synth_talker* tk, uint64_t seed, double f0, double jitter, double vibrato, double shimmer) {
    synth_plan* plan = &tk->plan;
    DSD_MEMSET(plan->ring, 0, sizeof(plan->ring));
    synth_rng_seed(&plan->rng, seed, 1U);
    plan->speech = NULL;
    /* Higher voices have shorter vocal tracts: formants up to 17% higher at 200 Hz and above. */
    plan->formant_scale = 1.0 + (0.17 * fmin(1.0, fmax(0.0, (f0 - 140.0) / 60.0)));
    plan->end = 0.0;
    plan->next = NEXT_PHRASE_PAUSE;
    plan->syllables_left = 0;
    plan->phrase_t0 = -1.0;
    for (int i = 0; i < 3; i++) {
        plan->last_form[i] = k_vowels[4][i] * plan->formant_scale;
    }
    plan->count = 0U;
    synth_voice* v = &tk->voice;
    synth_rng_seed(&v->rng, seed, 2U);
    v->f0 = f0;
    v->jitter = jitter;
    v->vibrato = vibrato;
    v->shimmer = shimmer;
    v->epoch = 0.0;
    v->period = 1.0 / f0;
    v->scale = 1.0;
    v->harmonics = 0;
}

void
dsd_test_speech_defaults(dsd_test_speech_params* p, uint64_t seed) {
    if (p == NULL) {
        return;
    }
    p->seed = seed;
    p->level_dbfs = -30.0;
    p->f0_hz = 120.0;
    p->jitter = 0.01;
    p->vibrato = 0.01;
    p->shimmer = 0.03;
    p->start_s = 0.0;
    p->steps = NULL;
    p->step_count = 0;
}

/* Bounded so a pitch period stays positive (jitter is clipped at 3 standard deviations) and the pitch stays within
   the harmonic table. */
static int
speech_params_ok(const dsd_test_speech_params* p) {
    return p->f0_hz >= 50.0 && p->f0_hz <= 400.0 && p->jitter >= 0.0 && p->jitter <= 0.1 && p->vibrato >= 0.0
           && p->vibrato <= 0.1 && p->shimmer >= 0.0 && p->shimmer <= 0.3 && p->start_s >= 0.0
           && (p->step_count == 0 || p->steps != NULL);
}

int
dsd_test_synth_speech(float* out, size_t n, int rate_hz, const dsd_test_speech_params* p) {
    if (!synth_rate_ok(out, rate_hz) || p == NULL || !speech_params_ok(p)) {
        return -1;
    }
    synth_talker tk;
    talker_init(&tk, p->seed, p->f0_hz, p->jitter, p->vibrato, p->shimmer);
    tk.plan.speech = p;
    talker_render(&tk, out, n, rate_hz, p->start_s);
    return 0;
}

void
dsd_test_vowel_defaults(dsd_test_vowel_params* v, uint64_t seed) {
    if (v == NULL) {
        return;
    }
    v->seed = seed;
    v->level_dbfs = -36.0;
    v->f0_hz = 120.0;
    v->duration_s = 0.5;
    v->steady = 0;
}

int
dsd_test_synth_vowel(float* out, size_t n, int rate_hz, const dsd_test_vowel_params* v) {
    if (!synth_rate_ok(out, rate_hz) || v == NULL || !(v->f0_hz >= 50.0 && v->f0_hz <= 450.0)
        || !(v->duration_s >= 0.04)) {
        return -1;
    }
    const int natural = v->steady ? 0 : 1;
    synth_talker tk;
    talker_init(&tk, v->seed, v->f0_hz, 0.015 * natural, 0.01 * natural, 0.03 * natural);
    synth_plan* plan = &tk.plan;
    plan->formant_scale = 1.0;
    for (int i = 0; i < 3; i++) {
        plan->last_form[i] = k_vowels[4][i];
    }
    synth_seg* seg = plan_push(plan, SEG_VOICED, v->duration_s);
    seg->attack = 0.02;
    seg->release = 0.02;
    seg->gain = synth_amp(v->level_dbfs);
    plan->next = NEXT_SILENT;
    talker_render(&tk, out, n, rate_hz, 0.0);
    return 0;
}

/* ------------------------------------------------------------------------------------------- steady signals */

int
dsd_test_synth_tone(float* out, size_t n, int rate_hz, double hz, double rms_dbfs) {
    if (!synth_rate_ok(out, rate_hz) || !(hz > 0.0 && hz < 0.5 * (double)rate_hz)) {
        return -1;
    }
    const double a = sqrt(2.0) * synth_amp(rms_dbfs);
    for (size_t m = 0; m < n; m++) {
        out[m] = (float)(a * sin(2.0 * M_PI * hz * ((double)m / (double)rate_hz)));
    }
    return 0;
}

static int
dtmf_pair(char digit, double* row, double* col) {
    static const char k_keys[] = "123A456B789C*0#D";
    static const double k_rows[4] = {697.0, 770.0, 852.0, 941.0};
    static const double k_cols[4] = {1209.0, 1336.0, 1477.0, 1633.0};
    for (int i = 0; i < 16; i++) {
        if (k_keys[i] == digit) {
            *row = k_rows[i / 4];
            *col = k_cols[i % 4];
            return 0;
        }
    }
    return -1;
}

int
dsd_test_synth_dtmf(float* out, size_t n, int rate_hz, char digit, double rms_dbfs) {
    double row = 0.0;
    double col = 0.0;
    if (!synth_rate_ok(out, rate_hz) || dtmf_pair(digit, &row, &col) != 0) {
        return -1;
    }
    /* Two uncorrelated sines of amplitude a carry a^2 / 2 each: the pair's RMS is a. */
    const double a = synth_amp(rms_dbfs);
    for (size_t m = 0; m < n; m++) {
        const double t = (double)m / (double)rate_hz;
        out[m] = (float)(a * (sin(2.0 * M_PI * row * t) + sin(2.0 * M_PI * col * t)));
    }
    return 0;
}

int
dsd_test_synth_heterodyne(float* out, size_t n, int rate_hz, double start_hz, double drift_hz_per_s, double rms_dbfs) {
    if (!synth_rate_ok(out, rate_hz)) {
        return -1;
    }
    const double end_hz = start_hz + (drift_hz_per_s * ((double)n / (double)rate_hz));
    if (!(fmin(start_hz, end_hz) >= 20.0 && fmax(start_hz, end_hz) <= SYNTH_BAND_TOP_HZ)) {
        return -1;
    }
    const double a = sqrt(2.0) * synth_amp(rms_dbfs);
    for (size_t m = 0; m < n; m++) {
        const double t = (double)m / (double)rate_hz;
        out[m] = (float)(a * sin(2.0 * M_PI * ((start_hz * t) + (0.5 * drift_hz_per_s * t * t))));
    }
    return 0;
}

int
dsd_test_synth_mains(float* out, size_t n, int rate_hz, dsd_test_mains_kind kind, double mains_hz, double rms_dbfs) {
    if (!synth_rate_ok(out, rate_hz) || !(mains_hz >= 40.0 && mains_hz <= 70.0)
        || (kind != DSD_TEST_MAINS_HUM && kind != DSD_TEST_MAINS_BUZZ)) {
        return -1;
    }
    double freq[SYNTH_MAX_HARMONICS];
    double amp[SYNTH_MAX_HARMONICS];
    int count = 0;
    if (kind == DSD_TEST_MAINS_HUM) {
        for (int h = 1; h <= 3; h++) {
            freq[count] = mains_hz * (double)h;
            amp[count] = pow(0.5, (double)(h - 1));
            count++;
        }
    } else {
        for (int h = 1; count < SYNTH_MAX_HARMONICS && 2.0 * mains_hz * (double)h <= SYNTH_BAND_TOP_HZ; h++) {
            if (2.0 * mains_hz * (double)h >= 300.0) {
                freq[count] = 2.0 * mains_hz * (double)h;
                amp[count] = 1.0;
                count++;
            }
        }
    }
    double power = 0.0;
    for (int k = 0; k < count; k++) {
        power += 0.5 * amp[k] * amp[k];
    }
    const double scale = synth_amp(rms_dbfs) / sqrt(power);
    for (size_t m = 0; m < n; m++) {
        const double t = (double)m / (double)rate_hz;
        double y = 0.0;
        for (int k = 0; k < count; k++) {
            y += amp[k] * cos(2.0 * M_PI * freq[k] * t);
        }
        out[m] = (float)(y * scale);
    }
    return 0;
}

/* ----------------------------------------------------------------------------------------------------- noise */

typedef struct {
    double b0, b1, b2, a1, a2;
    double x1, x2, y1, y2;
} synth_biquad;

enum { SYNTH_MAX_BIQUADS = 4 };

typedef struct {
    synth_biquad stage[SYNTH_MAX_BIQUADS];
    int stages;
    double deemph_a; /* one-pole de-emphasis coefficient; 0: none */
    double deemph_y;
} synth_noise_filter;

/* One section of a 4th-order Butterworth (RBJ cookbook biquad at the section's Q). */
static void
biquad_design(synth_biquad* bq, int highpass, double hz, double q, int rate_hz) {
    const double w0 = 2.0 * M_PI * hz / (double)rate_hz;
    const double cw = cos(w0);
    const double alpha = sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    const double edge = highpass ? (1.0 + cw) : (1.0 - cw);
    bq->b0 = 0.5 * edge / a0;
    bq->b1 = (highpass ? -edge : edge) / a0;
    bq->b2 = bq->b0;
    bq->a1 = (-2.0 * cw) / a0;
    bq->a2 = (1.0 - alpha) / a0;
    bq->x1 = bq->x2 = bq->y1 = bq->y2 = 0.0;
}

static void
noise_filter_add(synth_noise_filter* f, int highpass, double hz, int rate_hz) {
    static const double k_q[2] = {0.54119610, 1.30656296};
    for (int i = 0; i < 2; i++) {
        biquad_design(&f->stage[f->stages], highpass, hz, k_q[i], rate_hz);
        f->stages++;
    }
}

static void
noise_filter_init(synth_noise_filter* f, dsd_test_noise_kind kind, int rate_hz) {
    f->stages = 0;
    f->deemph_a = 0.0;
    f->deemph_y = 0.0;
    if (kind == DSD_TEST_NOISE_DEEMPH) {
        f->deemph_a = exp(-1.0 / ((double)rate_hz * 75e-6));
    }
    if (kind == DSD_TEST_NOISE_VOICE_BAND || kind == DSD_TEST_NOISE_DEEMPH) {
        noise_filter_add(f, 1, 300.0, rate_hz);
        noise_filter_add(f, 0, SYNTH_BAND_TOP_HZ, rate_hz);
    } else if (kind == DSD_TEST_NOISE_RUMBLE) {
        noise_filter_add(f, 0, 100.0, rate_hz);
    }
}

static double
noise_filter_run(synth_noise_filter* f, double x) {
    if (f->deemph_a > 0.0) {
        f->deemph_y = ((1.0 - f->deemph_a) * x) + (f->deemph_a * f->deemph_y);
        x = f->deemph_y;
    }
    for (int i = 0; i < f->stages; i++) {
        synth_biquad* bq = &f->stage[i];
        const double y = (bq->b0 * x) + (bq->b1 * bq->x1) + (bq->b2 * bq->x2) - (bq->a1 * bq->y1) - (bq->a2 * bq->y2);
        bq->x2 = bq->x1;
        bq->x1 = x;
        bq->y2 = bq->y1;
        bq->y1 = y;
        x = y;
    }
    return x;
}

int
dsd_test_deemphasis(float* buf, size_t n, int rate_hz, double tau_s) {
    if (!synth_rate_ok(buf, rate_hz) || !(tau_s > 0.0) || !isfinite(tau_s)) {
        return -1;
    }
    const double a = exp(-1.0 / ((double)rate_hz * tau_s));
    double y = 0.0;
    for (size_t i = 0; i < n; i++) {
        y = ((1.0 - a) * (double)buf[i]) + (a * y);
        buf[i] = (float)y;
    }
    return 0;
}

int
dsd_test_synth_noise(float* out, size_t n, int rate_hz, uint64_t seed, dsd_test_noise_kind kind, double rms_dbfs) {
    if (!synth_rate_ok(out, rate_hz) || (int)kind < DSD_TEST_NOISE_WHITE || (int)kind > DSD_TEST_NOISE_RUMBLE) {
        return -1;
    }
    synth_rng rng;
    synth_rng_seed(&rng, seed, 3U);
    synth_noise_filter filter;
    noise_filter_init(&filter, kind, rate_hz);
    const long warmup = lround(SYNTH_NOISE_WARMUP_S * (double)rate_hz);
    for (long i = 0; i < warmup; i++) {
        (void)noise_filter_run(&filter, synth_gauss(&rng));
    }
    double sum_sq = 0.0;
    for (size_t m = 0; m < n; m++) {
        const double y = noise_filter_run(&filter, synth_gauss(&rng));
        out[m] = (float)y;
        sum_sq += y * y;
    }
    if (n == 0 || !(sum_sq > 0.0)) {
        return 0;
    }
    const double scale = synth_amp(rms_dbfs) / sqrt(sum_sq / (double)n);
    for (size_t m = 0; m < n; m++) {
        out[m] = (float)((double)out[m] * scale);
    }
    return 0;
}
