// SPDX-License-Identifier: GPL-3.0-or-later
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

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_STREAM_C_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_STREAM_C_H_H

#include <dsd-neo/platform/platform.h>

#include <stddef.h>
#include <stdint.h>

#include <dsd-neo/core/input_level.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/io/rtl_stream_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DSD_ATTR_PACKED rtl_stream_output_kind {
    RTL_STREAM_OUTPUT_AUDIO_MONITOR = 0,
    RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR = 1,
    RTL_STREAM_OUTPUT_SYMBOL_CQPSK = 2,
} rtl_stream_output_kind;

typedef enum DSD_ATTR_PACKED rtl_stream_channel_profile {
    RTL_STREAM_CHANNEL_PROFILE_WIDE = 0,
    RTL_STREAM_CHANNEL_PROFILE_6K25 = 1,
    RTL_STREAM_CHANNEL_PROFILE_12K5 = 2,
    RTL_STREAM_CHANNEL_PROFILE_PROVOICE = 3,
    RTL_STREAM_CHANNEL_PROFILE_P25_C4FM = 4,
    RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK = 5,
} rtl_stream_channel_profile;

typedef enum DSD_ATTR_PACKED rtl_stream_tune_result {
    RTL_STREAM_TUNE_OK = 0,
    RTL_STREAM_TUNE_DEFERRED = 1,
    RTL_STREAM_TUNE_FAILED = -1,
    RTL_STREAM_TUNE_TIMEOUT = -2,
} rtl_stream_tune_result;

/* Lifecycle */
/**
 * @brief Create a new RTL-SDR stream context from an immutable options snapshot.
 *
 * The supplied options are copied internally and never mutated. Use
 * rtl_stream_create_mirrored() when the caller needs live RTL PPM updates to
 * propagate back into its owning @ref dsd_opts.
 *
 * @param opts Decoder options snapshot used to configure the stream. Must not be NULL.
 * @param out_ctx [out] On success, receives an opaque context pointer.
 * @return 0 on success; otherwise <0 on error.
 */
int rtl_stream_create(const dsd_opts* opts, RtlSdrContext** out_ctx);
/**
 * @brief Create a new RTL-SDR stream context mirrored to caller-owned options.
 *
 * The stream still owns an internal copy of @p opts, but live requested PPM
 * updates also write back into the caller-owned structure so restarts and
 * config snapshots retain the current correction.
 *
 * @param opts Mutable caller-owned decoder options to mirror. Must not be NULL.
 * @param out_ctx [out] On success, receives an opaque context pointer.
 * @return 0 on success; otherwise <0 on error.
 */
int rtl_stream_create_mirrored(dsd_opts* opts, RtlSdrContext** out_ctx);
/**
 * @brief Start the stream threads and device I/O.
 * @param ctx Stream context created by rtl_stream_create() or rtl_stream_create_mirrored().
 * @return 0 on success; otherwise <0 on error.
 */
int rtl_stream_start(RtlSdrContext* ctx);
/**
 * @brief Stop the stream and cleanup resources associated with the run.
 * Safe to call multiple times; subsequent calls are no-ops.
 * @param ctx Stream context created by rtl_stream_create() or rtl_stream_create_mirrored().
 * @return 0 on success; otherwise <0 on error.
 */
int rtl_stream_stop(RtlSdrContext* ctx);
/**
 * @brief Gracefully stop the stream without asserting global exit flags.
 *
 * Mirrors rtl_stream_stop() but avoids toggling global shutdown state so the
 * UI can reconfigure and restart streaming without terminating the process.
 *
 * @param ctx Stream context created by rtl_stream_create() or rtl_stream_create_mirrored().
 * @return 0 on success; otherwise <0 on error.
 */
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
 * @return rtl_stream_tune_result: 0 on success, 1 when deferred, negative on error/timeout.
 */
int rtl_stream_tune(RtlSdrContext* ctx, uint32_t center_freq_hz);
/**
 * @brief Publish a live RTL PPM request and assign it a fresh request generation.
 *
 * Runtime writers should use this helper instead of mutating
 * `opts->rtlsdr_ppm_error` directly so failed applies and same-value retries
 * remain distinguishable.
 *
 * @param opts Decoder options to update. Must not be NULL.
 * @param ppm Requested correction in PPM; clamped to [-200, 200].
 * @return 0 on success; otherwise <0 on invalid input.
 */
