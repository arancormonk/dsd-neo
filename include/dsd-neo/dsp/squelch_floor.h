// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The floor-relative ("auto") squelch: a per-window noise/carrier classifier, the channel's noise floor it
 * learns, and the gate it opens a margin above that floor (issue #518 follow-up).
 *
 * Everything here runs on the channel-filtered complex baseband z, before any demodulation, at the channel rate (the
 * rate the channel filter runs at, before any post-decimation), on the demod thread. It is pure: no clocks, no I/O, no
 * allocation. Every decision is taken at a sample-exact boundary, so the output depends only on the samples, never on
 * how they are cut into blocks.
 *
 * Classifier (tools/squelch_model.py, docs/testing.md "Auto squelch classifier"): per 40 ms window (boundary k at
 * floor(k fs / 25) samples) it measures P = mean |z|^2, CV^2 = (mean |z|^4 - P^2) / P^2 and the lag-L coherence
 * mean(u_k conj(u_{k-L})), u = z/|z|. Normalised by the window's effective sample count N_eff, X = (CV^2 - 1) sqrt(N_eff)
 * and Y = |coherence - beta| sqrt(N_eff), with beta the coherence complex Gaussian noise shows through the same filter.
 * A window is CARRIER when X <= -7.0 or Y >= 3.3 (an FM carrier's constant envelope, an AM carrier's steady phase), NOISE
 * when X >= -3.8 and Y <= 2.1, and UNDECIDED otherwise. A NOISE window whose two 20 ms halves differ in power by 3 dB or
 * more holds a carrier starting or ending inside it, and counts as UNDECIDED.
 *
 * Floor: LEARNING until 5 of the last 8 windows are NOISE, then the mean power of those becomes the floor (KNOWN). It
 * then falls fast (about an 80 ms time constant) and rises at most 1 dB/s, on NOISE windows only: carrier power never
 * becomes floor. Five seconds of NOISE windows all more than 6 dB above it, or one more than 20 dB below it, start
 * LEARNING again. A floor taken from the per-channel cache (SEEDED, the same context) or from a neighbour within 5 MHz
 * (PROVISIONAL) gates like a known one and becomes KNOWN after 3 NOISE windows within 6 dB of it, or starts LEARNING.
 *
 * Gate: while LEARNING it is open for exactly the windows classed CARRIER (a carrier on a channel with no floor yet
 * plays, and holds as long as it lasts). With a floor it opens when a 20 ms sub-window's power reaches the floor plus
 * the margin, and closes after a sub-window below that less 3 dB. A window classed NOISE closes it whatever its power;
 * when that noise reads within 3 dB of the opening level and at least 1.5 dB above the floor (a floor still rising to
 * meet it), the gate stays shut until a window classed CARRIER. Each sample's flag is the gate as it stood when the sample arrived: a decision applies from
 * the sample after the (sub-)window that took it.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_SQUELCH_FLOOR_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_SQUELCH_FLOOR_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /** Longest coherence lag a plan may use. */
    DSD_SQUELCH_FLOOR_MAX_LAG = 32,
    /** Classification windows remembered while learning. */
    DSD_SQUELCH_FLOOR_LEARN_WINDOWS = 8,
    /** NOISE windows among those that make a floor. */
    DSD_SQUELCH_FLOOR_LEARN_NEED = 5,
    /** Entries in the per-channel floor cache. */
    DSD_SQUELCH_FLOOR_CACHE_SIZE = 256,
};

/** @brief How far the floor is known (dsd_squelch_floor::state). */
typedef enum {
    DSD_SQUELCH_FLOOR_LEARNING = 0,
    DSD_SQUELCH_FLOOR_KNOWN = 1,
    DSD_SQUELCH_FLOOR_SEEDED = 2,
    DSD_SQUELCH_FLOOR_PROVISIONAL = 3,
} dsd_squelch_floor_state;

/** @brief A classification window's verdict. */
typedef enum {
    DSD_SQUELCH_WINDOW_UNDECIDED = 0,
    DSD_SQUELCH_WINDOW_NOISE = 1,
    DSD_SQUELCH_WINDOW_CARRIER = 2,
} dsd_squelch_window_class;

/**
 * @brief The classifier's constants for one channel plan: the channel taps, the half-band stage before them, and the
 * channel rate. A zeroed plan is not valid, and a tracker with no valid plan keeps its gate open.
 */
