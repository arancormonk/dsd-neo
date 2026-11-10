// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic speech-like and non-speech test signals for the voice level tests.
 *
 * Scale: every generator writes float samples in int16 units (full scale 32768) and never clips; a level set hot
 * enough gives samples past +-32768. Levels are in dBFS re 32768. Tones and noise take an RMS level, so a sine at
 * -3.01 dBFS peaks at full scale. Speech takes a nominal P.56 active level (p56.h); see dsd_test_speech_params.
 *
 * Same waveform at every rate. Speech, vowels, tones, DTMF, hum, buzz and heterodyne are continuous-time models:
 * every random choice (syllable timing, vowels, pitch periods, jitter) is drawn in model time from the seed, never per
 * sample, and every component stays below 3.4 kHz. Sample m at any rate is the model's value at t = m / rate, so a
 * signal generated at 8, 22.05 and 48 kHz is one waveform sampled three ways, matching to float rounding where the
 * sample times coincide. Any rate of at least 8000 Hz works.
 *
 * Noise is the exception: white, band-limited, de-emphasised and rumble noise are drawn per sample at the requested
 * rate. One seed always gives the same samples at one rate, and the level and spectrum shape match across rates, but
 * the samples do not.
 *
 * Random numbers come from a seeded xorshift64* generator inside this module, never from libc rand(), so one seed
 * gives the same samples in every process.
 *
 * Exact zeros need no generator: speech pauses are exact zeros already, and a test that needs a run of zeros clears
 * its buffer with DSD_MEMSET.
 */

#ifndef DSD_NEO_TESTS_TEST_SUPPORT_SPEECH_SYNTH_H_
#define DSD_NEO_TESTS_TEST_SUPPORT_SPEECH_SYNTH_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Lowest sample rate the generators accept: everything they make stays below 3.4 kHz. */
#define DSD_TEST_SYNTH_MIN_RATE_HZ 8000

/** @brief A talker level change: syllables that start at or after @p start_s play at @p level_dbfs. */
typedef struct {
    double start_s;
    double level_dbfs;
} dsd_test_level_step;

/**
 * @brief Running speech: phrases of syllables with real pauses between them.
 *
 * The model:
 * - Phrases of 3-7 syllables, separated by pauses of 0.4-1.0 s of exact zeros (longer than the gaps of about 0.3 s
 *   that P.56 bridges, so they count as pauses). Inside a phrase, syllables are 150-300 ms with gaps of 20-150 ms, and
 *   about one in three opens with a 60-140 ms fricative.
 * - Voiced sound: a glottal pulse train with integrated phase. Each pitch period is drawn when it starts, from the
 *   pitch contour (the talker's pitch, falling 8% per second through each phrase from 8% above it, a per-syllable
 *   accent of up to +15%, and a 5 Hz vibrato), times a cycle-to-cycle jitter. Its harmonics fall at 6 dB per octave
 *   (glottal source plus lip radiation) through three formant resonators that glide from a consonant transition to
 *   the syllable's vowel, and stop below 3.4 kHz. Each period carries a small random amplitude change (shimmer).
 * - Fricatives: noise-like sums of 24 sinusoids at random frequencies between 1.8 and 3.4 kHz, 12 dB under the vowels.
 * - Syllable envelopes rise and fall with raised-cosine edges; each syllable has a random stress of -4 to +2 dB.
 *
 * @p level_dbfs is the talker's nominal P.56 level: a vowel at 0 dB stress plays 3.9 dB above it, which puts the P.56
 * reading of 20 s within about 1 dB of it (activity about 0.8). A test that needs an exact input level measures it
 * with dsd_test_p56_level_dbfs().
 */
typedef struct {
    uint64_t seed;
    double level_dbfs; /**< nominal P.56 level of the talker */
    double f0_hz;      /**< the talker's mean pitch, 50-400 Hz (for example 110 male, 210 female) */
    double jitter;     /**< standard deviation of the cycle-to-cycle pitch period change, as a fraction (0-0.1) */
    double vibrato;    /**< 5 Hz vibrato depth, as a fraction of the pitch (0-0.1) */
    double shimmer;    /**< standard deviation of the per-period amplitude change, as a fraction (0-0.3) */
    double start_s;    /**< model time of the first sample: > 0 starts the signal inside running speech */
    const dsd_test_level_step* steps; /**< talker level steps, in increasing start time; may be NULL */
    size_t step_count;
} dsd_test_speech_params;

/** @brief Defaults: -30 dBFS, 120 Hz, 1% jitter, 1% vibrato, 3% shimmer, starting at time 0, no steps. */
void dsd_test_speech_defaults(dsd_test_speech_params* p, uint64_t seed);