int rtl_stream_request_ppm(dsd_opts* opts, int ppm);
/**
 * @brief Adjust the live RTL PPM request relative to its current published value.
 *
 * Uses the same synchronized publication path as rtl_stream_request_ppm() so
 * UI increment/decrement commands do not race a concurrent rollback or retry.
 *
 * @param opts Decoder options to update. Must not be NULL.
 * @param delta Signed delta in PPM; result is clamped to [-200, 200].
 * @return 0 on success; otherwise <0 on invalid input.
 */
int rtl_stream_adjust_ppm(dsd_opts* opts, int delta);
/**
 * @brief Read the live requested RTL PPM value using the synchronized runtime snapshot.
 *
 * Runtime UI code should prefer this helper over reading
 * `opts->rtlsdr_ppm_error` directly while the RTL stream is active.
 *
 * @param opts Decoder options to query. May be NULL.
 * @return The currently requested RTL PPM value, or 0 when `opts` is NULL.
 */
int rtl_stream_get_requested_ppm(const dsd_opts* opts);

/* I/O */
/**
 * @brief Read up to `count` interleaved audio samples into `out`.
 * @param ctx Stream context.
 * @param out Destination buffer for samples. Must not be NULL.
 * @param count Maximum number of samples to read.
 * @param out_got [out] Set to the number of samples actually read.
 * @return 0 on success; otherwise <0 on error (e.g., shutdown).
 */
int rtl_stream_read(RtlSdrContext* ctx, float* out, size_t count, int* out_got);
int rtl_stream_read_monitor(const RtlSdrContext* ctx, float* out, size_t count, int* out_got);
/**
 * @brief Get the current output sample rate in Hz.
 * @param ctx Stream context.
 * @return Output sample rate in Hz; returns 0 if `ctx` is invalid.
 */
uint32_t rtl_stream_output_rate(const RtlSdrContext* ctx);
uint32_t rtl_stream_monitor_rate(const RtlSdrContext* ctx);
/**
 * @brief Return the RTL output stream generation.
 *
 * Increments whenever the RTL output stream can contain a different logical
 * sample/symbol sequence, such as fresh stream setup, retune, restart, or
 * explicit output clear.
 */
uint32_t rtl_stream_output_generation(void);
/**
 * @brief Return 1 when an RTL-family stream context is currently running.
 *
 * The predicate is false before startup, after soft/hard stop, and during
 * cooperative shutdown. Use it before consuming stream-local metadata that may
 * retain its last configured value after cleanup.
 */
int rtl_stream_is_active(void);
/**
 * @brief Return the active RTL stream output kind.
 *
 * Digital RTL-family paths, including SoapySDR, return symbol kinds. Analog
 * monitor paths return AUDIO_MONITOR.
 */
int rtl_stream_get_output_kind(void);
int rtl_stream_get_symbol_profile(int* out_symbol_rate_hz, int* out_levels);
int rtl_stream_get_symbol_profile_full(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile);

/**
 * @brief Update symbol modem profile for RTL-family digital modes.
 *
 * @param symbol_rate_hz Symbol rate in Hz, e.g. 4800, 6000, 2400.
 * @param levels Number of FSK levels (2 or 4). CQPSK remains selected by modulation.
 * @param channel_profile rtl_stream_channel_profile profile id.
 * @return 0 on success, negative on invalid input.
 */
int rtl_stream_set_symbol_profile(int symbol_rate_hz, int levels, int channel_profile);

/**
 * @brief Queue a symbol/CQPSK timing profile to apply at the next RTL retune boundary.
 *
 * Trunking code may know the destination channel profile before the controller
 * thread has actually programmed the tuner. This helper records that profile
 * without mutating the active demodulator; the controller applies it after the
 * hardware frequency change and before retune DSP state reset.
 *
 * @param cqpsk_enable 0/1 to force CQPSK off/on, negative to leave unchanged.
 * @param symbol_rate_hz Symbol rate in Hz, e.g. 4800 or 6000.
 * @param levels Number of symbol levels, 2 or 4.
 * @param channel_profile rtl_stream_channel_profile profile id.
 * @param ted_sps CQPSK timing samples-per-symbol to apply; <=0 leaves the SPS unchanged.
 * @param persist_ted_override Non-zero keeps ted_sps as an override after retune.
 */
