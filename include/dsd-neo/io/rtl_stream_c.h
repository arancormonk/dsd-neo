// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief C shim header for the RTL-SDR orchestrator.
 *
 * Declares a minimal C API mirroring lifecycle, tuning, and I/O operations
 * of the C++ `RtlSdrOrchestrator`, for consumption by C translation units.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <dsd-neo/core/dsd.h>

/* Opaque stream context */
typedef struct RtlSdrContext RtlSdrContext;

/* Lifecycle */
/**
 * @brief Create a new RTL-SDR stream context from options.
 * @param opts Decoder options snapshot used to configure the stream. Must not be NULL.
 * @param out_ctx [out] On success, receives an opaque context pointer.
 * @return 0 on success; otherwise <0 on error.
 */
int rtl_stream_create(const dsd_opts* opts, RtlSdrContext** out_ctx);
/**
 * @brief Start the stream threads and device I/O.
 * @param ctx Stream context created by rtl_stream_create().
 * @return 0 on success; otherwise <0 on error.
 */
int rtl_stream_start(RtlSdrContext* ctx);
/**
 * @brief Stop the stream and cleanup resources associated with the run.
 * Safe to call multiple times; subsequent calls are no-ops.
 * @param ctx Stream context created by rtl_stream_create().
 * @return 0 on success; otherwise <0 on error.
 */
int rtl_stream_stop(RtlSdrContext* ctx);
int rtl_stream_soft_stop(RtlSdrContext* ctx);
/**
 * @brief Destroy the stream context and free all associated resources.
 * If the stream is running, it is stopped before destruction.
 * @param ctx Stream context to destroy. May be NULL.
 * @return 0 always.
 */
int rtl_stream_destroy(RtlSdrContext* ctx);

/* Control */
/**
 * @brief Tune to a new center frequency.
 * @param ctx Stream context.
 * @param center_freq_hz New center frequency in Hz.
 * @return 0 on success; otherwise <0 on error.
 */
int rtl_stream_tune(RtlSdrContext* ctx, uint32_t center_freq_hz);

/* I/O */
/**
 * @brief Read up to `count` interleaved audio samples into `out`.
 * @param ctx Stream context.
 * @param out Destination buffer for samples. Must not be NULL.
 * @param count Maximum number of samples to read.
 * @param out_got [out] Set to the number of samples actually read.
 * @return 0 on success; otherwise <0 on error (e.g., shutdown).
 */
int rtl_stream_read(RtlSdrContext* ctx, int16_t* out, size_t count, int* out_got);
/**
 * @brief Get the current output sample rate in Hz.
 * @param ctx Stream context.
 * @return Output sample rate in Hz; returns 0 if `ctx` is invalid.
 */
uint32_t rtl_stream_output_rate(const RtlSdrContext* ctx);

/* Optional helpers to mirror legacy API behavior */
/**
 * @brief Clear the output ring buffer and wake any waiting producer.
 * Mirrors the legacy behavior. The `ctx` parameter is currently ignored.
 * @param ctx Stream context (unused).
 */
void rtl_stream_clear_output(RtlSdrContext* ctx);
/**
 * @brief Return mean power approximation (RMS^2 proxy) for soft squelch.
 * The computation uses a small fixed sample window and mirrors the legacy implementation.
 * @param ctx Stream context (unused).
 * @return Mean power value (approximate RMS squared).
 */
long rtl_stream_return_pwr(const RtlSdrContext* ctx);

/**
 * @brief Enable or disable RTL-SDR bias tee at runtime.
 *
 * Applies to the active device if present. For rtl_tcp sources, forwards the
 * request to the server (protocol cmd 0x0E). For USB sources, requires
 * librtlsdr built with bias tee API support.
 *
 * @param on Non-zero to enable, zero to disable.
 * @return 0 on success; negative on failure or when no device is active.
 */
int rtl_stream_set_bias_tee(int on);

/**
 * @brief Get the currently applied tuner gain.
 *
 * Returns the driver-reported tuner gain in tenths of a dB (e.g., 270 for
 * 27.0 dB) via out_tenth_db, and whether auto-gain is active via out_is_auto
 * (1=auto, 0=manual). Any pointer may be NULL. Returns 0 on success; <0 if
 * the RTL stream/device is not available.
 */
int rtl_stream_get_gain(int* out_tenth_db, int* out_is_auto);
/* RTL-TCP adaptive networking (0/1) */
int rtl_stream_get_rtltcp_autotune(void);
void rtl_stream_set_rtltcp_autotune(int onoff);

