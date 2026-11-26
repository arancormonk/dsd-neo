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

#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/trunk_scan_lists.h>
#include <dsd-neo/platform/platform.h>

/* Include schema types first (before extern "C" for C++ compat) */
#include <dsd-neo/core/frontend_types.h>
#include <dsd-neo/runtime/config_schema.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ensure dsd_opts type is visible to prototypes below */
#include <dsd-neo/core/opts_fwd.h>

/* Forward-declare decoder state for user-config helpers */
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>
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
 * Rational resampler (polyphase upfirdn L/M)
 * - DSD_NEO_RESAMP
 *     Target output sample rate in Hz (RTL/RTL-TCP). The RTL demod pipeline
 *     enables resampling by default to keep the output rate at ~48 kHz.
 *     Values: "off" or "0" to disable; integer Hz (e.g., 48000) to enable/override.
 *
 * CQPSK/OP25 Gardner timing recovery
 * - DSD_NEO_TED_GAIN
 *     Native-float CQPSK Gardner timing gain override (OP25-style; typical 0.01..0.1).
 *
 * Audio processing
 * These controls apply to monitor/non-symbol discriminator audio, not the
 * RTL-family digital FSK symbol stream.
 * - DSD_NEO_DEEMPH
 *     Post-demod deemphasis time constant. Applies only when the active demod preset enables deemphasis.
 *     Values: "75" (75µs, default), "50" (50µs), "nfm" (~750µs), "off" (disable).
 * - DSD_NEO_AUDIO_LPF
 *     Optional one-pole low-pass filter after demod. Approximate cutoff in Hz.
 *     Values: "off" or "0" to disable; integer (e.g., 3000, 5000) to enable. Default: off.
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
 *     higher baseband rates (e.g., 24 kHz). By default this is enabled for
 *     RTL baseband rates >=20 kHz and uses a mode-appropriate passband.
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
 * - DSD_NEO_RETUNE_MUTE_MS
 *     Input sample mute duration around RTL retunes. This drops tuner-settling samples before they can train
 *     CQPSK recovery state. Values: integer 10..1000. Default: 120ms.
 *
 * TCP audio input
 * - DSD_NEO_TCPIN_BACKOFF_MS
 *     Backoff time (milliseconds) before attempting to reconnect when TCP audio input stalls/disconnects.
 *     Values: integer 50..5000. Default: 300.
 *
 * Symbol window debug/testing
 * - DSD_NEO_WINDOW_FREEZE
 *     Freeze symbol decision window selection and disable auto-centering nudges. Useful for A/B testing.
 *     Values: 1 freeze, else dynamic. Default: 0 (dynamic).
 *
 * Debug/advanced knobs (centralized for maintainability)
 * - DSD_NEO_DEBUG_SYNC, DSD_NEO_DEBUG_CQPSK
 * - DSD_NEO_DEBUG_SYMBOL_TIMING
 *     Symbol-timing diagnostics on the decoder's own symbol grid.
 *     Values: 0 off, 1 one measurement line per accepted frame sync (sub-symbol offset,
 *     samplesPerSymbol, jitter), 2 additionally the per-sample +/-/O/X trace. Default: 0.
 * - DSD_NEO_CQPSK, DSD_NEO_CQPSK_SYNC_INV, DSD_NEO_CQPSK_SYNC_NEG
 * - DSD_NEO_FTZ_DAZ
 * - DSD_NEO_NO_BOOTSTRAP
 *
 * TCP/RTL/rigctl knobs
 * - DSD_NEO_TCP_* (autotune, waitall, stats, buffers, timeouts, prebuffer)
 * - DSD_NEO_RIGCTL_RCVTIMEO
 * - DSD_NEO_RTL_* and DSD_NEO_TUNER_* (direct/offset, xtal, testmode, autogain)
 * - DSD_NEO_AUTO_PPM* (carrier/error-based PPM correction)
 *
 * Protocol timers/holds
 * - DSD_NEO_P25_* and DSD_NEO_DMR_* (hangtimes, grace windows, holds, watchdog)
 *
 * Cache/path knobs
 * - DSD_NEO_CACHE_DIR, DSD_NEO_CC_CACHE
 */

/* Levels for DSD_NEO_DEBUG_SYMBOL_TIMING. */
enum {
    DSD_NEO_SYMBOL_TIMING_OFF = 0,
    DSD_NEO_SYMBOL_TIMING_SYNC_LINE = 1,
    DSD_NEO_SYMBOL_TIMING_TRACE = 2,
};

typedef enum DSD_ATTR_PACKED {
    DSD_NEO_DEEMPH_UNSET = 0,
    DSD_NEO_DEEMPH_OFF,
    DSD_NEO_DEEMPH_50,
    DSD_NEO_DEEMPH_75,
    DSD_NEO_DEEMPH_NFM
} dsdneoDeemphMode;

