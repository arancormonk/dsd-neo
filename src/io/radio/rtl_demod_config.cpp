// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RTL-SDR demodulation configuration helpers.
 *
 * Centralizes initialization and configuration of the demodulation state
 * used by the RTL-SDR stream pipeline, including mode selection, env/opts
 * driven DSP toggles, and rate-dependent helpers.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/math_utils.h>
#include <dsd-neo/dsp/resampler.h>
#include <dsd-neo/dsp/ted.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/mem.h>
#include <dsd-neo/runtime/ring.h>
#include <dsd-neo/runtime/worker_pool.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/dsp/costas.h"
#include "dsd-neo/dsp/fsk_modem.h"
#include "dsd-neo/platform/threading.h"

/* Allow disabling the fs/4 capture frequency shift via env for trunking/exact-center use cases. */
int disable_fs4_shift = 0; /* Set by env DSD_NEO_DISABLE_FS4_SHIFT=1 */

namespace {

enum DemodMode : unsigned char { DEMOD_DIGITAL = 0, DEMOD_ANALOG = 1, DEMOD_RO2 = 2 };

struct DemodInitParams {
    int deemph_default;
};

static void
fsk_modem_apply_config(struct demod_state* s) {
    if (!s) {
        return;
    }
    dsd_fsk_modem_config cfg = {};
    cfg.sample_rate_hz = s->rate_out > 0 ? s->rate_out : s->rate_in;
    cfg.symbol_rate_hz = s->symbol_rate_hz > 0 ? s->symbol_rate_hz : 4800;
    cfg.levels = (s->symbol_levels == 2) ? 2 : 4;
    cfg.channel_profile = s->channel_lpf_profile;
    dsd_fsk_modem_configure(&s->fsk_modem_state, &cfg);
}

static int
opts_flag_is_set(int flag) {
    return (flag == 1) ? 1 : 0;
}

static int
opts_digital_mode_count(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return opts_flag_is_set(opts->frame_p25p1) + opts_flag_is_set(opts->frame_p25p2)
           + opts_flag_is_set(opts->frame_provoice) + opts_flag_is_set(opts->frame_dmr)
           + opts_flag_is_set(opts->frame_nxdn48) + opts_flag_is_set(opts->frame_nxdn96)
           + opts_flag_is_set(opts->frame_x2tdma) + opts_flag_is_set(opts->frame_ysf)
           + opts_flag_is_set(opts->frame_dstar) + opts_flag_is_set(opts->frame_dpmr)
           + opts_flag_is_set(opts->frame_m17);
}

static int
opts_6000_mode_count(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return opts_flag_is_set(opts->frame_p25p2) + opts_flag_is_set(opts->frame_x2tdma);
}

static int
opts_2400_mode_count(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return opts_flag_is_set(opts->frame_nxdn48) + opts_flag_is_set(opts->frame_dpmr);
}

static int
opts_has_any_four_level_mode(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1 || opts->frame_dmr == 1 || opts->frame_nxdn48 == 1
            || opts->frame_nxdn96 == 1 || opts->frame_x2tdma == 1 || opts->frame_ysf == 1 || opts->frame_dpmr == 1
            || opts->frame_m17 == 1);
}

static int
opts_has_4800_four_level_mode(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return (opts->frame_p25p1 == 1 || opts->frame_dmr == 1 || opts->frame_nxdn96 == 1 || opts->frame_ysf == 1
            || opts->frame_m17 == 1);
}

static int
opts_symbol_rate_hz(const dsd_opts* opts) {
    if (!opts) {
        return 4800;
    }
    int digital_count = opts_digital_mode_count(opts);
    if (opts->frame_provoice == 1 && digital_count == 1) {
        return 9600;
    }
    if ((opts->frame_p25p2 == 1 || opts->frame_x2tdma == 1) && opts->frame_p25p1 == 0
        && digital_count == opts_6000_mode_count(opts)) {
        return 6000;
    }
    if ((opts->frame_nxdn48 == 1 || opts->frame_dpmr == 1) && digital_count == opts_2400_mode_count(opts)) {
        return 2400;
    }
    return 4800;
}