void rtl_stream_prepare_retune_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile,
                                       int ted_sps, int persist_ted_override);

/**
 * @brief Queue a symbol/CQPSK timing profile for a specific RTL retune target.
 *
 * This is the preferred trunking helper when the destination frequency is
 * already known. The native RTL controller consumes the profile only for a
 * matching target frequency, so unrelated retunes cannot steal the queued
 * demodulator settings before the intended tune request is scheduled.
 *
 * @param target_freq_hz Intended center frequency in Hz; zero leaves the profile unbound.
 * @param cqpsk_enable 0/1 to force CQPSK off/on, negative to leave unchanged.
 * @param symbol_rate_hz Symbol rate in Hz, e.g. 4800 or 6000.
 * @param levels Number of symbol levels, 2 or 4.
 * @param channel_profile rtl_stream_channel_profile profile id.
 * @param ted_sps CQPSK timing samples-per-symbol to apply; <=0 leaves the SPS unchanged.
 * @param persist_ted_override Non-zero keeps ted_sps as an override after retune.
 */
void rtl_stream_prepare_retune_profile_for_target(uint32_t target_freq_hz, int cqpsk_enable, int symbol_rate_hz,
                                                  int levels, int channel_profile, int ted_sps,
                                                  int persist_ted_override);

typedef struct rtl_stream_retune_gain_profile {
    int tuner_gain_is_set;
    int tuner_gain_tenth_db;
    int tuner_gain_is_auto;
    int tuner_autogain_is_set;
    int tuner_autogain_on;
} rtl_stream_retune_gain_profile;

/**
 * @brief Queue a symbol/CQPSK timing profile plus optional tuner gain for a retune target.
 *
 * Gain fields are consumed at the same retune boundary as the demodulator
 * profile. Pass NULL or set tuner_gain_is_set to zero to leave tuner gain
 * unchanged. When set, tuner_gain_is_auto requests device auto gain; otherwise
 * tuner_gain_tenth_db is applied as nearest manual gain.
 *
 * @param target_freq_hz Intended center frequency in Hz; zero leaves the profile unbound.
 * @param cqpsk_enable 0/1 to force CQPSK off/on, negative to leave unchanged.
 * @param symbol_rate_hz Symbol rate in Hz, e.g. 4800 or 6000.
 * @param levels Number of symbol levels, 2 or 4.
 * @param channel_profile rtl_stream_channel_profile profile id.
 * @param ted_sps CQPSK timing samples-per-symbol to apply; <=0 leaves the SPS unchanged.
 * @param persist_ted_override Non-zero keeps ted_sps as an override after retune.
 * @param gain_profile Optional tuner gain/autogain profile to apply at retune.
 */
void rtl_stream_prepare_retune_profile_for_target_with_gain(uint32_t target_freq_hz, int cqpsk_enable,
                                                            int symbol_rate_hz, int levels, int channel_profile,
                                                            int ted_sps, int persist_ted_override,
                                                            const rtl_stream_retune_gain_profile* gain_profile);

/**
 * @brief Apply and clear the queued retune profile immediately.
 *
 * Use this when RTL audio is retuned by an external backend rather than the
 * native RTL controller, so there is no controller retune boundary to consume
 * the queued profile.
 */
void rtl_stream_apply_pending_retune_profile(void);

/**
 * @brief Apply and clear a queued retune profile for a specific external retune target.
 *
 * Use this when an external backend, such as rigctl, has completed a frequency
 * change and the queued profile was bound to that target.
 */
void rtl_stream_apply_pending_retune_profile_for_target(uint32_t target_freq_hz);

/**
 * @brief Clear any queued retune profile that has not yet been consumed.
 */
void rtl_stream_clear_pending_retune_profile(void);