typedef struct dsdneoRuntimeConfig {
    /* Field order is chosen to minimize padding and keep clang-tidy's
     * clang-analyzer-optin.performance.Padding check quiet. */

    /* 8-byte aligned scalars */

    /* DMR / trunking timers */
    double dmr_hangtime_s;
    double dmr_grant_timeout_s;

    /* P25 timers/holds */
    double p25_hangtime_s;
    double p25_grant_timeout_s;
    double p25_cc_grace_s;
    double p25_vc_grace_s;
    double p25_mac_hold_s;
    double p25_voice_hold_s;

    /* P25 follower (UI-exposed) knobs */
    double p25_min_follow_dwell_s;
    double p25_grant_voice_to_s;
    double p25_force_release_extra_s;
    double p25_force_release_margin_s;
    double p25p1_err_hold_pct;
    double p25p1_err_hold_s;

    /* Input processing knobs */
    double input_warn_db;

    /* Supervisory tuner autogain knobs */
    double tuner_autogain_seed_db;
    double tuner_autogain_spec_snr_db;
    double tuner_autogain_inband_ratio;
    double tuner_autogain_up_step_db;

    /* Auto-PPM (carrier/tracking-based) knobs */
    double auto_ppm_snr_db;
    double auto_ppm_pwr_db;
    double auto_ppm_zerolock_ppm;

    /* CQPSK Costas loop (carrier recovery) */
    double costas_loop_bw;
    double costas_damping;

    /* DMR TIII tools (one-shot LCN calculator) */
    long dmr_t3_step_hz;
    long dmr_t3_cc_freq_hz;
    long dmr_t3_cc_lcn;
    long dmr_t3_start_lcn;

    /* Realtime scheduling and CPU affinity */
    int rt_sched_is_set;
    int rt_sched_enable;
    int rt_prio_usb_is_set;
    int rt_prio_usb;
    int rt_prio_dongle_is_set;
    int rt_prio_dongle;
    int rt_prio_demod_is_set;
    int rt_prio_demod;
    int cpu_usb_is_set;
    int cpu_usb;
    int cpu_dongle_is_set;
    int cpu_dongle;
    int cpu_demod_is_set;
    int cpu_demod;

    /* Bootstrap/system toggles */
    int ftz_daz_is_set;
    int ftz_daz_enable;
    int no_bootstrap_is_set;
    int no_bootstrap_enable;
    int no_signal_handlers_is_set;
    int no_signal_handlers_enable;

    /* Debug/tuning toggles */
    int debug_sync_is_set;
    int debug_sync_enable;
    int debug_cqpsk_is_set;
    int debug_cqpsk_enable;
    /* Level rather than a toggle: the two things this gates differ by orders of magnitude in
     * volume. 1 is one line per accepted sync; 2 adds a character per input sample. */
    int debug_symbol_timing_is_set;
    int debug_symbol_timing;

    /* CQPSK runtime toggles */
    int cqpsk_is_set;
    int cqpsk_enable;
    int cqpsk_sync_inv_is_set;
    int cqpsk_sync_inv;
    int cqpsk_sync_neg_is_set;
    int cqpsk_sync_neg;
    int sync_warmstart_is_set;
    int sync_warmstart_enable;

    /* DMR / trunking timers */
    int dmr_hangtime_is_set;
    int dmr_grant_timeout_is_set;

    /* P25 timers/holds */
    int p25_hangtime_is_set;
    int p25_grant_timeout_is_set;
    int p25_cc_grace_is_set;
    int p25_vc_grace_is_set;
    int p25_mac_hold_is_set;
    int p25_voice_hold_is_set;
    int p25_wd_ms_is_set;
    int p25_wd_ms;

    /* P25 follower (UI-exposed) knobs */
    int p25_min_follow_dwell_is_set;
    int p25_grant_voice_to_is_set;
    int p25_force_release_extra_is_set;
    int p25_force_release_margin_is_set;
    int p25p1_err_hold_pct_is_set;
    int p25p1_err_hold_s_is_set;

    /* P25 status-symbol AFC gate. Off by default because status-derived direction is advisory. */
    int p25_afc_status_gate_is_set;
    int p25_afc_status_gate_enable;

    /* P25 soft-decision recovery knobs. */
    int p25_soft_erasure_threshold_is_set;
    int p25_soft_erasure_threshold;
    int p25p1_soft_erasure_threshold_is_set;
    int p25p1_soft_erasure_threshold;
    int p25p2_soft_erasure_threshold_is_set;
    int p25p2_soft_erasure_threshold;
    int p25_soft_hard_override_is_set;
    int p25_soft_hard_override_enable;

    /* Input processing knobs */
    int input_volume_is_set;
    int input_volume_multiplier;
    int input_warn_db_is_set;

    /* DMR TIII tools (one-shot LCN calculator) */
    int dmr_t3_calc_csv_is_set;
    int dmr_t3_step_hz_is_set;
    int dmr_t3_cc_freq_is_set;
    int dmr_t3_cc_lcn_is_set;
    int dmr_t3_start_lcn_is_set;
    /* Supplied (env or the --calc-* flag) but unusable: the matching *_is_set stays 0, and the calculator refuses
       the run rather than print a map without that setting. */
    int dmr_t3_step_hz_invalid;
    int dmr_t3_cc_freq_invalid;
    int dmr_t3_cc_lcn_invalid;
    int dmr_t3_start_lcn_invalid;

    /* DMR TIII heuristic fill (opt-in) */
    int dmr_t3_heur_is_set;
    int dmr_t3_heur_enable;

    /* User config discovery */
    int config_path_is_set;

    /* Cache/path knobs */
    int cache_dir_is_set;
    int cc_cache_is_set;
    int cc_cache_enable;

    /* TCP/rigctl knobs */
    int tcp_bufsz_is_set;
    int tcp_bufsz_bytes;
    int tcp_waitall_is_set;
    int tcp_waitall_enable;
    int tcp_autotune_is_set;
    int tcp_autotune_enable;
    int tcp_stats_is_set;
    int tcp_stats_enable;
    int tcp_max_timeouts_is_set;
    int tcp_max_timeouts;
    int tcp_rcvbuf_is_set;
    int tcp_rcvbuf_bytes;
    int tcp_rcvtimeo_is_set;
    int tcp_rcvtimeo_ms;
    int rigctl_rcvtimeo_is_set;
    int rigctl_rcvtimeo_ms;
    int tcp_prebuf_ms_is_set;
    int tcp_prebuf_ms;

    /* RTL device/tuner knobs */
    int rtl_agc_is_set;
    int rtl_agc_enable;
    int rtl_direct_is_set;
    int rtl_direct_mode; /* 0=off, 1=I, 2=Q */
    int rtl_offset_tuning_is_set;
    int rtl_offset_tuning_enable;
    int rtl_xtal_hz_is_set;
    int rtl_xtal_hz;
    int tuner_xtal_hz_is_set;
    int tuner_xtal_hz;
    int rtl_testmode_is_set;
    int rtl_testmode_enable;
    int rtl_if_gains_is_set;
    int rtl_verify_is_set;
    int rtl_verify_enable;
    int rtl_verify_attempts_is_set;
    int rtl_verify_attempts;
    int tuner_bw_hz_is_set;
    int tuner_bw_hz; /* 0=auto */

    /* Supervisory tuner autogain knobs */
    int tuner_autogain_is_set;
    int tuner_autogain_enable;
    int tuner_autogain_probe_ms_is_set;
    int tuner_autogain_probe_ms;
    int tuner_autogain_seed_db_is_set;
    int tuner_autogain_spec_snr_db_is_set;
    int tuner_autogain_inband_ratio_is_set;
    int tuner_autogain_up_step_db_is_set;
    int tuner_autogain_up_persist_is_set;
    int tuner_autogain_up_persist;

    /* Auto-PPM (carrier/tracking-based) knobs */
    int auto_ppm_is_set;
    int auto_ppm_enable;
    int auto_ppm_snr_db_is_set;
    int auto_ppm_pwr_db_is_set;
    int auto_ppm_zerolock_ppm_is_set;
    int auto_ppm_zerolock_hz_is_set;
    int auto_ppm_zerolock_hz;
    int auto_ppm_freeze_is_set;
    int auto_ppm_freeze_enable;

    /* Rational resampler target */
    int resamp_is_set;    /* env seen */
    int resamp_disable;   /* env explicitly disables */
    int resamp_target_hz; /* >0 when enabled */

    /* CQPSK Costas loop (carrier recovery) */
    int costas_bw_is_set;
    int costas_damping_is_set;

    /* CQPSK Gardner timing recovery */
    int ted_gain_is_set;
    float ted_gain; /* CQPSK timing gain (typ 0.01-0.1) */

    /* Deemphasis */
    int deemph_is_set;
    dsdneoDeemphMode deemph_mode;

    /* Post-demod audio LPF */
    int audio_lpf_is_set;
    int audio_lpf_disable;
    int audio_lpf_cutoff_hz; /* >0 when enabled */

    /* Frontend tuning behavior */
    int combine_rot_is_set;
    int combine_rot;
    int fs4_shift_disable_is_set;
    int fs4_shift_disable;
    int output_clear_on_retune_is_set;
    int output_clear_on_retune;
    int retune_drain_ms_is_set;
    int retune_drain_ms;
    int retune_mute_ms_is_set;
    int retune_mute_ms;

    /* TCP audio input */
    int tcpin_backoff_ms_is_set;
    int tcpin_backoff_ms;

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

    /* Complex DC blocker */
    int iq_dc_block_is_set;
    int iq_dc_block_enable;
    int iq_dc_shift_is_set;
    int iq_dc_shift;

    /* RTL channel complex low-pass (post-HB, complex baseband).
       Allows narrowing noise when running the RTL DSP baseband at higher
       sample rates (e.g., 24 kHz) without forcing it on for all modes. */
    int channel_lpf_is_set; /* env seen */
    int channel_lpf_enable; /* 0=off, 1=on */

    /* Inline strings */
    char dmr_t3_calc_csv[1024];
    char config_path[1024];
    char cache_dir[1024];
    char rtl_if_gains[1024];
}

