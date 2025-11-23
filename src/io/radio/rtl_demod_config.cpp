// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RTL-SDR demodulation configuration helpers.
 *
 * Centralizes initialization and configuration of the demodulation state
 * used by the RTL-SDR stream pipeline, including mode selection, env/opts
 * driven DSP toggles, and rate-dependent helpers.
 */

#include <dsd-neo/io/rtl_demod_config.h>

#include <math.h>

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/fll.h>
#include <dsd-neo/dsp/math_utils.h>
#include <dsd-neo/dsp/resampler.h>
#include <dsd-neo/dsp/ted.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/mem.h>
#include <dsd-neo/runtime/ring.h>
#include <dsd-neo/runtime/unicode.h>
#include <dsd-neo/runtime/worker_pool.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Debug/compat toggles via env (mirrored from rtl_sdr_fm.cpp responsibilities). */
int combine_rotate_enabled = 1;      /* DSD_NEO_COMBINE_ROT (1 default) */
int upsample_fixedpoint_enabled = 1; /* DSD_NEO_UPSAMPLE_FP (1 default) */

/* Runtime flag (default enabled). Set DSD_NEO_HB_DECIM=0 to use legacy decimator. */
int use_halfband_decimator = 1;

/* Allow disabling the fs/4 capture frequency shift via env for trunking/exact-center use cases. */
int disable_fs4_shift = 0; /* Set by env DSD_NEO_DISABLE_FS4_SHIFT=1 */

namespace {

enum DemodMode { DEMOD_DIGITAL = 0, DEMOD_ANALOG = 1, DEMOD_RO2 = 2 };

struct DemodInitParams {
    int deemph_default;
};

static void
demod_init_mode(struct demod_state* s, DemodMode mode, const DemodInitParams* p, int rtl_dsp_bw_hz,
                struct output_state* output) {
    /* Common defaults */
    s->rate_in = rtl_dsp_bw_hz;
    s->rate_out = rtl_dsp_bw_hz;
    s->squelch_level = 0;
    s->conseq_squelch = 10;
    s->terminate_on_squelch = 0;
    s->squelch_hits = 11;
    s->downsample_passes = 0;
    s->comp_fir_size = 0;
    s->prev_index = 0;
    s->post_downsample = 1;
    s->custom_atan = 0;
    s->deemph = 0;
    s->rate_out2 = -1;
    s->mode_demod = &dsd_fm_demod;
    s->pre_j = s->pre_r = s->now_r = s->now_j = 0;
    s->prev_lpr_index = 0;
    s->deemph_a = 0;
    s->deemph_avg = 0;
    /* Channel LPF (post-HB) */
    s->channel_lpf_enable = 0;    /* configured later by env/mode helper */
    s->channel_lpf_hist_len = 62; /* kChannelLpfHistLen */
    s->channel_lpf_profile = 0;   /* 0=wide/analog, 1=digital-narrow */
    for (int k = 0; k < 64; k++) {
        s->channel_lpf_hist_i[k] = 0;
        s->channel_lpf_hist_q[k] = 0;
    }
    /* Audio LPF defaults */
    s->audio_lpf_enable = 0;
    s->audio_lpf_alpha = 0;
    s->audio_lpf_state = 0;
    s->now_lpr = 0;
    s->dc_block = 1;
    s->dc_avg = 0;
    /* Resampler defaults */
    s->resamp_enabled = 0;
    s->resamp_target_hz = 0;
    s->resamp_L = 1;
    s->resamp_M = 1;
    s->resamp_phase = 0;
    s->resamp_taps_len = 0;
    s->resamp_taps_per_phase = 0;
    s->resamp_taps = NULL;
    s->resamp_hist = NULL;
    /* Post-demod audio polyphase decimator defaults */
    s->post_polydecim_enabled = 0;
    s->post_polydecim_M = 1;
    s->post_polydecim_K = 0;
    s->post_polydecim_hist_head = 0;
    s->post_polydecim_taps = NULL;
    s->post_polydecim_hist = NULL;
    /* FLL/TED defaults */
    s->fll_enabled = 0;
    s->fll_alpha_q15 = 0;
    s->fll_beta_q15 = 0;
    s->fll_freq_q15 = 0;
    s->fll_phase_q15 = 0;
    s->fll_prev_r = 0;
    s->fll_prev_j = 0;
    s->ted_enabled = 0;
    s->ted_gain_q20 = 0;
    s->ted_sps = 0;
    s->ted_mu_q20 = 0;
    /* Initialize FLL and TED module states */
    fll_init_state(&s->fll_state);
    ted_init_state(&s->ted_state);
    /* Squelch estimator init */
    s->squelch_running_power = 0;
    {
        /* Keep squelch timing roughly constant in seconds across rate changes.
           Baseline: 12 kHz -> stride 16, window 2048 (~170 ms). */
        const int base_fs = 12000;
        int stride = (s->rate_in > 0) ? (int)((int64_t)s->rate_in * 16 / base_fs) : 16;
        if (stride < 4) {
            stride = 4;
        } else if (stride > 256) {
            stride = 256;
        }
        s->squelch_decim_stride = stride;
        int window = (s->rate_in > 0) ? (int)((int64_t)s->rate_in * 2048 / base_fs) : 2048;
        if (window < 256) {
            window = 256;
        } else if (window > 32768) {
            window = 32768;
        }
        s->squelch_window = window;
    }
    s->squelch_decim_phase = 0;
    /* Squelch soft gate defaults */
    s->squelch_gate_open = 1;
    s->squelch_env_q15 = 32768;
    s->squelch_env_attack_q15 = 4096;  /* ~0.125 */
    s->squelch_env_release_q15 = 1024; /* ~0.031 */
    /* HB decimator histories */
    for (int st = 0; st < 10; st++) {
        memset(s->hb_hist_i[st], 0, sizeof(s->hb_hist_i[st]));
        memset(s->hb_hist_q[st], 0, sizeof(s->hb_hist_q[st]));
    }
    /* Legacy CIC histories used by fifth_order path */
    for (int st = 0; st < 10; st++) {
        memset(s->lp_i_hist[st], 0, sizeof(s->lp_i_hist[st]));
        memset(s->lp_q_hist[st], 0, sizeof(s->lp_q_hist[st]));
    }
    /* Input ring does not require double-buffer init */
    s->lowpassed = s->input_cb_buf;
    s->lp_len = 0;
    /* Reset IQ balance EMA */
    s->iqbal_alpha_ema_r_q15 = 0;
    s->iqbal_alpha_ema_i_q15 = 0;
    pthread_cond_init(&s->ready, NULL);
    pthread_mutex_init(&s->ready_m, NULL);
    s->output_target = output;