static int
opts_symbol_levels_for_rate(const dsd_opts* opts, int symbol_rate_hz) {
    if (!opts) {
        return 4;
    }
    if (symbol_rate_hz == 9600 && opts->frame_provoice == 1) {
        return 2;
    }
    if (symbol_rate_hz == 4800 && opts->frame_dstar == 1 && !opts_has_4800_four_level_mode(opts)) {
        return 2;
    }
    if ((opts->frame_dstar == 1 || opts->frame_provoice == 1) && !opts_has_any_four_level_mode(opts)) {
        return 2;
    }
    return 4;
}

static int
opts_has_6k25_mode(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return (opts->frame_nxdn48 == 1 || opts->frame_dpmr == 1 || opts->frame_dstar == 1);
}

static int
opts_has_p25_mode(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1);
}

static int
opts_has_6000_mode(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return (opts->frame_p25p2 == 1 || opts->frame_x2tdma == 1);
}

static int
opts_has_2400_mode(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return (opts->frame_nxdn48 == 1 || opts->frame_dpmr == 1);
}

static int
demod_uses_cqpsk_profile(const demod_state* demod) {
    return (demod && demod->cqpsk_enable) ? 1 : 0;
}

static int
opts_channel_profile_for_rate(const dsd_opts* opts, const demod_state* demod, int symbol_rate_hz) {
    if (!opts) {
        return DSD_CH_LPF_PROFILE_WIDE;
    }
    switch (symbol_rate_hz) {
        case 9600:
            if (opts->frame_provoice == 1) {
                return DSD_CH_LPF_PROFILE_PROVOICE;
            }
            break;
        case 2400:
            if (opts_has_2400_mode(opts)) {
                return DSD_CH_LPF_PROFILE_6K25;
            }
            break;
        case 6000:
            if (opts_has_6000_mode(opts)) {
                return (opts->frame_p25p2 == 1) ? DSD_CH_LPF_PROFILE_P25_CQPSK : DSD_CH_LPF_PROFILE_12K5;
            }
            break;
        default: break;
    }
    if (dsd_opts_uses_wide_4800_profile(opts)) {
        return DSD_CH_LPF_PROFILE_12K5;
    }
    if (opts_has_p25_mode(opts)) {
        return demod_uses_cqpsk_profile(demod) ? DSD_CH_LPF_PROFILE_P25_CQPSK : DSD_CH_LPF_PROFILE_P25_C4FM;
    }
    if (opts_has_6k25_mode(opts)) {
        return DSD_CH_LPF_PROFILE_6K25;
    }
    if (opts->frame_x2tdma == 1) {
        return DSD_CH_LPF_PROFILE_12K5;
    }
    if (opts->frame_provoice == 1) {
        return DSD_CH_LPF_PROFILE_PROVOICE;
    }
    return DSD_CH_LPF_PROFILE_WIDE;
}

static void
demod_apply_output_kind(struct demod_state* s, const dsd_opts* opts) {
    if (!s || !opts) {
        return;
    }
    s->symbol_rate_hz = opts_symbol_rate_hz(opts);
    s->symbol_levels = opts_symbol_levels_for_rate(opts, s->symbol_rate_hz);

    if (!dsd_opts_has_digital_decode_mode(opts) || opts->analog_only == 1 || opts->m17encoder == 1) {
        s->output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    } else if (s->cqpsk_enable) {
        s->output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
        s->symbol_levels = 4;
    } else {
        s->output_kind = DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
    }

    if (s->output_kind == DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR) {
        s->cqpsk_enable = 0;
        s->ted_enabled = 0;
    } else if (s->output_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK) {
        s->cqpsk_enable = 1;
        s->ted_enabled = 1;
    }
    fsk_modem_apply_config(s);
}

