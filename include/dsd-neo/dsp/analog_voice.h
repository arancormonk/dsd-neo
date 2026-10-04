// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Voice band-pass, legacy one-pole filters and AGC for the analog monitor (issue #518).
 *
 * Pure signal processing with no global state: the caller owns each instance and feeds it samples at the rate it was
 * designed for. Every core is a causal per-sample recurrence, so its output depends only on the sample sequence (and,
 * for the AGC, the per-sample playing flag), never on how the caller splits it into calls, and none adds latency.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_ANALOG_VOICE_H
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_ANALOG_VOICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Which voice band-pass to design. */
typedef enum {
    /** 6th-order elliptic high-pass at 300 Hz (every CTCSS tone, and everything else below 254.1 Hz, at least 40 dB
        down; a DCS signal shaped below 300 Hz, about 32 dB down overall), then a 4th-order Butterworth low-pass at
        3400 Hz. */
    DSD_VOICE_BAND_FM = 0,
    /** 4th-order Butterworth high-pass at 200 Hz (AM carries no sub-audible signalling), then the same low-pass. */
    DSD_VOICE_BAND_AM = 1,
    /** The 3400 Hz low-pass alone: an anti-alias filter ahead of keeping one sample in N to reach 8 kHz. */
    DSD_VOICE_BAND_LOWPASS = 2,
} dsd_voice_band_kind;

/** One second-order section in transposed direct form II, coefficients normalised to a0 = 1. */
typedef struct {
    double b0, b1, b2, a1, a2;
    double s1, s2;
} dsd_voice_biquad;

#define DSD_VOICE_BANDPASS_MAX_SECTIONS 5

/** A designed voice band-pass and its filter state. No sections means pass-through. */
typedef struct {
    int kind;
    int rate_hz;
    int sections;
    dsd_voice_biquad sec[DSD_VOICE_BANDPASS_MAX_SECTIONS];
} dsd_voice_bandpass;

/**
 * @brief Design @p kind at @p rate_hz with clear state.
 *
 * A section whose corner sits at or above 0.45 x @p rate_hz is left out (the 3400 Hz low-pass below 7556 Hz); a rate of
 * 0 or less designs a pass-through. tools/design_voice_filters.py derives the prototype and every figure the tests hold
 * the response to.
 *
 * @return 0 on success, -1 when @p bp is NULL or @p kind is unknown (@p bp untouched).
 */
int dsd_voice_bandpass_design(dsd_voice_bandpass* bp, dsd_voice_band_kind kind, int rate_hz);

/** Clear the filter state, keeping the design. */
void dsd_voice_bandpass_reset(dsd_voice_bandpass* bp);

/** Filter @p n samples of @p buf in place. A non-finite input sample comes out as 0 and leaves the state alone. */
void dsd_voice_bandpass_process(dsd_voice_bandpass* bp, float* buf, size_t n);

/** The designed magnitude response at @p hz, in dB (0 for a pass-through). */
double dsd_voice_bandpass_gain_db(const dsd_voice_bandpass* bp, double hz);

/** The legacy filters' corner (`-v 0x2` low-pass, `-v 0x4` high-pass). */
#define DSD_VOICE_ONEPOLE_CORNER_HZ 960.0

/** The legacy one-pole RC low-pass and high-pass at DSD_VOICE_ONEPOLE_CORNER_HZ, the sections init_audio_filters()
    designs for dsd_state, as an instance a chain owns at its own rate. */
typedef struct {
    double lp_a;
    double lp_y;
    double hp_c;
    double hp_x;
    double hp_y;
} dsd_voice_onepole;

/**
 * @brief Design both filters at @p rate_hz with clear state.
 *
 * @return 0 on success, -1 when @p f is NULL or @p rate_hz is 0 or less (@p f untouched).
 */
int dsd_voice_onepole_design(dsd_voice_onepole* f, int rate_hz);

/** Clear both filters' state, keeping the design. */
void dsd_voice_onepole_reset(dsd_voice_onepole* f);

/** Low-pass @p n samples of @p buf in place. A non-finite input sample comes out as 0 and leaves the state alone. */
void dsd_voice_onepole_lowpass(dsd_voice_onepole* f, float* buf, size_t n);

/** High-pass @p n samples of @p buf in place. A non-finite input sample comes out as 0 and leaves the state alone. */
void dsd_voice_onepole_highpass(dsd_voice_onepole* f, float* buf, size_t n);