    /* FM AGC auto-tune per-instance state */
    s->fm_agc_auto_init = 0;
    s->fm_agc_ema_rms = 0.0;
    s->fm_agc_clip_run = 0;
    s->fm_agc_under_run = 0;

    /* FM CMA (>=5 taps) persistent state */
    s->fm_cma5_inited = 0;
    s->fm_cma5_prev_mu = 0;
    s->fm_cma5_prev_strength = 0;
    s->fm_cma5_prev_taps = 0;
    s->fm_cma5_prev_warm_cfg = 0;
    s->fm_cma5_warm_rem = 0;
    for (int k = 0; k < 5; k++) {
        s->fm_cma5_taps_q15[k] = (k == 0) ? 32767 : 0;
    }
    s->fm_cma_guard_inited = 0;
    s->fm_cma_guard_reject_streak = 0;
    s->fm_cma_guard_mu_scale = 1.0;

    /* Experimental CQPSK path (off by default). Enable via env DSD_NEO_CQPSK=1 */
    s->cqpsk_enable = 0;
    const char* env_cqpsk = getenv("DSD_NEO_CQPSK");
    if (env_cqpsk
        && (*env_cqpsk == '1' || *env_cqpsk == 'y' || *env_cqpsk == 'Y' || *env_cqpsk == 't' || *env_cqpsk == 'T')) {
        s->cqpsk_enable = 1;
        fprintf(stderr, " DSP: CQPSK pre-processing enabled (experimental)\n");
    }

    /* CQPSK acquisition FLL defaults */
    s->cqpsk_acq_fll_enable = 0;
    s->cqpsk_acq_fll_locked = 0;
    s->cqpsk_acq_quiet_runs = 0;
    /* CQPSK differential history */
    s->cqpsk_diff_prev_r = 0;
    s->cqpsk_diff_prev_j = 0;