static void
demod_init_common_defaults(struct demod_state* s, int rtl_dsp_bw_hz, struct output_state* output) {
    (void)output;
    s->rate_in = rtl_dsp_bw_hz;
    s->rate_out = rtl_dsp_bw_hz;
    s->squelch_level = 0.0f;
    s->conseq_squelch = 10;
    s->terminate_on_squelch = 0;
    s->squelch_hits = 11;
    s->downsample_passes = 0;
    s->post_downsample = 1;
    s->custom_atan = 0;
    s->deemph = 0;
    s->rate_out2 = -1;
    s->mode_demod = &dsd_fm_demod;
    s->pre_j = s->pre_r = 0.0f;
    s->fm_demod_history_valid = 0;
    s->prev_lpr_index = 0;
    s->deemph_a = 0.0f;
    s->deemph_avg = 0.0f;
    s->channel_lpf_enable = 0;
    s->channel_lpf_hist_len = 143;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_WIDE;
    for (int k = 0; k < 144; k++) {
        s->channel_lpf_hist_i[k] = 0;
        s->channel_lpf_hist_q[k] = 0;
    }
    s->channel_pwr = 0.0f;
    s->channel_squelch_level = 0.0f;
    s->channel_squelched = 0;
    s->audio_lpf_enable = 0;
    s->audio_lpf_alpha = 0.0f;
    s->audio_lpf_state = 0.0f;
    s->now_lpr = 0.0f;
    s->dc_block = 1;
    s->dc_avg = 0.0f;
    s->resamp_enabled = 0;
    s->resamp_target_hz = 0;
    s->resamp_L = 1;
    s->resamp_M = 1;
    s->resamp_phase = 0;
    s->resamp_taps_len = 0;
    s->resamp_taps_per_phase = 0;
    s->resamp_taps = NULL;
    s->resamp_hist = NULL;
    s->post_polydecim_enabled = 0;
    s->post_polydecim_M = 1;
    s->post_polydecim_K = 0;
    s->post_polydecim_hist_head = 0;
    s->post_polydecim_taps = NULL;
    s->post_polydecim_hist = NULL;
    s->ted_enabled = 0;
    s->ted_gain = 0.0f;
    s->ted_gain_is_set = 0;
    s->ted_effective_gain = 0.0f;
    s->ted_sps = 0;
    s->ted_sps_override = 0;
    s->costas_reset_pending = 0;
    s->ted_mu = 0.0f;
    s->sps_is_integer = 1;
    ted_init_state(&s->ted_state);
    s->squelch_running_power = 0;
    s->squelch_decim_phase = 0;
    s->squelch_gate_open = 1;
    s->squelch_env = 1.0f;
    s->squelch_env_attack = 0.125f;
    s->squelch_env_release = 0.03125f;
    for (int st = 0; st < 10; st++) {
        DSD_MEMSET(s->hb_hist_i[st], 0, sizeof(s->hb_hist_i[st]));
        DSD_MEMSET(s->hb_hist_q[st], 0, sizeof(s->hb_hist_q[st]));
    }
    s->lowpassed = s->input_cb_buf;
    s->lp_len = 0;
    s->iqbal_alpha_ema_r = 0.0f;
    s->iqbal_alpha_ema_i = 0.0f;
    dsd_cond_init(&s->ready);
    dsd_mutex_init(&s->ready_m);
}

