// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The NFM noise squelch (`--squelch noise[+N]`, issue #518 follow-up): it opens when the FM discriminator's
 * output quiets N dB above the voice band, as a radio's noise squelch does.
 *
 * It runs on the discriminator output d (dsd_fm_demod(): the phase step per sample, in radians), at the channel rate
 * (before any post-decimation), on the demod thread. It is pure: no clocks, no I/O, no allocation, and every decision
 * is taken at a sample-exact boundary, so the output depends only on the samples, never on how they are cut into
 * blocks.
 *
 * Band (tools/noise_squelch_model.py, docs/testing.md "Noise squelch"): B = [3.8 kHz, hi], hi = min(edge - 800 Hz,
 * 0.45 fs), edge the channel taps' -1 dB point (fs/2 without a channel filter). A plan needs at least 1200 Hz there;
 * a narrower channel (8 and 10 kHz NFM widths) has none, and the demodulator runs the auto squelch for it instead. B is
 * cut into K equal sub-bands, K = floor(|B| / 300 Hz) held to 1..DSD_NOISE_SQUELCH_MAX_SUB_BANDS, and a staggered
 * set of K - 1 more, each a sub-band's width shifted by half of it, so a line on a boundary of the first set sits
 * inside one of the second. Each of the 2K - 1 is a Butterworth band-pass of prototype order 4 (four biquads).
 *
 * Quieting: per 40 ms window (two 20 ms halves, boundary k at floor(k fs / 50) samples), taken every 20 ms, each
 * band-pass's mean output power P_k gives Q_k = 10 log10(Pref_k / P_k), Pref_k the same for complex Gaussian noise
 * alone through the plan (a calibration run at design time, fixed seed). Noise reads about 0 dB whatever its level: the
 * discriminator sees phase only. A carrier quiets the band by about its CNR. The window's quieting is
 * Q = max(Q_sum, Q_max - 4 dB): Q_sum from the K sub-bands' summed powers (the whole band, tight on noise) and Q_max
 * the best of all 2K - 1. A strong carrier carrying a tone puts the tone's harmonics (the channel filter truncates its
 * sidebands) into the band as lines; Q_max finds a band-pass clear of them, so wanted modulation does not close the
 * gate. A window whose I/Q holds no signal at all (mean |z|^2 at or below 1e-30, exact zeros) reads 0 dB.
 *
 * Gate: closed after a reset; it opens at a window whose Q reaches N and closes at one below max(N - 3, 1.5) dB. Each
 * sample's flag is the gate as it stood when the sample arrived: a decision applies from the sample after the window
 * that took it.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_NFM_NOISE_SQUELCH_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_NFM_NOISE_SQUELCH_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /** Most sub-bands a plan cuts its band into (K). */
    DSD_NOISE_SQUELCH_MAX_SUB_BANDS = 15,
    /** Most band-passes a plan runs: its K sub-bands and the K - 1 staggered between them. */
    DSD_NOISE_SQUELCH_MAX_BANDS = (2 * DSD_NOISE_SQUELCH_MAX_SUB_BANDS) - 1,
    /** Biquads per band-pass: a Butterworth band-pass of prototype order 4. */
    DSD_NOISE_SQUELCH_SECTIONS = 4,
    /** Most channel taps a plan designs for (the channel filter's DSD_CHANNEL_LPF_MAX_TAPS). */
    DSD_NOISE_SQUELCH_MAX_TAPS = 288,
};

/** @brief One biquad, b = gain * [1, 0, -1], a = [1, a1, a2]. */
typedef struct {
    double gain;
    double a1;
    double a2;
} dsd_noise_squelch_biquad;

/**
 * @brief The band, its band-passes' filters and their noise references for one channel plan: the channel taps, the
 * half-band stage before them and the channel rate. A zeroed plan is not valid; a squelch with no valid plan keeps its
 * gate open.
 */
typedef struct {
    int valid;
    int rate_hz;    /**< channel rate, samples per second */
    double edge_hz; /**< the channel taps' -1 dB point */
    double lo_hz;   /**< the band's edges */
    double hi_hz;
    int sub_bands; /**< K */
    int bands;     /**< 2K - 1: the K sub-bands (indices 0..K-1), then the staggered set */
    dsd_noise_squelch_biquad section[DSD_NOISE_SQUELCH_MAX_BANDS][DSD_NOISE_SQUELCH_SECTIONS];
    double p_ref[DSD_NOISE_SQUELCH_MAX_BANDS]; /**< each band-pass's mean output power for noise alone */
    double p_ref_sum;                          /**< the K sub-bands' p_ref, summed */
} dsd_noise_squelch_plan;