    /* Mode-specific adjustments */
    if (mode == DEMOD_ANALOG) {
        s->downsample_passes = 1;
        s->comp_fir_size = 9;
        s->custom_atan = 0;
        s->deemph = 1;
        s->rate_out2 = rtl_dsp_bw_hz;
    } else if (mode == DEMOD_RO2) {
        s->downsample_passes = 0;
        s->comp_fir_size = 0;
        s->custom_atan = 0;
        s->deemph = (p && p->deemph_default) ? 1 : 0;
        s->rate_out2 = rtl_dsp_bw_hz;
    } else {
        /* Digital default */
        s->downsample_passes = 0;
        s->comp_fir_size = 0;
        s->custom_atan = 0;
        s->deemph = (p && p->deemph_default) ? 1 : 0;
        s->rate_out2 = rtl_dsp_bw_hz;
    }

    /* Legacy discriminator path removed; keep placeholders NULL. */
    s->discriminator = NULL;
    /* Initialize minimal worker pool (env-gated via DSD_NEO_MT). */
    demod_mt_init(s);

    /* Generic IQ balance defaults (image suppression); mode-aware guards in DSP pipeline.
       Start disabled so the UI/DSP menu fully controls this DSP block. */
    s->iqbal_enable = 0;
    s->iqbal_thr_q15 = 655; /* ~0.02 */
    s->iqbal_alpha_ema_r_q15 = 0;
    s->iqbal_alpha_ema_i_q15 = 0;
    s->iqbal_alpha_ema_a_q15 = 6553; /* ~0.2 */
}

} // namespace

/**
 * @brief Initialize the demodulator for the requested mode and attach output ring.
 *
 * Chooses RO2/digital/analog initialization based on @p opts flags, seeds mode
 * defaults, primes the worker pool, and wires up the output ring target.
 * Returns immediately when inputs are NULL.
 *
 * @param demod         Demodulator state to initialize.
 * @param output        Output ring target for demodulated audio.
 * @param opts          Decoder options to derive mode/config flags.
 * @param rtl_dsp_bw_hz Baseband bandwidth in Hz for initial rate defaults.
 */
void
rtl_demod_init_for_mode(struct demod_state* demod, struct output_state* output, const dsd_opts* opts,
                        int rtl_dsp_bw_hz) {
    if (!demod || !output || !opts) {
        return;
    }

    DemodInitParams params = {};
    if (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1 || opts->frame_provoice == 1) {
        demod_init_mode(demod, DEMOD_RO2, &params, rtl_dsp_bw_hz, output);
    } else if (opts->analog_only == 1 || opts->m17encoder == 1) {
        params.deemph_default = 1;
        demod_init_mode(demod, DEMOD_ANALOG, &params, rtl_dsp_bw_hz, output);
    } else {
        demod_init_mode(demod, DEMOD_DIGITAL, &params, rtl_dsp_bw_hz, output);
    }
}

/**
 * @brief Apply environment/runtime overrides to the demodulator state.
 *
 * Mirrors CLI/env-driven configuration into the demodulator, covering DSP
 * toggles (HB vs legacy decimator, FS/4 shift, combine-rotate), resampler
 * targets, FLL/TED tuning, CQPSK path enable, blanker/AGC/CMA knobs, and
 * IQ balance defaults. Early-exits on NULL inputs.
 *
 * @param demod Demodulator state to configure.
 * @param opts  Decoder options used for runtime flags.
 */