static void
demod_init_squelch_windows(struct demod_state* s) {
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

static void
demod_init_cqpsk_defaults(struct demod_state* s) {
    s->cqpsk_enable = 0;
    s->output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    s->symbol_rate_hz = 4800;
    s->symbol_levels = 4;
    s->cqpsk_diff_prev_r = 1.0f;
    s->cqpsk_diff_prev_j = 0.0f;
    s->cqpsk_agc_avg = 1.0f;
    dsd_fsk_modem_config fsk_cfg = {};
    fsk_cfg.sample_rate_hz = s->rate_out;
    fsk_cfg.symbol_rate_hz = s->symbol_rate_hz;
    fsk_cfg.levels = s->symbol_levels;
    fsk_cfg.channel_profile = s->channel_lpf_profile;
    dsd_fsk_modem_init(&s->fsk_modem_state, &fsk_cfg);
}

static void
demod_apply_mode_defaults(struct demod_state* s, DemodMode mode, const DemodInitParams* p, int rtl_dsp_bw_hz) {
    if (mode == DEMOD_ANALOG) {
        s->downsample_passes = 1;
        s->deemph = 1;
    } else {
        s->downsample_passes = 0;
        s->deemph = (p && p->deemph_default) ? 1 : 0;
    }
    s->custom_atan = 0;
    s->rate_out2 = rtl_dsp_bw_hz;
}

static void
demod_init_mode(struct demod_state* s, DemodMode mode, const DemodInitParams* p, int rtl_dsp_bw_hz,
                struct output_state* output) {
    demod_init_common_defaults(s, rtl_dsp_bw_hz, output);
    demod_init_squelch_windows(s);
    demod_init_cqpsk_defaults(s);
    demod_apply_mode_defaults(s, mode, p, rtl_dsp_bw_hz);

    /* Initialize minimal worker pool (env-gated via DSD_NEO_MT). */
    demod_mt_init(s);

    /* Generic IQ balance defaults (image suppression); mode-aware guards in DSP pipeline.
       Start disabled so the UI/DSP menu fully controls this DSP block. */
    s->iqbal_enable = 0;
    s->iqbal_thr = 0.02f;
    s->iqbal_alpha_ema_r = 0.0f;
    s->iqbal_alpha_ema_i = 0.0f;
    s->iqbal_alpha_ema_a = 0.2f;
}

static void
demod_apply_runtime_global_flags(const dsd_opts* opts, const dsdneoRuntimeConfig* cfg) {
    if (cfg->fs4_shift_disable_is_set) {
        disable_fs4_shift = (cfg->fs4_shift_disable != 0);
    }
    if (opts->rtltcp_enabled && !cfg->fs4_shift_disable_is_set) {
        disable_fs4_shift = 0;
    }
}

static void
demod_apply_resampler_target_defaults(struct demod_state* demod, const dsdneoRuntimeConfig* cfg) {
    int enable_resamp = 1;
    int target = 48000;
    if (cfg->resamp_is_set) {
        enable_resamp = cfg->resamp_disable ? 0 : 1;
        target = cfg->resamp_target_hz > 0 ? cfg->resamp_target_hz : 48000;
    }
    demod->resamp_target_hz = enable_resamp ? target : 0;
    demod->resamp_enabled = 0;
}

static void
demod_apply_costas_defaults(struct demod_state* demod, const dsdneoRuntimeConfig* cfg) {
    dsd_costas_loop_state_t* cl = &demod->costas_state;
    cl->phase = 0.0f;
    cl->freq = 0.0f;
    const float kTwoPi = 6.28318530717958647692f;
    float if_rate = (demod->rate_out > 0) ? (float)demod->rate_out : 24000.0f;
    cl->max_freq = kTwoPi * 2400.0f / if_rate;
    cl->min_freq = -cl->max_freq;
    cl->loop_bw = cfg->costas_bw_is_set ? (float)cfg->costas_loop_bw : 0.008f;
    cl->damping = cfg->costas_damping_is_set ? (float)cfg->costas_damping : dsd_neo_costas_default_damping();
    cl->alpha = 0.04f;
    cl->beta = 0.125f * cl->alpha * cl->alpha;
    cl->error = 0.0f;
    cl->error_smooth = 0.0f;
    cl->initialized = 0;
    demod->costas_err_avg_q14 = 0;
    demod->costas_err_raw_avg_q14 = 0;
    demod->costas_conf_avg_q14 = 0;
    demod->costas_zero_conf_pct = 0;
}

static void
demod_apply_ted_defaults(struct demod_state* demod, const dsdneoRuntimeConfig* cfg) {
    demod->ted_enabled = 0;
    demod->ted_gain = cfg->ted_gain_is_set ? cfg->ted_gain : 0.025f;
    demod->ted_gain_is_set = cfg->ted_gain_is_set ? 1 : 0;
    demod->ted_effective_gain = demod->ted_gain;
    demod->ted_sps = 10;
    demod->ted_mu = 0.0f;
}

static void
demod_apply_cqpsk_defaults(struct demod_state* demod, const dsd_opts* opts, const dsdneoRuntimeConfig* cfg) {
    demod->cqpsk_enable = (opts->mod_qpsk == 1) ? 1 : 0;
    if (cfg->cqpsk_is_set) {
        demod->cqpsk_enable = (cfg->cqpsk_enable != 0) ? 1 : 0;
    }
    if (demod->cqpsk_enable) {
        if (!demod->ted_enabled) {
            demod->ted_enabled = 1;
        }
        demod->mode_demod = &::qpsk_differential_demod;
        demod->cqpsk_diff_prev_r = 1.0f;
        demod->cqpsk_diff_prev_j = 0.0f;
    }
    demod_apply_output_kind(demod, opts);
}

static void
demod_apply_iq_defaults(struct demod_state* demod, const dsdneoRuntimeConfig* cfg) {
    demod->iq_dc_block_enable = cfg->iq_dc_block_is_set ? (cfg->iq_dc_block_enable != 0) : 0;
    demod->iq_dc_shift = cfg->iq_dc_shift_is_set ? cfg->iq_dc_shift : 11;
    demod->iq_dc_avg_r = demod->iq_dc_avg_i = 0;
    const char* iqb = getenv("DSD_NEO_IQ_BALANCE");
    if (iqb && *iqb) {
        int parsed = 0;
        demod->iqbal_enable = (dsd_parse_int_strict(iqb, 10, INT_MIN, INT_MAX, &parsed) == 0 && parsed != 0) ? 1 : 0;
    }
}

static void
demod_apply_channel_lpf_defaults(struct demod_state* demod, const dsd_opts* opts, const dsdneoRuntimeConfig* cfg) {
    int channel_lpf = 0;
    int profile = DSD_CH_LPF_PROFILE_WIDE;
    if (cfg->channel_lpf_is_set) {
        channel_lpf = (cfg->channel_lpf_enable != 0);
    } else if (demod->rate_in >= 20000) {
        channel_lpf = 1;
    }
    if (channel_lpf) {
        profile = opts_channel_profile_for_rate(opts, demod, demod->symbol_rate_hz);
    }
    demod->channel_lpf_enable = channel_lpf ? 1 : 0;
    demod->channel_lpf_profile = profile;
    if (demod->output_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK) {
        demod->channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_CQPSK;
    }
    fsk_modem_apply_config(demod);
}

static void
demod_finalize_runtime_profile(struct demod_state* demod, const dsd_opts* opts) {
    demod->channel_squelch_level = (float)opts->rtl_squelch_level;
    if (demod->output_kind == DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR) {
        demod->ted_enabled = 0;
    }
    fsk_modem_apply_config(demod);
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
    demod->cqpsk_enable = (opts->mod_qpsk == 1) ? 1 : 0;
    demod_apply_output_kind(demod, opts);
}

/**
 * @brief Apply environment/runtime overrides to the demodulator state.
 *
 * Mirrors CLI/env-driven configuration into the demodulator, covering DSP
 * toggles (FS/4 shift, combine-rotate), resampler targets, CQPSK path enable,
 * CQPSK timing gain, and IQ balance defaults. Early-exits on NULL inputs.
 *
 * @param demod Demodulator state to configure.
 * @param opts  Decoder options used for runtime flags.
 */
void
rtl_demod_config_from_env_and_opts(struct demod_state* demod, const dsd_opts* opts) {
    if (!demod || !opts) {
        return;
    }

    dsd_neo_config_init();
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        return;
    }

    demod_apply_runtime_global_flags(opts, cfg);
    demod_apply_resampler_target_defaults(demod, cfg);
    demod_apply_costas_defaults(demod, cfg);
    demod_apply_ted_defaults(demod, cfg);
    demod_apply_cqpsk_defaults(demod, opts, cfg);
    demod_apply_iq_defaults(demod, cfg);
    demod_apply_channel_lpf_defaults(demod, opts, cfg);
    demod_finalize_runtime_profile(demod, opts);
}