dsdneoRuntimeConfig;

/* Parse environment once. Safe to call multiple times; last call wins. */
/**
 * @brief Parse environment variables and initialize the runtime configuration.
 *
 * Safe to call multiple times; the most recent call wins.
 *
 */
void dsd_neo_config_init(void);

/* Get immutable pointer to current runtime config. */
/**
 * @brief Get immutable pointer to the current runtime configuration, or NULL if
 * initialization has not been performed.
 *
 * Thread-safety: returns a process-wide immutable snapshot pointer. The
 * returned pointer remains valid for the process lifetime and may be shared
 * across threads. Call again to observe newer published snapshots.
 *
 * @return Pointer to config or NULL.
 */
const dsdneoRuntimeConfig* dsd_neo_get_config(void);

/**
 * @brief Apply runtime config values to opts/state.
 *
 * Intended to centralize env-derived operational knobs that are still stored in
 * `dsd_opts` / `dsd_state` fields.
 *
 * @param cfg Runtime config snapshot (may be NULL).
 * @param opts Decoder options to mutate (may be NULL).
 * @param state Decoder state to mutate (may be NULL).
 */
void dsd_apply_runtime_config_to_opts(const dsdneoRuntimeConfig* cfg, dsd_opts* opts, dsd_state* state);

/*
 * User configuration (INI file)
 *
 * Represents persisted user preferences loaded from or written to an INI-style
 * configuration file. This is a narrow subset of dsd_opts/dsd_state focusing
 * on stable, user-facing knobs (input, output, decode mode, trunking).
 */

typedef enum DSD_ATTR_PACKED {
    DSDCFG_INPUT_UNSET = 0,
    DSDCFG_INPUT_PULSE,
    DSDCFG_INPUT_RTL,
    DSDCFG_INPUT_RTLTCP,
    DSDCFG_INPUT_SOAPY,
    DSDCFG_INPUT_FILE,
    DSDCFG_INPUT_TCP,
    DSDCFG_INPUT_UDP,
    DSDCFG_INPUT_AIRSPY,
    DSDCFG_INPUT_IQTCP, /* a raw I/Q stream dsd-neo connects to over TCP (issue #743) */
    DSDCFG_INPUT_IQUDP  /* a raw I/Q stream dsd-neo receives over UDP (issue #743) */
} dsdneoUserInputSource;

typedef enum DSD_ATTR_PACKED {
    DSDCFG_OUTPUT_UNSET = 0,
    DSDCFG_OUTPUT_PULSE,
    DSDCFG_OUTPUT_NULL
} dsdneoUserOutputBackend;

typedef enum DSD_ATTR_PACKED {
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
    DSDCFG_MODE_ANALOG,
    DSDCFG_MODE_DMR_MONO,
    DSDCFG_MODE_AM,   /* 16: the analog monitor with the AM envelope detector (IQ radio inputs only; issue #524) */
    DSDCFG_MODE_TETRA /* 17: TETRA (pi/4-DQPSK, 18 ksym/s) */
} dsdneoUserDecodeMode;

typedef enum DSD_ATTR_PACKED {
    DSDCFG_DEMOD_UNSET = 0,
    DSDCFG_DEMOD_AUTO,
    DSDCFG_DEMOD_C4FM,
    DSDCFG_DEMOD_GFSK,
    DSDCFG_DEMOD_QPSK
} dsdneoUserDemodPath;

/* [mode] edacs_addressing: the EDACS addressing mode, dsd_state::ea_mode -1/0/1. NONE is ProVoice alone (-fp). */
typedef enum DSD_ATTR_PACKED {
    DSDCFG_EDACS_ADDRESSING_UNSET = 0,
    DSDCFG_EDACS_ADDRESSING_NONE,
    DSDCFG_EDACS_ADDRESSING_STANDARD,
    DSDCFG_EDACS_ADDRESSING_EXTENDED
} dsdneoUserEdacsAddressing;