void
rtl_demod_config_from_env_and_opts(struct demod_state* demod, dsd_opts* opts) {
    if (!demod || !opts) {
        return;
    }

    dsd_neo_config_init(opts);
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        return;
    }

    if (cfg->hb_decim_is_set) {
        use_halfband_decimator = (cfg->hb_decim != 0);
    }
    if (cfg->combine_rot_is_set) {
        combine_rotate_enabled = (cfg->combine_rot != 0);
    }
    if (cfg->upsample_fp_is_set) {
        upsample_fixedpoint_enabled = (cfg->upsample_fp != 0);
    }

    if (cfg->fs4_shift_disable_is_set) {
        disable_fs4_shift = (cfg->fs4_shift_disable != 0);
    }

    /* rtltcp-specific sane defaults unless explicitly overridden via env/config */
    if (opts->rtltcp_enabled) {
        /* For rtl_tcp, prefer consistency with USB: allow fs/4 shift fallback when offset tuning unavailable */
        if (!cfg->fs4_shift_disable_is_set) {
            disable_fs4_shift = 0;
        }
    }

    int enable_resamp = 1;
    int target = 48000;
    if (cfg->resamp_is_set) {
        enable_resamp = cfg->resamp_disable ? 0 : 1;
        target = cfg->resamp_target_hz > 0 ? cfg->resamp_target_hz : 48000;
    }
    /* Defer resampler design until after capture settings establish actual rates */
    demod->resamp_target_hz = enable_resamp ? target : 0;
    demod->resamp_enabled = 0;

    demod->fll_enabled = cfg->fll_is_set ? (cfg->fll_enable != 0) : 0;
    demod->fll_alpha_q15 = cfg->fll_alpha_is_set ? cfg->fll_alpha_q15 : 50;
    demod->fll_beta_q15 = cfg->fll_beta_is_set ? cfg->fll_beta_q15 : 5;
    demod->fll_deadband_q14 = cfg->fll_deadband_is_set ? cfg->fll_deadband_q14 : 45;
    demod->fll_slew_max_q15 = cfg->fll_slew_is_set ? cfg->fll_slew_max_q15 : 64;
    demod->fll_freq_q15 = 0;
    demod->fll_phase_q15 = 0;
    demod->fll_prev_r = demod->fll_prev_j = 0;
    /* Costas loop state (GNU Radio control loop derivative) */
    dsd_costas_loop_state_t* cl = &demod->costas_state;
    cl->phase = 0.0f;
    cl->freq = 0.0f;
    cl->max_freq = 1.0f;
    cl->min_freq = -1.0f;
    cl->loop_bw = cfg->costas_bw_is_set ? (float)cfg->costas_loop_bw : dsd_neo_costas_default_loop_bw();
    cl->damping = cfg->costas_damping_is_set ? (float)cfg->costas_damping : dsd_neo_costas_default_damping();
    cl->alpha = 0.0f;
    cl->beta = 0.0f;
    cl->error = 0.0f;
    cl->noise = cfg->costas_noise_db_is_set ? (float)pow(10.0, cfg->costas_noise_db / 10.0) : 1.0f;
    cl->order = cfg->costas_order_is_set ? cfg->costas_order : 4;
    cl->use_snr = cfg->costas_use_snr_is_set ? cfg->costas_use_snr : 0;
    cl->initialized = 0;
    demod->costas_err_avg_q14 = 0;

    demod->ted_enabled = cfg->ted_is_set ? (cfg->ted_enable != 0) : 0;
    demod->ted_gain_q20 = cfg->ted_gain_is_set ? cfg->ted_gain_q20 : 64;
    demod->ted_sps = cfg->ted_sps_is_set ? cfg->ted_sps : 10;
    demod->ted_mu_q20 = 0;
    demod->ted_force = cfg->ted_force_is_set ? (cfg->ted_force != 0) : 0;

    /* Default all DSP handles Off unless explicitly requested via env/CLI. */
    demod->cqpsk_enable = 0;
    const char* ev_cq = getenv("DSD_NEO_CQPSK");
    if (ev_cq && (*ev_cq == '1' || *ev_cq == 'y' || *ev_cq == 'Y' || *ev_cq == 't' || *ev_cq == 'T')) {
        demod->cqpsk_enable = 1;
    }

    /* Optional: acquisition-only FLL for CQPSK (pre-Costas).
       Default OFF; may be enabled explicitly via env/UI. */
    demod->cqpsk_acq_fll_enable = 0;
    const char* af = getenv("DSD_NEO_CQPSK_ACQ_FLL");
    if (af) {
        if (*af == '1' || *af == 'y' || *af == 'Y' || *af == 't' || *af == 'T') {
            demod->cqpsk_acq_fll_enable = 1;
        }
    }
    demod->cqpsk_acq_fll_locked = 0;
    demod->cqpsk_acq_quiet_runs = 0;

    /* Map CLI runtime toggles for CQPSK LMS */
    if (opts->cqpsk_lms != 0) {
        demod->cqpsk_lms_enable = 1;
    }
    if (opts->cqpsk_mu_q15 > 0) {
        demod->cqpsk_mu_q15 = opts->cqpsk_mu_q15;
    } else if (demod->cqpsk_mu_q15 == 0) {
        demod->cqpsk_mu_q15 = 1; /* tiny default */
    }
    if (opts->cqpsk_stride > 0) {
        demod->cqpsk_update_stride = opts->cqpsk_stride;
    } else if (demod->cqpsk_update_stride == 0) {
        demod->cqpsk_update_stride = 4;
    }

    /* Matched filter pre-EQ default Off; allow env to enable */
    demod->cqpsk_mf_enable = 0;
    const char* mf = getenv("DSD_NEO_CQPSK_MF");
    if (mf && (*mf == '1' || *mf == 'y' || *mf == 'Y' || *mf == 't' || *mf == 'T')) {
        demod->cqpsk_mf_enable = 1;
    }

    /* Optional RRC matched filter configuration */
    demod->cqpsk_rrc_enable = 0;
    demod->cqpsk_rrc_alpha_q15 = (int)(0.25 * 32768.0); /* default 0.25 */
    demod->cqpsk_rrc_span_syms = 6;                     /* default 6 symbols (total span ~12) */
    const char* rrc = getenv("DSD_NEO_CQPSK_RRC");
    if (rrc && (*rrc == '1' || *rrc == 'y' || *rrc == 'Y' || *rrc == 't' || *rrc == 'T')) {
        demod->cqpsk_rrc_enable = 1;
    }
    const char* rrca = getenv("DSD_NEO_CQPSK_RRC_ALPHA");
    if (rrca) {
        int v = atoi(rrca);
        if (v < 1) {
            v = 1;
        }
        if (v > 100) {
            v = 100;
        }
        demod->cqpsk_rrc_alpha_q15 = (int)((v / 100.0) * 32768.0);
    }
    const char* rrcs = getenv("DSD_NEO_CQPSK_RRC_SPAN");
    if (rrcs) {
        int v = atoi(rrcs);
        if (v < 3) {
            v = 3;
        }
        if (v > 16) {
            v = 16;
        }
        demod->cqpsk_rrc_span_syms = v;
    }
    /* When CQPSK is enabled for P25 Phase 2 we keep MF/RRC disabled by default.
       Users may enable these helpers explicitly via env or the DSP menu. */

    /* FM/C4FM amplitude AGC (pre-discriminator): default OFF for all modes.
       Users can enable via env `DSD_NEO_FM_AGC=1` or the UI toggle. */
    int default_fm_agc = 0;
    demod->fm_agc_enable = cfg->fm_agc_is_set ? (cfg->fm_agc_enable != 0) : default_fm_agc;
    demod->fm_agc_target_rms = cfg->fm_agc_target_is_set ? cfg->fm_agc_target_rms : 10000;
    demod->fm_agc_min_rms = cfg->fm_agc_min_is_set ? cfg->fm_agc_min_rms : 2000;
    demod->fm_agc_alpha_up_q15 = cfg->fm_agc_alpha_up_is_set ? cfg->fm_agc_alpha_up_q15 : 8192;        /* ~0.25 */
    demod->fm_agc_alpha_down_q15 = cfg->fm_agc_alpha_down_is_set ? cfg->fm_agc_alpha_down_q15 : 24576; /* ~0.75 */
    if (demod->fm_agc_gain_q15 <= 0) {
        demod->fm_agc_gain_q15 = 32768; /* unity */
    }
    demod->fm_limiter_enable = cfg->fm_limiter_is_set ? (cfg->fm_limiter_enable != 0) : 0;
    demod->iq_dc_block_enable = cfg->iq_dc_block_is_set ? (cfg->iq_dc_block_enable != 0) : 0;
    demod->iq_dc_shift = cfg->iq_dc_shift_is_set ? cfg->iq_dc_shift : 11;
    demod->iq_dc_avg_r = demod->iq_dc_avg_i = 0;

    /* Impulse blanker (pre-decimation) */
    demod->blanker_enable = cfg->blanker_is_set ? (cfg->blanker_enable != 0) : 0;
    demod->blanker_thr = cfg->blanker_thr_is_set ? cfg->blanker_thr : 20000;
    demod->blanker_win = cfg->blanker_win_is_set ? cfg->blanker_win : 2;

    /* FM/FSK CMA equalizer defaults (pre-discriminator) */
    demod->fm_cma_enable = cfg->fm_cma_is_set ? (cfg->fm_cma_enable != 0) : 0;
    demod->fm_cma_taps = cfg->fm_cma_taps_is_set ? cfg->fm_cma_taps : 1;
    demod->fm_cma_mu_q15 = cfg->fm_cma_mu_is_set ? cfg->fm_cma_mu_q15 : 2;
    demod->fm_cma_warmup = cfg->fm_cma_warmup_is_set ? cfg->fm_cma_warmup : 20000;
    demod->fm_cma_strength = cfg->fm_cma_strength_is_set ? cfg->fm_cma_strength : 1;
    demod->fm_cma_guard_freeze = 0;
    demod->fm_cma_guard_accepts = 0;
    demod->fm_cma_guard_rejects = 0;

    /* Channel complex low-pass (post-HB, complex baseband).
       Default policy (Fs~=24 kHz RTL DSP baseband):
         - For analog-like modes, enable a wide channel LPF to narrow
           out-of-channel noise while preserving audio bandwidth.
         - For digital voice modes (P25/DMR/NXDN/etc.), enable a narrower
           digital-specific LPF tuned for ~4.8 ksps symbols to improve SNR.
       Env override:
         - DSD_NEO_CHANNEL_LPF=0 forces off (all modes).
         - DSD_NEO_CHANNEL_LPF!=0 forces on (all modes, wide profile). */
    int channel_lpf = 0;
    int channel_lpf_profile = 0; /* 0=wide/analog, 1=digital-narrow */
    if (cfg->channel_lpf_is_set) {
        /* Env forces on/off; when forced on, use wide profile to avoid
           surprising very narrow channels. */
        channel_lpf = (cfg->channel_lpf_enable != 0);
        channel_lpf_profile = 0;
    } else {
        int high_fs = (demod->rate_in >= 20000); /* currently 24 kHz DSP baseband */
        int digital_mode = (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1 || opts->frame_provoice == 1
                            || opts->frame_dmr == 1 || opts->frame_nxdn48 == 1 || opts->frame_nxdn96 == 1
                            || opts->frame_dstar == 1 || opts->frame_dpmr == 1 || opts->frame_m17 == 1);
        if (high_fs) {
            channel_lpf = 1;
            channel_lpf_profile = digital_mode ? 1 : 0;
        }
    }
    demod->channel_lpf_enable = channel_lpf ? 1 : 0;
    demod->channel_lpf_profile = channel_lpf_profile;
}