/** The AGC's output peak target: -10 dBFS. Every output sample stays at or below it in magnitude. */
#define DSD_VOICE_AGC_TARGET_PEAK           10362.0f
/** Largest boost over the reference gain, in dB. */
#define DSD_VOICE_AGC_MAX_BOOST_DB          18.0
/** Inputs this far below the reference input (the one the reference gain takes to the fixed-gain reference peak)
    never release the envelope, so silence, and the exact zeros of a squelched span, freeze the gain. */
#define DSD_VOICE_AGC_FLOOR_DB              (-36.0)
/** How long the envelope holds a peak before it releases. */
#define DSD_VOICE_AGC_HOLD_MS               500
/** Release rate while the input level stays within DSD_VOICE_AGC_NEAR_DB of the envelope (speech getting
    quieter)... */
#define DSD_VOICE_AGC_RELEASE_FAST_DB_PER_S 20.0
/** ... and once it falls further below (a pause), so a pause does not pump the hiss up. */
#define DSD_VOICE_AGC_RELEASE_SLOW_DB_PER_S 3.0
#define DSD_VOICE_AGC_NEAR_DB               20.0
/** The input level those two tests read: a peak follower with this decay time constant, so a waveform's zero
    crossings do not read as a pause. */
#define DSD_VOICE_AGC_LEVEL_MS              20
/** When the playing flag drops (the gate closed), the gain goes back to where it stood this long before: a squelch
    tail or key-up noise does not set the next transmission's level. */
#define DSD_VOICE_AGC_ROLLBACK_MS           250
/** Rollback checkpoints are taken this often while playing. */
#define DSD_VOICE_AGC_CHECKPOINT_MS         10
#define DSD_VOICE_AGC_CHECKPOINTS           ((DSD_VOICE_AGC_ROLLBACK_MS / DSD_VOICE_AGC_CHECKPOINT_MS) + 1)

typedef struct {
    double env;
    uint32_t hold;
} dsd_voice_agc_point;

/**
 * @brief Peak-envelope AGC with instantaneous attack (a causal per-sample recurrence).
 *
 * Per sample: the envelope takes |x| at once when |x| reaches it (attack), holds DSD_VOICE_AGC_HOLD_MS, then releases,
 * fast or slow by where the input level sits against it; the gain is DSD_VOICE_AGC_TARGET_PEAK / envelope, so |x| x gain
 * never exceeds the target, and it never exceeds the reference gain by more than DSD_VOICE_AGC_MAX_BOOST_DB. Every
 * duration is counted in samples.
 */
typedef struct {
    int rate_hz;
    double ref_gain;
    double max_gain;
    double floor_in;
    double env_min;
    double release_fast;
    double release_slow;
    double near_ratio;
    double level_decay;
    uint32_t hold_samples;
    uint32_t checkpoint_samples;

    double env;
    double level;
    uint32_t hold;
    int playing;
    uint32_t since_checkpoint;
    int points_head;
    int points_count;
    dsd_voice_agc_point points[DSD_VOICE_AGC_CHECKPOINTS];
} dsd_voice_agc;

/**
 * @brief Configure for @p rate_hz with @p ref_gain, the fixed gain that takes the reference input to the fixed-gain
 * reference peak, and reset (the gain starts at @p ref_gain).
 *
 * @return 0 on success, -1 when @p agc is NULL, @p rate_hz is 0 or less or @p ref_gain is not positive and finite.
 */
int dsd_voice_agc_init(dsd_voice_agc* agc, int rate_hz, double ref_gain);

/** Back to the reference gain, nothing held, no rollback history, not playing. */
void dsd_voice_agc_reset(dsd_voice_agc* agc);

/**
 * @brief Apply the AGC to @p n samples of @p buf in place.
 *
 * @p playing says whether these samples are heard. While it is 0 the gain neither attacks nor releases (the output is
 * still limited to the target); the sample at which it drops from 1 to 0 rolls the gain back by
 * DSD_VOICE_AGC_ROLLBACK_MS. A non-finite input sample comes out as 0 and leaves the state alone.
 */
void dsd_voice_agc_process(dsd_voice_agc* agc, float* buf, size_t n, int playing);

/** The gain the next sample would get. */
double dsd_voice_agc_gain(const dsd_voice_agc* agc);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_ANALOG_VOICE_H */
