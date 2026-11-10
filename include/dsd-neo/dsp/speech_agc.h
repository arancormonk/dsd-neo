// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Receive-side speech level AGC and a standalone peak limiter.
 *
 * One AGC for digital and analog voice. It aims every talker at one active speech level (ITU-T P.56, -24 dBFS by
 * default) with three separate parts:
 * - a slow level controller that learns only from frames it takes for speech (a level cue over a tracked noise floor
 *   and a pitch cue from the autocorrelation, with a run rule and a test that turns steady tones away),
 * - an attenuator that eases the gain down on long non-speech, such as open-squelch hiss, without touching what the
 *   controller learned,
 * - a causal peak limiter at the ceiling (-1 dBFS by default).
 *
 * The analysis runs on 20 ms frames. A finished frame's decision changes the gain from the next sample on, as a linear
 * ramp over one frame, so the AGC adds no latency and looks at no future samples. Every recurrence runs per sample or
 * per frame, so the output is the same bit for bit however the caller splits the samples into calls (for the same
 * per-sample learn, decodable and tone flags). Every duration is counted in samples, so one waveform at any supported
 * rate gets about the same gains. Samples are floats in int16 units (full scale 32768); the output stays within the
 * int16 range.
 *
 * The caller owns the state, which holds no pointers and allocates nothing: init sizes it for any rate from
 * DSD_SPEECH_AGC_MIN_RATE_HZ to DSD_SPEECH_AGC_MAX_RATE_HZ.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_SPEECH_AGC_H
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_SPEECH_AGC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Lowest supported rate: the digital voice rate. */
#define DSD_SPEECH_AGC_MIN_RATE_HZ       8000
/** Highest supported rate: the highest PCM input rate (`-s`, 192 kHz); the RTL monitor runs at 48 kHz by default. */
#define DSD_SPEECH_AGC_MAX_RATE_HZ       192000
/** The active speech level the AGC aims at, dBFS (P.56). */
#define DSD_SPEECH_AGC_TARGET_DBFS       (-24.0f)
/** The limiter ceiling, dBFS per sample: 29204 in int16 units. */
#define DSD_SPEECH_AGC_CEILING_DBFS      (-1.0f)
/** The same target in the controller's own frame-level domain (the power mean of committed 20 ms frames), calibrated
    on established speech (vocoded and analog talkers) so that it plays at DSD_SPEECH_AGC_TARGET_DBFS. */
#define DSD_SPEECH_AGC_FRAME_TARGET_DBFS (-20.278f)
/** The controller never moves the gain more than this far below or above the start gain, dB. */
#define DSD_SPEECH_AGC_MAX_CUT_DB        12.0f
#define DSD_SPEECH_AGC_MAX_BOOST_DB      15.0f
/** The start gain init and dsd_speech_agc_set_start_gain_db() take, +- dB: beyond it the gain times a large input
    would no longer stay finite. */
#define DSD_SPEECH_AGC_START_GAIN_MAX_DB 60.0f

/** Analysis frames a speech run needs before it commits: a first commit comes 120 ms into speech at the earliest. */
#define DSD_SPEECH_AGC_RUN_FRAMES        6
/** Frames the steady-tone test looks back over (240 ms), and the number of levels a run keeps. */
#define DSD_SPEECH_AGC_STATIONARY_FRAMES 12
/** Rollback checkpoints: one per finished frame, so a gate close undoes up to the last 12 frames. */
#define DSD_SPEECH_AGC_CHECKPOINTS       13

/** The pitch cue works on the frame decimated to about 8 kHz (one sample in round(rate / 8000)). The decimated rate
    stays under 12 kHz at every supported rate, so its 20 ms frame and its 15 ms longest lag stay under 240 and 180
    samples; the history holds both, plus the slack the bound check keeps. */
#define DSD_SPEECH_AGC_HISTORY_LEN       421
/** The most lags the pitch cue evaluates (2.5-15 ms at under 12 kHz). */
#define DSD_SPEECH_AGC_MAX_LAGS          151

/** Everything the controller and the attenuator learn. A rollback checkpoint is a copy of it, so a rollback restores
    all of it by construction. Levels are dB re int16 full scale, measured over the learning samples of a frame. */
