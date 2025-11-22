// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime configuration API and environment documentation.
 *
 * Exposes typed configuration parsed from environment variables and accessors
 * to initialize and retrieve the immutable configuration.
 */

#ifndef DSD_NEO_RUNTIME_CONFIG_H
#define DSD_NEO_RUNTIME_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Ensure dsd_opts type is visible to prototypes below */
#include <dsd-neo/core/opts_fwd.h>

/* Forward-declare decoder state for user-config helpers */
#include <dsd-neo/core/state_fwd.h>
#include <stdio.h>

/*
 * Runtime configuration (environment variables)
 *
 * Precedence: CLI/opts > environment > built-in defaults. This module parses
 * environment variables once (during open) and exposes a typed config.
 *
 * Realtime scheduling and CPU affinity
 * - DSD_NEO_RT_SCHED
 *     Enable best-effort realtime scheduling (SCHED_FIFO). Requires CAP_SYS_NICE or root.
 *     Values: "1" to enable, unset/other to disable. Default: disabled.
 * - DSD_NEO_RT_PRIO_USB | DSD_NEO_RT_PRIO_DONGLE | DSD_NEO_RT_PRIO_DEMOD
 *     Optional per-thread priorities (1..99, clamped to system limits). Used only if RT_SCHED=1.
 *     Example: export DSD_NEO_RT_PRIO_DEMOD=85
 * - DSD_NEO_CPU_USB | DSD_NEO_CPU_DONGLE | DSD_NEO_CPU_DEMOD
 *     Optional CPU core pinning for each thread. Integer CPU id (>=0). Example: export DSD_NEO_CPU_DEMOD=2
 *
 * Frontend/decimation/upsampling
 * - DSD_NEO_HB_DECIM
 *     Use half-band FIR decimator cascade (fast, good response) instead of legacy CIC-like path.
 *     Values: 1 enable, 0 disable. Default: 1 (enabled).
 * - DSD_NEO_COMBINE_ROT
 *     Combine 90° IQ rotation with USB byte→int16 widening in one pass when offset tuning is off.
 *     Values: 1 enable, 0 disable. Default: 1 (enabled).
 * - DSD_NEO_UPSAMPLE_FP
 *     Use fixed-point arithmetic in legacy linear upsampler for lower CPU/divisions.
 *     Values: 1 enable, 0 disable. Default: 1 (enabled).
 *
 * Rational resampler (polyphase upfirdn L/M)
 * - DSD_NEO_RESAMP
 *     Target output sample rate in Hz. Enables L/M resampler when set.
 *     Values: "off" or "0" to disable; integer Hz (e.g., 48000) to enable. Default: 48000 (enabled).
 *
 * Residual CFO frequency-locked loop (FLL)
 * - DSD_NEO_FLL
 *     Enable residual carrier frequency correction.
 *     Values: "1" or unset to enable; other values disable. Default: enabled.
 * - DSD_NEO_FLL_ALPHA, DSD_NEO_FLL_BETA
 *     Proportional and integral gains (Q15 fixed-point, ~value/32768). Typical small values.
 *     Defaults: ALPHA=100 (~0.003), BETA=10 (~0.0003). May be adjusted for digital modes if not set.
 * - DSD_NEO_FLL_DEADBAND
 *     Ignore small phase errors in the FLL loop to avoid audible low-frequency sweeps in analog FM.
 *     Values: Q14 integer threshold (pi == 1<<14). Example: 60 (~0.36 degrees). Default: 45.
 * - DSD_NEO_FLL_SLEW
 *     Limit per-update NCO frequency change (slew-rate) to prevent rapid ramps.
 *     Values: Q15 integer (2*pi == 1<<15). Example: 32..128. Default: 64.
 *
 * Gardner timing error detector (TED)
 * - DSD_NEO_TED
 *     Enable lightweight fractional-delay timing correction. Generally off for analog FM.
 *     Values: 1 enable, else disabled. Default: 0 (disabled). For certain digital modes, defaults are adjusted
 *     only if envs are not provided (still off unless forced via DSD_NEO_TED=1).
 * - DSD_NEO_TED_SPS
 *     Nominal samples-per-symbol (integer). If unset and a digital mode is active, it is derived from output rate.
 *     Default: 10.
 * - DSD_NEO_TED_GAIN
 *     Small loop gain (Q20). Default: 64; for common digital modes may default to 96 when not provided.
 * - DSD_NEO_TED_FORCE
 *     Force TED to run for FM/C4FM paths where it is normally skipped. Values: 1 enable, else disabled. Default: 0.
 *
 * C4FM clock assist (symbol-domain)
 * - DSD_NEO_C4FM_CLK
 *     Enable a lightweight clock loop on the C4FM (P25p1) symbol path.
 *     Values: "el" for Early-Late, "mm" for Mueller&Mueller, "0"/"off" to disable.
 *     Default: off. When enabled, the loop nudges the integer symbolCenter by ±1
 *     occasionally based on the error sign; it does not perform fractional delay.
 * - DSD_NEO_C4FM_CLK_SYNC
 *     Allow C4FM clock assist to remain active while synchronized (fine-trim).
 *     Values: 1 enable, else disabled. Default: 0 (disabled; assist runs only pre-sync).
 *
 * Audio processing
 * - DSD_NEO_DEEMPH
 *     Post-demod deemphasis time constant. Applies only when the active demod preset enables deemphasis.
 *     Values: "75" (75µs, default), "50" (50µs), "nfm" (~750µs), "off" (disable).
 * - DSD_NEO_AUDIO_LPF
 *     Optional one-pole low-pass filter after demod. Approximate cutoff in Hz.
 *     Values: "off" or "0" to disable; integer (e.g., 3000, 5000) to enable. Default: off.
 *
 * FM/C4FM amplitude stabilization (pre-discriminator)
 * - DSD_NEO_FM_AGC
 *     Enable a constant-envelope limiter/AGC on complex I/Q before FM discrimination. Helps stabilize
 *     RTL-SDR amplitude bounce (e.g., +/-3 dB) that can raise P25 P1 error rates.
 *     Default: off for all modes. Values: 1 enable, 0 disable.
 * - DSD_NEO_FM_AGC_TARGET
 *     Target RMS amplitude of the complex envelope |z| in int16 units after decimation. Typical 8000..12000.
 *     Default: 10000.
 * - DSD_NEO_FM_AGC_MIN
 *     Minimum RMS to engage AGC; below this, gain is held to avoid boosting noise. Default: 2000.
 * - DSD_NEO_FM_AGC_ALPHA_UP, DSD_NEO_FM_AGC_ALPHA_DOWN
 *     Q15 smoothing factors when the computed block gain increases vs decreases, respectively.
 *     Larger values react faster. Defaults: ALPHA_UP=8192 (~0.25), ALPHA_DOWN=24576 (~0.75).
 * - DSD_NEO_FM_LIMITER
 *     Enable constant-envelope limiter that normalizes each complex sample to a near-constant
 *     magnitude around the AGC target. Helpful to clamp fast AM ripple. Default: off (try enabling
 *     for P25 P1 if AGC alone is insufficient).
 *
 * Complex DC offset removal (baseband)
 * - DSD_NEO_IQ_DC_BLOCK
 *     Enable a leaky integrator high-pass on I/Q before FM discrimination: dc += (x-dc)>>k; y=x-dc.
 *     Values: 1 enable, 0 disable. Default: off.
 * - DSD_NEO_IQ_DC_SHIFT
 *     k in the above relation (10..14 typical, larger=k -> slower). Default: 11.
 *
 * Channel complex low-pass (RTL baseband)
 * - DSD_NEO_CHANNEL_LPF
 *     Optional complex low-pass on the RTL DSP baseband after half-band/CIC
 *     decimation. Intended to narrow out-of-channel noise when running at
 *     higher baseband rates (e.g., 24 kHz). By default this is enabled only
 *     for analog-like modes at >=20 kHz and disabled for digital voice modes.
 *     Values: 0 to force off; non-zero to force on regardless of mode.
 * Frontend tuning behavior
 * - DSD_NEO_DISABLE_FS4_SHIFT
 *     Disable +fs/4 capture frequency shift when offset_tuning is off. Useful for trunking where exact
 *     LO=center is desired by controller logic. Values: 1 to disable, else enabled. Default: 0 (enabled).
 * - DSD_NEO_OUTPUT_CLEAR_ON_RETUNE
 *     Force clearing the output audio ring on retune/hop. When disabled, audio drains naturally to avoid
 *     cutting off transmissions. Values: 1 to clear, else drain. Default: 0 (drain).
 * - DSD_NEO_RETUNE_DRAIN_MS
 *     Maximum time in milliseconds to wait for output ring to drain on retune/hop when not clearing.
 *     Default: 50ms.
 *
 * Symbol window debug/testing
 * - DSD_NEO_WINDOW_FREEZE
 *     Freeze symbol decision window selection and disable auto-centering nudges. Useful for A/B testing.
 *     Values: 1 freeze, else dynamic. Default: 0 (dynamic).
 *
 * Intra-block multithreading
 * - DSD_NEO_MT
 *     Enable a minimal 2-thread worker pool for certain CPU-heavy inner loops.
 *     Values: 1 enable, else disabled. Default: 0 (disabled).
 */

