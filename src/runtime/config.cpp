// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime configuration parser for environment-derived settings.
 *
 * Parses environment variables into a typed `dsd-neoRuntimeConfig` and exposes
 * an immutable accessor. Intended to be called early during application init.
 */

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/runtime/config.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static dsdneoRuntimeConfig g_config;
static int g_config_inited = 0;

/**
 * @brief Check whether an environment string is set and non-empty.
 *
 * @param v Environment value string pointer (may be NULL).
 * @return 1 if set and non-empty; otherwise 0.
 */
static int
env_is_set(const char* v) {
    return v && v[0] != '\0';
}

/**
 * @brief Convert environment string to integer with fallback.
 *
 * @param v Environment value string (may be NULL or empty).
 * @param fallback Fallback integer when `v` is unset or empty.
 * @return Parsed integer value or `fallback` when not set.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static int
env_as_int(const char* v, int fallback) {
    return env_is_set(v) ? atoi(v) : fallback;
}

/**
 * @brief Parse environment variables and initialize the runtime configuration.
 *
 * Precedence note: future CLI/opts may override env values; currently opts
 * are not applied beyond presence for future extension.
 *
 * @param opts Decoder options for potential precedence overrides.
 * @note Safe to call multiple times; the most recent call wins.
 */
void
dsd_neo_config_init(const dsd_opts* opts) {
    (void)opts; /* precedence hook reserved for future CLI/opts overrides */

    dsdneoRuntimeConfig c;
    memset(&c, 0, sizeof(c));

    /* HB_DECIM */
    const char* hb = getenv("DSD_NEO_HB_DECIM");
    c.hb_decim_is_set = env_is_set(hb);
    c.hb_decim = c.hb_decim_is_set ? (atoi(hb) != 0) : 1;

    /* COMBINE_ROT */
    const char* cr = getenv("DSD_NEO_COMBINE_ROT");
    c.combine_rot_is_set = env_is_set(cr);
    c.combine_rot = c.combine_rot_is_set ? (atoi(cr) != 0) : 1;

    /* UPSAMPLE_FP */
    const char* ufp = getenv("DSD_NEO_UPSAMPLE_FP");
    c.upsample_fp_is_set = env_is_set(ufp);
    c.upsample_fp = c.upsample_fp_is_set ? (atoi(ufp) != 0) : 1;

    /* RESAMP */
    const char* rs = getenv("DSD_NEO_RESAMP");
    c.resamp_is_set = env_is_set(rs);
    c.resamp_disable = 0;
    c.resamp_target_hz = 48000;
    if (c.resamp_is_set) {
        if (strcasecmp(rs, "off") == 0 || strcmp(rs, "0") == 0) {
            c.resamp_disable = 1;
        } else {
            int v = atoi(rs);
            if (v > 0) {
                c.resamp_target_hz = v;
            }
        }
    }

    /* FLL */
    const char* fll = getenv("DSD_NEO_FLL");
    c.fll_is_set = env_is_set(fll);
    c.fll_enable = (c.fll_is_set && fll[0] == '1') ? 1 : 0; /* may be overridden by mode later */

    const char* fa = getenv("DSD_NEO_FLL_ALPHA");
    const char* fb = getenv("DSD_NEO_FLL_BETA");
    const char* fdb = getenv("DSD_NEO_FLL_DEADBAND");
    const char* fsl = getenv("DSD_NEO_FLL_SLEW");
    c.fll_alpha_is_set = env_is_set(fa);
    c.fll_beta_is_set = env_is_set(fb);
    c.fll_deadband_is_set = env_is_set(fdb);
    c.fll_slew_is_set = env_is_set(fsl);
    /* Native float FLL parameters (GNU Radio style):
     * alpha: proportional gain (typ 0.001-0.01)
     * beta: integral gain (typ 0.0001-0.001)
     * deadband: minimum error threshold (typ 0.001-0.01)
     * slew_max: max freq change per sample in rad/sample */
    c.fll_alpha = c.fll_alpha_is_set ? (float)atof(fa) : 0.005f;
    c.fll_beta = c.fll_beta_is_set ? (float)atof(fb) : 0.0005f;
    c.fll_deadband = c.fll_deadband_is_set ? (float)atof(fdb) : 0.003f;
    c.fll_slew_max = c.fll_slew_is_set ? (float)atof(fsl) : 0.002f;

    /* CQPSK Costas loop (carrier recovery) using GNU Radio control loop */
    const char* cbw = getenv("DSD_NEO_COSTAS_BW");
    const char* cdp = getenv("DSD_NEO_COSTAS_DAMPING");
    c.costas_bw_is_set = env_is_set(cbw);
    c.costas_damping_is_set = env_is_set(cdp);
    c.costas_loop_bw = c.costas_bw_is_set ? atof(cbw) : dsd_neo_costas_default_loop_bw();
    c.costas_damping = c.costas_damping_is_set ? atof(cdp) : dsd_neo_costas_default_damping();

    /* TED - native float Gardner timing gain */
    const char* ted = getenv("DSD_NEO_TED");
    const char* tg = getenv("DSD_NEO_TED_GAIN");
    const char* ts = getenv("DSD_NEO_TED_SPS");
    const char* tf = getenv("DSD_NEO_TED_FORCE");
    c.ted_is_set = env_is_set(ted);
    c.ted_enable = (c.ted_is_set && ted[0] == '1') ? 1 : 0;
    c.ted_gain_is_set = env_is_set(tg);
    c.ted_gain = c.ted_gain_is_set ? (float)atof(tg) : 0.05f;
    c.ted_sps_is_set = env_is_set(ts);
    c.ted_sps = c.ted_sps_is_set ? atoi(ts) : 10;
    c.ted_force_is_set = env_is_set(tf);
    c.ted_force = (c.ted_force_is_set && tf[0] == '1') ? 1 : 0;

    /* C4FM clock assist */
    const char* clk = getenv("DSD_NEO_C4FM_CLK");
    const char* clk_sync = getenv("DSD_NEO_C4FM_CLK_SYNC");
    c.c4fm_clk_is_set = env_is_set(clk);
    c.c4fm_clk_mode = 0;
    if (c.c4fm_clk_is_set && clk && clk[0] != '\0') {
        if (strcasecmp(clk, "off") == 0 || strcmp(clk, "0") == 0) {
            c.c4fm_clk_mode = 0;
        } else if (strcasecmp(clk, "el") == 0 || strcmp(clk, "1") == 0) {
            c.c4fm_clk_mode = 1;
        } else if (strcasecmp(clk, "mm") == 0 || strcmp(clk, "2") == 0) {
            c.c4fm_clk_mode = 2;
        } else {
            /* Unrecognized → keep off */
            c.c4fm_clk_mode = 0;
        }
    }
    c.c4fm_clk_sync_is_set = env_is_set(clk_sync);
    c.c4fm_clk_sync = c.c4fm_clk_sync_is_set ? ((clk_sync[0] == '1') ? 1 : 0) : 0;

    /* Deemphasis */
    const char* deemph = getenv("DSD_NEO_DEEMPH");
    c.deemph_is_set = env_is_set(deemph);
    c.deemph_mode = DSD_NEO_DEEMPH_UNSET;
    if (c.deemph_is_set) {
        if (strcasecmp(deemph, "off") == 0 || strcmp(deemph, "0") == 0) {
            c.deemph_mode = DSD_NEO_DEEMPH_OFF;
        } else if (strcmp(deemph, "50") == 0) {
            c.deemph_mode = DSD_NEO_DEEMPH_50;
        } else if (strcmp(deemph, "75") == 0) {
            c.deemph_mode = DSD_NEO_DEEMPH_75;
        } else if (strcasecmp(deemph, "nfm") == 0) {
            c.deemph_mode = DSD_NEO_DEEMPH_NFM;
        }
    }

    /* Audio LPF */
    const char* alpf = getenv("DSD_NEO_AUDIO_LPF");
    c.audio_lpf_is_set = env_is_set(alpf);
    c.audio_lpf_disable = 0;
    c.audio_lpf_cutoff_hz = 0;
    if (c.audio_lpf_is_set) {
        if (strcasecmp(alpf, "off") == 0 || strcmp(alpf, "0") == 0) {
            c.audio_lpf_disable = 1;
        } else {
            int cutoff = atoi(alpf);
            if (cutoff > 0) {
                c.audio_lpf_cutoff_hz = cutoff;
            }
        }
    }

    /* MT (intra-block worker pool) */
    const char* mt = getenv("DSD_NEO_MT");
    c.mt_is_set = env_is_set(mt);
    c.mt_enable = (c.mt_is_set && mt[0] == '1') ? 1 : 0;

    /* Disable fs/4 capture shift */
    const char* dfs4 = getenv("DSD_NEO_DISABLE_FS4_SHIFT");
    c.fs4_shift_disable_is_set = env_is_set(dfs4);
    c.fs4_shift_disable = (c.fs4_shift_disable_is_set && dfs4[0] == '1') ? 1 : 0;

    /* Output clear/drain on retune */
    const char* clr = getenv("DSD_NEO_OUTPUT_CLEAR_ON_RETUNE");
    const char* dms = getenv("DSD_NEO_RETUNE_DRAIN_MS");
    c.output_clear_on_retune_is_set = env_is_set(clr);
    c.output_clear_on_retune = c.output_clear_on_retune_is_set ? (atoi(clr) != 0) : 0;
    c.retune_drain_ms_is_set = env_is_set(dms);
    c.retune_drain_ms = c.retune_drain_ms_is_set ? atoi(dms) : 50;

    /* Symbol window freeze for A/B testing */
    const char* wf = getenv("DSD_NEO_WINDOW_FREEZE");
    c.window_freeze_is_set = env_is_set(wf);
    c.window_freeze = c.window_freeze_is_set ? (atoi(wf) != 0) : 0;

    /* Optional JSON emitter for P25 PDUs */
    const char* pj = getenv("DSD_NEO_PDU_JSON");
    c.pdu_json_is_set = env_is_set(pj);
    c.pdu_json_enable = c.pdu_json_is_set ? (atoi(pj) != 0) : 0;

    /* Optional SNR-based digital squelch threshold (dB) */
    const char* snrsql = getenv("DSD_NEO_SNR_SQL_DB");
    c.snr_sql_is_set = env_is_set(snrsql);
    c.snr_sql_db = c.snr_sql_is_set ? atoi(snrsql) : 0;

    /* FM/C4FM amplitude AGC (pre-discriminator) */
    const char* fm_agc = getenv("DSD_NEO_FM_AGC");
    c.fm_agc_is_set = env_is_set(fm_agc);
    c.fm_agc_enable = c.fm_agc_is_set ? (atoi(fm_agc) != 0) : 0; /* default off unless overridden */

    const char* fm_tgt = getenv("DSD_NEO_FM_AGC_TARGET");
    c.fm_agc_target_is_set = env_is_set(fm_tgt);
    c.fm_agc_target_rms = c.fm_agc_target_is_set ? (float)atof(fm_tgt) : 0.30f;

    const char* fm_min = getenv("DSD_NEO_FM_AGC_MIN");
    c.fm_agc_min_is_set = env_is_set(fm_min);
    c.fm_agc_min_rms = c.fm_agc_min_is_set ? (float)atof(fm_min) : 0.06f;

    const char* fm_au = getenv("DSD_NEO_FM_AGC_ALPHA_UP");
    c.fm_agc_alpha_up_is_set = env_is_set(fm_au);
    c.fm_agc_alpha_up = c.fm_agc_alpha_up_is_set ? (float)atof(fm_au) : 0.25f; /* ~0.25 */

    const char* fm_ad = getenv("DSD_NEO_FM_AGC_ALPHA_DOWN");
    c.fm_agc_alpha_down_is_set = env_is_set(fm_ad);
    c.fm_agc_alpha_down = c.fm_agc_alpha_down_is_set ? (float)atof(fm_ad) : 0.75f; /* ~0.75 */

    /* FM constant-envelope limiter */
    const char* fml = getenv("DSD_NEO_FM_LIMITER");
    c.fm_limiter_is_set = env_is_set(fml);
    c.fm_limiter_enable = c.fm_limiter_is_set ? (atoi(fml) != 0) : 0;

    /* Complex DC blocker */
    const char* dcb = getenv("DSD_NEO_IQ_DC_BLOCK");
    const char* dck = getenv("DSD_NEO_IQ_DC_SHIFT");
    c.iq_dc_block_is_set = env_is_set(dcb);
    c.iq_dc_block_enable = c.iq_dc_block_is_set ? (atoi(dcb) != 0) : 0;
    c.iq_dc_shift_is_set = env_is_set(dck);
    c.iq_dc_shift = c.iq_dc_shift_is_set ? atoi(dck) : 11;

    /* Channel complex low-pass on RTL baseband (post-HB).
       Default: off for digital voice modes at 24 kHz; may be enabled via env. */
    const char* clpf = getenv("DSD_NEO_CHANNEL_LPF");
    c.channel_lpf_is_set = env_is_set(clpf);
    c.channel_lpf_enable = c.channel_lpf_is_set ? atoi(clpf) : 0;

    g_config = c;
    g_config_inited = 1;
}

