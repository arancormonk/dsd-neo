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
int rtl_stream_dsp_get(int* cqpsk_enable, int* fll_enable, int* ted_enable, int* auto_dsp_enable);
/** Toggle all automatic DSP assistance (e.g., BER-based tuning). */
void rtl_stream_toggle_auto_dsp(int onoff);

/**
 * @brief Auto-DSP tuning configuration (thresholds, windows, smoothing).
 *
 * All values are integers for easy ABI stability. Reasonable defaults are
 * applied internally; unset fields should be initialized to 0 to accept
 * defaults. Percent thresholds are in whole-percent units.
 */
typedef struct rtl_auto_dsp_config {
    /* P25 Phase 1 (BER-driven) */
    int p25p1_window_min_total; /* default 200 symbols */
    int p25p1_moderate_on_pct;  /* default 7 */
    int p25p1_moderate_off_pct; /* default 5 */
    int p25p1_heavy_on_pct;     /* default 15 */
    int p25p1_heavy_off_pct;    /* default 10 */
    int p25p1_cooldown_ms;      /* default 700 */

    /* P25 Phase 2 (FACCH/SACCH/voice deltas) */
    int p25p2_ok_min;         /* default 4 */
    int p25p2_err_margin_on;  /* default 2 */
    int p25p2_err_margin_off; /* default 0 */
    int p25p2_cooldown_ms;    /* default 500 */

    /* Common smoothing (Q15 fixed-point alpha; 0..32768). default ~0.2 */
    int ema_alpha_q15; /* default 6553 */
} rtl_auto_dsp_config;

/** Get current Auto-DSP configuration. Any pointer may be NULL. */
void rtl_stream_auto_dsp_get_config(rtl_auto_dsp_config* out);
/** Set Auto-DSP configuration (applies sane bounds; 0 uses defaults). */
void rtl_stream_auto_dsp_set_config(const rtl_auto_dsp_config* in);

/**
 * @brief Auto-DSP live status snapshot.
 *
 * Modes: 0=Clean, 1=Moderate, 2=Heavy.
 * Percent fields are whole-percent integers (0..100).
 */
typedef struct rtl_auto_dsp_status {
    int p25p1_mode;     /* 0..2 */
    int p25p1_ema_pct;  /* 0..100 */
    int p25p1_since_ms; /* milliseconds since last mode change */
    int p25p2_mode;     /* 0..2 */
    int p25p2_since_ms; /* milliseconds since last mode change */
} rtl_auto_dsp_status;

/** Get Auto-DSP live status (any pointer may be NULL). */
void rtl_stream_auto_dsp_get_status(rtl_auto_dsp_status* out);

/** Manual DSP override: when enabled, frame-sync logic will not auto-toggle
 * CQPSK/FLL/TED based on detected modulation. Useful to pin user-selected
 * DSP behavior across menu closes and non-QPSK intervals. */
void rtl_stream_set_manual_dsp(int onoff);
int rtl_stream_get_manual_dsp(void);

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
 * @brief Provide P25 Phase 2 RS/voice error deltas to drive auto-DSP tuning.
 *
 * Pass positive deltas (not totals). Slot is 0 or 1. Any delta may be 0 when
 * not applicable. No-ops when RTL stream inactive or auto-DSP disabled.
 */
void rtl_stream_p25p2_err_update(int slot, int facch_ok_delta, int facch_err_delta, int sacch_ok_delta,
                                 int sacch_err_delta, int voice_err_delta);

/*
 * Constellation capture (recent, decimated complex samples after DSP).
 * Copies up to max_points I/Q pairs into out_xy as interleaved int16 [I0,Q0,I1,Q1,...].
 * Returns the number of pairs copied (0 if unavailable).
 */
int rtl_stream_constellation_get(int16_t* out_xy, int max_points);

#ifdef __cplusplus
}
#endif