typedef enum {
    DSD_NEO_DEEMPH_UNSET = 0,
    DSD_NEO_DEEMPH_OFF,
    DSD_NEO_DEEMPH_50,
    DSD_NEO_DEEMPH_75,
    DSD_NEO_DEEMPH_NFM
} dsdneoDeemphMode;

typedef struct dsdneoRuntimeConfig {
    /* Half-band decimator toggle */
    int hb_decim_is_set;
    int hb_decim;

    /* Combine rotate + widen */
    int combine_rot_is_set;
    int combine_rot;

    /* Legacy upsampler fixed-point toggle */
    int upsample_fp_is_set;
    int upsample_fp;

    /* Rational resampler target */
    int resamp_is_set;    /* env seen */
    int resamp_disable;   /* env explicitly disables */
    int resamp_target_hz; /* >0 when enabled */

    /* Residual CFO FLL */
    int fll_is_set;
    int fll_enable;
    int fll_alpha_is_set;
    int fll_alpha_q15;
    int fll_beta_is_set;
    int fll_beta_q15;
    int fll_deadband_is_set;
    int fll_deadband_q14;
    int fll_slew_is_set;
    int fll_slew_max_q15;

    /* CQPSK Costas loop (carrier recovery) */
    int costas_bw_is_set;
    double costas_loop_bw;
    int costas_damping_is_set;
    double costas_damping;
    int costas_order_is_set;
    int costas_order;
    int costas_use_snr_is_set;
    int costas_use_snr;
    int costas_noise_db_is_set;
    double costas_noise_db;

    /* Gardner TED */
    int ted_is_set;
    int ted_enable;
    int ted_gain_is_set;
    int ted_gain_q20;
    int ted_sps_is_set;
    int ted_sps;
    int ted_force_is_set;
    int ted_force;

    /* C4FM clock assist */
    int c4fm_clk_is_set;      /* env seen */
    int c4fm_clk_mode;        /* 0=off, 1=EL, 2=MM */
    int c4fm_clk_sync_is_set; /* env seen */
    int c4fm_clk_sync;        /* 0=pre-sync only, 1=also while synced */

    /* Deemphasis */
    int deemph_is_set;
    dsdneoDeemphMode deemph_mode;

    /* Post-demod audio LPF */
    int audio_lpf_is_set;
    int audio_lpf_disable;
    int audio_lpf_cutoff_hz; /* >0 when enabled */

    /* Intra-block multithreading */
    int mt_is_set;
    int mt_enable;

    /* Frontend tuning behavior */
    int fs4_shift_disable_is_set;
    int fs4_shift_disable;
    int output_clear_on_retune_is_set;
    int output_clear_on_retune;
    int retune_drain_ms_is_set;
    int retune_drain_ms;

    /* Symbol window debug/testing */
    int window_freeze_is_set;
    int window_freeze;

    /* Optional JSON emitter for P25 PDUs */
    int pdu_json_is_set;
    int pdu_json_enable;

    /* Optional SNR-based digital squelch (dB threshold). When set, frame sync
     * may skip expensive searches if estimated SNR is below this value.
     * Applies to relevant digital modes (e.g., P25 C4FM/CQPSK, GFSK family). */
    int snr_sql_is_set;
    int snr_sql_db; /* integer dB threshold */

    /* FM/C4FM amplitude AGC */
    int fm_agc_is_set;
    int fm_agc_enable;
    int fm_agc_target_is_set;
    int fm_agc_target_rms;
    int fm_agc_min_is_set;
    int fm_agc_min_rms;
    int fm_agc_alpha_up_is_set;
    int fm_agc_alpha_up_q15;
    int fm_agc_alpha_down_is_set;
    int fm_agc_alpha_down_q15;

    /* FM constant-envelope limiter */
    int fm_limiter_is_set;
    int fm_limiter_enable;

    /* Complex DC blocker */
    int iq_dc_block_is_set;
    int iq_dc_block_enable;
    int iq_dc_shift_is_set;
    int iq_dc_shift;

    /* FM/FSK blind CMA equalizer (pre-discriminator) */
    int fm_cma_is_set;
    int fm_cma_enable;
    int fm_cma_taps_is_set;
    int fm_cma_taps;
    int fm_cma_mu_is_set;
    int fm_cma_mu_q15;
    int fm_cma_warmup_is_set;
    int fm_cma_warmup; /* samples; <=0 => continuous */
    int fm_cma_strength_is_set;
    int fm_cma_strength; /* 0=Light, 1=Medium, 2=Strong */

    /* C4FM symbol-domain DD equalizer (prototype) */
    int c4fm_dd_eq_is_set;
    int c4fm_dd_eq_enable;
    int c4fm_dd_eq_taps_is_set;
    int c4fm_dd_eq_taps; /* odd: 3,5,7,9 */
    int c4fm_dd_eq_mu_is_set;
    int c4fm_dd_eq_mu_q15; /* 1..64 */

    /* Impulse blanker (pre-decimation) */
    int blanker_is_set; /* env seen */
    int blanker_enable;
    int blanker_thr_is_set;
    int blanker_thr;
    int blanker_win_is_set;
    int blanker_win;

    /* RTL channel complex low-pass (post-HB, complex baseband).
       Allows narrowing noise when running the RTL DSP baseband at higher
       sample rates (e.g., 24 kHz) without forcing it on for all modes. */
    int channel_lpf_is_set; /* env seen */
    int channel_lpf_enable; /* 0=off, 1=on */
}

