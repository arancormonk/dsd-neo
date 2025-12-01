// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Demodulator state shared across DSP modules and RTL-SDR front-end.
 *
 * Centralized definition of `struct demod_state`. Mirrors the legacy layout in
 * `src/rtl_sdr_fm.cpp` and is intended to remain ABI-stable during ongoing
 * refactoring.
 */

#pragma once

#include <pthread.h>
#include <stdint.h>

#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/fll.h>
#include <dsd-neo/dsp/ted.h>

/* Buffer sizing constants used throughout the demodulator. Keep consistent
   with rtl_sdr_fm.cpp; guard to avoid redefinition across TUs. */
#ifndef DEFAULT_BUF_LENGTH
#define DEFAULT_BUF_LENGTH (1 * 16384)
#endif
#ifndef MAXIMUM_OVERSAMPLE
#define MAXIMUM_OVERSAMPLE 16
#endif
#ifndef MAXIMUM_BUF_LENGTH
#define MAXIMUM_BUF_LENGTH (MAXIMUM_OVERSAMPLE * DEFAULT_BUF_LENGTH)
#endif

/* Half-band decimator taps (HB_TAPS) are defined where needed in DSP modules.
   Here we dimension histories against the maximum half-band used by the
   complex decimator cascade. */
#ifndef HB_TAPS
#define HB_TAPS 15
#endif
#ifndef HB_TAPS_MAX
#define HB_TAPS_MAX 31
#endif

/* Channel LPF profile ids */
enum {
    DSD_CH_LPF_PROFILE_WIDE = 0,
    DSD_CH_LPF_PROFILE_DIGITAL = 1,
    DSD_CH_LPF_PROFILE_P25_HANN = 2,
};

/* Forward declaration to avoid heavy dependencies here */
struct output_state;

/**
 * @brief Aggregate state container for the demodulator processing chain.
 *
 * Holds working buffers, configuration, and module states used by the DSP
 * pipeline (filters, resamplers, FLL/TED, etc.) and by the RTL-SDR front-end
 * thread.
 *
 * @note Keep this definition synchronized with usages in:
 *  - src/rtl_sdr_fm.cpp
 *  - src/dsp/demod_pipeline.cpp
 *  - src/dsp/resampler.cpp
 */
struct demod_state {
    /* Large aligned buffers first to minimize padding */
    alignas(64) float hb_i_buf[MAXIMUM_BUF_LENGTH / 2];
    alignas(64) float hb_q_buf[MAXIMUM_BUF_LENGTH / 2];
    alignas(64) float hb_i_out[MAXIMUM_BUF_LENGTH / 2];
    alignas(64) float hb_q_out[MAXIMUM_BUF_LENGTH / 2];
    alignas(64) float input_cb_buf[MAXIMUM_BUF_LENGTH];
    alignas(64) float result[MAXIMUM_BUF_LENGTH];
    alignas(64) float timing_buf[MAXIMUM_BUF_LENGTH];
    alignas(64) float resamp_outbuf[MAXIMUM_BUF_LENGTH * 4];

    /* Pointers and 64-bit items next */
    pthread_t thread;
    float* lowpassed;
    double squelch_running_power;
    float* resamp_taps; /* normalized taps, length = K*L */
    float* resamp_hist; /* circular history, length = K */
    int (*discriminator)(int, int, int, int);
    void (*mode_demod)(struct demod_state*);
    struct output_state* output_target;
    double fm_agc_ema_rms;      /* normalized RMS estimator (0..~1.0) */
    float* post_polydecim_taps; /* normalized taps length K */
    float* post_polydecim_hist; /* circular history length K */
    pthread_t mt_threads[2];

    struct {
        void (*run)(void*);
        void* arg;
    } mt_tasks[2];

    struct {
        struct demod_state* s;
        int id;
    } mt_args[2];

    pthread_mutex_t mt_lock;
    pthread_mutex_t ready_m;
    pthread_cond_t mt_cv;
    pthread_cond_t mt_done_cv;
    pthread_cond_t ready;

    /* Scalars and small arrays */
    int exit_flag;
    int lp_len;
    int result_len;
    int rate_in;
    int rate_out;
    int rate_out2;
    float pre_r, pre_j;
    int post_downsample;
    float output_scale;
    float squelch_level;
    int conseq_squelch, squelch_hits, terminate_on_squelch;
    int squelch_decim_stride;
    int squelch_decim_phase;
    int squelch_window;
    /* Squelch soft gate (audio envelope) */
    int squelch_gate_open;     /* 1=open, 0=closed (latched per block) */
    float squelch_env;         /* envelope gain [0,1] */
    float squelch_env_attack;  /* attack alpha [0,1] for opening */
    float squelch_env_release; /* release alpha [0,1] for closing */
    int downsample_passes;
    int custom_atan;
    int deemph;
    float deemph_a; /* deemphasis alpha [0.0, 1.0] for one-pole IIR */
    float deemph_avg;
    /* Optional post-demod audio low-pass filter (one-pole) */
    int audio_lpf_enable;
    float audio_lpf_alpha; /* alpha [0.0, 1.0] for one-pole LPF */
    float audio_lpf_state; /* state/output y[n-1] */
    float now_lpr;
    int prev_lpr_index;
    int dc_block;
    float dc_avg;
    /* Half-band decimator */
    float hb_workbuf[MAXIMUM_BUF_LENGTH];
    float hb_hist_i[10][HB_TAPS_MAX - 1];
    float hb_hist_q[10][HB_TAPS_MAX - 1];