/**
 * @brief Get immutable pointer to the current runtime configuration, or NULL if
 * initialization has not been performed.
 *
 * @return Pointer to config or NULL.
 */
const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    return g_config_inited ? &g_config : NULL;
}

/* Runtime control for C4FM clock assist (0=off, 1=EL, 2=MM). */
extern "C" void
dsd_neo_set_c4fm_clk(int mode) {
    if (!g_config_inited) {
        memset(&g_config, 0, sizeof(g_config));
        g_config_inited = 1;
    }
    if (mode >= 0) {
        if (mode > 2) {
            mode = 0;
        }
        g_config.c4fm_clk_is_set = 1;
        g_config.c4fm_clk_mode = mode;
    }
}

extern "C" int
dsd_neo_get_c4fm_clk(void) {
    if (!g_config_inited) {
        return 0;
    }
    return g_config.c4fm_clk_is_set ? g_config.c4fm_clk_mode : 0;
}

extern "C" void
dsd_neo_set_c4fm_clk_sync(int enable) {
    if (!g_config_inited) {
        memset(&g_config, 0, sizeof(g_config));
        g_config_inited = 1;
    }
    g_config.c4fm_clk_sync_is_set = 1;
    g_config.c4fm_clk_sync = enable ? 1 : 0;
}

extern "C" int
dsd_neo_get_c4fm_clk_sync(void) {
    if (!g_config_inited) {
        return 0;
    }
    return g_config.c4fm_clk_sync_is_set ? (g_config.c4fm_clk_sync ? 1 : 0) : 0;
}