typedef struct {
    float noise_floor_db; /**< N: the tracked noise floor */
    int floor_frames;     /**< eligible frames since the floor (re)started: it rises faster for the first 2 s */
    int run_len;          /**< consecutive speech frames in the current run */
    float run_level_db[DSD_SPEECH_AGC_STATIONARY_FRAMES];
    float run_lag_ms[DSD_SPEECH_AGC_STATIONARY_FRAMES]; /**< the run's interpolated first-peak lags */
    float level_db;                                     /**< S: the speech level estimate, 10 log10(power) */
    float power;                                        /**< P: the mean square the estimate averages */
    int commits;                                        /**< committed frames since reset */
    int steps;                                          /**< gain steps since reset */
    float gain_db;                                      /**< G: the controller's gain */
    float atten_db;                                     /**< E: the attenuator, <= 0 */
    float noise_level_db;                               /**< the moving level of the frames that drove E down */
    int since_commit;                                   /**< frames since the last commit (a clock; saturates) */
    uint32_t rejections;                                /**< runs turned away as steady tones */
    uint32_t restarts;                                  /**< noise floor restarts */
    int stale_frames;                                   /**< frames in the current stale-floor run (saturates) */
    int stale_moves;                                    /**< frames in it whose pitch moved (saturates) */
    float stale_lag_min_ms;
    float stale_lag_max_ms;
    float stale_lag_last_ms;
} dsd_speech_agc_learned;

typedef struct {
    dsd_speech_agc_learned learned;
    uint64_t frames; /**< frames finished when it was taken */
} dsd_speech_agc_checkpoint;

/** One AGC instance. Treat every field as private; the learned block is readable for diagnostics and tests. */
typedef struct {
    /* Configuration */
    int rate_hz;
    int frame_len;     /**< F: samples per analysis frame */
    int decim;         /**< k: the pitch cue keeps one sample in k */
    int lag_lo;        /**< shortest lag, decimated samples */
    int lag_hi;        /**< longest lag, decimated samples */
    float target_dbfs; /**< the P.56 target */
    float target_db;   /**< T: the same target in the frame-level domain */
    float start_gain_db;
    float next_start_gain_db; /**< takes effect at the next reset */
    float abs_floor_db;       /**< A: frames below it never count */
    float ceiling;            /**< C: the limiter ceiling, int16 units */
    float lim_release;        /**< the limiter's per-sample release factor */
    float tone_cap;           /**< the start gain, linear: the most a tone frame gets */
    int fixed;
    float fixed_gain;

    dsd_speech_agc_learned learned;

    /* Signal state (not learned) */
    int frame_pos;
    int frame_learn; /**< learning samples in the current frame */
    float frame_sumsq;
    int decim_phase;
    int decim_pos;
    float history[DSD_SPEECH_AGC_HISTORY_LEN]; /**< decimated samples: lag_hi of history, then the current frame */
    float g_cur;
    float g_step;
    float g_target;
    float g_lim;
    int ramp_left;
    int gain_dirty; /**< G or E moved since g_target was computed */
    uint64_t frames;
    int decodable_prev;
    int tone_prev;
    int tone_ramp_left;
    float tone_end_gain;
    float g_last;   /**< the controller gain the last sample got (before the limiter) */
    int learn_prev; /**< process_runs: the last sample's learn flag, -1 for none */

    dsd_speech_agc_checkpoint checkpoints[DSD_SPEECH_AGC_CHECKPOINTS];
    int cp_head;
    int cp_count;
} dsd_speech_agc;

/**
 * @brief Configure for @p rate_hz and reset.
 *
 * @param start_gain_db The gain a new reception starts at, dB (G0: 0 for digital voice; analog per band), within
 *                      +-DSD_SPEECH_AGC_START_GAIN_MAX_DB. The controller stays within -12/+15 dB of it, and tone
 *                      frames never get more.
 * @param target_dbfs   The active speech level to aim at (DSD_SPEECH_AGC_TARGET_DBFS).
 * @param ceiling_dbfs  The per-sample limiter ceiling (DSD_SPEECH_AGC_CEILING_DBFS), at most 0.
 *
 * @return 0 on success, -1 when @p agc is NULL, the rate is outside DSD_SPEECH_AGC_MIN_RATE_HZ ..
 *         DSD_SPEECH_AGC_MAX_RATE_HZ, the start gain is out of range, the target is not finite, or the ceiling is not
 *         finite, above 0 dBFS or under one int16 unit (@p agc untouched).
 */