/**
 * @brief Request fresh acquisition for the active RTL FSK demod path.
 *
 * The request is consumed by the demod thread before the next FSK block so the
 * modem state is not mutated from decoder/control threads. Returns 1 when a
 * request was queued, 0 when RTL FSK output is inactive.
 */
int rtl_stream_request_fsk_reacquire(void);

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
 * @return Mean power value (approximate RMS squared, normalized to full scale 1.0).
 */
double rtl_stream_return_pwr(const RtlSdrContext* ctx);

/**
 * @brief Set the channel squelch level in the demod state.
 *
 * Call this whenever opts->rtl_squelch_level changes to keep the demod
 * state in sync for channel-based squelching.
 *
 * @param level Linear power threshold (same units as rtl_squelch_level).
 */
void rtl_stream_set_channel_squelch(float level);

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
/**
 * @brief Return whether rtl_tcp adaptive buffering/autotune is enabled.
 * @return 1 when enabled; 0 when disabled; negative on error.
 */
int rtl_stream_get_rtltcp_autotune(void);
/**
 * @brief Enable or disable rtl_tcp adaptive buffering/autotune logic.
 * @param onoff Non-zero to enable; zero to disable.
 */
void rtl_stream_set_rtltcp_autotune(int onoff);

/**
 * @brief Return the last RF center frequency applied by the controller thread.
 *
 * This reports the effective tuned frequency after any pending-retune coalescing.
 *
 * @param out_freq_hz [out] Applied center frequency in Hz.
 * @return 0 on success; negative on error.
 */
int rtl_stream_get_last_applied_freq(uint32_t* out_freq_hz);

#if defined(DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS)
/**
 * @brief Test-only helper to request a retune on an active stream context.
 *
 * Returns 0 on success, -1 when the request is rejected (for example replay),
 * and -2 on timeout.
 */
int rtl_stream_test_request_retune(const RtlSdrContext* ctx, uint32_t freq_hz, int timeout_ms);

/**
 * @brief Seed queued output, run reconfigure input preparation, and report the result.
 *
 * This verifies retune preparation preserves queued samples that the later
 * drain/clear policy is responsible for handling.
 */
int rtl_stream_test_prepare_reconfigure_input(size_t queued_samples, size_t* out_used_after,
                                              uint32_t* out_generation_before, uint32_t* out_generation_after);

/**
 * @brief Seed output/cache counts and report whether retune drain sees them.
 *
 * Decoder-owned cached symbols are reported for diagnostics but do not block
 * the retune drain predicate.
 */
int rtl_stream_test_retune_output_pending(size_t queued_samples, int cached_symbols, size_t* out_ring_pending,
                                          int* out_cache_pending, int* out_drained);

/**
 * @brief Seed output/cache counts, apply tune-result drain policy, and report the result.
 */
int rtl_stream_test_tune_result_output_drain(int tune_result, size_t queued_samples, int cached_symbols,
                                             size_t* out_used_after, int* out_cache_pending_after,
                                             uint32_t* out_generation_before, uint32_t* out_generation_after);

/**
 * @brief Seed output/cache counts, clear output, and report the resulting state.
 */
int rtl_stream_test_clear_output(size_t queued_samples, int cached_symbols, size_t* out_used_after,
                                 int* out_cache_pending_after, uint32_t* out_generation_before,
                                 uint32_t* out_generation_after);
int rtl_stream_test_clear_output_fsk_reset(size_t queued_samples, int* out_have_prev_after_clear,
                                           int* out_consumed_reset, int* out_have_prev_after_consume);
int rtl_stream_test_fsk_cfo_snapshot(double dc_rad_per_sample, int rate_out_hz, double* out_cfo_hz,
                                     int* out_after_reset_available);

/**
 * @brief Seed output/cache state, request FSK reacquire, and consume pending reset.
 */
int rtl_stream_test_fsk_reacquire(int output_kind, size_t queued_samples, int cached_symbols, size_t* out_used_after,
                                  int* out_cache_pending_after, uint32_t* out_generation_before,
                                  uint32_t* out_generation_after, int* out_request_rc, int* out_consumed);

/**
 * @brief Verify queued retune profiles are copied onto their scheduled requests.
 */