static int
rtl_demod_resolve_complex_rate(const struct demod_state* demod, const struct output_state* output) {
    int fs = demod->rate_out > 0 ? demod->rate_out : (int)output->rate;
    return (fs > 0) ? fs : 48000;
}

static int
rtl_demod_clamp_sps(int sps) {
    if (sps < 2) {
        return 2;
    }
    if (sps > 64) {
        return 64;
    }
    return sps;
}

static void
rtl_demod_log_non_integer_defaults(const struct demod_state* demod, int fs_cx, int sym_rate, int sps) {
    if (demod->cqpsk_enable) {
        LOG_WARN("WARNING: Non-integer SPS detected: %d Hz / %d sym/s = %.3f (rounded to %d). "
                 "CQPSK timing will continue at the rounded SPS. Use a DSP bandwidth that results in "
                 "integer SPS for optimal performance.\n",
                 fs_cx, sym_rate, (float)fs_cx / (float)sym_rate, sps);
        return;
    }
    LOG_WARN("WARNING: Non-integer SPS detected: %d Hz / %d sym/s = %.3f (rounded to %d). "
             "Symbol timing will use the rounded SPS. "
             "Use a DSP bandwidth that results in integer SPS for optimal performance.\n",
             fs_cx, sym_rate, (float)fs_cx / (float)sym_rate, sps);
}

