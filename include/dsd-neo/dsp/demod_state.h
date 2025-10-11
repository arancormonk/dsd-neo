// SPDX-License-Identifier: GPL-2.0-or-later
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
#ifndef MAX_BANDWIDTH_MULTIPLIER
#define MAX_BANDWIDTH_MULTIPLIER 8
#endif

/* Half-band decimator taps (HB_TAPS) are defined where needed in DSP modules.
   Here we only dimension history arrays using HB_TAPS-1 = 14 for 15-tap HB. */
#ifndef HB_TAPS
#define HB_TAPS 15
#endif

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
    alignas(64) int16_t hb_i_buf[MAXIMUM_BUF_LENGTH / 2];
    alignas(64) int16_t hb_q_buf[MAXIMUM_BUF_LENGTH / 2];
    alignas(64) int16_t hb_i_out[MAXIMUM_BUF_LENGTH / 2];
    alignas(64) int16_t hb_q_out[MAXIMUM_BUF_LENGTH / 2];
    alignas(64) int16_t input_cb_buf[MAXIMUM_BUF_LENGTH];
    alignas(64) int16_t result[MAXIMUM_BUF_LENGTH];
    alignas(64) int16_t timing_buf[MAXIMUM_BUF_LENGTH];
    alignas(64) int16_t resamp_outbuf[MAXIMUM_BUF_LENGTH * 4];
    alignas(64) int16_t upsample_buf[MAXIMUM_BUF_LENGTH * MAX_BANDWIDTH_MULTIPLIER];

    /* Pointers and 64-bit items next */
    pthread_t thread;
    int16_t* lowpassed;
    int64_t squelch_running_power;
    int16_t* resamp_taps; /* Q15 taps, length = K*L */
    int16_t* resamp_hist; /* circular history, length = K */
    int (*discriminator)(int, int, int, int);
    void (*mode_demod)(struct demod_state*);
    struct output_state* output_target;
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
    int16_t lp_i_hist[10][6];
    int16_t lp_q_hist[10][6];
    int16_t droop_i_hist[9];
    int16_t droop_q_hist[9];
    int result_len;
    int rate_in;
    int rate_out;
    int rate_out2;
    int now_r, now_j;
    int pre_r, pre_j;
    int prev_index;
    int downsample; /* min 1, max 256 */
    int post_downsample;
    int output_scale;
    int squelch_level, conseq_squelch, squelch_hits, terminate_on_squelch;
    int squelch_decim_stride;
    int squelch_decim_phase;
    int squelch_window;
    int downsample_passes;
    int comp_fir_size;
    int custom_atan;
    int deemph, deemph_a;
    int deemph_avg;
    /* Optional post-demod audio low-pass filter (one-pole) */
    int audio_lpf_enable;
    int audio_lpf_alpha; /* Q15 alpha for one-pole LPF */
    int audio_lpf_state; /* state/output y[n-1] in Q0 */
    int now_lpr;
    int prev_lpr_index;
    int dc_block, dc_avg;
    /* Half-band decimator */
    int16_t hb_workbuf[MAXIMUM_BUF_LENGTH];
    int16_t hb_hist_i[10][HB_TAPS - 1];
    int16_t hb_hist_q[10][HB_TAPS - 1];

    /* Polyphase rational resampler (L/M) */
    int resamp_enabled;
    int resamp_target_hz;      /* desired output sample rate */
    int resamp_L;              /* upsample factor */
    int resamp_M;              /* downsample factor */
    int resamp_phase;          /* 0..L-1 accumulator */
    int resamp_taps_len;       /* prototype taps length (padded to K*L) */
    int resamp_taps_per_phase; /* K = ceil(taps_len/L) */
    int resamp_hist_head;      /* head index into circular history [0..K-1] */

    /* Residual CFO FLL state */
    int fll_enabled;
    int fll_alpha_q15;    /* proportional gain (Q15) */
    int fll_beta_q15;     /* integral gain (Q15) */
    int fll_freq_q15;     /* NCO frequency increment (Q15 radians/sample scaled) */
    int fll_phase_q15;    /* NCO phase accumulator */
    int fll_deadband_q14; /* ignore small phase errors |err| <= deadband (Q14) */
    int fll_slew_max_q15; /* max |delta freq| per update (Q15) */
    int fll_prev_r;
    int fll_prev_j;

    /* Timing error detector (Gardner) */
    int ted_enabled;
    int ted_force;    /* allow forcing TED even for FM/C4FM paths */
    int ted_gain_q20; /* small gain (Q20) for stability */
    int ted_sps;      /* nominal samples per symbol */
    int ted_mu_q20;   /* fractional phase [0,1) in Q20 */

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

    /* Experimental CQPSK/LSM pre-processing */
    int cqpsk_enable;
    int cqpsk_lms_enable;    /* enable decision-directed LMS for CQPSK EQ */
    int cqpsk_mu_q15;        /* step size (Q15), small (1..64) */
    int cqpsk_update_stride; /* update every N complex samples */
    int cqpsk_mf_enable;     /* enable small matched filter pre-processing */
    int cqpsk_rrc_enable;    /* use RRC matched filter instead of 5-tap MF */
    int cqpsk_rrc_alpha_q15; /* RRC roll-off (Q15) */
    int cqpsk_rrc_span_syms; /* half-span in symbols on each side */

    /* Generic mode-aware IQ balance (image suppression) */
    int iqbal_enable;          /* 0/1 gate */
    int iqbal_thr_q15;         /* |alpha| threshold in Q15 for enable */
    int iqbal_alpha_ema_r_q15; /* EMA of alpha real (Q15) */
    int iqbal_alpha_ema_i_q15; /* EMA of alpha imag (Q15) */
    int iqbal_alpha_ema_a_q15; /* EMA alpha (Q15) */

    /* FM envelope AGC (pre-discriminator) */
    int fm_agc_enable;         /* 0/1 gate; constant-envelope limiter/AGC for FM/C4FM */
    int fm_agc_gain_q15;       /* smoothed gain in Q15 (applied to I/Q) */
    int fm_agc_target_rms;     /* target RMS magnitude for |z| in Q0 (int16 domain) */
    int fm_agc_min_rms;        /* minimum RMS to engage AGC (avoid boosting noise) */
    int fm_agc_alpha_up_q15;   /* smoothing when increasing gain (signal got weaker) */
    int fm_agc_alpha_down_q15; /* smoothing when decreasing gain (signal got stronger) */
    int fm_agc_auto_enable;    /* auto-tune AGC target/alphas based on runtime stats */

    /* Optional constant-envelope limiter for FM/C4FM */
    int fm_limiter_enable; /* 0/1 gate; per-sample normalize |z| to ~target */

    /* Complex DC blocker before discriminator */
    int iq_dc_block_enable; /* 0/1 gate */
    int iq_dc_shift;        /* shift k for dc += (x-dc)>>k; typical 10..14 */
    int iq_dc_avg_r;        /* running DC estimate for I */
    int iq_dc_avg_i;        /* running DC estimate for Q */

    /* Blind CMA equalizer for FM/FSK (pre-discriminator, constant-envelope)
       Uses a short fractionally-spaced FIR with CMA-only adaptation to mitigate
       fast AM ripple and short-delay multipath that drives SNR "breathing".
       Off by default; enable via env DSD_NEO_FM_CMA=1. */
    int fm_cma_enable;   /* 0/1 gate for CMA equalizer on non-QPSK paths */
    int fm_cma_taps;     /* odd (3..11), default 5 */
    int fm_cma_mu_q15;   /* CMA step (Q15), small (1..64), default 2 */
    int fm_cma_warmup;   /* CMA warmup samples; <=0 means continuous (large). Default 20000 */
    int fm_cma_strength; /* 0=Light ([1,4,1]/6), 1=Strong ([1,6,1]/8) */

    /* Adaptive 5-tap guard (status only; updated by DSP pipeline) */
    int fm_cma_guard_freeze;  /* remaining blocks held by stability guard */
    int fm_cma_guard_accepts; /* total accepted tap updates */
    int fm_cma_guard_rejects; /* total rejected tap updates */
};