int rtl_stream_test_retune_profile_request_binding(int* out_first_profile, int* out_second_profile,
                                                   uint32_t* out_first_freq_hz, uint32_t* out_second_freq_hz,
                                                   uint32_t* out_first_request_id, uint32_t* out_second_request_id);

/**
 * @brief Verify a coalesced no-profile retune preserves an already-bound profile.
 */
int rtl_stream_test_retune_profile_coalesced_no_profile(int* out_profile, uint32_t* out_profile_freq_hz,
                                                        uint32_t* out_manual_freq_hz, uint32_t* out_request_id,
                                                        uint32_t* out_coalesced_request_id);

/**
 * @brief Verify queued retune gain settings are copied onto their scheduled request.
 */
int rtl_stream_test_retune_profile_gain_binding(int* out_gain_is_set, int* out_gain_tenth_db, int* out_gain_is_auto,
                                                int* out_autogain_is_set, int* out_autogain_on);

typedef struct rtl_stream_test_replay_state {
    int replay_input_eof;
    int replay_input_drained;
    int replay_demod_drained;
    int replay_output_drained;
    int replay_forced_stop;
    int should_exit;
    uint64_t replay_last_submit_gen;
    uint64_t replay_last_submit_gen_at_eof;
    uint64_t replay_last_consume_gen;
    size_t input_ring_used;
    size_t output_ring_used;
    uint32_t replay_event_retune_count;
    uint32_t replay_event_mute_count;
    uint32_t replay_event_reset_count;
    uint32_t replay_event_last_frequency_hz;
    uint64_t replay_event_last_mute_bytes;
    int replay_event_last_reset_reason;
    uint32_t replay_loop_restart_count;
    uint32_t replay_loop_restart_last_frequency_hz;
} rtl_stream_test_replay_state;

/**
 * @brief Snapshot replay EOF state-machine fields for integration tests.
 *
 * Returns 0 on success and fills @p out_state, or a negative value if the
 * stream/context is unavailable.
 */
int rtl_stream_test_get_replay_state(const RtlSdrContext* ctx, rtl_stream_test_replay_state* out_state);

/**
 * @brief Return whether steady-state demod watermarks are enabled for a source.
 *
 * This protects live rtl_tcp from reintroducing demod-side refill pauses after
 * the startup prebuffer.
 */
int rtl_stream_test_steady_state_watermark_enabled(const char* audio_in_dev);
#endif

/**
 * @brief Get smoothed CQPSK timing residual from the demod pipeline in Q14 units.
 *
 * Positive values indicate persistent "sample early" bias (nudge center right),
 * negative values indicate "sample late" bias (nudge center left).
 * Returns 0 when unavailable.
 *
 * @param ctx Stream context (unused).
 * @return Signed Q14 residual (approximately float residual * 16384).
 */
int rtl_stream_cqpsk_timing_bias(const RtlSdrContext* ctx);

/**
 * @brief Get the configured CQPSK Gardner timing samples-per-symbol.
 *
 * @return Nominal SPS (>=2); 0 when unavailable.
 */
int rtl_stream_get_ted_sps(void);

/**
 * @brief Get the pending CQPSK Gardner timing samples-per-symbol override.
 *
 * @return Override SPS when set; 0 when normal rate-derived SPS is active.
 */
int rtl_stream_get_ted_sps_override(void);

/**
 * @brief Set the CQPSK Gardner timing samples-per-symbol.
 *
 * Use when switching between symbol rates (e.g., P25P1 4800 sym/s vs P25P2 6000 sym/s).
 * CQPSK timing will reinitialize its internal state (omega bounds, delay line) on SPS change.
 * Also sets an override flag that persists the value across rate-change refreshes.
 *
 * @param sps Nominal samples per symbol (clamped to [2, 64]).
 */
void rtl_stream_set_ted_sps(int sps);

/**
 * @brief Clear the CQPSK timing SPS override.
 *
 * Call when returning to control channel to allow normal SPS calculation
 * based on opts mode flags. Without clearing, the voice channel SPS would
 * persist incorrectly.
 */
void rtl_stream_clear_ted_sps_override(void);