/**
 * @brief Get smoothed TED residual (EMA of Gardner error) from demod pipeline.
 * Positive: persistent early (nudge center right). Negative: late (nudge left).
 * Returns 0 when unavailable.
 *
 * @param ctx Stream context (unused).
 * @return Residual value (coarse units, signed integer).
 */
int rtl_stream_ted_bias(const RtlSdrContext* ctx);
/** Get current TED SPS setting. */
int rtl_stream_get_ted_sps(void);
/** Set small TED gain (Q20). Typical 32..256 (default ~64). */
void rtl_stream_set_ted_gain(int gain_q20);
/** Get current TED gain (Q20). */
int rtl_stream_get_ted_gain(void);
/** Force TED even for FM/C4FM paths (0/1). */
void rtl_stream_set_ted_force(int onoff);
/** Get TED force flag (0/1). */
int rtl_stream_get_ted_force(void);

/* Eye diagram (timing) support for FSK/C4FM.
 * Copies up to max_samples real I-channel samples of the decimated complex baseband
 * and returns the number of samples. Also returns current nominal SPS via out_sps (may be 0 if unknown).
 */
int rtl_stream_eye_get(int16_t* out, int max_samples, int* out_sps);

/**
 * @brief Get smoothed demod SNR estimate in dB (post-filter, center-of-symbol).
 *
 * Computed on the demod thread for digital modes using I-channel samples near
 * symbol centers and a 4-level clustering heuristic for C4FM/FSK.
 * Returns a negative value when unavailable.
 */
double rtl_stream_get_snr_c4fm(void);
double rtl_stream_get_snr_cqpsk(void);
double rtl_stream_get_snr_gfsk(void);

/**
 * @brief Estimate C4FM SNR from the eye buffer as a lightweight fallback.
 *
 * Uses quartile clustering over eye-diagram I-channel samples near symbol
 * centers to approximate signal and noise variances, returning SNR in dB.
 * Returns a negative value (<= -50 dB) when insufficient data is available.
 */
double rtl_stream_estimate_snr_c4fm_eye(void);

/**
 * @brief Estimate QPSK SNR from the constellation snapshot as a fallback.
 *
 * Uses recent equalized I/Q points to estimate amplitude and EVM vs ideal
 * QPSK targets; returns SNR in dB. Returns <= -50 dB when insufficient data.
 */
double rtl_stream_estimate_snr_qpsk_const(void);

/**
 * @brief Estimate GFSK SNR from the eye buffer as a fallback.
 *
 * Uses a two-level (median split) clustering on eye-diagram I-channel
 * samples near symbol centers; returns SNR in dB. Returns <= -50 dB when
 * insufficient data.
 */
double rtl_stream_estimate_snr_gfsk_eye(void);

/* Impulse blanker (pre-decimation) runtime control */
int rtl_stream_get_blanker(int* out_thr, int* out_win);
void rtl_stream_set_blanker(int enable, int thr, int win);

/* Supervisory tuner autogain runtime control (0/1) */
int rtl_stream_get_tuner_autogain(void);
void rtl_stream_set_tuner_autogain(int onoff);

/* C4FM DD equalizer (symbol-domain) runtime control */
void rtl_stream_set_c4fm_dd_eq(int onoff);
int rtl_stream_get_c4fm_dd_eq(void);
void rtl_stream_set_c4fm_dd_eq_params(int taps, int mu_q15);
void rtl_stream_get_c4fm_dd_eq_params(int* taps, int* mu_q15);

/* C4FM clock assist (0=off, 1=EL, 2=MM) */
void rtl_stream_set_c4fm_clk(int mode);
int rtl_stream_get_c4fm_clk(void);
/* C4FM clock assist while synced (0/1) */
void rtl_stream_set_c4fm_clk_sync(int enable);
int rtl_stream_get_c4fm_clk_sync(void);

/**
 * @brief Get spectrum-based auto PPM status and last measurements.
 *
 * Returns current enable flag and the latest measurement snapshot used by
 * the spectrum-based auto-PPM routine: SNR in dB, estimated frequency
 * offset in Hz, estimated PPM error relative to tuned center, last applied
 * step direction (-1,0,+1), and cooldown countdown (loops).
 * Any pointer may be NULL. Returns 0 on success.
 */
int rtl_stream_auto_ppm_get_status(int* enabled, double* snr_db, double* df_hz, double* est_ppm, int* last_dir,
                                   int* cooldown, int* locked);
/** Return 1 if auto-PPM training is active (enabled, not locked, in training window). */
int rtl_stream_auto_ppm_training_active(void);
/** Get locked auto-PPM value and lock-time SNR/df snapshot, if available. */
int rtl_stream_auto_ppm_get_lock(int* ppm, double* snr_db, double* df_hz);
/** Runtime toggle for auto-PPM (0/1). */
void rtl_stream_set_auto_ppm(int onoff);
int rtl_stream_get_auto_ppm(void);

