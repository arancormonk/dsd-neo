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

#include <atomic>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/math_utils.h>
#include <dsd-neo/dsp/resampler.h>
#include <dsd-neo/dsp/ted.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/mem.h>
#include <dsd-neo/runtime/ring.h>
#include <dsd-neo/runtime/worker_pool.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/dsp/costas.h"
#include "dsd-neo/dsp/fsk_modem.h"
#include "dsd-neo/platform/threading.h"
#include "rtl_stream_mirrors.hpp"

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
    DSD_MEMSET(s->channel_lpf_hist_i, 0, sizeof(s->channel_lpf_hist_i));
    DSD_MEMSET(s->channel_lpf_hist_q, 0, sizeof(s->channel_lpf_hist_q));
    s->channel_pwr = 0.0f;
    g_channel_pwr.store(0.0f, std::memory_order_relaxed);
    s->channel_squelch_level.store(0.0f, std::memory_order_relaxed);
    s->channel_squelched = 0;
    s->audio_lpf_enable = 0;
    s->audio_lpf_alpha = 0.0f;
    s->audio_lpf_state = 0.0f;
    s->now_lpr = 0.0f;
    s->dc_block = 1;
    s->dc_avg = 0.0f;
    s->resamp_enabled = 0;
    s->digital_resample_mode = DSD_DIGITAL_RESAMPLE_AUTO;
    s->capture_rate_device_forced = 0;
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

/* The historical channel-LPF enable rule: DSD_NEO_CHANNEL_LPF when set, otherwise on from a 20 kHz rate_in. */
static int
demod_channel_lpf_default_enable(const struct demod_state* demod, const dsdneoRuntimeConfig* cfg) {
    if (cfg && cfg->channel_lpf_is_set) {
        return (cfg->channel_lpf_enable != 0) ? 1 : 0;
    }
    return (demod->rate_in >= 20000) ? 1 : 0;
}

static void
demod_apply_channel_lpf_defaults(struct demod_state* demod, const dsd_opts* opts, const dsdneoRuntimeConfig* cfg) {
    int channel_lpf = demod_channel_lpf_default_enable(demod, cfg);
    demod->channel_lpf_default_enable = channel_lpf;
    int profile = DSD_CH_LPF_PROFILE_WIDE;
    if (channel_lpf) {
        profile = opts_channel_profile_for_rate(opts, demod, demod->symbol_rate_hz);
    }
    demod->channel_lpf_enable = channel_lpf ? 1 : 0;
    demod->channel_lpf_profile = profile;
    if (demod->output_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK) {
        demod->channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_CQPSK;
    }
    demod->analog_family = 0;
    demod->analog_demod = DSD_ANALOG_DEMOD_FM;
    demod->channel_lpf_width_hz = 0;
    demod->analog_width_request_hz = 0;
    if (dsd_opts_is_analog_family(opts)) {
        /* Provisional: rate_out is not final yet. rtl_demod_finalize_analog_channel() settles it. */
        (void)rtl_demod_apply_analog_channel(demod, opts->analog_demod, dsd_opts_analog_width_hz(opts));
    }
    fsk_modem_apply_config(demod);
}