/**
 * @brief Set the CQPSK Gardner timing SPS without asserting the override.
 *
 * Sets ted_sps but leaves ted_sps_override unchanged (typically 0 after
 * clearing). Use when returning to CC or switching protocols where the
 * rate-change refresh should be allowed to recalculate SPS later.
 *
 * @param sps Nominal samples per symbol (clamped to [2, 64]).
 */
void rtl_stream_set_ted_sps_no_override(int sps);

/**
 * @brief Set the CQPSK Gardner timing loop gain (native float).
 *
 * @param gain Loop gain; typical 0.01..0.1, default ~0.05.
 */
void rtl_stream_set_ted_gain(float gain);

/**
 * @brief Get the current CQPSK Gardner timing loop gain.
 *
 * @return Native float loop gain.
 */
float rtl_stream_get_ted_gain(void);

/**
 * @brief Capture a snapshot of the eye diagram buffer (timing helper).
 *
 * Copies up to `max_samples` real I-channel samples from the decimated complex
 * baseband into `out` and returns the number of samples copied. Also writes
 * the current nominal SPS into out_sps when available.
 *
 * @param out Destination buffer for I-channel samples (must not be NULL).
 * @param max_samples Maximum number of samples to copy.
 * @param out_sps [out] Receives nominal samples-per-symbol (may be NULL).
 * @return Number of samples written; 0 if unavailable.
 */
int rtl_stream_eye_get(float* out, int max_samples, int* out_sps);

/**
 * @brief Get smoothed demod SNR estimate in dB (post-filter, center-of-symbol).
 *
 * Computed on the demod thread for digital modes using the active demodulation
 * domain: discriminator samples for FSK output, and symbol/constellation-domain
 * samples for CQPSK.
 * Returns a negative value when unavailable.
 *
 * @return SNR in dB, or negative when unavailable.
 */
double rtl_stream_get_snr_c4fm(void);
/**
 * @brief Get smoothed CQPSK/LSM demod SNR estimate in dB.
 * @return SNR in dB, or negative when unavailable.
 */
double rtl_stream_get_snr_cqpsk(void);
/**
 * @brief Get smoothed GFSK demod SNR estimate in dB.
 * @return SNR in dB, or negative when unavailable.
 */
double rtl_stream_get_snr_gfsk(void);

/**
 * @brief Get the current C4FM (4-level FSK) SNR estimator bias in dB.
 *
 * This bias accounts for both the statistical estimator bias and the
 * noise bandwidth correction based on current DSP settings (sample rate,
 * samples per symbol, and channel LPF profile).
 *
 * @return Bias value in dB to subtract from raw SNR estimate.
 */
double rtl_stream_get_snr_bias_c4fm(void);

/**
 * @brief Get the current EVM/GFSK/QPSK SNR estimator bias in dB.
 *
 * This bias accounts for both the statistical estimator bias and the
 * noise bandwidth correction based on current DSP settings.
 *
 * @return Bias value in dB to subtract from raw SNR estimate.
 */
double rtl_stream_get_snr_bias_evm(void);

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

/**
 * @brief Get supervisory tuner auto-gain enable flag.
 * @return 1 when auto-gain supervisor is enabled; 0 when disabled.
 */
int rtl_stream_get_tuner_autogain(void);
/**
 * @brief Enable or disable supervisory tuner auto-gain.
 * @param onoff Non-zero to enable; zero to disable.
 */
void rtl_stream_set_tuner_autogain(int onoff);

/**
 * @brief Get auto PPM status and last measurements.
 *
 * @param enabled [out] Current enable flag (0/1); may be NULL.
 * @param snr_db [out] Latest SNR estimate in dB; may be NULL.
 * @param df_hz [out] Latest residual frequency offset in Hz; may be NULL.
 * @param est_ppm [out] Estimated PPM error relative to center; may be NULL.
 * @param last_dir [out] Last applied step direction (-1,0,+1); may be NULL.
 * @param cooldown [out] Remaining cooldown iterations before next step; may be NULL.
 * @param locked [out] 1 if locked; 0 if training/idle; may be NULL.
 * @return 0 on success; negative on error or when unavailable.
 */
int rtl_stream_auto_ppm_get_status(int* enabled, double* snr_db, double* df_hz, double* est_ppm, int* last_dir,
                                   int* cooldown, int* locked);