/** @brief What the squelch publishes. */
typedef struct {
    int gate_open;      /**< 1 open */
    double quieting_db; /**< the last window's Q; 0 before the first */
    uint64_t windows;   /**< windows evaluated since the last reset */
} dsd_noise_squelch_status;

/** @brief The squelch. Plain data: a zeroed one is valid (no plan, gate open). */
typedef struct {
    dsd_noise_squelch_plan plan;
    int threshold_db;
    double open_db;
    double close_db;
    /* Biquad state (transposed direct form II). */
    double s1[DSD_NOISE_SQUELCH_MAX_BANDS][DSD_NOISE_SQUELCH_SECTIONS];
    double s2[DSD_NOISE_SQUELCH_MAX_BANDS][DSD_NOISE_SQUELCH_SECTIONS];
    /* Sample-exact half-window boundaries since the last reset. */
    uint64_t samples;
    uint64_t half_index;
    uint64_t half_end;
    /* The open half and the one before it: per band-pass output energy, I/Q energy and sample count. */
    double half_e[DSD_NOISE_SQUELCH_MAX_BANDS];
    double prev_e[DSD_NOISE_SQUELCH_MAX_BANDS];
    double half_iq;
    double prev_iq;
    double half_n;
    double prev_n;
    int have_prev;
    int gate_open;
    double quieting_db;
    uint64_t windows;
} dsd_noise_squelch;

/**
 * @brief The channel taps' -1 dB point: the first 10 Hz step from 0 Hz at which their response falls 1 dB under its
 * DC gain; @p rate_hz / 2 when it never does (no channel filter: @p taps NULL or @p taps_len 0).
 */
double dsd_noise_squelch_passband_edge_hz(const float* taps, int taps_len, int rate_hz);

/**
 * @brief Design the plan for the channel taps (@p taps, @p taps_len; NULL or 0 when no channel filter runs), the
 * half-band stage that feeds them (@p hb_taps, @p hb_len; NULL or 0 when none does), and the channel rate @p rate_hz:
 * the band, its band-passes' biquads (the sub-bands and the staggered set), and their noise references from 2 s of
 * fixed-seed complex Gaussian noise through the half-band stage (at twice the rate), the channel taps, the
 * discriminator and each band-pass.
 *
 * @return 0, or -1 (plan not valid) when less than 1200 Hz of band fits (the auto squelch then runs instead).
 */
int dsd_noise_squelch_plan_design(dsd_noise_squelch_plan* out, const float* taps, int taps_len, const float* hb_taps,
                                  int hb_len, int rate_hz);

/** @brief Whether @p a and @p b are the same plan (rate, band and references equal within a relative 1e-12). */
int dsd_noise_squelch_plan_equal(const dsd_noise_squelch_plan* a, const dsd_noise_squelch_plan* b);

/** @brief Take @p plan; another plan starts over (dsd_noise_squelch_reset()). */
void dsd_noise_squelch_set_plan(dsd_noise_squelch* t, const dsd_noise_squelch_plan* plan);

/** @brief Open at @p threshold_db (clamped to 3..30) of quieting; close below max(N - 3, 1.5) dB. */
void dsd_noise_squelch_set_threshold(dsd_noise_squelch* t, int threshold_db);

/** @brief Start over: gate closed, filters and windows empty; the plan and threshold stay. */
void dsd_noise_squelch_reset(dsd_noise_squelch* t);

/**
 * @brief Run @p count discriminator outputs (@p disc, radians) and the channel samples they came from (@p iq,
 * interleaved I/Q floats, @p count pairs: the sample each output ends on) through the band, the windows and the gate,
 * writing each sample's flag (DSD_SQUELCH_FLAG_CLOSED when the gate was closed) to @p flags (may be NULL).
 */
void dsd_noise_squelch_process(dsd_noise_squelch* t, const float* disc, const float* iq, int count, uint8_t* flags);

/** @brief A window's Q from its band-passes' powers @p p (plan->bands of them) and I/Q power @p iq_power (exposed for
 * tests). */
double dsd_noise_squelch_quieting_db(const dsd_noise_squelch_plan* plan, const double* p, double iq_power);

/** @brief What @p t publishes. */
void dsd_noise_squelch_get_status(const dsd_noise_squelch* t, dsd_noise_squelch_status* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_NFM_NOISE_SQUELCH_H_ */