/**
 * @brief Apply sane defaults for digital vs analog demodulation when unset.
 *
 * Populates TED/FLL defaults, TED SPS, channel/audio filter profiles, and
 * analog deemphasis based on the selected mode when the user has not
 * overridden settings via env/CLI. Relies on @p output for effective rate.
 *
 * @param demod  Demodulator state to update.
 * @param opts   Decoder options (mode flags).
 * @param output Output ring used to infer sample rate.
 */
void
rtl_demod_select_defaults_for_mode(struct demod_state* demod, dsd_opts* opts, const struct output_state* output) {
    if (!demod || !opts || !output) {
        return;
    }
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        return;
    }

    int env_fll_alpha_set = cfg->fll_alpha_is_set;
    int env_fll_beta_set = cfg->fll_beta_is_set;
    int env_fll_deadband_set = cfg->fll_deadband_is_set;
    int env_fll_slew_set = cfg->fll_slew_is_set;
    int env_ted_sps_set = cfg->ted_sps_is_set;
    int env_ted_gain_set = cfg->ted_gain_is_set;
    /* Treat all digital voice modes as digital for FLL/TED defaults */
    int digital_mode = (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1 || opts->frame_provoice == 1
                        || opts->frame_dmr == 1 || opts->frame_nxdn48 == 1 || opts->frame_nxdn96 == 1
                        || opts->frame_dstar == 1 || opts->frame_dpmr == 1 || opts->frame_m17 == 1);
    int p25_mode = (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1);
    if (digital_mode) {
        /* For digital modes, never auto-enable FLL/TED.
           Leave on/off decisions to env/CLI/UI, but still derive sane defaults
           for TED/FLL parameters when not explicitly provided. */
        if (!env_ted_sps_set) {
            int Fs_cx = 0;
            /* TED operates on complex baseband at demod->rate_out.
               Prefer that rate even when an audio resampler is enabled. */
            if (demod->rate_out > 0) {
                Fs_cx = demod->rate_out;
            } else {
                Fs_cx = (int)output->rate;
            }
            if (Fs_cx <= 0) {
                Fs_cx = 48000; /* safe default */
            }
            /* Choose symbol rate by mode; keep explicit branches for narrow paths. */
            int sym_rate = 4800; /* generic 4.8 ksps */
            if (opts->frame_p25p2 == 1 || opts->frame_x2tdma == 1) {
                sym_rate = 6000;
            } else if (opts->frame_nxdn48 == 1 || opts->frame_dpmr == 1) {
                sym_rate = 2400;
            } else if (opts->frame_p25p1 == 1 || opts->frame_provoice == 1) {
                sym_rate = 4800;
            }
            if (Fs_cx < (sym_rate * 2)) {
                LOG_WARNING("TED SPS: demod rate %d Hz is low for ~%d sym/s; clamping to minimum SPS.\n", Fs_cx,
                            sym_rate);
            }
            int sps = (Fs_cx + (sym_rate / 2)) / sym_rate; /* round(Fs/sym_rate) */
            if (sps < 2) {
                sps = 2;
            }
            if (sps > 64) {
                sps = 64;
            }
            demod->ted_sps = sps;
        }
        if (!env_ted_gain_set) {
            int base_gain = 64;
            /* For P25 at low SPS (e.g., 12 kHz / 4800 or 6000 sym/s),
               use a slightly stronger default Gardner gain. */
            if (p25_mode && demod->ted_sps > 0 && demod->ted_sps <= 4) {
                base_gain = 96;
            }
            demod->ted_gain_q20 = base_gain;
        }
        /* Digital defaults: slightly stronger, lower-deadband FLL for CQPSK/FM. */
        if (!env_fll_alpha_set) {
            demod->fll_alpha_q15 = 150;
        }
        if (!env_fll_beta_set) {
            demod->fll_beta_q15 = 15;
        }
        if (!env_fll_deadband_set) {
            demod->fll_deadband_q14 = 32;
        }
        if (!env_fll_slew_set) {
            demod->fll_slew_max_q15 = 128;
        }
    } else {
        /* For analog-like modes, also avoid auto-enabling FLL/TED.
           Respect any explicit env/CLI/UI decisions, but do not change gates. */
        if (!env_fll_alpha_set) {
            demod->fll_alpha_q15 = 50;
        }
        if (!env_fll_beta_set) {
            demod->fll_beta_q15 = 5;
        }
    }
}