/**
 * @brief Return 1 when auto-PPM training is active.
 *
 * @return 1 when enabled, not locked, and currently training; 0 otherwise.
 */
int rtl_stream_auto_ppm_training_active(void);

/**
 * @brief Get locked auto-PPM value and lock-time snapshot, if available.
 *
 * @param ppm [out] Locked PPM value; may be NULL.
 * @param snr_db [out] SNR at lock time in dB; may be NULL.
 * @param df_hz [out] Residual frequency offset at lock time in Hz; may be NULL.
 * @return 0 on success; negative when unavailable.
 */
int rtl_stream_auto_ppm_get_lock(int* ppm, double* snr_db, double* df_hz);
/**
 * @brief Runtime toggle for auto-PPM (0/1).
 *
 * @param onoff Non-zero to enable; zero to disable.
 */
void rtl_stream_set_auto_ppm(int onoff);
/**
 * @brief Return the current runtime auto-PPM toggle value (0/1).
 *
 * @return 1 when auto-PPM is enabled; 0 when disabled.
 */
int rtl_stream_get_auto_ppm(void);

/**
 * @brief Toggle generic IQ balance prefilter (mode-aware image cancel).
 *
 * @param onoff Non-zero to enable; zero to disable.
 */
void rtl_stream_toggle_iq_balance(int onoff);
/** Get generic IQ balance prefilter state; returns 1 if enabled. */
int rtl_stream_get_iq_balance(void);
/**
 * @brief Provide P25P1 FEC OK/ERR deltas to drive BER-adaptive tuning.
 * Call with positive deltas (not totals). No-ops when RTL stream inactive.
 *
 * @param fec_ok_delta Incremental count of successful FEC codewords.
 * @param fec_err_delta Incremental count of failed FEC codewords.
 */
void rtl_stream_p25p1_ber_update(int fec_ok_delta, int fec_err_delta);

typedef struct rtl_stream_costas_metrics {
    int err_smooth_avg_q14;
    int err_raw_avg_q14;
    int confidence_avg_q14;
    int zero_conf_pct;
} rtl_stream_costas_metrics;

typedef struct rtl_stream_decode_health {
    int valid;
    uint32_t generation;
    unsigned int p25p1_fec_ok;
    unsigned int p25p1_fec_err;
    unsigned int p25p2_facch_ok;
    unsigned int p25p2_facch_err;
    unsigned int p25p2_sacch_ok;
    unsigned int p25p2_sacch_err;
    unsigned int p25p2_voice_err;
} rtl_stream_decode_health;

/**
 * @brief Toggle CQPSK path pre-processing on/off (0=off, nonzero=on).
 *
 * @param onoff Non-zero to enable; zero to disable.
 */
void rtl_stream_toggle_cqpsk(int onoff);
/**
 * @brief Get current CQPSK recovery status; any pointer may be NULL.
 *
 * @param cqpsk_enable [out] CQPSK enable flag.
 * @param cqpsk_timing_active [out] CQPSK Gardner timing active flag.
 * @return 0 on success; negative on error.
 */
int rtl_stream_get_cqpsk_status(int* cqpsk_enable, int* cqpsk_timing_active);

/**
 * @brief Get RTL-path decode-health counters for the current output generation.
 *
 * Counters are reset on output generation changes and accumulate protocol
 * health callbacks such as P25 Phase 1 FEC and Phase 2 RS/voice deltas.
 *
 * @param out [out] Decode-health snapshot. Must not be NULL.
 * @return 0 on success; negative on invalid input.
 */
int rtl_stream_get_decode_health(rtl_stream_decode_health* out);

/**
 * @brief Get recent raw receiver input-level health metrics.
 *
 * The snapshot observes backend-native samples before demodulation (CU8 for
 * RTL/rtl_tcp, CS16 or CF32 for Soapy). When raw receiver metrics are not yet
 * available, the RTL path may return an invalid/unknown snapshot or a
 * soft-symbol clipping diagnostic.
 *
 * @param out [out] Input-level snapshot. Must not be NULL.
 * @return 0 on success; negative on invalid input.
 */
int rtl_stream_get_input_level(dsd_input_level_snapshot* out);