typedef struct dsdneoUserConfig {
    /* [input]: has_input through input_warn_db_is_set, one block a replay session retains whole
       (dsd_user_config_retain_input()), so every [input] field belongs inside it, and in the merge table of
       src/runtime/config_retained_input.c. */
    int has_input;
    dsdneoUserInputSource input_source;
    char pulse_input[256];
    /* The RTL-SDR the input opens: rtl_serial, a USB serial (dsd_rtl_serial_is_valid(); DSD_RTL_SERIAL_MAX + 1 bytes),
       wins over rtl_device, the index, which names it only while rtl_serial is "". rtl_device_is_set and
       rtl_serial_is_set say the config names the key: a patch merges the two into a retained [input] as one choice
       (dsd_user_config_retained_input_patched()). rtl_serial_invalid says it named a serial the rule refuses, which
       refuses the input too: its spec carries a device field the start refuses, never index 0. */
    int rtl_device;
    char rtl_serial[128];
    int rtl_device_is_set;
    int rtl_serial_is_set;
    int rtl_serial_invalid;
    char rtl_freq[64];
    int rtl_gain;
    int rtl_gain_is_set; /* distinguish explicit 0 (AGC) from omitted; see dsd_user_config_rtl_gain_is_set() */
    int rtl_ppm;
    int rtl_ppm_is_set; /* distinguish explicit 0 from omitted */
    int rtl_bw_khz;
    int rtl_sql;
    /* rtl_sql_mode (dsd_squelch_mode: 0 level, 1 auto, 2 noise) and rtl_sql_margin_db (the AUTO margin or the NOISE
       threshold, 3..30; 0 reads as the default 10). rtl_sql keeps the level under every mode. */
    int rtl_sql_mode;
    int rtl_sql_margin_db;
    /* 1 when the config names rtl_sql, rtl_sql_mode or rtl_sql_margin_db; a session's snapshot (radio or audio input)
       always sets it, so a save always writes them. A PCM source applies the keys a loaded config holds only then
       (issue #628): a config without them leaves its squelch alone. */
    int rtl_sql_is_set;
    /* 1 when a session's snapshot rounded its level to rtl_sql's whole dB (issue #899 B15), whose dB it held is
       rtl_sql_unrounded_db: a render of the snapshot says so. 0 in a loaded config. */
    int rtl_sql_rounded;
    double rtl_sql_unrounded_db;
    int rtl_volume;   /* monitor/non-symbol gain; does not scale symbol streams */
    int rtl_auto_ppm; /* bool */
    char rtltcp_host[128];
    int rtltcp_port;
    dsd_airspy_config airspy;
    int airspy_invalid; /* Invalid airspy_serial; other invalid Airspy keys warn and keep prior values. */
    char soapy_args[256];
    char soapy_profile[32];
    char soapy_stream_format[16];
    char soapy_antenna[64];
    char soapy_clock[64];
    char soapy_settings[1024];
    char soapy_gains[512];
    int soapy_bandwidth_hz;
    int soapy_bandwidth_hz_is_set;
    char digital_resample[8]; /* auto|on|off */
    char file_path[1024];
    int file_sample_rate;
    char tcp_host[128];
    int tcp_port;
    char udp_addr[64];
    int udp_port;
    /* A raw I/Q stream (source = "iqtcp" | "iqudp", issue #743): the spec's fields, iqtcp:iq_host:iq_port:iq_rate:
       iq_format[:iq_freq] (dsd_user_config_radio_input_spec()). iq_rate and iq_freq keep their text (Hz, or with a k or
       M suffix; iq_freq also G), as rtl_freq does; the spec parser reads them. */
    char iq_host[256];
    int iq_port;
    char iq_rate[32];
    char iq_format[8];
    char iq_freq[64];
    double input_warn_db;     /* low input-level advisory threshold in dBFS */
    int input_warn_db_is_set; /* distinguish explicit value from default */

    /* [output] */
    int has_output;
    dsdneoUserOutputBackend output_backend;
    char pulse_output[256];
    dsd_frontend_kind frontend_kind;
    int frontend_kind_is_set;
    int terminal_mouse_wheel;        /* terminal wheel steps -Y/trunk-scan channels */
    int terminal_mouse_wheel_is_set; /* distinguish explicit value from default */

    /* [mode] */
    int has_mode;
    dsdneoUserDecodeMode decode_mode;
    int has_dmr_mono;
    int dmr_mono;
    /* Comma-separated extra UDP ports decoded as DMR LRRP; empty means not set, so a
       reload leaves a CLI-supplied list alone. Split by core/lrrp_ports.h at apply time. */
    char dmr_lrrp_ports[64];
    /* Comma-separated "<index>=<port>" links for the manufacturer-specific DMR compressed-UDP
       port indices 95-127 ("97=4005,98=4007"); empty means not set, so a reload leaves a
       CLI-supplied map alone. Sized for all 33 indices as "127=65535,". Split by
       core/udp_comp_ports.h at apply time. */
    char dmr_udp_comp_ports[400];
    /* The EDACS EA/ESK variant. Applied AFTER the decode preset, which resets
       both: see apply_mode_config(). esk is a boolean over the single 0xA0 mask
       the four CLI variants and the RadioReference apply handler all use.
       has_edacs_variant is set by any of edacs_addressing, edacs_ea and edacs_esk.
       edacs_addressing, when not UNSET, wins over the older edacs_ea boolean
       (which can say only standard or extended), wherever each sits in the file. */
    int has_edacs_variant;
    int edacs_ea;
    int edacs_esk;
    dsdneoUserEdacsAddressing edacs_addressing;
    int has_demod;
    dsdneoUserDemodPath demod_path;

    /* [trunking] */
    int has_trunking;
    int trunk_enabled;
    char trunk_chan_csv[1024];
    char trunk_group_csv[1024];
    char trunk_p25_bandplan_csv[1024];
    char trunk_src_csv[1024]; // Source radio ID alias CSV path (labels only).
    int trunk_use_allow_list;
    int trunk_tune_group_calls;
    int trunk_tune_private_calls;
    int trunk_tune_data_calls;
    int trunk_tune_enc_calls;
    int trunk_persist_tg_lockouts;
    int trunk_scanner;
    int trunk_p25_prefer_candidates;
    int trunk_scan_voice_only;
    int trunk_scan_skip_silent;
    int trunk_scan_skip_silent_ms; /* 0 = the calibrated reading; else 10..500 (#834) */
    int trunk_scan_voice_qualify_ms;
    int trunk_scan_voice_hold_ms;
    int trunk_scan_max_visit_ms;

    /* [radioreference] */
    int has_radioreference;
    char rr_username[128];
    char rr_app_key[64];

    /* [trunk_scan] */
    int has_trunk_scan;
    int trunk_scan_enabled;
    /* targets_csv, targets_csv_2 .. targets_csv_8 and lists_off (issue #697), kept by list number: count is
       dsd_trunk_scan_lists_span(), so a gap or a lists_off entry past the last list stays for the start rules to
       refuse. A key a file or profile names replaces what an earlier one left; one it omits inherits. An empty
       targets_csv_N clears list N and every later one an earlier file or profile left (path and off state), an empty
       lists_off every off state. */
    dsd_trunk_scan_list_spec trunk_scan_lists[DSD_TRUNK_SCAN_MAX_LISTS];
    int trunk_scan_list_count;
    uint32_t trunk_scan_lists_named; /* bit N-1: the targets_csv key of list N was named, empty or not */
    int trunk_scan_lists_off_is_set;
    /* The loader's: what the file or profile being read names itself (the same bits, lists_off), so an empty
       targets_csv_N clears only what earlier ones left and a contradiction inside one stays for the gap rule. */
    uint32_t trunk_scan_lists_layer_named;
    int trunk_scan_lists_off_layer_set;
    int trunk_scan_idle_dwell_ms;
    int trunk_scan_activity_hold_ms;

    /* [logging] */
    int has_logging;
    char event_log[1024];
    char frame_log[1024];
    char p25_sm_log[1024];

    /* [alerts] */
    int has_alerts;
    int call_alert_enabled; /* bool */
    int call_alert_events;  /* bitmask of dsd_call_alert_event_t values */

    /* [recording] */
    int has_recording;
    int per_call_wav; /* bool */
    char per_call_wav_dir[512];
    char static_wav_path[1024];
    char raw_wav_path[1024];
    int rdio_mode;
    int rdio_system_id; /* 0..2147483647; the loader refuses a value outside it and keeps the one it had (issue #931) */
    char rdio_api_url[1024];
    char rdio_api_key[256];
    int rdio_upload_timeout_ms;
    int rdio_upload_retries;
    int rdio_api_delete_after_upload; /* bool */
    /* Analog calls' rdio-scanner system, 0..2147483647 (0: rdio_system_id), and the talkgroup of one with no frequency,
       0..2147483647 (0: not exported) (issue #668). The loader refuses a value outside the range and keeps the one it
       had. */
    int rdio_analog_system_id;
    int rdio_analog_talkgroup;
    /* The talkgroup of a digital call with no talkgroup and no frequency, 0..2147483647 (0: not exported) (issue
       #888). The loader refuses a value outside the range and keeps the one it had. */
    int rdio_unknown_talkgroup;
    int rdio_system_by_frequency; /* bool: system = the call's frequency in kHz (opts->rdio_system_by_frequency) */
    int unknown_talkgroups_only;  /* bool: record only talkgroups without a row (opts->record_unknown_only) */
    /* The longest pause in ms one analog call and its recording survive (opts->analog_record_max_gap_ms; issue #756):
       0..10000, 200 or less is off. The loader refuses a value outside the range and keeps the one it had. */
    int analog_max_gap_ms;
    /* The NFM per-call recording's pre-roll in ms (opts->analog_record_preroll_ms; issue #668): 0..1000, 0 is off. The
       loader refuses a value outside the range and keeps the one it had. */
    int analog_preroll_ms;

    /* [dsp] */
    int has_dsp;
    int iq_balance;  /* bool */
    int iq_dc_block; /* bool */

    /* [analog] (runtime/analog_channel.h). A width is the full RF channel-filter width in Hz; 0 is the kind's
       default (the key left out), which a save does not write. The loader refuses a value outside the kind's range
       and keeps the default. */
    int has_receive;
    int receive_refused;
    dsd_receive_policy receive;

    int has_analog;
    int analog_nfm_bandwidth_hz;
    int analog_am_bandwidth_hz;
    /* The CTCSS/DCS receive policy (issue #527): tone_filter (dsd_tone_filter_mode, off when left out) and tone_list
       (the list, empty when left out; off keeps it). A refused list is left empty with analog_tone_list_refused set,
       and a refused tone_filter keeps the mode already loaded; either way the loader warns, and a list policy without
       a usable list is applied as off. */
    int analog_tone_filter;
    dsd_tone_set analog_tone_set;
    int analog_tone_list_refused;
} dsdneoUserConfig;