/**
 * @brief Recompute resampler design after rate changes.
 *
 * Updates the resampler taps/ratios based on the current demod/output rates
 * and the requested target, falling back to @p rtl_dsp_bw_hz when needed.
 * Also updates output.rate to reflect the new sink rate.
 *
 * @param demod         Demodulator state (contains resampler config).
 * @param output        Output ring state to refresh.
 * @param rtl_dsp_bw_hz DSP baseband bandwidth in Hz when rate_out is unset.
 */
void
rtl_demod_maybe_update_resampler_after_rate_change(struct demod_state* demod, struct output_state* output,
                                                   int rtl_dsp_bw_hz) {
    if (!demod || !output) {
        return;
    }
    if (demod->resamp_target_hz <= 0) {
        demod->resamp_enabled = 0;
        output->rate = demod->rate_out;
        return;
    }
    int target = demod->resamp_target_hz;
    int inRate = demod->rate_out > 0 ? demod->rate_out : rtl_dsp_bw_hz;
    int g = gcd_int(inRate, target);
    int L = target / g;
    int M = inRate / g;
    if (L < 1) {
        L = 1;
    }
    if (M < 1) {
        M = 1;
    }
    int scale = (M > 0) ? ((L + M - 1) / M) : 1;

    if (scale > 12) {
        if (demod->resamp_enabled) {
            /* Disable and free on out-of-bounds ratio */
            if (demod->resamp_taps) {
                dsd_neo_aligned_free(demod->resamp_taps);
                demod->resamp_taps = NULL;
            }
            if (demod->resamp_hist) {
                dsd_neo_aligned_free(demod->resamp_hist);
                demod->resamp_hist = NULL;
            }
        }
        demod->resamp_enabled = 0;
        output->rate = demod->rate_out;
        LOG_WARNING("Resampler ratio too large on retune (L=%d,M=%d). Disabled.\n", L, M);
        return;
    }

    /* Re-design only if params changed or buffers not allocated */
    if (!demod->resamp_enabled || demod->resamp_L != L || demod->resamp_M != M || demod->resamp_taps == NULL
        || demod->resamp_hist == NULL) {
        if (demod->resamp_taps) {
            dsd_neo_aligned_free(demod->resamp_taps);
            demod->resamp_taps = NULL;
        }
        if (demod->resamp_hist) {
            dsd_neo_aligned_free(demod->resamp_hist);
            demod->resamp_hist = NULL;
        }
        resamp_design(demod, L, M);
        demod->resamp_L = L;
        demod->resamp_M = M;
        demod->resamp_enabled = 1;
        LOG_INFO("Resampler reconfigured: %d -> %d Hz (L=%d,M=%d).\n", inRate, target, L, M);
    }
    output->rate = target;
}