/**
 * @brief Fill @p out with @p n samples of running speech at @p rate_hz.
 * @return 0, or -1 for a NULL pointer, a rate under DSD_TEST_SYNTH_MIN_RATE_HZ, a parameter outside its range, or
 *         steps without a table.
 */
int dsd_test_synth_speech(float* out, size_t n, int rate_hz, const dsd_test_speech_params* p);

/**
 * @brief One sustained vowel (/a/, formants 730, 1090 and 2440 Hz) of @p duration_s seconds, then zeros.
 *
 * The vowel starts at time 0 and has 20 ms raised-cosine edges. A natural vowel carries a 1.5% cycle-to-cycle pitch
 * jitter, a 5 Hz vibrato of 1% and a 3% shimmer; a steady one has none of them and is exactly periodic. @p level_dbfs
 * is the RMS level between the edges.
 */
typedef struct {
    uint64_t seed;
    double level_dbfs;
    double f0_hz;      /**< 50-450 Hz */
    double duration_s; /**< at least 40 ms */
    int steady;        /**< 1: no jitter, vibrato or shimmer */
} dsd_test_vowel_params;

/** @brief Defaults: natural, 120 Hz, 0.5 s, -36 dBFS. */
void dsd_test_vowel_defaults(dsd_test_vowel_params* v, uint64_t seed);

/** @brief Fill @p out with @p n samples of the vowel. @return 0, or -1 for bad arguments. */
int dsd_test_synth_vowel(float* out, size_t n, int rate_hz, const dsd_test_vowel_params* v);

/** @brief A sine at @p hz, phase 0 at the first sample. @return 0, or -1 for bad arguments or @p hz >= rate / 2. */
int dsd_test_synth_tone(float* out, size_t n, int rate_hz, double hz, double rms_dbfs);

/**
 * @brief A DTMF pair for @p digit (0-9, *, #, A-D): two sines of equal amplitude whose sum has RMS @p rms_dbfs.
 * @return 0, or -1 for an unknown digit or bad arguments.
 */
int dsd_test_synth_dtmf(float* out, size_t n, int rate_hz, char digit, double rms_dbfs);

/**
 * @brief A heterodyne whistle: a sine starting at @p start_hz whose frequency drifts by @p drift_hz_per_s.
 * @return 0, or -1 for bad arguments or a frequency outside 20 Hz-3.4 kHz within the @p n samples.
 */
int dsd_test_synth_heterodyne(float* out, size_t n, int rate_hz, double start_hz, double drift_hz_per_s,
                              double rms_dbfs);

typedef enum {
    /** Transformer hum: the mains frequency with its 2nd and 3rd harmonics at -6 and -12 dB. */
    DSD_TEST_MAINS_HUM = 0,
    /** Rectifier buzz: a pulse train at twice the mains frequency, every harmonic from 300 Hz to 3.4 kHz equal. */
    DSD_TEST_MAINS_BUZZ = 1,
} dsd_test_mains_kind;

/** @brief Mains interference at @p mains_hz (50 or 60). @return 0, or -1 for bad arguments. */
int dsd_test_synth_mains(float* out, size_t n, int rate_hz, dsd_test_mains_kind kind, double mains_hz, double rms_dbfs);

typedef enum {
    /** Flat to the Nyquist frequency. Also the stand-in for a vocoder's comfort noise (-50 to -40 dBFS). */
    DSD_TEST_NOISE_WHITE = 0,
    /** White noise through 4th-order Butterworth high-pass 300 Hz and low-pass 3.4 kHz: open-squelch FM hiss. */
    DSD_TEST_NOISE_VOICE_BAND = 1,
    /** White noise through 75 us de-emphasis, then the voice band: FM hiss after a de-emphasising receiver. */
    DSD_TEST_NOISE_DEEMPH = 2,
    /** White noise through a 4th-order Butterworth low-pass at 100 Hz. */
    DSD_TEST_NOISE_RUMBLE = 3,
} dsd_test_noise_kind;

/**
 * @brief @p n samples of Gaussian noise of @p kind, scaled so these @p n samples have RMS @p rms_dbfs exactly.
 *
 * The filters run for 50 ms before the first sample, so the output starts in steady state.
 * @return 0, or -1 for bad arguments.
 */
int dsd_test_synth_noise(float* out, size_t n, int rate_hz, uint64_t seed, dsd_test_noise_kind kind, double rms_dbfs);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_TESTS_TEST_SUPPORT_SPEECH_SYNTH_H_ */