/**
 * @brief Set or disable the resampler target rate (applied on controller thread).
 * Pass 0 to disable the resampler; otherwise, pass desired Hz (e.g., 48000).
 *
 * @param target_hz Desired output sample rate in Hz (0 disables the resampler).
 */
void rtl_stream_set_resampler_target(int target_hz);

/**
 * @brief Provide P25 Phase 2 RS/voice error deltas for runtime helpers.
 *
 * Pass positive deltas (not totals). Slot is 0 or 1. Any delta may be 0 when
 * not applicable. No-ops when RTL stream inactive.
 *
 * @param slot Slot index (0 or 1).
 * @param facch_ok_delta Incremental FACCH success count.
 * @param facch_err_delta Incremental FACCH error count.
 * @param sacch_ok_delta Incremental SACCH success count.
 * @param sacch_err_delta Incremental SACCH error count.
 * @param voice_err_delta Incremental voice error count.
 */
void rtl_stream_p25p2_err_update(int slot, int facch_ok_delta, int facch_err_delta, int sacch_ok_delta,
                                 int sacch_err_delta, int voice_err_delta);

/**
 * @brief Capture a snapshot of recent constellation points after DSP.
 *
 * Copies up to `max_points` I/Q pairs into `out_xy` as interleaved floats
 * [I0,Q0,I1,Q1,...] on the normalized float amplitude scale used by the DSP.
 * Returns the number of pairs copied (0 if unavailable).
 *
 * @param out_xy Destination buffer for interleaved I/Q pairs (must not be NULL).
 * @param max_points Maximum number of pairs to write.
 * @return Number of pairs written; 0 if unavailable.
 */
int rtl_stream_constellation_get(float* out_xy, int max_points);

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

/**
 * @brief Set desired spectrum FFT size (power-of-two, clamped to allowed range).
 *
 * @param n Requested FFT size (power of two within supported bounds).
 * @return 0 on success; negative on error.
 */
int rtl_stream_spectrum_set_size(int n);
/** @brief Get current spectrum FFT size. */
int rtl_stream_spectrum_get_size(void);

/* Carrier/Costas diagnostics and control */
/**
 * @brief Reset Costas loop state for fresh carrier acquisition.
 *
 * Call on channel retunes to clear stale phase/frequency estimates from the
 * previous channel. Without reset, the loop starts with the old frequency
 * offset and must slew to the new carrier, delaying sync acquisition.
 */
void rtl_stream_reset_costas(void);

/** Return current NCO frequency used for carrier rotation (Costas/FLL), in Hz. */
double rtl_stream_get_cfo_hz(void);
/** Return residual CFO estimated from the spectrum peak offset from DC, in Hz. */
double rtl_stream_get_residual_cfo_hz(void);
/** Return 1 when carrier loop appears locked (CQPSK active, CFO/residual small, SNR ok), else 0. */
int rtl_stream_get_carrier_lock(void);
/** Return last average absolute smoothed Costas error magnitude (Q14, pi==1<<14). */
int rtl_stream_get_costas_err_q14(void);
/** Return Costas discriminator health metrics for the latest DSP block. */
int rtl_stream_get_costas_metrics(rtl_stream_costas_metrics* out);
/** Return raw NCO frequency control (Q15 cycles per sample). */
int rtl_stream_get_nco_q15(void);
/** Return current demod output sample rate (Hz). */
int rtl_stream_get_demod_rate_hz(void);

/** Return FLL band-edge frequency estimate in Hz (coarse freq offset for CQPSK). */
double rtl_stream_get_fll_band_edge_freq_hz(void);

/**
 * @brief Get complex I/Q DC blocker state and shift k.
 *
 * Any pointer may be NULL.
 *
 * @param out_shift_k [out] Current shift exponent k; may be NULL.
 * @return 1 if enabled; 0 otherwise.
 */
int rtl_stream_get_iq_dc(int* out_shift_k);
/**
 * @brief Set DC blocker enable (0/1) and/or shift k (>=6..<=15).
 *
 * @param enable Non-zero to enable; zero to disable.
 * @param shift_k Shift exponent k; pass negative to leave unchanged.
 */
void rtl_stream_set_iq_dc(int enable, int shift_k);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_STREAM_C_H_H */