    /* Fixed channel low-pass (post-HB) to bound noise bandwidth at higher Fs */
    int channel_lpf_enable; /* gate */
    int channel_lpf_hist_len;
    int channel_lpf_profile;      /* see DSD_CH_LPF_PROFILE_* */
    float channel_lpf_hist_i[64]; /* sized for up to 63-tap symmetric FIR (tap-1) */
    float channel_lpf_hist_q[64];

    /* Polyphase rational resampler (L/M) */
    int resamp_enabled;
    int resamp_target_hz;      /* desired output sample rate */
    int resamp_L;              /* upsample factor */
    int resamp_M;              /* downsample factor */
    int resamp_phase;          /* 0..L-1 accumulator */
    int resamp_taps_len;       /* prototype taps length (padded to K*L) */
    int resamp_taps_per_phase; /* K = ceil(taps_len/L) */
    int resamp_hist_head;      /* head index into circular history [0..K-1] */

    /* Residual CFO FLL state (GNU Radio-style native float) */
    int fll_enabled;
    float fll_alpha;    /* proportional gain (native float, ~0.002..0.02) */
    float fll_beta;     /* integral gain (native float, ~0.0002..0.002) */
    float fll_freq;     /* NCO frequency increment (rad/sample) */
    float fll_phase;    /* NCO phase accumulator (radians) */
    float fll_deadband; /* ignore small phase errors |err| <= deadband (radians) */
    float fll_slew_max; /* max |delta freq| per update (rad/sample) */
    float fll_prev_r;
    float fll_prev_j;

    /* CQPSK Costas loop tuning (separate from FLL) */
    dsd_costas_loop_state_t costas_state;

    /* Timing error detector (Gardner) - native float */
    int ted_enabled;
    int ted_force;        /* allow forcing TED even for FM/C4FM paths */
    float ted_gain;       /* loop gain, typically 0.01..0.1 */
    int ted_sps;          /* nominal samples per symbol */
    int ted_sps_override; /* >0 = manual override (used during P25P2 VC tunes) */
    float ted_mu;         /* fractional phase [0.0, 1.0) */

    /* Non-integer SPS detection: set when Fs/sym_rate doesn't divide evenly.
       Blocks like TED/FLL band-edge require integer SPS and auto-disable. */
    int sps_is_integer; /* 1 = integer SPS, 0 = non-integer (blocks disabled) */

    /* FLL and TED module states */
    fll_state_t fll_state;
    ted_state_t ted_state;

    /* Minimal 2-thread worker pool bookkeeping */
    int mt_enabled;
    int mt_ready;
    int mt_should_exit;
    int mt_epoch;
    int mt_completed_in_epoch;
    int mt_posted_count;
    int mt_worker_id[2];

    /* Experimental CQPSK pre-processing */
    int cqpsk_enable;
    int cqpsk_rms_agc_enable; /* 0/1: block RMS AGC before CQPSK acquisition FLL */
    float cqpsk_rms_agc_rms;  /* normalized RMS estimate for CQPSK AGC */

    /* CQPSK pre-Costas differential phasor history (previous raw sample) */
    float cqpsk_diff_prev_r;
    float cqpsk_diff_prev_j;

    /* Generic mode-aware IQ balance (image suppression) */
    int iqbal_enable;        /* 0/1 gate */
    float iqbal_thr;         /* |alpha| threshold for enable (normalized) */
    float iqbal_alpha_ema_r; /* EMA of alpha real (normalized) */
    float iqbal_alpha_ema_i; /* EMA of alpha imag (normalized) */
    float iqbal_alpha_ema_a; /* EMA smoothing alpha [0.0, 1.0] */

    /* FM envelope AGC (pre-discriminator) */
    int fm_agc_enable;       /* 0/1 gate; constant-envelope limiter/AGC for FM/C4FM */
    float fm_agc_gain;       /* smoothed gain applied to I/Q */
    float fm_agc_target_rms; /* target RMS magnitude for |z| (normalized float) */
    float fm_agc_min_rms;    /* minimum RMS to engage AGC (avoid boosting noise) */
    float fm_agc_alpha_up;   /* smoothing when increasing gain (signal got weaker) */
    float fm_agc_alpha_down; /* smoothing when decreasing gain (signal got stronger) */

    /* Optional constant-envelope limiter for FM/C4FM */
    int fm_limiter_enable; /* 0/1 gate; per-sample normalize |z| to ~target */

    /* Complex DC blocker before discriminator */
    int iq_dc_block_enable; /* 0/1 gate */
    int iq_dc_shift;        /* shift k for dc += (x-dc)>>k; typical 10..14 */
    float iq_dc_avg_r;      /* running DC estimate for I */
    float iq_dc_avg_i;      /* running DC estimate for Q */

    /* Post-demod audio polyphase decimator (M>2) */
    int post_polydecim_enabled;   /* 0/1 gate for audio polyphase decimator */
    int post_polydecim_M;         /* integer decimation factor */
    int post_polydecim_K;         /* taps per phase (phase==1), e.g., 16 */
    int post_polydecim_hist_head; /* head index into circular history [0..K-1] */
    int post_polydecim_phase;     /* sample phase accumulator [0..M-1] */

    /* CQPSK acquisition-only FLL helper (pre-Costas) */
    int cqpsk_acq_fll_enable;  /* 0/1: allow pre-Costas FLL pull-in */
    int cqpsk_acq_fll_locked;  /* 0/1: stop when locked */
    int cqpsk_acq_quiet_runs;  /* consecutive quiet blocks for lock */
    int cqpsk_acq_noisy_runs;  /* consecutive noisy blocks to force unlock */
    int cqpsk_fll_rot_applied; /* 0/1: cqpsk branch applied FLL rotation this block */

    /* Costas diagnostics (updated per block) */
    int costas_err_avg_q14; /* average |err| scaled to Q14 for UI/metrics */
};