typedef struct {
    int valid;
    int rate_hz;       /**< channel rate, samples per second */
    int lag;           /**< L, from the channel taps' autocorrelation */
    double rho_lag;    /**< the noise autocorrelation at L */
    double beta;       /**< the coherence complex Gaussian noise shows at L */
    double neff;       /**< effective samples in a 40 ms window */
    double sqrt_neff;  /**< its square root */
    double noise_gain; /**< noise power out for unit white complex noise in (the cache's density scale) */
} dsd_squelch_floor_plan;

/** @brief What the tracker publishes. */
typedef struct {
    int state;           /**< dsd_squelch_floor_state */
    int gate_open;       /**< 1 open */
    int last_class;      /**< dsd_squelch_window_class of the last window */
    double floor_power;  /**< the floor (mean |z|^2); 0 while LEARNING */
    double window_power; /**< the last window's mean |z|^2 */
    uint64_t windows;    /**< classification windows closed since the last reset */
} dsd_squelch_floor_status;

/** @brief The tracker. Plain data: a zeroed one is valid (no plan, gate open). */
typedef struct {
    dsd_squelch_floor_plan plan;
    int margin_db;
    double margin_lin;
    /* The last plan.lag samples, for the coherence's lag product. */
    float hist_re[DSD_SQUELCH_FLOOR_MAX_LAG];
    float hist_im[DSD_SQUELCH_FLOOR_MAX_LAG];
    int hist_pos;
    int hist_fill;
    /* Sample-exact boundaries since the last reset. */
    uint64_t samples;
    uint64_t sub_index;
    uint64_t sub_end;
    /* The open classification window and 20 ms sub-window. */
    double w_n, w_sa, w_sa2, w_su_re, w_su_im;
    double s_n, s_sa;
    double sub_power;       /* the last sub-window's mean |z|^2 */
    double first_sub_power; /* the open window's first sub-window's */
    /* Learning: the last windows' classes and powers, newest at learn_pos - 1. */
    uint8_t learn_class[DSD_SQUELCH_FLOOR_LEARN_WINDOWS];
    double learn_power[DSD_SQUELCH_FLOOR_LEARN_WINDOWS];
    int learn_pos;
    int learn_count;
    /* Floor and gate. */
    int state;
    double floor_power;
    int agree;
    int shift_windows;
    int gate_open;
    int noise_hold;
    int last_class;
    double window_power;
    uint64_t windows;
} dsd_squelch_floor;

/** @brief The context a floor belongs to: what sets the noise the channel filter passes. */
typedef struct {
    int64_t freq_hz;   /**< applied frequency */
    int32_t gain;      /**< applied tuner gain in tenths of a dB (0: tuner AGC) */
    int32_t tuner_agc; /**< tuner AGC on */
    int32_t bias;      /**< bias tee on */
    uint32_t device;   /**< device identity (a hash of the device string) */
    int32_t rate_hz;   /**< channel rate */
    int32_t chain;     /**< capture chain (live, replay, the decimation cascade) */
} dsd_squelch_floor_key;

/** @brief One remembered floor. */
typedef struct {
    dsd_squelch_floor_key key;
    double density; /**< floor / plan noise_gain */
    double stamp_s; /**< sample-time seconds when stored */
    uint64_t used;  /**< LRU stamp; 0: empty */
} dsd_squelch_floor_cache_entry;

/** @brief Per-channel floors, oldest-used evicted. Plain data: a zeroed one is empty. */
typedef struct {
    dsd_squelch_floor_cache_entry entries[DSD_SQUELCH_FLOOR_CACHE_SIZE];
    uint64_t use_clock;
    double clock_s; /**< sample time, advanced by the tracker's caller */
} dsd_squelch_floor_cache;