/**
 * @brief Resolve the platform-specific default config path (no I/O).
 *
 * Returns a pointer to an internal static buffer, or NULL when no reasonable
 * default can be determined.
 *
 * @return Pointer to default path string or NULL when unavailable.
 */
const char* dsd_user_config_default_path(void);

/**
 * @brief Resolve "<config dir>/imports" (no I/O).
 *
 * Derived by replacing the final component of dsd_user_config_default_path()
 * with "imports", using the platform separator. Recomputed on every call into
 * an internal static buffer: the value is not latched, so the pointer is only
 * valid until the next call.
 *
 * @return Pointer to the imports directory path, or NULL when no config path
 *         resolves (none of XDG_CONFIG_HOME/HOME, or APPDATA on Windows, is set).
 */
const char* dsd_user_imports_dir(void);

/**
 * @brief Create dsd_user_imports_dir() and its parents with mode 0700.
 *
 * Existing directories are accepted. The result is verified with stat, because
 * the underlying component walk reports nothing.
 *
 * @return 0 when the directory exists afterwards; -1 otherwise.
 */
int dsd_user_imports_dir_create(void);

/**
 * @brief Load a user config from the given path.
 *
 * On error (missing/unreadable file or parse error), cfg is zeroed: it holds the reset defaults and nothing of the file
 * or its includes. A read error in the file or in any file it includes fails the whole load (issue #931), with
 * DSD_USER_CONFIG_READ_ERROR; the failing file is logged once.
 *
 * @param path Path to the INI file.
 * @param cfg [out] Destination user config.
 * @return 0 on success; DSD_USER_CONFIG_READ_ERROR on a read error; other non-zero on other errors.
 */
int dsd_user_config_load(const char* path, dsdneoUserConfig* cfg);

/** dsd_user_config_load() / dsd_user_config_load_profile() result: a read error part way through the file or an
    include (the file is unreadable, not missing). */
#define DSD_USER_CONFIG_READ_ERROR (-3)

/**
 * @brief Atomically write cfg to the given path (for interactive save).
 *
 * @param path Destination path for the INI file.
 * @param cfg User config to persist.
 * @return 0 on success; non-zero on error.
 */
int dsd_user_config_save_atomic(const char* path, const dsdneoUserConfig* cfg);

/**
 * @brief Reset @p cfg to the state the INI loader starts every file from.
 *
 * Not all-zero: some keys default to on (group/private/encrypted call tuning,
 * persisted lockouts, ...), so a config built with dsd_user_config_apply_key()
 * must start here or an omitted key would read as "off". Safe on NULL.
 */
void dsd_user_config_init(dsdneoUserConfig* cfg);