/* Runtime DSP adjustments and feedback hooks */
/**
 * @brief Set CQPSK path parameters at runtime; pass -1 to leave any field unchanged.
 */
void rtl_stream_cqpsk_set(int lms_enable, int taps, int mu_q15, int update_stride, int wl_enable, int dfe_enable,
                          int dfe_taps, int mf_enable, int cma_warmup_samples);
/**
 * @brief Get CQPSK path parameters snapshot; returns 0 on success.
 */
int rtl_stream_cqpsk_get(int* lms_enable, int* taps, int* mu_q15, int* update_stride, int* wl_enable, int* dfe_enable,
                         int* dfe_taps, int* mf_enable, int* cma_warmup_remaining);
/**
 * @brief Configure RRC matched filter (pass -1 to leave field unchanged).
 * enable: 0/1, alpha_percent: 1..100, span_syms: 3..16.
 */
void rtl_stream_cqpsk_set_rrc(int enable, int alpha_percent, int span_syms);
/**
 * @brief Toggle DQPSK-aware decision mode (0=off, 1=on).
 */
void rtl_stream_cqpsk_set_dqpsk(int onoff);
/** Get current RRC params; returns 0 on success. */
int rtl_stream_cqpsk_get_rrc(int* enable, int* alpha_percent, int* span_syms);
/** Get DQPSK decision mode; returns 0 on success. */
int rtl_stream_cqpsk_get_dqpsk(int* onoff);
/* CQPSK acquisition-only pre-Costas FLL (0/1) */
int rtl_stream_get_cqpsk_acq_fll(void);
void rtl_stream_set_cqpsk_acq_fll(int onoff);
/** Get CQPSK acquisition FLL lock status (1 locked, 0 not locked). */
int rtl_stream_get_cqpsk_acq_fll_locked(void);

/** Toggle generic IQ balance prefilter (mode-aware image cancel). */
void rtl_stream_toggle_iq_balance(int onoff);
/** Get generic IQ balance prefilter state; returns 1 if enabled. */
int rtl_stream_get_iq_balance(void);
/**
 * @brief Provide P25P1 FEC OK/ERR deltas to drive BER-adaptive tuning.
 * Call with positive deltas (not totals). No-ops when RTL stream inactive.
 */
void rtl_stream_p25p1_ber_update(int fec_ok_delta, int fec_err_delta);

/* Coarse DSP feature toggles and snapshot */
/** Toggle CQPSK path pre-processing on/off (0=off, nonzero=on). */
void rtl_stream_toggle_cqpsk(int onoff);
/** Toggle FLL on/off (0=off, nonzero=on). */
void rtl_stream_toggle_fll(int onoff);
/** Toggle TED on/off (0=off, nonzero=on). */
void rtl_stream_toggle_ted(int onoff);
/** Get current coarse DSP feature flags; any pointer may be NULL. Returns 0 on success. */
int rtl_stream_dsp_get(int* cqpsk_enable, int* fll_enable, int* ted_enable);

/**
 * @brief Set or disable the resampler target rate (applied on controller thread).
 * Pass 0 to disable the resampler; otherwise, pass desired Hz (e.g., 48000).
 */
void rtl_stream_set_resampler_target(int target_hz);

/**
 * @brief Set the nominal samples-per-symbol used by the Gardner TED.
 *
 * Useful to align the complex DSP pipeline with protocol-layer symbol timing
 * (e.g., 10 for P25 Phase 1 at 48 kHz, 8 for P25 Phase 2 at 48 kHz).
 * Values < 2 are clamped to 2; large values are clamped to a safe range.
 */
void rtl_stream_set_ted_sps(int sps);

/**
 * @brief Provide P25 Phase 2 RS/voice error deltas for runtime helpers.
 *
 * Pass positive deltas (not totals). Slot is 0 or 1. Any delta may be 0 when
 * not applicable. No-ops when RTL stream inactive.
 */
void rtl_stream_p25p2_err_update(int slot, int facch_ok_delta, int facch_err_delta, int sacch_ok_delta,
                                 int sacch_err_delta, int voice_err_delta);

/*
 * Constellation capture (recent, decimated complex samples after DSP).
 * Copies up to max_points I/Q pairs into out_xy as interleaved int16 [I0,Q0,I1,Q1,...].
 * Returns the number of pairs copied (0 if unavailable).
 */