static void
rtl_demod_apply_digital_default_tracking(struct demod_state* demod, const dsd_opts* opts,
                                         const struct output_state* output, int ted_gain_is_set) {
    int fs_cx = rtl_demod_resolve_complex_rate(demod, output);
    int sym_rate = opts_symbol_rate_hz(opts);
    if (fs_cx < (sym_rate * 2)) {
        LOG_WARN("WARNING: CQPSK timing SPS: demod rate %d Hz is low for ~%d sym/s; clamping to minimum SPS.\n", fs_cx,
                 sym_rate);
    }
    int sps = rtl_demod_clamp_sps((fs_cx + (sym_rate / 2)) / sym_rate);
    demod->ted_sps = sps;
    if ((fs_cx % sym_rate) == 0) {
        demod->sps_is_integer = 1;
    } else {
        demod->sps_is_integer = 0;
        rtl_demod_log_non_integer_defaults(demod, fs_cx, sym_rate, sps);
    }
    if (!ted_gain_is_set) {
        demod->ted_gain = 0.025f;
        demod->ted_effective_gain = demod->ted_gain;
    }
}

static void
rtl_demod_free_resampler_buffers(struct demod_state* demod) {
    if (demod->resamp_taps) {
        dsd_neo_aligned_free(demod->resamp_taps);
        demod->resamp_taps = NULL;
    }
    if (demod->resamp_hist) {
        dsd_neo_aligned_free(demod->resamp_hist);
        demod->resamp_hist = NULL;
    }
}

static void
rtl_demod_disable_resampler(struct demod_state* demod, int reset_ratio) {
    rtl_demod_free_resampler_buffers(demod);
    demod->resamp_enabled = 0;
    if (!reset_ratio) {
        return;
    }
    demod->resamp_L = 1;
    demod->resamp_M = 1;
    demod->resamp_phase = 0;
    demod->resamp_hist_head = 0;
}

static int
rtl_demod_should_skip_resampler(const struct demod_state* demod) {
    return (demod->output_kind == DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR
            || demod->output_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK);
}

static void
rtl_demod_compute_resampler_ratio(int inRate, int target, int* L, int* M, int* scale) {
    int g = gcd_int(inRate, target);
    *L = target / g;
    *M = inRate / g;
    if (*L < 1) {
        *L = 1;
    }
    if (*M < 1) {
        *M = 1;
    }
    *scale = (*M > 0) ? ((*L + *M - 1) / *M) : 1;
}

static int
rtl_demod_resampler_needs_reconfigure(const struct demod_state* demod, int L, int M) {
    return (!demod->resamp_enabled || demod->resamp_L != L || demod->resamp_M != M || demod->resamp_taps == NULL
            || demod->resamp_hist == NULL);
}

static void
rtl_demod_log_non_integer_after_rate_change(const struct demod_state* demod, int fs_cx, int sym_rate) {
    if (demod->cqpsk_enable) {
        LOG_WARN("WARNING: Non-integer SPS after rate change: %d Hz / %d sym/s = %.3f. "
                 "CQPSK timing continues at the rounded SPS.\n",
                 fs_cx, sym_rate, (float)fs_cx / (float)sym_rate);
        return;
    }
    LOG_WARN("WARNING: Non-integer SPS after rate change: %d Hz / %d sym/s = %.3f. "
             "Symbol timing will use the rounded SPS.\n",
             fs_cx, sym_rate, (float)fs_cx / (float)sym_rate);
}

/**
 * @brief Apply sane defaults for digital vs analog demodulation when unset.
 *
 * Populates CQPSK timing defaults and SPS based on the selected mode when the
 * user has not overridden settings via env/CLI. Relies on @p output for
 * effective rate.
 *
 * @param demod  Demodulator state to update.
 * @param opts   Decoder options (mode flags).
 * @param output Output ring used to infer sample rate.
 */
