// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic analog monitor audio for the tone detector tests (issue #522): sub-audible
 * tones, in-band white noise and speech-like audio, all driven by a seeded generator so a
 * failure reproduces bit for bit.
 *
 * The speech model is deliberately hostile to a CTCSS detector. It is unfiltered -- nothing
 * removes the voice fundamental below 300 Hz the way a transmitter's voice high-pass would --
 * so every voiced segment puts a strong 85-255 Hz fundamental into the sub-audible band. The
 * fundamental jitters from cycle to cycle, drifts with intonation and carries vibrato, which
 * is what real voices do and what separates them from a tone.
 */

#ifndef DSD_NEO_TESTS_DSP_ANALOG_TONE_SYNTH_H_
#define DSD_NEO_TESTS_DSP_ANALOG_TONE_SYNTH_H_

#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** @brief xorshift64* generator: tests must not share process-global rand() state. */
typedef struct {
    uint64_t s;
} synth_rng;

static inline void
synth_rng_seed(synth_rng* rng, uint64_t seed) {
    rng->s = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

static inline uint64_t
synth_rng_next(synth_rng* rng) {
    uint64_t x = rng->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng->s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/** @brief Uniform in [0, 1). */
static inline double
synth_uniform(synth_rng* rng) {
    return (double)(synth_rng_next(rng) >> 11) * (1.0 / 9007199254740992.0);
}

/** @brief Uniform in [lo, hi). */
static inline double
synth_range(synth_rng* rng, double lo, double hi) {
    return lo + ((hi - lo) * synth_uniform(rng));
}

/** @brief Standard normal (Box-Muller, one value per call). */
static inline double
synth_gauss(synth_rng* rng) {
    double u1 = synth_uniform(rng);
    if (u1 < 1e-300) {
        u1 = 1e-300;
    }
    const double u2 = synth_uniform(rng);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/**
 * @brief White-noise standard deviation that puts @p snr_db of tone-to-noise in the band.
 *
 * "In-band" is the detector's sub-audible band, 0-290 Hz: white noise of variance s^2 at
 * rate fs carries s^2 * 290 / (fs / 2) of its power there. A tone of amplitude A has power
 * A^2 / 2.
 */
static inline double
synth_inband_noise_sigma(double tone_amp, double snr_db, double fs) {
    const double tone_power = tone_amp * tone_amp / 2.0;
    const double inband_noise = tone_power / pow(10.0, snr_db / 10.0);
    return sqrt(inband_noise * (fs / 2.0) / 290.0);
}

/** @brief A phase-continuous tone generator (the phase may be flipped for a reverse burst). */
typedef struct {
    double fs;
    double hz;
    double amp;
    double phase;
} synth_tone;

static inline float
synth_tone_next(synth_tone* tone) {
    const double v = tone->amp * cos(tone->phase);
    tone->phase += 2.0 * M_PI * tone->hz / tone->fs;
    if (tone->phase > 2.0 * M_PI) {
        tone->phase -= 2.0 * M_PI;
    }
    return (float)v;
}

/**
 * @brief Speech-like audio: voiced phrases with a wandering fundamental, fricatives, pauses.
 *
 * Voiced segments are a glottal pulse train through a -12 dB/octave source tilt, three
 * formant resonators and +6 dB/octave lip radiation, the standard source-filter model; the
 * fundamental is the strongest single harmonic below the first formant. Per phrase the fundamental starts
 * anywhere in 85-255 Hz; inside it, it drifts with intonation (up to +/-40 Hz/s), carries a
 * 5 Hz vibrato of about 1% and a cycle-to-cycle jitter of about 1%.
 */
typedef struct {
    double fs;
    synth_rng rng;
    int segment_left; /**< samples left in the current segment */
    int kind;         /**< 0 silence, 1 voiced, 2 unvoiced */
    double f0;        /**< current fundamental */
    double f0_slope;  /**< Hz per second */
    double f0_base;
    double vibrato_phase;
    double period_left; /**< samples until the next glottal pulse */
    double env;
    double env_target;
    double tilt1;
    double tilt2;
    double res_y1[3];
    double res_y2[3];
    double res_a1[3];
    double res_a2[3];
    double res_g[3];
    double radiation_x1;
    double level;
} synth_speech;

static inline void
synth_speech_resonator(synth_speech* sp, int i, double hz, double bw) {
    const double r = exp(-M_PI * bw / sp->fs);
    sp->res_a1[i] = 2.0 * r * cos(2.0 * M_PI * hz / sp->fs);
    sp->res_a2[i] = -r * r;
    sp->res_g[i] = 1.0 - r;
}

static inline void
synth_speech_new_segment(synth_speech* sp) {
    const double pick = synth_uniform(&sp->rng);
    if (pick < 0.6) {
        sp->kind = 1;
        sp->segment_left = (int)(sp->fs * synth_range(&sp->rng, 0.15, 0.6));
        if (synth_uniform(&sp->rng) < 0.4) {
            sp->f0_base = synth_range(&sp->rng, 85.0, 255.0);
        }
        sp->f0 = sp->f0_base * synth_range(&sp->rng, 0.9, 1.1);
        sp->f0_slope = synth_range(&sp->rng, -40.0, 40.0);
        synth_speech_resonator(sp, 0, synth_range(&sp->rng, 350.0, 850.0), 90.0);
        synth_speech_resonator(sp, 1, synth_range(&sp->rng, 900.0, 2300.0), 120.0);
        synth_speech_resonator(sp, 2, synth_range(&sp->rng, 2300.0, 3200.0), 180.0);
        sp->env_target = synth_range(&sp->rng, 0.5, 1.0);
    } else if (pick < 0.8) {
        sp->kind = 2;
        sp->segment_left = (int)(sp->fs * synth_range(&sp->rng, 0.05, 0.15));
        sp->env_target = synth_range(&sp->rng, 0.1, 0.3);
    } else {
        sp->kind = 0;
        sp->segment_left = (int)(sp->fs * synth_range(&sp->rng, 0.05, 0.3));
        sp->env_target = 0.0;
    }
}

static inline void
synth_speech_init(synth_speech* sp, double fs, uint64_t seed, double level) {
    for (int i = 0; i < 3; i++) {
        sp->res_y1[i] = 0.0;
        sp->res_y2[i] = 0.0;
    }
    sp->fs = fs;
    synth_rng_seed(&sp->rng, seed);
    sp->level = level;
    sp->segment_left = 0;
    sp->kind = 0;
    sp->f0_base = synth_range(&sp->rng, 85.0, 255.0);
    sp->f0 = sp->f0_base;
    sp->f0_slope = 0.0;
    sp->vibrato_phase = 0.0;
    sp->period_left = 0.0;
    sp->env = 0.0;
    sp->env_target = 0.0;
    sp->tilt1 = 0.0;
    sp->tilt2 = 0.0;
    sp->radiation_x1 = 0.0;
    synth_speech_resonator(sp, 0, 500.0, 90.0);
    synth_speech_resonator(sp, 1, 1500.0, 120.0);
    synth_speech_resonator(sp, 2, 2500.0, 180.0);
}

static inline double
synth_speech_excitation(synth_speech* sp) {
    if (sp->kind == 2) {
        return 0.3 * synth_gauss(&sp->rng);
    }
    if (sp->kind != 1) {
        return 0.0;
    }
    sp->f0 += sp->f0_slope / sp->fs;
    if (sp->f0 < 70.0 || sp->f0 > 300.0) {
        sp->f0_slope = -sp->f0_slope;
    }
    sp->vibrato_phase += 2.0 * M_PI * 5.0 / sp->fs;
    sp->period_left -= 1.0;
    if (sp->period_left > 0.0) {
        return 0.0;
    }
    const double f = sp->f0 * (1.0 + (0.01 * sin(sp->vibrato_phase)));
    const double jitter = 1.0 + (0.01 * synth_gauss(&sp->rng));
    sp->period_left += (sp->fs / f) * jitter;
    return 1.0;
}

static inline float
synth_speech_next(synth_speech* sp) {
    if (sp->segment_left-- <= 0) {
        synth_speech_new_segment(sp);
    }
    sp->env += (sp->env_target - sp->env) * (1.0 / (0.02 * sp->fs));
    double x = synth_speech_excitation(sp);
    /* Glottal source tilt: two one-pole low-passes at about 100 Hz. */
    const double a = exp(-2.0 * M_PI * 100.0 / sp->fs);
    sp->tilt1 = (a * sp->tilt1) + ((1.0 - a) * x * 40.0);
    sp->tilt2 = (a * sp->tilt2) + ((1.0 - a) * sp->tilt1);
    double src = (sp->kind == 1) ? sp->tilt2 : x;
    double out = src;
    for (int i = 0; i < 3; i++) {
        const double y = (sp->res_g[i] * src) + (sp->res_a1[i] * sp->res_y1[i]) + (sp->res_a2[i] * sp->res_y2[i]);
        sp->res_y2[i] = sp->res_y1[i];
        sp->res_y1[i] = y;
        out += y * 4.0;
    }
    /* Lip radiation: a first difference, +6 dB/octave. With the -12 dB/octave source tilt it
       gives the -6 dB/octave slope of radiated speech (the classic source-filter model). */
    const double radiated = out - (0.97 * sp->radiation_x1);
    sp->radiation_x1 = out;
    return (float)(radiated * sp->env * sp->level * 8.0);
}

/**
 * @brief Fourth-order Butterworth high-pass at 300 Hz: a transmitter's voice filter.
 *
 * Real FM transmitters high-pass the voice before adding the tone, which is what makes
 * CTCSS work at all; speech run through this is the "voice over a tone" case, while the
 * unfiltered model above is the hostile "PCM speech" case.
 */
typedef struct {
    double b0[2], b1[2], b2[2], a1[2], a2[2];
    double x1[2], x2[2], y1[2], y2[2];
} synth_voice_hpf;

static inline void
synth_voice_hpf_init(synth_voice_hpf* f, double fs) {
    static const double q[2] = {0.5411961, 1.3065630};
    const double w0 = 2.0 * M_PI * 300.0 / fs;
    for (int i = 0; i < 2; i++) {
        const double alpha = sin(w0) / (2.0 * q[i]);
        const double a0 = 1.0 + alpha;
        f->b0[i] = ((1.0 + cos(w0)) / 2.0) / a0;
        f->b1[i] = -(1.0 + cos(w0)) / a0;
        f->b2[i] = f->b0[i];
        f->a1[i] = (-2.0 * cos(w0)) / a0;
        f->a2[i] = (1.0 - alpha) / a0;
        f->x1[i] = f->x2[i] = f->y1[i] = f->y2[i] = 0.0;
    }
}

static inline float
synth_voice_hpf_next(synth_voice_hpf* f, float in) {
    double x = in;
    for (int i = 0; i < 2; i++) {
        const double y = (f->b0[i] * x) + (f->b1[i] * f->x1[i]) + (f->b2[i] * f->x2[i]) - (f->a1[i] * f->y1[i])
                         - (f->a2[i] * f->y2[i]);
        f->x2[i] = f->x1[i];
        f->x1[i] = x;
        f->y2[i] = f->y1[i];
        f->y1[i] = y;
        x = y;
    }
    return (float)x;
}

#endif /* DSD_NEO_TESTS_DSP_ANALOG_TONE_SYNTH_H_ */