/**
 * @brief Mark @p section present in @p cfg, as a `[section]` header in a file does.
 *
 * Some sections act even with no keys: an empty `[analog]` puts the analog
 * widths and tone policy back to their defaults. Call it for every section
 * before applying that section's keys with dsd_user_config_apply_key().
 *
 * @return 1 when @p cfg and @p section are valid and the name is shorter than
 *         64 bytes, 0 otherwise. An unknown section is accepted and ignored.
 */
int dsd_user_config_apply_section(dsdneoUserConfig* cfg, const char* section);

/**
 * @brief Apply a single user-config key/value pair in memory.
 *
 * The same `[section] key = value` mapping the INI loader uses, exposed so
 * embedders (the JSON API) can build a @ref dsdneoUserConfig from structured
 * input without a file. Start from dsd_user_config_init(). Section and key are
 * matched case-insensitively; @p value is the text an INI line would carry
 * after its quotes. An unknown section or key is ignored, as the loader
 * ignores it.
 *
 * @return 1 when @p cfg, @p section and @p key are valid handles and both
 *         names are shorter than 64 bytes, 0 otherwise.
 */
int dsd_user_config_apply_key(dsdneoUserConfig* cfg, const char* section, const char* key, const char* value);

/**
 * @brief Apply config-derived defaults to opts/state before env + CLI precedence.
 *
 * @param cfg User config to apply.
 * @param opts Decoder options to mutate.
 * @param state Decoder state to mutate.
 */
void dsd_apply_user_config_to_opts(const dsdneoUserConfig* cfg, dsd_opts* opts, dsd_state* state);

/**
 * @brief dsd_apply_user_config_to_opts() for an apply that can still be put back: its [mode] preset is staged
 * (dsd_stage_decode_mode_preset()), so what the TETRA decoder holds stays until the caller commits the apply with
 * dsd_decode_mode_commit_tetra(). A runtime config apply that rolls the session back (a reopen that fails, a switch
 * onto the analog monitor the front end refuses at once) thus goes back to a TETRA session whole.
 *
 * @param cfg User config to apply.
 * @param opts Decoder options to mutate.
 * @param state Decoder state to mutate.
 */
void dsd_stage_user_config_to_opts(const dsdneoUserConfig* cfg, dsd_opts* opts, dsd_state* state);

/**
 * @brief The conventional scanner (dsd_opts::scanner_mode) applying @p cfg leaves on, without applying anything.
 *
 * A [trunking] section leaves it on only with trunking off: one automatic tuner owner, trunking winning when the
 * config asks for both. A config without [trunking] leaves @p opts's setting (0 for NULL @p opts). The apply stages it
 * by this rule, and a runtime apply asks it beforehand to know whether a running scan's row scope is left.
 *
 * @return 1 when the scanner is on after the apply, 0 otherwise (and for NULL @p cfg).
 */
int dsd_user_config_scanner_after(const dsdneoUserConfig* cfg, const dsd_opts* opts);

/**
 * @brief The radio input spec the config's [input] sets, without applying anything.
 *
 * What dsd_apply_user_config_to_opts() writes to opts->audio_in_dev for an `rtl` source with `rtl_freq`, an `rtltcp`
 * source with `rtltcp_host` ("rtl:dev:freq:gain:ppm:bw:sql:vol", "rtltcp:host:port[:freq:...]"), a `soapy` source
 * ("soapy" or "soapy:<soapy_args>"), an `airspy` source ("airspy") or an `iqtcp` / `iqudp` source whose iq_* keys build
 * a raw I/Q stream spec (dsd_user_config_iq_stream_input_spec()), with the values the config leaves out taken from
 * @p opts as the apply takes them. A caller compares it with the running input to know whether the apply reopens the
 * device, and which kind of device then sets the rate.
 *
 * @return 0 when the config sets such a spec (written to @p out), -1 when its [input] leaves the input spec alone or
 *         sets a non-radio input (or on bad arguments).
 */
int dsd_user_config_radio_input_spec(const dsdneoUserConfig* cfg, const dsd_opts* opts, char* out, size_t out_size);

/**
 * @brief Whether applying @p cfg's [input] sets the input device (opts->audio_in_dev): a `pulse`, `soapy` or `airspy`
 *        source always, an `rtl` source with `rtl_freq`, an `rtltcp` one with `rtltcp_host`, a `file` one with
 *        `file_path`, a `tcp` one with `tcp_host`, a `udp` one with `udp_addr`, and an `iqtcp` / `iqudp` one whose keys
 *        build a spec. Otherwise the input stays as it was, and so does the [input] a save writes.
 *
 * @return 1 when it does, 0 otherwise (no [input], or NULL).
 */
int dsd_user_config_input_sets_device(const dsdneoUserConfig* cfg);

/**
 * @brief The raw I/Q stream spec an `iqtcp` or `iqudp` [input] builds (issue #743):
 *        `iqtcp:iq_host:iq_port:iq_rate:iq_format[:iq_freq]` in its canonical form, which `-i` takes too.
 *
 * @param cfg      The config.
 * @param out      Receives the spec.
 * @param out_size Capacity of @p out.
 * @param why      Receives the spec parser's reason, naming the field, when the keys build none (may be NULL).
 * @param why_size Capacity of @p why.
 * @return 0 when it builds one, -1 when the [input] is no stream source or its keys build no spec.
 */
int dsd_user_config_iq_stream_input_spec(const dsdneoUserConfig* cfg, char* out, size_t out_size, char* why,
                                         size_t why_size);

/**
 * @brief The [input] key whose value the spec parser refuses in an `iqtcp` / `iqudp` [input]
 *        (dsd_user_config_iq_stream_input_spec()), from the field the parser names (dsd_iq_net_spec_parse_field()):
 *        `iq_host`, `iq_port`, `iq_rate`, `iq_format` or `iq_freq`, else `source`.
 *
 * @return The key, or NULL when the keys build a spec or the [input] is no stream source.
 */
const char* dsd_user_config_iq_stream_bad_key(const dsdneoUserConfig* cfg);

/**
 * @brief Whether @p cfg names a tuner gain: an `rtl_gain` key that was read (0 included, which is AGC), or a nonzero
 * gain set in code. An omitted gain leaves the gain the session runs.
 */
static inline int
dsd_user_config_rtl_gain_is_set(const dsdneoUserConfig* cfg) {
    return cfg && (cfg->rtl_gain_is_set || cfg->rtl_gain != 0);
}