dsdneoRuntimeConfig;

/* Parse environment once. Safe to call multiple times; last call wins. */
/**
 * @brief Parse environment variables and initialize the runtime configuration.
 *
 * Safe to call multiple times; the most recent call wins.
 *
 * @param opts Decoder options for potential precedence overrides.
 */
void dsd_neo_config_init(const dsd_opts* opts);

/* Get immutable pointer to current runtime config. */
/**
 * @brief Get immutable pointer to the current runtime configuration, or NULL if
 * initialization has not been performed.
 *
 * @return Pointer to config or NULL.
 */
const dsdneoRuntimeConfig* dsd_neo_get_config(void);

/* Runtime updaters for DD equalizer (UI control) */
void dsd_neo_set_c4fm_dd_eq(int enable, int taps, int mu_q15);
void dsd_neo_get_c4fm_dd_eq(int* enable, int* taps, int* mu_q15);

/* Runtime control for C4FM clock assist (0=off, 1=EL, 2=MM) */
void dsd_neo_set_c4fm_clk(int mode);
int dsd_neo_get_c4fm_clk(void);
/* Toggle C4FM clock assist while synced (0/1) */
void dsd_neo_set_c4fm_clk_sync(int enable);
int dsd_neo_get_c4fm_clk_sync(void);

