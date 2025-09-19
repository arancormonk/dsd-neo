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
    int exit_flag;
    pthread_t thread;
    int16_t* lowpassed;
    /* Scratch buffer for demod thread to read blocks from the input ring */
    alignas(64) int16_t input_cb_buf[MAXIMUM_BUF_LENGTH];
    int lp_len;
    int16_t lp_i_hist[10][6];
    int16_t lp_q_hist[10][6];
    alignas(64) int16_t result[MAXIMUM_BUF_LENGTH];
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
    int64_t squelch_running_power;
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
    /* Half-band decimator state */
    int16_t hb_workbuf[MAXIMUM_BUF_LENGTH];
    int16_t hb_hist_i[10][HB_TAPS - 1];
    int16_t hb_hist_q[10][HB_TAPS - 1];
    alignas(64) int16_t hb_i_buf[MAXIMUM_BUF_LENGTH / 2];
    alignas(64) int16_t hb_q_buf[MAXIMUM_BUF_LENGTH / 2];
    alignas(64) int16_t hb_i_out[MAXIMUM_BUF_LENGTH / 2];
    alignas(64) int16_t hb_q_out[MAXIMUM_BUF_LENGTH / 2];
    /* Preallocated buffer for linear upsampler (bandwidth_multiplier) */
    alignas(64) int16_t upsample_buf[MAXIMUM_BUF_LENGTH * MAX_BANDWIDTH_MULTIPLIER];
    /* Polyphase rational resampler (L/M) state and output buffer */
    int resamp_enabled;
    int resamp_target_hz;      /* desired output sample rate */
    int resamp_L;              /* upsample factor */
    int resamp_M;              /* downsample factor */
    int resamp_phase;          /* 0..L-1 accumulator */
    int resamp_taps_len;       /* prototype taps length (padded to K*L) */
    int resamp_taps_per_phase; /* K = ceil(taps_len/L) */
    int16_t* resamp_taps;      /* Q15 taps, length = K*L */
    int16_t* resamp_hist;      /* circular history, length = K */
    int resamp_hist_head;      /* head index into circular history [0..K-1] */
    alignas(64) int16_t resamp_outbuf[MAXIMUM_BUF_LENGTH * 4];
    /* Residual CFO FLL state */
    int fll_enabled;
    int fll_alpha_q15;    /* proportional gain (Q15) */
    int fll_beta_q15;     /* integral gain (Q15) */
    int fll_freq_q15;     /* NCO frequency increment (Q15 radians/sample scaled) */
    int fll_phase_q15;    /* NCO phase accumulator (wrap at 2*pi -> 1<<15 scale) */
    int fll_deadband_q14; /* ignore small phase errors |err| <= deadband (Q14) */
    int fll_slew_max_q15; /* max |delta freq| per update (Q15) */
    int fll_prev_r;
    int fll_prev_j;
    /* Timing error detector (Gardner) fractional-delay state */
    int ted_enabled;
    int ted_force;    /* allow forcing TED even for FM/C4FM paths */
    int ted_gain_q20; /* small gain (Q20) for stability */
    int ted_sps;      /* nominal samples per symbol (e.g., 10 for 4800 sym/s at 48k) */
    int ted_mu_q20;   /* fractional phase [0,1) in Q20 */
    /* Work buffer for timing-adjusted I/Q */
    alignas(64) int16_t timing_buf[MAXIMUM_BUF_LENGTH];
    /* FLL and TED module states */
    fll_state_t fll_state;
    ted_state_t ted_state;
    /* Minimal 2-thread worker pool for intra-block parallelism */
    int mt_enabled;
    int mt_ready;
    pthread_t mt_threads[2];
    pthread_mutex_t mt_lock;
    pthread_cond_t mt_cv;
    pthread_cond_t mt_done_cv;
    int mt_should_exit;
    int mt_epoch;
    int mt_completed_in_epoch;
    int mt_posted_count;

    struct {
        void (*run)(void*);
        void* arg;
    } mt_tasks[2];

    int mt_worker_id[2];

    struct {
        struct demod_state* s;
        int id;
    } mt_args[2];

    int (*discriminator)(int, int, int, int);
    void (*mode_demod)(struct demod_state*);
    pthread_cond_t ready;
    pthread_mutex_t ready_m;
    struct output_state* output_target;

    /* Experimental: enable CQPSK/LSM path pre-processing (default off). */
    int cqpsk_enable;
    int cqpsk_lms_enable;    /* enable decision-directed LMS for CQPSK EQ */
    int cqpsk_mu_q15;        /* step size (Q15), small (1..64) */
    int cqpsk_update_stride; /* update every N complex samples */
    /* Optional pre-EQ matched filter (RRC-like) */
    int cqpsk_mf_enable;     /* enable small matched filter pre-processing */
    int cqpsk_rrc_enable;    /* use RRC matched filter instead of 5-tap MF */
    int cqpsk_rrc_alpha_q15; /* RRC roll-off (Q15), e.g., 0.20 -> 0.20*32768 */
    int cqpsk_rrc_span_syms; /* half-span in symbols on each side (total span ~ 2*span) */
    /* Generic mode-aware IQ balance (image suppression) */
    int iqbal_enable;          /* 0/1 gate */
    int iqbal_thr_q15;         /* |alpha| threshold in Q15 for enable (~0.02->655) */
    int iqbal_alpha_ema_r_q15; /* EMA of alpha real (Q15) */
    int iqbal_alpha_ema_i_q15; /* EMA of alpha imag (Q15) */
    int iqbal_alpha_ema_a_q15; /* EMA alpha (Q15), e.g., 6553 ~ 0.2 */
};