/**
 * @brief Design the classifier's constants for a plan: the channel taps (@p taps, @p taps_len; NULL or 0 when no
 * channel filter runs), the half-band stage that feeds them (@p hb_taps, @p hb_len; NULL or 0 when none does), and the
 * channel rate @p rate_hz.
 *
 * L is the smallest lag >= 1 at which the channel taps' autocorrelation |R_h(L)| / R_h(0) drops below 0.1. The noise
 * the classifier sees is white complex noise through the half-band stage (at twice the rate) and the channel taps;
 * rho_n is its autocorrelation, N_eff = N / sum_m rho_n(m)^2 for a 40 ms window of N samples, and
 * beta = (pi/4) rho_n(L) 2F1(1/2, 1/2; 2; rho_n(L)^2).
 *
 * @return 0, or -1 (plan not valid) for a rate below 100 Hz (two samples per 20 ms) or a lag past
 * DSD_SQUELCH_FLOOR_MAX_LAG.
 */
int dsd_squelch_floor_plan_design(dsd_squelch_floor_plan* out, const float* taps, int taps_len, const float* hb_taps,
                                  int hb_len, int rate_hz);

/** @brief Whether @p a and @p b classify the same way (rate, lag and constants equal within a relative 1e-12). */
int dsd_squelch_floor_plan_equal(const dsd_squelch_floor_plan* a, const dsd_squelch_floor_plan* b);

/** @brief Start over: LEARNING, gate closed, windows empty; the plan and margin stay. */
void dsd_squelch_floor_reset(dsd_squelch_floor* t);

/** @brief A gap in the samples: empty the windows and the lag history; the floor and the gate stay. */
void dsd_squelch_floor_restart_windows(dsd_squelch_floor* t);

/**
 * @brief Take @p plan. A plan that classifies differently starts the windows over; a floor already learned is kept,
 * rescaled by the ratio of the plans' noise gains (a width or kind change at the same context).
 */
void dsd_squelch_floor_set_plan(dsd_squelch_floor* t, const dsd_squelch_floor_plan* plan);

/** @brief Open @p margin_db (clamped to 3..30) above the floor. */
void dsd_squelch_floor_set_margin(dsd_squelch_floor* t, int margin_db);

/** @brief Gate from @p floor_power (mean |z|^2) at once: SEEDED, or PROVISIONAL when @p provisional. */
void dsd_squelch_floor_seed(dsd_squelch_floor* t, double floor_power, int provisional);

/**
 * @brief Run @p count complex samples (@p iq, interleaved I/Q floats) through the classifier, the floor and the gate,
 * writing each sample's flag (DSD_SQUELCH_FLAG_CLOSED when the gate was closed) to @p flags (may be NULL).
 */
void dsd_squelch_floor_process(dsd_squelch_floor* t, const float* iq, int count, uint8_t* flags);

/** @brief Classify one window's sums (exposed for tests). */
int dsd_squelch_floor_classify(const dsd_squelch_floor_plan* plan, double n, double sum_a, double sum_a2, double su_re,
                               double su_im);

/** @brief What @p t publishes. */
void dsd_squelch_floor_get_status(const dsd_squelch_floor* t, dsd_squelch_floor_status* out);

/** @brief Advance @p c's sample-time clock by @p seconds (entries go stale 30 minutes after they were stored). */
void dsd_squelch_floor_cache_advance(dsd_squelch_floor_cache* c, double seconds);

/** @brief Remember @p density (floor over the plan's noise gain) for @p key, replacing an entry for the same key. */
void dsd_squelch_floor_cache_store(dsd_squelch_floor_cache* c, const dsd_squelch_floor_key* key, double density);

/**
 * @brief Look up @p key: 1 with the entry's density for the same key, 2 (provisional) with the nearest entry within
 * 5 MHz that matches in everything but the frequency, 0 for none. Stale entries are not used.
 */
int dsd_squelch_floor_cache_find(dsd_squelch_floor_cache* c, const dsd_squelch_floor_key* key, double* density);

/** @brief Whether @p a and @p b are the same context. */
int dsd_squelch_floor_key_equal(const dsd_squelch_floor_key* a, const dsd_squelch_floor_key* b);

/**
 * @brief Move @p t to context @p next from @p prev (NULL: none yet): a KNOWN floor is remembered for @p prev, then
 * @p next's own floor seeds the tracker, or a neighbour's provisionally, or it starts LEARNING. No-op when the contexts
 * are the same.
 */
void dsd_squelch_floor_change_context(dsd_squelch_floor* t, dsd_squelch_floor_cache* c,
                                      const dsd_squelch_floor_key* prev, const dsd_squelch_floor_key* next);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_SQUELCH_FLOOR_H_ */