int dsd_speech_agc_init(dsd_speech_agc* agc, int rate_hz, float start_gain_db, float target_dbfs, float ceiling_dbfs);

/** A new reception: back to the start gain with nothing learned, an opening checkpoint, clear signal state. */
void dsd_speech_agc_reset(dsd_speech_agc* agc);

/** The start gain for the next reset (and every one after it); the current reception keeps its own. A gain out of
    init's range is ignored. */
void dsd_speech_agc_set_start_gain_db(dsd_speech_agc* agc, float start_gain_db);

/**
 * @brief Fixed or automatic gain.
 *
 * A positive finite @p gain is a fixed linear gain: nothing is learned and tone frames are not capped, but the limiter
 * still runs; gains above 1e6 count as 1e6. Anything else means automatic; leaving fixed mode keeps what was learned
 * and starts from the fixed gain, clamped into the controller's range, with the attenuator at 0. The switch is a new
 * opening checkpoint, so a gate close never restores a gain from before it.
 */
void dsd_speech_agc_set_fixed(dsd_speech_agc* agc, float gain);

/**
 * @brief Apply the AGC to @p n samples of @p buf in place, with one set of flags for all of them (digital: a frame).
 *
 * @p learn says the samples may teach the controller (decoded speech the policy admits). @p decodable 0 marks samples
 * that cannot be decrypted: they never learn, but the gain and the limiter still apply; when it returns to 1 the
 * limiter and the pitch history start over and the learned level stays. @p tone 1 marks a decoder's tone frame: it
 * never learns, and in automatic mode it plays at no more than the start gain from its first sample on (after it the
 * gain ramps back over one frame). A non-finite sample (or one of magnitude 1e15 or more) comes out as 0 and leaves
 * the state alone.
 */
void dsd_speech_agc_process(dsd_speech_agc* agc, float* buf, size_t n, int learn, int decodable, int tone);

/**
 * @brief Apply the AGC with a learn flag per sample (analog: the qualified reception), decodable and no tones.
 *
 * Where the flag drops from 1 to 0 the gate closes (dsd_speech_agc_gate_closed()) before that sample; where it rises
 * from 0 to 1 the gate opens (dsd_speech_agc_gate_opened()). The previous flag carries over from the last call, so
 * an edge on a call boundary counts the same; after init or reset the first sample is no edge. A NULL @p learn is all
 * zeros.
 */
void dsd_speech_agc_process_runs(dsd_speech_agc* agc, float* buf, size_t n, const uint8_t* learn);

/**
 * @brief A reception closed: undo what its tail may have taught.
 *
 * Restores the oldest checkpoint (up to 12 frames back, or where the gate opened if that was later), with the
 * since-commit clock advanced by the frames since; drops any pending speech run and the steady-tone window; clears the
 * pitch history, the partial frame and the gain ramp (the restored gain applies at once); resets the limiter; and
 * starts the checkpoint ring over from the restored state.
 */
void dsd_speech_agc_gate_closed(dsd_speech_agc* agc);

/** A reception opened: push a checkpoint, so a close never undoes more than this reception taught. */
void dsd_speech_agc_gate_opened(dsd_speech_agc* agc);

/** The gain the last sample got, controller, attenuator and limiter together, in dB (for readouts). */
float dsd_speech_agc_gain_db(const dsd_speech_agc* agc);

/** A standalone causal peak limiter, as the AGC's own: instant attack, 50 dB/s release, then the int16 clamp. */
typedef struct {
    float ceiling;
    float release;
    float gain;
} dsd_peak_limiter;

/** @return 0 on success, -1 for a NULL @p l, a rate under 1 Hz, or a ceiling that is not finite, above 0 dBFS or under
    one int16 unit (@p l untouched). */
int dsd_peak_limiter_init(dsd_peak_limiter* l, int rate_hz, float ceiling_dbfs);

/** Limit @p n samples of @p buf in place (int16 units). A non-finite sample comes out as 0 and leaves the state alone. */
void dsd_peak_limiter_process(dsd_peak_limiter* l, float* buf, size_t n);

/** Back to unity gain. */
void dsd_peak_limiter_reset(dsd_peak_limiter* l);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_SPEECH_AGC_H */