int rtl_stream_constellation_get(int16_t* out_xy, int max_points);

/**
 * @brief Get a snapshot of the current baseband power spectrum (magnitude, dBFS-like).
 *
 * Returns up to max_bins bins in out_db, equally spaced across the complex
 * baseband Nyquist span, with DC-centered ordering (i.e., out_db[0] ~ -Fs/2,
 * mid ~ DC, last ~ +Fs/2). Values are smoothed and approximately in dBFS.
 * Optionally returns the current demod output sample rate via out_rate.
 *
 * @param out_db Destination buffer for spectrum bins (float dB). Must not be NULL.
 * @param max_bins Maximum number of bins to write.
 * @param out_rate Optional pointer to receive current output sample rate in Hz.
 * @return Number of bins written (0 if unavailable).
 */
int rtl_stream_spectrum_get(float* out_db, int max_bins, int* out_rate);

/** Set desired spectrum FFT size (power-of-two, clamped to allowed range). */
int rtl_stream_spectrum_set_size(int n);
/** Get current spectrum FFT size. */
int rtl_stream_spectrum_get_size(void);

/* Carrier/Costas diagnostics */
/** Return current NCO frequency used for carrier rotation (Costas/FLL), in Hz. */
double rtl_stream_get_cfo_hz(void);
/** Return residual CFO estimated from the spectrum peak offset from DC, in Hz. */
double rtl_stream_get_residual_cfo_hz(void);
/** Return 1 when carrier loop appears locked (CQPSK active, CFO/residual small, SNR ok), else 0. */
int rtl_stream_get_carrier_lock(void);
/** Return last average absolute Costas error magnitude (Q14, pi==1<<14). */
int rtl_stream_get_costas_err_q14(void);
/** Return raw NCO frequency control (Q15 cycles per sample). */
int rtl_stream_get_nco_q15(void);
/** Return current demod output sample rate (Hz). */
int rtl_stream_get_demod_rate_hz(void);

/* -------- FM/C4FM amplitude stabilization + DC blocker (runtime) -------- */
/** Get FM AGC enable state (1 on, 0 off). */
int rtl_stream_get_fm_agc(void);
/** Enable/disable FM AGC (0 off, nonzero on). */
void rtl_stream_set_fm_agc(int onoff);
/**
 * Get FM AGC parameters (any pointer may be NULL).
 * target_rms/min_rms are int16-domain magnitudes (~1000..20000).
 * alpha_up/down are Q15 smoothing factors (0..32768).
 */
void rtl_stream_get_fm_agc_params(int* target_rms, int* min_rms, int* alpha_up_q15, int* alpha_down_q15);
/** Set FM AGC parameters; pass negative to leave a field unchanged. */
void rtl_stream_set_fm_agc_params(int target_rms, int min_rms, int alpha_up_q15, int alpha_down_q15);

/** Get FM constant-envelope limiter state (1 on, 0 off). */
int rtl_stream_get_fm_limiter(void);
/** Enable/disable FM constant-envelope limiter (0 off, nonzero on). */
void rtl_stream_set_fm_limiter(int onoff);

/**
 * Get complex I/Q DC blocker state and shift k (any pointer may be NULL).
 * Returns 1 if enabled, 0 otherwise; writes current k to out_shift_k if not NULL.
 */
int rtl_stream_get_iq_dc(int* out_shift_k);
/** Set DC blocker enable (0/1) and/or shift k (>=6..<=15). Pass shift_k<0 to keep unchanged. */
void rtl_stream_set_iq_dc(int enable, int shift_k);

/* FM/FSK blind CMA equalizer (pre-discriminator) */
/** Get FM CMA enable state (1 on, 0 off). */
int rtl_stream_get_fm_cma(void);
/** Enable/disable FM CMA equalizer (0 off, nonzero on). */
void rtl_stream_set_fm_cma(int onoff);
/** Get FM CMA parameters (any pointer may be NULL). */
void rtl_stream_get_fm_cma_params(int* taps, int* mu_q15, int* warmup_samples);
/** Set FM CMA parameters; pass negative to leave a field unchanged. */
void rtl_stream_set_fm_cma_params(int taps, int mu_q15, int warmup_samples);
/** Get/Set FM CMA strength (0=Light,1=Medium,2=Strong). */
int rtl_stream_get_fm_cma_strength(void);
void rtl_stream_set_fm_cma_strength(int strength);

#ifdef __cplusplus
}
#endif
/* Retrieve adaptive guard status for 5-tap CMA (freeze remaining, accepts, rejects). */
void rtl_stream_get_fm_cma_guard(int* freeze_blocks, int* accepts, int* rejects);