/**
 * @brief Apply config defaults before CLI parsing without activating file-rate timing yet.
 *
 * Bootstrap uses this to stage config-backed input/output defaults first, then
 * let CLI overrides settle before any file-input sample-rate side effects are
 * committed into decoder timing.
 *
 * @param cfg User config to apply.
 * @param opts Decoder options to mutate.
 * @param state Decoder state to mutate.
 */
void dsd_apply_user_config_to_opts_pre_cli(const dsdneoUserConfig* cfg, dsd_opts* opts, dsd_state* state);

/**
 * @brief Finalize config-file input sample-rate defaults after CLI parsing.
 *
 * Applies the configured file sample rate only when the config-file input
 * still survived CLI parsing for this run.
 *
 * @param cfg User config to finalize.
 * @param opts Decoder options to mutate.
 * @param state Decoder state to mutate.
 */
void dsd_finalize_user_config_file_input_after_cli(const dsdneoUserConfig* cfg, dsd_opts* opts, dsd_state* state);

/**
 * @brief Snapshot current opts/state into a user config (for save/print).
 *
 * @param opts Decoder options to read.
 * @param state Decoder state to read.
 * @param cfg [out] Destination user config snapshot.
 */
void dsd_snapshot_opts_to_user_config(const dsd_opts* opts, const dsd_state* state, dsdneoUserConfig* cfg);

/**
 * @brief The snapshot dsd_snapshot_opts_to_user_config() takes, before its last step turns off the keys an I/Q stream
 * [input] cannot start beside (dsd_user_config_turn_off_beside_iq_stream()): trunking, the -Y scanner and a trunk
 * scan as the session runs them (issue #743). A replay session with a retained stream [input] runs them, which every
 * save hides; a change that replaces the replay with that input must be judged on them.
 *
 * @param opts Decoder options to read.
 * @param state Decoder state to read.
 * @param cfg [out] Destination user config snapshot.
 */
void dsd_snapshot_opts_to_user_config_as_run(const dsd_opts* opts, const dsd_state* state, dsdneoUserConfig* cfg);

/*
 * The [input] a session running an I/Q replay saves (issue #743). A replay (--iq-replay) has no INI form, so every save
 * of such a session (autosave, Save, --print-config) writes the [input] of the config it loaded, unchanged, or no
 * [input] when it loaded none. Bootstrap retains it after the load; a config file loaded during the replay whose own
 * [input] stays applied with the replay still running replaces it, and a config patch (the control API's config_apply)
 * merges the keys it sets into it; a radio row that returns the session to a replay retains the input it left; the
 * session leaving the replay drops it. It lives in a state extension (DSD_STATE_EXT_RUNTIME_CONFIG_INPUT), freed with
 * the state. Decoder thread only, except the snapshot copy below.
 */

/**
 * @brief Retain @p cfg's [input] fields (has_input through input_warn_db_is_set) for a replay session's saves.
 *
 * A config without an [input] (or NULL) leaves none retained. An [input] no config can start (a file input whose
 * file_path dsd_user_config_file_path_is_input_spec() names) is not retained: the copy held stays.
 *
 * @param state Decoder state that owns the copy.
 * @param cfg Config whose [input] is retained.
 * @return 0 on success, -1 when @p state is NULL or the copy cannot be allocated (the previous copy is kept then).
 */
int dsd_user_config_retain_input(dsd_state* state, const dsdneoUserConfig* cfg);

/**
 * @brief Merge the [input] keys @p patch sets into the retained [input] (retained whole when none is held).
 *
 * A key counts as set when its value differs from dsd_user_config_init()'s, or, for a key with an `_is_set` flag
 * (rtl_gain, rtl_ppm, the squelch keys, soapy_bandwidth_hz, input_warn_db), when its flag is set; a key written with
 * its default value is not told apart from an absent one. The source and the endpoint keys stay as held unless the
 * patch names them. A merge that would leave an [input] no config can start keeps the copy held.
 *
 * @param state Decoder state that owns the copy.
 * @param patch Config whose [input] keys are merged (has_input set).
 * @return 0 on success (nothing to merge included), -1 when @p state is NULL or an allocation fails (copy unchanged).
 */
int dsd_user_config_merge_retained_input(dsd_state* state, const dsdneoUserConfig* patch);

/**
 * @brief The retained [input] as dsd_user_config_merge_retained_input() would leave it after @p patch, without changing
 * the copy held: a config apply judges a patch on the [input] it leaves (issue #743), so a sourceless patch to a raw
 * I/Q stream the session saves is refused when the stream it leaves builds no spec.
 *
 * @param state Decoder state that owns the copy.
 * @param patch Config whose [input] keys would be merged (has_input set).
 * @param out Receives a zeroed config holding the merged [input] block only.
 * @return 1 when a copy is held and @p out holds the merge, 0 when none is held, @p patch has no [input] or an argument
 * is NULL (@p out unchanged), -1 when an allocation fails (@p out unchanged).
 */
int dsd_user_config_retained_input_patched(const dsd_state* state, const dsdneoUserConfig* patch,
                                           dsdneoUserConfig* out);

/**
 * @brief Return 1 when @p path, as a file input's file_path, names an input spec no config can start (an I/Q replay,
 * `iqreplay:...`), 0 otherwise and for NULL. --validate-config reports such a file_path as an error.
 *
 * @param path A file_path value.
 */
int dsd_user_config_file_path_is_input_spec(const char* path);

/**
 * @brief Drop the retained [input], if any.
 *
 * @param state Decoder state that owns the copy (may be NULL).
 */
void dsd_user_config_forget_retained_input(dsd_state* state);

/**
 * @brief Copy the retained [input] fields into @p cfg.
 *
 * @param state Decoder state (or a snapshot of it) holding the copy.
 * @param cfg [out] Config whose [input] fields are overwritten; left as it is when none is retained.
 * @return 1 when an [input] was retained and copied, 0 otherwise.
 */
int dsd_user_config_retained_input(const dsd_state* state, dsdneoUserConfig* cfg);

/**
 * @brief Give the snapshot @p dst the retained [input] of @p src (none when @p src has none).
 *
 * Copies only when @p src retained another [input] since the last copy, so a snapshot publish pays a compare.
 *
 * @param dst Snapshot state to update.
 * @param src Decoder state (or the snapshot it was published to).
 */
void dsd_user_config_copy_retained_input(dsd_state* dst, const dsd_state* src);

/**
 * @brief Render a user config as INI to the given stream (stdout/stderr/file).
 *
 * @param cfg User config to render.
 * @param stream Output stream (stdout/stderr/file).
 */