/*
 * User configuration (INI file)
 *
 * Represents persisted user preferences loaded from or written to an INI-style
 * configuration file. This is a narrow subset of dsd_opts/dsd_state focusing
 * on stable, user-facing knobs (input, output, decode mode, trunking).
 */

typedef enum {
    DSDCFG_INPUT_UNSET = 0,
    DSDCFG_INPUT_PULSE,
    DSDCFG_INPUT_RTL,
    DSDCFG_INPUT_RTLTCP,
    DSDCFG_INPUT_FILE,
    DSDCFG_INPUT_TCP,
    DSDCFG_INPUT_UDP
} dsdneoUserInputSource;

typedef enum { DSDCFG_OUTPUT_UNSET = 0, DSDCFG_OUTPUT_PULSE, DSDCFG_OUTPUT_NULL } dsdneoUserOutputBackend;

typedef enum {
    DSDCFG_MODE_UNSET = 0,
    DSDCFG_MODE_AUTO,
    DSDCFG_MODE_P25P1,
    DSDCFG_MODE_P25P2,
    DSDCFG_MODE_DMR,
    DSDCFG_MODE_NXDN48,
    DSDCFG_MODE_NXDN96,
    DSDCFG_MODE_X2TDMA,
    DSDCFG_MODE_YSF,
    DSDCFG_MODE_DSTAR,
    DSDCFG_MODE_EDACS_PV,
    DSDCFG_MODE_DPMR,
    DSDCFG_MODE_M17,
    DSDCFG_MODE_TDMA,
    DSDCFG_MODE_ANALOG
} dsdneoUserDecodeMode;