void
rtl_demod_select_defaults_for_mode(struct demod_state* demod, const dsd_opts* opts, const struct output_state* output) {
    if (!demod || !opts || !output) {
        return;
    }
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        return;
    }

    int ted_gain_is_set = (demod->ted_gain_is_set || cfg->ted_gain_is_set) ? 1 : 0;
    if (dsd_opts_has_digital_decode_mode(opts)) {
        rtl_demod_apply_digital_default_tracking(demod, opts, output, ted_gain_is_set);
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
    int inRate = demod->rate_out > 0 ? demod->rate_out : rtl_dsp_bw_hz;
    if (rtl_demod_should_skip_resampler(demod)) {
        rtl_demod_disable_resampler(demod, 0);
        output->rate = inRate;
        fsk_modem_apply_config(demod);
        return;
    }
    if (demod->resamp_target_hz <= 0) {
        rtl_demod_disable_resampler(demod, 0);
        output->rate = inRate;
        return;
    }
    int target = demod->resamp_target_hz;
    if (target == inRate) {
        rtl_demod_disable_resampler(demod, 1);
        output->rate = inRate;
        return;
    }
    int L = 1;
    int M = 1;
    int scale = 1;
    rtl_demod_compute_resampler_ratio(inRate, target, &L, &M, &scale);

    if (scale > 12) {
        rtl_demod_disable_resampler(demod, 0);
        output->rate = inRate;
        LOG_WARN("WARNING: Resampler ratio too large on retune (L=%d,M=%d). Disabled.\n", L, M);
        return;
    }

    if (rtl_demod_resampler_needs_reconfigure(demod, L, M)) {
        rtl_demod_free_resampler_buffers(demod);
        resamp_design(demod, L, M);
        demod->resamp_L = L;
        demod->resamp_M = M;
        demod->resamp_enabled = 1;
        LOG_INFO("Resampler reconfigured: %d -> %d Hz (L=%d,M=%d).\n", inRate, target, L, M);
    }
    output->rate = target;
}

/**
 * @brief Refresh CQPSK timing SPS after capture/output rate changes.
 *
 * Recompute the nominal samples-per-symbol from the current output rate and
 * mode unless an explicit CQPSK timing SPS override is active.
 *
 * @param demod  Demodulator state.
 * @param opts   Decoder options (mode flags).
 * @param output Output ring (for sink rate).
 */
void
rtl_demod_maybe_refresh_ted_sps_after_rate_change(struct demod_state* demod, const dsd_opts* opts,
                                                  const struct output_state* output) {
    if (!demod || !output) {
        return;
    }

    int Fs_cx = rtl_demod_resolve_complex_rate(demod, output);
    int sps = 0;
    int sym_rate = demod->symbol_rate_hz > 0 ? demod->symbol_rate_hz : 4800;
    if (opts) {
        /* When only P25P2/X2-TDMA is enabled (without P25P1), use 6000 sym/s.
         * When mod_qpsk is set for P25P1 CQPSK/LSM, use 4800 sym/s.
         * When both P25P1 and P25P2 are enabled (trunking mode), default to
         * P25P1 rate (4800) since CC is typically encountered first; the trunk
         * state machine will override via ted_sps_override when tuning to P25P2 VC. */
        sym_rate = opts_symbol_rate_hz(opts);
        if (opts->mod_qpsk == 1 && sym_rate != 6000) {
            sym_rate = 4800;
        }
        if (Fs_cx < (sym_rate * 2)) {
            LOG_WARN("WARNING: CQPSK timing SPS: demod rate %d Hz is low for ~%d sym/s; clamping to minimum SPS.\n",
                     Fs_cx, sym_rate);
        }
        sps = (Fs_cx + (sym_rate / 2)) / sym_rate;
        if ((Fs_cx % sym_rate) == 0) {
            demod->sps_is_integer = 1;
        } else {
            demod->sps_is_integer = 0;
            rtl_demod_log_non_integer_after_rate_change(demod, Fs_cx, sym_rate);
        }
    } else {
        sps = (Fs_cx + 2400) / 4800;
        demod->sps_is_integer = ((Fs_cx % 4800) == 0) ? 1 : 0;
        if (!demod->sps_is_integer) {
            rtl_demod_log_non_integer_after_rate_change(demod, Fs_cx, 4800);
        }
    }
    demod->symbol_rate_hz = sym_rate;
    demod->symbol_levels = opts_symbol_levels_for_rate(opts, sym_rate);
    sps = rtl_demod_clamp_sps(sps);
    if (demod->ted_sps_override > 0) {
        demod->ted_sps = demod->ted_sps_override;
    } else {
        demod->ted_sps = sps;
    }
    if (demod->cqpsk_enable) {
        demod->channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_CQPSK;
    }
    fsk_modem_apply_config(demod);
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
    dsd_cond_destroy(&demod->ready);
    dsd_mutex_destroy(&demod->ready_m);
    demod_mt_destroy(demod);
    dsd_fsk_modem_release(&demod->fsk_modem_state);
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