void dsd_user_config_render_ini(const dsdneoUserConfig* cfg, FILE* stream);

/**
 * @brief Render a commented config template with all options and defaults.
 *
 * Generates a fully-commented INI file showing all available configuration
 * keys with their descriptions, types, and default values.
 *
 * @param stream Output stream (stdout/file).
 */
void dsd_user_config_render_template(FILE* stream);

/**
 * @brief Expand shell-like variables in a path string.
 *
 * Expands:
 *   ~       -> $HOME or platform home directory
 *   $VAR    -> environment variable VAR
 *   ${VAR}  -> environment variable VAR (braced form)
 *
 * Missing variables expand to empty string (no error).
 *
 * @param input Input path with possible variables.
 * @param output Output buffer for expanded path.
 * @param output_size Size of output buffer.
 * @return 0 on success; -1 on truncation or error.
 */
int dsd_config_expand_path(const char* input, char* output, size_t output_size);

/**
 * @brief Load a user config with optional profile overlay.
 *
 * Loads the base configuration from the INI file. If profile_name is non-NULL,
 * the named [profile.NAME] section is applied on top of the base config.
 *
 * Profile sections use dotted key syntax: section.key = value
 *
 * @param path Path to INI file.
 * @param profile_name Profile name (NULL for base config only).
 * @param cfg [out] Destination user config.
 * @return 0 on success; non-zero on error.
 */
int dsd_user_config_load_profile(const char* path, const char* profile_name, dsdneoUserConfig* cfg);

/**
 * @brief List available profile names in a config file.
 *
 * Scans the INI file for [profile.NAME] sections and returns the names.
 *
 * @param path Path to INI file.
 * @param names Output array of profile name pointers (caller provides).
 * @param names_buf Buffer to store profile name strings.
 * @param names_buf_size Size of names buffer.
 * @param max_names Maximum number of names to return.
 * @return Number of profiles found, or -1 on error.
 */
int dsd_user_config_list_profiles(const char* path, const char** names, char* names_buf, size_t names_buf_size,
                                  int max_names);

/**
 * @brief List available profile names from an already-open config stream.
 *
 * The stream must be readable and seekable. The parser rewinds it before use.
 *
 * @param stream Open INI stream.
 * @param names Output array of profile name pointers (caller provides).
 * @param names_buf Buffer to store profile name strings.
 * @param names_buf_size Size of names buffer.
 * @param max_names Maximum number of names to return.
 * @return Number of profiles found, or -1 on error.
 */
int dsd_user_config_list_profiles_stream(FILE* stream, const char** names, char* names_buf, size_t names_buf_size,
                                         int max_names);

/**
 * @brief Validate a config file and collect diagnostics.
 *
 * Parses the config file and checks for:
 *   - Unknown keys (warning)
 *   - Type mismatches (error)
 *   - Value range violations (warning)
 *
 * @param path Path to INI file.
 * @param diags [out] Diagnostic results (caller frees via dsdcfg_diags_free).
 * @return 0 if no errors; non-zero if errors present.
 */
int dsd_user_config_validate(const char* path, dsdcfg_diagnostics_t* diags);

/**
 * @brief Whether @p cfg names any trunk-scan list key (targets_csv, targets_csv_N or lists_off; issue #697).
 */
int dsd_user_config_names_trunk_scan_lists(const dsdneoUserConfig* cfg);

/**
 * @brief Put the trunk-scan lists @p cfg names over @p lists, as applying it does (issue #697).
 *
 * Key by key, as includes and profiles compose: each list path the config names (its targets_csv key, or a path
 * filled in) replaces that list's, an empty one clearing that list and every later one, off states included, unless
 * the config names lists_off; its start states replace every start state when it names lists_off (or marks a list
 * off). What it omits stays, so a config naming lists_off alone keeps the paths it is put over. Slots past @p count
 * are cleared first; @p count becomes the span (dsd_trunk_scan_lists_span()).
 */
void dsd_user_config_compose_trunk_scan_lists(const dsdneoUserConfig* cfg, dsd_trunk_scan_list_spec* lists, int* count);

/**
 * @brief Whether a composed config passes the trunk-scan rules startup refuses to start without (issue #671).
 *
 * The same rules dsd_user_config_validate() reports on a composed file: a trunk scan names its targets_csv, and no
 * global channel map, global P25 band plan or -Y scanner (trunking off) sits beside it. For a config a session would
 * save, such as the one a runtime config apply or a frontend's change leaves for the next start.
 *
 * @param before NULL to judge every rule; otherwise only a rule @p cfg breaks and @p before does not refuses it, so a
 *               change that keeps or fixes a rule the config already broke passes.
 * @param why [out] The first refusing rule's reason, as the validation reports it (may be NULL).
 * @return 0 when the config passes; -1 otherwise.
 */
int dsd_user_config_check_trunk_scan_startable(const dsdneoUserConfig* cfg, const dsdneoUserConfig* before, char* why,
                                               size_t why_size);

/**
 * @brief The first key an I/Q stream [input] cannot start beside that @p cfg turns on (issue #743, D8).
 *
 * The same rules as dsd_user_config_check_trunk_scan_startable() and dsd_user_config_turn_off_beside_iq_stream():
 * with @p cfg's [input] a raw I/Q stream (iqtcp or iqudp), `trunking.enabled`, else `trunking.scanner` (trunking off),
 * else `trunk_scan.enabled`. Startup names it in its refusal.
 *
 * @param section [out] The rule's section, such as "trunking" (may be NULL).
 * @param key [out] The rule's key, such as "enabled" (may be NULL).
 * @return 1 when @p cfg breaks one (@p section and @p key set); 0 otherwise.
 */
int dsd_user_config_iq_stream_refused_key(const dsdneoUserConfig* cfg, const char** section, const char** key);

/**
 * @brief Turn off in a config each key an I/Q stream [input] cannot start beside (issue #743, D8).
 *
 * When @p cfg's [input] is a raw I/Q stream (iqtcp or iqudp), every key the stream's next-start rule refuses
 * (dsd_user_config_check_trunk_scan_startable(): trunking, the -Y scanner, a trunk scan) is set to off, so the config
 * passes that rule. Driven by the same rules. Any other [input] is left as it is. A session snapshot
 * (dsd_snapshot_opts_to_user_config()) ends with it: a replay session that runs trunking or a scan beside the stream
 * [input] it loaded saves that [input] with them off, a config the next start runs.
 */
void dsd_user_config_turn_off_beside_iq_stream(dsdneoUserConfig* cfg);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_RUNTIME_CONFIG_H */