/**
 * @brief Refresh TED SPS after capture/output rate changes.
 *
 * When TED SPS is not explicitly forced via runtime configuration, recompute
 * the nominal samples-per-symbol from the current output rate and mode.
 *
 * @param demod  Demodulator state.
 * @param opts   Decoder options (mode flags).
 * @param output Output ring (for sink rate).
 */
void
rtl_demod_maybe_refresh_ted_sps_after_rate_change(struct demod_state* demod, const dsd_opts* opts,
                                                  const struct output_state* output) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!demod || !cfg || !output) {
        return;
    }

    if (cfg->ted_sps_is_set) {
        return; /* user explicitly set; do not override */
    }

    int Fs_cx = 0;
    /* TED always sees complex baseband at demod->rate_out; compute SPS in that domain,
       independent of any post-demod audio resampling. */
    if (demod->rate_out > 0) {
        Fs_cx = demod->rate_out;
    } else {
        Fs_cx = (int)output->rate;
    }
    if (Fs_cx <= 0) {
        Fs_cx = 48000;
    }
    int sps = 0;
    if (opts) {
        int sym_rate = 4800;
        if (opts->frame_p25p2 == 1 || opts->frame_x2tdma == 1) {
            sym_rate = 6000;
        } else if (opts->frame_nxdn48 == 1 || opts->frame_dpmr == 1) {
            sym_rate = 2400;
        } else if (opts->frame_p25p1 == 1 || opts->frame_provoice == 1) {
            sym_rate = 4800;
        }
        if (Fs_cx < (sym_rate * 2)) {
            LOG_WARNING("TED SPS: demod rate %d Hz is low for ~%d sym/s; clamping to minimum SPS.\n", Fs_cx, sym_rate);
        }
        sps = (Fs_cx + (sym_rate / 2)) / sym_rate;
    } else {
        sps = (Fs_cx + 2400) / 4800;
    }
    if (sps < 2) {
        sps = 2;
    }
    if (sps > 64) {
        sps = 64;
    }
    demod->ted_sps = sps;
}

/**
 * @brief Release resources allocated by demod_init_mode/config helpers.
 *
 * Tears down resampler/filter buffers, worker pools, and any dynamically
 * allocated state within the demodulator instance. Safe on partially
 * initialized structures.
 *
 * @param demod Demodulator state to clean up.
 */
void
rtl_demod_cleanup(struct demod_state* demod) {
    if (!demod) {
        return;
    }
    pthread_cond_destroy(&demod->ready);
    pthread_mutex_destroy(&demod->ready_m);
    demod_mt_destroy(demod);
    if (demod->resamp_taps) {
        dsd_neo_aligned_free(demod->resamp_taps);
        demod->resamp_taps = NULL;
    }
    if (demod->resamp_hist) {
        dsd_neo_aligned_free(demod->resamp_hist);
        demod->resamp_hist = NULL;
    }
    if (demod->post_polydecim_taps) {
        dsd_neo_aligned_free(demod->post_polydecim_taps);
        demod->post_polydecim_taps = NULL;
    }
    if (demod->post_polydecim_hist) {
        dsd_neo_aligned_free(demod->post_polydecim_hist);
        demod->post_polydecim_hist = NULL;
    }
}