typedef struct dsdneoUserConfig {
    int version; /* schema version, currently 1 */

    /* [input] */
    int has_input;
    dsdneoUserInputSource input_source;
    char pulse_input[256];
    int rtl_device;
    char rtl_freq[64];
    int rtl_gain;
    int rtl_ppm;
    int rtl_bw_khz;
    int rtl_sql;
    int rtl_volume;
    char rtltcp_host[128];
    int rtltcp_port;
    char file_path[1024];
    int file_sample_rate;
    char tcp_host[128];
    int tcp_port;
    char udp_addr[64];
    int udp_port;

    /* [output] */
    int has_output;
    dsdneoUserOutputBackend output_backend;
    char pulse_output[256];
    int ncurses_ui; /* bool */

    /* [mode] */
    int has_mode;
    dsdneoUserDecodeMode decode_mode;

    /* [trunking] */
    int has_trunking;
    int trunk_enabled;
    char trunk_chan_csv[1024];
    char trunk_group_csv[1024];
    int trunk_use_allow_list;
} dsdneoUserConfig;

/* Resolve platform-specific default config path (no I/O). Returns a pointer to
 * an internal static buffer, or NULL when no reasonable default can be
 * determined. */
const char* dsd_user_config_default_path(void);

/* Load config from a given path into cfg.
 * Returns 0 on success, non-zero on error (missing/unreadable file or parse
 * error). On error, cfg is zeroed. */
int dsd_user_config_load(const char* path, dsdneoUserConfig* cfg);

/* Atomically write cfg to the given path (for interactive save).
 * Returns 0 on success, non-zero on error. */
int dsd_user_config_save_atomic(const char* path, const dsdneoUserConfig* cfg);

/* Apply config-derived defaults to opts/state before env + CLI precedence. */
void dsd_apply_user_config_to_opts(const dsdneoUserConfig* cfg, dsd_opts* opts, dsd_state* state);

/* Snapshot current opts/state into a user config (for save/print). */
void dsd_snapshot_opts_to_user_config(const dsd_opts* opts, const dsd_state* state, dsdneoUserConfig* cfg);

/* Render a user config as INI to the given stream (stdout/stderr/file). */
void dsd_user_config_render_ini(const dsdneoUserConfig* cfg, FILE* stream);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_RUNTIME_CONFIG_H */