static void
demod_finalize_runtime_profile(struct demod_state* demod, const dsd_opts* opts) {
    demod->channel_squelch_level.store((float)opts->rtl_squelch_level, std::memory_order_relaxed);
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
    demod->digital_resample_mode = opts->digital_resample_mode;
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

int
rtl_demod_digital_resample_target_for(int output_kind, int digital_resample_mode, int resamp_target_hz,
                                      int symbol_rate_hz, int rate_out_hz, int capture_rate_device_forced) {
    /* Only the FSK discriminator stream is at sample rate. CQPSK output is already one value
       per symbol from the Gardner loop, which absorbs a fractional SPS on its own. */
    if (output_kind != DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR) {
        return 0;
    }
    if (digital_resample_mode == DSD_DIGITAL_RESAMPLE_OFF) {
        return 0;
    }
    const int target = resamp_target_hz;
    const int sym_rate = symbol_rate_hz > 0 ? symbol_rate_hz : 4800;
    const int in_rate = rate_out_hz;
    if (target <= 0 || in_rate <= 0 || target == in_rate) {
        return 0;
    }
    if ((target % sym_rate) != 0) {
        /* Resampling here would not buy an integer SPS. */
        return 0;
    }
    if (digital_resample_mode == DSD_DIGITAL_RESAMPLE_AUTO) {
        if ((in_rate % sym_rate) == 0) {
            return 0;
        }
        if (!capture_rate_device_forced) {
            /* The rate follows the requested DSP bandwidth, so leave the existing chain alone
               and let the non-integer SPS warning point the user at a better bandwidth. */
            return 0;
        }
    }
    return target;
}

int
rtl_demod_digital_resample_target_hz(const struct demod_state* demod) {
    if (!demod) {
        return 0;
    }
    return rtl_demod_digital_resample_target_for(demod->output_kind, demod->digital_resample_mode,
                                                 demod->resamp_target_hz, demod->symbol_rate_hz, demod->rate_out,
                                                 demod->capture_rate_device_forced);
}

static int
rtl_demod_should_skip_resampler(const struct demod_state* demod) {
    if (demod->output_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK) {
        return 1;
    }
    if (demod->output_kind == DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR) {
        return rtl_demod_digital_resample_target_hz(demod) <= 0;
    }
    return 0;
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
 * @brief Refresh timing SPS after capture/output rate changes.
 *
 * Recompute the nominal samples-per-symbol for the current output rate unless an explicit timing
 * SPS override is active. The symbol rate and level count come either from the profile the front
 * end is already on or from the decoder options, per @p preserve_active_profile.
 *
 * @param demod                   Demodulator state.
 * @param opts                    Decoder options (mode flags); may be NULL.
 * @param output                  Output ring (for sink rate).
 * @param preserve_active_profile Non-zero keeps the active symbol rate and level count (retunes);
 *                                zero derives them from @p opts (stream open).
 */
void
rtl_demod_maybe_refresh_ted_sps_after_rate_change(struct demod_state* demod, const dsd_opts* opts,
                                                  const struct output_state* output, int preserve_active_profile) {
    if (!demod || !output) {
        return;
    }

    int Fs_cx = rtl_demod_resolve_complex_rate(demod, output);
    int sym_rate;
    int sym_levels;
    if (preserve_active_profile) {
        /* Retunes keep whatever the SPS hunt, the trunking engine, or the operator last published
         * through rtl_stream_set_symbol_profile(); only the timing SPS follows the new rate. The
         * option flags cannot answer this: with more than one rate class enabled they always say
         * 4800/4, which would drag a run parked on 2400/4 (NXDN48, dPMR) or 9600/2 (ProVoice) off
         * its profile on every hop. Fall back to the option-derived default only when no profile
         * has been established yet. */
        sym_rate = demod->symbol_rate_hz > 0 ? demod->symbol_rate_hz : opts_symbol_rate_hz(opts);
        sym_levels = (demod->symbol_levels == 2 || demod->symbol_levels == 4)
                         ? demod->symbol_levels
                         : opts_symbol_levels_for_rate(opts, sym_rate);
    } else {
        /* When only P25P2/X2-TDMA is enabled (without P25P1), use 6000 sym/s.
         * When mod_qpsk is set for P25P1 CQPSK/LSM, use 4800 sym/s.
         * When both P25P1 and P25P2 are enabled (trunking mode), default to
         * P25P1 rate (4800) since CC is typically encountered first; the trunk
         * state machine will override via ted_sps_override when tuning to P25P2 VC. */
        sym_rate = opts_symbol_rate_hz(opts);
        if (opts && opts->mod_qpsk == 1 && sym_rate != 6000) {
            sym_rate = 4800;
        }
        sym_levels = opts_symbol_levels_for_rate(opts, sym_rate);
    }
    if (Fs_cx < (sym_rate * 2)) {
        LOG_WARN("WARNING: CQPSK timing SPS: demod rate %d Hz is low for ~%d sym/s; clamping to minimum SPS.\n", Fs_cx,
                 sym_rate);
    }
    int sps = (Fs_cx + (sym_rate / 2)) / sym_rate;
    if ((Fs_cx % sym_rate) == 0) {
        demod->sps_is_integer = 1;
    } else {
        demod->sps_is_integer = 0;
        rtl_demod_log_non_integer_after_rate_change(demod, Fs_cx, sym_rate);
    }
    demod->symbol_rate_hz = sym_rate;
    demod->symbol_levels = sym_levels;
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

/* ---------------- Analog receive family ---------------- */

static const double kAudioFilterPi = 3.14159265358979323846;

static void
demod_error_text(char* err, size_t err_size, const char* text) {
    if (err && err_size > 0U) {
        DSD_SNPRINTF(err, err_size, "%s", text);
    }
}

int
rtl_demod_analog_requested_width_hz(int kind, int explicit_width_hz) {
    if (explicit_width_hz > 0) {
        return explicit_width_hz;
    }
    return kind == DSD_ANALOG_DEMOD_FM ? 0 : dsd_analog_width_default_hz(kind);
}

int
rtl_demod_check_analog_channel(int kind, int explicit_width_hz, int rate_hz, char* err, size_t err_size) {
    demod_error_text(err, err_size, "");
    if (!dsd_analog_demod_is_valid(kind)) {
        demod_error_text(err, err_size, "unknown analog demodulator");
        return -1;
    }
    if (kind == DSD_ANALOG_DEMOD_AM) {
        demod_error_text(err, err_size,
                         "AM reception is not available on the radio front end yet; the analog monitor demodulates "
                         "NFM only");
        return -1;
    }
    if (explicit_width_hz <= 0 && kind == DSD_ANALOG_DEMOD_FM) {
        /* The unset NFM default keeps the historical filter behaviour at every rate. */
        return 0;
    }
    /* Every other width is a request for that filter, including the unset AM default: AM is new, so its default is
       held to the same rules as an explicit width. */
    const int width_hz = rtl_demod_analog_requested_width_hz(kind, explicit_width_hz);
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg && cfg->channel_lpf_is_set && cfg->channel_lpf_enable == 0) {
        if (err && err_size > 0U && explicit_width_hz > 0) {
            char width_text[DSD_ANALOG_WIDTH_TEXT_MAX];
            (void)dsd_analog_width_format(explicit_width_hz, width_text, sizeof width_text);
            DSD_SNPRINTF(err, err_size,
                         "%s bandwidth %s needs the channel filter, but DSD_NEO_CHANNEL_LPF=0 turns it off; unset "
                         "DSD_NEO_CHANNEL_LPF or drop the explicit bandwidth",
                         dsd_analog_demod_label(kind), width_text);
        } else if (err && err_size > 0U) {
            DSD_SNPRINTF(err, err_size,
                         "%s reception needs the channel filter, but DSD_NEO_CHANNEL_LPF=0 turns it off; unset "
                         "DSD_NEO_CHANNEL_LPF",
                         dsd_analog_demod_label(kind));
        }
        return -1;
    }
    if (rate_hz <= 0 && dsd_analog_width_in_range(kind, width_hz)) {
        /* No DSP rate yet (no stream running): the next stream open checks the width against the rate it delivers. */
        return 0;
    }
    return dsd_analog_width_check(kind, width_hz, rate_hz, err, err_size);
}

int
rtl_demod_check_analog_post_decimation(int kind, int explicit_width_hz, int rate_out_hz, int post_downsample, char* err,
                                       size_t err_size) {
    demod_error_text(err, err_size, "");
    if (post_downsample <= 1 || (explicit_width_hz <= 0 && kind == DSD_ANALOG_DEMOD_FM)) {
        /* No post-demod decimation, or the unset NFM default, which keeps the legacy design at every rate chain. */
        return 0;
    }
    if (err && err_size > 0U) {
        char width_text[DSD_ANALOG_WIDTH_TEXT_MAX];
        (void)dsd_analog_width_format(rtl_demod_analog_requested_width_hz(kind, explicit_width_hz), width_text,
                                      sizeof width_text);
        DSD_SNPRINTF(err, err_size,
                     "%s bandwidth %s cannot be applied to this I/Q replay: post_downsample %d runs the channel filter "
                     "at %d Hz, not the %d Hz demod rate; drop the explicit bandwidth or use a capture with "
                     "post_downsample 1",
                     dsd_analog_demod_label(kind), width_text, post_downsample, rate_out_hz * post_downsample,
                     rate_out_hz);
    }
    return -1;
}

int
rtl_demod_apply_analog_channel(struct demod_state* demod, int kind, int explicit_width_hz) {
    if (!demod) {
        return 0;
    }
    const int prev_family = demod->analog_family;
    const int prev_width = demod->channel_lpf_width_hz;
    const int prev_enable = demod->channel_lpf_enable;
    const int prev_profile = demod->channel_lpf_profile;
    const int analog_kind = dsd_analog_demod_is_valid(kind) ? kind : DSD_ANALOG_DEMOD_FM;
    const int requested_hz = rtl_demod_analog_requested_width_hz(analog_kind, explicit_width_hz);
    const int width_hz = dsd_analog_width_effective_hz(analog_kind, requested_hz);

    demod->analog_family = 1;
    demod->analog_demod = analog_kind;
    demod->analog_width_request_hz = requested_hz;
    demod->channel_lpf_profile = DSD_CH_LPF_PROFILE_WIDE;
    if (requested_hz > 0) {
        /* A requested width (explicit, or the AM default) is a request for that filter: it turns the channel LPF on. */
        demod->channel_lpf_enable = 1;
        demod->channel_lpf_width_hz = width_hz;
    } else {
        /* The unset NFM default keeps the enable decision stream configuration made, and where the rate cannot fit
           the default width the legacy WIDE design (width 0) stays in charge rather than failing the stream. */
        demod->channel_lpf_enable = demod->channel_lpf_default_enable;
        demod->channel_lpf_width_hz = dsd_analog_width_realizable(width_hz, demod->rate_out) ? width_hz : 0;
    }
    return (prev_family != 1 || prev_width != demod->channel_lpf_width_hz || prev_enable != demod->channel_lpf_enable
            || prev_profile != demod->channel_lpf_profile)
               ? 1
               : 0;
}

int
rtl_demod_refresh_analog_channel_for_rate(struct demod_state* demod, char* err, size_t err_size) {
    demod_error_text(err, err_size, "");
    /* The analog monitor output on the analog channel, not the family flag: CQPSK toggled on under -fA, or a typed
       digital scan row's symbol profile, keeps the analog family but runs its own profile filter, which re-applying
       the analog channel would replace with WIDE. */
    if (!dsd_demod_analog_monitor_active(demod)) {
        return 0;
    }
    const int explicit_width_hz = demod->analog_width_request_hz;
    int rc = 0;
    if (explicit_width_hz > 0
        && dsd_analog_width_check(demod->analog_demod, explicit_width_hz, demod->rate_out, err, err_size) != 0) {
        /* Kept as requested: never clamped and never swapped for another design. */
        rc = -1;
    }
    (void)rtl_demod_apply_analog_channel(demod, demod->analog_demod, explicit_width_hz);
    return rc;
}

int
rtl_demod_finalize_analog_channel(struct demod_state* demod, const dsd_opts* opts, char* err, size_t err_size) {
    demod_error_text(err, err_size, "");
    if (!demod || !opts) {
        return -1;
    }
    if (!dsd_opts_is_analog_family(opts)) {
        return 0;
    }
    const int kind = opts->analog_demod;
    const int explicit_width_hz = dsd_opts_analog_width_hz(opts);
    if (rtl_demod_check_analog_channel(kind, explicit_width_hz, demod->rate_out, err, err_size) != 0
        || rtl_demod_check_analog_post_decimation(kind, explicit_width_hz, demod->rate_out, demod->post_downsample, err,
                                                  err_size)
               != 0) {
        return -1;
    }
    (void)rtl_demod_apply_analog_channel(demod, kind, explicit_width_hz);
    return 0;
}

static int
demod_deemph_tau_us_from_config(const dsdneoRuntimeConfig* cfg) {
    if (!cfg || !cfg->deemph_is_set) {
        return 75;
    }
    switch (cfg->deemph_mode) {
        case DSD_NEO_DEEMPH_OFF: return 0;
        case DSD_NEO_DEEMPH_50: return 50;
        case DSD_NEO_DEEMPH_NFM: return 750;
        default: return 75;
    }
}

int
rtl_demod_apply_audio_filters_from_config(struct demod_state* demod) {
    if (!demod) {
        return 0;
    }
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    demod->deemph_tau_us = 0;
    if (demod->deemph) {
        demod->deemph_tau_us = demod_deemph_tau_us_from_config(cfg);
        if (demod->deemph_tau_us <= 0) {
            demod->deemph = 0;
        }
    }
    demod->audio_lpf_enable = 0;
    demod->audio_lpf_alpha = 0.0f;
    demod->audio_lpf_state = 0.0f;
    demod->audio_lpf_cutoff_hz = 0;
    if (cfg && cfg->audio_lpf_is_set && !cfg->audio_lpf_disable && cfg->audio_lpf_cutoff_hz > 0) {
        demod->audio_lpf_cutoff_hz = cfg->audio_lpf_cutoff_hz < 100 ? 100 : cfg->audio_lpf_cutoff_hz;
        demod->audio_lpf_enable = 1;
    }
    rtl_demod_refresh_audio_coefficients(demod);
    return demod->audio_lpf_enable;
}

void
rtl_demod_refresh_audio_coefficients(struct demod_state* demod) {
    if (!demod) {
        return;
    }
    double fs = (double)demod->rate_out;
    if (fs < 1.0) {
        fs = 1.0;
    }
    if (demod->deemph && demod->deemph_tau_us > 0) {
        const double tau_s = (double)demod->deemph_tau_us / 1000000.0;
        const double a = exp(-1.0 / (fs * tau_s));
        const double alpha = 1.0 - a;
        int coef_q15 = (int)lrint(alpha * (double)(1 << 15));
        if (coef_q15 < 1) {
            coef_q15 = 1;
        } else if (coef_q15 > ((1 << 15) - 1)) {
            coef_q15 = ((1 << 15) - 1);
        }
        demod->deemph_a = (float)((double)coef_q15 / (double)(1 << 15));
    }
    if (demod->audio_lpf_enable && demod->audio_lpf_cutoff_hz > 0) {
        double a = 1.0 - exp(-2.0 * kAudioFilterPi * (double)demod->audio_lpf_cutoff_hz / fs);
        if (a < 0.0) {
            a = 0.0;
        }
        if (a > 1.0) {
            a = 1.0;
        }
        demod->audio_lpf_alpha = (float)a;
    }
}

void
rtl_demod_reset_audio_monitor_state(struct demod_state* demod) {
    if (!demod) {
        return;
    }
    demod->deemph_avg = 0.0f;
    demod->dc_avg = 0.0f;
    demod->audio_lpf_state = 0.0f;
    /* Fresh-open envelope: open, so the first block of a live channel is not faded in from the last one's squelch. */
    demod->squelch_env = 1.0f;
    demod->squelch_gate_open = 1;
    demod->now_lpr = 0.0f;
    demod->prev_lpr_index = 0;
    demod->fm_demod_history_valid = 0;
    demod->pre_r = 0.0f;
    demod->pre_j = 0.0f;
}

void
rtl_demod_clear_filter_histories(struct demod_state* demod) {
    if (!demod) {
        return;
    }
    for (int st = 0; st < 10; st++) {
        DSD_MEMSET(demod->hb_hist_i[st], 0, sizeof(demod->hb_hist_i[st]));
        DSD_MEMSET(demod->hb_hist_q[st], 0, sizeof(demod->hb_hist_q[st]));
    }
    DSD_MEMSET(demod->channel_lpf_hist_i, 0, sizeof(demod->channel_lpf_hist_i));
    DSD_MEMSET(demod->channel_lpf_hist_q, 0, sizeof(demod->channel_lpf_hist_q));
    demod->channel_lpf_hist_len = 0;
}

void
rtl_demod_reset_resampler_state(struct demod_state* demod) {
    if (!demod) {
        return;
    }
    demod->resamp_phase = 0;
    demod->resamp_hist_head = 0;
    if (demod->resamp_hist && demod->resamp_taps_per_phase > 0) {
        DSD_MEMSET(demod->resamp_hist, 0, (size_t)demod->resamp_taps_per_phase * 2U * sizeof(float));
    }
}

/* A fresh open starts the carrier and timing loops from nothing: Costas and the band-edge FLL at zero frequency and
 * phase with empty delay lines, the Gardner TED uninitialised (it seeds itself from the SPS on its first block), and
 * the CQPSK differential and AGC state at their open values. A family switch carries none of the old session's
 * loop state over. */
static void
demod_family_switch_reset_loops(struct demod_state* demod) {
    ted_init_state(&demod->ted_state);
    demod->ted_mu = 0.0f;
    dsd_costas_loop_state_t* cl = &demod->costas_state;
    cl->phase = 0.0f;
    cl->freq = 0.0f;
    cl->error = 0.0f;
    cl->error_smooth = 0.0f;
    cl->initialized = 0;
    demod->costas_err_avg_q14 = 0;
    demod->costas_err_raw_avg_q14 = 0;
    demod->costas_conf_avg_q14 = 0;
    demod->costas_zero_conf_pct = 0;
    demod->costas_reset_pending = 0;
    dsd_fll_band_edge_state_t* fll = &demod->fll_band_edge_state;
    fll->freq = 0.0f;
    fll->phase = 0.0f;
    fll->delay_idx = 0;
    DSD_MEMSET(fll->delay_r, 0, sizeof(fll->delay_r));
    DSD_MEMSET(fll->delay_i, 0, sizeof(fll->delay_i));
    demod->cqpsk_diff_prev_r = 1.0f;
    demod->cqpsk_diff_prev_j = 0.0f;
    demod->cqpsk_agc_avg = 1.0f;
}

/* The input corrections and the post-demod decimator an open starts from nothing: the I/Q DC blocker and I/Q balance
 * estimates, and a replay's post-demod decimator (its delay line holds the old family's demodulated samples). */
static void
demod_family_switch_reset_input_and_decimator(struct demod_state* demod) {
    demod->iq_dc_avg_r = 0.0f;
    demod->iq_dc_avg_i = 0.0f;
    demod->iqbal_alpha_ema_r = 0.0f;
    demod->iqbal_alpha_ema_i = 0.0f;
    if (demod->post_polydecim_hist && demod->post_polydecim_K > 0) {
        DSD_MEMSET(demod->post_polydecim_hist, 0, (size_t)demod->post_polydecim_K * sizeof(float));
    }
    demod->post_polydecim_hist_head = 0;
    demod->post_polydecim_phase = 0;
}

static void
demod_family_switch_reset(struct demod_state* demod) {
    rtl_demod_reset_audio_monitor_state(demod);
    /* The squelch dwell an open starts from: the old family's run of squelched blocks must not count toward a
       multi-frequency hop on the new family's first squelched block. */
    demod->squelch_hits = 0;
    demod->squelch_running_power = 0;
    demod->squelch_decim_phase = 0;
    rtl_demod_clear_filter_histories(demod);
    rtl_demod_reset_resampler_state(demod);
    demod_family_switch_reset_input_and_decimator(demod);
    demod_family_switch_reset_loops(demod);
    /* Force a fresh channel-filter plan for the new family. */
    demod->channel_lpf_plan_taps_len = 0;
    demod->channel_lpf_plan_width_hz = 0;
}

int
rtl_demod_set_analog_kind(struct demod_state* demod, int kind) {
    if (!demod || !dsd_analog_demod_is_valid(kind)) {
        return -1;
    }
    if (demod->analog_family && demod->analog_demod == kind) {
        return 0;
    }
    demod->analog_demod = kind;
    /* rtl_demod_check_analog_channel() refuses AM before a stream gets here, so no AM demodulator is installed yet. */
    demod->mode_demod = &dsd_fm_demod;
    /* FM keeps the configured de-emphasis; AM runs without it. */
    demod->deemph = (kind == DSD_ANALOG_DEMOD_FM) ? 1 : 0;
    (void)rtl_demod_apply_audio_filters_from_config(demod);
    rtl_demod_reset_audio_monitor_state(demod);
    return 1;
}

void
rtl_demod_enter_analog_family(struct demod_state* demod, struct output_state* output, int kind, int explicit_width_hz,
                              int rtl_dsp_bw_hz) {
    if (!demod || !output) {
        return;
    }
    demod->cqpsk_enable = 0;
    demod->output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    demod->ted_enabled = 0;
    demod->ted_sps_override = 0;
    /* A fresh -fA open has no digital decoder enabled, so it derives the 4800/4 default symbol profile. */
    demod->symbol_rate_hz = 4800;
    demod->symbol_levels = 4;
    demod->analog_family = 0;
    (void)rtl_demod_set_analog_kind(demod, kind);
    (void)rtl_demod_apply_analog_channel(demod, kind, explicit_width_hz);
    rtl_demod_maybe_update_resampler_after_rate_change(demod, output, rtl_dsp_bw_hz);
    rtl_demod_maybe_refresh_ted_sps_after_rate_change(demod, NULL, output, /*preserve_active_profile=*/1);
    demod_family_switch_reset(demod);
}

void
rtl_demod_enter_digital_family(struct demod_state* demod, struct output_state* output, int channel_profile,
                               int cqpsk_enable, int symbol_rate_hz, int rtl_dsp_bw_hz) {
    if (!demod || !output) {
        return;
    }
    demod->analog_family = 0;
    demod->analog_demod = DSD_ANALOG_DEMOD_FM;
    demod->channel_lpf_width_hz = 0;
    demod->analog_width_request_hz = 0;
    demod->cqpsk_enable = 0;
    demod->ted_enabled = 0;
    demod->mode_demod = &dsd_fm_demod;
    /* Digital opens never run de-emphasis, and never compute its coefficient. */
    demod->deemph = 0;
    demod->deemph_tau_us = 0;
    demod->deemph_a = 0.0f;
    demod->channel_lpf_enable = demod->channel_lpf_default_enable;
    demod->channel_lpf_profile =
        (channel_profile >= DSD_CH_LPF_PROFILE_WIDE && channel_profile <= DSD_CH_LPF_PROFILE_P25_CQPSK)
            ? channel_profile
            : DSD_CH_LPF_PROFILE_WIDE;
    if (symbol_rate_hz > 0) {
        demod->symbol_rate_hz = symbol_rate_hz;
    }
    /* The integer-SPS flag an open derives from the complex demod rate and the symbol rate it runs. The analog family's
       flag was decided for the monitor's 4800 sym/s placeholder: at a forced 60000 Hz that is 12.5 samples per symbol,
       where a 2400 sym/s profile gets a whole 25. */
    const int fs_cx = rtl_demod_resolve_complex_rate(demod, output);
    demod->sps_is_integer = (demod->symbol_rate_hz > 0 && (fs_cx % demod->symbol_rate_hz) == 0) ? 1 : 0;
    /* The digital resampler depends on the symbol profile the family lands on, exactly as a fresh open decides it:
       CQPSK symbols are never resampled, and the FSK discriminator stream only when that profile's symbol rate needs
       it at a forced rate. It is decided here, once, so the output rate the caller commits is already final; the
       analog monitor's 4800 sym/s placeholder would pick the wrong chain at forced rates such as 78125 or 60000 Hz.
       The family itself lands on the FSK discriminator; the CQPSK toggle that follows the switch changes the output
       kind. */
    demod->output_kind = cqpsk_enable > 0 ? DSD_DEMOD_OUTPUT_SYMBOL_CQPSK : DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
    rtl_demod_maybe_update_resampler_after_rate_change(demod, output, rtl_dsp_bw_hz);
    demod->output_kind = DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
    if (!demod->resamp_enabled) {
        /* Match a fresh digital open, which never configured the monitor's ratio. */
        rtl_demod_disable_resampler(demod, 1);
    }
    fsk_modem_apply_config(demod);
    dsd_fsk_modem_reset(&demod->fsk_modem_state);
    demod_family_switch_reset(demod);
}

int
rtl_demod_monitor_output_rate_for(int resamp_target_hz, int rate_out_hz) {
    if (rate_out_hz <= 0) {
        return 0;
    }
    if (resamp_target_hz <= 0 || resamp_target_hz == rate_out_hz) {
        return rate_out_hz;
    }
    int L = 1;
    int M = 1;
    int scale = 1;
    rtl_demod_compute_resampler_ratio(rate_out_hz, resamp_target_hz, &L, &M, &scale);
    return (scale > 12) ? rate_out_hz : resamp_target_hz;
}
